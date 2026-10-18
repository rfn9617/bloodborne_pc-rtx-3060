// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NDEBUG
#define NDEBUG
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <SDL3/SDL.h>
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include <windows.h>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr,"FAIL: %s:%d %s\n",__FILE__,__LINE__,#v); std::abort(); } } while (false)
int main() {
    Frontend::WindowSDL window(640,360,"Bloodborne FG presentation check");
    Vulkan::Instance instance(window,0,true);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device=instance.GetDevice();
    Vulkan::Scheduler scheduler(instance);
    std::fprintf(stderr,"WSI instance/scheduler ready\n");
    Vulkan::Swapchain swapchain(instance,window);
    std::printf("Production WSI: present wait=%d images=%u\n",instance.SupportsPresentWait(),swapchain.GetImageCount());
    auto fence=Vulkan::Check(device.createFenceUnique({.flags=vk::FenceCreateFlagBits::eSignaled},nullptr,d));
    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
    for(u32 frame=0;frame<40;++frame) {
        SDL_PumpEvents();
        if(frame==20) {
            SDL_SetWindowSize(window.GetSDLWindow(),720,400);
            SDL_PumpEvents();
            swapchain.Recreate(720,400);
        }
        swapchain.WaitPreviousPresent();
        CHECK(device.waitForFences(*fence,true,1'000'000'000,d)==vk::Result::eSuccess);
        for(u32 slot=0;slot<2;++slot) {
            CHECK(swapchain.AcquireNextImage());
            const bool final=slot==1;
            if(final) CHECK(device.resetFences(*fence,d)==vk::Result::eSuccess);
            const auto cmd=scheduler.CommandBuffer();
            vk::ImageMemoryBarrier2 barrier{
                .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=swapchain.Image(),.subresourceRange=range};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},d);
            cmd.clearColorImage(swapchain.Image(),vk::ImageLayout::eTransferDstOptimal,
                vk::ClearColorValue{std::array<float,4>{0.05f,0.08f,0.12f,1.0f}},range,d);
            barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
            barrier.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;
            barrier.dstAccessMask=vk::AccessFlagBits2::eNone;
            barrier.oldLayout=vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout=vk::ImageLayout::ePresentSrcKHR;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},d);
            Vulkan::SubmitInfo info{};
            info.AddWait(swapchain.GetImageAcquiredSemaphore());
            info.AddSignal(swapchain.GetPresentReadySemaphore());
            if(final) info.AddSignal(*fence);
            scheduler.Flush(info);
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            CHECK(swapchain.Present());
        }
    }
    swapchain.WaitPreviousPresent(); scheduler.Finish();
    CHECK(device.waitForFences(*fence,true,1'000'000'000,d)==vk::Result::eSuccess);
    std::puts("PASS: 40 generated/real FIFO pairs, final-slot fence, present IDs/waits and swapchain resize");
}
