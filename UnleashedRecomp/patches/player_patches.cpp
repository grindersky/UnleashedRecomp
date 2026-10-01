#include <api/SWA.h>
#include <ui/game_window.h>
#include <user/config.h>
#include <os/logger.h>
#include <app.h>
#include <sdl_events.h>
#include <ui/tas_hud.h>

static uint32_t g_lastEnemyScore;
static uint32_t g_lastTrickScore;
static float g_lastDarkGaiaEnergy;
static bool g_isUnleashCancelled;

/* Hook function for when checkpoints are activated
   to preserve the current checkpoint score. */
PPC_FUNC_IMPL(__imp__sub_82624308);
PPC_FUNC(sub_82624308)
{
    __imp__sub_82624308(ctx, base);

    if (!Config::SaveScoreAtCheckpoints)
        return;

    if (auto pGameDocument = SWA::CGameDocument::GetInstance())
    {
        g_lastEnemyScore = pGameDocument->m_pMember->m_ScoreInfo.EnemyScore;
        g_lastTrickScore = pGameDocument->m_pMember->m_ScoreInfo.TrickScore;

        LOGFN("Score: {}", g_lastEnemyScore + g_lastTrickScore);
    }
}

/* Hook function that resets the score
   and restore the last checkpoint score. */
PPC_FUNC_IMPL(__imp__sub_8245F048);
PPC_FUNC(sub_8245F048)
{
    __imp__sub_8245F048(ctx, base);

    if (!Config::SaveScoreAtCheckpoints)
        return;

    if (auto pGameDocument = SWA::CGameDocument::GetInstance())
    {
        LOGFN("Score: {}", g_lastEnemyScore + g_lastTrickScore);

        pGameDocument->m_pMember->m_ScoreInfo.EnemyScore = g_lastEnemyScore;
        pGameDocument->m_pMember->m_ScoreInfo.TrickScore = g_lastTrickScore;
    }
}

void ResetScoreOnRestartMidAsmHook()
{
    g_lastEnemyScore = 0;
    g_lastTrickScore = 0;
}

// Dark Gaia energy change hook.
PPC_FUNC_IMPL(__imp__sub_823AF7A8);
PPC_FUNC(sub_823AF7A8)
{
    auto pEvilSonicContext = (SWA::Player::CEvilSonicContext*)g_memory.Translate(ctx.r3.u32);

    g_lastDarkGaiaEnergy = pEvilSonicContext->m_DarkGaiaEnergy;

    // Don't drain energy if out of control.
    if (Config::FixUnleashOutOfControlDrain && pEvilSonicContext->m_OutOfControlCount && ctx.f1.f64 < 0.0)
        return;

    __imp__sub_823AF7A8(ctx, base);

    if (!Config::AllowCancellingUnleash)
        return;

    auto pInputState = SWA::CInputState::GetInstance();

    // Don't allow cancelling Unleash if the intro anim is still playing.
    if (!pInputState || pEvilSonicContext->m_AnimationID == 39)
        return;

    if (pInputState->GetPadState().IsTapped(SWA::eKeyState_RightBumper))
    {
        pEvilSonicContext->m_DarkGaiaEnergy = 0.0f;
        g_isUnleashCancelled = true;
    }
}

void PostUnleashMidAsmHook(PPCRegister& r30)
{
    if (!g_isUnleashCancelled)
        return;

    if (auto pEvilSonicContext = (SWA::Player::CEvilSonicContext*)g_memory.Translate(r30.u32))
        pEvilSonicContext->m_DarkGaiaEnergy = std::max(0.0f, g_lastDarkGaiaEnergy - 35.0f);

    g_isUnleashCancelled = false;
}

// SWA::Player::CEvilSonicContext
PPC_FUNC_IMPL(__imp__sub_823B49D8);
PPC_FUNC(sub_823B49D8)
{
    uint32_t context = ctx.r3.u32;
    __imp__sub_823B49D8(ctx, base);

    TasHud::OnEvilSonicContext(context);

    App::s_isWerehog = true;

    SDL_User_EvilSonic(true);
}

// ~SWA::Player::CEvilSonicContext
PPC_FUNC_IMPL(__imp__sub_823B4590);
PPC_FUNC(sub_823B4590)
{
    __imp__sub_823B4590(ctx, base);

    App::s_isWerehog = false;

    SDL_User_EvilSonic(false);
}

// SWA::Player::CPlayerSpeedContext::CStateBrake::LeaveState (also used by CSonicContext's). Entering the brake
// starts its "sonic_brake" effect; leaving it stops the effect through its particle instance (+240), which
// is only attached to the effect some time after it's created. Leaving the brake before that, as in a very
// short brake during a drift, dereferenced the null instance and crashed. Leave the effect running then.
PPC_FUNC_IMPL(__imp__sub_82334A50);
PPC_FUNC(sub_82334A50)
{
    uint32_t state = ctx.r3.u32;
    uint32_t effect = PPC_LOAD_U32(state + 0x6C);

    if (effect != 0 && PPC_LOAD_U32(effect + 240) == 0)
    {
        // Hide the effect from the original, which then skips stopping it, and give it back afterwards.
        PPC_STORE_U32(state + 0x6C, 0);
        __imp__sub_82334A50(ctx, base);
        PPC_STORE_U32(state + 0x6C, effect);
        return;
    }

    __imp__sub_82334A50(ctx, base);
}
