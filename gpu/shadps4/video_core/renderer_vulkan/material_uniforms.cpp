// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <spirv/unified1/spirv.hpp11>
#include "core/emulator_settings.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/material_uniforms.h"
#include "video_core/renderer_vulkan/material_early_tests.h"
#include "video_core/renderer_vulkan/material_clip_fusion.h"
#include "video_core/renderer_vulkan/material_cse_profiles.h"
#include "video_core/renderer_vulkan/bloom_outlier_guard_profiles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::MaterialUniforms {
bool Use(const Shader::Info& info, u32 index, const Instance& instance) {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_MATERIAL_UBO");
        return value && value[0] == '1';
    }();
    if (!enabled || info.sw_stage != Shader::SwStage::Fragment ||
        info.pgm_hash != 0xbb10e440 || instance.UniformMinAlignment() > 256 ||
        EmulatorSettings.IsPatchShaders()) {
        return false;
    }
    const auto& buffer = info.buffers[index];
    return buffer.buffer_type == Shader::BufferType::Guest && !buffer.is_written &&
           !buffer.is_formatted && buffer.used_types == Shader::IR::Type::U32;
}

namespace {
using Instruction = std::span<const u32>;
spv::Op Op(Instruction i) { return static_cast<spv::Op>(i[0] & 0xffff); }
void Emit(std::vector<u32>& output, spv::Op op, std::initializer_list<u32> args) {
    output.push_back((u32(args.size() + 1) << 16) | u32(op));
    output.insert(output.end(), args);
}
}

bool Convert(std::span<const u32> source, std::span<const u32> bindings,
             std::vector<u32>& converted) {
    converted.clear();
    if (source.size() < 5 || source[0] != spv::MagicNumber || bindings.empty()) return false;
    std::vector<Instruction> instructions;
    std::unordered_map<u32, Instruction> types, definitions;
    std::unordered_map<u32, u32> variable_bindings, descriptor_sets;
    std::unordered_set<u32> readonly, readonly_blocks;
    u32 uint_type = 0;
    for (size_t at = 5; at < source.size();) {
        const u32 words = source[at] >> 16;
        if (words == 0 || words > source.size() - at) return false;
        const Instruction i = source.subspan(at, words);
        instructions.push_back(i);
        const auto op = Op(i);
        if (op >= spv::Op::OpTypeVoid && op <= spv::Op::OpTypeForwardPointer && words >= 2)
            types.emplace(i[1], i);
        if (op == spv::Op::OpTypeInt && words == 4 && i[2] == 32 && i[3] == 0) uint_type = i[1];
        if (op == spv::Op::OpConstant || op == spv::Op::OpIAdd ||
            op == spv::Op::OpShiftRightLogical || op == spv::Op::OpBitFieldUExtract)
            if (words >= 3) definitions.emplace(i[2], i);
        if (op == spv::Op::OpDecorate && words >= 3) {
            const auto decoration = static_cast<spv::Decoration>(i[2]);
            if (words == 4 && decoration == spv::Decoration::Binding) variable_bindings[i[1]] = i[3];
            if (words == 4 && decoration == spv::Decoration::DescriptorSet) descriptor_sets[i[1]] = i[3];
            if (decoration == spv::Decoration::NonWritable) readonly.insert(i[1]);
        }
        if (op == spv::Op::OpMemberDecorate && words == 4 && i[2] == 0 &&
            i[3] == u32(spv::Decoration::NonWritable)) readonly_blocks.insert(i[1]);
        at += words;
    }
    if (!uint_type) return false;
    const auto constant = [&](u32 id) -> std::optional<u32> {
        const auto found = definitions.find(id);
        if (found == definitions.end()) return {};
        const auto i = found->second;
        if (Op(i) != spv::Op::OpConstant || i.size() != 4) return {};
        const auto type = types.find(i[1]);
        if (type == types.end() || Op(type->second) != spv::Op::OpTypeInt ||
            type->second.size() != 4 || type->second[2] != 32) return {};
        return i[3];
    };
    // Prove every promoted address is a constant dword index plus a bounded
    // 8-bit push offset. This excludes arbitrary/dynamic SSBO addressing.
    const auto maximum = [&](auto&& self, u32 id, u32 depth) -> std::optional<u32> {
        if (depth > 12) return {};
        if (auto value = constant(id)) return value;
        const auto found = definitions.find(id);
        if (found == definitions.end()) return {};
        const auto i = found->second;
        if (Op(i) == spv::Op::OpIAdd && i.size() == 5) {
            auto a = self(self, i[3], depth + 1), b = self(self, i[4], depth + 1);
            if (a && b && *a <= Capacity / 4 && *b <= Capacity / 4)
                return *a + *b;
        }
        if (Op(i) == spv::Op::OpShiftRightLogical && i.size() == 5) {
            auto a = self(self, i[3], depth + 1), shift = constant(i[4]);
            if (a && shift && *shift < 32) return *a >> *shift;
        }
        if (Op(i) == spv::Op::OpBitFieldUExtract && i.size() == 6) {
            auto count = constant(i[5]), offset = constant(i[4]);
            if (count && offset && *count <= 8 && *offset <= 32 - *count)
                return (1u << *count) - 1;
        }
        return {};
    };
    std::unordered_set<u32> selected, seen_bindings;
    for (const auto i : instructions) {
        if (Op(i) != spv::Op::OpVariable || i.size() != 4 ||
            i[3] != u32(spv::StorageClass::StorageBuffer)) continue;
        const auto b = variable_bindings.find(i[2]);
        if (b == variable_bindings.end() ||
            std::ranges::find(bindings, b->second) == bindings.end()) continue;
        if (!descriptor_sets.contains(i[2]) || descriptor_sets.at(i[2]) != 0) return false;
        const auto pointer = types.find(i[1]);
        if (pointer == types.end() || pointer->second.size() != 4 ||
            Op(pointer->second) != spv::Op::OpTypePointer) return false;
        const auto block = types.find(pointer->second[3]);
        if (block == types.end() || block->second.size() != 3 ||
            Op(block->second) != spv::Op::OpTypeStruct) return false;
        if (!readonly.contains(i[2]) && !readonly_blocks.contains(block->first)) return false;
        const auto array = types.find(block->second[2]);
        if (array == types.end() || array->second.size() != 3 ||
            Op(array->second) != spv::Op::OpTypeRuntimeArray || array->second[2] != uint_type)
            return false;
        selected.insert(i[2]);
        seen_bindings.insert(b->second);
    }
    if (seen_bindings.size() != bindings.size()) return false;
    std::unordered_set<u32> pointers;
    for (const auto i : instructions) {
        if ((Op(i) == spv::Op::OpAccessChain || Op(i) == spv::Op::OpInBoundsAccessChain) &&
            i.size() >= 4 && selected.contains(i[3])) {
            if (i.size() != 6 || constant(i[4]) != 0) return false;
            const auto max = maximum(maximum, i[5], 0);
            if (!max || *max >= Capacity / 4) return false;
            const auto pointer = types.find(i[1]);
            if (pointer == types.end() || pointer->second.size() != 4 ||
                pointer->second[3] != uint_type) return false;
            pointers.insert(i[2]);
        }
    }
    for (const auto i : instructions) {
        if (Op(i) == spv::Op::OpStore && i.size() >= 2 && pointers.contains(i[1])) return false;
        if (Op(i) == spv::Op::OpCopyObject && i.size() >= 4 && pointers.contains(i[3])) return false;
    }
    u32 vector = 0;
    for (const auto& [id, type] : types) {
        if (Op(type) == spv::Op::OpTypeVector && type.size() == 4 &&
            type[2] == uint_type && type[3] == 4) vector = id;
    }
    u32 next = source[3];
    const bool new_vector = vector == 0;
    if (new_vector) vector = next++;
    const u32 count = next++, array = next++, block = next++;
    const u32 block_pointer = next++, word_pointer = next++, two = next++, three = next++;
    std::vector<u32> result(source.begin(), source.begin() + 5);
    bool annotations = false, declarations = false;
    for (const auto i : instructions) {
        const auto op = Op(i);
        if (!annotations && op >= spv::Op::OpTypeVoid && op <= spv::Op::OpTypeForwardPointer) {
            Emit(result, spv::Op::OpDecorate, {array, u32(spv::Decoration::ArrayStride), 16});
            Emit(result, spv::Op::OpDecorate, {block, u32(spv::Decoration::Block)});
            Emit(result, spv::Op::OpMemberDecorate, {block, 0, u32(spv::Decoration::Offset), 0});
            annotations = true;
        }
        if (op == spv::Op::OpDecorate && i.size() >= 3 && selected.contains(i[1]) &&
            i[2] == u32(spv::Decoration::NonWritable)) continue;
        if (op == spv::Op::OpVariable && i.size() == 4 && selected.contains(i[2])) {
            if (!declarations) {
                // std140 uvec4[] requires no optional uniform-layout feature.
                if (new_vector) Emit(result, spv::Op::OpTypeVector, {vector, uint_type, 4});
                Emit(result, spv::Op::OpConstant, {uint_type, count, Capacity / 16});
                Emit(result, spv::Op::OpTypeArray, {array, vector, count});
                Emit(result, spv::Op::OpTypeStruct, {block, array});
                Emit(result, spv::Op::OpTypePointer, {block_pointer, u32(spv::StorageClass::Uniform), block});
                Emit(result, spv::Op::OpTypePointer, {word_pointer, u32(spv::StorageClass::Uniform), uint_type});
                Emit(result, spv::Op::OpConstant, {uint_type, two, 2});
                Emit(result, spv::Op::OpConstant, {uint_type, three, 3});
                declarations = true;
            }
            Emit(result, op, {block_pointer, i[2], u32(spv::StorageClass::Uniform)});
        } else if ((op == spv::Op::OpAccessChain || op == spv::Op::OpInBoundsAccessChain) &&
                   i.size() == 6 && selected.contains(i[3])) {
            const u32 row = next++, component = next++;
            Emit(result, spv::Op::OpShiftRightLogical, {uint_type, row, i[5], two});
            Emit(result, spv::Op::OpBitwiseAnd, {uint_type, component, i[5], three});
            Emit(result, op, {word_pointer, i[2], i[3], i[4], row, component});
        } else {
            result.insert(result.end(), i.begin(), i.end());
        }
    }
    if (!declarations || !annotations) return false;
    result[3] = next;
    converted = std::move(result);
    return true;
}

vk::ShaderModule Compile(const Shader::Info& info, u32 first_binding,
                         std::span<const u32> source, const Instance& instance) {
    static const unsigned bloom_guard = [] {
        const char* value = std::getenv("BB_BLOOM_OUTLIER_GUARD");
        return value && value[0] == '2' ? 2u : value && value[0] == '1' ? 1u : 0u;
    }();
    if (bloom_guard && info.sw_stage == Shader::SwStage::Fragment &&
        info.pgm_hash == 0x4b766f62 && !EmulatorSettings.IsPatchShaders()) {
        const auto guarded = BloomOutlierGuard::Match(source, bloom_guard);
        if (!guarded.empty()) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                std::printf("Bloom outlier guard v%u: exact canonical shader match, transient module only; HDR scene unchanged\n", bloom_guard);
            return CompileSPV(guarded, instance.GetDevice());
        }
        static std::atomic<bool> skipped{false};
        if (!skipped.exchange(true))
            std::printf("Bloom outlier guard: unsupported shader permutation, original retained\n");
    }
    static const bool clip_fusion = [] {
        const char* value = std::getenv("BB_CLIP_TEST_FUSION");
        return value && value[0] == '1';
    }();
    std::vector<u32> clip_source;
    if (clip_fusion && info.sw_stage == Shader::SwStage::Fragment &&
        info.pgm_hash == 0xbb10e440 && !EmulatorSettings.IsPatchShaders() &&
        MaterialClipFusion::Convert(source, clip_source)) {
        source = clip_source;
        static std::atomic<bool> reported_clip{false};
        if (!reported_clip.exchange(true))
            std::printf("Clip test fusion: shader %08x, all four planes retained, 4 demote branches -> 1; canonical cache retained\n", u32(info.pgm_hash));
    }
    // Independent, opt-in experiment. The canonical cache and descriptor layout
    // are untouched. Unsupported permutations keep the original execution mode.
    static const bool early_tests = [] {
        const char* value = std::getenv("BB_MATERIAL_EARLY_TESTS");
        return value && value[0] == '1';
    }();
    std::vector<u32> early_source;
    if (early_tests && info.sw_stage == Shader::SwStage::Fragment &&
        info.pgm_hash == 0xbb10e440 && !EmulatorSettings.IsPatchShaders() &&
        std::ranges::none_of(info.buffers, [](const auto& buffer) { return buffer.is_written; })) {
        if (MaterialEarlyTests::Convert(source, early_source)) {
            source = early_source;
            static std::atomic<bool> reported_early{false};
            if (!reported_early.exchange(true))
                std::printf("Material early tests: shader %08x, explicit depth/stencil rejection; canonical cache retained\n", u32(info.pgm_hash));
        } else {
            static std::atomic<bool> reported_skip{false};
            if (!reported_skip.exchange(true))
                std::printf("Material early tests: unsupported permutation; original shader retained\n");
        }
    }
    std::vector<u32> bindings;
    for (u32 i = 0; i < info.buffers.size(); ++i)
        if (Use(info, i, instance)) bindings.push_back(first_binding + i);
    if (bindings.empty()) {
        static const bool cse = [] {
            const char* value = std::getenv("BB_MATERIAL_CSE");
            return value && value[0] == '1';
        }();
        if (cse && info.sw_stage == Shader::SwStage::Fragment &&
            info.pgm_hash == 0xbb10e440 && !EmulatorSettings.IsPatchShaders()) {
            const auto optimized = MaterialCse::Match(source);
            if (!optimized.empty()) {
                static std::atomic<bool> reported_cse{false};
                if (!reported_cse.exchange(true))
                    std::printf("Material CSE: exact source match, certified local value reuse; canonical cache retained\n");
                return CompileSPV(optimized, instance.GetDevice());
            }
            static std::atomic<bool> reported_cse_fallback{false};
            if (!reported_cse_fallback.exchange(true))
                std::printf("Material CSE: source not certified; original shader retained\n");
        }
        return CompileSPV(source, instance.GetDevice());
    }
    std::vector<u32> converted;
    ASSERT_MSG(Convert(source, bindings, converted),
               "Material UBO: shader {:#x} contains unsupported addresses or aliases", info.pgm_hash);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
        std::printf("Material UBO: shader %08x, %zu constant buffers; canonical cache retained\n",
                    u32(info.pgm_hash), bindings.size());
    return CompileSPV(converted, instance.GetDevice());
}
} // namespace Vulkan::MaterialUniforms
