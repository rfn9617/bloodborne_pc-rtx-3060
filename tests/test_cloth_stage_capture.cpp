// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise diagnostic transfers: HDR byte counts, depth aspect, frame IDs and
// the original final-frame recording path on the actual Vulkan device.
#include <array>
#include <cstdio>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_fg_motion_capture.h"
#include "video_core/texture_cache/image.h"
#ifdef _WIN32
#undef NDEBUG
#endif

int main() {
    Vulkan::Instance instance(0,true);
    Vulkan::Scheduler scheduler(instance);
    const auto device=instance.GetDevice();
    const std::array formats{vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR16G16B16A16Sfloat,vk::Format::eD32Sfloat,
        vk::Format::eR16G16Sfloat,vk::Format::eR8G8B8A8Unorm};
    std::array<VideoCore::UniqueImage,5> images;
    for(u32 i=0;i<5;++i) {
        const bool full=i==1 || i==4;
        images[i]=VideoCore::UniqueImage(device,instance.GetAllocator());
        images[i].Create({.imageType=vk::ImageType::e2D,.format=formats[i],
            .extent={full?1920u:1280u,full?1080u:720u,1},.mipLevels=1,.arrayLayers=1,
            .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
            .usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst});
        const auto aspect=i==2?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor;
        const vk::ImageMemoryBarrier2 transition{
            .srcStageMask=vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=vk::Image(images[i]),.subresourceRange={aspect,0,1,0,1}};
        const auto cmd=scheduler.CommandBuffer();
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&transition});
        if(i==2) cmd.clearDepthStencilImage(vk::Image(images[i]),vk::ImageLayout::eGeneral,
            {0.75f,0},vk::ImageSubresourceRange{aspect,0,1,0,1});
        else {
            const float value=i==1?0.25f:i==3?0.5f:1.f;
            cmd.clearColorImage(vk::Image(images[i]),vk::ImageLayout::eGeneral,
                vk::ClearColorValue(std::array{value,value,value,value}),
                vk::ImageSubresourceRange{aspect,0,1,0,1});
        }
    }
    Vulkan::FgMotionCapture scene(instance,scheduler,true),final(instance,scheduler);
    Vulkan::FrameGenerator::DiagnosticInputs inputs{};
    inputs.motion=vk::Image(images[3]);inputs.width=1280;inputs.height=720;
    inputs.frame_id=101;inputs.frame_ms=13.8889f;inputs.jitter={0.25f,-0.125f};
    BbStats::fg_motion_capture_seq.store(1);
    for(u32 n=0;n<2;++n) {
        const auto cmd=scheduler.CommandBuffer();
        scene.RecordScene(cmd,
            {vk::Image(images[0]),vk::ImageLayout::eGeneral,1280,720,formats[0],"scene_input"},
            {vk::Image(images[1]),vk::ImageLayout::eGeneral,1920,1080,formats[1],"scene_output"},
            {vk::Image(images[2]),vk::ImageLayout::eGeneral,1280,720,formats[2],"scene_depth",vk::ImageAspectFlagBits::eDepth},
            {vk::Image(images[3]),vk::ImageLayout::eGeneral,1280,720,formats[3],"scene_motion"},inputs);
        final.Record(cmd,
            {vk::Image(images[4]),vk::ImageLayout::eGeneral,1920,1080,formats[4],"real_present"},
            {n==0?vk::Image(images[4]):vk::Image{},vk::ImageLayout::eGeneral,1920,1080,formats[4],"generated_present"},inputs);
        ++inputs.frame_id;
    }
    scene.Drain();final.Drain();
    std::puts("Cloth stage capture Vulkan transfers completed; inspect binary streams and metadata.");
}
