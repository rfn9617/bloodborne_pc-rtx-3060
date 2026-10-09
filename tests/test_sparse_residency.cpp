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
    std::puts("Sparse ranges: splits, shared allocations, offsets, gaps and final removal PASS");
}
