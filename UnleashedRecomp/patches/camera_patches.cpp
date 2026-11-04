#include <api/SWA.h>
#include <ui/game_window.h>
#include <user/config.h>
#include <gpu/video.h>
#include <ui/collision_overlay.h>
#include <ui/tas/tas_camera.h>
#include "camera_patches.h"
#include "aspect_ratio_patches.h"

void CameraAspectRatioMidAsmHook(PPCRegister& r30, PPCRegister& r31)
{
    r30.u32 = 0;

    auto camera = (SWA::CCamera*)g_memory.Translate(r31.u32);

    // Dynamically adjust horizontal aspect ratio to window dimensions.
    camera->m_HorzAspectRatio = g_aspectRatio;
}

float AdjustFieldOfView(float fieldOfView, float aspectRatio)
{
    if (Config::AspectRatio == EAspectRatio::OriginalNarrow)
    {
        // Replicate the original incorrect field of view formula if requested. 
        fieldOfView *= NARROW_ASPECT_RATIO;
    }
    else if (aspectRatio < WIDE_ASPECT_RATIO)
    {
        // Use proper VERT+ otherwise for narrow aspect ratios.
        fieldOfView = 2.0 * atan(tan(0.5 * fieldOfView) / aspectRatio * WIDE_ASPECT_RATIO);
    }

    return fieldOfView;
}

void CameraFieldOfViewMidAsmHook(PPCRegister& r31, PPCRegister& f31)
{
    auto camera = (SWA::CCamera*)g_memory.Translate(r31.u32);

    f31.f64 = AdjustFieldOfView(f31.f64, camera->m_HorzAspectRatio);
}

PPC_FUNC_IMPL(__imp__sub_824697B0);
PPC_FUNC(sub_824697B0)
{
    auto pCamera = (SWA::CCamera*)g_memory.Translate(ctx.r3.u32);

    pCamera->m_InvertX = Config::HorizontalCamera == ECameraRotationMode::Reverse;
    pCamera->m_InvertY = Config::VerticalCamera == ECameraRotationMode::Reverse;

    __imp__sub_824697B0(ctx, base);
}

// SWA::CCamera::UpdateSerial
PPC_FUNC_IMPL(__imp__sub_82467890);
PPC_FUNC(sub_82467890)
{
    if (g_needsResize)
    {
        // Recompute the projection matrix for one frame to fix stretching on pause menu.
        auto r3 = ctx.r3;
        auto r4 = ctx.r4;
        sub_82468E38(ctx, base);
        ctx.r3 = r3;
        ctx.r4 = r4;
    }

    uint32_t camera = ctx.r3.u32;
    __imp__sub_82467890(ctx, base);
    CollisionOverlay::OnCameraUpdate(camera);
    TasCamera::OnCameraUpdate(camera);
}

// The motion blur's previous view (called only from its pass, sub_82BA9990, on the render side): r3 out, r4 the render
// camera's view (the pass's own copy on its stack), r5 the view it had last frame; it limits the camera's motion between
// them, and the pass makes g_MtxPrevInvViewProjection (for FxZBufferToVelocity) from it and the r4 copy. With the TAS
// camera on, both become the TAS camera's views for them, or the blur would be from the game camera's motion over the
// TAS camera's depth. Only the pass's copy and a temporary on the stack change; the game's camera and its stored
// previous view stay as they are.
PPC_FUNC_IMPL(__imp__sub_82BA88C8);
PPC_FUNC(sub_82BA88C8)
{
    auto load = [&](uint32_t address, float matrix[16])
    {
        auto values = reinterpret_cast<be<float>*>(g_memory.Translate(address));
        for (int i = 0; i < 16; i++)
            matrix[i] = values[i];
    };

    auto store = [&](uint32_t address, const float matrix[16])
    {
        auto values = reinterpret_cast<be<float>*>(g_memory.Translate(address));
        for (int i = 0; i < 16; i++)
            values[i] = matrix[i];
    };

    float gameView[16], gamePrevView[16], view[16], prevView[16];
    if (TasCamera::GetMode() == TasCamera::Mode::Off || ctx.r4.u32 == 0 || ctx.r5.u32 == 0)
    {
        __imp__sub_82BA88C8(ctx, base);
        return;
    }

    load(ctx.r4.u32, gameView);
    load(ctx.r5.u32, gamePrevView);

    if (!TasCamera::FindView(gameView, view))
    {
        __imp__sub_82BA88C8(ctx, base);
        return;
    }

    // No record for last frame's view (the TAS camera was just turned on): no camera motion.
    if (!TasCamera::FindView(gamePrevView, prevView))
        memcpy(prevView, view, sizeof(view));

    store(ctx.r4.u32, view);

    // The previous view goes in a temporary below the caller's stack. The callee writes at its entry r1 + 60 (the
    // caller's parameter area), so the matrix sits at + 128.
    uint32_t stack = ctx.r1.u32;
    ctx.r1.u32 = stack - 256;
    uint32_t temporary = ctx.r1.u32 + 128;
    store(temporary, prevView);
    ctx.r5.u32 = temporary;

    __imp__sub_82BA88C8(ctx, base);

    ctx.r1.u32 = stack;
}
