// Off-screen integration: the production DLSS wrapper, model K and Quality dimensions.
#include <array>
#include <cmath>
#include <cstdio>
#define NDEBUG
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"
#undef NDEBUG
#include <cassert>

int main() {
    using namespace Vulkan;
    BbSettings::Load();
    assert(BbSettings::Get().dlss_model == 11);
    assert(BbSettings::Get().alpha_detail == 0);
    Instance instance(0, false);
    auto* dlss = Dlss::Get();
    assert(dlss && dlss->Available());
    Scheduler scheduler(instance);
    const auto device = instance.GetDevice();
    std::array<VideoCore::UniqueImage, 4> images;
    std::array<vk::UniqueImageView, 4> views;
    std::array<Dlss::Resource, 4> resources;
    for (u32 n = 0; n < 4; ++n) {
        const auto format = n == 1 ? vk::Format::eD32Sfloat : n == 2
            ? vk::Format::eR16G16Sfloat : vk::Format::eR16G16B16A16Sfloat;
        const u32 width = n == 3 ? 1920 : 1440, height = n == 3 ? 1080 : 810;
        const auto aspect = n == 1 ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
        images[n] = VideoCore::UniqueImage(device, instance.GetAllocator());
        images[n].Create({.imageType=vk::ImageType::e2D,.format=format,.extent={width,height,1},
            .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                (n == 3 ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled)});
        views[n] = device.createImageViewUnique({.image=vk::Image(images[n]),
            .viewType=vk::ImageViewType::e2D,.format=format,
            .subresourceRange={aspect,0,1,0,1}}).value;
        resources[n]={vk::Image(images[n]), *views[n], format, aspect, width, height};
    }
    const auto cmd=scheduler.CommandBuffer();
    for (u32 n=0;n<4;++n) {
        const vk::ImageSubresourceRange range{resources[n].aspect,0,1,0,1};
        const vk::ImageMemoryBarrier2 initial{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .image=resources[n].image,.subresourceRange=range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&initial});
        if (n==1) cmd.clearDepthStencilImage(resources[n].image,vk::ImageLayout::eGeneral,{0.5f,0},range);
        else cmd.clearColorImage(resources[n].image,vk::ImageLayout::eGeneral,
            vk::ClearColorValue(std::array<float,4>{n==0 ? .25f : 0.f,n==0 ? .25f : 0.f,n==0 ? .25f : 0.f,1.f}),range);
    }
    const vk::MemoryBarrier2 ready{
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,.dstAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&ready});
    const Dlss::FeatureDesc feature{1440,810,1920,1080,Dlss::QualityForScale((4.f/3.f)),false,11};
    assert(dlss->CreateFeature(cmd,feature) && dlss->HasFeature(feature));
    assert(dlss->Evaluate(cmd,{resources[0],resources[1],resources[2],resources[3],0,0,true,13.8889f,0}));
    VideoCore::Buffer readback(instance,0,1920*1080*8,VideoCore::MemoryType::HostCached);
    cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&ready});
    const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                                   .imageExtent={1920,1080,1}};
    cmd.copyImageToBuffer(resources[3].image,vk::ImageLayout::eGeneral,readback.Handle(),copy);
    const vk::MemoryBarrier2 host{
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
    scheduler.Finish(); readback.Invalidate(0,1920*1080*8);
    const auto* values=reinterpret_cast<const u16*>(readback.mapped_data.data());
    double mean=0;
    for (u32 n=0;n<1920*1080;++n) {
        const u16 bits=values[n*4]; assert((bits&0x7c00)!=0x7c00);
        const float value=std::ldexp(float((bits&1023)+1024),int((bits>>10)&31)-25);
        mean += value;
    }
    mean/=1920*1080;
    assert(std::abs(mean-.25)<.05);
    dlss->ReleaseFeature(); dlss->Shutdown();
    std::printf("PASS: production DLSS K Ultra Quality 1440x810 -> 1920x1080, evaluation and finite GPU readback (mean %.5f).\n",mean);
}
