// SPDX-License-Identifier: GPL-2.0-or-later
// Production upload/coherence and deferred image-copy paths, no game/window.
#include <cassert>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#define NDEBUG
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/page_manager.h"
#undef NDEBUG
#include <cassert>
extern "C" {
extern int runtime_test_fake_memory;
extern uintptr_t runtime_test_memory_base;
extern uint64_t runtime_test_memory_size;
}
struct Caches {
    VideoCore::PageManager pages;
    VideoCore::BufferCache buffers;
    VideoCore::TextureCache textures;
    Caches(const Vulkan::Instance& i, Vulkan::Scheduler& s, Vulkan::Runtime& r)
        : pages{nullptr}, buffers{i,s,r,nullptr,textures,pages},
          textures{i,s,r,nullptr,buffers,pages} {}
};
int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    constexpr size_t GuestSize = 1 << 20;
    auto* guest = static_cast<u8*>(VirtualAlloc(reinterpret_cast<void*>(0x1000000000ull),
                              GuestSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(guest);
    runtime_test_fake_memory = 2;
    runtime_test_memory_base = reinterpret_cast<uintptr_t>(guest);
    runtime_test_memory_size = GuestSize;
    Instance instance(0, false);
    Scheduler scheduler(instance);
    Runtime runtime(instance, scheduler);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance()); dispatch.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    const auto make_info = [](u32 w, u32 h, u32 mips, u32 layers) {
        ImageInfo info;
        info.size = {w,h,1}; info.pitch = w; info.resources = {mips,layers};
        info.type = layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
        info.pixel_format = vk::Format::eR8G8B8A8Unorm; info.num_bits = 32;
        info.tile_mode = AmdGpu::TileMode::DisplayLinearAligned;
        info.array_mode = AmdGpu::ArrayMode::ArrayLinearAligned;
        info.UpdateSize(); return info;
    };
    const auto clear = [&](Image& image, u32 mip, u32 layer, std::array<u8,4> value) {
        runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        const std::array<float,4> c{value[0]/255.f,value[1]/255.f,value[2]/255.f,value[3]/255.f};
        scheduler.CommandBuffer().clearColorImage(image.GetImage(), vk::ImageLayout::eGeneral,
            vk::ClearColorValue{c}, vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor,mip,1,layer,1}, dispatch);
    };
    const auto pixels = [&](Image& image, u32 mip, u32 layer) {
        const u32 w=std::max(image.info.size.width>>mip,1u), h=std::max(image.info.size.height>>mip,1u);
        Buffer readback(instance,0,w*h*4,MemoryType::HostCached);
        const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,mip,layer,1},
                                      .imageExtent={w,h,1}};
        runtime.DownloadImage(&image,&readback,std::span{&copy,1});
        scheduler.Finish(); readback.Invalidate(0,w*h*4);
        return std::vector<u8>(readback.mapped_data.begin(),readback.mapped_data.begin()+w*h*4);
    };
    {
        Caches caches(instance,scheduler,runtime);
        TextureCache::ImageDesc desc;
        desc.info=make_info(1,1,1,1); desc.info.guest_address=reinterpret_cast<u64>(guest);
        desc.view_info.format=desc.info.pixel_format;
        guest[0]=0;guest[1]=0;guest[2]=255;guest[3]=255;
        const auto id=caches.textures.FindImage(desc);
        auto& image=caches.textures.GetImage(id);
        caches.textures.RefreshImage(image); scheduler.Finish();
        const bool early_hash = !std::getenv("BB_TEXTURE_UPLOAD_BASELINE") ||
                                 std::strcmp(std::getenv("BB_TEXTURE_UPLOAD_BASELINE"),"0") != 0;
        assert((image.mip_hashes[0]!=0) == early_hash);
        clear(image,0,0,{255,0,0,255}); scheduler.Finish();
        image.flags|=ImageFlagBits::GpuModified|ImageFlagBits::CpuDirty;
        image.flags&=~ImageFlagBits::GpuDirty;
        caches.textures.RefreshImage(image); scheduler.Finish();
        const auto result=pixels(image,0,0);
        assert(result[0]==(early_hash?255:0) && result[1]==0 &&
               result[2]==(early_hash?0:255) && result[3]==255);
        std::printf("Production texture upload: %s baseline behavior verified PASS\n",
                    early_hash ? "early" : "legacy");
        // Both modes must retain the old unchanged-hash skip after the first
        // GPU-modified refresh. Changing only the early baseline is essential to A/B isolation.
        clear(image,0,0,{255,0,0,255}); scheduler.Finish();
        image.flags|=ImageFlagBits::GpuModified|ImageFlagBits::CpuDirty;
        image.flags&=~ImageFlagBits::GpuDirty;
        caches.textures.RefreshImage(image); scheduler.Finish();
        const auto second=pixels(image,0,0);
        assert(second[0]==255 && second[1]==0 && second[2]==0 && second[3]==255);
        std::puts("Production texture upload: subsequent unchanged-hash skip retained in both modes PASS");
        scheduler.Finish();
    }
    Common::SlotVector<ImageView> views;
    auto info=make_info(63,35,6,2);
    Image src(instance,runtime,views,info), dst(instance,runtime,views,info);
    for (u32 mip=0;mip<6;++mip) for (u32 layer=0;layer<2;++layer) {
        clear(src,mip,layer,{u8(32*mip),u8(128*layer),64,255});
        clear(dst,mip,layer,{255,255,255,255});
    }
    Buffer scratch(instance,0,1<<20,MemoryType::DeviceLocal);
    runtime.CopyImageWithBuffer(&src,&dst,&scratch,128); // nonzero aligned scratch offset
    scheduler.Finish();
    size_t checked=0;
    for (u32 mip=0;mip<6;++mip) for (u32 layer=0;layer<2;++layer) {
        const auto result=pixels(dst,mip,layer);
        for (size_t pos=0;pos<result.size();pos+=4) {
            assert(result[pos]==u8(32*mip) && result[pos+1]==u8(128*layer) &&
                   result[pos+2]==64 && result[pos+3]==255);
        }
        checked+=result.size();
    }
    std::printf("Production deferred image copy: all bytes of 6 mips x2 layers, odd dimensions, scratch offset128 (%zu bytes) PASS\n",checked);
    scheduler.Finish();
    VirtualFree(guest,0,MEM_RELEASE);
}
