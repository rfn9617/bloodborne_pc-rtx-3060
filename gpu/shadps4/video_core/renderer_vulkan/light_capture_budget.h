// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>

namespace Vulkan {
// One F8 request captures ten input/output pairs, spaced by at least 200 ms.
// No images are kept waiting for the request; subsequent requests do not rearm.
class LightCaptureBudget {
public:
    static constexpr unsigned Limit = 10;
    using Clock = std::chrono::steady_clock;
    int Consume(std::uint32_t request, Clock::time_point now) {
        if (used >= Limit || !request) return -1;
        if (!started) { started = true; start = now; }
        if (used && now - last < std::chrono::milliseconds(200)) return -1;
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        last = now;
        return int(used++);
    }
    long long ElapsedMilliseconds() const { return elapsed_ms; }
private:
    bool started = false;
    unsigned used = 0;
    Clock::time_point start{}, last{};
    long long elapsed_ms = 0;
};
} // namespace Vulkan
