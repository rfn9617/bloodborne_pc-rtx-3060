// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
// Only a trigger dispatch may start a snapshot. Stage copies and the LDR tap
// must belong to that same command-stream frame; no image pool is retained.
class PostprocessCaptureBudget {
public:
    static constexpr unsigned Limit = 3, Stages = 16;
    int Begin(std::uint32_t request, std::uint64_t frame) {
        if (request == seen) return -1;
        seen = request;
        if (!request || request > Limit || used >= Limit) return -1;
        serial = frame;
        stages = 0;
        return snapshot = int(used++);
    }
    int NextStage(std::uint64_t frame) {
        if (snapshot < 0 || serial != frame || stages >= Stages) return -1;
        return int(stages++);
    }
    void End() { snapshot = -1; }
private:
    std::uint32_t seen = 0;
    std::uint64_t serial = 0;
    unsigned used = 0, stages = 0;
    int snapshot = -1;
};
}
