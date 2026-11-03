#pragma once

// The TAS camera: a view that only changes the picture. The game keeps simulating with its own camera (input
// directions, lock-on, culling); only the view matrix and eye position sent to the GPU are swapped, where they equal the
// game camera's (the shadow map uses its own and is left alone). "\" cycles: off, chase, top-down. Only with the TAS HUD
// on (UNLEASHED_TAS_HUD=1).

#include <cstdint>

namespace TasCamera
{
    enum class Mode
    {
        Off,
        Chase,
        TopDown
    };

    Mode GetMode();
    const char* GetModeName();

    // After the game camera's update (its UpdateSerial): records its view and eye, and the TAS view for them.
    void OnCameraUpdate(uint32_t camera);

    // Vertex/pixel constants being uploaded, still big-endian as the game wrote them: replaces the game camera's view
    // (16 floats) or eye (3 floats) with the TAS camera's. Returns whether it did.
    bool ReplaceView(uint32_t* constants);
    bool ReplaceEye(uint32_t* constants);

    // The TAS view and eye that match this game camera's current ones, for drawing the overlay on the picture.
    bool GetView(uint32_t camera, float view[16], float eye[3]);
}
