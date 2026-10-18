// SPDX-License-Identifier: GPL-2.0-or-later
#include <unordered_map>
#include <spirv/unified1/spirv.hpp11>
#include "video_core/renderer_vulkan/material_early_tests.h"

namespace Vulkan::MaterialEarlyTests {
bool Convert(std::span<const std::uint32_t> source, std::vector<std::uint32_t>& output) {
    output.clear();
    if (source.size() < 5 || source[0] != spv::MagicNumber) return false;
    using Word = std::uint32_t;
    using Instruction = std::span<const Word>;
    std::unordered_map<Word, spv::StorageClass> pointer_classes;
    std::unordered_map<Word, Word> value_types;
    Word entry = 0;
    bool already_early = false;
    size_t insertion = 0;
    for (size_t at = 5; at < source.size();) {
        const Word size = source[at] >> 16;
        if (!size || size > source.size() - at) return false;
        const Instruction i = source.subspan(at, size);
        const auto op = static_cast<spv::Op>(i[0] & 0xffff);
        if (op == spv::Op::OpEntryPoint) {
            if (size < 4 || entry || i[1] != Word(spv::ExecutionModel::Fragment)) return false;
            entry = i[2];
            insertion = at + size;
        }
        if (op == spv::Op::OpExecutionMode) {
            if (size < 3 || i[1] != entry) return false;
            insertion = at + size;
            const auto mode = static_cast<spv::ExecutionMode>(i[2]);
            if (mode == spv::ExecutionMode::EarlyFragmentTests) already_early = true;
            // Restrict coverage/order modes conservatively; float control modes
            // and OriginUpperLeft are safe and retained exactly.
            if (mode == spv::ExecutionMode::DepthReplacing ||
                mode == spv::ExecutionMode::DepthGreater || mode == spv::ExecutionMode::DepthLess ||
                mode == spv::ExecutionMode::DepthUnchanged ||
                mode == spv::ExecutionMode::PostDepthCoverage ||
                mode == spv::ExecutionMode::EarlyAndLateFragmentTestsAMD) return false;
        }
        if (op == spv::Op::OpTypePointer && size == 4)
            pointer_classes[i[1]] = static_cast<spv::StorageClass>(i[2]);
        if ((op == spv::Op::OpVariable || op == spv::Op::OpAccessChain ||
             op == spv::Op::OpInBoundsAccessChain || op == spv::Op::OpPtrAccessChain ||
             op == spv::Op::OpInBoundsPtrAccessChain) && size >= 3)
            value_types[i[2]] = i[1];
        if (op == spv::Op::OpDecorate && size >= 4 &&
            i[2] == Word(spv::Decoration::BuiltIn)) {
            const auto builtin = static_cast<spv::BuiltIn>(i[3]);
            if (builtin == spv::BuiltIn::FragDepth || builtin == spv::BuiltIn::SampleMask ||
                builtin == spv::BuiltIn::FragStencilRefEXT ||
                builtin == spv::BuiltIn::HelperInvocation) return false;
        }
        // Built-ins in interface blocks are deliberately unsupported.
        if (op == spv::Op::OpMemberDecorate && size >= 5 &&
            i[3] == Word(spv::Decoration::BuiltIn)) return false;
        if (op == spv::Op::OpTypeImage && size >= 4 &&
            i[3] == Word(spv::Dim::SubpassData)) return false;
        if (op == spv::Op::OpStore) {
            if (size != 3) return false;
            const auto value = value_types.find(i[1]);
            if (value == value_types.end()) return false;
            const auto pointer = pointer_classes.find(value->second);
            if (pointer == pointer_classes.end() || pointer->second != spv::StorageClass::Output)
                return false;
        }
        if (op == spv::Op::OpKill || op == spv::Op::OpTerminateInvocation ||
            op == spv::Op::OpDemoteToHelperInvocation || op == spv::Op::OpIsHelperInvocationEXT ||
            op == spv::Op::OpImageWrite || op == spv::Op::OpImageRead ||
            op == spv::Op::OpCopyMemory || op == spv::Op::OpCopyMemorySized ||
            op == spv::Op::OpFunctionCall || op == spv::Op::OpControlBarrier ||
            op == spv::Op::OpMemoryBarrier || op == spv::Op::OpExecutionModeId ||
            (op >= spv::Op::OpAtomicLoad && op <= spv::Op::OpAtomicXor) ||
            op == spv::Op::OpAtomicFlagTestAndSet || op == spv::Op::OpAtomicFlagClear ||
            op == spv::Op::OpAtomicFAddEXT || op == spv::Op::OpAtomicFMinEXT ||
            op == spv::Op::OpAtomicFMaxEXT ||
            (op >= spv::Op::OpGroupNonUniformElect && op <= spv::Op::OpGroupNonUniformQuadSwap))
            return false;
        at += size;
    }
    if (!entry || !insertion) return false;
    output.assign(source.begin(), source.begin() + insertion);
    if (!already_early) {
        output.insert(output.end(), {(3u << 16) | Word(spv::Op::OpExecutionMode), entry,
                                     Word(spv::ExecutionMode::EarlyFragmentTests)});
    }
    output.insert(output.end(), source.begin() + insertion, source.end());
    return true;
}
}
