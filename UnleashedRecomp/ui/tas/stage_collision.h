#pragma once

// The stage's own collision, read from SWA::CGameObjectTerrain's rigid bodies: its walls and floor edges for the radar,
// and its triangles for the homing attack estimate's line of sight (CollisionOverlay::IsSightBlocked).

#include "vector_math.h"

#include <array>
#include <compare>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Tas
{
    // A wall seen from above, or a floor's edge: a line, and how high it reaches.
    struct TerrainSegment
    {
        Vector3 A;
        Vector3 B;
        float Low;
        float High;
    };

    // A floor edge's corners, rounded to 1 cm, in order.
    struct EdgeKey
    {
        std::array<int32_t, 6> Corners;

        auto operator<=>(const EdgeKey&) const = default;
    };

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

    // Brings the stage's collision up to date with the terrain bodies in the world. Reads each body once (expect a hitch
    // the first time a big one is seen).
    void UpdateTerrain();

    // By rigid body.
    const std::unordered_map<uint32_t, TerrainBody>& GetTerrainBodies();

    // The floors' edges no other floor goes on from: ledges, the sides of pits and platforms.
    const std::vector<TerrainSegment>& GetTerrainLedges();
}
