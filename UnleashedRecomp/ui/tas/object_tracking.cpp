#include "object_tracking.h"
#include "collision.h"
#include "guest_memory.h"

#include <ui/collision_overlay.h>
#include <ui/tas_hud.h>

#include <algorithm>
#include <string_view>
#include <utility>

using namespace Tas;

bool Tas::IsTracking()
{
    return CollisionOverlay::IsEnabled() || TasHud::IsEnabled();
}

// --- The camera ---

static uint32_t g_camera;
static uint32_t g_cameraVftable;

void CollisionOverlay::OnCameraUpdate(uint32_t camera)
{
    if (IsTracking())
    {
        g_camera = camera;
        g_cameraVftable = LoadU32(camera);
    }
}

uint32_t Tas::GetCamera()
{
    return g_camera != 0 && LoadU32(g_camera) == g_cameraVftable ? g_camera : 0;
}

// --- The grind rails ---

// The game adds and removes them on its own thread.
static std::mutex g_grindPathsMutex;
static std::vector<uint32_t> g_grindPaths;

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

std::vector<uint32_t> Tas::GetGrindPaths()
{
    std::lock_guard lock(g_grindPathsMutex);
    return g_grindPaths;
}

// --- Objects' physics units ---

static std::mutex g_unitsMutex;
static std::unordered_map<uint32_t, TrackedUnit> g_units;
static std::unordered_map<uint32_t, ObjectKind> g_objectKinds;

// The object adding an event collision.
static thread_local uint32_t g_eventCollisionOwner;

std::mutex& Tas::GetUnitsMutex()
{
    return g_unitsMutex;
}

const std::unordered_map<uint32_t, TrackedUnit>& Tas::GetUnitsLocked()
{
    return g_units;
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

// Another (r3 = the object, r4 its name, r5 the shape): SWA::CObjHintRing's set-up (sub_827382C0) adds its box with it.
PPC_FUNC_IMPL(__imp__sub_82516960);
PPC_FUNC(sub_82516960)
{
    uint32_t owner = std::exchange(g_eventCollisionOwner, ctx.r3.u32);
    __imp__sub_82516960(ctx, base);
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

// --- Where objects are, for the homing attack's estimate ---

// The homing attack's target search (sub_8230E990) meets a collision whose own category set has the one at
// HOMING_TARGET_CATEGORY (58 at run time): its phantom (sub_82379718) has no categories of its own and collides with
// that one. Answering the priority message isn't it: the E-Fighter's missiles answer nothing and are targets. In
// DragonRoadAct1.ltm, filter infos 0x30 and 0x2F had it (enemies, E-Fighter missiles, springs, rockets, up reels);
// Sonic's 0x14, camera volumes' 8, point markers' 7, physics objects' 0x3E5 and CEnemyObjEFMissile's 0x3E9 didn't.
static bool IsHomingTargetCollision(uint32_t body)
{
    return HasCollisionCategory(body, HOMING_TARGET_CATEGORY);
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
        float convexRadius = LoadFloat(leaf + CONVEX_RADIUS_OFFSET);
        if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
        {
            Vector3 a = leafTransform(LoadVector(leaf + SEGMENT_START_OFFSET));
            Vector3 b = leafTransform(LoadVector(leaf + SEGMENT_END_OFFSET));
            setCentre((a + b) * 0.5f);
            addAxis((b - a) * 0.5f);
            volume.Radius = convexRadius + (vftable == CYLINDER_SHAPE_VFTABLE ? LoadFloat(leaf + CYLINDER_RADIUS_OFFSET) : 0.0f);
        }
        else if (vftable == SPHERE_SHAPE_VFTABLE)
        {
            setCentre(leafTransform.translation);
            volume.Radius = convexRadius;
        }
        else if (vftable == BOX_SHAPE_VFTABLE)
        {
            Vector3 half = LoadVector(leaf + BOX_HALF_EXTENTS_OFFSET);
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

    ForEachUnit([&](uint32_t unit, const TrackedUnit& tracked)
    {
        Vector3 centre;
        float radius;
        if (tracked.Kind == ObjectKind::Ring || tracked.Kind == ObjectKind::Terrain || !GetUnitCentre(unit, centre, radius) ||
            Length(centre - from) > range)
            return;

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
            return;

        object.HomingCandidate = true;
        GetHomingVolumes(transform, shape, object.HomingVolumes);
    });
}
