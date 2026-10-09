// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif

int main(int argc, char** argv) {
    const bool sparse = argc > 1 && std::string_view(argv[1]) == "sparse";
    const bool disabled = argc > 1 && std::string_view(argv[1]) == "disabled";
#ifdef _WIN32
    _putenv_s("BB_GPU_PROFILE", disabled ? "0" : "1");
    _putenv_s("BB_GPU_PROFILE_EVERY", sparse ? "61" : "1");
#else
    setenv("BB_GPU_PROFILE", disabled ? "0" : "1", 1);
    setenv("BB_GPU_PROFILE_EVERY", sparse ? "61" : "1", 1);
#endif
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::GpuProfiler::Init(instance, scheduler);
    auto* profiler = Vulkan::GpuProfiler::Get();
    if (disabled) {
        assert(!profiler);
        std::puts("GPU profiler: disabled path PASS");
        return 0;
    }
    assert(profiler && !profiler->IsSampling());
    if (sparse) {
        unsigned descriptions = 0;
        for (unsigned frame = 0; frame < 183; ++frame) {
            profiler->BeginFrame();
            assert(profiler->IsSampling() == (frame % 61 == 0));
            profiler->Mark(1, [&] { ++descriptions; return "sample start"; });
            profiler->Mark(2, [&] { ++descriptions; return "sample end"; });
            if (!profiler->IsSampling()) {
                assert(!profiler->Current() && !profiler->Records(&scheduler));
                profiler->Resume(1); // must not add commands on unsampled frames
            }
            scheduler.Finish(); // test only: complete each actual GPU submission
        }
        const auto result = profiler->GetDiagnostics();
        assert(descriptions == 2 && result.sampled_frames == 3);
        assert(result.collected_frames == 3 && result.collected_segments >= 6);
        assert(!result.busy_skips && !result.truncated_frames);
        assert(std::isfinite(result.collected_ms) && result.collected_ms >= 0);
        std::puts("GPU profiler: actual Vulkan timestamps, three samples / 183 frames, inactive callbacks skipped PASS");
        return 0;
    }
    // Leave four sampled frames in the same unsubmitted command buffer. Query slots must
    // remain intact when the ring wraps: collect/reset/wait would be invalid or deadlock.
    for (unsigned frame = 0; frame < 9; ++frame) {
        profiler->BeginFrame();
        profiler->Mark(1, [] { return "unsubmitted"; });
    }
    auto result = profiler->GetDiagnostics();
    assert(result.sampled_frames == 4 && result.collected_frames == 0);
    assert(result.busy_skips == 5 && !profiler->IsSampling());
    scheduler.Finish();
    for (unsigned frame = 0; frame < 4; ++frame) {
        profiler->BeginFrame();
    }
    result = profiler->GetDiagnostics();
    assert(result.collected_frames == 4 && result.collected_segments == 4);
    assert(std::isfinite(result.collected_ms));
    // Too many segments must not distort rankings: discard the truncated frame, then
    // reuse the completed query slot and verify that ordinary samples still work.
    for (unsigned mark = 0; mark < 4200; ++mark) {
        profiler->Mark(3, [] { return "overflow"; });
    }
    profiler->BeginFrame();
    scheduler.Finish();
    for (unsigned frame = 0; frame < 4; ++frame) {
        profiler->BeginFrame();
    }
    assert(profiler->GetDiagnostics().truncated_frames == 1);
    profiler->Mark(4, [] { return "after overflow"; });
    profiler->BeginFrame();
    scheduler.Finish();
    for (unsigned frame = 0; frame < 4; ++frame) {
        profiler->BeginFrame();
    }
    result = profiler->GetDiagnostics();
    assert(result.collected_frames >= 5 && result.truncated_frames == 1);
    std::puts("GPU profiler: unsubmitted query ring preserved without waiting, later recovery, overflow discarded PASS");
}
