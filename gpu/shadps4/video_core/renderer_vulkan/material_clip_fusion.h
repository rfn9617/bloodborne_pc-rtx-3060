// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace Vulkan::MaterialClipFusion {
// Fuses only four adjacent Input vec4 < 0 demote blocks. All four comparisons
// and input reads are retained; no interpolation or clipping plane is removed.
bool Convert(std::span<const std::uint32_t> source, std::vector<std::uint32_t>& output);
}
