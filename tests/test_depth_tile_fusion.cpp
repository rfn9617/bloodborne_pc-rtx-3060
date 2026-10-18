// SPDX-License-Identifier: GPL-2.0-or-later
// Real Vulkan upload -> fused/legacy tiling -> detiling -> byte comparison, no game/window.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
#define NDEBUG
#endif
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/tile_manager.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif

int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
    Scheduler scheduler(instance);
    Runtime runtime(instance, scheduler);
    StreamBuffer uniforms(instance, scheduler, MemoryType::HostCached, 4 * 1024 * 1024);
    TileManager tiler(instance, scheduler, runtime, uniforms);
    Common::SlotVector<ImageView> views;
    const u64 align = std::max<u64>(instance.StorageMinAlignment(), 256);
    unsigned cases = 0;
    for (auto format : {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint}) {
        for (auto mode : {AmdGpu::TileMode::Depth1DThin, AmdGpu::TileMode::Depth2DThin256}) {
            for (auto dims : {std::array<u32,3>{250,123,256}, {1280,720,1280}, {1920,1080,2048}}) {
                ImageInfo info;
                info.size = {dims[0],dims[1],1}; info.pitch = dims[2];
                info.resources = {1,1}; info.type = AmdGpu::ImageType::Color2D;
                info.pixel_format = format; info.num_bits = 32;
                info.props.is_depth = info.props.is_tiled = true;
                info.props.has_stencil = format == vk::Format::eD32SfloatS8Uint;
                info.tile_mode = mode;
                info.array_mode = mode == AmdGpu::TileMode::Depth1DThin
                    ? AmdGpu::ArrayMode::Array1DTiledThin1 : AmdGpu::ArrayMode::Array2DTiledThin1;
                info.bank_swizzle = mode == AmdGpu::TileMode::Depth1DThin ? 0 : 3;
                info.UpdateSize();
                Image image(instance, runtime, views, info);
                const auto& mip = info.mips_layout[0];
                const auto upload = runtime.GetStagingPool().Request(info.guest_size, MemoryType::HostCached);
                std::memset(upload.mapped, 0, upload.size);
                // Normals, both zeros, small normal/subnormal numbers and a nonuniform gradient.
                const std::array<u32,7> values{0,0x80000000,0x00800000,1,0x3e800000,0x3f400000,0x3f800000};
                std::vector<u32> expected(info.size.width * info.size.height);
                for (u32 y=0; y<info.size.height; ++y) for (u32 x=0; x<info.size.width; ++x) {
                    const u32 value = ((x+y)%11 < values.size()) ? values[(x+y)%11]
                        : 0x3e800000 + ((x*23+y*31)&0xfffff);
                    expected[y*info.size.width+x] = value;
                    std::memcpy(upload.mapped+(y*mip.pitch+x)*4, &value, 4);
                }
                upload.Flush();
                vk::BufferImageCopy copy{
                    .bufferOffset=upload.offset, .bufferRowLength=mip.pitch,
                    .bufferImageHeight=mip.height,
                    .imageSubresource={vk::ImageAspectFlagBits::eDepth,0,0,1},
                    .imageExtent={info.size.width,info.size.height,1},
                };
                runtime.UploadImage(&image, upload.buffer, std::span{&copy,1});
                copy.bufferOffset=0;
                const auto legacy=runtime.GetStagingPool().Request(info.guest_size+2*align,MemoryType::DeviceLocal,align);
                const auto fused=runtime.GetStagingPool().Request(info.guest_size+2*align,MemoryType::DeviceLocal,align);
                runtime.FillBuffer(legacy.buffer,legacy.offset,legacy.size,0xa5a5a5a5);
                runtime.FillBuffer(fused.buffer,fused.offset,fused.size,0xa5a5a5a5);
                auto legacy_copy=copy;
                tiler.TileImage(image,std::span{&legacy_copy,1},legacy.buffer,legacy.offset+align);
                assert(tiler.TryTileDepthImage(image,std::span{&copy,1},fused.buffer,fused.offset+align));

                // Refusal of unsupported partial, offset and stencil copies, and short outputs.
                auto bad=copy; bad.imageExtent.width--;
                assert(!tiler.TryTileDepthImage(image,std::span{&bad,1},fused.buffer,fused.offset+align));
                bad=copy; bad.bufferOffset=4;
                assert(!tiler.TryTileDepthImage(image,std::span{&bad,1},fused.buffer,fused.offset+align));
                bad=copy; bad.imageSubresource.aspectMask=vk::ImageAspectFlagBits::eStencil;
                assert(!tiler.TryTileDepthImage(image,std::span{&bad,1},fused.buffer,fused.offset+align));
                assert(!tiler.TryTileDepthImage(image,std::span{&copy,1},fused.buffer,fused.buffer->size_bytes));
                image.info.num_samples=2;
                assert(!tiler.TryTileDepthImage(image,std::span{&copy,1},fused.buffer,fused.offset+align));
                image.info.num_samples=1;

                runtime.IsBufferAccessed(legacy.buffer,legacy.offset+align,info.guest_size);
                runtime.IsBufferAccessed(fused.buffer,fused.offset+align,info.guest_size);
                const auto [old_linear,old_offset]=tiler.DetileImage(legacy.buffer,legacy.offset+align,info);
                const auto [new_linear,new_offset]=tiler.DetileImage(fused.buffer,fused.offset+align,info);
                const auto old_read=runtime.GetStagingPool().Request(info.guest_size,MemoryType::HostCached);
                const auto new_read=runtime.GetStagingPool().Request(info.guest_size,MemoryType::HostCached);
                const auto guards=runtime.GetStagingPool().Request(fused.size,MemoryType::HostCached);
                const vk::BufferCopy old_region{old_offset,old_read.offset,info.guest_size};
                const vk::BufferCopy new_region{new_offset,new_read.offset,info.guest_size};
                const vk::BufferCopy guard_region{fused.offset,guards.offset,fused.size};
                runtime.CopyBuffer(old_linear,old_read.buffer,std::span{&old_region,1});
                runtime.CopyBuffer(new_linear,new_read.buffer,std::span{&new_region,1});
                runtime.CopyBuffer(fused.buffer,guards.buffer,std::span{&guard_region,1});
                for (const auto& read : {old_read,new_read,guards})
                    runtime.AccessBuffer(read.buffer,read.offset,read.size,
                        vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead);
                runtime.FlushBarriers(); scheduler.Finish();
                old_read.Invalidate(); new_read.Invalidate(); guards.Invalidate();
                for (u32 y=0; y<info.size.height; ++y) for (u32 x=0; x<info.size.width; ++x) {
                    u32 before,after;
                    std::memcpy(&before,old_read.mapped+(y*mip.pitch+x)*4,4);
                    std::memcpy(&after,new_read.mapped+(y*mip.pitch+x)*4,4);
                    const auto wanted=expected[y*info.size.width+x];
                    if (before!=wanted || after!=wanted) {
                        std::fprintf(stderr,"FAIL %ux%u mode=%u fmt=%u (%u,%u): want=%08x old=%08x fused=%08x\n",
                            info.size.width,info.size.height,u32(mode),u32(format),x,y,wanted,before,after);
                        return 1;
                    }
                }
                for (u64 n=0;n<align;++n) {
                    assert(guards.mapped[n]==0xa5);
                    assert(guards.mapped[align+info.guest_size+n]==0xa5);
                }
                std::printf("PASS %ux%u pitch=%u mode=%u fmt=%u swizzle=%u\n",
                    info.size.width,info.size.height,mip.pitch,u32(mode),u32(format),info.bank_swizzle);
                ++cases;
                if (format==vk::Format::eD32SfloatS8Uint &&
                    mode==AmdGpu::TileMode::Depth2DThin256 && info.size.width==1280) {
                    const auto device=instance.GetDevice();
                    auto query=Vulkan::Check(device.createQueryPoolUnique({
                        .queryType=vk::QueryType::eTimestamp,.queryCount=2},nullptr,dispatch));
                    const float period=instance.GetPhysicalDevice().getProperties(dispatch).limits.timestampPeriod;
                    const auto measure=[&](bool fusion) {
                        const auto pool=*query;
                        scheduler.Record([pool,dispatch](vk::CommandBuffer cmd) {
                            cmd.resetQueryPool(pool,0,2,dispatch);
                            cmd.writeTimestamp(vk::PipelineStageFlagBits::eAllCommands,pool,0,dispatch);
                        });
                        constexpr unsigned rounds=32;
                        for (unsigned round=0;round<rounds;++round) {
                            // Serialize repeated writes to the same output in both paths.
                            if (runtime.IsBufferAccessed(fused.buffer,fused.offset+align,info.guest_size,true))
                                runtime.FlushBarriers();
                            auto region=copy;
                            if (fusion) {
                                assert(tiler.TryTileDepthImage(image,std::span{&region,1},fused.buffer,fused.offset+align));
                            } else {
                                tiler.TileImage(image,std::span{&region,1},fused.buffer,fused.offset+align);
                            }
                        }
                        scheduler.Record([pool,dispatch](vk::CommandBuffer cmd) {
                            cmd.writeTimestamp(vk::PipelineStageFlagBits::eAllCommands,pool,1,dispatch);
                        });
                        scheduler.Finish();
                        std::array<u64,2> times{};
                        const auto status=device.getQueryPoolResults(pool,0,2,sizeof(times),times.data(),
                            sizeof(u64),vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWait,dispatch);
                        assert(status==vk::Result::eSuccess);
                        return double(times[1]-times[0])*period/1e6/rounds;
                    };
                    for (unsigned trial=0;trial<3;++trial) {
                        const double before=measure(false),after=measure(true);
                        std::printf("Depth tiling GPU batch microbenchmark, 1280x720 (not game FPS): %.4f ms legacy / %.4f ms fused\n",before,after);
                    }
                }
                scheduler.Finish();
                runtime.TickFrame();
            }
        }
    }
    std::printf("Depth tile fusion: %u Vulkan byte-exact cases, padded rows, guards and fallback checks PASS\n",cases);
}
