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

    char const* Name() const override { return "PlayerParty"; }

private:
    // The party focus ladder — the mob every companion should be on right now, or nullptr
    // ("nothing to fight; follow"). Called once per behaviour tick (OOC via AcquireTarget,
    // in-combat via MaintainTarget), so the sticky budget advances once per tick, same as
    // TeamAuto.
    Unit* ResolvePartyFocus(AiBotAI& bot)
    {
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
        // [TACTICS] Damage dealers kill what makes the fight worse first.
        if (bot.GetCombatActiveRole() != ROLE_TANK && bot.GetCombatActiveRole() != ROLE_HEALER)
            if (Unit* first = KillPriority(bot, me))
            {
                TraceFocus(me, first, "kill order");
                return first;
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

    Unit* TankDuty(AiBotAI& bot, Player* me)
    {
        Unit* held = me->GetVictim();
        if (held && !(held->IsAlive() && held->GetVictim() == me && bot.IsValidAssistTarget(held)))
            held = nullptr;
        bool const heldElite = held && held->IsCreature() && static_cast<Creature*>(held)->IsElite();
        if (held && heldElite)
        {
            // ...unless a boss is loose on someone who is not a tank and no other tank is going:
            // the boss outranks a trash elite (2026-09-25, Majordomo among his adds).
            bool looseBoss = false;
            if (!static_cast<Creature*>(held)->IsWorldBoss())
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
            if (!bot.IsValidAssistTarget(mob))
                continue;
            bool claimed = false;
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
            float const d = me->GetDistance(mob) - (elite ? 1000.0f : 0.0f) - (boss ? 2000.0f : 0.0f);
            if (d < bestDist)
            {
                bestDist = d;
                best = mob;
            }
        }
        if (held && !(best && best->IsCreature() && static_cast<Creature*>(best)->IsElite()))
            return held;   // keep what I hold unless a loose elite needs me
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
        uint8 anchorRank = 7;   // (ranks are doubled: awake 2r, sheeped 2r+1)
        if (anchorVictim && anchorVictim->IsCreature())
            anchorRank = uint8(KillRank(static_cast<Creature*>(anchorVictim)) * 2 +
                (anchorVictim->HasBreakableByDamageCrowdControlAura() ? 1 : 0));
        // A kind that heals itself whole is below everything else (the anchor's own target too).
        for (Unit* mob : nearby)
            AiBotAI::ObserveRebound(mob);
        if (anchorVictim && AiBotAI::IsKnownRebounder(anchorVictim->GetEntry()))
            anchorRank = 8;

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
            // Two phases: bring every standing one down evenly to a quarter (the healthiest
            // first, keeping the current target unless it is 10 points ahead), then finish them
            // lowest first in one burst, inside the time the fallen lie on the floor.
            Unit* healthiest = nullptr;
            Unit* lowest = nullptr;
            bool allLow = true;
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
                float const hp = mob->GetHealthPercent();
                if (hp > 8.0f)
                    allLow = false;
                if (!healthiest || hp > healthiest->GetHealthPercent())
                    healthiest = mob;
                if (!lowest || hp < lowest->GetHealthPercent())
                    lowest = mob;
            }
            if (healthiest)
            {
                if (allLow)
                {
                    // one burst, spread: each bot keeps to its own share of the standing kin
                    std::sort(standing.begin(), standing.end(), [](Unit* a, Unit* b)
                        { return a->GetGUIDLow() < b->GetGUIDLow(); });
                    Unit* share = standing[me->GetGUIDLow() % standing.size()];
                    return share ? share : lowest;
                }
                // Each bot keeps its own share of the pack (by guid) and only helps the healthiest
                // when its own has fallen well behind: "the healthiest" alone had every damage
                // dealer switching targets every second (2026-09-25: 20-36 switches each in 40 s,
                // melee running between tanks, raid damage under 2k/s).
                std::sort(standing.begin(), standing.end(), [](Unit* x, Unit* y)
                    { return x->GetGUIDLow() < y->GetGUIDLow(); });
                Unit* share = standing[me->GetGUIDLow() % standing.size()];
                Unit* current = me->GetVictim();
                if (current && current->IsAlive() && risers.count(current->GetEntry()) &&
                    std::find(standing.begin(), standing.end(), current) != standing.end() &&
                    current->GetHealthPercent() > 8.0f &&
                    current->GetHealthPercent() + 15.0f >= healthiest->GetHealthPercent())
                    return current;
                if (share && share->GetHealthPercent() > 8.0f &&
                    share->GetHealthPercent() + 15.0f >= healthiest->GetHealthPercent())
                    return share;
                return healthiest;
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
            if (!victimPlayer || !group->IsMember(victimPlayer->GetObjectGuid()))
                continue;
            if (!bot.IsValidAssistTarget(mob) || AiBotAI::IsKnownRebounder(mob->GetEntry()))
                continue;
            // an awake one of a rank before a sheeped one of the same rank; a sheep before a worse rank
            uint8 rank = uint8(KillRank(static_cast<Creature*>(mob)) * 2 +
                (mob->HasBreakableByDamageCrowdControlAura() ? 1 : 0));
            // Peel: a small add chewing on a healer or a caster dies before anything else
            // (2026-09-25: Onyxia's whelps killed 18 while the damage stayed on her).
            // Ranged damage dealers peel only what chews on themselves: a stream of small adds
            // otherwise keeps every caster off the boss for good (2026-09-25: Onyxia's second
            // phase took six minutes, her whelps drawing all the ranged damage the whole time).
            if (!static_cast<Creature*>(mob)->IsElite() && victimPlayer &&
                (bot.GetCombatActiveRole() != ROLE_RANGE_DPS || victimPlayer == me))
                if (AiBotAI* vai = dynamic_cast<AiBotAI*>(victimPlayer->AI()))
                    if (vai->GetCombatActiveRole() != ROLE_TANK)
                        rank = 0;
            if (rank > bestRank || (rank == bestRank && (best == nullptr ? rank >= anchorRank : mob->GetHealthPercent() >= bestHealth)))
                continue;
            best = mob;
            bestRank = rank;
            bestHealth = mob->GetHealthPercent();
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
};

} // anonymous namespace

// Factory hook consumed by MakeDoctrine() in AiBotDoctrine.cpp.
std::unique_ptr<IEngagementDoctrine> MakePlayerPartyDoctrine()
{
    return std::unique_ptr<IEngagementDoctrine>(new AiBotDoctrinePlayerParty());
}