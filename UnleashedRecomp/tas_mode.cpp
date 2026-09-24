#include "tas_mode.h"

#include <chrono>

static std::atomic<uint64_t> g_timebaseReads;

static uint64_t TasReadTimebase()
{
    g_timebaseReads.fetch_add(1, std::memory_order_relaxed);

    // The Xbox 360 timebase runs at 49.875 MHz (what KeQueryPerformanceFrequency reports),
    // so convert nanoseconds with 49875000 / 1000000000 = 399 / 8000, without overflowing.
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return (ns / 8000) * 399 + (ns % 8000) * 399 / 8000;
}

void InstallTasTimebase()
{
    g_ppcTimebaseOverride = TasReadTimebase;
}

#ifdef __linux__

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

// libTAS creates the Vulkan device in "native" mode, and treats every thread created in native mode
// as a native thread: it never suspends, saves or restores it. Lavapipe (the software Vulkan driver)
// creates its queue and rendering threads inside vkCreateDevice, so they all end up untracked, and
// loading a savestate rewinds their memory while they keep running, which deadlocks the next frame.
//
// To avoid that, in TAS mode this executable exports its own pthread_create, which takes precedence
// over libTAS's and glibc's. Threads requested by the Mesa driver are handed to a spawner thread that
// was started normally, so libTAS registers them as ordinary game threads. Everything else goes
// straight through to libTAS (or glibc when not running under libTAS).

using PthreadCreateFunc = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

static PthreadCreateFunc GetRealPthreadCreate()
{
    static const PthreadCreateFunc s_pthreadCreate = []() -> PthreadCreateFunc
    {
        // Under libTAS, look the function up in libtas.so itself. dlsym(RTLD_NEXT, ...) can't be used for
        // that: libTAS hooks dlsym and resolves RTLD_NEXT relative to itself, which would skip its hook.
        const char* libtasPath = std::getenv("LIBTAS_LIBRARY_PATH");
        if (libtasPath != nullptr && libtasPath[0] != '\0')
        {
            if (void* libtas = dlopen(libtasPath, RTLD_NOW | RTLD_NOLOAD))
            {
                if (void* function = dlsym(libtas, "pthread_create"))
                    return reinterpret_cast<PthreadCreateFunc>(function);
            }
        }

        void* function = dlvsym(RTLD_NEXT, "pthread_create", "GLIBC_2.34");
        if (function == nullptr)
            function = dlvsym(RTLD_NEXT, "pthread_create", "GLIBC_2.2.5");

        return reinterpret_cast<PthreadCreateFunc>(function);
    }();

    return s_pthreadCreate;
}

static bool IsCalledFromMesa(void* returnAddress)
{
    Dl_info info{};
    if (!dladdr(returnAddress, &info) || info.dli_fname == nullptr)
        return false;

    return strstr(info.dli_fname, "libvulkan_lvp") != nullptr || strstr(info.dli_fname, "libgallium") != nullptr;
}

struct SpawnRequest
{
    pthread_t* thread;
    const pthread_attr_t* attr;
    void* (*start)(void*);
    void* arg;
    int result;
    bool done;
};

static std::atomic<bool> g_spawnerRunning;
static std::mutex g_spawnCallMutex;
static std::mutex g_spawnMutex;
static std::condition_variable g_spawnCondition;
static SpawnRequest* g_spawnRequest;

static void* SpawnerThread(void*)
{
    std::unique_lock lock(g_spawnMutex);

    while (true)
    {
        g_spawnCondition.wait(lock, []() { return g_spawnRequest != nullptr && !g_spawnRequest->done; });

        SpawnRequest* request = g_spawnRequest;
        request->result = GetRealPthreadCreate()(request->thread, request->attr, request->start, request->arg);
        request->done = true;

        g_spawnCondition.notify_all();
    }

    return nullptr;
}

void StartTasThreadSpawner()
{
    pthread_t thread;
    if (GetRealPthreadCreate()(&thread, nullptr, SpawnerThread, nullptr) == 0)
    {
        pthread_detach(thread);
        g_spawnerRunning = true;
    }
    else
    {
        fprintf(stderr, "TAS mode: failed to start the thread spawner, driver threads will be untracked by libTAS.\n");
    }
}

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) __THROWNL
{
    if (g_spawnerRunning.load(std::memory_order_acquire) && IsCalledFromMesa(__builtin_return_address(0)))
    {
        std::lock_guard callLock(g_spawnCallMutex);

        SpawnRequest request{ thread, attr, start, arg, 0, false };

        std::unique_lock lock(g_spawnMutex);
        g_spawnRequest = &request;
        g_spawnCondition.notify_all();
        g_spawnCondition.wait(lock, [&]() { return request.done; });
        g_spawnRequest = nullptr;

        static std::atomic<int> s_count;
        fprintf(stderr, "TAS mode: driver thread %d created through the spawner (result %d)\n", ++s_count, request.result);

        return request.result;
    }

    return GetRealPthreadCreate()(thread, attr, start, arg);
}

// The game loads data, mixes audio and runs jobs on background threads that run at the speed of the
// host CPU and race each other, while libTAS advances frames on its own schedule. Which thread gets a
// lock first, or how much a background thread gets done before a frame ends, then varies between runs,
// which desyncs movies.
//
// The scheduler removes that. Only one guest thread runs at a time: the one holding the run token. The
// token is only handed on when its holder blocks, sleeps or exits, and it goes to the thread that has
// been runnable the longest. Threads become runnable at points that only depend on what the guest does
// (being created, or another thread changing what they wait for), so the order is the same every run.
//
// Every guest-level wait goes through the scheduler. Whoever changes state tries every waiter's
// tryAcquire() under the scheduler lock, in the order they started waiting, and queues the ones that
// succeed. Before presenting, the presenting thread gives up the token until every other guest thread is
// blocked, so each frame contains the same work. Threads that sleep or yield, and the audio thread, run
// again at the next frame, or earlier if every thread is blocked (so a thread waiting on them can't
// deadlock). Savestates are always taken while every other guest thread is blocked in the scheduler.

#include <cpu/ppc_context.h>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <thread>
#include <vector>

namespace TasScheduler
{
    struct TrackedThread
    {
        pthread_t thread{};
        pid_t tid{};
        bool blocked = false;
        // Signalled when this thread is given the run token.
        std::condition_variable tokenCondition;
    };

    struct Waiter
    {
        const std::function<bool()>* tryAcquire;
        TrackedThread* tracked;
        bool done;
        // Only used by threads the scheduler doesn't track; tracked ones wait for the token instead.
        std::condition_variable condition;
        // For timed sleeps: the virtual time (in microseconds) to wake up at, or 0.
        uint64_t deadline = 0;
    };

    static std::vector<TrackedThread*> g_trackedThreads;
    static thread_local TrackedThread* g_self;
    static thread_local uint32_t g_pollsSinceBlocked;
    // Scheduler operations (locks, events, ...) the thread did since it last blocked. A thread that does
    // this many without ever blocking is probably waiting in a loop for another thread, which can't run
    // while it holds the token, so it's made to sleep briefly. Counting operations rather than time keeps
    // the point where that happens the same on every run.
    static thread_local uint32_t g_operationsSinceBlocked;
    static constexpr uint32_t OperationsBeforePreempting = 10000;
    static int g_stallFile = -1;

    static std::mutex g_mutex;
    static std::condition_variable g_presenterCondition; // Wakes a thread in WaitForOtherThreads().
    static std::vector<Waiter*> g_waiters;

    // The thread allowed to run guest code, and the ones waiting for it in order.
    static TrackedThread* g_tokenHolder;
    static std::deque<TrackedThread*> g_runnable;
    // Set if a thread held the token too long: threads then run in parallel as a fallback.
    static bool g_serialDisabled;
    // Counts token hand-offs, so the watchdog can tell when the token stopped moving.
    static uint64_t g_tokenPasses;
    // Threads in WaitForHost(). While there are any, frames don't end and blocked threads aren't released,
    // as how often that would happen would depend on how long the host takes.
    static int g_hostWaiters;

    static bool g_presenterWaiting;
    // Threads between WaitForOtherThreads() and AdvanceFrame(). Nothing else is released while one is
    // presenting, as libTAS does its frame processing (audio mixing, savestates) in the middle of that.
    static int g_presenting;
    // Incremented every frame: wakes threads waiting for the next frame (vsync waits, the loading loop).
    static uint64_t g_sleepGeneration;

    // Timed sleeps run on a virtual clock, in microseconds, so a thread sleeping 1 ms in a loop gets as
    // many iterations per frame as it would on the real hardware, however long frames take to render. It
    // only moves at deterministic points: stepping through a frame when it ends, or jumping to the next
    // wake-up when every thread is blocked.
    static constexpr uint64_t FrameLength = 1000000 / 60;
    static uint64_t g_virtualTime = 1;
    static uint64_t g_frameStart = 1;

    // Reported by TasTrace: jumps of the virtual clock made because every thread was blocked.
    static uint64_t g_statJumps;
    static uint64_t g_statJumpedTime;
    static uint64_t g_framesAdvanced;
    static int g_longFrameDumps;
    static void DumpAllThreads(const char* reason);

    // Totals reported by TasTrace.
    static uint64_t g_statBlocked;
    static uint64_t g_statSleeps;
    static uint64_t g_statGaveUp;

    // Virtual time the guest started at, and audio callbacks run since.
    static uint64_t g_audioStart;
    static uint64_t g_audioCallbacksDone;

    // Real time and sleeps that bypass libTAS, which would otherwise count them as game time.
    static uint64_t RealTimeMilliseconds()
    {
        timespec ts{};
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    static void RealSleepMilliseconds(uint32_t milliseconds)
    {
        timespec ts{ milliseconds / 1000, long(milliseconds % 1000) * 1000000 };
        syscall(SYS_nanosleep, &ts, nullptr);
    }

    static bool IsIdle()
    {
        if (g_serialDisabled)
        {
            // Threads don't take turns anymore, so check that each one is actually blocked.
            return std::all_of(g_trackedThreads.begin(), g_trackedThreads.end(), [](TrackedThread* thread) { return thread->blocked; });
        }

        return g_tokenHolder == nullptr && g_runnable.empty();
    }

    // Lock held.
    static void PassToken()
    {
        if (g_serialDisabled || g_tokenHolder != nullptr || g_runnable.empty())
            return;

        g_tokenHolder = g_runnable.front();
        g_runnable.pop_front();
        g_tokenPasses++;
        g_tokenHolder->tokenCondition.notify_one();
    }

    // Lock held. Blocks until the calling thread holds the token.
    static void WaitForToken(std::unique_lock<std::mutex>& lock)
    {
        if (g_serialDisabled)
            return;

        g_self->tokenCondition.wait(lock, []() { return g_tokenHolder == g_self || g_serialDisabled; });
    }

    // Lock held.
    static void ReleaseToken()
    {
        if (g_tokenHolder == g_self)
        {
            g_tokenHolder = nullptr;
            PassToken();
        }
    }

    static bool WakeReadyWaiters()
    {
        bool woke = false;

        for (auto it = g_waiters.begin(); it != g_waiters.end();)
        {
            Waiter* waiter = *it;

            if ((*waiter->tryAcquire)())
            {
                waiter->done = true;

                if (waiter->tracked != nullptr)
                {
                    waiter->tracked->blocked = false;

                    // Without turns, wake it directly rather than queueing it for the token.
                    if (g_serialDisabled)
                        waiter->tracked->tokenCondition.notify_one();
                    else
                        g_runnable.push_back(waiter->tracked);
                }
                else
                {
                    waiter->condition.notify_one();
                }

                it = g_waiters.erase(it);
                woke = true;
            }
            else
            {
                ++it;
            }
        }

        return woke;
    }

    // The earliest virtual time a timed sleep wakes up at, if any.
    static uint64_t NextDeadline()
    {
        uint64_t next = UINT64_MAX;

        for (Waiter* waiter : g_waiters)
        {
            if (waiter->deadline != 0)
                next = std::min(next, waiter->deadline);
        }

        return next;
    }

    // Must be called with the lock held after anything that can unblock a waiter or stop a thread.
    static void Update()
    {
        WakeReadyWaiters();

        // Every guest thread is blocked, and none is presenting: nothing will happen until time passes.
        // Jump to the next timed wake-up. Without one, let threads waiting for the next frame and the audio
        // thread run once more, as one of them may be what the others are waiting for.
        if (IsIdle() && g_presenting == 0 && g_hostWaiters == 0 && !g_waiters.empty())
        {
            uint64_t next = NextDeadline();

            if (next != UINT64_MAX)
            {
                if (next > g_virtualTime)
                {
                    g_statJumps++;
                    g_statJumpedTime += next - g_virtualTime;
                }

                g_virtualTime = std::max(g_virtualTime, next);

                // UNLEASHED_TAS_DUMP_LONG_FRAMES=<frame>: once past that many frames, dump every thread the
                // first few times a frame takes more than three frames of virtual time, to see what waits.
                static const uint64_t s_dumpAfter = []()
                {
                    const char* value = std::getenv("UNLEASHED_TAS_DUMP_LONG_FRAMES");
                    return value != nullptr ? std::strtoull(value, nullptr, 10) : UINT64_MAX;
                }();

                if (g_framesAdvanced >= s_dumpAfter && g_virtualTime > g_frameStart + 3 * FrameLength && g_longFrameDumps < 3)
                {
                    g_longFrameDumps++;
                    DumpAllThreads("frame took more than three frames of virtual time");
                }
            }
            else
            {
                g_sleepGeneration++;
            }

            WakeReadyWaiters();
        }

        PassToken();

        if (g_presenterWaiting)
            g_presenterCondition.notify_one();
    }

    static void WaitLocked(std::unique_lock<std::mutex>& lock, const std::function<bool()>& tryAcquire, uint64_t deadline = 0)
    {
        if (tryAcquire())
            return;

        Waiter waiter{ &tryAcquire, g_self, false };
        waiter.deadline = deadline;
        g_waiters.push_back(&waiter);
        g_statBlocked++;
        g_pollsSinceBlocked = 0;
        g_operationsSinceBlocked = 0;

        if (g_self != nullptr)
        {
            g_self->blocked = true;
            ReleaseToken();
        }

        Update();

        if (g_self != nullptr)
        {
            // Woken threads are queued for the token by whoever woke them.
            g_self->tokenCondition.wait(lock, [&]() { return waiter.done && (g_tokenHolder == g_self || g_serialDisabled); });
        }
        else
        {
            waiter.condition.wait(lock, [&]() { return waiter.done; });
        }
    }

    static thread_local uint32_t g_waitingLock;
    static thread_local uint32_t g_waitingLockOwner;

    void NoteWaitingOnLock(uint32_t lockAddress, uint32_t owner)
    {
        g_waitingLock = lockAddress;
        g_waitingLockOwner = owner;
    }

    // Writes the calling thread's stack to the stall report. Runs in a signal handler.
    static void StallBacktraceHandler(int)
    {
        // r13 is the guest's thread pointer, which is also what critical sections record as their owner.
        char header[160];
        int length = snprintf(header, sizeof(header), "--- thread %ld --- r13=0x%08X waiting_on_lock=0x%08X lock_owner=0x%08X\n",
            long(syscall(SYS_gettid)), g_ppcContext != nullptr ? g_ppcContext->r13.u32 : 0u, g_waitingLock, g_waitingLockOwner);
        syscall(SYS_write, g_stallFile, header, size_t(length));

        void* frames[48];
        int frameCount = backtrace(frames, int(std::size(frames)));
        backtrace_symbols_fd(frames, frameCount, g_stallFile);
    }

    static void FallBackToParallel(const char* reason);

    void InitMainThread()
    {
        std::lock_guard lock(g_mutex);

        g_self = new TrackedThread();
        g_self->thread = pthread_self();
        g_self->tid = pid_t(syscall(SYS_gettid));
        g_trackedThreads.push_back(g_self);

        // The main thread starts out holding the token.
        g_tokenHolder = g_self;
        g_audioStart = g_virtualTime;

        // libTAS calls exit() itself, e.g. when it loses its connection to the game. That runs static
        // destructors while guest threads are still running or blocked here, and destroying the audio
        // thread's std::thread aborts. Registered after the statics, this runs first and ends the process
        // right away instead, like App::Exit().
        std::atexit([]()
        {
            std::fflush(nullptr);
            std::_Exit(0);
        });

        // Where to report threads that keep a frame from ending, written with raw system calls so libTAS
        // doesn't intercept it. backtrace() is called once here, as its first call isn't signal-safe.
        if (const char* home = std::getenv("HOME"))
        {
            std::string path = fmt::format("{}/tas_stall.txt", home);
            g_stallFile = int(syscall(SYS_openat, AT_FDCWD, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644));
        }

        void* frames[1];
        backtrace(frames, 1);

        struct sigaction action{};
        action.sa_handler = StallBacktraceHandler;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(SIGURG, &action, nullptr);

        // Wakes a presenter that has been waiting for a long time, so it can give up (see below), and
        // notices when a thread keeps the token without blocking while others wait for it (a spin loop).
        std::thread([]()
        {
            uint64_t lastPasses = 0;
            uint64_t lastChange = RealTimeMilliseconds();

            while (true)
            {
                RealSleepMilliseconds(500);
                g_presenterCondition.notify_all();

                std::lock_guard lock(g_mutex);

                // While a frame is being presented, the holder may be inside libTAS's frame boundary for as
                // long as libTAS stays paused, so only count time outside of that.
                if (g_serialDisabled || g_presenting > 0 || g_hostWaiters > 0 || g_tokenPasses != lastPasses || g_tokenHolder == nullptr || g_runnable.empty())
                {
                    lastPasses = g_tokenPasses;
                    lastChange = RealTimeMilliseconds();
                }
                else if (RealTimeMilliseconds() - lastChange > 60000)
                {
                    FallBackToParallel("a guest thread kept the run token for 60 seconds without blocking");
                }
            }
        }).detach();
    }

    void* OnThreadCreated()
    {
        std::lock_guard lock(g_mutex);

        auto* thread = new TrackedThread();
        g_trackedThreads.push_back(thread);

        if (!g_serialDisabled)
            g_runnable.push_back(thread);

        return thread;
    }

    void OnThreadStarted(void* thread)
    {
        std::unique_lock lock(g_mutex);

        g_self = static_cast<TrackedThread*>(thread);
        g_self->thread = pthread_self();
        g_self->tid = pid_t(syscall(SYS_gettid));

        PassToken();
        WaitForToken(lock);
    }

    void OnThreadExited(const std::function<void()>& markExited)
    {
        std::lock_guard lock(g_mutex);

        markExited();

        if (g_self != nullptr)
        {
            ReleaseToken();
            std::erase(g_trackedThreads, g_self);
            delete g_self;
            g_self = nullptr;
        }

        Update();
    }

    // Called without the lock, before every operation of a thread that may hold the token. After enough
    // operations without blocking, hands the token to the next runnable thread, if there is one. This
    // mustn't sleep: with nothing else to run, that would make the virtual clock jump ahead of the frames
    // (the game's memory allocator alone takes its lock hundreds of thousands of times per frame).
    static void CountOperation()
    {
        if (g_self == nullptr || g_serialDisabled || ++g_operationsSinceBlocked < OperationsBeforePreempting)
            return;

        g_operationsSinceBlocked = 0;

        std::unique_lock lock(g_mutex);

        if (g_tokenHolder == g_self && !g_runnable.empty())
        {
            g_runnable.push_back(g_self);
            g_tokenHolder = nullptr;
            PassToken();
            WaitForToken(lock);
        }
    }

    void Wait(const std::function<bool()>& tryAcquire)
    {
        CountOperation();

        std::unique_lock lock(g_mutex);
        WaitLocked(lock, tryAcquire);
    }

    bool Run(const std::function<bool()>& tryAcquire)
    {
        CountOperation();

        std::lock_guard lock(g_mutex);
        return tryAcquire();
    }

    void Modify(const std::function<void()>& change)
    {
        CountOperation();

        std::lock_guard lock(g_mutex);
        change();
        Update();
    }

    void Sleep()
    {
        std::unique_lock lock(g_mutex);

        g_statSleeps++;

        // Until the next frame is presented, or until the virtual clock reaches the next frame boundary
        // (which it can do without a frame being presented, when every thread is blocked).
        uint64_t generation = g_sleepGeneration;
        uint64_t deadline = g_frameStart + FrameLength;
        while (deadline <= g_virtualTime)
            deadline += FrameLength;

        WaitLocked(lock, [generation, deadline]() { return g_sleepGeneration != generation || g_virtualTime >= deadline; }, deadline);
    }

    void SleepFor(uint32_t milliseconds)
    {
        std::unique_lock lock(g_mutex);

        g_statSleeps++;

        if (milliseconds == 0)
        {
            // A yield: let everything else run, and wake at the next scheduled event (a timed sleep ending,
            // an audio callback, the next frame), without adding time of its own. A thread polling in a loop
            // then takes as much virtual time as what it waits for, as on the real hardware, rather than
            // pushing the clock (and audio) ahead of the frames.
            uint64_t time = g_virtualTime;
            uint64_t generation = g_sleepGeneration;
            WaitLocked(lock, [time, generation]() { return g_virtualTime != time || g_sleepGeneration != generation; });
            return;
        }

        uint64_t deadline = g_virtualTime + uint64_t(milliseconds) * 1000;
        WaitLocked(lock, [deadline]() { return g_virtualTime >= deadline; }, deadline);
    }

    void YieldIfPolling()
    {
        if (g_self == nullptr)
            return;

        if (g_pollsSinceBlocked++ != 0)
            SleepFor(0);
    }

    void WaitForAudioCallback()
    {
        std::unique_lock lock(g_mutex);

        // Audio runs on the virtual clock like any timed sleep: one callback per 256 samples at 48 kHz,
        // i.e. every 16000 / 3 microseconds. It runs two frames ahead of it, so libTAS, which takes 800
        // samples each frame, always has enough queued: produced just in time, a frame that only got three
        // callbacks (768 samples) would be short and play a gap.
        constexpr uint64_t Headroom = 2 * FrameLength;
        uint64_t deadline = g_audioStart + (g_audioCallbacksDone + 1) * 16000 / 3;
        deadline = deadline > g_audioStart + Headroom ? deadline - Headroom : g_audioStart;
        WaitLocked(lock, [deadline]() { return g_virtualTime >= deadline; }, deadline);

        g_audioCallbacksDone++;
    }

    void WaitForHost(const std::function<bool()>& ready)
    {
        {
            std::lock_guard lock(g_mutex);

            g_hostWaiters++;

            if (g_self != nullptr)
                ReleaseToken();

            Update();
        }

        while (!ready())
            RealSleepMilliseconds(1);

        std::unique_lock lock(g_mutex);

        g_hostWaiters--;

        if (g_self != nullptr && !g_serialDisabled && g_tokenHolder != g_self)
        {
            g_runnable.push_back(g_self);
            PassToken();
            WaitForToken(lock);
        }

        Update();
    }

    static void ReportStall()
    {
        if (g_stallFile < 0)
            return;

        std::string header = fmt::format("=== stall: token held by thread {}, {} runnable, {} presenting, reported by thread {} ===\n",
            g_tokenHolder != nullptr ? long(g_tokenHolder->tid) : -1L, g_runnable.size(), g_presenting, long(syscall(SYS_gettid)));
        syscall(SYS_write, g_stallFile, header.data(), header.size());

        for (TrackedThread* thread : g_trackedThreads)
        {
            if (thread != g_self && !thread->blocked && thread->tid != 0)
                pthread_kill(thread->thread, SIGURG);
        }
    }

    // Lock held. Writes every tracked thread's stack to the stall report, one after the other.
    static void DumpAllThreads(const char* reason)
    {
        if (g_stallFile < 0)
            return;

        std::string header = fmt::format("=== {} (virtual time {} us, frame started at {} us) ===\n", reason, g_virtualTime, g_frameStart);
        syscall(SYS_write, g_stallFile, header.data(), header.size());

        for (TrackedThread* thread : g_trackedThreads)
        {
            if (thread->tid != 0 && thread != g_self)
            {
                pthread_kill(thread->thread, SIGURG);
                RealSleepMilliseconds(20);
            }
        }
    }

    // Lock held. Lets every thread run in parallel from now on, rather than deadlock.
    static void FallBackToParallel(const char* reason)
    {
        fprintf(stderr, "TAS mode: %s. Threads now run in parallel; playback won't be deterministic. See ~/tas_stall.txt.\n", reason);

        g_statGaveUp++;
        ReportStall();

        g_serialDisabled = true;
        g_tokenHolder = nullptr;
        g_runnable.clear();

        for (TrackedThread* thread : g_trackedThreads)
            thread->tokenCondition.notify_all();

        g_presenterCondition.notify_all();
    }

    void WaitForOtherThreads()
    {
        std::unique_lock lock(g_mutex);

        g_presenting++;
        g_presenterWaiting = true;

        // A new frame: the first poll in it doesn't need to yield.
        g_pollsSinceBlocked = 0;

        // Let every other runnable thread finish its work first.
        if (g_self != nullptr)
            ReleaseToken();

        const uint64_t start = RealTimeMilliseconds();

        auto waitUntilIdle = [&]()
        {
            while ((!IsIdle() || g_hostWaiters > 0) && !g_serialDisabled)
            {
                // A thread that spins without ever blocking would keep the token forever. Give up rather than
                // deadlock, and let threads run in parallel from then on; frames aren't deterministic after that.
                if (RealTimeMilliseconds() - start > 60000)
                {
                    FallBackToParallel("the other guest threads didn't all block within 60 seconds of a frame ending");
                    break;
                }

                g_presenterCondition.wait(lock);
            }
        };

        waitUntilIdle();

        // Step the virtual clock through the rest of the frame, waking timed sleeps in order and letting
        // everything run until blocked again after each one.
        const uint64_t frameEnd = g_frameStart + FrameLength;

        while (!g_serialDisabled)
        {
            uint64_t next = NextDeadline();
            if (next > frameEnd)
                break;

            g_virtualTime = std::max(g_virtualTime, next);
            WakeReadyWaiters();
            PassToken();

            waitUntilIdle();
        }

        g_virtualTime = std::max(g_virtualTime, frameEnd);
        g_presenterWaiting = false;

        if (g_self != nullptr && !g_serialDisabled)
            g_tokenHolder = g_self;
    }

    void AdvanceFrame()
    {
        std::lock_guard lock(g_mutex);

        if (g_presenting > 0)
            g_presenting--;

        g_sleepGeneration++;
        g_frameStart = g_virtualTime;
        g_framesAdvanced++;

        Update();
    }
}

#include <fcntl.h>
#include <kernel/heap.h>
#include <kernel/memory.h>

namespace TasTrace
{
    static int g_file = -1;
    static std::mutex g_writeMutex;
    static uint64_t g_frame;
    static std::atomic<uint64_t> g_audioDropped;
    static std::atomic<uint32_t> g_audioQueued;

    bool IsEnabled()
    {
        static const bool s_enabled = []()
        {
            const char* path = std::getenv("UNLEASHED_TAS_TRACE");
            if (!IsTasMode() || path == nullptr || path[0] == '\0')
                return false;

            // libTAS restarts the game for every playback, so give each run its own file.
            std::string fullPath = fmt::format("{}.{}", path, getpid());

            // Raw system calls: libTAS intercepts file writes from the game (to prevent it from saving),
            // and the trace must reach the disk.
            g_file = int(syscall(SYS_openat, AT_FDCWD, fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
            if (g_file < 0)
                fprintf(stderr, "TAS mode: couldn't open the trace file %s.\n", fullPath.c_str());
            else
                fprintf(stderr, "TAS mode: writing the trace to %s.\n", fullPath.c_str());

            return g_file >= 0;
        }();

        return s_enabled;
    }

    static void Write(const char* data, size_t size)
    {
        std::lock_guard lock(g_writeMutex);

        while (size != 0)
        {
            ssize_t written = syscall(SYS_write, g_file, data, size);
            if (written <= 0)
                return;

            data += written;
            size -= size_t(written);
        }
    }

    static size_t HeapAllocated(Mutex& mutex, O1HeapInstance* heap)
    {
        std::lock_guard lock(mutex);
        return o1heapGetDiagnostics(heap).allocated;
    }

    void OnFrame()
    {
        if (!IsEnabled())
            return;

        uint64_t blocked, sleeps, gaveUp, audio, virtualTime, jumps, jumpedTime;
        {
            std::lock_guard lock(TasScheduler::g_mutex);
            blocked = TasScheduler::g_statBlocked;
            sleeps = TasScheduler::g_statSleeps;
            gaveUp = TasScheduler::g_statGaveUp;
            audio = TasScheduler::g_audioCallbacksDone;
            virtualTime = TasScheduler::g_virtualTime;
            jumps = TasScheduler::g_statJumps;
            jumpedTime = TasScheduler::g_statJumpedTime;
        }

        uint64_t time = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

        std::string line = fmt::format("F {} time={} virtual_time={} jumps={} jumped_time={} timebase_reads={} blocked={} sleeps={} audio={} audio_dropped={} audio_queued={} gave_up={} heap={} physical_heap={} image=",
            g_frame++, time, virtualTime, jumps, jumpedTime, g_timebaseReads.load(), blocked, sleeps, audio, g_audioDropped.load(), g_audioQueued.load(), gaveUp,
            HeapAllocated(g_userHeap.mutex, g_userHeap.heap), HeapAllocated(g_userHeap.physicalMutex, g_userHeap.physicalHeap));

        // The game image holds the guest's global variables. Hash it in 1 MB chunks so a difference
        // can be narrowed down to a region.
        constexpr size_t ChunkSize = 0x100000;
        for (size_t offset = 0; offset < PPC_IMAGE_SIZE; offset += ChunkSize)
        {
            size_t size = std::min<size_t>(ChunkSize, PPC_IMAGE_SIZE - offset);
            uint32_t hash = uint32_t(XXH3_64bits(g_memory.base + PPC_IMAGE_BASE + offset, size));
            line += fmt::format("{:08x}{}", hash, offset + ChunkSize < PPC_IMAGE_SIZE ? "," : "");
        }

        // UNLEASHED_TAS_TRACE_CHUNK=<index> also hashes that chunk in 4 KB pages, to narrow a difference down.
        static const int s_detailChunk = []()
        {
            const char* value = std::getenv("UNLEASHED_TAS_TRACE_CHUNK");
            return value != nullptr ? std::atoi(value) : -1;
        }();

        if (s_detailChunk >= 0 && size_t(s_detailChunk) * ChunkSize < PPC_IMAGE_SIZE)
        {
            constexpr size_t PageSize = 0x1000;
            size_t chunkStart = size_t(s_detailChunk) * ChunkSize;
            size_t chunkEnd = std::min<size_t>(chunkStart + ChunkSize, PPC_IMAGE_SIZE);

            line += " pages=";
            for (size_t offset = chunkStart; offset < chunkEnd; offset += PageSize)
            {
                uint32_t hash = uint32_t(XXH3_64bits(g_memory.base + PPC_IMAGE_BASE + offset, PageSize));
                line += fmt::format("{:08x}{}", hash, offset + PageSize < chunkEnd ? "," : "");
            }
        }

        line += '\n';

        Write(line.data(), line.size());
    }

    void OnAudioSubmit(bool dropped, uint32_t queuedSamples)
    {
        if (dropped)
            g_audioDropped++;

        g_audioQueued = queuedSamples;
    }

    void OnFileRead(const char* fileName, uint64_t offset, uint32_t size)
    {
        if (!IsEnabled())
            return;

        std::string line = fmt::format("R {} {} offset={} size={}\n", g_frame, fileName, offset, size);
        Write(line.data(), line.size());
    }
}

#else

void StartTasThreadSpawner()
{
}

namespace TasScheduler
{
    void InitMainThread() {}
    void* OnThreadCreated() { return nullptr; }
    void OnThreadStarted(void* thread) {}
    void OnThreadExited(const std::function<void()>& markExited) { markExited(); }
    void Wait(const std::function<bool()>& tryAcquire) { while (!tryAcquire()) {} }
    bool Run(const std::function<bool()>& tryAcquire) { return tryAcquire(); }
    void Modify(const std::function<void()>& change) { change(); }
    void Sleep() {}
    void NoteWaitingOnLock(uint32_t lockAddress, uint32_t owner) {}
    void SleepFor(uint32_t milliseconds) {}
    void YieldIfPolling() {}
    void WaitForAudioCallback() {}
    void WaitForHost(const std::function<bool()>& ready) { while (!ready()) {} }
    void WaitForOtherThreads() {}
    void AdvanceFrame() {}
}

namespace TasTrace
{
    bool IsEnabled() { return false; }
    void OnFrame() {}
    void OnFileRead(const char* fileName, uint64_t offset, uint32_t size) {}
    void OnAudioSubmit(bool dropped, uint32_t queuedSamples) {}
}

#endif
