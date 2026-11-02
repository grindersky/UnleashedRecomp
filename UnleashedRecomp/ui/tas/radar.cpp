// The TAS HUD's radar (CollisionOverlay::DrawRadar): day Sonic seen from above, with the stage's walls and floor edges
// around his height, the rings, enemies and goal ring around him, and in the air an estimate of where he reaches the
// next ring's height.

#include "collision.h"
#include "drawing.h"
#include "object_tracking.h"
#include "stage_collision.h"

#include <gpu/imgui/imgui_snapshot.h>
#include <sdl_listener.h>
#include <ui/collision_overlay.h>
#include <ui/tas_hud.h>

#include <algorithm>
#include <array>
#include <atomic>

using namespace Tas;

// How far the radar reaches from Sonic, to each side; Home goes to the next size (back to the first after the last), M
// hides and shows it. Under libTAS they reach the game as keys the game doesn't use, so they go into the movie like the
// game's keys.
static constexpr float RADAR_HALF_WIDTHS[] = { 4.0f, 12.0f, 30.0f, 60.0f };
static std::atomic<int> g_radarSize{ 0 };
static std::atomic<bool> g_radarShown{ true };

static class RadarListener : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override
    {
        if (event->type != SDL_KEYDOWN || event->key.repeat)
            return false;

        if (TasHud::IsEnabled() && event->key.keysym.sym == SDLK_HOME)
            g_radarSize = (g_radarSize + 1) % int(std::size(RADAR_HALF_WIDTHS));

        if (TasHud::IsEnabled() && event->key.keysym.sym == SDLK_m)
            g_radarShown = !g_radarShown;

        return false;
    }
}
g_radarListener;

static const ImU32 RADAR_RING_COLOUR = IM_COL32(255, 210, 40, 255);
static const ImU32 RADAR_TOUCH_COLOUR = IM_COL32(80, 255, 80, 255);
static const ImU32 RADAR_ITEM_COLOUR = IM_COL32(255, 80, 255, 230);

// What the radar keeps from the last frame it was drawn on, to tell how fast Sonic's fall speeds up.
static uint32_t g_radarContext;
static uint32_t g_radarFrame;
static float g_radarVelocityY;
static bool g_radarAirborne;

// How far a point is from a segment, seen from above.
static float HorizontalDistance(const Vector3& point, const Vector3& a, const Vector3& b)
{
    float abX = b.x - a.x;
    float abZ = b.z - a.z;
    float apX = point.x - a.x;
    float apZ = point.z - a.z;
    float lengthSquared = abX * abX + abZ * abZ;
    float t = lengthSquared > 0.0f ? std::clamp((apX * abX + apZ * abZ) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return std::hypot(apX - abX * t, apZ - abZ * t);
}

// A footprint's outline (filled if a fill is given), with the radar's mapping.
template<typename ToRadar>
static void DrawFootprint(ImDrawList* drawList, const ToRadar& toRadar, float scale, const Footprint& footprint, const Vector3& offset,
    ImU32 outline, ImU32 fill = 0)
{
    constexpr int HALF_SEGMENTS = 12;

    ImVec2 a = toRadar(footprint.A + offset);
    ImVec2 b = toRadar(footprint.B + offset);
    float radius = std::max(footprint.Radius * scale, Scale(1.5f));
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length = std::hypot(dx, dy);
    if (length < 0.5f)
    {
        ImVec2 centre = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
        if (fill != 0)
            drawList->AddCircleFilled(centre, radius, fill, 24);

        drawList->AddCircle(centre, radius, outline, 24, Scale(1.2f));
        return;
    }

    // Half a circle around each end, joined by the sides: convex.
    std::array<ImVec2, 2 * (HALF_SEGMENTS + 1)> points;
    float angle = std::atan2(dy, dx);
    for (int i = 0; i <= HALF_SEGMENTS; i++)
    {
        float around = angle - PI / 2 + PI * i / HALF_SEGMENTS;
        points[i] = { b.x + std::cos(around) * radius, b.y + std::sin(around) * radius };
        points[HALF_SEGMENTS + 1 + i] = { a.x + std::cos(around + PI) * radius, a.y + std::sin(around + PI) * radius };
    }

    if (fill != 0)
        drawList->AddConvexPolyFilled(points.data(), int(points.size()), fill);

    drawList->AddPolyline(points.data(), int(points.size()), outline, ImDrawFlags_Closed, Scale(1.2f));
}

void CollisionOverlay::DrawRadar(float x, float y, float size, uint32_t context, bool grounded, bool diving, uint32_t frame, float deltaTime)
{
    int radarSize = g_radarSize;
    if (!g_radarShown || radarSize >= int(std::size(RADAR_HALF_WIDTHS)))
        return;

    uint32_t node = LoadU32(context + MATRIX_NODE_OFFSET);
    if (!IsReadable(node))
        return;

    uint32_t matrix = node + MATRIX_NODE_MATRIX_OFFSET;
    Vector3 front = LoadVector(matrix + 32);
    Vector3 position = LoadVector(matrix + 48);
    Vector3 velocity = LoadVector(context + VELOCITY_OFFSET);
    if (!std::isfinite(Length(position)) || !std::isfinite(Length(velocity)))
        return;

    // How much Sonic's vertical speed changed during the last update: gravity, or 0 at the fall's top speed (like
    // diving's DivingDiveMaxVelocityDown). Never upwards: what pushed him up (a spring, say) isn't expected to go on.
    float fallStep = NAN;
    if (!grounded && g_radarAirborne && g_radarContext == context && g_radarFrame + 1 == frame)
        fallStep = std::min(velocity.y - g_radarVelocityY, 0.0f);

    g_radarContext = context;
    g_radarFrame = frame;
    g_radarVelocityY = velocity.y;
    g_radarAirborne = !grounded;

    // Turned like the camera: up on the radar is where the camera looks, along the ground (the 3D stick goes by the
    // camera too). The camera's right is the view matrix's first column. Without the camera, Sonic's facing is up.
    const Vector3 up{ 0.0f, 1.0f, 0.0f };
    Vector3 right{};
    if (uint32_t camera = GetCamera())
        right = { LoadFloat(camera + CAMERA_VIEW_OFFSET), 0.0f, LoadFloat(camera + CAMERA_VIEW_OFFSET + 32) };

    if (!(Length(right) > 0.1f))
    {
        Vector3 facing{ front.x, 0.0f, front.z };
        right = Length(facing) > 0.1f ? Cross(facing, up) : Vector3{ 1.0f, 0.0f, 0.0f };
    }

    right = Normalised(right);
    Vector3 forward = Cross(up, right);

    float halfWidth = RADAR_HALF_WIDTHS[radarSize];
    ImVec2 min = { x, y };
    ImVec2 max = { x + size, y + size };
    ImVec2 centre = { x + size / 2, y + size / 2 };
    float scale = size / 2 / halfWidth;
    auto toRadar = [&](const Vector3& p)
    {
        Vector3 d = p - position;
        return ImVec2{ centre.x + Dot(d, right) * scale, centre.y - Dot(d, forward) * scale };
    };

    auto isInside = [&](const Vector3& p, float margin)
    {
        Vector3 d = p - position;
        return std::abs(Dot(d, right)) < halfWidth + margin && std::abs(Dot(d, forward)) < halfWidth + margin;
    };

    // Sonic's collision as it is now: his body, and what collects rings (all but the body and the path collision).
    std::vector<Footprint> body;
    std::vector<Footprint> items;
    for (auto& [id, rigidBody] : GetPlayerBodies(context))
    {
        if (id != SONIC_PATH_COLLISION_ID)
            GetFootprints(GetBodyTransform(rigidBody), LoadU32(rigidBody + RIGID_BODY_SHAPE_OFFSET), id == BODY_COLLISION_ID ? body : items);
    }

    const std::vector<Footprint>& collectors = items.empty() ? body : items;

    // The heights are compared at the middle of the widest collector.
    Vector3 reference = position + up * 0.5f;
    float widest = 0.0f;
    for (auto& footprint : collectors)
    {
        if (footprint.Radius > widest)
        {
            widest = footprint.Radius;
            reference = footprint.Centre();
        }
    }

    // The rings, enemies and goal ring around him.
    struct RadarObject
    {
        Vector3 Centre;
        float Radius;
        ObjectKind Kind;
    };

    std::vector<RadarObject> objects;
    ForEachUnit([&](uint32_t unit, const TrackedUnit& tracked)
    {
        uint32_t unitVftable = LoadU32(unit);
        if (tracked.Kind == ObjectKind::None || tracked.Kind == ObjectKind::Terrain ||
            (unitVftable != EVENT_COLLISION_VFTABLE && unitVftable != RIGID_BODY_UNIT_VFTABLE))
            return;

        Transform transform;
        uint32_t shape;
        if (!GetCollidable(LoadU32(unit + PHYSICS_UNIT_BODY_OFFSET), transform, shape) || !isInside(transform.translation, 5.0f))
            return;

        std::vector<Footprint> footprints;
        GetFootprints(transform, shape, footprints);
        if (!footprints.empty())
            objects.push_back({ footprints[0].Centre(), footprints[0].Radius, tracked.Kind });
        else
            objects.push_back({ transform.translation, 0.3f, tracked.Kind });
    });

    // Where he'll be when he gets down to each ring's height on the radar: his velocity along the ground kept, his fall
    // sped up by the same amount every frame. Only an estimate: the stick and the game's own steering (diving's) change
    // it, and nothing in between (the ground, walls) is checked.
    struct Crossing
    {
        size_t Object;
        int Frames;
        Vector3 Offset;
        float Gap;
    };

    std::vector<Crossing> crossings;
    std::vector<Vector3> path;
    bool predicted = !grounded && std::isfinite(fallStep) && deltaTime > 0.0f;
    if (predicted)
    {
        float lowest = INFINITY;
        for (auto& object : objects)
        {
            if (object.Kind == ObjectKind::Ring && object.Centre.y < reference.y && isInside(object.Centre, 0.0f))
                lowest = std::min(lowest, object.Centre.y - reference.y);
        }

        std::vector<bool> crossed(objects.size());
        Vector3 offset{};
        float velocityY = velocity.y;
        path.push_back(offset);
        int frames = std::isfinite(lowest) ? 600 : 60;
        for (int f = 1; f <= frames; f++)
        {
            velocityY += fallStep;
            Vector3 next = offset + Vector3{ velocity.x, velocityY, velocity.z } * deltaTime;
            for (size_t i = 0; i < objects.size(); i++)
            {
                const RadarObject& object = objects[i];
                float height = object.Centre.y - reference.y;
                if (object.Kind != ObjectKind::Ring || crossed[i] || !isInside(object.Centre, 0.0f) || !(offset.y > height && next.y <= height))
                    continue;

                crossed[i] = true;
                Vector3 at = offset + (next - offset) * ((offset.y - height) / (offset.y - next.y));
                float gap = INFINITY;
                for (auto& footprint : collectors)
                    gap = std::min(gap, HorizontalDistance(object.Centre, footprint.A + at, footprint.B + at) - footprint.Radius - object.Radius);

                crossings.push_back({ i, f, at, gap });
            }

            offset = next;
            path.push_back(offset);
            if (offset.y < lowest - 1.0f || !isInside(position + offset, halfWidth))
                break;
        }
    }

    auto drawList = ImGui::GetBackgroundDrawList();
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");

    drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 140), Scale(6));
    drawList->PushClipRect(min, max, true);
    drawList->AddLine({ centre.x, min.y }, { centre.x, max.y }, IM_COL32(255, 255, 255, 40), Scale(1));
    drawList->AddLine({ min.x, centre.y }, { max.x, centre.y }, IM_COL32(255, 255, 255, 40), Scale(1));

    // The stage around his height: the floors' edges from 8 m below his feet to 1.5 m above (fainter the further down),
    // then the walls reaching between 1.5 m below his feet and 4 m above.
    UpdateTerrain();
    auto isNear = [&](const TerrainSegment& segment)
    {
        Vector3 a = segment.A - position;
        Vector3 b = segment.B - position;
        float ar = Dot(a, right), br = Dot(b, right), af = Dot(a, forward), bf = Dot(b, forward);
        return !((ar > halfWidth && br > halfWidth) || (ar < -halfWidth && br < -halfWidth) ||
            (af > halfWidth && bf > halfWidth) || (af < -halfWidth && bf < -halfWidth));
    };

    float feet = position.y;
    for (auto& ledge : GetTerrainLedges())
    {
        if (ledge.High < feet - 8.0f || ledge.Low > feet + 1.5f || !isNear(ledge))
            continue;

        float below = std::clamp((feet - ledge.High) / 8.0f, 0.0f, 1.0f);
        drawList->AddLine(toRadar(ledge.A), toRadar(ledge.B), IM_COL32(110, 170, 255, int(200 - 130 * below)), Scale(1));
    }

    for (auto& [address, terrainBody] : GetTerrainBodies())
    {
        for (auto& wall : terrainBody.Walls)
        {
            if (wall.High < feet - 1.5f || wall.Low > feet + 4.0f || !isNear(wall))
                continue;

            drawList->AddLine(toRadar(wall.A), toRadar(wall.B), IM_COL32(235, 235, 235, 210), Scale(1.5f));
        }
    }

    // The enemies and the goal ring, then the rings: green where the estimate has his collectors touch them, with the
    // height of their middle over his feet. Rings above each other (a vertical line of them) share one label: the
    // nearest one below him (or above, if none is), and how many there are.
    std::vector<bool> touched(objects.size());
    for (auto& crossing : crossings)
        touched[crossing.Object] = crossing.Gap <= 0.0f;

    for (auto& object : objects)
    {
        ImVec2 point = toRadar(object.Centre);
        float radius = std::max(object.Radius * scale, Scale(2.5f));
        if (object.Kind == ObjectKind::Enemy)
            drawList->AddRectFilled({ point.x - radius, point.y - radius }, { point.x + radius, point.y + radius }, ENEMY_COLOUR);
        else if (object.Kind == ObjectKind::GoalRing)
            drawList->AddCircle(point, radius, IM_COL32_WHITE, 24, Scale(2));
    }

    std::vector<bool> labelled(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
    {
        const RadarObject& object = objects[i];
        if (object.Kind != ObjectKind::Ring)
            continue;

        ImVec2 point = toRadar(object.Centre);
        float radius = std::max(object.Radius * scale, Scale(2.5f));
        ImU32 colour = touched[i] ? RADAR_TOUCH_COLOUR : RADAR_RING_COLOUR;
        drawList->AddCircleFilled(point, radius, (colour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 70), 24);
        drawList->AddCircle(point, radius, colour, 24, Scale(1.5f));

        if (labelled[i])
            continue;

        int count = 0;
        float shown = NAN;
        for (size_t k = i; k < objects.size(); k++)
        {
            const RadarObject& other = objects[k];
            if (other.Kind != ObjectKind::Ring || labelled[k] || HorizontalDistance(other.Centre, object.Centre, object.Centre) > 0.25f)
                continue;

            labelled[k] = true;
            count++;
            float height = other.Centre.y - position.y;
            bool below = height < 0.0f;
            if (std::isnan(shown) || (below && (shown >= 0.0f || height > shown)) || (!below && shown >= 0.0f && height < shown))
                shown = height;
        }

        std::string label = count > 1 ? fmt::format("{:+.1f} x{}", shown, count) : fmt::format("{:+.1f}", shown);
        DrawShadowedText(drawList, font, Scale(10), { point.x + radius + Scale(2), point.y - Scale(5) }, colour, label);
    }

    // The estimated path, and where he reaches the first ring's height, with his collectors drawn there.
    const Crossing* next = crossings.empty() ? nullptr : &crossings[0];
    if (path.size() >= 2)
    {
        std::vector<ImVec2> points;
        for (auto& offset : path)
            points.push_back(toRadar(position + offset));

        drawList->AddPolyline(points.data(), int(points.size()), IM_COL32(255, 255, 255, 150), 0, Scale(1.5f));
    }

    if (next != nullptr)
    {
        ImU32 colour = next->Gap <= 0.0f ? RADAR_TOUCH_COLOUR : IM_COL32(255, 160, 40, 255);
        for (auto& footprint : collectors)
            DrawFootprint(drawList, toRadar, scale, footprint, next->Offset, (colour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 140));

        ImVec2 point = toRadar(position + next->Offset);
        float arm = Scale(5);
        drawList->AddLine({ point.x - arm, point.y - arm }, { point.x + arm, point.y + arm }, colour, Scale(2));
        drawList->AddLine({ point.x - arm, point.y + arm }, { point.x + arm, point.y - arm }, colour, Scale(2));
    }

    // Sonic: what collects rings, his body, and his facing.
    for (auto& footprint : items)
        DrawFootprint(drawList, toRadar, scale, footprint, {}, RADAR_ITEM_COLOUR);

    for (auto& footprint : body)
        DrawFootprint(drawList, toRadar, scale, footprint, {}, BODY_COLOUR, (BODY_COLOUR & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 120));

    Vector3 facing{ front.x, 0.0f, front.z };
    if (Length(facing) > 0.2f)
    {
        ImVec2 tip = toRadar(position + Normalised(facing) * (Scale(18) / scale));
        drawList->AddLine(centre, tip, ARROW_COLOUR, Scale(2));
        drawList->AddCircleFilled(tip, Scale(2.5f), ARROW_COLOUR);
    }

    drawList->PopClipRect();

    // The title, the scale, and what the estimate says about the next ring.
    float padding = Scale(6);
    float textSize = Scale(10);
    DrawShadowedText(drawList, font, textSize, { min.x + padding, min.y + padding }, IM_COL32(220, 220, 220, 255), "TOP VIEW (M, Home)");

    float bar = 0.5f;
    for (float length : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f })
    {
        if (length <= halfWidth / 2)
            bar = length;
    }

    std::string barText = fmt::format("{:g} m", bar);
    float barRight = max.x - padding;
    float barY = min.y + padding + Scale(12);
    drawList->AddLine({ barRight - bar * scale, barY }, { barRight, barY }, IM_COL32_WHITE, Scale(2));
    drawList->AddLine({ barRight - bar * scale, barY - Scale(3) }, { barRight - bar * scale, barY + Scale(3) }, IM_COL32_WHITE, Scale(1.5f));
    drawList->AddLine({ barRight, barY - Scale(3) }, { barRight, barY + Scale(3) }, IM_COL32_WHITE, Scale(1.5f));
    DrawShadowedText(drawList, font, textSize, { barRight - font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, barText.c_str()).x, min.y + padding },
        IM_COL32(220, 220, 220, 255), barText);

    if (predicted)
    {
        std::string text = "No ring below on the radar";
        ImU32 colour = IM_COL32(200, 200, 200, 255);
        if (next != nullptr)
        {
            text = next->Gap <= 0.0f ? fmt::format("Ring height in {}f: TOUCH", next->Frames) :
                fmt::format("Ring height in {}f: {:.2f} m off", next->Frames, next->Gap);
            colour = next->Gap <= 0.0f ? RADAR_TOUCH_COLOUR : IM_COL32(255, 160, 40, 255);
        }

        DrawShadowedText(drawList, font, textSize, { min.x + padding, max.y - padding - Scale(24) }, colour, text);
        DrawShadowedText(drawList, font, textSize, { min.x + padding, max.y - padding - Scale(11) }, IM_COL32(170, 170, 170, 255),
            diving ? "Estimate: diving steers with the stick" : "Estimate: no stick, no walls");
    }
}
