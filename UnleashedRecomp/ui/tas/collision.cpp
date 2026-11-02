#include "collision.h"

#include <algorithm>

using namespace Tas;

bool Tas::GetCollidable(uint32_t body, Transform& transform, uint32_t& shape)
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

bool Tas::HasCollisionCategory(uint32_t body, uint32_t categoryAddress)
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

std::vector<std::pair<uint32_t, uint32_t>> Tas::GetPlayerBodies(uint32_t context)
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

        uint32_t body = LoadU32(collision + PHYSICS_UNIT_BODY_OFFSET);
        if (LoadU32(body) == RIGID_BODY_VFTABLE && LoadU32(body + RIGID_BODY_WORLD_OFFSET) != 0)
            bodies.emplace_back(LoadU32(node + 12), body);
    }

    return bodies;
}

void Tas::GetFootprints(const Transform& transform, uint32_t shape, std::vector<Footprint>& footprints)
{
    ForEachLeafShape(transform, shape, [&](const Transform& leafTransform, uint32_t leaf)
    {
        uint32_t vftable = LoadU32(leaf);
        float convexRadius = LoadFloat(leaf + CONVEX_RADIUS_OFFSET);
        if (vftable == CYLINDER_SHAPE_VFTABLE || vftable == CAPSULE_SHAPE_VFTABLE)
        {
            float radius = convexRadius + (vftable == CYLINDER_SHAPE_VFTABLE ? LoadFloat(leaf + CYLINDER_RADIUS_OFFSET) : 0.0f);
            if (radius > 0.0f && radius < 100.0f)
                footprints.push_back({ leafTransform(LoadVector(leaf + SEGMENT_START_OFFSET)), leafTransform(LoadVector(leaf + SEGMENT_END_OFFSET)), radius });
        }
        else if (vftable == SPHERE_SHAPE_VFTABLE)
        {
            if (convexRadius > 0.0f && convexRadius < 100.0f)
                footprints.push_back({ leafTransform.translation, leafTransform.translation, convexRadius });
        }
        else if (vftable == BOX_SHAPE_VFTABLE)
        {
            Vector3 half = LoadVector(leaf + BOX_HALF_EXTENTS_OFFSET);
            float radius = std::max(half.x, half.z) + convexRadius;
            if (radius > 0.0f && radius < 100.0f)
                footprints.push_back({ leafTransform.translation, leafTransform.translation, radius });
        }
    });
}
