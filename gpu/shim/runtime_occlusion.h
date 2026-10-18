// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint64_t allocation, physical; } RuntimeOcclusionStamp;
int runtime_memory_occlusion_capture(uintptr_t address, uint64_t extent, RuntimeOcclusionStamp* stamp);
int runtime_memory_occlusion_write(uintptr_t address, uint32_t pipes, const RuntimeOcclusionStamp* stamp,
                                   int has_previous, uint64_t previous, uint64_t total);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
#ifdef __cplusplus
}
#endif
