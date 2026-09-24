#pragma once

#include <algorithm>
#include <memory>

#include <SDL.h>
#include <plume_render_interface.h>

using namespace plume;

// A swap chain used in TAS mode (UNLEASHED_TAS_MODE=1) in place of the Vulkan one.
//
// Frames are rendered into ordinary textures, read back to CPU memory and shown with
// SDL's software renderer (or its window surface, if that can't be created). This keeps
// every piece of presentation state inside the process (no WSI threads, present fences or
// compositor round trips), which is what libTAS needs to be able to save and load states,
// and SDL_RenderPresent is where libTAS draws its OSD. Presenting is synchronous, and
// semaphores are not used: the frame's command list must be submitted to the same queue
// before present().
struct TasSwapChain : RenderSwapChain
{
    static constexpr uint32_t TextureCount = 2;

    RenderDevice* m_device;
    RenderCommandQueue* m_queue;
    RenderWindow m_renderWindow;
    SDL_Window* m_sdlWindow;
    RenderFormat m_format;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_frameTexture = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_nextTextureIndex = 0;

    std::unique_ptr<RenderTexture> m_textures[TextureCount];
    std::unique_ptr<RenderBuffer> m_readbackBuffer;
    std::unique_ptr<RenderCommandList> m_commandList;
    std::unique_ptr<RenderCommandFence> m_fence;

    TasSwapChain(RenderDevice* device, RenderCommandQueue* queue, RenderWindow renderWindow, SDL_Window* sdlWindow, RenderFormat format);
    ~TasSwapChain() override;

    bool present(uint32_t textureIndex, RenderCommandSemaphore** waitSemaphores, uint32_t waitSemaphoreCount) override;
    void wait() override;
    bool resize() override;
    bool needsResize() const override;
    void setVsyncEnabled(bool vsyncEnabled) override;
    bool isVsyncEnabled() const override;
    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    RenderTexture* getTexture(uint32_t textureIndex) override;
    uint32_t getTextureCount() const override;
    bool acquireTexture(RenderCommandSemaphore* signalSemaphore, uint32_t* textureIndex) override;
    RenderWindow getWindow() const override;
    bool isEmpty() const override;
    uint32_t getRefreshRate() const override;

private:
    void GetWindowPixelSize(uint32_t& width, uint32_t& height) const;
};
