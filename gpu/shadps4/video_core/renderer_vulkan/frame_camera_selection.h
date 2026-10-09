// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {

// Auxiliary G-buffers may precede the main view. Select the largest view in this
// frame, while keeping the previous frame's camera when replacing an early candidate.
class FrameCameraSelection {
public:
    enum class Capture { Ignore, First, Replace };
    bool View(std::uint32_t width, std::uint32_t height) {
        const std::uint64_t area = std::uint64_t(width) * height;
        if (!area || area < largest_area) return false;
        largest_area = area;
        return true;
    }
    Capture Constants() {
        if (!largest_area || captured_area == largest_area) return Capture::Ignore;
        const auto action = captured_area ? Capture::Replace : Capture::First;
        captured_area = largest_area;
        return action;
    }
    // The predicate may read uncached mapped GPU memory. Once this view's camera
    // is captured, do not read it again. Invalid constants leave the view pending;
    // a later valid binding must still be able to capture or replace the camera.
    Capture ConstantsIf(auto&& is_valid) {
        if (!largest_area || captured_area == largest_area) return Capture::Ignore;
        if (!is_valid()) return Capture::Ignore;
        return Constants();
    }
    bool HasCamera() const { return captured_area != 0; }
    void Reset() { largest_area = captured_area = 0; }

private:
    std::uint64_t largest_area = 0, captured_area = 0;
};

} // namespace Vulkan
