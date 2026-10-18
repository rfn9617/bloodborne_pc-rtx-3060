// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: object motion vectors for temporal upscaling (docs/ROADMAP.md, step 1).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include "video_core/renderer_vulkan/motion_history.h"

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }
    [[nodiscard]] size_t ResidentImages() const noexcept { return images.size(); }

    using DrawInfo = Motion::Draw;
    u32 PrepareDraw(const DrawInfo& draw);
    /// Small skeletons: whether the bone palette changed (Motion::History::Moving).
    bool Moving(const Motion::History::GateKey& key, u64 palette) {
        return history.Moving(key, palette);
    }
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan,
                                               u64 revalidate = Motion::IndexRangeCache::Revalidate) {
        return index_ranges.Get(key, frame, scan, revalidate);
    }

    /// Rendering of a motion pipeline: the motion attachment (render-target size).
    void Attach(RenderState& state, u32 width, u32 height);

    /// Start of a frame (display pass).
    void OnFrameStart();
    void InvalidateHistory() { history.NextFrame(); history.NextFrame(); }

    /// The motion image for the compose pass (layout General after this call) and whether it
    /// holds this frame's vectors at `width` x `height`.
    vk::ImageView PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height, bool& valid);
    /// The image of the last PrepareRead (layout General), or null.
    [[nodiscard]] vk::ImageView View() const noexcept {
        return read_image ? *read_image->view : vk::ImageView{};
    }
    [[nodiscard]] vk::Image Image(u32 width, u32 height) const noexcept {
        const auto found = images.find({width, height});
        return found != images.end() && found->second->written
            ? vk::Image(found->second->image) : vk::Image{};
    }

private:
    struct MotionImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        bool written = false;
        u64 last_frame = 0;
        std::chrono::steady_clock::time_point last_use{};
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    MotionImage& EnsureImage(u32 width, u32 height);

    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false;

    // Parameter ring (host visible): FrameSlots frames of ParamsPerFrame entries (two u32x4),
    // element 0 all zero (motion off).
    static constexpr u32 FrameSlots = 4;
    static constexpr u32 ParamsPerFrame = 8192;
    // Positions: two bounded, per-frame arenas; element 0 is reserved. Default 8 Mi
    // vec4 entries = 256 MiB. Opt-in compact x/y/w keeps all three float32 values
    // losslessly, allowing 12 Mi entries = 288 MiB. Old draws do not accumulate:
    // History drops entries each frame and immediately reuses the previous arena.
    const bool compact_history;
    const bool diagnostics;
    const u32 PositionsPerFrame;
    Motion::History history{PositionsPerFrame};
    Motion::IndexRangeCache index_ranges;
    std::array<u64, FrameSlots> params_ticks{};
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params_mapped{};
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};

    u64 frame = 0;
    u32 params_used = 0;
    u32 peak_vertices = 0, peak_params = 0;
    u64 param_skips = 0, retired_images = 0;
    struct InvalidSummary {
        u64 count = 0;
        DrawInfo example{};
    };
    std::map<u64, InvalidSummary> invalid_by_shader;
    std::chrono::steady_clock::time_point frame_time{};
    // Auxiliary and main G-buffers can alternate resolutions within every frame.
    // Reuse each attachment instead of draining the GPU and reallocating on every switch.
    std::map<std::pair<u32, u32>, std::unique_ptr<MotionImage>> images;
    MotionImage* read_image = nullptr;
};

} // namespace Vulkan
