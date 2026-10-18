// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <deque>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;
namespace Vulkan {
class Instance;
class Scheduler;

// Opt-in FSR FI/OF, independent of the temporal upscaler. All commands and
// scratch images belong to the draw queue. Presenter copies the completed
// generated image into its fence-owned Frame before any next-frame writes.
class FrameGenerator {
public:
    FrameGenerator(const Instance& instance, Scheduler& scheduler);
    ~FrameGenerator();
    static bool Requested();
    static bool FastPath();
    // May flush while recreating resources; call before retaining a CommandBuffer.
    bool Prepare(u32 width, u32 height);
    void Inputs(vk::ImageView depth, vk::Image motion, u32 width, u32 height,
                float near_plane, float fov, std::array<float, 2> jitter,
                std::array<std::array<float, 3>, 4> camera, float frame_ms, bool reset);
    // Capture a UI-free reference in the game's intermediate encoding.
    void CaptureScene(vk::ImageView clean_scene, vk::ImageLayout layout, u32 width, u32 height);
    // Actual finished display, UNORM (never sRGB decoded), after game color/fade.
    void GenerateFinal(vk::ImageView display, vk::ImageView hudless, vk::ImageLayout layout, u32 width, u32 height,
                       vk::ImageLayout scene_layout=vk::ImageLayout::eGeneral);
    // Replay the game's display pass with the exact pre-UI source. Reuse composed
    // as its attachment, then clean as the canonical HUD-less SDK color.
    vk::ImageView CleanView(vk::Format format, vk::ComponentMapping mapping);
    vk::ImageView BeginDisplayReplay(vk::Format format);
    void EndDisplayReplay();
    // Attachment storage and VideoOut sampling may use different R/B ordering.
    vk::ImageView ReplayedDisplay(vk::Format sampling_format=vk::Format::eUndefined);
    // Called at the display-pass boundary, before the next scene begins.
    void Boundary();
    // Completed game display including UI, sampled as typed RGBA/BGRA UNORM.
    // Compare with final HUD-less color; preserve exact HUD after interpolation.
    // Returns ShaderReadOnlyOptimal for PP/FSR sampling; presenter copies it
    // into a fence-owned Frame before the next Compose overwrites it.
    vk::ImageView Compose(vk::ImageView scene_with_ui, vk::ImageLayout layout, bool srgb);
    vk::Image CleanImage() const { return clean.image; }
    vk::Image OutputImage() const { return composed.image; }
    struct DiagnosticInputs {
        vk::Image clean{},interpolated{},motion{},depth{};
        u32 width=0,height=0;
        u64 frame_id=0,resets=0;
        float frame_ms=0;
        std::array<float,2> jitter{};
    };
    DiagnosticInputs GetDiagnosticInputs() const {
        if(!context || failed) return {};
        return {clean.image,interpolated.image,motion,depth.image,width,height,next_id-1,resets,frame_ms,jitter};
    }
    bool HasHistory() const { return context != nullptr; }
    void Stop();
    // Opt-in sparse GPU/CPU attribution, independent of per-draw profiling.
    void TimingMark(u32 index);
    void FinishTimings();

private:
    bool Ensure(u32 width, u32 height);
    bool Allocate(VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                  u32 width, u32 height, vk::Format format);
    void Pipelines();
    void Copy(vk::ImageView input, vk::ImageLayout layout, VideoCore::UniqueImage& output,
              vk::ImageView output_view, bool depth);
    void Report();
    void BeginTimings();
    void CollectTimings();
    void PrintTimings();
    static constexpr u32 TimingStages=8, TimingSlices=4;
    struct TimingSlice {
        u64 tick=0;
        u32 mask=0;
        bool pending=false;
        std::array<double,TimingStages*2> cpu{};
    };
    struct TimingTotal {double gpu=0,cpu=0;u32 count=0;};
    vk::UniqueQueryPool timing_pool;
    std::array<TimingSlice,TimingSlices> timing_slices{};
    std::array<TimingTotal,TimingStages> timing_totals{};
    u32 timing_cursor=0, timing_frames=0, timing_every=120;
    bool timing_active=false;
    double timing_period=0;
    std::chrono::steady_clock::time_point timing_report{};

    const Instance& instance;
    Scheduler& scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext* context = nullptr;
    bool enabled = false, failed = false, inputs_ready = false;
    bool reference_ready = false, suspended = false;
    bool captured = false, pending_generated = false, reset_history = true;
    u32 width = 0, height = 0, idle_frames = 0;
    u64 next_id = 1, prepared = 0, generated = 0, resets = 0, skipped = 0;
    u32 budget_checks = 0;
    std::deque<std::pair<u64, u64>> retained;
    float near_plane = 0.1f, fov = 1.0f, frame_ms = 13.8889f;
    std::array<float, 2> jitter{};
    std::array<std::array<float, 3>, 4> camera{};
    vk::Image motion{};
    VideoCore::UniqueImage clean, depth, interpolated, composed, reference_display;
    vk::UniqueImageView reference_display_view;
    vk::UniqueImageView clean_view, depth_view, interpolated_view, composed_view, composed_srgb;
    struct MirrorView {vk::Format format; vk::ComponentMapping mapping; vk::UniqueImageView view;};
    std::vector<MirrorView> clean_views, replay_views;
    vk::ImageView Mirror(VideoCore::UniqueImage& image, std::vector<MirrorView>& views,
                         vk::Format format, vk::ComponentMapping mapping, vk::ImageUsageFlags usage);
    vk::Format replay_format{};
    bool replay_ready = false;
    vk::UniqueDescriptorSetLayout copy_desc, ui_desc;
    vk::UniquePipelineLayout copy_layout, ui_layout;
    vk::UniquePipeline color_copy, depth_copy, ui_pipeline;
    std::chrono::steady_clock::time_point last_report{};
};
} // namespace Vulkan
