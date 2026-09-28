#pragma once

#include <cstdint>

// A TAS mode overlay, enabled with UNLEASHED_TAS_HUD=1: Sonic's speed, his state (on the ground,
// sliding, whether a jump or stomp would be accepted), the left stick with the M-/D-Speed target zones and
// the break-jump zone, and the buttons. It's drawn into the game's frames, so it also shows up in encodes. It only reads the
// game's memory.
namespace TasHud
{
    bool IsEnabled();

    // From XamInputGetState: the left stick given to the game, in XInput's convention (Y up).
    void OnInputState(int16_t thumbLX, int16_t thumbLY);

    void Draw();
}
