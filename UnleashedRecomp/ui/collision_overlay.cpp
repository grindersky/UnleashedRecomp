#include "collision_overlay.h"
#include <api/SWA.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <ui/imgui_utils.h>
#include <ui/tas_hud.h>
#include <user/config.h>
#include <sdl_listener.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <unordered_map>
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

static const ImU32 RAIL_COLOUR = IM_COL32(255, 140, 0, 130);
static const ImU32 BODY_COLOUR = IM_COL32(0, 230, 255, 140);
static const ImU32 TRIGGER_COLOUR = IM_COL32(255, 80, 255, 100);
static const ImU32 PATH_COLOUR = IM_COL32(60, 90, 255, 110);

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

// A rigid body's transform.
struct Transform
{
    Vector3 columns[3];
    Vector3 translation;

    explicit Transform(uint32_t body)
    {
        for (int i = 0; i < 3; i++)
            columns[i] = LoadVector(body + RIGID_BODY_TRANSFORM_OFFSET + i * 16);

        translation = LoadVector(body + RIGID_BODY_TRANSFORM_OFFSET + 48);
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

// The overlay is hidden behind what the game drew, with the game's depth buffer; F12 switches to drawing it over
// everything and back. Under libTAS, F1 to F10 are savestate hotkeys, which the game doesn't get; F12 is passed on as a
// key, so it goes into the movie like the game's keys.
static std::atomic<bool> g_depthTested{ true };

static class CollisionOverlayListener : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override
    {
        if (CollisionOverlay::IsEnabled() && event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_F12 && !event->key.repeat)
            g_depthTested = !g_depthTested;

        return false;
    }
}
g_collisionOverlayListener;

void CollisionOverlay::OnCameraUpdate(uint32_t camera)
{
    if (IsEnabled())
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

static void DrawShape(ImDrawList* drawList, const Projector& projector, const Transform& transform, uint32_t shape, ImU32 colour)
{
    Surface surface;
    if (BuildShape(surface, shape))
        DrawSurface(drawList, projector, transform, surface, colour);
}

// The player's collision that's on: the body, day Sonic's path collision, and what touches rings, objects and triggers.
static void DrawPlayerCollision(ImDrawList* drawList, const Projector& projector, uint32_t context, bool werehog)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    uint32_t container = player != 0 ? LoadU32(player + PLAYER_COLLISIONS_OFFSET) : 0;
    uint32_t head = container != 0 ? LoadU32(container + COLLISIONS_HEAD_OFFSET) : 0;
    if (head == 0)
        return;

    // The tree's nodes: left at +0, right at +8, the ID at +12 and the shared_ptr at +16, the head's flag at +25.
    std::vector<uint32_t> nodes{ LoadU32(head + 4) };
    std::vector<std::pair<uint32_t, uint32_t>> bodies;
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

    // The body first: the others are around it, and it shows through them.
    std::stable_partition(bodies.begin(), bodies.end(), [](auto& entry) { return entry.first == BODY_COLLISION_ID; });
    for (auto& [id, body] : bodies)
    {
        ImU32 colour = id == BODY_COLLISION_ID ? BODY_COLOUR : !werehog && id == SONIC_PATH_COLLISION_ID ? PATH_COLOUR : TRIGGER_COLOUR;
        DrawShape(drawList, projector, Transform(body), LoadU32(body + RIGID_BODY_SHAPE_OFFSET), colour);
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
    if (!IsEnabled())
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

    // Behind walls: the game's depth buffer as the texture. Without it (with multisampling, say), over everything.
    GuestSurface* depthStencil = g_depthTested ? Video::GetSceneDepthStencil() : nullptr;
    projector.DepthTested = depthStencil != nullptr;
    if (projector.DepthTested)
    {
        drawList->PushTextureID(ImTextureID(depthStencil));
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

    DrawPlayerCollision(drawList, projector, context, werehog);

    if (projector.DepthTested)
    {
        SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);
        drawList->PopTextureID();
    }

    drawList->Flags = flags;
}
