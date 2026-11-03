// The TAS camera (see tas_camera.h). Everything here reads the game's memory only; what it changes is the host's copy of
// the shader constants on their way to the GPU, so the game and the movie's sync are untouched.

#include "tas_camera.h"
#include "game_addresses.h"
#include "guest_memory.h"
#include "object_tracking.h"
#include "vector_math.h"

#include <sdl_listener.h>
#include <ui/tas_hud.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

using namespace Tas;

// Chase: behind Sonic along his facing, above him, looking a bit ahead of him. Top-down: above him, his facing up.
static constexpr float CHASE_DISTANCE = 7.0f;
static constexpr float CHASE_HEIGHT = 3.0f;
static constexpr float CHASE_TARGET_HEIGHT = 1.0f;
static constexpr float CHASE_LOOK_AHEAD = 4.0f;
static constexpr float TOP_DOWN_HEIGHT = 40.0f;
static constexpr float TOP_DOWN_AHEAD = 10.0f;

static std::atomic<TasCamera::Mode> g_mode{ TasCamera::Mode::Off };

// "\" cycles the modes, with the TAS HUD on. Under libTAS it reaches the game as a key the game doesn't use, so it goes into the movie like
// the game's keys (as the HUD's keys do).
static class TasCameraListener : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override
    {
        if (!TasHud::IsEnabled() || event->type != SDL_KEYDOWN || event->key.repeat)
            return false;

        if (event->key.keysym.sym == SDLK_BACKSLASH || event->key.keysym.scancode == SDL_SCANCODE_BACKSLASH ||
            event->key.keysym.scancode == SDL_SCANCODE_NONUSBACKSLASH)
        {
            g_mode = TasCamera::Mode((int(g_mode.load()) + 1) % 3);
        }

        return false;
    }
}
g_tasCameraListener;

TasCamera::Mode TasCamera::GetMode()
{
    // Only with the TAS HUD on: without it, the camera is off (and "\" does nothing).
    return TasHud::IsEnabled() ? g_mode.load() : Mode::Off;
}

const char* TasCamera::GetModeName()
{
    switch (GetMode())
    {
    case Mode::Chase:
        return "TAS CAMERA: CHASE";
    case Mode::TopDown:
        return "TAS CAMERA: TOP-DOWN";
    default:
        return "";
    }
}

// What the game camera had after an update, and the TAS view made for it. The GPU upload can come a frame after the
// update, so the last few are kept and an upload is matched to the one it equals.
struct ViewRecord
{
    std::array<float, 16> GameView{};
    std::array<float, 3> GameEye{};
    std::array<float, 16> View{};
    std::array<float, 3> Eye{};
    bool Valid = false;
};

static std::mutex g_mutex;
static std::array<ViewRecord, 4> g_records;
static uint32_t g_nextRecord;

// A right-handed view for row vectors (position * view), as the game's: rows 0-2 hold the axes' x, y and z, row 3 the
// translation; the camera looks down -z.
static std::array<float, 16> MakeView(const Vector3& eye, const Vector3& x, const Vector3& y, const Vector3& z)
{
    return
    {
        x.x, y.x, z.x, 0.0f,
        x.y, y.y, z.y, 0.0f,
        x.z, y.z, z.z, 0.0f,
        -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1.0f
    };
}

static bool MakeTasView(std::array<float, 16>& view, Vector3& eye)
{
    bool werehog;
    uint32_t context = TasHud::GetPlayerContext(werehog);
    if (context == 0)
        return false;

    uint32_t node = LoadU32(context + MATRIX_NODE_OFFSET);
    if (node == 0)
        return false;

    Vector3 position = LoadVector(node + MATRIX_NODE_POSITION_OFFSET);
    Vector3 facing = LoadVector(node + MATRIX_NODE_FACING_OFFSET);

    // World up keeps the view level through loops and walls; the facing flattened onto the ground.
    Vector3 up = { 0.0f, 1.0f, 0.0f };
    Vector3 forward = facing - up * Dot(facing, up);
    if (Length(forward) < 0.01f)
        forward = { 0.0f, 0.0f, 1.0f };
    forward = Normalised(forward);

    if (TasCamera::GetMode() == TasCamera::Mode::Chase)
    {
        Vector3 target = position + up * CHASE_TARGET_HEIGHT + forward * CHASE_LOOK_AHEAD;
        eye = position - forward * CHASE_DISTANCE + up * CHASE_HEIGHT;
        Vector3 z = Normalised(eye - target);
        Vector3 x = Normalised(Cross(up, z));
        Vector3 y = Cross(z, x);
        view = MakeView(eye, x, y, z);
    }
    else
    {
        // Looking straight down (along -up), so z = up; the facing points to the top of the screen.
        eye = position + up * TOP_DOWN_HEIGHT + forward * TOP_DOWN_AHEAD;
        Vector3 z = up;
        Vector3 y = forward;
        Vector3 x = Cross(y, z);
        view = MakeView(eye, x, y, z);
    }

    for (float value : view)
    {
        if (!std::isfinite(value))
            return false;
    }

    return true;
}

// The record for the game camera's current view and eye, made (with the TAS view for this moment) when there's none yet.
// Call with g_mutex held.
static const ViewRecord* FindOrMakeRecord(uint32_t camera)
{
    if (camera == 0)
        return nullptr;

    std::array<float, 16> gameView;
    for (int i = 0; i < 16; i++)
        gameView[i] = LoadFloat(camera + CAMERA_VIEW_OFFSET + i * 4);

    for (auto& record : g_records)
    {
        if (record.Valid && record.GameView == gameView)
            return &record;
    }

    ViewRecord record;
    record.GameView = gameView;
    for (int i = 0; i < 3; i++)
        record.GameEye[i] = LoadFloat(camera + CAMERA_POSITION_OFFSET + i * 4);

    Vector3 eye;
    if (!MakeTasView(record.View, eye))
        return nullptr;

    record.Eye = { eye.x, eye.y, eye.z };
    record.Valid = true;

    auto& slot = g_records[g_nextRecord];
    g_nextRecord = (g_nextRecord + 1) % g_records.size();
    slot = record;
    return &slot;
}

void TasCamera::OnCameraUpdate(uint32_t camera)
{
    if (GetMode() == Mode::Off)
        return;

    std::lock_guard lock(g_mutex);
    FindOrMakeRecord(camera);
}

static float LoadBigEndian(const uint32_t* constants, int index)
{
    uint32_t value = __builtin_bswap32(constants[index]);
    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static void StoreBigEndian(uint32_t* constants, int index, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    constants[index] = __builtin_bswap32(bits);
}

static bool IsClose(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b));
}

// Replaces the N floats at constants with the TAS camera's, if they equal the game camera's (view or eye) in a record.
// The live camera gets a record first, so an upload of a view the last update did not record yet still matches.
template<size_t N>
static bool Replace(uint32_t* constants, std::array<float, N> ViewRecord::* game, std::array<float, N> ViewRecord::* tas)
{
    if (TasCamera::GetMode() == TasCamera::Mode::Off)
        return false;

    std::lock_guard lock(g_mutex);
    FindOrMakeRecord(GetCamera());

    for (auto& record : g_records)
    {
        if (!record.Valid)
            continue;

        bool match = true;
        for (size_t i = 0; i < N && match; i++)
            match = IsClose(LoadBigEndian(constants, int(i)), (record.*game)[i]);

        if (match)
        {
            for (size_t i = 0; i < N; i++)
                StoreBigEndian(constants, int(i), (record.*tas)[i]);
            return true;
        }
    }

    return false;
}

bool TasCamera::ReplaceView(uint32_t* constants)
{
    return Replace(constants, &ViewRecord::GameView, &ViewRecord::View);
}

bool TasCamera::ReplaceEye(uint32_t* constants)
{
    return Replace(constants, &ViewRecord::GameEye, &ViewRecord::Eye);
}

bool TasCamera::GetView(uint32_t camera, float view[16], float eye[3])
{
    if (GetMode() == Mode::Off)
        return false;

    std::lock_guard lock(g_mutex);
    auto record = FindOrMakeRecord(camera);
    if (record == nullptr)
        return false;

    memcpy(view, record->View.data(), sizeof(float) * 16);
    memcpy(eye, record->Eye.data(), sizeof(float) * 3);
    return true;
}
