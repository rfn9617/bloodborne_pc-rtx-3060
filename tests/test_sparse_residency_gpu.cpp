// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <memory>
#include <cstring>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vertex_fetch_prefix.h"
#include "video_core/texture_cache/texture_cache.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
extern "C" int runtime_test_fake_memory;
extern "C" uintptr_t runtime_test_memory_base;
extern "C" uint64_t runtime_test_memory_size;
extern "C" int runtime_test_write_fail;
extern "C" uint64_t runtime_test_write_calls;

int main() {
    using namespace VideoCore;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    runtime_test_fake_memory = 1;
    Vulkan::Instance instance(0, false);
    std::puts("Sparse GPU: device ready");
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    PageManager pages(nullptr);
    std::puts("Sparse GPU: scheduler/runtime/page manager ready");
    alignas(TextureCache) std::byte image_storage[sizeof(TextureCache)];
    auto* images = reinterpret_cast<TextureCache*>(image_storage);
    auto cache = std::make_unique<BufferCache>(instance, scheduler, runtime, nullptr, *images, pages);
    std::construct_at(images, instance, scheduler, runtime, nullptr, *cache, pages);
    std::puts("Sparse GPU: caches ready");
    scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo& info) { cache->SubmitPendingArenaBinds(info); });
    const u64 page = u64(1) << cache->GetSparsePageShift();
    const VAddr base = 0x1000000000;
    Buffer page_result(instance, 0, 8, MemoryType::HostCached, "sparse page table check");
    const auto page_address = [&](VAddr address) {
        const vk::BufferCopy page_copy{(address / page) * 8, 0, 8};
        runtime.CopyBuffer(cache->GetBdaPageTableBuffer(), &page_result, std::span{&page_copy, 1});
        runtime.AccessBuffer(&page_result, 0, 8, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
        runtime.FlushBarriers();
        scheduler.Finish();
        page_result.Invalidate(0, 8);
        u64 pointer;
        std::memcpy(&pointer, page_result.mapped_data.data(), 8);
        return pointer;
    };
    const auto [arena, offset] = cache->ObtainBuffer(base, 4 * page, false);
    std::puts("Sparse GPU: allocated initial pages");
    runtime.FillBuffer(arena, offset, 4 * page, 0x12345678);
    scheduler.Finish();
    std::puts("Sparse GPU: initial upload complete");
    assert(cache->ResidentBytes() == 4 * page);
    assert(page_address(base) != 0 && page_address(base + page) != 0);
    cache->ReleaseUnmappedMemory(base + page, 2 * page, true);
    assert(cache->ResidentBytes() == 4 * page); // The two edges still share this allocation.
    assert(page_address(base) != 0 && page_address(base + page) == 0 &&
           page_address(base + 2 * page) == 0 && page_address(base + 3 * page) != 0);
    const auto [rebound, rebound_offset] = cache->ObtainBuffer(base + page, 2 * page, false);
    runtime.FillBuffer(rebound, rebound_offset, 2 * page, 0xaabbccdd);
    scheduler.Finish();
    assert(cache->ResidentBytes() == 6 * page);
    assert(page_address(base + page) != 0);
    cache->ReleaseUnmappedMemory(base, page, true);
    assert(cache->ResidentBytes() == 6 * page);
    cache->ReleaseUnmappedMemory(base + 3 * page, page, true);
    assert(cache->ResidentBytes() == 2 * page); // Last edge released the original allocation.
    Buffer result(instance, 0, 4 * page, MemoryType::HostCached, "sparse rebind check");
    const vk::BufferCopy copy{rebound_offset, 0, 2 * page};
    runtime.CopyBuffer(rebound, &result, std::span{&copy, 1});
    runtime.AccessBuffer(&result, 0, 2 * page, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    runtime.FlushBarriers();
    scheduler.Finish();
    result.Invalidate(0, 2 * page);
    for (u64 n = 0; n < 2 * page; n += 4) {
        u32 value;
        std::memcpy(&value, result.mapped_data.data() + n, 4);
        assert(value == 0xaabbccdd);
    }
    cache->ReleaseUnmappedMemory(base + page, 2 * page, true);
    assert(cache->ResidentBytes() == 0);
    const auto [restored, restored_offset] = cache->ObtainBuffer(base, 4 * page, false);
    runtime.FillBuffer(restored, restored_offset, 4 * page, 0x87654321);
    scheduler.Finish();
    assert(cache->ResidentBytes() == 4 * page);
    cache->ReleaseUnmappedMemory(base + 1, page - 2, true); // Partial page must survive.
    assert(cache->ResidentBytes() == 4 * page);
    cache->ReleaseUnmappedMemory(base, 4 * page, true);
    assert(cache->ResidentBytes() == 0);

    // Migration leaves old VkBuffer aliases: release must unbind those too and preserve
    // physical offsets of backing ranges split by earlier partial unmaps.
    const VAddr boundary = base + (u64(1) << 32);
    const auto [right, right_offset] = cache->ObtainBuffer(boundary + 2 * page, 2 * page, false);
    runtime.FillBuffer(right, right_offset, 2 * page, 0x13572468);
    scheduler.Finish();
    const auto [merged, merged_offset] = cache->ObtainBuffer(boundary - page, 2 * page, false);
    (void)merged_offset;
    scheduler.Finish();
    const vk::BufferCopy migrated_copy{merged->Offset(boundary + 2 * page), 0, 2 * page};
    runtime.CopyBuffer(merged, &result, std::span{&migrated_copy, 1});
    runtime.AccessBuffer(&result, 0, 2 * page, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    runtime.FlushBarriers();
    scheduler.Finish();
    result.Invalidate(0, 2 * page);
    for (u64 n = 0; n < 2 * page; n += 4) {
        u32 value;
        std::memcpy(&value, result.mapped_data.data() + n, 4);
        assert(value == 0x13572468);
    }
    cache->ReleaseUnmappedMemory(boundary - page, 6 * page, true);
    assert(cache->ResidentBytes() == 0 && page_address(boundary + 2 * page) == 0);
#ifdef _WIN32
    {
    // An actual guest RAM mapping: idle GC must preserve GPU-only changes before retirement.
    const VAddr guest = 0x600000000;
    auto* ram = static_cast<u32*>(VirtualAlloc(reinterpret_cast<void*>(guest), 4 * page,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(ram && reinterpret_cast<VAddr>(ram) == guest);
    runtime_test_fake_memory = 2;
    runtime_test_memory_base = guest;
    runtime_test_memory_size = 4 * page;
    for (u64 i = 0; i < 4 * page / 4; ++i) ram[i] = 0x31323334;
    const auto [dirty, dirty_offset] = cache->ObtainBuffer(guest, 4 * page, true);
    runtime.FillBuffer(dirty, dirty_offset, 4 * page, 0x51525354);
    scheduler.Finish();
    cache->CollectIdleMemory(100, false);
    assert(cache->ResidentBytes() == 4 * page && ram[0] == 0x31323334);
    cache->CollectIdleMemory(129, true);
    assert(cache->ResidentBytes() == 4 * page); // startup/age guard
    runtime_test_write_fail = 1;
    cache->CollectIdleMemory(130, true);
    assert(runtime_test_write_calls > 0); // the failing write was actually attempted
    assert(cache->ResidentBytes() == 4 * page && cache->IsRegionGpuModified(guest, 4 * page));
    assert(ram[0] == 0x31323334 && page_address(guest));
    runtime_test_write_fail = 0;
    cache->CollectIdleMemory(131, true);
    assert(cache->ResidentBytes() == 0 && page_address(guest) == 0);
    for (u64 i = 0; i < 4 * page / 4; ++i) assert(ram[i] == 0x51525354);
    const auto [restored, restored_offset] = cache->ObtainBuffer(guest, 4 * page, false);
    const vk::BufferCopy restored_copy{restored_offset, 0, 4 * page};
    runtime.CopyBuffer(restored, &result, std::span{&restored_copy, 1});
    runtime.AccessBuffer(&result, 0, 4 * page, vk::PipelineStageFlagBits2::eHost,
                         vk::AccessFlagBits2::eHostRead);
    runtime.FlushBarriers();
    scheduler.Finish();
    result.Invalidate(0, 4 * page);
    for (u64 i = 0; i < 4 * page; i += 4) {
        u32 value;
        std::memcpy(&value, result.mapped_data.data() + i, 4);
        assert(value == 0x51525354);
    }
    cache->CollectIdleMemory(200, false);
    assert(cache->ResidentBytes() == 4 * page); // never collect during active gameplay
    cache->SynchronizeDmaBuffers();
    cache->CollectIdleMemory(201, true);
    assert(cache->ResidentBytes() == 4 * page); // recent indirect accesses pin allocations
    cache->ReleaseUnmappedMemory(guest, 4 * page, true);
    assert(cache->ResidentBytes() == 0);
    VirtualFree(ram, 0, MEM_RELEASE);
    runtime_test_fake_memory = 1;
    std::puts("Sparse GPU: idle preservation/re-upload, failed RAM write retention, age/gameplay/DMA guards PASS");
    }
    {
    // The prefix path must upload exact guest bytes and grow on a later larger draw.
    const VAddr guest = 0x700000000;
    const u64 declared = 16 * page;
    auto* ram = static_cast<u32*>(VirtualAlloc(reinterpret_cast<void*>(guest), declared,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(ram && reinterpret_cast<VAddr>(ram) == guest);
    runtime_test_fake_memory = 2;
    runtime_test_memory_base = guest;
    runtime_test_memory_size = declared;
    for (u64 i = 0; i < declared / 4; ++i) ram[i] = u32(i) ^ 0xabcdef01;
    const auto first = Vulkan::VertexFetch::PrefixSize(declared, 32, page / 32,
                                                       BufferCache::STREAM_THRESHOLD);
    const auto [small, small_offset] = cache->ObtainBuffer(guest, first, false);
    scheduler.Finish();
    assert(cache->ResidentBytes() < declared);
    assert(small_offset + declared <= small->SizeBytes());
    const auto check = [&](const Buffer* source, u64 source_offset, u64 bytes) {
        const vk::BufferCopy prefix_copy{source_offset, 0, bytes};
        runtime.CopyBuffer(source, &result, std::span{&prefix_copy, 1});
        runtime.AccessBuffer(&result, 0, bytes, vk::PipelineStageFlagBits2::eHost,
                             vk::AccessFlagBits2::eHostRead);
        runtime.FlushBarriers();
        scheduler.Finish();
        result.Invalidate(0, bytes);
        assert(std::memcmp(result.mapped_data.data(), ram, bytes) == 0);
    };
    check(small, small_offset, first);
    // This data was outside the first resident prefix and must be read on growth.
    ram[2 * page / 4] = 0x99887766;
    const auto next = Vulkan::VertexFetch::PrefixSize(declared, 32, 3 * page / 32,
                                                      BufferCache::STREAM_THRESHOLD);
    const auto [larger, larger_offset] = cache->ObtainBuffer(guest, next, false);
    check(larger, larger_offset, next);
    assert(cache->ResidentBytes() < declared);
    cache->ReleaseUnmappedMemory(guest, declared, true);
    assert(cache->ResidentBytes() == 0);
    VirtualFree(ram, 0, MEM_RELEASE);
    runtime_test_fake_memory = 1;
    std::puts("Sparse GPU: exact vertex-prefix upload and later range growth PASS");
    }
#endif
    scheduler.Finish();
    std::destroy_at(images);
    cache.reset();
    std::puts("Sparse GPU: shared backing, unbind/free ordering, unchanged rebound data, partial pages and restore PASS");
}
