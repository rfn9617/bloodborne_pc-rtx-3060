// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>
#include "video_core/renderer_vulkan/host_copy_sources.h"

int main() {
    using T = Vulkan::HostCopySources;
    T sources;
    sources.Note(0x20008, 16);
    assert(sources.NeedsWait(0x20000, 8)); // same conservative granule
    assert(!sources.NeedsWait(0x30000, 8));
    auto seen = sources.Snapshot();
    sources.Note(0x40000, 8); // published while a full wait was in progress
    sources.Complete(seen);
    assert(!sources.NeedsWait(0x20008, 16));
    assert(sources.NeedsWait(0x40000, 8));
    sources.Complete(sources.Snapshot());
    sources.Note(0x5fff8, 16);
    assert(sources.NeedsWait(0x60000, 8)); // crosses granule boundary
    sources.Complete(sources.Snapshot());
    std::uint64_t a = 1, b = 2;
    while (T::SlotOf(a) != T::SlotOf(b)) ++b;
    sources.Note(a << 16, 8);
    sources.Note(b << 16, 8);
    assert(sources.NeedsWait(a << 16, 8));
    assert(sources.NeedsWait(b << 16, 8));
    sources.Complete(sources.Snapshot());
    sources.Note(0, 65ull << 16);
    assert(sources.NeedsWait(0x900000000, 4));
    sources.Complete(sources.Snapshot());
    sources.Note(std::numeric_limits<std::uint64_t>::max() - 2, 8);
    assert(sources.NeedsWait(0x900000000, 4));
    sources.Complete(sources.Snapshot());
    assert(sources.NeedsWait(std::numeric_limits<std::uint64_t>::max() - 2, 8));
    // Differential check against exact pending intervals. False positives are
    // allowed; one missed overlapping read would corrupt an upload.
    struct Range { std::uint64_t start, size; };
    std::mt19937_64 rng{3060};
    std::size_t comparisons = 0;
    for (int batch = 0; batch < 400; ++batch) {
        sources.Complete(sources.Snapshot());
        std::vector<Range> pending;
        for (int i = 0; i < 300; ++i) {
            Range r{rng() % (256ull << 20), 1 + rng() % (3ull << 20)};
            pending.push_back(r);
            sources.Note(r.start, r.size);
            // Query an actual byte inside every newly published source.
            assert(sources.NeedsWait(r.start + rng() % r.size, 1));
            for (int q = 0; q < 8; ++q) {
                Range w{rng() % (256ull << 20), 1 + rng() % (2ull << 20)};
                bool overlap = false;
                for (const auto& read : pending)
                    overlap |= w.start < read.start + read.size && read.start < w.start + w.size;
                if (overlap) assert(sources.NeedsWait(w.start, w.size));
                ++comparisons;
            }
        }
    }
    std::printf("Host copy sources: boundaries, collision, snapshot retirement, overflow; %zu differential checks PASS\n", comparisons);
}
