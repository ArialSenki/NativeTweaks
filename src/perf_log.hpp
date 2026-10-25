#pragma once

// Optional CPU measurement for stutter reports. With [Diagnostics] PerfLog = 1
// in NativeTweaks.ini, every 30 s the log gets the CPU time used by each
// NativeTweaks worker thread and by the whole game process in that interval,
// then how long each worker iteration, game-thread hook and lock wait took
// (count, total and the longest one) and how many guarded memory reads
// faulted. Off by default; when off, nothing is recorded.

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace perf {

using LogFn = void (*)(const wchar_t* message) noexcept;

// Reads the switch; call before any worker thread starts.
void configure(const std::wstring& config_path, LogFn log) noexcept;

// Records a worker thread (the handle is duplicated; the caller keeps its own).
void thread_started(const wchar_t* name, void* thread) noexcept;

// Starts the reporting thread when enabled.
void start() noexcept;

// Durations of one kind of work, reported and reset every 30 s.
struct Timer {
    const wchar_t* name{};
    std::atomic<std::uint64_t> count{};
    std::atomic<std::uint64_t> total{};  // QueryPerformanceCounter ticks
    std::atomic<std::uint64_t> max{};
};

// Returns a timer with this name (one per name), or nullptr when the perf log
// is off, so the callers below do nothing. Call after configure.
Timer* timer(const wchar_t* name) noexcept;

inline std::uint64_t now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

void record(Timer* timer, std::uint64_t ticks) noexcept;

// Times its own lifetime.
class Scope {
public:
    explicit Scope(Timer* timer) noexcept : timer_(timer), start_(timer ? now() : 0) {}
    ~Scope() {
        if (timer_) record(timer_, now() - start_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Timer* timer_;
    std::uint64_t start_;
};

// Times the iterations of a worker loop: the work between two waits.
class Loop {
public:
    explicit Loop(const wchar_t* name) noexcept : timer_(timer(name)), start_(timer_ ? now() : 0) {}
    void sleep(DWORD ms) noexcept {
        pause();
        Sleep(ms);
        resume();
    }
    // Around any other wait.
    void pause() noexcept {
        if (timer_ && start_) record(timer_, now() - start_);
        start_ = 0;
    }
    void resume() noexcept {
        if (timer_) start_ = now();
    }

private:
    Timer* timer_;
    std::uint64_t start_;
};

// A guarded read or write that faulted (game::safe_read / safe_write).
void count_fault() noexcept;

} // namespace perf
