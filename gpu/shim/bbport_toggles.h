// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// bbport (Windows): no signals. The loader's exception handler resumes the faulting thread
// in bb_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int bb_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations, runtime_disabled_optimizations_high;
/// The current BB_TOGGLE_AB phase plus one, 0 without it (runtime_memory.c).
extern "C" unsigned runtime_toggle_phase;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    // RTX 3060 CPU work of the draw recording thread (2026-10-10), A/B in one run:
    ResidencyFastPath = 1ull << 58, ///< one pass over resident backings, no map lookups
    IndexRangeLongCache = 1ull << 59, ///< index ranges re-scanned every 240 frames, not 32
    SamplerMemo = 1ull << 60,         ///< recent samplers found without the cache lock
    PaletteHashStageA = 1ull << 61,   ///< bone palettes hashed on stage A, not read from VRAM
    // Less idle spinning (2026-10-10, laptop power), A/B in one run:
    ShortRecorderSpin = 1ull << 56, ///< the Vulkan recorder sleeps after 40 us without work, not 200
    DrainSleep = 1ull << 62,        ///< stage A sleeps in long drains instead of yielding in a loop
    PrepWakeOne = 1ull << 63,       ///< one draw-preparation worker woken per scanned submission
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
/// Bits 64 and up: the second number of BB_TOGGLE_FILE, "<low>+<high>" masks in BB_TOGGLE_AB.
enum class High : std::uint64_t {
    /// RunScaled with DLSS/FSR 4 and extra sharpening: one RCAS pass from the upscaler's output
    /// straight into the UI image, instead of a copy, RCAS in place and a blit.
    MergedUpscalerOutput = 1ull << 0, // 64
    /// No image barrier between two reads in the same layout (only the stages are merged).
    ReadAfterReadBarriers = 1ull << 1, // 65
    /// The GPU command thread waits as BB_FRAMES_AHEAD says; off: one frame more ahead.
    FramesAheadAsSet = 1ull << 2, // 66
};
inline bool Disabled(High bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations_high, __ATOMIC_RELAXED) &
            static_cast<std::uint64_t>(bit)) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
inline std::atomic<std::uint64_t> camera_snapshots{0}, camera_guest_changed{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Pipeline barriers recorded (Runtime::FlushBarriers) and the image barriers in them.
inline std::atomic<std::uint64_t> barrier_calls{0}, barrier_images{0};
/// The game shows no 3D scene for a while (loading screens, title menu): the CPU sampler's
/// BB_CPU_SAMPLE=3 records only then.
inline std::atomic<bool> loading_screen{false};
/// F9: save the next few seconds of presented frames as pictures (Presenter::PrepareFrame).
inline std::atomic<bool> frame_burst_request{false};
/// F10: a diagnostic snapshot (picture, frame analysis, upscaler inputs); +1 per press.
inline std::atomic<std::uint32_t> snapshot_seq{0};
/// This loading screen goes without the frame limit (BB_FAST_LOADING, the present thread).
inline std::atomic<bool> loading_unlimited{false};
/// Render pass instances begun (Scheduler::BeginRendering).
inline std::atomic<std::uint64_t> render_passes{0};
/// Image barriers made by Image::GetBarriers by old and new layout (LayoutIndex) and whether
/// the old access wrote; read-after-read barriers left out (BbToggle::High::ReadAfterReadBarriers).
inline constexpr unsigned NumLayoutKinds = 16;
inline std::atomic<std::uint64_t> image_barrier_kinds[NumLayoutKinds][NumLayoutKinds][2];
inline std::atomic<std::uint64_t> read_after_read_skipped{0};
inline constexpr const char* LayoutNames[NumLayoutKinds] = {
    "undefined", "general", "color", "depth-stencil", "depth-stencil read-only", "shader read",
    "transfer src", "transfer dst", "depth read-only/stencil", "depth/stencil read-only",
    "depth", "depth read-only", "feedback loop", "present", "other", "?"};
/// VkImageLayout -> LayoutNames index.
inline unsigned LayoutIndex(int layout) {
    switch (layout) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        return unsigned(layout);
    case 1000117000: return 8;  // DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
    case 1000117001: return 9;  // DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL
    case 1000241000: return 10; // DEPTH_ATTACHMENT_OPTIMAL
    case 1000241001: return 11; // DEPTH_READ_ONLY_OPTIMAL
    case 1000339000: return 12; // ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
    case 1000001002: return 13; // PRESENT_SRC_KHR
    default: return 14;
    }
}
/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace BbStats
