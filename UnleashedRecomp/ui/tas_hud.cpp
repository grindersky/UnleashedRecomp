#include "tas_hud.h"
#include <api/SWA.h>
#include <app.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <ui/imgui_utils.h>
#include <tas_mode.h>

#include <cmath>
#include <numbers>
#include <unordered_map>

// The day Sonic player context (SWA::Player::CSonicContext), recorded when its constructor runs. Its
// velocity is split like in Sonic Generations' CPlayerSpeedContext: the full velocity, the part along the
// ground and the part along the up vector (full = horizontal + vertical). Found by comparing per-frame
// dumps of the context with how Sonic's position moves.
static constexpr uint32_t SONIC_CONTEXT_VFTABLE = 0x820182C4;
static constexpr uint32_t VELOCITY_OFFSET = 0x210;
static constexpr uint32_t HORIZONTAL_VELOCITY_OFFSET = 0x220;
static constexpr uint32_t VERTICAL_VELOCITY_OFFSET = 0x230;

// More of the context, found in the game's code:
// - the player object, whose state machine is at +0xCC (the context's ChangeState, sub_82307E18);
static constexpr uint32_t PLAYER_OFFSET = 0x100;
static constexpr uint32_t PLAYER_STATE_MACHINE_OFFSET = 0xCC;
// - whether Sonic stands on the ground (sub_82316530 switches to "Fall" when it's false);
static constexpr uint32_t GROUNDED_OFFSET = 0x3C0;
// - two sets of state flags: counters, one byte per flag, in an array at +4 (sub_823171C8 and the next ones).
static constexpr uint32_t STATE_FLAGS_OFFSET = 0x4AC;
static constexpr uint32_t STATE_FLAGS_2_OFFSET = 0x4B0;
// Set by a short hop (CStateJumpShort) until it has lasted StompingDisableTime (parameter 292, 0.15 s by
// default): until then, stomping is refused.
static constexpr uint32_t STOMP_DISABLED_FLAG = 20;
static constexpr uint32_t STOMPING_DISABLE_TIME_PARAMETER = 292;
// Stomping is also refused while this is set, unless flag 7 of the second set is (sub_8231B7D8).
static constexpr uint32_t STOMP_BLOCKER_OFFSET = 0xCC8;
static constexpr uint32_t STOMP_BLOCKER_OVERRIDE_FLAG = 7;
// Hedgehog engine states: the context at +8, the time spent in the state, in seconds, at +0x10.
static constexpr uint32_t STATE_CONTEXT_OFFSET = 0x8;
static constexpr uint32_t STATE_TIME_OFFSET = 0x10;

static uint32_t g_speedContext;
static int16_t g_thumbLX;
static int16_t g_thumbLY;

// Counts drawn frames, to tell whether the checks below ran during the update of the frame being drawn.
static uint32_t g_frame = 1;
static uint32_t g_jumpCheckFrame;
static uint32_t g_stompCheckFrame;
static bool g_stompDisabledByHop;
static bool g_stompBlocked;
static float g_stompingDisableTime = 0.15f;

// SWA::Player::CPlayerSpeedContext::CPlayerSpeedContext (the base of the day Sonic context)
PPC_FUNC_IMPL(__imp__sub_82330188);
PPC_FUNC(sub_82330188)
{
    uint32_t context = ctx.r3.u32;
    __imp__sub_82330188(ctx, base);

    if (TasHud::IsEnabled())
        g_speedContext = context;
}

static bool IsReadable(uint32_t address)
{
    // Everything but the first page of guest memory can be read.
    return address >= 0x1000 && address < 0xFFFF0000;
}

static uint32_t LoadU32(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U32(address) : 0;
}

static uint8_t LoadU8(uint32_t address)
{
    uint8_t* base = g_memory.base;
    return IsReadable(address) ? PPC_LOAD_U8(address) : 0;
}

static float LoadFloat(uint32_t address)
{
    uint32_t value = LoadU32(address);
    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static bool IsSonicContext(uint32_t context)
{
    // The context may have been destroyed since, and its memory reused.
    return context != 0 && LoadU32(context) == SONIC_CONTEXT_VFTABLE;
}

static bool GetFlag(uint32_t context, uint32_t flagsOffset, uint32_t flag)
{
    uint32_t flags = LoadU32(context + flagsOffset);
    return flags != 0 && LoadU8(LoadU32(flags + 4) + flag) != 0;
}

// The context's TryJump: jumps if the jump button is pressed. The states that allow jumping call it every
// frame; others, like a quick step until it ends, don't.
PPC_FUNC_IMPL(__imp__sub_82330CB0);
PPC_FUNC(sub_82330CB0)
{
    if (TasHud::IsEnabled() && ctx.r3.u32 == g_speedContext)
        g_jumpCheckFrame = g_frame;

    __imp__sub_82330CB0(ctx, base);
}

// The context's TryStomp, called by the air states when no action tried before it (like a homing attack) was taken.
PPC_FUNC_IMPL(__imp__sub_823765C0);
PPC_FUNC(sub_823765C0)
{
    uint32_t context = ctx.r3.u32;

    if (TasHud::IsEnabled() && context == g_speedContext)
    {
        g_stompCheckFrame = g_frame;
        g_stompDisabledByHop = GetFlag(context, STATE_FLAGS_OFFSET, STOMP_DISABLED_FLAG);
        g_stompBlocked = LoadU32(context + STOMP_BLOCKER_OFFSET) != 0 && !GetFlag(context, STATE_FLAGS_2_OFFSET, STOMP_BLOCKER_OVERRIDE_FLAG);
    }

    __imp__sub_823765C0(ctx, base);
}

// Reads a float parameter of the player by ID. Records StompingDisableTime when the short hop reads it.
PPC_FUNC_IMPL(__imp__sub_8245DB60);
PPC_FUNC(sub_8245DB60)
{
    uint32_t parameter = ctx.r4.u32;
    __imp__sub_8245DB60(ctx, base);

    if (parameter == STOMPING_DISABLE_TIME_PARAMETER && TasHud::IsEnabled())
        g_stompingDisableTime = float(ctx.f1.f64);}

// The current state of the player's state machine, or 0. The machine points to the current state's record
// at +0x58 (compared with the new state in sub_82E669D8), which points to the state at +0x1C.
static uint32_t GetCurrentState(uint32_t context)
{
    uint32_t player = LoadU32(context + PLAYER_OFFSET);
    if (player == 0)
        return 0;

    uint32_t stateMachine = player + PLAYER_STATE_MACHINE_OFFSET;
    uint32_t record = LoadU32(stateMachine + 0x58);
    uint32_t state = LoadU32(record + 0x1C);
    return state != 0 && LoadU32(state + STATE_CONTEXT_OFFSET) == context ? state : 0;
}

// The name of the state's class, from the game's RTTI: "CStateJumpShort@CSonicContext@..." becomes "JumpShort".
static const std::string& GetStateName(uint32_t state)
{
    static std::unordered_map<uint32_t, std::string> s_names;

    uint32_t vtable = LoadU32(state);
    auto it = s_names.find(vtable);
    if (it != s_names.end())
        return it->second;

    std::string name = "?";

    // The vtable is preceded by its complete object locator, which points to the type descriptor at +12,
    // whose mangled name is at +8.
    uint32_t typeDescriptor = LoadU32(LoadU32(vtable - 4) + 12);
    if (vtable >= PPC_IMAGE_BASE && vtable < PPC_IMAGE_BASE + PPC_IMAGE_SIZE &&
        typeDescriptor >= PPC_IMAGE_BASE && typeDescriptor < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
    {
        auto text = reinterpret_cast<const char*>(g_memory.Translate(typeDescriptor + 8));
        std::string mangled(text, strnlen(text, 96));

        if (mangled.starts_with(".?AV"))
        {
            name = mangled.substr(4, mangled.find('@') - 4);

            for (std::string_view prefix : { "CPlayerSpeedState", "CSonicState", "CState" })
            {
                if (name.starts_with(prefix) && name.size() > prefix.size())
                {
                    name = name.substr(prefix.size());
                    break;
                }
            }
        }
    }

    return s_names.emplace(vtable, name).first->second;
}

static bool GetVelocity(uint32_t offset, float& x, float& y, float& z)
{
    if (!IsSonicContext(g_speedContext))
        return false;

    x = LoadFloat(g_speedContext + offset);
    y = LoadFloat(g_speedContext + offset + 4);
    z = LoadFloat(g_speedContext + offset + 8);
    return true;
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

// How the game reads the stick, fitted to the values it produces (within 0.001): each axis loses a
// deadzone and is scaled back to 0..1, then the vector is shortened to length 1 if it's longer.
static constexpr float GAME_STICK_DEADZONE = 0.2651f;

// The break-jump glitch: holding the stick through a switch to 2D and a jump, where the game reads it as at
// least 0.1 long (2D movement ignores shorter input) but shorter than 0.3 (NoPadStopWalkPadLengthLimit,
// parameter 140), makes Sonic's velocity explode. Found by replaying a movie with the stick at different
// positions, then matching the limits with the game's values.
static constexpr float BREAK_JUMP_MIN_LENGTH = 0.1f;
static constexpr float BREAK_JUMP_MAX_LENGTH = 0.3f;

// The raw stick positions the game reads as a given length form a square with rounded corners: the
// deadzone cross in the middle, then arcs of radius length * (1 - deadzone) around (±deadzone, ±deadzone).
// The zone is the ring between the two lengths.
static void DrawBreakJumpZone(ImDrawList* drawList, ImVec2 centre, float scale, bool active)
{
    constexpr int ARC_SEGMENTS = 8;
    constexpr int POINTS = 4 * (ARC_SEGMENTS + 1);

    ImVec2 inner[POINTS];
    ImVec2 outer[POINTS];
    int count = 0;

    for (int quadrant = 0; quadrant < 4; quadrant++)
    {
        float cornerX = (quadrant == 0 || quadrant == 3) ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cornerY = quadrant < 2 ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;

        for (int i = 0; i <= ARC_SEGMENTS; i++, count++)
        {
            float angle = (quadrant + float(i) / ARC_SEGMENTS) * PI / 2;
            float cosine = std::cos(angle) * (1.0f - GAME_STICK_DEADZONE);
            float sine = std::sin(angle) * (1.0f - GAME_STICK_DEADZONE);

            inner[count] = { centre.x + (cornerX + cosine * BREAK_JUMP_MIN_LENGTH) * scale, centre.y + (cornerY + sine * BREAK_JUMP_MIN_LENGTH) * scale };
            outer[count] = { centre.x + (cornerX + cosine * BREAK_JUMP_MAX_LENGTH) * scale, centre.y + (cornerY + sine * BREAK_JUMP_MAX_LENGTH) * scale };
        }
    }

    ImU32 fill = active ? IM_COL32(255, 150, 30, 200) : IM_COL32(255, 150, 30, 70);
    ImU32 outline = active ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 120);

    // The ring isn't convex, so fill it one quad at a time; the gaps between quadrants are the straight sides.
    for (int i = 0; i < POINTS; i++)
    {
        int next = (i + 1) % POINTS;
        drawList->AddQuadFilled(outer[i], outer[next], inner[next], inner[i], fill);
    }

    drawList->AddPolyline(outer, POINTS, outline, ImDrawFlags_Closed, Scale(1));
    drawList->AddPolyline(inner, POINTS, outline, ImDrawFlags_Closed, Scale(1));
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

static float TextWidth(ImFont* font, float size, const std::string& text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}

// A labelled box, lit when active: filled for what Sonic is doing, outlined for what a press would do.
static void DrawIndicator(ImDrawList* drawList, ImFont* font, ImVec2 pos, float width, const char* label, ImU32 colour, bool lit, bool outlined = false)
{
    float size = Scale(11);
    float textWidth = TextWidth(font, size, label);
    ImVec2 max = { pos.x + width, pos.y + Scale(16) };
    ImU32 textColour = IM_COL32(170, 170, 170, 255);

    if (outlined)
    {
        drawList->AddRectFilled(pos, max, IM_COL32(20, 20, 20, 200), Scale(3));
        drawList->AddRect(pos, max, lit ? colour : IM_COL32(80, 80, 80, 255), Scale(3), 0, Scale(1.5f));

        if (lit)
            textColour = colour;
    }
    else
    {
        drawList->AddRectFilled(pos, max, lit ? colour : IM_COL32(20, 20, 20, 200), Scale(3));

        if (lit)
            textColour = IM_COL32(0, 0, 0, 255);
    }

    drawList->AddText(font, size, { pos.x + (width - textWidth) / 2, pos.y + Scale(2) }, textColour, label);
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

static constexpr float CONTROLLER_HEIGHT = 46;

// The buttons used in play, as the game read them this frame: the triggers (filled as far as they're
// pulled), bumpers and Start on the left, the face buttons on the right. Held buttons are lit; ones pressed
// on this frame, which is what jumps and stomps need, also get a white ring.
static void DrawController(ImDrawList* drawList, ImFont* font, ImVec2 pos, float width, const SWA::SPadState* pad)
{
    uint32_t down = pad != nullptr ? uint32_t(pad->DownState) : 0;
    uint32_t tapped = pad != nullptr ? uint32_t(pad->TappedState) : 0;
    float size = Scale(12);
    ImU32 ring = IM_COL32(255, 255, 255, 255);
    ImU32 unlitFill = IM_COL32(20, 20, 20, 200);

    auto drawLabel = [&](ImVec2 centre, const char* label, ImU32 colour)
    {
        ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
        drawList->AddText(font, size, { centre.x - textSize.x / 2, centre.y - textSize.y / 2 }, colour, label);
    };

    auto drawBox = [&](ImVec2 min, ImVec2 max, const char* label, uint32_t key, float fill = -1.0f)
    {
        bool lit = (down & key) != 0;
        drawList->AddRectFilled(min, max, unlitFill, Scale(3));

        // Triggers are analog: fill them as far as they're pulled.
        float amount = fill >= 0.0f ? std::clamp(fill, 0.0f, 1.0f) : (lit ? 1.0f : 0.0f);
        if (amount > 0.0f)
            drawList->AddRectFilled(min, { min.x + (max.x - min.x) * amount, max.y }, IM_COL32(220, 220, 220, 230), Scale(3));

        drawList->AddRect(min, max, (tapped & key) ? ring : IM_COL32(110, 110, 110, 255), Scale(3), 0, (tapped & key) ? Scale(2) : Scale(1));
        drawLabel({ (min.x + max.x) / 2, (min.y + max.y) / 2 }, label, amount > 0.5f ? IM_COL32(0, 0, 0, 255) : IM_COL32(200, 200, 200, 255));
    };

    auto drawButton = [&](ImVec2 centre, float radius, const char* label, uint32_t key, ImU32 colour)
    {
        bool lit = (down & key) != 0;
        drawList->AddCircleFilled(centre, radius, lit ? colour : unlitFill, 16);
        drawList->AddCircle(centre, radius, (tapped & key) ? ring : colour, 16, (tapped & key) ? Scale(2) : Scale(1.2f));
        drawLabel(centre, label, lit ? IM_COL32(0, 0, 0, 255) : colour);
    };

    // Triggers and bumpers in their order on the controller, Start under them.
    float middle = pos.y + CONTROLLER_HEIGHT * Scale(1) / 2;
    float rowHeight = Scale(18);
    float rowGap = Scale(4);
    float top = middle - rowHeight - rowGap / 2;
    float gap = Scale(4);
    float slot = (width * 0.6f - 3 * gap) / 4;
    auto slotX = [&](int i) { return pos.x + i * (slot + gap); };

    drawBox({ slotX(0), top }, { slotX(0) + slot, top + rowHeight }, "LT", SWA::eKeyState_LeftTrigger, pad != nullptr ? float(pad->LeftTrigger) : 0.0f);
    drawBox({ slotX(1), top }, { slotX(1) + slot, top + rowHeight }, "LB", SWA::eKeyState_LeftBumper);
    drawBox({ slotX(2), top }, { slotX(2) + slot, top + rowHeight }, "RB", SWA::eKeyState_RightBumper);
    drawBox({ slotX(3), top }, { slotX(3) + slot, top + rowHeight }, "RT", SWA::eKeyState_RightTrigger, pad != nullptr ? float(pad->RightTrigger) : 0.0f);

    float startTop = middle + rowGap / 2;
    drawBox({ slotX(1), startTop }, { slotX(2) + slot, startTop + rowHeight }, "START", SWA::eKeyState_Start);

    // Face buttons.
    ImVec2 face = { pos.x + width * 0.8f, middle };
    float spread = Scale(12);
    float buttonRadius = Scale(10);
    drawButton({ face.x, face.y - spread }, buttonRadius, "Y", SWA::eKeyState_Y, IM_COL32(255, 200, 40, 255));
    drawButton({ face.x - spread * 1.6f, face.y }, buttonRadius, "X", SWA::eKeyState_X, IM_COL32(60, 140, 255, 255));
    drawButton({ face.x + spread * 1.6f, face.y }, buttonRadius, "B", SWA::eKeyState_B, IM_COL32(255, 70, 60, 255));
    drawButton({ face.x, face.y + spread }, buttonRadius, "A", SWA::eKeyState_A, IM_COL32(80, 220, 80, 255));
}

void TasHud::Draw()
{
    if (!IsEnabled())
        return;

    auto drawList = ImGui::GetBackgroundDrawList();
    auto& res = ImGui::GetIO().DisplaySize;
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");

    // Wide enough for the longest stick line and three indicators a row as wide as the widest, away from
    // the edge of the screen.
    float radius = Scale(70);
    float padding = Scale(8);
    float gap = Scale(4);
    float width = std::max({ 2 * radius + 2 * padding,
        TextWidth(font, Scale(11), "X -1.00 Y -1.00  (-32768, -32768)") + 2 * padding,
        3 * (TextWidth(font, Scale(11), "CAN STOMP") + Scale(10)) + 2 * gap + 2 * padding });
    ImVec2 panelMax = { res.x - Scale(40), res.y - Scale(22) };
    ImVec2 centre = { panelMax.x - width / 2, panelMax.y - Scale(58 + CONTROLLER_HEIGHT) - radius };
    ImVec2 panelMin = { panelMax.x - width, centre.y - radius - Scale(102) };
    float left = panelMin.x + padding;

    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 140), Scale(6));

    // Speedometer.
    float hx, hy, hz, vx, vy, vz, fx, fy, fz;
    if (GetVelocity(HORIZONTAL_VELOCITY_OFFSET, hx, hy, hz) && GetVelocity(VERTICAL_VELOCITY_OFFSET, vx, vy, vz) &&
        GetVelocity(VELOCITY_OFFSET, fx, fy, fz))
    {
        DrawText(drawList, font, Scale(18), { left, panelMin.y + Scale(6) }, IM_COL32_WHITE,
            fmt::format("Speed {:.2f}", std::sqrt(hx * hx + hy * hy + hz * hz)));

        // The vertical part's sign: along the up vector or against it.
        float vertical = std::sqrt(vx * vx + vy * vy + vz * vz) * (vy < 0 ? -1.0f : 1.0f);
        DrawText(drawList, font, Scale(11), { left, panelMin.y + Scale(28) }, IM_COL32(220, 220, 220, 255),
            fmt::format("Total {:.2f}   Vertical {:.2f}", std::sqrt(fx * fx + fy * fy + fz * fz), vertical));
    }
    else
    {
        DrawText(drawList, font, Scale(18), { left, panelMin.y + Scale(6) }, IM_COL32(160, 160, 160, 255), "Speed --");
    }

    // What Sonic is doing, then what a press on this frame would have done: the game checked for the jump
    // or stomp button during this frame's update. Both need a new press, holding the button doesn't count.
    float row = panelMin.y + Scale(46);
    float pressRow = row + Scale(20);
    float stateRow = pressRow + Scale(20);

    if (IsSonicContext(g_speedContext))
    {
        uint32_t context = g_speedContext;
        uint32_t state = GetCurrentState(context);
        std::string stateName = state != 0 ? GetStateName(state) : "?";
        float stateTime = state != 0 ? LoadFloat(state + STATE_TIME_OFFSET) : 0.0f;
        float deltaTime = std::max(float(App::s_deltaTime), 0.001f);

        bool grounded = LoadU8(context + GROUNDED_OFFSET) != 0;
        bool sliding = stateName.starts_with("Sliding");
        bool stomping = stateName.starts_with("Stomping");
        bool canJump = g_jumpCheckFrame == g_frame;
        bool stompChecked = g_stompCheckFrame == g_frame;
        bool canStomp = stompChecked && !g_stompDisabledByHop && !g_stompBlocked;

        float indicatorWidth = (width - 2 * padding - 2 * gap) / 3;
        float column2 = left + indicatorWidth + gap;
        float column3 = column2 + indicatorWidth + gap;

        DrawIndicator(drawList, font, { left, row }, indicatorWidth, grounded ? "GROUND" : "AIR", grounded ? IM_COL32(80, 220, 80, 230) : IM_COL32(90, 170, 255, 230), true);
        DrawIndicator(drawList, font, { column2, row }, indicatorWidth, "SLIDING", IM_COL32(255, 200, 60, 230), sliding);
        DrawIndicator(drawList, font, { column3, row }, indicatorWidth, "STOMPING", IM_COL32(255, 110, 80, 230), stomping);

        DrawIndicator(drawList, font, { left, pressRow }, indicatorWidth, "CAN JUMP", IM_COL32(80, 230, 80, 255), canJump, true);
        DrawIndicator(drawList, font, { column2, pressRow }, indicatorWidth, "CAN STOMP", IM_COL32(80, 230, 80, 255), canStomp, true);

        // During a short hop: in how many frames the stomp will be accepted. The hop adds the frame's time
        // (in single precision) to its own, and only then compares it and tries the stomp. Leaving the hop
        // (for an air boost, say) clears the flag right away instead.
        bool hopping = stateName == "JumpShort" || stateName == "JumpHurdle";
        if (stompChecked && g_stompDisabledByHop && hopping)
        {
            int frames = 0;
            float time = stateTime;
            do
            {
                time += deltaTime;
                frames++;
            } while (time <= g_stompingDisableTime && frames < 99);

            DrawText(drawList, font, Scale(11), { column3, pressRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", frames));
        }

        DrawText(drawList, font, Scale(11), { left, stateRow }, IM_COL32(220, 220, 220, 255),
            fmt::format("{}  {}f", stateName, int(std::lround(stateTime / deltaTime))));
    }
    else
    {
        DrawText(drawList, font, Scale(11), { left, row }, IM_COL32(160, 160, 160, 255), "No player");
    }

    // Stick, as SDL (and libTAS) reports it: XInput's Y is flipped with a bitwise NOT.
    int16_t sdlX = g_thumbLX;
    int16_t sdlY = int16_t(~g_thumbLY);
    float x = ApplyDeadzone(sdlX / 32768.0f);
    float y = ApplyDeadzone(sdlY / 32768.0f);

    // And as the game read it this frame.
    const SWA::SPadState* padState = nullptr;
    if (auto inputState = SWA::CInputState::GetInstance())
        padState = &inputState->GetPadState();

    float gameX = padState != nullptr ? float(padState->LeftStickHorizontal) : 0.0f;
    float gameY = padState != nullptr ? float(padState->LeftStickVertical) : 0.0f;
    float gameLengthSquared = gameX * gameX + gameY * gameY;
    bool breakJump = gameLengthSquared >= BREAK_JUMP_MIN_LENGTH * BREAK_JUMP_MIN_LENGTH &&
        gameLengthSquared < BREAK_JUMP_MAX_LENGTH * BREAK_JUMP_MAX_LENGTH;

    drawList->AddCircle(centre, radius, IM_COL32(255, 255, 255, 200), 64, Scale(1.5f));
    drawList->AddLine({ centre.x - radius, centre.y }, { centre.x + radius, centre.y }, IM_COL32(255, 255, 255, 90), Scale(1));
    drawList->AddLine({ centre.x, centre.y - radius }, { centre.x, centre.y + radius }, IM_COL32(255, 255, 255, 90), Scale(1));

    DrawBreakJumpZone(drawList, centre, radius, breakJump);

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

    std::string zoneText = activeZone >= 0 ? fmt::format("Zone {}", activeZone + 1) : "";
    if (breakJump)
        zoneText += zoneText.empty() ? "Break-jump" : " + Break-jump";

    ImU32 zoneColour = activeZone >= 0 ? IM_COL32(80, 255, 80, 255) : breakJump ? IM_COL32(255, 160, 40, 255) : IM_COL32(200, 200, 200, 255);
    DrawText(drawList, font, Scale(11), { left, centre.y + radius + Scale(6) }, zoneColour, zoneText.empty() ? "No zone" : zoneText);

    // What to type into libTAS, and what the game made of it.
    DrawText(drawList, font, Scale(11), { left, centre.y + radius + Scale(20) }, IM_COL32_WHITE,
        fmt::format("X {:.2f} Y {:.2f}  ({}, {})", x, y, sdlX, sdlY));

    if (padState != nullptr)
    {
        DrawText(drawList, font, Scale(11), { left, centre.y + radius + Scale(34) }, IM_COL32(200, 200, 200, 255),
            fmt::format("Game {:.3f} {:.3f}  length {:.3f}", gameX, gameY, std::sqrt(gameLengthSquared)));
    }

    DrawController(drawList, font, { left, centre.y + radius + Scale(52) }, width - 2 * padding, padState);

    g_frame++;
}
