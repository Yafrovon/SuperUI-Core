#pragma once

// =====================================================================================================================
// [TACTICS LAW] Raid tactic containment (owner, 2026-11-15): "keep it contained to the raid it's on ... it's invalid when
// we are talking about raids where the behavior is pack/boss specific."
//
// Every tactic added for a raid fight is registered here with the raid it was made for (map ids) and, where it is about
// particular creatures, the creature entries (a boss, a pack's kinds). The code that applies it asks SuiTactic::On(me,
// id, subject) and keeps the previous behaviour everywhere else - never a generic change that silently reaches every
// other raid, dungeon and boss already working. Widening a tactic to another raid or boss is an edit of its row here,
// made after that fight showed the need, with the evidence in the row's `origin`.
//
// A row with no map is refused by tools/raid-tactic-scope-check.sh (MSUIClient repo); so is a dated [TACTICS] comment
// from 2026-11-15 on in SuiBots/ or SuiAutopilot.cpp without a SuiTactic::On gate beside it. Law: MSUIClient
// shared_docs/RAID_TACTIC_SCOPE_LAW.md.
// =====================================================================================================================

#include "Common.h"
#include "Unit.h"
#include "Map.h"

namespace SuiTactic
{
    enum Id : uint8
    {
        CampHoldTankTarget,      // autopilot: a tank stays at the camp only while its own target is there too
        FlooredRiserNoDamage,    // no direct damage on a Core Hound held at its floor while a kin stands above it
        BurstEarlySlowed,        // a death-burster's circle is fire from 35 % (50 % while slowed), not 20 %
        PetHoldAtCamp,           // pets wait at the camp with their owners while the pull comes in
        HotWalkCamp,             // a lured boss's camp keeps clear of his walk where it passes idle packs
        IdleReachHazard,         // healers and casters step out of a sleeping pack's reach during a fight
        IdleReachAllRoles,       // ...tanks and melee too (their fire escapes land outside it)
        PeelToTank,              // a healer/caster an elite is hitting walks to the nearest tank
        NoDodgeOwnAttacker,      // nobody steps out of the front of the mob that is hitting them
        ConeAvoidsIdleBossRoam,  // a cone step never ends inside an idle boss's whole roam
        ShortRepeatWipeHold,     // a boss's repeated threat wipes hold the raid 4 s, not up to 30 s
        NoBubbleUnderBoss,       // a paladin tank held by the boss never Divine Shields (Lay on Hands instead)
        AirborneMeleeSpread,     // while the boss is airborne, at most two melee per small add
        HealWalkAvoidsArcs,      // a healer's closing walk never crosses a breather's other arc
        PrePullFearWard,         // priests Fear Ward every tank before the pull
        FearWardEveryTank,       // with the boss tank and backup warded, the other tanks get one too
        CouncilNo2HForTank,      // loot council never hands a tank a two-handed weapon
        CouncilWearsChoice,      // loot council skips pieces the member's scoring rejects and puts the chosen piece on itself
        SaveTauntForLanding,     // tanks never taunt the boss while she is airborne: the taunt is kept for her landing
        WallAwayFromRaid,        // the cone boss's wall is the one with the fewest members between her head and it
        HealersDecursedFirst,    // decursers take the healers first and walk to a cursed healer out of their reach
        NoTurnAfterLanding,      // once she has landed (below 40 %) her tank holds her where she faces: no wall turning
        RiserKeepsFightOpen,     // a pack member lying down that rises again keeps the fight open (no "fight over")
        RiserBurstFocus,         // the pack's burst: everyone stays on its low one, then all on the lowest left
        SpawnerFirstForRanged,   // the ranged kill this summoner before its multiplying spawn; the melee keep the spawn down
        DamageRaceNoThreatHold,  // a damage-race boss: no damage dealer or healer holds for threat on it
        PeelOutsideFireShield,   // a peel stops outside a Flameguard's Fire Shield pulse, not at the tank's feet
        TanksStageForLanding,    // the boss's two top tanks wait at its landing spot before it lands
        BanishPackElementals,    // warlocks banish a pack's banishable elementals at the pull; the immune ones die first
        OnyxiaThreatDiscipline,  // the 11-14 threat rules: tanks-first windows, backup tank, 75/80 % caps, healer holds
        TankHoldsBossAtCamp,     // between pulses the boss's tank walks him back to the fight's camp (no drift)
        PatrolBackoffWholeRaid,  // a timed walker's back-off triggers on the nearest member, not only on the leader
        WideIdlePackReach,       // a sleeping elite of these kinds is credited 34 yd of reach, not 28
        BackoffAwayFromWalker,   // the leader's back-off from a timed walker replaces its walk and heads away from him
        GarrTanksAtCamp,         // the tanks wait at the camp for this pack instead of walking out with the puller
        SkipUnambushablePatrol,  // a patroller with no clear ambush three times running is skipped for 15 min
        DeepBreathEscape,        // a forecast Deep Breath is escaped once to the nearest spot outside every impact, held
        WhelpPostsAtSpawns,      // the off-tanks hold posts at the two whelp spawn points and take the whelps there
        LandingHeadSpot,         // landed, her two tanks hold her head spot and anyone else she turns on brings her there
        FlanksOffHeadDirection,  // landed, the raid's flanks are measured from her fixed head direction, not her facing
        LandingDescentHold,      // airborne under 40 % (her script resets threat as she turns to land): tanks first
        PairMeetsHer,            // landed away from her landing spot: the main pair tank takes her there and walks her home
        WhelpsWalkedToTanks,     // a whelp on a healer/caster: it walks the whelp to the nearest tank outside her pair
        NoSplashOnLooseWhelps,   // no area spell of a non-tank reaches a whelp no tank holds
        WhelpSpawnKeepOut,       // healers and casters keep 20 yd off the whelp spawn points (a new wave takes the nearest)
        CorridorClearOfPacks,    // a corridor pull plan needs its pull spot and camp 20 yd clear of idle packs
        SelfAreaTaunt,           // Challenging Shout (a self-centred area taunt) is cast on myself, for >= 2 loose mobs in 10 yd
        HealerLaneClear,         // airborne at a flight point: a HEALER deep in that point's Deep Breath lane steps out early (<= 26 yd)
        PatrolAmbushClear,       // a trash patrol's ambush point needs 35 yd from idle packs, or it is "no clear ambush"
        LureNeedsPackDead,       // Lucifron's lure counts every LIVING Core Hound within 40 yd of it, idle or not
        OnyxiaBeforeWhelps,      // her (elite) whelps do not put Onyxia behind them in the kill order
        PatrolWalksOverlap,      // a trash patrol's ambush counts other lone trash patrols where they stand, not along their walk
        RaidWideSpawnTarget,     // one multiplying spawn (Lava Spawn) is the whole raid's target until it dies
        FlooredRiserStopSwing,   // no white swing / landing cast on a Core Hound held at its floor while a kin stands above it
        RoomPatrolTagToCamp,     // a patrol a boss waits for with no clear ambush: tagged where it walks clear, fought at the camp
        CrowdedPatrolTimesPull,  // a patrol just found with no clear ambush times the next pull whose pack its walk passes
        WhelpPostSweepsHealers,  // an Onyxia whelp-post tank with no whelp of its own picks up a whelp on a healer near its post
        LavaPackPullByElemental, // a Firewalker/Flameguard/Lava Elemental pack is pulled by one of its Lava Elementals
        LavaPackCampClear,       // a camp closer than 55 yd to an idle Lava pack is crowded: that pack goes first
        CorridorSkipHolds,       // a patrol skipped by corridor-clear-of-packs is not un-skipped as 'in the way': what it blocks waits with it
        GiantPackBeforeFirelord, // an idle Molten Giant/Destroyer pack within 20 yd of a Firelord/Annihilator pull goes first
        BossPassSkipHolds,       // room trash whose walk passes the boss stays put off; another pack member / room pack is tried
        NoPlanNoWalkIn,          // a Firelord/Annihilator with no pull plan is not walked at: hold at the clear spot, then skip once
        TimedWalkerGoesFirst,    // a patrol the walk has backed off from for 120 s becomes the next pull itself
        TimedWaitNotSelf,        // a pull is never 'timed' around the target itself (wait for X to walk away from X)
        WhelpsOffHealers,        // Onyxia's (elite) whelps on a healer are peeled like small adds: melee always, ranged within 25 yd
        NoPlanSkipHolds,         // a Firelord/Annihilator skipped for having no plan stays skipped; what it stands in front of waits with it
        AmbushSkipHolds,         // a patrol skipped for having no ambush stays skipped; a pull whose walk passes it waits with it
        Count
    };

    struct Scope
    {
        Id id;
        char const* key;
        uint32 maps[4];          // raid map ids it applies on; at least one (no global tactics)
        uint32 entries[12];      // creature entries it is about; all zero = the whole raid
        char const* origin;      // the fight that showed the need
    };

    // Raid maps
    uint32 constexpr kOnyxia = 249, kMoltenCore = 409;

    inline Scope const kScopes[] =
    {
        { CampHoldTankTarget,     "camp-hold-tank-target",   { kMoltenCore }, {},
          "2026-11-15 R3W0 MC Lucifron: tanks ran 45 yd out to the paladin's hounds, held them on his walk" },
        { FlooredRiserNoDamage,   "floored-riser-no-damage", { kMoltenCore }, { 11671 },
          "2026-11-15 R3W0 MC Lucifron: Core Hounds floored one at a time by the tanks' own strikes for 167 s" },
        { BurstEarlySlowed,       "burst-early-slowed",      { kMoltenCore }, { 12099 },
          "2026-11-15 R3W0 MC Garr: Magma Shackles held rogues in a Firesworn's eruption" },
        { PetHoldAtCamp,          "pet-hold-at-camp",        { kMoltenCore }, {},
          "2026-11-15 R3W0 MC Geddon: a cat bit Geddon on his walk beside a Firewalker pack" },
        { HotWalkCamp,            "hot-walk-camp",           { kMoltenCore }, { 12056 },
          "2026-11-15 R3W0 MC Geddon: 98 % wipes, the camp beside his walk where it passes a pack" },
        { IdleReachHazard,        "idle-reach-hazard",       { kMoltenCore }, {},
          "2026-11-15 R3W0 MC Geddon: healers 21-29 yd from the Firewalker pack, it joined at +47/+93 s" },
        { IdleReachAllRoles,      "idle-reach-all-roles",    { kMoltenCore }, {},
          "2026-11-15 R3W1 MC Geddon: tanks stepped out of his Inferno onto his Firewalker pack" },
        { PeelToTank,             "peel-to-tank",            { kMoltenCore }, { 11666, 11667, 12076, 12100 },
          "2026-11-15 R3W1 MC: Geddon's Firewalker/Flameguard/Lava Elemental packs on healers, no Taunt landed" },
        { NoDodgeOwnAttacker,     "no-dodge-own-attacker",   { kMoltenCore }, { 11666, 11667, 12076, 12100 },
          "2026-11-15 R3W1 MC: healers stepped out of the front of their own attacker every 1-2 s" },
        { ConeAvoidsIdleBossRoam, "cone-avoids-idle-boss",   { kMoltenCore }, {},
          "2026-11-15 R3W1 MC: a mage's cone step 40 yd from Shazzrah's home woke him" },
        { ShortRepeatWipeHold,    "short-repeat-wipe-hold",  { kMoltenCore }, { 12264 },
          "2026-11-15 R3W1 MC Shazzrah: Gate wipes every ~45 s, 30 s holds, 680-808 raid dps" },
        { NoBubbleUnderBoss,      "no-bubble-under-boss",    { kOnyxia }, { 10184 },
          "2026-11-15 R3W2 Onyxia: the paladin tank's Divine Shield at +16 s sent her into the raid" },
        { AirborneMeleeSpread,    "airborne-melee-spread",   { kOnyxia }, { 10184 },
          "2026-11-15 R3W2 Onyxia phase 2: seven rogues on one whelp, one Engulfing Flames, 15 dead" },
        { HealWalkAvoidsArcs,     "heal-walk-avoids-arcs",   { kOnyxia }, { 10184 },
          "2026-11-15 R3W2 Onyxia phase 3: healers crossed her front, one Flame Breath killed 8" },
        { PrePullFearWard,        "pre-pull-fear-ward",      { kOnyxia }, {},
          "2026-11-15 R3W0 Onyxia: two phase-3 wipes began with a feared, unwarded tank" },
        { FearWardEveryTank,      "fear-ward-every-tank",    { kOnyxia }, {},
          "2026-11-15 R3W0 Onyxia: the first ward came 35 s after she landed" },
        { CouncilNo2HForTank,     "council-no-2h-for-tank",  { kOnyxia, kMoltenCore }, {},
          "2026-11-15 R3W0/R3W1: Earthshaker handed to a tank at every council, never worn" },
        { SpawnerFirstForRanged,  "spawner-first-for-ranged", { kMoltenCore }, { 11668 },
          "2026-11-17 R3W6/W9/W16 MC: three two-Firelord pulls ran away into +117/+211/+223 Lava Spawns (they split every 10 s) while the whole raid killed spawns and both Firelords stood at 34-44 % summoning more" },
        { RiserBurstFocus,        "riser-burst-focus",       { kMoltenCore }, { 11671 },
          "2026-11-17 R3W11/R3W12 MC: a Core Hound pack rose 27 times in 595 s (W11, 33 dead); in the burst the last two at 5-6 % lived through the window while ~60 'focus -> Core Hound' switches went out in 3 s (each bot's share reshuffled at every fall)" },
        { RiserKeepsFightOpen,    "riser-keeps-fight-open",  { kMoltenCore }, { 11671 },
          "2026-11-16 R3W8/R3W9 MC: a Core Hound pack's fight was closed while hounds lay down; they rose, the raid walked off, a 1 s '2 dead wipe' (W8) and Lucifron joining the risen pack (W9, 38/40)" },
        { NoTurnAfterLanding,     "no-turn-after-landing",   { kOnyxia }, { 10184 },
          "2026-11-16 R3W8/R3W9 Onyxia: every phase-3 wall turn swept her Flame Breath across the raid (hits up to 59 deg off her facing on members already moving out); W9 att. 1 33 %, att. 2 wipe" },
        { HealersDecursedFirst,   "healers-decursed-first",  { kMoltenCore }, { 12264 },
          "2026-11-16 R3W7 MC Shazzrah 31 %: five priests dead by +50 s to doubled Arcane Explosions (~2000); they stood out of the mages' 30 yd decurse reach - 1-4 decurses each against 13-18 for the paladins beside the mages" },
        { WallAwayFromRaid,       "wall-away-from-raid",     { kOnyxia }, { 10184 },
          "2026-11-16 R3W7 Onyxia 33 %: her head was turned to the nearest wall, 28 yd west, where the healers and ranged stood - one Flame Breath at +311 s killed 3, the next 3 more" },
        { SaveTauntForLanding,    "save-taunt-for-landing",  { kOnyxia }, { 10184 },
          "2026-11-16 R3W6 Onyxia 39 %: tanks taunted her in the air (no effect), Taunt was on cooldown when she landed, a hunter held her and one Flame Breath killed 6" },
        { CouncilWearsChoice,     "council-wears-choice",    { kOnyxia, kMoltenCore }, {},
          "2026-11-16 R3W5: Staff of Dominance + Earthshaker 'in bags' on an Arms warrior at every council (both scored -100000, worn sword+shield -200000); Helm of Wrath, Belt/Sabatons of Might, Eskhandar's Collar lost to 'Sylwhisper has no bag room'" },
        { DamageRaceNoThreatHold, "damage-race-no-hold",     { kMoltenCore }, { 12264 },
          "2026-11-15 R3W2 MC Shazzrah: 44 % at 646 raid dps, 154 threat holds at 0 threat; guides race him" },
        { PeelOutsideFireShield,  "peel-outside-fire-shield", { kMoltenCore }, { 11667 },
          "2026-11-15 R3W2 MC: three Geddon-pack wipes; peeled healers stood 2.5 yd from the tank in the 10 yd Fire Shield" },
        { TanksStageForLanding,   "tanks-stage-for-landing", { kOnyxia }, { 10184 },
          "2026-11-15 R3W3 Onyxia: she landed 66-88 yd from every tank, a warlock held her, one Flame Breath killed 6" },
        { BanishPackElementals,   "banish-pack-elementals", { kMoltenCore }, { 12076, 12100, 12099 },
          "2026-11-15 R3W3 MC: three wipes on Firewalker+Flameguard+Lava Elemental packs, no banish until a 5th elite joined; R3W5 Garr's Firesworn (12099): two warlocks dead at +7 s, banishes broke at +34 s, 4 loose" },
        { OnyxiaThreatDiscipline, "onyxia-threat-discipline", { kOnyxia }, { 10184 },
          "2026-11-14 Onyxia dev rounds (taunt-immune boss, threat wipes, landing); generic until 11-15, held MC bosses and trash back" },
        { TankHoldsBossAtCamp,    "tank-holds-boss-at-camp", { kMoltenCore }, { 12056 },
          "2026-11-16 R3W4 MC Geddon 20 %: Inferno steps walked him 90 yd south into a corner, one Inferno killed 4 tanks + 5 healers" },
        { PatrolBackoffWholeRaid, "patrol-backoff-whole-raid", { kMoltenCore }, { 12056, 12118 },
          "2026-11-16 R3W4 MC Geddon 98 %: back-off began at the leader's 51 yd, a rogue was 9 yd from him and he woke with a pack; R3W5 Lucifron (same back-off, 40/40)" },
        { SkipUnambushablePatrol, "skip-unambushable-patrol", { kMoltenCore }, { 12101, 11673 },
          "2026-11-16 R3W5 MC: a Lava Surger (56847) with 'no clear way to an ambush' was re-chosen 1786 times, the raid stood 35 min before Golemagg; R3W6: skipped, it walked into the Lava Elemental pack pulled next at +16 s (40/40) - a skipped one now times the pulls near its walk | widened 2026-11-20 R4W3 MC 11:50-12:20 to the Ancient Core Hound (11673): with patrol-ambush-clear it had no ambush and was re-chosen 1287 times in 30 min (stall)" },
        { GarrTanksAtCamp,        "garr-tanks-at-camp",      { kMoltenCore }, { 12057, 12099 },
          "2026-11-16 R3W5 MC Garr 40/40 twice: the paladin tank walked out with the puller and six Firesworn killed him in 0.1 s at the pull" },
        { BackoffAwayFromWalker,  "backoff-away-from-walker", { kMoltenCore }, { 12056, 12118 },
          "2026-11-16 R3W5 MC Lucifron 40/40: 'backing off' four times while the walk to his Core Hounds went on - 56 -> 26 yd, he and 5 hounds woke on the leader; R3W8 twice 38/40 + 40/40: the raid waited at a camp on his walk while he came at it (91 -> 67 -> 43 yd)" },
        { WideIdlePackReach,      "wide-idle-pack-reach",    { kMoltenCore }, { 11666, 11667, 12076, 12100 },
          "2026-11-16 R3W4 MC Geddon 44 %: a mage's step after a Living Bomb stopped 29 yd from pack 56722-24 (28 yd credited); it aggroed, 40/40" },
        { DeepBreathEscape,       "deep-breath-escape",      { kOnyxia }, { 10184 },
          "2026-11-18 R4W0 Onyxia att. 1 39 %: one Deep Breath at +126 s killed 5 mages + a priest; its 12 forecast impacts (30 yd, spell_target_position) were known 6 s ahead, but the step-out rings stopped at 25 yd, refused paths into the next circle, and the facing-based line step re-aimed bots onto the centre line" },
        { WhelpPostsAtSpawns,     "whelp-posts-at-spawns",   { kOnyxia }, { 10184 },
          "2026-11-18 R4W0 Onyxia att. 1 39 %: 30 of 40 deaths took whelp melee; the six tanks chased whelps one at a time across the lair (focus switches 36-40 each), 132 Taunts refused for reach, 330 Mocking Blows for stance, 0 Challenging Shouts; the script sends each new whelp at a random member from two fixed spawn points" },
        { LandingHeadSpot,        "landing-head-spot",       { kOnyxia }, { 10184 },
          "2026-11-18 R4W0 Onyxia drill, 3 kills in 12: after the landing her target stayed a ranged member from the air for 4-38 s (11 of 12 pulls); that member stepped out of her front, she followed and her Flame Breath swept the raid (10-25 hit by one breath in 9 pulls); her two staged tanks left the head spot (out of her front, not her victim) and a whelp-post tank chased her and walked her back to its post" },
        { FlanksOffHeadDirection, "flanks-off-head-direction", { kOnyxia }, { 10184 },
          "2026-11-19 R4W0 Onyxia drill 2 pull 3 17 %: her pair tank held her at the head spot, her Knock Away moved him 6 yd and she turned with him; 21 members re-picked flank spots from her new facing in the same second, walked through her front and one Flame Breath hit them (-25..-32 deg, all moving)" },
        { LandingDescentHold,     "landing-descent-hold",    { kOnyxia }, { 10184 },
          "2026-11-19 R4W0 Onyxia drill 2, 3 kills in 12: her script resets threat (DoResetThreat) as she turns to land at < 40 %; the casters fired through the descent and a caster held her at touch-down in 4 of 6 pulls (pull 9: a mage 1348, her pair tank 744); her breath on him and the next killed 13-26" },
        { PairMeetsHer,           "pair-meets-her",          { kOnyxia }, { 10184 },
          "2026-11-19 R4W0 Onyxia drill 2 pulls 7/9/10: at < 40 % west of x -40 she lands at her south point and fights from 25-40 yd west of her landing spot; her pair stood at the head spot 25-36 yd from her, a whelp-post tank kept whelps, she stayed on casters 10-20 s" },
        { WhelpsWalkedToTanks,    "whelps-walked-to-tanks",  { kOnyxia }, { 11262 },
          "2026-11-19 R4W0 Onyxia drill 3 pulls 3-6: whelps killed 24/27/33/31 of 40 in phase 2 - each whelp attacks a random member, and the healers and casters it chose stayed where they were while the post tanks waited at the spawn points" },
        { NoSplashOnLooseWhelps,  "no-splash-on-loose-whelps", { kOnyxia }, { 11262 },
          "2026-11-19 R4W0 Onyxia drill 3 pulls 4-6: mages' Arcane Explosion / Frost Nova were the first hit on 42-47 whelps a pull that were on no tank yet (the splash rule treats a mob on a non-tank as loose); the whelps turned on the 3.2k-health mages, then on the healers beside them" },
        { WhelpSpawnKeepOut,      "whelp-spawn-keep-out",    { kOnyxia }, { 10184 },
          "2026-11-19 R4W0 Onyxia drill 3 pull 8 40 %: a wave of 8 whelps all went for two priests standing 10-13 yd from the east spawn point (lane spot, heal walk, peel to the post tank there) - both dead in 2 s, then two more priests; whelps 32 of 40 deaths" },
        { CorridorClearOfPacks,   "corridor-clear-of-packs", { kMoltenCore }, {},
          "2026-11-19 R4W1 MC 16:50: a corridor plan for the Lava Surger R56843 walking through the imp room took a pull spot 1 yd and a route 1 yd from idle packs ('clear of other packs by 1 yd'); at the pull spot the Surger and imp packs P56584 + P56594 engaged together, 40/40 in 78 s" },
        { SelfAreaTaunt,          "self-area-taunt",         { kMoltenCore }, {},
          "2026-11-19 R4W1 MC Garr 91 %: Challenging Shout was never cast from the taunt rung - 'not cast: ready 1 ... dist 3', telemetry 1161:gate 9-39 a fight: CanTryToCastSpell measured the enemy against the shout's 0 yd range; the paladin tank died at the pull holding the Firesworn (6th time: W8, W9, W10, W12, W16, R4W1)" },
        { HealerLaneClear,        "healer-lane-clear",       { kOnyxia }, { 10184 },
          "2026-11-19 R4W0/R4W1 Onyxia drills 4-5: healers took ~53 of ~100 Deep Breath deaths (10 of 40 members; the three paladin healers 8-10 each), and a breath that killed healers began the whelp cascade in most phase-2 wipes (drill 5 pulls 2-4). The all-member lane clear (deep-breath-lane-clear) was REVERTED - 100-160 walks to 60 yd a pull; this one moves only healers, at most 26 yd, to a spot that sees a tank" },
        { PatrolAmbushClear,      "patrol-ambush-clear",     { kMoltenCore }, { 11673, 12101 },
          "2026-11-20 R4W2 MC 01:16: the Ancient Core Hound R56855 (walks 9 yd past Garr) was ambushed at (706, -475), '26 yd from other packs' = 28 yd from Garr's centre, inside his 42 yd reach ring; Garr + 8 Firesworn joined at +7 s, 40/40. RAID_MAP: B-Garr 16 win / 27 wipe as the pulled target, R56855 joined 7 (6 wipes); Garr's pack joined R56855's fights 8 times" },
        { LureNeedsPackDead,      "lure-needs-pack-dead",    { kMoltenCore }, { 12118 },
          "2026-11-20 R4W3 MC Lucifron att. 1 100 %: the playbook lure (1000, -945) ran with pack A (P56634) alive - RAID_MAP: the lure is 22 yd from it, inside its idle reach - and its five hounds joined at +0 s; the 25 yd clearance passed because the hounds were not idle at that moment (a hound pack fight beside the lure had just ended). Same cause as R3W15's 100 % wipe" },
        { OnyxiaBeforeWhelps,     "onyxia-before-whelps",    { kOnyxia }, { 11262 },
          "2026-11-20 R4W4 Onyxia att. 3 18 %: in phase 3 'focus -> Onyxian Whelp (kill order)' 138 times against 'Onyxia (kill order)' 54 - the whelps are elite, so 'a boss goes after its elite adds' ranked her last (rank 10); she lost 22 % in 173 s while 75 whelp engagements killed 24. The 11-14 fix meant small adds never hold her up but counted the whelps as elite; phase 2 had run 150-200 s the same way" },
        { PatrolWalksOverlap,     "patrol-walks-overlap",    { kMoltenCore }, { 11673, 12101 },
          "2026-11-20 R4W4 MC 17:43-19:01: Garr's Ancient Core Hounds R56855 and R56857 walk crossing lines (6 yd apart at one end); each one's walk crowded the other's ambush below patrol-ambush-clear's 35 yd (20 / 25 yd), both were skipped and RoomTrash took them back for Garr - ~4400 re-choices in 78 min, the night ended at 3/10 (R4W3: a similar ACH loop, 30 min)" },
        { RaidWideSpawnTarget,    "raid-wide-spawn-target",  { kMoltenCore }, { 12265 },
          "2026-11-20 R4W4 MC 15:21 (P56777) + 15:44 (P56720): both Firelords dead, then the Lava Spawns ran away - 2933 focus switches onto Lava Spawns and 4 killed in one fight, 366 up; the 'oldest spawn' was chosen per bot over its own 45 yd, so the raid split its damage" },
        { FlooredRiserStopSwing,  "floored-riser-stop-swing", { kMoltenCore }, { 11671 },
          "2026-11-20 R4W5 MC 23:42 (P56634, 3rd failure on that pack): 187 s, 13 hound downs; the first went down at kin 28 24 24 16 - damage on hounds held at their floor came from tank white swings (Boomwarrior 6.9k, Shieldwall 6.0k, Ironwarden 4.5k) and Frostbolts/Shadow Bolts already in the air (2.7-6.0k each); floored-riser-no-damage only gates new casts" },
        { RoomPatrolTagToCamp,    "room-patrol-tag-to-camp", { kMoltenCore }, { 11673 },
          "2026-11-21 R4W5 MC 01:19-01:40: Garr's Ancient Core Hound R56855 alone (R56857 dead) - best ambush point 26 yd from idle packs (patrol-ambush-clear needs 35), skipped by skip-unambushable-patrol, RoomTrash took it back for Garr: 414 re-choices in 20 min (R4W4: 78 min)" },
        { CrowdedPatrolTimesPull, "crowded-patrol-times-pull", { kMoltenCore }, { 12101, 11673 },
          "2026-11-21 R4W5 03:00 + 03:21 and R4W6 09:07: Lava pack P56729 pulled seconds after Lava Surger R56849 got no clear ambush (its walk passes 9 yd from the pack); the Surger joined at +7-13 s each time, 40/40, 23/40, 40/40 - the timing only applied to SKIPPED patrols (three no-ambush in a row)" },
        { WhelpPostSweepsHealers, "whelp-post-sweeps-healers", { kOnyxia }, { 11262 },
          "2026-11-21 R4W7 att. 2-4 (35/29/36 %) and R4W8 att. 1 (29 %): after the landing whelps killed the healers first every time (17-30 deaths by whelp melee); whelp-posts-at-spawns takes only whelps within 16 yd of a post, a whelp past the posts had no taker, and warrior taunts are melee range" },
        { LavaPackPullByElemental, "lava-pack-pull-by-elemental", { kMoltenCore }, { 11666, 11667 },
          "2026-11-21 P56750: R4W6 09:21 and R4W8 19:45 pulled by its Firewalker, 40/40 both; the R4W8 drill (20:14-20:38) 9 of 9 clear when the planner pulled a Lava Elemental, the one fail (10 dead) pulled by the Flameguard" },
        { LavaPackCampClear,      "lava-pack-camp-clear",    { kMoltenCore }, { 11666, 11667, 12076, 12100 },
          "2026-11-22 R4W9 23:39 (P91268 joined an ACH fight from a camp 34 yd off, 21 dead) and R4W10 05:13 (P56722 woke at +116 s, camp 46 yd off, 40/40): a sleeping Lava elite reaches 34 yd (wide-idle-pack-reach) and the raid spreads ~20 yd round its camp; the crowded test only asked 35 yd" },
        { CorridorSkipHolds,      "corridor-skip-holds",     { kMoltenCore }, { 12101 },
          "2026-11-22 R4W11 10:38-10:59 (after Sulfuron, three runes and Majordomo left): Lava Surger R56845 had no corridor plan clear of packs, was skipped for 10 min and was un-skipped the next second as standing in the way of the other Surger - eleven rounds of 120 s, the night ended with Majordomo never pulled" },
        { GiantPackBeforeFirelord, "giant-pack-before-firelord", { kMoltenCore }, { 11658, 11659 },
          "2026-11-22 R4W11 08:35 + R4W12 14:54, Gehennas' room: Firelord pulls next to P56708 (Molten Giant + Destroyer, 16-18 yd from Firelords 56788 / 91282) woke it at +4 s / +55 s - 40/40 both weeks, the long fight let ~150 Lava Spawns split. P56708 pulled as the target: 23 fights, 23 wins" },
        { BossPassSkipHolds,      "boss-pass-skip-holds",    { kMoltenCore }, { 11666, 11667, 12100 },
          "2026-11-22 R4W13 22:20-22:48 (Golemagg's room, Golemagg and Sulfuron left): Firewalker 56797 / Flameguard 56799 (P56797) were put off because the walk passed Golemagg at 27-31 yd, and RoomTrash un-skipped them the next second - 500+ picks, no pull, the night ended at 6/10. R4W12 pulled the same pack through its Lava Reaver 56798 from (855,-1012)" },
        { NoPlanNoWalkIn,         "no-plan-no-walk-in",      { kMoltenCore }, { 11668, 11665 },
          "2026-11-23 R4W14 23:35-23:38 P56786: corridor plan for Firelord 56796 refused 13 times while the leader walked from (1130,-525) to (1176,-595); aggroed at 25 yd (unplanned), camp 12 yd off Lava Surger R56839's walk, Surger +14 s, ACH R56854 +34 s, 40/40. Same cause as R4W6 05:57 on P56786 (no plan, walked in, 40/40); R4W14 00:43 P56721 unplanned too" },
        { TimedWalkerGoesFirst,   "timed-walker-goes-first", { kMoltenCore }, { 12101, 11673 },
          "2026-11-23 R4W15 04:50-05:00 imp room: 'Lava Surger comes near (42-57 yd) on the walk to Flame Imp - backing off' 594 times in 10 min, no pull; it ended when the Surger was taken first (05:06, 27 s, 0 dead)" },
        { TimedWaitNotSelf,       "timed-wait-not-self",     { kMoltenCore }, { 12101, 11673 },
          "2026-11-23 R4W15 05:00-05:06 imp room: the Surger 56841 became the target while the imp pull was still timed around it - 'waiting for Lava Surger to walk away from Lava Surger (pack 0 ...)' until the 300 s cap; R3W15 had the same self-timing in the back-off" },
        { WhelpsOffHealers,       "whelps-off-healers",      { kOnyxia }, { 11262 },
          "2026-11-23 R4W14 att. 1, R4W15 att. 1, R4W16 att. 1: healers first to whelp melee in phase 2 every night (4, 7, 6 healers at +160-250 s). The peel rule (a small add on a healer dies first) skips elites and her whelps are elite: only post tanks ever took a whelp off a healer; R4W16 whelp 1701038 went Vexmark -> Nerret -> Clawknight in 45 s, each healer walking it to a tank" },
        { NoPlanSkipHolds,        "no-plan-skip-holds",      { kMoltenCore }, { 11668, 11665 },
          "2026-11-23 R4W17 MC 14:32 P56721: Firelord 91283 held 60 s with no plan and was skipped for 10 min, and the next second 'Firelord stands in the way of Molten Giant - it goes first' made it the target again; pulled unplanned at 14:32:58, 39/40, Firelord 79 %, Annihilator 7 %, the raid on Lava Spawns. Same class as corridor-skip-holds (R4W11)" },
        { AmbushSkipHolds,        "ambush-skip-holds",       { kMoltenCore }, { 12101, 11673 },
          "2026-11-23 R4W18 MC 18:04-18:07 corridor: Lava Surger 56839 skipped 15 min at 18:04:13 (no ambush three times running) was un-skipped 52 s later by 'Lava Surger stands 28 yd from the walk to Molten Destroyer - it goes first'; it woke on the way in at (1155,-588), five Firelords/Annihilators and Ancient Core Hound R56854 joined at +27-44 s, 40/40 in 106 s" },



    };

    inline Scope const* Find(Id id)
    {
        for (Scope const& s : kScopes)
            if (s.id == id)
                return &s;
        return nullptr;
    }

    // A row switched off for this run by its key in sui-tactics-off.txt beside mangosd.conf (one key per line, '#'
    // comments; read once per server start - AiBotAISpecCombat.cpp). For A/B drills without a rebuild: the file records
    // what was off, the ledger cites it (shared_docs/RAID_TACTIC_LEDGER.md, rule 1).
    bool OffByFile(char const* key);

    // Is this tactic on for `me` here (and, when the tactic names creatures, about `subject`)? Off everywhere else:
    // the caller keeps the behaviour it had before the tactic.
    inline bool On(Unit const* me, Id id, Unit const* subject = nullptr)
    {
        Scope const* s = Find(id);
        if (!s || !me || !me->IsInWorld() || OffByFile(s->key))
            return false;
        uint32 const map = me->GetMapId();
        bool mapOk = false;
        for (uint32 m : s->maps)
            if (m && m == map)
                mapOk = true;
        if (!mapOk)
            return false;
        if (!s->entries[0])
            return true;
        if (!subject)
            return false;
        for (uint32 e : s->entries)
            if (e && e == subject->GetEntry())
                return true;
        return false;
    }

    // On for any creature attacking `me` (a tactic about the boss holding me).
    inline bool OnAnyAttacker(Unit const* me, Id id)
    {
        if (!me)
            return false;
        for (Unit* a : const_cast<Unit*>(me)->GetAttackers())
            if (a && a->IsAlive() && On(me, id, a))
                return true;
        return false;
    }
}
