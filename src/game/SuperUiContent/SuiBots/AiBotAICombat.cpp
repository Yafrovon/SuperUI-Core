#include <mutex>
#include <set>
/*
 * AiBotAICombat.cpp — combat behaviour for the autonomous AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the combat domain:
 *   - mount / eat-drink / aggro-distance utility
 *   - the combat-ignore set + the [TEAMPLAY] assist-target seam (IsValidAssistTarget)
 *   - the stalemate breaker and overpull-retreat discipline
 *   - AttackStart / SelectAttackTarget / CheckForUnreachableTarget
 *   - the UpdateInCombatAI / UpdateOutOfCombatAI class dispatchers
 *   - all 18 per-class combat methods (verbatim from BattleBotAI)
 *
 * All members of AiBotAI, so they link across the sibling TUs transparently.
 * Cross-TU members called from here (StopMoving / ReGroundZ / FindNavBoundaryNear from
 * Movement, BridgeSendEvent from Bridge, CountNearbyHostiles from Grind) are defined in
 * those siblings.
 */

#include "AiBotAIMain.h"
#include "AiBotAITeamPlay.h"   // [TEAMPLAY] ResolveCombatTarget — the group focus-fire resolver
#include "AiBotCircuit.h"      // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "Player.h"
#include <cstring>
#include <cstdio>
#include "Group.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "World.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "BattleGround.h"
#include "TargetedMovementGenerator.h"
#include "QuestDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Server/Packets/Channel.h"
#include "ChannelMgr.h"
#include "Bag.h"
#include "PathFinder.h"
#include "MoveMap.h"
#include "DynamicObject.h"
#include "SuiRaidTelemetry.h"
#include "SuiRaidPlaybook.h"
#include "SuiAutopilot.h"
#include "Movement/WaypointManager.h"
#include "MoveSpline.h"

// BG flag auras (referenced in the combat methods below — never active in open world).
// File-local: these formerly lived in the AiBotSpells enum, but that enum moved to the shared
// header during the split, and BattleBotAI.h also defines these two (enum FlagSpellsWS) — so a
// TU including both headers (PlayerBotMgr.cpp) would conflict. This TU does NOT include
// BattleBotAI.h, so a file-local definition is safe and keeps the combat bodies byte-for-byte.
enum AiBotCombatFlagAuras
{
    AURA_WARSONG_FLAG    = 23333,
    AURA_SILVERWING_FLAG = 23335,
};

// ============================================================
// UTILITY METHODS (adapted from BattleBotAI — BG code removed)
// ============================================================



uint32 AiBotAI::GetMountSpellId() const
{
    if (me->GetLevel() >= 60)
    {   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_PALADIN)
            return AB_SPELL_MOUNT_60_PALADIN;   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_WARLOCK)
            return AB_SPELL_MOUNT_60_WARLOCK;   // cb:fold hot per-update detail

        switch (me->GetRace())
        {
            case RACE_HUMAN:    return AB_SPELL_MOUNT_60_HUMAN;   // cb:fold hot per-update detail
            case RACE_NIGHTELF: return AB_SPELL_MOUNT_60_NELF;   // cb:fold hot per-update detail
            case RACE_DWARF:    return AB_SPELL_MOUNT_60_DWARF;   // cb:fold hot per-update detail
            case RACE_GNOME:    return AB_SPELL_MOUNT_60_GNOME;   // cb:fold hot per-update detail
            case RACE_TROLL:    return AB_SPELL_MOUNT_60_TROLL;   // cb:fold hot per-update detail
            case RACE_ORC:      return AB_SPELL_MOUNT_60_ORC;   // cb:fold hot per-update detail
            case RACE_TAUREN:   return AB_SPELL_MOUNT_60_TAUREN;   // cb:fold hot per-update detail
            case RACE_UNDEAD:   return AB_SPELL_MOUNT_60_UNDEAD;   // cb:fold hot per-update detail
        }
    }
    else if (me->GetLevel() >= 40)
    {   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_PALADIN)
            return AB_SPELL_MOUNT_40_PALADIN;   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_WARLOCK)
            return AB_SPELL_MOUNT_40_WARLOCK;   // cb:fold hot per-update detail

        switch (me->GetRace())
        {
            case RACE_HUMAN:    return AB_SPELL_MOUNT_40_HUMAN;   // cb:fold hot per-update detail
            case RACE_NIGHTELF: return AB_SPELL_MOUNT_40_NELF;   // cb:fold hot per-update detail
            case RACE_DWARF:    return AB_SPELL_MOUNT_40_DWARF;   // cb:fold hot per-update detail
            case RACE_GNOME:    return AB_SPELL_MOUNT_40_GNOME;   // cb:fold hot per-update detail
            case RACE_TROLL:    return AB_SPELL_MOUNT_40_TROLL;   // cb:fold hot per-update detail
            case RACE_ORC:      return AB_SPELL_MOUNT_40_ORC;   // cb:fold hot per-update detail
            case RACE_TAUREN:   return AB_SPELL_MOUNT_40_TAUREN;   // cb:fold hot per-update detail
            case RACE_UNDEAD:   return AB_SPELL_MOUNT_40_UNDEAD;   // cb:fold hot per-update detail
        }
    }

    return 0;
}

bool AiBotAI::UseMount()
{
    if (me->IsMounted())
        return false;   // cb:fold hot per-update detail

    if (me->IsMoving())
        return false;   // cb:fold hot per-update detail

    if (me->GetDisplayId() != me->GetNativeDisplayId())
        return false;   // cb:fold hot per-update detail

    if (me->GetClass() == CLASS_ROGUE)
        return false;   // cb:fold hot per-update detail

    if (me->HasAura(SPELL_AURA_MOD_STEALTH))
        return false;   // cb:fold hot per-update detail

    uint32 spellId = GetMountSpellId();
    if (!spellId)
        return false;   // cb:fold hot per-update detail

    if (me->CastSpell(me, spellId, false) == SPELL_CAST_OK)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: mounted up", spellId);
        return true;
    }

    return false;
}

bool AiBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;   // cb:fold hot per-update detail

    if (me->IsMounted())
        return false;   // cb:fold hot per-update detail

    if (me->GetVictim())
        return false;   // cb:fold hot per-update detail

    bool const needToEat = me->GetHealthPercent() < 100.0f;
    bool const needToDrink = (me->GetPowerType() == POWER_MANA) && (me->GetPowerPercent(POWER_MANA) < 100.0f);

    if (!needToEat && !needToDrink)
        return false;   // cb:fold hot per-update detail

    bool const isEating = me->HasAura(AB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(AB_SPELL_DRINK);

    if (!isEating && needToEat)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: eating, hp below full", me->GetHealthPercent());
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {   // cb:fold hot per-update detail
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(AB_SPELL_FOOD))
        {   // cb:fold hot per-update detail
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(*pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: drinking, mana below full", me->GetPowerPercent(POWER_MANA));
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {   // cb:fold hot per-update detail
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(AB_SPELL_DRINK))
        {   // cb:fold hot per-update detail
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(*pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

float AiBotAI::GetMaxAggroDistanceForMap() const
{
    return 50.0f;
}


bool AiBotAI::IsCombatIgnored(uint32 guidLow) const
{
    return guidLow != 0 && m_combatIgnore.find(guidLow) != m_combatIgnore.end();
}

// [TEAMPLAY] Public seam for TeamPlay::ResolveCombatTarget — "is the anchor's victim something
// I may legally focus?" Wraps the (AI-internal) IsValidHostileTarget + an alive check, so the
// free-function resolver never reaches into protected combat internals.
static bool IsBanishedUnit(Unit const* u)
{
    for (auto const& entry : u->GetSpellAuraHolderMap())
        if (entry.second && entry.second->HasMechanic(MECHANIC_BANISH))
            return true;
    return false;
}

bool AiBotAI::IsValidAssistTarget(Unit* pTarget) const
{
    // [TACTICS] A banished enemy takes nothing; attacking it only breaks nothing and wastes time.
    if (pTarget && IsBanishedUnit(pTarget))
        return false;
    // [TACTICS] An enemy lying on the ground as dead (a feigned death) takes nothing from a hit:
    // attacks go to what is still standing.
    if (pTarget && (pTarget->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD) ||
        pTarget->GetStandState() == UNIT_STAND_STATE_DEAD))
        return false;
    return pTarget && pTarget->IsAlive() && IsValidHostileTarget(pTarget);
}

// ============================================================
// Combat stalemate breaker — in-combat with a victim neither side can damage
// (bot stranded on a navmesh seam / mob unreachable across geometry). Respects
// real fights: only no-damage-either-way for AIBOT_STALEMATE_NUDGE_MS trips it.
//
// Stage 1 nudges (short-ignore the mob so we don't re-chase mid-hop).
//
// Stage 2 (CHANGED 2026-06-22): the old version did CombatStop(true)+StopMoving
// IN PLACE on the premise "the unreachable mob then evades." That premise is false
// when the mob stays in aggro range: dropping OUR combat doesn't make an adjacent
// mob evade, so it re-aggros next tick and the lock reforms forever (Hu's metronomic
// combat_stalemate loop, hp=100% throughout). The 60s guid-ignore only stops US from
// re-SELECTING it — it can't stop the mob re-engaging US. So Stage 2 now EXTRACTS the
// bot: first a real ~30yd flee AWAY from the stuck mob (the proven HandleOverpullRetreat
// pattern) so the mob loses contact + leashes; after AIBOT_STALEMATE_MAX_DISENGAGES
// flees that still haven't broken it (bot on a no-navmesh tile — the water-spawn strand),
// a hard NearTeleportTo to a known-good anchor (nav-seam outer point, else spawn).
//
// NO-VICTIM FIX (2026-07-03): me->GetVictim()==nullptr with the combat flag still set
// (the ordinary ~5s tag-linger right after a kill, or any other transient gap) used to
// fall into the SAME no-damage-either-way path as a genuine deadlock — botHp unchanged
// (nobody's hitting us) read as "no damage moved," so a bot that just killed its last
// target and has nothin0g left to fight would accumulate stalemate time and nudge
// against guid 0 (confirmed live: Piv, 2026-07-03 — both observed nudges logged
// "vs guid 0"). There is nothing to be deadlocked AGAINST with no victim, so bail
// before the accumulator. This also stops those non-problems from burning the
// Stage-1/Stage-2 escalation budget a genuine stuck-mob deadlock will want.
//
// Returns true when it acted this tick (caller must skip combat AI / return).
// ============================================================
bool AiBotAI::HandleCombatStalemate()
{
    // [PULL] A proactive pull-and-retreat is deliberately no-damage while we close in and drag the
    // mob to open ground — that is NOT a deadlock. Don't accumulate stalemate time or nudge during
    // it (a nudge would fight the retreat). Keep m_lastHealth fresh so the detector compares against
    // current HP when the real fight resumes. HandlePullRetreat owns the pull sequence. (FINDING_005)
    if (m_pullActive)
    {   // cb:fold hot per-update detail
        m_stalemateMs = 0;
        m_lastHealth = me->GetHealth();
        return false;
    }
    // [TACTICS] A grouped bot inside an instance is never "stranded": a pause in the damage is a
    // tactic (waiting outside a boss's inferno, a rooted boss) - and the old nudge dropped the
    // boss, whose empty threat list then reset the fight (2026-09-25: Baron Geddon evaded at 40 %).
    if (me->GetGroup() && me->GetMap()->IsDungeon())
    {   // cb:fold hot per-update detail
        m_stalemateMs = 0;
        m_stalemateNudges = 0;
        m_stalemateVictim.Clear();
        m_lastHealth = me->GetHealth();
        return false;
    }

    if (!me->IsInCombat())
    {   // cb:fold hot per-update detail
        // Out of combat. If we're mid-disengage-hop, let it finish before wiping state —
        // a Stage-2 flee that briefly drops combat must NOT reset m_stalemateDisengages,
        // or a persistent re-aggro would flee 3× / reset / flee 3× forever and never reach
        // the teleport backstop. Once the hop is done and we're still out of combat, the
        // lock is genuinely broken → full reset.
        if (m_stalemateHoldMs > AIBOT_UPDATE_INTERVAL)
        {   // cb:fold hot per-update detail
            m_stalemateHoldMs -= AIBOT_UPDATE_INTERVAL;
            return true;
        }
        m_stalemateMs = 0;
        m_stalemateHoldMs = 0;
        m_stalemateNudges = 0;
        m_stalemateDisengages = 0;
        m_stalemateVictim.Clear();
        return false;
    }

    // Mid-nudge / mid-flee hold (in combat): let the hop run; don't let the combat AI
    // re-chase this tick.
    if (m_stalemateHoldMs > 0)
    {   // cb:fold hot per-update detail
        m_stalemateHoldMs = (m_stalemateHoldMs > AIBOT_UPDATE_INTERVAL)
                          ? m_stalemateHoldMs - AIBOT_UPDATE_INTERVAL : 0;
        return true;
    }

    Unit* pVictim = me->GetVictim();

    // NO-VICTIM FIX — see header comment. Nothing to be deadlocked against; don't
    // accumulate, don't nudge. m_lastHealth still tracks so a real fight starting next
    // tick compares against fresh HP instead of a stale reading from before the gap.
    if (!pVictim)
    {   // cb:fold hot per-update detail
        m_lastHealth = me->GetHealth();
        return false;
    }

    uint32 botHp = me->GetHealth();
    uint32 vicHp = pVictim->GetHealth();

    bool damageMoved = (botHp != m_lastHealth) ||
                       (m_lastVictimHealth != 0 && vicHp != m_lastVictimHealth);
    m_lastHealth = botHp;
    m_lastVictimHealth = vicHp;

    // Damage flowing either way → genuine fight. Respect it (and clear the disengage
    // counter — a real fight means we are NOT stalemate-locked).
    if (damageMoved)
    {   // cb:fold hot per-update detail
        m_stalemateMs = 0;
        m_stalemateNudges = 0;
        m_stalemateDisengages = 0;
        m_stalemateVictim.Clear();
        return false;
    }

    // No damage either way — anchor the stalemate target once, then accumulate.
    if (m_stalemateVictim.IsEmpty())
        m_stalemateVictim = pVictim->GetObjectGuid();   // cb:fold hot per-update detail

    m_stalemateMs += AIBOT_UPDATE_INTERVAL;
    if (m_stalemateMs < AIBOT_STALEMATE_NUDGE_MS)
        return false;   // cb:fold hot per-update detail
    m_stalemateMs = 0;

    uint32 stuckGuidLow = m_stalemateVictim.GetCounter();

    // ── Stage 2: nudges exhausted → break the lock for real (flee, then teleport) ──
    if (m_stalemateNudges >= AIBOT_STALEMATE_MAX_NUDGES)
    {   // cb:fold hot per-update detail
        if (stuckGuidLow)
            m_combatIgnore[stuckGuidLow] = AIBOT_STALEMATE_IGNORE_MS;   // cb:fold hot per-update detail

        me->AttackStop();
        me->CombatStop(true);   // clear OUR refs; the flee/teleport below takes us out of range
        m_stalemateNudges = 0;

        // Resolve the stuck mob's position so we flee directly AWAY from it. Fall back to
        // the failing task dest (the stuck geometry), then to a random direction.
        float awayX = 0.0f, awayY = 0.0f;
        bool haveAway = false;
        if (Creature* pStuck = me->GetMap()->GetCreature(m_stalemateVictim))
        {   // cb:fold hot per-update detail
            awayX = pStuck->GetPositionX();
            awayY = pStuck->GetPositionY();
            haveAway = true;
        }
        else if (m_currentTask.x != 0.0f || m_currentTask.y != 0.0f)
        {   // cb:fold hot per-update detail
            awayX = m_currentTask.x;
            awayY = m_currentTask.y;
            haveAway = true;
        }

        m_stalemateVictim.Clear();

        // ── Hard escape: repeated flees haven't broken it → teleport out ──
        // (Bot is stranded on a tile the navmesh can't path off of — the water-spawn /
        // off-mesh strand. Even the flee no_path's, so only a teleport ignores the mesh.)
        if (m_stalemateDisengages >= AIBOT_STALEMATE_MAX_DISENGAGES)
        {   // cb:fold hot per-update detail
            CB_HIT(me->GetGUIDLow(), "cpp-combat: stalemate flee failed, hard teleport escape");
            m_stalemateDisengages = 0;

            float tx = me->GetPositionX(), ty = me->GetPositionY(), tz = me->GetPositionZ();
            const char* how = "current pos (no anchor)";
            if (NavBoundary const* b = FindNavBoundaryNear(tx, ty, AIBOT_BOUNDARY_SCOPE))
            {   // cb:fold hot per-update detail
                tx = b->outerX; ty = b->outerY; tz = b->outerZ; how = "nav-seam outer anchor";
            }
            else if (m_spawnMapId == me->GetMapId())
            {   // cb:fold hot per-update detail
                tx = m_spawnX; ty = m_spawnY; tz = m_spawnZ; how = "spawn";
            }

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-STALEMATE] %s: flee did not break lock — HARD TELEPORT (%s) to (%.1f, %.1f, %.1f)",
                me->GetName(), how, tx, ty, tz);

            ReGroundZ(tx, ty, tz, "stalemate-tp");   // [GROUND]
            StopMoving();
            me->NearTeleportTo(tx, ty, tz, me->GetOrientation());

            std::string ev = "dest_x=" + std::to_string(m_currentTask.x) +
                             "|dest_y=" + std::to_string(m_currentTask.y) +
                             "|dest_z=" + std::to_string(m_currentTask.z) +
                             "|reason=combat_stalemate";
            BridgeSendEvent("MOVE_FAILED", ev.c_str());
            return true;
        }

        // ── Flee: hop a real distance AWAY from the stuck mob, then re-evaluate ──
        float fx = me->GetPositionX(), fy = me->GetPositionY(), fz = me->GetPositionZ();
        if (haveAway)
        {   // cb:fold hot per-update detail
            float ax = fx - awayX, ay = fy - awayY;
            float len = sqrtf(ax * ax + ay * ay);
            if (len > 0.5f)
            {   // cb:fold hot per-update detail
                fx += (ax / len) * AIBOT_STALEMATE_FLEE_DIST;
                fy += (ay / len) * AIBOT_STALEMATE_FLEE_DIST;
            }
        }
        me->GetRandomPoint(fx, fy, fz, AIBOT_STALEMATE_FLEE_DIST, fx, fy, fz);   // snap to valid terrain

        m_stalemateDisengages++;
        CB_HITV(me->GetGUIDLow(), "cpp-combat: stalemate disengage, fleeing stuck mob", m_stalemateDisengages);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-STALEMATE] %s: disengage %u/%u — fleeing %.0fyd from stuck guid %u to (%.1f, %.1f)",
            me->GetName(), m_stalemateDisengages, AIBOT_STALEMATE_MAX_DISENGAGES,
            AIBOT_STALEMATE_FLEE_DIST, stuckGuidLow, fx, fy);

        StopMoving();
        MovePointRun(AIBOT_POINT_STALEMATE_NUDGE, fx, fy, fz);
        m_stalemateHoldMs = AIBOT_STALEMATE_FLEE_HOLD_MS;

        // Tell C# the leg failed so it re-plans now instead of eating the 480s deadline.
        std::string ev = "dest_x=" + std::to_string(m_currentTask.x) +
                         "|dest_y=" + std::to_string(m_currentTask.y) +
                         "|dest_z=" + std::to_string(m_currentTask.z) +
                         "|reason=combat_stalemate";
        BridgeSendEvent("MOVE_FAILED", ev.c_str());
        return true;
    }

    // ── Stage 1: nudge (short-ignore the mob so the combat AI can't re-chase the hop) ──
    m_stalemateNudges++;
    CB_HITV(me->GetGUIDLow(), "cpp-combat: stalemate nudge hop", m_stalemateNudges);
    if (stuckGuidLow)
        m_combatIgnore[stuckGuidLow] = AIBOT_STALEMATE_NUDGE_HOLD_MS + 500;   // cb:fold hot per-update detail

    me->AttackStop(false);

    float nx = me->GetPositionX();
    float ny = me->GetPositionY();
    float nz = me->GetPositionZ();
    if (m_currentTask.x != 0.0f || m_currentTask.y != 0.0f)   // hop toward the journey dest if we have one
    {   // cb:fold hot per-update detail
        float dx = m_currentTask.x - nx;
        float dy = m_currentTask.y - ny;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.5f)
        {   // cb:fold hot per-update detail
            nx += (dx / len) * AIBOT_STALEMATE_NUDGE_DIST;
            ny += (dy / len) * AIBOT_STALEMATE_NUDGE_DIST;
        }
    }
    me->GetRandomPoint(nx, ny, nz, AIBOT_STALEMATE_NUDGE_DIST, nx, ny, nz);   // snap to valid terrain

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-STALEMATE] %s: nudge %u/%u vs guid %u — hopping to (%.1f, %.1f)",
        me->GetName(), m_stalemateNudges, AIBOT_STALEMATE_MAX_NUDGES, stuckGuidLow, nx, ny);

    StopMoving();
    MovePointRun(AIBOT_POINT_STALEMATE_NUDGE, nx, ny, nz);
    m_stalemateHoldMs = AIBOT_STALEMATE_NUDGE_HOLD_MS;
    return true;
}

// ============================================================
// [PULLGATE] PullReady — fight-initiation readiness (2026-07-05, solo discipline).
//
// The bot may DEFEND at any HP — this gates only the INITIATION of a new pull (the
// TASK_GRIND dispatch and the enriched-MOVE_TO approach scan in UpdateAI). Below the
// floor the caller latches m_eatRecoveryLatch and stands down; the OOC eat block then
// owns every tick until the latch releases at AIBOT_EAT_EXIT_HP/_MANA. Rage/energy
// classes gate on HP alone (rage starts empty by design; energy refills in seconds).
//
// Why 70: a pull initiated at 41-60% (where the old one-way eat gate parked the fleet)
// is a coin flip against an even-level mob and a guaranteed loss against one add. Every
// survival net downstream of this line fires AFTER the corpse; this is the first that
// fires before it.
// ============================================================
bool AiBotAI::PullReady() const
{
    if (me->GetHealthPercent() < AIBOT_PULL_MIN_HP)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: pull gated, hp below floor", me->GetHealthPercent());
        return false;
    }

    if (me->GetPowerType() == POWER_MANA &&
        me->GetPowerPercent(POWER_MANA) < AIBOT_PULL_MIN_MANA)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: pull gated, mana below floor", me->GetPowerPercent(POWER_MANA));
        return false;
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// AiBotAICombat.cpp — FUNCTION REPLACEMENT (2026-07-05)
//
// ONE function changes in this 1900-line TU: OverpullGuard.
//
// WHY: the old guard self-disabled when grouped ("let the pack absorb density") — written
// when every member pulled independently and a pre-pull cap on one bot meant nothing. Under
// the one-picker doctrine (AiBotDoctrineTeam.cpp, same date) the ANCHOR's pull is the TEAM's
// pull: followers never self-acquire, so the anchor diving a target buried in a dense cluster
// commits the whole trio — the 23-kobold Fargodeep camp dive from the 2026-07-04 death
// forensics. The anchor now respects the GROUP density cap (AIBOT_OVERPULL_GROUP, 6) before
// engaging; the in-combat HandleOverpullRetreat backstop is unchanged. Followers still hit
// this via HoldPull, harmlessly — they have no autonomous pulls left to veto.
// ════════════════════════════════════════════════════════════════════════════════════════

// [OVERPULL] Solo density cap scaled to survivability. AIBOT_OVERPULL_SOLO (3) is calibrated for
// a settled mid-level bot; a fresh L1 (~40 HP) dies to 2-3 even-con mobs long before the flee
// cycle recovers, and 3 attackers sits one BELOW the flat > cap trigger — so it never fled. Cap
// now scales with level, tightened further when already hurt. Grouped bots keep AIBOT_OVERPULL_GROUP.
uint32 AiBotAI::SoloOverpullCap() const
{
    uint32 const lvl = me->GetLevel();
    uint32 cap = (lvl < 5) ? 1u : (lvl < 10) ? 2u : (uint32)AIBOT_OVERPULL_SOLO;
    if (me->GetHealthPercent() < 35.0f && cap > 1u)
        --cap;   // already bloodied — bail sooner -- cb:fold hot per-update detail
    return cap;
}

bool AiBotAI::OverpullGuard(Unit* target) const
{
    if (!target)
        return false;   // cb:fold hot per-update detail

    // Neutral TARGET: attacking it doesn't proximity-aggro the field (no cascade), so the cluster
    // depth is irrelevant — it's always safe to pull. Only a reaction-hostile target risks the
    // bum-rush. (As with CountNearbyHostiles, same-faction social-assist is the lone residue, deferred.)
    if (!me->IsHostileTo(target))
        return false;   // cb:fold hot per-update detail

    // Count the OTHER will-aggro hostiles clustered with the target — that's what wakes when we
    // pull it. Solo cap for a solo bot; the GROUP cap for a grouped one (2026-07-05: no longer a
    // grouped no-op — under the one-picker doctrine the anchor's pull commits the whole team, so
    // the team-sized density cap must gate it. A camp denser than the cap = hold + patrol instead
    // of diving in; the C# grind-lock wall clock relocates a team parked against an un-pullable
    // field, so a hold here can never wedge the group forever).
    uint32 const cap = me->GetGroup() ? AIBOT_OVERPULL_GROUP : SoloOverpullCap();
    uint32 const packed = CountNearbyHostiles(target, AIBOT_PULL_DENSITY_RADIUS);
    if (packed > cap)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: overpull guard veto, camp too dense", packed);
        return true;
    }
    return false;
}

bool AiBotAI::HandleOverpullRetreat()
{
    if (!me->IsInCombat())
    {   // cb:fold hot per-update detail
        m_overpullFleeHoldMs = 0;
        m_overpullFlees = 0;
        return false;
    }
 
    // Mid-retreat hold: let the hop run; don't let the combat AI re-chase this tick.
    if (m_overpullFleeHoldMs > 0)
    {   // cb:fold hot per-update detail
        m_overpullFleeHoldMs = (m_overpullFleeHoldMs > AIBOT_UPDATE_INTERVAL)
                             ? m_overpullFleeHoldMs - AIBOT_UPDATE_INTERVAL : 0;
        return true;
    }
 
    uint32 const cap = me->GetGroup() ? AIBOT_OVERPULL_GROUP : SoloOverpullCap();
    auto const& attackers = me->GetAttackers();   // melee attacker set (Unit*)
    uint32 const count = (uint32)attackers.size();
 
    if (count <= cap)
    {   // cb:fold hot per-update detail
        m_overpullFlees = 0;   // recovered (mobs leashed / we whittled them down)
        return false;
    }
 
    // Already bailed the max number of times and still buried — stop thrashing; let the
    // combat AI fight it out (whether that's a death or a clutch kill is now the C# planner's
    // call via the death-loop / group escalation).
    if (m_overpullFlees >= AIBOT_OVERPULL_MAX_FLEES)
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: overpull flees exhausted, standing to fight");
        return false;
    }
 
    // Retreat away from the hostile centroid toward open ground; short-ignore the attackers
    // so Select*Target doesn't re-acquire them while we run.
    float cx = 0.0f, cy = 0.0f; uint32 n = 0;
    for (Unit* a : attackers)
    {
        if (!a)
            continue;   // cb:fold hot per-update detail
        cx += a->GetPositionX();
        cy += a->GetPositionY();
        ++n;
        m_combatIgnore[a->GetGUIDLow()] = AIBOT_OVERPULL_FLEE_HOLD_MS + 500;
    }
 
    float rx = me->GetPositionX(), ry = me->GetPositionY(), rz = me->GetPositionZ();
    if (n > 0)
    {   // cb:fold hot per-update detail
        cx /= n; cy /= n;
        float ax = rx - cx, ay = ry - cy;
        float len = sqrtf(ax * ax + ay * ay);
        if (len > 0.5f)
        {   // cb:fold hot per-update detail
            rx += (ax / len) * AIBOT_OVERPULL_RETREAT_DIST;
            ry += (ay / len) * AIBOT_OVERPULL_RETREAT_DIST;
        }
    }
    me->GetRandomPoint(rx, ry, rz, AIBOT_OVERPULL_RETREAT_DIST, rx, ry, rz);  // snap to valid terrain
 
    m_overpullFlees++;
    CB_HITV(me->GetGUIDLow(), "cpp-combat: overpull retreat, attackers over cap", count);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-OVERPULL] %s: %u attackers > cap %u (%s) — retreat %u/%u to (%.1f, %.1f)",
        me->GetName(), count, cap, me->GetGroup() ? "group" : "solo",
        m_overpullFlees, AIBOT_OVERPULL_MAX_FLEES, rx, ry);
 
    me->AttackStop();
    StopMoving();
    MovePointRun(AIBOT_POINT_OVERPULL_FLEE, rx, ry, rz);
    m_overpullFleeHoldMs = AIBOT_OVERPULL_FLEE_HOLD_MS;
    return true;
}

// ============================================================================
// [PULL] Proactive pull-and-retreat (FINDING_005, 2026-08-06)
//
// The old engage body-DIVED the camp: AttackStart -> MoveChase straight INTO the mob, so even a
// "least-clustered" pick woke its neighbours by proximity/social aggro, and only the REACTIVE
// HandleOverpullRetreat bailed — AFTER the overpull already happened (the 100-attacker Menethil
// pulls). BeginPull makes a fresh grind engagement a real PULL: tag the mob, then drag it back to
// an open-ground anchor so only it (+ tightly-linked neighbours) follows; fight it isolated.
// HandleOverpullRetreat stays as the in-combat backstop. Ranged tag at ~25y (their caster chase),
// melee body-tag at reach; both then retreat. Routed from the SHARED TASK_GRIND AttackStart, so
// the Solo scan AND the TeamAuto anchor both pull-and-retreat (followers hold → never self-pull).
// ============================================================================
bool AiBotAI::BeginPull(Unit* pTarget)
{
    if (!pTarget)
        return false;   // cb:fold hot per-update detail

    // Immobile targets (totems) can't be dragged — a pull-retreat just runs away from a mob that
    // won't follow, then comes back and re-tags (the StormSamurai "Strength of Earth Totem" loop).
    // Fight those in place; no pull. (FINDING_005)
    if (pTarget->IsCreature() && static_cast<Creature*>(pTarget)->IsTotem())
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: pull skipped, totem fought in place", pTarget->GetEntry());
        m_pullActive = false;
        return AttackStart(pTarget);
    }

    // Anchor = our current spot pushed AIBOT_PULL_RETREAT_DIST directly AWAY from the target
    // (open ground behind us), snapped to valid terrain. Computed BEFORE we close in.
    float rx = me->GetPositionX(), ry = me->GetPositionY(), rz = me->GetPositionZ();
    float ax = rx - pTarget->GetPositionX(), ay = ry - pTarget->GetPositionY();
    float len = sqrtf(ax * ax + ay * ay);
    if (len > 0.5f)
    {   // cb:fold hot per-update detail
        rx += (ax / len) * AIBOT_PULL_RETREAT_DIST;
        ry += (ay / len) * AIBOT_PULL_RETREAT_DIST;
    }
    me->GetRandomPoint(rx, ry, rz, AIBOT_PULL_RETREAT_DIST, rx, ry, rz);

    m_pullAnchorX = rx;
    m_pullAnchorY = ry;
    m_pullAnchorZ = rz;
    m_pullTargetGuid = pTarget->GetObjectGuid();
    m_pullTagged = false;
    m_pullRetreatHoldMs = 0;
    m_pullElapsedMs = 0;

    // Tag it. AttackStart drives the approach (melee -> reach; ranged -> ~25y caster chase) and
    // starts auto-attack/auto-shot; the mob aggroes when the first swing/shot lands, and
    // HandlePullRetreat then breaks off and drags it to the anchor. Only arm the pull if the
    // attack actually started (else leave m_pullActive false so nothing lingers).
    if (!AttackStart(pTarget))
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull aborted, attack start refused");
        m_pullActive = false;
        return false;
    }
    m_pullActive = true;
    CB_HITV(me->GetGUIDLow(), "cpp-combat: pull begun, tag then drag to anchor", pTarget->GetEntry());
    return true;
}

bool AiBotAI::HandlePullRetreat()
{
    if (!m_pullActive)
        return false;   // cb:fold hot per-update detail

    // Combat gone (mob dead / we died / it leashed) -> pull is over; normal flow resumes.
    if (!me->IsInCombat())
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull over, combat dropped");
        m_pullActive = false;
        return false;
    }

    // Safety timeout: never let the pull sequence run forever (e.g. an unreachable tag) -> drop to
    // normal combat, which owns its own stalemate/overpull handling.
    m_pullElapsedMs += AIBOT_UPDATE_INTERVAL;
    if (m_pullElapsedMs >= AIBOT_PULL_MAX_MS)
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull timeout, dropping to normal combat");
        m_pullActive = false;
        return false;
    }

    Creature* pTarget = me->GetMap()->GetCreature(m_pullTargetGuid);
    if (!pTarget || !pTarget->IsAlive())
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull target gone");
        m_pullActive = false;
        return false;
    }

    float const tagRange = (m_role == ROLE_RANGE_DPS && IsRangedDamageClass(me->GetClass()))
                         ? AIBOT_PULL_RANGED_TAG_RANGE : AIBOT_PULL_MELEE_TAG_RANGE;

    // Phase 1 -- APPROACH/TAG: let AttackStart's MoveChase close the gap; once we're within tag
    // range of our victim (swing/shot has connected -> mob aggroed), break off and retreat.
    if (!m_pullTagged)
    {   // cb:fold hot per-update detail
        bool const tagged = (me->GetVictim() == pTarget) &&
                            (me->GetCombatDistance(pTarget) <= tagRange);
        if (!tagged)
            return false;   // still closing; leave AttackStart's chase running this tick   // cb:fold hot per-update detail

        m_pullTagged = true;
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull tagged, dragging mob to anchor");
        me->AttackStop();
        StopMoving();
        MovePointRun(AIBOT_POINT_PULL_RETREAT, m_pullAnchorX, m_pullAnchorY, m_pullAnchorZ);
        m_pullRetreatHoldMs = AIBOT_PULL_RETREAT_HOLD_MS;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PULL] %s: tagged %s — dragging %.0fyd back to open ground",
            me->GetName(), pTarget->GetName(), AIBOT_PULL_RETREAT_DIST);
        return true;
    }

    // Phase 2 -- RETREAT: hold the hop. Stop early once we're back at the anchor with the mob in
    // tow, else run the hold out; then turn and fight the (now isolated) mob.
    bool const atAnchor = me->GetDistance2d(m_pullAnchorX, m_pullAnchorY) <= AIBOT_PULL_ARRIVE_RANGE;
    if (m_pullRetreatHoldMs > AIBOT_UPDATE_INTERVAL && !atAnchor)
    {   // cb:fold hot per-update detail
        m_pullRetreatHoldMs -= AIBOT_UPDATE_INTERVAL;
        return true;   // keep retreating
    }

    // Arrived (or hold expired): resume normal combat here, isolated from the camp.
    CB_HIT(me->GetGUIDLow(), "cpp-combat: pull complete, engaging isolated target");
    m_pullActive = false;
    AttackStart(pTarget);
    return true;
}

// Where a melee body stands on its victim: tanks in front, damage dealers behind or at the flank,
// both further out on a creature of long reach. Every melee chase goes through here.
// [AUTOPILOT] A pet waits at the camp with its owner while the leader brings a pull there: a hunter's cat sent
// out bit Baron Geddon at his walk point 25 yd from a Firewalker pack, which joined at +1 s (2026-11-15, R3W0 MC:
// 40 dead at 98 %); the owners were held at the camp, their pets were not.
bool AiBotAI::HoldPetAtCamp(Pet* pet)
{
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
    if (!pet || !pet->IsAlive() || !SuiTactic::On(me, SuiTactic::PetHoldAtCamp) || !SuiAutopilot::CampHold(me, hx, hy, hz))
        return false;
    if (pet->GetVictim() && pet->GetVictim()->GetDistance(hx, hy, hz) > 15.0f)
    {
        pet->AttackStop();
        if (pet->GetCharmInfo())
            pet->GetCharmInfo()->SetIsCommandAttack(false);
    }
    if (!pet->GetVictim() && pet->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        pet->GetMotionMaster()->MoveFollow(me, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
    return true;
}

static void ChaseInPlace(Player* me, Unit* pVictim, CombatBotRoles role)
{
    // [AUTOPILOT] The leader is bringing a timed pull back to the camp: wait there for it, do not run
    // out to it (2026-11-04: a Firewalker's aggro put the raid in combat and every tank charged to the
    // pull spot 13 yd from Baron Geddon's walk - he came by at +18 s).
    {
        float hx = 0.0f, hy = 0.0f, hz = 0.0f;
        if (SuiAutopilot::CampHold(me, hx, hy, hz))
        {
            if (me->GetDistance(hx, hy, hz) > 3.0f)
                me->GetMotionMaster()->MovePoint(0, hx, hy, hz, MOVE_PATHFINDING | MOVE_RUN_MODE);
            return;
        }
    }
    // Melee damage dealers go behind - unless the creature is a kind seen breathing a cone,
    // which sweeps its tail too: its flank then, behind the shoulder (2026-09-25, Onyxia:
    // Tail Sweep 106k).
    float angle = role == ROLE_MELEE_DPS ? 3.0f : 0.0f;
    float offset = 1.0f;
    if (role == ROLE_MELEE_DPS && pVictim->IsCreature())
    {
        if (SuiRaidTelemetry::EntryConeRadius(pVictim->GetEntry()) > 0.0f)
            angle = (me->GetGUIDLow() % 2) ? 1.83f : -1.83f;
        // A big creature reaches far: the melee stand well out from its centre, where a cleave
        // chaining from its tank (10 yd a jump) does not find them (2026-09-25: Onyxia, 23 yd
        // reach, 1.8 yd body - every rogue stood at 3 yd beside the tank and died in 10 s).
        float const reach = pVictim->GetCombatReach();
        if (reach >= 8.0f)
            offset = std::min(reach * 0.5f, 10.0f);
    }
    // Its tanks too stand out in front of it, together: inside a 2 yd body around a 23 yd reach
    // the tanks stood all round Onyxia and every swap of threat between them turned her whole
    // breath and tail across the raid (2026-09-25).
    if (role == ROLE_TANK && pVictim->IsCreature() && pVictim->GetCombatReach() >= 8.0f)
        offset = std::min(pVictim->GetCombatReach() * 0.35f, 7.0f);
    me->GetMotionMaster()->MoveChase(pVictim, offset, angle);
}

// A caster's chase to 25 yd, held at the camp like the melee one (ChaseInPlace).
static void RangedChaseInPlace(Player* me, Unit* pVictim)
{
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
    if (SuiAutopilot::CampHold(me, hx, hy, hz))
    {
        if (me->GetDistance(hx, hy, hz) > 3.0f)
            me->GetMotionMaster()->MovePoint(0, hx, hy, hz, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return;
    }
    // Out of a known cone's reach (+5 yd), within spell range: a Flameguard's 20 yd Cone of Fire reaches
    // 24 yd, and every turn of it moved the casters standing at 25 yd (2026-11-04: 418 cone steps, 44 %
    // of the casters' time moving, 925 damage a second from seventeen of them - the pack never died).
    float dist = 25.0f;
    if (pVictim->IsCreature())
        if (float const cone = SuiRaidTelemetry::EntryConeRadius(pVictim->GetEntry()))
            dist = std::min(29.0f, std::max(dist, cone + pVictim->GetObjectBoundingRadius() + 5.0f));
    me->GetMotionMaster()->MoveChase(pVictim, dist);
}

bool AiBotThreatHoldKeepsSwingOff(Player* me, Unit* victim);   // AiBotAIMain.cpp

bool AiBotAI::AttackStart(Unit* pVictim)
{
    // [SUI] Manual primary: only an explicit RTS ATTACK order (m_suiOrderedAttackPass) may start
    // a fight; assist, grind, defend and the doctrine all stand down.
    if (m_suiManual && !m_suiOrderedAttackPass)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat: attack start refused, manual primary");
        return false;
    }
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);   // cb:fold hot per-update detail

    // [TACTICS] A threat hold has stopped the swing on this victim: it stays off until the hold lifts.
    if (AiBotThreatHoldKeepsSwingOff(me, pVictim))
        return true;

    if (me->Attack(pVictim, true))
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: attack start, chasing victim", pVictim->GetEntry());
        if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
            IsRangedDamageClass(me->GetClass()) &&
            me->GetPowerPercent(POWER_MANA) > 10.0f &&
            me->GetCombatDistance(pVictim) > 8.0f)
            me->SetCasterChaseDistance(25.0f);   // cb:fold hot per-update detail
        else if (me->HasDistanceCasterMovement())   // cb:fold hot per-update detail
            me->SetCasterChaseDistance(0.0f);   // cb:fold hot per-update detail

        // A new target does not call off a step out of harm under way: the chase starts when the
        // step ends (2026-09-25: under Onyxia's Deep Breath the kill order switched whelps every
        // second, each switch turned the escape back into a chase - six stood and died in it).
        if (m_repositionUntilMs && WorldTimer::getMSTime() < m_repositionUntilMs && me->IsMoving())
            return true;
        ChaseInPlace(me, pVictim, m_role);
        return true;
    }

    return false;
}

Unit* AiBotAI::SelectAttackTarget(Unit* pExcept) const
{
    // [DOCTRINE] SelectAttackTarget is now the PURE solo re-pick. The group focus-fire override
    // that used to prefix it here moved into the TeamAuto doctrine's MaintainTarget
    // (AiBotDoctrineTeam.cpp) — the single in-combat target authority — so this method runs only
    // as the Solo / defer fallback. The pExcept contract (never return the unit the caller is
    // switching away from) is unchanged for that solo path.

    // 1. Check units we are currently in combat with.

    std::list<Unit*> targets;
    HostileReference* pReference = me->GetHostileRefManager().getFirst();

    while (pReference)
    {
        if (Unit* pTarget = pReference->getSourceUnit())
        {   // cb:fold hot per-update detail
            if (pTarget != pExcept &&
                IsValidHostileTarget(pTarget) &&
                !IsCombatIgnored(pTarget->GetGUIDLow()) &&
                me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            {   // cb:fold hot per-update detail
                targets.push_back(pTarget);
            }
        }
        pReference = pReference->next();
    }

    if (!targets.empty())
    {   // cb:fold hot per-update detail
        targets.sort([this](Unit* pUnit1, const Unit* pUnit2)
        {
            return me->GetDistance(pUnit1) < me->GetDistance(pUnit2);
        });

        CB_HITV(me->GetGUIDLow(), "cpp-combat: target acquired, nearest combat ref", (*targets.begin())->GetEntry());
        return *targets.begin();
    }

    // 2. Find nearby hostile creatures (mobs that aggro us in the open world).

    float const maxAggroDistance = GetMaxAggroDistanceForMap();

    // 3. Check party attackers.

    if (Group* pGroup = me->GetGroup())
    {   // cb:fold hot per-update detail
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {   // cb:fold hot per-update detail
                if (pMember == me)
                    continue;   // cb:fold hot per-update detail

                if (me->GetDistance(pMember) > 30.0f)
                    continue;   // cb:fold hot per-update detail

                if (Unit* pAttacker = pMember->GetAttackerForHelper())
                {   // cb:fold hot per-update detail
                    if (pAttacker != pExcept &&
                        IsValidHostileTarget(pAttacker) &&
                        !IsCombatIgnored(pAttacker->GetGUIDLow()) &&
                        me->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        me->GetDistanceZ(pAttacker) < 10.0f &&
                        me->IsWithinLOSInMap(pAttacker))
                    {   // cb:fold hot per-update detail
                        CB_HITV(me->GetGUIDLow(), "cpp-combat: target acquired, assisting group member", pAttacker->GetEntry());
                        return pAttacker;
                    }
                }
            }
        }
    }

    return nullptr;
}

bool AiBotAI::CheckForUnreachableTarget()
{
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
       !me->GetMotionMaster()->GetCurrent()->IsReachable())
    {   // cb:fold hot per-update detail
        if (Unit* pTarget = static_cast<ChaseMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent())->GetTarget())
        {   // cb:fold hot per-update detail
            if (!me->CanReachWithMeleeAutoAttack(pTarget))
            {   // cb:fold hot per-update detail
                if (!me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
                {   // cb:fold hot per-update detail
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target dropped, out of visibility");
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }

                // [REMESH] If WE are the unreachable one (off the mesh), walk back on before any cheat.
                if (!me->IsMoving() && TryRemeshStep("unreachable-target"))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target, bot was off-mesh, remesh step issued");
                    return true;
                }

                if (pTarget->IsCreature() && !me->IsMoving())
                {   // cb:fold hot per-update detail
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target, teleporting onto mob");
                    // Cheating to prevent getting stuck because of bad mmaps.
                    // [GROUND] Teleport onto the mob, grounded. A ground-spawned mob is a
                    // no-op; this only bites if the chase target itself sits on a float.
                    // Switched from NearTeleportTo(GetPosition()) to the 4-arg form so the
                    // bot keeps its own facing (irrelevant for melee — combat sets facing).
                    float tx = pTarget->GetPositionX();
                    float ty = pTarget->GetPositionY();
                    float tz = pTarget->GetPositionZ();
                    ReGroundZ(tx, ty, tz, "unreach-tp");
                    me->NearTeleportTo(tx, ty, tz, me->GetOrientation());
                    return true;
                }

                if (me->GetDistanceZ(pTarget) > 10.0f)
                {   // cb:fold hot per-update detail
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target above or below, attack stopped");
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }
            }
        }
    }

    return false;
}

// ============================================================
// 18 COMBAT METHODS + DISPATCHERS (verbatim from BattleBotAI,
// class name replaced: BattleBotAI -> AiBotAI)
// ============================================================

void AiBotAI::UpdateOutOfCombatAI()
{
    // People, not animals, between fights (owner 2026-08-25): combat forms
    // drop once the fight has been over for a grace period, so the fleet
    // reads as characters while questing/idle. Travel/Aquatic forms are
    // journey tools and stay; the next fight re-shifts per role/spec.
    if (me->GetClass() == CLASS_DRUID)
    {   // cb:fold hot per-update detail
        ShapeshiftForm const form = me->GetShapeshiftForm();
        if ((form == FORM_BEAR || form == FORM_DIREBEAR || form == FORM_CAT ||
             form == FORM_MOONKIN) &&
            WorldTimer::getMSTimeDiff(m_lastCombatMs, WorldTimer::getMSTime()) > 8000)
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);   // cb:fold hot per-update detail
    }

    // Raid practice: the tanks are Fear Warded before the pull, not after the first fear (it lasts 10 min, so a ward put
    // on at the pull still holds when Onyxia lands at ~3.5 min - 2026-11-15, R3W0 Onyxia: two phase-3 wipes began with a
    // feared tank and one Flame Breath over the casters).
    if (me->GetClass() == CLASS_PRIEST && SuiTactic::On(me, SuiTactic::PrePullFearWard) && TrySpecFearWardTank())
        return;

    // A validated persisted profile owns its deterministic OOC maintenance.
    // Unknown/conflict/invalid/error profiles intentionally fall through to
    // the proven legacy class behavior below.
    if (UpdateSpecOutOfCombatAI())
        return;   // cb:fold hot per-update detail

    switch (me->GetClass())
    {
        case CLASS_PALADIN:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

void AiBotAI::UpdateInCombatAI()
{
    m_lastCombatMs = WorldTimer::getMSTime();
    // [ROTATION] A loaded slate OWNS in-combat casting — the class switch below is the
    // vanilla else-branch (RotationSlate design, 2026-05-11). The 250ms sub-tick in
    // UpdateAI is the main driver; this 1s call is just one more evaluation, so a bot
    // whose sub-tick guards skipped (e.g. mid-cast) still gets the behaviour-tick try.
    if (!m_rotation.empty())
    {   // cb:fold hot per-update detail
        UpdateRotationSlate();
        if (me->GetVictim())
            UseTrinketEffects();   // cb:fold hot per-update detail
        return;
    }

    if (UpdateSpecCombatAI())
    {   // cb:fold hot per-update detail
        if (me->GetVictim())
            UseTrinketEffects();   // cb:fold hot per-update detail
        return;
    }

    switch (me->GetClass())
    {
        case CLASS_PALADIN:   // cb:fold hot per-update detail
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:   // cb:fold hot per-update detail
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:   // cb:fold hot per-update detail
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:   // cb:fold hot per-update detail
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:   // cb:fold hot per-update detail
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:   // cb:fold hot per-update detail
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:   // cb:fold hot per-update detail
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:   // cb:fold hot per-update detail
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:   // cb:fold hot per-update detail
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();   // cb:fold hot per-update detail
}

// ============================================================================
// [ROTATION] The slate evaluator (2026-07-16; RotationSlate design 2026-05-11).
//
// Walk the priority-sorted instructions; the FIRST one whose target resolves,
// whose HP window and aura condition pass, and that CanTryToCastSpell accepts,
// is cast — first match wins the tick, exactly the design. Called from the
// 250ms sub-tick in UpdateAI and from UpdateInCombatAI; both gate on IsAlive/
// InCombat/not-casting before reaching here, so this stays lean for 4 Hz.
//
// Notes locked at build time:
//  - pSpell was resolved at LOAD (unknown/unlearned spells are null → skipped);
//  - CanTryToCastSpell owns GCD / cooldown / power / range; DoCastSpell owns
//    stopping to cast — the same primitives every class rotation trusts;
//  - an active autorepeat of the SAME spell is skipped (don't restart the wand
//    every sub-tick), but a DIFFERENT castable instruction interrupts wanding
//    naturally via DoCastSpell — the wand-trap cure;
//  - returns true whenever a slate is present: slate present == combat handled,
//    even on a tick where nothing was castable (the vanilla switch must not run).
// ============================================================================
bool AiBotAI::UpdateRotationSlate()
{
    if (m_rotation.empty())
        return false;   // cb:fold rotation rung, outcome probed at cast

    for (auto const& inst : m_rotation)
    {
        SpellEntry const* pSpell = inst.pSpell;
        // Defense in depth: a talent reset or any later spell loss must never
        // leave a cached slate pointer capable of casting an unlearned spell.
        if (!pSpell || !me->HasSpell(inst.spellId))
            continue;   // cb:fold rotation rung, outcome probed at cast

        // Already wanding/shooting this exact spell — let the autorepeat run.
        if (Spell* pAuto = me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            if (pAuto->m_spellInfo == pSpell)   // cb:fold rotation rung, outcome probed at cast
                continue;   // cb:fold rotation rung, outcome probed at cast

        Unit* pTarget = ResolveRotationTarget(inst.target);
        if (!pTarget || !pTarget->IsAlive())
            continue;   // cb:fold rotation rung, outcome probed at cast

        float const hp = pTarget->GetHealthPercent();
        if (hp < (float)inst.hpMin || hp > (float)inst.hpMax)
            continue;   // cb:fold rotation rung, outcome probed at cast

        if (inst.auraId)
        {   // cb:fold rotation rung, outcome probed at cast
            bool const has = pTarget->HasAura(inst.auraId);
            if (has != inst.auraPresent)
                continue;   // cb:fold rotation rung, outcome probed at cast
        }

        if (!CanTryToCastSpell(pTarget, pSpell))
            continue;   // cb:fold rotation rung, outcome probed at cast

        if (DoCastSpell(pTarget, pSpell) == SPELL_CAST_OK)
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HITV(me->GetGUIDLow(), "cpp-combat: rotation slate winner cast", inst.spellId);
            return true;
        }
    }

    return true;   // slate present — combat handled even when nothing fired this tick
}

// [ROTATION] Target kinds the slate can name. 0=SELF, 1=CURRENT_TARGET (the spine/
// doctrine-owned victim — the slate never picks fights, it only executes on them),
// 2=LOWEST_HP_PARTY (lowest-HP% living group player within 40yd, self included; solo
// bots resolve to self, and the instruction's own hpMax window gates whether a heal
// actually fires). Unknown kinds resolve null and the instruction is skipped.
Unit* AiBotAI::ResolveRotationTarget(uint8 kind)
{
    switch (kind)
    {
        case 0:   // cb:fold hot per-update detail
            return me;
        case 1:   // cb:fold hot per-update detail
            return me->GetVictim();
        case 2:   // cb:fold hot per-update detail
        {
            Group* pGroup = me->GetGroup();
            if (!pGroup)
                return me;   // cb:fold hot per-update detail
            Unit* pBest = nullptr;
            float bestHp = 101.0f;
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* pMember = itr->getSource();
                if (!pMember || !pMember->IsAlive() || !pMember->IsInWorld())
                    continue;   // cb:fold hot per-update detail
                if (pMember->GetMapId() != me->GetMapId())
                    continue;   // cb:fold hot per-update detail
                if (pMember != me && !me->IsWithinDist(pMember, 40.0f))
                    continue;   // cb:fold hot per-update detail
                float const hp = pMember->GetHealthPercent();
                if (hp < bestHp)
                {   // cb:fold hot per-update detail
                    bestHp = hp;
                    pBest = pMember;
                }
            }
            return pBest ? pBest : me;
        }
    }
    return nullptr;
}

void AiBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.paladin.pBlessingBuff)
    {   // cb:fold rotation rung, outcome probed at cast
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {   // cb:fold rotation rung, outcome probed at cast
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {   // cb:fold rotation rung, outcome probed at cast
                    m_isBuffing = true;
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    FindAndHealInjuredAlly();
}

void AiBotAI::UpdateInCombatAI_Paladin()
{
    // (never a tank: a boss leaves the shield for the healers - 2026-11-15, R3W2 Onyxia; see UpdateSpecCombatPaladin)
    if (m_spells.paladin.pDivineShield &&
        !(GetCombatActiveRole() == ROLE_TANK && SuiTactic::OnAnyAttacker(me, SuiTactic::NoBubbleUnderBoss)) &&
       (me->GetHealthPercent() < 20.0f) &&
       (me->GetPowerPercent(POWER_MANA) > 40.0f) &&
       !me->HasAura(AURA_WARSONG_FLAG) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

    if (!hasSeal &&
        m_spells.paladin.pSeal &&
        CanTryToCastSpell(me, m_spells.paladin.pSeal))
    {   // cb:fold rotation rung, outcome probed at cast
        me->CastSpell(me, m_spells.paladin.pSeal, false);
    }

    // Holy Shock is a triage spell first.  The legacy ordering tried offense
    // before it ever looked for an injured ally.
    if (m_spells.paladin.pHolyShock)
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* pHeal = SelectHealTarget(65.0f, 65.0f))
        {   // cb:fold rotation rung, outcome probed at cast
            if (CanTryToCastSpell(pHeal, m_spells.paladin.pHolyShock) &&
                DoCastSpell(pHeal, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (hasSeal && m_spells.paladin.pJudgement &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHammerOfJustice &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHammerOfWrath &&
            pVictim->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield) &&
           (IsMeleeDamageClass(pVictim->GetClass()) || (GetAttackersInRangeCount(8.0f) > 1)))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pConsecration &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.paladin.pConsecration))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHolyShock &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pExorcism &&
            pVictim->IsCreature() &&
           (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
           !me->CanReachWithMeleeAutoAttack(pVictim))
        {   // cb:fold rotation rung, outcome probed at cast
            ChaseInPlace(me, pVictim, m_role);
        }
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
           !pFriend->HasAura(AURA_WARSONG_FLAG) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
            pFriend->HasAura(AURA_WARSONG_FLAG) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (m_spells.paladin.pBlessingOfFreedom &&
       (me->HasUnitState(UNIT_STATE_ROOT) || me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
        CanTryToCastSpell(me, m_spells.paladin.pBlessingOfFreedom))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.paladin.pCleanse)
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
        {   // cb:fold rotation rung, outcome probed at cast
            if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pFriend, m_spells.paladin.pCleanse) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
    }

    FindAndHealInjuredAlly(me->IsTotalImmune() ? 80.0f : 40.0f, 50.0f);
}

void AiBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (SummonShamanTotems())
            return;   // cb:fold rotation rung, outcome probed at cast

        UpdateInCombatAI_Shaman();
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.shaman.pGhostWolf &&
           !me->IsMoving() && !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.shaman.pGhostWolf))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.shaman.pGhostWolf) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pGhostWolf &&
        me->GetShapeshiftForm() == FORM_GHOSTWOLF)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat: ghost wolf dropped for combat");
        me->RemoveAurasDueToSpellByCancel(m_spells.shaman.pGhostWolf->Id);
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.shaman.pManaTideTotem &&
           (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
            CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pElementalMastery &&
            me->GetAttackers().empty() &&
            CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pEarthShock &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pFrostShock &&
            pVictim->IsMoving() &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pStormstrike &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pChainLightning &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pPurge &&
            IsValidDispelTarget(pVictim, m_spells.shaman.pPurge) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pPurge))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pFlameShock &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pLightningBolt &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (SummonShamanTotems())
        return;   // cb:fold rotation rung, outcome probed at cast

    if (m_spells.shaman.pCureDisease &&
        CanTryToCastSpell(me, m_spells.shaman.pCureDisease) &&
        IsValidDispelTarget(me, m_spells.shaman.pCureDisease))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.shaman.pCurePoison &&
        CanTryToCastSpell(me, m_spells.shaman.pCurePoison) &&
        IsValidDispelTarget(me, m_spells.shaman.pCurePoison))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    FindAndHealInjuredAlly(40.0f);
}

void AiBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheCheetah &&
       !me->IsMounted() &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheCheetah) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (Pet* pPet = me->GetPet())
        {   // cb:fold rotation rung, outcome probed at cast
            if (!HoldPetAtCamp(pPet) && !pPet->GetVictim())
            {   // cb:fold rotation rung, outcome probed at cast
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Hunter();
    }
}

void AiBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            RangedChaseInPlace(me, pVictim);
        }

        if (me->HasSpell(AB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {   // cb:fold rotation rung, outcome probed at cast
            switch (me->CastSpell(pVictim, AB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:   // cb:fold shares the restock probe below
                case SPELL_FAILED_NO_AMMO:   // cb:fold rotation rung, outcome probed at cast
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: out of ammo, restocking");
                    AddHunterAmmo();
                    break;
                }
            }
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pMultiShot && CanUseSpecAoE(pVictim, 10.0f, 1) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pAspectOfTheCheetah &&
            me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id))
        {   // cb:fold rotation rung, outcome probed at cast
            if (pVictim->CanReachWithMeleeAutoAttack(me))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pAspectOfTheMonkey &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            else
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pAspectOfTheHawk &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {   // cb:fold rotation rung, outcome probed at cast
            if (me->HasUnitState(UNIT_STATE_ROOT))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pMongooseBite &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.hunter.pMongooseBite) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.hunter.pRaptorStrike &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            else
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pWingClip &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
                {   // cb:fold rotation rung, outcome probed at cast
                    DoCastSpell(pVictim, m_spells.hunter.pWingClip);
                }
            }
        }

        if (!me->HasUnitState(UNIT_STATE_ROOT) &&
            (me->GetCombatDistance(pVictim) < 8.0f) &&
             me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HIT(me->GetGUIDLow(), "cpp-combat: hunter opening range from melee");
            if (!me->IsStopped())
                me->StopMoving();   // cb:fold hot per-update detail
            me->GetMotionMaster()->Clear();
            if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateOutOfCombatAI_Mage()
{
    if (m_spells.mage.pArcaneBrilliance)
    {   // cb:fold rotation rung, outcome probed at cast
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneBrilliance))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pArcaneBrilliance) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    else if (m_spells.mage.pArcaneIntellect)
    {   // cb:fold rotation rung, outcome probed at cast
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneIntellect))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pArcaneIntellect) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.mage.pCombustion &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pPyroblast &&
            m_spells.mage.pPresenceOfMind &&
            me->HasAura(m_spells.mage.pPresenceOfMind->Id) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pManaShield &&
            IsPhysicalDamageClass(pVictim->GetClass()) &&
           (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pManaShield))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            RangedChaseInPlace(me, pVictim);
        }
        else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                (pVictim->GetVictim() == me) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.mage.pConeofCold &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.mage.pBlink &&
               (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                CanTryToCastSpell(me, m_spells.mage.pBlink))
            {   // cb:fold rotation rung, outcome probed at cast
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                    me->GetMotionMaster()->MoveIdle();   // cb:fold rotation rung, outcome probed at cast

                if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.mage.pFrostNova &&
                    !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
                    !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                    CanTryToCastSpell(me, m_spells.mage.pFrostNova))
                {   // cb:fold rotation rung, outcome probed at cast
                    DoCastSpell(me, m_spells.mage.pFrostNova);
                }

                // [STOP-AND-CAST] Do NOT kite. Stand where we are and let the nuke rotation below
                // fire, instead of running 25yd away every time a mob reaches melee (the "mages keep
                // running away from their target" bug). Frost Nova above still roots the adjacent mob.
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                    me->GetMotionMaster()->MoveIdle();   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (GetAttackersInRangeCount(10.0f) > 1)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        // [STOP-AND-CAST] (kite removed above) — always fall through to the nuke rotation; never
        // skip casting to finish a run-away.

        if (m_spells.mage.pRemoveLesserCurse &&
           (me->GetAttackers().size() < 3) &&
            CanTryToCastSpell(me, m_spells.mage.pRemoveLesserCurse) &&
            IsValidDispelTarget(me, m_spells.mage.pRemoveLesserCurse))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pRemoveLesserCurse) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pPolymorph)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pTarget = SelectSafeSpecAdd(pVictim))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.mage.pPolymorph))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pPresenceOfMind &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->HasSpell(AB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, AB_SPELL_SHOOT_WAND, false);   // cb:fold rotation rung, outcome probed at cast
    }
}

void AiBotAI::UpdateOutOfCombatAI_Priest()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.priest.pPrayerofFortitude)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofFortitude))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofFortitude))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofFortitude) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pPrayerofSpirit)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofSpirit))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofSpirit))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofSpirit) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pShadowProtection)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.priest.pShadowProtection))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.priest.pShadowProtection) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else if (bg && bg->GetStatus() == STATUS_IN_PROGRESS)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.priest.pPowerWordFortitude &&
            IsValidBuffTarget(me, m_spells.priest.pPowerWordFortitude) &&
            CanTryToCastSpell(me, m_spells.priest.pPowerWordFortitude))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pPowerWordFortitude) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.priest.pDivineSpirit &&
            IsValidBuffTarget(me, m_spells.priest.pDivineSpirit) &&
            CanTryToCastSpell(me, m_spells.priest.pDivineSpirit))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pDivineSpirit) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {   // cb:fold rotation rung, outcome probed at cast
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Priest();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Priest()
{
    if (m_spells.priest.pPowerWordShield &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {   // cb:fold rotation rung, outcome probed at cast
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    // Heal
    if (me->GetShapeshiftForm() == FORM_NONE &&
        FindAndHealInjuredAlly(40.0f))
        return;   // cb:fold rotation rung, outcome probed at cast

    // Dispels
    if (m_spells.priest.pDispelMagic)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_role == ROLE_HEALER)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pFriend, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pDispelMagic) &&
                 CanTryToCastSpell(me, m_spells.priest.pDispelMagic))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    if (m_spells.priest.pAbolishDisease)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_role == ROLE_HEALER)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pFriend, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pAbolishDisease) &&
                 CanTryToCastSpell(me, m_spells.priest.pAbolishDisease))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    // Attack
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pSilence &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            RangedChaseInPlace(me, pVictim);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (me->HasSpell(AB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, AB_SPELL_SHOOT_WAND, false);   // cb:fold rotation rung, outcome probed at cast
    }

}

void AiBotAI::UpdateOutOfCombatAI_Warlock()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warlock.pDetectInvisibility)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {   // cb:fold rotation rung, outcome probed at cast
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (Pet* pPet = me->GetPet())
        {   // cb:fold rotation rung, outcome probed at cast
            if (!HoldPetAtCamp(pPet) && !pPet->GetVictim())
            {   // cb:fold rotation rung, outcome probed at cast
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pShadowWard &&
           (pVictim->GetClass() == CLASS_WARLOCK) &&
            CanTryToCastSpell(me, m_spells.warlock.pShadowWard))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warlock.pShadowWard) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pImmolate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pCorruption &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pFear)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pAdd = SelectSafeSpecAdd(pVictim))
                if (CanTryToCastSpell(pAdd, m_spells.warlock.pFear) &&   // cb:fold rotation rung, outcome probed at cast
                    DoCastSpell(pAdd, m_spells.warlock.pFear) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (pVictim->IsCaster())
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warlock.pCurseofTongues &&
                CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofTongues))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warlock.pCurseofTongues) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warlock.pCurseofExhaustion &&
                CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofExhaustion))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warlock.pCurseofExhaustion) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            RangedChaseInPlace(me, pVictim);
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->HasSpell(AB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, AB_SPELL_SHOOT_WAND, false);   // cb:fold rotation rung, outcome probed at cast
    }
}

void AiBotAI::UpdateOutOfCombatAI_Warrior()
{
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);   // cb:fold rotation rung, outcome probed at cast
        else if (m_spells.warrior.pBloodrage &&   // cb:fold rotation rung, outcome probed at cast
            (me->GetPower(POWER_RAGE) < 100) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {   // cb:fold rotation rung, outcome probed at cast
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateInCombatAI_Warrior()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {   // cb:fold rotation rung, outcome probed at cast
            if (!me->GetAttackers().empty() &&
                IsPhysicalDamageClass(pVictim->GetClass()))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.warrior.pShieldBlock &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.warrior.pShieldWall &&
                    (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pHamstring &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
            if (m_spells.warrior.pPiercingHowl &&
               (me->GetCombatDistance(pVictim) <= 10.0f) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPiercingHowl))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pPiercingHowl) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.warrior.pRend &&
           (pVictim->GetClass() == CLASS_ROGUE) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 50.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pRetaliation &&
            IsMeleeDamageClass(pVictim->GetClass()) &&
           (me->GetHealthPercent() > 70.0f) &&
           ((GetAttackersInRangeCount(10.0f) > 1) || (pVictim->GetClass() == CLASS_ROGUE)) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if ((me->GetHealthPercent() > 60.0f) && (pVictim->GetHealthPercent() > 40.0f) &&
            (pVictim->GetClass() == CLASS_WARLOCK || pVictim->GetClass() == CLASS_PRIEST) &&
            !me->HasUnitState(UNIT_STATE_ROOT) &&
            !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.warrior.pBerserkerRage &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerRage))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.warrior.pBerserkerRage) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetHealthPercent() < 20.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pBerserkerStance &&
               (pVictim->GetClass() != CLASS_ROGUE) &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }

        if (m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pWhirlwind &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pWhirlwind))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {   // cb:fold rotation rung, outcome probed at cast
            ChaseInPlace(me, pVictim, m_role);
        }

        if (m_spells.warrior.pHeroicStrike &&
           (me->GetPower(POWER_RAGE) > 300) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    else // no victim
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    // Stealth is an engage tool, not a travel stance. The inherited BattleBot rotation cast it
    // on every out-of-combat tick before even checking for a victim, so open-world rogues walked
    // entire quest/vendor/trainer routes at the stealth speed penalty. Enter it only on the final
    // approach to a selected victim. Cancel an ordinary stale stealth while travelling; preserve
    // low-health stealth so Vanish can still create its intended escape window.
    Unit* const pVictim = me->GetVictim();
    bool const closeApproach = pVictim && me->IsWithinDistInMap(pVictim, 35.0f);

    if (m_spells.rogue.pStealth && me->HasAura(m_spells.rogue.pStealth->Id))
    {   // cb:fold rotation rung, outcome probed at cast
        if (!closeApproach && me->GetHealthPercent() >= 25.0f)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combat: stale stealth cancelled for travel");
            me->RemoveAurasDueToSpellByCancel(m_spells.rogue.pStealth->Id);
        }
    }
    else if (m_spells.rogue.pStealth &&
             closeApproach &&
             CanTryToCastSpell(me, m_spells.rogue.pStealth) &&
            !me->HasAura(AURA_WARSONG_FLAG) &&
            !me->HasAura(AURA_SILVERWING_FLAG))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.rogue.pStealth) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (pVictim)
        UpdateInCombatAI_Rogue();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            else
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish->Id) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        CB_HIT(me->GetGUIDLow(), "cpp-combat: vanish escape, breaking away");
                        if (me->GetMotionMaster()->MoveDistance(pVictim, 40.0f))
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                }
            }
        }

        if (me->GetComboPoints() > 4 &&
            me->GetComboTargetGuid() == pVictim->GetObjectGuid())
        {   // cb:fold rotation rung, outcome probed at cast
            SpellEntry const* pComboSpell = nullptr;
            if (pVictim->GetHealthPercent() > 65.0f &&
                m_spells.rogue.pSliceAndDice &&
                !me->HasAura(m_spells.rogue.pSliceAndDice->Id))
                pComboSpell = m_spells.rogue.pSliceAndDice;   // cb:fold rotation rung, outcome probed at cast
            else if (pVictim->GetHealthPercent() > 75.0f && m_spells.rogue.pRupture)   // cb:fold rotation rung, outcome probed at cast
                pComboSpell = m_spells.rogue.pRupture;   // cb:fold rotation rung, outcome probed at cast
            else
                pComboSpell = m_spells.rogue.pEviscerate;   // cb:fold rotation rung, outcome probed at cast

            if (pComboSpell && CanTryToCastSpell(
                    pComboSpell == m_spells.rogue.pSliceAndDice ? me : pVictim, pComboSpell))
            {   // cb:fold rotation rung, outcome probed at cast
                Unit* pComboTarget = pComboSpell == m_spells.rogue.pSliceAndDice ? me : pVictim;
                if (DoCastSpell(pComboTarget, pComboSpell) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.rogue.pBlind)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.rogue.pColdBlood &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry && CanUseSpecAoE(pVictim, 8.0f, 1) &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateOutOfCombatAI_Druid()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pGiftoftheWild)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pGiftoftheWild))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.druid.pGiftoftheWild))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.druid.pGiftoftheWild) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.druid.pThorns)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pMarkoftheWild && CanTryToCastSpell(me, m_spells.druid.pMarkoftheWild))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pMarkoftheWild) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.druid.pThorns && CanTryToCastSpell(me, m_spells.druid.pThorns))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pThorns) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_role == ROLE_TANK)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.druid.pBearForm &&
                CanTryToCastSpell(me, m_spells.druid.pBearForm))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
        else if (m_role == ROLE_MELEE_DPS)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.druid.pCatForm &&
                CanTryToCastSpell(me, m_spells.druid.pCatForm))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if ((me->GetPowerPercent(POWER_MANA) >  80.0f) &&
                FindAndHealInjuredAlly(80.0f))
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pProwl &&
            CanTryToCastSpell(me, m_spells.druid.pProwl) &&
            !me->HasAura(AURA_WARSONG_FLAG) &&
            !me->HasAura(AURA_SILVERWING_FLAG))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pProwl) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        UpdateInCombatAI_Druid();
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pMoonkinForm &&
            me->GetShapeshiftForm() == FORM_MOONKIN)
            me->RemoveAurasDueToSpellByCancel(m_spells.druid.pMoonkinForm->Id);   // cb:fold rotation rung, outcome probed at cast

        if (m_spells.druid.pTravelForm &&
           !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.druid.pTravelForm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pTravelForm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateInCombatAI_Druid()
{
    if (m_spells.druid.pTravelForm &&
        me->GetShapeshiftForm() == FORM_TRAVEL)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat: travel form dropped for combat");
        me->RemoveAurasDueToSpellByCancel(m_spells.druid.pTravelForm->Id);
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pHibernate &&
            !me->GetAttackers().empty())
        {   // cb:fold rotation rung, outcome probed at cast
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        // Heal
        if (FindAndHealInjuredAlly(80.0f))
            return;   // cb:fold rotation rung, outcome probed at cast

        // Dispels
       SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                                         m_spells.druid.pAbolishPoison :
                                         m_spells.druid.pCurePoison;
       if (pDispelSpell)
       {   // cb:fold rotation rung, outcome probed at cast
           if (m_role == ROLE_HEALER)
           {   // cb:fold rotation rung, outcome probed at cast
               if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
               {   // cb:fold rotation rung, outcome probed at cast
                   if (CanTryToCastSpell(pFriend, pDispelSpell))
                   {   // cb:fold rotation rung, outcome probed at cast
                       if (DoCastSpell(pFriend, pDispelSpell) == SPELL_CAST_OK)
                           return;   // cb:fold rotation rung, outcome probed at cast
                   }
               }
           }
           else if (IsValidDispelTarget(me, pDispelSpell) &&
                    CanTryToCastSpell(me, pDispelSpell))
           {   // cb:fold rotation rung, outcome probed at cast
               if (DoCastSpell(me, pDispelSpell) == SPELL_CAST_OK)
                   return;   // cb:fold rotation rung, outcome probed at cast
           }
       }

        if (m_spells.druid.pRemoveCurse)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pFriend, m_spells.druid.pRemoveCurse) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }

        if (m_spells.druid.pInnervate &&
            me->GetVictim() &&
           (me->GetHealthPercent() > 40.0f) &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pInnervate))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pVictim = me->GetVictim())
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.druid.pBearForm &&
                    pVictim->CanReachWithMeleeAutoAttack(me) &&
                    IsPhysicalDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(me, m_spells.druid.pBearForm))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pCatForm &&
                    CanTryToCastSpell(me, m_spells.druid.pCatForm))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->HasUnitState(UNIT_STATE_ROOT) &&
            me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combat: druid rooted, shifting out of form");
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        ShapeshiftForm const form = me->GetShapeshiftForm();
        if (m_spells.druid.pBarkskin &&
           (form == FORM_NONE || form == FORM_MOONKIN) &&
           (me->GetHealthPercent() < 50.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pBarkskin))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        switch (form)
        {
            case FORM_CAT:   // cb:fold rotation rung, outcome probed at cast
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);   // cb:fold rotation rung, outcome probed at cast

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {   // cb:fold rotation rung, outcome probed at cast
                    ChaseInPlace(me, pVictim, m_role);
                }

                if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (m_spells.druid.pPounce &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    if (m_spells.druid.pRavage &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    if (m_spells.druid.pTigersFury &&
                        CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    return;
                }

                if (me->GetComboPoints() > 4)
                {   // cb:fold rotation rung, outcome probed at cast
                    if (m_spells.druid.pFerociousBite &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }

                    if (m_spells.druid.pRip &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                }

                if (!me->CanReachWithMeleeAutoAttack(pVictim))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (m_spells.druid.pFaerieFireFeral &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }

                    if (m_spells.druid.pDash &&
                        pVictim->IsMoving() &&
                        CanTryToCastSpell(me, m_spells.druid.pDash))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                }

                if (m_spells.druid.pShred &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pShred))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pRake &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRake))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pClaw &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                break;
            }
            case FORM_BEAR:   // cb:fold rotation rung, outcome probed at cast
            case FORM_DIREBEAR:   // cb:fold rotation rung, outcome probed at cast
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);   // cb:fold rotation rung, outcome probed at cast

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {   // cb:fold rotation rung, outcome probed at cast
                    ChaseInPlace(me, pVictim, m_role);
                }

                if (m_spells.druid.pFeralCharge &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pBash &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pBash))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pFrenziedRegeneration &&
                   (me->GetHealthPercent() < 30.0f) &&
                    CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pDemoralizingRoar &&
                    IsMeleeDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pSwipe &&
                   ((me->GetPower(POWER_RAGE) > 500) || (GetAttackersInRangeCount(10.0f) > 1)) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pMaul &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
                break;
            }
            case FORM_NONE:   // cb:fold rotation rung, outcome probed at cast
            case FORM_MOONKIN:   // cb:fold rotation rung, outcome probed at cast
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                    me->GetDistance(pVictim) > 30.0f)
                {   // cb:fold rotation rung, outcome probed at cast
                    RangedChaseInPlace(me, pVictim);
                }
                else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                        (pVictim->GetVictim() == me) &&
                        !me->HasUnitState(UNIT_STATE_ROOT) &&
                        (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
                {   // cb:fold rotation rung, outcome probed at cast
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: druid caster opening range from melee");
                    if (m_spells.druid.pEntanglingRoots &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    me->SetCasterChaseDistance(25.0f);
                    if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pFaerieFire &&
                   (pVictim->GetClass() == CLASS_ROGUE) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pInsectSwarm &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                    return;   // cb:fold rotation rung, outcome probed at cast

                if (m_spells.druid.pMoonfire &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pStarfire &&
                   (pVictim->GetHealthPercent() > 50.0f) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pWrath &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                break;
            }
        }
    }
}

// [TACTICS] Tank facing (owner 2026-09-23: "tanks are not turning core hounds around").
// A mob faces its victim. When the tank stands between the group and the mob, the mob's
// front - its cleave, breath or cone - points at the group. The tank therefore steps to the
// far side of the mob, so the mob turns its back to everyone else. Only while the tank
// holds the mob, only when it is badly misaligned, and at most every 1.5 s.
// A tank's step must keep sight of its mob and be walkable in a short path (2026-09-25: a facing
// step pressed Sulfuron's priest's tank into rock - 1 yd apart without sight for 25 s, and the
// priest evaded "target unreachable", resetting the whole encounter).
namespace { bool NearIdleHostile(Player* me, float x, float y); bool Molten(Player* me, float x, float y, float z); }

// Standing at (x, y, z), do I see `other`? Measured the way the game measures a cast: from my own eye
// height there to its chest (+1 yd). `other->IsWithinLOS(x, y, z + 1.5f)` looked at a point 3.5 yd up
// (the call adds 2 yd of its own) and passed spots below a ledge (2026-11-06, Majordomo: healers below
// his platform step "closed in" to 20 yd of the tank again and again, still without sight; the leader
// died twice with no heal in 8 s).
static bool SeesFrom(Player const* me, float x, float y, float z, Unit const* other)
{
    return me->IsWithinLOSAtPosition(x, y, z, other->GetPositionX(), other->GetPositionY(), other->GetPositionZ(), true, 1.0f);
}

static bool TankStepOk(Player* me, Unit* mob, float x, float y, float z)
{
    if (!SeesFrom(me, x, y, z, mob))
        return false;
    // ...and never into the reach of a pack not in the fight: the mob follows its tank (2026-11-05:
    // a separation step took a Flameguard's tank 20 yd from the next pack's Lava Reaver - all 39 dead).
    if (NearIdleHostile(me, x, y))
        return false;
    // ...and never out of every healer's sight: separation steps of 1-3 yd each walked a tank down
    // Majordomo's terrace until no healer saw him, and he died with nothing healed in 8 s (2026-11-08).
    if (Group* group = me->GetGroup())
    {
        bool anyHealer = false, seen = false;
        for (GroupReference* itr = group->GetFirstMember(); itr && !seen; itr = itr->next())
        {
            Player* h = itr->getSource();
            AiBotAI const* hai = h && h != me && h->IsAlive() && h->IsInMap(me) ? dynamic_cast<AiBotAI const*>(h->AI()) : nullptr;
            if (!hai || hai->GetCombatActiveRole() != ROLE_HEALER || !h->IsWithinDist3d(x, y, z, 60.0f))
                continue;
            anyHealer = true;
            seen = h->GetDistance(x, y, z) <= 38.0f && SeesFrom(me, x, y, z, h);
        }
        if (anyHealer && !seen)
            return false;
        // ...nor further than 35 yd from the healers' middle, unless the step brings it closer: separation
        // steps spread five held mobs over 90 yd, and the healers dealt to a tank 60-70 yd out never reached
        // it (2026-11-08, Majordomo: a paladin tank held an Elite 52-73 yd from eight of nine healers and
        // died with one priest in range).
        float hx = 0.0f, hy = 0.0f;
        uint32 hn = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* h = itr->getSource();
            AiBotAI const* hai = h && h != me && h->IsAlive() && h->IsInMap(me) ? dynamic_cast<AiBotAI const*>(h->AI()) : nullptr;
            if (!hai || hai->GetCombatActiveRole() != ROLE_HEALER || !me->IsWithinDistInMap(h, 80.0f))
                continue;
            hx += h->GetPositionX();
            hy += h->GetPositionY();
            ++hn;
        }
        if (hn >= 2)
        {
            hx /= hn;
            hy /= hn;
            float const there = std::sqrt((x - hx) * (x - hx) + (y - hy) * (y - hy));
            float const here = me->GetDistance2d(hx, hy);
            if (there > 35.0f && there > here)   // (28 kept a Healer stacked on Majordomo, both tanks in its Shock)
                return false;
        }
    }
    PathInfo path(me);
    path.calculate(x, y, z, false);
    if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
        return false;
    PointsArray const& pts = path.getPath();
    float len = 0.0f;
    for (size_t k = 1; k < pts.size(); ++k)
        len += (pts[k] - pts[k - 1]).length();
    return len <= me->GetDistance(x, y, z) * 2.0f + 4.0f;
}

static std::vector<uint64> OnyxiaLandingPair(Player* me, Creature* boss, Group* group, Map const* map);
static bool OnyxiaLandedWithPair(Player* me, Creature* ony, bool& inPair, bool& onPair);

bool AiBotAI::MaintainTankFacing(Unit* victim)
{
    if (!victim || !victim->IsCreature() || GetCombatActiveRole() != ROLE_TANK || !victim->IsAlive())
        return false;
    // [TACTICS] 2026-11-18 R4W0 Onyxia drill: landed, a tank who is not one of her two never chases her onto another
    // member (a whelp-post tank chased her off a ranged member, held her and walked her back to its post): the member
    // she is on brings her to the head spot (SuiTactic::LandingHeadSpot).
    if (victim->GetEntry() == 10184 && victim->GetVictim() != me && SuiTactic::On(me, SuiTactic::LandingHeadSpot, victim))
    {
        // (from the moment her pair is chosen, airborne at <= 42 %: a post tank chased her off a ranged member at her
        // touch-down tick, before "landed" - R4W0 drill 2 pull 5)
        std::vector<uint64> const pair = me->GetGroup() && victim->GetHealthPercent() <= 42.0f ?
            OnyxiaLandingPair(me, static_cast<Creature*>(victim), me->GetGroup(), me->GetMap()) : std::vector<uint64>();
        if (!pair.empty() && std::find(pair.begin(), pair.end(), me->GetObjectGuid().GetRawValue()) == pair.end())
            return false;
    }
    // After a facing step the tank stands on its point; if the mob drifts out of reach,
    // resume the chase rather than standing there swinging at air. Also when the mob has turned
    // on someone else: a threat wipe sends it to a healer, and its tank must follow it to taunt
    // (2026-11-08, Majordomo: his teleport resets his threat every 20-30 s; his tank stood 30 s
    // on its facing point while he walked 47 yd and killed two priests).
    if (!me->IsMoving() && !me->CanReachWithMeleeAutoAttack(victim) &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
    {
        if (victim->GetVictim() != me && WorldTimer::getMSTime() >= m_tankLostLogMs)
        {
            m_tankLostLogMs = WorldTimer::getMSTime() + 5000;
            Unit* on = victim->GetVictim();
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: %s turned on %s - chases it again (%.0f yd, motion %u)",
                me->GetName(), victim->GetName(), on ? on->GetName() : "nobody", me->GetDistance(victim),
                uint32(me->GetMotionMaster()->GetCurrentMovementGeneratorType()));
        }
        ChaseInPlace(me, victim, ROLE_TANK);
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE || victim->GetVictim() == me)
            return true;
    }
    if (victim->GetVictim() != me)
        return false;
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_tankFacingNextMs || me->IsMoving() || me->IsNonMeleeSpellCasted(false))
        return false;
    Group* group = me->GetGroup();
    if (!group)
        return false;
    // Two held mobs side by side double every point-blank area hit on the melee between them
    // (two Molten Giants' Smash). A tank whose mob stands within 12 yd of a mob another tank
    // holds walks its own a few steps further away; the mob follows its victim. Two bursters are
    // taken out of each other's circle (2026-11-07, Majordomo: four tanks held the four woken Healers
    // within 10 yd of each other, each tank took all four Shadow Shocks and died in 9-14 s).
    float const myPulse = SuiRaidTelemetry::EntryPulseRadius(victim->GetEntry());
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* other = itr->getSource();
        Unit* otherMob = other && other != me && other->IsAlive() ? other->GetVictim() : nullptr;
        if (!otherMob || otherMob == victim || otherMob->GetVictim() != other || !IsSpecAoETankHolder(other))
            continue;
        float const apart = std::max(12.0f, std::max(myPulse, SuiRaidTelemetry::EntryPulseRadius(otherMob->GetEntry())) + 4.0f);
        if (!otherMob->IsWithinDistInMap(victim, apart))
            continue;
        float ox = victim->GetPositionX() - otherMob->GetPositionX(), oy = victim->GetPositionY() - otherMob->GetPositionY();
        float const olen = std::sqrt(ox * ox + oy * oy);
        if (olen < 0.5f)
        {
            ox = std::cos(me->GetOrientation());
            oy = std::sin(me->GetOrientation());
        }
        else
        {
            ox /= olen;
            oy /= olen;
        }
        // Only one of the two moves, so they do not dance around each other: the one whose mob has the
        // smaller circle (a burster dragged along sweeps its circle over the melee and healers), the higher
        // guid when they match (2026-11-08, Majordomo: the guid alone made the paladin tank walk every time).
        float const otherPulse = SuiRaidTelemetry::EntryPulseRadius(otherMob->GetEntry());
        if (myPulse > otherPulse + 0.5f || (std::fabs(myPulse - otherPulse) <= 0.5f && other->GetGUIDLow() > me->GetGUIDLow()))
            break;
        for (float turn : { 0.0f, 0.785f, -0.785f, 1.571f, -1.571f })
        {
            float const c = std::cos(turn), sn = std::sin(turn);
            float const ux = ox * c - oy * sn, uy = ox * sn + oy * c;
            float x = me->GetPositionX() + ux * 8.0f, y = me->GetPositionY() + uy * 8.0f, z = me->GetPositionZ();
            ReGroundZ(x, y, z, "tank-separation");
            if (std::fabs(z - me->GetPositionZ()) > 4.0f || Molten(me, x, y, z) || !TankStepOk(me, victim, x, y, z))
                continue;
            m_tankFacingNextMs = now + 2500;
            CB_HIT(me->GetGUIDLow(), "cpp-combat: tank separating its mob from another tank's");
            if (myPulse > 0.0f)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: takes %s out of %s's circle (%.0f yd apart, wants %.0f)",
                    me->GetName(), victim->GetName(), otherMob->GetName(), victim->GetDistance(otherMob), apart);
            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
            return true;
        }
        break;
    }
    // The fight is taken away from sleeping bursters: everyone who stands by a held mob stands by the sheeps
    // too, and at the wake-up nobody can get out of their circles in time (2026-11-09, Majordomo attempt 83:
    // the four sheeped Healers lay among the casters by the last Elite; 11 s of clear steps from 0-10 yd, three
    // dead at the wake-up and the Healers loose on the rest). A held mob within a sheep's circle + 8 yd walks
    // 8 yd away from the sheeps' middle.
    if (!(victim->IsCreature() && static_cast<Creature*>(victim)->IsWorldBoss()))
    {
        std::list<Unit*> around;
        me->GetEnemyListInRadiusAround(victim, 40.0f, around);
        float sx = 0.0f, sy = 0.0f;
        uint32 near = 0;
        float worst = 0.0f;
        for (Unit* u : around)
        {
            if (!u || u == victim || !u->IsCreature() || !u->IsAlive() || !u->HasBreakableByDamageCrowdControlAura())
                continue;
            float const pr = SuiRaidTelemetry::EntryPulseRadius(u->GetEntry());
            if (pr < 18.0f)
                continue;
            float const inside = pr + 8.0f - victim->GetDistance(u);
            if (inside <= 0.0f)
                continue;
            sx += u->GetPositionX();
            sy += u->GetPositionY();
            ++near;
            worst = std::max(worst, inside);
        }
        if (near)
        {
            sx /= near;
            sy /= near;
            float ox = victim->GetPositionX() - sx, oy = victim->GetPositionY() - sy;
            float const olen = std::sqrt(ox * ox + oy * oy);
            if (olen < 0.5f)
            {
                ox = std::cos(me->GetOrientation());
                oy = std::sin(me->GetOrientation());
            }
            else
            {
                ox /= olen;
                oy /= olen;
            }
            for (float turn : { 0.0f, 0.785f, -0.785f, 1.571f, -1.571f })
            {
                float const c = std::cos(turn), sn = std::sin(turn);
                float const ux = ox * c - oy * sn, uy = ox * sn + oy * c;
                float x = me->GetPositionX() + ux * 8.0f, y = me->GetPositionY() + uy * 8.0f, z = me->GetPositionZ();
                ReGroundZ(x, y, z, "tank-sheep-clear");
                if (std::fabs(z - me->GetPositionZ()) > 4.0f || Molten(me, x, y, z) || !TankStepOk(me, victim, x, y, z))
                    continue;
                m_tankFacingNextMs = now + 2500;
                if (now >= m_tankSheepLogMs)
                {
                    m_tankSheepLogMs = now + 5000;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: takes %s away from %u sleeping burster(s) (%.0f yd inside)",
                        me->GetName(), victim->GetName(), near, worst);
                }
                me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                return true;
            }
        }
    }
    // A kind that pulses around itself and has never breathed forward gains nothing from being
    // turned, and every turn walks the fight away from healers kept outside its circle
    // (2026-09-25: Baron Geddon's tank died with the healers 42-83 yd away).
    if (SuiRaidTelemetry::EntryPulseRadius(victim->GetEntry()) > 0.0f &&
        SuiRaidTelemetry::EntryConeRadius(victim->GetEntry()) <= 0.0f)
        return false;

    // A world boss that breathes a cone is held with its head to the nearest wall, and held there: its breath goes into
    // the stone and its facing no longer follows the raid. Squaring it to the group's middle turned it every time the
    // group moved, and the group moved every time it turned (2026-11-14, Onyxia dev attempt 16: 372 "out of the front"
    // steps in the first minute, the ranged casting 35 % of the fight; the raid's guides tank her facing the north wall).
    // The facing is chosen once where it stands and kept for every tank who holds it, until it moves 12 yd.
    // [TACTICS] 2026-11-16 R3W8/R3W9 Onyxia: each phase-3 wall turn swept her Flame Breath across the raid - hits up to
    // 59 degrees off her facing on members already moving out of it, 11 dead in one breath. Once she has landed (below
    // 40 %) her tank holds her where she faces and the raid leaves her front once. Gated by SuiTactic::NoTurnAfterLanding.
    if (victim->IsCreature() && victim->GetHealthPercent() < 40.0f && SuiTactic::On(me, SuiTactic::NoTurnAfterLanding, victim))
        return false;
    if (victim->IsCreature() && static_cast<Creature*>(victim)->IsWorldBoss() &&
        SuiRaidTelemetry::EntryConeRadius(victim->GetEntry()) > 0.0f &&
        !victim->IsFlying() && !victim->IsLevitating() && !victim->IsHovering())
    {
        struct WallFacing { float bx, by, fx, fy; uint32 setMs; bool none; };
        static std::mutex wallLock;
        static std::unordered_map<uint64, WallFacing> walls;
        float const reach = victim->GetObjectBoundingRadius() + me->GetObjectBoundingRadius() +
            (victim->GetCombatReach() >= 8.0f ? std::min(victim->GetCombatReach() * 0.35f, 7.0f) : 1.5f);
        float const bx = victim->GetPositionX(), by = victim->GetPositionY(), bz = victim->GetPositionZ();
        WallFacing wf{};
        bool have = false;
        {
            std::lock_guard<std::mutex> guard(wallLock);
            auto it = walls.find(victim->GetObjectGuid().GetRawValue());
            if (it != walls.end() && now - it->second.setMs < 900000 &&
                (it->second.bx - bx) * (it->second.bx - bx) + (it->second.by - by) * (it->second.by - by) < 144.0f)
            {
                wf = it->second;
                have = true;
            }
        }
        if (!have)
        {
            // 24 directions; the wall is where the boss's line of sight at head height first breaks, 2 yd steps out to
            // 30 yd. The tank must have room between the boss and the wall and floor to stand on.
            float bestWall = 1e9f, bestFx = 0.0f, bestFy = 0.0f;
            // [TACTICS] 2026-11-16 R3W7 Onyxia 33 %: the nearest wall was 28 yd west and the healers and ranged stood
            // between it and her head; turned there, one Flame Breath killed three at +311 s and the next three more.
            // The wall chosen is the one with the fewest members inside her cone (50 degrees, 45 yd), the nearer one
            // when even. Gated by SuiTactic::WallAwayFromRaid.
            bool const awayFromRaid = SuiTactic::On(me, SuiTactic::WallAwayFromRaid, victim) && me->GetGroup();
            uint32 bestCrowd = 1000;
            for (int k = 0; k < 24; ++k)
            {
                float const th = float(k) * float(M_PI) / 12.0f, fx = std::cos(th), fy = std::sin(th);
                float wall = 0.0f;
                for (float d = 6.0f; d <= 30.0f; d += 2.0f)
                    if (!victim->GetMap()->isInLineOfSight(bx, by, bz + 3.0f, bx + fx * d, by + fy * d, bz + 3.0f))
                    {
                        wall = d;
                        break;
                    }
                uint32 crowd = 0;
                if (awayFromRaid)
                    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
                    {
                        Player* m = itr->getSource();
                        if (!m || !m->IsAlive() || !m->IsInMap(me))
                            continue;
                        float const mx = m->GetPositionX() - bx, my = m->GetPositionY() - by;
                        float const md = std::sqrt(mx * mx + my * my);
                        if (md > 45.0f || md < 1.0f)
                            continue;
                        if ((mx * fx + my * fy) / md > 0.64f)   // within 50 degrees of her head
                            ++crowd;
                    }
                if (wall <= 0.0f || wall < reach + 3.0f || crowd > bestCrowd || (crowd == bestCrowd && wall >= bestWall))
                    continue;
                float x = bx + fx * reach, y = by + fy * reach, z = bz;
                ReGroundZ(x, y, z, "tank-wall");
                if (std::fabs(z - bz) > 4.0f || Molten(me, x, y, z))
                    continue;
                bestCrowd = crowd;
                bestWall = wall;
                bestFx = fx;
                bestFy = fy;
            }
            wf = { bx, by, bestFx, bestFy, now, bestWall > 1e8f };
            {
                std::lock_guard<std::mutex> guard(wallLock);
                walls[victim->GetObjectGuid().GetRawValue()] = wf;
            }
            if (wf.none)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: no wall within 30 yd of %s at (%.1f, %.1f) - turns it from the group",
                    me->GetName(), victim->GetName(), bx, by);
            else
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: holds %s with its head to the wall (%.0f deg, wall %.0f yd) at (%.1f, %.1f)",
                    me->GetName(), victim->GetName(), std::atan2(wf.fy, wf.fx) * 180.0f / float(M_PI), bestWall, bx, by);
        }
        if (!wf.none)
        {
            float tx = me->GetPositionX() - bx, ty = me->GetPositionY() - by;
            float const tlen = std::sqrt(tx * tx + ty * ty);
            if (tlen > 0.1f && (tx * wf.fx + ty * wf.fy) / tlen > 0.94f)   // within ~20 degrees
                return false;
            m_tankFacingNextMs = now + 1500;
            float x = bx + wf.fx * reach, y = by + wf.fy * reach, z = bz;
            ReGroundZ(x, y, z, "tank-wall");
            if (std::fabs(z - bz) > 4.0f || !TankStepOk(me, victim, x, y, z))
                return false;
            CB_HIT(me->GetGUIDLow(), "cpp-combat: tank holding a cone boss's head to the wall");
            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
            return true;
        }
    }

    float cx = 0.0f, cy = 0.0f;
    uint32 count = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || m == me || !m->IsAlive() || !m->IsInWorld() || !m->IsInMap(me) ||
            IsSpecAoETankHolder(m) || !m->IsWithinDistInMap(victim, 40.0f))
            continue;
        cx += m->GetPositionX();
        cy += m->GetPositionY();
        ++count;
    }
    if (count == 0)
        return false;
    cx /= count;
    cy /= count;

    // Wanted: the tank on the mob's far side from the group.
    float ax = victim->GetPositionX() - cx, ay = victim->GetPositionY() - cy;
    float const alen = std::sqrt(ax * ax + ay * ay);
    float tx = me->GetPositionX() - victim->GetPositionX(), ty = me->GetPositionY() - victim->GetPositionY();
    float const tlen = std::sqrt(tx * tx + ty * ty);
    if (alen < 2.0f || tlen < 0.1f)
        return false;
    ax /= alen; ay /= alen;
    // A kind that breathes a cone sweeps its tail as well: the group belongs at its side, so its
    // tank stands square to the group, on whichever side it already is (2026-09-25: turned "away
    // from the group", Onyxia put the whole raid behind her - Tail Sweep 108k in 77 s).
    if (SuiRaidTelemetry::EntryConeRadius(victim->GetEntry()) > 0.0f)
    {
        float px = -ay, py = ax;
        if (tx * px + ty * py < 0.0f)
        {
            px = -px;
            py = -py;
        }
        ax = px;
        ay = py;
    }
    float const dot = (tx * ax + ty * ay) / tlen;
    if (dot > 0.64f)   // within ~50 degrees of ideal
        return false;

    m_tankFacingNextMs = now + 1500;
    // (a creature of long reach is tanked from further out, as ChaseInPlace places its tanks)
    float const reach = victim->GetObjectBoundingRadius() + me->GetObjectBoundingRadius() +
        (victim->GetCombatReach() >= 8.0f ? std::min(victim->GetCombatReach() * 0.35f, 7.0f) : 1.5f);
    float x = victim->GetPositionX() + ax * reach, y = victim->GetPositionY() + ay * reach;
    float z = victim->GetPositionZ();
    ReGroundZ(x, y, z, "tank-facing");
    if (std::fabs(z - victim->GetPositionZ()) > 4.0f || !TankStepOk(me, victim, x, y, z))
        return false;   // a ledge, a lava lip or rock on the far side: stay put
    CB_HIT(me->GetGUIDLow(), "cpp-combat: tank stepping to turn the mob away from the group");
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return true;
}

// [TACTICS] Ranged spacing (2026-09-23: Molten Giant Smash killed the healers and casters
// standing where the giant was brought). A ranged damage dealer or a healer in a group fight
// that finds a hostile within 12 yd steps directly away from it to about 22 yd, but never
// further than 35 yd from its anchor (the tank it heals and the raid it supports). At most
// every 2 s, never while casting, and only on ground the navmesh can walk.
namespace
{
    struct GroundHazard { float x, y, r; };
    struct HostileAreasInRange
    {
        WorldObject const* origin; float range;
        bool operator()(DynamicObject* area) const
        {
            return area->GetType() == DYNAMIC_OBJECT_AREA_SPELL && area->IsInWorld() && origin->IsWithinDist(area, range, false);
        }
        bool operator()(WorldObject*) const { return false; }
    };
    struct TrapsInRange
    {
        WorldObject const* origin; float range;
        bool operator()(GameObject* go) const
        {
            GameObjectInfo const* info = go->GetGOInfo();
            return info && info->type == GAMEOBJECT_TYPE_TRAP && go->isSpawned() && origin->IsWithinDist(go, range, false);
        }
    };
    // True when the spell deals damage (direct or periodic) in an area; radius from its data.
    bool HarmfulArea(SpellEntry const* spell, float& radius)
    {
        bool harmful = false;
        radius = 0.0f;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            bool const aura = spell->Effect[i] == SPELL_EFFECT_APPLY_AURA || spell->Effect[i] == SPELL_EFFECT_PERSISTENT_AREA_AURA;
            bool const damage = spell->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE ||
                (aura && (spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
                    spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE_PERCENT));
            if (!damage)
                continue;
            harmful = true;
            if (SpellRadiusEntry const* entry = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                radius = std::max(radius, Spells::GetSpellRadius(entry));
        }
        return harmful;
    }
    // The radius of the area an aura on `u` sets off around it (a periodic trigger of an area
    // damage spell centred on its source), or 0. `harmful` picks the aura side: a bomb put on a
    // member is harmful to it, a pulse an enemy wears on itself is positive for it. Spell data only.
    float PulsingAreaRadius(Unit const* u, bool harmful, uint32* remainingMs = nullptr, uint32* auraId = nullptr)
    {
        for (auto const& entry : u->GetSpellAuraHolderMap())
        {
            SpellAuraHolder const* holder = entry.second;
            if (!holder || holder->IsPositive() == harmful)
                continue;
            SpellEntry const* spell = holder->GetSpellProto();
            for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (spell->EffectApplyAuraName[i] != SPELL_AURA_PERIODIC_TRIGGER_SPELL || !spell->EffectTriggerSpell[i])
                    continue;
                SpellEntry const* trig = sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]);
                float radius = 0.0f;
                if (!trig || !HarmfulArea(trig, radius) || radius <= 0.0f)
                    continue;
                bool atSource = false;
                for (uint8 k = 0; k < MAX_EFFECT_INDEX; ++k)
                    if (trig->EffectImplicitTargetA[k] == TARGET_LOCATION_CASTER_SRC ||
                        trig->EffectImplicitTargetA[k] == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC ||
                        trig->EffectImplicitTargetB[k] == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC)
                        atSource = true;
                if (!atSource)
                    continue;
                if (remainingMs)
                    *remainingMs = holder->GetAuraDuration() > 0 ? uint32(holder->GetAuraDuration()) : 8000u;
                if (auraId)
                    *auraId = spell->Id;
                return radius;
            }
        }
        return 0.0f;
    }

    // A trash creature's pulse that hits for under a quarter of my health is healed through, not run
    // from (2026-11-05: a Flameguard's Fire Shield, 250 every 2 s, sent its tank "out of the fire"
    // every 2 s; the Flameguard followed and swung its Cone of Fire through the healers). The hit is
    // the biggest one seen from its kind, or the listed damage of the spell its aura fires. A world
    // boss's pulse always counts: Baron Geddon's Inferno is scripted and ramps from small ticks.
    bool LightPulse(Player const* me, Unit const* u)
    {
        if (!u->IsCreature() || static_cast<Creature const*>(u)->IsWorldBoss())
            return false;
        uint32 hit = SuiRaidTelemetry::EntryBurstMaxHit(u->GetEntry());
        for (auto const& entry : u->GetSpellAuraHolderMap())
            if (SpellAuraHolder const* holder = entry.second)
                for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                    if (holder->GetSpellProto()->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_TRIGGER_SPELL)
                        if (SpellEntry const* trig = sSpellMgr.GetSpellEntry(holder->GetSpellProto()->EffectTriggerSpell[i]))
                            for (uint8 k = 0; k < MAX_EFFECT_INDEX; ++k)
                                if (trig->Effect[k] == SPELL_EFFECT_SCHOOL_DAMAGE)
                                    hit = std::max<uint32>(hit, uint32(std::max(0, trig->EffectBasePoints[k] + trig->EffectDieSides[k])));
        return hit > 0 && hit * 4 < me->GetMaxHealth();
    }

    // Hostile damaging areas on the ground near me, from the live objects and their spell data only
    // (a rain of fire, a burning patch, a trap): no spell, creature or encounter is named.
    void CollectGroundHazards(Player* me, std::vector<GroundHazard>& out)
    {
        float const range = 40.0f;
        CellPair pair(MaNGOS::ComputeCellPair(me->GetPositionX(), me->GetPositionY()));
        Cell cell(pair);
        cell.SetNoCreate();
        std::list<WorldObject*> areas;
        HostileAreasInRange areaCheck{me, range};
        MaNGOS::WorldObjectListSearcher<HostileAreasInRange> areaSearcher(areas, areaCheck);
        TypeContainerVisitor<MaNGOS::WorldObjectListSearcher<HostileAreasInRange>, GridTypeMapContainer> areaVisitor(areaSearcher);
        cell.Visit(pair, areaVisitor, *me->GetMap(), *me, range);
        for (WorldObject* object : areas)
        {
            DynamicObject* area = static_cast<DynamicObject*>(object);
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(area->GetSpellId());
            float radius = 0.0f;
            if (!spell || !HarmfulArea(spell, radius) || !area->IsHostileTo(me))
                continue;
            out.push_back({area->GetPositionX(), area->GetPositionY(), std::max(area->GetRadius(), radius) + 1.5f});
        }
        std::list<GameObject*> traps;
        TrapsInRange trapCheck{me, range};
        MaNGOS::GameObjectListSearcher<TrapsInRange> trapSearcher(traps, trapCheck);
        TypeContainerVisitor<MaNGOS::GameObjectListSearcher<TrapsInRange>, GridTypeMapContainer> trapVisitor(trapSearcher);
        cell.Visit(pair, trapVisitor, *me->GetMap(), *me, range);
        for (GameObject* go : traps)
        {
            GameObjectInfo const* info = go->GetGOInfo();
            if (info->trap.radius == 0 && info->trap.cooldown == 0)
                continue;   // a one-shot scripted burst, inert afterwards
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(info->trap.spellId);
            float radius = 0.0f;
            // A hostile trap sprung by walking near it is kept away from whatever it does - it may
            // hatch or call something rather than burn (2026-09-25: Onyxia's eggs, 10 yd, hatched
            // whelps under the raid all through her first phase).
            bool const burns = spell && HarmfulArea(spell, radius);
            if (!spell || (!burns && info->trap.radius == 0))
                continue;
            if (!burns)
            {
                // it springs on distance in three dimensions: from a ledge above it is no danger
                // (2026-09-25: the eggs lie in a pit 10 yd below the way into Onyxia's lair)
                float const dz = std::fabs(go->GetPositionZ() - me->GetPositionZ());
                float const r = float(info->trap.radius);
                if (dz >= r)
                    continue;
                Unit* owner = go->GetOwner();
                if (owner ? !owner->IsHostileTo(me) : !go->IsHostileTo(me))
                    continue;
                out.push_back({go->GetPositionX(), go->GetPositionY(), std::sqrt(r * r - dz * dz) + 1.5f});
                continue;
            }
            // A trap nobody can spring by walking on it (no trigger radius) goes off only when the
            // encounter's script fires it: dormant until one of its kind has been seen going off in
            // this instance (2026-09-25: fifty of Onyxia's lava fissures, dormant until her last
            // phase, herded the whole raid into one corner behind her tail from the first second).
            if (info->trap.radius == 0)
            {
                static std::set<std::pair<uint32, uint32>> sFired;   // (instance, spell)
                static std::mutex sFiredLock;
                std::pair<uint32, uint32> const key{me->GetInstanceId(), info->trap.spellId};
                std::lock_guard<std::mutex> guard(sFiredLock);
                if (go->getLootState() == GO_ACTIVATED || go->getLootState() == GO_JUST_DEACTIVATED)
                    sFired.insert(key);
                if (!sFired.count(key))
                    continue;
            }
            Unit* owner = go->GetOwner();
            if (owner ? !owner->IsHostileTo(me) : !go->IsHostileTo(me))
                continue;
            out.push_back({go->GetPositionX(), go->GetPositionY(), std::max(float(info->trap.radius), radius) + 1.5f});
        }
        // A creature about to die that bursts as it does (a Firesworn's eruption, 4k fire in 15 yd
        // against 2.7k-health rogues - 2026-11-03, nine dead to it on Garr): its circle is fire already
        // below a fifth of its health. Everyone but the one holding it steps out; the ranged finish it.
        {
            std::list<Unit*> foes;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, range);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(foes, check);
            Cell::VisitAllObjects(me, searcher, range);
            // ...at a third of its health, half while slowed: at a fifth Garr's Magma Shackles (-60 % speed) held
            // four rogues inside the circle until it went off 3 s later (2026-11-15, R3W0 MC, nine mages on it)
            float const burstAt = me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ? 50.0f : 35.0f;
            for (Unit* u : foes)
                if (u->IsCreature() && u->IsAlive() && u->IsInCombat() &&
                    u->GetHealthPercent() <= (SuiTactic::On(me, SuiTactic::BurstEarlySlowed, u) ? burstAt : 20.0f) &&
                    u->GetVictim() != me)
                    if (float const r = SuiRaidTelemetry::EntryDeathBurstRadius(u->GetEntry()))
                        out.push_back({u->GetPositionX(), u->GetPositionY(), r + 2.0f});
        }
        // Impacts forecast from a chain of fixed-spot spells (a breath across a room).
        {
            std::vector<std::tuple<float, float, float>> fc;
            SuiRaidTelemetry::ForecastHazards(me, range, fc);
            for (auto const& f : fc)
                out.push_back({std::get<0>(f), std::get<1>(f), std::get<2>(f) + 2.0f});
        }
        // Units that pulse area damage around themselves: an enemy wearing it (an inferno), or a
        // member carrying a bomb that will go off around them.
        std::list<Unit*> units;
        MaNGOS::AnyUnitInObjectRangeCheck unitCheck(me, range);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> unitSearcher(units, unitCheck);
        Cell::VisitAllObjects(me, unitSearcher, range);
        for (Unit* u : units)
        {
            if (u == me || !u->IsAlive())
                continue;
            bool const enemy = u->IsHostileTo(me);
            bool const member = !enemy && u->IsPlayer() && me->GetGroup() &&
                me->GetGroup()->IsMember(u->GetObjectGuid());
            if (!enemy && !member)
                continue;
            // An area around a unit reaches from its edge to mine: the server's area search adds both
            // bodies' sizes to the radius (2026-09-25: bots stepped to 22 yd of Baron Geddon's centre
            // and still burned in his 20 yd Inferno; 2026-11-06: a 1.5 yd margin with no sizes sent
            // three healers "out of the fire" to 21-22 yd of him - dead to its last 5000 tick).
            float const edge = enemy ? u->GetObjectBoundingRadius() + me->GetObjectBoundingRadius() + 1.5f : 2.0f;
            if (enemy && LightPulse(me, u))
                continue;
            if (float const r = PulsingAreaRadius(u, member))
                out.push_back({u->GetPositionX(), u->GetPositionY(), r + edge});
            else if (enemy)
                if (float const pr = SuiRaidTelemetry::PulseRadius(u))
                    out.push_back({u->GetPositionX(), u->GetPositionY(), pr + edge});
        }
    }
    // Molten or poisonous liquid (lava, slime) at a spot: environmental damage every tick.
    bool Molten(Player* me, float x, float y, float z)
    {
        GridMapLiquidStatus const res = me->GetTerrain()->getLiquidStatus(x, y, z + 0.01f,
            MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME, nullptr);
        return (res & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)) != 0;
    }

    // Idle creatures that would attack us (not in any fight): the positions a repositioning step
    // must keep out of reach of (2026-09-24: the raid's spreading and stepping drifted into the
    // Core Hound pack beside Lucifron, and Lucifron joined).
    void CollectIdleHostiles(Player* me, float range, std::vector<GroundHazard>& out)
    {
        // A patrolling boss's walk counts wherever on it he is now (2026-11-04: with Baron Geddon at the
        // far end of his loop his walk beside the fight was "clear"; a healer dodging a cone stood on it
        // and he came round 10 s later): bosses are searched out to 320 yd, everything else to `range`.
        float const wide = std::max(range, 320.0f);
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, wide);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(me, searcher, wide);
        for (Creature* c : near)
            if (c->IsAlive() && !c->IsInCombat() && !c->IsTotem() && !c->IsCivilian() &&
                c->GetCreatureType() != CREATURE_TYPE_CRITTER && c->IsHostileTo(me) &&
                (me->IsWithinDistInMap(c, range) ||
                 (c->IsWorldBoss() && c->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)))
            {
                // an elite sees further than the 20 yd it is credited with once the raid spreads round it
                // [TACTICS] 2026-11-16 R3W4 MC Geddon: a mage stepped to 29 yd of a sleeping Lava pack and it aggroed
                // (Geddon 44 %, 40/40 dead): these kinds get 34 yd. Gated by SuiTactic::WideIdlePackReach.
                float const reach = SuiTactic::On(me, SuiTactic::WideIdlePackReach, c) ? 34.0f : (c->IsElite() ? 28.0f : 25.0f);
                out.push_back({c->GetPositionX(), c->GetPositionY(), reach});
                // A wanderer's reach is its whole wander circle, not where it stands this second
                // (2026-11-06: a mage's cone step kept 28 yd off Shazzrah; he wandered 20 yd toward
                // him, aggroed at 18 yd and joined a Lava Reaver pack's fight - all 39 dead).
                if (c->GetDefaultMovementType() == RANDOM_MOTION_TYPE && c->GetWanderDistance() > 0.0f)
                {
                    float hx, hy, hz;
                    c->GetRespawnCoord(hx, hy, hz);
                    out.push_back({hx, hy, (c->IsElite() ? 28.0f : 25.0f) + c->GetWanderDistance()});
                }
                // A patrol's whole route is its reach (it walks back into the fight): sampled every 5 yd,
                // not only at its waypoints (2026-11-04: Baron Geddon runs his loop in ~30 s; healers
                // stepping out of a cone stood 21 yd from his walk between two waypoints and he took them).
                if (c->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE && c->HasStaticDBSpawnData())
                    if (WaypointPath const* path = sWaypointMgr.GetDefaultPath(c->GetEntry(), c->GetDBTableGUIDLow()))
                    {
                        float const r = c->IsWorldBoss() ? 26.0f : 22.0f;
                        WaypointNode const* prev = nullptr;
                        for (auto const& node : *path)
                        {
                            WaypointNode const& n = node.second;
                            if (prev)
                            {
                                float const dx = n.x - prev->x, dy = n.y - prev->y;
                                float const len = std::sqrt(dx * dx + dy * dy);
                                for (float s = 5.0f; s < len; s += 5.0f)
                                {
                                    float const x = prev->x + dx * s / len, y = prev->y + dy * s / len;
                                    if (me->GetDistance2d(x, y) < range + r)
                                        out.push_back({x, y, r});
                                }
                            }
                            if (me->GetDistance2d(n.x, n.y) < range + r)
                                out.push_back({n.x, n.y, r});
                            prev = &n;
                        }
                    }
            }
    }

    // Idle world bosses' reach alone (a wanderer's whole circle): never an escape spot - standing in the fire costs one
    // member, pulling a boss costs the raid (2026-11-11, week 1, Baron Geddon at 35 %: a healer's two "out of the fire"
    // steps ended 3 yd inside Shazzrah's wander circle, the idle penalty being only a score; he joined, all 40 died).
    void CollectIdleBosses(Player* me, std::vector<GroundHazard>& out)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, 120.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(me, searcher, 120.0f);
        for (Creature* c : near)
            if (c->IsAlive() && c->IsWorldBoss() && !c->IsInCombat() && c->IsHostileTo(me))
            {
                out.push_back({c->GetPositionX(), c->GetPositionY(), 30.0f});
                // round its home as far as it has wandered or may wander (same day: Shazzrah 23 yd from home walked up
                // to an off-tank's fire step 33 yd from where he had stood)
                float hx, hy, hz;
                c->GetRespawnCoord(hx, hy, hz);
                float const roamed = std::max(c->GetWanderDistance(), c->GetDistance2d(hx, hy));
                if (roamed > 0.0f)
                    out.push_back({hx, hy, 35.0f + roamed});
            }
    }

    // A spot the navmesh walks to from here in a short path - not a rock top or a pocket that looks
    // near and is not connected (2026-09-25: escapes onto a plateau beside Baron Geddon left healers
    // with no path back and no sight of the tank - and Geddon reset, unable to reach them).
    bool Walkable(Player* me, float x, float y, float z, float slack = 2.0f)
    {
        PathInfo path(me);
        path.calculate(x, y, z, false);
        if (path.getPathType() & PATHFIND_NOPATH)
            return false;
        PointsArray const& pts = path.getPath();
        if (pts.empty() || (pts.back() - Vector3(x, y, z)).length() > 3.0f)
            return false;
        float len = 0.0f;
        for (size_t k = 1; k < pts.size(); ++k)
            len += (pts[k] - pts[k - 1]).length();
        return len <= me->GetDistance(x, y, z) * slack + 5.0f;
    }

    bool InHazard(std::vector<GroundHazard> const& hazards, float x, float y);

    bool NearIdleHostile(Player* me, float x, float y)
    {
        std::vector<GroundHazard> idle;
        CollectIdleHostiles(me, 60.0f, idle);
        return InHazard(idle, x, y);
    }

    bool InHazard(std::vector<GroundHazard> const& hazards, float x, float y)
    {
        for (GroundHazard const& h : hazards)
            if ((x - h.x) * (x - h.x) + (y - h.y) * (y - h.y) < h.r * h.r)
                return true;
        return false;
    }

    // The straight way from (x0, y0) to (x1, y1) passes through a circle I do not stand in now
    // (2026-09-25: a rogue out of the near end of Onyxia's breath stepped back across the line).
    bool CrossesNewHazard(std::vector<GroundHazard> const& hazards, float x0, float y0, float x1, float y1)
    {
        for (GroundHazard const& h : hazards)
        {
            float const r2 = h.r * h.r;
            if ((x0 - h.x) * (x0 - h.x) + (y0 - h.y) * (y0 - h.y) < r2)
                continue;
            for (float f : {0.2f, 0.4f, 0.6f, 0.8f})
            {
                float const x = x0 + (x1 - x0) * f, y = y0 + (y1 - y0) * f;
                if ((x - h.x) * (x - h.x) + (y - h.y) * (y - h.y) < r2)
                    return true;
            }
        }
        return false;
    }
}

// Onyxia's whelp spawn points (boss_onyxia afSpawnLocations): a breath escape or lane spot of a non-tank keeps 12 yd off
// them (R4W0 Onyxia drill 3 pull 4: a mage's escape spot was 2 yd from the east spawn; a wave appeared on him, his
// Arcane Explosion took six whelps and they killed him and two healers beside him). The callers check their own rows.
static float const kOnyWhelpSpawn[2][2] = { { -30.127f, -254.463f }, { -30.817f, -177.106f } };

// The keep-off radius of a whelp spawn for my role: tanks none (they hold the posts 8 yd inside), melee 12 yd, healers
// and casters 20 yd - a new wave runs at the nearest member (R4W0 drill 3 pull 8: eight whelps on two priests 10-13 yd
// from the east spawn).
static float OnyWhelpSpawnKeepOff(Player* me)
{
    AiBotAI* ai = dynamic_cast<AiBotAI*>(me->AI());
    CombatBotRoles const role = ai ? ai->GetCombatActiveRole() : ROLE_INVALID;
    if (role == ROLE_TANK)
        return 0.0f;
    return role == ROLE_HEALER || role == ROLE_RANGE_DPS ? 20.0f : 12.0f;
}

static bool NearOnyWhelpSpawn(Player* me, float x, float y)
{
    float const r = OnyWhelpSpawnKeepOff(me);
    for (auto const& p : kOnyWhelpSpawn)
        if ((x - p[0]) * (x - p[0]) + (y - p[1]) * (y - p[1]) < r * r)
            return true;
    return false;
}

// [TACTICS] Out of the fire: a member standing in a hostile damaging area steps to the nearest
// clear spot; a melee member or tank prefers a spot still next to its target (the target follows
// a tank), a caster the shortest step. What a player sees on the ground, nothing more.
// [TACTICS] 2026-11-18 R4W0 Onyxia att. 1 (39 %): one Deep Breath at +126 s killed five mages and a priest. Its chain of
// twelve fixed impacts (spell_target_position, 30 yd each) was forecast 6 s ahead, but the general step-out below searched
// rings to 25 yd, refused any path into the next circle of the chain and pulled toward the raid standing on the line, and
// the facing-based line step in AvoidFrontalCone re-aimed every 0.5 s onto the room's centre line. Scoped
// (SuiTactic::DeepBreathEscape): the nearest pathable spot outside every forecast circle is chosen ONCE and held for the
// breath; a member pushed off it walks back; arrived, it casts as usual.
bool AiBotAI::EscapeBreathForecast(uint32 now)
{
    if (now < m_breathEscUntilMs)
    {
        if (me->GetDistance2d(m_breathEscX, m_breathEscY) <= 2.5f)
            return false;   // there: free to cast
        if (me->IsNonMeleeSpellCasted(false))
            me->InterruptNonMeleeSpells(false);
        if (!me->IsMoving() || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            me->GetMotionMaster()->MovePoint(0, m_breathEscX, m_breathEscY, m_breathEscZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }
    Creature* boss = me->FindNearestCreature(10184, 150.0f, true);
    if (!boss || !SuiTactic::On(me, SuiTactic::DeepBreathEscape, boss))
        return false;
    std::vector<std::tuple<float, float, float>> fc;
    SuiRaidTelemetry::ForecastHazards(me, 80.0f, fc);
    std::vector<GroundHazard> big;
    for (auto const& f : fc)
        if (std::get<2>(f) >= 20.0f)
            big.push_back({std::get<0>(f), std::get<1>(f), std::get<2>(f) + 3.0f});
    if (big.empty() || !InHazard(big, me->GetPositionX(), me->GetPositionY()))
        return false;
    struct Spot { float x, y, z, d; };
    std::vector<Spot> spots;
    for (float dist : {6.0f, 10.0f, 14.0f, 18.0f, 22.0f, 26.0f, 30.0f, 34.0f, 38.0f, 42.0f, 48.0f})
        for (int i = 0; i < 24; ++i)
        {
            float const ang = i * float(M_PI) / 12.0f;
            float x = me->GetPositionX() + dist * std::cos(ang), y = me->GetPositionY() + dist * std::sin(ang);
            float z = me->GetPositionZ();
            if (InHazard(big, x, y) || NearOnyWhelpSpawn(me, x, y))
                continue;
            me->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - me->GetPositionZ()) > 8.0f || Molten(me, x, y, z))
                continue;
            spots.push_back({x, y, z, dist});
        }
    uint32 probes = 0;
    for (Spot const& s : spots)
    {
        if (++probes > 14)
            break;
        PathInfo path(me);
        path.calculate(s.x, s.y, s.z, false);
        if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            continue;
        float len = 0.0f;
        PointsArray const& pts = path.getPath();
        for (size_t k = 1; k < pts.size(); ++k)
            len += (pts[k] - pts[k - 1]).length();
        if (len > s.d * 1.5f + 4.0f)
            continue;
        m_breathEscUntilMs = now + 14000;
        m_breathEscX = s.x;
        m_breathEscY = s.y;
        m_breathEscZ = s.z;
        if (me->IsNonMeleeSpellCasted(false))
            me->InterruptNonMeleeSpells(false);
        SuiRaidTelemetry::OnReposition(me, "breath");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BREATH] %s: out of %s's Deep Breath -> (%.1f, %.1f), %.0f yd walk, %u circles",
            me->GetName(), boss->GetName(), s.x, s.y, len, uint32(big.size()));
        me->GetMotionMaster()->MovePoint(0, s.x, s.y, s.z, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BREATH] %s: inside %s's Deep Breath and no spot found (%u candidates, %u probed)",
        me->GetName(), boss->GetName(), uint32(spots.size()), probes);
    return false;
}

// [TACTICS] 2026-11-19 R4W0/R4W1 Onyxia drills 4-5: healers took ~53 of ~100 Deep Breath deaths (10 of 40 members), and a
// breath that killed healers began the whelp cascade in most phase-2 wipes. Her breath is known ahead: at each flight point
// she waits 15-25 s, then flies on (70 %) or breathes that point's own spell (30 %; boss_onyxia DoMovement). While she waits,
// a HEALER more than 12 yd deep in that lane steps to the nearest spot outside it - at most 26 yd, in sight of a tank, off
// the whelp spawns - and holds it until she moves on. Only healers: the all-member version (deep-breath-lane-clear, reverted
// 11-19) moved 100-160 members a pull. Gated by SuiTactic::HealerLaneClear.
// 2026-11-22 R4W12 att. 1 (57 %): the hold re-walked only when no point walk was under way, so a heal-range walk to a tank
// 2 s after the lane walk kept Gankreaper in the lane and it died there to the breath; only a walk to the lane spot counts now.
namespace
{
    struct OnyFlightPoint { float x, y; uint32 breath; };
    // boss_onyxia.cpp aMoveData (point, its Deep Breath spell)
    OnyFlightPoint const kOnyFlight[8] = {
        { 10.2191f, -247.912f, 18617 }, { -31.4963f, -250.123f, 18576 }, { -63.5156f, -240.096f, 18564 },
        { -65.8444f, -213.809f, 18351 }, { -58.2509f, -189.020f, 18596 }, { -33.5561f, -182.682f, 18609 },
        { 6.8951f, -180.246f, 18584 }, { 22.8763f, -217.152f, 17086 } };
}

bool AiBotAI::HealerClearBreathLane(uint32 now)
{
    if (GetCombatActiveRole() != ROLE_HEALER)
        return false;
    Creature* ony = me->FindNearestCreature(10184, 150.0f, true);
    if (!ony || !ony->IsInCombat() || !(ony->IsFlying() || ony->IsLevitating() || ony->IsHovering()) ||
        !SuiTactic::On(me, SuiTactic::HealerLaneClear, ony))
    {
        m_hLaneUntilMs = 0;
        return false;
    }
    int point = -1;
    if (!ony->IsMoving())
        for (int i = 0; i < 8; ++i)
            if (ony->GetDistance2d(kOnyFlight[i].x, kOnyFlight[i].y) < 6.0f)
                point = i;
    if (point != m_hLanePoint)
    {
        m_hLanePoint = point;
        m_hLaneUntilMs = 0;
        m_hLaneNextMs = 0;
    }
    if (point < 0)
        return false;
    if (now < m_hLaneUntilMs)
    {
        if (me->GetDistance2d(m_hLaneX, m_hLaneY) <= 2.5f || me->IsNonMeleeSpellCasted(false))
            return false;   // there (free to heal), or finishing a heal first
        // (any point walk used to count as "on my way" - see the R4W12 note above: only a walk ending at the lane spot counts)
        G3D::Vector3 const dest = me->movespline->FinalDestination();
        bool const onMyWay = me->IsMoving() && !me->movespline->Finalized() &&
            (dest - G3D::Vector3(m_hLaneX, m_hLaneY, m_hLaneZ)).length() <= 3.0f;
        if (!onMyWay)
            me->GetMotionMaster()->MovePoint(0, m_hLaneX, m_hLaneY, m_hLaneZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }
    if (now < m_hLaneNextMs)
        return false;
    m_hLaneNextMs = now + 1000;
    std::vector<std::tuple<float, float, float>> chain;
    SuiRaidTelemetry::ChainImpacts(kOnyFlight[point].breath, me->GetMapId(), chain);
    std::vector<GroundHazard> lane, deep;
    for (auto const& c : chain)
        if (std::get<2>(c) >= 20.0f)
        {
            lane.push_back({ std::get<0>(c), std::get<1>(c), std::get<2>(c) + 3.0f });
            deep.push_back({ std::get<0>(c), std::get<1>(c), std::get<2>(c) + 3.0f - 12.0f });
        }
    if (lane.empty() || !InHazard(deep, me->GetPositionX(), me->GetPositionY()))
        return false;
    // the tanks I heal: any living tank of the group within 45 yd of the spot must be in its sight
    std::vector<Player*> tanks;
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
        if (Player* m = itr->getSource())
            if (m->IsAlive() && m->IsInMap(me))
                if (AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI()))
                    if (ai->GetCombatActiveRole() == ROLE_TANK)
                        tanks.push_back(m);
    uint32 probes = 0;   // path probes, at most 12 a second
    for (float dist : {6.0f, 10.0f, 14.0f, 18.0f, 22.0f, 26.0f})
        for (int i = 0; i < 24; ++i)
        {
            float const ang = i * float(M_PI) / 12.0f;
            float x = me->GetPositionX() + dist * std::cos(ang), y = me->GetPositionY() + dist * std::sin(ang);
            float z = me->GetPositionZ();
            if (InHazard(lane, x, y) || NearOnyWhelpSpawn(me, x, y))
                continue;
            me->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - me->GetPositionZ()) > 8.0f || Molten(me, x, y, z))
                continue;
            bool seesTank = tanks.empty();
            for (Player* t : tanks)
                if (t->GetDistance2d(x, y) <= 40.0f &&
                    me->GetMap()->isInLineOfSight(x, y, z + 2.0f, t->GetPositionX(), t->GetPositionY(), t->GetPositionZ() + 2.0f))
                {
                    seesTank = true;
                    break;
                }
            if (!seesTank)
                continue;
            if (++probes > 12)
                return false;
            PathInfo path(me);
            path.calculate(x, y, z, false);
            if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                continue;
            float len = 0.0f;
            PointsArray const& pts = path.getPath();
            for (size_t k = 1; k < pts.size(); ++k)
                len += (pts[k] - pts[k - 1]).length();
            if (len > dist * 1.5f + 4.0f)
                continue;
            m_hLaneUntilMs = now + 30000;
            m_hLaneX = x;
            m_hLaneY = y;
            m_hLaneZ = z;
            SuiRaidTelemetry::OnReposition(me, "breath lane");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BREATH] %s: healer out of %s's Deep Breath lane at flight point %d -> (%.1f, %.1f), %.0f yd walk",
                me->GetName(), ony->GetName(), point, x, y, len);
            if (!me->IsNonMeleeSpellCasted(false))
                me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
            return true;
        }
    return false;
}

bool AiBotAI::AvoidGroundHazard()
{
    if (!me->IsAlive() || !me->GetGroup() || m_possessed || m_suiManual)
        return false;
    uint32 const now = WorldTimer::getMSTime();
    // Waiting outside a pulse my target stands in: no chase back into it between checks.
    if (now < m_hazardMeleeHoldUntil && now < m_hazardNextMs)
        return true;
    if (now < m_hazardNextMs)
        return false;
    m_hazardNextMs = now + 400;

    // [TACTICS] 2026-11-18 R4W0 Onyxia: a forecast Deep Breath first - escaped once, held (SuiTactic::DeepBreathEscape,
    // the gate is repeated inside for the held spot).
    if (me->GetMapId() == SuiTactic::kOnyxia)
        if (Creature* ony = me->FindNearestCreature(10184, 150.0f, true))
        {
            if (SuiTactic::On(me, SuiTactic::DeepBreathEscape, ony) && EscapeBreathForecast(now))
                return true;
            // (Gated by SuiTactic::HealerLaneClear inside: a healer deep in her next breath's lane steps out early)
            if (HealerClearBreathLane(now))
                return true;
            // [TACTICS] 2026-11-19 R4W0 Onyxia drill 3 pull 8: a new whelp wave runs at the nearest member - eight went for
            // two priests 10-13 yd from the east spawn. From phase 2 a healer or caster inside 20 yd of a spawn point
            // walks out to 23 yd (SuiTactic::WhelpSpawnKeepOut).
            if (ony->IsInCombat() && ony->GetHealthPercent() <= 66.0f && SuiTactic::On(me, SuiTactic::WhelpSpawnKeepOut, ony))
            {
                CombatBotRoles const kr = GetCombatActiveRole();
                if (kr == ROLE_HEALER || kr == ROLE_RANGE_DPS)
                    for (auto const& p : kOnyWhelpSpawn)
                    {
                        float dx = me->GetPositionX() - p[0], dy = me->GetPositionY() - p[1];
                        float const d = std::sqrt(dx * dx + dy * dy);
                        if (d >= 20.0f)
                            continue;
                        // out to 23 yd from the spawn: along my bearing, turned toward the room centre until the spot
                        // sees her landing spot (drill 3 pull 10: straight back from the east spawn a healer stood
                        // behind the rock edge at (-8.8, -263.1), no sight of the tanks, and they died to the whelps)
                        float const cx = -8.860f - p[0], cy = -212.752f - p[1], cn = std::sqrt(cx * cx + cy * cy);
                        float bx = cx / cn, by = cy / cn;
                        if (d >= 1.0f)
                        {
                            bx = dx / d;
                            by = dy / d;
                        }
                        float x = 0.0f, y = 0.0f, z = me->GetPositionZ();
                        for (float t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                        {
                            float ux = bx * (1.0f - t) + cx / cn * t, uy = by * (1.0f - t) + cy / cn * t;
                            float const un = std::max(0.1f, std::sqrt(ux * ux + uy * uy));
                            x = p[0] + ux / un * 23.0f;
                            y = p[1] + uy / un * 23.0f;
                            z = me->GetPositionZ();
                            me->UpdateAllowedPositionZ(x, y, z);
                            if (std::fabs(z - me->GetPositionZ()) <= 8.0f && !Molten(me, x, y, z) &&
                                me->GetMap()->isInLineOfSight(x, y, z + 2.0f, -8.860f, -212.752f, -85.5f))
                                break;
                        }
                        if (me->IsNonMeleeSpellCasted(false) && me->GetHealthPercent() > 50.0f && d > 12.0f)
                            break;   // finish the cast first
                        m_hazardNextMs = now + 1000;
                        SuiRaidTelemetry::OnReposition(me, "whelp spawn");
                        if (now >= m_spawnKeepLogMs)
                        {
                            m_spawnKeepLogMs = now + 5000;
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-WHELP] %s: %.0f yd from a whelp spawn point - out to (%.1f, %.1f)",
                                me->GetName(), d, x, y);
                        }
                        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                        return true;
                    }
            }
        }

    // I carry a bomb: away from every member until it has gone off (2026-09-24, Baron Geddon's
    // Living Bomb - recognised from its data: a harmful aura that triggers area damage around me).
    uint32 bombMs = 0, bombAura = 0;
    // ...except a tank holding a world boss: the boss follows it and turns its breath with it, and the raid keeps
    // clear of the carrier anyway (2026-11-14, Onyxia phase 3: tanks walked Engulfing Flames away 32 times a fight;
    // she turned after them and her Flame Breath killed five beside her while she was tanked).
    bool holdsBoss = false;
    if (GetCombatActiveRole() == ROLE_TANK)
        for (Unit* a : me->GetAttackers())
            if (a && a->IsAlive() && a->GetVictim() == me && a->IsCreature() && static_cast<Creature*>(a)->IsWorldBoss())
                holdsBoss = true;
    if (float const bomb = holdsBoss ? 0.0f : PulsingAreaRadius(me, true, &bombMs, &bombAura))
    {
        Group* group = me->GetGroup();
        // Where the other carriers are walking counts as where they stand: several carriers chose their spots on their
        // own and walked to the same corner, burning each other (2026-11-14, Onyxia dev attempt 12: one Fireball gave
        // Engulfing Flames to four melee on a whelp; all four died to each other's flames within 6 s).
        struct BombSpot { uint64 who; float x, y; uint32 until; };
        static std::mutex bombSpotLock;
        static std::vector<BombSpot> bombSpots;
        std::vector<BombSpot> others;
        {
            std::lock_guard<std::mutex> guard(bombSpotLock);
            bombSpots.erase(std::remove_if(bombSpots.begin(), bombSpots.end(), [now](BombSpot const& b) { return now >= b.until; }),
                bombSpots.end());
            for (BombSpot const& b : bombSpots)
                if (b.who != me->GetObjectGuid().GetRawValue())
                    others.push_back(b);
        }
        auto nearest = [&](float x, float y)
        {
            float best = 1e9f;
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                if (Player* m = itr->getSource())
                    if (m != me && m->IsAlive() && m->IsInMap(me))
                        best = std::min(best, m->GetDistance2d(x, y));
            for (BombSpot const& b : others)
                best = std::min(best, float(std::sqrt((b.x - x) * (b.x - x) + (b.y - y) * (b.y - y))) - 4.0f);
            return best;
        };
        float const need = bomb + 4.0f;
        if (nearest(me->GetPositionX(), me->GetPositionY()) >= need)
            return me->IsMoving();   // far enough: stay put (keep casting)
        std::vector<GroundHazard> idle;
        CollectIdleHostiles(me, 60.0f, idle);
        // ...nor into an idle boss's roam (2026-11-11, week 2: a mage carried Geddon's Living Bomb into Shazzrah, 14 yd
        // from his home - the plain idle circle counts only where he stood)
        CollectIdleBosses(me, idle);
        // ...and never into fire on the way (2026-09-25: Baron Geddon's tank carried his Living
        // Bomb away from the raid straight into his Inferno).
        std::vector<GroundHazard> burning;
        CollectGroundHazards(me, burning);
        {
            // my own bomb is in that list too: drop the circle centred on me
            float const mx = me->GetPositionX(), my = me->GetPositionY();
            burning.erase(std::remove_if(burning.begin(), burning.end(), [mx, my](GroundHazard const& h)
                { return (h.x - mx) * (h.x - mx) + (h.y - my) * (h.y - my) < 1.0f; }), burning.end());
        }
        float bestX = 0, bestY = 0, bestZ = 0, bestD = 1e9f;
        for (float dist : {12.0f, 18.0f, 24.0f, 30.0f, 38.0f})
            for (int i = 0; i < 16; ++i)
            {
                float const ang = i * float(M_PI) / 8.0f;
                float x = me->GetPositionX() + dist * std::cos(ang), y = me->GetPositionY() + dist * std::sin(ang);
                float z = me->GetPositionZ();
                me->UpdateAllowedPositionZ(x, y, z);
                if (std::fabs(z - me->GetPositionZ()) > 6.0f || Molten(me, x, y, z) || InHazard(idle, x, y) ||
                    InHazard(burning, x, y) || nearest(x, y) < need)
                    continue;
                if (dist < bestD && Walkable(me, x, y, z))
                {
                    bestD = dist;
                    bestX = x;
                    bestY = y;
                    bestZ = z;
                }
            }
        if (bestD > 1e8f)
            return false;
        if (me->IsNonMeleeSpellCasted(false))
            me->InterruptNonMeleeSpells(false);
        {
            std::lock_guard<std::mutex> guard(bombSpotLock);
            bombSpots.erase(std::remove_if(bombSpots.begin(), bombSpots.end(),
                [this](BombSpot const& b) { return b.who == me->GetObjectGuid().GetRawValue(); }), bombSpots.end());
            bombSpots.push_back({me->GetObjectGuid().GetRawValue(), bestX, bestY, now + bombMs + 1000});
        }
        m_repositionUntilMs = now + bombMs + 500;
        SuiRaidTelemetry::OnReposition(me, "bomb");
        if (Player* anchor = FindEscortBoss())
        {
            m_hazardHoldUntil = now + bombMs + 1000;
            m_hazardAnchorX = anchor->GetPositionX();
            m_hazardAnchorY = anchor->GetPositionY();
        }
        m_hazardNextMs = now + 1000;
        {
            std::string from = "?";
            if (SpellAuraHolder const* h = me->GetSpellAuraHolder(bombAura))
                if (Unit* c = h->GetCaster())
                    from = c->GetName();
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: carrying a bomb (%.0f yd, aura %u from %s) - away from the raid",
                me->GetName(), bomb, bombAura, from.c_str());
        }
        me->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }

    std::vector<GroundHazard> hazards;
    CollectGroundHazards(me, hazards);
    // A melee damage dealer is out of a pulser's circle before its next burst is due: from 3.4k
    // health the first three ticks of a ramping inferno kill a rogue still walking out (2026-09-25,
    // Baron Geddon: eight rogues per attempt). The rhythm is the one this creature has shown.
    if (GetCombatActiveRole() == ROLE_MELEE_DPS)
        if (Unit* foe = me->GetVictim())
            if (foe->IsCreature() && foe->IsAlive())
                if (float const pr = SuiRaidTelemetry::EntryPulseRadius(foe->GetEntry()))
                {
                    uint32 const age = LightPulse(me, foe) ? 0u : SuiRaidTelemetry::PulseAge(foe);
                    if (age >= 17500 && age <= 30000)
                        hazards.push_back({foe->GetPositionX(), foe->GetPositionY(),
                            pr + foe->GetObjectBoundingRadius() + me->GetObjectBoundingRadius() + 1.5f});
                }
    // A healer or caster never stands inside an idle elite's reach mid-fight: its ordinary moves (range, sight of
    // the tank) do not consult it, and the pack wakes on it (2026-11-15, R3W0 MC: Baron Geddon at 49 % and 67 % - the
    // Firewalker pack joined at +47 s and +93 s on healers standing 21-29 yd from it). Its reach is fire to step out of.
    {
        // ...and the tanks and melee too: out of Baron Geddon's Inferno the tanks stepped to (637, -818) and (641, -808),
        // onto his Firewalker pack, which joined at +44 s and +117 s (2026-11-15, R3W1 MC, both attempts 40/40 and 39/40)
        if (me->IsInCombat() && me->GetGroup() && me->GetMap()->IsRaid() && SuiTactic::On(me, SuiTactic::IdleReachHazard) &&
            (GetCombatActiveRole() == ROLE_HEALER || GetCombatActiveRole() == ROLE_RANGE_DPS ||
             SuiTactic::On(me, SuiTactic::IdleReachAllRoles)))
        {
            std::vector<GroundHazard> reach;
            CollectIdleHostiles(me, 40.0f, reach);
            for (GroundHazard const& h : reach)
                hazards.push_back(h);
        }
    }
    bool const molten = Molten(me, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
    if (!molten && (hazards.empty() || !InHazard(hazards, me->GetPositionX(), me->GetPositionY())))
    {
        // Out of it, but my melee target stands inside a pulse (a boss's inferno): stay out until
        // it stops instead of chasing back in (2026-09-24: the tanks and rogues stepped out of
        // Baron Geddon's Inferno and walked straight back to him, ten dead in two seconds).
        m_hazardMeleeHoldUntil = 0;
        Unit* victim = me->GetVictim();
        CombatBotRoles const role = GetCombatActiveRole();
        if (victim && (role == ROLE_TANK || role == ROLE_MELEE_DPS) && !hazards.empty() &&
            InHazard(hazards, victim->GetPositionX(), victim->GetPositionY()))
        {
            m_hazardMeleeHoldUntil = now + 450;
            if (me->IsMoving() && me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                me->StopMoving();
            return true;
        }
        // [TACTICS] 2026-11-16 R3W4 MC Geddon: every Inferno step pulled him after his tank and nothing walked him back -
        // 90 yd south in 140 s, onto the healers in a corner, where one Inferno found no way out for ten (4 tanks). Between
        // pulses the tank he is on walks back to the fight's camp and he follows. Gated by SuiTactic::TankHoldsBossAtCamp.
        if (victim && role == ROLE_TANK && victim->IsAlive() && victim->GetVictim() == me &&
            SuiTactic::On(me, SuiTactic::TankHoldsBossAtCamp, victim))
        {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            if (SuiAutopilot::FightAnchor(me, cx, cy, cz) && victim->GetDistance2d(cx, cy) > 10.0f &&
                me->GetDistance2d(cx, cy) > 2.5f)
            {
                m_hazardMeleeHoldUntil = now + 450;
                if (!me->IsMoving() || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: walks %s back to the camp (%.1f, %.1f), %.0f yd off",
                        me->GetName(), victim->GetName(), cx, cy, victim->GetDistance2d(cx, cy));
                    me->GetMotionMaster()->MovePoint(0, cx, cy, cz, MOVE_PATHFINDING | MOVE_RUN_MODE);
                }
                return true;
            }
        }
        return false;
    }
    Unit* target = me->GetVictim();
    CombatBotRoles const role = GetCombatActiveRole();
    bool const melee = target && (role == ROLE_TANK || role == ROLE_MELEE_DPS);
    std::vector<GroundHazard> idle;
    CollectIdleHostiles(me, 60.0f, idle);
    std::vector<GroundHazard> idleBosses;
    CollectIdleBosses(me, idleBosses);
    // Out toward the raid, not away from it: a boss follows its tank out of every inferno, and
    // escapes that pick any side walk the fight across the room into the next pack (2026-09-25:
    // Baron Geddon drifted 100 yd in four Infernos and a Firewalker pack joined).
    float ax = 0.0f, ay = 0.0f;
    uint32 an = 0;
    if (Group* group = me->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->getSource())
                if (m != me && m->IsAlive() && m->IsInWorld() && m->IsInMap(me) &&
                    me->IsWithinDistInMap(m, 60.0f))
                {
                    ax += m->GetPositionX();
                    ay += m->GetPositionY();
                    ++an;
                }
    float const pull = an ? (role == ROLE_TANK ? 0.8f : 0.3f) : 0.0f;
    if (an)
    {
        ax /= an;
        ay /= an;
    }
    // A tank steps toward the fight's anchor (the raid's camp), not the drifting middle of the raid.
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    if (role == ROLE_TANK && SuiAutopilot::FightAnchor(me, fx, fy, fz))
    {
        ax = fx;
        ay = fy;
    }
    float bestX = 0, bestY = 0, bestZ = 0, bestScore = 1e9f;
    struct EscapeSpot { float x, y, z, score; };
    std::vector<EscapeSpot> spots;
    for (float dist : {4.0f, 7.0f, 10.0f, 14.0f, 19.0f, 25.0f})
    {
        for (int i = 0; i < 16; ++i)
        {
            float const angle = i * float(M_PI) / 8.0f;
            float x = me->GetPositionX() + dist * std::cos(angle), y = me->GetPositionY() + dist * std::sin(angle);
            float z = me->GetPositionZ();
            if (InHazard(hazards, x, y) || CrossesNewHazard(hazards, me->GetPositionX(), me->GetPositionY(), x, y))
                continue;
            me->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - me->GetPositionZ()) > 6.0f || !me->IsWithinLOS(x, y, z + 1.5f) || Molten(me, x, y, z) ||
                InHazard(idleBosses, x, y))
                continue;
            float score = dist;
            if (melee)
                score += 2.0f * std::max(0.0f, target->GetDistance2d(x, y) - 4.0f);
            if (pull > 0.0f)
                score += pull * std::sqrt((x - ax) * (x - ax) + (y - ay) * (y - ay));
            if (InHazard(idle, x, y))
                score += 100.0f;
            spots.push_back({x, y, z, score});
        }
    }
    std::sort(spots.begin(), spots.end(), [](EscapeSpot const& a, EscapeSpot const& b) { return a.score < b.score; });
    uint32 probes = 0;
    for (EscapeSpot const& s : spots)
    {
        if (++probes > 8)
            break;
        if (!Walkable(me, s.x, s.y, s.z))
            continue;
        bestScore = s.score;
        bestX = s.x;
        bestY = s.y;
        bestZ = s.z;
        break;
    }
    if (bestScore > 1e8f)
    {
        // Cornered (a wall, lava, a ledge within 25 yd): look further out, without the straight
        // sight line, and take the nearest spot the navmesh walks to in a short path (2026-09-25:
        // Baron Geddon's tank stood through a whole Inferno, no step found).
        for (float dist : {30.0f, 36.0f, 42.0f})
        {
            for (int i = 0; i < 16 && bestScore > 1e8f; ++i)
            {
                float const angle = i * float(M_PI) / 8.0f;
                float x = me->GetPositionX() + dist * std::cos(angle), y = me->GetPositionY() + dist * std::sin(angle);
                float z = me->GetPositionZ();
                if (InHazard(hazards, x, y) || CrossesNewHazard(hazards, me->GetPositionX(), me->GetPositionY(), x, y))
                    continue;
                me->UpdateAllowedPositionZ(x, y, z);
                if (std::fabs(z - me->GetPositionZ()) > 8.0f || Molten(me, x, y, z) || InHazard(idle, x, y))
                    continue;
                PathInfo path(me);
                path.calculate(x, y, z, false);
                if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                    continue;
                float len = 0.0f;
                PointsArray const& pts = path.getPath();
                for (size_t k = 1; k < pts.size(); ++k)
                    len += (pts[k] - pts[k - 1]).length();
                if (len > dist * 1.6f)
                    continue;
                bestScore = dist;
                bestX = x;
                bestY = y;
                bestZ = z;
            }
            if (bestScore < 1e8f)
                break;
        }
    }
    if (bestScore > 1e8f)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: in the fire and no way out found", me->GetName());
        m_hazardNextMs = now + 1000;
        return false;
    }
    m_hazardNextMs = now + 1200;
    if (Player* anchor = FindEscortBoss())
    {
        m_hazardHoldUntil = now + 60000;
        m_hazardAnchorX = anchor->GetPositionX();
        m_hazardAnchorY = anchor->GetPositionY();
    }
    if (me->IsNonMeleeSpellCasted(false))
        me->InterruptNonMeleeSpells(false);
    CB_HIT(me->GetGUIDLow(), "cpp-combat: stepping out of a ground hazard"); m_repositionUntilMs = WorldTimer::getMSTime() + 3000; SuiRaidTelemetry::OnReposition(me, "hazard");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: out of the %s -> (%.1f, %.1f)", me->GetName(),
        molten ? "lava" : "fire", bestX, bestY);
    me->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return true;
}

// [TACTICS] Raid practice: whoever pulls aggro off the tanks brings the mob to a tank - Taunt is melee range in this game,
// so a tank can only take back what reaches it (2026-11-15, R3W1 MC: the four-elite pack by Geddon turned on healers and
// warlocks, who dodged away from it while five warriors chased it; not one Taunt landed in two wipes, 78 dead).
// A non-boss elite hitting a healer or a caster sends it to the nearest living tank; it stops once that mob is in the
// tank's reach. A cast in progress is finished unless the mob is killing me.
bool AiBotAI::PeelToTank()
{
    if (!me->IsAlive() || !me->IsInCombat() || m_possessed || m_suiManual)
        return false;
    Group* group = me->GetGroup();
    if (!group || !me->GetMap()->IsRaid())
        return false;
    CombatBotRoles const role = GetCombatActiveRole();
    if (role != ROLE_HEALER && role != ROLE_RANGE_DPS)
        return false;
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_peelNextMs)
        return false;
    m_peelNextMs = now + 400;

    Unit* mob = nullptr;
    bool whelpPeel = false;
    for (Unit* a : me->GetAttackers())
    {
        if (!a || !a->IsAlive() || !a->IsCreature() || a->GetVictim() != me)
            continue;
        // [TACTICS] 2026-11-19 R4W0 Onyxia drill 3 pulls 3-6: whelps killed 24-33 of 40 a pull in phase 2. Each whelp
        // attacks a random member (boss_onyxia summons, AttackStart); the one it chose stayed put while the post tanks
        // waited at the spawn points. Whoever a whelp is on walks it to the nearest tank outside her landing pair
        // (SuiTactic::WhelpsWalkedToTanks), as for an elite below.
        if (a->GetEntry() == 11262 && SuiTactic::On(me, SuiTactic::WhelpsWalkedToTanks, a))
        {
            if (me->GetDistance(a) > 12.0f)
                continue;
            mob = a;
            whelpPeel = true;
            break;
        }
        if (!static_cast<Creature*>(a)->IsElite() || static_cast<Creature*>(a)->IsWorldBoss() || a->HasBreakableByDamageCrowdControlAura())
            continue;
        // ...a real elite, not a summoned add or a kind that multiplies: Firelords' Lava Spawns sent healers and mages
        // walking to the tanks 87 times in one fight, the spawns multiplied to 211 and the raid wiped (2026-11-15, R3W2 MC)
        if (!SuiTactic::On(me, SuiTactic::PeelToTank, a))
            continue;   // its raid and kinds only (Geddon's elite packs)
        if (static_cast<Creature*>(a)->IsTemporarySummon() || !a->GetCreatorGuid().IsEmpty() || IsKnownMultiplier(a->GetEntry()))
            continue;
        if (me->GetDistance(a) > 12.0f)
            continue;   // a caster mob shooting from range: walking to a tank does not move it
        mob = a;
        break;
    }
    if (!mob)
        return false;

    Player* tank = nullptr;
    float best = 45.0f;
    // (a whelp is never walked to her landing pair: in phase 3 they stand in her front)
    std::vector<uint64> onyPair;
    if (whelpPeel)
        if (Creature* ony = me->FindNearestCreature(10184, 150.0f, true))
            onyPair = OnyxiaLandingPair(me, ony, group, me->GetMap());
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || m == me || !m->IsAlive() || !m->IsInMap(me))
            continue;
        AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
        if (!ai || ai->GetCombatActiveRole() != ROLE_TANK)
            continue;
        if (whelpPeel && std::find(onyPair.begin(), onyPair.end(), m->GetObjectGuid().GetRawValue()) != onyPair.end())
            continue;
        float const dm = me->GetDistance(m);
        if (dm < best)
        {
            best = dm;
            tank = m;
        }
    }
    if (!tank)
        return false;
    // It is in that tank's reach already: stay and cast, the tank takes it.
    if (mob->GetDistance(tank) <= 6.0f || me->GetDistance(tank) <= 4.0f)
        return false;
    if (me->IsNonMeleeSpellCasted(false))
    {
        if (me->GetHealthPercent() > 50.0f)
            return false;
        me->InterruptNonMeleeSpells(false);
    }
    // a spot 2.5 yd short of the tank, on my side of it
    float dx = me->GetPositionX() - tank->GetPositionX(), dy = me->GetPositionY() - tank->GetPositionY();
    float const len = std::max(0.5f, std::sqrt(dx * dx + dy * dy));
    float stopAt = 2.5f;
    // ...but outside a Flameguard's Fire Shield (19627: 249 fire every 2 s to all within 10 yd): the peeled healers stood at
    // the tank's feet beside the Flameguard he held and died to its pulse and the Lava Elementals' barrage (2026-11-15,
    // R3W2 MC: three wipes on Geddon's packs, 8-32 peels each, healers dead first). The mob follows me to that spot and the
    // tank steps to it; once I stand there I stay and cast.
    {
        std::list<Unit*> near;
        me->GetEnemyListInRadiusAround(tank, 25.0f, near);
        std::vector<Unit*> shields;
        for (Unit* u : near)
            if (u && u->IsAlive() && u->IsCreature() && SuiTactic::On(me, SuiTactic::PeelOutsideFireShield, u))
                shields.push_back(u);
        if (!shields.empty())
        {
            for (float d = 2.5f; d <= std::min(len, 20.0f); d += 1.5f)
            {
                float const px = tank->GetPositionX() + dx / len * d, py = tank->GetPositionY() + dy / len * d;
                bool clear = true;
                for (Unit* s : shields)
                    if (std::hypot(s->GetPositionX() - px, s->GetPositionY() - py) < 11.5f)
                        clear = false;
                stopAt = d;
                if (clear)
                    break;
            }
            if (std::fabs(len - stopAt) <= 2.0f)
                return false;   // I stand at that spot already: stay and cast
        }
    }
    float x = tank->GetPositionX() + dx / len * stopAt, y = tank->GetPositionY() + dy / len * stopAt, z = tank->GetPositionZ();
    // (a whelp is walked to a post tank only to 21 yd off that post's spawn point - the post takes whelps within 16 yd -
    // so the next wave does not take me; R4W0 Onyxia drill 3 pull 8, SuiTactic::WhelpSpawnKeepOut)
    if (whelpPeel)
        if (Creature* ony = me->FindNearestCreature(10184, 150.0f, true))
            if (SuiTactic::On(me, SuiTactic::WhelpSpawnKeepOut, ony))
                for (auto const& p : kOnyWhelpSpawn)
                {
                    float const sx = x - p[0], sy = y - p[1], sd = std::sqrt(sx * sx + sy * sy);
                    if (sd >= 20.0f)
                        continue;
                    float ux = sx, uy = sy, un = sd;
                    if (un < 1.0f)
                    {
                        ux = me->GetPositionX() - p[0];
                        uy = me->GetPositionY() - p[1];
                        un = std::max(0.1f, std::sqrt(ux * ux + uy * uy));
                    }
                    x = p[0] + ux / un * 21.0f;
                    y = p[1] + uy / un * 21.0f;
                    if (me->GetDistance2d(x, y) <= 2.5f)
                        return false;   // there: the post tank takes it from here
                }
    me->UpdateAllowedPositionZ(x, y, z);
    if (std::fabs(z - tank->GetPositionZ()) > 4.0f || Molten(me, x, y, z))
    {
        x = tank->GetPositionX();
        y = tank->GetPositionY();
        z = tank->GetPositionZ();
    }
    m_peelNextMs = now + 1200;
    m_repositionUntilMs = now + 1500;
    m_coneNextMs = now + 1500;
    SuiRaidTelemetry::OnReposition(me, "peel");
    if (now >= m_peelLogMs)
    {
        m_peelLogMs = now + 4000;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-PEEL] %s: %s (guid %u) is on me - to tank %s (%.0f yd; it is %.0f yd from the tank)",
            me->GetName(), mob->GetName(), mob->GetGUIDLow(), tank->GetName(), best, mob->GetDistance(tank));
    }
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return true;
}

// 2026-11-15 R3W3 Onyxia: she lands at 40 % on her landing spot with her threat list wiped (the script's DoResetThreat)
// and turns on whoever hits her first; the tanks were 66-88 yd away on the whelps and out of the fire, a warlock held her,
// she was feared off him onto a tank who stood with the casters in her front, and one Flame Breath killed six (wipe at 9 %).
// Every guide has the tanks at the landing spot before she comes down. While she is airborne under 45 %, the two tanks
// highest on her list walk there and wait with her head-to-the-wall side free; the ground fight takes over once she lands.
// [TACTICS] 2026-11-18 R4W0 Onyxia att. 3 (23 %): the pair staged for her landing was re-picked from live threat every
// 0.5 s (four different tanks walked to the landing spot) and the whelp posts kept a different pair, so at the landing
// two staged tanks left for the posts and a mage, then a warlock held her (Tail Sweep + Flame Breath through the raid).
// One pair for the fight: the two tanks highest on her list the first time she is airborne at <= 42 %, a dead one
// replaced by the next. Both StageForLanding and WhelpPostFor read it (SuiTactic::TanksStageForLanding gates the
// staging; SuiTactic::WhelpPostsAtSpawns the posts - both checked by the callers before this is asked).
// Onyxia's head direction from her landing spot, chosen once a fight (OnyxiaHeadSpot); cleared with her landing pair.
static std::mutex gOnyHeadLock;
static std::map<uint64, std::pair<float, float>> gOnyHead;

static std::vector<uint64> OnyxiaLandingPair(Player* me, Creature* boss, Group* group, Map const* map)
{
    if (!SuiTactic::On(me, SuiTactic::TanksStageForLanding, boss) && !SuiTactic::On(me, SuiTactic::WhelpPostsAtSpawns, boss))
        return {};
    static std::mutex lock;
    static std::map<uint64, std::vector<uint64>> pairs;
    std::lock_guard<std::mutex> guard(lock);
    std::vector<uint64>& p = pairs[boss->GetObjectGuid().GetRawValue()];
    if (!boss->IsInCombat() || boss->GetHealthPercent() > 66.0f)
    {
        p.clear();
        std::lock_guard<std::mutex> headGuard(gOnyHeadLock);
        gOnyHead.erase(boss->GetObjectGuid().GetRawValue());
        return p;
    }
    bool const air = boss->IsFlying() || boss->IsLevitating() || boss->IsHovering();
    if (p.empty() && !(air && boss->GetHealthPercent() <= 42.0f))
        return p;   // not chosen before the landing window opens
    std::vector<std::pair<Player*, float>> tanks;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInWorld() || m->GetMap() != map)
            continue;
        AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
        if (!ai || ai->GetCombatActiveRole() != ROLE_TANK)
            continue;
        tanks.push_back({m, boss->GetThreatManager().getThreat(m)});
    }
    // drop the dead / gone, then fill up to two by threat
    p.erase(std::remove_if(p.begin(), p.end(), [&](uint64 g)
        { return std::none_of(tanks.begin(), tanks.end(), [g](auto const& t) { return t.first->GetObjectGuid().GetRawValue() == g; }); }),
        p.end());
    std::sort(tanks.begin(), tanks.end(), [](auto const& a, auto const& b)
        { return a.second != b.second ? a.second > b.second : a.first->GetGUIDLow() < b.first->GetGUIDLow(); });
    for (auto const& t : tanks)
        if (p.size() < 2 && std::find(p.begin(), p.end(), t.first->GetObjectGuid().GetRawValue()) == p.end())
        {
            p.push_back(t.first->GetObjectGuid().GetRawValue());
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s is one of %s's two landing tanks (threat %.0f)",
                t.first->GetName(), boss->GetName(), t.second);
        }
    return p;
}

// Onyxia's head spot: 7 yd off her landing spot (boss_onyxia.cpp, P3: MovePoint to (-8.860, -212.752)) toward the
// direction with the fewest members in front of it (60 degrees, 50 yd; tanks not counted), the nearer wall when even,
// chosen once a fight when first asked (her pair is chosen at <= 42 % in the air) - the nearest wall alone was a rock on
// the raid's side (R4W0 drill 2 pull 1: her victim brought her there, one Flame Breath hit 26). The backup 4 yd
// to the main tank's left. Used by StageForLanding and the LandingHeadSpot callers (each caller checks its own row).
static void OnyxiaHeadSpot(Player* me, Creature* ony, bool backup, float& x, float& y, float& z)
{
    float const lx = -8.860f, ly = -212.752f, lz = -87.482f;
    float fx = 0.0f, fy = 0.0f;
    bool have = false;
    {
        std::lock_guard<std::mutex> guard(gOnyHeadLock);
        auto it = gOnyHead.find(ony->GetObjectGuid().GetRawValue());
        if (it != gOnyHead.end())
        {
            fx = it->second.first;
            fy = it->second.second;
            have = true;
        }
    }
    if (!have)
    {
        uint32 bestCrowd = 1000;
        float bestWall = 1e9f, nearFx = 0.0f, nearFy = 0.0f, nearWall = 1e9f;
        for (int k = 0; k < 24; ++k)
        {
            float const th = float(k) * float(M_PI) / 12.0f, dx = std::cos(th), dy = std::sin(th);
            float wall = 60.0f;
            for (float d = 4.0f; d <= 60.0f; d += 2.0f)
                if (!me->GetMap()->isInLineOfSight(lx, ly, lz + 3.0f, lx + dx * d, ly + dy * d, lz + 3.0f))
                {
                    wall = d;
                    break;
                }
            if (wall >= 10.0f && wall < nearWall)
            {
                nearWall = wall;
                nearFx = dx;
                nearFy = dy;
            }
            if (wall < 10.0f)
                continue;   // no room for the tanks and her head
            float sx = lx + dx * 7.0f, sy = ly + dy * 7.0f, sz = lz;
            me->UpdateAllowedPositionZ(sx, sy, sz);
            if (std::fabs(sz - lz) > 4.0f)
                continue;
            uint32 crowd = 0;
            if (Group* g = me->GetGroup())
                for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
                {
                    Player* m = itr->getSource();
                    if (!m || !m->IsAlive() || !m->IsInMap(me))
                        continue;
                    AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
                    if (ai && ai->GetCombatActiveRole() == ROLE_TANK)
                        continue;
                    float const mx = m->GetPositionX() - lx, my = m->GetPositionY() - ly;
                    float const md = std::sqrt(mx * mx + my * my);
                    if (md < 1.0f || md > 50.0f)
                        continue;
                    if ((mx * dx + my * dy) / md > 0.5f)
                        ++crowd;
                }
            if (crowd > bestCrowd || (crowd == bestCrowd && wall >= bestWall))
                continue;
            bestCrowd = crowd;
            bestWall = wall;
            fx = dx;
            fy = dy;
        }
        if (bestCrowd == 1000)
        {
            fx = nearFx;
            fy = nearFy;
        }
        {
            std::lock_guard<std::mutex> guard(gOnyHeadLock);
            gOnyHead[ony->GetObjectGuid().GetRawValue()] = { fx, fy };
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s's head spot: %.0f deg from her landing spot (%u members in front, wall %.0f yd)",
            ony->GetName(), std::atan2(fy, fx) * 180.0f / float(M_PI), bestCrowd == 1000 ? 0u : bestCrowd, bestWall > 1e8f ? nearWall : bestWall);
    }
    x = lx + fx * 7.0f;
    y = ly + fy * 7.0f;
    z = lz;
    if (backup)
    {
        x += -fy * 4.0f;
        y += fx * 4.0f;
    }
    me->UpdateAllowedPositionZ(x, y, z);
}

// Onyxia landed (phase 3) with her landing pair chosen; inPair: I am one of the two; onPair: she is on one of them.
// The callers check SuiTactic::LandingHeadSpot.
static bool OnyxiaLandedWithPair(Player* me, Creature* ony, bool& inPair, bool& onPair)
{
    inPair = onPair = false;
    if (!me->GetGroup() || ony->IsFlying() || ony->IsLevitating() || ony->IsHovering() || ony->GetHealthPercent() > 41.0f)
        return false;
    std::vector<uint64> const pair = OnyxiaLandingPair(me, ony, me->GetGroup(), me->GetMap());
    if (pair.empty())
        return false;
    inPair = std::find(pair.begin(), pair.end(), me->GetObjectGuid().GetRawValue()) != pair.end();
    Unit* v = ony->GetVictim();
    onPair = v && std::find(pair.begin(), pair.end(), v->GetObjectGuid().GetRawValue()) != pair.end();
    return true;
}

// [TACTICS] 2026-11-18 R4W0 Onyxia att. 1 (39 %): 30 of the 40 deaths took whelp melee. The script summons two whelps a
// second from two fixed spots (afSpawnLocations, east (-30.1, -254.5), west (-30.8, -177.1)) - 16 at phase 2, then 5-7
// every 30 s, one every 1-10 s in phase 3 - and sends each at a RANDOM member; the six tanks chased them one at a time
// across the lair (36-40 focus switches each), Taunt refused for reach 132 times. The guide's answer: the off-tanks stand
// at the spawn points and pick each whelp up as it appears. Scoped (SuiTactic::WhelpPostsAtSpawns): from phase 2 every
// tank but the two staged for her landing (the TanksStageForLanding pair, remembered for the fight) holds a post 8 yd
// inside a spawn point, three a side, 7 yd apart (one Fireball's Engulfing Flames burns friends within 5 yd), takes only
// whelps within 16 yd of it and uses Demoralizing Shout / Challenging Shout there; her two tanks keep her in phase 3.
bool AiBotAI::WhelpPostFor(float& x, float& y, float& z)
{
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_whelpPostCheckMs)
    {
        x = m_whelpPostX; y = m_whelpPostY; z = m_whelpPostZ;
        return m_whelpPostOn;
    }
    m_whelpPostCheckMs = now + 1000;
    m_whelpPostOn = false;
    if (!me->IsAlive() || !me->IsInCombat() || GetCombatActiveRole() != ROLE_TANK || me->GetMapId() != SuiTactic::kOnyxia)
        return false;
    Group* group = me->GetGroup();
    Creature* ony = me->FindNearestCreature(10184, 150.0f, true);
    if (!group || !ony || !ony->IsInCombat() || !SuiTactic::On(me, SuiTactic::WhelpPostsAtSpawns, ony))
        return false;
    bool const air = ony->IsFlying() || ony->IsLevitating() || ony->IsHovering();
    if (!air && ony->GetHealthPercent() > 41.0f)
        return false;   // phase 1: no whelps yet, every tank on her
    // her two tanks: the pair staged for the landing (top threat while airborne at <= 42 %), kept for the fight
    std::vector<std::pair<Player*, float>> tanks;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInMap(me))
            continue;
        AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
        if (!ai || ai->GetCombatActiveRole() != ROLE_TANK)
            continue;
        tanks.push_back({m, ony->GetThreatManager().getThreat(m)});
    }
    std::set<uint64> hers;
    for (uint64 g : OnyxiaLandingPair(me, ony, group, me->GetMap()))
        hers.insert(g);
    if (hers.empty() && !air)
    {
        // landed with no pair chosen (should not happen): her two highest tanks stay hers
        std::vector<std::pair<Player*, float>> byT = tanks;
        std::sort(byT.begin(), byT.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        for (size_t i = 0; i < byT.size() && i < 2; ++i)
            hers.insert(byT[i].first->GetObjectGuid().GetRawValue());
    }
    // [TACTICS] 2026-11-18 R4W0 Onyxia drill: her victim taken out of the post order re-numbered every post each time she
    // turned. With SuiTactic::LandingHeadSpot her victim leaves its post (it brings her to the head spot) and keeps its
    // place in the order.
    bool const headSpot = SuiTactic::On(me, SuiTactic::LandingHeadSpot, ony);
    if (!air && headSpot && ony->GetVictim() == me)
        return false;
    if (!air && !headSpot)
        if (Unit* v = ony->GetVictim())
            if (v->IsPlayer())
                hers.insert(v->GetObjectGuid().GetRawValue());
    if (hers.count(me->GetObjectGuid().GetRawValue()))
        return false;
    std::vector<Player*> posts;
    for (auto const& t : tanks)
        if (!hers.count(t.first->GetObjectGuid().GetRawValue()))
            posts.push_back(t.first);
    std::sort(posts.begin(), posts.end(), [](Player* a, Player* b) { return a->GetGUIDLow() < b->GetGUIDLow(); });
    int idx = -1;
    for (size_t i = 0; i < posts.size(); ++i)
        if (posts[i] == me)
            idx = int(i);
    if (idx < 0)
        return false;
    int const side = idx % 2, slot = idx / 2;
    int const n = int(posts.size()), nSide = side == 0 ? (n + 1) / 2 : n / 2;
    float const sx = side == 0 ? -30.1f : -30.8f, sy = side == 0 ? -254.5f : -177.1f, sz = -89.4f;
    float const cx = -21.0f, cy = -215.0f;
    float dx = cx - sx, dy = cy - sy;
    float const len = std::sqrt(dx * dx + dy * dy);
    dx /= len; dy /= len;
    float const lateral = (float(slot) - float(nSide - 1) * 0.5f) * 7.0f;
    x = sx + dx * 8.0f - dy * lateral;
    y = sy + dy * 8.0f + dx * lateral;
    z = sz;
    me->UpdateAllowedPositionZ(x, y, z);
    m_whelpPostX = x; m_whelpPostY = y; m_whelpPostZ = z;
    m_whelpPostOn = true;
    return true;
}

// The whelp a post tank takes: one within 16 yd of its post, a loose one (not on a tank) first, then the nearest to the
// post. Not a post tank (or no whelp there): postTank says which. Gated in WhelpPostFor (SuiTactic::WhelpPostsAtSpawns).
static std::unordered_map<uint64, uint32> gWhelpSweepUntil;   // post tank -> sweeping until (SuiTactic::WhelpPostSweepsHealers)
Unit* AiBotAI::WhelpPostTarget(bool& postTank)
{
    float px, py, pz;
    postTank = WhelpPostFor(px, py, pz);
    if (!postTank)
        return nullptr;
    std::list<Unit*> foes;
    me->GetEnemyListInRadiusAround(me, 40.0f, foes);
    Unit* best = nullptr;
    float bestScore = 1e9f;
    for (Unit* u : foes)
    {
        if (!u || !u->IsCreature() || !u->IsAlive() || u->GetEntry() != 11262 || u->GetDistance2d(px, py) > 16.0f)
            continue;
        Unit* v = u->GetVictim();
        bool onTank = false;
        if (v && v->IsPlayer())
            if (AiBotAI* ai = dynamic_cast<AiBotAI*>(static_cast<Player*>(v)->AI()))
                onTank = ai->GetCombatActiveRole() == ROLE_TANK;
        float const score = u->GetDistance2d(px, py) + (onTank ? 30.0f : 0.0f);
        if (score < bestScore)
        {
            bestScore = score;
            best = u;
        }
    }
    // [TACTICS] 2026-11-21 R4W7 att. 2-4, R4W8 att. 1: whelps past the posts killed the healers first in all four phase-3 losses
    // (17-30 deaths by whelp melee) - nobody takes a whelp more than 16 yd from a post. A post tank with no whelp of its own
    // goes for the nearest whelp on a non-tank within 25 yd of its post (taunts are melee range: it walks there), then returns.
    // Gated by SuiTactic::WhelpPostSweepsHealers.
    uint64 const myKey = me->GetObjectGuid().GetRawValue();
    gWhelpSweepUntil.erase(myKey);
    if (!best)
    {
        float bestD = 1e9f;
        for (Unit* u : foes)
        {
            if (!u || !u->IsCreature() || !u->IsAlive() || u->GetEntry() != 11262 || u->GetDistance2d(px, py) > 25.0f ||
                !SuiTactic::On(me, SuiTactic::WhelpPostSweepsHealers, u))
                continue;
            Unit* v = u->GetVictim();
            if (!v || !v->IsPlayer() || v == me)
                continue;
            AiBotAI* vai = dynamic_cast<AiBotAI*>(static_cast<Player*>(v)->AI());
            if (vai && vai->GetCombatActiveRole() == ROLE_TANK)
                continue;
            float const d = u->GetDistance2d(px, py);
            if (d < bestD)
            {
                bestD = d;
                best = u;
            }
        }
        if (best)
        {
            gWhelpSweepUntil[myKey] = WorldTimer::getMSTime() + 3000;
            static std::unordered_map<uint64, uint32> noted;
            uint32 const nowMs = WorldTimer::getMSTime();
            if (nowMs >= noted[myKey])
            {
                noted[myKey] = nowMs + 5000;
                Unit* v = best->GetVictim();
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-WHELP] %s: no whelp at my post - taking the one on %s (%.0f yd from the post)",
                    me->GetName(), v ? v->GetName() : "?", bestD);
            }
        }
    }
    return best;
}

// A post tank picking up a whelp on a healer may leave its post up to 30 yd (WhelpPostTarget, SuiTactic::WhelpPostSweepsHealers).
static bool WhelpSweeping(Player* me, float px, float py)
{
    if (me->GetDistance2d(px, py) > 30.0f)
        return false;
    auto it = gWhelpSweepUntil.find(me->GetObjectGuid().GetRawValue());
    if (it != gWhelpSweepUntil.end() && WorldTimer::getMSTime() < it->second)
        return true;   // WhelpPostTarget chose a whelp on a healer within the last 3 s
    Unit* v = me->GetVictim();
    if (!v || !v->IsCreature() || v->GetEntry() != 11262 || !v->IsAlive())
        return false;
    Unit* vv = v->GetVictim();
    if (!vv || vv == me || !vv->IsPlayer())
        return false;
    AiBotAI* vai = dynamic_cast<AiBotAI*>(static_cast<Player*>(vv)->AI());
    return (!vai || vai->GetCombatActiveRole() != ROLE_TANK) && SuiTactic::On(me, SuiTactic::WhelpPostSweepsHealers, v);
}

// Walk to the post; there, shout the whelps onto me. Gated in WhelpPostFor (SuiTactic::WhelpPostsAtSpawns).
bool AiBotAI::HoldWhelpPost()
{
    if (m_possessed || m_suiManual)
        return false;
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_whelpPostNextMs)
        return false;
    m_whelpPostNextMs = now + 400;
    float x, y, z;
    if (!WhelpPostFor(x, y, z))
        return false;
    if (me->GetDistance2d(x, y) > 6.0f && WhelpSweeping(me, x, y))
        return false;   // picking up a whelp on a healer; the combat AI closes on it
    if (me->GetDistance2d(x, y) > 6.0f)
    {
        if (!me->IsMoving() || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            if (now >= m_whelpPostLogMs)
            {
                m_whelpPostLogMs = now + 5000;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-WHELP] %s: to the whelp post (%.1f, %.1f), %.0f yd",
                    me->GetName(), x, y, me->GetDistance2d(x, y));
            }
            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
        }
        return true;
    }
    if (me->GetClass() == CLASS_WARRIOR && !me->IsNonMeleeSpellCasted(false))
    {
        std::list<Unit*> foes;
        me->GetEnemyListInRadiusAround(me, 10.0f, foes);
        uint32 whelps = 0, loose = 0, bare = 0;
        uint32 const demo = m_spells.warrior.pDemoralizingShout ? m_spells.warrior.pDemoralizingShout->Id : 0;
        for (Unit* u : foes)
        {
            if (!u || !u->IsCreature() || !u->IsAlive() || u->GetEntry() != 11262)
                continue;
            ++whelps;
            if (u->GetVictim() != me)
                ++loose;
            if (demo && !u->HasAura(demo))
                ++bare;
        }
        if (loose >= 4 && TrySpecSpell(me, 1161u))   // Challenging Shout
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-WHELP] %s: Challenging Shout on %u loose whelps at the post", me->GetName(), loose);
        else if (bare >= 2 && m_spells.warrior.pDemoralizingShout && TrySpecSpell(me, m_spells.warrior.pDemoralizingShout))
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-WHELP] %s: Demoralizing Shout on %u whelps at the post", me->GetName(), bare);
    }
    return false;   // fight from here (the focus keeps me on the post's whelps)
}

bool AiBotAI::StageForLanding()
{
    if (!me->IsAlive() || !me->IsInCombat() || m_possessed || m_suiManual || GetCombatActiveRole() != ROLE_TANK)
        return false;
    Group* group = me->GetGroup();
    if (!group || !me->GetMap()->IsRaid())
        return false;
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_stageNextMs)
        return m_stageMeeting;
    m_stageNextMs = now + 500;
    m_stageMeeting = false;
    Creature* boss = nullptr;
    std::list<Unit*> foes;
    me->GetEnemyListInRadiusAround(me, 150.0f, foes);
    for (Unit* u : foes)
        if (u && u->IsCreature() && u->IsAlive() && u->IsInCombat() && static_cast<Creature*>(u)->IsWorldBoss() &&
            SuiTactic::On(me, SuiTactic::TanksStageForLanding, u))
        {
            boss = static_cast<Creature*>(u);
            break;
        }
    // [TACTICS] 2026-11-16 R3W5 Onyxia 34 %: staged from 45 % the tanks stood on the landing spot through 60 s of phase 2 and
    // a Deep Breath there killed two of them and five healers beside them; 42 % leaves ~25 s, enough for a 50 yd walk.
    // Gated by SuiTactic::TanksStageForLanding (the boss search above).
    if (!boss || boss->GetHealthPercent() > 42.0f)
        return false;
    // [TACTICS] 2026-11-18 R4W0 Onyxia drill: landed, she stayed 4-38 s on a ranged member from the air and her two staged
    // tanks left the head spot (out of her front, not her victim); until one of them holds her they keep it, and the
    // member she is on brings her there (SuiTactic::LandingHeadSpot; AvoidFrontalCone).
    // [TACTICS] 2026-11-19 R4W0 Onyxia drill 2 pulls 7/9/10: turning to land west of x -40 she touches down at her south
    // point and fights 25-40 yd west of her landing spot; her pair waited at the head spot while she stayed on casters.
    // Landed more than 12 yd from her landing spot: the main pair tank goes to her and takes her, then walks her to the
    // head spot (she follows him). Gated by SuiTactic::PairMeetsHer.
    bool meet = false, bringHome = false, far = false;
    if (!(boss->IsFlying() || boss->IsLevitating() || boss->IsHovering()))
    {
        bool inPair = false, onPair = false;
        if (!SuiTactic::On(me, SuiTactic::LandingHeadSpot, boss) || !OnyxiaLandedWithPair(me, boss, inPair, onPair) || !inPair)
            return false;
        far = boss->GetDistance2d(-8.860f, -212.752f) > 12.0f && SuiTactic::On(me, SuiTactic::PairMeetsHer, boss);
        if (onPair)
        {
            // (drill 3 pull 1: feared off his spot, her main tank took her back wherever the fear left him and her
            // breath went where he stood - also back to the spot when he is more than 4 yd off it, checked below)
            if (boss->GetVictim() != me || !SuiTactic::On(me, SuiTactic::PairMeetsHer, boss))
                return false;
            bringHome = true;
        }
        else
            meet = far;
    }
    // the two living tanks highest on her list
    Player* top[2] = { nullptr, nullptr };
    float topT[2] = { -1.0f, -1.0f };
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInMap(me))
            continue;
        AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
        if (!ai || ai->GetCombatActiveRole() != ROLE_TANK)
            continue;
        float const t = boss->GetThreatManager().getThreat(m);
        if (t > topT[0]) { top[1] = top[0]; topT[1] = topT[0]; top[0] = m; topT[0] = t; }
        else if (t > topT[1]) { top[1] = m; topT[1] = t; }
    }
    // the pair is chosen once for the fight (OnyxiaLandingPair, R4W0 att. 3); the one of the two now higher
    // on her list is the main tank
    {
        std::vector<uint64> const pair = OnyxiaLandingPair(me, boss, group, me->GetMap());
        uint64 const mine = me->GetObjectGuid().GetRawValue();
        if (std::find(pair.begin(), pair.end(), mine) == pair.end())
            return false;
        Player* other = nullptr;
        for (uint64 g : pair)
            if (g != mine)
                other = ObjectAccessor::FindPlayer(ObjectGuid(g));
        float const tMe = boss->GetThreatManager().getThreat(me), tOther = other ? boss->GetThreatManager().getThreat(other) : -1.0f;
        bool const backup = other && (tOther > tMe || (tOther == tMe && other->GetGUIDLow() < me->GetGUIDLow()));
        top[0] = backup ? other : me;
        top[1] = backup ? me : other;
    }
    // (PairMeetsHer, above) the main tank goes to her where she landed and takes her; the backup keeps the head spot
    if (meet && me == top[0])
    {
        if (me->GetVictim() != boss)
            me->Attack(boss, true);
        if (me->CanReachWithMeleeAutoAttack(boss))
            return false;   // in reach: the rotation takes her (re-aimed at her every tick of mine)
        if (now >= m_stageLogMs)
        {
            m_stageLogMs = now + 5000;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: %s landed %.0f yd from her landing spot - to her, %.0f yd",
                me->GetName(), boss->GetName(), boss->GetDistance2d(-8.860f, -212.752f), me->GetDistance2d(boss));
        }
        me->GetMotionMaster()->MoveChase(boss);
        m_stageMeeting = true;
        return true;
    }
    // the head spot (OnyxiaHeadSpot): the main tank 7 yd off her landing spot toward the nearest wall, the backup beside
    // (bringHome: she is on me, away from her landing spot - my spot is the main one whatever the threat order says)
    float x, y, z;
    OnyxiaHeadSpot(me, boss, !bringHome && me == top[1], x, y, z);
    if (bringHome && !far && me->GetDistance2d(x, y) <= 4.0f)
        return false;   // she is on me at my spot: hold her there
    if (me->GetDistance2d(x, y) <= 2.0f)
    {
        me->SetFacingTo(std::atan2(-212.752f - y, -8.860f - x));
        return false;   // in place: whelps that reach me are still fought
    }
    if (now >= m_stageLogMs)
    {
        m_stageLogMs = now + 5000;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: to %s's landing spot (%.1f, %.1f), %.0f yd (she is at %.0f %%)%s",
            me->GetName(), boss->GetName(), x, y, me->GetDistance2d(x, y), boss->GetHealthPercent(),
            bringHome ? " - walking her home" : "");
    }
    m_stageMeeting = bringHome;
    m_stageNextMs = now + 1200;
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return true;
}

bool AiBotAI::AvoidFrontalCone()
{
    if (!me->IsAlive() || !me->GetGroup() || m_possessed || m_suiManual || !(me->IsInCombat() || HealerOnDuty()))
        return false;
    CombatBotRoles const role = GetCombatActiveRole();
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_coneNextMs)
        return false;
    m_coneNextMs = now + 500;

    // A long breath being cast sweeps the whole line in front of its caster, tank or not (2026-09-25:
    // Onyxia's Deep Breath across her lair killed twenty in one pass): off that line, sideways.
    {
        std::list<Unit*> far;
        me->GetEnemyListInRadiusAround(me, 110.0f, far);
        for (Unit* u : far)
        {
            if (!u || !u->IsCreature() || !u->IsAlive())
                continue;
            bool cone = false;
            Spell* cs = u->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (cs && cs->m_spellInfo && cs->GetCastTime() >= 1000)
                for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                    if (cs->m_spellInfo->EffectImplicitTargetA[i] == TARGET_ENUM_UNITS_ENEMY_IN_CONE_24 ||
                        cs->m_spellInfo->EffectImplicitTargetA[i] == TARGET_ENUM_UNITS_ENEMY_IN_CONE_54)
                        cone = true;
            // ...and an airborne world boss that holds still with no target: Onyxia's Deep Breath is cast triggered (no
            // cast time) as a chain of line hits, and for its 5 s she clears her target and faces where she will fly
            // (boss_onyxia DoMovement). The cast test never saw it (2026-11-14, Onyxia dev attempt 19: ten dead to one
            // Deep Breath at +202 s, not one "off the line" step before it).
            bool airborneBreath = false;
            if (!cone && static_cast<Creature*>(u)->IsWorldBoss() && (u->IsFlying() || u->IsLevitating() || u->IsHovering()) &&
                u->GetTargetGuid().IsEmpty() && !u->IsMoving())
                cone = airborneBreath = true;
            if (!cone)
                continue;
            // [TACTICS] 2026-11-18 R4W0 Onyxia att. 1: her Deep Breath is escaped from its forecast impacts
            // (EscapeBreathForecast); this facing-based line re-aimed bots onto the room's centre line every 0.5 s
            // and five died on it (SuiTactic::DeepBreathEscape). Her cast Flame Breath stays here.
            if (airborneBreath && SuiTactic::On(me, SuiTactic::DeepBreathEscape, u))
                continue;
            // [TACTICS] 2026-11-19 R4W0 Onyxia drill 3 pull 1: her backup tank stepped 28 yd "off the line" of her cast
            // breath; a Bellowing Roar feared her main tank, she turned to the backup there and her breath hit 18 on
            // the flank. Landed, her pair keeps the head spot through her breath (SuiTactic::LandingHeadSpot).
            if (!airborneBreath && u->GetEntry() == 10184 && SuiTactic::On(me, SuiTactic::LandingHeadSpot, u))
            {
                bool inPair = false, onPair = false;
                if (OnyxiaLandedWithPair(me, static_cast<Creature*>(u), inPair, onPair) && inPair)
                    continue;
            }
            float const o = u->GetOrientation(), fx = std::cos(o), fy = std::sin(o);
            float const dx = me->GetPositionX() - u->GetPositionX(), dy = me->GetPositionY() - u->GetPositionY();
            float const along = dx * fx + dy * fy;
            float const lateral = dx * fy - dy * fx;   // signed distance off the line
            // (only the tank it breathes at stays: 2026-11-14, Onyxia phase 3 - off-tanks holding whelps stood in her front)
            if (along < 0.0f || std::fabs(lateral) > 32.0f || (role == ROLE_TANK && u->GetVictim() == me))
                continue;
            float const side = lateral >= 0.0f ? 1.0f : -1.0f;
            for (float shift : {36.0f, 44.0f})
            {
                float const need = shift - std::fabs(lateral);
                float x = me->GetPositionX() + fy * side * need, y = me->GetPositionY() - fx * side * need;
                float z = me->GetPositionZ();
                me->UpdateAllowedPositionZ(x, y, z);
                if (std::fabs(z - me->GetPositionZ()) > 8.0f || Molten(me, x, y, z) || !Walkable(me, x, y, z, 2.5f))
                    continue;
                if (me->IsNonMeleeSpellCasted(false))
                    me->InterruptNonMeleeSpells(false);
                m_coneNextMs = now + 2000;
                m_repositionUntilMs = now + 3000;
                SuiRaidTelemetry::OnReposition(me, "breath");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: off the line of %s's breath -> (%.1f, %.1f)",
                    me->GetName(), u->GetName(), x, y);
                me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                return true;
            }
        }
    }
    // A tank keeps out of the front and tail of a coned kind it does not hold, like everyone else: the small adds it
    // holds stand where it stands, and the melee and casters killing them follow (2026-11-14, Onyxia phase 3: three
    // off-tanks held whelps inside her front; one Flame Breath at her own tank killed five beside them).
    std::list<Unit*> nearby;
    me->GetEnemyListInRadiusAround(me, 50.0f, nearby);
    for (Unit* u : nearby)
    {
        if (!u || !u->IsCreature() || !u->IsAlive() || !u->IsInCombat())
            continue;
        // [TACTICS] 2026-11-18 R4W0 Onyxia drill (SuiTactic::LandingHeadSpot): landed, her two tanks keep her head spot
        // whether she is on them or not, and anyone else she turns on walks her there instead of out of her front (her
        // landing target stepped out of it 4-5 times, she followed and her Flame Breath swept 10-25 members at once)
        if (u->GetEntry() == 10184 && SuiTactic::On(me, SuiTactic::LandingHeadSpot, u))
        {
            bool inPair = false, onPair = false;
            if (OnyxiaLandedWithPair(me, static_cast<Creature*>(u), inPair, onPair))
            {
                if (inPair)
                    continue;
                if (u->GetVictim() == me)
                {
                    float hx, hy, hz;
                    OnyxiaHeadSpot(me, static_cast<Creature*>(u), false, hx, hy, hz);
                    if (me->GetDistance2d(hx, hy) <= 3.0f)
                        continue;   // there: her head is to the wall
                    if (me->IsNonMeleeSpellCasted(false))
                        me->InterruptNonMeleeSpells(false);
                    m_coneNextMs = now + 1000;
                    m_repositionUntilMs = now + 1500;
                    SuiRaidTelemetry::OnReposition(me, "head spot");
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: %s is on me - to her head spot (%.1f, %.1f), %.0f yd",
                        me->GetName(), u->GetName(), hx, hy, me->GetDistance2d(hx, hy));
                    me->GetMotionMaster()->MovePoint(0, hx, hy, hz, MOVE_PATHFINDING | MOVE_RUN_MODE);
                    return true;
                }
                // [TACTICS] 2026-11-19 R4W0 Onyxia drill 2 pull 3: her Knock Away moves her tank, she turns 20-40 degrees with
                // him, and every member re-picked a flank spot from her new facing in the same second and walked through her
                // front (21 hit by one breath). Landed, the flanks are measured from her head direction (fixed for the
                // fight): her front is within 75 degrees of it, her tail within 70 of its back; only a member in one of them
                // moves, to 85-100 degrees on its own side, so nobody crosses her front (SuiTactic::FlanksOffHeadDirection).
                float hfx = 0.0f, hfy = 0.0f;
                bool haveHead = false;
                {
                    std::lock_guard<std::mutex> guard(gOnyHeadLock);
                    auto it = gOnyHead.find(u->GetObjectGuid().GetRawValue());
                    if (it != gOnyHead.end())
                    {
                        hfx = it->second.first;
                        hfy = it->second.second;
                        haveHead = true;
                    }
                }
                // (a tank outside her pair is not moved here - it holds its whelp post; at her flank in melee reach it built
                // threat and took her from her pair, R4W0 drill 2 pull 5)
                if (haveHead && role == ROLE_TANK && SuiTactic::On(me, SuiTactic::FlanksOffHeadDirection, u))
                    continue;
                if (haveHead && SuiTactic::On(me, SuiTactic::FlanksOffHeadDirection, u))
                {
                    // (drill 3 pull 2: her melee reach is ~26 yd - landed away from her landing spot she hits her tank at
                    // the head spot from where she stands and faces him, not the head direction; 4 breaths hit 4-11 at
                    // 55-60 deg. Her head points from her to the main head spot; the fixed direction only once there.)
                    float ho = std::atan2(hfy, hfx);
                    {
                        float hsx, hsy, hsz;
                        OnyxiaHeadSpot(me, static_cast<Creature*>(u), false, hsx, hsy, hsz);
                        if (u->GetDistance2d(hsx, hsy) > 3.0f)
                            ho = std::atan2(hsy - u->GetPositionY(), hsx - u->GetPositionX());
                    }
                    float const dx = me->GetPositionX() - u->GetPositionX(), dy = me->GetPositionY() - u->GetPositionY();
                    float const d = std::sqrt(dx * dx + dy * dy);
                    float rel = std::atan2(dy, dx) - ho;
                    while (rel > float(M_PI)) rel -= 2.0f * float(M_PI);
                    while (rel < -float(M_PI)) rel += 2.0f * float(M_PI);
                    float const absRel = std::fabs(rel);
                    float const reachO = SuiRaidTelemetry::EntryConeRadius(u->GetEntry()) + u->GetObjectBoundingRadius() + 3.0f;
                    bool const front = absRel < float(M_PI) * 75.0f / 180.0f;
                    bool const tail = absRel > float(M_PI) * 110.0f / 180.0f;
                    if (d > reachO || !(front || tail))
                        continue;   // on a flank (or out of her reach): stay
                    bool const melee = role == ROLE_MELEE_DPS || role == ROLE_TANK;
                    float const keepO = melee ? u->GetObjectBoundingRadius() + me->GetObjectBoundingRadius() +
                        std::min(u->GetCombatReach() * 0.5f, 10.0f) : std::min(std::max(d, 12.0f), 30.0f);
                    float const side = rel >= 0.0f ? 1.0f : -1.0f;
                    bool moved = false;
                    for (float sgn : { side, -side })
                    {
                        for (float off : { 1.57f, 1.66f, 1.48f, 1.75f })
                        {
                            float const ang = ho + sgn * off;
                            float x = u->GetPositionX() + keepO * std::cos(ang), y = u->GetPositionY() + keepO * std::sin(ang);
                            float z = u->GetPositionZ();
                            me->UpdateAllowedPositionZ(x, y, z);
                            if (std::fabs(z - u->GetPositionZ()) > 6.0f || Molten(me, x, y, z) || !SeesFrom(me, x, y, z, u) ||
                                !Walkable(me, x, y, z))
                                continue;
                            if (me->IsNonMeleeSpellCasted(false))
                                me->InterruptNonMeleeSpells(false);
                            m_coneNextMs = now + 1500;
                            m_repositionUntilMs = now + 2500;
                            SuiRaidTelemetry::OnReposition(me, "flank");
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: to %s's %s flank off her head direction -> (%.1f, %.1f) (was %.0f deg)",
                                me->GetName(), u->GetName(), sgn > 0.0f ? "left" : "right", x, y, rel * 180.0f / float(M_PI));
                            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                            moved = true;
                            break;
                        }
                        if (moved || front)
                            break;   // from her front only my own side: the other one is across her breath
                    }
                    if (moved)
                        return true;
                    continue;
                }
            }
        }
        // (and nobody steps out of the front of the one hitting them: it turns with them - 2026-11-15, R3W1 MC, the
        // Firewalker/Flameguard/Lava Elemental pack by Geddon: healers it had turned on stepped "out of the front" every
        // 1-2 s, cast almost nothing, and the tanks chasing it never got into Taunt's melee reach; 38 dead at 100 %)
        if (u->GetVictim() == me && (role == ROLE_TANK || SuiTactic::On(me, SuiTactic::NoDodgeOwnAttacker, u)))
            continue;
        float const cone = SuiRaidTelemetry::EntryConeRadius(u->GetEntry());
        if (cone <= 0.0f)
            continue;
        // (2026-11-14, Onyxia phase 2: hovering, she has no Flame Breath or Tail Sweep, and the healers stepped "out of
        // her front" 124 times a fight - moving 59 % of it. A breath she casts from the air is the block above.)
        if (u->IsFlying() || u->IsLevitating() || u->IsHovering())
            continue;
        float const reach = cone + u->GetObjectBoundingRadius() + 3.0f;
        float const d = me->GetDistance2d(u);
        // In front means within 70 degrees of where it faces (the cone plus a margin); a kind with a
        // cone may sweep its tail too (the same cone data, behind it), so its back is no place
        // either - the sides are (2026-09-25, Onyxia: Tail Sweep 96k on the melee behind her).
        bool const inFront = u->HasInArc(me, float(M_PI) * 7.0f / 9.0f);
        // ...but only a dragon-sized body has a tail to sweep: a hound's back is the healers' place
        // (2026-11-05, Magmadar: front and back both closed left a 40-degree wedge each side; every
        // healer dodged "out of the front" every 3.5 s, cast 43 % of the fight, the tanks died).
        bool const behind = u->GetCombatReach() >= 15.0f &&
            !u->HasInArc(me, float(M_PI) * 11.0f / 9.0f);   // within 70 degrees of her back
        if (d > reach || !(inFront || behind))
            continue;
        // A heal or a shot already on its way is finished unless the breath is being cast.
        if (me->IsNonMeleeSpellCasted(false))
        {
            Spell* theirs = u->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!theirs)
                return false;
            me->InterruptNonMeleeSpells(false);
        }
        // Its side or back, at my own distance (melee: close behind it).
        bool const melee = role == ROLE_MELEE_DPS || role == ROLE_TANK;
        float const bigReach = u->GetCombatReach() >= 8.0f ? std::min(u->GetCombatReach() * 0.5f, 10.0f) : 1.0f;
        float const keep = melee ? u->GetObjectBoundingRadius() + me->GetObjectBoundingRadius() + bigReach :
            std::max(d, 12.0f);
        std::vector<GroundHazard> hazards;
        CollectGroundHazards(me, hazards);
        std::vector<GroundHazard> idle;
        CollectIdleHostiles(me, 60.0f, idle);
        // ...and an idle boss's whole roam (35 yd + his wander) like the fire step-out: a mage's cone step to 40 yd from
        // Shazzrah's home (28 + 10 credited) woke him at +13 s of his trash pack's fight, all 40 dead (2026-11-15, R3W1 MC)
        if (SuiTactic::On(me, SuiTactic::ConeAvoidsIdleBossRoam))
            CollectIdleBosses(me, idle);
        float bestX = 0, bestY = 0, bestZ = 0, bestScore = 1e9f;
        // its flanks - behind the shoulder, clear of both its front and its tail; at my own distance first, then nearer
        // and farther (2026-11-14, Onyxia dev attempt 11: a warlock 19 yd out at 22 degrees found no flank spot at 19 yd
        // in her cave, stood still and took three Flame Breaths in 30 s - six flank spots, all walled or out of sight)
        std::vector<float> keeps = { keep };
        if (!melee)
            for (float k : { keep * 0.7f, keep * 1.3f, 12.0f, keep * 0.5f, keep * 1.6f })
                if (k >= 10.0f && k <= 33.0f)   // still in spell range
                    keeps.push_back(k);
        uint32 rejZ = 0, rejSight = 0, rejMolten = 0, rejHazard = 0, rejIdle = 0, rejWalk = 0;
        auto tryBand = [&](float k, std::initializer_list<float> offs)
        {
            for (float off : offs)
            {
                float const ang = u->GetOrientation() + off;
                float x = u->GetPositionX() + k * std::cos(ang), y = u->GetPositionY() + k * std::sin(ang);
                float z = u->GetPositionZ();
                me->UpdateAllowedPositionZ(x, y, z);
                if (std::fabs(z - u->GetPositionZ()) > 6.0f) { ++rejZ; continue; }
                if (!SeesFrom(me, x, y, z, u)) { ++rejSight; continue; }
                if (Molten(me, x, y, z)) { ++rejMolten; continue; }
                if (InHazard(hazards, x, y)) { ++rejHazard; continue; }
                if (InHazard(idle, x, y)) { ++rejIdle; continue; }
                float const score = me->GetDistance2d(x, y);
                if (score >= bestScore)
                    continue;
                if (!Walkable(me, x, y, z)) { ++rejWalk; continue; }
                bestScore = score;
                bestX = x;
                bestY = y;
                bestZ = z;
            }
        };
        for (float k : keeps)
        {
            tryBand(k, {1.83f, -1.83f, 1.7f, -1.7f, 1.57f, -1.57f, 1.4f, -1.4f});
            if (bestScore < 1e8f)
                break;   // the nearest band that has a spot
        }
        // ...then the whole flank arcs, near to far (2026-11-14, Onyxia dev attempt 15: fought where she woke, with walls
        // and egg piles at her sides - 1024 "no flank spot" in one fight; the ranged stood behind her and took her Tail
        // Sweep 15-32 times each)
        if (bestScore > 1e8f)
            for (float k = melee ? keep : 8.0f; k <= (melee ? keep + 6.0f : 33.0f) && bestScore > 1e8f; k += 3.0f)
                tryBand(k, {1.31f, -1.31f, 1.48f, -1.48f, 1.66f, -1.66f, 1.83f, -1.83f});
        if (bestScore > 1e8f)
        {
            static std::mutex noSpotLock;
            static std::unordered_map<uint64, uint32> noSpotLogMs;
            std::lock_guard<std::mutex> guard(noSpotLock);
            uint32& next = noSpotLogMs[me->GetObjectGuid().GetRawValue()];
            if (now >= next)
            {
                next = now + 5000;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: in the front of %s (%.0f yd) - no flank spot found "
                    "(rejected: height %u sight %u lava %u hazard %u idle %u path %u; it stands at %.1f, %.1f, %.1f facing %.2f)",
                    me->GetName(), u->GetName(), d, rejZ, rejSight, rejMolten, rejHazard, rejIdle, rejWalk,
                    u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), u->GetOrientation());
            }
            return false;
        }
        m_coneNextMs = now + 1500;
        m_repositionUntilMs = now + 2500;
        SuiRaidTelemetry::OnReposition(me, "cone");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: out of the front of %s -> (%.1f, %.1f)",
            me->GetName(), u->GetName(), bestX, bestY);
        me->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }
    return false;
}

// A sleeping burster is kept clear of only once it may wake: when at most one awake enemy is left beside
// it and that one is nearly dead (a script can wake every sheep at that death). Before that, sheeped
// circles round the tanks only take the raid's places (2026-11-07, Majordomo with five tanks: all four
// Healers sheeped in the first 3 s, the ranged stepped "clear" 458 times, 737 dps, first Elite dead at 117 s).
// (Tried 2026-11-09 and reverted: "any awake add under 20 %" - an Elite parked at the kill order's floor kept it
// true, the free tanks stood by the sheeps for 30 s and nobody woke the released Healer; attempt 70 killed nothing.)
// (an awake one of a sleeping kind does not count: one Healer broke its sheep early and was held 36 yd off, so the
// rule never fired - no tank by the sheeps, nobody out of their circles, six dead at the wake-up - 2026-11-09,
// Majordomo attempt 84)
static bool SleepersMayWake(std::list<Unit*> const& around, bool anyHealth = false)
{
    std::set<uint32> sleeping;
    for (Unit* u : around)
        if (u && u->IsCreature() && u->IsAlive() && u->HasBreakableByDamageCrowdControlAura())
            sleeping.insert(u->GetEntry());
    uint32 awake = 0;
    float hp = 100.0f;
    for (Unit* u : around)
        if (u && u->IsCreature() && u->IsAlive() && u->IsInCombat() && !u->HasBreakableByDamageCrowdControlAura() &&
            !static_cast<Creature*>(u)->IsWorldBoss() && !sleeping.count(u->GetEntry()))
        {
            ++awake;
            hp = u->GetHealthPercent();
        }
    // (50 %, was 35 %: the free tanks walk 60 yd to the sheeps and the last Elite fell from 35 % in 8 s -
    // 2026-11-09, Majordomo attempt 86)
    return awake == 0 || (awake == 1 && (anyHealth || hp < 50.0f));
}

// sleeping burster -> (the tank standing by it, until ms): one tank to each
static std::mutex gSleeperWatchLock;
static std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> gSleeperWatch;

namespace
{
    struct HealSpotClaim { ObjectGuid who; float x, y; uint32 untilMs; };
    std::mutex gHealSpotLock;
    std::vector<HealSpotClaim> gHealSpots;
}

bool AiBotAI::MaintainRangedSpacing()
{
    CombatBotRoles const role = GetCombatActiveRole();
    // [TACTICS] A wounded melee damage dealer steps out of point-blank area damage (a Molten
    // Giant's Smash killed the rogues in 22 s) and waits to be healed, then goes back in.
    // Against a kind that pulses fire around itself, a melee damage dealer cannot take the next
    // burst below two thirds of its health (2026-09-25: seven rogues died to Baron Geddon's Inferno).
    Unit* const meleeVictim = me->GetVictim();
    bool const pulseFoe = meleeVictim && meleeVictim->IsCreature() &&
        SuiRaidTelemetry::EntryPulseRadius(meleeVictim->GetEntry()) > 0.0f;
    // (65 % / 90 % against a burster: four rogues died in the same second to one Blast Wave every 4 s, 735-772
    // a hit, none healed in 8 s - 2026-11-09, Majordomo attempt 64)
    bool const woundedMelee = role == ROLE_MELEE_DPS && me->IsInCombat() &&
        (me->GetHealthPercent() < (pulseFoe ? 65.0f : 40.0f) ||
         (m_meleeSteppedOut && me->GetHealthPercent() < (pulseFoe ? 90.0f : 70.0f)));
    m_meleeSteppedOut = woundedMelee;
    // A melee damage dealer keeps out of a crowd-controlled burster's circle too, and holds outside it
    // while its own target stands inside (2026-11-07, Majordomo: the 4th add died, the four sheeped
    // Healers woke at once beside the last Elite and their first Shadow Shocks killed all eight melee in 2 s).
    bool heldBurstMelee = false;
    if (role == ROLE_MELEE_DPS && me->IsInCombat() && !woundedMelee)
    {
        Unit* const v = me->GetVictim();
        std::list<Unit*> around;
        me->GetEnemyListInRadiusAround(me, 45.0f, around);
        bool const mayWake = SleepersMayWake(around);
        // ...and out of a second AWAKE wide burster's circle while my target is one: inside two at once a
        // melee takes both bursts (2026-11-09, Majordomo attempt 82: the four Healers woke immune, six rogues
        // went in on one beside the others and died to ~1k Shadow Shocks within 13 s). One alone is still hit.
        bool const wideVictim = v && v->IsCreature() && SuiRaidTelemetry::EntryPulseRadius(v->GetEntry()) >= 18.0f;
        for (Unit* u : around)
        {
            if (!u || !u->IsCreature() || !u->IsAlive())
                continue;
            bool const asleep = u->HasBreakableByDamageCrowdControlAura();
            if (asleep ? !mayWake : !(wideVictim && u != v && u->IsInCombat()))
                continue;
            float const pr = SuiRaidTelemetry::EntryPulseRadius(u->GetEntry());
            if (pr <= 0.0f || (!asleep && pr < 18.0f))
                continue;
            if (me->GetDistance(u) < pr + 1.2f || (v && v->GetDistance(u) < pr + 1.2f + 4.0f))
            {
                heldBurstMelee = true;
                break;
            }
        }
    }
    // [TACTICS] A free tank stands beside a sleeping burster that may wake - one tank to each - and takes
    // it the moment it wakes (2026-11-07, Majordomo with five tanks: the 4th Elite died, the four Healers
    // woke immune to Polymorph on the casters and killed 39 in 36 s with no tank on any of them).
    if (role == ROLE_TANK && me->IsInCombat() && !m_possessed && !m_suiManual && me->GetGroup())
    {
        Unit* const v = me->GetVictim();
        bool const holding = v && v->IsAlive() && v->GetVictim() == me;
        bool const bossTank = v && v->IsCreature() && static_cast<Creature*>(v)->IsWorldBoss();
        if (!holding && !bossTank)
        {
            // 80 yd: the tanks take their Elites away from the sheeps, and at 45 yd two free tanks 60 yd off never
            // saw them - one tank stood by four sheeps (2026-11-09, Majordomo attempt 86).
            std::list<Unit*> around;
            me->GetEnemyListInRadiusAround(me, 80.0f, around);
            Unit* sleeper = nullptr;
            float bestD = 1e9f;
            uint32 const nowMs = WorldTimer::getMSTime();
            // A free tank goes as soon as one add is left awake, whatever its health (no playbook release
            // waiting for a tank): the last Elite died 14 s after the third and only one tank reached a sheep in
            // time; three woke loose into the casters (2026-11-10, Majordomo verification V10).
            if (SleepersMayWake(around, !AiBotAI::PlaybookReleaseTarget(me)))
            {
                std::lock_guard<std::mutex> guard(gSleeperWatchLock);
                for (Unit* u : around)
                {
                    if (!u || !u->IsCreature() || !u->IsAlive() || !u->HasBreakableByDamageCrowdControlAura() ||
                        SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()) <= 0.0f)
                        continue;
                    auto c = gSleeperWatch.find(u->GetObjectGuid());
                    if (c != gSleeperWatch.end() && c->second.first != me->GetObjectGuid() && nowMs < c->second.second)
                        continue;
                    float const d = me->GetDistance(u);
                    if (d < bestD)
                    {
                        bestD = d;
                        sleeper = u;
                    }
                }
                if (sleeper)
                    gSleeperWatch[sleeper->GetObjectGuid()] = std::make_pair(me->GetObjectGuid(), nowMs + 3000);
            }
            if (sleeper)
            {
                if (nowMs >= m_sleeperWatchLogMs)
                {
                    m_sleeperWatchLogMs = nowMs + 5000;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: stands by the sleeping %s (guid %u, %.0f yd) for its wake-up",
                        me->GetName(), sleeper->GetName(), sleeper->GetGUIDLow(), bestD);
                }
                if (v)
                    me->AttackStop();
                if (bestD > 4.0f && nowMs >= m_rangedSpacingNextMs)
                {
                    float dx = me->GetPositionX() - sleeper->GetPositionX(), dy = me->GetPositionY() - sleeper->GetPositionY();
                    float const len = std::max(0.5f, std::sqrt(dx * dx + dy * dy));
                    float x = sleeper->GetPositionX() + dx / len * 2.5f, y = sleeper->GetPositionY() + dy / len * 2.5f;
                    float z = sleeper->GetPositionZ();
                    me->UpdateAllowedPositionZ(x, y, z);
                    m_rangedSpacingNextMs = nowMs + 1500;
                    m_repositionUntilMs = nowMs + 2000;
                    SuiRaidTelemetry::OnReposition(me, "watch");
                    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                }
                return true;
            }
        }
    }
    // (a melee chasing its target is always moving: that chase is what the sleeping burster stops -
    // 2026-11-07, the rule never ran while the eight melee chased the last Elite beside the sheeps)
    // (...and so is a wounded one: the tanks drag the Healers apart, the rogues chase them, and three at 20-48 %
    // never stepped out of Shadow Shock - 2026-11-09, Majordomo attempt 87)
    bool const chasing = me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;
    // A melee damage dealer under a boss in the air has nothing to hit and stands with the other melee: it spreads
    // like a caster when the boss's spells splash (2026-11-14, Onyxia dev attempt 8: nine melee stood under her in
    // phase 2 and one Fireball's Engulfing Flames killed five of them in the same second).
    if (role == ROLE_MELEE_DPS && me->IsInCombat() && meleeVictim && meleeVictim->IsCreature() &&
        static_cast<Creature*>(meleeVictim)->IsWorldBoss() &&
        (meleeVictim->IsFlying() || meleeVictim->IsLevitating() || meleeVictim->IsHovering()) &&
        me->GetGroup() && !m_possessed && !m_suiManual && !me->IsNonMeleeSpellCasted(false) &&
        WorldTimer::getMSTime() >= m_rangedSpacingNextMs)
    {
        // (5 yd at least: Onyxia's Fireball splashes through Engulfing Flames, a triggered spell the splash census does not see)
        float const splash = std::max(SuiRaidTelemetry::EntrySplashRadius(meleeVictim->GetEntry()),
            SuiRaidPlaybook::SpreadYards(meleeVictim->GetEntry()));
        {
            float const apart = std::max(5.0f, splash + 1.0f);
            Group* group = me->GetGroup();
            auto crowd = [&](float x, float y)
            {
                uint32 n = 0;
                for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                {
                    Player* o = itr->getSource();
                    if (o && o != me && o->IsAlive() && o->IsInWorld() && o->IsInMap(me) && o->GetDistance2d(x, y) < apart)
                        ++n;
                }
                return n;
            };
            uint32 bestN = crowd(me->GetPositionX(), me->GetPositionY());
            if (bestN > 0)
            {
                std::vector<GroundHazard> burning;
                CollectGroundHazards(me, burning);
                float bestX = 0, bestY = 0, bestZ = 0;
                for (float dist : {apart, apart * 1.4f, apart * 1.8f})
                    for (int i = 0; i < 12; ++i)
                    {
                        float const ang = i * float(M_PI) / 6.0f;
                        float x = me->GetPositionX() + dist * std::cos(ang), y = me->GetPositionY() + dist * std::sin(ang);
                        float z = me->GetPositionZ();
                        me->UpdateAllowedPositionZ(x, y, z);
                        if (std::fabs(z - me->GetPositionZ()) > 4.0f || !me->IsWithinLOS(x, y, z + 1.5f))
                            continue;
                        if (me->GetTerrain()->getLiquidStatus(x, y, z + 0.01f, MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME, nullptr) &
                            (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK))
                            continue;
                        if (meleeVictim->GetDistance2d(x, y) > std::max(30.0f, meleeVictim->GetDistance2d(me) + 2.0f))
                            continue;   // near enough to be back in reach when it lands
                        if (InHazard(burning, x, y))
                            continue;
                        uint32 const n = crowd(x, y);
                        if (n < bestN && Walkable(me, x, y, z))
                        {
                            bestN = n;
                            bestX = x;
                            bestY = y;
                            bestZ = z;
                        }
                    }
                if (bestX != 0 || bestY != 0)
                {
                    m_rangedSpacingNextMs = WorldTimer::getMSTime() + 3000;
                    m_repositionUntilMs = WorldTimer::getMSTime() + 2000;
                    SuiRaidTelemetry::OnReposition(me, "spread");
                    me->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
                    return true;
                }
            }
        }
    }
    if ((role != ROLE_RANGE_DPS && role != ROLE_HEALER && !woundedMelee && !heldBurstMelee) ||
        !(me->IsInCombat() || HealerOnDuty()) || !me->GetGroup() ||
        (me->IsMoving() && !((heldBurstMelee || woundedMelee) && chasing)) ||
        m_possessed || m_suiManual)
        return false;
    uint32 const now = WorldTimer::getMSTime();
    if (now < m_rangedSpacingNextMs)
        return false;
    // A crowd-control cast is finished before any step: the step cancels it, and the mob it was for
    // keeps beating on me (2026-11-07, Majordomo: the same mage began Polymorph on one Flamewaker
    // Healer three times in 2 s while it hit him; the Healers were loose for 50 s and killed the casters).
    if (Spell const* cast = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (SpellEntry const* info = cast->m_spellInfo)
            if (info->Mechanic == MECHANIC_POLYMORPH || info->Mechanic == MECHANIC_BANISH ||
                info->Mechanic == MECHANIC_SHACKLE || info->Mechanic == MECHANIC_SLEEP)
            {
                if (now >= m_ccCastHoldLogMs)
                {
                    m_ccCastHoldLogMs = now + 5000;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CC] %s: holds still to finish %s",
                        me->GetName(), info->SpellName[0].c_str());
                }
                return false;
            }
    // A healer finishes a heal that lands within 1.2 s unless it is below half itself: the nine healers cast
    // 63 % of the time and landed one heal per 7 s each, 2.2k healing a second for the raid, while the
    // steps cut their casts (2026-11-09, Majordomo attempt 54).
    if (role == ROLE_HEALER && me->GetHealthPercent() > 50.0f)
        if (Spell const* cast = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (cast->GetCastTime() > 0 && cast->GetCastedTime() <= 1200)
                return false;
    // ...and a wanted one is begun before any step while I am above half health: at the pull the mages
    // stepped out of an Elite's 14 yd circle for 10 s and one Healer of four was sheeped; the three loose
    // ones killed the paladin tank and the puller by 15 s (2026-11-08, Majordomo attempt 52).
    if (role == ROLE_RANGE_DPS && me->IsInCombat() && me->GetHealthPercent() > 50.0f &&
        ((me->GetClass() == CLASS_MAGE && TrySpecPolymorphSpareElite()) ||
         (me->GetClass() == CLASS_WARLOCK && TrySpecBanishSpareElite())))
        return true;

    std::list<Unit*> nearby;
    me->GetEnemyListInRadiusAround(me, 40.0f, nearby);
    // A healer holds its ground while a tank in its reach is going down and nothing is hitting the healer:
    // a burst circle is healed through, a dead tank is not (2026-11-08, Majordomo: four healers within 15 yd
    // of the paladin tank, two idle at full mana, stepped out of a Healer's 21 yd circle while he died with
    // 756 healed in 8 s).
    if (role == ROLE_HEALER && me->GetHealthPercent() > 50.0f && me->GetGroup())
    {
        bool hitMe = false;
        for (Unit* u : nearby)
            if (u && u->IsAlive() && u->GetVictim() == me)
                hitMe = true;
        if (!hitMe)
            for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
            {
                Player* t = itr->getSource();
                AiBotAI const* tai = t && t != me && t->IsAlive() && t->IsInMap(me) ? dynamic_cast<AiBotAI const*>(t->AI()) : nullptr;
                if (tai && tai->GetCombatActiveRole() == ROLE_TANK && t->GetHealthPercent() < 50.0f &&
                    me->IsWithinDistInMap(t, 36.0f) && me->IsWithinLOSInMap(t))
                {
                    if (now >= m_holdForTankLogMs)
                    {
                        m_holdForTankLogMs = now + 5000;
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HEAL] %s: holds its ground - %s at %.0f%% in reach",
                            me->GetName(), t->GetName(), t->GetHealthPercent());
                    }
                    return false;
                }
            }
    }
    Unit* closest = nullptr;
    float closestDist = 1e9f;
    float keepDist = 14.0f;
    bool pulseKeep = false;
    float worst = 0.0f;
    // Healers stay out of a burst circle all fight; a ranged damage dealer stands in it while it
    // can take it (outside, most spells do not reach: Shazzrah's Arcane Explosion reaches 30 yd,
    // and with every caster outside the raid did 1.5k/s) and steps out below 60 % until 90 %.
    bool const pulseOut = role == ROLE_HEALER ||
        me->GetHealthPercent() < 40.0f || (m_pulseSteppedOut && me->GetHealthPercent() < 70.0f);
    m_pulseSteppedOut = role != ROLE_HEALER && pulseOut;
    bool const sleepersWake = SleepersMayWake(nearby);
    for (Unit* u : nearby)
    {
        if (!u || !u->IsCreature() || !u->IsAlive() || !u->IsInCombat())
            continue;
        // A crowd-controlled kind that bursts is kept clear of all the same: it wakes where it
        // stands, and a script can wake every sheep at once (2026-11-06, Majordomo: the 4th add
        // died, the Healers turned immune to Polymorph, and the first Shadow Shock killed four
        // casters standing beside the sheeps).
        bool const held = u->HasBreakableByDamageCrowdControlAura();
        if (held && (!sleepersWake || !SuiRaidTelemetry::EntryPulseRadius(u->GetEntry())))
            continue;
        if (heldBurstMelee && !held)
            continue;   // that melee minds only the sleeping bursters; the awake ones are its fight
        // A multiplier's spawn (Lava Spawn) is killed, not walked from: it chases whoever it hits, and ranged stepping clear
        // of every spawn near them cast 19 % of the fight (2026-11-11, week 1 fight 21 and week 3 fight 7: two Firelords,
        // 120-180 spawns, 342 clear steps, one Firelord left at 7 % - all 40 dead).
        if (IsKnownMultiplier(u->GetEntry()) && !SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()))
            continue;
        // ...and so is any small add beating on someone else: it hits for little and is the tanks' to gather (2026-11-14,
        // Onyxia phase 2: ~70 whelps; casters and healers stepped clear of whelps on others 326 times a fight).
        if (!static_cast<Creature*>(u)->IsElite() && !static_cast<Creature*>(u)->IsWorldBoss() && u->GetVictim() != me &&
            !SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()))
            continue;
        float const d = me->GetDistance(u);
        // A kind seen pulsing fire around itself (an inferno): ranged and healers stay outside
        // that circle all fight long - from 1 yd inside it the first four ticks kill a caster
        // before it can walk out (2026-09-25, Baron Geddon).
        float keep = held ? 0.0f : 14.0f;
        bool pulse = false;
        uint32 const rhythm = SuiRaidTelemetry::EntryBurstInterval(u->GetEntry());
        // (Keeping every caster out of a rapid burster left the damage to hunters and melee:
        // Shazzrah's 30 yd explosion is wider than most spells reach - 1.7k/s. Casters stand in
        // it, drink its school's protection and step out when wounded; healers always keep out.)
        uint32 kin = 0;
        for (Unit* o : nearby)
            if (o && o->IsAlive() && o->IsInCombat() && o->GetEntry() == u->GetEntry())
                ++kin;
        // bursting every few seconds, counting every one of its kind in the fight: stand at its rim
        bool const rapid = rhythm > 0 && rhythm < 10000u * std::max<uint32>(kin, 1u);
        // a burst that hits for under a quarter of my health is healed through, not danced around
        uint32 const burstHit = SuiRaidTelemetry::EntryBurstMaxHit(u->GetEntry());
        bool const light = burstHit > 0 && burstHit * 4 * std::max<uint32>(kin, 1u) < me->GetMaxHealth();
        // A circle narrower than a spell's reach costs a caster nothing to stand outside: only one as
        // wide as Shazzrah's 30 yd is stood in (2026-11-07: rooted in his Inferno, Baron Geddon's 20 yd
        // circle killed ranged who kept only 14 yd - "Inferno 2000" deaths at 81-100 s).
        float const reachable = SuiRaidTelemetry::EntryPulseRadius(u->GetEntry());
        bool const narrow = reachable > 0.0f && reachable + 1.2f <= 28.0f;
        // (a wounded melee leaves the whole circle too: it kept 14 yd inside a 20 yd Shadow Shock and three rogues
        // took two more at 20-48 % - 2026-11-09, Majordomo attempt 87)
        // (playbook standInPulse: a boss whose circle the raid stands in - Ragnaros' Wrath, 1000 + knockback every ~26 s:
        // kept out of it, ranged cast 54 % of the fight and the raid did 3.1k/s; standing in it, 70 % and 4.6k/s - 2026-11-10)
        if (!light && !SuiRaidPlaybook::StandInPulse(u->GetEntry()) && (woundedMelee || pulseOut || rapid || held || narrow))
            if (float const pr = SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()))
                // `d` is edge to edge already, and the burst reaches pr past both edges: subtracting
                // the sizes again stood healers inside it (same law as CollectGroundHazards)
                if (pr + 1.2f > keep)
                {
                    keep = pr + 1.2f;
                    pulse = true;
                }
        if (d >= keep)
            continue;
        if (keep - d > worst)
        {
            worst = keep - d;
            closestDist = d;
            closest = u;
            keepDist = keep;
            pulseKeep = pulse;
        }
    }
    if (!closest)
    {
        // A stepped-out melee holds here (no re-engage) until it has been healed, or until the sleeping
        // burster beside its target is awake or dead.
        if (woundedMelee || heldBurstMelee)
            return true;
        // A ranged damage dealer whose target is out of sight closes in along the path until it
        // can see it (2026-09-24: most Frostbolts and every hunter shot failed LINE_OF_SIGHT from
        // the raid's camp around a corner - mages did less than one rogue).
        // (a cast just refused for sight is enough, wherever the anchor stands: Majordomo's Healers are tanked
        // 50+ yd from the leader on the boss, and six casters were refused 59-271 times a fight without one
        // step - 2026-11-09, attempt 78; the walk still stops at the first spot in sight 30 yd from the target)
        if (role == ROLE_RANGE_DPS && !me->IsNonMeleeSpellCasted(false))
            if (Unit* target = me->GetVictim())
                if (target->IsAlive() && !me->IsWithinLOSInMap(target) && target->GetVictim() &&
                    me->GetGroup()->IsMember(target->GetVictim()->GetCharmerOrOwnerOrOwnGuid()) &&
                    (!FindEscortBoss() || FindEscortBoss()->IsWithinDistInMap(target, 45.0f) ||
                     (m_losRefusedGuid == target->GetObjectGuid() && now - m_losRefusedMs < 4000 && me->IsWithinDistInMap(target, 70.0f))))
                {
                    // Only the group's own fight next to the tank: never walk off toward a
                    // stale target (2026-09-24: a hunter wandered 77 yd and pulled a Firelord).
                    m_rangedSpacingNextMs = now + 2500;
                    PathInfo path(me);
                    path.calculate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
                    PointsArray const& points = path.getPath();
                    std::vector<GroundHazard> idleNear;
                    CollectIdleHostiles(me, 60.0f, idleNear);
                    // Walk the path in 2 yd steps to the first spot with sight of the target, in
                    // spell reach, outside a circle it is known to burn and outside an idle
                    // pack's reach (2026-09-25: walking to 20 yd put the casters inside Baron
                    // Geddon's Inferno circle, and the keep-out walked them back behind a pillar -
                    // hundreds of LINE_OF_SIGHT failures a fight).
                    float keepOut = 0.0f;
                    if (target->IsCreature())
                        if (float const pr = SuiRaidTelemetry::EntryPulseRadius(target->GetEntry()))
                            keepOut = pr + 1.2f - target->GetObjectBoundingRadius();
                    for (size_t i = 1; i < points.size(); ++i)
                    {
                        Vector3 const seg = points[i] - points[i - 1];
                        float const len = seg.length();
                        for (float s = 2.0f; s <= len + 0.01f; s += 2.0f)
                        {
                            Vector3 const q = points[i - 1] + seg * (len > 0.0f ? std::min(1.0f, s / len) : 1.0f);
                            float const d = target->GetDistance2d(q.x, q.y);
                            if (InHazard(idleNear, q.x, q.y) || d < keepOut || d > 30.0f)
                                continue;
                            if (!SeesFrom(me, q.x, q.y, q.z, target))
                                continue;
                            CB_HIT(me->GetGUIDLow(), "cpp-combat: ranged closing for line of sight"); m_repositionUntilMs = WorldTimer::getMSTime() + 3000; SuiRaidTelemetry::OnReposition(me, "sight");
                            me->GetMotionMaster()->MovePoint(0, q.x, q.y, q.z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                            return true;
                        }
                    }
                    // Nothing on the straight path: the nearest spot round the target, outside its burst and every
                    // other burster's, in reach and in sight (2026-11-09, Majordomo attempt 79: the path from the camp
                    // to a Healer tanked round a rock had no point 19-30 yd out with sight - 883 refused casts).
                    {
                        std::list<Unit*> foes;
                        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck foeCheck(me, me, 70.0f);
                        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> foeSearcher(foes, foeCheck);
                        Cell::VisitAllObjects(me, foeSearcher, 70.0f);
                        std::vector<std::pair<float, Vector3>> spots;
                        float const inner = std::max(keepOut + 2.0f, 10.0f);
                        for (float r : { inner, (inner + 29.0f) * 0.5f, 29.0f })
                        {
                            if (r < inner || r > 29.0f)
                                continue;
                            for (int a = 0; a < 16; ++a)
                            {
                                float const ang = a * float(M_PI) / 8.0f;
                                float x = target->GetPositionX() + std::cos(ang) * r, y = target->GetPositionY() + std::sin(ang) * r;
                                float z = target->GetPositionZ();
                                ReGroundZ(x, y, z, "ranged-sight");
                                if (std::fabs(z - target->GetPositionZ()) > 6.0f || InHazard(idleNear, x, y) || Molten(me, x, y, z) ||
                                    !SeesFrom(me, x, y, z, target))
                                    continue;
                                bool burnt = false;
                                for (Unit* f : foes)
                                    if (f != target && f->IsAlive() && f->IsInCombat() && f->IsCreature() &&
                                        !f->HasBreakableByDamageCrowdControlAura())
                                        if (float const fr = SuiRaidTelemetry::EntryPulseRadius(f->GetEntry()))
                                            if (f->GetDistance2d(x, y) < fr + 1.2f)
                                                burnt = true;
                                if (!burnt)
                                    spots.push_back({ me->GetDistance(x, y, z), Vector3(x, y, z) });
                            }
                        }
                        std::sort(spots.begin(), spots.end(), [](auto const& l, auto const& r) { return l.first < r.first; });
                        for (size_t k = 0; k < spots.size() && k < 4; ++k)
                        {
                            Vector3 const& q = spots[k].second;
                            if (spots[k].first > 45.0f || !Walkable(me, q.x, q.y, q.z))
                                continue;
                            m_repositionUntilMs = WorldTimer::getMSTime() + 3000;
                            SuiRaidTelemetry::OnReposition(me, "sight");
                            me->GetMotionMaster()->MovePoint(0, q.x, q.y, q.z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                            return true;
                        }
                    }
                    if (now - m_sightLogMs > 10000)
                    {
                        m_sightLogMs = now;
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-SIGHT] %s: no spot in sight of %s on a %u-point path (%.0f yd, keep-out %.0f)",
                            me->GetName(), target->GetName(), uint32(points.size()), me->GetDistance(target), keepOut);
                    }
                    return false;
                }
        // Spread: area damage lands on clumps (2026-09-24: a Lava Surger's Surge - a charge that
        // burns everyone near where it lands - did 120k to a raid standing in formation). A ranged
        // member or healer with two or more others within 5 yd steps to the least crowded spot 6-10
        // yd away that keeps its reach and sight.
        if (!me->IsNonMeleeSpellCasted(false) && me->IsInCombat())
        {
            Group* group = me->GetGroup();
            auto crowd = [&](float x, float y, float r)
            {
                uint32 n = 0;
                for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                {
                    Player* o = itr->getSource();
                    if (o && o != me && o->IsAlive() && o->IsInWorld() && o->IsInMap(me) &&
                        o->GetDistance2d(x, y) < r)
                        ++n;
                }
                return n;
            };
            // A foe in the fight whose spells burst around their target: that far apart, alone.
            float splash = 0.0f;
            {
                std::list<Unit*> foes;
                me->GetEnemyListInRadiusAround(me, 100.0f, foes);
                // (not a multiplier's spawns: 200 Lava Spawns made every caster spread 531 times in one fight - 2026-11-12,
                // week 3, two Firelords)
                for (Unit* f : foes)
                    if (f && f->IsCreature() && f->IsAlive() && f->IsInCombat() && !IsKnownMultiplier(f->GetEntry()))
                        splash = std::max({splash, SuiRaidTelemetry::EntrySplashRadius(f->GetEntry()),
                            SuiRaidPlaybook::SpreadYards(f->GetEntry())});
            }
            float const apart = splash > 0.0f ? std::max(5.0f, splash + 1.0f) : 5.0f;
            if (crowd(me->GetPositionX(), me->GetPositionY(), apart) >= (splash > 0.0f ? 1u : 2u))
            {
                Player* anchor = role == ROLE_HEALER ? ActiveTank() : nullptr;
                if (!anchor)
                    anchor = FindEscortBoss();
                Unit* victim = me->GetVictim();
                std::vector<GroundHazard> idleNear;
                CollectIdleHostiles(me, 45.0f, idleNear);
                std::vector<GroundHazard> burning;
                CollectGroundHazards(me, burning);
                float bestX = 0, bestY = 0, bestZ = 0;
                float const ring = splash > 0.0f ? apart : 6.0f;
                uint32 bestN = crowd(me->GetPositionX(), me->GetPositionY(), ring);
                for (float dist : {ring, ring * 1.4f, ring * 1.8f})
                    for (int i = 0; i < 12; ++i)
                    {
                        float const ang = i * float(M_PI) / 6.0f;
                        float x = me->GetPositionX() + dist * std::cos(ang), y = me->GetPositionY() + dist * std::sin(ang);
                        float z = me->GetPositionZ();
                        me->UpdateAllowedPositionZ(x, y, z);
                        if (std::fabs(z - me->GetPositionZ()) > 4.0f || !me->IsWithinLOS(x, y, z + 1.5f))
                            continue;
                        if (me->GetTerrain()->getLiquidStatus(x, y, z + 0.01f, MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME, nullptr) &
                            (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK))
                            continue;
                        // (an anchor already beyond the limit only forbids spots farther than now: 2026-11-14, Onyxia phase 2 -
                        // healers stood stacked in pairs 66-71 yd from their tank, no spot passed, and one Fireball's splash
                        // gave Engulfing Flames to the whole clump)
                        if (anchor && anchor != me && (anchor->GetDistance(x, y, z) >
                                std::max(role == ROLE_HEALER ? 28.0f : 35.0f, anchor->GetDistance(me) + 2.0f) ||
                            !SeesFrom(me, x, y, z, anchor)))
                            continue;
                        if (victim && role == ROLE_RANGE_DPS && (victim->GetDistance(x, y, z) > std::max(33.0f, victim->GetDistance(me) + 2.0f) ||
                            !SeesFrom(me, x, y, z, victim)))
                            continue;
                        if (InHazard(idleNear, x, y) || InHazard(burning, x, y))
                            continue;
                        uint32 const n = crowd(x, y, ring);
                        if (n < bestN && Walkable(me, x, y, z))
                        {
                            bestN = n;
                            bestX = x;
                            bestY = y;
                            bestZ = z;
                        }
                    }
                if (bestX != 0 || bestY != 0)
                {
                    m_rangedSpacingNextMs = now + 3000;
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: spreading out"); m_repositionUntilMs = WorldTimer::getMSTime() + 2000; SuiRaidTelemetry::OnReposition(me, "spread");
                    me->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
                    return true;
                }
            }
        }
        // Nothing too close: a healer that lost reach or sight of the tank closes back in - the
        // tank actually holding the fight, not the raid's leader (2026-09-25: Baron Geddon's tank
        // died with all ten healers 51-59 yd away beside the leader).
        Player* tank = nullptr;
        // ...and a wounded tank no healer sees (a second tank holding one add round a corner, or a
        // tank Majordomo ported away) brings its nearest healer into sight (2026-11-06: an add tank
        // died with 0 healing in 8 s and 9 of 10 healers out of sight, all of them on the main tank).
        // Out of heal range counts as unseen (2026-11-08, Majordomo: two add tanks 50-60 yd from all nine
        // healers, in plain sight, died in the same second with nothing healed in 8 s).
        if (role == ROLE_HEALER && me->GetGroup())
        {
            float lowest = 75.0f;
            for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
            {
                Player* t = itr->getSource();
                if (!t || t == me || !t->IsAlive() || !t->IsInWorld() || !t->IsInMap(me) ||
                    !t->IsInCombat() || t->GetAttackers().empty() || t->GetHealthPercent() >= lowest ||
                    !me->IsWithinDistInMap(t, 80.0f) || (me->IsWithinDistInMap(t, 40.0f) && me->IsWithinLOSInMap(t)))
                    continue;
                AiBotAI* tai = dynamic_cast<AiBotAI*>(t->AI());
                if (!tai || tai->GetCombatActiveRole() != ROLE_TANK)
                    continue;
                bool seen = false;
                Player* nearest = nullptr;
                float nearestD = 1e9f;
                for (GroupReference* jtr = me->GetGroup()->GetFirstMember(); jtr && !seen; jtr = jtr->next())
                {
                    Player* h = jtr->getSource();
                    if (!h || !h->IsAlive() || !h->IsInMap(me) || h == t)
                        continue;
                    AiBotAI* hai = dynamic_cast<AiBotAI*>(h->AI());
                    if (!hai || hai->GetCombatActiveRole() != ROLE_HEALER)
                        continue;
                    if (h->IsWithinDistInMap(t, 40.0f) && h->IsWithinLOSInMap(t))
                        seen = true;
                    else if (h->GetDistance(t) < nearestD)
                    {
                        nearestD = h->GetDistance(t);
                        nearest = h;
                    }
                }
                if (!seen && nearest == me)
                {
                    lowest = t->GetHealthPercent();
                    tank = t;
                }
            }
        }
        if (!tank)
            tank = ActiveTank();
        if (!tank)
            tank = FindEscortBoss();
        if (role != ROLE_HEALER || !tank || tank == me || !tank->IsAlive())
            return false;
        // [TACTICS] 2026-11-22 R4W12 Onyxia att. 1 (57 %): Gankreaper stepped out of her next Deep Breath lane at 13:00:51, then
        // "closing on Stormdoom (40 yd)" at 13:00:53 walked it back in; it and Shiftftw died to the breath at 13:01:06 and the
        // healer-less raid fell to the whelps. A healer holding its lane spot or a forecast-breath escape does not close on a
        // tank. (m_hLaneUntilMs / m_breathEscUntilMs are only set under SuiTactic::HealerLaneClear / DeepBreathEscape.)
        {
            uint32 const holdNow = WorldTimer::getMSTime();
            if (holdNow < m_hLaneUntilMs || holdNow < m_breathEscUntilMs)
                if (Creature* ony = me->FindNearestCreature(10184, 150.0f, true))
                    if (SuiTactic::On(me, SuiTactic::HealerLaneClear, ony))
                        return false;
        }
        // The tank's enemy pulses fire around itself: heal from outside that circle (heal range is
        // 40 yd), rather than walking back into it between bursts.
        float pulseKeepOut = 0.0f;
        Unit* tankFoe = nullptr;
        for (Unit* a : tank->GetAttackers())
            if (a && a->IsAlive() && a->IsCreature())
                // measured from its edge (GetDistance2d of a point drops its size), plus mine
                if (float const pr = SuiRaidTelemetry::EntryPulseRadius(a->GetEntry()))
                    if (pr + me->GetObjectBoundingRadius() + 1.5f > pulseKeepOut)
                    {
                        pulseKeepOut = pr + me->GetObjectBoundingRadius() + 1.5f;
                        tankFoe = a;
                    }
        bool const inReach = me->IsWithinDistInMap(tank, pulseKeepOut > 0.0f ? 38.0f : 28.0f) &&
            me->IsWithinLOSInMap(tank);
        if (inReach)
            return false;
        if (Spell* cast = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            // Busy healing someone else while the tank is out of this healer's reach: the heal
            // can wait, the tank cannot - and a healer always casting never walked back in at
            // all (2026-09-24: behind a rock; 2026-09-25: 42-83 yd from Baron Geddon's tank).
            if (cast->m_targets.getUnitTargetGuid() == tank->GetObjectGuid())
                return false;
            me->InterruptNonMeleeSpells(false);
        }
        else if (me->IsNonMeleeSpellCasted(false))
            return false;
        m_rangedSpacingNextMs = now + 2000;
        // The nearest spot on rings around the tank that sees it, stays out of any circle its enemy
        // burns and of idle packs, is not lava, and has a path: a walk along the path toward the
        // tank alone found none in 161 of 186 tries beside Baron Geddon (2026-09-25).
        std::vector<GroundHazard> idleNear;
        CollectIdleHostiles(me, 60.0f, idleNear);
        std::vector<GroundHazard> burning;
        CollectGroundHazards(me, burning);
        struct Cand { float x, y, z, score; };
        std::vector<Cand> cands;
        // In front of or behind a kind that breathes a cone and sweeps its tail (its flanks are 70-110 degrees off).
        auto inArc = [&](float x, float y)
        {
            for (Unit* a : tank->GetAttackers())
                if (a && a->IsAlive() && a->IsCreature())
                    if (float const cone = SuiRaidTelemetry::EntryConeRadius(a->GetEntry()))
                        if (a->GetDistance2d(x, y) < cone + a->GetObjectBoundingRadius() + 3.0f)
                        {
                            float rel = a->GetAngle(x, y) - a->GetOrientation();
                            while (rel > float(M_PI)) rel -= 2.0f * float(M_PI);
                            while (rel < -float(M_PI)) rel += 2.0f * float(M_PI);
                            float const off = std::fabs(rel);
                            if (off < float(M_PI) * 7.0f / 18.0f)
                                return 1;   // its front
                            // [TACTICS] 2026-11-19 R4W0 Onyxia drill 4 pull 11: her tail was given her breath's reach (45 yd +
                            // her size); with her head to the east wall the tail covered the whole way in from the tunnel,
                            // every healer logged "cannot reach Boomwarrior (8 spots)" with no sight and her tank died at +33 s.
                            // Her Tail Sweep never hit beyond 28 yd (CONEHIT, all of 11-19): that is the tail arc here.
                            // Gated by SuiTactic::HealWalkAvoidsArcs.
                            if (off > float(M_PI) * 11.0f / 18.0f &&
                                (!SuiTactic::On(me, SuiTactic::HealWalkAvoidsArcs, a) || a->GetDistance2d(x, y) < 31.0f))
                                return 2;   // its tail
                        }
            return 0;
        };
        int const startArc = inArc(me->GetPositionX(), me->GetPositionY());
        float const rings[3] = { pulseKeepOut > 0.0f ? 30.0f : 20.0f, pulseKeepOut > 0.0f ? 34.0f : 26.0f, pulseKeepOut > 0.0f ? 26.0f : 14.0f };
        for (float r : rings)
            for (int i = 0; i < 24; ++i)
            {
                float const ang = i * float(M_PI) / 12.0f;
                float x = tank->GetPositionX() + r * std::cos(ang), y = tank->GetPositionY() + r * std::sin(ang);
                float z = tank->GetPositionZ();
                me->UpdateAllowedPositionZ(x, y, z);
                if (std::fabs(z - tank->GetPositionZ()) > 6.0f || Molten(me, x, y, z) ||
                    InHazard(burning, x, y) || InHazard(idleNear, x, y))
                    continue;
                if (tankFoe && tankFoe->GetDistance2d(x, y) < pulseKeepOut)
                    continue;
                // ...nor in front of or behind a kind that breathes a cone and sweeps its tail
                // (2026-09-25: healers closing on Onyxia's tank stood in her Tail Sweep).
                bool arc = false;
                for (Unit* a : tank->GetAttackers())
                    if (a && a->IsAlive() && a->IsCreature())
                        if (float const cone = SuiRaidTelemetry::EntryConeRadius(a->GetEntry()))
                            if (a->GetDistance2d(x, y) < cone + a->GetObjectBoundingRadius() + 3.0f)
                            {
                                float rel = a->GetAngle(x, y) - a->GetOrientation();
                                while (rel > float(M_PI)) rel -= 2.0f * float(M_PI);
                                while (rel < -float(M_PI)) rel += 2.0f * float(M_PI);
                                float const off = std::fabs(rel);
                                // (her tail is 28 yd, not her breath's reach - see inArc below; SuiTactic::HealWalkAvoidsArcs)
                                if (off < float(M_PI) * 7.0f / 18.0f || (off > float(M_PI) * 11.0f / 18.0f &&
                                    (!SuiTactic::On(me, SuiTactic::HealWalkAvoidsArcs, a) || a->GetDistance2d(x, y) < 31.0f)))
                                    arc = true;
                            }
                if (arc)
                    continue;
                if (!SeesFrom(me, x, y, z, tank))
                    continue;
                // ...nor onto another member or a spot another healer is walking to (2026-11-14, Onyxia phase 2: healers
                // closing on one tank took the same ring spot in pairs; a Fireball splash there is Engulfing Flames on both)
                float penalty = 0.0f;
                for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
                    if (Player* o = itr->getSource())
                        if (o != me && o->IsAlive() && o->IsInMap(me) && o->GetDistance2d(x, y) < 7.0f)
                            penalty += 12.0f;
                {
                    std::lock_guard<std::mutex> guard(gHealSpotLock);
                    for (auto const& c : gHealSpots)
                        if (c.who != me->GetObjectGuid() && now < c.untilMs &&
                            (c.x - x) * (c.x - x) + (c.y - y) * (c.y - y) < 49.0f)
                            penalty += 25.0f;
                }
                cands.push_back({x, y, z, me->GetDistance(x, y, z) + penalty});
            }
        std::sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b) { return a.score < b.score; });
        uint32 tried = 0;
        for (Cand const& c : cands)
        {
            if (++tried > 10)
                break;
            PathInfo path(me);
            path.calculate(c.x, c.y, c.z, false);
            if (path.getPathType() & PATHFIND_NOPATH)
                continue;
            // A ring point a little off the mesh reads "incomplete": fine if the walk ends close.
            PointsArray const& pts = path.getPath();
            if (pts.empty() || (path.getPathType() & PATHFIND_INCOMPLETE &&
                (pts.back() - Vector3(c.x, c.y, c.z)).length() > 4.0f))
                continue;
            float walk = 0.0f;
            for (size_t k = 1; k < pts.size(); ++k)
                walk += (pts[k] - pts[k - 1]).length();
            if (walk > c.score * 2.0f + 10.0f)
                continue;   // a detour round a ridge, not a step into sight
            // ...and the walk there stays out of the fire too, not only its end: the way to a tank
            // on the far side of a boss runs through his circle (2026-11-06: two paladins "closing on"
            // Baron Geddon's tank walked through his Inferno and died to its last tick).
            bool const startBurning = InHazard(burning, me->GetPositionX(), me->GetPositionY()) ||
                (tankFoe && tankFoe->GetDistance2d(me->GetPositionX(), me->GetPositionY()) < pulseKeepOut);
            bool through = false;
            for (size_t k = 1; k < pts.size() && !through && !startBurning; ++k)
            {
                Vector3 const d = pts[k] - pts[k - 1];
                int const n = std::max(1, int(d.length() / 2.0f));
                for (int j = 1; j <= n && !through; ++j)
                {
                    Vector3 const p = pts[k - 1] + d * (float(j) / n);
                    through = InHazard(burning, p.x, p.y) ||
                        (tankFoe && tankFoe->GetDistance2d(p.x, p.y) < pulseKeepOut);
                }
            }
            if (through)
                continue;
            // ...and not across a breather's front or tail on the way: healers walking from Onyxia's tail round to a
            // flank spot crossed her front together and one Flame Breath killed eight of them at 21-40 yd (2026-11-15,
            // R3W2 Onyxia attempt 3, phase 3). The arc I start in is the way out of it; the other is not crossed.
            bool crosses = false;
            bool const arcWalkOn = SuiTactic::OnAnyAttacker(tank, SuiTactic::HealWalkAvoidsArcs);
            for (size_t k = 1; k < pts.size() && !crosses && arcWalkOn; ++k)
            {
                Vector3 const d = pts[k] - pts[k - 1];
                int const n = std::max(1, int(d.length() / 2.0f));
                for (int j = 1; j <= n && !crosses; ++j)
                {
                    Vector3 const p = pts[k - 1] + d * (float(j) / n);
                    int const arcHere = inArc(p.x, p.y);
                    crosses = arcHere != 0 && arcHere != startArc;
                }
            }
            if (crosses)
                continue;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HEAL] %s: closing on %s (%.0f yd%s) -> %.0f yd from it",
                me->GetName(), tank->GetName(), me->GetDistance(tank), me->IsWithinLOSInMap(tank) ? "" : ", no sight",
                tank->GetDistance(c.x, c.y, c.z));
            {
                std::lock_guard<std::mutex> guard(gHealSpotLock);
                gHealSpots.erase(std::remove_if(gHealSpots.begin(), gHealSpots.end(), [&](HealSpotClaim const& h)
                    { return h.who == me->GetObjectGuid() || now >= h.untilMs; }), gHealSpots.end());
                gHealSpots.push_back({me->GetObjectGuid(), c.x, c.y, now + 4000});
            }
            CB_HIT(me->GetGUIDLow(), "cpp-combat: healer closing to heal range of the tank"); m_repositionUntilMs = WorldTimer::getMSTime() + 3000; SuiRaidTelemetry::OnReposition(me, "healrange");
            me->GetMotionMaster()->MovePoint(0, c.x, c.y, c.z, MOVE_PATHFINDING | MOVE_RUN_MODE);
            return true;
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HEAL] %s: cannot reach %s (%.0f yd%s, %u spots, keep-out %.0f)",
            me->GetName(), tank->GetName(), me->GetDistance(tank), me->IsWithinLOSInMap(tank) ? "" : ", no sight",
            uint32(cands.size()), pulseKeepOut);
        return false;
    }
    // A cast in progress is kept unless the hostile is close enough for point-blank area
    // damage to reach (2026-09-24: healers chain-casting beside a Molten Giant never moved
    // and died to its 10 yd Smash).
    // A clear step that twice in a row did not move me (the spot is never reached) is given up for a while:
    // each try dropped the cast (2026-11-08, Majordomo: a mage picked the same spot 13 times in 26 s while the
    // released Healer stood untouched - the ranged cast 13 % of the fight).
    if (now < m_spacingHoldUntilMs && closestDist > 11.0f)
        return false;
    if (me->IsNonMeleeSpellCasted(false))
    {
        // Inside the circle of a kind that bursts every few seconds the cast is dropped too:
        // healers finishing heal after heal stood 20-30 s in Shazzrah's Arcane Explosion
        // (2026-09-25). Anything else waits for the cast unless it is point-blank close.
        if (closestDist > 11.0f && !pulseKeep)
            return false;
        me->InterruptNonMeleeSpells(false);
    }
    m_rangedSpacingNextMs = now + 2000;

    float dx = me->GetPositionX() - closest->GetPositionX(), dy = me->GetPositionY() - closest->GetPositionY();
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5f)
    {
        float const o = me->GetOrientation() + M_PI_F;
        dx = std::cos(o);
        dy = std::sin(o);
        len = 1.0f;
    }
    dx /= len;
    dy /= len;
    // An elite beating on me is brought to a tank, not run away from: a few yards past the nearest
    // tank, where it can take it off me (2026-11-06, Majordomo: after each of his threat resets he
    // went for a healer, who stepped away from the tanks; seven healers died 2-4 s after his swap).
    if (closest->GetVictim() == me && closest->IsCreature() && static_cast<Creature*>(closest)->IsElite() && me->GetGroup())
    {
        Player* tankTo = nullptr;
        float bestT = 40.0f;
        for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->getSource())
                if (m != me && m->IsAlive() && m->IsInWorld() && m->IsInMap(me))
                    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI()))
                        if (ai->GetCombatActiveRole() == ROLE_TANK && me->GetDistance(m) < bestT)
                        {
                            bestT = me->GetDistance(m);
                            tankTo = m;
                        }
        if (tankTo && bestT > 6.0f)
        {
            float vx = tankTo->GetPositionX() - me->GetPositionX(), vy = tankTo->GetPositionY() - me->GetPositionY();
            float const vl = std::max(0.5f, std::sqrt(vx * vx + vy * vy));
            float x = tankTo->GetPositionX() + vx / vl * 3.0f, y = tankTo->GetPositionY() + vy / vl * 3.0f;
            float z = tankTo->GetPositionZ();
            me->UpdateAllowedPositionZ(x, y, z);
            std::vector<GroundHazard> burningTo;
            CollectGroundHazards(me, burningTo);
            if (std::fabs(z - tankTo->GetPositionZ()) < 4.0f && !Molten(me, x, y, z) && !InHazard(burningTo, x, y) &&
                Walkable(me, x, y, z))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-HAZARD] %s: %s is on me - to %s (%.0f yd)",
                    me->GetName(), closest->GetName(), tankTo->GetName(), bestT);
                m_repositionUntilMs = WorldTimer::getMSTime() + 3000;
                SuiRaidTelemetry::OnReposition(me, "totank");
                me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                return true;
            }
        }
    }
    Player* anchor = role == ROLE_HEALER ? ActiveTank() : nullptr;
    if (!anchor)
        anchor = FindEscortBoss();
    // (out of a burst circle by 5 yd, not onto its rim: its tank walks it a yard and the ring steps again -
    // 2026-11-08, Majordomo: 186 of 357 sampled clear steps ran from the one Healer being killed)
    float const step = std::max(22.0f, keepDist + (pulseKeep ? 5.0f : 1.0f)) - closestDist;
    std::vector<GroundHazard> idleNear;
    CollectIdleHostiles(me, 50.0f, idleNear);
    // Try straight away, then 45 and 90 degrees either side, keeping near the anchor - first only spots
    // that still see what I cast on (my target; a healer's tank), then the old way: a step behind a pillar
    // costs every cast after it (2026-11-08, Majordomo: ranged casts refused for line of sight 3,286 times
    // against 375 that went off; the clear step never looked).
    Unit* const sightOf = role == ROLE_HEALER ? static_cast<Unit*>(anchor) :
        role == ROLE_RANGE_DPS ? me->GetVictim() : nullptr;
    for (int pass = sightOf && sightOf != me && sightOf->IsAlive() ? 0 : 1; pass < 2; ++pass)
    for (float turn : { 0.0f, 0.785f, -0.785f, 1.571f, -1.571f })
    {
        if (pass == 1 && std::fabs(turn) > 1.0f)
            continue;   // (the old way tried three directions)
        float const c = std::cos(turn), s = std::sin(turn);
        float const ux = dx * c - dy * s, uy = dx * s + dy * c;
        float x = me->GetPositionX() + ux * step, y = me->GetPositionY() + uy * step;
        float z = me->GetPositionZ();
        ReGroundZ(x, y, z, "ranged-spacing");
        if (std::fabs(z - me->GetPositionZ()) > 5.0f)
            continue;
        // Healers keep the tank inside heal range; ranged may use the full shooting range.
        if (anchor && anchor != me && anchor->GetDistance(x, y, z) > (role == ROLE_HEALER ? (pulseKeep ? 38.0f : 28.0f) : 38.0f))
            continue;
        if (pass == 0 && (!SeesFrom(me, x, y, z, sightOf) ||
            (role == ROLE_RANGE_DPS && sightOf->GetDistance(x, y, z) > 33.0f)))
            continue;
        if (InHazard(idleNear, x, y) || !Walkable(me, x, y, z))
            continue;
        if (me->GetDistance2d(m_spacingFromX, m_spacingFromY) < 1.5f)
        {
            if (++m_spacingFutile >= 2)
            {
                m_spacingFutile = 0;
                m_spacingHoldUntilMs = now + 8000;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-SPACING] %s: two clear steps from %s went nowhere - holds and casts 8 s",
                    me->GetName(), closest->GetName());
                return false;
            }
        }
        else
            m_spacingFutile = 0;
        m_spacingFromX = me->GetPositionX();
        m_spacingFromY = me->GetPositionY();
        CB_HIT(me->GetGUIDLow(), "cpp-combat: ranged stepping clear of a close hostile"); m_repositionUntilMs = WorldTimer::getMSTime() + 3000; SuiRaidTelemetry::OnReposition(me, "clear");
        // (diagnostic 2026-11-08: which enemy each clear step runs from - Majordomo's ranged spent 28 % of
        // the fight moving, 299 clear steps)
        if (now >= m_spacingWhyLogMs)
        {
            m_spacingWhyLogMs = now + 4000;
            Unit* on = closest->GetVictim();
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-CLEAR] %s (%s): from %s entry %u%s on %s, %.1f of %.1f yd%s",
                me->GetName(), role == ROLE_HEALER ? "healer" : role == ROLE_RANGE_DPS ? "ranged" : "melee",
                closest->GetName(), closest->GetEntry(), closest->HasBreakableByDamageCrowdControlAura() ? " asleep" : "",
                on ? (on == me ? "me" : on->GetName()) : "nobody", closestDist, keepDist, pulseKeep ? " circle" : "");
        }
        if (woundedMelee || heldBurstMelee)
            me->AttackStop();
        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }
    return false;
}

// [TACTICS] 2026-09-23 telemetry: the tank died at the start of every pull with no heal
// landing - the healers' in-combat routine only runs once the healer itself is in combat,
// and at the start of a pull only the tank is.
Player* AiBotAI::ActiveTank() const
{
    Group* group = me->GetGroup();
    if (!group)
        return nullptr;
    // A healer stays with its own tank when several hold the fight: the healers are dealt out over the
    // tanks in guid order (2026-11-08, Majordomo: all nine healers stood by the tank with the most attackers
    // while two add tanks 50-74 yd away died in the same second, nothing healed in 8 s).
    if (GetCombatActiveRole() == ROLE_HEALER)
    {
        std::vector<Player*> tanks, healers;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->getSource();
            if (!m || !m->IsAlive() || !m->IsInWorld() || !m->IsInMap(me) || !me->IsWithinDistInMap(m, 80.0f))
                continue;
            AiBotAI const* ai = dynamic_cast<AiBotAI const*>(m->AI());
            if (!ai)
                continue;
            if (ai->GetCombatActiveRole() == ROLE_HEALER)
                healers.push_back(m);
            else if (ai->GetCombatActiveRole() == ROLE_TANK && m->IsInCombat())
            {
                bool held = false;
                for (Unit* a : m->GetAttackers())
                    if (a && a->IsAlive() && a->GetVictim() == m && a->IsCreature() &&
                        (static_cast<Creature*>(a)->IsElite() || static_cast<Creature*>(a)->IsWorldBoss()))
                        held = true;
                if (held)
                    tanks.push_back(m);
            }
        }
        // (the deal holds 15 s while that tank lives and fights: re-dealt every time one tank held nothing for
        // a moment, the healers crossed the fight between tanks every few seconds - 2026-11-08, Majordomo)
        uint32 const nowMs = WorldTimer::getMSTime();
        if (!m_dealtTank.IsEmpty() && nowMs < m_dealtUntilMs)
            if (Player* kept = me->GetMap()->GetPlayer(m_dealtTank))
                if (kept->IsAlive() && kept->IsInCombat() && group->IsMember(kept->GetObjectGuid()) &&
                    me->IsWithinDistInMap(kept, 80.0f) && tanks.size() >= 2)
                    return kept;
        if (tanks.size() >= 2)
        {
            auto byGuid = [](Player* a, Player* b) { return a->GetGUIDLow() < b->GetGUIDLow(); };
            std::sort(tanks.begin(), tanks.end(), byGuid);
            std::sort(healers.begin(), healers.end(), byGuid);
            auto it = std::find(healers.begin(), healers.end(), me);
            if (it != healers.end())
            {
                Player* dealt = tanks[size_t(it - healers.begin()) % tanks.size()];
                m_dealtTank = dealt->GetObjectGuid();
                m_dealtUntilMs = nowMs + 15000;
                return dealt;
            }
        }
    }
    Player* best = nullptr;
    float bestScore = 0.0f;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (!m || !m->IsAlive() || !m->IsInWorld() || !m->IsInMap(me) || !m->IsInCombat() ||
            !me->IsWithinDistInMap(m, 80.0f))
            continue;
        float score = 0.0f;
        for (Unit* attacker : m->GetAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != m)
                continue;
            // A boss in the air picks whom it casts at (2026-11-14, Onyxia phase 2: each Fireball's target, a new caster
            // every 3 s, became the healers' "tank" - they ran 40-70 yd after it and the dying mages got nothing in 8 s).
            if (Creature* cr = attacker->ToCreature())
                if (cr->IsWorldBoss() && (cr->IsFlying() || cr->IsLevitating() || cr->IsHovering()))
                    continue;
            if (Creature* cr = attacker->ToCreature())
                score += cr->IsWorldBoss() ? 10.0f : (cr->IsElite() ? 3.0f : 1.0f);
            else
                score += 1.0f;
        }
        if (score <= 0.0f)
            continue;
        if (AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI()))
            if (ai->GetCombatActiveRole() == ROLE_TANK)
                score += 0.5f;
        if (score > bestScore)
        {
            bestScore = score;
            best = m;
        }
    }
    return best;
}

bool AiBotAI::HealerOnDuty() const
{
    // Healers: whenever the group nearby fights. Everyone else: when the group fights and the
    // bot already has a target - a caster whose first spell never landed (out of sight) is
    // not "in combat" yet and used to skip its rotation and its repositioning entirely.
    if (!me->GetGroup() || (GetCombatActiveRole() != ROLE_HEALER && !me->GetVictim()))
        return false;
    for (GroupReference* itr = me->GetGroup()->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->getSource();
        if (m && m != me && m->IsAlive() && m->IsInWorld() && m->IsInMap(me) &&
            m->IsInCombat() && !m->GetAttackers().empty() &&
            me->IsWithinDistInMap(m, GetCombatActiveRole() == ROLE_HEALER ? 80.0f : 40.0f))
            return true;
    }
    return false;
}
