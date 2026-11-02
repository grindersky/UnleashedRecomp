#include "hud_predictions.h"
#include "hud_state.h"

#include <ui/collision_overlay.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace Tas;

static float Dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// --- M-Speed and D-Speed ---

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
static constexpr float M_SPEED_STILL_MIN_AIM = 0.25f;
static constexpr float M_SPEED_STILL_DIFFERENCE = 0.1f;

MSpeedRule Tas::GetMSpeedRule(uint32_t context, bool grounded)
{
    const SonicChecks& checks = GetSonicChecks();
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

    float speed = std::sqrt(Dot3(velocity, velocity));

    // The stick's X goes along the basis' first row and its Y against the third, and Y is drawn upside down.
    if (node != 0 && std::isfinite(Dot3(facing, facing)))
    {
        rule.FacingX = Dot3(right, facing);
        rule.FacingY = Dot3(back, facing);
    }

    rule.Target = std::max(checks.InAirMaxAccelVelocity, speed);
    rule.AlongFacing = Dot3(velocity, facing);

    // The path the 2D input used for this frame or the last; otherwise (it hasn't run) his facing.
    rule.PathX = rule.FacingX;
    rule.PathY = rule.FacingY;
    if (GetHudFrame() - checks.PathFrame <= 1 && std::isfinite(Dot3(checks.PathDirection, checks.PathDirection)))
    {
        rule.PathX = Dot3(right, checks.PathDirection);
        rule.PathY = Dot3(back, checks.PathDirection);
        if (node != 0 && std::isfinite(Dot3(facing, facing)))
            rule.PathForward = Dot3(checks.PathDirection, facing);
    }

    return rule;
}

// All of the stick if he faces straight up or down.
bool Tas::IsFacingSide(const MSpeedRule& rule, float cosine, float sine)
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

float Tas::GetMSpeedMaxInput(const MSpeedRule& rule, float cosine, float sine)
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

float Tas::GetStickLengthForMovementInput(float input)
{
    return std::cbrt(input) / MOVEMENT_INPUT_SCALE;
}

MSpeedInput Tas::GetMSpeedInput(const MSpeedRule& rule, float gameX, float gameY)
{
    MSpeedInput result;
    float length = std::hypot(gameX, gameY);
    float input = GetMovementInput(length);
    if (!(length > 0.0f) || input < GetSonicChecks().MovementInputDeadzone || input > M_SPEED_MAX_INPUT)
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
// Below 0.02 (zeroed) or against his front, the 2D air posture brakes instead (sub_8231B0F0, see
// SonicChecks::Air2DBrakeFrame): the same stick can D-Speed on one frame and brake on the next.
static float GetDSpeedInput(const MSpeedRule& rule, float gameX, float gameY)
{
    // The stick as drawn (y down) along the path.
    return gameX * rule.PathX - gameY * rule.PathY;
}

float Tas::GetDSpeedMaxInput(const MSpeedRule& rule)
{
    if (!(rule.PathForward > 0.0f))
        return 0.0f;

    return std::min(M_SPEED_MAX_INPUT, GetMSpeedMaxAim(rule) / (rule.PathForward * rule.Target));
}

bool Tas::HasDSpeedSide(const MSpeedRule& rule)
{
    return std::hypot(rule.PathX, rule.PathY) >= 0.001f;
}

MSpeedInput Tas::GetDSpeedState(const MSpeedRule& rule, float gameX, float gameY)
{
    MSpeedInput result;
    float input = GetDSpeedInput(rule, gameX, gameY);
    if (!HasDSpeedSide(rule) || input < GetSonicChecks().MovementInputDeadzone || input > M_SPEED_MAX_INPUT)
        return result;

    result.InZone = true;
    result.Works = input <= GetDSpeedMaxInput(rule);
    return result;
}

// --- Quick steps ---

// Quick steps: the ground actions (the context's vfunc 36, sub_82386FF8) call TryQuickStep (sub_82386E60) when Sonic is on
// the ground. It quick-steps if LB or RB was just pressed, unless the action blocker is set or QUICK_STEP_DISABLED_OFFSET
// is. Above a speed (parameter 240) it's a CStateRunQuickStep, below a CStateQuickStep. The quick steps only run the
// ground actions, and so allow the next quick step, from a frame of their animation on: their updates (sub_823968D8 and
// sub_82397370) compare the animation frame (sub_82307D38) with these.
static constexpr float QUICK_STEP_ACTIONS_FRAME = 20.0f;
static constexpr float RUN_QUICK_STEP_ACTIONS_FRAME = 8.0f;
// How many frames the run quick step's out-of-control lasts after its animation reaches that frame.
static constexpr int RUN_QUICK_STEP_OUT_OF_CONTROL_FRAMES = 1;

// During a quick step, in how many frames its update will find the animation at the frame it runs the ground actions
// from, and so allow the next quick step, or -1. The animation's local time is stepped like Havok steps it: by the
// frame's time times the playback speed, in single precision (or by how much it moved on the last frame, if that
// didn't match). A run quick step also puts Sonic out of control (CPlayerSpeedPosturePluginOutOfControl, flag 3, which
// makes the ground actions do nothing) until its animation reaches that frame (sub_8238D578): that lasts through the
// update that gets there, so the next quick step comes a frame later (RooftopRunAct1.ltm, frames 2446-2458).
static int GetQuickStepFramesLeft(float actionsFrame, int outOfControlFrames, float deltaTime)
{
    const SonicChecks& checks = GetSonicChecks();
    uint32_t animation = checks.QuickStepAnimation;
    if (checks.QuickStepUpdateFrame != GetHudFrame() || LoadU32(animation) != ANIMATION_CONTROL_SINGLE_VFTABLE)
        return -1;

    if (checks.QuickStepAnimationFrame >= actionsFrame)
        return outOfControlFrames > 0 ? outOfControlFrames : -1;

    uint32_t control = LoadU32(animation + ANIMATION_CONTROL_OFFSET);
    float start = LoadFloat(animation + ANIMATION_START_TIME_OFFSET);
    float duration = LoadFloat(LoadU32(LoadU32(control + ANIMATION_CONTROL_BINDING_OFFSET)) + ANIMATION_DURATION_OFFSET);
    float elapsed = checks.QuickStepLocalTime - start;
    if (!(elapsed > 0.0f) || !(duration > 0.0f))
        return -1;

    // The animation's frame count - 1, from the frame the update got.
    float frames = std::round(checks.QuickStepAnimationFrame * duration / elapsed);
    float rate = frames / duration;

    float step = deltaTime * LoadFloat(control + ANIMATION_CONTROL_SPEED_OFFSET);
    float previous = checks.QuickStepPreviousLocalTime;
    if (!std::isnan(previous))
    {
        float expected = previous + step;
        if (expected != checks.QuickStepLocalTime)
            step = checks.QuickStepLocalTime - previous;
    }

    // The animation may have moved on since the update.
    float time = checks.QuickStepLocalTime;
    float drawnTime = LoadFloat(control + ANIMATION_CONTROL_TIME_OFFSET);
    for (int framesLeft = 1; framesLeft < 100; framesLeft++)
    {
        if (framesLeft == 1 && drawnTime != checks.QuickStepLocalTime)
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

// --- The homing attack's estimate ---

// Homing attacks: the air actions first call TryHomingAttack (sub_823778D8). Unless the action blocker is set, and with
// flags 19 and 17 (set by JumpShort, GrindJumpShort and the context's vfunc 32, cleared by HomingAttack, AirBoost and
// damage), it runs the target search (sub_82375FE0), which keeps, changes or drops the target at HOMING_TARGET_OFFSET and
// starts the reticle (CSonicHudHomingAttack at +4260, sub_823A3748), then changes to HomingAttack when A is just pressed.
// Without a target, HomingAttack dashes forward (its Enter, sub_82393F48). With parameter 294 (XButtonHoming, off with the
// HomingAttackOnJump code) it returns after the search, and TryAirBoost homes on X instead, when there's a target.
// The search tries two directions: the one sub_82373E58 picks (from the stick), then the facing. For each, it places the
// phantom (+3448, +3464 in 2D) at Sonic, turned to it (sub_82494318), and asks every object whose collision the phantom
// touches for its priority (sub_823751A8) and position (sub_82375258) (sub_8230E990). The target is one in a 75° cone
// around the phantom's forward (+Z) from Sonic's position, in sight (a raycast), and the nearest or the one of highest
// priority. The phantom is a view frustum (sub_82379718) looking along its +Z from the point at LOCKON_FRUSTUM_EYE, from
// parameters 297 (aspect), 298 (vertical field of view, degrees), 299 (near) and 300 (far).
static constexpr float HOMING_CONE_ANGLE = 1.3089969f;

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

// Whether the phantom's rows are its axes (else its columns are): decided by which turns to the facing, the second
// direction searched.
static bool g_homingPoseRows = true;

// How the facing's phantom sits on Sonic (learnt from the searches): its axes and its position from his, in his frame.
static bool g_homingRelationValid;
static float g_homingRelationAxes[3][3];
static float g_homingRelationOrigin[3];

static bool EstimateNextHomingTarget(uint32_t context)
{
    const SonicChecks& checks = GetSonicChecks();
    bool fresh = checks.HomingSearchFrame == GetHudFrame() && checks.HomingSearchPoseCount > 0;
    if (!fresh && !g_homingRelationValid)
        return false;

    const float* frustum = checks.LockonFrustums[LoadU8(context + SIDE_VIEW_OFFSET) != 0 ? 1 : 0];
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
        delta[i] = position[i] - checks.HomingSearchPosition[i];
        facing[i] = LoadFloat(node + MATRIX_NODE_FACING_OFFSET + i * 4);
        eye[i] = LoadFloat(LOCKON_FRUSTUM_EYE + i * 4);
    }

    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    // The phantoms' axes and positions, as the next search would place them.
    int poseCount = fresh ? checks.HomingSearchPoseCount : 1;
    float axes[2][3][3], origins[2][3];
    if (fresh)
    {
        if (poseCount == 2)
        {
            const float* pose = checks.HomingSearchPoses[1];
            float row[3] = { pose[8], pose[9], pose[10] };
            float column[3] = { pose[2], pose[6], pose[10] };
            g_homingPoseRows = dot(row, facing) >= dot(column, facing);
        }

        for (int k = 0; k < poseCount; k++)
        {
            const float* pose = checks.HomingSearchPoses[k];
            for (int axis = 0; axis < 3; axis++)
            {
                for (int i = 0; i < 3; i++)
                    axes[k][axis][i] = g_homingPoseRows ? pose[axis * 4 + i] : pose[i * 4 + axis];
            }

            for (int i = 0; i < 3; i++)
                origins[k][i] = pose[12 + i] + delta[i];
        }

        // How the last direction (the facing, when there are two) sits on Sonic: its axes and position in his frame.
        const float* pose = checks.HomingSearchPoses[poseCount - 1];
        float offset[3] = { pose[12] - checks.HomingSearchPosition[0], pose[13] - checks.HomingSearchPosition[1],
            pose[14] - checks.HomingSearchPosition[2] };
        for (int j = 0; j < 3; j++)
        {
            for (int axis = 0; axis < 3; axis++)
                g_homingRelationAxes[axis][j] = dot(axes[poseCount - 1][axis], checks.HomingSearchRotation[j]);

            g_homingRelationOrigin[j] = dot(offset, checks.HomingSearchRotation[j]);
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
                rotation[row][i] = LoadFloat(node + MATRIX_NODE_MATRIX_OFFSET + row * 16 + i * 4);
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

// --- Day Sonic's presses ---

// How many frames after an air boost (a one-frame tap) the stomp is accepted.
static constexpr int AIR_BOOST_STOMP_FRAMES = 2;

// Sonic's state on the last drawn frame.
static std::string g_sonicPreviousState;
static uint32_t g_sonicPreviousStateObject;

// What a press entered now would do: the next update reads it. A state keeps trying what it tried during this frame's
// update, and a state entered during it tries what its update starts with: the states in GROUND_ACTIONS_STATE_VFTABLES
// try the jump, then the quick step (Land skips them only on the update that ends its animation, and StompingLand also
// when it has lasted 0.1 s with the stick held past 0.25: then it picks the next state; LandJumpShort skips them on the
// update that ends its animation, sub_82307CB0, or while Sonic is faster than parameter 83: it changes state instead);
// those in AIR_ACTIONS_STATE_VFTABLES the homing attack, the stomp and the air boost; those in BOOST_STATE_VFTABLES the
// boost. The states in JUMP_BUFFER_STATE_VFTABLES jump on the ground from a press buffered on the update before: running
// on water keeps Sonic in Fall but on the ground, where a press jumps on the next update.
SonicPresses Tas::PredictSonicPresses(uint32_t context, uint32_t state, const std::string& stateName, float stateTime, bool grounded,
    float deltaTime)
{
    const SonicChecks& checks = GetSonicChecks();
    uint32_t frame = GetHudFrame();
    SonicPresses presses;

    presses.Sliding = stateName.starts_with("Sliding");
    presses.Stomping = stateName.starts_with("Stomping");

    bool entered = state != g_sonicPreviousStateObject;
    bool outOfControl = GetFlag(context, STATE_FLAGS_OFFSET, OUT_OF_CONTROL_FLAG);
    bool groundActionsNext = grounded && !outOfControl && entered && IsStateIn(state, GROUND_ACTIONS_STATE_VFTABLES);

    // A quick step runs the ground actions from a frame of its animation on: in how many updates. (The run quick
    // step is still out of control then, and ends instead.)
    bool quickStepping = stateName == "QuickStep" || stateName == "RunQuickStep";
    if (quickStepping)
    {
        presses.QuickStepUpdates = stateName == "QuickStep" ? GetQuickStepFramesLeft(QUICK_STEP_ACTIONS_FRAME, 0, deltaTime) :
            GetQuickStepFramesLeft(RUN_QUICK_STEP_ACTIONS_FRAME, RUN_QUICK_STEP_OUT_OF_CONTROL_FRAMES, deltaTime);
    }

    if (presses.QuickStepUpdates == 1 && grounded && !outOfControl)
        groundActionsNext = true;

    bool jumpTried = checks.JumpCheckFrame == frame && (!entered || checks.JumpCheckState == state);
    bool jumpBuffered = IsStateIn(state, JUMP_BUFFER_STATE_VFTABLES);
    presses.CanJump = grounded && (jumpTried || groundActionsNext || jumpBuffered);

    // The air boost's state doesn't try the stomp, and stomping doesn't either.
    bool stompChecked = checks.StompCheckFrame == frame;
    bool stompTried = stompChecked && !checks.StompDisabledByHop && !checks.StompBlocked && stateName != "AirBoost" && !presses.Stomping;

    // During a short hop: in how many updates the stomp will be accepted. The short hops (CStateJumpShort, JumpHurdle) set
    // flag 20 until they have lasted StompingDisableTime (parameter 292, 0.15 s by default): until then, stomping is
    // refused. The hop adds the frame's time (in single precision) to its own, and only then compares it and tries the
    // stomp. Leaving the hop (for an air boost, say) clears the flag right away instead. The air boost's state ends on the
    // update after the boost button is let go (CSonicStateAirBoost, sub_82390198): after a one-frame tap, the stomp is
    // accepted 2 frames after the boost (jump, air boost, a frame, stomp in RooftopRunAct1.ltm).
    bool hopping = stateName == "JumpShort" || stateName == "JumpHurdle";
    if (hopping && GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG))
    {
        float time = stateTime;
        presses.StompUpdates = 0;
        do
        {
            time += deltaTime;
            presses.StompUpdates++;
        } while (time <= checks.StompingDisableTime && presses.StompUpdates < 99);
    }
    else if (!grounded && stateName == "AirBoost")
    {
        presses.StompUpdates = AIR_BOOST_STOMP_FRAMES;
    }
    else if (!grounded && entered && g_sonicPreviousState == "AirBoost")
    {
        // The air boost ended during this frame's update: the new state tries the stomp.
        presses.StompUpdates = 1;
    }

    // A state entered during this frame's update whose update calls the air actions tries the stomp on the next (like
    // SpecialJump after the zipline's HangOn, which never tries it). A hop that sets the stomp lock on entering is
    // counted down above instead.
    bool stompNext = entered && IsStateIn(state, AIR_ACTIONS_STATE_VFTABLES) && !outOfControl &&
        !GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG) && !IsActionBlocked(context);

    presses.CanStomp = !grounded && !presses.Stomping && (stompTried || stompNext || (presses.StompUpdates == 1 && !IsActionBlocked(context)));

    // Whether LB or RB would quick-step: the ground actions try it when no jump was taken. It also needs a new press.
    bool quickStepTried = checks.QuickStepCheckFrame == frame && (!entered || checks.QuickStepCheckState == state) &&
        !checks.QuickStepDisabled && !checks.QuickStepBlocked;
    bool quickStepFree = LoadU8(context + QUICK_STEP_DISABLED_OFFSET) == 0 && !IsActionBlocked(context);
    presses.CanQuickStep = grounded && (quickStepTried || (groundActionsNext && quickStepFree));

    // Whether A (X with XButtonHoming) would homing attack: the next update's air actions run the target search, then take
    // the press. This goes by the target this frame's search kept, as the reticle does; the next search can still find one
    // (a press works a frame before the reticle shows: estimated below), change or drop it. HomingAttackAfter (sub_82394900),
    // which the state machine adds the frame's time to before its update, runs the air actions once that time is over
    // parameter 278: in how many updates.
    if (!grounded && stateName == "HomingAttackAfter" && !std::isnan(checks.ActionEnableAfterHomingAttack))
    {
        float time = stateTime;
        presses.HomingUpdates = 0;
        do
        {
            time += deltaTime;
            presses.HomingUpdates++;
        } while (time <= checks.ActionEnableAfterHomingAttack && presses.HomingUpdates < 99);
    }

    // Whether the next update runs the air actions (also for the air boost below).
    bool airActionsRan = !entered && (checks.HomingCheckFrame == frame || checks.AirBoostCheckFrame == frame);
    bool airActionsNext = airActionsRan ||
        (stateName == "HomingAttackAfter" ? presses.HomingUpdates == 1 : IsStateIn(state, AIR_ACTIONS_STATE_VFTABLES));
    presses.HomingAllowed = !outOfControl && !IsActionBlocked(context) && GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_ENABLED_FLAG) &&
        GetFlag(context, STATE_FLAGS_OFFSET, HOMING_ATTACK_ENABLED_FLAG) &&
        !(checks.AirBoostHoming && GetFlag(context, STATE_FLAGS_OFFSET, AIR_BOOST_DISABLED_FLAG));
    presses.HomingTarget = LoadU32(context + HOMING_TARGET_OFFSET) != 0;
    presses.HomingPress = !grounded && airActionsNext && presses.HomingAllowed;
    presses.CanHomingAttack = presses.HomingPress && presses.HomingTarget;
    // Without a target yet, the next search may find one before it reads the press (the reticle then shows with the
    // homing attack): only estimated.
    presses.HomingEstimated = presses.HomingPress && !presses.HomingTarget && EstimateNextHomingTarget(context);

    // Whether X would boost: on the ground TryBoost, in the air TryAirBoost, with enough energy in the gauge (out of
    // 100). As for the other presses, a state entered during this frame's update tries it if its update starts with it.
    presses.Energy = LoadFloat(context + CHAOS_ENERGY_OFFSET);
    presses.Boosting = GetFlag(context, STATE_FLAGS_OFFSET, BOOSTING_FLAG);
    if (grounded)
    {
        bool boostTried = checks.BoostCheckFrame == frame && (!entered || checks.BoostCheckState == state) && checks.BoostAllowed;
        // Sliding's update (sub_823997C8) only tries the boost while ctx+1056 (a length the posture's ground check,
        // sub_8232F370, stores) is at least 1, so on its first frames it can skip it during this frame's update and try
        // on the next. This goes by the value as the HUD sees it, assuming the next update reads the same one.
        bool slideBoostNext = stateName == "Sliding" && !outOfControl && LoadFloat(context + SLIDE_BOOST_LENGTH_OFFSET) >= 1.0f;
        bool boostNext = (groundActionsNext || (entered && IsStateIn(state, BOOST_STATE_VFTABLES)) || slideBoostNext) &&
            IsBoostAllowed(context);
        presses.CanBoost = presses.Energy > checks.BoostEnableEnergy && (boostTried || boostNext);
    }
    else
    {
        // The air actions do nothing while Sonic is on the ground or out of control (sub_82316118, vfunc 56), so this
        // goes by the state after this frame's update: a hop that just left the ground, or the end of an
        // out-of-control (RooftopRunAct1.ltm, frames 1159 and 3037). HomingAttackAfter waits for parameter 278 first.
        presses.CanBoost = presses.Energy > checks.AirBoostEnableEnergy && airActionsNext && !outOfControl && IsAirBoostAllowed(context);
    }

    g_sonicPreviousState = stateName;
    g_sonicPreviousStateObject = state;
    return presses;
}

// --- The Werehog ---

// The Werehog's state and attack action on the last drawn frame, and the frame they started on.
static uint32_t g_evilState;
static uint32_t g_evilAction;
static float g_evilStateTime;
static uint32_t g_evilStateStartFrame;
// The attack animation and its local time on the last drawn frame.
static uint32_t g_evilAnimation;
static float g_evilAnimationTime;

WerehogAttack Tas::PredictWerehogAttack(uint32_t context, uint32_t state, const std::string& stateName, float stateTime, bool grounded,
    float deltaTime)
{
    WerehogAttack attack;
    attack.Dashing = stateName == "Dash";
    attack.Attacking = stateName.starts_with("AttackAction") || stateName == "SuperAttack";
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
            attack.FramesLeft = std::max(1, int(std::ceil((end - time) / step)));

            float endWithStick = duration - LoadFloat(motion + MOTION_END_SKIP_TIME_OFFSET);
            int withStick = 1 + std::max(0, int(std::ceil((endWithStick - time) / step)));
            if (grounded && withStick < attack.FramesLeft)
                attack.FramesLeftWithStick = withStick;
        }

        attack.NextAction = GetSharedString(action + ACTION_KEY_END_OFFSET);
        g_evilAnimationTime = time;
    }

    g_evilAnimation = animation;

    // Whether LB on this frame would guard. The states whose update checks for it, and changes to "Guard":
    // standing, walking, running and landing guard right away; an attack only on the ground, and if its action
    // allows it (sub_823CE5F8). All of them also need sub_823A4420 (an out-of-control check on the player),
    // which isn't replicated, and neither are the rarer cases in Damage and Fall.
    attack.CanGuard = stateName == "Idle" || stateName == "WalkSlowE" || stateName == "WalkE" || stateName == "RunE" ||
        stateName == "Land" || (action != 0 && grounded && LoadU8(action + ACTION_GUARD_OFFSET) != 0);

    // All attacks run in one state: show the attack list's name for the action instead.
    attack.StateLabel = stateName;
    if (action != 0)
    {
        if (std::string actionName = GetSharedString(action + ACTION_NAME_OFFSET); !actionName.empty())
            attack.StateLabel = "Attack " + actionName;
    }

    // Count drawn frames since the state or action changed: some attacks slow the game down, and their state
    // time then advances by less than a frame.
    if (state != g_evilState || action != g_evilAction || stateTime < g_evilStateTime)
        g_evilStateStartFrame = GetHudFrame();

    g_evilState = state;
    g_evilAction = action;
    g_evilStateTime = stateTime;
    attack.StateFrames = int(GetHudFrame() - g_evilStateStartFrame);
    return attack;
}
