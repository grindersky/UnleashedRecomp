#pragma once

// Havok collision as the game keeps it: rigid bodies and phantoms, their shapes, the game's collision categories, and
// the player's collisions.

#include "game_addresses.h"
#include "vector_math.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace Tas
{
    // A rigid body's transform.
    inline Transform GetBodyTransform(uint32_t body)
    {
        return Transform::At(body + RIGID_BODY_TRANSFORM_OFFSET);
    }

    // A rigid body or shape phantom in the world, where it is now; false if it isn't one, or isn't in the world.
    bool GetCollidable(uint32_t body, Transform& transform, uint32_t& shape);

    // Calls visit(transform, shape) for each of the shapes a shape is made of, with where it is.
    template<typename Visit>
    void ForEachLeafShape(const Transform& transform, uint32_t shape, const Visit& visit, int depth = 0)
    {
        if (depth > 4 || !IsReadable(shape))
            return;

        uint32_t vftable = LoadU32(shape);
        if (vftable == CONVEX_TRANSLATE_SHAPE_VFTABLE)
        {
            Transform translation;
            translation.translation = LoadVector(shape + CONVEX_WRAPPER_TRANSFORM_OFFSET);
            ForEachLeafShape(transform * translation, LoadU32(shape + CONVEX_WRAPPER_CHILD_OFFSET), visit, depth + 1);
            return;
        }

        if (vftable == CONVEX_TRANSFORM_SHAPE_VFTABLE)
        {
            ForEachLeafShape(transform * Transform::At(shape + CONVEX_WRAPPER_TRANSFORM_OFFSET), LoadU32(shape + CONVEX_WRAPPER_CHILD_OFFSET),
                visit, depth + 1);
            return;
        }

        if (vftable == TRANSFORM_SHAPE_VFTABLE)
        {
            ForEachLeafShape(transform * Transform::At(shape + TRANSFORM_SHAPE_TRANSFORM_OFFSET), LoadU32(shape + TRANSFORM_SHAPE_CHILD_OFFSET),
                visit, depth + 1);
            return;
        }

        if (vftable == LIST_SHAPE_VFTABLE)
        {
            uint32_t children = LoadU32(shape + LIST_SHAPE_CHILDREN_OFFSET);
            uint32_t count = std::min(LoadU32(shape + LIST_SHAPE_COUNT_OFFSET), 64u);
            for (uint32_t i = 0; i < count; i++)
                ForEachLeafShape(transform, LoadU32(children + i * LIST_SHAPE_CHILD_SIZE), visit, depth + 1);

            return;
        }

        visit(transform, shape);
    }

    // Whether a collidable's own category set has the category whose number is at the address.
    bool HasCollisionCategory(uint32_t body, uint32_t categoryAddress);

    // The player's collision that's on, by ID, with its rigid body.
    std::vector<std::pair<uint32_t, uint32_t>> GetPlayerBodies(uint32_t context);

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

    void GetFootprints(const Transform& transform, uint32_t shape, std::vector<Footprint>& footprints);
}
