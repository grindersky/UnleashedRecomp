#pragma once

// The game's collision debug views (the EnableStageCollisionDebugView, EnableEventCollisionDebugView and
// EnableObjectCollisionDebugView codes). F11 switches between them and the level as it normally looks.
namespace CollisionView
{
    // Whether any of the codes is on.
    bool IsEnabled();

    // Whether the views are shown (with IsEnabled), or the level as it normally looks.
    bool IsShown();
}
