// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
// No images are retained while waiting for a rare defect. Every accepted key request
// captures one evaluated frame, at most four frames over the entire process lifetime.
class BlurSnapshotBudget {
public:
    static constexpr unsigned Limit = 4;
    int Consume(std::uint32_t request) {
        if (request == seen) return -1;
        seen = request;
        if (!request || request > Limit || used >= Limit) return -1;
        return int(used++);
    }
private:
    std::uint32_t seen = 0;
    unsigned used = 0;
};
} // namespace Vulkan
