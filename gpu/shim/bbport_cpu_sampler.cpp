// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_CPU_SAMPLE=1, a sampling CPU profiler of the whole process (Windows).
//
// Every millisecond the sampler thread reads the CPU cycles of each thread of the process
// (QueryThreadCycleTime). A thread that ran since the previous tick is suspended just long
// enough to copy its registers and the top of its stack, then resumed; its call stack is
// unwound afterwards on that copy with the x64 unwind tables (RtlVirtualUnwind), so nothing
// that could wait for a lock the suspended thread holds runs while it is stopped. Stacks are
// counted per thread, weighted by the cycles the thread ran before the sample, and written
// every 10 s to BB_CPU_SAMPLE_FILE (default: cpu-samples.txt in the user folder); stdout gets
// each busy thread's share of a core. Guest (PS4) code has no unwind tables: a stack ends at
// its first guest frame. tools/cpu_samples.py turns the file into a report with the symbol
// table of bb-probe.exe.
// BB_CPU_SAMPLE=3: only while the game shows no 3D scene (loading screens; needs
// BB_FRAME_STATS=1 and DLSS), every thread 100 times a second whether it ran or not, so the
// stacks also show what the waiting threads wait for (sample counts = time).
#include "bbport_cpu_sampler.h"
#include "bbport_toggles.h"

#ifdef _WIN32

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <x86intrin.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace BbCpuSampler {

namespace {

constexpr size_t MaxFrames = 48;
constexpr size_t StackCopyBytes = 96 * 1024;
/// Zeroed space after the copy: an unwind step reading past the copied part reads zeros.
constexpr size_t StackPadding = 64 * 1024;
/// Share of the time since the last tick a thread must have run to be sampled. Suspending a
/// thread runs a kernel APC on it, which counts as running: a fixed small threshold made every
/// thread "busy" again at each tick (2026-10-10 run: all ~60 threads sampled ~415 times a second).
constexpr ULONG64 MinShareDivisor = 20; // 5%

struct StackEntry {
    uint64_t samples = 0;
    uint64_t cycles = 0;
};

struct Thread {
    DWORD id = 0;
    HANDLE handle = nullptr;
    std::string name;
    ULONG64 last_cycles = 0;
    uint64_t total_cycles = 0;  ///< since the sampler started
    uint64_t window_cycles = 0; ///< since the last report
    uint64_t samples = 0;
    std::unordered_map<std::string, StackEntry> stacks; ///< key: the frames' raw bytes
};

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

template <class Function>
Function Kernel32(const char* name) {
    return reinterpret_cast<Function>(
        reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name)));
}

std::string Narrow(const wchar_t* text) {
    if (!text || !text[0]) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size - 1 : 0, '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    for (char& c : out) {
        if (c == ' ' || c == '\t' || c == '\n') c = '_';
    }
    return out;
}

class Sampler {
public:
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        if (const auto set_description = Kernel32<SetThreadDescriptionFn>("SetThreadDescription")) {
            set_description(GetCurrentThread(), L"bb:CpuSampler");
        }
        self = GetCurrentThreadId();
        get_description = Kernel32<GetThreadDescriptionFn>("GetThreadDescription");
        copy.assign(StackCopyBytes + StackPadding, 0);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        const auto start = std::chrono::steady_clock::now();
        auto last_refresh = start - std::chrono::seconds(10);
        auto last_report = start;
        start_tsc = window_tsc = __rdtsc();
        start_time = window_time = start;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_refresh >= std::chrono::seconds(1)) {
                last_refresh = now;
                RefreshThreads();
            }
            Tick();
            if (now - last_report >= std::chrono::seconds(10)) {
                last_report = now;
                Report(now);
            }
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -10000; // 1 ms, relative
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
        }
    }

    std::string path;

private:
    void RefreshThreads() {
        const DWORD pid = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        std::vector<DWORD> alive;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
            alive.push_back(entry.th32ThreadID);
            auto it = std::find_if(threads.begin(), threads.end(), [&](const auto& thread) {
                return thread->id == entry.th32ThreadID && thread->handle;
            });
            if (it == threads.end()) {
                HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                               THREAD_QUERY_INFORMATION,
                                           FALSE, entry.th32ThreadID);
                if (!handle) continue;
                auto thread = std::make_unique<Thread>();
                thread->id = entry.th32ThreadID;
                thread->handle = handle;
                QueryThreadCycleTime(handle, &thread->last_cycles);
                threads.push_back(std::move(thread));
            }
        }
        CloseHandle(snapshot);
        for (auto& thread : threads) {
            if (!thread->handle) continue;
            if (std::find(alive.begin(), alive.end(), thread->id) == alive.end()) {
                CloseHandle(thread->handle); // ended: its counts stay in the report
                thread->handle = nullptr;
                continue;
            }
            if (get_description) {
                PWSTR description = nullptr;
                if (SUCCEEDED(get_description(thread->handle, &description)) && description) {
                    std::string name = Narrow(description);
                    if (!name.empty()) thread->name = std::move(name);
                    LocalFree(description);
                }
            }
        }
    }

    void Tick() {
        const uint64_t now_tsc = __rdtsc();
        const ULONG64 min_cycles = std::max<ULONG64>((now_tsc - tick_tsc) / MinShareDivisor, 4000);
        tick_tsc = now_tsc;
        // BB_CPU_SAMPLE=3: every thread on every 10th tick, during loading screens only.
        const bool sample_all =
            loading_only ? BbStats::loading_screen.load(std::memory_order_relaxed) &&
                               ++loading_ticks % 10 == 0
                         : every_thread;
        for (auto& thread : threads) {
            if (!thread->handle) continue;
            ULONG64 cycles = thread->last_cycles;
            QueryThreadCycleTime(thread->handle, &cycles); // on failure: did not run
            const ULONG64 delta = cycles - thread->last_cycles;
            thread->last_cycles = cycles;
            thread->total_cycles += delta;
            thread->window_cycles += delta;
            if (sample_all || (!loading_only && delta >= min_cycles)) {
                Sample(*thread, delta);
            }
        }
    }

    void Sample(Thread& thread, ULONG64 cycles) {
        if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) return;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        size_t length = 0;
        // GetThreadContext also waits until the thread has actually stopped.
        const bool have_context = GetThreadContext(thread.handle, &context);
        if (have_context) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &info, sizeof(info)) &&
                info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                const uint64_t end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
                length = std::min<uint64_t>(end - context.Rsp, StackCopyBytes) & ~uint64_t{7};
                std::memcpy(copy.data(), reinterpret_cast<const void*>(context.Rsp), length);
            }
        }
        ResumeThread(thread.handle);
        if (!have_context) return;
        std::memset(copy.data() + length, 0, StackPadding);

        uint64_t frames[MaxFrames];
        uint32_t count = 0;
        frames[count++] = context.Rip;
        if (length) {
            Unwind(context, length, frames, count);
        }
        ++thread.samples;
        auto& entry = thread.stacks[std::string(reinterpret_cast<const char*>(frames),
                                                count * sizeof(uint64_t))];
        ++entry.samples;
        entry.cycles += cycles;
    }

    /// Unwinds `context` on the stack copy: pointers into the original stack are moved there.
    void Unwind(CONTEXT& context, size_t length, uint64_t* frames, uint32_t& count) {
        const uint64_t low = context.Rsp, high = context.Rsp + length;
        const uint64_t base = reinterpret_cast<uint64_t>(copy.data());
        const auto rebase = [&](DWORD64& value) {
            if (value >= low && value < high) value = value - low + base;
        };
        for (size_t offset = 0; offset < length; offset += 8) {
            uint64_t value;
            std::memcpy(&value, copy.data() + offset, 8);
            if (value >= low && value < high) {
                value = value - low + base;
                std::memcpy(copy.data() + offset, &value, 8);
            }
        }
        rebase(context.Rsp);
        rebase(context.Rbp);
        rebase(context.Rbx);
        rebase(context.Rsi);
        rebase(context.Rdi);
        rebase(context.R12);
        rebase(context.R13);
        rebase(context.R14);
        rebase(context.R15);
        const uint64_t copy_end = base + length;
        while (count < MaxFrames) {
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
            if (function) {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                                 &handler_data, &establisher, nullptr);
            } else {
                // A leaf function of a module (no unwind entry): the return address is on top.
                // Anything else without an entry is guest code, which cannot be unwound.
                PVOID module = nullptr;
                if (count != 1 || !RtlPcToFileHeader(reinterpret_cast<PVOID>(context.Rip), &module) ||
                    !module || context.Rsp < base || context.Rsp + 8 > copy_end) {
                    break;
                }
                std::memcpy(&context.Rip, reinterpret_cast<const void*>(context.Rsp), 8);
                context.Rsp += 8;
            }
            if (context.Rip == 0 || context.Rsp < base || context.Rsp >= copy_end) break;
            frames[count++] = context.Rip;
        }
    }

    void Report(std::chrono::steady_clock::time_point now) {
        const uint64_t tsc = __rdtsc();
        const double seconds = std::chrono::duration<double>(now - window_time).count();
        const double window_tsc_cycles = double(tsc - window_tsc);
        std::vector<Thread*> busy;
        for (auto& thread : threads) {
            if (thread->window_cycles) busy.push_back(thread.get());
        }
        std::sort(busy.begin(), busy.end(),
                  [](const Thread* a, const Thread* b) { return a->window_cycles > b->window_cycles; });
        std::string line = "CPU sampler: one core = 100%, last " + std::to_string(int(seconds + 0.5)) + " s:";
        double total = 0;
        for (const auto* thread : busy) total += thread->window_cycles / window_tsc_cycles;
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), " process %.0f%%;", total * 100.0);
        line += buffer;
        for (size_t i = 0; i < busy.size() && i < 14; ++i) {
            const double share = busy[i]->window_cycles / window_tsc_cycles * 100.0;
            if (share < 1.0) break;
            std::snprintf(buffer, sizeof(buffer), " %s %.0f%%",
                          busy[i]->name.empty() ? ("tid" + std::to_string(busy[i]->id)).c_str()
                                                : busy[i]->name.c_str(),
                          share);
            line += buffer;
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        for (auto& thread : threads) thread->window_cycles = 0;
        window_tsc = tsc;
        window_time = now;
        Write(now, tsc);
    }

    void Write(std::chrono::steady_clock::time_point now, uint64_t tsc) {
        const std::string temporary = path + ".tmp";
        FILE* file = std::fopen(temporary.c_str(), "wb");
        if (!file) return;
        const double seconds = std::chrono::duration<double>(now - start_time).count();
        std::fprintf(file, "# bbport CPU samples 1\nseconds %.3f\ntsc_per_second %.0f\n", seconds,
                     double(tsc - start_tsc) / std::max(seconds, 1e-3));
        HMODULE modules[1024];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
            const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
            for (DWORD i = 0; i < count; ++i) {
                MODULEINFO info{};
                wchar_t name[MAX_PATH];
                if (!GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info)) ||
                    !GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, MAX_PATH)) {
                    continue;
                }
                std::fprintf(file, "module %llx %lx %s\n",
                             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.lpBaseOfDll)),
                             static_cast<unsigned long>(info.SizeOfImage), Narrow(name).c_str());
            }
        }
        for (const auto& thread : threads) {
            if (!thread->samples) continue;
            std::fprintf(file, "thread %lu %s samples %llu cycles %llu\n",
                         static_cast<unsigned long>(thread->id),
                         thread->name.empty() ? "-" : thread->name.c_str(),
                         static_cast<unsigned long long>(thread->samples),
                         static_cast<unsigned long long>(thread->total_cycles));
            for (const auto& [key, entry] : thread->stacks) {
                std::fprintf(file, "stack %lu %llu %llu", static_cast<unsigned long>(thread->id),
                             static_cast<unsigned long long>(entry.samples),
                             static_cast<unsigned long long>(entry.cycles));
                for (size_t offset = 0; offset < key.size(); offset += 8) {
                    uint64_t frame;
                    std::memcpy(&frame, key.data() + offset, 8);
                    std::fprintf(file, " %llx", static_cast<unsigned long long>(frame));
                }
                std::fputc('\n', file);
            }
        }
        std::fclose(file);
        MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    DWORD self = 0;
    /// BB_CPU_SAMPLE=2: every thread at every tick, ran or not (waits included).
    const bool every_thread = std::getenv("BB_CPU_SAMPLE")[0] == '2';
    const bool loading_only = std::getenv("BB_CPU_SAMPLE")[0] == '3';
    uint64_t loading_ticks = 0;
    GetThreadDescriptionFn get_description = nullptr;
    std::vector<std::unique_ptr<Thread>> threads;
    std::vector<uint8_t> copy;
    uint64_t start_tsc = 0, window_tsc = 0, tick_tsc = __rdtsc();
    std::chrono::steady_clock::time_point start_time, window_time;
};

} // namespace

void Start() {
    const char* enabled = std::getenv("BB_CPU_SAMPLE");
    if (!enabled || (enabled[0] != '1' && enabled[0] != '2' && enabled[0] != '3')) return;
    auto* sampler = new Sampler; // runs until the process ends
    if (const char* file = std::getenv("BB_CPU_SAMPLE_FILE"); file && file[0]) {
        sampler->path = file;
    } else {
        const char* user = std::getenv("BB_GPU_USER_DIR");
        sampler->path = std::string(user && user[0] ? user : ".") + "\\cpu-samples.txt";
    }
    std::printf("CPU sampler: on, %s, stacks every 10 s to %s\n",
                enabled[0] == '3' ? "loading screens only, every thread at 100 Hz" : "1 kHz",
                sampler->path.c_str());
    std::thread([sampler] { sampler->Run(); }).detach();
}

} // namespace BbCpuSampler

#else

namespace BbCpuSampler {
void Start() {}
} // namespace BbCpuSampler

#endif
