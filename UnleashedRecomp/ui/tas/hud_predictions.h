#pragma once

// What the TAS HUD predicts: what a press entered now would do on the next update (jump, stomp, quick step, homing
// attack, boost), when the Werehog's attack ends, and where on the stick M-Speed and D-Speed work.

#include <cstdint>
#include <string>

namespace Tas
{
    // --- The stick ---

    // How the game reads the stick: each axis loses a deadzone of 8689 and the 24078 left (to 32767) is scaled to 0..1
    // (sub_82EA7350 sets them), then the vector is shortened to length 1 if it's longer. On the stick as SDL reports it
    // (-1..1, units of 32768): the deadzone, and how far the stick goes past it per unit the game reads.
    inline constexpr float GAME_STICK_DEADZONE = 8689.0f / 32768.0f;
    inline constexpr float GAME_STICK_RANGE = 24078.0f / 32768.0f;

    // The break-jump glitch: holding the stick through a switch to 2D and a jump, where the game reads it as at least 0.1
    // long (2D movement ignores shorter input) but shorter than 0.3 (NoPadStopWalkPadLengthLimit, parameter 140), makes
    // Sonic's velocity explode. Found by replaying a movie with the stick at different positions, then matching the
    // limits with the game's values.
    inline constexpr float BREAK_JUMP_MIN_LENGTH = 0.1f;
    inline constexpr float BREAK_JUMP_MAX_LENGTH = 0.3f;

    // The longest movement input that M-Speeds (and D-Speeds): above it, the air update clears flag 27.
    inline constexpr float M_SPEED_MAX_INPUT = 0.3f;

    // What decides M-Speed's directions, for the frame being drawn. Sonic's facing on the stick, as drawn (x right, y
    // down): an input of length m in direction a aims for m * (cos a * FacingX + sin a * FacingY) * Target along his
    // facing. Off in the air once the flag is cleared, until he lands.
    // In 2D, the path's direction on the stick the same way, by the input basis as it is now (PathX, PathY), and how much
    // of it points along his facing (PathForward): an input's part a along the path aims for a * PathForward * Target.
    struct MSpeedRule
    {
        float FacingX = 0.0f;
        float FacingY = -1.0f;
        float Target = 15.0f;
        float AlongFacing = 0.0f;
        bool Off = false;
        float PathX = 0.0f;
        float PathY = -1.0f;
        float PathForward = 1.0f;
    };

    // Where the stick as the game reads it (y up) is: in the zone's range of lengths (on the side Sonic faces, for
    // M-Speed), and whether it M-Speeds (D-Speeds) there.
    struct MSpeedInput
    {
        bool InZone = false;
        bool Works = false;
    };

    MSpeedRule GetMSpeedRule(uint32_t context, bool grounded);

    // Whether direction a (as drawn) is on the side of the stick Sonic faces, where M-Speed can be.
    bool IsFacingSide(const MSpeedRule& rule, float cosine, float sine);
    // The longest movement input that M-Speeds in direction a (as drawn), or 0.
    float GetMSpeedMaxInput(const MSpeedRule& rule, float cosine, float sine);
    // The stick length (as the game reads it) that gives a 3D movement input.
    float GetStickLengthForMovementInput(float input);
    MSpeedInput GetMSpeedInput(const MSpeedRule& rule, float gameX, float gameY);

    // The longest part along the path that D-Speeds, or 0; whether there's a path direction on the stick at all.
    float GetDSpeedMaxInput(const MSpeedRule& rule);
    bool HasDSpeedSide(const MSpeedRule& rule);
    MSpeedInput GetDSpeedState(const MSpeedRule& rule, float gameX, float gameY);

    // --- Day Sonic's presses ---

    // What a press entered now would do on the next update, and otherwise in how many frames it would (-1: unknown or not
    // counting down).
    struct SonicPresses
    {
        bool Sliding = false;
        bool Stomping = false;
        bool CanJump = false;
        bool CanStomp = false;
        int StompUpdates = -1;
        bool CanQuickStep = false;
        int QuickStepUpdates = -1;
        // The homing attack: whether the next update's air actions take a press, with a target; or, without one yet, only
        // estimated; in how many updates HomingAttackAfter runs them; and whether A would only dash forward.
        bool HomingAllowed = false;
        bool HomingPress = false;
        bool HomingTarget = false;
        bool CanHomingAttack = false;
        bool HomingEstimated = false;
        int HomingUpdates = -1;
        bool CanBoost = false;
        bool Boosting = false;
        float Energy = 0.0f;
    };

    // Also remembers the state for the next frame: call once per drawn frame, with day Sonic shown.
    SonicPresses PredictSonicPresses(uint32_t context, uint32_t state, const std::string& stateName, float stateTime, bool grounded,
        float deltaTime);

    // --- The Werehog ---

    struct WerehogAttack
    {
        bool Dashing = false;
        bool Attacking = false;
        bool CanGuard = false;
        // When the attack's action ends on its own (-1: not in an attack), and on the ground with the stick held; the action
        // that follows (its KEY__End), if any.
        int FramesLeft = -1;
        int FramesLeftWithStick = -1;
        std::string NextAction;
        // The state as shown ("Attack <ActionName>" during an attack), and the drawn frames since it or the action changed.
        std::string StateLabel;
        int StateFrames = 0;
    };

    // Also remembers the state, action and animation for the next frame: call once per drawn frame, with the Werehog shown.
    WerehogAttack PredictWerehogAttack(uint32_t context, uint32_t state, const std::string& stateName, float stateTime, bool grounded,
        float deltaTime);
}
