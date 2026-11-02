#include "tas_hud_dump.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef __linux__
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static FILE* g_file;
static bool g_opened;
static int g_vertexStart;
static int g_indexStart;
static uint64_t g_frame;

static FILE* GetFile()
{
    if (!g_opened)
    {
        g_opened = true;

        const char* path = getenv("UNLEASHED_TAS_HUD_DUMP");
        if (path != nullptr && path[0] != '\0')
        {
#ifdef __linux__
            // A raw openat: libTAS's "Prevent writing to disk" keeps files opened through libc in memory.
            int fd = int(syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
            g_file = fd >= 0 ? fdopen(fd, "w") : nullptr;
#else
            g_file = fopen(path, "w");
#endif
        }
    }

    return g_file;
}

void TasHudDump::Begin()
{
    if (GetFile() == nullptr)
        return;

    auto drawList = ImGui::GetBackgroundDrawList();
    g_vertexStart = drawList->VtxBuffer.Size;
    g_indexStart = drawList->IdxBuffer.Size;
}

void TasHudDump::End()
{
    auto file = GetFile();
    if (file == nullptr)
        return;

    auto drawList = ImGui::GetBackgroundDrawList();

    // FNV-1a over the vertices' bytes (ImDrawVert has no padding: two float pairs and a 32-bit colour).
    uint64_t hash = 0xCBF29CE484222325ull;
    auto bytes = reinterpret_cast<const uint8_t*>(drawList->VtxBuffer.Data + g_vertexStart);
    size_t count = size_t(drawList->VtxBuffer.Size - g_vertexStart) * sizeof(ImDrawVert);
    for (size_t i = 0; i < count; i++)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }

    fprintf(file, "%llu %d %d %016llx\n", (unsigned long long)g_frame++, drawList->VtxBuffer.Size - g_vertexStart,
        drawList->IdxBuffer.Size - g_indexStart, (unsigned long long)hash);
    fflush(file);
}
