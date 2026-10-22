#pragma once

#include <cstdint>
#include <vector>

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

    // The TAS HUD's radar, a square with its top left corner at (x, y): day Sonic seen from above, turned like the camera,
    // with what collects rings, the rings around him (with their height over his feet), enemies and the goal ring, and in
    // the air an estimate of his path and of where he reaches the next ring's height. Home switches between two sizes and
    // off. Works without the collision views.
    void DrawRadar(float x, float y, float size, uint32_t context, bool grounded, bool diving, uint32_t frame, float deltaTime);

    // The objects (but rings) with a collision within range of a point, each where its smallest collision is.
    struct ObjectCentre
    {
        uint32_t Object;
        uint32_t Vftable;
        float X, Y, Z;
        float Radius;
        bool Enemy;
    };

    void GetObjectCentres(float x, float y, float z, float range, std::vector<ObjectCentre>& centres);
    bool GetObjectCentre(uint32_t object, float& x, float& y, float& z);
}
