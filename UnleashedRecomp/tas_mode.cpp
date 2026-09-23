#include "tas_mode.h"

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

#else

void StartTasThreadSpawner()
{
}

#endif
