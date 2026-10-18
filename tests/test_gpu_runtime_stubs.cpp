// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#include "bbport_toggles.h"
#include "runtime_occlusion.h"
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
void runtime_memory_read_backing(uintptr_t a, void* p, uint64_t n) {
    if (runtime_test_fake_memory != 3) std::abort();
    __builtin_memcpy(p,reinterpret_cast<void*>(a),n);
}
int runtime_memory_occlusion_capture(uintptr_t a, uint64_t n, RuntimeOcclusionStamp* stamp) {
    if (runtime_test_fake_memory != 3) std::abort();
    if (a < runtime_test_memory_base || n > runtime_test_memory_size ||
        a - runtime_test_memory_base > runtime_test_memory_size - n) return 0;
    *stamp={1,a};return 1;
}
int runtime_memory_occlusion_write(uintptr_t a,uint32_t pipes,const RuntimeOcclusionStamp* stamp,
                                  int previous,uint64_t old,uint64_t total) {
    RuntimeOcclusionStamp now{};
    if (!runtime_memory_occlusion_capture(a,16ull*(pipes-1)+8,&now) ||
        now.allocation!=stamp->allocation || now.physical!=stamp->physical) return 0;
    auto* p=reinterpret_cast<uint64_t*>(a);
    for (uint32_t k=0;k<pipes;++k)
        if (p[k*2] && !(previous && p[k*2]==((old/pipes+(k<old%pipes))|(1ull<<63)))) return 0;
    for (uint32_t k=0;k<pipes;++k) p[k*2]=(total/pipes+(k<total%pipes))|(1ull<<63);
    return 1;
}
uint64_t runtime_tsc_frequency() { return 1000000000; }
void runtime_file_stats(uint64_t out[5]) { for (int i = 0; i < 5; ++i) out[i] = 0; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t size) { if (runtime_test_fake_memory) return size; std::abort(); }
int runtime_memory_copy_key(uintptr_t address, uint64_t, uint64_t* key) {
    if (runtime_test_fake_memory) { *key = address; return 1; }
    std::abort();
}
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

