// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generator.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vk_mem_alloc.h>
#include "bbport_toggles.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "video_core/host_shaders/framegen_color_copy_comp.h"
#include "video_core/host_shaders/framegen_depth_copy_comp.h"
#include "video_core/host_shaders/framegen_ui_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
namespace {
constexpr u32 OutputWidth = 1920, OutputHeight = 1080;
constexpr u64 MiB = 1024 * 1024;
constexpr u64 ResidentLimit = 192 * MiB; // generator-owned images + SDK
constexpr auto All = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto ReadWrite = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

void Barrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout old_layout,
             vk::ImageLayout new_layout) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = All, .srcAccessMask = ReadWrite,
        .dstStageMask = All, .dstAccessMask = ReadWrite,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
}
FfxVkFsr3_3_1_6FrameGenerationImage Describe(vk::Image image, VkFormat format, u32 w, u32 h,
                                         VkImageLayout layout) {
    return {image,format,w,h,layout,VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
}
}

bool FrameGenerator::Requested() {
    static const bool requested = [] {const char* value=std::getenv("BB_FRAME_GEN");
        return value && value[0]=='1';}();
    return requested;
}
bool FrameGenerator::FastPath() {
    static const bool fast=[] {const char* value=std::getenv("BB_FG_FAST_PATH");return !value || value[0]!='0';}();
    return fast;
}
FrameGenerator::FrameGenerator(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, enabled{Requested()} {}
FrameGenerator::~FrameGenerator() { Stop(); }

bool FrameGenerator::Allocate(VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                              u32 w, u32 h, vk::Format format) {
    image = VideoCore::UniqueImage(instance.GetDevice(),instance.GetAllocator());
    const vk::ImageCreateInfo ci{
        .flags=vk::ImageCreateFlagBits::eMutableFormat,.imageType=vk::ImageType::e2D,
        .format=format,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
        .tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
               (format==vk::Format::eR8G8B8A8Unorm ? vk::ImageUsageFlags(vk::ImageUsageFlagBits::eColorAttachment) : vk::ImageUsageFlags{})};
    const auto raw=static_cast<VkImageCreateInfo>(ci);
    const VmaAllocationCreateInfo allocation{.flags=VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage handle{};
    if (vmaCreateImage(instance.GetAllocator(),&raw,&allocation,&handle,&image.allocation,nullptr)!=VK_SUCCESS)
        return false;
    image.image=handle;
    image.image_ci=ci;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(instance.GetAllocator(),image.allocation,&info);
    image.size_bytes=info.size;
    auto [result,created]=instance.GetDevice().createImageViewUnique({
        .image=image.image,.viewType=vk::ImageViewType::e2D,.format=format,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    if(result!=vk::Result::eSuccess) return false;
    view=std::move(created);
    return true;
}

void FrameGenerator::Stop() {
    if (!context && !clean && !depth && !interpolated && !composed && !reference_display) return;
    FinishTimings();
    scheduler.Finish();
    CollectTimings(); PrintTimings();
    if (context) ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
    context=nullptr;
    retained.clear();
    clean_views.clear(); replay_views.clear(); replay_ready=false;
    reference_display_view.reset();
    clean_view.reset(); depth_view.reset(); interpolated_view.reset();
    composed_srgb.reset(); composed_view.reset();
    clean.Destroy(); depth.Destroy(); interpolated.Destroy(); composed.Destroy(); reference_display.Destroy();
    pending_generated=inputs_ready=captured=reference_ready=false;
    reset_history=true;
    width=height=0;
}

bool FrameGenerator::Ensure(u32 w,u32 h) {
    if (!enabled || failed || !w || !h || w>OutputWidth || h>OutputHeight) return false;
    if (context && width==w && height==h) return true;
    Stop();
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(instance.GetAllocator(),budgets);
    const auto memory=instance.GetPhysicalDevice().getMemoryProperties();
    u64 free_device=0;
    for(u32 i=0;i<memory.memoryHeapCount;++i) {
        if(memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
            free_device=std::max(free_device,budgets[i].budget>budgets[i].usage
                ? budgets[i].budget-budgets[i].usage : 0ull);
    }
    // Leave room for presentation images, streamed scene data and driver use.
    if(free_device<512*MiB) {
        std::printf("Frame generation: insufficient GPU budget (%llu MiB free), ordinary frames retained\n",
                    static_cast<unsigned long long>(free_device/MiB));
        failed=true; return false;
    }
    const FfxVkFsr3_3_1_6FrameGenerationCreateInfo ci{
        instance.GetPhysicalDevice(),instance.GetDevice(),w,h,OutputWidth,OutputHeight,VK_FORMAT_R8G8B8A8_UNORM};
    const auto result=ffxVkFsr3_3_1_6FrameGenerationContextCreate(&ci,&context);
    if(result!=FFX_VK_FSR3_3_1_6_FRAMEGEN_OK ||
       (!FastPath() && !Allocate(reference_display,reference_display_view,OutputWidth,OutputHeight,vk::Format::eR8G8B8A8Unorm)) ||
       !Allocate(clean,clean_view,OutputWidth,OutputHeight,vk::Format::eR8G8B8A8Unorm) ||
       !Allocate(interpolated,interpolated_view,OutputWidth,OutputHeight,vk::Format::eR8G8B8A8Unorm) ||
       !Allocate(composed,composed_view,OutputWidth,OutputHeight,vk::Format::eR8G8B8A8Unorm) ||
       !Allocate(depth,depth_view,w,h,vk::Format::eR32Sfloat)) {
        std::printf("Frame generation: allocation/context failed (%d), ordinary frames retained\n",int(result));
        Stop(); failed=true; return false;
    }
    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    const auto usage_result=ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context,&usage);
    const u64 resident=usage.totalUsageInBytes+clean.size_bytes+interpolated.size_bytes+
                       composed.size_bytes+depth.size_bytes+reference_display.size_bytes;
    if(usage_result!=FFX_VK_FSR3_3_1_6_FRAMEGEN_OK || resident>ResidentLimit) {
        std::printf("Frame generation: resident budget exceeded (%llu MiB), ordinary frames retained\n",
                    static_cast<unsigned long long>(resident/MiB));
        Stop(); failed=true; return false;
    }
    const vk::ImageViewUsageCreateInfo sampled_only{.usage=vk::ImageUsageFlagBits::eSampled};
    composed_srgb=Check(instance.GetDevice().createImageViewUnique({
        .pNext=&sampled_only,.image=composed.image,.viewType=vk::ImageViewType::e2D,.format=vk::Format::eR8G8B8A8Srgb,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
    width=w; height=h;
    if(!color_copy) Pipelines();
    std::printf("Frame generation: FSR 3.1.6 FI/OF, %ux%u -> 1920x1080, %.2f MiB owned (presentation excluded), limit 192 MiB\n",
                w,h,double(resident)/MiB);
    std::printf("Frame generation path: %s\n",FastPath() ? "optimized: direct display sample, single HUD compose" : "reference: display copy plus SDK UI extraction");
    const char* search=std::getenv("BB_FG_GPU_SEARCH");
    std::printf("FG optical-flow search: %s\n",search && search[0]=='1' ?
        "register reduction, single writer, fixed-row unroll" : search && search[0]=='2' ?
        "original algorithm, new compiler diagnostic" : "original shader");
    return true;
}

bool FrameGenerator::Prepare(u32 w,u32 h) {
    suspended=BbStats::loading_screen.load(std::memory_order_relaxed);
    if(suspended) {
        inputs_ready=captured=reference_ready=pending_generated=false;
        reset_history=true;
        return false;
    }
    // This entry point runs before the upscaler retains its command buffer:
    // Stop may submit/wait for it. Never perform this check inside Inputs.
    if(failed) { Stop(); return false; }
    if(context && ++budget_checks % 16u == 0u) {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(instance.GetAllocator(),budgets);
        const auto memory=instance.GetPhysicalDevice().getMemoryProperties();
        u64 free_device=0;
        for(u32 i=0;i<memory.memoryHeapCount;++i) {
            if(memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
                free_device=std::max(free_device,budgets[i].budget>budgets[i].usage
                    ? budgets[i].budget-budgets[i].usage : 0ull);
        }
        if(free_device<256*MiB) {
            std::printf("Frame generation: GPU budget pressure (%llu MiB free); disabled for this run, ordinary frames retained\n",
                        static_cast<unsigned long long>(free_device/MiB));
            Stop(); failed=true; return false;
        }
    }
    const bool ready=Ensure(w,h);
    if(ready) BeginTimings();
    return ready;
}

void FrameGenerator::Pipelines() {
    const auto device=instance.GetDevice();
    const auto create=[&](u32 sampled,vk::UniqueDescriptorSetLayout& desc,vk::UniquePipelineLayout& layout) {
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        for(u32 i=0;i<=sampled;++i) bindings.push_back({i,i==sampled
            ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,1,
            vk::ShaderStageFlagBits::eCompute,nullptr});
        desc=Check(device.createDescriptorSetLayoutUnique({
            .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount=u32(bindings.size()),.pBindings=bindings.data()}));
        layout=Check(device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*desc}));
    };
    create(1,copy_desc,copy_layout); create(3,ui_desc,ui_layout);
    const auto pipeline=[&](std::span<const u32> words,vk::PipelineLayout layout) {
        const auto module=CompileSPV(words,device);
        auto result=Check(device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=module,.pName="main"},.layout=layout}));
        device.destroyShaderModule(module); return result;
    };
    color_copy=pipeline(FRAMEGEN_COLOR_COPY_COMP,*copy_layout);
    depth_copy=pipeline(FRAMEGEN_DEPTH_COPY_COMP,*copy_layout);
    ui_pipeline=pipeline(FRAMEGEN_UI_COMP,*ui_layout);
}

void FrameGenerator::Copy(vk::ImageView input,vk::ImageLayout layout,VideoCore::UniqueImage& output,
                          vk::ImageView output_view,bool copy_depth) {
    scheduler.EndRendering();
    const auto cmd=scheduler.CommandBuffer();
    // Undefined discards prior contents; AllCommands still orders earlier reads.
    Barrier(cmd,output.image,vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral);
    const std::array infos{vk::DescriptorImageInfo{.imageView=input,.imageLayout=layout},
        vk::DescriptorImageInfo{.imageView=output_view,.imageLayout=vk::ImageLayout::eGeneral}};
    const std::array writes{
        vk::WriteDescriptorSet{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[0]},
        vk::WriteDescriptorSet{.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&infos[1]}};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,copy_depth?*depth_copy:*color_copy);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*copy_layout,0,writes);
    cmd.dispatch((output.image_ci.extent.width+7)/8,(output.image_ci.extent.height+7)/8,1);
    Barrier(cmd,output.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eGeneral);
}

void FrameGenerator::Inputs(vk::ImageView input_depth,vk::Image input_motion,u32 w,u32 h,
                            float near_,float fov_,std::array<float,2> jitter_,
                            std::array<std::array<float,3>,4> camera_,float milliseconds,bool reset) {
    if(suspended || !std::isfinite(near_) || near_<=0 || !std::isfinite(fov_) || fov_<=0 || !Ensure(w,h)) return;
    while(!retained.empty() && scheduler.IsFree(retained.front().second)) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context,retained.front().first);
        retained.pop_front();
    }
    TimingMark(0);
    Copy(input_depth,vk::ImageLayout::eGeneral,depth,*depth_view,true);
    TimingMark(1);
    near_plane=near_; fov=fov_; jitter=jitter_; camera=camera_;
    frame_ms=std::clamp(milliseconds,1.0f,200.0f);
    motion=input_motion; inputs_ready=true; reset_history|=reset;
}

void FrameGenerator::CaptureScene(vk::ImageView scene,vk::ImageLayout layout,u32 w,u32 h) {
    if(!context || !inputs_ready || captured || w!=OutputWidth || h!=OutputHeight) return;
    captured=reference_ready=true; pending_generated=false;
    TimingMark(2);
    Copy(scene,layout,clean,*clean_view,false);
    TimingMark(3);
}

vk::ImageView FrameGenerator::Mirror(VideoCore::UniqueImage& image,std::vector<MirrorView>& views,
        vk::Format format,vk::ComponentMapping mapping,vk::ImageUsageFlags usage) {
    for(const auto& view:views) if(view.format==format && view.mapping==mapping) return *view.view;
    const vk::ImageViewUsageCreateInfo restrict_usage{.usage=usage};
    auto view=Check(instance.GetDevice().createImageViewUnique({.pNext=&restrict_usage,
        .image=image.image,.viewType=vk::ImageViewType::e2D,.format=format,.components=mapping,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
    const auto handle=*view; views.push_back({format,mapping,std::move(view)}); return handle;
}
vk::ImageView FrameGenerator::CleanView(vk::Format format,vk::ComponentMapping mapping) {
    if(!context || !reference_ready || failed || suspended) return {};
    return Mirror(clean,clean_views,format,mapping,vk::ImageUsageFlagBits::eSampled);
}
vk::ImageView FrameGenerator::BeginDisplayReplay(vk::Format format) {
    if(!context || !reference_ready || failed || suspended ||
       (format!=vk::Format::eR8G8B8A8Srgb && format!=vk::Format::eB8G8R8A8Srgb &&
        format!=vk::Format::eR8G8B8A8Unorm && format!=vk::Format::eB8G8R8A8Unorm)) return {};
    replay_ready=false; replay_format=format;
    scheduler.EndRendering();
    TimingMark(4);
    Barrier(scheduler.CommandBuffer(),composed.image,vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral);
    return Mirror(composed,replay_views,format,{},vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment);
}
void FrameGenerator::EndDisplayReplay() {
    scheduler.EndRendering();
    Barrier(scheduler.CommandBuffer(),composed.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eGeneral);
    TimingMark(5);
    replay_ready=true;
}
vk::ImageView FrameGenerator::ReplayedDisplay(vk::Format sampling_format) {
    if(!replay_ready || !context) return {};
    const auto source_format=sampling_format==vk::Format::eUndefined?replay_format:sampling_format;
    if(source_format!=vk::Format::eR8G8B8A8Srgb && source_format!=vk::Format::eR8G8B8A8Unorm &&
       source_format!=vk::Format::eB8G8R8A8Srgb && source_format!=vk::Format::eB8G8R8A8Unorm) return {};
    const auto format=(source_format==vk::Format::eB8G8R8A8Srgb || source_format==vk::Format::eB8G8R8A8Unorm)
        ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm;
    return Mirror(composed,replay_views,format,{},vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment);
}

void FrameGenerator::GenerateFinal(vk::ImageView scene,vk::ImageView hudless,vk::ImageLayout layout,u32 w,u32 h,vk::ImageLayout scene_layout) {
    if(!hudless) {pending_generated=false; reference_ready=false; reset_history=true; return;}
    if(!context || !reference_ready || failed || suspended || w!=OutputWidth || h!=OutputHeight) return;
    reference_ready=false;
    TimingMark(6);
    if(!FastPath()) Copy(scene,scene_layout,reference_display,*reference_display_view,false);
    Copy(hudless,layout,clean,*clean_view,false);
    TimingMark(7);
    const auto cmd=scheduler.CommandBuffer();
    const u64 id=next_id++;
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer=cmd;
    prepare.color=Describe(clean.image,VK_FORMAT_R8G8B8A8_UNORM,w,h,VK_IMAGE_LAYOUT_GENERAL);
    prepare.depth=Describe(depth.image,VK_FORMAT_R32_SFLOAT,width,height,VK_IMAGE_LAYOUT_GENERAL);
    prepare.motionVectors=Describe(motion,VK_FORMAT_R16G16_SFLOAT,width,height,VK_IMAGE_LAYOUT_GENERAL);
    prepare.renderWidth=width; prepare.renderHeight=height;
    prepare.jitterOffsetX=jitter[0]; prepare.jitterOffsetY=jitter[1];
    prepare.motionVectorScaleX=prepare.motionVectorScaleY=1.0f;
    prepare.frameTimeMilliseconds=frame_ms; prepare.maxLuminance=1.0f;
    prepare.transferFunction=FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    prepare.cameraNear=near_plane; prepare.cameraFar=3000.0f;
    prepare.cameraVerticalFovRadians=fov; prepare.viewSpaceToMeters=1.0f;
    std::copy(camera[0].begin(),camera[0].end(),prepare.cameraPosition);
    std::copy(camera[1].begin(),camera[1].end(),prepare.cameraUp);
    std::copy(camera[2].begin(),camera[2].end(),prepare.cameraRight);
    std::copy(camera[3].begin(),camera[3].end(),prepare.cameraForward);
    prepare.frameId=id; prepare.reset=reset_history;
    TimingMark(8);
    auto result=ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context,&prepare);
    TimingMark(9);
    if(result==FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        TimingMark(10);
        Barrier(cmd,interpolated.image,vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral);
        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
        dispatch.commandBuffer=cmd; dispatch.color=prepare.color;
        if(!FastPath()) dispatch.presentColor=Describe(reference_display.image,VK_FORMAT_R8G8B8A8_UNORM,w,h,VK_IMAGE_LAYOUT_GENERAL);
        // Interpolate HUD-less color only. Our exact final mask performs UI
        // composition once; avoid the SDK's duplicate automatic UI extraction.
        dispatch.output=Describe(interpolated.image,VK_FORMAT_R8G8B8A8_UNORM,w,h,VK_IMAGE_LAYOUT_GENERAL);
        dispatch.displayWidth=w; dispatch.displayHeight=h;
        dispatch.frameTimeMilliseconds=frame_ms;
        dispatch.cameraNear=near_plane; dispatch.cameraFar=3000.0f;
        dispatch.cameraVerticalFovRadians=fov; dispatch.viewSpaceToMeters=1.0f;
        dispatch.maxLuminance=1.0f; dispatch.transferFunction=prepare.transferFunction;
        dispatch.frameId=id; dispatch.reset=prepare.reset;
        result=ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context,&dispatch);
        TimingMark(11);
    }
    retained.emplace_back(id,scheduler.CurrentTick());
    if(result!=FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::printf("Frame generation: dispatch failed (%d); ordinary frames retained\n",int(result));
        failed=true; inputs_ready=false; return;
    }
    Barrier(cmd,interpolated.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eGeneral);
    ++prepared;
    if(reset_history) ++resets; else {pending_generated=true; ++generated;}
    reset_history=false;
}

void FrameGenerator::Boundary() {
    if(!enabled) return;
    replay_ready=false;
    if(!captured) {reference_ready=false; pending_generated=false; reset_history=true; ++skipped; ++idle_frames;}
    else idle_frames=0;
    inputs_ready=captured=false;
    // Release scene-dependent history when menus/loading have no valid inputs.
    if(idle_frames==30 && context) {Stop(); std::printf("Frame generation: scene history released while idle\n");}
    Report();
}

vk::ImageView FrameGenerator::Compose(vk::ImageView with_ui,vk::ImageLayout layout,bool srgb) {
    if(!pending_generated || !context || failed) return {};
    pending_generated=false;
    TimingMark(12);
    // SDK history and optical flow are HUD-less; copy HUD pixels exactly here.
    scheduler.EndRendering();
    const auto cmd=scheduler.CommandBuffer();
    Barrier(cmd,composed.image,vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral);
    const std::array infos{
        vk::DescriptorImageInfo{.imageView=with_ui,.imageLayout=layout},
        vk::DescriptorImageInfo{.imageView=*clean_view,.imageLayout=vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{.imageView=*interpolated_view,.imageLayout=vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{.imageView=*composed_view,.imageLayout=vk::ImageLayout::eGeneral}};
    std::array<vk::WriteDescriptorSet,4> writes{};
    for(u32 i=0;i<4;++i) writes[i]={.dstBinding=i,.descriptorCount=1,
        .descriptorType=i==3?vk::DescriptorType::eStorageImage:vk::DescriptorType::eSampledImage,.pImageInfo=&infos[i]};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*ui_pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*ui_layout,0,writes);
    cmd.dispatch((OutputWidth+7)/8,(OutputHeight+7)/8,1);
    // Presenter FSR/PP sample through read-only descriptors. Publish the
    // compute result in that layout, with its writes visible to those readers.
    Barrier(cmd,composed.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eShaderReadOnlyOptimal);
    TimingMark(13);
    return srgb?*composed_srgb:*composed_view;
}

void FrameGenerator::BeginTimings() {
    static const bool requested=[] {const char* v=std::getenv("BB_FG_TIMINGS");return v && v[0]=='1';}();
    if(!requested) return;
    if(!timing_pool) {
        timing_pool=Check(instance.GetDevice().createQueryPoolUnique({
            .queryType=vk::QueryType::eTimestamp,.queryCount=TimingSlices*TimingStages*2}));
        timing_period=instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        if(const char* value=std::getenv("BB_FG_TIMINGS_EVERY")) timing_every=std::max(1, std::atoi(value));
        std::printf("Frame generation timings: sparse, 1/%u real frames, no GPU wait; per-draw profiling off\n",timing_every);
    }
    FinishTimings(); CollectTimings();
    if(++timing_frames % timing_every) return;
    timing_cursor=(timing_cursor+1)%TimingSlices;
    auto& slice=timing_slices[timing_cursor];
    if(slice.pending) return;
    instance.GetDevice().resetQueryPool(*timing_pool,timing_cursor*TimingStages*2,TimingStages*2);
    slice.mask=0; timing_active=true;
}
void FrameGenerator::TimingMark(u32 index) {
    if(!timing_active || index>=TimingStages*2) return;
    auto& slice=timing_slices[timing_cursor]; slice.mask|=1u<<index;
    slice.cpu[index]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto pool=*timing_pool; const u32 query=timing_cursor*TimingStages*2+index;
    scheduler.Record([pool,query](vk::CommandBuffer cmd) {cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands,pool,query);});
}
void FrameGenerator::FinishTimings() {
    if(!timing_active) return;
    auto& slice=timing_slices[timing_cursor];
    slice.tick=scheduler.CurrentTick();slice.pending=slice.mask!=0;timing_active=false;
}
void FrameGenerator::CollectTimings() {
    if(!timing_pool) return;
    for(u32 i=0;i<TimingSlices;++i) {
        auto& slice=timing_slices[i]; if(!slice.pending || !scheduler.IsFree(slice.tick)) continue;
        std::array<double,TimingStages> durations{}; bool ready=true;
        for(u32 stage=0;stage<TimingStages;++stage) {
            if((slice.mask&(3u<<(stage*2)))!=(3u<<(stage*2))) continue;
            std::array<u64,2> stamps{};
            const auto result=instance.GetDevice().getQueryPoolResults(*timing_pool,
                i*TimingStages*2+stage*2,2,sizeof(stamps),stamps.data(),sizeof(u64),vk::QueryResultFlagBits::e64);
            if(result!=vk::Result::eSuccess) {ready=false;break;}
            durations[stage]=double(stamps[1]-stamps[0])*timing_period/1e6;
        }
        if(!ready) continue;
        for(u32 stage=0;stage<TimingStages;++stage) {
            if((slice.mask&(3u<<(stage*2)))!=(3u<<(stage*2))) continue;
            auto& total=timing_totals[stage];total.gpu+=durations[stage];
            total.cpu+=slice.cpu[stage*2+1]-slice.cpu[stage*2];++total.count;
        }
        slice.pending=false;
    }
    if(std::chrono::steady_clock::now()-timing_report>std::chrono::seconds(5)) PrintTimings();
}
void FrameGenerator::PrintTimings() {
    static constexpr const char* names[]{"depth-copy","pre-UI-copy","HUDless-display","final-color-copy","OF+prepare","FI","HUD-compose","generated-PP"};
    bool any=false;for(const auto& total:timing_totals) any|=total.count!=0;
    if(!any) return;
    timing_report=std::chrono::steady_clock::now();
    double gpu=0,cpu=0;
    for(u32 stage=0;stage<TimingStages;++stage) {
        auto& total=timing_totals[stage];if(!total.count) continue;
        const double g=total.gpu/total.count,c=total.cpu/total.count;gpu+=g;cpu+=c;
        std::printf("  FG timing %s: GPU %.3f ms, CPU record %.3f ms (%u samples)\n",names[stage],g,c,total.count);
        total={};
    }
    std::printf("Frame generation measured stages: GPU %.3f ms, CPU record %.3f ms; independent overlapping domains, do not sum them\n",gpu,cpu);
}

void FrameGenerator::Report() {
    const auto now=std::chrono::steady_clock::now();
    if(now-last_report<std::chrono::seconds(5)) return;
    last_report=now;
    if(context) {
        FfxVkFsr3TransientStats stats{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetTransientStats(context,&stats);
        std::printf("FG GPU sync: SDK jobs=%llu calls=%llu image deps=%llu global deps=%llu removed duplicates=%llu batched calls=%llu\n",
            (unsigned long long)stats.sdkBarrierJobs,(unsigned long long)stats.sdkBarrierCalls,
            (unsigned long long)stats.imageDependencies,(unsigned long long)stats.memoryDependencies,
            (unsigned long long)stats.duplicateDependenciesRemoved,(unsigned long long)stats.batchedBarrierCalls);
        std::printf("FG CPU resources: constants created=%llu reused=%llu, descriptor sets created=%llu reused=%llu; completed cache %llu bytes constants/%llu buffers/%llu sets; pending %llu buffers/%llu sets\n",
            (unsigned long long)stats.constantsCreated,(unsigned long long)stats.constantsReused,
            (unsigned long long)stats.descriptorsCreated,(unsigned long long)stats.descriptorsReused,
            (unsigned long long)stats.cachedConstantBytes,(unsigned long long)stats.cachedConstants,
            (unsigned long long)stats.cachedDescriptors,(unsigned long long)stats.pendingConstants,
            (unsigned long long)stats.pendingDescriptors);
    }
    std::printf("Frame generation: prepared=%llu generated=%llu resets=%llu skipped=%llu retained=%zu failed=%d\n",
        static_cast<unsigned long long>(prepared),static_cast<unsigned long long>(generated),
        static_cast<unsigned long long>(resets),static_cast<unsigned long long>(skipped),retained.size(),failed);
}
} // namespace Vulkan
