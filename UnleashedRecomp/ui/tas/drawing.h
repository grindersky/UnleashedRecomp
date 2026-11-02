#pragma once

// Drawing helpers the TAS HUD, the radar and the collision overlay share.

#include <ui/imgui_utils.h>

#include <string>

namespace Tas
{
    // Text with a dark shadow a pixel down and right.
    inline void DrawShadowedText(ImDrawList* drawList, ImFont* font, float size, ImVec2 pos, ImU32 colour, const std::string& text)
    {
        drawList->AddText(font, size, { pos.x + Scale(1), pos.y + Scale(1) }, IM_COL32(0, 0, 0, 200), text.c_str());
        drawList->AddText(font, size, pos, colour, text.c_str());
    }

    // The player's body, enemies and his facing, in the 3D overlay and on the radar.
    inline const ImU32 BODY_COLOUR = IM_COL32(0, 240, 255, 200);
    inline const ImU32 ENEMY_COLOUR = IM_COL32(255, 40, 40, 110);
    inline const ImU32 ARROW_COLOUR = IM_COL32(255, 255, 255, 240);
}
