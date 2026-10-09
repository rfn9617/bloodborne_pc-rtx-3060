// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#ifdef _WIN32
#define NDEBUG
#endif
#include "common/slot_vector.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/linear_image_readback.h"
#include "video_core/texture_cache/blit_helper.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif
extern "C" {
extern int runtime_test_fake_memory;
extern uintptr_t runtime_test_memory_base;
extern uint64_t runtime_test_memory_size;
extern int runtime_test_write_fail;
extern uint64_t runtime_test_write_calls;
}

int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    Instance instance(0, false);
    Scheduler scheduler(instance);
    Runtime runtime(instance, scheduler);
    Common::SlotVector<ImageView> views;
    std::vector<std::unique_ptr<Image>> images;
    std::vector<std::vector<u8>> patterns;
    std::vector<u8> guest(40 * 1024 * 1024, 0xa5);
    runtime_test_fake_memory = 2;
    runtime_test_memory_base = reinterpret_cast<uintptr_t>(guest.data());
    runtime_test_memory_size = guest.size();
    u64 offset = 0;
    for (const auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR16G16B16A16Sfloat,
                              vk::Format::eD32SfloatS8Uint}) {
        ImageInfo info;
        info.size = {1920, 1080, 1};
        info.pitch = 2048; // include row padding, as in guest targets
        info.resources = {1, 1};
        info.type = AmdGpu::ImageType::Color2D;
        info.pixel_format = format;
        info.num_bits = format == vk::Format::eR16G16B16A16Sfloat ? 64 : 32;
        info.props.is_depth = info.props.has_stencil = format == vk::Format::eD32SfloatS8Uint;
        info.tile_mode = AmdGpu::TileMode::DisplayLinearAligned;
        info.array_mode = AmdGpu::ArrayMode::ArrayLinearAligned;
        info.UpdateSize();
        info.guest_address = runtime_test_memory_base + offset;
        offset += info.guest_size;
        assert(offset <= guest.size());
        auto image = std::make_unique<Image>(instance, runtime, views, info);
        const u32 pixel_bytes = info.num_bits / 8;
        const u32 bytes = info.pitch * info.size.height * pixel_bytes;
        patterns.emplace_back(bytes, 0);
        auto& pattern = patterns.back();
        for (u32 y = 0; y < info.size.height; ++y) {
            for (u32 x = 0; x < info.size.width; ++x) {
                auto* pixel = pattern.data() + (y * info.pitch + x) * pixel_bytes;
                if (info.props.is_depth) {
                    const float value = ((x ^ y) & 1) ? 0.75f : 0.25f;
                    std::memcpy(pixel, &value, 4);
                } else if (pixel_bytes == 4) {
                    const u32 value = ((x * 7 + y * 11) & 255) << 24 |
                        (x * 19 & 255) | (y * 23 & 255) << 8 | ((x ^ y) * 31 & 255) << 16;
                    std::memcpy(pixel, &value, 4);
                } else {
                    const std::array<u16, 4> value{u16(0x3c00 + (x & 127)),
                        u16(0x3800 + (y & 127)), u16(0x4000 + ((x ^ y) & 127)),
                        u16(0x3800 + ((x + y) & 127))};
                    std::memcpy(pixel, value.data(), 8);
                }
            }
        }
        Buffer upload(instance, 0, bytes, MemoryType::HostCached);
        std::memcpy(upload.mapped_data.data(), pattern.data(), bytes);
        upload.Flush(0, bytes);
        const vk::BufferImageCopy copy{
            .bufferRowLength = info.pitch, .bufferImageHeight = info.size.height,
            .imageSubresource = {info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                    : vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {info.size.width, info.size.height, 1},
        };
        runtime.UploadImage(image.get(), &upload, std::span{&copy, 1});
        scheduler.Finish(); // upload allocation can now be destroyed
        assert(!QueueLinearImageReadback(runtime, *image, true)); // unmodified GPU surface
        image->flags |= ImageFlagBits::GpuModified;
        images.push_back(std::move(image));
    }
    const auto write = [](VAddr address, const u8* data, u32 bytes) {
        return Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(address), data, bytes);
    };
    const auto verify = [&] {
        for (u32 i = 0; i < images.size(); ++i) {
            const auto& info = images[i]->info;
            const auto* data = reinterpret_cast<const u8*>(info.guest_address);
            const u32 stride = info.pitch * info.num_bits / 8;
            for (u32 y = 0; y < info.size.height; ++y) {
                assert(std::memcmp(data + y * stride, patterns[i].data() + y * stride,
                                   info.size.width * info.num_bits / 8) == 0);
            }
        }
    };
    const auto run = [&](bool batch, bool fail) {
        std::vector<LinearImageReadback> readbacks;
        const auto tick = scheduler.CurrentTick();
        const auto writes = runtime_test_write_calls;
        runtime_test_write_fail = fail;
        u32 successful = 0;
        for (const auto& image : images) {
            const auto readback = QueueLinearImageReadback(runtime, *image, true);
            assert(readback);
            for (const auto& held : readbacks) {
                assert(held.staging.buffer != readback->staging.buffer);
            }
            if (batch) {
                readbacks.push_back(*readback);
            } else {
                successful += CompleteLinearImageReadbacks(runtime, scheduler,
                                            std::span{&*readback, 1}, write);
            }
        }
        if (batch) {
            assert(scheduler.CurrentTick() == tick); // queueing must not submit or wait
            successful = CompleteLinearImageReadbacks(runtime, scheduler, readbacks, write);
        }
        assert(scheduler.CurrentTick() == tick + (batch ? 1 : images.size()));
        assert(runtime_test_write_calls == writes + images.size());
        assert(successful == (fail ? 0 : images.size()));
        if (!fail) verify();
        runtime_test_write_fail = 0;
    };
    for (u32 round = 0; round < 4; ++round) {
        run(false, false);
        run(true, false);
    }
    run(true, true); // rejecting a guest write must still release every held allocation
    run(true, false);
    const auto empty_tick = scheduler.CurrentTick();
    assert(CompleteLinearImageReadbacks(runtime, scheduler, {}, write) == 0);
    assert(scheduler.CurrentTick() == empty_tick);
    std::puts("Linear image readback: RGBA8/FP16-alpha/depth pixels, padded rows, synchronous guest writes, held allocation isolation/reuse and failed-write cleanup PASS");
    const auto measure = [&](bool batch) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 round = 0; round < 12; ++round) run(batch, false);
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/12;
    };
    const double serial_ms = measure(false), batch_ms = measure(true);
    std::printf("Readback microbenchmark (copies + CPU verification, not game FPS): %.3f ms serial / %.3f ms batch, submits 3 -> 1\n", serial_ms, batch_ms);
    scheduler.Finish();
}
