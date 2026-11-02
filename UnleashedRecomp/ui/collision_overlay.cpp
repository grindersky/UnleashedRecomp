#include "collision_overlay.h"
#include <api/SWA.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <patches/collision_view_patches.h>
#include <ui/imgui_utils.h>
#include <ui/tas_hud.h>
#include <user/config.h>
#include <sdl_listener.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <numbers>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The game's camera (SWA::CCamera), as its UpdateSerial leaves it: the view matrix at +0x130 and the projection at
// +0x170 (sub_82468E38), both for row vectors (point * view * projection), and the eye at +0x1B0. Found from dumps of
// a stage: the eye is the view's translation run back through its rotation, and the projection is a right-handed
// perspective (w = -z) with its near plane at 0.1.
static constexpr uint32_t CAMERA_VIEW_OFFSET = 0x130;
static constexpr uint32_t CAMERA_PROJECTION_OFFSET = 0x170;
static constexpr uint32_t CAMERA_POSITION_OFFSET = 0x1B0;
static constexpr float NEAR_PLANE = 0.1f;

// Havok objects. A rigid body (hkpRigidBody) is in the world (+8) while it collides; its shape is at +0x10 and its
// transform at +0xE0: the rotation's columns, then the translation at +0x110.
static constexpr uint32_t RIGID_BODY_VFTABLE = 0x82125110;
static constexpr uint32_t RIGID_BODY_WORLD_OFFSET = 0x8;
static constexpr uint32_t RIGID_BODY_SHAPE_OFFSET = 0x10;
static constexpr uint32_t RIGID_BODY_TRANSFORM_OFFSET = 0xE0;
static constexpr uint32_t MOPP_SHAPE_VFTABLE = 0x8212CF68;
static constexpr uint32_t MOPP_SHAPE_CHILD_OFFSET = 0x34;
static constexpr uint32_t EXTENDED_MESH_SHAPE_VFTABLE = 0x8212D064;
// The one loaded from files (the stage's collision), with the same layout (they share the destructor sub_82F20340).
static constexpr uint32_t STORAGE_EXTENDED_MESH_SHAPE_VFTABLE = 0x82130554;
// Its triangle subparts (+0x50, how many at +0x54): the triangle count at +0x10, then the vertices (base, stride and
// count at +0x14/+0x18/+0x1C) and the indices (base, stride and type at +0x30/+0x34/+0x38: 1 = 16-bit, 2 = 32-bit).
static constexpr uint32_t EXTENDED_MESH_SUBPARTS_OFFSET = 0x50;
// The triangles' radius: Havok's triangles collide this far out on both sides (0.05 for the rails, like the half height
// of their bounding box).
static constexpr uint32_t EXTENDED_MESH_RADIUS_OFFSET = 0xB0;
// Convex shapes have their convex radius at +0x10: a cylinder (hkpCylinderShape) adds it to its own radius at +0x14,
// between the points at +0x20 and +0x30, like a capsule (hkpCapsuleShape) without the cylinder radius; a sphere
// (hkpSphereShape) has only it; a box (hkpBoxShape) has its half extents at +0x20.
static constexpr uint32_t CYLINDER_SHAPE_VFTABLE = 0x8212FD54;
static constexpr uint32_t CAPSULE_SHAPE_VFTABLE = 0x8212F8F4;
static constexpr uint32_t SPHERE_SHAPE_VFTABLE = 0x8212D0A4;
static constexpr uint32_t BOX_SHAPE_VFTABLE = 0x8212F9E4;

// The grind rails (SWA::CObjGrindPath): +212 points to a part (at +0x110) of the rail's CPathRigidBodyCollision,
// whose rigid body (+4) has a MOPP shape over a mesh: a strip of triangles along the rail, which Sonic grinds on when
// he touches it (the strip nearest to him was 0.8 away while he grinded in Rooftop Run).
static constexpr uint32_t GRIND_PATH_VFTABLE = 0x8200B360;
static constexpr uint32_t GRIND_PATH_COLLISION_OFFSET = 212;
static constexpr uint32_t PATH_RIGID_BODY_COLLISION_PART_VFTABLE = 0x8200B06C;
static constexpr uint32_t PATH_RIGID_BODY_COLLISION_PART_OFFSET = 0x110;
static constexpr uint32_t PATH_RIGID_BODY_COLLISION_VFTABLE = 0x8200B084;
static constexpr uint32_t PATH_RIGID_BODY_COLLISION_BODY_OFFSET = 4;

// The player's collision (SWA::CEventCollision objects, each with its rigid body at +4), by ID, in an
// std::map<uint32_t, boost::shared_ptr<...>> at +4 of the player's collision container (+616), its head at +8. Day
// Sonic switches them by what he does (sub_82353ED8): ID 1 is his body, a cylinder CollisionRadius wide and
// CollisionStandHeight (or CollisionSquatHeight) high, and the rest touch rings, objects and triggers (the item
// collision, bigger while boosting, and others). The Werehog's ID 1 is a capsule. Day Sonic's set-up (sub_82362650)
// names them as it makes them: Normal, Item, ItemBoost, SquatKick, then Path, ID 5, a capsule that's always on and
// most likely what touches the game's paths, like the grind rails.
static constexpr uint32_t PLAYER_OFFSET = 0x100;
static constexpr uint32_t PLAYER_COLLISIONS_OFFSET = 616;
static constexpr uint32_t COLLISIONS_HEAD_OFFSET = 8;
static constexpr uint32_t EVENT_COLLISION_VFTABLE = 0x8202C7B4;
static constexpr uint32_t EVENT_COLLISION_BODY_OFFSET = 4;
static constexpr uint32_t BODY_COLLISION_ID = 1;
static constexpr uint32_t SONIC_PATH_COLLISION_ID = 5;

// Sonic's attacks hit what his collision touches. His body (Normal) is a TypePlayer collision that detects TypeEvent
// ones, the others (Item and the rest, all but Path) TypePlayerItem ones that detect TypeEventItem, TypeEvent and
// TypeObjectPhysics ones (the filters made in sub_824950C0, and in the set-up). When one touches something
// (MsgHitEventCollision, sub_82360248) while he has an attack type (sliding sets TypeSonicSliding, sub_8232F950), he
// damages it: no shape of its own. Setting an attack type also sets this state flag, and removing it clears it.
static constexpr uint32_t CONTEXT_STATE_FLAGS_OFFSET = 0x4AC;
static constexpr uint32_t ATTACKING_FLAG = 6;

// What other objects add: event collisions (sub_82516528, sub_82516690 and sub_82516838, which make them with
// sub_8248F210), and SWA::CRigidBody objects (sub_825146A8). Both are physics units with their Havok object at +4; their
// base's constructor is sub_824932A8 and its destructor sub_82493098.
static constexpr uint32_t RIGID_BODY_UNIT_VFTABLE = 0x8200CC68;
static constexpr uint32_t PHYSICS_UNIT_BODY_OFFSET = 4;
// Some may be shape phantoms. Like rigid bodies, they keep their shape at +0x10, and their collidable's motion (+0x18)
// points to their transform (a rigid body's at +0xE0).
static constexpr uint32_t SIMPLE_SHAPE_PHANTOM_VFTABLE = 0x821280F4;
static constexpr uint32_t CACHING_SHAPE_PHANTOM_VFTABLE = 0x82125A3C;
static constexpr uint32_t COLLIDABLE_MOTION_OFFSET = 0x18;

// Shapes made of others, as their methods read them: hkpConvexTranslateShape (its child at +0x18, moved by +0x20),
// hkpConvexTransformShape (its child at +0x18, transformed by +0x20: columns, then the translation),
// hkpTransformShape (its child at +0x14, transformed by +0x30) and hkpListShape (its children at +0x18, 16 bytes each
// with the shape first, how many at +0x1C).
static constexpr uint32_t CONVEX_TRANSLATE_SHAPE_VFTABLE = 0x8212F4CC;
static constexpr uint32_t CONVEX_TRANSFORM_SHAPE_VFTABLE = 0x8212F574;
static constexpr uint32_t TRANSFORM_SHAPE_VFTABLE = 0x8212EA84;
static constexpr uint32_t LIST_SHAPE_VFTABLE = 0x82130428;

// The player's matrix node (SWA::CMatrixNodeTransform, at +16 of both contexts), whose matrix at +128 (its vfunc 2) has
// the axes as rows, then the position: his facing is its third row (sub_8230A4E0), and his position is at his feet.
static constexpr uint32_t CONTEXT_MATRIX_NODE_OFFSET = 16;
static constexpr uint32_t MATRIX_NODE_MATRIX_OFFSET = 128;

// Day Sonic's velocity, in world space (the HUD's "Total").
static constexpr uint32_t SONIC_VELOCITY_OFFSET = 0x210;

// The arrow along his facing: a flat arrow given some thickness, a little above his feet.
static constexpr float ARROW_HEIGHT = 0.05f;
static constexpr float ARROW_THICKNESS = 0.05f;
static constexpr float ARROW_LENGTH = 1.0f;
static constexpr float ARROW_HEAD_LENGTH = 0.3f;
static constexpr float ARROW_HALF_WIDTH = 0.04f;
static constexpr float ARROW_HEAD_HALF_WIDTH = 0.15f;

static const ImU32 RAIL_COLOUR = IM_COL32(255, 140, 0, 130);
static const ImU32 BODY_COLOUR = IM_COL32(0, 240, 255, 200);
static const ImU32 TRIGGER_COLOUR = IM_COL32(255, 80, 255, 85);
static const ImU32 PATH_COLOUR = IM_COL32(60, 90, 255, 90);
static const ImU32 ATTACK_COLOUR = IM_COL32(60, 255, 90, 100);
static const ImU32 ENEMY_COLOUR = IM_COL32(255, 40, 40, 110);
static const ImU32 GOAL_COLOUR = IM_COL32(255, 255, 255, 110);
static const ImU32 ARROW_COLOUR = IM_COL32(255, 255, 255, 240);

static constexpr float PI = std::numbers::pi_v<float>;

// How far rails are drawn: they fade out from the first distance to the second.
static constexpr float RAIL_FADE_START = 150.0f;
static constexpr float RAIL_FADE_END = 400.0f;

struct Vector3
{
    float x, y, z;

    Vector3 operator+(const Vector3& other) const { return { x + other.x, y + other.y, z + other.z }; }
    Vector3 operator-(const Vector3& other) const { return { x - other.x, y - other.y, z - other.z }; }
    Vector3 operator*(float scale) const { return { x * scale, y * scale, z * scale }; }
};

static float Length(const Vector3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static float Dot(const Vector3& a, const Vector3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vector3 Cross(const Vector3& a, const Vector3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static Vector3 Normalised(const Vector3& v)
{
    float length = Length(v);
    return length > 0.0f ? v * (1.0f / length) : Vector3{ 0.0f, 1.0f, 0.0f };
}

static bool IsReadable(uint32_t address)
{
    return address >= 0x1000 && address < 0xFFFF0000;
}

static uint32_t LoadU32(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U32(address) : 0;
}

static uint16_t LoadU16(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U16(address) : 0;
}

static uint8_t LoadU8(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U8(address) : 0;
}

static float LoadFloat(uint32_t address)
{
    uint32_t value = LoadU32(address);
    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static Vector3 LoadVector(uint32_t address)
{
    return { LoadFloat(address), LoadFloat(address + 4), LoadFloat(address + 8) };
}

// A transform as Havok keeps it: the rotation's columns, then the translation.
struct Transform
{
    Vector3 columns[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    Vector3 translation{};

    Transform() = default;

    // A rigid body's.
    explicit Transform(uint32_t body) : Transform(At(body + RIGID_BODY_TRANSFORM_OFFSET))
    {
    }

    static Transform At(uint32_t address)
    {
        Transform transform;
        for (int i = 0; i < 3; i++)
            transform.columns[i] = LoadVector(address + i * 16);

        transform.translation = LoadVector(address + 48);
        return transform;
    }

    // Another transform, in this one's space.
    Transform operator*(const Transform& other) const
    {
        Transform result;
        for (int i = 0; i < 3; i++)
            result.columns[i] = Rotate(other.columns[i]);

        result.translation = (*this)(other.translation);
        return result;
    }

    Vector3 operator()(const Vector3& p) const
    {
        return Rotate(p) + translation;
    }

    Vector3 Rotate(const Vector3& v) const
    {
        return columns[0] * v.x + columns[1] * v.y + columns[2] * v.z;
    }
};

static uint32_t g_camera;
static uint32_t g_cameraVftable;

// The grind rails set up and not destroyed yet. The game adds and removes them on its own thread.
static std::mutex g_grindPathsMutex;
static std::vector<uint32_t> g_grindPaths;

// A rail's mesh, read once, by mesh: its vertices, its triangles, each vertex's normal (the average of its triangles'),
// and the edges only one triangle has, with that triangle's other vertex, for the sides of the slab.
struct RailMesh
{
    uint32_t Vertices = 0;
    uint32_t Indices = 0;
    uint32_t Triangles = 0;
    std::vector<Vector3> Points;
    std::vector<Vector3> Normals;
    std::vector<std::array<uint32_t, 3>> Faces;
    std::vector<std::array<uint32_t, 3>> Edges;
    bool Used = false;
};

static std::unordered_map<uint32_t, RailMesh> g_railMeshes;

bool CollisionOverlay::IsEnabled()
{
    return Config::EnableStageCollisionDebugView || Config::EnableEventCollisionDebugView || Config::EnableObjectCollisionDebugView;
}

// The camera and the objects are also followed for the HUD's radar.
static bool IsTracking()
{
    return CollisionOverlay::IsEnabled() || TasHud::IsEnabled();
}

// The overlay is hidden behind what the game drew, with the game's depth buffer; F12 switches to drawing it over
// everything and back. Under libTAS, F1 to F10 are savestate hotkeys, which the game doesn't get; F12 is passed on as a
// key, so it goes into the movie like the game's keys.
static std::atomic<bool> g_depthTested{ true };

// How far the HUD's radar reaches from Sonic, to each side; Home goes to the next size (back to the first after the
// last), M hides and shows it. Like F12, they reach the game as keys the game doesn't use.
static constexpr float RADAR_HALF_WIDTHS[] = { 4.0f, 12.0f, 30.0f, 60.0f };
static std::atomic<int> g_radarSize{ 0 };
static std::atomic<bool> g_radarShown{ true };

static class CollisionOverlayListener : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override
    {
        if (event->type != SDL_KEYDOWN || event->key.repeat)
            return false;

        if (CollisionOverlay::IsEnabled() && event->key.keysym.sym == SDLK_F12)
            g_depthTested = !g_depthTested;

        if (TasHud::IsEnabled() && event->key.keysym.sym == SDLK_HOME)
            g_radarSize = (g_radarSize + 1) % int(std::size(RADAR_HALF_WIDTHS));

        if (TasHud::IsEnabled() && event->key.keysym.sym == SDLK_m)
            g_radarShown = !g_radarShown;

        return false;
    }
}
g_collisionOverlayListener;

void CollisionOverlay::OnCameraUpdate(uint32_t camera)
{
    if (IsTracking())
    {
        g_camera = camera;
        g_cameraVftable = LoadU32(camera);
    }
}

// SWA::CObjGrindPath's set-up (its vfunc 5), which creates the rail's collision.
PPC_FUNC_IMPL(__imp__sub_822E21F8);
PPC_FUNC(sub_822E21F8)
{
    uint32_t path = ctx.r3.u32;
    __imp__sub_822E21F8(ctx, base);

    if (CollisionOverlay::IsEnabled())
    {
        std::lock_guard lock(g_grindPathsMutex);
        if (std::find(g_grindPaths.begin(), g_grindPaths.end(), path) == g_grindPaths.end())
            g_grindPaths.push_back(path);
    }
}

// SWA::CObjPathBase's deleting destructor (its vfunc 0), which the grind rails share.
PPC_FUNC_IMPL(__imp__sub_822E4290);
PPC_FUNC(sub_822E4290)
{
    {
        std::lock_guard lock(g_grindPathsMutex);
        std::erase(g_grindPaths, ctx.r3.u32);
    }

    __imp__sub_822E4290(ctx, base);
}

// The objects whose collision is drawn: the enemies (any class with "Enemy" in its name among the object's classes,
// like SWA::CSonicEnemyBase, the CEnemy* classes, their shots and weapons, and SWA::EvilEnemy::IEvilEnemy) and the goal
// ring (SWA::CObjGoalRing), always: the event and object collision views (ms_IsTriggerRender, ms_IsObjectCollisionRender)
// were meant to draw them, but don't (user, 2026-10-23: two enemies left to the views, none drawn). The rings (SWA::CObjRing, whose set-up,
// sub_825EA530, adds a sphere through sub_825169F8 -> sub_82516528, and SWA::CObjSuperRing) are only on the radar, and
// so is the stage's own collision (SWA::CGameObjectTerrain's rigid bodies, added by its set-up sub_82518620 through
// sub_825146A8).
enum class ObjectKind
{
    None,
    Enemy,
    GoalRing,
    Ring,
    Terrain
};

struct TrackedUnit
{
    ObjectKind Kind;
    uint32_t Object;
};

// The physics units objects added, until they're destroyed: those of every object, as the HUD's homing estimate looks
// for any target. The game adds them on its own threads.
static std::mutex g_unitsMutex;
static std::unordered_map<uint32_t, TrackedUnit> g_units;
static std::unordered_map<uint32_t, ObjectKind> g_objectKinds;

// The object adding an event collision.
static thread_local uint32_t g_eventCollisionOwner;

// Calls visit(index, mangled name) for a vtable's classes in the game's RTTI, the class itself first, until it returns
// true: the vtable is preceded by its complete object locator, whose class hierarchy (+16) lists its classes (+12, how
// many at +8), each with its type descriptor first, whose mangled name is at +8.
template<typename Visit>
static void ForEachClassName(uint32_t vftable, const Visit& visit)
{
    if (vftable < PPC_IMAGE_BASE || vftable >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
        return;

    uint32_t hierarchy = LoadU32(LoadU32(vftable - 4) + 16);
    uint32_t count = LoadU32(hierarchy + 8);
    uint32_t classes = LoadU32(hierarchy + 12);
    if (count >= 64)
        return;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t descriptor = LoadU32(LoadU32(classes + i * 4));
        if (descriptor < PPC_IMAGE_BASE || descriptor >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
            break;

        auto text = reinterpret_cast<const char*>(g_memory.Translate(descriptor + 8));
        if (visit(i, std::string_view(text, strnlen(text, 128))))
            break;
    }
}

// An object's kind, from its classes. Called with g_unitsMutex held.
static ObjectKind GetObjectKind(uint32_t object)
{
    uint32_t vftable = LoadU32(object);
    auto it = g_objectKinds.find(vftable);
    if (it != g_objectKinds.end())
        return it->second;

    ObjectKind kind = ObjectKind::None;
    ForEachClassName(vftable, [&](uint32_t i, std::string_view name)
    {
        if (i == 0 && name == ".?AVCObjGoalRing@SWA@@")
            kind = ObjectKind::GoalRing;
        else if (i == 0 && (name == ".?AVCObjRing@SWA@@" || name == ".?AVCObjSuperRing@SWA@@"))
            kind = ObjectKind::Ring;
        else if (i == 0 && name == ".?AVCGameObjectTerrain@SWA@@")
            kind = ObjectKind::Terrain;
        else if (name.find("Enemy") != std::string_view::npos)
            kind = ObjectKind::Enemy;

        return kind != ObjectKind::None;
    });

    g_objectKinds.emplace(vftable, kind);
    return kind;
}


static void TrackUnit(uint32_t object, uint32_t unit)
{
    if (!IsTracking() || !IsReadable(object) || !IsReadable(unit))
        return;

    std::lock_guard lock(g_unitsMutex);
    g_units[unit] = { GetObjectKind(object), object };
}

static void ForgetUnit(uint32_t unit)
{
    if (!IsTracking())
        return;

    std::lock_guard lock(g_unitsMutex);
    g_units.erase(unit);
}

// The helpers objects (r3) add event collisions with.
PPC_FUNC_IMPL(__imp__sub_82516528);
PPC_FUNC(sub_82516528)
{
    uint32_t owner = std::exchange(g_eventCollisionOwner, ctx.r3.u32);
    __imp__sub_82516528(ctx, base);
    g_eventCollisionOwner = owner;
}

PPC_FUNC_IMPL(__imp__sub_82516690);
PPC_FUNC(sub_82516690)
{
    uint32_t owner = std::exchange(g_eventCollisionOwner, ctx.r3.u32);
    __imp__sub_82516690(ctx, base);
    g_eventCollisionOwner = owner;
}

PPC_FUNC_IMPL(__imp__sub_82516838);
PPC_FUNC(sub_82516838)
{
    uint32_t owner = std::exchange(g_eventCollisionOwner, ctx.r3.u32);
    __imp__sub_82516838(ctx, base);
    g_eventCollisionOwner = owner;
}

// Makes an event collision (SWA::CEventCollision) into the shared_ptr at r3.
PPC_FUNC_IMPL(__imp__sub_8248F210);
PPC_FUNC(sub_8248F210)
{
    uint32_t collision = ctx.r3.u32;
    __imp__sub_8248F210(ctx, base);

    if (g_eventCollisionOwner != 0)
        TrackUnit(g_eventCollisionOwner, LoadU32(collision));
}

// An object (r3) adds a SWA::CRigidBody (the shared_ptr at r4).
PPC_FUNC_IMPL(__imp__sub_825146A8);
PPC_FUNC(sub_825146A8)
{
    TrackUnit(ctx.r3.u32, LoadU32(ctx.r4.u32));
    __imp__sub_825146A8(ctx, base);
}

// The physics units' base constructor and destructor: forget the units that are gone, and the new ones made where they
// were.
PPC_FUNC_IMPL(__imp__sub_824932A8);
PPC_FUNC(sub_824932A8)
{
    ForgetUnit(ctx.r3.u32);
    __imp__sub_824932A8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82493098);
PPC_FUNC(sub_82493098)
{
    ForgetUnit(ctx.r3.u32);
    __imp__sub_82493098(ctx, base);
}

namespace
{
    // Projects world space points onto the screen with the camera's matrices.
    struct Projector
    {
        float View[16];
        float Projection[16];
        Vector3 Eye;
        ImVec2 Origin;
        ImVec2 Size;

        Vector3 ToView(const Vector3& p) const
        {
            return {
                p.x * View[0] + p.y * View[4] + p.z * View[8] + View[12],
                p.x * View[1] + p.y * View[5] + p.z * View[9] + View[13],
                p.x * View[2] + p.y * View[6] + p.z * View[10] + View[14] };
        }

        // In front of the near plane: right-handed, so the camera looks down -z.
        static bool IsVisible(const Vector3& viewPoint)
        {
            return viewPoint.z < -NEAR_PLANE;
        }

        // A point on the screen, with its depth as the game's depth buffer has it. The game's viewport reverses the
        // projection's depth (its minimum depth is 1, its maximum 0, SPEC_CONSTANT_REVERSE_Z): 1 at the near plane.
        ImVec2 ToScreen(const Vector3& v, float& depth) const
        {
            float x = v.x * Projection[0] + v.y * Projection[4] + v.z * Projection[8] + Projection[12];
            float y = v.x * Projection[1] + v.y * Projection[5] + v.z * Projection[9] + Projection[13];
            float z = v.x * Projection[2] + v.y * Projection[6] + v.z * Projection[10] + Projection[14];
            float w = v.x * Projection[3] + v.y * Projection[7] + v.z * Projection[11] + Projection[15];
            depth = 1.0f - z / w;
            return { Origin.x + (x / w * 0.5f + 0.5f) * Size.x, Origin.y + (0.5f - y / w * 0.5f) * Size.y };
        }

        // Whether the pixels behind the scene are left out (IMGUI_SHADER_MODIFIER_DEPTH_TEST): the triangles then carry
        // their depth in their texture coordinates.
        bool DepthTested = false;

        // A triangle, cut where it goes behind the near plane: the rails' triangles are long, and often reach behind
        // the camera.
        void Triangle(ImDrawList* drawList, const Vector3& a, const Vector3& b, const Vector3& c, ImU32 colour) const
        {
            Vector3 view[3] = { ToView(a), ToView(b), ToView(c) };

            // What's in front of the near plane: a polygon of up to 4 corners.
            Vector3 polygon[4];
            int corners = 0;
            for (int i = 0; i < 3; i++)
            {
                const Vector3& current = view[i];
                const Vector3& next = view[(i + 1) % 3];
                if (IsVisible(current))
                    polygon[corners++] = current;

                if (IsVisible(current) != IsVisible(next))
                    polygon[corners++] = current + (next - current) * ((-NEAR_PLANE - current.z) / (next.z - current.z));
            }

            for (int i = 1; i + 1 < corners; i++)
                ViewTriangle(drawList, polygon[0], polygon[i], polygon[i + 1], colour);
        }

        void ViewTriangle(ImDrawList* drawList, const Vector3& a, const Vector3& b, const Vector3& c, ImU32 colour) const
        {
            drawList->PrimReserve(3, 3);
            ImDrawIdx index = ImDrawIdx(drawList->_VtxCurrentIdx);
            for (int i = 0; i < 3; i++)
                drawList->PrimWriteIdx(ImDrawIdx(index + i));

            for (const Vector3* point : { &a, &b, &c })
            {
                float depth;
                ImVec2 position = ToScreen(*point, depth);
                drawList->PrimWriteVtx(position, DepthTested ? ImVec2(depth, 0.0f) : drawList->_Data->TexUvWhitePixel, colour);
            }
        }
    };
}

static ImU32 Fade(ImU32 colour, float amount)
{
    uint32_t alpha = uint32_t(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(amount, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

// Two directions square to an axis and to each other.
static void GetPerpendiculars(const Vector3& axis, Vector3& u, Vector3& v)
{
    Vector3 other = std::abs(axis.y) < 0.9f ? Vector3{ 0.0f, 1.0f, 0.0f } : Vector3{ 1.0f, 0.0f, 0.0f };
    u = Normalised(Cross(axis, other));
    v = Cross(axis, u);
}

// A shape's surface, in triangles, in the shape's own space; all the shapes are convex, around their centre.
struct Surface
{
    std::vector<Vector3> Triangles;
    Vector3 Centre{};

    void Quad(const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d)
    {
        Triangles.insert(Triangles.end(), { a, b, c, a, c, d });
    }
};

static constexpr int SEGMENTS = 24;
static constexpr int RINGS = 6;

// A cylinder between two points, or a capsule, with its rounded ends.
static void BuildTube(Surface& surface, const Vector3& a, const Vector3& b, float radius, bool rounded)
{
    Vector3 axis = Normalised(b - a);
    Vector3 u, v;
    GetPerpendiculars(axis, u, v);
    surface.Centre = (a + b) * 0.5f;

    auto around = [&](int i) { float angle = 2.0f * PI * i / SEGMENTS; return u * std::cos(angle) + v * std::sin(angle); };

    for (int i = 0; i < SEGMENTS; i++)
    {
        Vector3 r0 = around(i) * radius;
        Vector3 r1 = around(i + 1) * radius;
        surface.Quad(a + r0, a + r1, b + r1, b + r0);

        if (!rounded)
        {
            surface.Triangles.insert(surface.Triangles.end(), { a, a + r1, a + r0, b, b + r0, b + r1 });
            continue;
        }

        // The half spheres, ring by ring towards each end.
        for (int k = 0; k < RINGS; k++)
        {
            float from = 0.5f * PI * k / RINGS;
            float to = 0.5f * PI * (k + 1) / RINGS;
            for (float sign : { 1.0f, -1.0f })
            {
                const Vector3& end = sign > 0.0f ? b : a;
                Vector3 up = axis * (sign * radius);
                auto point = [&](const Vector3& direction, float latitude) { return end + direction * (radius * std::cos(latitude)) + up * std::sin(latitude); };
                surface.Quad(point(around(i), from), point(around(i + 1), from), point(around(i + 1), to), point(around(i), to));
            }
        }
    }
}

static void BuildSphere(Surface& surface, float radius)
{
    auto point = [&](int i, int k)
    {
        float longitude = 2.0f * PI * i / SEGMENTS;
        float latitude = PI * k / (2 * RINGS) - 0.5f * PI;
        return Vector3{ std::cos(latitude) * std::cos(longitude), std::sin(latitude), std::cos(latitude) * std::sin(longitude) } * radius;
    };

    for (int i = 0; i < SEGMENTS; i++)
    {
        for (int k = 0; k < 2 * RINGS; k++)
            surface.Quad(point(i, k), point(i + 1, k), point(i + 1, k + 1), point(i, k + 1));
    }
}

static void BuildBox(Surface& surface, const Vector3& half)
{
    auto corner = [&](int i) { return Vector3{ (i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z }; };
    for (auto [a, b, c, d] : { std::array{ 0, 1, 3, 2 }, std::array{ 4, 6, 7, 5 }, std::array{ 0, 4, 5, 1 }, std::array{ 2, 3, 7, 6 },
        std::array{ 0, 2, 6, 4 }, std::array{ 1, 5, 7, 3 } })
    {
        surface.Quad(corner(a), corner(b), corner(c), corner(d));
    }
}

static bool BuildShape(Surface& surface, uint32_t shape)
{
    uint32_t vftable = LoadU32(shape);
    float convexRadius = LoadFloat(shape + 0x10);

    if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
    {
        bool cylinder = vftable == CYLINDER_SHAPE_VFTABLE;
        float radius = convexRadius + (cylinder ? LoadFloat(shape + 0x14) : 0.0f);
        if (!(radius > 0.0f && radius < 100.0f))
            return false;

        BuildTube(surface, LoadVector(shape + 0x20), LoadVector(shape + 0x30), radius, !cylinder);
        return true;
    }

    if (vftable == SPHERE_SHAPE_VFTABLE)
    {
        if (!(convexRadius > 0.0f && convexRadius < 100.0f))
            return false;

        BuildSphere(surface, convexRadius);
        return true;
    }

    if (vftable == BOX_SHAPE_VFTABLE)
    {
        Vector3 half = LoadVector(shape + 0x20) + Vector3{ convexRadius, convexRadius, convexRadius };
        if (!(half.x > 0.0f && half.y > 0.0f && half.z > 0.0f && half.x < 100.0f && half.y < 100.0f && half.z < 100.0f))
            return false;

        BuildBox(surface, half);
        return true;
    }

    return false;
}

// Fills a face of a solid if it faces the camera, lighter the more it does, so solids read as see-through volumes.
// The face's outside is the side the direction points to.
static void DrawFace(ImDrawList* drawList, const Projector& projector, const Vector3& a, const Vector3& b, const Vector3& c,
    const Vector3& outside, ImU32 colour)
{
    Vector3 middle = (a + b + c) * (1.0f / 3.0f);
    Vector3 normal = Normalised(Cross(b - a, c - a));
    if (Dot(normal, outside) < 0.0f)
        normal = normal * -1.0f;

    float facing = Dot(normal, Normalised(projector.Eye - middle));
    if (facing <= 0.0f)
        return;

    float light = 0.45f + 0.55f * facing;
    auto channel = [&](int shift) { return uint32_t(((colour >> shift) & 0xFF) * light) << shift; };
    ImU32 shaded = channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | (colour & IM_COL32_A_MASK);
    projector.Triangle(drawList, a, b, c, shaded);
}

static void DrawSurface(ImDrawList* drawList, const Projector& projector, const Transform& transform, const Surface& surface, ImU32 colour)
{
    Vector3 centre = transform(surface.Centre);
    for (size_t i = 0; i + 2 < surface.Triangles.size(); i += 3)
    {
        Vector3 a = transform(surface.Triangles[i]);
        Vector3 b = transform(surface.Triangles[i + 1]);
        Vector3 c = transform(surface.Triangles[i + 2]);
        DrawFace(drawList, projector, a, b, c, (a + b + c) * (1.0f / 3.0f) - centre, colour);
    }
}

// Calls visit(transform, shape) for each of the shapes a shape is made of, with where it is.
template<typename Visit>
static void ForEachLeafShape(const Transform& transform, uint32_t shape, const Visit& visit, int depth = 0)
{
    if (depth > 4 || !IsReadable(shape))
        return;

    uint32_t vftable = LoadU32(shape);
    if (vftable == CONVEX_TRANSLATE_SHAPE_VFTABLE)
    {
        Transform translation;
        translation.translation = LoadVector(shape + 0x20);
        ForEachLeafShape(transform * translation, LoadU32(shape + 0x18), visit, depth + 1);
        return;
    }

    if (vftable == CONVEX_TRANSFORM_SHAPE_VFTABLE)
    {
        ForEachLeafShape(transform * Transform::At(shape + 0x20), LoadU32(shape + 0x18), visit, depth + 1);
        return;
    }

    if (vftable == TRANSFORM_SHAPE_VFTABLE)
    {
        ForEachLeafShape(transform * Transform::At(shape + 0x30), LoadU32(shape + 0x14), visit, depth + 1);
        return;
    }

    if (vftable == LIST_SHAPE_VFTABLE)
    {
        uint32_t children = LoadU32(shape + 0x18);
        uint32_t count = std::min(LoadU32(shape + 0x1C), 64u);
        for (uint32_t i = 0; i < count; i++)
            ForEachLeafShape(transform, LoadU32(children + i * 16), visit, depth + 1);

        return;
    }

    visit(transform, shape);
}

static void DrawShape(ImDrawList* drawList, const Projector& projector, const Transform& transform, uint32_t shape, ImU32 colour)
{
    ForEachLeafShape(transform, shape, [&](const Transform& leafTransform, uint32_t leaf)
    {
        Surface surface;
        if (BuildShape(surface, leaf))
            DrawSurface(drawList, projector, leafTransform, surface, colour);
    });
}

static bool IsAttacking(uint32_t context)
{
    uint32_t flags = LoadU32(context + CONTEXT_STATE_FLAGS_OFFSET);
    return flags != 0 && LoadU8(LoadU32(flags + 4) + ATTACKING_FLAG) != 0;
}

// A rigid body or shape phantom in the world, where it is now; false if it isn't one, or isn't in the world.
static bool GetCollidable(uint32_t body, Transform& transform, uint32_t& shape)
{
    uint32_t vftable = LoadU32(body);
    if (vftable != RIGID_BODY_VFTABLE && vftable != SIMPLE_SHAPE_PHANTOM_VFTABLE && vftable != CACHING_SHAPE_PHANTOM_VFTABLE)
        return false;

    uint32_t motion = LoadU32(body + COLLIDABLE_MOTION_OFFSET);
    if (LoadU32(body + RIGID_BODY_WORLD_OFFSET) == 0 || !IsReadable(motion))
        return false;

    transform = Transform::At(motion);
    shape = LoadU32(body + RIGID_BODY_SHAPE_OFFSET);
    return true;
}

// A solid arrow from the player's feet along his facing, a little above them: the flat arrow's outline, given thickness.
static void DrawFacingArrow(ImDrawList* drawList, const Projector& projector, uint32_t context)
{
    uint32_t node = LoadU32(context + CONTEXT_MATRIX_NODE_OFFSET);
    if (!IsReadable(node))
        return;

    uint32_t matrix = node + MATRIX_NODE_MATRIX_OFFSET;
    Vector3 up = LoadVector(matrix + 16);
    Vector3 front = LoadVector(matrix + 32);
    Vector3 position = LoadVector(matrix + 48);
    if (!(Length(up) > 0.5f) || !(Length(front) > 0.5f) || !std::isfinite(Length(position)))
        return;

    up = Normalised(up);
    front = Normalised(front);
    Vector3 side = Normalised(Cross(up, front));

    // The outline, counterclockwise from the back of the shaft (along the facing, then to the side), so each edge's
    // outside is to its right.
    constexpr float NECK = ARROW_LENGTH - ARROW_HEAD_LENGTH;
    constexpr std::array<std::array<float, 2>, 7> OUTLINE =
    { {
        { 0.0f, -ARROW_HALF_WIDTH }, { NECK, -ARROW_HALF_WIDTH }, { NECK, -ARROW_HEAD_HALF_WIDTH }, { ARROW_LENGTH, 0.0f },
        { NECK, ARROW_HEAD_HALF_WIDTH }, { NECK, ARROW_HALF_WIDTH }, { 0.0f, ARROW_HALF_WIDTH },
    } };

    Vector3 bottom = position + up * ARROW_HEIGHT;
    Vector3 top = bottom + up * ARROW_THICKNESS;
    auto at = [&](const Vector3& base, int i) { return base + front * OUTLINE[i][0] + side * OUTLINE[i][1]; };

    // The top and the bottom: the shaft, then the head.
    for (auto [base, outside] : { std::pair{ top, up }, std::pair{ bottom, up * -1.0f } })
    {
        DrawFace(drawList, projector, at(base, 0), at(base, 1), at(base, 5), outside, ARROW_COLOUR);
        DrawFace(drawList, projector, at(base, 0), at(base, 5), at(base, 6), outside, ARROW_COLOUR);
        DrawFace(drawList, projector, at(base, 2), at(base, 3), at(base, 4), outside, ARROW_COLOUR);
    }

    // The sides.
    for (int i = 0; i < int(OUTLINE.size()); i++)
    {
        int next = (i + 1) % int(OUTLINE.size());
        float dx = OUTLINE[next][0] - OUTLINE[i][0];
        float dy = OUTLINE[next][1] - OUTLINE[i][1];
        Vector3 outside = front * dy - side * dx;

        DrawFace(drawList, projector, at(bottom, i), at(bottom, next), at(top, next), outside, ARROW_COLOUR);
        DrawFace(drawList, projector, at(bottom, i), at(top, next), at(top, i), outside, ARROW_COLOUR);
    }
}

// The player's collision that's on, by ID, with its rigid body.
static std::vector<std::pair<uint32_t, uint32_t>> GetPlayerBodies(uint32_t context)
{
    std::vector<std::pair<uint32_t, uint32_t>> bodies;
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    uint32_t container = player != 0 ? LoadU32(player + PLAYER_COLLISIONS_OFFSET) : 0;
    uint32_t head = container != 0 ? LoadU32(container + COLLISIONS_HEAD_OFFSET) : 0;
    if (head == 0)
        return bodies;

    // The tree's nodes: left at +0, right at +8, the ID at +12 and the shared_ptr at +16, the head's flag at +25.
    std::vector<uint32_t> nodes{ LoadU32(head + 4) };
    for (size_t i = 0; i < nodes.size() && i < 64; i++)
    {
        uint32_t node = nodes[i];
        if (node == 0 || node == head || LoadU8(node + 25) != 0)
            continue;

        nodes.push_back(LoadU32(node));
        nodes.push_back(LoadU32(node + 8));

        uint32_t collision = LoadU32(node + 16);
        if (LoadU32(collision) != EVENT_COLLISION_VFTABLE)
            continue;

        uint32_t body = LoadU32(collision + EVENT_COLLISION_BODY_OFFSET);
        if (LoadU32(body) == RIGID_BODY_VFTABLE && LoadU32(body + RIGID_BODY_WORLD_OFFSET) != 0)
            bodies.emplace_back(LoadU32(node + 12), body);
    }

    return bodies;
}

// The player's collision that's on: the body, day Sonic's path collision, and what touches rings, objects and triggers.
static void DrawPlayerCollision(ImDrawList* drawList, const Projector& projector, uint32_t context, bool werehog)
{
    auto bodies = GetPlayerBodies(context);

    // While day Sonic attacks (a slide, say), the collision around his body is what reaches enemies first.
    bool attacking = !werehog && IsAttacking(context);

    // The body last, over the others around it, so it stands out.
    std::stable_partition(bodies.begin(), bodies.end(), [](auto& entry) { return entry.first != BODY_COLLISION_ID; });
    for (auto& [id, body] : bodies)
    {
        ImU32 colour = id == BODY_COLLISION_ID ? BODY_COLOUR : !werehog && id == SONIC_PATH_COLLISION_ID ? PATH_COLOUR :
            attacking ? ATTACK_COLOUR : TRIGGER_COLOUR;
        DrawShape(drawList, projector, Transform(body), LoadU32(body + RIGID_BODY_SHAPE_OFFSET), colour);
    }
}

// How far other objects' collision is drawn.
static constexpr float OBJECT_DRAW_DISTANCE = 150.0f;

// The collision of the enemies and of the goal ring that's on.
static void DrawObjectCollision(ImDrawList* drawList, const Projector& projector)
{
    std::vector<std::pair<uint32_t, TrackedUnit>> units;
    {
        std::lock_guard lock(g_unitsMutex);
        units.assign(g_units.begin(), g_units.end());
    }

    for (auto& [unit, tracked] : units)
    {
        ObjectKind kind = tracked.Kind;
        if (kind == ObjectKind::None || kind == ObjectKind::Ring || kind == ObjectKind::Terrain)
            continue;

        uint32_t unitVftable = LoadU32(unit);
        if (unitVftable != EVENT_COLLISION_VFTABLE && unitVftable != RIGID_BODY_UNIT_VFTABLE)
            continue;

        Transform transform;
        uint32_t shape;
        if (!GetCollidable(LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET), transform, shape) ||
            Length(transform.translation - projector.Eye) > OBJECT_DRAW_DISTANCE)
        {
            continue;
        }

        DrawShape(drawList, projector, transform, shape, kind == ObjectKind::GoalRing ? GOAL_COLOUR : ENEMY_COLOUR);
    }
}

static const RailMesh& GetRailMesh(uint32_t mesh, uint32_t vertices, uint32_t vertexStride, uint32_t vertexCount, uint32_t indices,
    uint32_t indexStride, bool indices32, uint32_t triangles)
{
    auto& rail = g_railMeshes[mesh];
    rail.Used = true;
    if (rail.Vertices == vertices && rail.Indices == indices && rail.Triangles == triangles)
        return rail;

    rail = { vertices, indices, triangles };
    rail.Used = true;
    rail.Points.resize(vertexCount);
    rail.Normals.assign(vertexCount, {});
    for (uint32_t v = 0; v < vertexCount; v++)
        rail.Points[v] = LoadVector(vertices + v * vertexStride);

    std::map<std::pair<uint32_t, uint32_t>, std::pair<int, uint32_t>> edges;
    for (uint32_t t = 0; t < triangles; t++)
    {
        std::array<uint32_t, 3> face;
        for (uint32_t k = 0; k < 3; k++)
        {
            uint32_t address = indices + t * indexStride + k * (indices32 ? 4 : 2);
            face[k] = std::min(indices32 ? LoadU32(address) : uint32_t(LoadU16(address)), vertexCount - 1);
        }

        // The strips keep one winding, so their normals agree.
        Vector3 normal = Normalised(Cross(rail.Points[face[1]] - rail.Points[face[0]], rail.Points[face[2]] - rail.Points[face[0]]));
        for (uint32_t index : face)
            rail.Normals[index] = rail.Normals[index] + normal;

        for (uint32_t k = 0; k < 3; k++)
        {
            auto& edge = edges[std::minmax(face[k], face[(k + 1) % 3])];
            edge.first++;
            edge.second = face[(k + 2) % 3];
        }

        rail.Faces.push_back(face);
    }

    for (auto& normal : rail.Normals)
        normal = Normalised(normal);

    for (auto& [edge, count] : edges)
    {
        if (count.first == 1)
            rail.Edges.push_back({ edge.first, edge.second, count.second });
    }

    return rail;
}

// A rail as the slab Havok makes of it: its strip pushed out by the triangles' radius on both sides, with the strip's
// edges closing it.
static void DrawRail(ImDrawList* drawList, const Projector& projector, uint32_t path)
{
    if (LoadU32(path) != GRIND_PATH_VFTABLE)
        return;

    uint32_t part = LoadU32(path + GRIND_PATH_COLLISION_OFFSET);
    if (LoadU32(part) != PATH_RIGID_BODY_COLLISION_PART_VFTABLE)
        return;

    uint32_t collision = part - PATH_RIGID_BODY_COLLISION_PART_OFFSET;
    uint32_t body = LoadU32(collision + PATH_RIGID_BODY_COLLISION_BODY_OFFSET);
    if (LoadU32(collision) != PATH_RIGID_BODY_COLLISION_VFTABLE || LoadU32(body) != RIGID_BODY_VFTABLE)
        return;

    uint32_t mopp = LoadU32(body + RIGID_BODY_SHAPE_OFFSET);
    uint32_t mesh = LoadU32(mopp + MOPP_SHAPE_CHILD_OFFSET);
    if (LoadU32(mopp) != MOPP_SHAPE_VFTABLE || LoadU32(mesh) != EXTENDED_MESH_SHAPE_VFTABLE || LoadU32(mesh + EXTENDED_MESH_SUBPARTS_OFFSET + 4) < 1)
        return;

    uint32_t subpart = LoadU32(mesh + EXTENDED_MESH_SUBPARTS_OFFSET);
    uint32_t triangles = LoadU32(subpart + 0x10);
    uint32_t vertices = LoadU32(subpart + 0x14);
    uint32_t vertexStride = LoadU32(subpart + 0x18);
    uint32_t vertexCount = LoadU32(subpart + 0x1C);
    uint32_t indices = LoadU32(subpart + 0x30);
    uint32_t indexStride = LoadU32(subpart + 0x34);
    uint8_t indexType = LoadU8(subpart + 0x38);
    if (triangles == 0 || triangles > 65536 || vertexCount > 65536 || vertexStride < 12 || (indexType != 1 && indexType != 2))
        return;

    if (vertexCount == 0)
        return;

    float radius = LoadFloat(mesh + EXTENDED_MESH_RADIUS_OFFSET);
    if (!(radius > 0.0f && radius < 1.0f))
        radius = 0.05f;

    const RailMesh& rail = GetRailMesh(mesh, vertices, vertexStride, vertexCount, indices, indexStride, indexType == 2, triangles);
    Transform transform(body);
    auto top = [&](uint32_t index, float side) { return transform(rail.Points[index]) + transform.Rotate(rail.Normals[index]) * (radius * side); };
    auto fade = [&](const Vector3& point) { return 1.0f - (Length(point - projector.Eye) - RAIL_FADE_START) / (RAIL_FADE_END - RAIL_FADE_START); };

    for (auto& face : rail.Faces)
    {
        Vector3 middle = transform((rail.Points[face[0]] + rail.Points[face[1]] + rail.Points[face[2]]) * (1.0f / 3.0f));
        float amount = fade(middle);
        if (amount <= 0.0f)
            continue;

        Vector3 normal = transform.Rotate(rail.Normals[face[0]] + rail.Normals[face[1]] + rail.Normals[face[2]]);
        for (float side : { 1.0f, -1.0f })
            DrawFace(drawList, projector, top(face[0], side), top(face[1], side), top(face[2], side), normal * side, Fade(RAIL_COLOUR, amount));
    }

    for (auto& [first, second, other] : rail.Edges)
    {
        Vector3 a = transform(rail.Points[first]);
        Vector3 b = transform(rail.Points[second]);
        float amount = fade((a + b) * 0.5f);
        if (amount <= 0.0f)
            continue;

        // Outwards: away from the triangle's other corner.
        Vector3 outside = (a + b) * 0.5f - transform(rail.Points[other]);
        ImU32 colour = Fade(RAIL_COLOUR, amount);
        DrawFace(drawList, projector, top(first, 1.0f), top(second, 1.0f), top(second, -1.0f), outside, colour);
        DrawFace(drawList, projector, top(first, 1.0f), top(second, -1.0f), top(first, -1.0f), outside, colour);
    }
}

void CollisionOverlay::Draw()
{
    // Hidden with the views (F11).
    if (!IsEnabled() || !CollisionView::IsShown())
        return;

    // Only in levels, with the camera that was updated last.
    bool werehog;
    uint32_t context = TasHud::GetPlayerContext(werehog);
    if (context == 0 || g_camera == 0 || LoadU32(g_camera) != g_cameraVftable)
        return;

    Projector projector;
    for (int i = 0; i < 16; i++)
    {
        projector.View[i] = LoadFloat(g_camera + CAMERA_VIEW_OFFSET + i * 4);
        projector.Projection[i] = LoadFloat(g_camera + CAMERA_PROJECTION_OFFSET + i * 4);
    }

    projector.Eye = LoadVector(g_camera + CAMERA_POSITION_OFFSET);

    // The scene keeps its aspect ratio (the projection's), centred in the window.
    auto& res = ImGui::GetIO().DisplaySize;
    float aspectRatio = projector.Projection[0] != 0.0f ? projector.Projection[5] / projector.Projection[0] : res.x / res.y;
    if (!(aspectRatio > 0.0f) || !std::isfinite(aspectRatio))
        return;

    projector.Size = res.x / res.y > aspectRatio ? ImVec2(res.y * aspectRatio, res.y) : ImVec2(res.x, res.x / aspectRatio);
    projector.Origin = { (res.x - projector.Size.x) / 2, (res.y - projector.Size.y) / 2 };

    auto drawList = ImGui::GetBackgroundDrawList();

    // Without the anti-aliased fringes, which would overlap between the triangles of a see-through surface.
    ImDrawListFlags flags = drawList->Flags;
    drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;

    // With the game's anti-aliasing, into a target with as many samples as its scene, so the edges are smoothed like
    // the scene's (and the faces' shared edges still don't overlap); it's put onto the screen after.
    GuestSurface* sceneDepthStencil = Video::GetSceneDepthStencil();
    uint32_t sampleCount = sceneDepthStencil != nullptr ? uint32_t(sceneDepthStencil->sampleCount) : 1;
    bool multisampled = sampleCount == 2 || sampleCount == 4 || sampleCount == 8;

    // Behind walls: the game's depth buffer as the texture (multisampled: tested per sample). Without it, over everything.
    GuestSurface* depthStencil = g_depthTested ? sceneDepthStencil : nullptr;
    projector.DepthTested = depthStencil != nullptr;
    if (projector.DepthTested)
        drawList->PushTextureID(ImTextureID(depthStencil));

    if (multisampled)
    {
        auto callbackData = AddImGuiCallback(ImGuiCallback::SetMultisampled);
        callbackData->setMultisampled.sampleCount = sampleCount;
        callbackData->setMultisampled.shaderModifier = projector.DepthTested ? IMGUI_SHADER_MODIFIER_MULTISAMPLED_DEPTH_TEST : IMGUI_SHADER_MODIFIER_MULTISAMPLED;
    }
    else if (projector.DepthTested)
    {
        SetShaderModifier(IMGUI_SHADER_MODIFIER_DEPTH_TEST);
    }

    if (*SWA::SGlobals::ms_IsCollisionRender)
    {
        std::vector<uint32_t> paths;
        {
            std::lock_guard lock(g_grindPathsMutex);
            paths = g_grindPaths;
        }

        for (auto& [mesh, rail] : g_railMeshes)
            rail.Used = false;

        for (uint32_t path : paths)
            DrawRail(drawList, projector, path);

        // Forget the meshes of the rails that are gone.
        std::erase_if(g_railMeshes, [](auto& entry) { return !entry.second.Used; });
    }

    DrawObjectCollision(drawList, projector);
    DrawPlayerCollision(drawList, projector, context, werehog);
    DrawFacingArrow(drawList, projector, context);

    if (multisampled)
    {
        auto callbackData = AddImGuiCallback(ImGuiCallback::SetMultisampled);
        callbackData->setMultisampled.sampleCount = 0;
        callbackData->setMultisampled.shaderModifier = IMGUI_SHADER_MODIFIER_NONE;
    }
    else if (projector.DepthTested)
    {
        SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);
    }

    if (projector.DepthTested)
        drawList->PopTextureID();

    drawList->Flags = flags;
}

// A shape seen from above: the points a segment between two ends and a radius around it covers (a capsule's, a
// cylinder's, a sphere's with both ends at its centre, a box's roughly), in world space.
struct Footprint
{
    Vector3 A;
    Vector3 B;
    float Radius;

    Vector3 Centre() const
    {
        return (A + B) * 0.5f;
    }
};

static void GetFootprints(const Transform& transform, uint32_t shape, std::vector<Footprint>& footprints)
{
    ForEachLeafShape(transform, shape, [&](const Transform& leafTransform, uint32_t leaf)
    {
        uint32_t vftable = LoadU32(leaf);
        float convexRadius = LoadFloat(leaf + 0x10);
        if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
        {
            float radius = convexRadius + (vftable == CYLINDER_SHAPE_VFTABLE ? LoadFloat(leaf + 0x14) : 0.0f);
            if (radius > 0.0f && radius < 100.0f)
                footprints.push_back({ leafTransform(LoadVector(leaf + 0x20)), leafTransform(LoadVector(leaf + 0x30)), radius });
        }
        else if (vftable == SPHERE_SHAPE_VFTABLE)
        {
            if (convexRadius > 0.0f && convexRadius < 100.0f)
                footprints.push_back({ leafTransform.translation, leafTransform.translation, convexRadius });
        }
        else if (vftable == BOX_SHAPE_VFTABLE)
        {
            Vector3 half = LoadVector(leaf + 0x20);
            float radius = std::max(half.x, half.z) + convexRadius;
            if (radius > 0.0f && radius < 100.0f)
                footprints.push_back({ leafTransform.translation, leafTransform.translation, radius });
        }
    });
}

// How far a point is from a segment, seen from above.
static float HorizontalDistance(const Vector3& point, const Vector3& a, const Vector3& b)
{
    float abX = b.x - a.x;
    float abZ = b.z - a.z;
    float apX = point.x - a.x;
    float apZ = point.z - a.z;
    float lengthSquared = abX * abX + abZ * abZ;
    float t = lengthSquared > 0.0f ? std::clamp((apX * abX + apZ * abZ) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return std::hypot(apX - abX * t, apZ - abZ * t);
}

// The game's collision categories. A collision's filter info (hkpCollidable's, at +0x2C of the Havok object) is an index
// into the game's table of category sets (sub_8248EAF0 adds them: a vector at *0x83366F54, begin +4, end +8, 20 bytes
// each: its own categories (a 64-bit set, bits 0-31 in the first word), those it collides with, flags).
// The homing attack's target search (sub_8230E990) uses two of them, both with no categories of their own, so they meet
// a collision whose own set has theirs (the category numbers are read at run time):
// - its phantom (sub_82379718) collides with the one at 0x83367180 (58): the targets. Answering the priority message
//   isn't it: the E-Fighter's missiles answer nothing and are targets. In DragonRoadAct1.ltm, filter infos 0x30 and 0x2F
//   had it (enemies, E-Fighter missiles, springs, rockets, up reels); Sonic's 0x14, camera volumes' 8, point markers' 7,
//   physics objects' 0x3E5 and CEnemyObjEFMissile's 0x3E9 didn't;
// - its line of sight raycast (sub_822EC588 -> hkpWorld::castRay, from Sonic to the target, both 0.5 higher; any hit
//   drops the target) the one at 0x83367060.
static constexpr uint32_t COLLIDABLE_FILTER_INFO_OFFSET = 0x2C;
static constexpr uint32_t COLLISION_CATEGORY_TABLE = 0x83366F54;
static constexpr uint32_t COLLISION_CATEGORY_ENTRY_SIZE = 20;
static constexpr uint32_t HOMING_TARGET_CATEGORY = 0x83367180;
static constexpr uint32_t HOMING_SIGHT_CATEGORY = 0x83367060;

static bool HasCollisionCategory(uint32_t body, uint32_t categoryAddress)
{
    uint32_t category = LoadU32(categoryAddress);
    uint32_t table = LoadU32(COLLISION_CATEGORY_TABLE);
    if (category >= 64 || !IsReadable(table) || !IsReadable(body))
        return false;

    uint32_t begin = LoadU32(table + 4);
    uint32_t end = LoadU32(table + 8);
    uint32_t info = LoadU32(body + COLLIDABLE_FILTER_INFO_OFFSET);
    if (!IsReadable(begin) || end < begin || info >= (end - begin) / COLLISION_CATEGORY_ENTRY_SIZE)
        return false;

    uint32_t categories = LoadU32(begin + info * COLLISION_CATEGORY_ENTRY_SIZE + (category / 32) * 4);
    return (categories >> (category % 32)) & 1;
}

static bool IsHomingTargetCollision(uint32_t body)
{
    return HasCollisionCategory(body, HOMING_TARGET_CATEGORY);
}

// The stage's collision on the radar: its walls (triangles steeper than 60 degrees), and the edges of its floors where
// no other floor goes on (ledges, the sides of pits and platforms). Read once per rigid body of the stage's terrain
// object (a MOPP tree over an extended mesh, like the rails), in world space; the floors' edges are matched by their
// corners (rounded to 1 cm), also between bodies, so where two pieces of floor meet isn't drawn.
static constexpr float TERRAIN_FLOOR_NORMAL_Y = 0.5f;
// Triangle subparts are 64 bytes each (the mesh's destructor frees count * 64).
static constexpr uint32_t TRIANGLES_SUBPART_SIZE = 64;
static constexpr uint32_t TERRAIN_MAX_TRIANGLES = 4 * 1024 * 1024;

struct TerrainSegment
{
    Vector3 A;
    Vector3 B;
    float Low;
    float High;
};

struct EdgeKey
{
    std::array<int32_t, 6> Corners;

    auto operator<=>(const EdgeKey&) const = default;
};

static EdgeKey GetEdgeKey(const Vector3& a, const Vector3& b)
{
    auto round = [](float value) { return int32_t(std::clamp(std::lround(value * 100.0f), -2000000000l, 2000000000l)); };
    std::array<int32_t, 3> first{ round(a.x), round(a.y), round(a.z) };
    std::array<int32_t, 3> second{ round(b.x), round(b.y), round(b.z) };
    if (second < first)
        std::swap(first, second);

    return { { first[0], first[1], first[2], second[0], second[1], second[2] } };
}

struct TerrainBody
{
    uint32_t Shape = 0;
    Transform Placement;
    std::vector<TerrainSegment> Walls;
    // The floors' edges that only one of this body's floor triangles has.
    std::vector<std::pair<EdgeKey, TerrainSegment>> Edges;
    // All its triangles, and whether the homing attack's line of sight raycast hits it.
    std::vector<std::array<Vector3, 3>> Triangles;
    bool BlocksSight = false;
    bool Used = false;
};

// The triangles that block the homing attack's line of sight, by 8 m square of the ground (each in every square its
// bounds touch, up to 64 of them).
static constexpr float SIGHT_GRID_CELL = 8.0f;
static std::vector<std::array<Vector3, 3>> g_sightTriangles;
static std::unordered_map<uint64_t, std::vector<uint32_t>> g_sightGrid;

static uint64_t GetSightCell(int32_t x, int32_t z)
{
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
}

static std::unordered_map<uint32_t, TerrainBody> g_terrainBodies;
static std::vector<TerrainSegment> g_terrainLedges;

static bool IsSamePlacement(const Transform& a, const Transform& b)
{
    for (int i = 0; i < 3; i++)
    {
        if (Length(a.columns[i] - b.columns[i]) > 0.001f)
            return false;
    }

    return Length(a.translation - b.translation) <= 0.001f;
}

// One extended mesh's triangles.
static void ReadTerrainMesh(const Transform& transform, uint32_t mesh, TerrainBody& body)
{
    uint32_t subparts = LoadU32(mesh + EXTENDED_MESH_SUBPARTS_OFFSET);
    uint32_t subpartCount = std::min(LoadU32(mesh + EXTENDED_MESH_SUBPARTS_OFFSET + 4), 4096u);
    std::vector<std::pair<EdgeKey, TerrainSegment>> floorEdges;

    for (uint32_t s = 0; s < subpartCount; s++)
    {
        uint32_t subpart = subparts + s * TRIANGLES_SUBPART_SIZE;
        if (!IsReadable(subpart))
            break;

        uint32_t triangles = LoadU32(subpart + 0x10);
        uint32_t vertices = LoadU32(subpart + 0x14);
        uint32_t vertexStride = LoadU32(subpart + 0x18);
        uint32_t vertexCount = LoadU32(subpart + 0x1C);
        uint32_t indices = LoadU32(subpart + 0x30);
        uint32_t indexStride = LoadU32(subpart + 0x34);
        uint8_t indexType = LoadU8(subpart + 0x38);
        if (triangles == 0 || triangles > TERRAIN_MAX_TRIANGLES || vertexCount == 0 || vertexCount > TERRAIN_MAX_TRIANGLES ||
            vertexStride < 12 || vertexStride > 256 || (indexType != 1 && indexType != 2) || !IsReadable(vertices) || !IsReadable(indices))
        {
            continue;
        }

        bool indices32 = indexType == 2;
        std::vector<Vector3> points(vertexCount);
        for (uint32_t v = 0; v < vertexCount; v++)
            points[v] = transform(LoadVector(vertices + v * vertexStride));

        for (uint32_t t = 0; t < triangles; t++)
        {
            std::array<Vector3, 3> corners;
            for (uint32_t k = 0; k < 3; k++)
            {
                uint32_t address = indices + t * indexStride + k * (indices32 ? 4 : 2);
                corners[k] = points[std::min(indices32 ? LoadU32(address) : uint32_t(LoadU16(address)), vertexCount - 1)];
            }

            Vector3 normal = Cross(corners[1] - corners[0], corners[2] - corners[0]);
            float area = Length(normal);
            if (!(area > 1e-6f) || !std::isfinite(area))
                continue;

            if (body.BlocksSight)
                body.Triangles.push_back(corners);

            float low = std::min({ corners[0].y, corners[1].y, corners[2].y });
            float high = std::max({ corners[0].y, corners[1].y, corners[2].y });
            if (std::abs(normal.y) / area >= TERRAIN_FLOOR_NORMAL_Y)
            {
                for (uint32_t k = 0; k < 3; k++)
                {
                    const Vector3& a = corners[k];
                    const Vector3& b = corners[(k + 1) % 3];
                    floorEdges.push_back({ GetEdgeKey(a, b), { a, b, std::min(a.y, b.y), std::max(a.y, b.y) } });
                }

                continue;
            }

            // A wall seen from above is about a line: between its two corners furthest apart along the ground.
            float longest = 0.0f;
            int first = 0;
            int second = 1;
            for (int k = 0; k < 3; k++)
            {
                const Vector3& a = corners[k];
                const Vector3& b = corners[(k + 1) % 3];
                float distance = std::hypot(b.x - a.x, b.z - a.z);
                if (distance > longest)
                {
                    longest = distance;
                    first = k;
                    second = (k + 1) % 3;
                }
            }

            if (longest >= 0.02f)
                body.Walls.push_back({ corners[first], corners[second], low, high });
        }
    }

    std::sort(floorEdges.begin(), floorEdges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < floorEdges.size();)
    {
        size_t end = i + 1;
        while (end < floorEdges.size() && floorEdges[end].first == floorEdges[i].first)
            end++;

        if (end - i == 1)
            body.Edges.push_back(floorEdges[i]);

        i = end;
    }
}

// Brings the bodies up to date with the terrain bodies in the world; called with g_unitsMutex not held.
static void UpdateTerrain()
{
    std::vector<uint32_t> bodies;
    {
        std::lock_guard lock(g_unitsMutex);
        for (auto& [unit, tracked] : g_units)
        {
            if (tracked.Kind == ObjectKind::Terrain && LoadU32(unit) == RIGID_BODY_UNIT_VFTABLE)
                bodies.push_back(LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET));
        }
    }

    bool changed = false;
    for (auto& [address, body] : g_terrainBodies)
        body.Used = false;

    for (uint32_t address : bodies)
    {
        Transform transform;
        uint32_t shape;
        if (!GetCollidable(address, transform, shape) || LoadU32(address) != RIGID_BODY_VFTABLE)
            continue;

        auto& body = g_terrainBodies[address];
        body.Used = true;
        if (body.Shape == shape && IsSamePlacement(body.Placement, transform))
            continue;

        body = { shape, transform };
        body.Used = true;
        body.BlocksSight = HasCollisionCategory(address, HOMING_SIGHT_CATEGORY);
        changed = true;
        ForEachLeafShape(transform, shape, [&](const Transform& leafTransform, uint32_t leaf)
        {
            uint32_t vftable = LoadU32(leaf);
            if (vftable == MOPP_SHAPE_VFTABLE)
            {
                leaf = LoadU32(leaf + MOPP_SHAPE_CHILD_OFFSET);
                vftable = LoadU32(leaf);
            }

            if (vftable == EXTENDED_MESH_SHAPE_VFTABLE || vftable == STORAGE_EXTENDED_MESH_SHAPE_VFTABLE)
                ReadTerrainMesh(leafTransform, leaf, body);
        });
    }

    changed |= std::erase_if(g_terrainBodies, [](const auto& entry) { return !entry.second.Used; }) != 0;
    if (!changed)
        return;


    // The floors' edges no other body has either.
    std::vector<const std::pair<EdgeKey, TerrainSegment>*> edges;
    for (auto& [address, body] : g_terrainBodies)
    {
        for (auto& edge : body.Edges)
            edges.push_back(&edge);
    }

    std::sort(edges.begin(), edges.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    g_terrainLedges.clear();
    for (size_t i = 0; i < edges.size();)
    {
        size_t end = i + 1;
        while (end < edges.size() && edges[end]->first == edges[i]->first)
            end++;

        if (end - i == 1)
            g_terrainLedges.push_back(edges[i]->second);

        i = end;
    }

    // The line of sight's triangles, by square.
    g_sightTriangles.clear();
    g_sightGrid.clear();
    for (auto& [address, body] : g_terrainBodies)
    {
        for (auto& triangle : body.Triangles)
        {
            float minX = std::min({ triangle[0].x, triangle[1].x, triangle[2].x });
            float maxX = std::max({ triangle[0].x, triangle[1].x, triangle[2].x });
            float minZ = std::min({ triangle[0].z, triangle[1].z, triangle[2].z });
            float maxZ = std::max({ triangle[0].z, triangle[1].z, triangle[2].z });
            int32_t x0 = int32_t(std::floor(minX / SIGHT_GRID_CELL)), x1 = int32_t(std::floor(maxX / SIGHT_GRID_CELL));
            int32_t z0 = int32_t(std::floor(minZ / SIGHT_GRID_CELL)), z1 = int32_t(std::floor(maxZ / SIGHT_GRID_CELL));
            if (int64_t(x1 - x0 + 1) * (z1 - z0 + 1) > 64)
                continue;

            uint32_t index = uint32_t(g_sightTriangles.size());
            g_sightTriangles.push_back(triangle);
            for (int32_t x = x0; x <= x1; x++)
            {
                for (int32_t z = z0; z <= z1; z++)
                    g_sightGrid[GetSightCell(x, z)].push_back(index);
            }
        }
    }
}

// Whether the segment from a to b goes through the triangle (either side; Moller-Trumbore).
static bool SegmentHitsTriangle(const Vector3& a, const Vector3& b, const std::array<Vector3, 3>& triangle)
{
    Vector3 direction = b - a;
    Vector3 edge1 = triangle[1] - triangle[0];
    Vector3 edge2 = triangle[2] - triangle[0];
    Vector3 p = Cross(direction, edge2);
    float determinant = Dot(edge1, p);
    if (std::abs(determinant) < 1e-8f)
        return false;

    float inverse = 1.0f / determinant;
    Vector3 s = a - triangle[0];
    float u = Dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f)
        return false;

    Vector3 q = Cross(s, edge1);
    float v = Dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    float t = Dot(edge2, q) * inverse;
    return t >= 0.0f && t <= 1.0f;
}

void CollisionOverlay::UpdateStage()
{
    UpdateTerrain();
}

bool CollisionOverlay::IsSightBlocked(float ax, float ay, float az, float bx, float by, float bz)
{
    Vector3 a{ ax, ay, az };
    Vector3 b{ bx, by, bz };
    int32_t x0 = int32_t(std::floor(std::min(ax, bx) / SIGHT_GRID_CELL)), x1 = int32_t(std::floor(std::max(ax, bx) / SIGHT_GRID_CELL));
    int32_t z0 = int32_t(std::floor(std::min(az, bz) / SIGHT_GRID_CELL)), z1 = int32_t(std::floor(std::max(az, bz) / SIGHT_GRID_CELL));
    if (int64_t(x1 - x0 + 1) * (z1 - z0 + 1) > 256)
        return false;

    for (int32_t x = x0; x <= x1; x++)
    {
        for (int32_t z = z0; z <= z1; z++)
        {
            auto it = g_sightGrid.find(GetSightCell(x, z));
            if (it == g_sightGrid.end())
                continue;

            for (uint32_t index : it->second)
            {
                if (SegmentHitsTriangle(a, b, g_sightTriangles[index]))
                    return true;
            }
        }
    }

    return false;
}

static const ImU32 RADAR_RING_COLOUR = IM_COL32(255, 210, 40, 255);
static const ImU32 RADAR_TOUCH_COLOUR = IM_COL32(80, 255, 80, 255);
static const ImU32 RADAR_ITEM_COLOUR = IM_COL32(255, 80, 255, 230);

// What the radar keeps from the last frame it was drawn on, to tell how fast Sonic's fall speeds up.
static uint32_t g_radarContext;
static uint32_t g_radarFrame;
static float g_radarVelocityY;
static bool g_radarAirborne;

static void DrawRadarText(ImDrawList* drawList, ImFont* font, float size, ImVec2 pos, ImU32 colour, const std::string& text)
{
    drawList->AddText(font, size, { pos.x + Scale(1), pos.y + Scale(1) }, IM_COL32(0, 0, 0, 200), text.c_str());
    drawList->AddText(font, size, pos, colour, text.c_str());
}

// A footprint's outline (filled if a fill is given), with the radar's mapping.
template<typename ToRadar>
static void DrawFootprint(ImDrawList* drawList, const ToRadar& toRadar, float scale, const Footprint& footprint, const Vector3& offset,
    ImU32 outline, ImU32 fill = 0)
{
    constexpr int HALF_SEGMENTS = 12;

    ImVec2 a = toRadar(footprint.A + offset);
    ImVec2 b = toRadar(footprint.B + offset);
    float radius = std::max(footprint.Radius * scale, Scale(1.5f));
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length = std::hypot(dx, dy);
    if (length < 0.5f)
    {
        ImVec2 centre = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
        if (fill != 0)
            drawList->AddCircleFilled(centre, radius, fill, 24);

        drawList->AddCircle(centre, radius, outline, 24, Scale(1.2f));
        return;
    }

    // Half a circle around each end, joined by the sides: convex.
    std::array<ImVec2, 2 * (HALF_SEGMENTS + 1)> points;
    float angle = std::atan2(dy, dx);
    for (int i = 0; i <= HALF_SEGMENTS; i++)
    {
        float around = angle - PI / 2 + PI * i / HALF_SEGMENTS;
        points[i] = { b.x + std::cos(around) * radius, b.y + std::sin(around) * radius };
        points[HALF_SEGMENTS + 1 + i] = { a.x + std::cos(around + PI) * radius, a.y + std::sin(around + PI) * radius };
    }

    if (fill != 0)
        drawList->AddConvexPolyFilled(points.data(), int(points.size()), fill);

    drawList->AddPolyline(points.data(), int(points.size()), outline, ImDrawFlags_Closed, Scale(1.2f));
}

void CollisionOverlay::DrawRadar(float x, float y, float size, uint32_t context, bool grounded, bool diving, uint32_t frame, float deltaTime)
{
    int radarSize = g_radarSize;
    if (!g_radarShown || radarSize >= int(std::size(RADAR_HALF_WIDTHS)))
        return;

    uint32_t node = LoadU32(context + CONTEXT_MATRIX_NODE_OFFSET);
    if (!IsReadable(node))
        return;

    uint32_t matrix = node + MATRIX_NODE_MATRIX_OFFSET;
    Vector3 front = LoadVector(matrix + 32);
    Vector3 position = LoadVector(matrix + 48);
    Vector3 velocity = LoadVector(context + SONIC_VELOCITY_OFFSET);
    if (!std::isfinite(Length(position)) || !std::isfinite(Length(velocity)))
        return;

    // How much Sonic's vertical speed changed during the last update: gravity, or 0 at the fall's top speed (like
    // diving's DivingDiveMaxVelocityDown). Never upwards: what pushed him up (a spring, say) isn't expected to go on.
    float fallStep = NAN;
    if (!grounded && g_radarAirborne && g_radarContext == context && g_radarFrame + 1 == frame)
        fallStep = std::min(velocity.y - g_radarVelocityY, 0.0f);

    g_radarContext = context;
    g_radarFrame = frame;
    g_radarVelocityY = velocity.y;
    g_radarAirborne = !grounded;

    // Turned like the camera: up on the radar is where the camera looks, along the ground (the 3D stick goes by the
    // camera too). The camera's right is the view matrix's first column. Without the camera, Sonic's facing is up.
    const Vector3 up{ 0.0f, 1.0f, 0.0f };
    Vector3 right{};
    if (g_camera != 0 && LoadU32(g_camera) == g_cameraVftable)
        right = { LoadFloat(g_camera + CAMERA_VIEW_OFFSET), 0.0f, LoadFloat(g_camera + CAMERA_VIEW_OFFSET + 32) };

    if (!(Length(right) > 0.1f))
    {
        Vector3 facing{ front.x, 0.0f, front.z };
        right = Length(facing) > 0.1f ? Cross(facing, up) : Vector3{ 1.0f, 0.0f, 0.0f };
    }

    right = Normalised(right);
    Vector3 forward = Cross(up, right);

    float halfWidth = RADAR_HALF_WIDTHS[radarSize];
    ImVec2 min = { x, y };
    ImVec2 max = { x + size, y + size };
    ImVec2 centre = { x + size / 2, y + size / 2 };
    float scale = size / 2 / halfWidth;
    auto toRadar = [&](const Vector3& p)
    {
        Vector3 d = p - position;
        return ImVec2{ centre.x + Dot(d, right) * scale, centre.y - Dot(d, forward) * scale };
    };

    auto isInside = [&](const Vector3& p, float margin)
    {
        Vector3 d = p - position;
        return std::abs(Dot(d, right)) < halfWidth + margin && std::abs(Dot(d, forward)) < halfWidth + margin;
    };

    // Sonic's collision as it is now: his body, and what collects rings (all but the body and the path collision).
    std::vector<Footprint> body;
    std::vector<Footprint> items;
    for (auto& [id, rigidBody] : GetPlayerBodies(context))
    {
        if (id != SONIC_PATH_COLLISION_ID)
            GetFootprints(Transform(rigidBody), LoadU32(rigidBody + RIGID_BODY_SHAPE_OFFSET), id == BODY_COLLISION_ID ? body : items);
    }

    const std::vector<Footprint>& collectors = items.empty() ? body : items;

    // The heights are compared at the middle of the widest collector.
    Vector3 reference = position + up * 0.5f;
    float widest = 0.0f;
    for (auto& footprint : collectors)
    {
        if (footprint.Radius > widest)
        {
            widest = footprint.Radius;
            reference = footprint.Centre();
        }
    }

    // The rings, enemies and goal ring around him.
    struct RadarObject
    {
        Vector3 Centre;
        float Radius;
        ObjectKind Kind;
    };

    std::vector<RadarObject> objects;
    {
        std::lock_guard lock(g_unitsMutex);
        for (auto& [unit, tracked] : g_units)
        {
            uint32_t unitVftable = LoadU32(unit);
            if (tracked.Kind == ObjectKind::None || tracked.Kind == ObjectKind::Terrain ||
                (unitVftable != EVENT_COLLISION_VFTABLE && unitVftable != RIGID_BODY_UNIT_VFTABLE))
                continue;

            Transform transform;
            uint32_t shape;
            if (!GetCollidable(LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET), transform, shape) || !isInside(transform.translation, 5.0f))
                continue;

            std::vector<Footprint> footprints;
            GetFootprints(transform, shape, footprints);
            if (!footprints.empty())
                objects.push_back({ footprints[0].Centre(), footprints[0].Radius, tracked.Kind });
            else
                objects.push_back({ transform.translation, 0.3f, tracked.Kind });
        }
    }

    // Where he'll be when he gets down to each ring's height on the radar: his velocity along the ground kept, his fall
    // sped up by the same amount every frame. Only an estimate: the stick and the game's own steering (diving's) change
    // it, and nothing in between (the ground, walls) is checked.
    struct Crossing
    {
        size_t Object;
        int Frames;
        Vector3 Offset;
        float Gap;
    };

    std::vector<Crossing> crossings;
    std::vector<Vector3> path;
    bool predicted = !grounded && std::isfinite(fallStep) && deltaTime > 0.0f;
    if (predicted)
    {
        float lowest = INFINITY;
        for (auto& object : objects)
        {
            if (object.Kind == ObjectKind::Ring && object.Centre.y < reference.y && isInside(object.Centre, 0.0f))
                lowest = std::min(lowest, object.Centre.y - reference.y);
        }

        std::vector<bool> crossed(objects.size());
        Vector3 offset{};
        float velocityY = velocity.y;
        path.push_back(offset);
        int frames = std::isfinite(lowest) ? 600 : 60;
        for (int f = 1; f <= frames; f++)
        {
            velocityY += fallStep;
            Vector3 next = offset + Vector3{ velocity.x, velocityY, velocity.z } * deltaTime;
            for (size_t i = 0; i < objects.size(); i++)
            {
                const RadarObject& object = objects[i];
                float height = object.Centre.y - reference.y;
                if (object.Kind != ObjectKind::Ring || crossed[i] || !isInside(object.Centre, 0.0f) || !(offset.y > height && next.y <= height))
                    continue;

                crossed[i] = true;
                Vector3 at = offset + (next - offset) * ((offset.y - height) / (offset.y - next.y));
                float gap = INFINITY;
                for (auto& footprint : collectors)
                    gap = std::min(gap, HorizontalDistance(object.Centre, footprint.A + at, footprint.B + at) - footprint.Radius - object.Radius);

                crossings.push_back({ i, f, at, gap });
            }

            offset = next;
            path.push_back(offset);
            if (offset.y < lowest - 1.0f || !isInside(position + offset, halfWidth))
                break;
        }
    }

    auto drawList = ImGui::GetBackgroundDrawList();
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");

    drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 140), Scale(6));
    drawList->PushClipRect(min, max, true);
    drawList->AddLine({ centre.x, min.y }, { centre.x, max.y }, IM_COL32(255, 255, 255, 40), Scale(1));
    drawList->AddLine({ min.x, centre.y }, { max.x, centre.y }, IM_COL32(255, 255, 255, 40), Scale(1));

    // The stage around his height: the floors' edges from 8 m below his feet to 1.5 m above (fainter the further down),
    // then the walls reaching between 1.5 m below his feet and 4 m above.
    UpdateTerrain();
    auto isNear = [&](const TerrainSegment& segment)
    {
        Vector3 a = segment.A - position;
        Vector3 b = segment.B - position;
        float ar = Dot(a, right), br = Dot(b, right), af = Dot(a, forward), bf = Dot(b, forward);
        return !((ar > halfWidth && br > halfWidth) || (ar < -halfWidth && br < -halfWidth) ||
            (af > halfWidth && bf > halfWidth) || (af < -halfWidth && bf < -halfWidth));
    };

    float feet = position.y;
    for (auto& ledge : g_terrainLedges)
    {
        if (ledge.High < feet - 8.0f || ledge.Low > feet + 1.5f || !isNear(ledge))
            continue;

        float below = std::clamp((feet - ledge.High) / 8.0f, 0.0f, 1.0f);
        drawList->AddLine(toRadar(ledge.A), toRadar(ledge.B), IM_COL32(110, 170, 255, int(200 - 130 * below)), Scale(1));
    }

    for (auto& [address, body] : g_terrainBodies)
    {
        for (auto& wall : body.Walls)
        {
            if (wall.High < feet - 1.5f || wall.Low > feet + 4.0f || !isNear(wall))
                continue;

            drawList->AddLine(toRadar(wall.A), toRadar(wall.B), IM_COL32(235, 235, 235, 210), Scale(1.5f));
        }
    }

    // The enemies and the goal ring, then the rings: green where the estimate has his collectors touch them, with the
    // height of their middle over his feet. Rings above each other (a vertical line of them) share one label: the
    // nearest one below him (or above, if none is), and how many there are.
    std::vector<bool> touched(objects.size());
    for (auto& crossing : crossings)
        touched[crossing.Object] = crossing.Gap <= 0.0f;

    for (auto& object : objects)
    {
        ImVec2 point = toRadar(object.Centre);
        float radius = std::max(object.Radius * scale, Scale(2.5f));
        if (object.Kind == ObjectKind::Enemy)
            drawList->AddRectFilled({ point.x - radius, point.y - radius }, { point.x + radius, point.y + radius }, ENEMY_COLOUR);
        else if (object.Kind == ObjectKind::GoalRing)
            drawList->AddCircle(point, radius, IM_COL32_WHITE, 24, Scale(2));
    }

    std::vector<bool> labelled(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
    {
        const RadarObject& object = objects[i];
        if (object.Kind != ObjectKind::Ring)
            continue;

        ImVec2 point = toRadar(object.Centre);
        float radius = std::max(object.Radius * scale, Scale(2.5f));
        ImU32 colour = touched[i] ? RADAR_TOUCH_COLOUR : RADAR_RING_COLOUR;
        drawList->AddCircleFilled(point, radius, (colour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 70), 24);
        drawList->AddCircle(point, radius, colour, 24, Scale(1.5f));

        if (labelled[i])
            continue;

        int count = 0;
        float shown = NAN;
        for (size_t k = i; k < objects.size(); k++)
        {
            const RadarObject& other = objects[k];
            if (other.Kind != ObjectKind::Ring || labelled[k] || HorizontalDistance(other.Centre, object.Centre, object.Centre) > 0.25f)
                continue;

            labelled[k] = true;
            count++;
            float height = other.Centre.y - position.y;
            bool below = height < 0.0f;
            if (std::isnan(shown) || (below && (shown >= 0.0f || height > shown)) || (!below && shown >= 0.0f && height < shown))
                shown = height;
        }

        std::string label = count > 1 ? fmt::format("{:+.1f} x{}", shown, count) : fmt::format("{:+.1f}", shown);
        DrawRadarText(drawList, font, Scale(10), { point.x + radius + Scale(2), point.y - Scale(5) }, colour, label);
    }

    // The estimated path, and where he reaches the first ring's height, with his collectors drawn there.
    const Crossing* next = crossings.empty() ? nullptr : &crossings[0];
    if (path.size() >= 2)
    {
        std::vector<ImVec2> points;
        for (auto& offset : path)
            points.push_back(toRadar(position + offset));

        drawList->AddPolyline(points.data(), int(points.size()), IM_COL32(255, 255, 255, 150), 0, Scale(1.5f));
    }

    if (next != nullptr)
    {
        ImU32 colour = next->Gap <= 0.0f ? RADAR_TOUCH_COLOUR : IM_COL32(255, 160, 40, 255);
        for (auto& footprint : collectors)
            DrawFootprint(drawList, toRadar, scale, footprint, next->Offset, (colour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 140));

        ImVec2 point = toRadar(position + next->Offset);
        float arm = Scale(5);
        drawList->AddLine({ point.x - arm, point.y - arm }, { point.x + arm, point.y + arm }, colour, Scale(2));
        drawList->AddLine({ point.x - arm, point.y + arm }, { point.x + arm, point.y - arm }, colour, Scale(2));
    }

    // Sonic: what collects rings, his body, and his facing.
    for (auto& footprint : items)
        DrawFootprint(drawList, toRadar, scale, footprint, {}, RADAR_ITEM_COLOUR);

    for (auto& footprint : body)
        DrawFootprint(drawList, toRadar, scale, footprint, {}, BODY_COLOUR, (BODY_COLOUR & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 120));

    Vector3 facing{ front.x, 0.0f, front.z };
    if (Length(facing) > 0.2f)
    {
        ImVec2 tip = toRadar(position + Normalised(facing) * (Scale(18) / scale));
        drawList->AddLine(centre, tip, ARROW_COLOUR, Scale(2));
        drawList->AddCircleFilled(tip, Scale(2.5f), ARROW_COLOUR);
    }

    drawList->PopClipRect();

    // The title, the scale, and what the estimate says about the next ring.
    float padding = Scale(6);
    float textSize = Scale(10);
    DrawRadarText(drawList, font, textSize, { min.x + padding, min.y + padding }, IM_COL32(220, 220, 220, 255), "TOP VIEW (M, Home)");

    float bar = 0.5f;
    for (float length : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f })
    {
        if (length <= halfWidth / 2)
            bar = length;
    }

    std::string barText = fmt::format("{:g} m", bar);
    float barRight = max.x - padding;
    float barY = min.y + padding + Scale(12);
    drawList->AddLine({ barRight - bar * scale, barY }, { barRight, barY }, IM_COL32_WHITE, Scale(2));
    drawList->AddLine({ barRight - bar * scale, barY - Scale(3) }, { barRight - bar * scale, barY + Scale(3) }, IM_COL32_WHITE, Scale(1.5f));
    drawList->AddLine({ barRight, barY - Scale(3) }, { barRight, barY + Scale(3) }, IM_COL32_WHITE, Scale(1.5f));
    DrawRadarText(drawList, font, textSize, { barRight - font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, barText.c_str()).x, min.y + padding },
        IM_COL32(220, 220, 220, 255), barText);

    if (predicted)
    {
        std::string text = "No ring below on the radar";
        ImU32 colour = IM_COL32(200, 200, 200, 255);
        if (next != nullptr)
        {
            text = next->Gap <= 0.0f ? fmt::format("Ring height in {}f: TOUCH", next->Frames) :
                fmt::format("Ring height in {}f: {:.2f} m off", next->Frames, next->Gap);
            colour = next->Gap <= 0.0f ? RADAR_TOUCH_COLOUR : IM_COL32(255, 160, 40, 255);
        }

        DrawRadarText(drawList, font, textSize, { min.x + padding, max.y - padding - Scale(24) }, colour, text);
        DrawRadarText(drawList, font, textSize, { min.x + padding, max.y - padding - Scale(11) }, IM_COL32(170, 170, 170, 255),
            diving ? "Estimate: diving steers with the stick" : "Estimate: no stick, no walls");
    }
}

// Where an object's collision is, by its unit with the smallest footprint (an enemy's body rather than the sphere it
// notices Sonic with).
static bool GetUnitCentre(uint32_t unit, Vector3& centre, float& radius)
{
    uint32_t unitVftable = LoadU32(unit);
    if (unitVftable != EVENT_COLLISION_VFTABLE && unitVftable != RIGID_BODY_UNIT_VFTABLE)
        return false;

    Transform transform;
    uint32_t shape;
    if (!GetCollidable(LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET), transform, shape))
        return false;

    std::vector<Footprint> footprints;
    GetFootprints(transform, shape, footprints);
    centre = footprints.empty() ? transform.translation : footprints[0].Centre();
    radius = footprints.empty() ? 0.3f : footprints[0].Radius;
    return true;
}

// A shape's pieces as the homing estimate tests them against the search's frustum: a capsule or cylinder as its half
// segment and radius, a sphere as its radius, a box as its three half extents and convex radius, anything else as 1 m
// around where it is. Plus 0.05, as Havok's convex shapes collide a little outside themselves.
static void GetHomingVolumes(const Transform& transform, uint32_t shape, std::vector<CollisionOverlay::ObjectCentre::Volume>& volumes)
{
    ForEachLeafShape(transform, shape, [&](const Transform& leafTransform, uint32_t leaf)
    {
        CollisionOverlay::ObjectCentre::Volume volume{};
        auto setCentre = [&](const Vector3& c) { volume.Centre[0] = c.x; volume.Centre[1] = c.y; volume.Centre[2] = c.z; };
        auto addAxis = [&](const Vector3& a)
        {
            volume.Axes[volume.AxisCount][0] = a.x;
            volume.Axes[volume.AxisCount][1] = a.y;
            volume.Axes[volume.AxisCount][2] = a.z;
            volume.AxisCount++;
        };

        uint32_t vftable = LoadU32(leaf);
        float convexRadius = LoadFloat(leaf + 0x10);
        if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
        {
            Vector3 a = leafTransform(LoadVector(leaf + 0x20));
            Vector3 b = leafTransform(LoadVector(leaf + 0x30));
            setCentre((a + b) * 0.5f);
            addAxis((b - a) * 0.5f);
            volume.Radius = convexRadius + (vftable == CYLINDER_SHAPE_VFTABLE ? LoadFloat(leaf + 0x14) : 0.0f);
        }
        else if (vftable == SPHERE_SHAPE_VFTABLE)
        {
            setCentre(leafTransform.translation);
            volume.Radius = convexRadius;
        }
        else if (vftable == BOX_SHAPE_VFTABLE)
        {
            Vector3 half = LoadVector(leaf + 0x20);
            setCentre(leafTransform.translation);
            addAxis(leafTransform.columns[0] * half.x);
            addAxis(leafTransform.columns[1] * half.y);
            addAxis(leafTransform.columns[2] * half.z);
            volume.Radius = convexRadius;
        }
        else
        {
            setCentre(leafTransform.translation);
            volume.Radius = 1.0f;
        }

        if (!(std::isfinite(volume.Radius) && volume.Radius >= 0.0f && volume.Radius < 100.0f))
            volume.Radius = 1.0f;

        volume.Radius += 0.05f;
        volumes.push_back(volume);
    });
}

void CollisionOverlay::GetObjectCentres(float x, float y, float z, float range, std::vector<ObjectCentre>& centres)
{
    centres.clear();
    Vector3 from{ x, y, z };
    std::unordered_map<uint32_t, size_t> indices;

    std::lock_guard lock(g_unitsMutex);
    for (auto& [unit, tracked] : g_units)
    {
        Vector3 centre;
        float radius;
        if (tracked.Kind == ObjectKind::Ring || tracked.Kind == ObjectKind::Terrain || !GetUnitCentre(unit, centre, radius) ||
            Length(centre - from) > range)
            continue;

        auto [it, added] = indices.emplace(tracked.Object, centres.size());
        if (added)
        {
            ObjectCentre object{};
            object.Object = tracked.Object;
            object.Vftable = LoadU32(tracked.Object);
            object.Radius = INFINITY;
            centres.push_back(std::move(object));
        }

        auto& object = centres[it->second];
        if (radius < object.Radius)
        {
            object.X = centre.x;
            object.Y = centre.y;
            object.Z = centre.z;
            object.Radius = radius;
        }

        // Its collisions the search meets.
        uint32_t body = LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET);
        Transform transform;
        uint32_t shape;
        if (!IsHomingTargetCollision(body) || !GetCollidable(body, transform, shape))
            continue;

        object.HomingCandidate = true;
        GetHomingVolumes(transform, shape, object.HomingVolumes);
    }
}
