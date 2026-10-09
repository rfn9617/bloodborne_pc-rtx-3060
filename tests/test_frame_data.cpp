// SPDX-License-Identifier: GPL-2.0-or-later
// Regression checks on a real Vulkan device: delayed constants and lossless tiled readback.
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
// Vulkan-Hpp's dispatcher base changes layout with NDEBUG. Match the release renderer;
// enable this executable's own assertions again after the renderer headers.
#define NDEBUG
#endif
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/frame_camera_selection.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/image_readback.h"
#include "video_core/texture_cache/blit_helper.h"
#include <vk_mem_alloc.h>
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
    Common::SlotVector<ImageView> views;

    {
        ImageInfo info;
        info.guest_address = 0x1000000000;
        info.guest_size = 16384;
        // No Vulkan image is allocated for an undefined format; exercise writeback ownership.
        info.pixel_format = vk::Format::eUndefined;
        Image guard(instance, runtime, views, info);
        assert(!guard.SafeForGcWriteback());
        guard.track_addr = info.guest_address;
        guard.track_addr_end = info.guest_address + info.guest_size;
        assert(guard.SafeForGcWriteback());
        guard.track_addr += 4096;
        assert(!guard.SafeForGcWriteback());
        guard.gc_writeback_safe = false;
        guard.track_addr = info.guest_address;
        assert(!guard.SafeForGcWriteback()); // Re-protection cannot undo a lost edge watch.
        std::puts("Frame data: pressure writeback refuses partially watched/reused guest pages PASS");
    }

    // The guest reuses its camera while stage B is delayed. Both host reconstruction and
    // the GPU draw must still see exactly the bytes captured for that draw.
    ConstantRing constants(instance, scheduler);
    std::array<float, 216> guest{};
    for (u32 i = 0; i < guest.size(); ++i) guest[i] = float(i);
    const auto captured = guest;
    const auto offset = constants.Allocate(sizeof(guest), 256);
    assert(offset);
    std::memcpy(constants.Data(*offset), guest.data(), sizeof(guest));
    constants.Flush(*offset, sizeof(guest));
    guest.fill(-1.f);
    assert(std::memcmp(constants.Data(*offset), captured.data(), sizeof(guest)) == 0);
    Buffer constant_readback(instance, 0, sizeof(guest), MemoryType::HostCached, "test camera");
    const auto cmd = scheduler.CommandBuffer();
    cmd.copyBuffer(constants.Handle(), constant_readback.Handle(),
                   vk::BufferCopy{*offset, 0, sizeof(guest)}, dispatch);
    const vk::MemoryBarrier2 host{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host}, dispatch);
    scheduler.Finish();
    constant_readback.Invalidate(0, sizeof(guest));
    assert(std::memcmp(constant_readback.mapped_data.data(), captured.data(), sizeof(guest)) == 0);
    std::puts("Frame data: captured camera survives guest reuse and matches GPU bytes PASS");

    // Measure the same Stream allocation policy used by the constant ring, without
    // starting the game. No timing assertion: mapping choices differ across GPUs.
    // The lazy path must still return exactly one first capture and ignore repeats.
    {
        Buffer camera_stream(instance, 0, ConstantRing::Capacity, MemoryType::Stream);
        std::array<float,216> scene{};
        scene[0]=3000.f; scene[1]=1.f/3000.f; scene[4]=1280.f; scene[5]=720.f;
        std::memcpy(camera_stream.mapped_data.data(),scene.data(),sizeof(scene));
        camera_stream.Flush(0,sizeof(scene));
        VkMemoryPropertyFlags properties{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(),camera_stream.buffer.allocation,
                                         &properties);
        const volatile float* mapped = reinterpret_cast<const float*>(camera_stream.mapped_data.data());
        constexpr unsigned repeats=4096;
        const auto measure = [&](bool lazy) {
            FrameCameraSelection view;
            view.View(1280,720);
            unsigned reads=0,first=0;
            const auto valid = [&] {
                ++reads;
                return !(mapped[0] != 3000.f || mapped[4] < 64.f || mapped[5] < 64.f ||
                         std::abs(mapped[1]*mapped[0]-1.f) > 1e-3f);
            };
            const auto start=std::chrono::steady_clock::now();
            for (unsigned i=0;i<repeats;++i) {
                const auto action=lazy ? view.ConstantsIf(valid) :
                    valid() ? view.Constants() : FrameCameraSelection::Capture::Ignore;
                first += action == FrameCameraSelection::Capture::First;
                assert(action == (i == 0 ? FrameCameraSelection::Capture::First :
                                          FrameCameraSelection::Capture::Ignore));
            }
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-start).count();
            assert(first == 1 && reads == (lazy ? 1u : repeats));
            return double(ns)/repeats/1000;
        };
        const auto legacy_us=measure(false),lazy_us=measure(true);
        std::printf("Mapped camera CPU reads: Stream memory properties %#x, cached=%d; "
                    "validation-first %.3f us/call, lazy %.3f us/call; identical selection PASS "
                    "(isolated CPU access benchmark, not gameplay FPS)\n", properties,
                    !!(properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT),legacy_us,lazy_us);
    }

    StreamBuffer stream(instance, scheduler, MemoryType::Stream, 1_MB);
    TileManager tiler(instance, scheduler, runtime, stream);
    for (const auto mode : {AmdGpu::TileMode::Thin1DThin, AmdGpu::TileMode::Thin2DThin}) {
        for (const u32 bits : {32u, 64u}) {
            ImageInfo info;
            info.size = {128, 128, 1};
            info.pitch = 128;
            info.resources = {1, 1};
            info.type = AmdGpu::ImageType::Color2D;
            info.pixel_format = bits == 32 ? vk::Format::eR8G8B8A8Unorm
                                          : vk::Format::eR16G16B16A16Sfloat;
            info.num_bits = bits;
            info.tile_mode = mode;
            info.array_mode = AmdGpu::GetArrayMode(mode);
            info.props.is_tiled = true;
            info.UpdateSize();
            Image image(instance, runtime, views, info);
            assert(CanReadbackColor(image));
            auto excluded = info;
            image.info.props.has_stencil = true;
            assert(!CanReadbackColor(image));
            image.info = excluded;
            image.info.resources.levels = 2;
            assert(!CanReadbackColor(image));
            image.info = excluded;
            const auto row_bytes = info.mips_layout[0].pitch * bits / 8;
            Buffer upload(instance, 0, info.guest_size, MemoryType::HostCached, "test pattern");
            std::memset(upload.mapped_data.data(), 0, info.guest_size);
            for (u32 y = 0; y < 128; ++y) {
                for (u32 x = 0; x < 128; ++x) {
                    auto* p = upload.mapped_data.data() + y * row_bytes + x * bits / 8;
                    if (bits == 32) {
                        const u32 value = 0xff000000 | (x * 19 & 255) | (y * 23 & 255) << 8 |
                                          ((x ^ y) * 31 & 255) << 16;
                        std::memcpy(p, &value, 4);
                    } else {
                        const std::array<u16, 4> value{u16(0x3c00 + (x & 127)),
                            u16(0x3800 + (y & 127)), u16(0x4000 + ((x ^ y) & 127)), 0x3c00};
                        std::memcpy(p, value.data(), 8);
                    }
                }
            }
            upload.Flush(0, info.guest_size);
            const vk::BufferImageCopy copy{.bufferRowLength = info.mips_layout[0].pitch,
                .bufferImageHeight = info.mips_layout[0].height,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {128, 128, 1}};
            runtime.UploadImage(&image, &upload, std::span{&copy, 1});
            const auto preserved = ReadbackColor(runtime, tiler, image);
            scheduler.Finish();
            preserved.Invalidate();
            Buffer saved(instance, 0, info.guest_size, MemoryType::HostCached, "test RAM copy");
            std::memcpy(saved.mapped_data.data(), preserved.mapped, info.guest_size);
            saved.Flush(0, info.guest_size);
            const auto [linear, linear_offset] = tiler.DetileImage(&saved, 0, info);
            Buffer result(instance, 0, info.guest_size, MemoryType::HostCached, "test restore");
            const vk::BufferCopy restore{linear_offset, 0, info.guest_size};
            runtime.CopyBuffer(linear, &result, std::span{&restore, 1});
            runtime.AccessBuffer(&result, 0, info.guest_size, vk::PipelineStageFlagBits2::eHost,
                                 vk::AccessFlagBits2::eHostRead);
            runtime.FlushBarriers();
            scheduler.Finish();
            result.Invalidate(0, info.guest_size);
            for (u32 y = 0; y < 128; ++y) {
                assert(std::memcmp(upload.mapped_data.data() + y * row_bytes,
                                   result.mapped_data.data() + y * row_bytes, 128 * bits / 8) == 0);
            }
            std::printf("Frame data: tiled color GPU -> RAM -> restore, mode %u / %u bits PASS\n",
                        u32(mode), bits);
        }
    }
    scheduler.Finish();
}
