#include "hud_state.h"

#include <ui/tas_hud.h>

#include <unordered_map>

using namespace Tas;

// --- The player contexts ---

static uint32_t g_speedContext;
static uint32_t g_evilContext;
// Which of the two was made last, for when both are around.
static bool g_evilContextIsNewer;

// SWA::Player::CPlayerSpeedContext::CPlayerSpeedContext (the base of the day Sonic context). Recorded even without the
// HUD, for the collision overlay.
PPC_FUNC_IMPL(__imp__sub_82330188);
PPC_FUNC(sub_82330188)
{
    uint32_t context = ctx.r3.u32;
    __imp__sub_82330188(ctx, base);

    g_speedContext = context;
    g_evilContextIsNewer = false;
}

void TasHud::OnEvilSonicContext(uint32_t context)
{
    g_evilContext = context;
    g_evilContextIsNewer = true;
}

static bool IsSonicContext(uint32_t context)
{
    // The context may have been destroyed since, and its memory reused.
    return context != 0 && LoadU32(context) == SONIC_CONTEXT_VFTABLE;
}

static bool IsEvilSonicContext(uint32_t context)
{
    return context != 0 && LoadU32(context) == EVIL_SONIC_CONTEXT_VFTABLE;
}

// The player to show: day Sonic or the Werehog, the one made last if both are alive, or 0 outside of levels.
uint32_t TasHud::GetPlayerContext(bool& werehog)
{
    bool sonic = IsSonicContext(g_speedContext);
    werehog = IsEvilSonicContext(g_evilContext) && (!sonic || g_evilContextIsNewer);
    return werehog ? g_evilContext : sonic ? g_speedContext : 0;
}

// --- Drawn frames ---

static uint32_t g_frame = 1;

uint32_t Tas::GetHudFrame()
{
    return g_frame;
}

void Tas::AdvanceHudFrame()
{
    g_frame++;
}

// --- Reading the player ---

// The machine points to the current state's record at +0x58 (compared with the new state in sub_82E669D8), which points
// to the state at +0x1C.
uint32_t Tas::GetCurrentState(uint32_t context)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    if (player == 0)
        return 0;

    uint32_t stateMachine = player + PLAYER_STATE_MACHINE_OFFSET;
    uint32_t record = LoadU32(stateMachine + STATE_MACHINE_CURRENT_OFFSET);
    uint32_t state = LoadU32(record + STATE_RECORD_STATE_OFFSET);
    return state != 0 && LoadU32(state + STATE_CONTEXT_OFFSET) == context ? state : 0;
}

const std::string& Tas::GetStateName(uint32_t state)
{
    static std::unordered_map<uint32_t, std::string> s_names;

    uint32_t vtable = LoadU32(state);
    auto it = s_names.find(vtable);
    if (it != s_names.end())
        return it->second;

    std::string name = "?";

    // The vtable is preceded by its complete object locator, which points to the type descriptor at +12,
    // whose mangled name is at +8.
    uint32_t typeDescriptor = LoadU32(LoadU32(vtable - 4) + 12);
    if (IsInImage(vtable) && IsInImage(typeDescriptor))
    {
        auto text = reinterpret_cast<const char*>(g_memory.Translate(typeDescriptor + 8));
        std::string mangled(text, strnlen(text, 96));

        if (mangled.starts_with(".?AV"))
        {
            name = mangled.substr(4, mangled.find('@') - 4);

            for (std::string_view prefix : { "CPlayerSpeedState", "CSonicState", "CState" })
            {
                if (name.starts_with(prefix) && name.size() > prefix.size())
                {
                    name = name.substr(prefix.size());
                    break;
                }
            }
        }
    }

    return s_names.emplace(vtable, name).first->second;
}

bool Tas::GetFlag(uint32_t context, uint32_t flagsOffset, uint32_t flag)
{
    uint32_t flags = LoadU32(context + flagsOffset);
    return flags != 0 && LoadU8(LoadU32(flags + 4) + flag) != 0;
}

// sub_8231B7D8.
bool Tas::IsActionBlocked(uint32_t context)
{
    return LoadU32(context + ACTION_BLOCKER_OFFSET) != 0 && !GetFlag(context, STATE_FLAGS_2_OFFSET, ACTION_BLOCKER_OVERRIDE_FLAG);
}

// Boosts: the ground actions call TryBoost (sub_823787B0), the air actions TryAirBoost (sub_82375748). Both refuse while
// the action blocker is set or Sonic already boosts (flag 14), and need X just pressed and more energy than
// BoostEnableChaosEnergy (215) or AirBoostEnableChaosEnergy (264). On the ground, flags 15 and 66 also refuse it. In the
// air, flag 19 is needed and flag 18 refuses it; with parameter 294 on, a target makes it a homing attack instead.
bool Tas::IsBoostAllowed(uint32_t context)
{
    return !IsActionBlocked(context) && !GetFlag(context, STATE_FLAGS_OFFSET, BOOSTING_FLAG) &&
        !GetFlag(context, STATE_FLAGS_OFFSET, BOOST_DISABLED_FLAG) && !GetFlag(context, STATE_FLAGS_OFFSET, BOOST_DISABLED_FLAG_2);
}

bool Tas::IsAirBoostAllowed(uint32_t context)
{
    return !IsActionBlocked(context) && !GetFlag(context, STATE_FLAGS_OFFSET, BOOSTING_FLAG) &&
        GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_ENABLED_FLAG) && !GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_DISABLED_FLAG) &&
        !(GetSonicChecks().AirBoostHoming && LoadU32(context + HOMING_TARGET_OFFSET) != 0);
}

const MovementMode* Tas::GetMovementMode(uint32_t context)
{
    uint32_t parameters = LoadU32(context + PARAMETERS_OFFSET);
    for (auto& mode : MOVEMENT_MODES)
    {
        if (parameters != 0 && parameters == LoadU32(mode.Parameters))
            return &mode;
    }

    return nullptr;
}

float Tas::VectorLength(uint32_t address, float& y)
{
    float x = LoadFloat(address);
    float z = LoadFloat(address + 8);
    y = LoadFloat(address + 4);
    return std::sqrt(x * x + y * y + z * z);
}

uint32_t Tas::GetEvilAttackHelper(uint32_t context)
{
    uint32_t node = FindMapNode(context + EVIL_HELPERS_OFFSET, EVIL_ATTACK_HELPER);
    return node != 0 ? LoadU32(node + 16) : 0;
}

// The motions are in an std::map per level of the attack list, by the action's MotionName.
uint32_t Tas::GetAttackMotion(uint32_t helper, uint32_t action)
{
    uint32_t node = FindMapNode(ATTACK_MOTION_MAPS + 12 * LoadU32(helper + ATTACK_HELPER_LEVEL_OFFSET),
        GetSharedString(action + ACTION_MOTION_NAME_OFFSET));
    return node != 0 ? LoadU32(node + 16) : 0;
}

// The player's animation state machine (sub_82307C70) maps names to IDs at +20 and IDs to states at +8 (sub_82BB97E8).
uint32_t Tas::GetAnimationState(uint32_t context, const std::string& name)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    uint32_t stateMachine = player != 0 ? LoadU32(player + PLAYER_ANIMATION_STATE_MACHINE_OFFSET) : 0;
    if (stateMachine == 0)
        return 0;

    uint32_t idNode = FindMapNode(stateMachine + 20, name, 21);
    uint32_t stateNode = idNode != 0 ? FindMapNode(stateMachine + 8, LoadU16(idNode + 16)) : 0;
    return stateNode != 0 ? LoadU32(stateNode + 16) : 0;
}

// --- What the game's checks did ---

static SonicChecks g_checks;

const SonicChecks& Tas::GetSonicChecks()
{
    return g_checks;
}

// Set while the lock-on phantom's set-up reads its frustum (which one: 3D or 2D), during the target search, while a
// quick step's update reads its animation frame, and while the 2D input runs.
static thread_local int g_recordingLockonFrustum = -1;
static thread_local bool g_inHomingSearch;
static bool g_recordQuickStepFrame;
static bool g_recordQuickStepAnimation;
static bool g_inPathInput = false;

// CPlayerSpeedPosturePluginOutOfControl's update: its time already has this frame's added. It ends the plugin once its
// time is past its duration, or on the ground with flag 25.
PPC_FUNC_IMPL(__imp__sub_82351090);
PPC_FUNC(sub_82351090)
{
    uint32_t plugin = ctx.r3.u32;
    __imp__sub_82351090(ctx, base);

    if (TasHud::IsEnabled() && LoadU32(plugin + PLUGIN_CONTEXT_OFFSET) == g_speedContext)
    {
        g_checks.OutOfControlFrame = g_frame;
        g_checks.OutOfControlTime = LoadFloat(plugin + STATE_TIME_OFFSET);
        g_checks.OutOfControlDuration = LoadFloat(plugin + OUT_OF_CONTROL_DURATION_OFFSET);
        g_checks.OutOfControlEnded = LoadU8(plugin + PLUGIN_ENDED_OFFSET) != 0;
    }
}

// The context's TryJump: jumps if the jump button is pressed. The states that allow jumping call it every
// frame; others, like a quick step until it ends, don't.
PPC_FUNC_IMPL(__imp__sub_82330CB0);
PPC_FUNC(sub_82330CB0)
{
    if (TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext)
    {
        g_checks.JumpCheckFrame = g_frame;
        g_checks.JumpCheckState = GetCurrentState(ctx.r3.u32);
    }

    __imp__sub_82330CB0(ctx, base);
}

// The context's TryHomingAttack, the first thing the air actions try once Sonic is in the air and in control.
PPC_FUNC_IMPL(__imp__sub_823778D8);
PPC_FUNC(sub_823778D8)
{
    if (TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext)
        g_checks.HomingCheckFrame = g_frame;

    __imp__sub_823778D8(ctx, base);
}

// The context's target search.
PPC_FUNC_IMPL(__imp__sub_82375FE0);
PPC_FUNC(sub_82375FE0)
{
    uint32_t context = ctx.r3.u32;
    bool record = TasHud::IsEnabled() && context == g_speedContext;

    if (record)
    {
        uint32_t node = LoadU32(context + MATRIX_NODE_OFFSET);
        for (int i = 0; i < 3; i++)
        {
            g_checks.HomingSearchPosition[i] = LoadFloat(node + MATRIX_NODE_POSITION_OFFSET + i * 4);
            for (int row = 0; row < 3; row++)
                g_checks.HomingSearchRotation[row][i] = LoadFloat(node + MATRIX_NODE_MATRIX_OFFSET + row * 16 + i * 4);
        }

        g_checks.HomingSearchPoseCount = 0;
        g_inHomingSearch = true;
    }

    __imp__sub_82375FE0(ctx, base);

    if (record)
    {
        g_inHomingSearch = false;
        g_checks.HomingSearchFrame = g_frame;
    }
}

// Places a phantom (r3) with a matrix (r4). Records the lock-on phantom's placements during the search.
PPC_FUNC_IMPL(__imp__sub_82494318);
PPC_FUNC(sub_82494318)
{
    if (g_inHomingSearch && g_checks.HomingSearchPoseCount < 2)
    {
        for (int i = 0; i < 16; i++)
            g_checks.HomingSearchPoses[g_checks.HomingSearchPoseCount][i] = LoadFloat(ctx.r4.u32 + i * 4);

        g_checks.HomingSearchPoseCount++;
    }

    __imp__sub_82494318(ctx, base);
}

// Makes a lock-on phantom (r6) for the context (r3). Records its frustum's parameters, which it reads in order.
PPC_FUNC_IMPL(__imp__sub_82379718);
PPC_FUNC(sub_82379718)
{
    bool record = TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext;
    if (record)
        g_recordingLockonFrustum = ctx.r6.u32 - ctx.r3.u32 == HOMING_PHANTOM_2D_OFFSET ? 1 : 0;

    __imp__sub_82379718(ctx, base);
    g_recordingLockonFrustum = -1;
}

// The context's TryStomp, called by the air states when no action tried before it (like a homing attack) was taken.
PPC_FUNC_IMPL(__imp__sub_823765C0);
PPC_FUNC(sub_823765C0)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_checks.StompCheckFrame = g_frame;
        g_checks.StompDisabledByHop = GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG);
        g_checks.StompBlocked = IsActionBlocked(context);
    }

    __imp__sub_823765C0(ctx, base);
}

// The context's TryQuickStep, called by the ground actions when no jump was taken.
PPC_FUNC_IMPL(__imp__sub_82386E60);
PPC_FUNC(sub_82386E60)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_checks.QuickStepCheckFrame = g_frame;
        g_checks.QuickStepCheckState = GetCurrentState(context);
        g_checks.QuickStepDisabled = LoadU8(context + QUICK_STEP_DISABLED_OFFSET) != 0;
        g_checks.QuickStepBlocked = IsActionBlocked(context);
    }

    __imp__sub_82386E60(ctx, base);
}

// The player's parameter getters by ID (sub_8245DB60 floats in f1, sub_8245DB18 bools in r3).
PPC_FUNC_IMPL(__imp__sub_8245DB60);
PPC_FUNC_IMPL(__imp__sub_8245DB18);

// More than the getters' calls use below the caller's stack pointer.
static constexpr uint32_t PARAMETER_GETTER_STACK_SIZE = 0x1000;

// Calls a parameter getter on a copy of the registers and returns them. The getters only look the value up (no writes
// outside their stack), and the stack they use is put back, so the game's memory and registers stay as they were.
static PPCContext CallParameterGetter(const PPCContext& ctx, uint8_t* base, uint32_t context, uint32_t parameter,
    void (*getter)(PPCContext& __restrict, uint8_t*))
{
    PPCContext call = ctx;
    call.r3.u64 = LoadU32(context + PARAMETER_READER_OFFSET);
    call.r4.u64 = parameter;

    uint8_t* stack = base + (ctx.r1.u32 - PARAMETER_GETTER_STACK_SIZE);
    static uint8_t saved[PARAMETER_GETTER_STACK_SIZE];
    memcpy(saved, stack, sizeof(saved));
    getter(call, base);
    memcpy(stack, saved, sizeof(saved));
    return call;
}

// The context's TryBoost, called by the ground actions, sliding and pushing a wall. It reads BoostEnableChaosEnergy only
// once X is pressed, so the HUD reads it here on every try: a mod's value then counts before the first press.
PPC_FUNC_IMPL(__imp__sub_823787B0);
PPC_FUNC(sub_823787B0)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_checks.BoostCheckFrame = g_frame;
        g_checks.BoostCheckState = GetCurrentState(context);
        g_checks.BoostAllowed = IsBoostAllowed(context);
        g_checks.BoostEnableEnergy =
            float(CallParameterGetter(ctx, base, context, BOOST_ENABLE_CHAOS_ENERGY_PARAMETER, __imp__sub_8245DB60).f1.f64);
    }

    __imp__sub_823787B0(ctx, base);
}

// The context's TryAirBoost, called by the air actions after TryStomp. Like TryBoost, it reads its parameters
// (XButtonHoming, AirBoostEnableChaosEnergy) only once X is pressed.
PPC_FUNC_IMPL(__imp__sub_82375748);
PPC_FUNC(sub_82375748)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_checks.AirBoostCheckFrame = g_frame;
        g_checks.AirBoostHoming = CallParameterGetter(ctx, base, context, AIR_BOOST_HOMING_PARAMETER, __imp__sub_8245DB18).r3.u8 != 0;
        g_checks.AirBoostEnableEnergy =
            float(CallParameterGetter(ctx, base, context, AIR_BOOST_ENABLE_CHAOS_ENERGY_PARAMETER, __imp__sub_8245DB60).f1.f64);
    }

    __imp__sub_82375748(ctx, base);
}

// The quick steps' updates, which first read the animation frame they compare.
static void UpdateQuickStep(PPCContext& ctx, uint8_t* base, void (*update)(PPCContext&, uint8_t*))
{
    uint32_t state = ctx.r3.u32;
    g_recordQuickStepFrame = TasHud::IsEnabled() && g_speedContext != 0 && LoadU32(state + STATE_CONTEXT_OFFSET) == g_speedContext;
    update(ctx, base);
    g_recordQuickStepFrame = false;
}

// SWA::Player::CSonicContext::CStateQuickStep::UpdateState
PPC_FUNC_IMPL(__imp__sub_823968D8);
PPC_FUNC(sub_823968D8)
{
    UpdateQuickStep(ctx, base, __imp__sub_823968D8);
}

// SWA::Player::CStateRunQuickStep::UpdateState
PPC_FUNC_IMPL(__imp__sub_82397370);
PPC_FUNC(sub_82397370)
{
    UpdateQuickStep(ctx, base, __imp__sub_82397370);
}

// The frame of the player's current animation: the elapsed time times the animation's frame count - 1 over its
// duration, in single precision (sub_82BBD568).
PPC_FUNC_IMPL(__imp__sub_82307D38);
PPC_FUNC(sub_82307D38)
{
    bool record = g_recordQuickStepFrame;
    g_recordQuickStepFrame = false;

    if (record)
    {
        g_recordQuickStepAnimation = true;
        g_checks.QuickStepAnimation = 0;
    }

    __imp__sub_82307D38(ctx, base);

    if (record)
    {
        g_recordQuickStepAnimation = false;
        g_checks.QuickStepPreviousLocalTime = g_checks.QuickStepUpdateFrame + 1 == g_frame ? g_checks.QuickStepLocalTime : NAN;
        g_checks.QuickStepLocalTime = LoadFloat(LoadU32(g_checks.QuickStepAnimation + ANIMATION_CONTROL_OFFSET) + ANIMATION_CONTROL_TIME_OFFSET);
        g_checks.QuickStepAnimationFrame = float(ctx.f1.f64);
        g_checks.QuickStepUpdateFrame = g_frame;
    }
}

// The animation state behind sub_82307D38.
PPC_FUNC_IMPL(__imp__sub_82BBD568);
PPC_FUNC(sub_82BBD568)
{
    if (g_recordQuickStepAnimation)
    {
        g_checks.QuickStepAnimation = ctx.r3.u32;
        g_recordQuickStepAnimation = false;
    }

    __imp__sub_82BBD568(ctx, base);
}

// The 2D input (sub_8234F140: CPlayerSpeedPostureInput2DStandard, also OnPath and ForcePath).
PPC_FUNC_IMPL(__imp__sub_8234F140);
PPC_FUNC(sub_8234F140)
{
    g_inPathInput = true;
    __imp__sub_8234F140(ctx, base);
    g_inPathInput = false;
}

// Keeps the part of the input (r5) along the path's direction (r6, turned to Sonic's facing side).
PPC_FUNC_IMPL(__imp__sub_8231EE70);
PPC_FUNC(sub_8231EE70)
{
    if (g_inPathInput && TasHud::IsEnabled())
    {
        g_checks.PathFrame = g_frame;
        for (int i = 0; i < 3; i++)
            g_checks.PathDirection[i] = LoadFloat(ctx.r6.u32 + i * 4);
    }

    __imp__sub_8231EE70(ctx, base);
}

// The 2D air posture's Brake state's update. The Normal state (sub_82344868, where M-Speed happens) switches to it when
// sub_8231B0F0 says so: Sonic's speed at least 1 and his input, after the deadzone, more than BrakeEnableAngle
// (parameter 31, degrees) away from his front (or his velocity, by ctx+960), or zeroed by the deadzone.
PPC_FUNC_IMPL(__imp__sub_82344B88);
PPC_FUNC(sub_82344B88)
{
    if (TasHud::IsEnabled())
        g_checks.Air2DBrakeFrame = g_frame;

    __imp__sub_82344B88(ctx, base);
}

// Reads a float parameter of the player by ID. Records StompingDisableTime when the short hop reads it, the
// movement input's deadzone when the input is read, day Sonic's InAirMaxAccelVelocity when the air update reads it, the
// lock-on frustum, and how long HomingAttackAfter waits before the air actions. (The boosts' parameters are read in
// TryBoost and TryAirBoost above.)
PPC_FUNC_IMPL(__imp__sub_8245DB60);
PPC_FUNC(sub_8245DB60)
{
    uint32_t reader = ctx.r3.u32;
    uint32_t parameter = ctx.r4.u32;
    __imp__sub_8245DB60(ctx, base);

    if (parameter == STOMPING_DISABLE_TIME_PARAMETER && TasHud::IsEnabled())
        g_checks.StompingDisableTime = float(ctx.f1.f64);

    if (parameter == MOVEMENT_INPUT_DEADZONE_PARAMETER && TasHud::IsEnabled())
        g_checks.MovementInputDeadzone = float(ctx.f1.f64);

    if (parameter == IN_AIR_MAX_ACCEL_VELOCITY_PARAMETER && TasHud::IsEnabled() && g_speedContext != 0 &&
        reader == LoadU32(g_speedContext + PARAMETER_READER_OFFSET))
    {
        g_checks.InAirMaxAccelVelocity = float(ctx.f1.f64);
    }

    if (g_recordingLockonFrustum >= 0 && parameter >= LOCKON_FRUSTUM_FIRST_PARAMETER && parameter < LOCKON_FRUSTUM_FIRST_PARAMETER + 4)
        g_checks.LockonFrustums[g_recordingLockonFrustum][parameter - LOCKON_FRUSTUM_FIRST_PARAMETER] = float(ctx.f1.f64);

    if (parameter == ACTION_ENABLE_AFTER_HOMING_ATTACK_PARAMETER && TasHud::IsEnabled() && g_speedContext != 0 &&
        reader == LoadU32(g_speedContext + PARAMETER_READER_OFFSET))
    {
        g_checks.ActionEnableAfterHomingAttack = float(ctx.f1.f64);
    }
}
