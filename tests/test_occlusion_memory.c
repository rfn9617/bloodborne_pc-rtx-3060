// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include "../gpu/shim/runtime_occlusion.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
void runtime_restart(void) { abort(); }
void compat_sleep_ns(uint64_t ns) { (void)ns;abort(); }
uintptr_t runtime_lookup(const RuntimeExport* table,size_t count,const char* nid) {
    (void)table;(void)count;(void)nid;abort();
}
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef int32_t (ABI *Allocate)(int64_t,int64_t,uint64_t,uint64_t,int,int64_t*);
typedef int32_t (ABI *Map)(void**,uint64_t,int,int,int64_t,uint64_t);
typedef int32_t (ABI *Unmap)(void*,uint64_t);
typedef int32_t (ABI *Release)(uint64_t,uint64_t);
int main(void) {
    runtime_memory_reserve();
    Allocate allocate=(Allocate)runtime_memory_resolve("rTXw65xmLIA#p#J");
    Map map=(Map)runtime_memory_resolve("L-Q3LEjIbgA#p#J");
    Unmap unmap=(Unmap)runtime_memory_resolve("cQke9UuBQOk#p#J");
    Release release=(Release)runtime_memory_resolve("MBuItvba6z8#p#J");
    int64_t physical;assert(!allocate(0,0x10000000,32768,16384,3,&physical));
    void* p=NULL;assert(!map(&p,16384,3,0,physical,16384));
    uint64_t* words=p;memset(p,0,16384);
    RuntimeOcclusionStamp stamp;assert(runtime_memory_occlusion_capture((uintptr_t)p,128,&stamp));
    assert(stamp.allocation);
    // Successful strided writes preserve every companion word.
    for(unsigned k=0;k<8;++k)words[2*k+1]=0x12345678;
    assert(runtime_memory_occlusion_write((uintptr_t)p,8,&stamp,0,0,17));
    uint64_t sum=0;
    for(unsigned k=0;k<8;++k) {sum+=words[2*k]&~(1ull<<63);assert(words[2*k+1]==0x12345678);}
    assert(sum==17);
    // A foreign last pipe must reject the entire update, including earlier pipes.
    uint64_t saved[16];words[14]=0xabcdef;memcpy(saved,p,128);
    assert(!runtime_memory_occlusion_write((uintptr_t)p,8,&stamp,1,17,30));
    assert(!memcmp(saved,p,128));
    words[14]=(1ull<<63)|2;
    assert(runtime_memory_occlusion_write((uintptr_t)p,8,&stamp,1,17,30));
    // Protection changes are not new allocations. Backing access never faults.
    runtime_memory_gpu_protect((uintptr_t)p,16384,0,0);
    assert(runtime_memory_occlusion_write((uintptr_t)p,8,&stamp,1,30,31));
    runtime_memory_gpu_protect((uintptr_t)p,16384,1,1);
    // Aliased mappings see backing writes but have separate mapping identity.
    void* alias=NULL;assert(!map(&alias,16384,3,0,physical,16384));
    RuntimeOcclusionStamp other;assert(runtime_memory_occlusion_capture((uintptr_t)alias,128,&other));
    assert(other.physical==stamp.physical && other.allocation!=stamp.allocation);
    assert(!runtime_memory_occlusion_write((uintptr_t)alias,8,&stamp,1,31,32));
    assert(((uint64_t*)alias)[0]==words[0]);
    assert(!unmap(alias,16384));
    // VA/physical ABA: a new zero-filled block at the exact same address is NOT ours.
    uintptr_t old_address=(uintptr_t)p;assert(!unmap(p,16384));
    assert(!runtime_memory_occlusion_write(old_address,8,&stamp,1,31,32));
    p=(void*)old_address;assert(!map(&p,16384,3,0x10,physical,16384));memset(p,0,16384);
    assert(!runtime_memory_occlusion_write(old_address,8,&stamp,0,0,1));
    RuntimeOcclusionStamp newer;assert(runtime_memory_occlusion_capture(old_address,128,&newer));
    assert(newer.allocation!=stamp.allocation);
    // Crossing an unmapped tail must never write a mapped prefix.
    uint64_t* tail=(uint64_t*)(old_address+16384-64);memset(tail,0,64);
    assert(!runtime_memory_occlusion_capture((uintptr_t)tail,128,&newer));
    assert(!runtime_memory_occlusion_write((uintptr_t)tail,8,&stamp,0,0,1));
    for(unsigned k=0;k<8;++k)assert(tail[k]==0);
    assert(!unmap(p,16384));assert(!release(physical,32768));
    puts("Occlusion real guest backing: strided writes, foreign rejection, protection, alias, VA/PA remap and unmapped tail PASS");
}
