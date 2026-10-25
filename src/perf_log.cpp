#include "perf_log.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace perf {
namespace {

constexpr DWORD kIntervalMs = 30000;
constexpr std::size_t kMaxThreads = 32;
constexpr std::size_t kMaxTimers = 48;

struct Entry {
    const wchar_t* name{};
    HANDLE thread{};
    std::uint64_t last{};  // kernel + user time, 100 ns units
};

bool g_enabled{};
LogFn g_log{};
SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Entry, kMaxThreads> g_threads{};
std::size_t g_count{};
std::array<Timer, kMaxTimers> g_timers{};
std::atomic<std::size_t> g_timer_count{};
std::atomic<std::uint64_t> g_faults{};
double g_ticks_per_ms{1.0};

std::uint64_t to_u64(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::uint64_t thread_cpu(HANDLE thread) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    return to_u64(kernel) + to_u64(user);
}

std::uint64_t process_cpu() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return to_u64(kernel) + to_u64(user);
}

// "Perf timings": count / total ms / longest ms of every timer used in the
// interval, in lines short enough for the log.
void report_timers() noexcept {
    wchar_t line[2048]{};
    int used = swprintf_s(line, L"Perf timings (calls/total ms/max ms):");
    bool any{};
    const std::size_t count = g_timer_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count && used > 0; ++i) {
        Timer& timer = g_timers[i];
        const std::uint64_t calls = timer.count.exchange(0, std::memory_order_relaxed);
        const std::uint64_t total = timer.total.exchange(0, std::memory_order_relaxed);
        const std::uint64_t longest = timer.max.exchange(0, std::memory_order_relaxed);
        if (!calls) continue;
        if (used > static_cast<int>(std::size(line)) - 128) {
            if (g_log) g_log(line);
            used = swprintf_s(line, L"Perf timings (cont.):");
        }
        const int added = swprintf_s(line + used, std::size(line) - used, L" %ls %llu/%.1f/%.2f",
            timer.name, static_cast<unsigned long long>(calls), static_cast<double>(total) / g_ticks_per_ms,
            static_cast<double>(longest) / g_ticks_per_ms);
        if (added < 0) break;
        used += added;
        any = true;
    }
    const std::uint64_t faults = g_faults.exchange(0, std::memory_order_relaxed);
    if (used > 0) {
        swprintf_s(line + used, std::size(line) - used, L"%ls; guarded memory faults %llu.",
            any ? L"" : L" none", static_cast<unsigned long long>(faults));
    }
    if (g_log) g_log(line);
}

DWORD WINAPI reporter(void*) noexcept {
    std::uint64_t last_process = process_cpu();
    ULONGLONG last_tick = GetTickCount64();
    for (;;) {
        Sleep(kIntervalMs);
        const std::uint64_t process = process_cpu();
        const ULONGLONG tick = GetTickCount64();
        const double wall_ms = static_cast<double>(tick - last_tick);
        const double process_ms = static_cast<double>(process - last_process) / 10000.0;
        last_process = process;
        last_tick = tick;

        wchar_t line[2048]{};
        int used = swprintf_s(line,
            L"Perf %.0f s: game process CPU %.0f ms (%.1f cores); NativeTweaks threads (ms):",
            wall_ms / 1000.0, process_ms, wall_ms > 0 ? process_ms / wall_ms : 0.0);
        double ours = 0;
        AcquireSRWLockExclusive(&g_lock);
        for (std::size_t i = 0; i < g_count && used > 0; ++i) {
            Entry& entry = g_threads[i];
            const std::uint64_t now = thread_cpu(entry.thread);
            const double ms = static_cast<double>(now - entry.last) / 10000.0;
            entry.last = now;
            ours += ms;
            const int added = swprintf_s(line + used, std::size(line) - used,
                L" %ls %.1f", entry.name, ms);
            if (added < 0) break;
            used += added;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (used > 0) {
            swprintf_s(line + used, std::size(line) - used,
                L"; total %.1f ms (%.3f%% of one core).", ours, wall_ms > 0 ? ours * 100.0 / wall_ms : 0.0);
        }
        if (g_log) g_log(line);
        report_timers();
    }
}

} // namespace

void configure(const std::wstring& config_path, LogFn log) noexcept {
    g_log = log;
    g_enabled = !config_path.empty() &&
        GetPrivateProfileIntW(L"Diagnostics", L"PerfLog", 0, config_path.c_str()) != 0;
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
        g_ticks_per_ms = static_cast<double>(frequency.QuadPart) / 1000.0;
}

void thread_started(const wchar_t* name, void* thread) noexcept {
    if (!g_enabled || !thread) return;
    HANDLE copy{};
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(thread), GetCurrentProcess(),
            &copy, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_count < kMaxThreads) {
        g_threads[g_count++] = Entry{name, copy, thread_cpu(copy)};
        copy = nullptr;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (copy) CloseHandle(copy);
}

void start() noexcept {
    if (!g_enabled) return;
    if (HANDLE thread = CreateThread(nullptr, 0, &reporter, nullptr, 0, nullptr)) CloseHandle(thread);
    if (g_log) g_log(L"Perf log enabled ([Diagnostics] PerfLog = 1): CPU and timing report every 30 s.");
}

Timer* timer(const wchar_t* name) noexcept {
    if (!g_enabled || !name) return nullptr;
    Timer* found{};
    AcquireSRWLockExclusive(&g_lock);
    const std::size_t count = g_timer_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count && !found; ++i)
        if (std::wcscmp(g_timers[i].name, name) == 0) found = &g_timers[i];
    if (!found && count < kMaxTimers) {
        found = &g_timers[count];
        found->name = name;
        g_timer_count.store(count + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

void record(Timer* timer, std::uint64_t ticks) noexcept {
    if (!timer) return;
    timer->count.fetch_add(1, std::memory_order_relaxed);
    timer->total.fetch_add(ticks, std::memory_order_relaxed);
    std::uint64_t longest = timer->max.load(std::memory_order_relaxed);
    while (ticks > longest &&
        !timer->max.compare_exchange_weak(longest, ticks, std::memory_order_relaxed)) {
    }
}

void count_fault() noexcept {
    if (g_enabled) g_faults.fetch_add(1, std::memory_order_relaxed);
}

} // namespace perf
