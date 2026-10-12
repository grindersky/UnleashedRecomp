#include "collision_overlay.h"
#include <api/SWA.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <ui/imgui_utils.h>
#include <ui/tas_hud.h>
#include <user/config.h>

#include <algorithm>
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
// collision, bigger while boosting, and others). The Werehog's ID 1 is a capsule.
static constexpr uint32_t PLAYER_OFFSET = 0x100;
static constexpr uint32_t PLAYER_COLLISIONS_OFFSET = 616;
static constexpr uint32_t COLLISIONS_HEAD_OFFSET = 8;
static constexpr uint32_t EVENT_COLLISION_VFTABLE = 0x8202C7B4;
static constexpr uint32_t EVENT_COLLISION_BODY_OFFSET = 4;
static constexpr uint32_t BODY_COLLISION_ID = 1;

static const ImU32 RAIL_FILL = IM_COL32(255, 140, 0, 60);
static const ImU32 RAIL_OUTLINE = IM_COL32(255, 160, 40, 230);
static const ImU32 BODY_COLOUR = IM_COL32(0, 230, 255, 230);
static const ImU32 TRIGGER_COLOUR = IM_COL32(255, 80, 255, 230);

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
        return columns[0] * p.x + columns[1] * p.y + columns[2] * p.z + translation;
    }
};

static uint32_t g_camera;
static uint32_t g_cameraVftable;

// The grind rails set up and not destroyed yet. The game adds and removes them on its own thread.
static std::mutex g_grindPathsMutex;
static std::vector<uint32_t> g_grindPaths;

// The rails' outlines: the triangle edges that only one triangle has, by mesh.
struct RailOutline
{
    uint32_t Indices;
    uint32_t Triangles;
    std::vector<std::pair<uint32_t, uint32_t>> Edges;
};

static std::unordered_map<uint32_t, RailOutline> g_railOutlines;

bool CollisionOverlay::IsEnabled()
{
    return Config::EnableStageCollisionDebugView || Config::EnableEventCollisionDebugView || Config::EnableObjectCollisionDebugView;
}

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

        ImVec2 ToScreen(const Vector3& v) const
        {
            float x = v.x * Projection[0] + v.y * Projection[4] + v.z * Projection[8] + Projection[12];
            float y = v.x * Projection[1] + v.y * Projection[5] + v.z * Projection[9] + Projection[13];
            float w = v.x * Projection[3] + v.y * Projection[7] + v.z * Projection[11] + Projection[15];
            return { Origin.x + (x / w * 0.5f + 0.5f) * Size.x, Origin.y + (0.5f - y / w * 0.5f) * Size.y };
        }

        // A line, cut where it goes behind the near plane.
        void Line(ImDrawList* drawList, const Vector3& a, const Vector3& b, ImU32 colour, float thickness = 1.5f) const
        {
            Vector3 va = ToView(a);
            Vector3 vb = ToView(b);
            if (!IsVisible(va) && !IsVisible(vb))
                return;

            if (!IsVisible(va))
                va = vb + (va - vb) * ((-NEAR_PLANE - vb.z) / (va.z - vb.z));
            else if (!IsVisible(vb))
                vb = va + (vb - va) * ((-NEAR_PLANE - va.z) / (vb.z - va.z));

            drawList->AddLine(ToScreen(va), ToScreen(vb), colour, Scale(thickness));
        }

        void Triangle(ImDrawList* drawList, const Vector3& a, const Vector3& b, const Vector3& c, ImU32 colour) const
        {
            Vector3 va = ToView(a);
            Vector3 vb = ToView(b);
            Vector3 vc = ToView(c);
            if (IsVisible(va) && IsVisible(vb) && IsVisible(vc))
                drawList->AddTriangleFilled(ToScreen(va), ToScreen(vb), ToScreen(vc), colour);
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

// A circle in the shape's own space, around a centre in the plane of u and v.
static void DrawCircle(ImDrawList* drawList, const Projector& projector, const Transform& transform, const Vector3& centre,
    const Vector3& u, const Vector3& v, float radius, ImU32 colour, float startAngle = 0.0f, float endAngle = 2.0f * PI)
{
    constexpr int SEGMENTS = 24;
    Vector3 previous{};
    for (int i = 0; i <= SEGMENTS; i++)
    {
        float angle = startAngle + (endAngle - startAngle) * i / SEGMENTS;
        Vector3 point = transform(centre + u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius));
        if (i > 0)
            projector.Line(drawList, previous, point, colour);

        previous = point;
    }
}

// A cylinder or a capsule between two points; a capsule also gets its rounded ends.
static void DrawTube(ImDrawList* drawList, const Projector& projector, const Transform& transform, const Vector3& a, const Vector3& b,
    float radius, bool rounded, ImU32 colour)
{
    Vector3 axis = Normalised(b - a);
    Vector3 u, v;
    GetPerpendiculars(axis, u, v);

    DrawCircle(drawList, projector, transform, a, u, v, radius, colour);
    DrawCircle(drawList, projector, transform, b, u, v, radius, colour);

    for (int i = 0; i < 4; i++)
    {
        float angle = PI * i / 2;
        Vector3 offset = u * (std::cos(angle) * radius) + v * (std::sin(angle) * radius);
        projector.Line(drawList, transform(a + offset), transform(b + offset), colour);
    }

    if (rounded)
    {
        // Half circles over each end, in two planes through the axis.
        for (const Vector3& side : { u, v })
        {
            DrawCircle(drawList, projector, transform, b, side, axis, radius, colour, 0.0f, PI);
            DrawCircle(drawList, projector, transform, a, side, axis * -1.0f, radius, colour, 0.0f, PI);
        }
    }
}

static void DrawShape(ImDrawList* drawList, const Projector& projector, const Transform& transform, uint32_t shape, ImU32 colour)
{
    uint32_t vftable = LoadU32(shape);
    float convexRadius = LoadFloat(shape + 0x10);

    if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
    {
        bool cylinder = vftable == CYLINDER_SHAPE_VFTABLE;
        float radius = convexRadius + (cylinder ? LoadFloat(shape + 0x14) : 0.0f);
        if (radius > 0.0f && radius < 100.0f)
            DrawTube(drawList, projector, transform, LoadVector(shape + 0x20), LoadVector(shape + 0x30), radius, !cylinder, colour);
    }
    else if (vftable == SPHERE_SHAPE_VFTABLE)
    {
        if (convexRadius > 0.0f && convexRadius < 100.0f)
        {
            Vector3 x{ 1.0f, 0.0f, 0.0f }, y{ 0.0f, 1.0f, 0.0f }, z{ 0.0f, 0.0f, 1.0f };
            DrawCircle(drawList, projector, transform, {}, x, y, convexRadius, colour);
            DrawCircle(drawList, projector, transform, {}, y, z, convexRadius, colour);
            DrawCircle(drawList, projector, transform, {}, z, x, convexRadius, colour);
        }
    }
    else if (vftable == BOX_SHAPE_VFTABLE)
    {
        Vector3 half = LoadVector(shape + 0x20) + Vector3{ convexRadius, convexRadius, convexRadius };
        if (half.x > 0.0f && half.y > 0.0f && half.z > 0.0f && half.x < 100.0f && half.y < 100.0f && half.z < 100.0f)
        {
            auto corner = [&](int i) { return transform({ (i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z }); };
            for (int i = 0; i < 8; i++)
            {
                for (int bit : { 1, 2, 4 })
                {
                    if (!(i & bit))
                        projector.Line(drawList, corner(i), corner(i | bit), colour);
                }
            }
        }
    }
}

// The player's collision that's on: the body and what touches rings, objects and triggers.
static void DrawPlayerCollision(ImDrawList* drawList, const Projector& projector, uint32_t context)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    uint32_t container = player != 0 ? LoadU32(player + PLAYER_COLLISIONS_OFFSET) : 0;
    uint32_t head = container != 0 ? LoadU32(container + COLLISIONS_HEAD_OFFSET) : 0;
    if (head == 0)
        return;

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
        if (LoadU32(body) != RIGID_BODY_VFTABLE || LoadU32(body + RIGID_BODY_WORLD_OFFSET) == 0)
            continue;

        ImU32 colour = LoadU32(node + 12) == BODY_COLLISION_ID ? BODY_COLOUR : TRIGGER_COLOUR;
        DrawShape(drawList, projector, Transform(body), LoadU32(body + RIGID_BODY_SHAPE_OFFSET), colour);
    }
}

static const RailOutline& GetRailOutline(uint32_t mesh, uint32_t indices, uint32_t indexStride, bool indices32, uint32_t triangles)
{
    auto& outline = g_railOutlines[mesh];
    if (outline.Indices == indices && outline.Triangles == triangles)
        return outline;

    std::map<std::pair<uint32_t, uint32_t>, int> counts;
    for (uint32_t t = 0; t < triangles; t++)
    {
        uint32_t index[3];
        for (uint32_t k = 0; k < 3; k++)
        {
            uint32_t address = indices + t * indexStride + k * (indices32 ? 4 : 2);
            index[k] = indices32 ? LoadU32(address) : LoadU16(address);
        }

        for (uint32_t k = 0; k < 3; k++)
            counts[std::minmax(index[k], index[(k + 1) % 3])]++;
    }

    outline = { indices, triangles, {} };
    for (auto& [edge, count] : counts)
    {
        if (count == 1)
            outline.Edges.push_back(edge);
    }

    return outline;
}

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

    bool indices32 = indexType == 2;
    Transform transform(body);
    auto vertex = [&](uint32_t index) { return transform(LoadVector(vertices + std::min(index, vertexCount - 1) * vertexStride)); };
    auto fade = [&](const Vector3& point) { return 1.0f - (Length(point - projector.Eye) - RAIL_FADE_START) / (RAIL_FADE_END - RAIL_FADE_START); };

    for (uint32_t t = 0; t < triangles; t++)
    {
        uint32_t index[3];
        for (uint32_t k = 0; k < 3; k++)
        {
            uint32_t address = indices + t * indexStride + k * (indices32 ? 4 : 2);
            index[k] = indices32 ? LoadU32(address) : LoadU16(address);
        }

        Vector3 a = vertex(index[0]), b = vertex(index[1]), c = vertex(index[2]);
        float amount = fade((a + b + c) * (1.0f / 3.0f));
        if (amount > 0.0f)
            projector.Triangle(drawList, a, b, c, Fade(RAIL_FILL, amount));
    }

    for (auto& [first, second] : GetRailOutline(mesh, indices, indexStride, indices32, triangles).Edges)
    {
        Vector3 a = vertex(first), b = vertex(second);
        float amount = fade((a + b) * 0.5f);
        if (amount > 0.0f)
            projector.Line(drawList, a, b, Fade(RAIL_OUTLINE, amount), 2.5f);
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

    if (*SWA::SGlobals::ms_IsCollisionRender)
    {
        std::vector<uint32_t> paths;
        {
            std::lock_guard lock(g_grindPathsMutex);
            paths = g_grindPaths;
        }

        for (uint32_t path : paths)
            DrawRail(drawList, projector, path);
    }

    DrawPlayerCollision(drawList, projector, context);
}
