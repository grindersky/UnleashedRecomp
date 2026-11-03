#include "collision_overlay.h"
#include <api/SWA.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <patches/collision_view_patches.h>
#include <ui/imgui_utils.h>
#include <ui/tas/collision.h>
#include <ui/tas/drawing.h>
#include <ui/tas/object_tracking.h>
#include <ui/tas/tas_camera.h>
#include <ui/tas_hud.h>
#include <user/config.h>
#include <sdl_listener.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The 3D part of the collision overlay: what the collision views leave out, drawn over the frame with the game's camera,
// and the labels over the hint rings. The radar is in ui/tas/radar.cpp; what's followed in the game (camera, objects,
// rails) in ui/tas/object_tracking.cpp; the stage's collision in ui/tas/stage_collision.cpp.

using namespace Tas;

// The arrow along his facing: a flat arrow given some thickness, a little above his feet.
static constexpr float ARROW_HEIGHT = 0.05f;
static constexpr float ARROW_THICKNESS = 0.05f;
static constexpr float ARROW_LENGTH = 1.0f;
static constexpr float ARROW_HEAD_LENGTH = 0.3f;
static constexpr float ARROW_HALF_WIDTH = 0.04f;
static constexpr float ARROW_HEAD_HALF_WIDTH = 0.15f;

// Sonic's attacks hit what his collision touches. His body (Normal) is a TypePlayer collision that detects TypeEvent
// ones, the others (Item and the rest, all but Path) TypePlayerItem ones that detect TypeEventItem, TypeEvent and
// TypeObjectPhysics ones (the filters made in sub_824950C0, and in the set-up). When one touches something
// (MsgHitEventCollision, sub_82360248) while he has an attack type (sliding sets TypeSonicSliding, sub_8232F950), he
// damages it: no shape of its own. Setting an attack type also sets ATTACKING_FLAG, and removing it clears it: his
// collision around his body is then drawn as attacking.
static const ImU32 RAIL_COLOUR = IM_COL32(255, 140, 0, 130);
static const ImU32 TRIGGER_COLOUR = IM_COL32(255, 80, 255, 85);
static const ImU32 PATH_COLOUR = IM_COL32(60, 90, 255, 90);
static const ImU32 ATTACK_COLOUR = IM_COL32(60, 255, 90, 100);
static const ImU32 GOAL_COLOUR = IM_COL32(255, 255, 255, 110);

// How far rails are drawn: they fade out from the first distance to the second.
static constexpr float RAIL_FADE_START = 150.0f;
static constexpr float RAIL_FADE_END = 400.0f;

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
        if (event->type != SDL_KEYDOWN || event->key.repeat)
            return false;

        if (CollisionOverlay::IsEnabled() && event->key.keysym.sym == SDLK_F12)
            g_depthTested = !g_depthTested;

        return false;
    }
}
g_collisionOverlayListener;

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
            return viewPoint.z < -CAMERA_NEAR_PLANE;
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
                    polygon[corners++] = current + (next - current) * ((-CAMERA_NEAR_PLANE - current.z) / (next.z - current.z));
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
    float convexRadius = LoadFloat(shape + CONVEX_RADIUS_OFFSET);

    if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
    {
        bool cylinder = vftable == CYLINDER_SHAPE_VFTABLE;
        float radius = convexRadius + (cylinder ? LoadFloat(shape + CYLINDER_RADIUS_OFFSET) : 0.0f);
        if (!(radius > 0.0f && radius < 100.0f))
            return false;

        BuildTube(surface, LoadVector(shape + SEGMENT_START_OFFSET), LoadVector(shape + SEGMENT_END_OFFSET), radius, !cylinder);
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
        Vector3 half = LoadVector(shape + BOX_HALF_EXTENTS_OFFSET) + Vector3{ convexRadius, convexRadius, convexRadius };
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
    ForEachLeafShape(transform, shape, [&](const Transform& leafTransform, uint32_t leaf)
    {
        Surface surface;
        if (BuildShape(surface, leaf))
            DrawSurface(drawList, projector, leafTransform, surface, colour);
    });
}

static bool IsAttacking(uint32_t context)
{
    uint32_t flags = LoadU32(context + STATE_FLAGS_OFFSET);
    return flags != 0 && LoadU8(LoadU32(flags + 4) + ATTACKING_FLAG) != 0;
}

// A solid arrow from the player's feet along his facing, a little above them: the flat arrow's outline, given thickness.
static void DrawFacingArrow(ImDrawList* drawList, const Projector& projector, uint32_t context)
{
    uint32_t node = LoadU32(context + MATRIX_NODE_OFFSET);
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
        DrawShape(drawList, projector, GetBodyTransform(body), LoadU32(body + RIGID_BODY_SHAPE_OFFSET), colour);
    }
}

// How far other objects' collision is drawn.
static constexpr float OBJECT_DRAW_DISTANCE = 150.0f;

// The collision of the enemies and of the goal ring that's on.
static void DrawObjectCollision(ImDrawList* drawList, const Projector& projector)
{
    std::vector<std::pair<uint32_t, TrackedUnit>> units;
    ForEachUnit([&](uint32_t unit, const TrackedUnit& tracked) { units.emplace_back(unit, tracked); });

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
    uint32_t triangles = LoadU32(subpart + SUBPART_TRIANGLE_COUNT_OFFSET);
    uint32_t vertices = LoadU32(subpart + SUBPART_VERTICES_OFFSET);
    uint32_t vertexStride = LoadU32(subpart + SUBPART_VERTEX_STRIDE_OFFSET);
    uint32_t vertexCount = LoadU32(subpart + SUBPART_VERTEX_COUNT_OFFSET);
    uint32_t indices = LoadU32(subpart + SUBPART_INDICES_OFFSET);
    uint32_t indexStride = LoadU32(subpart + SUBPART_INDEX_STRIDE_OFFSET);
    uint8_t indexType = LoadU8(subpart + SUBPART_INDEX_TYPE_OFFSET);
    if (triangles == 0 || triangles > 65536 || vertexCount > 65536 || vertexStride < 12 || (indexType != 1 && indexType != 2))
        return;

    if (vertexCount == 0)
        return;

    float radius = LoadFloat(mesh + EXTENDED_MESH_RADIUS_OFFSET);
    if (!(radius > 0.0f && radius < 1.0f))
        radius = 0.05f;

    const RailMesh& rail = GetRailMesh(mesh, vertices, vertexStride, vertexCount, indices, indexStride, indexType == 2, triangles);
    Transform transform = GetBodyTransform(body);
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

// The camera that was updated last, as a projector onto the window; false if there's none.
static bool MakeProjector(Projector& projector)
{
    uint32_t camera = GetCamera();
    if (camera == 0)
        return false;

    for (int i = 0; i < 16; i++)
    {
        projector.View[i] = LoadFloat(camera + CAMERA_VIEW_OFFSET + i * 4);
        projector.Projection[i] = LoadFloat(camera + CAMERA_PROJECTION_OFFSET + i * 4);
    }

    projector.Eye = LoadVector(camera + CAMERA_POSITION_OFFSET);

    // With the TAS camera on, the picture is drawn from its view (same projection).
    float tasEye[3];
    if (TasCamera::GetView(camera, projector.View, tasEye))
        projector.Eye = { tasEye[0], tasEye[1], tasEye[2] };

    // The scene keeps its aspect ratio (the projection's), centred in the window.
    auto& res = ImGui::GetIO().DisplaySize;
    float aspectRatio = projector.Projection[0] != 0.0f ? projector.Projection[5] / projector.Projection[0] : res.x / res.y;
    if (!(aspectRatio > 0.0f) || !std::isfinite(aspectRatio))
        return false;

    projector.Size = res.x / res.y > aspectRatio ? ImVec2(res.y * aspectRatio, res.y) : ImVec2(res.x, res.x / aspectRatio);
    projector.Origin = { (res.x - projector.Size.x) / 2, (res.y - projector.Size.y) / 2 };
    return true;
}

void CollisionOverlay::Draw()
{
    // Hidden with the views (F11).
    if (!IsEnabled() || !CollisionView::IsShown())
        return;

    // Only in levels, with the camera that was updated last.
    bool werehog;
    uint32_t context = TasHud::GetPlayerContext(werehog);
    Projector projector;
    if (context == 0 || !MakeProjector(projector))
        return;

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
        std::vector<uint32_t> paths = GetGrindPaths();

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

// SWA::CObjHintRing (vtable 0x82064034; constructor sub_82739988, set-up sub_827382C0, update sub_82738088, messages
// sub_82739870). What touches it is a box event collision named "HintRing" (half extents 0.5, 0.7, 0.2 plus 0.05, much
// smaller than the ring of orbs), which only the player's body collision meets. MsgHitEventCollision (sub_827391E0) is
// accepted from the player while the ring is untouched (+328), not turned off (+356 set with +360 4) and its timer
// (+348) is at 0 or below. The constructor sets the timer to 2 s, and so does the hint's end (MsgNoticeFinishHelp,
// sub_82738E20); the update takes the frame's time off it while the ring is untouched (-1 once it's done). Rings are made
// as Sonic comes near, so at speed he can reach one before it's ready: in a friend's Windmill Isle Act 2 movie he hit one
// with 1.25 s left, and nothing happened. Havok sends the hit once when the contact begins, so staying inside doesn't
// count later. The touch is handled after the update that frame (its timer was the one that update left).

// How far from the camera the labels are shown.
static constexpr float HINT_RING_LABEL_DISTANCE = 300.0f;

// Each ring's timer on the last drawn frame, to step it by as much as it moved since (slowdowns change the step).
static std::unordered_map<uint32_t, std::pair<uint32_t, float>> g_hintRingTimers;
static uint32_t g_hintRingFrame;

void CollisionOverlay::DrawHintRings(float deltaTime)
{
    g_hintRingFrame++;

    Projector projector;
    if (!MakeProjector(projector))
        return;

    // The rings' touch boxes in the world.
    std::vector<std::pair<uint32_t, uint32_t>> rings;
    ForEachUnit([&](uint32_t unit, const TrackedUnit& tracked)
    {
        if (LoadU32(tracked.Object) == HINT_RING_VFTABLE)
            rings.emplace_back(tracked.Object, LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET));
    });

    auto drawList = ImGui::GetBackgroundDrawList();
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");
    float size = Scale(14);

    for (auto& [ring, body] : rings)
    {
        Transform transform;
        uint32_t shape;
        if (!GetCollidable(body, transform, shape))
            continue;

        bool touched = LoadU8(ring + HINT_RING_TOUCHED_OFFSET) != 0;
        float timer = LoadFloat(ring + HINT_RING_TIMER_OFFSET);
        bool off = LoadU8(ring + HINT_RING_OFF_OFFSET) != 0 && LoadU32(ring + HINT_RING_MODE_OFFSET) == HINT_RING_OFF_MODE;

        // The step: how much the timer dropped since the last drawn frame, or the frame's time.
        float step = deltaTime;
        auto previous = g_hintRingTimers.find(ring);
        if (previous != g_hintRingTimers.end() && previous->second.first + 1 == g_hintRingFrame && previous->second.second > timer &&
            previous->second.second - timer < 1.0f)
        {
            step = previous->second.second - timer;
        }

        g_hintRingTimers[ring] = { g_hintRingFrame, timer };

        // Above the box's top, along its up.
        Vector3 top = transform(Vector3{ 0.0f, 1.1f, 0.0f });
        if (Length(top - projector.Eye) > HINT_RING_LABEL_DISTANCE)
            continue;

        Vector3 view = projector.ToView(top);
        if (!Projector::IsVisible(view))
            continue;

        // In how many updates the timer gets to 0 or below, taking the step off in single precision like the game: a
        // touch in that update counts.
        std::string label;
        ImU32 colour;
        if (off)
        {
            continue;
        }
        else if (touched)
        {
            label = "HINT touched";
            colour = IM_COL32(170, 170, 170, 230);
        }
        else if (timer <= 0.0f)
        {
            label = "HINT ready";
            colour = IM_COL32(80, 220, 80, 255);
        }
        else
        {
            int frames = 0;
            for (float left = timer; left > 0.0f && frames < 10000; frames++)
                left -= step;

            label = fmt::format("HINT ready in {}f", frames);
            colour = IM_COL32(255, 200, 60, 255);
        }

        float depth;
        ImVec2 point = projector.ToScreen(view, depth);
        float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str()).x;
        DrawShadowedText(drawList, font, size, { point.x - width / 2, point.y - size }, colour, label);
    }

    // Forget the rings that weren't seen for a while.
    std::erase_if(g_hintRingTimers, [](auto& entry) { return entry.second.first + 60 < g_hintRingFrame; });
}
