#pragma once

#include <cstdint>

// A TAS mode overlay, enabled with UNLEASHED_TAS_HUD=1, shown in levels: the player's speed and state (day
// Sonic: his movement mode, on the ground, sliding, whether a jump or stomp would be accepted; the Werehog: on
// the ground, dashing, attacking, which attack and when it ends, whether a guard would be accepted, his Unleash
// gauge and the stage time), the left stick (for Sonic with the M-/D-Speed target zones and the break-jump zone),
// and the buttons. It's drawn into the game's frames, so it also shows up in encodes. It only reads the game's
// memory.
namespace TasHud
{
    bool IsEnabled();

    // From XamInputGetState: the left stick given to the game, in XInput's convention (Y up).
    void OnInputState(int16_t thumbLX, int16_t thumbLY);

    // From the constructor of the Werehog's player context (SWA::Player::CEvilSonicContext).
    void OnEvilSonicContext(uint32_t context);

    void Draw();
}
