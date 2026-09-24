#include "tas_swap_chain.h"

TasSwapChain::TasSwapChain(RenderDevice* device, RenderCommandQueue* queue, RenderWindow renderWindow, SDL_Window* sdlWindow, RenderFormat format)
    : m_device(device), m_queue(queue), m_renderWindow(renderWindow), m_sdlWindow(sdlWindow), m_format(format)
{
    // Draw to the window with plain X11 calls instead of letting SDL create its own GPU renderer.
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");

    m_commandList = m_queue->createCommandList();
    m_fence = m_device->createCommandFence();

    // A software renderer draws to the window surface too, but libTAS can draw its OSD with it.
    m_renderer = SDL_CreateRenderer(m_sdlWindow, -1, SDL_RENDERER_SOFTWARE);
    if (m_renderer == nullptr)
        fprintf(stderr, "TAS mode: couldn't create an SDL software renderer (%s), presenting without libTAS's OSD.\n", SDL_GetError());

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

    if (m_renderer != nullptr)
    {
        // B8G8R8A8 in memory is SDL's RGB888 (XRGB8888) on little-endian hosts. It has no alpha, so the
        // frame's alpha channel isn't blended against what was there before.
        m_frameTexture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, int(m_width), int(m_height));
        if (m_frameTexture == nullptr)
            fprintf(stderr, "TAS mode: couldn't create the frame texture: %s\n", SDL_GetError());
    }

    return true;
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
