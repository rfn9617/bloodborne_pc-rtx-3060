// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/frame_camera_selection.h"

int main() {
    using Vulkan::FrameCameraSelection;
    using Capture = FrameCameraSelection::Capture;
    FrameCameraSelection frame;
    assert(!frame.HasCamera());
    assert(frame.Constants() == Capture::Ignore);
    // The sewer capture contains a half-size G-buffer before the main view.
    assert(frame.View(960, 540));
    assert(frame.Constants() == Capture::First);
    assert(frame.View(960, 540));
    assert(frame.Constants() == Capture::Ignore);
    assert(frame.View(1920, 1080));
    assert(frame.Constants() == Capture::Replace); // keep last frame's main camera
    assert(!frame.View(960, 540)); // a late auxiliary view cannot replace the main depth
    assert(frame.Constants() == Capture::Ignore);
    assert(frame.HasCamera());
    frame.Reset();
    assert(!frame.HasCamera());
    assert(frame.View(640, 360)); // explicit startup render resolution
    assert(frame.Constants() == Capture::First);
    assert(frame.View(1280, 720));
    assert(frame.Constants() == Capture::Replace);
    assert(!frame.View(0, 0));
    frame.Reset();
    assert(frame.View(1920, 1080)); // ordinary frame without an auxiliary view
    assert(frame.Constants() == Capture::First);
    assert(frame.View(1920, 1080));
    assert(frame.Constants() == Capture::Ignore);
    // Lazy validation must preserve both the selected camera and replacement order.
    // Compare to the previous validation-first path with invalid bindings mixed in.
    FrameCameraSelection legacy, lazy;
    unsigned legacy_reads = 0, lazy_reads = 0;
    const auto bind = [&](bool valid) {
        ++legacy_reads;
        const auto expected = valid ? legacy.Constants() : Capture::Ignore;
        const auto actual = lazy.ConstantsIf([&] { ++lazy_reads; return valid; });
        assert(actual == expected);
        assert(lazy.HasCamera() == legacy.HasCamera());
    };
    bind(true); // no G-buffer: no mapped memory access needed
    assert(lazy_reads == 0);
    assert(legacy.View(960,540) == lazy.View(960,540));
    bind(false); // unrelated 864-byte constants cannot consume the pending view
    assert(!lazy.HasCamera() && lazy_reads == 1);
    bind(true);
    for (unsigned draw = 0; draw < 2000; ++draw) bind(draw % 2 == 0);
    assert(lazy_reads == 2);
    assert(legacy.View(1920,1080) == lazy.View(1920,1080));
    bind(false);
    bind(true); // replace the auxiliary camera, preserving previous-frame history
    for (unsigned draw = 0; draw < 2000; ++draw) bind(true);
    assert(lazy_reads == 4);
    assert(legacy.View(640,360) == lazy.View(640,360));
    bind(true); // late auxiliary view cannot replace the main camera
    assert(lazy_reads == 4);
    legacy.Reset(); lazy.Reset();
    bind(true);
    assert(lazy_reads == 4);
    assert(legacy.View(1280,720) == lazy.View(1280,720));
    bind(true); // the next frame must capture again
    assert(lazy_reads == 5 && legacy_reads > 4000);
    std::puts("Frame camera selection: auxiliary/main ordering and history advance PASS");
    std::puts("Camera constant reads: lazy validation matches legacy across invalid/repeated bindings, larger views and frame resets PASS");
}
