#pragma once

// The game's addresses and structure offsets the TAS HUD and the collision overlay read, with where each was found
// (sub_XXXXXXXX: the game function that reads or writes it). Found by reading the game's code (the recompiled
// sources' asm comments, the RTTI) and checked against per-frame dumps; the notes at the code that uses them say how
// the game uses them.

#include <cstdint>

namespace Tas
{
    // --- Player contexts ---

    // Day Sonic's (SWA::Player::CSonicContext; its base CPlayerSpeedContext's constructor is sub_82330188) and the
    // Werehog's (SWA::Player::CEvilSonicContext, constructor sub_823B49D8). Both are built on the same base (sub_8230D620).
    inline constexpr uint32_t SONIC_CONTEXT_VFTABLE = 0x820182C4;
    inline constexpr uint32_t EVIL_SONIC_CONTEXT_VFTABLE = 0x8201DD6C;

    // The player object (both contexts).
    inline constexpr uint32_t PLAYER_OFFSET = 0x100;
    // The player's matrix node (SWA::CMatrixNodeTransform, both contexts).
    inline constexpr uint32_t MATRIX_NODE_OFFSET = 16;
    // Its matrix (its vfunc 2): the axes as rows, then the position, at the feet. The facing is the third row (sub_8230A4E0).
    inline constexpr uint32_t MATRIX_NODE_MATRIX_OFFSET = 128;
    inline constexpr uint32_t MATRIX_NODE_FACING_OFFSET = MATRIX_NODE_MATRIX_OFFSET + 32;
    // The position the homing attack's search reads (sub_823087C8).
    inline constexpr uint32_t MATRIX_NODE_POSITION_OFFSET = 112;

    // Day Sonic's velocity, split like Sonic Generations' CPlayerSpeedContext (from per-frame dumps): the full velocity
    // in world space, the part along the ground (it stops at MaxVelocityFinalMax, parameter 253, 145) and the part along
    // his up vector.
    inline constexpr uint32_t VELOCITY_OFFSET = 0x210;
    inline constexpr uint32_t HORIZONTAL_VELOCITY_OFFSET = 0x220;
    inline constexpr uint32_t VERTICAL_VELOCITY_OFFSET = 0x230;
    inline constexpr uint32_t UP_OFFSET = 0x2F0;
    // The basis the 3D input turns the stick into the world with (sub_82316128): the stick's X along the first row, its
    // Y against the third.
    inline constexpr uint32_t INPUT_BASIS_OFFSET = 0x290;
    // On the ground (sub_82316530 switches to "Fall" when it's false; sub_82316118).
    inline constexpr uint32_t GROUNDED_OFFSET = 0x3C0;
    // The parameter table in use (boost::shared_ptr, set by sub_8230B630): see MOVEMENT_MODES.
    inline constexpr uint32_t PARAMETERS_OFFSET = 0x1F4;
    // What the context reads its parameters by ID with (sub_8245DB60 floats, sub_8245DB18 bools).
    inline constexpr uint32_t PARAMETER_READER_OFFSET = 0x1FC;
    // Two sets of state flags: byte counters in an array at +4 (sub_823171C8 to sub_823171E8).
    inline constexpr uint32_t STATE_FLAGS_OFFSET = 0x4AC;
    inline constexpr uint32_t STATE_FLAGS_2_OFFSET = 0x4B0;
    // Refuses stomps, quick steps, boosts and homing attacks unless flag 7 of the second set is on (sub_8231B7D8).
    inline constexpr uint32_t ACTION_BLOCKER_OFFSET = 0xCC8;
    inline constexpr uint32_t ACTION_BLOCKER_OVERRIDE_FLAG = 7;
    // Refuses quick steps (sub_82316208, read by TryQuickStep sub_82386E60).
    inline constexpr uint32_t QUICK_STEP_DISABLED_OFFSET = 0x4B4;
    // The boost gauge, the chaos energy, out of 100 (sub_82316B70).
    inline constexpr uint32_t CHAOS_ENERGY_OFFSET = 0x53C;
    // A length the posture's ground check stores (sub_8232F370); sliding only tries the boost while it's at least 1
    // (sub_823997C8).
    inline constexpr uint32_t SLIDE_BOOST_LENGTH_OFFSET = 1056;
    // The homing attack's target (kept, changed or dropped by the search, sub_82375FE0; cleared by sub_82370768).
    inline constexpr uint32_t HOMING_TARGET_OFFSET = 0xF8C;
    // The search's lock-on phantoms, 3D at +3448 and 2D at +3464 (made by sub_82379718).
    inline constexpr uint32_t HOMING_PHANTOM_2D_OFFSET = 3464;
    // In a side view (2D) section (sub_82316208).
    inline constexpr uint32_t SIDE_VIEW_OFFSET = 1204;
    // Set while on a rail, it seems: the grind actions do nothing without it (the context's vfunc 35, sub_82370750).
    inline constexpr uint32_t GRIND_RAIL_OFFSET = 3600;

    // CPlayerSpeedPosturePluginOutOfControl (vftable 0x82013B00), which keeps flag 3 set (sub_82351068, sub_82351148).
    // Like a state, it has its time at +16 (STATE_TIME_OFFSET), and +28 is set once it ends (sub_82E62780).
    inline constexpr uint32_t PLUGIN_CONTEXT_OFFSET = 8;
    inline constexpr uint32_t PLUGIN_ENDED_OFFSET = 28;
    // Its duration: it ends on the update its time gets past this (below 0: no timer).
    inline constexpr uint32_t OUT_OF_CONTROL_DURATION_OFFSET = 96;

    // State flags (first set).
    inline constexpr uint32_t OUT_OF_CONTROL_FLAG = 3; // CPlayerSpeedPosturePluginOutOfControl, the context's vfunc 56
    inline constexpr uint32_t ATTACKING_FLAG = 6; // set with an attack type (SetAttackType sub_8232F950)
    inline constexpr uint32_t BOOSTING_FLAG = 14;
    inline constexpr uint32_t BOOST_DISABLED_FLAG = 15;
    inline constexpr uint32_t HOMING_ATTACK_ENABLED_FLAG = 17; // set by JumpShort, GrindJumpShort, vfunc 32 (sub_82370F68)
    inline constexpr uint32_t AIR_BOOST_DISABLED_FLAG = 18;
    inline constexpr uint32_t AIR_BOOST_ENABLED_FLAG = 19; // one air boost or homing attack per airtime
    inline constexpr uint32_t STOMP_DISABLED_FLAG = 20; // set by the short hops until StompingDisableTime
    inline constexpr uint32_t OUT_OF_CONTROL_UNTIL_LANDING_FLAG = 25; // the out-of-control also ends on the ground (sub_82351208)
    inline constexpr uint32_t NO_AIR_DECELERATION_FLAG = 27; // set on leaving the ground (vfunc 25, sub_82374360)
    inline constexpr uint32_t BOOST_DISABLED_FLAG_2 = 66;

    // Parameters by ID (names from Sonic's parameter files, IDs from the readers' calls).
    inline constexpr uint32_t MOVEMENT_INPUT_DEADZONE_PARAMETER = 12; // read by sub_823083F0, 0.02
    inline constexpr uint32_t IN_AIR_MAX_ACCEL_VELOCITY_PARAMETER = 22; // InAirMaxAccelVelocity, 15
    inline constexpr uint32_t BOOST_ENABLE_CHAOS_ENERGY_PARAMETER = 215; // BoostEnableChaosEnergy
    inline constexpr uint32_t AIR_BOOST_ENABLE_CHAOS_ENERGY_PARAMETER = 264; // AirBoostEnableChaosEnergy
    inline constexpr uint32_t ACTION_ENABLE_AFTER_HOMING_ATTACK_PARAMETER = 278; // ActionEnableAfterHomingAttack
    inline constexpr uint32_t STOMPING_DISABLE_TIME_PARAMETER = 292; // StompingDisableTime, 0.15 s
    inline constexpr uint32_t AIR_BOOST_HOMING_PARAMETER = 294; // XButtonHoming (bool; off with HomingAttackOnJump)
    // The lock-on frustum's aspect, vertical field of view (degrees), near and far: 297 to 300 (read by sub_82379718).
    inline constexpr uint32_t LOCKON_FRUSTUM_FIRST_PARAMETER = 297;
    // The point the lock-on frustum looks from, in the phantom's frame (sub_82379718).
    inline constexpr uint32_t LOCKON_FRUSTUM_EYE = 0x83397230;

    // Movement modes: each switches the context to its own parameter table, kept in a global shared_ptr, filled by Sonic's
    // parameter loader (sub_8237A828) from the groups named in the notes.
    struct MovementMode
    {
        uint32_t Parameters;
        const char* Name;
        bool SideView = false;
    };

    inline constexpr MovementMode MOVEMENT_MODES[] =
    {
        { 0x83362FA4, "3D" }, // ForwardView
        { 0x83362FAC, "2D", true }, // SideView (sub_82320B70)
        { 0x83364750, "FORWARD" }, // DashMode (sub_82377D00)
        { 0x83364758, "FORWARD" }, // QSM (sub_82378500)
        { 0x83362FB4, "ON WATER" }, // OnWater (sub_823746C0)
        { 0x83362FBC, "SHOES SLIDING" }, // ShoesSliding3D (sub_82316B90; defaults sub_82324410)
        { 0x83362FC4, "SHOES SLIDING 2D", true }, // ShoesSliding2D
        { 0x83364728, "WALL RUN" }, // WallCorrection (Rooftop Run's wall)
        { 0x83362FCC, "ON ICE" }, // OnIce3D
        { 0x83362FD4, "ON ICE 2D", true }, // OnIce2D
        { 0x83364760, "INTERCEPTOR" }, // Special 0 (MsgStartSonicSpecialMode, sub_82365BE8): Savannah Citadel's chase
        { 0x83364768, "SPECIAL 1" },
        { 0x83364770, "SPECIAL 2" },
        { 0x83364778, "SPECIAL 3" },
    };

    // --- States (Hedgehog engine state machines) ---

    // The player's state machine (the context's ChangeState, sub_82307E18), its current state's record (compared in
    // sub_82E669D8), and the state in it.
    inline constexpr uint32_t PLAYER_STATE_MACHINE_OFFSET = 0xCC;
    inline constexpr uint32_t STATE_MACHINE_CURRENT_OFFSET = 0x58;
    inline constexpr uint32_t STATE_RECORD_STATE_OFFSET = 0x1C;
    // A state's context, and the time spent in it in seconds (the machine adds it before the update, sub_82E65D80).
    inline constexpr uint32_t STATE_CONTEXT_OFFSET = 0x8;
    inline constexpr uint32_t STATE_TIME_OFFSET = 0x10;

    // The states whose update runs the ground actions on every update that doesn't change the state first (the context's
    // vfunc 36, sub_82386FF8: TryJump, then TryQuickStep and TryBoost; all skipped while Sonic is out of control).
    inline constexpr uint32_t GROUND_ACTIONS_STATE_VFTABLES[] =
    {
        0x820167F4, // CPlayerSpeedStateStand
        0x82016908, // CSonicStateStand (sub_8239B050 -> sub_8233EDE0)
        0x820128B0, // CPlayerSpeedContext::CStateWalk
        0x82016934, // CSonicContext::CStateWalk (sub_8239DF90)
        0x820169E4, // CSonicContext::CStateBrake (sub_82390338: after its own state changes)
        0x82016A10, // CSonicContext::CStateBrakeEnd
        0x8201658C, // CPlayerSpeedStateBrakeTurn (sub_82335318: unless its animation ends)
        0x82011EC4, // CPlayerSpeedStateJumpSpringHeadLand (sub_82339488: unless its animation ends)
        0x8201C0D8, // CSonicContext::CStateSlidingEnd (sub_8239A010)
        0x820169B8, // CSonicStateMoveStop
        0x8201B7F0, // CSonicStateDrift
        0x8201BC60, // CSonicStateOnIce
        0x82016B48, // CSonicStateShoesSliding
        0x8201C550, // CSonicStateWallRunDamage
        0x82011EF8, // CPlayerSpeedContext::CStateLand
        0x82011F90, // CPlayerSpeedContext::CStateLandJumpShort (sub_823399B8)
        0x82016A68, // CSonicStateLand
        0x8201C388, // CSonicStateStompingLand (sub_8239C598)
    };

    // The states whose update calls the air actions (the context's vfunc 37, sub_823795F8: the homing attack, TryStomp,
    // then TryAirBoost).
    inline constexpr uint32_t AIR_ACTIONS_STATE_VFTABLES[] =
    {
        0x82011E80, // CPlayerSpeedContext::CStateJumpSpring
        0x820125C8, // CPlayerSpeedStateSpecialJump
        0x82016A3C, // CSonicStateFall
        0x82016B74, // CSonicStateFinishExternalControlAir
        0x8201B8D8, // CSonicStateGrindJumpShort
        0x8201BAC0, // CSonicStateHomingAttackAfter (sub_82394900)
        0x8201698C, // CSonicStateJumpBall
        0x82016560, // CSonicContext::CStateJumpHurdle
        0x82016960, // CSonicContext::CStateJumpShort
        0x8201BEF4, // CSonicStateSelectJumpAfter::CStateUp
        0x8201BF44, // CSonicStateSelectJumpAfter::CStateFrontAir
        0x82016B1C, // CSonicStateWallJump
    };

    // The states that call TryBoost themselves.
    inline constexpr uint32_t BOOST_STATE_VFTABLES[] =
    {
        0x8201C0AC, // CSonicContext::CStateSliding (sub_823997C8)
        0x82016BA0, // CSonicStatePushingWall
    };

    // The states whose update runs the grind actions (sub_82387F50: while GRIND_RAIL_OFFSET is set and Sonic is in
    // control, three rail actions, then TryBoost).
    inline constexpr uint32_t GRIND_ACTIONS_STATE_VFTABLES[] =
    {
        0x8201B81C, // CSonicStateGrind (sub_82392510)
        0x8201BC20, // CSonicStateLandGrindJumpShort (sub_82395C20: unless its animation ends)
    };

    // The states with a jump buffer instead of TryJump: a timer at ctx+3384 started on A (sub_82316210), a jump on the
    // ground while it's under parameter 176 (sub_82330D18).
    inline constexpr uint32_t JUMP_BUFFER_STATE_VFTABLES[] =
    {
        0x82016A3C, // CSonicStateFall (sub_82392308 -> sub_823365A8)
        0x8201698C, // CSonicStateJumpBall (sub_82394A58 -> sub_82338470)
        0x82016560, // CSonicContext::CStateJumpHurdle (sub_82394F30)
        0x82011E44, // CPlayerSpeedContext::CStateJumpShort (sub_823386E0)
    };

    // --- The Werehog ---

    // His velocity, a single world space vector: how fast the point under him (+0x580) and his height (+0x560) change
    // (from per-frame dumps).
    inline constexpr uint32_t EVIL_VELOCITY_OFFSET = 0x900;
    // What his IsOnGround (sub_823A4080) returns.
    inline constexpr uint32_t EVIL_GROUNDED_OFFSET = 0xAE1;
    // His helpers: std::map<uint16_t, boost::shared_ptr<...>> (sub_82BB93D0 looks them up, inserting). Helper 2 is the
    // attacks' (SWA::Player::CEvilAttackAction): its level of the attack list at +8, its current action (sub_82DE0870)
    // at +20.
    inline constexpr uint32_t EVIL_HELPERS_OFFSET = 0x3BC;
    inline constexpr uint16_t EVIL_ATTACK_HELPER = 2;
    inline constexpr uint32_t ATTACK_HELPER_LEVEL_OFFSET = 8;
    inline constexpr uint32_t ATTACK_HELPER_ACTION_OFFSET = 20;
    // An action: a node of the attack list read from EvilAttackAction*.xml (sub_824241D0).
    inline constexpr uint32_t ACTION_NAME_OFFSET = 16; // ActionName
    inline constexpr uint32_t ACTION_MOTION_NAME_OFFSET = 20; // MotionName
    inline constexpr uint32_t ACTION_KEY_END_OFFSET = 76; // KEY__End: the action that follows when it ends
    inline constexpr uint32_t ACTION_GUARD_OFFSET = 148; // Guard: whether a guard can cancel it
    // The motions read from EvilAttackMotionFile.xml: an std::map<CSharedString, boost::shared_ptr<...>> per level of the
    // attack list, 12 bytes apart (sub_82423B90). A motion's animation file name is at +0.
    inline constexpr uint32_t ATTACK_MOTION_MAPS = 0x833655D8;
    inline constexpr uint32_t MOTION_END_SKIP_TIME_OFFSET = 268; // EndSkipTimeWhenPad (sub_82424A18)
    // The player's animation state machine: CEvilSonic's interface at +0xC0 returns it from its +380 (sub_82303030).
    // It maps names to IDs at +20 and IDs to states at +8 (sub_82BB97E8).
    inline constexpr uint32_t PLAYER_ANIMATION_STATE_MACHINE_OFFSET = 0xC0 + 380;

    // --- Animations ---

    // An animation state (SWA::CAnimationControlSingle): its Havok control (hkaDefaultAnimationControl) at +4 and its
    // start time at +20 (sub_82BBD030). The control has the local time at +8, the playback speed at +0x40 and the
    // binding at +0x28, whose animation (+0) has its duration at +12 (the state's vfunc 2, sub_82BC0870).
    inline constexpr uint32_t ANIMATION_CONTROL_SINGLE_VFTABLE = 0x820CB3DC;
    inline constexpr uint32_t ANIMATION_CONTROL_OFFSET = 4;
    inline constexpr uint32_t ANIMATION_START_TIME_OFFSET = 20;
    inline constexpr uint32_t ANIMATION_CONTROL_TIME_OFFSET = 8;
    inline constexpr uint32_t ANIMATION_CONTROL_SPEED_OFFSET = 0x40;
    inline constexpr uint32_t ANIMATION_CONTROL_BINDING_OFFSET = 0x28;
    inline constexpr uint32_t ANIMATION_DURATION_OFFSET = 12;

    // --- The stage ---

    // The game document (SWA::CGameDocument::GetInstance), whose member keeps the stage time at +0x5C: it stops at the
    // goal, and counts up from below 0 during a stage's intro (+0x60 keeps going after the goal).
    inline constexpr uint32_t GAME_DOCUMENT = 0x83367900;
    inline constexpr uint32_t GAME_DOCUMENT_MEMBER_OFFSET = 8;
    inline constexpr uint32_t STAGE_TIME_OFFSET = 0x5C;

    // --- The camera ---

    // SWA::CCamera, as its UpdateSerial (sub_82467890) leaves it: the view matrix and the projection (sub_82468E38), both
    // for row vectors (point * view * projection), and the eye. The projection is right-handed (w = -z).
    inline constexpr uint32_t CAMERA_VIEW_OFFSET = 0x130;
    inline constexpr uint32_t CAMERA_PROJECTION_OFFSET = 0x170;
    inline constexpr uint32_t CAMERA_POSITION_OFFSET = 0x1B0;
    inline constexpr float CAMERA_NEAR_PLANE = 0.1f;

    // --- Havok ---

    // A rigid body (hkpRigidBody): in the world (+8) while it collides; its shape and its transform (the rotation's
    // columns, then the translation at +0x110).
    inline constexpr uint32_t RIGID_BODY_VFTABLE = 0x82125110;
    inline constexpr uint32_t RIGID_BODY_WORLD_OFFSET = 0x8;
    inline constexpr uint32_t RIGID_BODY_SHAPE_OFFSET = 0x10;
    inline constexpr uint32_t RIGID_BODY_TRANSFORM_OFFSET = 0xE0;
    // Shape phantoms keep their shape like rigid bodies; a collidable's motion (+0x18) points to its transform (a rigid
    // body's at +0xE0).
    inline constexpr uint32_t SIMPLE_SHAPE_PHANTOM_VFTABLE = 0x821280F4;
    inline constexpr uint32_t CACHING_SHAPE_PHANTOM_VFTABLE = 0x82125A3C;
    inline constexpr uint32_t COLLIDABLE_MOTION_OFFSET = 0x18;
    // A collidable's filter info: an index into the game's category sets (below).
    inline constexpr uint32_t COLLIDABLE_FILTER_INFO_OFFSET = 0x2C;

    // Convex shapes: the convex radius at +0x10; a cylinder (hkpCylinderShape) adds its own radius (+0x14) around the
    // segment between +0x20 and +0x30, a capsule (hkpCapsuleShape) has no cylinder radius, a sphere (hkpSphereShape) only
    // the convex radius, a box (hkpBoxShape) its half extents at +0x20.
    inline constexpr uint32_t CYLINDER_SHAPE_VFTABLE = 0x8212FD54;
    inline constexpr uint32_t CAPSULE_SHAPE_VFTABLE = 0x8212F8F4;
    inline constexpr uint32_t SPHERE_SHAPE_VFTABLE = 0x8212D0A4;
    inline constexpr uint32_t BOX_SHAPE_VFTABLE = 0x8212F9E4;
    inline constexpr uint32_t CONVEX_RADIUS_OFFSET = 0x10;
    inline constexpr uint32_t CYLINDER_RADIUS_OFFSET = 0x14;
    inline constexpr uint32_t SEGMENT_START_OFFSET = 0x20;
    inline constexpr uint32_t SEGMENT_END_OFFSET = 0x30;
    inline constexpr uint32_t BOX_HALF_EXTENTS_OFFSET = 0x20;

    // Shapes made of others, as their methods read them: hkpConvexTranslateShape (child at +0x18, moved by +0x20),
    // hkpConvexTransformShape (child at +0x18, transformed by +0x20), hkpTransformShape (child at +0x14, transformed by
    // +0x30) and hkpListShape (children at +0x18, 16 bytes each with the shape first, how many at +0x1C).
    inline constexpr uint32_t CONVEX_TRANSLATE_SHAPE_VFTABLE = 0x8212F4CC;
    inline constexpr uint32_t CONVEX_TRANSFORM_SHAPE_VFTABLE = 0x8212F574;
    inline constexpr uint32_t TRANSFORM_SHAPE_VFTABLE = 0x8212EA84;
    inline constexpr uint32_t LIST_SHAPE_VFTABLE = 0x82130428;
    inline constexpr uint32_t CONVEX_WRAPPER_CHILD_OFFSET = 0x18;
    inline constexpr uint32_t CONVEX_WRAPPER_TRANSFORM_OFFSET = 0x20;
    inline constexpr uint32_t TRANSFORM_SHAPE_CHILD_OFFSET = 0x14;
    inline constexpr uint32_t TRANSFORM_SHAPE_TRANSFORM_OFFSET = 0x30;
    inline constexpr uint32_t LIST_SHAPE_CHILDREN_OFFSET = 0x18;
    inline constexpr uint32_t LIST_SHAPE_COUNT_OFFSET = 0x1C;
    inline constexpr uint32_t LIST_SHAPE_CHILD_SIZE = 16;

    // Meshes: a MOPP tree (hkpMoppBvTreeShape) over an extended mesh (hkpExtendedMeshShape; the one loaded from files,
    // the stage's, shares its layout and destructor, sub_82F20340). Triangle subparts at +0x50 (how many at +0x54, 64
    // bytes each); the triangles collide this far out on both sides (+0xB0).
    inline constexpr uint32_t MOPP_SHAPE_VFTABLE = 0x8212CF68;
    inline constexpr uint32_t MOPP_SHAPE_CHILD_OFFSET = 0x34;
    inline constexpr uint32_t EXTENDED_MESH_SHAPE_VFTABLE = 0x8212D064;
    inline constexpr uint32_t STORAGE_EXTENDED_MESH_SHAPE_VFTABLE = 0x82130554;
    inline constexpr uint32_t EXTENDED_MESH_SUBPARTS_OFFSET = 0x50;
    inline constexpr uint32_t EXTENDED_MESH_RADIUS_OFFSET = 0xB0;
    inline constexpr uint32_t TRIANGLES_SUBPART_SIZE = 64;
    // A triangle subpart: the triangle count, the vertices (base, stride, count) and the indices (base, stride, type:
    // 1 = 16-bit, 2 = 32-bit).
    inline constexpr uint32_t SUBPART_TRIANGLE_COUNT_OFFSET = 0x10;
    inline constexpr uint32_t SUBPART_VERTICES_OFFSET = 0x14;
    inline constexpr uint32_t SUBPART_VERTEX_STRIDE_OFFSET = 0x18;
    inline constexpr uint32_t SUBPART_VERTEX_COUNT_OFFSET = 0x1C;
    inline constexpr uint32_t SUBPART_INDICES_OFFSET = 0x30;
    inline constexpr uint32_t SUBPART_INDEX_STRIDE_OFFSET = 0x34;
    inline constexpr uint32_t SUBPART_INDEX_TYPE_OFFSET = 0x38;

    // The game's collision categories: a vector at *0x83366F54 (begin +4, end +8) of 20-byte sets (sub_8248EAF0 adds
    // them): its own categories (64 bits, bits 0-31 in the first word), those it collides with, flags.
    inline constexpr uint32_t COLLISION_CATEGORY_TABLE = 0x83366F54;
    inline constexpr uint32_t COLLISION_CATEGORY_ENTRY_SIZE = 20;
    // The category numbers the homing attack's search meets (read at run time): its phantom's targets (58), and its line
    // of sight raycast's (sub_822EC588).
    inline constexpr uint32_t HOMING_TARGET_CATEGORY = 0x83367180;
    inline constexpr uint32_t HOMING_SIGHT_CATEGORY = 0x83367060;

    // --- Collisions of the player and of objects ---

    // The player's collision container, with an std::map<uint32_t, boost::shared_ptr<SWA::CEventCollision>> at +4 (its
    // head at +8). Day Sonic's set-up (sub_82362650) makes, in order: Normal (ID 1, the body), Item, ItemBoost,
    // SquatKick, Path (ID 5, a capsule that's always on), and a diving one; he switches them by mode (sub_82353ED8).
    inline constexpr uint32_t PLAYER_COLLISIONS_OFFSET = 616;
    inline constexpr uint32_t COLLISIONS_HEAD_OFFSET = 8;
    inline constexpr uint32_t BODY_COLLISION_ID = 1;
    inline constexpr uint32_t SONIC_PATH_COLLISION_ID = 5;

    // Physics units: SWA::CEventCollision (made by sub_8248F210, for objects by sub_82516528, sub_82516690, sub_82516838
    // and sub_82516960) and SWA::CRigidBody (added by sub_825146A8), each with its Havok object at +4. Their base's
    // constructor is sub_824932A8 and its destructor sub_82493098.
    inline constexpr uint32_t EVENT_COLLISION_VFTABLE = 0x8202C7B4;
    inline constexpr uint32_t RIGID_BODY_UNIT_VFTABLE = 0x8200CC68;
    inline constexpr uint32_t PHYSICS_UNIT_BODY_OFFSET = 4;

    // The grind rails (SWA::CObjGrindPath; set-up vfunc 5 sub_822E21F8, CObjPathBase's deleting destructor
    // sub_822E4290): +212 points to a part (at +0x110) of the rail's CPathRigidBodyCollision, whose rigid body is at +4.
    inline constexpr uint32_t GRIND_PATH_VFTABLE = 0x8200B360;
    inline constexpr uint32_t GRIND_PATH_COLLISION_OFFSET = 212;
    inline constexpr uint32_t PATH_RIGID_BODY_COLLISION_PART_VFTABLE = 0x8200B06C;
    inline constexpr uint32_t PATH_RIGID_BODY_COLLISION_PART_OFFSET = 0x110;
    inline constexpr uint32_t PATH_RIGID_BODY_COLLISION_VFTABLE = 0x8200B084;
    inline constexpr uint32_t PATH_RIGID_BODY_COLLISION_BODY_OFFSET = 4;

    // The hint rings (SWA::CObjHintRing; constructor sub_82739988, set-up sub_827382C0, update sub_82738088, messages
    // sub_82739870, hits sub_827391E0): whether it was touched, its timer, and the two fields that turn it off.
    inline constexpr uint32_t HINT_RING_VFTABLE = 0x82064034;
    inline constexpr uint32_t HINT_RING_TOUCHED_OFFSET = 328;
    inline constexpr uint32_t HINT_RING_TIMER_OFFSET = 348;
    inline constexpr uint32_t HINT_RING_OFF_OFFSET = 356;
    inline constexpr uint32_t HINT_RING_MODE_OFFSET = 360;
    inline constexpr uint32_t HINT_RING_OFF_MODE = 4;
}
