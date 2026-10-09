// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <span>
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {

// The existing linear image writeback layout: mip zero, all layers, depth aspect only for
// depth/stencil. This does not replace the tiler-based lossless GC color preservation path.
struct LinearImageReadback {
    VAddr address;
    Vulkan::StagingBufferRef staging;
    u32 bytes;
};

inline std::optional<LinearImageReadback> QueueLinearImageReadback(
    Vulkan::Runtime& runtime, Image& image, bool held) {
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return std::nullopt;
    }
    const auto& info = image.info;
    const u32 bytes = info.pitch * info.size.height * info.size.depth *
                      info.resources.layers * (info.num_bits / 8);
    ASSERT(bytes <= info.guest_size);
    const auto staging = runtime.GetStagingPool().Request(bytes, MemoryType::HostCached, 16, held);
    const vk::BufferImageCopy copy{
        .bufferOffset = staging.offset,
        .bufferRowLength = info.pitch,
        .bufferImageHeight = info.size.height,
        .imageSubresource = {
            .aspectMask = info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                              : vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = info.resources.layers,
        },
        .imageExtent = {info.size.width, info.size.height, info.size.depth},
    };
    runtime.DownloadImage(&image, staging.buffer, std::span{&copy, 1});
    return LinearImageReadback{info.guest_address, staging, bytes};
}

// All staging allocations must be held: GPU completion alone must not allow the pool to
// recycle an earlier image before its bytes have been copied to guest RAM. Writes complete
// synchronously before returning, including when a guest writeback rejects an unmapped range.
template <typename Write>
u32 CompleteLinearImageReadbacks(Vulkan::Runtime& runtime, Vulkan::Scheduler& scheduler,
                                std::span<const LinearImageReadback> readbacks, Write&& write) {
    if (readbacks.empty()) {
        return 0;
    }
    scheduler.Finish();
    u32 successful = 0;
    for (const auto& readback : readbacks) {
        readback.staging.Invalidate();
        successful += write(readback.address, readback.staging.mapped, readback.bytes);
        runtime.GetStagingPool().FreeDeferred(readback.staging);
    }
    return successful;
}
} // namespace VideoCore
