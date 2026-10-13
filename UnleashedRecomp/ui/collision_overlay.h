#pragma once

#include <cstdint>

// Draws what the game's collision debug views (the EnableStageCollisionDebugView, EnableEventCollisionDebugView and
// EnableObjectCollisionDebugView codes) leave out: with the stage collision view, the grind rails' collision, and with
// any of them, the player's collision (day Sonic's or the Werehog's; his body over the rest, and day Sonic's others
// green while he attacks) and an arrow along his facing at his feet, and the enemies' (red) and the goal ring's (white)
// where the event and object views don't draw them, as see-through solids.
// Drawn over the frame, projected with the game's camera, behind what the game drew (tested against its depth buffer);
// F12 switches to over everything and back. Hidden with the views (F11). It only reads the game's memory.
namespace CollisionOverlay
{
    bool IsEnabled();

    // From SWA::CCamera::UpdateSerial.
    void OnCameraUpdate(uint32_t camera);

    void Draw();
}
