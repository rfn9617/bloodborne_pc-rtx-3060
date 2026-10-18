// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
// One request, one sample per command-stream frame, no rearm. The ROI readback
// is also subject to DumpImages' independent 256-MiB process allocation limit.
class GlowCaptureBudget {
public:
    static constexpr unsigned Limit = 64;
    int Consume(std::uint32_t request, std::uint64_t frame) {
        if (!request || used >= Limit || (used && frame <= previous)) return -1;
        previous = frame;
        return int(used++);
    }
private:
    unsigned used = 0;
    std::uint64_t previous = 0;
};
} // namespace Vulkan
