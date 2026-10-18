// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vk_mem_alloc.h>
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_frame_generator.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
// Diagnostic only. A bounded host-only pool; no GPU or disk wait on Record().
// Missing samples remain explicit. No capture files are interpreted as FPS tests.
class FgMotionCapture {
public:
    static bool Requested() {
        const char* v=std::getenv("BB_FG_MOTION_CAPTURE");
        return v && v[0]=='1';
    }
    static constexpr u32 CropX=768,CropY=432,CropW=384,CropH=640;
    static constexpr u32 MaxFrames=216,MaxBursts=2,PoolSlots=8;
    static constexpr u64 SlotBytes=u64(CropW)*CropH*4*6;
    struct Image {vk::Image image{};vk::ImageLayout layout{};u32 w=0,h=0;vk::Format format{};const char* kind="";vk::ImageAspectFlags aspect=vk::ImageAspectFlagBits::eColor;};
    static constexpr u32 PixelBytes(vk::Format format) {
        return format==vk::Format::eR16G16B16A16Sfloat ? 8u : 4u;
    }
    FgMotionCapture(const Instance& instance_,Scheduler& scheduler_,bool scene_stages_=false)
        : instance(instance_),scheduler(scheduler_),scene_stages(scene_stages_) {
        if(!Requested()) return;
        const char* directory=std::getenv("BB_DUMP_DIR");
        if(!directory || !*directory) return;
        directory_root=directory;
        // Artifact diagnosis needs the environment and HUD, beyond the coat crop.
        // Keep this explicit and bounded; ordinary capture remains unchanged.
        const char* full=std::getenv("BB_FG_CAPTURE_FULL_FRAME");
        if(full && full[0]=='1') {
            crop_x=crop_y=0;crop_w=1920;crop_h=1080;
            frame_limit=48;burst_limit=4;
            slot_bytes=u64(crop_w)*crop_h*4*6;
        }
        // Diagnostic session limit only; no change to the rendering or pool size.
        if(const char* limit=std::getenv("BB_FG_CAPTURE_BURSTS"))
            burst_limit=std::clamp(std::atoi(limit),1,8);
        auto p=std::make_shared<Pool>(); p->allocator=instance.GetAllocator();
        const auto memory=instance.GetPhysicalDevice().getMemoryProperties();
        u32 host_only=0;
        for(u32 i=0;i<memory.memoryTypeCount;++i) {
            const auto flags=memory.memoryTypes[i].propertyFlags;
            if((flags & vk::MemoryPropertyFlagBits::eHostVisible) && !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) host_only|=1u<<i;
        }
        if(!host_only) {std::puts("FG motion capture disabled: no host-only memory type");return;}
        for(auto& slot:p->slots) {
            const VkBufferCreateInfo ci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=slot_bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                .usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST,.memoryTypeBits=host_only};
            if(vmaCreateBuffer(p->allocator,&ci,&ai,&slot.buffer,&slot.allocation,&slot.info)!=VK_SUCCESS) {
                std::puts("FG motion capture disabled: host pool allocation failed");return;
            }
        }
        pool=std::move(p);
        std::printf("FG motion capture ready: F9, %u frames, %u bursts max, crop %u,%u %ux%u, %.2f MiB host-only pool\n",frame_limit,burst_limit,crop_x,crop_y,crop_w,crop_h,double(slot_bytes*PoolSlots)/(1024*1024));
    }
    ~FgMotionCapture() {Drain();}
    void Record(vk::CommandBuffer cmd,const Image& real,const Image& generated,
                const FrameGenerator::DiagnosticInputs& inputs) {
        const std::array<Image,6> images{real,generated,
            Image{inputs.clean,vk::ImageLayout::eGeneral,1920,1080,vk::Format::eR8G8B8A8Unorm,"hudless"},
            Image{inputs.interpolated,vk::ImageLayout::eGeneral,1920,1080,vk::Format::eR8G8B8A8Unorm,"fi_before_hud"},
            Image{inputs.motion,vk::ImageLayout::eGeneral,inputs.width,inputs.height,vk::Format::eR16G16Sfloat,"motion"},
            Image{inputs.depth,vk::ImageLayout::eGeneral,inputs.width,inputs.height,vk::Format::eR32Sfloat,"depth"}};
        RecordImages(cmd,images,inputs,real.image && real.w==1920 && real.h==1080);
    }
    void RecordScene(vk::CommandBuffer cmd,const Image& input,const Image& output,
                     const Image& depth,const Image& motion,
                     const FrameGenerator::DiagnosticInputs& inputs) {
        const std::array<Image,6> images{input,output,depth,motion,Image{},Image{}};
        const bool input_format=input.format==vk::Format::eR8G8B8A8Unorm ||
            input.format==vk::Format::eR8G8B8A8Srgb || input.format==vk::Format::eB8G8R8A8Unorm ||
            input.format==vk::Format::eB8G8R8A8Srgb;
        const bool valid=scene_stages && input.image && output.image && input_format &&
            output.format==vk::Format::eR16G16B16A16Sfloat;
        RecordImages(cmd,images,inputs,valid);
    }
private:
    void RecordImages(vk::CommandBuffer cmd,const std::array<Image,6>& images,
                      const FrameGenerator::DiagnosticInputs& inputs,bool valid) {
        if(!pool) return;
        const auto request=BbStats::fg_motion_capture_seq.load(std::memory_order_relaxed);
        if(request!=request_seen) {
            request_seen=request;
            if(!burst && burst_count<burst_limit && !BbStats::loading_screen.load()) Start();
            else std::puts("FG motion capture: request ignored (active, loading or burst limit)");
        }
        if(!burst) return;
        if(BbStats::loading_screen.load()) {End("loading interrupted capture");return;}
        const auto b=burst;
        auto packet=std::make_shared<Packet>();
        packet->index=u32(b->packets.size());packet->gpu_tick=scheduler.CurrentTick();
        packet->cpu_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-b->start).count();
        packet->fg_frame_id=inputs.frame_id;packet->frame_ms=inputs.frame_ms;packet->jitter=inputs.jitter;
        packet->resets=inputs.resets;packet->has_generated=!scene_stages && bool(images[1].image);
        b->packets.push_back(packet);
        for(const auto& image:images) if(image.image) {
            if(!image.w || !image.h || image.w>1920 || image.h>1080) {valid=false;break;}
            const u32 x=crop_x*image.w/1920,y=crop_y*image.h/1080;
            const u32 right=((crop_x+crop_w)*image.w+1919)/1920;
            const u32 bottom=((crop_y+crop_h)*image.h+1079)/1080;
            packet->streams.push_back({image.kind,packet->bytes,x,y,right-x,bottom-y,u32(image.format),PixelBytes(image.format)});
            packet->bytes+=u64(right-x)*(bottom-y)*PixelBytes(image.format);
        }
        int available=-1;
        if(valid && packet->bytes && packet->bytes<=slot_bytes) for(u32 i=0;i<PoolSlots;++i) {
            bool expected=false;
            if(pool->slots[i].busy.compare_exchange_strong(expected,true)) {available=int(i);break;}
        }
        if(available<0) packet->status=valid?"dropped-pool-full":"unsupported-input";
        else {
            packet->status="queued";
            auto p=pool;const u32 slot_index=u32(available);auto& slot=p->slots[slot_index];
            size_t stream_index=0;
            for(const auto& image:images) if(image.image) {
                const auto& stream=packet->streams[stream_index++];
                ImageBarrier(cmd,image,vk::ImageLayout::eTransferSrcOptimal);
                cmd.copyImageToBuffer(image.image,vk::ImageLayout::eTransferSrcOptimal,slot.buffer,
                    vk::BufferImageCopy{.bufferOffset=stream.offset,.imageSubresource={image.aspect,0,0,1},
                        .imageOffset={s32(stream.x),s32(stream.y),0},.imageExtent={stream.w,stream.h,1}});
                auto transferred=image;transferred.layout=vk::ImageLayout::eTransferSrcOptimal;
                ImageBarrier(cmd,transferred,image.layout);
            }
            const vk::BufferMemoryBarrier2 to_host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=slot.buffer,.offset=0,.size=packet->bytes};
            cmd.pipelineBarrier2({.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&to_host});
            scheduler.DeferPriorityOperation([p,b,packet,slot_index] {
                auto& slot=p->slots[slot_index];
                bool okay=vmaInvalidateAllocation(p->allocator,slot.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS;
                char name[64];std::snprintf(name,sizeof(name),"pair-%06u.bin",packet->index);
                if(okay) {
                    if(FILE* file=std::fopen((b->path/name).string().c_str(),"wb")) {
                        okay=std::fwrite(slot.info.pMappedData,1,packet->bytes,file)==packet->bytes;
                        okay=(std::fclose(file)==0) && okay;
                    } else okay=false;
                }
                packet->status=okay?"written":"write-error";
                {std::lock_guard lock(p->mutex);slot.busy.store(false);}
                p->cv.notify_all();
            });
        }
        if(b->packets.size()==frame_limit) End("frame limit reached");
    }
public:
    // Only called at test/end-of-process, after all recorded commands can submit.
    void Drain() {
        if(!pool) return;
        if(burst) End("capture ended before frame limit");
        scheduler.Finish();
        std::unique_lock lock(pool->mutex);
        pool->cv.wait(lock,[&] {if(pool->summaries.load()) return false;for(auto& s:pool->slots) if(s.busy.load()) return false;return true;});
        // Callback closures may briefly retain Pool after notifying us. Release
        // allocator-backed storage here, while Instance is certainly alive.
        pool->ReleaseBuffers();
        lock.unlock();pool.reset();
    }
private:
    struct Slot {VkBuffer buffer{};VmaAllocation allocation{};VmaAllocationInfo info{};std::atomic<bool> busy{false};};
    struct Pool {
        VmaAllocator allocator{};std::array<Slot,PoolSlots> slots{};std::atomic<u32> summaries{0};std::mutex mutex;std::condition_variable cv;
        void ReleaseBuffers() {for(auto& s:slots) if(s.buffer) {vmaDestroyBuffer(allocator,s.buffer,s.allocation);s.buffer={};s.allocation={};}}
        ~Pool() {ReleaseBuffers();}
    };
    struct Stream {const char* kind;u64 offset;u32 x,y,w,h,format,pixel_bytes;};
    struct Packet {
        u32 index=0;u64 gpu_tick=0,cpu_ns=0,fg_frame_id=0,resets=0,bytes=0;float frame_ms=0;
        std::array<float,2> jitter{};bool has_generated=false;const char* status="not-recorded";
        std::vector<Stream> streams;
    };
    struct Burst {std::filesystem::path path;std::chrono::steady_clock::time_point start;std::vector<std::shared_ptr<Packet>> packets;};
    static void ImageBarrier(vk::CommandBuffer cmd,const Image& image,vk::ImageLayout target) {
        const vk::ImageMemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=image.layout,.newLayout=target,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image.image,
            .subresourceRange={image.aspect,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
    }
    void Start() {
        auto b=std::make_shared<Burst>();b->start=std::chrono::steady_clock::now();
        char name[64];std::snprintf(name,sizeof(name),scene_stages?"scene-motion-%03u":"fg-motion-%03u",++burst_count);
        b->path=directory_root/name;
        std::error_code error;
        // Never overwrite a prior burst. Each launcher uses a unique session directory.
        if(!std::filesystem::create_directory(b->path,error) || error) {std::puts("FG motion capture: cannot create fresh burst directory");return;}
        b->packets.reserve(frame_limit);burst=std::move(b);
        std::printf("FG motion capture: burst %u started\n",burst_count);
    }
    void End(const char* reason) {
        auto b=std::move(burst);auto p=pool;p->summaries.fetch_add(1);
        const auto crop=std::array{crop_x,crop_y,crop_w,crop_h};
        const u64 pool_bytes=slot_bytes*PoolSlots;
        scheduler.DeferPriorityOperation([b,p,reason,crop,pool_bytes] {
            u32 written=0,dropped=0,errors=0;
            if(FILE* file=std::fopen((b->path/"sequence.json").string().c_str(),"wb")) {
                std::fprintf(file,"{\"schema\":1,\"reason\":\"%s\",\"diagnostic_not_benchmark\":true,\"physical_display_timing_measured\":false,\"host_pool_bytes\":%llu,\"crop_output\":[%u,%u,%u,%u],\"frames\":[",reason,(unsigned long long)pool_bytes,crop[0],crop[1],crop[2],crop[3]);
                for(size_t i=0;i<b->packets.size();++i) {
                    const auto& q=*b->packets[i];
                    if(std::string(q.status)=="written") ++written;else if(std::string(q.status)=="write-error") ++errors;else ++dropped;
                    std::fprintf(file,"%s{\"index\":%u,\"file\":\"pair-%06u.bin\",\"status\":\"%s\",\"cpu_relative_ns\":%llu,\"gpu_tick\":%llu,\"fg_frame_id\":%llu,\"fg_resets\":%llu,\"frame_ms\":%.9g,\"jitter\":[%.9g,%.9g],\"has_generated\":%s,\"bytes\":%llu,\"streams\":[",i?",":"",q.index,q.index,q.status,(unsigned long long)q.cpu_ns,(unsigned long long)q.gpu_tick,(unsigned long long)q.fg_frame_id,(unsigned long long)q.resets,q.frame_ms,q.jitter[0],q.jitter[1],q.has_generated?"true":"false",(unsigned long long)q.bytes);
                    for(size_t j=0;j<q.streams.size();++j) {
                        const auto& s=q.streams[j];
                        std::fprintf(file,"%s{\"kind\":\"%s\",\"offset\":%llu,\"crop\":[%u,%u,%u,%u],\"vk_format\":%u,\"bytes_per_pixel\":%u}",j?",":"",s.kind,(unsigned long long)s.offset,s.x,s.y,s.w,s.h,s.format,s.pixel_bytes);
                    }
                    std::fprintf(file,"]}");
                }
                std::fprintf(file,"],\"written\":%u,\"dropped\":%u,\"write_errors\":%u}",written,dropped,errors);
                if(std::fclose(file)!=0) ++errors;
            } else {++errors;std::puts("FG motion capture: metadata write failed");}
            std::printf("FG motion capture complete: %u written, %u dropped, %u write errors (%s)\n",written,dropped,errors,reason);
            {std::lock_guard lock(p->mutex);p->summaries.fetch_sub(1);}p->cv.notify_all();
        });
    }
    const Instance& instance;Scheduler& scheduler;std::filesystem::path directory_root;
    bool scene_stages=false;
    u32 crop_x=CropX,crop_y=CropY,crop_w=CropW,crop_h=CropH;
    u32 frame_limit=MaxFrames,burst_limit=MaxBursts;u64 slot_bytes=SlotBytes;
    std::shared_ptr<Pool> pool;std::shared_ptr<Burst> burst;u32 burst_count=0,request_seen=0;
};
} // namespace Vulkan
