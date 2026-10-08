// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <limits>
#include "gpu/shadps4/video_core/renderer_vulkan/staging_policy.h"

int main() {
    using namespace Vulkan::StagingPolicy;
    const auto keep = KeepBlocks(1024);
    assert(keep == 64 && PrewarmBlocks(keep, 256) == 16);
    assert(RetainedBlocks(true, keep) == 64);
    assert(RetainedBlocks(false, keep) == 0); // RAM budget must never reserve this much VRAM
    assert(PrewarmBlocks(KeepBlocks(0), 256) == 0);
    assert(PrewarmBlocks(KeepBlocks(128), 512) == 8);
    assert(KeepBlocks(std::numeric_limits<unsigned long long>::max()) == 256);
    std::puts("Staging: RAM upload retention, bounded prewarm and trimmable VRAM PASS");
}
