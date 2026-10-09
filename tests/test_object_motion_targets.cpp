// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#define NDEBUG
#endif
#include "bbport_settings.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif

int main() {
    BbSettings::Get().object_motion = true;
    BbSettings::Get().upscaler = BbSettings::UpscalerDlss;
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    Vulkan::ObjectMotion motion(instance, scheduler);
    assert(motion.Enabled());
    VideoCore::Buffer readback(instance, 0, 64 * 64 * 16, VideoCore::MemoryType::HostCached);
    auto draw = [&](u32 size, float value) {
        Vulkan::RenderState state{};
        state.width = state.height = size;
        state.num_layers = 1;
        motion.Attach(state, size, size);
        auto& attachment = state.color_attachments[Shader::MotionVectors::Output];
        assert(attachment.is_clear);
        attachment.clear_value.fill(std::bit_cast<u32>(value));
        scheduler.BeginRendering(state);
        scheduler.EndRendering();
        return motion.Image(size, size);
    };
    vk::Image main, aux;
    auto verify = [&](u32 size, float expected) {
        bool valid = false;
        const auto cmd = scheduler.CommandBuffer();
        const auto view = motion.PrepareRead(cmd, size, size, valid);
        assert(view && valid && motion.View() == view);
        const auto image = motion.Image(size, size);
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {size, size, 1},
        };
        cmd.copyImageToBuffer(image, vk::ImageLayout::eGeneral, readback.Handle(), copy);
        runtime.AccessBuffer(&readback, 0, size * size * 16,
                             vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
        runtime.FlushBarriers();
        scheduler.Finish();
        readback.Invalidate(0, size * size * 16);
        for (u32 offset = 0; offset < size * size * 16; offset += 4) {
            float value;
            std::memcpy(&value, readback.mapped_data.data() + offset, 4);
            assert(value == expected);
        }
    };
    for (u32 frame = 0; frame < 8; ++frame) {
        motion.OnFrameStart();
        assert(!motion.Image(64, 64) && !motion.Image(32, 32));
        const auto before = scheduler.CurrentTick();
        const auto a = draw(64, 1.0f);
        const auto b = draw(32, 2.0f);
        assert(scheduler.CurrentTick() == before); // switching sizes must never Finish/Flush
        if (frame == 0) { main = a; aux = b; }
        assert(a == main && b == aux && a != b);
        assert(motion.Image(64, 64) == main && motion.Image(32, 32) == aux);
        Vulkan::RenderState second{};
        second.width = second.height = 64;
        second.num_layers = 1;
        motion.Attach(second, 64, 64);
        assert(!second.color_attachments[Shader::MotionVectors::Output].is_clear);
        assert(scheduler.CurrentTick() == before);
        verify(64, 1.0f);
        verify(32, 2.0f);
    }
    bool valid = true;
    assert(motion.PrepareRead(scheduler.CommandBuffer(), 8, 8, valid) && !valid);
    scheduler.Finish();
    std::puts("Object motion: alternating auxiliary/main sizes, persistent GPU data, no switch submits, per-frame clears and exact-size compose PASS");
}
