// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <vector>
#include "gpu/shadps4/common/lru_cache.h"
#include "gpu/shadps4/video_core/texture_cache/gc_age.h"

int main() {
    VideoCore::GcAge age;
    Common::LeastRecentlyUsedCache<int, std::uint64_t> cache;
    age.Record(100, 10);
    const auto old_area = cache.Insert(1, 10);
    cache.Insert(2, 11);
    age.Record(101, 12);
    age.Record(101, 999); // several submissions in one second keep its starting tick
    assert(age.Before(102, 1) == 12);
    // A loading screen skipped several seconds: old textures must still become collectible.
    age.Record(110, 20);
    cache.Touch(old_area, 20); // revisited/current area must stay resident
    std::vector<int> evicted;
    cache.ForEachItemBelow(age.Before(110, 5), [&](int image) { evicted.push_back(image); });
    assert(evicted == std::vector<int>{2});
    age.Record(200, 30); // pause exceeds the entire ring
    assert(age.Before(200, 5) == 29 && age.Before(200, 60) == 29);
    age.Record(201, 31);
    assert(age.Before(201, 1) == 30);
    VideoCore::GcAge startup;
    startup.Record(100, 40);
    assert(startup.Before(100, 20) == 0);
    // Tick zero is also a real first-submission tick. An uninitialised age slot must not
    // make fresh startup images eligible for below-budget collection.
    VideoCore::GcAge first_submission;
    assert(!first_submission.HasHistory(100, 20));
    first_submission.Record(100, 0);
    assert(!first_submission.HasHistory(100, 20));
    assert(!first_submission.HasHistory(119, 20));
    first_submission.Record(120, 10);
    assert(first_submission.HasHistory(120, 20) && first_submission.Before(120, 20) == 0);
    assert(age.HasHistory(201, 60));
    std::puts("Texture GC: loading gaps, long suspension and current-area retention PASS");
}
