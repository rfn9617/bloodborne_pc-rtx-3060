// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace VideoCore {

// Submission ticks continue across wall-clock gaps (loading screens, suspended windows).
// Fill skipped seconds too, otherwise a stale ring slot can postpone collection for a minute.
class GcAge {
public:
    void Record(std::uint64_t second, std::uint64_t tick) {
        if (initialized && second == last_second) return;
        if (!initialized) {
            first_second = second;
            ticks[second % ticks.size()] = tick;
            initialized = true;
        } else if (second < last_second || second - last_second >= ticks.size()) {
            if (second < last_second) first_second = second;
            ticks.fill(tick > 0 ? tick - 1 : 0);
            ticks[second % ticks.size()] = tick;
        } else {
            for (auto s = last_second + 1; s < second; ++s) {
                ticks[s % ticks.size()] = tick > 0 ? tick - 1 : 0;
            }
            ticks[second % ticks.size()] = tick;
        }
        last_second = second;
    }

    std::uint64_t Before(std::uint64_t second, std::uint64_t age) const {
        return age > second ? 0 : ticks[(second - age) % ticks.size()];
    }

    bool HasHistory(std::uint64_t second, std::uint64_t age) const {
        return initialized && second >= first_second && second - first_second >= age;
    }

private:
    std::array<std::uint64_t, 64> ticks{};
    std::uint64_t last_second = 0;
    std::uint64_t first_second = 0;
    bool initialized = false;
};

} // namespace VideoCore
