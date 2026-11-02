#pragma once

// What the collision overlay, the radar and the HUD follow in the game: the camera, the grind rails, and the physics
// units objects add, with their object and its kind. Kept while the overlay or the HUD is on.

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Tas
{
    // Whether the camera and the objects are followed: with the collision views (the overlay) or the HUD (its radar).
    bool IsTracking();

    // The camera that was updated last, while it's still alive; or 0.
    uint32_t GetCamera();

    // The grind rails set up and not destroyed yet.
    std::vector<uint32_t> GetGrindPaths();

    // The objects whose collision is drawn: the enemies (any class with "Enemy" in its name among the object's classes,
    // like SWA::CSonicEnemyBase, the CEnemy* classes, their shots and weapons, and SWA::EvilEnemy::IEvilEnemy) and the
    // goal ring (SWA::CObjGoalRing), always: the event and object collision views (ms_IsTriggerRender,
    // ms_IsObjectCollisionRender) were meant to draw them, but don't (user, 2026-10-23: two enemies left to the views,
    // none drawn). The rings (SWA::CObjRing, whose set-up, sub_825EA530, adds a sphere through sub_825169F8 ->
    // sub_82516528, and SWA::CObjSuperRing) are only on the radar, and so is the stage's own collision
    // (SWA::CGameObjectTerrain's rigid bodies, added by its set-up sub_82518620 through sub_825146A8).
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
    // for any target. The game adds them on its own threads: visit(unit, tracked) is called with them locked, so it
    // mustn't call the functions here.
    std::mutex& GetUnitsMutex();
    const std::unordered_map<uint32_t, TrackedUnit>& GetUnitsLocked();

    template<typename Visit>
    void ForEachUnit(const Visit& visit)
    {
        std::lock_guard lock(GetUnitsMutex());
        for (auto& [unit, tracked] : GetUnitsLocked())
            visit(unit, tracked);
    }
}
