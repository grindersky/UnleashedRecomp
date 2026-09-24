#include "tas_hud.h"
#include <api/SWA.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <ui/imgui_utils.h>
#include <tas_mode.h>

#include <cmath>
#include <numbers>

// The day Sonic player context, recorded when its constructor runs. Its velocity is split like in Sonic
// Generations' CPlayerSpeedContext: the full velocity, the part along the ground and the part along the
// up vector (full = horizontal + vertical). Found by comparing per-frame dumps of the context with how
// Sonic's position moves.
static constexpr uint32_t SONIC_CONTEXT_VFTABLE = 0x820182C4;
static constexpr uint32_t VELOCITY_OFFSET = 0x210;
static constexpr uint32_t HORIZONTAL_VELOCITY_OFFSET = 0x220;
static constexpr uint32_t VERTICAL_VELOCITY_OFFSET = 0x230;

static uint32_t g_speedContext;
static int16_t g_thumbLX;
static int16_t g_thumbLY;

// SWA::Player::CPlayerSpeedContext::CPlayerSpeedContext (the base of the day Sonic context)
PPC_FUNC_IMPL(__imp__sub_82330188);
PPC_FUNC(sub_82330188)
{
    uint32_t context = ctx.r3.u32;
    __imp__sub_82330188(ctx, base);

    if (TasHud::IsEnabled())
        g_speedContext = context;
}

// The target zones of the M-/D-Speed Visualiser, on the stick as SDL reports it: -1..1, Y pointing
// down, angles as atan2(y, x). Its per-axis deadzone applies before checking them.
struct TargetZone
{
    float StartAngle;
    float EndAngle;
    float InnerRadius;
    float OuterRadius;
};

static constexpr float PI = std::numbers::pi_v<float>;
static constexpr float STICK_DEADZONE = 0.05f;

static constexpr TargetZone TARGET_ZONES[] =
{
    { 5 * PI / 4, 7 * PI / 4, 0.45f, 0.70f }, // Up
    { -PI / 4, PI / 4, 0.29f, 0.50f },        // Right
    { 3 * PI / 4, 5 * PI / 4, 0.29f, 0.50f }, // Left
};

static bool IsInZone(const TargetZone& zone, float x, float y)
{
    float radius = std::hypot(x, y);
    if (radius < zone.InnerRadius || radius > zone.OuterRadius)
        return false;

    // Angle from the start of the zone, in [0, 2π), so zones that cross angle 0 work too.
    float angle = std::fmod(std::atan2(y, x) - zone.StartAngle + 4 * PI, 2 * PI);
    return angle <= zone.EndAngle - zone.StartAngle;
}

static float ApplyDeadzone(float value)
{
    return std::abs(value) < STICK_DEADZONE ? 0.0f : value;
}

static float LoadFloat(uint32_t address)
{
    uint8_t* base = g_memory.base;
    uint32_t value = PPC_LOAD_U32(address);
    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static bool GetVelocity(uint32_t offset, float& x, float& y, float& z)
{
    uint8_t* base = g_memory.base;

    // The context may have been destroyed since, and its memory reused.
    if (g_speedContext == 0 || PPC_LOAD_U32(g_speedContext) != SONIC_CONTEXT_VFTABLE)
        return false;

    x = LoadFloat(g_speedContext + offset);
    y = LoadFloat(g_speedContext + offset + 4);
    z = LoadFloat(g_speedContext + offset + 8);
    return true;
}

bool TasHud::IsEnabled()
{
    static const bool s_enabled = []()
    {
        const char* value = std::getenv("UNLEASHED_TAS_HUD");
        return IsTasMode() && value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();

    return s_enabled;
}

void TasHud::OnInputState(int16_t thumbLX, int16_t thumbLY)
{
    g_thumbLX = thumbLX;
    g_thumbLY = thumbLY;
}

static void DrawText(ImDrawList* drawList, ImFont* font, float size, ImVec2 pos, ImU32 colour, const std::string& text)
{
    drawList->AddText(font, size, { pos.x + Scale(1), pos.y + Scale(1) }, IM_COL32(0, 0, 0, 200), text.c_str());
    drawList->AddText(font, size, pos, colour, text.c_str());
}

static void DrawZone(ImDrawList* drawList, ImVec2 centre, float scale, const TargetZone& zone, bool active)
{
    constexpr int SEGMENTS = 20;

    ImU32 fill = active ? IM_COL32(0, 255, 0, 200) : IM_COL32(0, 255, 0, 80);
    ImU32 outline = active ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 160);

    ImVec2 outer[SEGMENTS + 1];
    ImVec2 inner[SEGMENTS + 1];

    for (int i = 0; i <= SEGMENTS; i++)
    {
        float angle = zone.StartAngle + (zone.EndAngle - zone.StartAngle) * i / SEGMENTS;
        outer[i] = { centre.x + std::cos(angle) * zone.OuterRadius * scale, centre.y + std::sin(angle) * zone.OuterRadius * scale };
        inner[i] = { centre.x + std::cos(angle) * zone.InnerRadius * scale, centre.y + std::sin(angle) * zone.InnerRadius * scale };
    }

    // The band isn't convex, so fill it one quad at a time.
    for (int i = 0; i < SEGMENTS; i++)
        drawList->AddQuadFilled(outer[i], outer[i + 1], inner[i + 1], inner[i], fill);

    drawList->AddPolyline(outer, SEGMENTS + 1, outline, 0, Scale(1));
    drawList->AddPolyline(inner, SEGMENTS + 1, outline, 0, Scale(1));
    drawList->AddLine(outer[0], inner[0], outline, Scale(1));
    drawList->AddLine(outer[SEGMENTS], inner[SEGMENTS], outline, Scale(1));
}

void TasHud::Draw()
{
    if (!IsEnabled())
        return;

    auto drawList = ImGui::GetBackgroundDrawList();
    auto& res = ImGui::GetIO().DisplaySize;
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");

    float radius = Scale(70);
    ImVec2 centre = { res.x - Scale(40) - radius, res.y - Scale(70) - radius };
    ImVec2 panelMin = { centre.x - radius - Scale(10), centre.y - radius - Scale(56) };
    ImVec2 panelMax = { centre.x + radius + Scale(10), centre.y + radius + Scale(48) };

    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 120), Scale(6));

    // Speedometer.
    float hx, hy, hz, vx, vy, vz, fx, fy, fz;
    if (GetVelocity(HORIZONTAL_VELOCITY_OFFSET, hx, hy, hz) && GetVelocity(VERTICAL_VELOCITY_OFFSET, vx, vy, vz) &&
        GetVelocity(VELOCITY_OFFSET, fx, fy, fz))
    {
        DrawText(drawList, font, Scale(18), { panelMin.x + Scale(8), panelMin.y + Scale(6) }, IM_COL32_WHITE,
            fmt::format("Speed {:.2f}", std::sqrt(hx * hx + hy * hy + hz * hz)));

        // The vertical part's sign: along the up vector or against it.
        float vertical = std::sqrt(vx * vx + vy * vy + vz * vz) * (vy < 0 ? -1.0f : 1.0f);
        DrawText(drawList, font, Scale(11), { panelMin.x + Scale(8), panelMin.y + Scale(28) }, IM_COL32(220, 220, 220, 255),
            fmt::format("Total {:.2f}   Vertical {:.2f}", std::sqrt(fx * fx + fy * fy + fz * fz), vertical));
    }
    else
    {
        DrawText(drawList, font, Scale(18), { panelMin.x + Scale(8), panelMin.y + Scale(6) }, IM_COL32(160, 160, 160, 255), "Speed --");
    }

    // Stick, as SDL (and libTAS) reports it: XInput's Y is flipped with a bitwise NOT.
    int16_t sdlX = g_thumbLX;
    int16_t sdlY = int16_t(~g_thumbLY);
    float x = ApplyDeadzone(sdlX / 32768.0f);
    float y = ApplyDeadzone(sdlY / 32768.0f);

    drawList->AddCircle(centre, radius, IM_COL32(255, 255, 255, 200), 64, Scale(1.5f));
    drawList->AddLine({ centre.x - radius, centre.y }, { centre.x + radius, centre.y }, IM_COL32(255, 255, 255, 90), Scale(1));
    drawList->AddLine({ centre.x, centre.y - radius }, { centre.x, centre.y + radius }, IM_COL32(255, 255, 255, 90), Scale(1));

    int activeZone = -1;
    for (int i = 0; i < int(std::size(TARGET_ZONES)); i++)
    {
        bool active = IsInZone(TARGET_ZONES[i], x, y);
        if (active && activeZone < 0)
            activeZone = i;

        DrawZone(drawList, centre, radius, TARGET_ZONES[i], active);
    }

    ImVec2 stick = { centre.x + x * radius, centre.y + y * radius };
    drawList->AddLine(centre, stick, IM_COL32(255, 60, 60, 255), Scale(2));
    drawList->AddCircleFilled(stick, Scale(4.5f), IM_COL32(255, 60, 60, 255));

    ImU32 zoneColour = activeZone >= 0 ? IM_COL32(80, 255, 80, 255) : IM_COL32(200, 200, 200, 255);
    std::string zoneText = activeZone >= 0 ? fmt::format("Zone {}", activeZone + 1) : "No zone";
    DrawText(drawList, font, Scale(11), { panelMin.x + Scale(8), centre.y + radius + Scale(6) }, zoneColour, zoneText);

    // What to type into libTAS, and what the game made of it.
    DrawText(drawList, font, Scale(11), { panelMin.x + Scale(8), centre.y + radius + Scale(20) }, IM_COL32_WHITE,
        fmt::format("X {:.2f} Y {:.2f}  ({}, {})", x, y, sdlX, sdlY));

    if (auto inputState = SWA::CInputState::GetInstance())
    {
        auto& padState = inputState->GetPadState();
        DrawText(drawList, font, Scale(11), { panelMin.x + Scale(8), centre.y + radius + Scale(34) }, IM_COL32(200, 200, 200, 255),
            fmt::format("Game {:.2f} {:.2f}", float(padState.LeftStickHorizontal), float(padState.LeftStickVertical)));
    }
}
