#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/blur_snapshot_budget.h"
int main() {
    Vulkan::BlurSnapshotBudget budget;
    // More than an hour at 144 Hz: no captures, allocations or capacity consumed.
    for (unsigned frame = 0; frame < 600000; ++frame) assert(budget.Consume(0) == -1);
    for (unsigned request = 1; request <= 4; ++request) {
        assert(budget.Consume(request) == int(request - 1));
        for (unsigned frame = 0; frame < 100000; ++frame) assert(budget.Consume(request) == -1);
    }
    for (unsigned request = 5; request < 10000; ++request) assert(budget.Consume(request) == -1);
    Vulkan::BlurSnapshotBudget delayed;
    assert(delayed.Consume(2) == 0); // multiple requests before next evaluated frame coalesce
    assert(delayed.Consume(2) == -1);
    assert(delayed.Consume(3) == 1);
    assert(delayed.Consume(5) == -1); // overlay rejects a fifth key press, even after coalescing
    std::puts("Blur snapshot budget: long idle, repeated requests, coalescing and process limit PASS");
}
