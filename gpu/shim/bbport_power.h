// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: power-related statistics for BB_FRAME_STATS (Windows): the CPU time of the process and
// of the whole machine, and the NVIDIA GPU's power, clocks, temperature, load and clock limits
// (NVML from the driver, loaded at run time). One "Power:" line per statistics window.
#pragma once

namespace BbPower {

/// Starts the sampling (call once at startup; BB_FRAME_STATS=1 only).
void Start();

/// Prints the averages since the previous call over `frames` frames (no-op without
/// BB_FRAME_STATS).
void PrintWindow(double seconds, unsigned frames);

/// CPU time of the whole process so far (user + kernel, all threads), in seconds.
double ProcessCpuSeconds();

} // namespace BbPower
