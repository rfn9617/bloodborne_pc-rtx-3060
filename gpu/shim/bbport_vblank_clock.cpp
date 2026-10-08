// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_vblank_clock.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>
#endif

namespace BbVblank {

namespace {

std::mutex mutex;
Sample latest{};
bool have_sample = false;
unsigned rejected = 0; ///< deltas in a row that did not match the period
std::atomic<bool> running{false};
std::atomic<bool> stop_requested{false};

void Publish(std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock{mutex};
    if (have_sample) {
        const auto delta = now - latest.time;
        // A missed or doubled wait (window moved, display mode change) is not a period.
        if (latest.period.count() == 0) {
            latest.period = std::chrono::duration_cast<std::chrono::nanoseconds>(delta);
        } else if (delta > latest.period / 2 && delta < latest.period * 3 / 2) {
            // Smoothed: the wake-up after each blank has some scheduling jitter.
            latest.period += (std::chrono::duration_cast<std::chrono::nanoseconds>(delta) -
                              latest.period) /
                             16;
            rejected = 0;
        } else if (++rejected > 32) {
            // Not a missed blank but another period (a first measurement taken across a stall,
            // the display switched to another refresh rate): start over from this one.
            latest.period = std::chrono::duration_cast<std::chrono::nanoseconds>(delta);
            rejected = 0;
        }
    }
    latest.time = now;
    have_sample = true;
}

#if defined(_WIN32)

IDXGIOutput* FindOutput(void* native_window) {
    HMONITOR monitor = native_window
                           ? MonitorFromWindow(static_cast<HWND>(native_window),
                                               MONITOR_DEFAULTTOPRIMARY)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    IDXGIOutput* found = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; !found && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                found = output;
            } else {
                output->Release();
            }
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

void Run(IDXGIOutput* output) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    unsigned failures = 0;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        if (FAILED(output->WaitForVBlank())) {
            // Display off, mode change: try again a little later; stop after a long failure.
            if (++failures > 500) {
                break;
            }
            Sleep(10);
            continue;
        }
        failures = 0;
        Publish(std::chrono::steady_clock::now());
    }
    output->Release();
    running = false;
    std::scoped_lock lock{mutex};
    have_sample = false;
}

#endif

} // namespace

bool Start(void* native_window) {
    if (running) {
        return true;
    }
#if defined(_WIN32)
    IDXGIOutput* output = FindOutput(native_window);
    if (!output) {
        std::printf("VideoOut: display refresh clock unavailable (no DXGI output)\n");
        return false;
    }
    stop_requested = false;
    running = true;
    // Detached: a joinable std::thread left at process exit would terminate the game.
    std::thread(Run, output).detach();
    return true;
#else
    (void)native_window;
    return false;
#endif
}

bool Latest(Sample& out) {
    std::scoped_lock lock{mutex};
    if (!have_sample || latest.period.count() == 0) {
        return false;
    }
    out = latest;
    return true;
}

void Stop() {
    stop_requested = true;
}

} // namespace BbVblank
