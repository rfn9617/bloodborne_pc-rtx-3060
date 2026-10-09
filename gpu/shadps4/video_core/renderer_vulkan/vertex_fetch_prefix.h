// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Vulkan::VertexFetch {

// A prefix keeps the original vertex-buffer origin and all Vulkan draw indices intact.
// Every 16-bit index fits this bound, even when RAM changes or the GPU writes indices.
// Scanning every index on every draw was slower than binding the original full pools.
inline std::optional<std::uint64_t> Index16End(std::int32_t base_vertex) {
    if (base_vertex < 0) return {};
    return std::uint64_t(base_vertex) + 65536;
}

struct Request {
    std::uint64_t base, size;
};

// Trim only the tail of an existing merged range. Do not split it into more cache calls.
inline std::uint64_t MergedEnd(std::uint64_t base, std::uint64_t end,
                               std::span<const Request> requests) {
    auto result = base;
    for (const auto& request : requests) {
        if (!request.size || request.base < base || request.base >= end) continue;
        if (request.size > end - request.base) return end;
        result = std::max(result, request.base + request.size);
    }
    return result == base ? end : result;
}

inline std::optional<std::uint64_t> DirectEnd(std::uint32_t first, std::uint32_t count) {
    if (!count) return {};
    return std::uint64_t(first) + count;
}

inline bool BindingFits(std::uint64_t arena_size, std::uint64_t offset,
                        std::uint64_t declared) {
    return offset <= arena_size && declared <= arena_size - offset;
}

inline std::uint64_t PrefixSize(std::uint64_t declared, std::uint32_t stride,
                                std::optional<std::uint64_t> end,
                                std::uint64_t stream_threshold) {
    // PS4 vertex formats contain at most 16 bytes and this renderer uses attribute offset 0.
    // Keep the whole last attribute even for strides smaller than its format footprint.
    constexpr std::uint64_t AttributeBytes = 16;
    if (!end || !*end || !stride || declared <= stream_threshold ||
        *end - 1 > (std::numeric_limits<std::uint64_t>::max() - AttributeBytes) / stride) {
        return declared;
    }
    const auto required = (*end - 1) * stride + AttributeBytes;
    if (required > declared) return declared; // Preserve the original robust out-of-bounds path.
    // Do not turn a resident vertex pool into per-draw stream copies.
    return std::min(declared, std::max(required, stream_threshold + 1));
}

} // namespace Vulkan::VertexFetch
