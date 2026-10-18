// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual SPIR-V backend without opening a window or loading the game.
// Output is validated by spirv-val (see docs/upscaler.md).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <setjmp.h>
#include <cassert>
#include <cstring>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vertex_input_format.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"


int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);
    using namespace Shader;
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    MotionVectors::params_address = 0x10000;
    MotionVectors::positions_address = 0x20000;
    MotionVectors::position_stride = argc > 2 ? 12 : 16;
    for (bool vertex : {true, false}) {
        for (bool motion : {false, true}) {
            Info info{};
            info.hw_stage = vertex ? HwStage::Vertex : HwStage::Fragment;
            info.sw_stage = vertex ? SwStage::Vertex : SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            if (vertex) {
                runtime.hw.vs.motion_vectors = motion;
            } else {
                runtime.hw.fs.motion_vectors = motion;
                runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
            }
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            IR::IREmitter ir(block);
            const auto output = vertex ? IR::Attribute::Position0 : IR::Attribute::RenderTarget0;
            for (u32 component = 0; component < 4; ++component) {
                info.stores.Set(output, component);
                if (!vertex && component < 2) {
                    info.loads.Set(IR::Attribute::FragCoord, component);
                    ir.SetAttribute(output, ir.GetAttribute(IR::Attribute::FragCoord, component), component);
                } else {
                    const std::array<float, 4> clip{0.125f, -0.25f, 0.75f, 2.0f};
                    ir.SetAttribute(output, ir.Imm32(vertex ? clip[component] : component == 3 ? 1.0f : 0.0f), component);
                }
            }
            ir.Epilogue();
            IR::Program program(info);
            program.blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block},
                                           .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            Profile profile{};
            profile.supported_spirv = 0x00010600;
            profile.support_int64 = true;
            Backend::Bindings bindings{};
            const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
            const auto path = dir / (std::string(vertex ? "vertex" : "fragment") +
                                      (motion ? "-motion.spv" : "-plain.spv"));
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            if (!out) { return 1; }
            if (vertex && motion) {
                // bbport: a cached motion vertex shader is re-addressed for a new session: the
                // three embedded bases (params, params + 16, positions) are replaced, and an
                // identical compile with the new addresses gives the same module.
                auto patched = code;
                MotionVectors::params_address = 0x7f0000100000;
                MotionVectors::positions_address = 0x7f0000200000;
                const u32 replaced = Vulkan::PatchMotionAddresses(patched, 0x10000, 0x20000);
                const auto again = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
                MotionVectors::params_address = 0x10000;
                MotionVectors::positions_address = 0x20000;
                std::printf("motion vertex shader: %u addresses replaced, %s a fresh compile\n",
                            replaced, patched == again ? "identical to" : "DIFFERENT from");
                std::ofstream(dir / "vertex-motion-patched.spv", std::ios::binary)
                    .write(reinterpret_cast<const char*>(patched.data()),
                           patched.size() * sizeof(u32));
                if (replaced != 3 || patched != again) { return 2; }
                // Execute the production vertex epilogue on the actual GPU, with nonzero
                // vertex/instance offsets and guard bytes on both sides of the stored range.
                Vulkan::Instance device_instance(0, false);
                std::puts("motion GPU fixture: instance ready");
                Vulkan::Scheduler scheduler(device_instance);
                VideoCore::Buffer params(device_instance, 0, 96, VideoCore::MemoryType::HostCached);
                VideoCore::Buffer positions(device_instance, 0, 4096, VideoCore::MemoryType::HostCached);
                std::puts("motion GPU fixture: buffers ready");
                std::memset(params.mapped_data.data(), 0, params.mapped_data.size());
                std::memset(positions.mapped_data.data(), 0xcd, positions.mapped_data.size());
                const std::array<u32, 8> draw{5, 100, 3, 3, 20, 4, 2, 0};
                std::memcpy(params.mapped_data.data() + 32, draw.data(), 32);
                params.Flush(0, 96);
                positions.Flush(0, 4096);
                auto gpu_code = code;
                MotionVectors::params_address = params.BufferDeviceAddress();
                MotionVectors::positions_address = positions.BufferDeviceAddress();
                assert(Vulkan::PatchMotionAddresses(gpu_code, 0x10000, 0x20000) == 3);
                std::puts("motion GPU fixture: addresses ready");
                const auto device = device_instance.GetDevice();
                const auto module = device.createShaderModuleUnique(
                    {.codeSize = gpu_code.size() * 4, .pCode = gpu_code.data()}).value;
                const vk::PushConstantRange constants{vk::ShaderStageFlagBits::eVertex, 0, sizeof(PushData)};
                const auto layout = device.createPipelineLayoutUnique(
                    {.pushConstantRangeCount = 1, .pPushConstantRanges = &constants}).value;
                const vk::PipelineShaderStageCreateInfo stage{
                    .stage = vk::ShaderStageFlagBits::eVertex, .module = *module, .pName = "main"};
                const auto [empty_df,empty_nf] = Vulkan::VertexInputDescriptorFormat(AmdGpu::Buffer{});
                const vk::VertexInputAttributeDescription empty_attribute{
                    .location=0,.binding=0,.format=Vulkan::LiverpoolToVK::SurfaceFormat(empty_df,empty_nf)};
                const vk::VertexInputBindingDescription empty_binding{.binding=0,.stride=0};
                const vk::PipelineVertexInputStateCreateInfo input{
                    .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&empty_binding,
                    .vertexAttributeDescriptionCount=1,.pVertexAttributeDescriptions=&empty_attribute};
                const vk::PipelineInputAssemblyStateCreateInfo assembly{
                    .topology = vk::PrimitiveTopology::eTriangleList};
                const vk::PipelineRasterizationStateCreateInfo raster{
                    .rasterizerDiscardEnable = true, .lineWidth = 1};
                const vk::PipelineRenderingCreateInfo rendering{};
                const auto pipeline = device.createGraphicsPipelineUnique({}, {
                    .pNext = &rendering, .stageCount = 1, .pStages = &stage,
                    .pVertexInputState = &input, .pInputAssemblyState = &assembly,
                    .pRasterizationState = &raster, .layout = *layout}).value;
                std::puts("motion GPU fixture: pipeline ready");
                PushData push{};
                push.motion_param = 1;
                const auto command = scheduler.CommandBuffer();
                command.beginRendering({.renderArea = {{0, 0}, {1, 1}}, .layerCount = 1});
                command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
                command.bindVertexBuffers(0, vk::Buffer{}, vk::DeviceSize{0});
                command.pushConstants(*layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
                command.draw(3, 2, 20, 4);
                command.draw(3, 2, 30, 4); // out of stored vertex range: must touch no bytes
                command.endRendering();
                const vk::MemoryBarrier2 host_read{
                    .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                    .dstAccessMask = vk::AccessFlagBits2::eHostRead};
                command.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host_read});
                scheduler.Finish();
                positions.Invalidate(0, 4096);
                auto expected = std::vector<u8>(4096, 0xcd);
                const std::array<float, 4> full{0.125f, -0.25f, 0.75f, 2.0f};
                const std::array<float, 3> compact{0.125f, -0.25f, 2.0f};
                for (u32 vertex_id = 5; vertex_id < 11; ++vertex_id) {
                    const void* value = MotionVectors::position_stride == 12
                        ? static_cast<const void*>(compact.data()) : static_cast<const void*>(full.data());
                    std::memcpy(expected.data() + vertex_id * MotionVectors::position_stride,
                                value, MotionVectors::position_stride);
                }
                assert(std::memcmp(expected.data(), positions.mapped_data.data(), 4096) == 0);
                std::puts("Null optional vertex binding: production fallback format, Vulkan draw and full-buffer side effects PASS");
                std::printf("motion vertex GPU storage: stride %u, offsets/range guards/full-buffer bytes PASS\n",
                            MotionVectors::position_stride);
                MotionVectors::params_address = 0x10000;
                MotionVectors::positions_address = 0x20000;
            }
        }
    }
}
