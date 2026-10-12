#pragma once

#include <cstdint>

// Draws what the game's collision debug views (the EnableStageCollisionDebugView, EnableEventCollisionDebugView and
// EnableObjectCollisionDebugView codes) leave out: with the stage collision view, the grind rails' collision, and with
// any of them, the player's collision (day Sonic's or the Werehog's), as see-through solids. Drawn over the frame,
// projected with the game's camera; F12 switches between over everything and behind what the game drew (tested
// against its depth buffer). It only reads the game's memory.
namespace CollisionOverlay
{
    bool IsEnabled();

    // From SWA::CCamera::UpdateSerial.
    void OnCameraUpdate(uint32_t camera);

    void Draw();
}
