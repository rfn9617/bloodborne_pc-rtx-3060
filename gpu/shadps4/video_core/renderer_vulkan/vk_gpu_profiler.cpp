// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void GpuProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* env = std::getenv("BB_GPU_PROFILE");
    if (!env || env[0] != '1' || instance_ptr) {
        return;
    }
    instance_ptr = new GpuProfiler(instance, scheduler);
}

GpuProfiler::GpuProfiler(const Instance& instance, Scheduler& scheduler_)
    : device{instance.GetDevice()}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    };
    pool = Check<"create timestamp query pool">(device.createQueryPoolUnique(info));
    device.resetQueryPool(*pool, 0, NumSlices * SliceQueries);
    period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    for (auto& k : keys) {
        k.reserve(SliceQueries);
    }
    if (const char* value = std::getenv("BB_GPU_PROFILE_EVERY")) {
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed >= 1 && parsed <= 1000000) {
            every = static_cast<u32>(parsed);
        }
    }
    std::printf("GPU profile: on (timestamp period %.2f ns, one frame in %u, nonblocking)\n",
                period_ns, every);
}

void GpuProfiler::WriteTimestamp(u64 key) {
    if (used[slice] + 1 >= SliceQueries) {
        truncated[slice] = true;
        return; // discard this frame's incomplete attribution when collecting it
    }
    // Outside render passes: radv_CmdWriteTimestamp2 crashed inside some. Marks sit where a
    // pass, dispatch or submission ends anyway.
    scheduler.EndRendering();
    const u32 query = slice * SliceQueries + used[slice]++;
    keys[slice].push_back(key);
    current = key;
    scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuProfiler::BeginFrame() {
    // Close the frame: one more timestamp without a label.
    if (recording && used[slice] > 0 && used[slice] < SliceQueries) {
        scheduler.EndRendering();
        const u32 query = slice * SliceQueries + used[slice]++;
        scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        });
        pending[slice] = true;
        ticks[slice] = scheduler.CurrentTick();
    }
    recording = false;
    current = 0;
    slice = (slice + 1) % NumSlices;
    // A delayed GPU or an unsubmitted command buffer must never block or have its queries reset.
    if (!pending[slice] || Collect(slice)) {
        if (used[slice]) {
            device.resetQueryPool(*pool, slice * SliceQueries, used[slice]);
        }
        used[slice] = 0;
        keys[slice].clear();
        pending[slice] = false;
        truncated[slice] = false;
    }
    if (diagnostics.observed_frames++ % every == 0) {
        if (pending[slice]) {
            ++diagnostics.busy_skips;
        } else {
            recording = true;
            ++diagnostics.sampled_frames;
        }
    }
    Print();
}

bool GpuProfiler::Collect(u32 which) {
    if (!scheduler.IsFree(ticks[which])) {
        return false;
    }
    const u32 count = used[which];
    std::vector<u64> stamps(count);
    // The timeline is complete; omit eWait even if a driver still reports queries unavailable.
    const auto result = device.getQueryPoolResults(
        *pool, which * SliceQueries, count, count * sizeof(u64), stamps.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    if (truncated[which]) {
        ++diagnostics.truncated_frames;
        return true;
    }
    for (u32 i = 0; i + 1 < count; ++i) {
        const double ms = double(stamps[i + 1] - stamps[i]) * period_ns / 1e6;
        auto& total = totals[keys[which][i]];
        total.ms += ms;
        ++total.segments;
        ++diagnostics.collected_segments;
        diagnostics.collected_ms += ms;
    }
    ++frames;
    ++diagnostics.collected_frames;
    return true;
}

void GpuProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5) || frames == 0) {
        return;
    }
    window = now;
    std::vector<std::pair<u64, Total>> sorted(totals.begin(), totals.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double sum = 0;
    for (const auto& [key, total] : sorted) {
        sum += total.ms;
    }
    std::printf("GPU profile: %.2f ms/sampled-frame over %llu samples (every %u frames), "
                "%zu labels; lifetime busy-skips=%llu truncated=%llu\n", sum / frames,
                static_cast<unsigned long long>(frames), every, sorted.size(),
                static_cast<unsigned long long>(diagnostics.busy_skips),
                static_cast<unsigned long long>(diagnostics.truncated_frames));
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 30); ++i) {
        const auto& [key, total] = sorted[i];
        std::printf("  %6.3f ms/frame %5.1f/frame  %s\n", total.ms / frames,
                    double(total.segments) / frames, described[key].c_str());
    }
    totals.clear();
    frames = 0;
}

} // namespace Vulkan
