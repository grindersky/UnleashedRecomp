#include "tas_hud.h"
#include <api/SWA.h>
#include <app.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <ui/collision_overlay.h>
#include <ui/imgui_utils.h>
#include <tas_mode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <mutex>
#include <numbers>
#include <unordered_map>
#include <vector>

// The day Sonic player context (SWA::Player::CSonicContext), recorded when its constructor runs. Its
// velocity is split like in Sonic Generations' CPlayerSpeedContext: the full velocity, the part along the
// ground and the part along the up vector (full = horizontal + vertical). Found by comparing per-frame
// dumps of the context with how Sonic's position moves.
static constexpr uint32_t SONIC_CONTEXT_VFTABLE = 0x820182C4;
static constexpr uint32_t VELOCITY_OFFSET = 0x210;
static constexpr uint32_t HORIZONTAL_VELOCITY_OFFSET = 0x220;
static constexpr uint32_t VERTICAL_VELOCITY_OFFSET = 0x230;
// Sonic's up vector.
static constexpr uint32_t UP_OFFSET = 0x2F0;

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
// InAirMaxAccelVelocity (15 by default), read by the 3D air update.
static constexpr uint32_t IN_AIR_MAX_ACCEL_VELOCITY_PARAMETER = 22;
// What the context reads its parameters by ID with (sub_8245DB60).
static constexpr uint32_t PARAMETER_READER_OFFSET = 0x1FC;
// The player's matrix node (SWA::CMatrixNodeTransform), whose matrix at +128 has the axes as rows: his facing is the
// third (sub_8230A4E0).
static constexpr uint32_t MATRIX_NODE_OFFSET = 16;
static constexpr uint32_t MATRIX_NODE_ROTATION_OFFSET = 128;
static constexpr uint32_t MATRIX_NODE_FACING_OFFSET = 128 + 32;
// The basis the 3D input turns the stick into the world with (sub_82316128): the stick's X along the first row, its Y
// against the third.
static constexpr uint32_t INPUT_BASIS_OFFSET = 0x290;
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
// Out of control (CPlayerSpeedPosturePluginOutOfControl, the context's vfunc 56): the ground actions try nothing.
static constexpr uint32_t OUT_OF_CONTROL_FLAG = 3;
// The boost gauge (the chaos energy, sub_82316B70).
static constexpr uint32_t CHAOS_ENERGY_OFFSET = 0x53C;
static constexpr uint32_t SLIDE_BOOST_LENGTH_OFFSET = 1056;
// Boosts: the ground actions call TryBoost (sub_823787B0), the air actions TryAirBoost (sub_82375748). Both refuse
// while the stomp's blocker is set or Sonic already boosts (flag 14), and need X just pressed and more energy than
// BoostEnableChaosEnergy (215) or AirBoostEnableChaosEnergy (264). On the ground, flags 15 and 66 also refuse it. In the
// air, flag 19 is needed and flag 18 refuses it; with parameter 294 on, a set word at +0xF8C makes it a homing attack
// instead. Not replicated: sub_82308838 (ground) and the ability check sub_82374CE0 (air).
static constexpr uint32_t BOOSTING_FLAG = 14;
static constexpr uint32_t BOOST_DISABLED_FLAG = 15;
static constexpr uint32_t BOOST_DISABLED_FLAG_2 = 66;
static constexpr uint32_t AIR_BOOST_ENABLED_FLAG = 19;
static constexpr uint32_t AIR_BOOST_DISABLED_FLAG = 18;
static constexpr uint32_t AIR_BOOST_HOMING_OFFSET = 0xF8C;
static constexpr uint32_t BOOST_ENABLE_CHAOS_ENERGY_PARAMETER = 215;
static constexpr uint32_t AIR_BOOST_ENABLE_CHAOS_ENERGY_PARAMETER = 264;
static constexpr uint32_t AIR_BOOST_HOMING_PARAMETER = 294;
// Homing attacks: the air actions first call TryHomingAttack (sub_823778D8). Unless the stomp's blocker is set, and with
// flags 19 and 17 (set by JumpShort, GrindJumpShort and the context's vfunc 32, cleared by HomingAttack, AirBoost and
// damage), it runs the target search (sub_82375FE0), which keeps, changes or drops the target at +0xF8C and starts the
// reticle (CSonicHudHomingAttack at +4260, sub_823A3748), then changes to HomingAttack when A is just pressed. Without a
// target, HomingAttack dashes forward (its Enter, sub_82393F48). With parameter 294 (XButtonHoming, off with the
// HomingAttackOnJump code) it returns after the search, and TryAirBoost homes on X instead, when there's a target.
static constexpr uint32_t HOMING_ATTACK_ENABLED_FLAG = 17;
// HomingAttackAfter (sub_82394900) only runs the air actions once it has lasted longer than this.
static constexpr uint32_t ACTION_ENABLE_AFTER_HOMING_ATTACK_PARAMETER = 278;
// The search tries two directions: the one sub_82373E58 picks (from the stick), then the facing. For each, it places the
// phantom at +3448 (+3464 in 2D) at Sonic, turned to it (sub_82494318), and asks every object whose collision the
// phantom touches for its priority (sub_823751A8) and position (sub_82375258) (sub_8230E990). The target is one in a 75°
// cone around the phantom's forward (+Z) from Sonic's position, in sight (a raycast), and the nearest or the one of
// highest priority. The phantom is a view frustum (sub_82379718) looking along its +Z from the point at 0x83397230,
// from parameters 297 (aspect), 298 (vertical field of view, degrees), 299 (near) and 300 (far).
static constexpr uint32_t HOMING_PHANTOM_2D_OFFSET = 3464;
static constexpr uint32_t LOCKON_FRUSTUM_FIRST_PARAMETER = 297;
static constexpr uint32_t LOCKON_FRUSTUM_EYE = 0x83397230;
static constexpr float HOMING_CONE_ANGLE = 1.3089969f;
// Sonic's position, as the search reads it (sub_823087C8).
static constexpr uint32_t MATRIX_NODE_POSITION_OFFSET = 112;
// Whether Sonic is in a side view section (sub_82316208): the search then uses the 2D phantom.
static constexpr uint32_t SIDE_VIEW_OFFSET = 1204;
// The states whose update starts with the ground actions, which try the jump, then the quick step: entered during a
// frame's update, they try them on the next. Land skips them only on the update that ends its animation, and
// StompingLand (sub_8239C598) also when it has lasted 0.1 s with the stick held past 0.25: then it picks the next state.
// LandJumpShort (sub_823399B8) skips them on the update that ends its animation (sub_82307CB0) or while Sonic is faster
// than parameter 83: it changes state instead.
static constexpr uint32_t GROUND_ACTIONS_STATE_VFTABLES[] =
{
    0x820167F4, // CPlayerSpeedStateStand
    0x820128B0, // CPlayerSpeedContext::CStateWalk
    0x82016934, // CSonicContext::CStateWalk
    0x82016A10, // CSonicContext::CStateBrakeEnd
    0x820169B8, // CSonicStateMoveStop
    0x8201B7F0, // CSonicStateDrift
    0x8201BC60, // CSonicStateOnIce
    0x82016B48, // CSonicStateShoesSliding
    0x8201C550, // CSonicStateWallRunDamage
    0x82011EF8, // CPlayerSpeedContext::CStateLand
    0x82011F90, // CPlayerSpeedContext::CStateLandJumpShort
    0x82016A68, // CSonicStateLand
    0x8201C388, // CSonicStateStompingLand
};
// The states whose update calls the air actions (the context's vfunc 37, sub_823795F8: the homing attack, TryStomp, then
// TryAirBoost), and those that call TryBoost themselves: entered during a frame's update, they try the boost on the next.
static constexpr uint32_t AIR_ACTIONS_STATE_VFTABLES[] =
{
    0x82011E80, // CPlayerSpeedContext::CStateJumpSpring
    0x820125C8, // CPlayerSpeedStateSpecialJump
    0x82016A3C, // CSonicStateFall
    0x82016B74, // CSonicStateFinishExternalControlAir
    0x8201B8D8, // CSonicStateGrindJumpShort
    0x8201BAC0, // CSonicStateHomingAttackAfter
    0x8201698C, // CSonicStateJumpBall
    0x82016560, // CSonicContext::CStateJumpHurdle
    0x82016960, // CSonicContext::CStateJumpShort
    0x8201BEF4, // CSonicStateSelectJumpAfter::CStateUp
    0x8201BF44, // CSonicStateSelectJumpAfter::CStateFrontAir
    0x82016B1C, // CSonicStateWallJump
};
static constexpr uint32_t BOOST_STATE_VFTABLES[] =
{
    0x8201C0AC, // CSonicContext::CStateSliding
    0x82016BA0, // CSonicStatePushingWall
};
// The states with a jump buffer instead of TryJump: their update first starts a timer at ctx+3384 when A is pressed
// (sub_82316210), then, if Sonic is on the ground (sub_82316118), jumps if the timer is under parameter 176
// (sub_82330D18) and lands otherwise. Running on water keeps Sonic in Fall but on the ground: there, a press jumps on the
// next update.
static constexpr uint32_t JUMP_BUFFER_STATE_VFTABLES[] =
{
    0x82016A3C, // CSonicStateFall (sub_82392308 -> sub_823365A8)
    0x8201698C, // CSonicStateJumpBall (sub_82394A58 -> sub_82338470)
    0x82016560, // CSonicContext::CStateJumpHurdle (sub_82394F30)
    0x82011E44, // CPlayerSpeedContext::CStateJumpShort (sub_823386E0)
};
// Hedgehog engine states: the context at +8, the time spent in the state, in seconds, at +0x10.
static constexpr uint32_t STATE_CONTEXT_OFFSET = 0x8;
static constexpr uint32_t STATE_TIME_OFFSET = 0x10;
// The parameter table in use (boost::shared_ptr, set by sub_8230B630). Each movement mode switches to its own table,
// kept in a global shared_ptr. Sonic's parameter loader (sub_8237A828) fills them from the parameter groups named
// below: free 3D is "ForwardView", a 2D section switches to "SideView" (sub_82320B70) and back, and the forward
// sections, with quick steps, to "DashMode" (sub_82377D00), then "QSM" (sub_82378500). The others are switched to by
// the context's update (sub_823746C0) while CPlayerSpeedPosturePluginOnWater or CSonicPosturePluginWallCorrection
// (Rooftop Run's wall run) is on, and by MsgStartSonicSpecialMode, which the Egg Beetle, the Egg Ray Bird and the
// E-BigChaser send (sub_82365BE8, an index of 0 to 3 into the "Special" tables). The ShoesSliding3D/2D tables (defaults
// set by sub_82324410) are switched to by sub_82316B90, seen in the ShoesSliding state.
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
    { 0x83362FBC, "SHOES SLIDING" },
    { 0x83362FC4, "SHOES SLIDING 2D", true },
    { 0x83364728, "WALL RUN" },
    { 0x83362FCC, "ON ICE" },
    { 0x83362FD4, "ON ICE 2D", true },
    { 0x83364760, "INTERCEPTOR" }, // Special 0: the interceptor chase (Savannah Citadel)
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

// Counts drawn frames, to tell whether the checks below ran during the update of the frame being drawn, and in which
// state.
static uint32_t g_frame = 1;
static uint32_t g_jumpCheckFrame;
static uint32_t g_jumpCheckState;
static uint32_t g_stompCheckFrame;
static bool g_stompDisabledByHop;
static bool g_stompBlocked;
static float g_stompingDisableTime = 0.15f;
static float g_movementInputDeadzone = 0.02f;
static float g_inAirMaxAccelVelocity = 15.0f;
static uint32_t g_quickStepCheckFrame;
static uint32_t g_quickStepCheckState;
// Sonic's state on the last drawn frame.
static std::string g_sonicPreviousState;
static uint32_t g_sonicPreviousStateObject;
static bool g_quickStepDisabled;
static bool g_quickStepBlocked;
// The boosts' checks: the frame and state they ran in, and whether everything but the press and the energy allowed
// the boost then. The energy needed is what the game read last (both are 1 by default, and were 1 in MSPEED_TEST.ltm and
// RooftopRunAct1.ltm).
static uint32_t g_boostCheckFrame;
static uint32_t g_boostCheckState;
static bool g_boostAllowed;
static uint32_t g_airBoostCheckFrame;
static float g_boostEnableEnergy = 1.0f;
static float g_airBoostEnableEnergy = 1.0f;
static bool g_airBoostHoming;
static uint32_t g_homingCheckFrame;
static float g_actionEnableAfterHomingAttack = NAN;

// The lock-on frustums (3D, 2D): aspect, field of view, near, far.
static float g_lockonFrustums[2][4] = { { NAN, NAN, NAN, NAN }, { NAN, NAN, NAN, NAN } };
static thread_local int g_recordingLockonFrustum = -1;
// The last target search: Sonic's position then, and the phantom's placements (rows: its axes, then its position).
static thread_local bool g_inHomingSearch;
static uint32_t g_homingSearchFrame;
static float g_homingSearchPosition[3];
// Sonic's rotation then (his axes, as rows).
static float g_homingSearchRotation[3][3];
static float g_homingSearchPoses[2][16];
static int g_homingSearchPoseCount;

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
uint32_t TasHud::GetPlayerContext(bool& werehog)
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

static uint32_t GetCurrentState(uint32_t context);

// The context's TryJump: jumps if the jump button is pressed. The states that allow jumping call it every
// frame; others, like a quick step until it ends, don't.
PPC_FUNC_IMPL(__imp__sub_82330CB0);
PPC_FUNC(sub_82330CB0)
{
    if (TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext)
    {
        g_jumpCheckFrame = g_frame;
        g_jumpCheckState = GetCurrentState(ctx.r3.u32);
    }

    __imp__sub_82330CB0(ctx, base);
}

// The blocker that refuses stomps and quick steps (sub_8231B7D8).
static bool IsActionBlocked(uint32_t context)
{
    return LoadU32(context + STOMP_BLOCKER_OFFSET) != 0 && !GetFlag(context, STATE_FLAGS_2_OFFSET, STOMP_BLOCKER_OVERRIDE_FLAG);
}

// The context's TryHomingAttack, the first thing the air actions try once Sonic is in the air and in control.
PPC_FUNC_IMPL(__imp__sub_823778D8);
PPC_FUNC(sub_823778D8)
{
    if (TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext)
        g_homingCheckFrame = g_frame;

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
            g_homingSearchPosition[i] = LoadFloat(node + MATRIX_NODE_POSITION_OFFSET + i * 4);
            for (int row = 0; row < 3; row++)
                g_homingSearchRotation[row][i] = LoadFloat(node + MATRIX_NODE_ROTATION_OFFSET + row * 16 + i * 4);
        }

        g_homingSearchPoseCount = 0;
        g_inHomingSearch = true;
    }

    __imp__sub_82375FE0(ctx, base);

    if (record)
    {
        g_inHomingSearch = false;
        g_homingSearchFrame = g_frame;
    }
}

// Places a phantom (r3) with a matrix (r4). Records the lock-on phantom's placements during the search.
PPC_FUNC_IMPL(__imp__sub_82494318);
PPC_FUNC(sub_82494318)
{
    if (g_inHomingSearch && g_homingSearchPoseCount < 2)
    {
        for (int i = 0; i < 16; i++)
            g_homingSearchPoses[g_homingSearchPoseCount][i] = LoadFloat(ctx.r4.u32 + i * 4);

        g_homingSearchPoseCount++;
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
        g_quickStepCheckState = GetCurrentState(context);
        g_quickStepDisabled = LoadU8(context + QUICK_STEP_DISABLED_OFFSET) != 0;
        g_quickStepBlocked = IsActionBlocked(context);
    }

    __imp__sub_82386E60(ctx, base);
}

// Whether TryBoost and TryAirBoost would boost on a press, apart from the energy.
static bool IsBoostAllowed(uint32_t context)
{
    return !IsActionBlocked(context) && !GetFlag(context, STATE_FLAGS_OFFSET, BOOSTING_FLAG) &&
        !GetFlag(context, STATE_FLAGS_OFFSET, BOOST_DISABLED_FLAG) && !GetFlag(context, STATE_FLAGS_OFFSET, BOOST_DISABLED_FLAG_2);
}

static bool IsAirBoostAllowed(uint32_t context)
{
    return !IsActionBlocked(context) && !GetFlag(context, STATE_FLAGS_OFFSET, BOOSTING_FLAG) &&
        GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_ENABLED_FLAG) && !GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_DISABLED_FLAG) &&
        !(g_airBoostHoming && LoadU32(context + AIR_BOOST_HOMING_OFFSET) != 0);
}

// The context's TryBoost, called by the ground actions, sliding and pushing a wall.
PPC_FUNC_IMPL(__imp__sub_823787B0);
PPC_FUNC(sub_823787B0)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_boostCheckFrame = g_frame;
        g_boostCheckState = GetCurrentState(context);
        g_boostAllowed = IsBoostAllowed(context);
    }

    __imp__sub_823787B0(ctx, base);
}

// The context's TryAirBoost, called by the air actions after TryStomp.
PPC_FUNC_IMPL(__imp__sub_82375748);
PPC_FUNC(sub_82375748)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
        g_airBoostCheckFrame = g_frame;

    __imp__sub_82375748(ctx, base);
}

// Reads a bool parameter of the player by ID. Records whether the air boost can turn into a homing attack.
PPC_FUNC_IMPL(__imp__sub_8245DB18);
PPC_FUNC(sub_8245DB18)
{
    uint32_t reader = ctx.r3.u32;
    uint32_t parameter = ctx.r4.u32;
    __imp__sub_8245DB18(ctx, base);

    if (parameter == AIR_BOOST_HOMING_PARAMETER && TasHud::IsEnabled() && g_speedContext != 0 &&
        reader == LoadU32(g_speedContext + PARAMETER_READER_OFFSET))
    {
        g_airBoostHoming = ctx.r3.u8 != 0;
    }
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

// The path direction the 2D input last split the stick along (see D-Speed), and the drawn frame it was for. Recorded
// while the 2D input (sub_8234F140: CPlayerSpeedPostureInput2DStandard, also OnPath and ForcePath) runs, from its call
// to sub_8231EE70.
static bool g_inPathInput = false;
static uint32_t g_pathFrame = 0;
static float g_pathDirection[3];

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
        g_pathFrame = g_frame;
        for (int i = 0; i < 3; i++)
            g_pathDirection[i] = LoadFloat(ctx.r6.u32 + i * 4);
    }

    __imp__sub_8231EE70(ctx, base);
}

// The drawn frame the 2D air posture last ran its Brake state's update on (CPlayerSpeedPosture2DAir::CStateBrake,
// sub_82344B88). The Normal state (sub_82344868, where M-Speed happens) switches to it when sub_8231B0F0 says so: Sonic's
// speed at least 1 and his input, after the deadzone, more than BrakeEnableAngle (parameter 31, degrees) away from his
// front (or his velocity, by ctx+960), or zeroed by the deadzone.
static uint32_t g_air2DBrakeFrame = 0;

PPC_FUNC_IMPL(__imp__sub_82344B88);
PPC_FUNC(sub_82344B88)
{
    if (TasHud::IsEnabled())
        g_air2DBrakeFrame = g_frame;

    __imp__sub_82344B88(ctx, base);
}

// Reads a float parameter of the player by ID. Records StompingDisableTime when the short hop reads it, the
// movement input's deadzone when the input is read, day Sonic's InAirMaxAccelVelocity when the air update reads it, and
// how long HomingAttackAfter waits before the air actions.
PPC_FUNC_IMPL(__imp__sub_8245DB60);
PPC_FUNC(sub_8245DB60)
{
    uint32_t reader = ctx.r3.u32;
    uint32_t parameter = ctx.r4.u32;
    __imp__sub_8245DB60(ctx, base);

    if (parameter == STOMPING_DISABLE_TIME_PARAMETER && TasHud::IsEnabled())
        g_stompingDisableTime = float(ctx.f1.f64);

    if (parameter == MOVEMENT_INPUT_DEADZONE_PARAMETER && TasHud::IsEnabled())
        g_movementInputDeadzone = float(ctx.f1.f64);

    if (parameter == IN_AIR_MAX_ACCEL_VELOCITY_PARAMETER && TasHud::IsEnabled() && g_speedContext != 0 &&
        reader == LoadU32(g_speedContext + PARAMETER_READER_OFFSET))
    {
        g_inAirMaxAccelVelocity = float(ctx.f1.f64);
    }

    if ((parameter == BOOST_ENABLE_CHAOS_ENERGY_PARAMETER || parameter == AIR_BOOST_ENABLE_CHAOS_ENERGY_PARAMETER) &&
        TasHud::IsEnabled() && g_speedContext != 0 && reader == LoadU32(g_speedContext + PARAMETER_READER_OFFSET))
    {
        (parameter == BOOST_ENABLE_CHAOS_ENERGY_PARAMETER ? g_boostEnableEnergy : g_airBoostEnableEnergy) = float(ctx.f1.f64);
    }

    if (g_recordingLockonFrustum >= 0 && parameter >= LOCKON_FRUSTUM_FIRST_PARAMETER && parameter < LOCKON_FRUSTUM_FIRST_PARAMETER + 4)
        g_lockonFrustums[g_recordingLockonFrustum][parameter - LOCKON_FRUSTUM_FIRST_PARAMETER] = float(ctx.f1.f64);

    if (parameter == ACTION_ENABLE_AFTER_HOMING_ATTACK_PARAMETER && TasHud::IsEnabled() && g_speedContext != 0 &&
        reader == LoadU32(g_speedContext + PARAMETER_READER_OFFSET))
    {
        g_actionEnableAfterHomingAttack = float(ctx.f1.f64);
    }
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

template<size_t N>
static bool IsStateIn(uint32_t state, const uint32_t (&vftables)[N])
{
    uint32_t vftable = state != 0 ? LoadU32(state) : 0;
    return std::find(std::begin(vftables), std::end(vftables), vftable) != std::end(vftables);
}

static bool StartsWithGroundActions(uint32_t state)
{
    return IsStateIn(state, GROUND_ACTIONS_STATE_VFTABLES);
}

// Whether the phantom's rows are its axes (else its columns are): decided by which turns to the facing, the second
// direction searched.
static bool g_homingPoseRows = true;

// An estimate of whether the next search finds a target when this frame's didn't: the phantom placed as the last search
// did, moved with Sonic since (that matched the next search's placement within 2 cm on every frame of
// DragonRoadAct1.ltm), against the objects with a collision the search meets (CollisionOverlay: by collision category;
// their boxes, capsules and spheres against the frustum's planes), the 75 degree cone against their smallest collision's
// centre, and the search's line of sight raycast against the stage's collision. When there was no search this frame
// (the game skips it in some states, and the first frame a press becomes possible has none yet), the facing's phantom
// is placed on Sonic as it sat on him at the last search (learnt from each search, in his frame).
// Not replicated: the direction the next stick input picks, other shapes (taken as 1 m around them), the line of sight
// against anything but the stage, the priorities (every object asked answered 10), and what moves during the next update
// before its search. DragonRoadAct1.ltm: lit on all 9 frames before a target appeared, and on 15 where none did (two
// E-Fighter missiles and an E-Spanner the game never took, cause not found). HomingAttackTest.ltm (the user's, X =
// homing): lit before every X press that homed (4), dark before every one that didn't (7); misses a wide spring right
// below Sonic after he bounces off it (the game's collision test takes it about 1 m outside the frustum; not found why).
static constexpr float HOMING_SIGHT_HEIGHT = 0.5f;

// How the facing's phantom sits on Sonic (learnt from the searches): its axes and its position from his, in his frame.
static bool g_homingRelationValid;
static float g_homingRelationAxes[3][3];
static float g_homingRelationOrigin[3];

static bool EstimateNextHomingTarget(uint32_t context)
{
    bool fresh = g_homingSearchFrame == g_frame && g_homingSearchPoseCount > 0;
    if (!fresh && !g_homingRelationValid)
        return false;

    const float* frustum = g_lockonFrustums[LoadU8(context + SIDE_VIEW_OFFSET) != 0 ? 1 : 0];
    if (std::isnan(frustum[0]) || std::isnan(frustum[1]) || std::isnan(frustum[2]) || std::isnan(frustum[3]))
        return false;

    float tanY = std::tan(frustum[1] * std::numbers::pi_v<float> / 360.0f);
    float tanX = tanY * frustum[0];
    float coneCos = std::cos(HOMING_CONE_ANGLE);

    uint32_t node = LoadU32(context + MATRIX_NODE_OFFSET);
    float position[3], delta[3], facing[3], eye[3];
    for (int i = 0; i < 3; i++)
    {
        position[i] = LoadFloat(node + MATRIX_NODE_POSITION_OFFSET + i * 4);
        delta[i] = position[i] - g_homingSearchPosition[i];
        facing[i] = LoadFloat(node + MATRIX_NODE_FACING_OFFSET + i * 4);
        eye[i] = LoadFloat(LOCKON_FRUSTUM_EYE + i * 4);
    }

    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    // The phantoms' axes and positions, as the next search would place them.
    int poseCount = fresh ? g_homingSearchPoseCount : 1;
    float axes[2][3][3], origins[2][3];
    if (fresh)
    {
        if (poseCount == 2)
        {
            const float* pose = g_homingSearchPoses[1];
            float row[3] = { pose[8], pose[9], pose[10] };
            float column[3] = { pose[2], pose[6], pose[10] };
            g_homingPoseRows = dot(row, facing) >= dot(column, facing);
        }

        for (int k = 0; k < poseCount; k++)
        {
            const float* pose = g_homingSearchPoses[k];
            for (int axis = 0; axis < 3; axis++)
            {
                for (int i = 0; i < 3; i++)
                    axes[k][axis][i] = g_homingPoseRows ? pose[axis * 4 + i] : pose[i * 4 + axis];
            }

            for (int i = 0; i < 3; i++)
                origins[k][i] = pose[12 + i] + delta[i];
        }

        // How the last direction (the facing, when there are two) sits on Sonic: its axes and position in his frame.
        const float* pose = g_homingSearchPoses[poseCount - 1];
        float offset[3] = { pose[12] - g_homingSearchPosition[0], pose[13] - g_homingSearchPosition[1], pose[14] - g_homingSearchPosition[2] };
        for (int j = 0; j < 3; j++)
        {
            for (int axis = 0; axis < 3; axis++)
                g_homingRelationAxes[axis][j] = dot(axes[poseCount - 1][axis], g_homingSearchRotation[j]);

            g_homingRelationOrigin[j] = dot(offset, g_homingSearchRotation[j]);
        }

        g_homingRelationValid = true;
    }
    else
    {
        // No search this frame (the game didn't run it, or not yet in this state): the facing's phantom, placed on
        // Sonic as it sat on him at the last search.
        float rotation[3][3];
        for (int row = 0; row < 3; row++)
        {
            for (int i = 0; i < 3; i++)
                rotation[row][i] = LoadFloat(node + MATRIX_NODE_ROTATION_OFFSET + row * 16 + i * 4);
        }

        for (int i = 0; i < 3; i++)
        {
            origins[0][i] = position[i];
            for (int j = 0; j < 3; j++)
                origins[0][i] += g_homingRelationOrigin[j] * rotation[j][i];

            for (int axis = 0; axis < 3; axis++)
            {
                axes[0][axis][i] = 0.0f;
                for (int j = 0; j < 3; j++)
                    axes[0][axis][i] += g_homingRelationAxes[axis][j] * rotation[j][i];
            }
        }
    }

    // The frustum's far corners are further than its far plane (27 m for 85 degrees and 20 m), and objects' collisions
    // reach out from their centres.
    std::vector<CollisionOverlay::ObjectCentre> objects;
    CollisionOverlay::GetObjectCentres(position[0], position[1], position[2], frustum[3] * 1.5f + 10.0f, objects);
    CollisionOverlay::UpdateStage();

    for (auto& object : objects)
    {
        if (!object.HomingCandidate)
            continue;

        float toTarget[3] = { object.X - position[0], object.Y - position[1], object.Z - position[2] };
        float distance = std::sqrt(dot(toTarget, toTarget));
        for (int k = 0; k < poseCount; k++)
        {
            float forwardLength = std::sqrt(dot(axes[k][2], axes[k][2]));
            bool inCone = distance > 0.0f && forwardLength > 0.0f && dot(toTarget, axes[k][2]) / (distance * forwardLength) >= coneCos;
            if (!inCone)
                continue;

            // Each of its shapes against the frustum's six planes (in the phantom's frame: near z, far z, and the four
            // sides |x| = z tan, |y| = z tan): outside one if its centre is further out than the shape reaches towards it.
            bool inFrustum = false;
            for (auto& volume : object.HomingVolumes)
            {
                float d[3] = { volume.Centre[0] - origins[k][0], volume.Centre[1] - origins[k][1], volume.Centre[2] - origins[k][2] };
                float c[3] = { dot(d, axes[k][0]) - eye[0], dot(d, axes[k][1]) - eye[1], dot(d, axes[k][2]) - eye[2] };
                float a[3][3];
                for (int i = 0; i < volume.AxisCount; i++)
                {
                    for (int j = 0; j < 3; j++)
                        a[i][j] = dot(volume.Axes[i], axes[k][j]);
                }

                auto reach = [&](float nx, float ny, float nz)
                {
                    float e = volume.Radius * std::sqrt(nx * nx + ny * ny + nz * nz);
                    for (int i = 0; i < volume.AxisCount; i++)
                        e += std::abs(a[i][0] * nx + a[i][1] * ny + a[i][2] * nz);

                    return e;
                };

                bool outside = frustum[2] - c[2] > reach(0.0f, 0.0f, 1.0f) || c[2] - frustum[3] > reach(0.0f, 0.0f, 1.0f) ||
                    c[0] - c[2] * tanX > reach(1.0f, 0.0f, -tanX) || -c[0] - c[2] * tanX > reach(-1.0f, 0.0f, -tanX) ||
                    c[1] - c[2] * tanY > reach(0.0f, 1.0f, -tanY) || -c[1] - c[2] * tanY > reach(0.0f, -1.0f, -tanY);
                if (!outside)
                {
                    inFrustum = true;
                    break;
                }
            }

            // Then the line of sight, as the search casts it: from Sonic to the target, both 0.5 higher, against the
            // stage's collision.
            if (inFrustum &&
                !CollisionOverlay::IsSightBlocked(position[0], position[1] + HOMING_SIGHT_HEIGHT, position[2], object.X, object.Y + HOMING_SIGHT_HEIGHT, object.Z))
            {
                return true;
            }
        }
    }

    return false;
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

static constexpr float PI = std::numbers::pi_v<float>;

// The per-axis deadzone the stick is drawn with, on the stick as SDL reports it (-1..1, Y pointing down).
static constexpr float STICK_DEADZONE = 0.05f;

// M-Speed, from the 3D air update (sub_82346880). Leaving the ground sets flag 27 (the context's vfunc 25,
// sub_82374360; landing clears it), and the update clears it as soon as the movement input is longer than 0.3; while
// it's set, the air deceleration is 0. The update (sub_8234C5F8) aims for a speed along Sonic's facing: the input's part
// along it times max(InAirMaxAccelVelocity, his speed). If that's above his speed along his facing, he accelerates
// toward it (an input against his facing always is). Otherwise he decelerates, by 0, and then, while his speed along his
// facing is below that max, he's pushed along his velocity by a force that grows with the square of his speed: he gains
// speed every frame. Unless the aim is over 0.25 and within 0.1 of his speed along his facing: then nothing happens. So
// the input has to point to his facing's side of the stick, and at low speeds, not too far toward it. The 3D input
// (CPlayerSpeedPostureInput3DStandard, sub_8234EE88; PathLocal, sub_8234F4D0, does the same) is the stick's direction
// times (length / 0.9)³, at most 1, turned into the world by the context's input basis (sub_82316128), and is ignored
// below parameter 12 (0.02, sub_823083F0). So on the stick as the game reads it, M-Speed takes a length from about 0.244
// to 0.602, on the side Sonic faces. 2D reads the stick without the curve (D-Speed, below).
static constexpr float MOVEMENT_INPUT_SCALE = 1.0f / 0.9f;
static constexpr float M_SPEED_MAX_INPUT = 0.3f;
static constexpr float M_SPEED_STILL_MIN_AIM = 0.25f;
static constexpr float M_SPEED_STILL_DIFFERENCE = 0.1f;

// The flag that keeps the air deceleration at 0 (see above).
static constexpr uint32_t NO_AIR_DECELERATION_FLAG = 27;

// What decides M-Speed's directions, for the frame being drawn. Sonic's facing on the stick, as drawn (x right, y
// down): an input of length m in direction a aims for m * (cos a * FacingX + sin a * FacingY) * Target along his facing.
// Off in the air once the flag is cleared, until he lands.
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

static MSpeedRule GetMSpeedRule(uint32_t context, bool grounded)
{
    MSpeedRule rule;
    rule.Off = !grounded && !GetFlag(context, STATE_FLAGS_OFFSET, NO_AIR_DECELERATION_FLAG);

    uint32_t node = LoadU32(context + MATRIX_NODE_OFFSET);
    float facing[3], right[3], back[3], velocity[3];
    for (int i = 0; i < 3; i++)
    {
        facing[i] = LoadFloat(node + MATRIX_NODE_FACING_OFFSET + i * 4);
        right[i] = LoadFloat(context + INPUT_BASIS_OFFSET + i * 4);
        back[i] = LoadFloat(context + INPUT_BASIS_OFFSET + 32 + i * 4);
        velocity[i] = LoadFloat(context + HORIZONTAL_VELOCITY_OFFSET + i * 4);
    }

    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float speed = std::sqrt(dot(velocity, velocity));

    // The stick's X goes along the basis' first row and its Y against the third, and Y is drawn upside down.
    if (node != 0 && std::isfinite(dot(facing, facing)))
    {
        rule.FacingX = dot(right, facing);
        rule.FacingY = dot(back, facing);
    }

    rule.Target = std::max(g_inAirMaxAccelVelocity, speed);
    rule.AlongFacing = dot(velocity, facing);

    // The path the 2D input used for this frame or the last; otherwise (it hasn't run) his facing.
    rule.PathX = rule.FacingX;
    rule.PathY = rule.FacingY;
    if (g_frame - g_pathFrame <= 1 && std::isfinite(dot(g_pathDirection, g_pathDirection)))
    {
        rule.PathX = dot(right, g_pathDirection);
        rule.PathY = dot(back, g_pathDirection);
        if (node != 0 && std::isfinite(dot(facing, facing)))
            rule.PathForward = dot(g_pathDirection, facing);
    }

    return rule;
}

// Whether direction a (as drawn) is on the side of the stick Sonic faces, where M-Speed can be. All of it if he faces
// straight up or down.
static bool IsFacingSide(const MSpeedRule& rule, float cosine, float sine)
{
    return std::hypot(rule.FacingX, rule.FacingY) < 0.001f || cosine * rule.FacingX + sine * rule.FacingY > 0.0f;
}

// The highest aim (the input's part along Sonic's facing times the target) that M-Speeds, or 0.
static float GetMSpeedMaxAim(const MSpeedRule& rule)
{
    if (rule.Off || !(rule.AlongFacing < rule.Target))
        return 0.0f;

    return rule.AlongFacing > M_SPEED_STILL_MIN_AIM ?
        std::max(M_SPEED_STILL_MIN_AIM, rule.AlongFacing - M_SPEED_STILL_DIFFERENCE) : rule.AlongFacing;
}

// The longest movement input that M-Speeds in direction a (as drawn), or 0.
static float GetMSpeedMaxInput(const MSpeedRule& rule, float cosine, float sine)
{
    float forward = cosine * rule.FacingX + sine * rule.FacingY;
    if (!(forward > 0.0f))
        return 0.0f;

    return std::min(M_SPEED_MAX_INPUT, GetMSpeedMaxAim(rule) / (forward * rule.Target));
}

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

// Where the stick as the game reads it (y up) is: in the M-Speed zone's range of lengths on the side Sonic faces, and
// whether it M-Speeds there.
struct MSpeedInput
{
    bool InZone = false;
    bool Works = false;
};

static MSpeedInput GetMSpeedInput(const MSpeedRule& rule, float gameX, float gameY)
{
    MSpeedInput result;
    float length = std::hypot(gameX, gameY);
    float input = GetMovementInput(length);
    if (!(length > 0.0f) || input < g_movementInputDeadzone || input > M_SPEED_MAX_INPUT)
        return result;

    float cosine = gameX / length;
    float sine = -gameY / length;
    result.InZone = IsFacingSide(rule, cosine, sine);
    result.Works = result.InZone && input <= GetMSpeedMaxInput(rule, cosine, sine);
    return result;
}

// D-Speed, M-Speed in 2D. The 2D air update (sub_82344868) clears flag 27 above an input of 0.3 too and aims with the
// same sub_8234C5F8. The 2D input (CPlayerSpeedPostureInput2DStandard, sub_8234F140, shared with OnPath and ForcePath)
// turns the stick into the world with the input basis, without the curve, then keeps its part along the path
// (sub_8231EE70, the path's direction turned to Sonic's facing side) and drops its part across it, into the screen.
// Outside the boost (flag 14), that's all: the input is the stick's part along the path by the basis, a. In 2D the
// basis keeps turning toward the stick over a few frames (checked in game), so where a lands moves with it. M-Speed's
// rule then holds with a as the whole input: from parameter 12 (0.02) to 0.3, green up to the longest that M-Speeds.
// Below 0.02 (zeroed) or against his front, the 2D air posture brakes instead (sub_8231B0F0, see g_air2DBrakeFrame):
// the same stick can D-Speed on one frame and brake on the next.
static float GetDSpeedInput(const MSpeedRule& rule, float gameX, float gameY)
{
    // The stick as drawn (y down) along the path.
    return gameX * rule.PathX - gameY * rule.PathY;
}

static float GetDSpeedMaxInput(const MSpeedRule& rule)
{
    if (!(rule.PathForward > 0.0f))
        return 0.0f;

    return std::min(M_SPEED_MAX_INPUT, GetMSpeedMaxAim(rule) / (rule.PathForward * rule.Target));
}

static bool HasDSpeedSide(const MSpeedRule& rule)
{
    return std::hypot(rule.PathX, rule.PathY) >= 0.001f;
}

static MSpeedInput GetDSpeedState(const MSpeedRule& rule, float gameX, float gameY)
{
    MSpeedInput result;
    float input = GetDSpeedInput(rule, gameX, gameY);
    if (!HasDSpeedSide(rule) || input < g_movementInputDeadzone || input > M_SPEED_MAX_INPUT)
        return result;

    result.InZone = true;
    result.Works = input <= GetDSpeedMaxInput(rule);
    return result;
}

static float ApplyDeadzone(float value)
{
    return std::abs(value) < STICK_DEADZONE ? 0.0f : value;
}

// How the game reads the stick: each axis loses a deadzone of 8689 and the 24078 left (to 32767) is scaled to 0..1
// (sub_82EA7350 sets them), then the vector is shortened to length 1 if it's longer. On the stick as SDL reports it
// (-1..1, units of 32768): the deadzone, and how far the stick goes past it per unit the game reads.
static constexpr float GAME_STICK_DEADZONE = 8689.0f / 32768.0f;
static constexpr float GAME_STICK_RANGE = 24078.0f / 32768.0f;

// The break-jump glitch: holding the stick through a switch to 2D and a jump, where the game reads it as at
// least 0.1 long (2D movement ignores shorter input) but shorter than 0.3 (NoPadStopWalkPadLengthLimit,
// parameter 140), makes Sonic's velocity explode. Found by replaying a movie with the stick at different
// positions, then matching the limits with the game's values.
static constexpr float BREAK_JUMP_MIN_LENGTH = 0.1f;
static constexpr float BREAK_JUMP_MAX_LENGTH = 0.3f;

// The raw stick positions the game reads as a given length form a square with rounded corners: the
// deadzone cross in the middle, then arcs of radius length * (1 - deadzone) around (±deadzone, ±deadzone).
// The zone is the ring between the two lengths.
// The zones are filled one quad at a time. ImGui fades each filled shape's edges out over a pixel, so where two quads
// meet, both faded edges let the background show through as a thin line. While this is alive, fills get hard edges, which
// meet exactly; the zones' outlines are drawn over their outer edges, smoothed as usual.
struct HardEdgedFills
{
    ImDrawList* DrawList;
    ImDrawListFlags Flags;

    explicit HardEdgedFills(ImDrawList* drawList) : DrawList(drawList), Flags(drawList->Flags)
    {
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    }

    ~HardEdgedFills()
    {
        DrawList->Flags = Flags;
    }
};

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
            float cosine = std::cos(angle) * GAME_STICK_RANGE;
            float sine = std::sin(angle) * GAME_STICK_RANGE;

            inner[count] = { centre.x + (cornerX + cosine * minLength) * scale, centre.y + (cornerY + sine * minLength) * scale };
            outer[count] = { centre.x + (cornerX + cosine * maxLength) * scale, centre.y + (cornerY + sine * maxLength) * scale };
        }
    }

    // The ring isn't convex, so fill it one quad at a time; the gaps between quadrants are the straight sides.
    {
        HardEdgedFills hardEdges(drawList);
        for (int i = 0; i < POINTS; i++)
        {
            int next = (i + 1) % POINTS;
            drawList->AddQuadFilled(outer[i], outer[next], inner[next], inner[i], fill);
        }
    }

    drawList->AddPolyline(outer, POINTS, outline, ImDrawFlags_Closed, Scale(1));
    drawList->AddPolyline(inner, POINTS, outline, ImDrawFlags_Closed, Scale(1));
}

// A zone's edge in one direction from the middle of a quadrant's corner: where it starts, where M-Speed stops working
// and where it ends. Works and Fails say whether there's green or red there; Clipped, that it ends at the stick's circle.
struct ZoneSample
{
    bool InZone;
    bool Works;
    bool Fails;
    ImVec2 Inner;
    ImVec2 Middle;
    ImVec2 Outer;
    bool Clipped = false;
};

// Fills a zone sampled around the stick, quadrant by quadrant (between two quadrants, at the same angle, its straight
// sides): green from its start to where M-Speed stops working, red from there to its end. Bright where the stick is.
static void DrawZoneSamples(ImDrawList* drawList, const std::vector<ZoneSample>& samples, const MSpeedInput& stick)
{
    ImU32 works = stick.Works ? IM_COL32(0, 255, 0, 200) : IM_COL32(0, 255, 0, 80);
    ImU32 fails = stick.InZone && !stick.Works ? IM_COL32(255, 40, 40, 200) : IM_COL32(255, 40, 40, 80);
    ImU32 outline = stick.InZone ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 160);

    // Start outside the zone, so each part of it is drawn in one go.
    size_t count = samples.size();
    size_t start = 0;
    while (start < count && samples[start].InZone)
        start++;

    std::vector<ImVec2> inner;
    std::vector<ImVec2> outer;
    const ZoneSample* previous = nullptr;
    auto flush = [&]()
    {
        if (outer.size() >= 2)
        {
            // The outline: the outer edge, then back along the inner one.
            std::vector<ImVec2> line(outer);
            line.insert(line.end(), inner.rbegin(), inner.rend());
            drawList->AddPolyline(line.data(), int(line.size()), outline, ImDrawFlags_Closed, Scale(1));
        }

        inner.clear();
        outer.clear();
        previous = nullptr;
    };

    for (size_t step = 0; step <= count; step++)
    {
        const ZoneSample& sample = samples[(start + step) % count];
        if (!sample.InZone)
        {
            flush();
            continue;
        }

        if (previous != nullptr)
        {
            HardEdgedFills hardEdges(drawList);
            if (previous->Works || sample.Works)
                drawList->AddQuadFilled(previous->Middle, sample.Middle, sample.Inner, previous->Inner, works);

            if (previous->Fails || sample.Fails)
                drawList->AddQuadFilled(previous->Outer, sample.Outer, sample.Middle, previous->Middle, fails);
        }

        inner.push_back(sample.Inner);
        outer.push_back(sample.Outer);
        previous = &sample;
    }

    flush();
}

// The D-Speed zone: the stick positions whose part along the path (by the input basis as it is now) is from the
// deadzone's input to 0.3, a strip across the stick, as far as the stick's circle. It moves as the basis turns. Sampled
// like the M-Speed zone, in the stick as the game reads it (around each quadrant's corner of the deadzone cross); where
// the strip ends or reaches the circle inside a quadrant, the edge is found exactly, so its corners aren't cut.
static void DrawDSpeedZone(ImDrawList* drawList, ImVec2 centre, float scale, const MSpeedRule& rule, const MSpeedInput& stick)
{
    constexpr int ARC_SEGMENTS = 24;
    constexpr int EDGE_STEPS = 24;

    float shrink = GAME_STICK_RANGE;
    float maxInput = GetDSpeedMaxInput(rule);
    bool hasSide = HasDSpeedSide(rule);

    auto sample = [&](int quadrant, float angle)
    {
        float cornerX = (quadrant == 0 || quadrant == 3) ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cornerY = quadrant < 2 ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cosine = std::cos(angle);
        float sine = std::sin(angle);
        float forward = cosine * rule.PathX + sine * rule.PathY;

        // How far the game's stick goes this way before the raw stick leaves its circle (|corner + direction *
        // shrink * length| = 1), and at most 1.
        float b = (cornerX * cosine + cornerY * sine) * shrink;
        float c = cornerX * cornerX + cornerY * cornerY - 1.0f;
        float edge = std::min(1.0f, (-b + std::sqrt(b * b - shrink * shrink * c)) / (shrink * shrink));

        ZoneSample result{};
        if (!hasSide || !(forward > 0.0f))
            return result;

        float innerLength = g_movementInputDeadzone / forward;
        float outerLength = std::min(M_SPEED_MAX_INPUT / forward, edge);
        if (!(innerLength < outerLength))
            return result;

        float middleLength = std::clamp(maxInput / forward, innerLength, outerLength);
        auto point = [&](float length)
        {
            return ImVec2{ centre.x + (cornerX + cosine * shrink * length) * scale, centre.y + (cornerY + sine * shrink * length) * scale };
        };

        return ZoneSample{ true, middleLength > innerLength, middleLength < outerLength, point(innerLength), point(middleLength),
            point(outerLength), M_SPEED_MAX_INPUT / forward > edge };
    };

    // Outside, ending at 0.3, or ending at the circle.
    auto kind = [](const ZoneSample& sample) { return !sample.InZone ? 0 : sample.Clipped ? 2 : 1; };

    std::vector<ZoneSample> samples;
    for (int quadrant = 0; quadrant < 4; quadrant++)
    {
        float previousAngle = 0.0f;
        int previousKind = 0;
        for (int i = 0; i <= ARC_SEGMENTS; i++)
        {
            float angle = (quadrant + float(i) / ARC_SEGMENTS) * PI / 2;
            ZoneSample current = sample(quadrant, angle);

            // Where the strip ends, or its edge reaches the circle, between two samples: halve the gap until it's found,
            // and add a sample there.
            if (i > 0 && kind(current) != previousKind)
            {
                float from = previousAngle;
                float to = angle;
                for (int step = 0; step < EDGE_STEPS; step++)
                {
                    float middle = (from + to) / 2;
                    if (kind(sample(quadrant, middle)) == previousKind)
                        from = middle;
                    else
                        to = middle;
                }

                samples.push_back(sample(quadrant, previousKind != 0 ? from : to));
            }

            samples.push_back(current);
            previousAngle = angle;
            previousKind = kind(current);
        }
    }

    DrawZoneSamples(drawList, samples, stick);
}

// The M-Speed zone: like the length zone, from the deadzone's input to 0.3, on the side of the stick Sonic faces. Green up
// to the longest input that M-Speeds in each direction, red past it (all of it where nothing does). Bright where the
// stick is.
static void DrawMSpeedZone(ImDrawList* drawList, ImVec2 centre, float scale, const MSpeedRule& rule, const MSpeedInput& stick)
{
    constexpr int ARC_SEGMENTS = 24;

    float minLength = GetStickLengthForMovementInput(g_movementInputDeadzone);
    float maxLength = GetStickLengthForMovementInput(M_SPEED_MAX_INPUT);

    std::vector<ZoneSample> samples;
    for (int quadrant = 0; quadrant < 4; quadrant++)
    {
        float cornerX = (quadrant == 0 || quadrant == 3) ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cornerY = quadrant < 2 ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;

        for (int i = 0; i <= ARC_SEGMENTS; i++)
        {
            float angle = (quadrant + float(i) / ARC_SEGMENTS) * PI / 2;
            float cosine = std::cos(angle);
            float sine = std::sin(angle);
            float maxInput = GetMSpeedMaxInput(rule, cosine, sine);
            float middleLength = GetStickLengthForMovementInput(std::clamp(maxInput, g_movementInputDeadzone, M_SPEED_MAX_INPUT));

            float shrink = GAME_STICK_RANGE;
            auto point = [&](float length)
            {
                return ImVec2{ centre.x + (cornerX + cosine * shrink * length) * scale, centre.y + (cornerY + sine * shrink * length) * scale };
            };

            samples.push_back({ IsFacingSide(rule, cosine, sine), maxInput > g_movementInputDeadzone, maxInput < M_SPEED_MAX_INPUT,
                point(minLength), point(middleLength), point(maxLength) });
        }
    }

    DrawZoneSamples(drawList, samples, stick);
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

    // Centred on the letters themselves: the text's box has room under the baseline and beside the letters, which put
    // capitals high and to one side. ImGui truncates text positions to whole pixels, so the position is rounded instead.
    auto drawLabel = [&](ImVec2 centre, const char* label, ImU32 colour)
    {
        float scale = size / font->FontSize;
        float penX = 0.0f;
        ImVec2 min = { FLT_MAX, FLT_MAX };
        ImVec2 max = { -FLT_MAX, -FLT_MAX };
        for (const char* c = label; *c != '\0'; c++)
        {
            const ImFontGlyph* glyph = font->FindGlyph(ImWchar(*c));
            if (glyph == nullptr)
                continue;

            min = { std::min(min.x, penX + glyph->X0 * scale), std::min(min.y, glyph->Y0 * scale) };
            max = { std::max(max.x, penX + glyph->X1 * scale), std::max(max.y, glyph->Y1 * scale) };
            penX += glyph->AdvanceX * scale;
        }

        if (min.x > max.x)
            return;

        ImVec2 pos = { std::round(centre.x - (min.x + max.x) / 2), std::round(centre.y - (min.y + max.y) / 2) };
        drawList->AddText(font, size, pos, colour, label);
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
    // Sonic has two rows more, for the homing attack and the boost.
    ImVec2 panelMin = { panelMax.x - width, centre.y - radius - Scale(werehog ? 122 : 162) };
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
        // Split from the full velocity along Sonic's up vector: the game's own horizontal velocity (+0x220) stops at
        // the max speed (param 253, 145), while he can move faster than that (the break-jump, M-Speed).
        float y;
        total = VectorLength(context + VELOCITY_OFFSET, y);
        float upX = LoadFloat(context + UP_OFFSET);
        float upY = LoadFloat(context + UP_OFFSET + 4);
        float upZ = LoadFloat(context + UP_OFFSET + 8);
        float upLength = std::sqrt(upX * upX + upY * upY + upZ * upZ);
        if (upLength > 0.5f && std::isfinite(upLength))
        {
            vertical = (LoadFloat(context + VELOCITY_OFFSET) * upX + y * upY + LoadFloat(context + VELOCITY_OFFSET + 8) * upZ) / upLength;
            speed = std::sqrt(std::max(total * total - vertical * vertical, 0.0f));
        }
        else
        {
            speed = VectorLength(context + HORIZONTAL_VELOCITY_OFFSET, y);
            vertical = VectorLength(context + VERTICAL_VELOCITY_OFFSET, y);
            if (y < 0)
                vertical = -vertical;
        }
    }

    DrawText(drawList, font, Scale(18), { left, panelMin.y + Scale(6) }, IM_COL32_WHITE, fmt::format("Speed {:.2f}", speed));
    DrawText(drawList, font, Scale(11), { left, panelMin.y + Scale(28) }, IM_COL32(220, 220, 220, 255),
        fmt::format("Total {:.2f}   Vertical {:.2f}", total, vertical));

    // What the player is doing. For Sonic, then what a press entered now would do: whether the next update checks
    // for the jump or stomp button. Both need a new press, holding the button doesn't count.
    float row = panelMin.y + Scale(46);
    float pressRow = row + Scale(20);
    // Sonic has three more rows of presses, for the quick step, the homing attack and the boost; the Werehog has a row
    // under his state instead, for when his attack ends.
    float quickStepRow = pressRow + Scale(20);
    float homingRow = quickStepRow + Scale(20);
    float boostRow = homingRow + Scale(20);
    float stateRow = (werehog ? pressRow : boostRow) + Scale(20);
    float attackRow = stateRow + Scale(20);

    uint32_t state = GetCurrentState(context);
    std::string stateName = state != 0 ? GetStateName(state) : "?";
    float stateTime = state != 0 ? LoadFloat(state + STATE_TIME_OFFSET) : 0.0f;
    float deltaTime = std::max(float(App::s_deltaTime), 0.001f);

    // Over the hint rings: when each can be touched.
    CollisionOverlay::DrawHintRings(deltaTime);

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

        // What a press entered now would do: the next update reads it. A state keeps trying what it tried during this
        // frame's update, and a state entered during it tries what its update starts with.
        bool entered = state != g_sonicPreviousStateObject;
        bool outOfControl = GetFlag(context, STATE_FLAGS_OFFSET, OUT_OF_CONTROL_FLAG);
        bool groundActionsNext = grounded && !outOfControl && entered && StartsWithGroundActions(state);

        // A quick step runs the ground actions from a frame of its animation on: in how many updates. (The run quick
        // step is still out of control then, and ends instead.)
        bool quickStepping = stateName == "QuickStep" || stateName == "RunQuickStep";
        int quickStepUpdates = -1;
        if (quickStepping)
        {
            quickStepUpdates = stateName == "QuickStep" ? GetQuickStepFramesLeft(QUICK_STEP_ACTIONS_FRAME, 0, deltaTime) :
                GetQuickStepFramesLeft(RUN_QUICK_STEP_ACTIONS_FRAME, RUN_QUICK_STEP_OUT_OF_CONTROL_FRAMES, deltaTime);
        }

        if (quickStepUpdates == 1 && grounded && !outOfControl)
            groundActionsNext = true;

        bool jumpTried = g_jumpCheckFrame == g_frame && (!entered || g_jumpCheckState == state);
        bool jumpBuffered = IsStateIn(state, JUMP_BUFFER_STATE_VFTABLES);
        bool canJump = grounded && (jumpTried || groundActionsNext || jumpBuffered);

        // The air boost's state doesn't try the stomp, and stomping doesn't either.
        bool stompChecked = g_stompCheckFrame == g_frame;
        bool stompTried = stompChecked && !g_stompDisabledByHop && !g_stompBlocked && stateName != "AirBoost" && !stomping;

        // During a short hop: in how many updates the stomp will be accepted. The hop adds the frame's time
        // (in single precision) to its own, and only then compares it and tries the stomp. Leaving the hop
        // (for an air boost, say) clears the flag right away instead. The air boost's state ends on the update after
        // the boost button is let go (CSonicStateAirBoost, sub_82390198): after a one-frame tap, the stomp is accepted
        // 2 frames after the boost (jump, air boost, a frame, stomp in RooftopRunAct1.ltm).
        bool hopping = stateName == "JumpShort" || stateName == "JumpHurdle";
        int stompUpdates = -1;
        if (hopping && GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG))
        {
            float time = stateTime;
            stompUpdates = 0;
            do
            {
                time += deltaTime;
                stompUpdates++;
            } while (time <= g_stompingDisableTime && stompUpdates < 99);
        }
        else if (!grounded && stateName == "AirBoost")
        {
            stompUpdates = AIR_BOOST_STOMP_FRAMES;
        }
        else if (!grounded && entered && g_sonicPreviousState == "AirBoost")
        {
            // The air boost ended during this frame's update: the new state tries the stomp.
            stompUpdates = 1;
        }

        // A state entered during this frame's update whose update calls the air actions tries the stomp on the next (like
        // SpecialJump after the zipline's HangOn, which never tries it). A hop that sets the stomp lock on entering is
        // counted down above instead.
        bool stompNext = entered && IsStateIn(state, AIR_ACTIONS_STATE_VFTABLES) && !outOfControl &&
            !GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG) && !IsActionBlocked(context);

        bool canStomp = !grounded && !stomping && (stompTried || stompNext || (stompUpdates == 1 && !IsActionBlocked(context)));

        DrawIndicator(drawList, font, { column2, row }, indicatorWidth, "SLIDING", IM_COL32(255, 200, 60, 230), sliding);
        DrawIndicator(drawList, font, { column3, row }, indicatorWidth, "STOMPING", IM_COL32(255, 110, 80, 230), stomping);

        // The movement mode, where the Werehog has the stage time. Diving (CSonicPostureDiving) keeps the table it
        // started with.
        auto movementMode = GetMovementMode(context);
        std::string mode = fmt::format("{} MODE", stateName.starts_with("Diving") ? "DIVING" : movementMode != nullptr ? movementMode->Name : "?");
        DrawText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), mode), panelMin.y + Scale(10) },
            IM_COL32(220, 220, 220, 255), mode);

        DrawIndicator(drawList, font, { left, pressRow }, indicatorWidth, "CAN JUMP", IM_COL32(80, 230, 80, 255), canJump, true);
        DrawIndicator(drawList, font, { column2, pressRow }, indicatorWidth, "CAN STOMP", IM_COL32(80, 230, 80, 255), canStomp, true);

        // Otherwise, how many frames later a press would do it.
        if (!canStomp && stompUpdates > 1)
            DrawText(drawList, font, Scale(11), { column3, pressRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", stompUpdates - 1));

        // Whether LB or RB would quick-step: the ground actions try it when no jump was taken. It also needs a new press.
        // During a quick step: how many frames later the next one can start.
        bool quickStepTried = g_quickStepCheckFrame == g_frame && (!entered || g_quickStepCheckState == state) &&
            !g_quickStepDisabled && !g_quickStepBlocked;
        bool quickStepFree = LoadU8(context + QUICK_STEP_DISABLED_OFFSET) == 0 && !IsActionBlocked(context);
        bool canQuickStep = grounded && (quickStepTried || (groundActionsNext && quickStepFree));
        DrawIndicator(drawList, font, { left, quickStepRow }, 2 * indicatorWidth + gap, "CAN QUICKSTEP", IM_COL32(80, 230, 80, 255), canQuickStep, true);

        if (!canQuickStep && quickStepUpdates > 1)
            DrawText(drawList, font, Scale(11), { column3, quickStepRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", quickStepUpdates - 1));

        // Whether A (X with XButtonHoming) would homing attack: the next update's air actions run the target search, then
        // take the press. This goes by the target this frame's search kept, as the reticle does; the next search can still
        // find one (a press works a frame before the reticle shows: estimated below), change or drop it. HomingAttackAfter, which the state machine adds the frame's time to
        // before its update, runs the air actions once that time is over parameter 278: in how many updates.
        int homingUpdates = -1;
        if (!grounded && stateName == "HomingAttackAfter" && !std::isnan(g_actionEnableAfterHomingAttack))
        {
            float time = stateTime;
            homingUpdates = 0;
            do
            {
                time += deltaTime;
                homingUpdates++;
            } while (time <= g_actionEnableAfterHomingAttack && homingUpdates < 99);
        }

        // Whether the next update runs the air actions (also for the air boost below).
        bool airActionsRan = !entered && (g_homingCheckFrame == g_frame || g_airBoostCheckFrame == g_frame);
        bool airActionsNext = airActionsRan ||
            (stateName == "HomingAttackAfter" ? homingUpdates == 1 : IsStateIn(state, AIR_ACTIONS_STATE_VFTABLES));
        bool homingAllowed = !outOfControl && !IsActionBlocked(context) && GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_ENABLED_FLAG) &&
            GetFlag(context, STATE_FLAGS_OFFSET, HOMING_ATTACK_ENABLED_FLAG) &&
            !(g_airBoostHoming && GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_DISABLED_FLAG));
        bool homingTarget = LoadU32(context + AIR_BOOST_HOMING_OFFSET) != 0;
        bool homingPress = !grounded && airActionsNext && homingAllowed;
        bool canHomingAttack = homingPress && homingTarget;
        // Without a target yet, the next search may find one before it reads the press (the reticle then shows with the
        // homing attack): only estimated, in yellow.
        bool homingEstimated = homingPress && !homingTarget && EstimateNextHomingTarget(context);
        DrawIndicator(drawList, font, { left, homingRow }, 2 * indicatorWidth + gap, "CAN HOMING ATTACK",
            homingEstimated ? IM_COL32(255, 200, 60, 255) : IM_COL32(80, 230, 80, 255), canHomingAttack || homingEstimated, true);

        // Otherwise, how many frames later a press would do it, or that A would only dash forward, without a target.
        if (homingEstimated)
            DrawText(drawList, font, Scale(11), { column3, homingRow + Scale(2) }, IM_COL32(255, 200, 60, 255), "estimate");
        else if (!canHomingAttack && homingUpdates > 1 && homingAllowed)
            DrawText(drawList, font, Scale(11), { column3, homingRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", homingUpdates - 1));
        else if (homingPress && !homingTarget && !g_airBoostHoming)
            DrawText(drawList, font, Scale(11), { column3, homingRow + Scale(2) }, IM_COL32(170, 170, 170, 255), "air dash");

        // Whether X would boost: on the ground TryBoost, in the air TryAirBoost, with enough energy in the gauge (out of
        // 100). As for the other presses, a state entered during this frame's update tries it if its update starts with it.
        float energy = LoadFloat(context + CHAOS_ENERGY_OFFSET);
        bool boosting = GetFlag(context, STATE_FLAGS_OFFSET, BOOSTING_FLAG);
        bool canBoost;
        if (grounded)
        {
            bool boostTried = g_boostCheckFrame == g_frame && (!entered || g_boostCheckState == state) && g_boostAllowed;
            // Sliding's update (sub_823997C8) only tries the boost while ctx+1056 (a length the posture's ground check,
            // sub_8232F370, stores) is at least 1, so on its first frames it can skip it during this frame's update and try
            // on the next. This goes by the value as the HUD sees it, assuming the next update reads the same one.
            bool slideBoostNext = stateName == "Sliding" && !outOfControl && LoadFloat(context + SLIDE_BOOST_LENGTH_OFFSET) >= 1.0f;
            bool boostNext = (groundActionsNext || (entered && IsStateIn(state, BOOST_STATE_VFTABLES)) || slideBoostNext) &&
                IsBoostAllowed(context);
            canBoost = energy > g_boostEnableEnergy && (boostTried || boostNext);
        }
        else
        {
            // The air actions do nothing while Sonic is on the ground or out of control (sub_82316118, vfunc 56), so this
            // goes by the state after this frame's update: a hop that just left the ground, or the end of an
            // out-of-control (RooftopRunAct1.ltm, frames 1159 and 3037). HomingAttackAfter waits for parameter 278 first.
            canBoost = energy > g_airBoostEnableEnergy && airActionsNext && !outOfControl && IsAirBoostAllowed(context);
        }

        DrawIndicator(drawList, font, { left, boostRow }, indicatorWidth, "CAN BOOST", IM_COL32(80, 230, 80, 255), canBoost, true);
        DrawIndicator(drawList, font, { column2, boostRow }, indicatorWidth, "BOOSTING", IM_COL32(90, 170, 255, 230), boosting);

        std::string gauge = fmt::format("Boost {:.2f}", energy);
        DrawText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), gauge), boostRow + Scale(2) },
            IM_COL32(220, 220, 220, 255), gauge);

        // The radar, above the panel, while Sonic is alive (not in DrowningDead, NormalDamageDead, PressDead...).
        if (stateName.find("Dead") == std::string::npos)
        {
            float radarSize = std::min(width, panelMin.y - Scale(28));
            if (radarSize > Scale(80))
            {
                CollisionOverlay::DrawRadar(panelMax.x - radarSize, panelMin.y - Scale(8) - radarSize, radarSize, context, grounded,
                    stateName.starts_with("Diving"), g_frame, deltaTime);
            }
        }

        g_sonicPreviousState = stateName;
        g_sonicPreviousStateObject = state;
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
    // goes through the curve, D-Speed for 2D, where its part along the path counts.
    std::string zoneText;
    bool mSpeedBlocked = false;
    if (!werehog)
    {
        DrawLengthZone(drawList, centre, radius, BREAK_JUMP_MIN_LENGTH, BREAK_JUMP_MAX_LENGTH,
            breakJump ? IM_COL32(255, 150, 30, 200) : IM_COL32(255, 150, 30, 70), breakJump ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 120));

        auto movementMode = GetMovementMode(context);
        bool sideView = movementMode != nullptr && movementMode->SideView;
        const char* zoneName = sideView ? "D-Speed" : "M-Speed";

        MSpeedRule rule = GetMSpeedRule(context, grounded);
        MSpeedInput input = sideView ? GetDSpeedState(rule, gameX, gameY) : GetMSpeedInput(rule, gameX, gameY);

        // In 2D air, the Brake state ran during this frame's update instead of the one D-Speed happens in.
        bool airBrake = sideView && !grounded && g_air2DBrakeFrame == g_frame;
        if (airBrake)
        {
            input.Works = false;
            zoneText = input.InZone ? "D-Speed blocked (air brake)" : "Air brake";
            mSpeedBlocked = true;
        }
        else if (input.Works)
        {
            zoneText = zoneName;
        }
        else if (input.InZone)
        {
            zoneText = fmt::format("{} blocked", zoneName);
            mSpeedBlocked = true;
        }

        if (sideView)
            DrawDSpeedZone(drawList, centre, radius, rule, input);
        else
            DrawMSpeedZone(drawList, centre, radius, rule, input);
    }

    ImVec2 stick = { centre.x + x * radius, centre.y + y * radius };
    drawList->AddLine(centre, stick, IM_COL32(255, 60, 60, 255), Scale(2));
    drawList->AddCircleFilled(stick, Scale(4.5f), IM_COL32(255, 60, 60, 255));

    if (!werehog)
    {
        bool zone = !zoneText.empty();
        if (breakJump)
            zoneText += zoneText.empty() ? "Break-jump" : " + Break-jump";

        ImU32 zoneColour = mSpeedBlocked ? IM_COL32(255, 80, 80, 255) : zone ? IM_COL32(80, 255, 80, 255) :
            breakJump ? IM_COL32(255, 160, 40, 255) : IM_COL32(200, 200, 200, 255);
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
