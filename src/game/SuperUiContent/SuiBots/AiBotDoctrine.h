#ifndef MANGOS_AIBOTDOCTRINE_H
#define MANGOS_AIBOTDOCTRINE_H

// =====================================================================================
//  AiBotDoctrine.h  --  Engagement Doctrines (Layer D)
//
//  Freezes the M0 doctrine interface from AIBOT_PLAYBOOK_ARCHITECTURE v3 (§0 pillar 1,
//  §2). The three base combat states (Solo / TeamAuto / Directed) become three TUs
//  behind ONE top-level dispatch, so group behaviour can never again leak through solo
//  code paths.
//
//  REVISION (this pass, per Nico's "split everything, full extraction" call):
//    (1) Added HoldingForTeam() -- the signal that lets B3 (wait-for-anchor pull-hold)
//        live INSIDE TeamAuto while the freeze/GRIND_BLOCKED bookkeeping stays in the
//        spine. Non-pure with a false default, so Solo/Directed need not override it.
//    (2) Added the three factory-hook free functions (one per doctrine TU) that the
//        central MakeDoctrine() in AiBotDoctrine.cpp dispatches to. Each doctrine TU
//        keeps its concrete class file-local and exposes only its Make*() hook.
//    (3) 2026-07-07: added the FOURTH doctrine, PlayerParty (escort mode) -- a REAL
//        player invited this bot to their party, the human IS the coordinator. Auto-
//        selected off FindPartyBoss(), ranked ABOVE TeamAuto so a stale C# directive
//        can never out-vote a live human. AiBotDoctrinePlayerParty.cpp.
//
//  Line endings: LF (C++ repo convention).
// =====================================================================================

#include <memory>

class AiBotAI;
class Unit;

// -------------------------------------------------------------------------------------
//  The three base combat states (§2.1). One dispatch selects exactly one.
// -------------------------------------------------------------------------------------
enum class DoctrineKind
{
    Solo,       // ungrouped, or grouped with no active directive. Today's solo behaviour.
    TeamAuto,   // assist directive active + autonomous posture (the god-bot's group).
                //   The WHOLE group-fight decision -- resolver-first acquisition, B3
                //   wait-for-anchor pull-hold, B2 mid-combat convergence, sticky-assist --
                //   lives in AiBotDoctrineTeam.cpp and nowhere else.
    Directed,   // companion / puppet posture (M2). Orders + the player are the authority.
    PlayerParty // (2026-07-07) a REAL player is in this bot's group -- the human IS the
                //   coordinator. Follow + assist his fights (his victim -> his attacker ->
                //   a party member's attacker -> sticky), defend, NEVER initiate. Selected
                //   off FindPartyBoss(), ranked ABOVE TeamAuto (a live human outranks a
                //   stale directive stamp). AiBotDoctrinePlayerParty.cpp.
    ,
    EncounterPlay // (PLAN_19 M-D) fleet raiding under an adopted raid plan:
                //   TeamAuto COMPOSED with the plan's add-duty targeting.
                //   Selected on the TeamAuto conditions + HasRaidPlan(); ranked
                //   below PlayerParty. AiBotDoctrineEncounter.cpp.
};

// -------------------------------------------------------------------------------------
//  Conduct posture -- the input the resolver reads first (§2.2, §3). AUTONOMOUS is the
//  compiled-in default (absent conduct stamp == today, bit-for-bit); until the M2 conduct
//  substrate lands, every bot is Autonomous, so ResolveDoctrine() only ever picks between
//  TeamAuto and Solo and the Directed branch is present-but-dead.
// -------------------------------------------------------------------------------------
enum class ConductPosture
{
    Autonomous,
    Companion,
    Puppet
};

// -------------------------------------------------------------------------------------
//  IEngagementDoctrine -- the decision surface (§2.3).
//
//  A doctrine owns the ENGAGEMENT decisions and its own transient state. It does NOT own
//  the shared machinery: rotations + guards (Layer R/G), kill detection, loot,
//  AttackStart/CanTryToCastSpell/DoCastSpell, the stalemate/retreat *mechanisms* (opt-in
//  only, via UseX()), task bookkeeping, the bridge, echo fields. Transient state (the B3
//  pull-hold counter, the sticky-assist memo) lives on the concrete instance, so a
//  doctrine swap resets it by construction.
// -------------------------------------------------------------------------------------
struct IEngagementDoctrine
{
    virtual ~IEngagementDoctrine() = default;

    // OOC acquisition (§2.4): who to pull for the current task. nullptr = hold/patrol/wait.
    // Dispatch sites: TASK_GRIND acquire, TASK_MOVE_TO approach scan.
    virtual Unit* AcquireTarget(AiBotAI& bot) = 0;

    // Pull discipline (§2.4): final veto before AttackStart on an acquired candidate.
    virtual bool HoldPull(AiBotAI& bot, Unit* candidate) = 0;

    // In-combat maintenance (§2.4). Contract: return the unit the doctrine INSISTS on this tick
    // (the spine commits to it -- switches if it differs, holds if it matches -- bypassing the
    // legacy range/reselect machinery), or nullptr to DEFER to the spine's legacy in-combat
    // handling (kill-credit, the VISIBILITY_DISTANCE_SMALL drop, the solo reselect). Solo
    // always defers (nullptr); TeamAuto commits to the anchor's mob, or defers when it has no
    // group opinion. Routing both the valid-victim branch and the dead-victim reselect through
    // this one call is what makes the four-way target flap structurally impossible.
    virtual Unit* MaintainTarget(AiBotAI& bot, Unit* victim) = 0;

    // Shared-machinery opt-ins (§2.4): if (doctrine->UseX() && HandleX()) return;
    virtual bool UseStalemateBreaker() const = 0;
    virtual bool UseOverpullRetreat()  const = 0;
    virtual bool UseTapRespect()       const = 0;

    // B3 seam: after AcquireTarget() returns nullptr, the spine asks whether that null was a
    // DELIBERATE wait-for-anchor hold (patrol, do NOT count a freeze tick / fire
    // GRIND_BLOCKED) or a genuinely dry field (run the freeze bookkeeping). Only TeamAuto
    // ever returns true; the default keeps Solo/Directed on the plain "no target" path.
    virtual bool HoldingForTeam() const { return false; }

    // After MaintainTarget() returns nullptr in combat: true when that null means "stop hitting what
    // you hit" (a kill order holds this bot off every add it could reach), not "keep your own victim".
    virtual bool HoldsOffVictim() const { return false; }

    // Observability: echoed on STATE (`doctrine`), logged on every resolve swap.
    virtual char const* Name() const = 0;
};

// -------------------------------------------------------------------------------------
//  Resolver + factory. ResolveDoctrine reads posture + m_combatDirective + grouping off
//  the bot (definition in AiBotDoctrine.cpp, the activation commit -- needs full AiBotAI).
//  MakeDoctrine dispatches to the per-TU hooks below.
// -------------------------------------------------------------------------------------
DoctrineKind ResolveDoctrine(AiBotAI const& bot);
std::unique_ptr<IEngagementDoctrine> MakeDoctrine(DoctrineKind kind);

// Per-TU factory hooks (each defined in its own doctrine .cpp; concrete classes stay
// file-local there). MakeDoctrine() in AiBotDoctrine.cpp is the only caller.
std::unique_ptr<IEngagementDoctrine> MakeSoloDoctrine();
std::unique_ptr<IEngagementDoctrine> MakeTeamDoctrine();
std::unique_ptr<IEngagementDoctrine> MakeDirectedDoctrine();
std::unique_ptr<IEngagementDoctrine> MakeEncounterDoctrine();
std::unique_ptr<IEngagementDoctrine> MakePlayerPartyDoctrine();   // escort mode (2026-07-07)

#endif // MANGOS_AIBOTDOCTRINE_H