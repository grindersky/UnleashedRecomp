#include "tas_hud.h"
#include <api/SWA.h>
#include <app.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <ui/imgui_utils.h>
#include <tas_mode.h>

#include <cmath>
#include <numbers>
#include <unordered_map>

// The day Sonic player context (SWA::Player::CSonicContext), recorded when its constructor runs. Its
// velocity is split like in Sonic Generations' CPlayerSpeedContext: the full velocity, the part along the
// ground and the part along the up vector (full = horizontal + vertical). Found by comparing per-frame
// dumps of the context with how Sonic's position moves.
static constexpr uint32_t SONIC_CONTEXT_VFTABLE = 0x820182C4;
static constexpr uint32_t VELOCITY_OFFSET = 0x210;
static constexpr uint32_t HORIZONTAL_VELOCITY_OFFSET = 0x220;
static constexpr uint32_t VERTICAL_VELOCITY_OFFSET = 0x230;

// More of the context, found in the game's code:
// - the player object, whose state machine is at +0xCC (the context's ChangeState, sub_82307E18);
static constexpr uint32_t PLAYER_OFFSET = 0x100;
static constexpr uint32_t PLAYER_STATE_MACHINE_OFFSET = 0xCC;
// - whether Sonic stands on the ground (sub_82316530 switches to "Fall" when it's false);
static constexpr uint32_t GROUNDED_OFFSET = 0x3C0;
// - two sets of state flags: counters, one byte per flag, in an array at +4 (sub_823171C8 and the next ones).
static constexpr uint32_t STATE_FLAGS_OFFSET = 0x4AC;
static constexpr uint32_t STATE_FLAGS_2_OFFSET = 0x4B0;
// Set by a short hop (CStateJumpShort) until it has lasted StompingDisableTime (parameter 292, 0.15 s by
// default): until then, stomping is refused.
static constexpr uint32_t STOMP_DISABLED_FLAG = 20;
static constexpr uint32_t STOMPING_DISABLE_TIME_PARAMETER = 292;
// How many frames after an air boost (a one-frame tap) the stomp is accepted.
static constexpr int AIR_BOOST_STOMP_FRAMES = 2;
// The movement input is ignored while it's shorter than this (parameter 12, read by sub_823083F0).
static constexpr uint32_t MOVEMENT_INPUT_DEADZONE_PARAMETER = 12;
// Stomping is also refused while this is set, unless flag 7 of the second set is (sub_8231B7D8).
static constexpr uint32_t STOMP_BLOCKER_OFFSET = 0xCC8;
static constexpr uint32_t STOMP_BLOCKER_OVERRIDE_FLAG = 7;
// Quick steps: the ground actions (the context's vfunc 36, sub_82386FF8) call TryQuickStep (sub_82386E60) when Sonic
// is on the ground. It quick-steps if LB or RB was just pressed, unless the stomp's blocker above is set or this
// byte is (sub_82316208). Above a speed (parameter 240) it's a CStateRunQuickStep, below a CStateQuickStep.
static constexpr uint32_t QUICK_STEP_DISABLED_OFFSET = 0x4B4;
// The quick steps only run the ground actions, and so allow the next quick step, from a frame of their animation on:
// their updates (sub_823968D8 and sub_82397370) compare the animation frame (sub_82307D38) with these.
static constexpr float QUICK_STEP_ACTIONS_FRAME = 20.0f;
static constexpr float RUN_QUICK_STEP_ACTIONS_FRAME = 8.0f;
// How many frames the run quick step's out-of-control lasts after its animation reaches that frame.
static constexpr int RUN_QUICK_STEP_OUT_OF_CONTROL_FRAMES = 1;
// Hedgehog engine states: the context at +8, the time spent in the state, in seconds, at +0x10.
static constexpr uint32_t STATE_CONTEXT_OFFSET = 0x8;
static constexpr uint32_t STATE_TIME_OFFSET = 0x10;
// The parameter table in use (boost::shared_ptr, set by sub_8230B630). Each movement mode switches to its own table,
// kept in a global shared_ptr. Sonic's parameter loader (sub_8237A828) fills them from the parameter groups named
// below: free 3D is "ForwardView", a 2D section switches to "SideView" (sub_82320B70) and back, and the forward
// sections, with quick steps, to "DashMode" (sub_82377D00), then "QSM" (sub_82378500). The others are switched to by
// the context's update (sub_823746C0) while CPlayerSpeedPosturePluginOnWater or CSonicPosturePluginWallCorrection
// (Rooftop Run's wall run) is on, and by MsgStartSonicSpecialMode, which the Egg Beetle, the Egg Ray Bird and the
// E-BigChaser send (sub_82365BE8, an index of 0 to 3 into the "Special" tables).
static constexpr uint32_t PARAMETERS_OFFSET = 0x1F4;

struct MovementMode
{
    uint32_t Parameters;
    const char* Name;
    bool SideView = false;
};

static constexpr MovementMode MOVEMENT_MODES[] =
{
    { 0x83362FA4, "3D" },
    { 0x83362FAC, "2D", true },
    { 0x83364750, "FORWARD" },
    { 0x83364758, "FORWARD" },
    { 0x83362FB4, "ON WATER" },
    { 0x83364728, "WALL RUN" },
    { 0x83362FCC, "ON ICE" },
    { 0x83362FD4, "ON ICE 2D", true },
    { 0x83364760, "SPECIAL 0" },
    { 0x83364768, "SPECIAL 1" },
    { 0x83364770, "SPECIAL 2" },
    { 0x83364778, "SPECIAL 3" },
};

// The Werehog's player context (SWA::Player::CEvilSonicContext). Both contexts are built on the same base
// (sub_8230D620), so the player and its states are found the same way, but the Werehog keeps his velocity
// elsewhere: a single vector, in world space. Found by comparing per-frame dumps of the context with how he
// moves: it's how fast the point under him (+0x580) and his height (+0x560) change.
static constexpr uint32_t EVIL_SONIC_CONTEXT_VFTABLE = 0x8201DD6C;
static constexpr uint32_t EVIL_VELOCITY_OFFSET = 0x900;
// Whether he's on the ground: what his IsOnGround (sub_823A4080) returns. It stays set until a jump takes him
// about 0.1 up, and is cleared while an attack lifts him off the ground.
static constexpr uint32_t EVIL_GROUNDED_OFFSET = 0xAE1;
// His helpers: an std::map<uint16_t, boost::shared_ptr<...>> (sub_82BB93D0 looks them up). Helper 2 handles
// attacks (SWA::Player::CEvilAttackAction); its current action (sub_82DE0870) is a node of the attack list read
// from EvilAttackAction*.xml (sub_824241D0), where +16 is its ActionName, +20 its MotionName and +148 its Guard
// flag: whether a guard can cancel it.
static constexpr uint32_t EVIL_HELPERS_OFFSET = 0x3BC;
static constexpr uint16_t EVIL_ATTACK_HELPER = 2;
static constexpr uint32_t ATTACK_HELPER_LEVEL_OFFSET = 8;
static constexpr uint32_t ATTACK_HELPER_ACTION_OFFSET = 20;
static constexpr uint32_t ATTACK_MOTION_MAPS = 0x833655D8;
static constexpr uint32_t ACTION_NAME_OFFSET = 16;
static constexpr uint32_t ACTION_MOTION_NAME_OFFSET = 20;
static constexpr uint32_t ACTION_KEY_END_OFFSET = 76;
static constexpr uint32_t ACTION_GUARD_OFFSET = 148;
// The motion's EndSkipTimeWhenPad (sub_82424A18).
static constexpr uint32_t MOTION_END_SKIP_TIME_OFFSET = 268;
// The player's animation state machine: CEvilSonic's interface at +0xC0 returns it from its +380 (sub_82303030).
static constexpr uint32_t PLAYER_ANIMATION_STATE_MACHINE_OFFSET = 0xC0 + 380;
// An animation state (SWA::CAnimationControlSingle): its Havok control (hkaDefaultAnimationControl) at +4, with the
// local time at +8, the playback speed at +0x40 and the animation binding at +0x28, whose animation (+0) has its
// duration at +12 (the state's vfunc 2, sub_82BC0870). The state's start time is at +20 (sub_82BBD030).
static constexpr uint32_t ANIMATION_CONTROL_SINGLE_VFTABLE = 0x820CB3DC;
static constexpr uint32_t ANIMATION_CONTROL_OFFSET = 4;
static constexpr uint32_t ANIMATION_START_TIME_OFFSET = 20;
static constexpr uint32_t ANIMATION_CONTROL_TIME_OFFSET = 8;
static constexpr uint32_t ANIMATION_CONTROL_SPEED_OFFSET = 0x40;
static constexpr uint32_t ANIMATION_CONTROL_BINDING_OFFSET = 0x28;
static constexpr uint32_t ANIMATION_DURATION_OFFSET = 12;

// The game document (SWA::CGameDocument::GetInstance), whose member keeps the stage time at +0x5C: it stops at the
// goal, and counts up from below 0 during a stage's intro. (+0x60 keeps going after the goal.)
static constexpr uint32_t GAME_DOCUMENT = 0x83367900;
static constexpr uint32_t GAME_DOCUMENT_MEMBER_OFFSET = 8;
static constexpr uint32_t STAGE_TIME_OFFSET = 0x5C;

static uint32_t g_speedContext;
static uint32_t g_evilContext;
// Which of the two was made last, for when both are around.
static bool g_evilContextIsNewer;
static int16_t g_thumbLX;
static int16_t g_thumbLY;

// Counts drawn frames, to tell whether the checks below ran during the update of the frame being drawn.
static uint32_t g_frame = 1;
static uint32_t g_jumpCheckFrame;
static uint32_t g_stompCheckFrame;
static bool g_stompDisabledByHop;
static bool g_stompBlocked;
static float g_stompingDisableTime = 0.15f;
static float g_movementInputDeadzone = 0.02f;
static uint32_t g_quickStepCheckFrame;
// Sonic's state on the last drawn frame.
static std::string g_sonicPreviousState;
static bool g_quickStepDisabled;
static bool g_quickStepBlocked;

// The animation frame a quick step's update compared on the last frame, with the animation and its local time then.
static bool g_recordQuickStepFrame;
static bool g_recordQuickStepAnimation;
static uint32_t g_quickStepUpdateFrame;
static float g_quickStepAnimationFrame;
static uint32_t g_quickStepAnimation;
static float g_quickStepLocalTime;
static float g_quickStepPreviousLocalTime;

// The Werehog's state and attack action on the last drawn frame, and the frame they started on.
static uint32_t g_evilState;
static uint32_t g_evilAction;
static float g_evilStateTime;
static uint32_t g_evilStateStartFrame;
// The attack animation and its local time on the last drawn frame.
static uint32_t g_evilAnimation;
static float g_evilAnimationTime;

// SWA::Player::CPlayerSpeedContext::CPlayerSpeedContext (the base of the day Sonic context)
PPC_FUNC_IMPL(__imp__sub_82330188);
PPC_FUNC(sub_82330188)
{
    uint32_t context = ctx.r3.u32;
    __imp__sub_82330188(ctx, base);

    if (TasHud::IsEnabled())
    {
        g_speedContext = context;
        g_evilContextIsNewer = false;
    }
}

void TasHud::OnEvilSonicContext(uint32_t context)
{
    if (IsEnabled())
    {
        g_evilContext = context;
        g_evilContextIsNewer = true;
    }
}

static bool IsReadable(uint32_t address)
{
    // Everything but the first page of guest memory can be read.
    return address >= 0x1000 && address < 0xFFFF0000;
}

static uint32_t LoadU32(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U32(address) : 0;
}

static uint8_t LoadU8(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U8(address) : 0;
}

static uint16_t LoadU16(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U16(address) : 0;
}

static float LoadFloat(uint32_t address)
{
    uint32_t value = LoadU32(address);
    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
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
static uint32_t GetPlayerContext(bool& werehog)
{
    bool sonic = IsSonicContext(g_speedContext);
    werehog = IsEvilSonicContext(g_evilContext) && (!sonic || g_evilContextIsNewer);
    return werehog ? g_evilContext : sonic ? g_speedContext : 0;
}

// A CSharedString's text.
static std::string GetSharedString(uint32_t address)
{
    uint32_t text = LoadU32(address);
    if (!IsReadable(text))
        return {};

    auto chars = reinterpret_cast<const char*>(g_memory.Translate(text));
    return std::string(chars, strnlen(chars, 64));
}

// Looks up an std::map node like std::map::find, without inserting (the game's lookups insert missing keys).
// MSVC's tree: the head node at +4 of the map, whose parent (+4) is the root; nodes have their left child at +0,
// right child at +8, key at +12 and value at +16, then the colour and a byte set in the head: at +25 after an
// 8-byte value (like a shared_ptr), at +21 after a 2-byte one. Returns the node, or 0.
template<typename Less, typename Equal>
static uint32_t FindMapNode(uint32_t map, uint32_t headFlagOffset, Less less, Equal equal)
{
    uint32_t head = LoadU32(map + 4);
    if (head == 0)
        return 0;

    uint32_t found = head;
    uint32_t node = LoadU32(head + 4);
    for (int depth = 0; depth < 64 && node != 0 && LoadU8(node + headFlagOffset) == 0; depth++)
    {
        if (less(node + 12))
        {
            node = LoadU32(node + 8);
        }
        else
        {
            found = node;
            node = LoadU32(node);
        }
    }

    return found != head && equal(found + 12) ? found : 0;
}

static uint32_t FindMapNode(uint32_t map, uint16_t key, uint32_t headFlagOffset = 25)
{
    return FindMapNode(map, headFlagOffset, [&](uint32_t k) { return LoadU16(k) < key; }, [&](uint32_t k) { return LoadU16(k) == key; });
}

static uint32_t FindMapNode(uint32_t map, const std::string& key, uint32_t headFlagOffset = 25)
{
    return FindMapNode(map, headFlagOffset, [&](uint32_t k) { return GetSharedString(k) < key; },
        [&](uint32_t k) { return GetSharedString(k) == key; });
}

// The Werehog's attack helper, or 0.
static uint32_t GetEvilAttackHelper(uint32_t context)
{
    uint32_t node = FindMapNode(context + EVIL_HELPERS_OFFSET, EVIL_ATTACK_HELPER);
    return node != 0 ? LoadU32(node + 16) : 0;
}

// The motion an attack action plays, or 0: the motions read from EvilAttackMotionFile.xml are in an
// std::map<CSharedString, boost::shared_ptr<...>> per level of the attack list (0x833655D8 + 12 * the helper's level
// at +8), by the action's MotionName at +20 (sub_82423B90). The motion's animation file name is at +0.
static uint32_t GetAttackMotion(uint32_t helper, uint32_t action)
{
    uint32_t node = FindMapNode(ATTACK_MOTION_MAPS + 12 * LoadU32(helper + ATTACK_HELPER_LEVEL_OFFSET),
        GetSharedString(action + ACTION_MOTION_NAME_OFFSET));
    return node != 0 ? LoadU32(node + 16) : 0;
}

// The player's animation state for an animation, or 0 (sub_82BB97E8): the player's animation state machine
// (sub_82307C70) maps names to IDs at +20 and IDs to states at +8.
static uint32_t GetAnimationState(uint32_t context, const std::string& name)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    uint32_t stateMachine = player != 0 ? LoadU32(player + PLAYER_ANIMATION_STATE_MACHINE_OFFSET) : 0;
    if (stateMachine == 0)
        return 0;

    uint32_t idNode = FindMapNode(stateMachine + 20, name, 21);
    uint32_t stateNode = idNode != 0 ? FindMapNode(stateMachine + 8, LoadU16(idNode + 16)) : 0;
    return stateNode != 0 ? LoadU32(stateNode + 16) : 0;
}

// Sonic's movement mode, from the parameter table he uses, or null for another one.
static const MovementMode* GetMovementMode(uint32_t context)
{
    uint32_t parameters = LoadU32(context + PARAMETERS_OFFSET);
    for (auto& mode : MOVEMENT_MODES)
    {
        if (parameters != 0 && parameters == LoadU32(mode.Parameters))
            return &mode;
    }

    return nullptr;
}

// During a quick step, in how many frames its update will find the animation at the frame it runs the ground actions
// from, and so allow the next quick step, or -1. The animation's local time is stepped like Havok steps it: by the
// frame's time times the playback speed, in single precision (or by how much it moved on the last frame, if that
// didn't match). A run quick step also puts Sonic out of control (CPlayerSpeedPosturePluginOutOfControl, flag 3, which
// makes the ground actions do nothing) until its animation reaches that frame (sub_8238D578): that lasts through the
// update that gets there, so the next quick step comes a frame later (RooftopRunAct1.ltm, frames 2446-2458).
static int GetQuickStepFramesLeft(float actionsFrame, int outOfControlFrames, float deltaTime)
{
    uint32_t animation = g_quickStepAnimation;
    if (g_quickStepUpdateFrame != g_frame || LoadU32(animation) != ANIMATION_CONTROL_SINGLE_VFTABLE)
        return -1;

    if (g_quickStepAnimationFrame >= actionsFrame)
        return outOfControlFrames > 0 ? outOfControlFrames : -1;

    uint32_t control = LoadU32(animation + ANIMATION_CONTROL_OFFSET);
    float start = LoadFloat(animation + ANIMATION_START_TIME_OFFSET);
    float duration = LoadFloat(LoadU32(LoadU32(control + ANIMATION_CONTROL_BINDING_OFFSET)) + ANIMATION_DURATION_OFFSET);
    float elapsed = g_quickStepLocalTime - start;
    if (!(elapsed > 0.0f) || !(duration > 0.0f))
        return -1;

    // The animation's frame count - 1, from the frame the update got.
    float frames = std::round(g_quickStepAnimationFrame * duration / elapsed);
    float rate = frames / duration;

    float step = deltaTime * LoadFloat(control + ANIMATION_CONTROL_SPEED_OFFSET);
    float previous = g_quickStepPreviousLocalTime;
    if (!std::isnan(previous))
    {
        float expected = previous + step;
        if (expected != g_quickStepLocalTime)
            step = g_quickStepLocalTime - previous;
    }

    // The animation may have moved on since the update.
    float time = g_quickStepLocalTime;
    float drawnTime = LoadFloat(control + ANIMATION_CONTROL_TIME_OFFSET);
    for (int framesLeft = 1; framesLeft < 100; framesLeft++)
    {
        if (framesLeft == 1 && drawnTime != g_quickStepLocalTime)
            time = drawnTime;
        else
            time += step;

        float sinceStart = time - start;
        float frame = sinceStart * rate;
        if (frame >= actionsFrame)
            return framesLeft + outOfControlFrames;
    }

    return -1;
}

static bool GetFlag(uint32_t context, uint32_t flagsOffset, uint32_t flag)
{
    uint32_t flags = LoadU32(context + flagsOffset);
    return flags != 0 && LoadU8(LoadU32(flags + 4) + flag) != 0;
}

// The context's TryJump: jumps if the jump button is pressed. The states that allow jumping call it every
// frame; others, like a quick step until it ends, don't.
PPC_FUNC_IMPL(__imp__sub_82330CB0);
PPC_FUNC(sub_82330CB0)
{
    if (TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext)
        g_jumpCheckFrame = g_frame;

    __imp__sub_82330CB0(ctx, base);
}

// The blocker that refuses stomps and quick steps (sub_8231B7D8).
static bool IsActionBlocked(uint32_t context)
{
    return LoadU32(context + STOMP_BLOCKER_OFFSET) != 0 && !GetFlag(context, STATE_FLAGS_2_OFFSET, STOMP_BLOCKER_OVERRIDE_FLAG);
}

// The context's TryStomp, called by the air states when no action tried before it (like a homing attack) was taken.
PPC_FUNC_IMPL(__imp__sub_823765C0);
PPC_FUNC(sub_823765C0)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_stompCheckFrame = g_frame;
        g_stompDisabledByHop = GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG);
        g_stompBlocked = IsActionBlocked(context);
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
        g_quickStepCheckFrame = g_frame;
        g_quickStepDisabled = LoadU8(context + QUICK_STEP_DISABLED_OFFSET) != 0;
        g_quickStepBlocked = IsActionBlocked(context);
    }

    __imp__sub_82386E60(ctx, base);
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
        g_quickStepAnimation = 0;
    }

    __imp__sub_82307D38(ctx, base);

    if (record)
    {
        g_recordQuickStepAnimation = false;
        g_quickStepPreviousLocalTime = g_quickStepUpdateFrame + 1 == g_frame ? g_quickStepLocalTime : NAN;
        g_quickStepLocalTime = LoadFloat(LoadU32(g_quickStepAnimation + ANIMATION_CONTROL_OFFSET) + ANIMATION_CONTROL_TIME_OFFSET);
        g_quickStepAnimationFrame = float(ctx.f1.f64);
        g_quickStepUpdateFrame = g_frame;
    }
}

// The animation state behind sub_82307D38.
PPC_FUNC_IMPL(__imp__sub_82BBD568);
PPC_FUNC(sub_82BBD568)
{
    if (g_recordQuickStepAnimation)
    {
        g_quickStepAnimation = ctx.r3.u32;
        g_recordQuickStepAnimation = false;
    }

    __imp__sub_82BBD568(ctx, base);
}

// Reads a float parameter of the player by ID. Records StompingDisableTime when the short hop reads it, and the
// movement input's deadzone when the input is read.
PPC_FUNC_IMPL(__imp__sub_8245DB60);
PPC_FUNC(sub_8245DB60)
{
    uint32_t parameter = ctx.r4.u32;
    __imp__sub_8245DB60(ctx, base);

    if (parameter == STOMPING_DISABLE_TIME_PARAMETER && TasHud::IsEnabled())
        g_stompingDisableTime = float(ctx.f1.f64);

    if (parameter == MOVEMENT_INPUT_DEADZONE_PARAMETER && TasHud::IsEnabled())
        g_movementInputDeadzone = float(ctx.f1.f64);
}

// The current state of the player's state machine, or 0. The machine points to the current state's record
// at +0x58 (compared with the new state in sub_82E669D8), which points to the state at +0x1C.
static uint32_t GetCurrentState(uint32_t context)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    if (player == 0)
        return 0;

    uint32_t stateMachine = player + PLAYER_STATE_MACHINE_OFFSET;
    uint32_t record = LoadU32(stateMachine + 0x58);
    uint32_t state = LoadU32(record + 0x1C);
    return state != 0 && LoadU32(state + STATE_CONTEXT_OFFSET) == context ? state : 0;
}

// The name of the state's class, from the game's RTTI: "CStateJumpShort@CSonicContext@..." becomes "JumpShort".
static const std::string& GetStateName(uint32_t state)
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
    if (vtable >= PPC_IMAGE_BASE && vtable < PPC_IMAGE_BASE + PPC_IMAGE_SIZE &&
        typeDescriptor >= PPC_IMAGE_BASE && typeDescriptor < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
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

static float VectorLength(uint32_t address, float& y)
{
    float x = LoadFloat(address);
    float z = LoadFloat(address + 8);
    y = LoadFloat(address + 4);
    return std::sqrt(x * x + y * y + z * z);
}

// The D-Speed target zones of the M-/D-Speed Visualiser, on the stick as SDL reports it: -1..1, Y pointing
// down, angles as atan2(y, x). Its per-axis deadzone applies before checking them.
struct TargetZone
{
    const char* Name;
    float StartAngle;
    float EndAngle;
    float InnerRadius;
    float OuterRadius;
};

static constexpr float PI = std::numbers::pi_v<float>;
static constexpr float STICK_DEADZONE = 0.05f;

static constexpr TargetZone TARGET_ZONES[] =
{
    { "D-Speed", -PI / 4, PI / 4, 0.29f, 0.50f },        // Right
    { "D-Speed", 3 * PI / 4, 5 * PI / 4, 0.29f, 0.50f }, // Left
};

// M-Speed, from the 3D air update (sub_82346880). Leaving the ground sets flag 27 (the context's vfunc 25,
// sub_82374360; landing clears it), and the update clears it as soon as the movement input is longer than 0.3; while
// it's set, the air deceleration is 0. The update aims for the input's part along Sonic's facing times
// max(InAirMaxAccelVelocity, his speed) (sub_8234C5F8). A short input puts that below his speed, so the update
// decelerates him, by 0, and then pushes him along his velocity instead, by a force that grows with the square of his
// speed: he gains speed every frame. The 3D input (CPlayerSpeedPostureInput3DStandard, sub_8234EE88; PathLocal,
// sub_8234F4D0, does the same) is the stick's direction times (length / 0.9)³, at most 1, and is ignored below
// parameter 12 (0.02, sub_823083F0). So on the stick as the game reads it, M-Speed takes a length from about 0.244 to
// 0.602, in any direction. 2D reads the stick without the curve.
static constexpr float MOVEMENT_INPUT_SCALE = 1.0f / 0.9f;
static constexpr float M_SPEED_MAX_INPUT = 0.3f;

// The 3D movement input's length for a stick length.
static float GetMovementInput(float stickLength)
{
    if (!(stickLength > 0.01f))
        return 0.0f;

    return std::min(float(std::pow(double(stickLength * MOVEMENT_INPUT_SCALE), 3.0)), 1.0f);
}

static float GetStickLengthForMovementInput(float input)
{
    return std::cbrt(input) / MOVEMENT_INPUT_SCALE;
}

static bool IsMSpeedInput(float stickLength)
{
    float input = GetMovementInput(stickLength);
    return input >= g_movementInputDeadzone && input <= M_SPEED_MAX_INPUT;
}

static bool IsInZone(const TargetZone& zone, float x, float y)
{
    float radius = std::hypot(x, y);
    if (radius < zone.InnerRadius || radius > zone.OuterRadius)
        return false;

    // Angle from the start of the zone, in [0, 2π), so zones that cross angle 0 work too.
    float angle = std::fmod(std::atan2(y, x) - zone.StartAngle + 4 * PI, 2 * PI);
    return angle <= zone.EndAngle - zone.StartAngle;
}

static float ApplyDeadzone(float value)
{
    return std::abs(value) < STICK_DEADZONE ? 0.0f : value;
}

// How the game reads the stick, fitted to the values it produces (within 0.001): each axis loses a
// deadzone and is scaled back to 0..1, then the vector is shortened to length 1 if it's longer.
static constexpr float GAME_STICK_DEADZONE = 0.2651f;

// The break-jump glitch: holding the stick through a switch to 2D and a jump, where the game reads it as at
// least 0.1 long (2D movement ignores shorter input) but shorter than 0.3 (NoPadStopWalkPadLengthLimit,
// parameter 140), makes Sonic's velocity explode. Found by replaying a movie with the stick at different
// positions, then matching the limits with the game's values.
static constexpr float BREAK_JUMP_MIN_LENGTH = 0.1f;
static constexpr float BREAK_JUMP_MAX_LENGTH = 0.3f;

// The raw stick positions the game reads as a given length form a square with rounded corners: the
// deadzone cross in the middle, then arcs of radius length * (1 - deadzone) around (±deadzone, ±deadzone).
// The zone is the ring between the two lengths.
static void DrawLengthZone(ImDrawList* drawList, ImVec2 centre, float scale, float minLength, float maxLength, ImU32 fill, ImU32 outline)
{
    constexpr int ARC_SEGMENTS = 8;
    constexpr int POINTS = 4 * (ARC_SEGMENTS + 1);

    ImVec2 inner[POINTS];
    ImVec2 outer[POINTS];
    int count = 0;

    for (int quadrant = 0; quadrant < 4; quadrant++)
    {
        float cornerX = (quadrant == 0 || quadrant == 3) ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cornerY = quadrant < 2 ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;

        for (int i = 0; i <= ARC_SEGMENTS; i++, count++)
        {
            float angle = (quadrant + float(i) / ARC_SEGMENTS) * PI / 2;
            float cosine = std::cos(angle) * (1.0f - GAME_STICK_DEADZONE);
            float sine = std::sin(angle) * (1.0f - GAME_STICK_DEADZONE);

            inner[count] = { centre.x + (cornerX + cosine * minLength) * scale, centre.y + (cornerY + sine * minLength) * scale };
            outer[count] = { centre.x + (cornerX + cosine * maxLength) * scale, centre.y + (cornerY + sine * maxLength) * scale };
        }
    }

    // The ring isn't convex, so fill it one quad at a time; the gaps between quadrants are the straight sides.
    for (int i = 0; i < POINTS; i++)
    {
        int next = (i + 1) % POINTS;
        drawList->AddQuadFilled(outer[i], outer[next], inner[next], inner[i], fill);
    }

    drawList->AddPolyline(outer, POINTS, outline, ImDrawFlags_Closed, Scale(1));
    drawList->AddPolyline(inner, POINTS, outline, ImDrawFlags_Closed, Scale(1));
}

bool TasHud::IsEnabled()
{
    static const bool s_enabled = []()
    {
        const char* value = std::getenv("UNLEASHED_TAS_HUD");
        return IsTasMode() && value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();

    return s_enabled;
}

void TasHud::OnInputState(int16_t thumbLX, int16_t thumbLY)
{
    g_thumbLX = thumbLX;
    g_thumbLY = thumbLY;
}

static void DrawText(ImDrawList* drawList, ImFont* font, float size, ImVec2 pos, ImU32 colour, const std::string& text)
{
    drawList->AddText(font, size, { pos.x + Scale(1), pos.y + Scale(1) }, IM_COL32(0, 0, 0, 200), text.c_str());
    drawList->AddText(font, size, pos, colour, text.c_str());
}

static float TextWidth(ImFont* font, float size, const std::string& text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}

// A labelled box, lit when active: filled for what Sonic is doing, outlined for what a press would do.
static void DrawIndicator(ImDrawList* drawList, ImFont* font, ImVec2 pos, float width, const char* label, ImU32 colour, bool lit, bool outlined = false)
{
    float size = Scale(11);
    float textWidth = TextWidth(font, size, label);
    ImVec2 max = { pos.x + width, pos.y + Scale(16) };
    ImU32 textColour = IM_COL32(170, 170, 170, 255);

    if (outlined)
    {
        drawList->AddRectFilled(pos, max, IM_COL32(20, 20, 20, 200), Scale(3));
        drawList->AddRect(pos, max, lit ? colour : IM_COL32(80, 80, 80, 255), Scale(3), 0, Scale(1.5f));

        if (lit)
            textColour = colour;
    }
    else
    {
        drawList->AddRectFilled(pos, max, lit ? colour : IM_COL32(20, 20, 20, 200), Scale(3));

        if (lit)
            textColour = IM_COL32(0, 0, 0, 255);
    }

    drawList->AddText(font, size, { pos.x + (width - textWidth) / 2, pos.y + Scale(2) }, textColour, label);
}

static void DrawZone(ImDrawList* drawList, ImVec2 centre, float scale, const TargetZone& zone, bool active)
{
    constexpr int SEGMENTS = 20;

    ImU32 fill = active ? IM_COL32(0, 255, 0, 200) : IM_COL32(0, 255, 0, 80);
    ImU32 outline = active ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 160);

    ImVec2 outer[SEGMENTS + 1];
    ImVec2 inner[SEGMENTS + 1];

    for (int i = 0; i <= SEGMENTS; i++)
    {
        float angle = zone.StartAngle + (zone.EndAngle - zone.StartAngle) * i / SEGMENTS;
        outer[i] = { centre.x + std::cos(angle) * zone.OuterRadius * scale, centre.y + std::sin(angle) * zone.OuterRadius * scale };
        inner[i] = { centre.x + std::cos(angle) * zone.InnerRadius * scale, centre.y + std::sin(angle) * zone.InnerRadius * scale };
    }

    // The band isn't convex, so fill it one quad at a time.
    for (int i = 0; i < SEGMENTS; i++)
        drawList->AddQuadFilled(outer[i], outer[i + 1], inner[i + 1], inner[i], fill);

    drawList->AddPolyline(outer, SEGMENTS + 1, outline, 0, Scale(1));
    drawList->AddPolyline(inner, SEGMENTS + 1, outline, 0, Scale(1));
    drawList->AddLine(outer[0], inner[0], outline, Scale(1));
    drawList->AddLine(outer[SEGMENTS], inner[SEGMENTS], outline, Scale(1));
}

static constexpr float CONTROLLER_HEIGHT = 46;

// The buttons used in play, as the game read them this frame: the triggers (filled as far as they're
// pulled), bumpers and Start on the left, the face buttons on the right. Held buttons are lit; ones pressed
// on this frame, which is what jumps and stomps need, also get a white ring.
static void DrawController(ImDrawList* drawList, ImFont* font, ImVec2 pos, float width, const SWA::SPadState* pad)
{
    uint32_t down = pad != nullptr ? uint32_t(pad->DownState) : 0;
    uint32_t tapped = pad != nullptr ? uint32_t(pad->TappedState) : 0;
    float size = Scale(12);
    ImU32 ring = IM_COL32(255, 255, 255, 255);
    ImU32 unlitFill = IM_COL32(20, 20, 20, 200);

    auto drawLabel = [&](ImVec2 centre, const char* label, ImU32 colour)
    {
        ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
        drawList->AddText(font, size, { centre.x - textSize.x / 2, centre.y - textSize.y / 2 }, colour, label);
    };

    auto drawBox = [&](ImVec2 min, ImVec2 max, const char* label, uint32_t key, float fill = -1.0f)
    {
        bool lit = (down & key) != 0;
        drawList->AddRectFilled(min, max, unlitFill, Scale(3));

        // Triggers are analog: fill them as far as they're pulled.
        float amount = fill >= 0.0f ? std::clamp(fill, 0.0f, 1.0f) : (lit ? 1.0f : 0.0f);
        if (amount > 0.0f)
            drawList->AddRectFilled(min, { min.x + (max.x - min.x) * amount, max.y }, IM_COL32(220, 220, 220, 230), Scale(3));

        drawList->AddRect(min, max, (tapped & key) ? ring : IM_COL32(110, 110, 110, 255), Scale(3), 0, (tapped & key) ? Scale(2) : Scale(1));
        drawLabel({ (min.x + max.x) / 2, (min.y + max.y) / 2 }, label, amount > 0.5f ? IM_COL32(0, 0, 0, 255) : IM_COL32(200, 200, 200, 255));
    };

    auto drawButton = [&](ImVec2 centre, float radius, const char* label, uint32_t key, ImU32 colour)
    {
        bool lit = (down & key) != 0;
        drawList->AddCircleFilled(centre, radius, lit ? colour : unlitFill, 16);
        drawList->AddCircle(centre, radius, (tapped & key) ? ring : colour, 16, (tapped & key) ? Scale(2) : Scale(1.2f));
        drawLabel(centre, label, lit ? IM_COL32(0, 0, 0, 255) : colour);
    };

    // Triggers and bumpers in their order on the controller, Start under them.
    float middle = pos.y + CONTROLLER_HEIGHT * Scale(1) / 2;
    float rowHeight = Scale(18);
    float rowGap = Scale(4);
    float top = middle - rowHeight - rowGap / 2;
    float gap = Scale(4);
    float slot = (width * 0.6f - 3 * gap) / 4;
    auto slotX = [&](int i) { return pos.x + i * (slot + gap); };

    drawBox({ slotX(0), top }, { slotX(0) + slot, top + rowHeight }, "LT", SWA::eKeyState_LeftTrigger, pad != nullptr ? float(pad->LeftTrigger) : 0.0f);
    drawBox({ slotX(1), top }, { slotX(1) + slot, top + rowHeight }, "LB", SWA::eKeyState_LeftBumper);
    drawBox({ slotX(2), top }, { slotX(2) + slot, top + rowHeight }, "RB", SWA::eKeyState_RightBumper);
    drawBox({ slotX(3), top }, { slotX(3) + slot, top + rowHeight }, "RT", SWA::eKeyState_RightTrigger, pad != nullptr ? float(pad->RightTrigger) : 0.0f);

    float startTop = middle + rowGap / 2;
    drawBox({ slotX(1), startTop }, { slotX(2) + slot, startTop + rowHeight }, "START", SWA::eKeyState_Start);

    // Face buttons.
    ImVec2 face = { pos.x + width * 0.8f, middle };
    float spread = Scale(12);
    float buttonRadius = Scale(10);
    drawButton({ face.x, face.y - spread }, buttonRadius, "Y", SWA::eKeyState_Y, IM_COL32(255, 200, 40, 255));
    drawButton({ face.x - spread * 1.6f, face.y }, buttonRadius, "X", SWA::eKeyState_X, IM_COL32(60, 140, 255, 255));
    drawButton({ face.x + spread * 1.6f, face.y }, buttonRadius, "B", SWA::eKeyState_B, IM_COL32(255, 70, 60, 255));
    drawButton({ face.x, face.y + spread }, buttonRadius, "A", SWA::eKeyState_A, IM_COL32(80, 220, 80, 255));
}

void TasHud::Draw()
{
    if (!IsEnabled())
        return;

    // Only in levels, where Sonic or the Werehog is around.
    bool werehog;
    uint32_t context = GetPlayerContext(werehog);
    if (context == 0)
    {
        g_frame++;
        return;
    }

    auto drawList = ImGui::GetBackgroundDrawList();
    auto& res = ImGui::GetIO().DisplaySize;
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");

    // Wide enough for the longest stick line and three indicators a row as wide as the widest, away from
    // the edge of the screen.
    float radius = Scale(70);
    float padding = Scale(8);
    float gap = Scale(4);
    float indicatorText = std::max({ TextWidth(font, Scale(11), "CAN STOMP"), TextWidth(font, Scale(11), "ATTACKING"),
        TextWidth(font, Scale(11), "CAN GUARD") });
    float width = std::max({ 2 * radius + 2 * padding,
        TextWidth(font, Scale(11), "X -1.00 Y -1.00  (-32768, -32768)") + 2 * padding,
        3 * (indicatorText + Scale(10)) + 2 * gap + 2 * padding });
    ImVec2 panelMax = { res.x - Scale(40), res.y - Scale(22) };
    ImVec2 centre = { panelMax.x - width / 2, panelMax.y - Scale(58 + CONTROLLER_HEIGHT) - radius };
    ImVec2 panelMin = { panelMax.x - width, centre.y - radius - Scale(122) };
    float left = panelMin.x + padding;

    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 140), Scale(6));

    // Speedometer: the speed along the ground, the full speed, and the vertical part, along the up vector
    // or against it.
    float speed, total, vertical;
    if (werehog)
    {
        // Split along the world's up, which is the Werehog's (his up vector stayed (0, 1, 0) everywhere).
        total = VectorLength(context + EVIL_VELOCITY_OFFSET, vertical);
        speed = std::sqrt(std::max(total * total - vertical * vertical, 0.0f));
    }
    else
    {
        float y;
        speed = VectorLength(context + HORIZONTAL_VELOCITY_OFFSET, y);
        total = VectorLength(context + VELOCITY_OFFSET, y);
        vertical = VectorLength(context + VERTICAL_VELOCITY_OFFSET, y);
        if (y < 0)
            vertical = -vertical;
    }

    DrawText(drawList, font, Scale(18), { left, panelMin.y + Scale(6) }, IM_COL32_WHITE, fmt::format("Speed {:.2f}", speed));
    DrawText(drawList, font, Scale(11), { left, panelMin.y + Scale(28) }, IM_COL32(220, 220, 220, 255),
        fmt::format("Total {:.2f}   Vertical {:.2f}", total, vertical));

    // What the player is doing. For Sonic, then what a press on this frame would have done: the game checked
    // for the jump or stomp button during this frame's update. Both need a new press, holding the button
    // doesn't count.
    float row = panelMin.y + Scale(46);
    float pressRow = row + Scale(20);
    // Sonic has a second row of presses, for the quick step; the Werehog has a row under his state instead, for when
    // his attack ends.
    float quickStepRow = pressRow + Scale(20);
    float stateRow = (werehog ? pressRow : quickStepRow) + Scale(20);
    float attackRow = stateRow + Scale(20);

    uint32_t state = GetCurrentState(context);
    std::string stateName = state != 0 ? GetStateName(state) : "?";
    float stateTime = state != 0 ? LoadFloat(state + STATE_TIME_OFFSET) : 0.0f;
    float deltaTime = std::max(float(App::s_deltaTime), 0.001f);

    float indicatorWidth = (width - 2 * padding - 2 * gap) / 3;
    float column2 = left + indicatorWidth + gap;
    float column3 = column2 + indicatorWidth + gap;

    bool grounded = LoadU8(context + (werehog ? EVIL_GROUNDED_OFFSET : GROUNDED_OFFSET)) != 0;
    DrawIndicator(drawList, font, { left, row }, indicatorWidth, grounded ? "GROUND" : "AIR", grounded ? IM_COL32(80, 220, 80, 230) : IM_COL32(90, 170, 255, 230), true);

    std::string stateLabel = stateName;
    int stateFrames = int(std::lround(stateTime / deltaTime));

    if (werehog)
    {
        bool dashing = stateName == "Dash";
        bool attacking = stateName.starts_with("AttackAction") || stateName == "SuperAttack";
        uint32_t helper = stateName == "AttackAction_byList" ? GetEvilAttackHelper(context) : 0;
        uint32_t action = helper != 0 ? LoadU32(helper + ATTACK_HELPER_ACTION_OFFSET) : 0;

        // When the attack's action ends on its own. Its animation advances by its playback speed times the game's
        // speed, which some attacks lower for a while: the counts go by the last frame's rate, so they drop when such
        // a slowdown ends. Matched against every attack that ended without a button press in two movies:
        // - the action ends on the frame its animation's time gets within half a frame of the animation's end;
        // - on the ground with the stick held, already on the frame after the time passes the end minus the
        //   motion's EndSkipTimeWhenPad.
        // The action then either leads to the one named by its KEY__End (like the parts of a jumping slash), or
        // lets go of the Werehog.
        int framesLeft = -1;
        int framesLeftWithStick = -1;
        std::string nextAction;
        uint32_t motion = action != 0 ? GetAttackMotion(helper, action) : 0;
        uint32_t animation = motion != 0 ? GetAnimationState(context, GetSharedString(motion)) : 0;
        if (animation != 0)
        {
            uint32_t control = LoadU32(animation + ANIMATION_CONTROL_OFFSET);
            float duration = LoadFloat(LoadU32(LoadU32(control + ANIMATION_CONTROL_BINDING_OFFSET)) + ANIMATION_DURATION_OFFSET);
            float time = LoadFloat(control + ANIMATION_CONTROL_TIME_OFFSET) - LoadFloat(animation + ANIMATION_START_TIME_OFFSET);
            float step = animation == g_evilAnimation && time > g_evilAnimationTime ? time - g_evilAnimationTime :
                deltaTime * LoadFloat(control + ANIMATION_CONTROL_SPEED_OFFSET);

            if (duration > 0.0f && step > 0.0f)
            {
                float end = duration - 1.0f / 120.0f;
                framesLeft = std::max(1, int(std::ceil((end - time) / step)));

                float endWithStick = duration - LoadFloat(motion + MOTION_END_SKIP_TIME_OFFSET);
                int withStick = 1 + std::max(0, int(std::ceil((endWithStick - time) / step)));
                if (grounded && withStick < framesLeft)
                    framesLeftWithStick = withStick;
            }

            nextAction = GetSharedString(action + ACTION_KEY_END_OFFSET);
            g_evilAnimationTime = time;
        }

        g_evilAnimation = animation;

        DrawIndicator(drawList, font, { column2, row }, indicatorWidth, "DASHING", IM_COL32(255, 200, 60, 230), dashing);
        DrawIndicator(drawList, font, { column3, row }, indicatorWidth, "ATTACKING", IM_COL32(255, 110, 80, 230), attacking);

        // Whether LB on this frame would guard. The states whose update checks for it, and changes to "Guard":
        // standing, walking, running and landing guard right away; an attack only on the ground, and if its action
        // allows it (sub_823CE5F8). All of them also need sub_823A4420 (an out-of-control check on the player),
        // which isn't replicated, and neither are the rarer cases in Damage and Fall.
        bool canGuard = stateName == "Idle" || stateName == "WalkSlowE" || stateName == "WalkE" || stateName == "RunE" ||
            stateName == "Land" || (action != 0 && grounded && LoadU8(action + ACTION_GUARD_OFFSET) != 0);
        DrawIndicator(drawList, font, { left, pressRow }, indicatorWidth, "CAN GUARD", IM_COL32(80, 230, 80, 255), canGuard, true);

        if (framesLeft >= 0)
        {
            std::string text;
            if (!nextAction.empty())
                text = fmt::format("{} in {}f", nextAction, framesLeft);
            else if (framesLeftWithStick >= 0)
                text = fmt::format("free in {}f, {}f with the stick", framesLeft, framesLeftWithStick);
            else
                text = fmt::format("free in {}f", framesLeft);

            DrawText(drawList, font, Scale(11), { left, attackRow }, IM_COL32(255, 200, 60, 255), text);
        }

        // The Unleash gauge, filled by defeating enemies.
        auto evilSonicContext = reinterpret_cast<SWA::Player::CEvilSonicContext*>(g_memory.Translate(context));
        std::string gauge = fmt::format("Unleash {:.1f}", float(evilSonicContext->m_DarkGaiaEnergy));
        DrawText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), gauge), pressRow + Scale(2) },
            IM_COL32(220, 220, 220, 255), gauge);

        // The stage time, which the Werehog's stages don't show: as the day stages show it (truncated to
        // hundredths), and in frames.
        uint32_t document = LoadU32(GAME_DOCUMENT);
        uint32_t member = document != 0 ? LoadU32(document + GAME_DOCUMENT_MEMBER_OFFSET) : 0;
        if (member != 0)
        {
            float stageTime = LoadFloat(member + STAGE_TIME_OFFSET);
            int hundredths = int(std::abs(stageTime) * 100.0f);
            std::string text = fmt::format("TIME {}{:02}:{:02}:{:02}  {}f", stageTime < 0.0f ? "-" : "", hundredths / 6000,
                hundredths / 100 % 60, hundredths % 100, int(std::lround(stageTime * 60.0f)));
            DrawText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), text), panelMin.y + Scale(10) },
                IM_COL32(220, 220, 220, 255), text);
        }

        // All attacks run in one state: show the attack list's name for the action instead.
        if (action != 0)
        {
            if (std::string actionName = GetSharedString(action + ACTION_NAME_OFFSET); !actionName.empty())
                stateLabel = "Attack " + actionName;
        }

        // Count drawn frames since the state or action changed: some attacks slow the game down, and their state
        // time then advances by less than a frame.
        if (state != g_evilState || action != g_evilAction || stateTime < g_evilStateTime)
            g_evilStateStartFrame = g_frame;

        g_evilState = state;
        g_evilAction = action;
        g_evilStateTime = stateTime;
        stateFrames = int(g_frame - g_evilStateStartFrame);
    }
    else
    {
        bool sliding = stateName.starts_with("Sliding");
        bool stomping = stateName.starts_with("Stomping");
        bool canJump = g_jumpCheckFrame == g_frame;
        bool stompChecked = g_stompCheckFrame == g_frame;
        bool canStomp = stompChecked && !g_stompDisabledByHop && !g_stompBlocked;

        DrawIndicator(drawList, font, { column2, row }, indicatorWidth, "SLIDING", IM_COL32(255, 200, 60, 230), sliding);
        DrawIndicator(drawList, font, { column3, row }, indicatorWidth, "STOMPING", IM_COL32(255, 110, 80, 230), stomping);

        // The movement mode, where the Werehog has the stage time. Diving (CSonicPostureDiving) keeps the table it
        // started with.
        auto movementMode = GetMovementMode(context);
        std::string mode = fmt::format("MODE {}", stateName.starts_with("Diving") ? "DIVING" : movementMode != nullptr ? movementMode->Name : "?");
        DrawText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), mode), panelMin.y + Scale(10) },
            IM_COL32(220, 220, 220, 255), mode);

        DrawIndicator(drawList, font, { left, pressRow }, indicatorWidth, "CAN JUMP", IM_COL32(80, 230, 80, 255), canJump, true);
        DrawIndicator(drawList, font, { column2, pressRow }, indicatorWidth, "CAN STOMP", IM_COL32(80, 230, 80, 255), canStomp, true);

        // During a short hop: in how many frames the stomp will be accepted. The hop adds the frame's time
        // (in single precision) to its own, and only then compares it and tries the stomp. Leaving the hop
        // (for an air boost, say) clears the flag right away instead. The air boost's state doesn't try the stomp,
        // and ends on the update after the boost button is let go (CSonicStateAirBoost, sub_82390198): after a
        // one-frame tap, the stomp is accepted 2 frames after the boost (jump, air boost, a frame, stomp in
        // RooftopRunAct1.ltm).
        bool hopping = stateName == "JumpShort" || stateName == "JumpHurdle";
        int stompFrames = -1;
        if (stompChecked && g_stompDisabledByHop && hopping)
        {
            float time = stateTime;
            stompFrames = 0;
            do
            {
                time += deltaTime;
                stompFrames++;
            } while (time <= g_stompingDisableTime && stompFrames < 99);
        }
        else if (!canStomp && !grounded && stateName == "AirBoost")
        {
            stompFrames = AIR_BOOST_STOMP_FRAMES;
        }
        else if (!canStomp && !grounded && g_sonicPreviousState == "AirBoost")
        {
            // The air boost ended on this frame's update, instead of the new state's trying the stomp.
            stompFrames = 1;
        }

        if (stompFrames > 0)
            DrawText(drawList, font, Scale(11), { column3, pressRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", stompFrames));

        // Whether LB or RB on this frame would quick-step: the ground actions tried it during this frame's update. It
        // also needs a new press. During a quick step: in how many frames it lets the next one start.
        bool canQuickStep = g_quickStepCheckFrame == g_frame && !g_quickStepDisabled && !g_quickStepBlocked;
        DrawIndicator(drawList, font, { left, quickStepRow }, 2 * indicatorWidth + gap, "CAN QUICKSTEP", IM_COL32(80, 230, 80, 255), canQuickStep, true);

        bool quickStepping = stateName == "QuickStep" || stateName == "RunQuickStep";
        int quickStepFrames = -1;
        if (!canQuickStep && quickStepping)
        {
            quickStepFrames = stateName == "QuickStep" ? GetQuickStepFramesLeft(QUICK_STEP_ACTIONS_FRAME, 0, deltaTime) :
                GetQuickStepFramesLeft(RUN_QUICK_STEP_ACTIONS_FRAME, RUN_QUICK_STEP_OUT_OF_CONTROL_FRAMES, deltaTime);
        }
        else if (!canQuickStep && grounded && (g_sonicPreviousState == "QuickStep" || g_sonicPreviousState == "RunQuickStep"))
        {
            // The quick step ended on this frame's update, instead of the new state's running the ground actions.
            quickStepFrames = 1;
        }

        if (quickStepFrames > 0)
            DrawText(drawList, font, Scale(11), { column3, quickStepRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", quickStepFrames));

        g_sonicPreviousState = stateName;
    }

    DrawText(drawList, font, Scale(11), { left, stateRow }, IM_COL32(220, 220, 220, 255),
        fmt::format("{}  {}f", stateLabel, stateFrames));

    // Stick, as SDL (and libTAS) reports it: XInput's Y is flipped with a bitwise NOT.
    int16_t sdlX = g_thumbLX;
    int16_t sdlY = int16_t(~g_thumbLY);
    float x = ApplyDeadzone(sdlX / 32768.0f);
    float y = ApplyDeadzone(sdlY / 32768.0f);

    // And as the game read it this frame.
    const SWA::SPadState* padState = nullptr;
    if (auto inputState = SWA::CInputState::GetInstance())
        padState = &inputState->GetPadState();

    float gameX = padState != nullptr ? float(padState->LeftStickHorizontal) : 0.0f;
    float gameY = padState != nullptr ? float(padState->LeftStickVertical) : 0.0f;
    float gameLengthSquared = gameX * gameX + gameY * gameY;
    bool breakJump = gameLengthSquared >= BREAK_JUMP_MIN_LENGTH * BREAK_JUMP_MIN_LENGTH &&
        gameLengthSquared < BREAK_JUMP_MAX_LENGTH * BREAK_JUMP_MAX_LENGTH;

    drawList->AddCircle(centre, radius, IM_COL32(255, 255, 255, 200), 64, Scale(1.5f));
    drawList->AddLine({ centre.x - radius, centre.y }, { centre.x + radius, centre.y }, IM_COL32(255, 255, 255, 90), Scale(1));
    drawList->AddLine({ centre.x, centre.y - radius }, { centre.x, centre.y + radius }, IM_COL32(255, 255, 255, 90), Scale(1));

    // M-Speed, D-Speed and the break-jump are Sonic's: the Werehog can't do them. M-Speed is for 3D, where the stick
    // goes through the curve, D-Speed for 2D.
    std::string zoneText;
    if (!werehog)
    {
        DrawLengthZone(drawList, centre, radius, BREAK_JUMP_MIN_LENGTH, BREAK_JUMP_MAX_LENGTH,
            breakJump ? IM_COL32(255, 150, 30, 200) : IM_COL32(255, 150, 30, 70), breakJump ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 120));

        auto movementMode = GetMovementMode(context);
        if (movementMode != nullptr && movementMode->SideView)
        {
            for (auto& zone : TARGET_ZONES)
            {
                bool active = IsInZone(zone, x, y);
                if (active && zoneText.empty())
                    zoneText = zone.Name;

                DrawZone(drawList, centre, radius, zone, active);
            }
        }
        else
        {
            bool active = IsMSpeedInput(std::sqrt(gameLengthSquared));
            if (active)
                zoneText = "M-Speed";

            DrawLengthZone(drawList, centre, radius, GetStickLengthForMovementInput(g_movementInputDeadzone),
                GetStickLengthForMovementInput(M_SPEED_MAX_INPUT), active ? IM_COL32(0, 255, 0, 200) : IM_COL32(0, 255, 0, 80),
                active ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 160));
        }
    }

    ImVec2 stick = { centre.x + x * radius, centre.y + y * radius };
    drawList->AddLine(centre, stick, IM_COL32(255, 60, 60, 255), Scale(2));
    drawList->AddCircleFilled(stick, Scale(4.5f), IM_COL32(255, 60, 60, 255));

    if (!werehog)
    {
        bool zone = !zoneText.empty();
        if (breakJump)
            zoneText += zoneText.empty() ? "Break-jump" : " + Break-jump";

        ImU32 zoneColour = zone ? IM_COL32(80, 255, 80, 255) : breakJump ? IM_COL32(255, 160, 40, 255) : IM_COL32(200, 200, 200, 255);
        DrawText(drawList, font, Scale(11), { left, centre.y + radius + Scale(6) }, zoneColour, zoneText.empty() ? "No zone" : zoneText);
    }

    // What to type into libTAS, and what the game made of it.
    DrawText(drawList, font, Scale(11), { left, centre.y + radius + Scale(20) }, IM_COL32_WHITE,
        fmt::format("X {:.2f} Y {:.2f}  ({}, {})", x, y, sdlX, sdlY));

    if (padState != nullptr)
    {
        DrawText(drawList, font, Scale(11), { left, centre.y + radius + Scale(34) }, IM_COL32(200, 200, 200, 255),
            fmt::format("Game {:.3f} {:.3f}  length {:.3f}", gameX, gameY, std::sqrt(gameLengthSquared)));
    }

    DrawController(drawList, font, { left, centre.y + radius + Scale(52) }, width - 2 * padding, padState);

    g_frame++;
}
