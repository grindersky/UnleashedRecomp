#pragma once

// A check for changes to the TAS HUD and the collision overlay that shouldn't change what they show: with
// UNLEASHED_TAS_HUD_DUMP=<file>, every frame writes a line with what they added to the background draw list (how many
// vertices and indices, and a hash of the vertices: positions, texture coordinates, which carry the depth when tested
// against the game's, and colours). Two playbacks of a movie with the same settings give the same lines when nothing
// they draw changed.
namespace TasHudDump
{
    // Around CollisionOverlay::Draw and TasHud::Draw.
    void Begin();
    void End();
}
