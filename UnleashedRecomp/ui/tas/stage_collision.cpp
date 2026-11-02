#include "stage_collision.h"
#include "collision.h"
#include "object_tracking.h"

#include <ui/collision_overlay.h>

#include <algorithm>

using namespace Tas;

// The stage's collision on the radar: its walls (triangles steeper than 60 degrees), and the edges of its floors where
// no other floor goes on (ledges, the sides of pits and platforms). Read once per rigid body of the stage's terrain
// object (a MOPP tree over an extended mesh, like the rails), in world space; the floors' edges are matched by their
// corners (rounded to 1 cm), also between bodies, so where two pieces of floor meet isn't drawn. SWA::CGameObjectTerrain
// (vtable 0x82032C44)'s set-up (sub_82518620) adds its rigid bodies through sub_825146A8; bodies with property 8196 of
// types 2, 12, 13 and 14 go to sub_82B16998 / sub_82510828 instead (special surfaces, not read).
static constexpr float TERRAIN_FLOOR_NORMAL_Y = 0.5f;
static constexpr uint32_t TERRAIN_MAX_TRIANGLES = 4 * 1024 * 1024;

static EdgeKey GetEdgeKey(const Vector3& a, const Vector3& b)
{
    auto round = [](float value) { return int32_t(std::clamp(std::lround(value * 100.0f), -2000000000l, 2000000000l)); };
    std::array<int32_t, 3> first{ round(a.x), round(a.y), round(a.z) };
    std::array<int32_t, 3> second{ round(b.x), round(b.y), round(b.z) };
    if (second < first)
        std::swap(first, second);

    return { { first[0], first[1], first[2], second[0], second[1], second[2] } };
}

// The triangles that block the homing attack's line of sight, by 8 m square of the ground (each in every square its
// bounds touch, up to 64 of them). The search's raycast (sub_822EC588 -> hkpWorld::castRay, from Sonic to the target,
// both 0.5 higher; any hit drops the target) meets the collisions whose own category set has the one at
// HOMING_SIGHT_CATEGORY.
static constexpr float SIGHT_GRID_CELL = 8.0f;
static std::vector<std::array<Vector3, 3>> g_sightTriangles;
static std::unordered_map<uint64_t, std::vector<uint32_t>> g_sightGrid;

static uint64_t GetSightCell(int32_t x, int32_t z)
{
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
}

static std::unordered_map<uint32_t, TerrainBody> g_terrainBodies;
static std::vector<TerrainSegment> g_terrainLedges;

const std::unordered_map<uint32_t, TerrainBody>& Tas::GetTerrainBodies()
{
    return g_terrainBodies;
}

const std::vector<TerrainSegment>& Tas::GetTerrainLedges()
{
    return g_terrainLedges;
}

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

        uint32_t triangles = LoadU32(subpart + SUBPART_TRIANGLE_COUNT_OFFSET);
        uint32_t vertices = LoadU32(subpart + SUBPART_VERTICES_OFFSET);
        uint32_t vertexStride = LoadU32(subpart + SUBPART_VERTEX_STRIDE_OFFSET);
        uint32_t vertexCount = LoadU32(subpart + SUBPART_VERTEX_COUNT_OFFSET);
        uint32_t indices = LoadU32(subpart + SUBPART_INDICES_OFFSET);
        uint32_t indexStride = LoadU32(subpart + SUBPART_INDEX_STRIDE_OFFSET);
        uint8_t indexType = LoadU8(subpart + SUBPART_INDEX_TYPE_OFFSET);
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

void Tas::UpdateTerrain()
{
    std::vector<uint32_t> bodies;
    ForEachUnit([&](uint32_t unit, const TrackedUnit& tracked)
    {
        if (tracked.Kind == ObjectKind::Terrain && LoadU32(unit) == RIGID_BODY_UNIT_VFTABLE)
            bodies.push_back(LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET));
    });

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
