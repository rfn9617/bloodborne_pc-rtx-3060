// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_copy.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif
extern "C" int runtime_test_fake_memory;

int main() {
    runtime_test_fake_memory = 1;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance, true);
    alignas(16) unsigned char source[65536]{};
    unsigned char output[16]{};
    std::memset(source, 0x39, sizeof(source));
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    std::atomic<bool> started{false}, release{false};
    scheduler.NoteHostCopySource(address, 16);
    scheduler.RecordHostCopy([&] {
        started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        std::memcpy(output, source, sizeof(output));
    });
    // Distinct range may proceed without kicking or waiting for the pending copy.
    scheduler.WaitHostCopiesFor(address + 0x20000, 16);
    assert(!started.load(std::memory_order_acquire));
    std::jthread unblock([&] {
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        release.store(true, std::memory_order_release);
    });
    scheduler.WaitHostCopiesFor(address, 16);
    std::memset(source, 0x71, 16); // WRITE_DATA only after the overlapping read
    for (auto byte : output) assert(byte == 0x39);
    const BbCopy::Item item{
        .run = [](const BbCopy::Item& copy) {
            std::memcpy(reinterpret_cast<void*>(copy.destination),
                        reinterpret_cast<const void*>(copy.source), copy.size);
        },
        .context = nullptr, .source = address,
        .destination = reinterpret_cast<std::uintptr_t>(output), .size = sizeof(output), .extra = 0,
    };
    scheduler.NoteHostCopySource(address, 16);
    BbCopy::QueueCopy(item); // still in the producer's unflushed batch
    scheduler.WaitHostCopiesFor(address, 16);
    std::memset(source, 0x22, 16);
    for (auto byte : output) assert(byte == 0x71);
    // Submission/fence style full wait still drains an otherwise unrelated copy.
    scheduler.NoteHostCopySource(address, 16);
    BbCopy::Async([&] { std::memcpy(output, source, sizeof(output)); });
    scheduler.WaitHostCopies();
    for (auto byte : output) assert(byte == 0x22);
    scheduler.Finish();
    std::puts("Host copy scheduler: independent write skip, overlapping recorder read, unflushed batch and async full-wait data integrity PASS");
}
