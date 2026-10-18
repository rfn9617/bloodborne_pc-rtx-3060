// SPDX-License-Identifier: GPL-2.0-or-later
// Production FG capture/dispatch/UI composition and menu release, without a game.
#ifndef NDEBUG
#define NDEBUG
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_generator.h"
#include "video_core/renderer_vulkan/vk_fg_motion_capture.h"
#include <vk_mem_alloc.h>
#include "bbport_toggles.h"

#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort();} } while(0)

int main(int argc, char** argv) {
    const bool stress = argc > 1 && std::strcmp(argv[1], "--stress") == 0;
    const bool captureSequence=argc>1 && std::strcmp(argv[1],"--capture-sequence")==0;
    const bool verticalMotion = argc > 1 && std::strcmp(argv[1], "--vertical-motion") == 0;
    const float shiftX=verticalMotion ? 0.0f : 12.0f, shiftY=verticalMotion ? 12.0f : 0.0f;
    const bool hudMotion = argc > 1 && std::strcmp(argv[1], "--hud-motion") == 0;
    const bool crossFormat=argc>1 && std::strcmp(argv[1],"--replay-cross-format")==0;
    const bool displayFormatFix=[] {const char* v=std::getenv("BB_FG_DISPLAY_FORMAT_FIX");return v && v[0]=='1';}();
    const bool replaySrgb = crossFormat || argc > 1 && std::strcmp(argv[1], "--replay-srgb") == 0;
    const bool replayHud = replaySrgb || argc > 1 && std::strcmp(argv[1], "--replay-hud") == 0;
    const bool finalColor = argc > 1 && std::strcmp(argv[1], "--final-color") == 0;
    const bool present = captureSequence || verticalMotion || finalColor || hudMotion || replayHud || argc > 1 && std::strcmp(argv[1], "--present") == 0;
    const bool moving = present || (argc > 1 && std::strcmp(argv[1], "--motion") == 0);
    constexpr size_t frameBytes = 1920u * 1080u * 4u;
    constexpr size_t uploadOffset = 64u, readbackOffset = uploadOffset + frameBytes;
    const auto texture = [](float x, float y) {
        const auto channel = [](float v) { return u32(std::clamp(std::lround(v), 0l, 255l)); };
        const u32 r = channel(128 + 70 * std::sin(x / 17.0f) * std::cos(y / 31.0f));
        const u32 g = channel(128 + 70 * std::cos(x / 29.0f + y / 43.0f));
        const u32 b = channel(128 + 70 * std::sin(x / 37.0f - y / 19.0f));
        return 0xff000000u | r | (g << 8) | (b << 16);
    };
    const auto grade=[](u32 pixel) {
        // An asymmetric, dark final-game transform: detects early-stage capture,
        // swapped red/blue and an extra gamma pass in either output frame.
        const u32 r=20+((pixel&255)*110+127)/255;
        const u32 g=10+(((pixel>>8)&255)*90+127)/255;
        const u32 b=5+(((pixel>>16)&255)*70+127)/255;
        return 0xff000000u | r | (g<<8) | (b<<16);
    };
    const auto expectedTexture=[&](float x,float y) {
        const auto color=texture(x,y);
        if(crossFormat) return (color&0xff00ff00u)|((color&255)<<16)|((color>>16)&255);
        return finalColor ? grade(color) : color;
    };
    const auto displayColor=[&](u32 color) {return crossFormat?(color&0xff00ff00u)|((color&255)<<16)|((color>>16)&255):color;};
    constexpr size_t displayUploadOffset=readbackOffset+frameBytes;
    constexpr size_t hudlessUploadOffset=displayUploadOffset+frameBytes;
    constexpr size_t uiUploadOffset=hudlessUploadOffset+frameBytes;
    const auto hudPixel=[](u32 x,u32 y,u32 background) {
        if(x>=50 && x<350 && y>=40 && y<160 && ((x/7+y/11)%3==0)) return 0xff20e040u;
        if(x>=50 && x<350 && y>=40 && y<160) {
            // Semi-transparent panel, separate from its opaque glyphs.
            u32 result=0xff000000u;
            for(u32 c=0;c<3;++c) result|=((((background>>(8*c))&255)+20)/2)<<(8*c);
            return result;
        }
        return background;
    };
    using namespace Vulkan;
    CHECK(FrameGenerator::Requested());
    Instance instance(0,true);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance()); dispatch.init(instance.GetDevice());
    Scheduler scheduler(instance);
    FrameGenerator fg(instance,scheduler);
    const auto device=instance.GetDevice();
    const auto allocator=instance.GetAllocator();
    VideoCore::UniqueImage scene(device,allocator),ui(device,allocator),depth(device,allocator),motion(device,allocator);
    const auto create=[&](VideoCore::UniqueImage& image,u32 w,u32 h,vk::Format format,bool is_depth) {
        image.Create({.flags=vk::ImageCreateFlagBits::eMutableFormat,.imageType=vk::ImageType::e2D,.format=format,.extent={w,h,1},
            .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .usage=vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                   vk::ImageUsageFlagBits::eTransferDst | (is_depth?vk::ImageUsageFlagBits::eDepthStencilAttachment:vk::ImageUsageFlagBits::eStorage)});
    };
    create(scene,1920,1080,vk::Format::eR8G8B8A8Unorm,false);
    create(ui,1920,1080,vk::Format::eR8G8B8A8Unorm,false);
    create(depth,1280,720,vk::Format::eD32SfloatS8Uint,true);
    create(motion,1280,720,vk::Format::eR16G16Sfloat,false);
    const auto view=[&](VideoCore::UniqueImage& image,bool is_depth) {
        return Check(device.createImageViewUnique({.image=image.image,.viewType=vk::ImageViewType::e2D,
            .format=image.image_ci.format,.subresourceRange={is_depth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,dispatch));
    };
    auto scene_view=view(scene,false),ui_view=view(ui,false),depth_view=view(depth,true);
    VkBuffer host{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(finalColor || hudMotion || replayHud) ? uiUploadOffset+frameBytes : moving ? readbackOffset + frameBytes : 64,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    CHECK(vmaCreateBuffer(allocator,&bi,&ac,&host,&allocation,&info)==VK_SUCCESS);
    const u32 hud=0xff00ff00;
    std::memcpy(info.pMappedData,&hud,4);
    vmaFlushAllocation(allocator,allocation,0,4);
    const auto barrier=[&](vk::CommandBuffer cmd,vk::Image image,bool is_depth,vk::ImageLayout old_layout,vk::ImageLayout new_layout) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=old_layout,.newLayout=new_layout,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
            .subresourceRange={is_depth?vk::ImageAspectFlagBits::eDepth|vk::ImageAspectFlagBits::eStencil:vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b},dispatch);
    };
    {
        const auto cmd=scheduler.CommandBuffer();
        for(auto* image : {&scene,&ui,&depth,&motion}) {
            const bool is_depth=image==&depth;
            barrier(cmd,image->image,is_depth,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
            if(is_depth) cmd.clearDepthStencilImage(image->image,vk::ImageLayout::eTransferDstOptimal,
                vk::ClearDepthStencilValue{.depth=0.5f,.stencil=0},vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth|vk::ImageAspectFlagBits::eStencil,0,1,0,1},dispatch);
            else cmd.clearColorImage(image->image,vk::ImageLayout::eTransferDstOptimal,
                vk::ClearColorValue{image==&motion?std::array<float,4>{0,0,0,0}:std::array<float,4>{0.25f,0.5f,0.75f,1}},
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1},dispatch);
            barrier(cmd,image->image,is_depth,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
        }
        barrier(cmd,ui.image,false,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferDstOptimal);
        const vk::BufferImageCopy patch{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageOffset={20,20,0},.imageExtent={1,1,1}};
        cmd.copyBufferToImage(host,ui.image,vk::ImageLayout::eTransferDstOptimal,patch,dispatch);
        barrier(cmd,ui.image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
        scheduler.Finish();
    }
    VideoCore::UniqueImage presented(device,allocator);
    vk::UniqueImageView presentedView;
    HostPasses::PostProcessingPass postProcess;
    if(present) {
        presented.Create({.imageType=vk::ImageType::e2D,.format=vk::Format::eR8G8B8A8Unorm,
            .extent={1920,1080,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .usage=vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc});
        presentedView=view(presented,false);
        postProcess.Create(device,vk::Format::eR8G8B8A8Unorm);
    }
    VideoCore::UniqueImage finalDisplay(device,allocator);
    vk::UniqueImageView finalDisplayView, finalDisplayRaw;
    if(finalColor || replayHud) {
        finalDisplay.Create({.flags=vk::ImageCreateFlagBits::eMutableFormat,.imageType=vk::ImageType::e2D,.format=crossFormat?vk::Format::eR8G8B8A8Srgb:replaySrgb ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm,
            .extent={1920,1080,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eColorAttachment});
        finalDisplayView=view(finalDisplay,false);
        finalDisplayRaw=Check(device.createImageViewUnique({.image=finalDisplay.image,.viewType=vk::ImageViewType::e2D,
            .format=vk::Format::eB8G8R8A8Unorm,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,dispatch));
    }
    VideoCore::UniqueImage hudlessFinal(device,allocator);
    vk::UniqueImageView hudlessFinalView;
    HostPasses::PostProcessingPass displayPass;
    vk::UniqueImageView uiSrgb;
    if(finalColor) {
        hudlessFinal.Create({.imageType=vk::ImageType::e2D,.format=vk::Format::eB8G8R8A8Unorm,
            .extent={1920,1080,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst});
        hudlessFinalView=view(hudlessFinal,false);
    }
    if(replayHud) {
        displayPass.Create(device,crossFormat?vk::Format::eR8G8B8A8Srgb:replaySrgb ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm);
        const vk::ImageViewUsageCreateInfo usage{.usage=vk::ImageUsageFlagBits::eSampled};
        uiSrgb=Check(device.createImageViewUnique({.pNext=&usage,.image=ui.image,
            .viewType=vk::ImageViewType::e2D,.format=vk::Format::eR8G8B8A8Srgb,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,dispatch));
    }
    VmaTotalStatistics before{}; vmaCalculateStatistics(allocator,&before);
    std::unique_ptr<FgMotionCapture> sequence;
    if(captureSequence) {CHECK(FgMotionCapture::Requested());sequence=std::make_unique<FgMotionCapture>(instance,scheduler);}
    CHECK(fg.Prepare(1280,720));
    const std::array<std::array<float,3>,4> camera{{{0,0,0},{0,1,0},{1,0,0},{0,0,1}}};
    for(u32 frame=0;frame<(stress ? 2050u : moving ? 32u : 8u);++frame) {
        CHECK(fg.Prepare(1280,720));
        if(moving) {
            auto* pixels = reinterpret_cast<u32*>(static_cast<char*>(info.pMappedData) + uploadOffset);
            for(u32 y=0;y<1080;++y) for(u32 x=0;x<1920;++x)
                pixels[y*1920+x] = texture(float(x)-float(frame)*shiftX,float(y)-float(frame)*shiftY);
            vmaFlushAllocation(allocator,allocation,uploadOffset,frameBytes);
            const auto cmd=scheduler.CommandBuffer();
            const vk::BufferImageCopy full{.bufferOffset=uploadOffset,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1920,1080,1}};
            for(auto* image : {&scene,&ui}) {
                barrier(cmd,image->image,false,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferDstOptimal);
                cmd.copyBufferToImage(host,image->image,vk::ImageLayout::eTransferDstOptimal,full,dispatch);
                if(image==&ui) {
                    barrier(cmd,image->image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eTransferDstOptimal);
                    const vk::BufferImageCopy patch{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                        .imageOffset={20,20,0},.imageExtent={1,1,1}};
                    cmd.copyBufferToImage(host,image->image,vk::ImageLayout::eTransferDstOptimal,patch,dispatch);
                }
                barrier(cmd,image->image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
            }
            if(hudMotion || replayHud) {
                auto* pixels=reinterpret_cast<u32*>(static_cast<char*>(info.pMappedData)+uiUploadOffset);
                for(u32 y=0;y<1080;++y) for(u32 x=0;x<1920;++x)
                    pixels[y*1920+x]=hudPixel(x,y,texture(float(x)-float(frame)*shiftX,float(y)-float(frame)*shiftY));
                pixels[20*1920+20]=hud;
                vmaFlushAllocation(allocator,allocation,uiUploadOffset,frameBytes);
                barrier(cmd,ui.image,false,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferDstOptimal);
                const vk::BufferImageCopy fullUi{.bufferOffset=uiUploadOffset,
                    .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1920,1080,1}};
                cmd.copyBufferToImage(host,ui.image,vk::ImageLayout::eTransferDstOptimal,fullUi,dispatch);
                barrier(cmd,ui.image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
            }
            // Previous-minus-current movement: 12 output pixels = 8 render pixels.
            barrier(cmd,motion.image,false,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferDstOptimal);
            cmd.clearColorImage(motion.image,vk::ImageLayout::eTransferDstOptimal,
                vk::ClearColorValue{std::array<float,4>{frame ? -shiftX/1.5f : 0.0f,frame ? -shiftY/1.5f : 0.0f,0,0}},
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,0,1,0,1},dispatch);
            barrier(cmd,motion.image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
        }
        if(finalColor) {
            auto* pixels=reinterpret_cast<u32*>(static_cast<char*>(info.pMappedData)+displayUploadOffset);
            const auto packBgra=[](u32 p) { return (p&0xff00ff00u)|((p&255)<<16)|((p>>16)&255); };
            for(u32 y=0;y<1080;++y) for(u32 x=0;x<1920;++x)
                pixels[y*1920+x]=packBgra(expectedTexture(float(x)-float(frame)*shiftX,float(y)-float(frame)*shiftY));
            pixels[20*1920+20]=packBgra(grade(hud));
            vmaFlushAllocation(allocator,allocation,displayUploadOffset,frameBytes);
            const auto cmd=scheduler.CommandBuffer();
            barrier(cmd,finalDisplay.image,false,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
            const vk::BufferImageCopy full{.bufferOffset=displayUploadOffset,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1920,1080,1}};
            cmd.copyBufferToImage(host,finalDisplay.image,vk::ImageLayout::eTransferDstOptimal,full,dispatch);
            barrier(cmd,finalDisplay.image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
        }
        if(finalColor) {
            auto* pixels=reinterpret_cast<u32*>(static_cast<char*>(info.pMappedData)+hudlessUploadOffset);
            for(u32 y=0;y<1080;++y) for(u32 x=0;x<1920;++x) {
                const u32 c=expectedTexture(float(x)-float(frame)*shiftX,float(y)-float(frame)*shiftY);
                pixels[y*1920+x]=(c&0xff00ff00u)|((c&255)<<16)|((c>>16)&255);
            }
            vmaFlushAllocation(allocator,allocation,hudlessUploadOffset,frameBytes);
            const auto cmd=scheduler.CommandBuffer();
            barrier(cmd,hudlessFinal.image,false,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
            const vk::BufferImageCopy full{.bufferOffset=hudlessUploadOffset,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1920,1080,1}};
            cmd.copyBufferToImage(host,hudlessFinal.image,vk::ImageLayout::eTransferDstOptimal,full,dispatch);
            barrier(cmd,hudlessFinal.image,false,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
        }
        fg.Inputs(*depth_view,motion.image,1280,720,0.1f,1.0472f,{},camera,1000.0f/72.0f,frame==0 || frame==4);
        fg.CaptureScene(*scene_view,vk::ImageLayout::eGeneral,1920,1080);
        fg.Boundary();
        vk::ImageView hudless=finalColor ? *hudlessFinalView : *scene_view;
        if(replayHud) {
            const auto target=fg.BeginDisplayReplay(crossFormat?vk::Format::eR8G8B8A8Srgb:replaySrgb ? vk::Format::eB8G8R8A8Srgb : vk::Format::eB8G8R8A8Unorm);
            CHECK(target);
            const auto clean=fg.CleanView(vk::Format::eR8G8B8A8Srgb,{});
            CHECK(clean);
            const auto cmd=scheduler.CommandBuffer();
            // Exercise the actual render attachment / sampled view replay API.
            Frame replay{.width=1920,.height=1080,.image=fg.OutputImage(),.image_view=target};
            barrier(cmd,fg.CleanImage(),false,vk::ImageLayout::eGeneral,vk::ImageLayout::eShaderReadOnlyOptimal);
            barrier(cmd,fg.OutputImage(),false,vk::ImageLayout::eGeneral,vk::ImageLayout::eColorAttachmentOptimal);
            displayPass.Render(cmd,clean,{1920,1080},replay,{.hdr=replaySrgb ? 1u : 0u});
            barrier(cmd,fg.CleanImage(),false,vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageLayout::eGeneral);
            fg.EndDisplayReplay();
            hudless=fg.ReplayedDisplay(crossFormat && displayFormatFix?vk::Format::eB8G8R8A8Srgb:vk::Format::eUndefined); CHECK(hudless);
            barrier(cmd,finalDisplay.image,false,vk::ImageLayout::eUndefined,vk::ImageLayout::eColorAttachmentOptimal);
            Frame real{.width=1920,.height=1080,.image=finalDisplay.image,.image_view=*finalDisplayView};
            barrier(cmd,ui.image,false,vk::ImageLayout::eGeneral,vk::ImageLayout::eShaderReadOnlyOptimal);
            displayPass.Render(cmd,*uiSrgb,{1920,1080},real,{.hdr=replaySrgb ? 1u : 0u});
            barrier(cmd,ui.image,false,vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageLayout::eGeneral);
        }
        fg.GenerateFinal((finalColor || replayHud) ? *finalDisplayRaw : *ui_view,hudless,vk::ImageLayout::eGeneral,1920,1080);
        const auto output=fg.Compose((finalColor || replayHud) ? *finalDisplayRaw : *ui_view,vk::ImageLayout::eGeneral,present);
        CHECK(bool(output)==(frame!=0 && frame!=4)); // Resets establish history; no fake generated frame.
        if(output) {
            const auto cmd=scheduler.CommandBuffer();
            const vk::Image readImage=present ? presented.image : fg.OutputImage();
            if(present) {
                barrier(cmd,presented.image,false,vk::ImageLayout::eUndefined,vk::ImageLayout::eColorAttachmentOptimal);
                Frame outputFrame{.width=1920,.height=1080,.image=presented.image,.image_view=*presentedView};
                fg.TimingMark(14);
                postProcess.Render(cmd,output,{1920,1080},outputFrame,{});
                fg.TimingMark(15);
            }
            if(sequence) {
                if(frame==1) BbStats::fg_motion_capture_seq.fetch_add(1);
                // Baseline-shaped records also exercise the absent-FG case.
                const bool recordFg=frame<16 || frame>19;
                const u32 repeats=frame==10?12u:1u; // bounded pool must drop, never wait.
                for(u32 repeat=0;repeat<repeats;++repeat) sequence->Record(cmd,
                    {ui.image,vk::ImageLayout::eGeneral,1920,1080,vk::Format::eR8G8B8A8Unorm,"real_present"},
                    {recordFg?presented.image:vk::Image{},vk::ImageLayout::eGeneral,1920,1080,vk::Format::eR8G8B8A8Unorm,"generated_present"},
                    recordFg?fg.GetDiagnosticInputs():FrameGenerator::DiagnosticInputs{});
            }
            const auto readableLayout=present ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal;
            barrier(cmd,readImage,false,readableLayout,vk::ImageLayout::eTransferSrcOptimal);
            const std::array regions{
                vk::BufferImageCopy{.bufferOffset=16,.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageOffset={20,20,0},.imageExtent={1,1,1}},
                vk::BufferImageCopy{.bufferOffset=20,.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageOffset={100,100,0},.imageExtent={1,1,1}}};
            cmd.copyImageToBuffer(readImage,vk::ImageLayout::eTransferSrcOptimal,host,regions,dispatch);
            if(moving) {
                const vk::BufferImageCopy full{.bufferOffset=readbackOffset,
                    .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1920,1080,1}};
                cmd.copyImageToBuffer(readImage,vk::ImageLayout::eTransferSrcOptimal,host,full,dispatch);
            }
            barrier(cmd,readImage,false,vk::ImageLayout::eTransferSrcOptimal,readableLayout);
            const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b},dispatch);
            fg.FinishTimings();
            scheduler.Finish();
            vmaInvalidateAllocation(allocator,allocation,16,8);
            u32 pixel{},background{};
            std::memcpy(&pixel,static_cast<char*>(info.pMappedData)+16,4);
            std::memcpy(&background,static_cast<char*>(info.pMappedData)+20,4);
            CHECK(pixel==(finalColor ? grade(hud) : displayColor(hud)));
            // Constant scene should remain the same; allow rounding of the FI shader.
            if(!moving) for(u32 channel=0;channel<3;++channel) {
                const int actual=(background>>(8*channel))&255;
                const int expected=channel==0?64:channel==1?128:191;
                CHECK(std::abs(actual-expected)<=2);
            }
            if(moving && frame>=8) {
                vmaInvalidateAllocation(allocator,allocation,readbackOffset,frameBytes);
                const auto* pixels = reinterpret_cast<const u32*>(static_cast<char*>(info.pMappedData)+readbackOffset);
                uint64_t checksum=14695981039346656037ull;
                for(size_t i=0;i<frameBytes;++i) {
                    checksum^=reinterpret_cast<const unsigned char*>(pixels)[i];checksum*=1099511628211ull;
                }
                std::printf("Generated frame %u checksum=%016llx\n",frame,(unsigned long long)checksum);
                double midpointError=0,currentError=0; size_t samples=0;
                for(u32 y=128;y<952;y+=4) for(u32 x=400;x<1520;x+=4) {
                    const u32 actual=pixels[y*1920+x];
                    const u32 middle=expectedTexture(float(x)-(float(frame)-0.5f)*shiftX,float(y)-(float(frame)-0.5f)*shiftY);
                    const u32 current=expectedTexture(float(x)-float(frame)*shiftX,float(y)-float(frame)*shiftY);
                    for(u32 c=0;c<3;++c) {
                        midpointError+=std::abs(int((actual>>(8*c))&255)-int((middle>>(8*c))&255));
                        currentError+=std::abs(int((actual>>(8*c))&255)-int((current>>(8*c))&255));
                        ++samples;
                    }
                }
                midpointError/=samples; currentError/=samples;
                std::printf("Motion frame %u: midpoint MAE=%.3f, current MAE=%.3f\n",frame,midpointError,currentError);
                std::fflush(stdout);
                if(frame==8 || frame==31) {
                    char name[64]; std::snprintf(name,sizeof(name),"framegen-motion-%u.ppm",frame);
                    if(FILE* file=std::fopen(name,"wb")) {
                        std::fprintf(file,"P6\n1920 1080\n255\n");
                        for(size_t i=0;i<1920u*1080u;++i) std::fwrite(&pixels[i],1,3,file);
                        std::fclose(file);
                    }
                }
                CHECK(midpointError<6.0 && midpointError<currentError*0.7);
                if(hudMotion || replayHud) {
                    double edgeError=0; size_t edgeSamples=0;
                    int opaqueError=0,edgeMax=0;
                    for(u32 y=44;y<156;++y) {
                        for(u32 x=52;x<348;++x) if((x/7+y/11)%3==0) {
                            const u32 a=pixels[y*1920+x];
                            for(u32 c=0;c<3;++c) opaqueError=std::max(opaqueError,std::abs(int((a>>(8*c))&255)-int((displayColor(0xff20e040u)>>(8*c))&255)));
                        }
                        for(u32 x=26;x<375;++x) {
                            if(x>=48 && x<352) continue;
                            const u32 a=pixels[y*1920+x],e=expectedTexture(float(x)-(float(frame)-0.5f)*shiftX,float(y)-(float(frame)-0.5f)*shiftY);
                            for(u32 c=0;c<3;++c) {const int error=std::abs(int((a>>(8*c))&255)-int((e>>(8*c))&255)); edgeError+=error; edgeMax=std::max(edgeMax,error); ++edgeSamples;}
                        }
                    }
                    edgeError/=edgeSamples;
                    std::printf("HUD frame %u: opaque max error=%d, adjacent scene MAE=%.3f, max=%d\n",frame,opaqueError,edgeError,edgeMax);
                    std::fflush(stdout);
                    CHECK(opaqueError<=2 && edgeError<6.0 && edgeMax<=8);
                }
            }
        } else {fg.FinishTimings();scheduler.Finish();}
    }
    if(sequence) {sequence->Drain();sequence.reset();}
    // Loading has no trustworthy scene motion; do not interpolate it or keep
    // its last scene as history for the next location.
    BbStats::loading_screen.store(true);
    CHECK(!fg.Prepare(1280,720));
    fg.Inputs(*depth_view,motion.image,1280,720,0.1f,1.0472f,{},camera,1000.0f/72.0f,false);
    fg.CaptureScene(*scene_view,vk::ImageLayout::eGeneral,1920,1080);
    fg.GenerateFinal(*ui_view,*scene_view,vk::ImageLayout::eGeneral,1920,1080);
    CHECK(!fg.Compose(*ui_view,vk::ImageLayout::eGeneral,present));
    for(u32 frame=0;frame<31;++frame) fg.Boundary();
    BbStats::loading_screen.store(false);
    CHECK(!fg.HasHistory());
    VmaTotalStatistics after{}; vmaCalculateStatistics(allocator,&after);
    CHECK(before.total.statistics.allocationCount==after.total.statistics.allocationCount);
    CHECK(before.total.statistics.allocationBytes==after.total.statistics.allocationBytes);
    vmaDestroyBuffer(allocator,host,allocation);
    if(stress) std::puts("PASS: 2050 production FG frames, final-display dispatch, exact background/HUD and idle reclamation");
    else if(replayHud) std::puts("PASS: native display replay with clean sRGB input/BGRA attachment, large HUD over moving background, no adjacent HUD ghosts");
    else if(hudMotion) std::puts("PASS: large opaque/semitransparent HUD decoupled from moving scene, no adjacent HUD ghosts");
    else if(finalColor) std::puts("PASS: final-display BGRA input, dark color transform, exact graded HUD, sRGB PP, loading fallback");
    else if(moving) std::puts("PASS: 32 moving frames, render-space motion, midpoint reconstruction and exact HUD");
    else std::puts("PASS: production FG, D32S8->R32 depth, zero-motion scene, reset fallback, exact HUD pixel, 8 frames and idle history reclamation");
}
