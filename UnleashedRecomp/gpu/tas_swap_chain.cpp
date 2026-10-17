#include "tas_swap_chain.h"

#include <atomic>

#ifdef __linux__
#include <SDL_syswm.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static std::atomic<uint32_t> g_uploadErrors;
static std::atomic<uint32_t> g_presentCount;
static std::atomic<bool> g_rebuildingSurface;

#ifdef __linux__
// The frame upload to the X server can fail with BadGC (seen once under VcXsrv, in Empire City Act 2 Day: "X Error
// of failed request: BadGC, Major opcode 130 (MIT-SHM), Minor opcode 3 (X_ShmPutImage)"), which Xlib's default handler
// answers by exiting. Why the GC SDL made for the window was gone isn't known yet. Those errors are logged and ignored
// instead (a frame isn't shown), and if uploads keep failing, the window surface is made again (see present()).
// Other errors go to the previous handler.
static constexpr int X_REQUEST_PUT_IMAGE = 72;
static constexpr int X_REQUEST_FREE_GC = 60;
static constexpr int X_SHM_DETACH = 2;
static constexpr int X_SHM_PUT_IMAGE = 3;
static constexpr int SHM_BAD_SEG = 0;

static XErrorHandler g_previousErrorHandler;
static int g_shmMajorOpcode = -1;
static int g_shmFirstError = -1;
// The GC of the last failed upload, whose XFreeGC fails the same way when the surface is made again.
static std::atomic<XID> g_staleGC;

static int TasXErrorHandler(Display* display, XErrorEvent* event)
{
    bool shmRequest = g_shmMajorOpcode >= 0 && event->request_code == g_shmMajorOpcode;
    bool badShmSeg = g_shmFirstError >= 0 && event->error_code == g_shmFirstError + SHM_BAD_SEG;
    bool badGC = event->error_code == BadGC;

    bool upload = (shmRequest && event->minor_code == X_SHM_PUT_IMAGE) || event->request_code == X_REQUEST_PUT_IMAGE;
    bool uploadError = upload && (badGC || badShmSeg);
    bool cleanupError = (event->request_code == X_REQUEST_FREE_GC && badGC && event->resourceid == g_staleGC) ||
        (g_rebuildingSurface && shmRequest && event->minor_code == X_SHM_DETACH && (badShmSeg || event->error_code == BadValue));

    if (!uploadError && !cleanupError)
        return g_previousErrorHandler != nullptr ? g_previousErrorHandler(display, event) : 0;

    if (uploadError)
    {
        g_uploadErrors++;
        if (badGC)
            g_staleGC = event->resourceid;
    }

    // Only the first ones: a stale surface fails on every frame until it's made again.
    static std::atomic<uint32_t> s_logged;
    if (s_logged++ < 50)
    {
        char text[128] = {};
        XGetErrorText(display, event->error_code, text, sizeof(text));
        fprintf(stderr, "TAS mode: ignored X error %s (%d) for request %d.%d, resource 0x%lx, serial %lu, present %u, thread %ld%s\n",
            text, int(event->error_code), int(event->request_code), int(event->minor_code), event->resourceid, event->serial,
            g_presentCount.load(), long(syscall(SYS_gettid)), cleanupError ? " (freeing the old window surface)" : "");
    }

    return 0;
}

static void InstallXErrorHandler(Display* display)
{
    static bool s_installed;
    if (s_installed || display == nullptr)
        return;

    int firstEvent;
    if (!XQueryExtension(display, "MIT-SHM", &g_shmMajorOpcode, &firstEvent, &g_shmFirstError))
    {
        g_shmMajorOpcode = -1;
        g_shmFirstError = -1;
    }

    g_previousErrorHandler = XSetErrorHandler(TasXErrorHandler);
    s_installed = true;
}
#endif

TasSwapChain::TasSwapChain(RenderDevice* device, RenderCommandQueue* queue, RenderWindow renderWindow, SDL_Window* sdlWindow, RenderFormat format)
    : m_device(device), m_queue(queue), m_renderWindow(renderWindow), m_sdlWindow(sdlWindow), m_format(format)
{
    // Draw to the window with plain X11 calls instead of letting SDL create its own GPU renderer.
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");

#ifdef __linux__
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_sdlWindow, &info) && info.subsystem == SDL_SYSWM_X11)
        InstallXErrorHandler(info.info.x11.display);
#endif

    m_commandList = m_queue->createCommandList();
    m_fence = m_device->createCommandFence();

    CreateRenderer();
    resize();

    // libTAS picks how to capture the screen on its first frame boundary, which it can also insert
    // on its own while the game is loading. The Vulkan device already exists at this point, so unless
    // it has seen a renderer or window surface present by then, it picks Vulkan capture and later
    // crashes looking for swap chain images that don't exist. Present a black frame right away.
    if (m_renderer != nullptr)
    {
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
        SDL_RenderClear(m_renderer);
        SDL_RenderPresent(m_renderer);
    }
    else if (SDL_Surface* surface = SDL_GetWindowSurface(m_sdlWindow))
    {
        SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, 0, 0, 0));
        SDL_UpdateWindowSurface(m_sdlWindow);
    }
}

TasSwapChain::~TasSwapChain()
{
    if (m_frameTexture != nullptr)
        SDL_DestroyTexture(m_frameTexture);

    if (m_renderer != nullptr)
        SDL_DestroyRenderer(m_renderer);
}

void TasSwapChain::CreateRenderer()
{
    // A software renderer draws to the window surface too, but libTAS can draw its OSD with it.
    m_renderer = SDL_CreateRenderer(m_sdlWindow, -1, SDL_RENDERER_SOFTWARE);
    if (m_renderer == nullptr)
        fprintf(stderr, "TAS mode: couldn't create an SDL software renderer (%s), presenting without libTAS's OSD.\n", SDL_GetError());
}

// Makes SDL create the window surface again, with a new GC and shared memory segment: the renderer goes first (it keeps
// the surface), then the surface (SDL_GetWindowSurface only makes a new one when the window is resized otherwise).
// libTAS lets go of its screen capture with the renderer, and sets it up again on the next frame boundary.
void TasSwapChain::RebuildWindowSurface()
{
    fprintf(stderr, "TAS mode: the frame upload failed on %u presents in a row, making the window surface again.\n", m_failedUploads);

    if (m_frameTexture != nullptr)
    {
        SDL_DestroyTexture(m_frameTexture);
        m_frameTexture = nullptr;
    }

    if (m_renderer != nullptr)
    {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }

    g_rebuildingSurface = true;
    SDL_DestroyWindowSurface(m_sdlWindow);
    g_rebuildingSurface = false;

    // The frames' textures stay: the game's renderer uses them.
    CreateRenderer();
    CreateFrameTexture();
    m_failedUploads = 0;
}

void TasSwapChain::GetWindowPixelSize(uint32_t& width, uint32_t& height) const
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_sdlWindow, &w, &h);
    width = uint32_t(std::max(w, 0));
    height = uint32_t(std::max(h, 0));
}

bool TasSwapChain::present(uint32_t textureIndex, RenderCommandSemaphore** waitSemaphores, uint32_t waitSemaphoreCount)
{
    if (isEmpty() || textureIndex >= TextureCount)
        return false;

    RenderTexture* texture = m_textures[textureIndex].get();

    // The frame was submitted to the same queue before this, so the barrier orders the copy after it.
    m_commandList->begin();
    m_commandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
    m_commandList->copyTextureRegion(
        RenderTextureCopyLocation::PlacedFootprint(m_readbackBuffer.get(), m_format, m_width, m_height, 1, m_width),
        RenderTextureCopyLocation::Subresource(texture));
    m_commandList->end();

    m_queue->executeCommandLists(m_commandList.get(), m_fence.get());
    m_queue->waitForCommandFence(m_fence.get());

    // SDL syncs with the X server after the upload, so its errors arrive before SDL returns.
    g_presentCount++;
    uint32_t uploadErrors = g_uploadErrors;
    bool presented = presentFrame();
    if (g_uploadErrors != uploadErrors)
    {
        if (++m_failedUploads >= 2)
            RebuildWindowSurface();
    }
    else
    {
        m_failedUploads = 0;
    }

    return presented;
}

bool TasSwapChain::presentFrame()
{
    if (m_renderer != nullptr)
    {
        if (m_frameTexture != nullptr)
        {
            // Scaled to the window, which may have been resized since the frame was rendered.
            SDL_UpdateTexture(m_frameTexture, nullptr, m_readbackBuffer->map(), int(m_width * 4));
            m_readbackBuffer->unmap();
            SDL_RenderCopy(m_renderer, m_frameTexture, nullptr, nullptr);
        }

        // libTAS treats this call as the frame boundary, and draws its OSD before presenting.
        SDL_RenderPresent(m_renderer);
        return true;
    }

    SDL_Surface* surface = SDL_GetWindowSurface(m_sdlWindow);
    if (surface != nullptr)
    {
        const void* pixels = m_readbackBuffer->map();

        // Copy what fits; the window may have been resized since the frame was rendered.
        int width = std::min(int(m_width), surface->w);
        int height = std::min(int(m_height), surface->h);

        if (width > 0 && height > 0)
        {
            SDL_LockSurface(surface);

            // B8G8R8A8 in memory is SDL's ARGB8888 on little-endian hosts.
            SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_ARGB8888, pixels, int(m_width * 4),
                surface->format->format, surface->pixels, surface->pitch);

            SDL_UnlockSurface(surface);
        }

        m_readbackBuffer->unmap();

        // libTAS treats this call as the frame boundary.
        SDL_UpdateWindowSurface(m_sdlWindow);
    }

    return true;
}

void TasSwapChain::wait()
{
    // present() is synchronous, so there is never anything to wait for.
}

bool TasSwapChain::resize()
{
    GetWindowPixelSize(m_width, m_height);

    for (auto& texture : m_textures)
        texture.reset();

    m_readbackBuffer.reset();

    if (m_frameTexture != nullptr)
    {
        SDL_DestroyTexture(m_frameTexture);
        m_frameTexture = nullptr;
    }

    if (isEmpty())
        return false;

    for (auto& texture : m_textures)
        texture = m_device->createTexture(RenderTextureDesc::Texture2D(m_width, m_height, 1, m_format, RenderTextureFlag::RENDER_TARGET));

    m_readbackBuffer = m_device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(m_width) * m_height * 4));

    CreateFrameTexture();
    return true;
}

void TasSwapChain::CreateFrameTexture()
{
    if (m_renderer == nullptr || isEmpty())
        return;

    // B8G8R8A8 in memory is SDL's RGB888 (XRGB8888) on little-endian hosts. It has no alpha, so the
    // frame's alpha channel isn't blended against what was there before.
    m_frameTexture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, int(m_width), int(m_height));
    if (m_frameTexture == nullptr)
        fprintf(stderr, "TAS mode: couldn't create the frame texture: %s\n", SDL_GetError());
}

bool TasSwapChain::needsResize() const
{
    uint32_t width, height;
    GetWindowPixelSize(width, height);
    return isEmpty() || width != m_width || height != m_height;
}

void TasSwapChain::setVsyncEnabled(bool vsyncEnabled)
{
    // libTAS controls the frame rate.
}

bool TasSwapChain::isVsyncEnabled() const
{
    return false;
}

uint32_t TasSwapChain::getWidth() const
{
    return m_width;
}

uint32_t TasSwapChain::getHeight() const
{
    return m_height;
}

RenderTexture* TasSwapChain::getTexture(uint32_t textureIndex)
{
    return textureIndex < TextureCount ? m_textures[textureIndex].get() : nullptr;
}

uint32_t TasSwapChain::getTextureCount() const
{
    return TextureCount;
}

bool TasSwapChain::acquireTexture(RenderCommandSemaphore* signalSemaphore, uint32_t* textureIndex)
{
    // No semaphore is signalled; in TAS mode the frame is submitted without waiting on one.
    if (isEmpty())
        return false;

    *textureIndex = m_nextTextureIndex;
    m_nextTextureIndex = (m_nextTextureIndex + 1) % TextureCount;
    return true;
}

RenderWindow TasSwapChain::getWindow() const
{
    return m_renderWindow;
}

bool TasSwapChain::isEmpty() const
{
    return m_width == 0 || m_height == 0;
}

uint32_t TasSwapChain::getRefreshRate() const
{
    return 60;
}
