/*
 * AiBotDoctrinePlayerParty.cpp — the PlayerParty (escort) engagement doctrine (2026-07-07).
 *
 * State: the bot's group contains a REAL player (a non-bot session). A human invited this
 * bot to their party — the human IS the coordinator. Auto-selected by ResolveDoctrine
 * (AiBotDoctrine.cpp) whenever FindPartyBoss() resolves, ranked ABOVE the directive-driven
 * TeamAuto so a stale C# stamp can never out-vote a live human; reverts the tick after the
 * bot leaves the group (kick / disband / logout), swap == reset as always.
 *
 * The spec (Nico, 2026-07-07): "They would attack my targets, and only attack not-my-target
 * if I'm not attacking anything but being attacked; healers keep their rotation." Verbatim
 * the assist-priority ladder, so this doctrine is deliberately the SMALL sibling of TeamAuto:
 * the same resolver rungs with the real player as the anchor, MINUS every self-election path.
 *
 *   AcquireTarget  → ResolvePartyFocus or NOTHING. A companion NEVER falls through to
 *                    SelectGrindTarget / ScanApproachTarget — zero initiation, ever. (The
 *                    escort spine hook in UpdateAI also returns before the task-dispatch
 *                    blocks, so the grind machinery is doubly unreachable; this doctrine
 *                    holds the invariant even if that ordering ever changes.)
 *   MaintainTarget → the focus if one resolves (converge/switch, exactly the TeamAuto
 *                    seam); else KEEP my own still-valid victim (finish the mob that jumped
 *                    me); else nullptr → the spine's SelectAttackTarget, which picks only
 *                    from attackers of me + my group = pure defense, never a fresh pull.
 *
 * ResolvePartyFocus rungs, in order:
 *   1. the boss's live victim            — "attack my targets";
 *   2. the boss's attacker               — "I'm not attacking, but I'm being attacked"
 *                                          (GetAttackerForHelper: one deterministic read,
 *                                          every companion converges on the SAME unit);
 *   3. any party member's attacker       — defend the rest of the party (the healer being
 *                                          eaten while the boss reads a quest text);
 *   4. the sticky bridge                 — the TeamAuto between-kill memo, same budget, so
 *                                          companions don't flicker off a mob the instant
 *                                          the boss's GetVictim() nulls mid-chain-pull.
 *
 * Rotations are untouched: this doctrine only decides WHO to hit; healers keep healing via
 * their class AI exactly as everywhere else.
 *
 * Shared-machinery stances (the deliberate differences from TeamAuto):
 *   UseOverpullRetreat = FALSE — a companion must NEVER AttackStop and flee 30yd out of the
 *     player's fight because the pull ran deep; the HUMAN owns pull depth, and abandoning
 *     his fight mid-heal is the worst possible move. Defense-to-the-end beside a real
 *     player is correct — he can heal, peel, or run, and the C# death machinery still
 *     recovers a wipe.
 *   UseStalemateBreaker = TRUE — an unreachable-mob deadlock is a navmesh problem, not a
 *     tactics problem; the escape machinery stays.
 *   UseTapRespect = TRUE — the boss's tap is group-shared (the spine's tapperInMyGroup
 *     check passes it); a STRANGER's tapped mob is still off-limits — companions don't
 *     steal mobs from other real players.
 *
 * Self-contained: reads only the public AiBotAI surface (GetBotPlayer, FindPartyBoss,
 * IsValidAssistTarget) plus the live Group. Never moves the bot, never touches
 * m_currentTask, never emits a bridge event — the spine owns actuation and the wire
 * (the escort follow/engage hook lives in AiBotAIMain.cpp).
 *
 * Line endings: LF (C++ repo convention).
 */

#include <algorithm>
#include "AiBotDoctrine.h"
#include "SuiAutopilot.h"
#include "SuiRaidPlaybook.h"
#include "SuiRaidTelemetry.h"
#include "AiBotAIMain.h"
#include "AiBotCircuit.h" // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "Player.h"
#include "WorldSession.h"   // [MULTI-HUMAN] GetSession()->GetBot() — the real-player test in rungs 1-2
#include "Group.h"
#include "Creature.h"   // Map::GetCreature returns Creature* — need the complete type (sticky lookup)
#include "Map.h"
#include "Log.h"
#include "GridNotifiers.h"       // rung 3b mob-side scan (2026-07-08)
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

#include <memory>
#include <list>
#include <mutex>
#include <unordered_map>
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Spell.h"

namespace
{

// [CIRCUIT] Null-safe guid for probes; evaluated only inside an armed probe.
uint32 CbGuid(AiBotAI const& bot)
{
    Player* p = bot.GetBotPlayer();
    return p ? p->GetGUIDLow() : 0;
}

class AiBotDoctrinePlayerParty final : public IEngagementDoctrine
{
public:
    // ── OOC acquisition ─────────────────────────────────────────────────────────────────
    // The party focus or NOTHING. No dwell, no expiry, no starvation valve — a companion
    // with no team fight to join stands at the boss's shoulder (the escort hook follows);
    // it never seeds a fight. m_heldForTeam stays true so, should the grind dispatch ever
    // be reached with this doctrine live, a null here reads as a deliberate hold — never a
    // freeze tick, never a GRIND_BLOCKED handback.
    Unit* AcquireTarget(AiBotAI& bot) override
    {
        m_heldForTeam = true;
        return ResolvePartyFocus(bot);
    }

    // ── Pull discipline ─────────────────────────────────────────────────────────────────
    // Never veto. The only pulls this doctrine can produce are the boss's own fights
    // (rung 1/2/3 targets are already engaged with the party) — density is the human's
    // call, and holding a companion out of his 6-mob pull is exactly wrong.
    bool HoldPull(AiBotAI& bot, Unit* candidate) override
    {
        (void)bot; (void)candidate;
        return false;
    }

    // ── In-combat maintenance ───────────────────────────────────────────────────────────
    // Converge on the party focus every tick (the boss retargets → the whole escort
    // switches, the TeamAuto flap fix inherited whole). No focus → keep my own still-valid
    // victim (the mob that jumped ME — finish it). Neither → nullptr, and the spine's
    // SelectAttackTarget picks from attackers of me + my group: defense, never initiation.
    Unit* MaintainTarget(AiBotAI& bot, Unit* victim) override
    {
        if (Unit* focus = ResolvePartyFocus(bot))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: pparty, converging on party focus");
            return focus;
        }
        // held off by the kill order: not my own victim either - the rogues' white swings killed an Elite held at
        // its floor while the next Healer slept, the 4th death (2026-11-09, Majordomo attempt 61)
        if (m_heldOff)
            return nullptr;

        if (victim && bot.IsValidAssistTarget(victim))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: pparty, keeping own valid victim");
            return victim;
        }

        return nullptr;
    }

    // ── Shared-machinery stances (see the file header for the reasoning) ────────────────
    bool UseStalemateBreaker() const override { return true;  }
    bool UseOverpullRetreat()  const override { return false; }  // never flee the human's fight
    bool UseTapRespect()       const override { return true;  }  // party taps pass; strangers' don't

    bool HoldingForTeam() const override { return m_heldForTeam; }
    bool HoldsOffVictim() const override { return m_heldOff; }

    char const* Name() const override { return "PlayerParty"; }

private:
    // The party focus ladder — the mob every companion should be on right now, or nullptr
    // ("nothing to fight; follow"). Called once per behaviour tick (OOC via AcquireTarget,
    // in-combat via MaintainTarget), so the sticky budget advances once per tick, same as
    // TeamAuto.
    Unit* ResolvePartyFocus(AiBotAI& bot)
    {
        m_heldOff = false;
        Player* me = bot.GetBotPlayer();
        if (!me || !me->IsInWorld())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-doctrine: pparty, no focus, bot not in world");
            return nullptr;
        }

        // [TACTICS] A tank's first duty is the group's safety, not the anchor's target.
        if (bot.GetCombatActiveRole() == ROLE_TANK)
            if (Unit* duty = TankDuty(bot, me))
            {
                TraceFocus(me, duty, "tank duty");
                return duty;
            }
        // [TACTICS] Damage dealers kill what makes the fight worse first - and so does a tank with nothing to
        // hold: it used to fall through to "a member's attacker" and stack on another tank's Elite past the
        // kill order's floor, so the 4th add to die was an Elite and two Healers woke immune (2026-11-09,
        // Majordomo attempt 55: Boomwarrior on Shieldwall's Elite, which died second at 147 s).
        if (bot.GetCombatActiveRole() != ROLE_HEALER)
        {
            if (Unit* first = KillPriority(bot, me))
            {
                // A melee damage dealer keeps its pick 4 s: the pick flipped Healer <-> Elite every 1-2 s and the
                // rogues ran between them (2026-11-09, Majordomo attempt 55: 30-54 switches each, ~75 dps a rogue).
                if (bot.GetCombatActiveRole() == ROLE_MELEE_DPS && first->GetObjectGuid() != m_killPickGuid)
                {
                    uint32 const nowMs = WorldTimer::getMSTime();
                    Creature* prev = m_killPickGuid.IsEmpty() ? nullptr : me->GetMap()->GetCreature(m_killPickGuid);
                    // (only above half: below it the kill order's floors decide, every tick)
                    if (prev && prev->IsAlive() && prev->IsInCombat() && nowMs - m_killPickMs < 4000 && prev->GetHealthPercent() >= 50.0f &&
                        !prev->HasBreakableByDamageCrowdControlAura() && bot.IsValidAssistTarget(prev))
                        first = prev;
                    else
                    {
                        m_killPickGuid = first->GetObjectGuid();
                        m_killPickMs = nowMs;
                    }
                }
                TraceFocus(me, first, "kill order");
                return first;
            }
            if (m_meleeHeld)
            {
                ClearSticky();
                TraceFocus(me, nullptr, "held off by the kill order");
                m_heldOff = true;
                return nullptr;
            }
        }

        Player* boss = bot.FindPartyBoss();
        // [AUTOPILOT] The leader is its own anchor: rungs 3/3b defend the group around it.
        if (!boss && SuiAutopilot::IsLeader(me))
            boss = me;
        if (!boss || !boss->IsInWorld())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, boss gone, standing down");
            ClearSticky();
            return nullptr;   // human left / logged — ResolveDoctrine flips us back next tick
        }

        // [MULTI-HUMAN] (2026-07-16) Rungs 1-2 run over EVERY real player, the bot's
        // ASSIGNED human first — with two humans in the party, the OTHER human's fights
        // used to be invisible to these rungs (only rung 3/3b could catch them, and only
        // once a mob was already beating on a player), which is exactly the "bots stand
        // there for a hot minute" Deadmines report. FindEscortBoss front-loads the human
        // this bot escorts, so "attack my targets" now means YOUR human's targets first,
        // then any human's — deterministic per bot, converged per escort half.
        Player* humans[8];
        int humanCount = 0;
        {
            if (Player* pAssigned = bot.FindEscortBoss())
                humans[humanCount++] = pAssigned;   // cb:fold roster gather detail
            if (Group* pGroup = me->GetGroup())
            { // cb:fold roster gather, rung probes carry the outcome
                for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr && humanCount < 8; itr = itr->next())
                {
                    Player* pMember = itr->getSource();
                    if (!pMember || pMember == me || !pMember->IsInWorld())
                        continue;   // cb:fold roster gather scan
                    WorldSession* pSess = pMember->GetSession();
                    if (!pSess || pSess->GetBot())
                        continue;   // cb:fold roster gather scan, a bot not a human
                    if (humanCount > 0 && pMember == humans[0])
                        continue;   // cb:fold roster gather scan, assigned human already front-loaded
                    humans[humanCount++] = pMember;
                }
            }
        }

        for (int h = 0; h < humanCount; ++h)
        {
            Player* pHuman = humans[h];
            if (!pHuman->IsAlive())
                continue;   // cb:fold per-human scan, dead human

            // Rung 1: a human's own fight is authoritative — "attack my targets".
            if (Unit* pVictim = pHuman->GetVictim())
            { // cb:fold candidate gate, rung 1 probe below
                if (bot.IsValidAssistTarget(pVictim) &&
                    me->IsWithinDist(pVictim, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 1, attacking human victim");
                    m_lastAssistedVictimGuid = pVictim->GetObjectGuid();
                    m_assistStickyTicks = 0;
                    TraceFocus(me, pVictim, h == 0 ? "my human's victim" : "human victim");
                    return pVictim;
                }
            }

            // Rung 2: the human is not attacking, but something is attacking HIM —
            // "only attack not-my-target if I'm not attacking anything but being attacked".
            // GetAttackerForHelper is the same deterministic read for every companion, so
            // each escort half converges on the SAME unit.
            if (Unit* pAggro = pHuman->GetAttackerForHelper())
            { // cb:fold candidate gate, rung 2 probe below
                if (bot.IsValidAssistTarget(pAggro) &&
                    me->IsWithinDist(pAggro, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 2, defending human under attack");
                    m_lastAssistedVictimGuid = pAggro->GetObjectGuid();
                    m_assistStickyTicks = 0;
                    TraceFocus(me, pAggro, h == 0 ? "my human's attacker" : "human attacker");
                    return pAggro;
                }
            }
        }

        // Rung 3: defend the rest of the party — a mob eating the priest while the boss
        // reads quest text. First member-under-attack in group order (deterministic enough;
        // the whole escort walks the same list).
        if (Group* pGroup = me->GetGroup())
        { // cb:fold group walk, rung 3 probe below
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* pMember = itr->getSource();
                if (!pMember || pMember == me || pMember == boss || !pMember->IsAlive())
                    continue;   // cb:fold member scan gate
                if (!me->IsWithinDist(pMember, VISIBILITY_DISTANCE_NORMAL))
                    continue;   // cb:fold member scan, out of range
                if (Unit* pAggro = pMember->GetAttackerForHelper())
                { // cb:fold candidate gate, rung 3 probe below
                    if (bot.IsValidAssistTarget(pAggro) &&
                        me->IsWithinDist(pAggro, VISIBILITY_DISTANCE_NORMAL))
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 3, defending party member");
                        m_lastAssistedVictimGuid = pAggro->GetObjectGuid();
                        m_assistStickyTicks = 0;
                        TraceFocus(me, pAggro, "member attacker");
                        return pAggro;
                    }
                }
            }
        }

        // Rung 3b (2026-07-08, the standing-escort fix): the MOB-SIDE read — any hostile
        // creature near me whose victim IS a party member. Rungs 1-3 read the fight off the
        // PLAYER side (GetVictim / GetAttackerForHelper), which TeamAuto only ever proved
        // against BOT anchors; this rung reads it off the CREATURE side (c->GetVictim() ∈
        // group), which cannot lie — if a mob is beating on anyone in the party, this sees
        // it regardless of how the core books a real player's victim/attacker state. Same
        // acquisition shape as vmangos's own PartyBotAI. Nearest wins; boss-targeting mobs
        // outrank member-targeting ones so the spec's priority survives inside the rung.
        {
            std::list<Unit*> nearby;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(me, me, 40.0f);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, u_check);
            Cell::VisitAllObjects(me, searcher, 40.0f);

            Unit* bestBoss = nullptr;   float bestBossD = 99999.0f;
            Unit* bestMemb = nullptr;   float bestMembD = 99999.0f;
            Group* pGroup = me->GetGroup();
            for (Unit* u : nearby)
            {
                if (!u->IsCreature())
                    continue;   // cb:fold mob-side scan gate
                Unit* uVictim = u->GetVictim();
                if (!uVictim)
                    continue;   // cb:fold mob-side scan gate, idle mob
                // [PET-FIX] (2026-07-16) Resolve pets/minions to their owning player: a mob
                // beating on the warlock's voidwalker or the hunter's cat IS a party fight,
                // but uVictim->IsPlayer() used to exclude it — the Hogger report (mob tagged
                // on a real player's minion, escort stood watching) lands exactly here.
                Player* pVictimPlayer = uVictim->GetCharmerOrOwnerPlayerOrPlayerItself();
                if (!pVictimPlayer)
                    continue;   // cb:fold mob-side scan gate, not a player fight
                bool const onBoss = (pVictimPlayer == boss);
                bool const onMember = onBoss ||
                    (pVictimPlayer == me) ||
                    (pGroup && pGroup->IsMember(pVictimPlayer->GetObjectGuid()));
                if (!onMember)
                    continue;   // cb:fold mob-side scan gate, not our party
                if (!bot.IsValidAssistTarget(u))
                    continue;   // cb:fold mob-side scan gate, invalid target
                float const d = me->GetDistance(u);
                if (onBoss)      { if (d < bestBossD) { bestBossD = d; bestBoss = u; } }   // cb:fold nearest-pick bookkeeping
                else             { if (d < bestMembD) { bestMembD = d; bestMemb = u; } }   // cb:fold nearest-pick bookkeeping
            }
            if (Unit* pick = bestBoss ? bestBoss : bestMemb)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 3b, mob-side scan pick");
                m_lastAssistedVictimGuid = pick->GetObjectGuid();
                m_assistStickyTicks = 0;
                TraceFocus(me, pick, "mob-side scan");
                return pick;
            }
        }

        // Rung 4: the sticky bridge — the boss's GetVictim() nulls for a beat between
        // swings / kills; keep the escort on the last assisted mob through the gap, same
        // validity gate + budget as TeamAuto (a mob the boss just killed self-clears).
        if (!m_lastAssistedVictimGuid.IsEmpty() && m_assistStickyTicks < AIBOT_ASSIST_STICKY_MAX_TICKS)
        { // cb:fold sticky gate, hold probe below
            if (Creature* pSticky = me->GetMap()->GetCreature(m_lastAssistedVictimGuid))
            { // cb:fold sticky lookup, hold probe below
                if (bot.IsValidAssistTarget(pSticky) &&
                    me->IsWithinDist(pSticky, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 4, sticky bridge hold", m_assistStickyTicks);
                    ++m_assistStickyTicks;
                    return pSticky;
                }
            }
        }

        ClearSticky();

        // [DIAG] Null-focus tracer (2026-07-08, throttled ~1/10s per bot): when the escort
        // stands while the party fights, ONE of these fields names the failing gate — a live
        // bossVictim with v_valid=0 indicts IsValidHostileTarget; victim=none + attacker=none
        // while mobs visibly swing indicts the player-side reads (and rung 3b above should
        // have caught it mob-side). Cheap; remove after the shakedown.
        if (++m_diagTick >= 10)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, no focus, diag window");
            m_diagTick = 0;
            Unit* bv = boss->IsAlive() ? boss->GetVictim() : nullptr;
            Unit* ba = boss->IsAlive() ? boss->GetAttackerForHelper() : nullptr;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PARTY-DIAG] %s: no focus — boss=%s alive=%u victim=%s v_valid=%u attacker=%s a_valid=%u",
                me->GetName(), boss->GetName(), boss->IsAlive() ? 1u : 0u,
                bv ? bv->GetName() : "(none)", bv ? (bot.IsValidAssistTarget(bv) ? 1u : 0u) : 0u,
                ba ? ba->GetName() : "(none)", ba ? (bot.IsValidAssistTarget(ba) ? 1u : 0u) : 0u);
        }
        return nullptr;
    }

    // [TACTICS] Tank duty (owner 2026-09-23: "if I'm tanking a mob, another tank shouldn't
    // steal it from me"). In order:
    //   1. keep a mob I am holding (its victim is me) - a tank does not drop what it holds;
    //   2. take a loose mob: one beating on a group member who is NOT a tank (a healer, a
    //      caster), nearest first, skipping mobs another tank is already turning toward;
    //   3. nothing loose: no duty - the ordinary ladder assists the anchor's target, and the
    //      taunt rule (TrySpecTaunt) keeps this tank from ripping a mob off another tank.
    // A loose mob a tank has just gone for is its own for a few seconds: three tanks used to pick
    // the same loose elite in the same second and all switch together (2026-09-25).
    static std::mutex& TankClaimLock() { static std::mutex m; return m; }
    static std::map<ObjectGuid, std::pair<ObjectGuid, uint32>>& TankClaims()
    {
        static std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> claims;
        return claims;
    }

    // Another living tank near me that can taunt: the playbook's sheep is its to wake, not mine.
    static bool OtherTaunterNear(AiBotAI& bot, Player* me, Group* group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* p = itr->getSource();
            if (p && p != me && p->IsAlive() && p->IsInMap(me) && p->IsWithinDistInMap(me, 80.0f) &&
                bot.IsSpecAoETankHolder(p) && AiBotAI::TankCanTaunt(p))
                return true;
        }
        return false;
    }

    Unit* TankDuty(AiBotAI& bot, Player* me)
    {
        // The off-tank a boss with an ordered kill takes for the whole fight is that boss's tank: back on him after
        // every threat wipe (2026-11-09, Majordomo attempt 80: the off-tank held an Elite and died on it, while
        // Majordomo's teleport left him on the healers three times in 25 s). No claim: whoever is nearest may
        // still take him while I walk back from his port.
        if (bot.IsBossOffTank())
        {
            std::list<Unit*> wide;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck wideCheck(me, me, 100.0f);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> wideSearcher(wide, wideCheck);
            Cell::VisitAllObjects(me, wideSearcher, 100.0f);
            for (Unit* u : wide)
                if (u->IsCreature() && u->IsAlive() && u->IsInCombat() && static_cast<Creature*>(u)->IsWorldBoss() &&
                    bot.IsValidAssistTarget(u))
                    return u;
        }
        Unit* held = me->GetVictim();
        if (held && !(held->IsAlive() && held->GetVictim() == me && bot.IsValidAssistTarget(held)))
            held = nullptr;
        bool const heldElite = held && held->IsCreature() && static_cast<Creature*>(held)->IsElite();
        // A healer kind is the mages' to sheep while an elite that hits is loose: a tank holding one
        // looks for such an elite first (2026-11-06, Majordomo: the four tanks held three Flamewaker
        // Healers and the boss, the four Elites were tanked 0-6 % of the fight - all 39 dead).
        // A healer kind nobody can sheep any more is held like any elite: tanks left their woken Healers for
        // Majordomo after each threat wipe, and the loose Healers killed four casters in 13 s (2026-11-09,
        // attempt 85, after Immunity).
        bool const heldHealer = heldElite && KillRank(static_cast<Creature*>(held)) == 1 &&
            !held->IsImmuneToMechanic(MECHANIC_POLYMORPH);
        if (held && heldElite && !heldHealer)
        {
            // ...unless a boss is loose on someone who is not a tank and no other tank is going:
            // the boss outranks a trash elite (2026-09-25, Majordomo among his adds).
            // A boss with its own off-tank alive is that tank's: it is on its way back from the port.
            bool looseBoss = false;
            bool bossTankAlive = false;
            if (Group* g = me->GetGroup())
                for (GroupReference* itr = g->GetFirstMember(); itr && !bossTankAlive; itr = itr->next())
                    if (Player* p = itr->getSource())
                        if (p != me && p->IsAlive() && p->IsInMap(me))
                            if (AiBotAI const* ai = dynamic_cast<AiBotAI const*>(p->AI()))
                                bossTankAlive = ai->IsBossOffTank();
            if (!static_cast<Creature*>(held)->IsWorldBoss() && !bossTankAlive)
            {
                std::list<Unit*> around;
                MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck bossCheck(me, me, 40.0f);
                MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> bossSearcher(around, bossCheck);
                Cell::VisitAllObjects(me, bossSearcher, 40.0f);
                uint32 const nowMs = WorldTimer::getMSTime();
                for (Unit* u : around)
                {
                    if (!u->IsCreature() || !u->IsAlive() || !static_cast<Creature*>(u)->IsWorldBoss())
                        continue;
                    Unit* v = u->GetVictim();
                    Player* vp = v ? v->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                    if (!vp || bot.IsSpecAoETankHolder(vp))
                        continue;
                    std::lock_guard<std::mutex> guard(TankClaimLock());
                    auto c = TankClaims().find(u->GetObjectGuid());
                    if (c != TankClaims().end() && c->second.first != me->GetObjectGuid() && nowMs < c->second.second)
                        continue;
                    looseBoss = true;
                }
            }
            if (!looseBoss)
                return held;
        }

        Group* group = me->GetGroup();
        if (!group)
            return nullptr;
        // A tank with nothing to hold wakes the playbook's released add itself, so its first enemy is a
        // tank (2026-11-08, Majordomo: a rogue broke the released Healer's sheep, the Healer turned on the
        // melee 60 % of its life and killed two rogues before it died, 41 s after the release).
        // A tank whose elite has turned on someone who is not a tank takes it back first: a threat wipe
        // is not a free tank (2026-11-08, Majordomo: his teleport reset his threat at the moment a Healer
        // was released; his tank went to wake the Healer and Majordomo killed two priests).
        if (!held)
            if (Unit* mine = me->GetVictim())
                if (mine->IsAlive() && mine->IsCreature() && static_cast<Creature*>(mine)->IsElite() &&
                    !mine->HasBreakableByDamageCrowdControlAura() && bot.IsValidAssistTarget(mine))
                {
                    Unit* v = mine->GetVictim();
                    Player* vp = v ? v->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                    if (vp && vp != me && group->IsMember(vp->GetObjectGuid()) && !bot.IsSpecAoETankHolder(vp))
                    {
                        std::lock_guard<std::mutex> guard(TankClaimLock());
                        TankClaims()[mine->GetObjectGuid()] = std::make_pair(me->GetObjectGuid(), WorldTimer::getMSTime() + 4000);
                        return mine;
                    }
                }
        // ...if it can taunt the woken add back, or no tank that can is left (2026-11-08, Majordomo: the
        // paladin tank woke Healers that stayed on a tank 2-17 % of their lives).
        // (an off-tank never wakes one: it is there for a tank that is missing, not as a free one)
        if (!held && !bot.IsOffTankOnDuty() && (AiBotAI::TankCanTaunt(me) || !OtherTaunterNear(bot, me, group)))
            if (Unit* release = AiBotAI::PlaybookReleaseTarget(me))
                if (release->IsAlive() && release->HasBreakableByDamageCrowdControlAura())
                {
                    std::lock_guard<std::mutex> guard(TankClaimLock());
                    uint32 const nowMs = WorldTimer::getMSTime();
                    auto c = TankClaims().find(release->GetObjectGuid());
                    Player* other = c != TankClaims().end() && c->second.first != me->GetObjectGuid() &&
                        nowMs < c->second.second ? me->GetMap()->GetPlayer(c->second.first) : nullptr;
                    if (!other || !other->IsAlive())
                    {
                        TankClaims()[release->GetObjectGuid()] = std::make_pair(me->GetObjectGuid(), nowMs + 4000);
                        return release;
                    }
                }
        std::list<Unit*> nearby;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 60.0f);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
        Cell::VisitAllObjects(me, searcher, 60.0f);

        Unit* best = nullptr;
        float bestDist = 1e9f;
        for (Unit* mob : nearby)
        {
            if (!mob->IsCreature() || !mob->IsAlive())
                continue;
            Unit* victim = mob->GetVictim();
            Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (!victimPlayer || victimPlayer == me || !group->IsMember(victimPlayer->GetObjectGuid()))
                continue;
            if (victim == victimPlayer && bot.IsSpecAoETankHolder(victimPlayer))
            {
                // On a tank already: not loose - unless that tank is taking several at once
                // and this one is not its own target (2026-09-24: two Molten Giants on one
                // tank killed it in under a second while the spare tanks stood by).
                uint32 onHim = 0;
                for (Unit* a : victimPlayer->GetAttackers())
                    if (a && a->IsCreature() && a->GetVictim() == victimPlayer)
                        ++onHim;
                if (onHim < 2 || victimPlayer->GetVictim() == mob)
                    continue;
            }
            if (!bot.IsValidAssistTarget(mob) || mob->HasBreakableByDamageCrowdControlAura())
                continue;
            bool claimed = false;
            // ...nor one a mage of ours is sheeping right now: its sheep had run out, a mage sheeped it again
            // and the free tank's Seal of Righteousness broke the new one at once (2026-11-08, Majordomo
            // attempt 46 - an unplanned Healer awake, an Elite died out of order).
            for (GroupReference* itr = group->GetFirstMember(); itr && !claimed; itr = itr->next())
                if (Player* other = itr->getSource())
                    if (Spell* s = other->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                        if (s->m_spellInfo && s->m_targets.getUnitTarget() == mob &&
                            (s->m_spellInfo->Mechanic == MECHANIC_POLYMORPH || s->m_spellInfo->EffectMechanic[0] == MECHANIC_POLYMORPH))
                            claimed = true;
            for (GroupReference* itr = group->GetFirstMember(); itr && !claimed; itr = itr->next())
            {
                Player* other = itr->getSource();
                // Claimed means held: a tank hitting a mob that is beating on a warlock has not
                // got it (2026-09-25: a Flameguard killed three warlocks while one tank swung at it
                // and the other tanks counted it as taken).
                if (other && other != me && other->IsAlive() && other->GetVictim() == mob &&
                    mob->GetVictim() == other && bot.IsSpecAoETankHolder(other))
                    claimed = true;
            }
            if (claimed)
                continue;
            {
                std::lock_guard<std::mutex> guard(TankClaimLock());
                auto c = TankClaims().find(mob->GetObjectGuid());
                if (c != TankClaims().end() && c->second.first != me->GetObjectGuid() &&
                    WorldTimer::getMSTime() < c->second.second)
                    if (Player* other = me->GetMap()->GetPlayer(c->second.first))
                        if (other->IsAlive())
                            continue;   // another tank is on its way to it
            }
            // A boss loose on the raid first, then elites (an elite loose on the raid kills; an imp
            // does not), then nearest (2026-09-25: Majordomo walked through the healers while all
            // four tanks held his adds).
            bool const elite = static_cast<Creature*>(mob)->IsElite();
            bool const boss = static_cast<Creature*>(mob)->IsWorldBoss();
            bool const healerKind = elite && !boss && KillRank(static_cast<Creature*>(mob)) == 1;
            float const d = me->GetDistance(mob) - (elite ? (healerKind ? 500.0f : 1000.0f) : 0.0f) - (boss ? 2000.0f : 0.0f);
            if (d < bestDist)
            {
                bestDist = d;
                best = mob;
            }
        }
        if (held && !(best && best->IsCreature() && static_cast<Creature*>(best)->IsElite()))
            return held;   // keep what I hold unless a loose elite needs me
        // A tank with no taunt and nothing loose to take goes for the boss a taunting tank holds: the boss's
        // threat is wiped every 20-30 s (Majordomo's teleport) and then turns to the one beside it, and the
        // taunting tank is free for the next sheep (2026-11-08, Majordomo: the paladin tank woke Healers he
        // could not keep, and the release waited on a free tank while he stood with nothing).
        if (!held && !best && !AiBotAI::TankCanTaunt(me))
            for (Unit* mob : nearby)
            {
                if (!mob->IsCreature() || !mob->IsAlive() || !mob->IsInCombat() || !static_cast<Creature*>(mob)->IsWorldBoss() ||
                    mob->HasBreakableByDamageCrowdControlAura() || !bot.IsValidAssistTarget(mob) || !me->IsWithinDistInMap(mob, 40.0f))
                    continue;
                Unit* v = mob->GetVictim();
                Player* vp = v ? v->ToPlayer() : nullptr;
                if (!vp || vp == me || !group->IsMember(vp->GetObjectGuid()) || !bot.IsSpecAoETankHolder(vp) ||
                    !AiBotAI::TankCanTaunt(vp))
                    continue;
                std::lock_guard<std::mutex> guard(TankClaimLock());
                uint32 const nowMs = WorldTimer::getMSTime();
                auto c = TankClaims().find(mob->GetObjectGuid());
                if (c == TankClaims().end() || c->second.first != me->GetObjectGuid() || nowMs >= c->second.second)
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-TANK] %s: no taunt, nothing loose - takes %s off %s",
                        me->GetName(), mob->GetName(), vp->GetName());
                TankClaims()[mob->GetObjectGuid()] = std::make_pair(me->GetObjectGuid(), nowMs + 4000);
                return mob;
            }
        if (heldHealer && best && KillRank(static_cast<Creature*>(best)) == 1 && !static_cast<Creature*>(best)->IsWorldBoss())
            return held;   // one healer kind for another: no swap
        if (best)
        {
            std::lock_guard<std::mutex> guard(TankClaimLock());
            TankClaims()[best->GetObjectGuid()] = std::make_pair(me->GetObjectGuid(), WorldTimer::getMSTime() + 4000);
        }
        return best;
    }

    // [TACTICS] Raiding kill order, derived per creature entry from its own spell list:
    // 0 summoner (it adds to the fight), 1 healer (it undoes the damage), 2 caster (its
    // damage ignores the tank), 3 everything else. Cached; a script-driven creature whose
    // spells live only in its C++ AI ranks 3 and is simply assisted as before.
    static uint8 KillRank(Creature const* c)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, uint8> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto found = cache.find(c->GetEntry());
        if (found != cache.end())
            return found->second;
        uint8 rank = 3;
        CreatureInfo const* info = c->GetCreatureInfo();
        std::vector<uint32> spells;
        if (info)
        {
            if (info->spell_list_id)
                if (CreatureSpellsList const* list = sObjectMgr.GetCreatureSpellsList(info->spell_list_id))
                    for (CreatureSpellsEntry const& e : *list)
                        spells.push_back(e.spellId);
            for (uint32 id : info->spells)
                if (id)
                    spells.push_back(id);
            if (info->unit_class == CLASS_MAGE)
                rank = 2;
        }
        // An aura that fires a spell every few seconds (a Firewalker's Fire Blossom) is that spell:
        // follow the trigger so the one throwing bolts at the casters counts as a caster.
        for (size_t k = 0; k < spells.size() && k < 64; ++k)
            if (SpellEntry const* spell = sSpellMgr.GetSpellEntry(spells[k]))
                for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                    if (spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_TRIGGER_SPELL && spell->EffectTriggerSpell[i])
                        spells.push_back(spell->EffectTriggerSpell[i]);
        for (uint32 id : spells)
        {
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(id);
            if (!spell)
                continue;
            for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                uint32 const effect = spell->Effect[i];
                if (effect == SPELL_EFFECT_SUMMON || effect == SPELL_EFFECT_SUMMON_WILD ||
                    effect == SPELL_EFFECT_SUMMON_GUARDIAN)
                    rank = 0;
                else if ((effect == SPELL_EFFECT_HEAL || effect == SPELL_EFFECT_HEAL_MAX_HEALTH) &&
                    rank > 1)
                    rank = 1;
                else if (effect == SPELL_EFFECT_SCHOOL_DAMAGE && rank > 2 &&
                    spell->rangeIndex > 1)
                    rank = 2;
            }
        }
        cache.emplace(c->GetEntry(), rank);
        return rank;
    }

    Unit* KillPriority(AiBotAI& bot, Player* me)
    {
        m_meleeHeld = false;
        Group* group = me->GetGroup();
        if (!group)
            return nullptr;
        std::list<Unit*> nearby;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, 45.0f);   // ranged stand 35-40 yd out
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
        Cell::VisitAllObjects(me, searcher, 45.0f);

        Unit* anchorVictim = nullptr;
        if (Player* anchor = bot.FindEscortBoss())
            anchorVictim = anchor->GetVictim();
        for (Unit* mob : nearby)
            AiBotAI::ObserveMultiplier(mob);
        // (ranks: 0 a multiplier, 1 a peeled small add, then 2 + doubled kill rank: awake 2r; held by
        // crowd control 9, after every awake one: a healer kind broken out of its sheep is loose on
        // the casters - 2026-11-06, Majordomo: the kill order took sheeped Flamewaker Healers first
        // while four Elites ran untanked, and their Shadow Shocks did 101k; all 39 dead in 60 s)
        // A boss in the fight whose playbook lists its adds' kill order decides it for those adds
        // (DATA: Majordomo's Healers are sheeped while his Elites die - the Healers turn immune to
        // Polymorph once half the adds are down).
        // (the boss is looked for 100 yd out: a rogue on an Elite tanked 50 yd from Majordomo saw no boss in its
        // 45 yd, so no playbook rule held it and its white swings killed Elites under the floor - 2026-11-09,
        // attempts 62-63)
        uint32 orderBoss = 0;
        {
            std::list<Unit*> wide;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck wideCheck(me, me, 100.0f);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> wideSearcher(wide, wideCheck);
            Cell::VisitAllObjects(me, wideSearcher, 100.0f);
            for (Unit* mob : wide)
                if (mob->IsCreature() && mob->IsAlive() && mob->IsInCombat() && static_cast<Creature*>(mob)->IsWorldBoss())
                {
                    orderBoss = mob->GetEntry();
                    break;
                }
        }
        bool noFreeTank = false;
        Unit* const release = orderBoss ? AiBotAI::PlaybookReleaseTarget(me, &noFreeTank) : nullptr;
        // the released one counts wherever it was sheeped (2026-11-07, Majordomo: sheeped 50 yd out, the
        // next Healer was attacked 32 s after the last one died, while an Elite took the damage)
        if (release && std::find(nearby.begin(), nearby.end(), release) == nearby.end())
            nearby.push_back(release);
        // ...and so does one woken on purpose, wherever its tank holds it: past my 45 yd it was out of my
        // list and the next kind in the order took the damage (2026-11-08, Majordomo: the second Healer
        // was tanked 25-30 yd from the stack, ten bots switched to an Elite by "kill order", and that
        // Elite's death was the 4th - two Healers woke immune).
        if (orderBoss)
        {
            std::list<Unit*> far;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck farCheck(me, me, 90.0f);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> farSearcher(far, farCheck);
            Cell::VisitAllObjects(me, farSearcher, 90.0f);
            for (Unit* u : far)
                if (u->IsAlive() && (u->IsInCombat() || u->HasBreakableByDamageCrowdControlAura()) && AiBotAI::WasPlaybookReleased(u) &&
                    std::find(nearby.begin(), nearby.end(), u) == nearby.end())
                    nearby.push_back(u);
        }
        auto rankOf = [orderBoss, release, noFreeTank, &bot](Unit* mob)
        {
            if (AiBotAI::IsKnownMultiplier(mob->GetEntry()))
                return uint8(0);
            if (mob->HasBreakableByDamageCrowdControlAura() && mob != release)
                return uint8(9);
            // An add the raid could sheep, awake while no tank is free and not woken on purpose, is sheeped
            // again rather than killed - a tank on it is one the next release needs (2026-11-08, Majordomo,
            // Healers first: the ranged opened on the Healers, an Auto Shot in flight broke a fresh sheep
            // 2 s in, and the loose Healer killed two melee untanked).
            // (...unless a tank holds it: then it is as good as released, and no other one is released while it is
            // awake - the raid killed Elites beside a tanked Healer instead, 2026-11-09, Majordomo attempt 74)
            Player* holder = mob->GetVictim() ? mob->GetVictim()->ToPlayer() : nullptr;
            if (orderBoss && noFreeTank && mob != release && mob->IsCreature() &&
                !static_cast<Creature*>(mob)->IsWorldBoss() && !mob->IsImmuneToMechanic(MECHANIC_POLYMORPH) &&
                !AiBotAI::WasPlaybookReleased(mob) && !(holder && bot.IsSpecAoETankHolder(holder)))
                return uint8(9);
            if (orderBoss)
            {
                int const k = SuiRaidPlaybook::KillOrder(orderBoss, mob->GetEntry());
                if (k >= 0)
                    return uint8(2 + std::min(k, 3) * 2);
                // the boss that orders its adds comes after all of them (2026-11-08, Majordomo: his own spells
                // rank him 2, the Elites came 4th in the order once the Healers slept, and 70 s of the raid's
                // damage went into a boss that cannot die - no Elite fell)
                if (mob->GetEntry() == orderBoss)
                    return uint8(10);
            }
            return uint8(2 + KillRank(static_cast<Creature*>(mob)) * 2);
        };
        uint8 anchorRank = 9;
        if (anchorVictim && anchorVictim->IsCreature())
            anchorRank = rankOf(anchorVictim);
        // A kind that heals itself whole is below everything else (the anchor's own target too).
        for (Unit* mob : nearby)
            AiBotAI::ObserveRebound(mob);
        if (anchorVictim && AiBotAI::IsKnownRebounder(anchorVictim->GetEntry()))
            anchorRank = 10;
        // A boss whose playbook orders its adds: that order decides, not the leader's target - a tank
        // leader swaps between the boss and loose adds (2026-11-06, Majordomo: 103 focus changes to
        // "my human's victim" in 96 s, the damage spread over four Elites and none died).
        if (orderBoss)
            for (Unit* mob : nearby)
                if (mob->IsCreature() && mob->IsAlive() && SuiRaidPlaybook::KillOrder(orderBoss, mob->GetEntry()) >= 0)
                {
                    anchorRank = 99;
                    break;
                }

        // A pack whose fallen get back up (one lies "dead" while its kin fight on - Core Hounds
        // rise again unless the whole pack is down) dies together: damage goes to the healthiest
        // standing member of that kind, so they all reach the floor at once.
        // Learned once seen, so the next pack of that kind is balanced from the first hit.
        static std::set<uint32> knownRisers;
        static std::mutex risersLock;
        std::set<uint32> risers;
        {
            std::lock_guard<std::mutex> guard(risersLock);
            for (Unit* mob : nearby)
                if (mob->IsCreature() && mob->IsAlive() && mob->IsInCombat() &&
                    (mob->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD) || mob->GetStandState() == UNIT_STAND_STATE_DEAD))
                {
                    knownRisers.insert(mob->GetEntry());
                    AiBotAI::NoteRiser(mob->GetEntry());
                    static std::map<ObjectGuid, uint32> logged;
                    uint32 const nowMs = WorldTimer::getMSTime();
                    if (nowMs - logged[mob->GetObjectGuid()] > 12000)
                    {
                        logged[mob->GetObjectGuid()] = nowMs;
                        std::string kin;
                        for (Unit* k : nearby)
                            if (k != mob && k->IsCreature() && k->IsAlive() && k->GetEntry() == mob->GetEntry() && k->IsInCombat())
                                kin += std::to_string(int(k->GetHealthPercent())) + (k->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD) ? "(down) " : " ");
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-RISER] %s (guid %u) is down; its kin at %s",
                            mob->GetName(), mob->GetGUIDLow(), kin.c_str());
                    }
                }
            for (Unit* mob : nearby)
                if (mob->IsCreature() && mob->IsInCombat() &&
                    (knownRisers.count(mob->GetEntry()) || AiBotAI::IsKnownRiser(mob->GetEntry())))
                    risers.insert(mob->GetEntry());
        }
        if (!risers.empty())
        {
            // Two phases: every standing one down evenly to a floor, then one burst, inside the time
            // the fallen lie on the floor.
            std::vector<Unit*> standing;
            // The pack's revival looks 100 yd around each fallen one, so the balance does too: a
            // kin tanked further out than my own scan still has to come down with the rest.
            std::list<Unit*> riserPool(nearby.begin(), nearby.end());
            {
                std::set<ObjectGuid> seen;
                for (Unit* mob : nearby)
                    seen.insert(mob->GetObjectGuid());
                for (uint32 entry : risers)
                {
                    std::list<Creature*> kin;
                    me->GetCreatureListWithEntryInGrid(kin, entry, 100.0f);
                    for (Creature* k : kin)
                        if (seen.insert(k->GetObjectGuid()).second)
                            riserPool.push_back(k);
                }
            }
            for (Unit* mob : riserPool)
            {
                if (!mob->IsCreature() || !mob->IsAlive() || !mob->IsInCombat() || !risers.count(mob->GetEntry()) ||
                    !bot.IsValidAssistTarget(mob))
                    continue;
                standing.push_back(mob);
            }
            if (!standing.empty())
            {
                // Down to a floor first, then one burst inside the pack's window: a fallen one gets back up
                // at full once ten seconds pass with any kin in the fight still above 1 health
                // (mob_core_hound). Nothing hits one at the floor or lying down until all are there.
                // 2026-11-03: the old 8 % line with "help the healthiest" floored them one at a time - a
                // five-hound pack (236k health) took 261 s and 1.6M damage.
                float const floorPct = 18.0f;
                std::vector<Unit*> up, low;
                for (Unit* s : standing)
                {
                    if (s->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD) || s->GetStandState() == UNIT_STAND_STATE_DEAD)
                        continue;   // lying down: damage on it is lost
                    (s->GetHealthPercent() > floorPct ? up : low).push_back(s);
                }
                auto byGuid = [](Unit* x, Unit* y) { return x->GetGUIDLow() < y->GetGUIDLow(); };
                std::sort(up.begin(), up.end(), byGuid);
                std::sort(low.begin(), low.end(), byGuid);
                if (up.empty())
                    return low.empty() ? nullptr : low[me->GetGUIDLow() % low.size()];   // the burst: each its share
                Unit* top = *std::max_element(up.begin(), up.end(), [](Unit* x, Unit* y)
                    { return x->GetHealthPercent() < y->GetHealthPercent(); });
                // Each bot keeps its own share above the floor and only helps the healthiest when its own
                // has fallen well behind (2026-09-25: "the healthiest" alone had every damage dealer
                // switching every second).
                Unit* current = me->GetVictim();
                if (current && std::find(up.begin(), up.end(), current) != up.end() &&
                    current->GetHealthPercent() + 20.0f >= top->GetHealthPercent())
                    return current;
                Unit* share = up[me->GetGUIDLow() % up.size()];
                if (share->GetHealthPercent() + 20.0f >= top->GetHealthPercent())
                    return share;
                return top;
            }
        }

        Unit* best = nullptr;
        uint8 bestRank = anchorRank;
        float bestHealth = 101.0f;
        for (Unit* mob : nearby)
        {
            if (!mob->IsCreature() || !mob->IsAlive() || !mob->IsInCombat())
                continue;
            Unit* victim = mob->GetVictim();
            Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            // (a sheep has no victim: the playbook's released one is a candidate all the same - 2026-11-07,
            // Majordomo: the next Healer was never attacked and woke only when its sheep ran out, 21 s later)
            if ((!victimPlayer || !group->IsMember(victimPlayer->GetObjectGuid())) && mob != release)
                continue;
            // (the released sheep is the tank's to wake: its first enemy should be the one that holds it)
            // (12 s, not 6: a tank 24 yd off took 8 s to reach it and a warlock's Immolate woke it first -
            // 2026-11-08, Majordomo; while it sleeps nothing is lost by waiting)
            if (mob == release && mob->HasBreakableByDamageCrowdControlAura() && AiBotAI::PlaybookReleaseAgeMs(mob) < 12000)
                continue;
            // ...and once awake, until a tank holds it (15 s at most): the damage dealers' threat takes a fresh
            // Healer off a tank that woke it from range (2026-11-08, Majordomo: a paladin's Judgement woke it from
            // 10 yd and it spent 98 % of its life off the tanks).
            if (!mob->HasBreakableByDamageCrowdControlAura() && AiBotAI::WasPlaybookReleased(mob) &&
                AiBotAI::PlaybookReleaseAgeMs(mob) < 15000)
            {
                Unit* v = mob->GetVictim();
                Player* vp = v ? v->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                if (!vp || !bot.IsSpecAoETankHolder(vp))
                    continue;
            }
            // A melee damage dealer leaves an ordered add whose own burst reaches 18 yd or more to the ranged - it
            // cannot be hit from outside that circle - while a tanked add of a later kind is above half: those it
            // takes down to half for the endgame (2026-11-08, Majordomo attempt 44: all seven rogues died to the
            // woken Healers' 20 yd Shadow Shock, ~1k a hit, while the Elites stood tanked at full health). Below
            // half it leaves the later kind alone while an earlier one lives, unless the plan needs one dead to
            // free a tank (attempt 45: melee on the Elites alone killed three of them before the Healers - the
            // fourth death woke two Healers immune).
            if (orderBoss && bot.GetCombatActiveRole() == ROLE_MELEE_DPS && mob->IsCreature() &&
                !static_cast<Creature*>(mob)->IsWorldBoss())
            {
                int const mk = SuiRaidPlaybook::KillOrder(orderBoss, mob->GetEntry());
                if (mk >= 0 && SuiRaidTelemetry::EntryPulseRadius(mob->GetEntry()) >= 18.0f)
                {
                    bool elseToHit = false;
                    for (Unit* u : nearby)
                        if (u != mob && u->IsAlive() && u->IsInCombat() && u->GetHealthPercent() > 50.0f &&
                            !u->HasBreakableByDamageCrowdControlAura() && SuiRaidPlaybook::KillOrder(orderBoss, u->GetEntry()) > mk &&
                            SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()) < 18.0f)
                            if (Player* up = u->GetVictim() ? u->GetVictim()->ToPlayer() : nullptr)
                                if (bot.IsSpecAoETankHolder(up))
                                    elseToHit = true;
                    if (elseToHit)
                    {
                        m_meleeHeld = true;
                        continue;
                    }
                }
                else if (mk > 0 && mob->GetHealthPercent() < 50.0f)
                {
                    // (all tanks busy only asks for a death while no earlier kind is awake: with a released
                    // Healer up every tank is busy too - attempt 48, the second Elite died before the first Healer)
                    bool earlierAlive = false, earlierAwake = false;
                    for (Unit* u : nearby)
                        if (u != mob && u->IsAlive() && (u->IsInCombat() || u->HasBreakableByDamageCrowdControlAura()))
                        {
                            int const uk = SuiRaidPlaybook::KillOrder(orderBoss, u->GetEntry());
                            if (uk >= 0 && uk < mk)
                            {
                                earlierAlive = true;
                                if (!u->HasBreakableByDamageCrowdControlAura())
                                    earlierAwake = true;
                            }
                        }
                    if (earlierAlive && (!noFreeTank || earlierAwake))
                    {
                        m_meleeHeld = true;
                        continue;
                    }
                }
            }
            // (a later kind in the order is not finished while an earlier one is woken on purpose: the
            // order of the deaths is the plan - 2026-11-08, Majordomo: the second Elite, low from splash, died
            // while the second Healer was being killed, so the fourth death woke two immune Healers, not one)
            // (45 %, not 30: ten casters' bolts already in flight carried the second Elite from 30 % to dead while
            // the next Healer slept out its 12 s for its tank - 2026-11-09, Majordomo attempt 60)
            if (orderBoss && mob->GetHealthPercent() < 45.0f && SuiRaidPlaybook::KillOrder(orderBoss, mob->GetEntry()) > 0)
            {
                int const mk = SuiRaidPlaybook::KillOrder(orderBoss, mob->GetEntry());
                // (released ones only: counting a Healer loose from the pull held the first Elite at the floor until
                // 108 s, and its death is what frees a tank for the first release - 2026-11-09, attempt 68)
                // (a sheep counts though it may not be "in combat": the released Healer slept out its 12 s, the floor
                // never saw it and the rogues' white swings killed an Elite 3rd - attempt 73; same in the melee hold)
                bool earlierUp = false;
                for (Unit* u : nearby)
                    if (u != mob && u->IsAlive() && (u->IsInCombat() || u->HasBreakableByDamageCrowdControlAura()) && (u == release || AiBotAI::WasPlaybookReleased(u)))
                    {
                        int const uk = SuiRaidPlaybook::KillOrder(orderBoss, u->GetEntry());
                        if (uk >= 0 && uk < mk)
                            earlierUp = true;
                    }
                // ...and held off it, not just passed over: with nothing else chosen the anchor rungs ("a member's
                // attacker") picked it straight back up (2026-11-09, Majordomo attempt 62: rogues' white swings
                // killed two Elites under the floor)
                if (earlierUp)
                {
                    m_meleeHeld = true;
                    continue;
                }
            }
            // (the validity test refuses anything under crowd control - the released sheep is woken on purpose)
            if ((mob != release && !bot.IsValidAssistTarget(mob)) || (mob == release && !mob->IsAlive()) ||
                (AiBotAI::IsKnownRebounder(mob->GetEntry()) && !AiBotAI::IsKnownMultiplier(mob->GetEntry())))
                continue;
            // an awake one of a rank before a sheeped one of the same rank; a sheep before a worse rank
            uint8 rank = rankOf(mob);
            // Peel: a small add chewing on a healer or a caster dies before anything else
            // (2026-09-25: Onyxia's whelps killed 18 while the damage stayed on her).
            // Ranged damage dealers peel only what chews on themselves: a stream of small adds
            // otherwise keeps every caster off the boss for good (2026-09-25: Onyxia's second
            // phase took six minutes, her whelps drawing all the ranged damage the whole time).
            if (!static_cast<Creature*>(mob)->IsElite() && victimPlayer &&
                (bot.GetCombatActiveRole() != ROLE_RANGE_DPS || victimPlayer == me))
                if (AiBotAI* vai = dynamic_cast<AiBotAI*>(victimPlayer->AI()))
                    if (vai->GetCombatActiveRole() != ROLE_TANK && rank > 1)
                        rank = 1;
            // Multipliers: everyone on the same one - the oldest (lowest guid, nearest its split)
            // unless another is already half down.
            if (rank == 0 && best && bestRank == 0)
            {
                bool const mobLow = mob->GetHealthPercent() < 50.0f, bestLow = bestHealth < 50.0f;
                if (mobLow != bestLow ? !mobLow : mob->GetGUIDLow() > best->GetGUIDLow())
                    continue;
            }
            // Health compared in tenths: four of a kind near full that heal and regenerate flip the
            // lowest every tick, and the whole raid switched with it (2026-11-07, Majordomo: thirty
            // "converge Flamewaker Healer -> Flamewaker Healer" in 30 s, no Healer below 91 % in 100 s).
            else if (rank > bestRank || (rank == bestRank && (best == nullptr ? (rank > anchorRank || (rank == anchorRank && rank != 0)) :
                int(mob->GetHealthPercent() / 10.0f) > int(bestHealth / 10.0f) ||
                (int(mob->GetHealthPercent() / 10.0f) == int(bestHealth / 10.0f) && mob->GetGUIDLow() > best->GetGUIDLow()))))
                continue;
            best = mob;
            bestRank = rank;
            bestHealth = mob->GetHealthPercent();
        }
        // A melee held off everything in its 45 yd goes to a tanked add above half further out (the tanks
        // stand up to 35 yd from the healers), never back to the one it was held off (2026-11-08, Majordomo
        // attempt 50: with nothing chosen the sticky rung kept seven rogues on an Elite under half, and it
        // died second, while the first Healer was being killed).
        if (!best && m_meleeHeld && orderBoss)
        {
            std::list<Unit*> far;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck farCheck(me, me, 90.0f);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> farSearcher(far, farCheck);
            Cell::VisitAllObjects(me, farSearcher, 90.0f);
            float bestDist = 1e9f;
            for (Unit* u : far)
            {
                if (!u->IsCreature() || !u->IsAlive() || !u->IsInCombat() || static_cast<Creature*>(u)->IsWorldBoss() ||
                    u->HasBreakableByDamageCrowdControlAura() || u->GetHealthPercent() <= 50.0f ||
                    SuiRaidPlaybook::KillOrder(orderBoss, u->GetEntry()) <= 0 ||
                    SuiRaidTelemetry::EntryPulseRadius(u->GetEntry()) >= 18.0f || !bot.IsValidAssistTarget(u))
                    continue;
                Player* up = u->GetVictim() ? u->GetVictim()->ToPlayer() : nullptr;
                if (!up || !bot.IsSpecAoETankHolder(up))
                    continue;
                float const d = me->GetDistance(u);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = u;
                }
            }
        }
        // [AIBOT-FLOOR] probe: a later kind under the floor picked anyway (2026-11-09, Majordomo attempt 77: seven
        // melee kept one Elite from 45 % to dead 54 s after a Healer was released - no "held off" ever logged)
        if (best && orderBoss && best->GetHealthPercent() < 45.0f && SuiRaidPlaybook::KillOrder(orderBoss, best->GetEntry()) > 0)
        {
            uint32 const nowMs = WorldTimer::getMSTime();
            if (nowMs - m_floorLogMs > 5000)
            {
                m_floorLogMs = nowMs;
                std::string seen;
                for (Unit* u : nearby)
                    if (u != best && u->IsAlive() && SuiRaidPlaybook::KillOrder(orderBoss, u->GetEntry()) >= 0)
                    {
                        char buf[96];
                        snprintf(buf, sizeof(buf), " %u:%s%.0f%%%s%s%s", u->GetGUIDLow(), u->GetEntry() == best->GetEntry() ? "same " : "",
                            u->GetHealthPercent(), u->IsInCombat() ? " comb" : "", u->HasBreakableByDamageCrowdControlAura() ? " cc" : "",
                            u == release ? " REL" : AiBotAI::WasPlaybookReleased(u) ? " rel" : "");
                        seen += buf;
                    }
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-FLOOR] %s (role %u) picks %s (guid %u) at %.0f%% - ordered in reach:%s",
                    me->GetName(), uint32(bot.GetCombatActiveRole()), best->GetName(), best->GetGUIDLow(), best->GetHealthPercent(),
                    seen.empty() ? " none" : seen.c_str());
            }
        }
        return best;
    }

    // Log only when the resolved focus CHANGES (never per-tick) — the TeamAuto tracer pattern.
    void TraceFocus(Player* me, Unit* focus, char const* how)
    {
        ObjectGuid const g = focus ? focus->GetObjectGuid() : ObjectGuid();
        if (g == m_lastFocusGuid)
            return;   // cb:fold log throttle, focus unchanged
        m_lastFocusGuid = g;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PARTY] %s: focus -> %s (%s)",
            me->GetName(), focus ? focus->GetName() : "(none)", how);
    }

    void ClearSticky()
    {
        m_lastAssistedVictimGuid.Clear();
        m_assistStickyTicks = 0;
    }

    // Instance-owned transient state — a doctrine swap (human leaves) destroys it with the
    // instance, no reset checklist needed (the same "swap == reset" the split is built on).
    ObjectGuid m_lastAssistedVictimGuid;
    ObjectGuid m_lastFocusGuid;           // focus-change tracer memo (log on change only)
    uint8      m_assistStickyTicks = 0;
    uint8      m_diagTick          = 0;   // null-focus DIAG throttle (~1 line / 10 ticks)
    bool       m_heldForTeam       = true;
    bool       m_meleeHeld         = false;   // KillPriority held this melee off an ordered add this tick
    bool       m_heldOff           = false;   // this tick's focus is null because the kill order holds me off
    uint32     m_floorLogMs        = 0;       // [AIBOT-FLOOR] probe throttle
    ObjectGuid m_killPickGuid;                // a melee damage dealer's kill-order pick and when it was made
    uint32     m_killPickMs        = 0;
};

} // anonymous namespace

// Factory hook consumed by MakeDoctrine() in AiBotDoctrine.cpp.
std::unique_ptr<IEngagementDoctrine> MakePlayerPartyDoctrine()
{
    return std::unique_ptr<IEngagementDoctrine>(new AiBotDoctrinePlayerParty());
}