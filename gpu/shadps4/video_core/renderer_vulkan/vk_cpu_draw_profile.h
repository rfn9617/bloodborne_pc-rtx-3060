// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Vulkan::CpuDrawProfile {

enum class Phase : unsigned {
    Pending, Frame, Targets, Resources, Begin, Inputs, Barriers, Motion, State, Record,
    Buffers, Textures, RingLookup, Camera, Clamp, Obtain, Residency, Upload, Access,
    Stream, Count
};
enum class Kind : unsigned { Gbuffer, Depth, Other };
enum class Counter : unsigned { Bindings, Rings, Arenas, Streams, Uploads, UploadBytes, Count };
using Clock = std::chrono::steady_clock;
constexpr unsigned PhaseCount = unsigned(Phase::Count);
constexpr unsigned CounterCount = unsigned(Counter::Count);

struct Bucket {
    std::uint64_t calls = 0, samples = 0, total_ns = 0;
    std::array<std::uint64_t, PhaseCount> phase_ns{};
    std::array<std::uint64_t, CounterCount> counters{};
};
struct Window {
    Clock::time_point start = Clock::now();
    std::array<Bucket, 3> buckets{};
    std::uint64_t sequence = 0;
};

class Sample;
inline thread_local Sample* active_sample = nullptr;
inline thread_local Window window;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_CPU_DRAW_PROFILE");
        return value && value[0] == '1';
    }();
    return enabled;
}

// Samples 1/257 direct draws. Times are CPU wall time (including waits/preemption),
// not GPU timestamps or CPU utilization. Nested buffer/texture scopes are inclusive
// parts of Resources; helper-thread work does not write into this thread's sample.
class Sample {
public:
    Sample(bool stage_b, Kind kind) : stage_b{stage_b}, kind{kind} {
        if (!Enabled() || active_sample) return;
        ++window.buckets[unsigned(kind)].calls;
        if (++window.sequence % 257 != 1) return;
        active_sample = this;
        start = mark = Clock::now();
        selected = true;
    }
    ~Sample() {
        if (!selected) return;
        const auto now = Clock::now();
        phase_ns[unsigned(phase)] += Nanoseconds(now - mark);
        auto& bucket = window.buckets[unsigned(kind)];
        ++bucket.samples;
        bucket.total_ns += Nanoseconds(now - start);
        for (unsigned i = 0; i < PhaseCount; ++i) bucket.phase_ns[i] += phase_ns[i];
        for (unsigned i = 0; i < CounterCount; ++i) bucket.counters[i] += counters[i];
        active_sample = nullptr;
        if (now - window.start < std::chrono::seconds(5)) return;
        static constexpr const char* kinds[] = {"G-buffer", "depth", "other"};
        static constexpr const char* phases[] = {"pending", "frame", "targets", "resources",
            "begin", "inputs", "barriers", "motion", "state", "record", "buffers", "textures",
            "ring-lookup", "camera", "clamp", "obtain", "resident", "upload", "access",
            "stream"};
        static constexpr const char* counts[] = {"bindings", "rings", "arenas", "streams",
                                                 "uploads", "upload-bytes"};
        for (unsigned k = 0; k < window.buckets.size(); ++k) {
            const auto& value = window.buckets[k];
            if (!value.samples) continue;
            const double divisor = 1000.0 * value.samples;
            std::array<char, 2048> line{};
            unsigned used = std::snprintf(line.data(), line.size(),
                        "CPU direct-draw sample: %s / %s, %llu samples of %llu calls; "
                        "mean %.3f us/draw;", stage_b ? "stage B" : "serial", kinds[k],
                        (unsigned long long)value.samples, (unsigned long long)value.calls,
                        value.total_ns / divisor);
            for (unsigned p = 0; p < PhaseCount; ++p) {
                used += std::snprintf(line.data() + used, line.size() - used,
                                     " %s %.3f us", phases[p], value.phase_ns[p] / divisor);
            }
            for (unsigned c = 0; c < CounterCount; ++c) {
                used += std::snprintf(line.data() + used, line.size() - used,
                                     "; %s %.2f/draw", counts[c],
                                     double(value.counters[c]) / value.samples);
            }
            std::printf("%s (CPU wall time; nested timers overlap; obtain/resident/upload/stream "
                        "also cover inputs)\n", line.data());
        }
        window.buckets = {};
        window.start = now;
    }
    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;

    void Mark(Phase next) {
        if (!selected) return;
        const auto now = Clock::now();
        phase_ns[unsigned(phase)] += Nanoseconds(now - mark);
        phase = next;
        mark = now;
    }
    void Add(Phase nested, Clock::duration elapsed) {
        phase_ns[unsigned(nested)] += Nanoseconds(elapsed);
    }
    void Count(Counter counter, std::uint64_t value) {
        counters[unsigned(counter)] += value;
    }

private:
    static std::uint64_t Nanoseconds(Clock::duration elapsed) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    }
    bool selected = false, stage_b;
    Kind kind;
    Phase phase = Phase::Pending;
    Clock::time_point start{}, mark{};
    std::array<std::uint64_t, PhaseCount> phase_ns{};
    std::array<std::uint64_t, CounterCount> counters{};
};

inline void Count(Counter counter, std::uint64_t value = 1) {
    if (active_sample) active_sample->Count(counter, value);
}

class Section {
public:
    explicit Section(Phase phase) : sample{active_sample}, phase{phase} {
        if (sample) start = Clock::now();
    }
    ~Section() {
        if (sample) sample->Add(phase, Clock::now() - start);
    }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
private:
    Sample* sample;
    Phase phase;
    Clock::time_point start{};
};

template <Phase phase, typename Function>
decltype(auto) Measure(Function&& function) {
    Section section(phase);
    return function();
}

struct PacketWindow {
    Clock::time_point start = Clock::now();
    std::array<Bucket, 6> buckets{};
};
inline thread_local PacketWindow packet_window;

// Includes state-copy tasks, compute, clears and indirect commands, so work outside
// DrawRecord is visible too. Each kind is independently sampled once per 257 calls.
class PacketSample {
public:
    explicit PacketSample(unsigned kind) : kind{kind} {
        if (!Enabled() || kind >= packet_window.buckets.size()) return;
        auto& bucket = packet_window.buckets[kind];
        if (++bucket.calls % 257 != 1) return;
        selected = true;
        start = Clock::now();
    }
    ~PacketSample() {
        if (!selected) return;
        const auto now = Clock::now();
        auto& bucket = packet_window.buckets[kind];
        ++bucket.samples;
        bucket.total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count();
        if (now-packet_window.start < std::chrono::seconds(5)) return;
        static constexpr const char* names[] = {"draw", "task", "dispatch", "special",
                                               "indirect", "indirect-dispatch"};
        for (unsigned i = 0; i < packet_window.buckets.size(); ++i) {
            const auto& value = packet_window.buckets[i];
            if (!value.samples) continue;
            std::printf("CPU packet sample: %s, %llu samples of %llu calls; mean %.3f us/packet "
                        "(CPU wall time; direct draw samples included)\n", names[i],
                        (unsigned long long)value.samples, (unsigned long long)value.calls,
                        value.total_ns/(1000.0*value.samples));
        }
        packet_window.buckets = {};
        packet_window.start = now;
    }
    PacketSample(const PacketSample&) = delete;
    PacketSample& operator=(const PacketSample&) = delete;
private:
    bool selected = false;
    unsigned kind;
    Clock::time_point start{};
};

} // namespace Vulkan::CpuDrawProfile
