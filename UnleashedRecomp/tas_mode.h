#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

// TAS mode is enabled by setting UNLEASHED_TAS_MODE=1 in the environment.
// It adjusts behaviour that breaks under libTAS (frozen fake time, savestates),
// while leaving the normal game untouched.
inline bool IsTasMode()
{
#ifdef __linux__
    static const bool s_tasMode = []()
    {
        const char* value = std::getenv("UNLEASHED_TAS_MODE");
        return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
    }();

    return s_tasMode;
#else
    return false;
#endif
}

// Makes the guest timebase (mftb) follow std::chrono::steady_clock, which libTAS controls,
// instead of the CPU's time stamp counter. Must be called before guest code runs.
void InstallTasTimebase();

// Starts the thread that creates the Vulkan driver's threads on its behalf (see tas_mode.cpp).
// Must be called before the Vulkan device is created.
void StartTasThreadSpawner();

// Makes the game's threads deterministic (see tas_mode.cpp): only one guest thread runs at a time,
// in a fixed order. Only used in TAS mode; callers check IsTasMode() first.
namespace TasScheduler
{
    // Registers the calling thread, which runs the guest's main function, before guest code starts.
    void InitMainThread();

    // Called by the creator of a thread that will run guest code, before that thread can run. Queues the
    // thread to run at this point, so the order doesn't depend on when the OS starts it. The result must
    // be passed to OnThreadStarted() by the new thread.
    void* OnThreadCreated();
    // Called by that thread once it runs, before running any guest code. Waits for its turn.
    void OnThreadStarted(void* thread);
    // Called by a thread that ran guest code when it's done. Runs markExited under the scheduler lock.
    void OnThreadExited(const std::function<void()>& markExited);

    // Blocks until tryAcquire() succeeds. tryAcquire() is always called under the scheduler lock.
    void Wait(const std::function<bool()>& tryAcquire);
    // A single attempt, under the scheduler lock.
    bool Run(const std::function<bool()>& tryAcquire);
    // Changes state that waiting threads may be blocked on, and wakes the ones that can proceed.
    void Modify(const std::function<void()>& change);

    // Blocks until the next frame (vsync waits and the like), or until every other thread is blocked.
    void Sleep();
    // Guest sleeps and yields: blocks for that long on the scheduler's virtual clock (see tas_mode.cpp).
    void SleepFor(uint32_t milliseconds);
    // For functions the game polls in loops without blocking: sleeps if the calling thread already
    // called one since it last blocked, so the threads it's waiting for get to run.
    void YieldIfPolling();
    // For the audio thread: blocks until another audio callback is due.
    void WaitForAudioCallback();

    // Diagnostics: records the critical section the calling thread is about to wait for (0 when done),
    // so stall reports can say who holds it.
    void NoteWaitingOnLock(uint32_t lockAddress, uint32_t owner);

    // For host code waiting on host threads (e.g. pipeline compilation): gives up the run token until
    // ready() is true, polling it. Frames don't end meanwhile, so how long the host takes doesn't matter.
    void WaitForHost(const std::function<bool()>& ready);

    // Before presenting: waits until every other guest thread is blocked.
    void WaitForOtherThreads();
    // After presenting (the libTAS frame boundary): starts the next frame.
    void AdvanceFrame();
}

// A per-frame record of the game's state, for finding what differs between two playbacks of the
// same movie. Written in TAS mode when UNLEASHED_TAS_TRACE is set to a file path.
namespace TasTrace
{
    bool IsEnabled();

    // At the start of Video::Present, while every other guest thread is blocked.
    void OnFrame();
    void OnFileRead(const char* fileName, uint64_t offset, uint32_t size);
    // From the audio thread: whether a callback's audio was queued or dropped, and how much was queued.
    void OnAudioSubmit(bool dropped, uint32_t queuedSamples);
}
