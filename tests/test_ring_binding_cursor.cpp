// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/shadps4/video_core/renderer_vulkan/ring_binding_cursor.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include <chrono>
#include <cstdlib>
struct Binding { std::uint32_t index; std::uint64_t offset,hash; };
static const Binding* Reference(std::span<const Binding> bindings,std::uint32_t index) {
    for (const auto& binding:bindings) if (binding.index==index) return &binding;
    return nullptr;
}
int main(int argc,char** argv) {
    std::uint64_t checks=0;
    for(unsigned mask=0;mask<65536;++mask) {
        std::vector<Binding> bindings;
        for(unsigned i=0;i<16;++i) if(mask&(1u<<i))bindings.push_back({i,i*1024ull,mask^i});
        Vulkan::RingBindingCursor<Binding> cursor(bindings);
        for(unsigned i=0;i<18;++i) {
            assert(cursor.Find(i)==Reference(bindings,i));++checks;
            // Repeated equal queries must still return the same first entry.
            assert(cursor.Find(i)==Reference(bindings,i));++checks;
        }
    }
    std::uint64_t random=42;
    for(unsigned trial=0;trial<8192;++trial) {
        std::vector<Binding> bindings;
        for(unsigned i=0;i<64;++i) {
            random^=random<<13;random^=random>>7;random^=random<<17;
            if(random&1)bindings.push_back({i,random,~random});
        }
        Vulkan::RingBindingCursor<Binding> cursor(bindings);
        for(unsigned i=0;i<=64;++i) {assert(cursor.Find(i)==Reference(bindings,i));++checks;}
    }
    std::array<Binding,2> first{{{0,123,456},{5,789,1011}}};
    std::array<Binding,2> second{{{0,999,888},{5,777,666}}};
    Vulkan::RingBindingCursor<Binding> a(first),b(second);
    assert(a.Find(5)==&first[1] && b.Find(5)==&second[1]);checks+=2;
    std::printf("PASS: %llu pointer-identical comparisons; sparse/all/empty resource sets, repeated queries, missing indices, and fresh draw views\n",(unsigned long long)checks);
    // Lookup-only timings; neither the stage search nor the rest of BindBuffers is measured.
    const unsigned resources=argc>1?std::atoi(argv[1]):12;
    const unsigned captured=argc>2?std::atoi(argv[2]):9;
    assert(captured>0 && resources>=captured && resources<=64);
    std::vector<Binding> representative(captured);
    for(unsigned i=0;i<captured;++i)representative[i]={i*resources/captured,17+i,0};
    using Clock=std::chrono::steady_clock;
    volatile std::uint64_t sink=0;
    const auto start=Clock::now();
    for(unsigned round=0;round<200000;++round) for(unsigned i=0;i<resources;++i) {
        const auto* p=Reference(representative,i);if(p)sink=sink+p->offset;
    }
    const auto mid=Clock::now();
    for(unsigned round=0;round<200000;++round) {
        Vulkan::RingBindingCursor<Binding> cursor(representative);
        for(unsigned i=0;i<resources;++i){const auto* p=cursor.Find(i);if(p)sink=sink+p->offset;}
    }
    const auto end=Clock::now();
    std::printf("LOOKUP ONLY resources %u, captured %u, legacy %.3f ms, cursor %.3f ms; not an FPS prediction; checksum %llu\n",resources,captured,
        std::chrono::duration<double,std::milli>(mid-start).count(),
        std::chrono::duration<double,std::milli>(end-mid).count(),(unsigned long long)sink);
}
