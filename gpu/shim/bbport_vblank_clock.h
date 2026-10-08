// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the display's real refresh (vertical blank) times, for a frame limit locked to the
// display (VideoOutDriver::PresentThread). With 72 FPS on a 144 Hz display every frame must stay
// on screen for exactly two refreshes; a limiter that only counts 13.9 ms on the CPU clock
// drifts against the display, and frames finishing just after a refresh are shown for three
// refreshes and the next one for one: judder that frame times do not show.
//
// Windows: a thread waits for each vertical blank of the window's monitor
// (IDXGIOutput::WaitForVBlank). Elsewhere, or when DXGI fails, Start() returns false and the
// limiter keeps its CPU timer.

#pragma once

#include <chrono>
#include <cstdint>

namespace BbVblank {

/// Starts the clock for the monitor showing `native_window` (an HWND on Windows; null: the
/// primary monitor). Idempotent; false when unavailable.
bool Start(void* native_window);

struct Sample {
    std::chrono::steady_clock::time_point time; ///< the latest vertical blank
    std::chrono::nanoseconds period;            ///< measured refresh period
};
/// The latest vertical blank; false before the first one or when the clock is not running.
bool Latest(Sample& out);

void Stop();

} // namespace BbVblank
