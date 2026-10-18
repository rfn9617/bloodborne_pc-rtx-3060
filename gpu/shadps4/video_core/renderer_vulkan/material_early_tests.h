// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace Vulkan::MaterialEarlyTests {
// Only accepts a single fragment entry point without discard, depth/coverage
// exports, attachment reads, stores outside color outputs, atomics or barriers.
// The instruction stream is otherwise copied verbatim; failure leaves output empty.
bool Convert(std::span<const std::uint32_t> source, std::vector<std::uint32_t>& output);
}
