// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#include "bbport_toggles.h"
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
int runtime_test_fake_memory = 0; // Explicitly enabled only by sparse-residency GPU tests.
uintptr_t runtime_test_memory_base = 0;
uint64_t runtime_test_memory_size = 0;
int runtime_test_write_fail = 0;
uint64_t runtime_test_write_calls = 0;
__thread sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_disabled_optimizations_high = 0;
unsigned runtime_toggle_phase = 0;
#ifdef _WIN32
int bb_setjmp(sigjmp_buf) { std::abort(); }
#endif
uint64_t runtime_tsc_frequency() { return 1000000000; }
void runtime_file_stats(uint64_t out[5]) { for (int i = 0; i < 5; ++i) out[i] = 0; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t size) { if (runtime_test_fake_memory) return size; std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped) {
    if (runtime_test_fake_memory == 2 && address >= runtime_test_memory_base &&
        address < runtime_test_memory_base + runtime_test_memory_size) {
        *start = runtime_test_memory_base;
        *end = runtime_test_memory_base + runtime_test_memory_size;
        *mapped = 1;
        return 1;
    }
    if (runtime_test_fake_memory) return 0;
    std::abort();
}
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { if (runtime_test_fake_memory) return; std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size) {
    if (runtime_test_fake_memory == 2) {
        ++runtime_test_write_calls;
        if (runtime_test_write_fail || address < runtime_test_memory_base ||
            size > runtime_test_memory_size ||
            address - runtime_test_memory_base > runtime_test_memory_size - size) return 0;
        __builtin_memcpy(reinterpret_cast<void*>(address), data, size);
        return 1;
    }
    std::abort();
}
}

