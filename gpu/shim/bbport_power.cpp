// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see bbport_power.h.
#include "bbport_power.h"
#include "bbport_toggles.h"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void runtime_file_stats(uint64_t out[5]);

namespace BbPower {

namespace {

// NVML (nvml.h), only what is used here.
using NvmlReturn = int;
using NvmlDevice = void*;
struct NvmlUtilization {
    unsigned int gpu, memory;
};
using InitFn = NvmlReturn (*)();
using HandleFn = NvmlReturn (*)(unsigned int, NvmlDevice*);
using UintFn = NvmlReturn (*)(NvmlDevice, unsigned int*);
using ClockFn = NvmlReturn (*)(NvmlDevice, int, unsigned int*);
using TemperatureFn = NvmlReturn (*)(NvmlDevice, int, unsigned int*);
using UtilizationFn = NvmlReturn (*)(NvmlDevice, NvmlUtilization*);
using ReasonsFn = NvmlReturn (*)(NvmlDevice, unsigned long long*);
constexpr int ClockGraphics = 0, ClockMemory = 2, TemperatureGpu = 0;

template <class Function>
Function Load(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

uint64_t FileTime(const FILETIME& time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct Window {
    double power_mw = 0, graphics_mhz = 0, memory_mhz = 0, load = 0;
    unsigned samples = 0, power_samples = 0, max_temperature = 0, min_graphics_mhz = ~0u;
    unsigned long long reasons = 0;
};

class Monitor {
public:
    Monitor() {
        const char* stats = std::getenv("BB_FRAME_STATS");
        if (!stats || stats[0] != '1') {
            return;
        }
        enabled = true;
        HMODULE nvml = LoadLibraryW(L"nvml.dll");
        if (!nvml) {
            nvml = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
        if (nvml) {
            const auto init = Load<InitFn>(nvml, "nvmlInit_v2");
            const auto handle = Load<HandleFn>(nvml, "nvmlDeviceGetHandleByIndex_v2");
            power = Load<UintFn>(nvml, "nvmlDeviceGetPowerUsage");
            power_limit = Load<UintFn>(nvml, "nvmlDeviceGetEnforcedPowerLimit");
            clock = Load<ClockFn>(nvml, "nvmlDeviceGetClockInfo");
            temperature = Load<TemperatureFn>(nvml, "nvmlDeviceGetTemperature");
            utilization = Load<UtilizationFn>(nvml, "nvmlDeviceGetUtilizationRates");
            reasons = Load<ReasonsFn>(nvml, "nvmlDeviceGetCurrentClocksEventReasons");
            if (!reasons) {
                reasons = Load<ReasonsFn>(nvml, "nvmlDeviceGetCurrentClocksThrottleReasons");
            }
            if (init && handle && init() == 0 && handle(0, &device) == 0) {
                gpu = true;
            }
        }
        std::printf("Power: statistics on (%s)\n",
                    gpu ? "process and machine CPU time, NVIDIA GPU through NVML"
                        : "process and machine CPU time; NVML not available");
        if (gpu) {
            sampler = std::thread([this] { Sample(); });
            sampler.detach();
        }
        Reset();
    }

    void Print(double seconds, unsigned frames) {
        if (!enabled) {
            return;
        }
        FILETIME creation, exit, kernel, user, idle, system_kernel, system_user;
        uint64_t process = 0;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
            process = FileTime(kernel) + FileTime(user);
        }
        uint64_t machine_busy = 0, machine_total = 0;
        if (GetSystemTimes(&idle, &system_kernel, &system_user)) {
            // Kernel time includes idle time.
            machine_total = FileTime(system_kernel) + FileTime(system_user);
            machine_busy = machine_total - FileTime(idle);
        }
        const double process_cores = (process - last_process) / (seconds * 1e7);
        const double machine_load = machine_total > last_machine_total
                                        ? 100.0 * double(machine_busy - last_machine_busy) /
                                              double(machine_total - last_machine_total)
                                        : 0.0;
        last_process = process;
        last_machine_busy = machine_busy;
        last_machine_total = machine_total;

        std::string line;
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "Power: process CPU %.2f cores, machine CPU %.0f%% busy", process_cores,
                      machine_load);
        line = buffer;
        if (gpu) {
            Window window;
            {
                std::scoped_lock lock{mutex};
                window = current;
                current = {};
            }
            if (window.samples) {
                const double n = window.samples;
                std::snprintf(buffer, sizeof(buffer),
                              "; GPU load %.0f%%, graphics clock %.0f MHz (min %u), memory %.0f MHz, "
                              "max %u C",
                              window.load / n, window.graphics_mhz / n, window.min_graphics_mhz,
                              window.memory_mhz / n, window.max_temperature);
                line += buffer;
                if (window.power_samples) {
                    unsigned limit = 0;
                    if (!power_limit || power_limit(device, &limit) != 0) {
                        limit = 0;
                    }
                    std::snprintf(buffer, sizeof(buffer), ", power %.1f W (limit %.0f W)",
                                  window.power_mw / window.power_samples / 1000.0, limit / 1000.0);
                    line += buffer;
                }
                // Reasons other than idle: what held the clock below its maximum.
                struct {
                    unsigned long long bit;
                    const char* name;
                } constexpr names[] = {{0x4, "power cap"},        {0x8, "hardware slowdown"},
                                       {0x20, "thermal (software)"}, {0x40, "thermal (hardware)"},
                                       {0x80, "power brake"},      {0x2, "application clocks"},
                                       {0x10, "sync boost"},       {0x100, "display clock"}};
                std::string limits;
                for (const auto& [bit, name] : names) {
                    if (window.reasons & bit) {
                        limits += limits.empty() ? "" : ", ";
                        limits += name;
                    }
                }
                line += "; clock limited by: " + (limits.empty() ? std::string{"nothing"} : limits);
            }
        }
        // Guest file reads (runtime_file.c): loading screens and streaming.
        uint64_t files[5] = {};
        runtime_file_stats(files);
        if (files[1] != last_files[1]) {
            std::snprintf(buffer, sizeof(buffer),
                          "; files: %llu opened, %llu reads, %.1f MB, %.0f ms reading, %.0f ms "
                          "touching pages",
                          static_cast<unsigned long long>(files[0] - last_files[0]),
                          static_cast<unsigned long long>(files[1] - last_files[1]),
                          (files[2] - last_files[2]) / 1e6, (files[3] - last_files[3]) / 1e6,
                          (files[4] - last_files[4]) / 1e6);
            line += buffer;
        }
        std::copy(std::begin(files), std::end(files), std::begin(last_files));
        const uint64_t calls = BbStats::barrier_calls.load(std::memory_order_relaxed);
        const uint64_t images = BbStats::barrier_images.load(std::memory_order_relaxed);
        const uint64_t passes = BbStats::render_passes.load(std::memory_order_relaxed);
        const uint64_t skipped = BbStats::read_after_read_skipped.load(std::memory_order_relaxed);
        if (frames) {
            std::snprintf(buffer, sizeof(buffer),
                          "; barriers %.0f/frame (%.0f image barriers, %.1f read-after-read "
                          "left out); render passes %.0f/frame",
                          double(calls - last_barriers) / frames,
                          double(images - last_image_barriers) / frames,
                          double(skipped - last_skipped) / frames,
                          double(passes - last_passes) / frames);
            line += buffer;
        }
        last_barriers = calls;
        last_image_barriers = images;
        last_passes = passes;
        last_skipped = skipped;
        const double since_start =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("Power: t=%.0f s; %s\n", since_start, line.c_str() + 7);
        PrintBarrierKinds(frames);
    }

private:
    /// The most frequent image barriers since the last window: old -> new layout.
    void PrintBarrierKinds(unsigned frames) {
        struct Kind {
            uint64_t count;
            unsigned from, to, after_write;
        };
        std::vector<Kind> kinds;
        uint64_t total = 0;
        for (unsigned from = 0; from < BbStats::NumLayoutKinds; ++from) {
            for (unsigned to = 0; to < BbStats::NumLayoutKinds; ++to) {
                for (unsigned write = 0; write < 2; ++write) {
                    const uint64_t now = BbStats::image_barrier_kinds[from][to][write].load(
                        std::memory_order_relaxed);
                    uint64_t& last = last_kinds[from][to][write];
                    if (now != last) {
                        kinds.push_back({now - last, from, to, write});
                        total += now - last;
                    }
                    last = now;
                }
            }
        }
        if (!frames || kinds.empty()) {
            return;
        }
        std::sort(kinds.begin(), kinds.end(),
                  [](const Kind& a, const Kind& b) { return a.count > b.count; });
        std::string line = "Image barriers by layout (per frame):";
        char buffer[160];
        for (size_t i = 0; i < kinds.size() && i < 10; ++i) {
            const Kind& kind = kinds[i];
            std::snprintf(buffer, sizeof(buffer), "%s %s -> %s%s %.1f", i ? ";" : "",
                          BbStats::LayoutNames[kind.from], BbStats::LayoutNames[kind.to],
                          kind.after_write ? " after write" : "", double(kind.count) / frames);
            line += buffer;
        }
        std::snprintf(buffer, sizeof(buffer), "; all %.1f", double(total) / frames);
        line += buffer;
        std::printf("%s\n", line.c_str());
    }

    void Reset() {
        FILETIME creation, exit, kernel, user, idle, system_kernel, system_user;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
            last_process = FileTime(kernel) + FileTime(user);
        }
        if (GetSystemTimes(&idle, &system_kernel, &system_user)) {
            last_machine_total = FileTime(system_kernel) + FileTime(system_user);
            last_machine_busy = last_machine_total - FileTime(idle);
        }
    }

    void Sample() {
        using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (const auto set_description = Load<SetDescriptionFn>(GetModuleHandleW(L"kernel32.dll"),
                                                                "SetThreadDescription")) {
            set_description(GetCurrentThread(), L"bb:PowerStats");
        }
        while (true) {
            Window sample;
            unsigned value = 0;
            if (clock && clock(device, ClockGraphics, &value) == 0) {
                sample.graphics_mhz = value;
                sample.min_graphics_mhz = value;
            }
            if (clock && clock(device, ClockMemory, &value) == 0) {
                sample.memory_mhz = value;
            }
            if (temperature && temperature(device, TemperatureGpu, &value) == 0) {
                sample.max_temperature = value;
            }
            NvmlUtilization load{};
            if (utilization && utilization(device, &load) == 0) {
                sample.load = load.gpu;
            }
            unsigned long long limited = 0;
            if (reasons && reasons(device, &limited) == 0) {
                sample.reasons = limited & ~1ull; // not "idle"
            }
            const bool have_power = power && power(device, &value) == 0;
            {
                std::scoped_lock lock{mutex};
                current.graphics_mhz += sample.graphics_mhz;
                current.memory_mhz += sample.memory_mhz;
                current.load += sample.load;
                current.max_temperature = std::max(current.max_temperature, sample.max_temperature);
                current.min_graphics_mhz =
                    std::min(current.min_graphics_mhz, unsigned(sample.graphics_mhz));
                current.reasons |= sample.reasons;
                ++current.samples;
                if (have_power) {
                    current.power_mw += value;
                    ++current.power_samples;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    bool enabled = false, gpu = false;
    NvmlDevice device = nullptr;
    UintFn power = nullptr, power_limit = nullptr;
    ClockFn clock = nullptr;
    TemperatureFn temperature = nullptr;
    UtilizationFn utilization = nullptr;
    ReasonsFn reasons = nullptr;
    std::thread sampler;
    std::mutex mutex;
    Window current;
    uint64_t last_process = 0, last_machine_busy = 0, last_machine_total = 0;
    uint64_t last_files[5] = {}, last_barriers = 0, last_image_barriers = 0, last_passes = 0,
             last_skipped = 0;
    uint64_t last_kinds[BbStats::NumLayoutKinds][BbStats::NumLayoutKinds][2] = {};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

Monitor& Get() {
    static Monitor* monitor = new Monitor; // lives until the process ends
    return *monitor;
}

} // namespace

void Start() {
    Get();
}

void PrintWindow(double seconds, unsigned frames) {
    Get().Print(seconds, frames);
}

double ProcessCpuSeconds() {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    return double(FileTime(kernel) + FileTime(user)) / 1e7;
}

} // namespace BbPower

#else

#include <sys/resource.h>

namespace BbPower {
void Start() {}
void PrintWindow(double, unsigned) {}
double ProcessCpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
} // namespace BbPower

#endif
