// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include "video_core/renderer_vulkan/vertex_fetch_prefix.h"

int main() {
    using namespace Vulkan::VertexFetch;
    constexpr std::uint64_t floor = 16384;
    assert(BindingFits(65536, 32768, 32768));
    assert(!BindingFits(65536, 32768, 32769));
    assert(!BindingFits(65536, 65537, 1));
    assert(!BindingFits(std::numeric_limits<std::uint64_t>::max(),
                        std::numeric_limits<std::uint64_t>::max(), 1));
    assert(Index16End(0) == 65536);
    assert(Index16End(123) == 65659);
    assert(!Index16End(-1));
    assert(Index16End(std::numeric_limits<std::int32_t>::max()) == 2147549183ull);
    std::array<Request, 3> streams{{{100, 40}, {130, 40}, {300, 20}}};
    // The gap between independent prefixes stays inside one original merged request.
    assert(MergedEnd(100, 1000, streams) == 320);
    assert(MergedEnd(100, 250, streams) == 170);
    assert(MergedEnd(2000, 3000, streams) == 3000);
    streams[2].size = 800;
    assert(MergedEnd(100, 1000, streams) == 1000);
    assert(!DirectEnd(42, 0));
    assert(DirectEnd(0xffffffff, 2) == 0x100000001ull);
    assert(PrefixSize(1<<20, 16, DirectEnd(0,1024), floor) == 16385);
    assert(PrefixSize(1<<20, 32, DirectEnd(10,1024), floor) == 33072);
    assert(PrefixSize(1<<20, 0, DirectEnd(0,1024), floor) == 1<<20);
    assert(PrefixSize(1<<20, 16, {}, floor) == 1<<20);
    assert(PrefixSize(1<<20, 32, DirectEnd(0,65536), floor) == 1<<20);
    assert(PrefixSize(1<<20, 32, std::numeric_limits<std::uint64_t>::max(), floor) == 1<<20);
    // All possible indices fit, including mutable/GPU-written lists and restart disabled.
    // Differential checks use actual fetches from independent streams sharing one range.
    std::mt19937 random(11800);
    std::array<Request, 8> requests;
    for (unsigned trial=0; trial<5000; ++trial) {
        constexpr std::uint64_t merged_base = 1<<20;
        constexpr std::uint64_t declared = 8<<20;
        const std::uint32_t first = random()%2048;
        const std::uint32_t stride = 1+random()%64;
        for (unsigned i=0; i<requests.size(); ++i) {
            const auto offset = random()%65536;
            const auto prefix = PrefixSize(declared-offset,stride,Index16End(first),floor);
            requests[i] = {merged_base+offset,prefix};
            assert(prefix > floor && prefix <= declared-offset);
        }
        const auto merged_end = MergedEnd(merged_base,merged_base+declared,requests);
        for (const auto& request : requests) {
            assert(request.base+request.size <= merged_end);
            const std::uint32_t indices[] = {0, 65535, std::uint32_t(random()%65536), std::uint32_t(random()%65536)};
            for (auto index : indices) {
                assert(request.base+(std::uint64_t(index)+first)*stride+16 <= merged_end);
            }
        }
    }
    std::puts("Vertex prefix v2: conservative index16 bound, preserved merged requests, gaps, offsets, overflow, arena floor; 5000 differential fetch bounds PASS");
}
