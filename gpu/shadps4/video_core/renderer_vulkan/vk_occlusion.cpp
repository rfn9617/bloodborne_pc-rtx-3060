// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: real occlusion query results (vk_occlusion.h). Design after droogie/bbhost b68ed17.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/occlusion_model.h"
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_occlusion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"



namespace Vulkan {

namespace {
using OcclusionModel::MaxPipes;
using OcclusionModel::Share;
using OcclusionModel::Unmeasured;
using OcclusionModel::ValidBit;

u64 EnvU64(const char* name, u64 fallback) {
    const char* env = std::getenv(name);
    return env && *env ? std::strtoull(env, nullptr, 10) : fallback;
}

/// Reads guest memory through the runtime's backing view: no page protection (a read fault on
/// this thread would wait for the GPU thread), zeroes where nothing is mapped.
void ReadGuest(VAddr address, void* data, u64 size) {
    runtime_memory_read_backing(address, data, size);
}

/// A Gnm block as the game reads it now.
OcclusionModel::Block ReadBlock(VAddr block, u32 pipes) {
    std::array<u64, 2 * MaxPipes> words{};
    ReadGuest(block, words.data(), 16ull * pipes);
    return OcclusionModel::ReadBlock(words.data(), pipes);
}
} // namespace

struct Occlusion::Shared {
    /// The resolver's state and the pool's life (the destructor takes it too).
    std::mutex mutex;
    /// Only the queue: the recording side never waits for a resolve in progress.
    std::mutex queue_mutex;
    bool alive = true;
    vk::Device device;
    vk::QueryPool pool;
    std::mutex buffers_mutex;
    std::unordered_map<u64, std::pair<VAddr, u64>> buffers;
    std::atomic<u64> buffers_min{~0ull}, buffers_max{0};
    bool precise = false;
    u64 max_lag = 3;
    u64 min_samples = 0;  ///< BB_OCCLUSION_MIN_SAMPLES: 1..N-1 reads visible
    u32 hold_frames = 0;  ///< BB_OCCLUSION_HOLD_FRAMES: zeros stay visible this long

    struct Pending {
        u64 tick;   ///< the submission holding the dump (and every query before it)
        VAddr va;
        u64 frame;
        u64 serial;
        u64 first;  ///< the interval's first query, as a running count
        u32 count;  ///< its queries
        u32 pipes;
        bool poison; ///< a draw in the interval got no query
        u64 token;
        RuntimeOcclusionStamp stamp;
    };
    std::deque<Pending> pending; ///< under queue_mutex
    std::vector<Pending> ready;  ///< taken from it by the resolver
    /// Per query: the sample scale of its draw (16.16; reduced scene targets), written by the
    /// recording side before the dump is queued under the mutex.
    std::array<u32, PoolSize> scale{};
    std::atomic<u64> released{0};        ///< queries read and reset (reusable)
    std::atomic<u64> resolved_serial{0}; ///< dumps processed, in order

    u64 total = 0; ///< the DBs' running count
    /// What we last wrote at each dump address: our own value may be overwritten (a block the
    /// game reuses), anything else that is not zero is not ours to write over.
    std::unordered_map<VAddr, u64> written;
    std::unordered_map<VAddr, u64> mapping_ids;
    struct KnownBlock { u64 serial; RuntimeOcclusionStamp stamp; };
    std::unordered_map<VAddr, KnownBlock> known_blocks; // buffers_mutex
    /// Begin dumps (+0) written in this use of their block: the total they wrote, taken by the
    /// block's end dump (+8) for the block's result.
    std::unordered_map<VAddr, u64> begins;
    /// Per block (its +0 address): the hysteresis of its results (OcclusionModel::BlockHistory).
    std::unordered_map<VAddr, OcclusionModel::BlockHistory> histories;
    std::unordered_map<VAddr, u32> hold_zeros;
    std::vector<u64> results;

    // Statistics (BB_FRAME_STATS), under the mutex except the recording side's atomics.
    u64 dumps = 0, queries = 0, unavailable = 0, poisoned = 0, visible = 0, occluded = 0;
    u64 late = 0, foreign = 0, unbacked = 0, written_dumps = 0;
    u64 held_visible = 0; ///< blocks that measured nothing but read visible (hysteresis)
    u64 late_written = 0; ///< late dumps of known blocks written all the same
    std::atomic<u64> pool_full{0}, pred_sets{0}, pred_resolved{0}, pred_skipped{0},
        pred_drawn{0};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 window_frames = 0, window_draws = 0;
    std::array<u64, 14> last{};

    void Resolve(u64 tick);
    void Process(const Pending& dump);
    /// The end dump of `block` (its +0 address) is about to be written: hysteresis.
    void HoldVisible(VAddr block);
    /// False when it did not land (not a query block now, or unmapped).
    bool WriteBlock(const Pending& dump);
    void Report();
};

bool Occlusion::Wanted() {
    static const bool on = [] {
        const char* env = std::getenv("BB_OCCLUSION");
        return env && std::strcmp(env, "1") == 0;
    }();
    return on;
}

Occlusion::Occlusion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

std::unique_ptr<Occlusion> Occlusion::Create(const Instance& instance, Scheduler& scheduler) {
    if (!Wanted()) {
        std::printf("Occlusion: off (BB_OCCLUSION=0)\n");
        return nullptr;
    }
    std::unique_ptr<Occlusion> occlusion{new Occlusion(instance, scheduler)};
    if (!occlusion->Init()) {
        return nullptr;
    }
    return occlusion;
}

bool Occlusion::Init() {
    // Results are read and the queries reset on the CPU once the GPU is past them: no reset
    // commands outside render passes, no waits.
    if (!instance.IsHostQueryResetSupported()) {
        std::printf("Occlusion: off - the device has no host query reset; every query the game "
                    "makes reads visible, as before\n");
        return false;
    }
    const vk::Device device = instance.GetDevice();
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = PoolSize,
    };
    const auto [result, pool] = device.createQueryPool(info);
    if (result != vk::Result::eSuccess) {
        std::printf("Occlusion: off - no query pool (%s); every query the game makes reads "
                    "visible, as before\n",
                    vk::to_string(result).c_str());
        return false;
    }
    device.resetQueryPool(pool, 0, PoolSize);
    precise = instance.IsOcclusionQueryPreciseSupported();
    cap = static_cast<u32>(std::clamp<u64>(EnvU64("BB_OCCLUSION_CAP", 64), 1, 1024));
    shared = std::make_shared<Shared>();
    shared->device = device;
    shared->pool = pool;

    shared->precise = precise;
    shared->max_lag = EnvU64("BB_OCCLUSION_MAX_LAG", 3);
    shared->min_samples = EnvU64("BB_OCCLUSION_MIN_SAMPLES", 0);
    shared->hold_frames = static_cast<u32>(EnvU64("BB_OCCLUSION_HOLD_FRAMES", 0));
    shared->results.resize(2ull * PoolSize);
    std::printf("Occlusion: real sample counts (BB_OCCLUSION=0: the old always-visible fake) - a "
                "query on each of the %u draws after a block's begin, results written when the "
                "GPU has run them (at most %llu frames late), %s; SET_PREDICATION skips only on "
                "known results\n",
                cap, static_cast<unsigned long long>(shared->max_lag),
                precise ? "precise" : "not precise (a nonzero count reads plainly visible)");
    return true;
}

Occlusion::~Occlusion() {
    if (!shared) {
        return;
    }
    // A resolve still queued on the scheduler's thread finds it gone and does nothing.
    std::scoped_lock lk{shared->mutex};
    shared->alive = false;
    shared->device.destroyQueryPool(shared->pool);
    shared->pool = nullptr;
}

Occlusion::DumpTask Occlusion::MakeDump(VAddr va, u32 num_pipes) noexcept {
    DumpTask task{va, issued, BbStats::gpu_frames.load(std::memory_order_relaxed),
                  std::clamp(num_pipes, 1u, MaxPipes), 0, ++next_token, {}};
    const u64 extent = 16ull * (task.pipes - 1) + 8;
    if (runtime_memory_occlusion_capture(va, extent, &task.stamp)) {
        std::scoped_lock lock{shared->buffers_mutex};
        shared->buffers.emplace(task.token, std::pair{va, extent});
        shared->buffers_min.store(std::min(shared->buffers_min.load(), va), std::memory_order_release);
        shared->buffers_max.store(std::max(shared->buffers_max.load(), va + extent), std::memory_order_release);
    }
    return task;
}

bool Occlusion::MayWriteGuestBuffer(VAddr address, u64 size) const noexcept {
    if (!size || address >= shared->buffers_max.load(std::memory_order_acquire) ||
        address + size <= shared->buffers_min.load(std::memory_order_acquire)) return false;
    std::unique_lock lock{shared->buffers_mutex, std::try_to_lock};
    if (!lock.owns_lock()) return true; // never wait on the resolver from stage A
    for (const auto& [token, range] : shared->buffers) {
        if (address < range.first + range.second && range.first < address + size) return true;
    }
    return false;
}

void Occlusion::Dump(const DumpTask& task) {
    // Every draw stage A decoded since the last dump got a query, or the interval reads visible:
    // a draw dropped before recording (pipeline still compiling, filtered, failed) must not
    // count as occluded.
    const bool poison =
        !interval.open || task.issued - interval.issued_at != interval.queries;
    const u64 tick = scheduler.CurrentTick();
    const u64 serial = ++dump_serial;
    if (block_serial.size() > 65536) {
        block_serial.clear();
    }
    block_serial[task.va & ~15ull] = serial;
    {
        std::scoped_lock lk{shared->queue_mutex};
        shared->pending.push_back({tick, task.va, task.frame, serial, interval.first,
                                   interval.queries, task.pipes, poison, task.token, task.stamp});
    }
    if (tick != registered_tick) {
        // One resolve per submission, on the scheduler's thread once the GPU has finished it.
        registered_tick = tick;
        scheduler.DeferPriorityOperation(
            [shared = shared, tick] { shared->Resolve(tick); });
    }
    // Gnm's begin dump (+0 of a 16-byte cell) measures the draws up to its end dump; after any
    // other dump only the next draw is measured (droogie's caps), the rest read visible.
    interval = {true, (task.va & 15) == 0 ? cap : 1u, 0, query_head, task.issued};
    pipes = task.pipes;
}

Occlusion::DrawQuery Occlusion::AllocateQuery(u32 scene_size) noexcept {
    if (query_head - shared->released.load(std::memory_order_acquire) >= PoolSize) {
        shared->pool_full.fetch_add(1, std::memory_order_relaxed);
        return {}; // unmeasured: reads visible
    }
    const u32 index = static_cast<u32>(query_head % PoolSize);
    // A pass into the reduced scene targets counts fewer samples than the PS4's 1920x1080; the
    // count is scaled back so a threshold the game applies means the same.
    u32 scale = 1u << 16;
    if (scene_size) {
        const auto size = SceneResolution::Unpack(scene_size);
        scale = OcclusionModel::SampleScale(size.width, size.height);
    }
    shared->scale[index] = scale;
    ++query_head;
    ++interval.queries;
    return {shared->pool, index,
            precise ? vk::QueryControlFlags{vk::QueryControlFlagBits::ePrecise}
                    : vk::QueryControlFlags{}};
}

void Occlusion::SetPredication(const PredicationTask& task) {
    shared->pred_sets.fetch_add(1, std::memory_order_relaxed);
    if (task.op == 0) {
        pred_mode = PredMode::None;
        pred_visible = pred_unknown = false;
        return;
    }
    if (task.op != 1) {
        pred_mode = PredMode::Draw; // primitive-count predication (streamout): not emulated
        return;
    }
    if (!(task.cont && pred_mode != PredMode::None)) {
        pred_visible = pred_unknown = false;
    }
    // Known only when every dump into the block has been resolved: then the block holds this
    // use's result (or is not ready). Results come a frame late, so mostly unknown: drawn.
    const auto it = block_serial.find(task.block);
    bool known = false;
    {
        std::unique_lock lock{shared->buffers_mutex, std::try_to_lock};
        if (lock.owns_lock() && it != block_serial.end()) {
            const auto end = shared->known_blocks.find(task.block);
            RuntimeOcclusionStamp now{};
            if (end != shared->known_blocks.end() && end->second.serial == it->second &&
                runtime_memory_occlusion_capture(task.block, 16ull * pipes, &now)) {
                known = now.allocation == end->second.stamp.allocation &&
                        now.physical == end->second.stamp.physical;
            }
        }
    }
    if (known) {
        const auto state = ReadBlock(task.block, pipes);
        pred_unknown |= state == OcclusionModel::Block::NotReady;
        pred_visible |= state == OcclusionModel::Block::Visible;
        if (state != OcclusionModel::Block::NotReady) {
            shared->pred_resolved.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        pred_unknown = true;
    }
    // A wrong "occluded" makes geometry vanish: unknown draws.
    const bool draw =
        OcclusionModel::PredicatedDraws(pred_visible, pred_unknown, task.draw_if_visible != 0);
    pred_mode = draw ? PredMode::Draw : PredMode::Skip;
}

bool Occlusion::SkipPredicated() noexcept {
    if (pred_mode == PredMode::Skip) {
        shared->pred_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    shared->pred_drawn.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Occlusion::Shared::Resolve(u64 tick) {
    std::scoped_lock lk{mutex};
    if (!alive) {
        return;
    }
    ready.clear();
    {
        std::scoped_lock queue{queue_mutex};
        while (!pending.empty() && pending.front().tick <= tick) {
            ready.push_back(pending.front());
            pending.pop_front();
        }
    }
    for (const Pending& dump : ready) {
        Process(dump);
    }
    Report();
}

void Occlusion::Shared::Process(const Pending& dump) {
    ++dumps;
    bool bad = dump.poison;
    u64 samples = 0;
    if (dump.count) {
        queries += dump.count;
        // The ring may wrap inside the interval: at most two ranges.
        const u32 first = static_cast<u32>(dump.first % PoolSize);
        const u32 head_part = std::min<u32>(dump.count, PoolSize - first);
        const std::array<std::pair<u32, u32>, 2> ranges{{{first, head_part},
                                                         {0, dump.count - head_part}}};
        for (const auto& [start, count] : ranges) {
            if (!count) {
                continue;
            }
            // Without waiting: the GPU has finished the submission. A query a driver did not
            // resolve (AMD while the window is unfocused, KyoPS4x #220) reads visible.
            const vk::Result result = device.getQueryPoolResults(
                pool, start, count, 16ull * count, results.data(), 16,
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
            for (u32 i = 0; i < count; ++i) {
                const bool failed = result != vk::Result::eSuccess &&
                                    result != vk::Result::eNotReady;
                const bool available = !failed && results[2 * i + 1] != 0;
                unavailable += available ? 0 : 1;
                // Imprecise results may be 0/1 (NVIDIA): one sample read as "nearly hidden" was
                // the likeliest cause of KyoPS4x #219's black world.
                samples += OcclusionModel::Samples(results[2 * i], available, precise,
                                                   scale[start + i]);
            }
            device.resetQueryPool(pool, start, count);
        }
        released.fetch_add(dump.count, std::memory_order_release);
    }
    if (bad) {
        ++poisoned;
        samples += Unmeasured;
    }
    total += samples;
    const u64 lag = BbStats::gpu_frames.load(std::memory_order_relaxed) - dump.frame;
    const bool is_begin = (dump.va & 15) == 0;
    const VAddr block = dump.va & ~15ull;
    if (mapping_ids[block] != dump.stamp.allocation) {
        mapping_ids[block] = dump.stamp.allocation;
        written.erase(block); written.erase(block + 8);
        begins.erase(block); histories.erase(block); hold_zeros.erase(block);
    }
    bool successful = false;
    // A late dump is written into a block we wrote before (it then holds the game's zeroes or
    // our last value, checked by WriteBlock) up to twice the lag: the block keeps its last
    // result instead of reading not ready. Later, or into memory we never wrote, the game may
    // have freed and reused it by now: left as it is (not ready).
    if (lag > max_lag && (lag > 2 * max_lag || !written.contains(dump.va))) {
        ++late;
        if (is_begin) {
            begins.erase(dump.va);
        }
    } else {
        late_written += lag > max_lag ? 1 : 0;
        if ((dump.va & 15) == 8) {
            HoldVisible(dump.va - 8);
        }
        const bool landed = WriteBlock(dump);
        successful = landed;
        if (is_begin) {
            if (landed) {
                if (begins.size() > 65536) {
                    begins.clear();
                }
                begins[dump.va] = total;
            } else {
                begins.erase(dump.va);
            }
        }
    }
    {
        std::scoped_lock lock{buffers_mutex};
        buffers.erase(dump.token);
        if (mapping_ids.size() > 65536) mapping_ids.clear();
        if (known_blocks.size() > 65536) known_blocks.clear();
        if (successful && !is_begin) {
            RuntimeOcclusionStamp full{};
            if (runtime_memory_occlusion_capture(block, 16ull * dump.pipes, &full) &&
                full.allocation == dump.stamp.allocation)
                known_blocks[block] = {dump.serial, full};
            else known_blocks.erase(block);
        } else known_blocks.erase(block);
        if (buffers.empty()) {
            buffers_min.store(~0ull, std::memory_order_release);
            buffers_max.store(0, std::memory_order_release);
        }
    }
    resolved_serial.store(dump.serial, std::memory_order_release);
}

void Occlusion::Shared::HoldVisible(VAddr block) {
    // The block's result as the game reads it (end - begin): its begin dump must have landed
    // in this use of the block, else the block reads not ready anyway.
    const auto begin = begins.find(block);
    if (begin == begins.end()) {
        return;
    }
    u64 measured = total - begin->second;
    begins.erase(begin);
    if (min_samples && measured > 0 && measured < min_samples) {
        measured = min_samples;
    }
    if (hold_frames) {
        auto& zeros = hold_zeros[block];
        if (measured == 0) {
            if (zeros < hold_frames) {
                ++zeros;
                measured = OcclusionModel::BlockHistory::UnknownPast;
            }
        } else {
            zeros = 0;
        }
        if (hold_zeros.size() > 65536) {
            hold_zeros.clear();
        }
    }
    if (histories.size() > 65536) {
        histories.clear();
    }
    const u64 reported = histories[block].Report(measured);
    if (reported > measured) {
        // Added to the running total: every block keeps reading end - begin.
        total += reported - measured;
        held_visible += measured == 0 ? 1 : 0;
    }
}

bool Occlusion::Shared::WriteBlock(const Pending& dump) {
    const auto own = written.find(dump.va);
    const u64 old_total = own == written.end() ? 0 : own->second;
    if (!runtime_memory_occlusion_write(dump.va, dump.pipes, &dump.stamp,
                                       own != written.end(), old_total, total)) {
        ++foreign;
        return false;
    }
    // Never mutate Rasterizer::pending_writes on this resolver thread. Outstanding
    // destinations are tracked separately and checked before stage-A snapshots.
    ++written_dumps;
    if (written.size() > 262144) {
        written.clear();
    }
    written[dump.va] = total;
    // The end dump of a Gnm block (+8): how the pair reads, for the statistics.
    if ((dump.va & 15) == 8) {
        if (const auto begin = written.find(dump.va - 8); begin != written.end()) {
            ++(total != begin->second ? visible : occluded);
        }
    }
    return true;
}

void Occlusion::Shared::Report() {
    static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
    if (!stats) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < 5.0) {
        return;
    }
    window = now;
    const u64 frames_now = BbStats::gpu_frames.load(std::memory_order_relaxed);
    const u64 draws_now = BbStats::draws.load(std::memory_order_relaxed);
    const std::array<u64, 14> now_counts{
        dumps,
        queries,
        visible,
        occluded,
        poisoned,
        unavailable,
        late + foreign + unbacked,
        pool_full.load(std::memory_order_relaxed),
        pred_sets.load(std::memory_order_relaxed),
        pred_resolved.load(std::memory_order_relaxed),
        pred_skipped.load(std::memory_order_relaxed),
        pred_drawn.load(std::memory_order_relaxed),
        held_visible,
        late_written,
    };
    std::array<double, 14> d{};
    const double frames = std::max<double>(1.0, double(frames_now - window_frames));
    for (size_t i = 0; i < d.size(); ++i) {
        d[i] = double(now_counts[i] - last[i]) / frames;
    }
    const double pairs = d[2] + d[3];
    std::printf("Occlusion per frame: %.1f dumps, %.1f queries, blocks %.1f nonzero / %.1f "
                "zero (%.0f%%), %.1f intervals read visible (%.1f queries unavailable), %.1f "
                "dumps not written (totals: late %llu, reused memory %llu, unmapped %llu), pool "
                "full %.1f; SET_PREDICATION %.1f (%.1f known), predicated draws %.1f skipped / "
                "%.1f drawn; %.0f draws/frame; held visible %.1f, late written %.1f\n",
                d[0], d[1], d[2], d[3], pairs > 0 ? 100.0 * d[3] / pairs : 0.0, d[4], d[5], d[6],
                (unsigned long long)late, (unsigned long long)foreign,
                (unsigned long long)unbacked, d[7], d[8], d[9], d[10], d[11],
                double(draws_now - window_draws) / frames, d[12], d[13]);
    std::fflush(stdout);
    last = now_counts;
    window_frames = frames_now;
    window_draws = draws_now;
}

} // namespace Vulkan
