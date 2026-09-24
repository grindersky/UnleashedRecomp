#pragma once

#include <cstdint>

// A TAS mode overlay, enabled with UNLEASHED_TAS_HUD=1: Sonic's speed, and the left stick with the
// M-/D-Speed target zones. It's drawn into the game's frames, so it also shows up in encodes.
namespace TasHud
{
    bool IsEnabled();

    // From XamInputGetState: the left stick given to the game, in XInput's convention (Y up).
    void OnInputState(int16_t thumbLX, int16_t thumbLY);

    void Draw();
}
