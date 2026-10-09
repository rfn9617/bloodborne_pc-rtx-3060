// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_CPU_SAMPLE=1, a sampling CPU profiler of the whole process (Windows only).
#pragma once

namespace BbCpuSampler {

/// Starts the sampler thread when BB_CPU_SAMPLE=1 (no-op elsewhere and on other systems).
void Start();

} // namespace BbCpuSampler
