#pragma once

// The TAS HUD's view of the player: his context and state, read from the game's memory, and what the game's own checks
// did during the last update (recorded by hooks on them, only while the HUD is on).

#include "game_addresses.h"
#include "guest_memory.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace Tas
{
    // Drawn frames, counted by TasHud::Draw: whether a check below ran during the update of the frame being drawn.
    uint32_t GetHudFrame();
    void AdvanceHudFrame();

    // The current state of the player's state machine, or 0.
    uint32_t GetCurrentState(uint32_t context);

    // The name of the state's class, from the game's RTTI: "CStateJumpShort@CSonicContext@..." becomes "JumpShort".
    const std::string& GetStateName(uint32_t state);

    bool GetFlag(uint32_t context, uint32_t flagsOffset, uint32_t flag);

    template<size_t N>
    bool IsStateIn(uint32_t state, const uint32_t (&vftables)[N])
    {
        uint32_t vftable = state != 0 ? LoadU32(state) : 0;
        return std::find(std::begin(vftables), std::end(vftables), vftable) != std::end(vftables);
    }

    // Whether the action blocker (ACTION_BLOCKER_OFFSET) refuses stomps, quick steps, boosts and homing attacks.
    bool IsActionBlocked(uint32_t context);

    // Whether TryBoost and TryAirBoost would boost on a press, apart from the energy. Not replicated: sub_82308838
    // (ground) and the ability check sub_82374CE0 (air).
    bool IsBoostAllowed(uint32_t context);
    bool IsAirBoostAllowed(uint32_t context);

    // Sonic's movement mode, from the parameter table he uses, or null for another one.
    const MovementMode* GetMovementMode(uint32_t context);

    // A vector's length, with its Y.
    float VectorLength(uint32_t address, float& y);

    // The Werehog's attack helper, or 0.
    uint32_t GetEvilAttackHelper(uint32_t context);
    // The motion an attack action plays, or 0.
    uint32_t GetAttackMotion(uint32_t helper, uint32_t action);
    // The player's animation state for an animation, or 0.
    uint32_t GetAnimationState(uint32_t context, const std::string& name);

    // What day Sonic's checks did, with the drawn frame they ran in (see GetHudFrame), and the parameters the game read.
    struct SonicChecks
    {
        // TryJump (sub_82330CB0), and the state it ran in.
        uint32_t JumpCheckFrame = 0;
        uint32_t JumpCheckState = 0;
        // TryStomp (sub_823765C0), and whether the hop's lock or the blocker refused it.
        uint32_t StompCheckFrame = 0;
        bool StompDisabledByHop = false;
        bool StompBlocked = false;
        // TryQuickStep (sub_82386E60), its state, and what refused it.
        uint32_t QuickStepCheckFrame = 0;
        uint32_t QuickStepCheckState = 0;
        bool QuickStepDisabled = false;
        bool QuickStepBlocked = false;
        // TryBoost (sub_823787B0), its state, and whether everything but the press and the energy allowed the boost;
        // TryAirBoost (sub_82375748).
        uint32_t BoostCheckFrame = 0;
        uint32_t BoostCheckState = 0;
        bool BoostAllowed = false;
        uint32_t AirBoostCheckFrame = 0;
        // TryHomingAttack (sub_823778D8).
        uint32_t HomingCheckFrame = 0;

        // Parameters, as the game read them last (defaults until then). The boosts' energies were 1 in MSPEED_TEST.ltm and
        // RooftopRunAct1.ltm.
        float StompingDisableTime = 0.15f;
        float MovementInputDeadzone = 0.02f;
        float InAirMaxAccelVelocity = 15.0f;
        float BoostEnableEnergy = 1.0f;
        float AirBoostEnableEnergy = 1.0f;
        bool AirBoostHoming = false;
        float ActionEnableAfterHomingAttack = NAN;
        // The lock-on frustums (3D, 2D): aspect, field of view, near, far.
        float LockonFrustums[2][4] = { { NAN, NAN, NAN, NAN }, { NAN, NAN, NAN, NAN } };

        // The last target search (sub_82375FE0): Sonic's position and rotation (his axes, as rows) then, and the
        // phantom's placements (sub_82494318: rows of its axes, then its position).
        uint32_t HomingSearchFrame = 0;
        float HomingSearchPosition[3] = {};
        float HomingSearchRotation[3][3] = {};
        float HomingSearchPoses[2][16] = {};
        int HomingSearchPoseCount = 0;

        // The animation frame a quick step's update compared on the last frame (sub_82307D38), with the animation and its
        // local time then, and the time on the frame before.
        uint32_t QuickStepUpdateFrame = 0;
        float QuickStepAnimationFrame = 0.0f;
        uint32_t QuickStepAnimation = 0;
        float QuickStepLocalTime = 0.0f;
        float QuickStepPreviousLocalTime = 0.0f;

        // The path direction the 2D input last split the stick along (sub_8231EE70 inside sub_8234F140), turned to Sonic's
        // facing side.
        uint32_t PathFrame = 0;
        float PathDirection[3] = {};

        // The out-of-control plugin's last update (sub_82351090): its time and duration then, and whether it ended.
        uint32_t OutOfControlFrame = 0;
        float OutOfControlTime = 0.0f;
        float OutOfControlDuration = 0.0f;
        bool OutOfControlEnded = false;

        // The 2D air posture's Brake state's update (CPlayerSpeedPosture2DAir::CStateBrake, sub_82344B88).
        uint32_t Air2DBrakeFrame = 0;
    };

    const SonicChecks& GetSonicChecks();
}
