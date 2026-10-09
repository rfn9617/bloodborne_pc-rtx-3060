// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {
// Only layouts whose GPU bytes can be preserved losslessly by the existing tiler.
// Depth/stencil, compression, MSAA and mip chains require separate readback handling.
inline bool CanReadbackColor(const Image& image) {
    const auto& i = image.info;
    return image.backing && i.num_samples == 1 && image.backing->num_samples == 1 &&
           i.resources.levels == 1 && i.resources.layers == 1 && i.size.depth == 1 &&
           !i.props.is_depth && !i.props.has_stencil && !i.props.is_block && !i.props.is_volume &&
           i.pixel_format == image.backing->image.image_ci.format &&
           (i.num_bits == 8 || i.num_bits == 16 || i.num_bits == 32 || i.num_bits == 64 ||
            i.num_bits == 128) && i.guest_size != 0;
}

// Records a copy in the guest's original tiling layout. Finish the scheduler and invalidate
// this mapping before accessing it; keep the original image/page tracking alive until then.
inline Vulkan::StagingBufferRef ReadbackColor(Vulkan::Runtime& runtime, TileManager& tiler,
                                             Image& image) {
    ASSERT(CanReadbackColor(image));
    const auto& i = image.info;
    const auto readback = runtime.GetStagingPool().Request(i.guest_size, MemoryType::HostCached);
    vk::BufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = i.mips_layout[0].pitch,
        .bufferImageHeight = i.mips_layout[0].height,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {i.size.width, i.size.height, 1},
    };
    tiler.TileImage(image, std::span{&copy, 1}, readback.buffer, readback.offset);
    runtime.AccessBuffer(readback.buffer, readback.offset, readback.size,
                         vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    runtime.FlushBarriers();
    return readback;
}
} // namespace VideoCore
