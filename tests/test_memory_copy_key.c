// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual Windows mappings and generation-aware region cache.
#include "../src/runtime_memory.c"
#include <assert.h>
// Unused service entry points from the included translation unit must never run.
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid; abort();
}
void compat_sleep_ns(uint64_t ns) { (void)ns; abort(); }
int main(void) {
    int64_t physical = -1;
    const uint64_t length = 65536;
    assert(direct_allocate(0, 536870912, length, length, 0, &physical) == 0);
    void *a = NULL, *b = NULL, *flex = NULL;
    assert(direct_map(&a, length, 3, 0, physical, length) == 0);
    assert(direct_map(&b, length, 3, 0, physical, length) == 0);
    uint64_t ka = 0, kb = 1, kf = 0;
    assert(runtime_memory_copy_key((uintptr_t)a + 12, 4, &ka));
    assert(runtime_memory_copy_key((uintptr_t)b + 12, 4, &kb));
    assert(ka == kb && ka == (uint64_t)physical + 12);
    *(uint32_t *)a = 3060;
    assert(*(uint32_t *)b == 3060);
    assert(runtime_memory_copy_key((uintptr_t)a + 24, 4, &ka));
    assert(ka == (uint64_t)physical + 24); // cached physical offset
    assert(!runtime_memory_copy_key((uintptr_t)a + length - 4, 8, &ka));
    assert(!runtime_memory_copy_key(UINTPTR_MAX - 2, 8, &ka));
    assert(!runtime_memory_copy_key((uintptr_t)a, 0, &ka));
    assert(map_flexible(&flex, length, 3, 0) == 0);
    assert(runtime_memory_copy_key((uintptr_t)flex, 4, &kf));
    assert(kf >= POOL_SIZE); // separate backing domain, no direct/flexible collision
    assert(direct_unmap(a, length) == 0);
    assert(!runtime_memory_copy_key((uintptr_t)a, 4, &ka)); // invalidates cached result
    assert(direct_unmap(b, length) == 0);
    assert(release_flexible(flex, length) == 0);
    assert(direct_release(physical, length) == 0);
    puts("Memory copy keys: shared Windows aliases, physical offsets, flexible memory, unmap invalidation and invalid ranges PASS");
    return 0;
}
