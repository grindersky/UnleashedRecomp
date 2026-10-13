#include "collision_view_patches.h"
#include <api/SWA.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <user/config.h>
#include <sdl_listener.h>

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <utility>

// The collision views make what they draw as the stage loads, so they can't be switched by switching the codes. With
// any of them on, the stage loads both what they draw and the terrain the stage collision view leaves out, and F11
// leaves one or the other out while the game draws: the stage's terrain while the views are shown, what they draw
// while they aren't. Under libTAS, F11 is passed on as a key, so it goes into the movie like the game's keys.

// SWA::CTerrainManager2nd, which draws the stage's terrain (the Terrain block of Stage.set.xml with a TerrainInfoFile).
static constexpr uint32_t TERRAIN_MANAGER_2ND_VFTABLE = 0x820339F8;

// The literal "" the stage loader compares its strings with. A CSharedString holds literals from the image as they are,
// without counting references.
static constexpr uint32_t EMPTY_STRING = 0x8200A621;

// The shadow map's mode, the second of its parameters.
static constexpr uint32_t SHADOW_MAP_MODE = 0x83302768;

static std::atomic<bool> g_shown{ true };

bool CollisionView::IsEnabled()
{
    return Config::EnableStageCollisionDebugView || Config::EnableEventCollisionDebugView || Config::EnableObjectCollisionDebugView;
}

bool CollisionView::IsShown()
{
    return g_shown;
}

// The stage collision view's switch, set every frame from its code.
static bool IsStageViewOn()
{
    return SWA::SGlobals::ms_IsCollisionRender != nullptr && *SWA::SGlobals::ms_IsCollisionRender;
}

static class CollisionViewListener : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override
    {
        if (CollisionView::IsEnabled() && event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_F11 && !event->key.repeat)
            g_shown = !g_shown;

        return false;
    }
}
g_collisionViewListener;

// The renderables (Hedgehog::Mirage::CRenderable) the views draw, and the stage's terrain.
static std::mutex g_renderablesMutex;
static std::unordered_set<uint32_t> g_viewRenderables;
static std::unordered_set<uint32_t> g_terrainRenderables;

// While the views make their renderables.
static thread_local int g_makingViewRenderables;

// Makes the getter of ms_IsCollisionRender answer that the stage collision view is off: for a while, or once.
static thread_local bool g_stageViewHidden;
static thread_local bool g_stageViewHiddenOnce;

// The stage loader's terrain pass for a Terrain block that also holds the stage's collision.
static thread_local bool g_terrainPass;
static thread_local bool g_needsTerrainPass;

// SWA::SGlobals::ms_IsCollisionRender's getter.
PPC_FUNC_IMPL(__imp__sub_824F7098);
PPC_FUNC(sub_824F7098)
{
    __imp__sub_824F7098(ctx, base);

    if (g_stageViewHidden || std::exchange(g_stageViewHiddenOnce, false))
        ctx.r3.u64 = 0;
}

// The stage collision view (ms_IsCollisionRender) makes the stage loader (sub_82564100) skip the Terrain blocks of
// Stage.set.xml with a TerrainInfoFile, and load the blocks with only a RigidBodyContainer as collision it draws. A few
// stages, like Rooftop Run's day acts, have both in one block: load its collision like a collision-only block's, then
// load the block again for its terrain, without its collision. Load the other terrain blocks like without the view.
// Without the view, the loader runs unchanged.
bool StageCollisionDebugViewTerrainMidAsmHook(PPCRegister& r1)
{
    if (!IsStageViewOn())
        return false;

    // The block's RigidBodyContainer, read into a CSharedString on the stack.
    uint8_t* base = g_memory.base;
    uint32_t rigidBodyContainer = PPC_LOAD_U32(r1.u32 + 108);
    bool hasCollision = rigidBodyContainer != 0 && PPC_LOAD_U8(rigidBodyContainer) != '\0';

    if (hasCollision && !g_terrainPass)
    {
        g_needsTerrainPass = true;
        return true;
    }

    // The ms_IsCollisionRender check next decides whether to skip the terrain.
    g_stageViewHiddenOnce = true;
    return false;
}

// The stage loader's handler for a Terrain block of Stage.set.xml.
PPC_FUNC_IMPL(__imp__sub_82564100);
PPC_FUNC(sub_82564100)
{
    if (!IsStageViewOn())
    {
        __imp__sub_82564100(ctx, base);
        return;
    }

    PPCRegister arguments[] = { ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7 };

    g_needsTerrainPass = false;
    __imp__sub_82564100(ctx, base);
    g_stageViewHiddenOnce = false;

    if (!g_needsTerrainPass)
        return;

    g_needsTerrainPass = false;
    ctx.r3 = arguments[0];
    ctx.r4 = arguments[1];
    ctx.r5 = arguments[2];
    ctx.r6 = arguments[3];
    ctx.r7 = arguments[4];

    g_terrainPass = true;
    __imp__sub_82564100(ctx, base);
    g_terrainPass = false;
    g_stageViewHiddenOnce = false;
}

// Makes the loading of a Terrain block's RigidBodyContainer (the CSharedString at r4), in its folder (r5). In the terrain
// pass, the block's collision is loaded already: load it like a block without a RigidBodyContainer.
PPC_FUNC_IMPL(__imp__sub_8255F970);
PPC_FUNC(sub_8255F970)
{
    if (!g_terrainPass)
    {
        __imp__sub_8255F970(ctx, base);
        return;
    }

    uint32_t name = ctx.r4.u32;
    uint32_t rigidBodyContainer = PPC_LOAD_U32(name);
    PPC_STORE_U32(name, EMPTY_STRING);

    __imp__sub_8255F970(ctx, base);

    PPC_STORE_U32(name, rigidBodyContainer);
}

// SWA::CGameObjectTerrain's update (its vfunc 2), which makes the stage collision view's renderables, ten at a time.
PPC_FUNC_IMPL(__imp__sub_82518C50);
PPC_FUNC(sub_82518C50)
{
    g_makingViewRenderables++;
    __imp__sub_82518C50(ctx, base);
    g_makingViewRenderables--;
}

// SWA::CGameObjectCollisionRender's, for the object and event collision views.
PPC_FUNC_IMPL(__imp__sub_82490EB0);
PPC_FUNC(sub_82490EB0)
{
    g_makingViewRenderables++;
    __imp__sub_82490EB0(ctx, base);
    g_makingViewRenderables--;
}

// Makes a SWA::CHavokRenderable, a rigid body's shape drawn as a mesh, into the shared_ptr at r3. Not only for the views:
// SWA::CObjBendBox draws itself with one.
PPC_FUNC_IMPL(__imp__sub_822F3348);
PPC_FUNC(sub_822F3348)
{
    uint32_t renderable = ctx.r3.u32;
    __imp__sub_822F3348(ctx, base);

    if (g_makingViewRenderables > 0 && CollisionView::IsEnabled())
    {
        std::lock_guard lock(g_renderablesMutex);
        g_viewRenderables.insert(PPC_LOAD_U32(renderable));
    }
}

// Adds a game object's (r3) renderable (the shared_ptr at r5) to a render category (r4).
PPC_FUNC_IMPL(__imp__sub_82514360);
PPC_FUNC(sub_82514360)
{
    if (CollisionView::IsEnabled() && ctx.r3.u32 != 0 && PPC_LOAD_U32(ctx.r3.u32) == TERRAIN_MANAGER_2ND_VFTABLE)
    {
        std::lock_guard lock(g_renderablesMutex);
        g_terrainRenderables.insert(PPC_LOAD_U32(ctx.r5.u32));
    }

    __imp__sub_82514360(ctx, base);
}

static void Forget(uint32_t renderable)
{
    if (!CollisionView::IsEnabled())
        return;

    std::lock_guard lock(g_renderablesMutex);
    g_viewRenderables.erase(renderable);
    g_terrainRenderables.erase(renderable);
}

// Hedgehog::Mirage::CRenderable's constructor and destructor: forget the renderables that are gone, and the new ones
// made where they were.
PPC_FUNC_IMPL(__imp__sub_82E1A4D0);
PPC_FUNC(sub_82E1A4D0)
{
    Forget(ctx.r3.u32);
    __imp__sub_82E1A4D0(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82E1A4E8);
PPC_FUNC(sub_82E1A4E8)
{
    Forget(ctx.r3.u32);
    __imp__sub_82E1A4E8(ctx, base);
}

// Draws a renderable (r3) unless it's disabled (CRenderable::m_Enabled): Hedgehog::Mirage::CBundle draws its renderables
// with it.
PPC_FUNC_IMPL(__imp__sub_82E1A4F8);
PPC_FUNC(sub_82E1A4F8)
{
    if (CollisionView::IsEnabled())
    {
        std::lock_guard lock(g_renderablesMutex);
        auto& hidden = g_shown ? g_terrainRenderables : g_viewRenderables;
        if (hidden.contains(ctx.r3.u32))
            return;
    }

    __imp__sub_82E1A4F8(ctx, base);
}

// SWA::CFxShadowMap's vfuncs 1 and 2. The stage collision view sets the shadow map's mode to 0 in vfunc 1, every frame,
// where the stage's parameters set it, and both use it. Keep the stage's mode for the level as it normally looks, and
// give the shadow map 0 only while the view is shown.
PPC_FUNC_IMPL(__imp__sub_82BAE248);
PPC_FUNC(sub_82BAE248)
{
    if (!IsStageViewOn())
    {
        __imp__sub_82BAE248(ctx, base);
        return;
    }

    uint32_t mode = PPC_LOAD_U32(SHADOW_MAP_MODE);
    g_stageViewHidden = !g_shown;
    __imp__sub_82BAE248(ctx, base);
    g_stageViewHidden = false;
    PPC_STORE_U32(SHADOW_MAP_MODE, mode);
}

PPC_FUNC_IMPL(__imp__sub_82BADDF0);
PPC_FUNC(sub_82BADDF0)
{
    if (!IsStageViewOn() || !g_shown)
    {
        __imp__sub_82BADDF0(ctx, base);
        return;
    }

    uint32_t mode = PPC_LOAD_U32(SHADOW_MAP_MODE);
    PPC_STORE_U32(SHADOW_MAP_MODE, 0);
    __imp__sub_82BADDF0(ctx, base);
    PPC_STORE_U32(SHADOW_MAP_MODE, mode);
}
