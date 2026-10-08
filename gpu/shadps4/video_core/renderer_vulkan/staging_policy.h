// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan::StagingPolicy {
constexpr std::uint64_t BlockMiB = 16;
constexpr std::uint64_t MaxKeepMiB = 4096;
constexpr std::uint64_t KeepBlocks(std::uint64_t mib) {
    return std::min(mib, MaxKeepMiB) / BlockMiB;
}
constexpr std::uint64_t PrewarmBlocks(std::uint64_t keep, std::uint64_t mib) {
    return std::min(keep, KeepBlocks(mib));
}
constexpr std::uint64_t RetainedBlocks(bool host_upload, std::uint64_t keep) {
    return host_upload ? keep : 0;
}
} // namespace Vulkan::StagingPolicy
