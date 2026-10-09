// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "common/interval_set.h"

struct Backing : Interval {
    u64 memory, offset;
    bool CanMergeWith(const Backing& other) const {
        return memory == other.memory && offset + end - start == other.offset;
    }
    Backing SubRange(u64 a, u64 b) const { return {{a, b}, memory, offset + a - start}; }
};

int main() {
    IntervalList<Backing> ranges;
    ranges.Add({{10, 20}, 1, 0});
    ranges.Subtract(13, 17);
    assert(ranges.Size() == 2);
    assert(ranges.Find(12)->offset == 0 && ranges.Find(17)->offset == 7);
    assert(!ranges.Contains(13) && !ranges.Contains(16));
    ranges.Add({{13, 17}, 2, 0});
    assert(ranges.Size() == 3 && ranges.Find(17)->memory == 1);
    ranges.Subtract(10, 13);
    assert(ranges.Size() == 2 && !ranges.Contains(12));
    ranges.Subtract(17, 20);
    assert(ranges.Size() == 1 && ranges.Find(14)->memory == 2);
    ranges.Subtract(0, 100);
    assert(ranges.Empty());
    ranges.Add({{1, 3}, 1, 0});
    ranges.Add({{8, 12}, 2, 0});
    ranges.Subtract(2, 10);
    assert(ranges.Size() == 2 && ranges.Find(10)->offset == 2);
    ranges.Subtract(3, 9); // Gap only.
    assert(ranges.Size() == 2);
    ranges.Subtract(1, 1); // Empty request.
    assert(ranges.Size() == 2);
    // VisitCovering (EnsureResident's fast path): visits overlaps, reports full coverage only.
    IntervalList<Backing> cover;
    cover.Add({{0, 4}, 1, 0});
    cover.Add({{4, 8}, 2, 0}); // adjacent, another allocation: not merged
    cover.Add({{10, 12}, 3, 0});
    int visited = 0;
    assert(cover.VisitCovering(1, 7, [&](const Backing&) { ++visited; }) && visited == 2);
    visited = 0;
    assert(!cover.VisitCovering(6, 11, [&](const Backing&) { ++visited; }) && visited == 2);
    assert(!cover.VisitCovering(8, 10, [](const Backing&) {}));   // gap only
    assert(!cover.VisitCovering(11, 13, [](const Backing&) {}));  // runs past the last one
    assert(!cover.VisitCovering(20, 30, [](const Backing&) {}));  // after everything
    assert(cover.VisitCovering(10, 12, [](const Backing&) {}));
    assert(cover.VisitCovering(5, 5, [](const Backing&) {}));     // empty request
    std::puts("Sparse ranges: splits, shared allocations, offsets, gaps, coverage and final "
              "removal PASS");
}
