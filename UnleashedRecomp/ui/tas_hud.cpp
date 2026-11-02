#include "tas_hud.h"
#include <api/SWA.h>
#include <app.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <gpu/video.h>
#include <ui/collision_overlay.h>
#include <ui/imgui_utils.h>
#include <ui/tas/drawing.h>
#include <ui/tas/hud_predictions.h>
#include <ui/tas/hud_state.h>
#include <ui/tas/vector_math.h>
#include <tas_mode.h>

#include <algorithm>
#include <cmath>
#include <vector>

// The TAS HUD's drawing. What it shows is read in ui/tas/hud_state.cpp (the player, and what the game's checks did) and
// predicted in ui/tas/hud_predictions.cpp; the radar and the hint ring labels are the collision overlay's.

using namespace Tas;

// The left stick given to the game, in XInput's convention (Y up).
static int16_t g_thumbLX;
static int16_t g_thumbLY;

// The per-axis deadzone the stick is drawn with, on the stick as SDL reports it (-1..1, Y pointing down).
static constexpr float STICK_DEADZONE = 0.05f;

static float ApplyDeadzone(float value)
{
    return std::abs(value) < STICK_DEADZONE ? 0.0f : value;
}

// The raw stick positions the game reads as a given length form a square with rounded corners: the
// deadzone cross in the middle, then arcs of radius length * (1 - deadzone) around (±deadzone, ±deadzone).
// The zone is the ring between the two lengths.
// The zones are filled one quad at a time. ImGui fades each filled shape's edges out over a pixel, so where two quads
// meet, both faded edges let the background show through as a thin line. While this is alive, fills get hard edges, which
// meet exactly; the zones' outlines are drawn over their outer edges, smoothed as usual.
struct HardEdgedFills
{
    ImDrawList* DrawList;
    ImDrawListFlags Flags;

    explicit HardEdgedFills(ImDrawList* drawList) : DrawList(drawList), Flags(drawList->Flags)
    {
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    }

    ~HardEdgedFills()
    {
        DrawList->Flags = Flags;
    }
};

static void DrawLengthZone(ImDrawList* drawList, ImVec2 centre, float scale, float minLength, float maxLength, ImU32 fill, ImU32 outline)
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
            float cosine = std::cos(angle) * GAME_STICK_RANGE;
            float sine = std::sin(angle) * GAME_STICK_RANGE;

            inner[count] = { centre.x + (cornerX + cosine * minLength) * scale, centre.y + (cornerY + sine * minLength) * scale };
            outer[count] = { centre.x + (cornerX + cosine * maxLength) * scale, centre.y + (cornerY + sine * maxLength) * scale };
        }
    }

    // The ring isn't convex, so fill it one quad at a time; the gaps between quadrants are the straight sides.
    {
        HardEdgedFills hardEdges(drawList);
        for (int i = 0; i < POINTS; i++)
        {
            int next = (i + 1) % POINTS;
            drawList->AddQuadFilled(outer[i], outer[next], inner[next], inner[i], fill);
        }
    }

    drawList->AddPolyline(outer, POINTS, outline, ImDrawFlags_Closed, Scale(1));
    drawList->AddPolyline(inner, POINTS, outline, ImDrawFlags_Closed, Scale(1));
}

// A zone's edge in one direction from the middle of a quadrant's corner: where it starts, where M-Speed stops working
// and where it ends. Works and Fails say whether there's green or red there; Clipped, that it ends at the stick's circle.
struct ZoneSample
{
    bool InZone;
    bool Works;
    bool Fails;
    ImVec2 Inner;
    ImVec2 Middle;
    ImVec2 Outer;
    bool Clipped = false;
};

// Fills a zone sampled around the stick, quadrant by quadrant (between two quadrants, at the same angle, its straight
// sides): green from its start to where M-Speed stops working, red from there to its end. Bright where the stick is.
static void DrawZoneSamples(ImDrawList* drawList, const std::vector<ZoneSample>& samples, const MSpeedInput& stick)
{
    ImU32 works = stick.Works ? IM_COL32(0, 255, 0, 200) : IM_COL32(0, 255, 0, 80);
    ImU32 fails = stick.InZone && !stick.Works ? IM_COL32(255, 40, 40, 200) : IM_COL32(255, 40, 40, 80);
    ImU32 outline = stick.InZone ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 160);

    // Start outside the zone, so each part of it is drawn in one go.
    size_t count = samples.size();
    size_t start = 0;
    while (start < count && samples[start].InZone)
        start++;

    std::vector<ImVec2> inner;
    std::vector<ImVec2> outer;
    const ZoneSample* previous = nullptr;
    auto flush = [&]()
    {
        if (outer.size() >= 2)
        {
            // The outline: the outer edge, then back along the inner one.
            std::vector<ImVec2> line(outer);
            line.insert(line.end(), inner.rbegin(), inner.rend());
            drawList->AddPolyline(line.data(), int(line.size()), outline, ImDrawFlags_Closed, Scale(1));
        }

        inner.clear();
        outer.clear();
        previous = nullptr;
    };

    for (size_t step = 0; step <= count; step++)
    {
        const ZoneSample& sample = samples[(start + step) % count];
        if (!sample.InZone)
        {
            flush();
            continue;
        }

        if (previous != nullptr)
        {
            HardEdgedFills hardEdges(drawList);
            if (previous->Works || sample.Works)
                drawList->AddQuadFilled(previous->Middle, sample.Middle, sample.Inner, previous->Inner, works);

            if (previous->Fails || sample.Fails)
                drawList->AddQuadFilled(previous->Outer, sample.Outer, sample.Middle, previous->Middle, fails);
        }

        inner.push_back(sample.Inner);
        outer.push_back(sample.Outer);
        previous = &sample;
    }

    flush();
}

// The D-Speed zone: the stick positions whose part along the path (by the input basis as it is now) is from the
// deadzone's input to 0.3, a strip across the stick, as far as the stick's circle. It moves as the basis turns. Sampled
// like the M-Speed zone, in the stick as the game reads it (around each quadrant's corner of the deadzone cross); where
// the strip ends or reaches the circle inside a quadrant, the edge is found exactly, so its corners aren't cut.
static void DrawDSpeedZone(ImDrawList* drawList, ImVec2 centre, float scale, const MSpeedRule& rule, const MSpeedInput& stick)
{
    constexpr int ARC_SEGMENTS = 24;
    constexpr int EDGE_STEPS = 24;

    float shrink = GAME_STICK_RANGE;
    float maxInput = GetDSpeedMaxInput(rule);
    bool hasSide = HasDSpeedSide(rule);

    auto sample = [&](int quadrant, float angle)
    {
        float cornerX = (quadrant == 0 || quadrant == 3) ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cornerY = quadrant < 2 ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cosine = std::cos(angle);
        float sine = std::sin(angle);
        float forward = cosine * rule.PathX + sine * rule.PathY;

        // How far the game's stick goes this way before the raw stick leaves its circle (|corner + direction *
        // shrink * length| = 1), and at most 1.
        float b = (cornerX * cosine + cornerY * sine) * shrink;
        float c = cornerX * cornerX + cornerY * cornerY - 1.0f;
        float edge = std::min(1.0f, (-b + std::sqrt(b * b - shrink * shrink * c)) / (shrink * shrink));

        ZoneSample result{};
        if (!hasSide || !(forward > 0.0f))
            return result;

        float innerLength = GetSonicChecks().MovementInputDeadzone / forward;
        float outerLength = std::min(M_SPEED_MAX_INPUT / forward, edge);
        if (!(innerLength < outerLength))
            return result;

        float middleLength = std::clamp(maxInput / forward, innerLength, outerLength);
        auto point = [&](float length)
        {
            return ImVec2{ centre.x + (cornerX + cosine * shrink * length) * scale, centre.y + (cornerY + sine * shrink * length) * scale };
        };

        return ZoneSample{ true, middleLength > innerLength, middleLength < outerLength, point(innerLength), point(middleLength),
            point(outerLength), M_SPEED_MAX_INPUT / forward > edge };
    };

    // Outside, ending at 0.3, or ending at the circle.
    auto kind = [](const ZoneSample& sample) { return !sample.InZone ? 0 : sample.Clipped ? 2 : 1; };

    std::vector<ZoneSample> samples;
    for (int quadrant = 0; quadrant < 4; quadrant++)
    {
        float previousAngle = 0.0f;
        int previousKind = 0;
        for (int i = 0; i <= ARC_SEGMENTS; i++)
        {
            float angle = (quadrant + float(i) / ARC_SEGMENTS) * PI / 2;
            ZoneSample current = sample(quadrant, angle);

            // Where the strip ends, or its edge reaches the circle, between two samples: halve the gap until it's found,
            // and add a sample there.
            if (i > 0 && kind(current) != previousKind)
            {
                float from = previousAngle;
                float to = angle;
                for (int step = 0; step < EDGE_STEPS; step++)
                {
                    float middle = (from + to) / 2;
                    if (kind(sample(quadrant, middle)) == previousKind)
                        from = middle;
                    else
                        to = middle;
                }

                samples.push_back(sample(quadrant, previousKind != 0 ? from : to));
            }

            samples.push_back(current);
            previousAngle = angle;
            previousKind = kind(current);
        }
    }

    DrawZoneSamples(drawList, samples, stick);
}

// The M-Speed zone: like the length zone, from the deadzone's input to 0.3, on the side of the stick Sonic faces. Green up
// to the longest input that M-Speeds in each direction, red past it (all of it where nothing does). Bright where the
// stick is.
static void DrawMSpeedZone(ImDrawList* drawList, ImVec2 centre, float scale, const MSpeedRule& rule, const MSpeedInput& stick)
{
    constexpr int ARC_SEGMENTS = 24;

    float minLength = GetStickLengthForMovementInput(GetSonicChecks().MovementInputDeadzone);
    float maxLength = GetStickLengthForMovementInput(M_SPEED_MAX_INPUT);

    std::vector<ZoneSample> samples;
    for (int quadrant = 0; quadrant < 4; quadrant++)
    {
        float cornerX = (quadrant == 0 || quadrant == 3) ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;
        float cornerY = quadrant < 2 ? GAME_STICK_DEADZONE : -GAME_STICK_DEADZONE;

        for (int i = 0; i <= ARC_SEGMENTS; i++)
        {
            float angle = (quadrant + float(i) / ARC_SEGMENTS) * PI / 2;
            float cosine = std::cos(angle);
            float sine = std::sin(angle);
            float maxInput = GetMSpeedMaxInput(rule, cosine, sine);
            float middleLength = GetStickLengthForMovementInput(std::clamp(maxInput, GetSonicChecks().MovementInputDeadzone, M_SPEED_MAX_INPUT));

            float shrink = GAME_STICK_RANGE;
            auto point = [&](float length)
            {
                return ImVec2{ centre.x + (cornerX + cosine * shrink * length) * scale, centre.y + (cornerY + sine * shrink * length) * scale };
            };

            samples.push_back({ IsFacingSide(rule, cosine, sine), maxInput > GetSonicChecks().MovementInputDeadzone, maxInput < M_SPEED_MAX_INPUT,
                point(minLength), point(middleLength), point(maxLength) });
        }
    }

    DrawZoneSamples(drawList, samples, stick);
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

    // Centred on the letters themselves: the text's box has room under the baseline and beside the letters, which put
    // capitals high and to one side. ImGui truncates text positions to whole pixels, so the position is rounded instead.
    auto drawLabel = [&](ImVec2 centre, const char* label, ImU32 colour)
    {
        float scale = size / font->FontSize;
        float penX = 0.0f;
        ImVec2 min = { FLT_MAX, FLT_MAX };
        ImVec2 max = { -FLT_MAX, -FLT_MAX };
        for (const char* c = label; *c != '\0'; c++)
        {
            const ImFontGlyph* glyph = font->FindGlyph(ImWchar(*c));
            if (glyph == nullptr)
                continue;

            min = { std::min(min.x, penX + glyph->X0 * scale), std::min(min.y, glyph->Y0 * scale) };
            max = { std::max(max.x, penX + glyph->X1 * scale), std::max(max.y, glyph->Y1 * scale) };
            penX += glyph->AdvanceX * scale;
        }

        if (min.x > max.x)
            return;

        ImVec2 pos = { std::round(centre.x - (min.x + max.x) / 2), std::round(centre.y - (min.y + max.y) / 2) };
        drawList->AddText(font, size, pos, colour, label);
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

    // Only in levels, where Sonic or the Werehog is around.
    bool werehog;
    uint32_t context = GetPlayerContext(werehog);
    if (context == 0)
    {
        AdvanceHudFrame();
        return;
    }

    auto drawList = ImGui::GetBackgroundDrawList();
    auto& res = ImGui::GetIO().DisplaySize;
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");

    // Wide enough for the longest stick line and three indicators a row as wide as the widest, away from
    // the edge of the screen.
    float radius = Scale(70);
    float padding = Scale(8);
    float gap = Scale(4);
    float indicatorText = std::max({ TextWidth(font, Scale(11), "CAN STOMP"), TextWidth(font, Scale(11), "ATTACKING"),
        TextWidth(font, Scale(11), "CAN GUARD") });
    float width = std::max({ 2 * radius + 2 * padding,
        TextWidth(font, Scale(11), "X -1.00 Y -1.00  (-32768, -32768)") + 2 * padding,
        3 * (indicatorText + Scale(10)) + 2 * gap + 2 * padding });
    ImVec2 panelMax = { res.x - Scale(40), res.y - Scale(22) };
    ImVec2 centre = { panelMax.x - width / 2, panelMax.y - Scale(58 + CONTROLLER_HEIGHT) - radius };
    // Sonic has two rows more, for the homing attack and the boost.
    ImVec2 panelMin = { panelMax.x - width, centre.y - radius - Scale(werehog ? 122 : 162) };
    float left = panelMin.x + padding;

    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 140), Scale(6));

    // On dozen, loading a libTAS savestate crashes the game (Video::CreateHostDevice says why): under the panel.
    if (Video::IsVulkanOnD3D12())
    {
        DrawShadowedText(drawList, font, Scale(11), { left, panelMax.y + Scale(4) }, IM_COL32(255, 160, 40, 255),
            "DOZEN: no savestates");
    }

    // Speedometer: the speed along the ground, the full speed, and the vertical part, along the up vector
    // or against it.
    float speed, total, vertical;
    if (werehog)
    {
        // Split along the world's up, which is the Werehog's (his up vector stayed (0, 1, 0) everywhere).
        total = VectorLength(context + EVIL_VELOCITY_OFFSET, vertical);
        speed = std::sqrt(std::max(total * total - vertical * vertical, 0.0f));
    }
    else
    {
        // Split from the full velocity along Sonic's up vector: the game's own horizontal velocity (+0x220) stops at
        // the max speed (param 253, 145), while he can move faster than that (the break-jump, M-Speed).
        float y;
        total = VectorLength(context + VELOCITY_OFFSET, y);
        float upX = LoadFloat(context + UP_OFFSET);
        float upY = LoadFloat(context + UP_OFFSET + 4);
        float upZ = LoadFloat(context + UP_OFFSET + 8);
        float upLength = std::sqrt(upX * upX + upY * upY + upZ * upZ);
        if (upLength > 0.5f && std::isfinite(upLength))
        {
            vertical = (LoadFloat(context + VELOCITY_OFFSET) * upX + y * upY + LoadFloat(context + VELOCITY_OFFSET + 8) * upZ) / upLength;
            speed = std::sqrt(std::max(total * total - vertical * vertical, 0.0f));
        }
        else
        {
            speed = VectorLength(context + HORIZONTAL_VELOCITY_OFFSET, y);
            vertical = VectorLength(context + VERTICAL_VELOCITY_OFFSET, y);
            if (y < 0)
                vertical = -vertical;
        }
    }

    DrawShadowedText(drawList, font, Scale(18), { left, panelMin.y + Scale(6) }, IM_COL32_WHITE, fmt::format("Speed {:.2f}", speed));
    DrawShadowedText(drawList, font, Scale(11), { left, panelMin.y + Scale(28) }, IM_COL32(220, 220, 220, 255),
        fmt::format("Total {:.2f}   Vertical {:.2f}", total, vertical));

    // What the player is doing. For Sonic, then what a press entered now would do: whether the next update checks
    // for the jump or stomp button. Both need a new press, holding the button doesn't count.
    float row = panelMin.y + Scale(46);
    float pressRow = row + Scale(20);
    // Sonic has three more rows of presses, for the quick step, the homing attack and the boost; the Werehog has a row
    // under his state instead, for when his attack ends.
    float quickStepRow = pressRow + Scale(20);
    float homingRow = quickStepRow + Scale(20);
    float boostRow = homingRow + Scale(20);
    float stateRow = (werehog ? pressRow : boostRow) + Scale(20);
    float attackRow = stateRow + Scale(20);

    uint32_t state = GetCurrentState(context);
    std::string stateName = state != 0 ? GetStateName(state) : "?";
    float stateTime = state != 0 ? LoadFloat(state + STATE_TIME_OFFSET) : 0.0f;
    float deltaTime = std::max(float(App::s_deltaTime), 0.001f);

    // Over the hint rings: when each can be touched.
    CollisionOverlay::DrawHintRings(deltaTime);

    float indicatorWidth = (width - 2 * padding - 2 * gap) / 3;
    float column2 = left + indicatorWidth + gap;
    float column3 = column2 + indicatorWidth + gap;

    bool grounded = LoadU8(context + (werehog ? EVIL_GROUNDED_OFFSET : GROUNDED_OFFSET)) != 0;
    DrawIndicator(drawList, font, { left, row }, indicatorWidth, grounded ? "GROUND" : "AIR", grounded ? IM_COL32(80, 220, 80, 230) : IM_COL32(90, 170, 255, 230), true);

    std::string stateLabel = stateName;
    int stateFrames = int(std::lround(stateTime / deltaTime));

    if (werehog)
    {
        WerehogAttack attack = PredictWerehogAttack(context, state, stateName, stateTime, grounded, deltaTime);

        DrawIndicator(drawList, font, { column2, row }, indicatorWidth, "DASHING", IM_COL32(255, 200, 60, 230), attack.Dashing);
        DrawIndicator(drawList, font, { column3, row }, indicatorWidth, "ATTACKING", IM_COL32(255, 110, 80, 230), attack.Attacking);
        DrawIndicator(drawList, font, { left, pressRow }, indicatorWidth, "CAN GUARD", IM_COL32(80, 230, 80, 255), attack.CanGuard, true);

        if (attack.FramesLeft >= 0)
        {
            std::string text;
            if (!attack.NextAction.empty())
                text = fmt::format("{} in {}f", attack.NextAction, attack.FramesLeft);
            else if (attack.FramesLeftWithStick >= 0)
                text = fmt::format("free in {}f, {}f with the stick", attack.FramesLeft, attack.FramesLeftWithStick);
            else
                text = fmt::format("free in {}f", attack.FramesLeft);

            DrawShadowedText(drawList, font, Scale(11), { left, attackRow }, IM_COL32(255, 200, 60, 255), text);
        }

        // The Unleash gauge, filled by defeating enemies.
        auto evilSonicContext = reinterpret_cast<SWA::Player::CEvilSonicContext*>(g_memory.Translate(context));
        std::string gauge = fmt::format("Unleash {:.1f}", float(evilSonicContext->m_DarkGaiaEnergy));
        DrawShadowedText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), gauge), pressRow + Scale(2) },
            IM_COL32(220, 220, 220, 255), gauge);

        // The stage time, which the Werehog's stages don't show: as the day stages show it (truncated to
        // hundredths), and in frames.
        uint32_t document = LoadU32(GAME_DOCUMENT);
        uint32_t member = document != 0 ? LoadU32(document + GAME_DOCUMENT_MEMBER_OFFSET) : 0;
        if (member != 0)
        {
            float stageTime = LoadFloat(member + STAGE_TIME_OFFSET);
            int hundredths = int(std::abs(stageTime) * 100.0f);
            std::string text = fmt::format("TIME {}{:02}:{:02}:{:02}  {}f", stageTime < 0.0f ? "-" : "", hundredths / 6000,
                hundredths / 100 % 60, hundredths % 100, int(std::lround(stageTime * 60.0f)));
            DrawShadowedText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), text), panelMin.y + Scale(10) },
                IM_COL32(220, 220, 220, 255), text);
        }

        stateLabel = attack.StateLabel;
        stateFrames = attack.StateFrames;
    }
    else
    {
        SonicPresses presses = PredictSonicPresses(context, state, stateName, stateTime, grounded, deltaTime);

        DrawIndicator(drawList, font, { column2, row }, indicatorWidth, "SLIDING", IM_COL32(255, 200, 60, 230), presses.Sliding);
        DrawIndicator(drawList, font, { column3, row }, indicatorWidth, "STOMPING", IM_COL32(255, 110, 80, 230), presses.Stomping);

        // The movement mode, where the Werehog has the stage time. Diving (CSonicPostureDiving) keeps the table it
        // started with.
        auto movementMode = GetMovementMode(context);
        std::string mode = fmt::format("{} MODE", stateName.starts_with("Diving") ? "DIVING" : movementMode != nullptr ? movementMode->Name : "?");
        DrawShadowedText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), mode), panelMin.y + Scale(10) },
            IM_COL32(220, 220, 220, 255), mode);

        DrawIndicator(drawList, font, { left, pressRow }, indicatorWidth, "CAN JUMP", IM_COL32(80, 230, 80, 255), presses.CanJump, true);
        DrawIndicator(drawList, font, { column2, pressRow }, indicatorWidth, "CAN STOMP", IM_COL32(80, 230, 80, 255), presses.CanStomp, true);

        // Otherwise, how many frames later a press would do it.
        if (!presses.CanStomp && presses.StompUpdates > 1)
            DrawShadowedText(drawList, font, Scale(11), { column3, pressRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", presses.StompUpdates - 1));

        // During a quick step: how many frames later the next one can start.
        DrawIndicator(drawList, font, { left, quickStepRow }, 2 * indicatorWidth + gap, "CAN QUICKSTEP", IM_COL32(80, 230, 80, 255), presses.CanQuickStep, true);

        if (!presses.CanQuickStep && presses.QuickStepUpdates > 1)
            DrawShadowedText(drawList, font, Scale(11), { column3, quickStepRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", presses.QuickStepUpdates - 1));

        // The homing attack: an estimate in yellow.
        DrawIndicator(drawList, font, { left, homingRow }, 2 * indicatorWidth + gap, "CAN HOMING ATTACK",
            presses.HomingEstimated ? IM_COL32(255, 200, 60, 255) : IM_COL32(80, 230, 80, 255), presses.CanHomingAttack || presses.HomingEstimated, true);

        // Otherwise, how many frames later a press would do it, or that A would only dash forward, without a target.
        if (presses.HomingEstimated)
            DrawShadowedText(drawList, font, Scale(11), { column3, homingRow + Scale(2) }, IM_COL32(255, 200, 60, 255), "estimate");
        else if (!presses.CanHomingAttack && presses.HomingUpdates > 1 && presses.HomingAllowed)
            DrawShadowedText(drawList, font, Scale(11), { column3, homingRow + Scale(2) }, IM_COL32(255, 200, 60, 255), fmt::format("in {}f", presses.HomingUpdates - 1));
        else if (presses.HomingPress && !presses.HomingTarget && !GetSonicChecks().AirBoostHoming)
            DrawShadowedText(drawList, font, Scale(11), { column3, homingRow + Scale(2) }, IM_COL32(170, 170, 170, 255), "air dash");

        DrawIndicator(drawList, font, { left, boostRow }, indicatorWidth, "CAN BOOST", IM_COL32(80, 230, 80, 255), presses.CanBoost, true);
        DrawIndicator(drawList, font, { column2, boostRow }, indicatorWidth, "BOOSTING", IM_COL32(90, 170, 255, 230), presses.Boosting);

        std::string gauge = fmt::format("Boost {:.2f}", presses.Energy);
        DrawShadowedText(drawList, font, Scale(11), { panelMax.x - padding - TextWidth(font, Scale(11), gauge), boostRow + Scale(2) },
            IM_COL32(220, 220, 220, 255), gauge);

        // The radar, above the panel, while Sonic is alive (not in DrowningDead, NormalDamageDead, PressDead...).
        if (stateName.find("Dead") == std::string::npos)
        {
            float radarSize = std::min(width, panelMin.y - Scale(28));
            if (radarSize > Scale(80))
            {
                CollisionOverlay::DrawRadar(panelMax.x - radarSize, panelMin.y - Scale(8) - radarSize, radarSize, context, grounded,
                    stateName.starts_with("Diving"), GetHudFrame(), deltaTime);
            }
        }
    }

    DrawShadowedText(drawList, font, Scale(11), { left, stateRow }, IM_COL32(220, 220, 220, 255),
        fmt::format("{}  {}f", stateLabel, stateFrames));

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

    // M-Speed, D-Speed and the break-jump are Sonic's: the Werehog can't do them. M-Speed is for 3D, where the stick
    // goes through the curve, D-Speed for 2D, where its part along the path counts.
    std::string zoneText;
    bool mSpeedBlocked = false;
    if (!werehog)
    {
        DrawLengthZone(drawList, centre, radius, BREAK_JUMP_MIN_LENGTH, BREAK_JUMP_MAX_LENGTH,
            breakJump ? IM_COL32(255, 150, 30, 200) : IM_COL32(255, 150, 30, 70), breakJump ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 120));

        auto movementMode = GetMovementMode(context);
        bool sideView = movementMode != nullptr && movementMode->SideView;
        const char* zoneName = sideView ? "D-Speed" : "M-Speed";

        MSpeedRule rule = GetMSpeedRule(context, grounded);
        MSpeedInput input = sideView ? GetDSpeedState(rule, gameX, gameY) : GetMSpeedInput(rule, gameX, gameY);

        // In 2D air, the Brake state ran during this frame's update instead of the one D-Speed happens in.
        bool airBrake = sideView && !grounded && GetSonicChecks().Air2DBrakeFrame == GetHudFrame();
        if (airBrake)
        {
            input.Works = false;
            zoneText = input.InZone ? "D-Speed blocked (air brake)" : "Air brake";
            mSpeedBlocked = true;
        }
        else if (input.Works)
        {
            zoneText = zoneName;
        }
        else if (input.InZone)
        {
            zoneText = fmt::format("{} blocked", zoneName);
            mSpeedBlocked = true;
        }

        if (sideView)
            DrawDSpeedZone(drawList, centre, radius, rule, input);
        else
            DrawMSpeedZone(drawList, centre, radius, rule, input);
    }

    ImVec2 stick = { centre.x + x * radius, centre.y + y * radius };
    drawList->AddLine(centre, stick, IM_COL32(255, 60, 60, 255), Scale(2));
    drawList->AddCircleFilled(stick, Scale(4.5f), IM_COL32(255, 60, 60, 255));

    if (!werehog)
    {
        bool zone = !zoneText.empty();
        if (breakJump)
            zoneText += zoneText.empty() ? "Break-jump" : " + Break-jump";

        ImU32 zoneColour = mSpeedBlocked ? IM_COL32(255, 80, 80, 255) : zone ? IM_COL32(80, 255, 80, 255) :
            breakJump ? IM_COL32(255, 160, 40, 255) : IM_COL32(200, 200, 200, 255);
        DrawShadowedText(drawList, font, Scale(11), { left, centre.y + radius + Scale(6) }, zoneColour, zoneText.empty() ? "No zone" : zoneText);
    }

    // What to type into libTAS, and what the game made of it.
    DrawShadowedText(drawList, font, Scale(11), { left, centre.y + radius + Scale(20) }, IM_COL32_WHITE,
        fmt::format("X {:.2f} Y {:.2f}  ({}, {})", x, y, sdlX, sdlY));

    if (padState != nullptr)
    {
        DrawShadowedText(drawList, font, Scale(11), { left, centre.y + radius + Scale(34) }, IM_COL32(200, 200, 200, 255),
            fmt::format("Game {:.3f} {:.3f}  length {:.3f}", gameX, gameY, std::sqrt(gameLengthSquared)));
    }

    DrawController(drawList, font, { left, centre.y + radius + Scale(52) }, width - 2 * padding, padState);

    AdvanceHudFrame();
}
