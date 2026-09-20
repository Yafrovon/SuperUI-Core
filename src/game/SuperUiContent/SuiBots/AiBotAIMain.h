/*
 * AiBotAIMain.h — Autonomous AI bot for VMaNGOS 1.12.1
 *
 * Forked from BattleBotAI. Replaces BG logic with a task executor
 * state machine + TCP bridge for C# BotBrainService communication.
 *
 * Phase 1: Wander, fight, eat/drink, self-revive. ✓
 * Phase 2: TCP bridge on port 3444 — HELLO, STATE, EVENT, inbound commands.
 * Phase 2.5: TASK_GRIND — area grind with proactive creature scanning.
 * Session 25: Chunked pathing — mmap waypoints fed in ~200yd chunks via MovePoint.
 * Session 36: Lossless outbound bridge — BridgeSend queues whole lines,
 *             BridgeFlush drains with partial-write/EWOULDBLOCK tolerance.
 *             Fixes silent event loss (dropped TASK_COMPLETE/KILL/LOOT) under load.
 * [TEAMPLAY]: CombatDirective — the per-member group focus-fire stamp (assist seam).
 *
 * ──────────────────────────────────────────────────────────────────────────
 * FILE SPLIT (this header is the single class declaration; the implementation
 * is divided across per-concern translation units — see each .cpp):
 *
 *   AiBotAIMain.cpp     lifecycle + main loop: OnSessionLoaded / OnPlayerLogin /
 *                       OnPacketReceived / MovementInform / UpdateAI
 *   AiBotAICombat.cpp   targeting + per-class combat (18 verbatim BattleBotAI
 *                       methods + dispatchers), AttackStart / SelectAttackTarget /
 *                       CheckForUnreachableTarget, stalemate + overpull discipline,
 *                       self-maintenance (mount / eat-drink)
 *   AiBotAIBridge.cpp   TCP bridge (connect/send/recv/flush), JsonExtract* helpers,
 *                       BridgeProcessLine + every BridgeHandle* command handler,
 *                       Send*Event emitters
 *   AiBotAIMovement.cpp pathing + navmesh: MoveToDestination, chunked pathing,
 *                       IsPathSafe, ReGroundZ, nav-boundary record/find,
 *                       FindNearestNavmeshPoint, StopMoving, DoRandomWander
 *   AiBotAILoot.cpp     DoAutoLoot, TryAutoEquip(Bags), ScoreItem (+ class weights /
 *                       slot map), ChooseQuestReward
 *   AiBotAIGrind.cpp    SelectGrindTarget, DoGrindPatrol, CountNearbyHostiles,
 *                       ScanApproachTarget, ConvertMoveToGrindInPlace
 *
 * AiBotAI.h is now a one-line compatibility shim (#include this) so existing
 * include sites (AiBotAITeamPlay.*, PlayerBotMgr, the factory) are unchanged.
 * ──────────────────────────────────────────────────────────────────────────
 */

#ifndef MANGOS_AIBOTAI_H
#define MANGOS_AIBOTAI_H

#include "CombatBotBaseAI.h"
#include "Timer.h"
#include "Log.h"
#include "PathFinder.h"   // Vector3, PointsArray — needed for SmoothPathCorners' signature
#include "AiBotDoctrine.h" // [DOCTRINE] IEngagementDoctrine + ResolveDoctrine/MakeDoctrine (Layer D)
#include "PlayerBotMgr.h"  // PlayerBotEntry (complete type for m_ownedDummyEntry)
#include <initializer_list>
#include <atomic>       // [SUI] fleet-stagger spawn phase counter (see the constructor)

#ifdef _WIN32
  #include <winsock2.h>
#include <deque>
#include <array>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET BridgeSocket;
  #define BRIDGE_INVALID_SOCKET INVALID_SOCKET
  #define BRIDGE_CLOSE_SOCKET closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int BridgeSocket;
  #define BRIDGE_INVALID_SOCKET (-1)
  #define BRIDGE_CLOSE_SOCKET close
#endif

#define AIBOT_UPDATE_INTERVAL 1000
#define AIBOT_ROTATION_SUBTICK_MS 250   // [ROTATION] slate evaluation cadence in combat — the 1s behaviour tick starves GCD weaving; 4 Hz tracks it

// Bridge config
#define BRIDGE_HOST "127.0.0.1"
#define BRIDGE_PORT 3444
#define BRIDGE_STATE_INTERVAL 5000       // ms between STATE messages
#define BRIDGE_RECONNECT_BASE 2000       // ms initial reconnect delay
#define BRIDGE_RECONNECT_MAX  30000      // ms max reconnect delay
#define BRIDGE_RECV_BUF_SIZE  4096       // inbound buffer
#define BRIDGE_SEND_BUF_MAX   65536      // outbound queue cap (bytes) — drop oldest past this
#define BRIDGE_PROTOCOL_VERSION 6        // v6: restart-safe circuit epochs + bounded combat-latch reset

// Movement point IDs
#define AIBOT_POINT_WANDER          100
#define AIBOT_POINT_TASK_DEST       101
#define AIBOT_POINT_GRIND_PATROL    102
#define AIBOT_POINT_TAXI_APPROACH   103
#define AIBOT_POINT_STALEMATE_NUDGE 104  // short hop to break an in-combat stalemate
#define AIBOT_POINT_OVERPULL_FLEE   105  // retreat hop away from a too-dense pull
#define AIBOT_POINT_PULL_RETREAT    106  // proactive pull: drag a freshly-tagged mob back to open ground
#define AIBOT_POINT_INTERACT_NPC    107  // bridge-owned NPC approach; never aliases a task arrival
#define AIBOT_POINT_REMESH          108  // [REMESH] short straight step back onto the navmesh from an off-mesh start
#define AIBOT_MOVE_POINT_REFUSED_INTERVAL 10000 // ms; autonomous NOPATH telemetry is rate-limited per bot
#define AIBOT_COMBAT_RESET_HOLD_MS 5000  // ms; keep task autonomy parked while ACK + fresh OOC STATE ship
#define AIBOT_COMBAT_RESET_MIN_STILL_SEC 120 // must match the C# fresh-STATE physical-still proof window
#define AIBOT_ARRIVE_JITTER_MIN 0.2f     // never path to the EXACT dest coord (it can land on a bad poly / seam edge → off-mesh).
#define AIBOT_ARRIVE_JITTER_MAX 2.0f     //   instead resolve to a validated point this far out, random angle — fans bots out + dodges the bad poly.
#define AIBOT_ARRIVE_JITTER_TRIES 16     // ring samples to try before giving up and using the exact coord (lets a real no_path surface).

// Chunked pathing config (Session 25)
#define AIBOT_PATH_CHUNK_DIST 200.0f     // max yards per MovePoint chunk

// Navmesh-seam crossing (overworld caves — interior mesh that doesn't stitch to terrain)
#define AIBOT_SEAM_CROSS_DIST 12.0f      // a NOPATH dest within this = a seam → teleport across
#define AIBOT_OFFMESH_EPSILON  2.0f      // bot within this of a navmesh poly = ON mesh; beyond = off-mesh ALARM → re-tare
#define AIBOT_BOUNDARY_SCOPE  60.0f      // a recorded seam helps escape from anywhere within this of its inner point
#define AIBOT_NAVMESH_SNAP_SEARCH 40.0f  // how far out to look for a navmesh poly to snap an off-mesh bot onto
#define AIBOT_REMESH_STEP_MAX      8.0f  // [REMESH] walk (never port) back onto the mesh if it is within this; beyond = refuse + log
#define AIBOT_REMESH_STEP_COOLDOWN_MS 3000 // [REMESH] one step per window so a refused hop cannot spam splines

//Organic movement
#define AIBOT_FILLET_VALIDATE_SEARCH_YARDS 5.0f
#define AIBOT_FILLET_VALIDATE_EPSILON      1.0f
#define AIBOT_JOURNEY_DEST_EPSILON         1.0f

// [FINDING_017 v2] Ring-scan radius, promoted from the magic 3.0f literal in
// AiBotAIMovement's ring scan. The scan accepts a pathable point up to this far
// off the true dest and MoveToDestination writes THAT back into m_currentTask —
// so on an unreachable dest the task jitters by up to this radius every leg.
#define AIBOT_RING_SCAN_RADIUS             3.0f
// The continuation "same journey" tolerance. MUST exceed AIBOT_RING_SCAN_RADIUS
// so ring-scan jitter is NOT mistaken for a new destination (which re-arms the
// no-progress gate every leg and lets the 017 livelock survive its own fix).
// 2x radius covers a full ring-diameter excursion of the pathing target.
#define AIBOT_CONT_SAME_DEST_EPSILON       (2.0f * AIBOT_RING_SCAN_RADIUS)
#define AIBOT_FILLET_MIN_TURN_EPSILON_DEG 5.0f   // below this a vertex is "basically straight" — doesn't start or continue a merge run
#define AIBOT_FILLET_MAX_WINDOW_POINTS    8       // cap on how many consecutive vertices one run can merge (~40yd at this fork's ~5yd spacing) — generous for a real corner, bounded so a long gentle bend can't get swallowed whole

// ── [GROUND] Per-waypoint grounding + snap sanity (2026-07-20, the fly fix) ──
// Every point handed to MovebyPath is now re-grounded (GroundPathPoints). These two caps stop
// a BAD ground reading from turning a 1yd float correction into a cliff drop: a genuine
// navmesh-over-hull float is sub-yard to a few yards, so a "correction" larger than the cap is a
// bad measurement (an unmeshed hole, the floor under a bridge, a wrong-floor poly), not a float.
// Refuse it and log loudly rather than acting on it.
#define AIBOT_WAYPOINT_GROUND_MAX_DROP  200.0f   // waypoint re-ground: refuse a drop beyond this, keep the Detour Z, log [AIBOT-GROUND] REFUSED
#define AIBOT_NAVSNAP_MAX_DROP          8.0f   // same cap inside FindNearestNavmeshPointNear's divergence guard (it recorded 159yd "corrections" in the wild)
// [FINDING_019] Travel-stuck watchdog: a bot physically not advancing toward its task dest for
// this long (ungameable REAL XYZ, unlike the SET_TASK-ack-refreshed progress clock the C# wedge
// uses) is snapped to the nearest navmesh poly (local un-stick, stays in zone) and its leg dropped.
#define AIBOT_TRAVEL_STUCK_MS       90000   // no real advance toward the dest this long -> unstick
#define AIBOT_TRAVEL_STUCK_RADIUS   8.0f    // "advanced" = moved at least this far from the reference
#define AIBOT_TRAVEL_UNSTUCK_SEARCH 20.0f   // nearest-navmesh search radius for the snap target
// Fillet validation must check Z, not just X/Y. findNearestPoly's search box is 50yd tall, so a
// candidate can match a poly on an entirely different floor and pass a 1yd 2D test — the exact
// defect that got the wide bow rolled back on 2026-07-01, still live in the shipped fillet until now.
#define AIBOT_FILLET_VALIDATE_Z_YARDS   3.0f   // |matched poly Z - candidate Z| beyond this = reject the fillet, keep the raw run

// ── [TRACE] Per-bot movement trace (2026-07-20) ──
// Opt-in, per-bot, file-armed — see AiBotTraceIsArmed() in AiBotAIMovement.cpp. Off = zero cost
// beyond one string compare per dispatch. Two halves on ONE timeline: dispatch-time per-waypoint
// dz (the CAUSE — what Z we told the spline to fly, vs the floor) and a 4 Hz position sampler
// (the SYMPTOM — where the bot actually is, vs the floor).
#define AIBOT_TRACE_LIST_FILE      "aibot_trace.txt"  // cwd = run/bin. one bot name per line; "*" = whole fleet. re-read every AIBOT_TRACE_LIST_RELOAD_SEC.
#define AIBOT_TRACE_LIST_RELOAD_SEC 10                // arm/disarm live by editing the file — no restart, no deploy
#define AIBOT_TRACE_SAMPLE_MS       250               // position sampler cadence (1 Hz is 7yd of travel — far too coarse to see a fillet arc)
#define AIBOT_TRACE_FLOAT_YARDS     1.5f              // |pos.z - floor| at or above this = a float incident (open/close logged, not every sample)
#define AIBOT_TRACE_WAYPOINT_DZ     0.5f              // per-waypoint dz at or above this gets its own line; everything else rolls into the PATH summary


// Combat-stalemate breaker (in-combat, neither side dealing damage — navmesh seam / unreachable mob)
#define AIBOT_STALEMATE_NUDGE_MS      3000   // no-damage-in-combat time before each nudge
#define AIBOT_STALEMATE_MAX_NUDGES    3      // nudges before forced disengage
#define AIBOT_STALEMATE_NUDGE_DIST    6.0f   // hop distance
#define AIBOT_STALEMATE_NUDGE_HOLD_MS 2000   // let the hop run uninterrupted this long
#define AIBOT_STALEMATE_IGNORE_MS     60000  // ignore the unreachable guid this long after disengage
#define AIBOT_TAPPED_IGNORE_MS  8000   // after disengaging a foreign-tapped mob, ignore its guid this long (don't re-acquire)
// Stage-2 extraction (CombatStop-in-place doesn't free a still-in-range mob — flee, then teleport)
#define AIBOT_STALEMATE_MAX_DISENGAGES 3     // flee attempts before a hard teleport out
#define AIBOT_STALEMATE_FLEE_DIST      30.0f // real distance to flee out of the mob's reach/leash
#define AIBOT_STALEMATE_FLEE_HOLD_MS   2500  // let the flee hop run uninterrupted this long

// Overpull discipline (solo bots must not bum-rush dense packs / respawn fields)
#define AIBOT_OVERPULL_SOLO         3       // solo: > this many mobs on/around me => bail / don't pull
#define AIBOT_OVERPULL_GROUP        6       // grouped: the pack can hold a deeper pull
#define AIBOT_PULL_DENSITY_RADIUS   20.0f   // hostiles within this of a target = "what wakes when I pull it"
#define AIBOT_OVERPULL_RETREAT_DIST 30.0f   // how far to retreat away from the hostile centroid
#define AIBOT_OVERPULL_FLEE_HOLD_MS 2500    // let a retreat hop run uninterrupted this long
#define AIBOT_OVERPULL_MAX_FLEES    4       // retreats before we stop thrashing and let the fight resolve
#define AIBOT_HEARTH_SPELL_ID       8690    // Hearthstone — cast for the authentic stranded-escape port
#define AIBOT_NEWBIE_GRAVE_LEVEL    10      // <= this level: rez at the racial START if it is closer than
                                            // the stock graveyard (starter-valley graveyard-link gap fix)
#define AIBOT_GRIND_SCAN_YARDS   100.0f  // indefinite grind: bot-centric scan radius for the nearest valid XP mob
#define AIBOT_GRIND_HIGH_OFFSET  3       // …skip a mob more than this many levels above the bot (no red suicide)
#define AIBOT_GRIND_FREEZE_DWELL    3      // consecutive frozen grind ticks (~1s each) — over-cap veto OR no valid target — before handing back to C# (GRIND_BLOCKED). ~3s, fast vs the old 8-hold dwell.
// [PLAYERPARTY] Escort mode (2026-07-07) — a REAL player invited this bot to their party.
#define AIBOT_PARTY_FOLLOW_DIST      3.0f   // MoveFollow distance behind the boss (per-bot angle spread on top)
#define AIBOT_PARTY_CATCHUP_TELEPORT 100.0f // same-map gap beyond this = we were left behind (boss ported) → NearTeleportTo the boss
#define AIBOT_PARTY_INSTANCE_DWELL_MS 3000  // boss on ANOTHER map this long (and not taxi/transport) → TeleportTo him (instance-follow, symmetric both directions)
// Fight-initiation discipline + recovery hysteresis (2026-07-05, solo death reduction).
// The OLD eat gate was a one-way threshold: with an active task, DrinkAndEat was reachable
// only below 40% HP / 20% mana — so the loop RELEASED the bot back into the grind at
// 41%/21%, every time. A tasked bot lived permanently in the 40-60% band (the fight-losing
// band; any add = death), and a caster re-pulled at 21% mana as a melee caster. The latch
// (m_eatRecoveryLatch) turns the threshold into hysteresis: enter at the floor, hold until
// genuinely restored — the exact shape the C# rez heal proved (RezHealTarget 0.95/0.85).
// The PULL floor gates fight INITIATION only; defense at any HP is untouched.
#define AIBOT_EAT_ENTER_HP    40.0f   // dip below → recovery latch ON (the old one-way gate value)
#define AIBOT_EAT_ENTER_MANA  20.0f
#define AIBOT_EAT_EXIT_HP     90.0f   // latch releases only when BOTH exits are met (mana classes)
#define AIBOT_EAT_EXIT_MANA   85.0f
#define AIBOT_PULL_MIN_HP     70.0f   // never INITIATE a new pull below this (solo doctrine only)
#define AIBOT_PULL_MIN_MANA   50.0f

// [PULL] Proactive pull-and-retreat (FINDING_005 anti-overpull, 2026-08-06). A fresh grind
// engagement TAGS the mob then drags it back to open ground so only it (+ tight neighbours)
// follows, instead of body-diving the camp.
#define AIBOT_PULL_RETREAT_DIST     20.0f  // how far back toward open ground to drag a freshly-tagged mob
#define AIBOT_PULL_RETREAT_HOLD_MS  2000   // let the retreat hop run uninterrupted this long
#define AIBOT_PULL_MELEE_TAG_RANGE   6.0f  // melee: combat-dist within this = tagged → break off + retreat
#define AIBOT_PULL_RANGED_TAG_RANGE 26.0f  // ranged: within this = tagged (matches the ~25y caster chase)
#define AIBOT_PULL_ARRIVE_RANGE      9.0f  // back within this of the anchor = stop, turn and fight
#define AIBOT_PULL_MAX_MS           8000   // abandon the pull sequence after this (fall back to fight-in-place)

// [SPREAD] Anti-convergence (FINDING_005): keep independent solo bots from all dogpiling one camp.
#define AIBOT_SPREAD_RADIUS   30.0f  // count other bots within this of a candidate grind spot (~one camp)
#define AIBOT_SPREAD_BOT_CAP  4      // >= this many OTHER solo bots already here → defer + relocate (not join)

// SELLING STUFF THRESHOLD
#define AIBOT_SELL_KEEP_UPGRADE_LEVELS  5   // keep an unusable gear drop only if it becomes usable within this many levels (grow-into); else sell regardless of quality
#define AIBOT_CONSUMABLE_STALE_LEVELS  12   // a non-food consumable (potion/elixir/scroll/bandage) is "too weak" and sells once the bot outlevels its RequiredLevel by more than this; level-appropriate ones are kept (staged for future potion use). Food & drink always sells — bots have unlimited/conjured food.

// Spell / item ids referenced across the combat + self-maintenance methods.
// (Moved here from AiBotAI.cpp at the file split so every TU — combat, movement,
// loot — sees the same constants; combat uses AB_SPELL_AUTO_SHOT / AB_SPELL_SHOOT_WAND
// / the BG-flag auras, the self-maintenance helpers use the mount + food/drink ids.)
enum AiBotSpells
{
    AB_SPELL_FOOD = 1131,
    AB_SPELL_DRINK = 1137,
    AB_SPELL_AUTO_SHOT = 75,
    AB_SPELL_SHOOT_WAND = 5019,

    AB_SPELL_MOUNT_40_HUMAN = 470,
    AB_SPELL_MOUNT_40_NELF = 10787,
    AB_SPELL_MOUNT_40_DWARF = 6896,
    AB_SPELL_MOUNT_40_GNOME = 17456,
    AB_SPELL_MOUNT_40_TROLL = 10795,
    AB_SPELL_MOUNT_40_ORC = 581,
    AB_SPELL_MOUNT_40_TAUREN = 18363,
    AB_SPELL_MOUNT_40_UNDEAD = 8980,

    AB_SPELL_MOUNT_60_HUMAN = 22717,
    AB_SPELL_MOUNT_60_NELF = 22723,
    AB_SPELL_MOUNT_60_DWARF = 22720,
    AB_SPELL_MOUNT_60_GNOME = 22719,
    AB_SPELL_MOUNT_60_TROLL = 22721,
    AB_SPELL_MOUNT_60_ORC = 22724,
    AB_SPELL_MOUNT_60_TAUREN = 22718,
    AB_SPELL_MOUNT_60_UNDEAD = 22722,

    AB_SPELL_MOUNT_40_PALADIN = 13819,
    AB_SPELL_MOUNT_60_PALADIN = 23214,

    AB_SPELL_MOUNT_40_WARLOCK = 5784,
    AB_SPELL_MOUNT_60_WARLOCK = 23161,

    AB_ITEM_ARROW  = 2512,
    AB_ITEM_BULLET = 2516,

    // NOTE: the BG flag auras (AURA_WARSONG_FLAG / AURA_SILVERWING_FLAG) are intentionally
    // NOT declared here. BattleBotAI.h already defines them (enum FlagSpellsWS), and TUs that
    // include both this header and BattleBotAI.h (e.g. PlayerBotMgr.cpp) would get a redeclaration
    // conflict. They're referenced only by the combat method bodies, so they live file-local in
    // AiBotAICombat.cpp instead — see the anonymous enum at the top of that TU.
};

// Task types for the state machine
enum AiBotTask : uint8
{
    TASK_IDLE = 0,           // Wander, emote, look alive
    TASK_MOVE_TO,            // Pathfind to destination, report arrival
    TASK_QUEST_PICKUP,       // Walk to NPC, accept quest
    TASK_QUEST_KILL,         // Move to area, kill targets, track progress
    TASK_QUEST_GATHER,       // Move to area, interact with game objects
    TASK_QUEST_TURNIN,       // Walk to NPC, turn in quest
    TASK_FOLLOW_PLAYER,      // Grouped with real player — follow mode
    TASK_TRAVEL,             // Long-distance zone transition
    TASK_REST,               // Sit at inn, eat/drink, idle RP behavior
    TASK_GRIND,              // Area grind: patrol radius, kill creature_entry, count down
    TASK_TAXI,               // Flight path: teleport to source node, fly to destination
};

// Current task data (payload from bridge or default)
struct AiBotTaskData
{
    AiBotTask type       = TASK_IDLE;
    uint64 commandCbt    = 0;       // protocol-v4 owner of async terminal outcomes for this task
    bool isRtsCommand = false; // player-issued RTS order
    uint32 questId       = 0;
    float x              = 0.0f;
    float y              = 0.0f;
    float z              = 0.0f;
    float radius         = 0.0f;
    uint32 npcGuid       = 0;

    // TASK_GRIND fields
    uint32 creatureEntry = 0;      // 0 = kill anything hostile. The PRIMARY objective entry —
                                    // this alone still drives the dispatch coordinate (where the
                                    // bot walks / the grind-in-place center / the Held-objective
                                    // stamp for the 1c reconcile). Nothing about navigation changes.
    int32  killGoal      = 0;      // total kills needed (0 = indefinite)
    int32  killCount     = 0;      // kills so far

    // Alternate entries that satisfy the SAME objective as creatureEntry — for ITEM-DROP
    // objectives only, where multiple creature species genuinely tie on drop odds for the
    // required item (e.g. Young Wolf + Timber Wolf both drop Tough Wolf Meat at the same
    // chance). C# resolves the tie and ships the alternates here on MOVE_TO/SET_TASK
    // (alt_entry1/2/3). Deliberately NOT used for kill objectives — a kill quest names one
    // specific creature and the server only credits kills of that exact entry, so widening
    // match there would let the bot "believe" it's progressing on kills the server never
    // credits. 0 = unused slot. Always 0 unless C# explicitly ships a real tie.
    static const int MAX_ALT_ENTRIES = 3;
    uint32 altCreatureEntries[MAX_ALT_ENTRIES] = { 0, 0, 0 };

    // TASK_TAXI fields
    uint32 taxiSourceNode = 0;
    uint32 taxiDestNode   = 0;

    // True if `entry` is a valid kill for the CURRENT objective — creatureEntry itself, or
    // any non-zero altCreatureEntries slot. Used everywhere the old code checked
    // `entry == creatureEntry`: ScanApproachTarget's valid-kill union, SelectGrindTarget's
    // aggro match + objective ring-scan, and the kill-credit check in UpdateAI. entry==0 is
    // never a match (0 means "no entry / not a creature kill" at every call site).
    bool MatchesObjectiveEntry(uint32 entry) const
    {
        if (entry == 0)
            return false;   // cb:fold pure helper on task data, no bot in reach
        if (entry == creatureEntry)
            return true;   // cb:fold pure helper on task data, no bot in reach
        for (int i = 0; i < MAX_ALT_ENTRIES; ++i)
            if (altCreatureEntries[i] == entry)
                return true;   // cb:fold pure helper on task data, no bot in reach
        return false;
    }

    void Clear()
    {
        type = TASK_IDLE;
        commandCbt = 0;
        isRtsCommand = false;
        questId = 0;
        x = y = z = radius = 0.0f;
        npcGuid = 0;
        creatureEntry = 0;
        killGoal = 0;
        killCount = 0;
        for (int i = 0; i < MAX_ALT_ENTRIES; ++i)
            altCreatureEntries[i] = 0;
        taxiSourceNode = 0;
        taxiDestNode = 0;
    }
};

// A learned navmesh discontinuity (e.g. an overworld cave mouth where the
// interior mesh doesn't connect to the terrain at the portal). Recorded the
// first time this bot crosses it inbound (teleport), then reused to escape
// outbound. World geometry, but kept per-bot in memory for now (the durable,
// fleet-shared version persists these via a bridge event + C# table).
struct NavBoundary
{
    float innerX, innerY, innerZ;   // the off-mesh dest we crossed TO (inside)
    float outerX, outerY, outerZ;   // the connected-mesh point we crossed FROM (outside)
    uint32 mapId;
};

// ============================================================
// [TEAMPLAY] Per-member combat directive — the group focus-fire seam.
//
// The god-bot coordinator (C#) stamps one of these on each grouped member every brain tick
// via COMBAT_DIRECTIVE. It is STATE (the bot's job) — the stateless TeamPlay resolvers
// (AiBotAITeamPlay.cpp) only READ it; they never write it, never move the bot, never touch
// the wire. v1 carries ONE mode (assist) + the anchor's low GUID. Richer roles
// (interrupt / move-to-ally / focus) arrive as NEW WIRE KEYS when their behaviour ships —
// they are documented intent here, deliberately NOT declared as speculative dead fields
// (that is exactly the debt the C# cleanup just removed).
// ============================================================
enum AiBotCombatMode : uint8
{
    COMBAT_MODE_NONE   = 0,
    COMBAT_MODE_ASSIST = 1,
    // future: COMBAT_MODE_FOCUS, COMBAT_MODE_INTERRUPT, … (add WITH the behaviour, not before)
};

struct CombatDirective
{
    AiBotCombatMode mode = COMBAT_MODE_NONE;
    uint32 anchorGuidLow = 0;
    // RESERVED INTENT (do not declare as fields until the behaviour ships):
    //   role / focusGuidLow / interruptGuidLow / moveToAllyGuidLow
    bool IsActive() const { return mode != COMBAT_MODE_NONE; }
    void Clear()          { mode = COMBAT_MODE_NONE; anchorGuidLow = 0; }
};

// [TEAMPLAY] Sticky-assist safety valve (2026-07-02, closes the "anchor between kills"
// divergence — see AiBotAITeamPlay.cpp for the full trace). Caps how many consecutive
// ResolveCombatTarget calls a follower may keep assisting the anchor's LAST victim after
// the anchor's live GetVictim() goes null, before yielding back to solo selection. Not a
// wall-clock bound — the resolver has no tick-delta visibility, so this is a generous
// call-count instead, same spirit as the ms-counters elsewhere on this class. Tune once
// real resolver call cadence is visible in logs.
#define AIBOT_ASSIST_STICKY_MAX_TICKS 8

// [TEAMPLAY] Pull discipline (2026-07-02, B3 — closes the fresh-engagement SEEDING gap the
// sticky memo structurally cannot: sticky bridges gaps in an EXISTING assist, but when the
// group first arrives at a camp nobody is fighting, every follower's resolver yields, and
// each member solo-pulls its own nearest simultaneously — the arrival fan-out that made
// "each bot on its own mob" persist). A FOLLOWER whose assist directive is live but whose
// resolver has no opinion HOLDS its grind pull this many consecutive dispatch ticks (~1s
// each), letting the ANCHOR pull first so the resolver has a victim to hand everyone.
// Bounded so a distracted anchor (eating / wandering / stuck) can never starve the group:
// on expiry the follower falls through to a normal solo pull, and the counter re-arms the
// next time an assist actually resolves — a permanently idle anchor costs one dwell total,
// not one dwell per camp. Enforced at the DISPATCH site in UpdateAI, never inside
// SelectGrindTarget — so a hold can never increment m_grindFreezeStreak or fire a false
// GRIND_BLOCKED|no_target handback to C#.
#define AIBOT_ASSIST_PULL_HOLD_TICKS 5

#include "RaidPlanLaw.h"   // [RAID-PLAN] SuiRaidPlan member + parse (PLAN_19 M-C)

class AiBotAI : public CombatBotBaseAI
{
public:
    AiBotAI(uint8 race, uint8 cls, uint32 level, uint32 mapId, uint32 instanceId,
            float x, float y, float z, float o)
        : CombatBotBaseAI(),
          m_spawnRace(race), m_spawnClass(cls), m_spawnLevel(level),
          m_spawnMapId(mapId), m_spawnInstanceId(instanceId),
          m_spawnX(x), m_spawnY(y), m_spawnZ(z), m_spawnO(o),
          m_lastDispatchedDestX(0.0f), m_lastDispatchedDestY(0.0f), m_lastDispatchedDestZ(0.0f),
          m_hasDispatchedDest(false), m_pathJourneySeed(0),
          m_bridgeSocket(BRIDGE_INVALID_SOCKET)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] AiBotAI constructor fired (factory creation)");

        // [SUI] STAGGER THE FLEET, OR THE WHOLE FLEET THINKS ON THE SAME TICK.
        //
        // This was a flat Reset(2000), and the per-tick reset in UpdateAI is a flat
        // Reset(AIBOT_UPDATE_INTERVAL). Two constants, no jitter anywhere: every bot
        // constructed on the same map tick decremented by the same diff and expired on the
        // same tick, forever. Nothing ever made them diverge. A 300-bot fleet therefore ran
        // 300 brains on ONE tick per second and almost nothing on the other nine.
        //
        // That is what put Map::UpdatePlayers at 200-500ms against a 100ms MapUpdateInterval.
        // Measured 2026-09-01: 646 over-budget ticks on map 1 in a day, the players phase
        // ~95% of
        // every one, median fine with a heavy 300-600ms tail — the signature of a herd, not
        // of steady load. The client felt it as commands that would not register.
        //
        // Spreading the INITIAL phase across one interval spreads the cost across the ticks
        // in that interval (~10 at MapUpdateInterval 100). No bot's 1Hz rate changes, and the
        // per-tick Reset preserves whatever offset a bot starts with, so this only has to be
        // right once. The first tick still lands in [1s, 2s) as it always did.
        //
        // The 617 stride must stay COPRIME with AIBOT_UPDATE_INTERVAL: that is what makes the
        // sequence walk the whole range instead of clustering, so any fleet size spreads
        // evenly. Stride 1 would pack 300 bots into the first 300ms and fix nothing.
        // Atomic because bot construction is not guaranteed to be single-threaded.
        static std::atomic<uint32> s_spawnPhase{0};
        uint32 const phase =
            s_spawnPhase.fetch_add(617, std::memory_order_relaxed) % AIBOT_UPDATE_INTERVAL;
        m_updateTimer.Reset(AIBOT_UPDATE_INTERVAL + phase);
    }

    ~AiBotAI()
    {
        BridgeDisconnect();
    }

    // The brain socket is otherwise closed only by ~AiBotAI -- but a PlayerBot's AI is
    // NOT destroyed on logout: PlayerBotEntry::ai keeps owning it so the bot can be
    // re-added (".bot delete" parks the entry at PB_STATE_OFFLINE for ".bot add_all").
    // Without this override the fd, and the C# brain's view of a bot that is already
    // gone, both outlive the logout. Player::~Player -> RemoveAI() virtual-dispatches
    // here, so the socket closes on exactly the right edge, while PlayerBotAI::Remove()
    // still only detaches and never deletes. BridgeDisconnect() clears m_bridgeConnected,
    // so UpdateBridgeTick -> BridgeConnect() re-establishes the link on the next login.
    void Remove() override
    {
        BridgeDisconnect();
        PlayerBotAI::Remove();
    }

    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override;

    // Quick-spawn name override for the ".bot addai <class> [race] [name]" path.
    // When non-empty, OnSessionLoaded renames the freshly-created player to this
    // instead of the ObjectMgr auto-generated name. Empty = keep the generated
    // name (old behaviour). The persistent Load() path sets m_spawnName from the
    // playerbot.name column; this setter is for the on-the-fly command path.
    void SetSpawnName(std::string const& name) { m_spawnName = name; }

    // --- Lifecycle ---
    void OnPlayerLogin() override;
    void UpdateAI(uint32 const diff) override;
    void OnPacketReceived(WorldPacket const* packet) override;
    void MovementInform(uint32 MovementType, uint32 Data = 0) override;

    // --- SUI possession (SuiPossess.cpp) ---
    // While a real player drives this bot, every autonomous behaviour is
    // suspended: the rotation/movement sub-ticks, the 1 Hz behaviour body, and
    // all mutating bridge commands. The bridge connection itself stays alive so
    // the C# brain keeps receiving STATE (with possessed:1) and stands down.
    void SetPossessed(bool on);
    bool IsPossessed() const { return m_possessed; }
    void UpdateBridgeTick();   // bridge connect/recv/state/flush, shared by both tick paths

    // [SUI] RTS waypoint chain (Ctrl+RightClick in the free view). ORDER_MOVE_QUEUE
    // appends; arrival chains into the next leg; ORDER_MOVE / ORDER_STOP clear it.
    void SuiQueueWaypoint(float x, float y, float z);
    void SuiClearWaypoints()
    {
        m_suiWaypoints.clear();
        m_suiPatrolLoop = false;
        m_suiFormationFacing = -1000.f;
    }
    // [SUI] Void the whole journey — brain task, stored path, queued RTS waypoints,
    // and the RTS discipline latches. Called when a human takes the body (TryBegin)
    // and when a Solo-era errand must not survive joining a human's party
    // (RefreshDoctrine).
    void SuiAbandonJourney()
    {
        // A manual NPC approach is a separate bridge-owned motion. Taking the
        // body, joining a player party, conscripting, or any other whole-journey
        // teardown must retire that owner too; otherwise the OOC resume block
        // can restart an obsolete interaction after the newer intent finishes.
        CancelPendingNpcInteraction("abandon_journey");
        m_currentTask.Clear();
        ClearStoredPath();
        SuiClearWaypoints();
        m_suiRtsHold = false;
        m_suiSheathOverride = -1;
    }
    std::deque<std::array<float, 3>> m_suiWaypoints;
    bool m_suiPatrolLoop = false;   // arrival re-queues the popped waypoint (ORDER_PATROL)
    bool m_suiUnlinked = false;     // chain broken (ORDER_LINK): never formation-follows
    // [SUI-TAXI] Landed a flight without its human aboard (the human hopped to another
    // body mid-air): hold where it landed instead of catch-up teleporting back to the
    // party (owner 2026-09-03). Cleared when the party arrives within catch-up range,
    // when the human possesses it again, or by any RTS order task.
    bool m_suiLandedHold = false;
    // [SUI-TAXI] Who this bot followed last tick (and where it stood): a same-map gap
    // beyond catch-up range is a PORT only when the SAME boss jumped; a different boss
    // far away means the human hopped to a distant body — the rest stay (hold).
    ObjectGuid m_suiLastBossGuid;
    bool m_suiBossFlewAway = false; // the boss was on a taxi since we last stood near him
    // [CHAIN] Explicit anchor (ORDER_FOLLOW to ANY group member — owner 2026-09-03: "chain
    // 2 players and 2 others, not just main to main"). Empty = the group rules (the
    // human's driven body). FindEscortBoss honours it first.
    ObjectGuid m_suiChainAnchor;
    void SuiStopFollowForHold();    // end an active FOLLOW leg when a hold begins
    float m_suiLastBossX = 0.f, m_suiLastBossY = 0.f, m_suiLastBossZ = 0.f;
    // [SUI] RTS discipline. Any explicit RTS order stands the bot at attention:
    // the idle wander/stroll stays suppressed until the journey is abandoned
    // (doctrine change, possession). Formation orders also stamp a slot facing
    // taken on arrival, and ORDER_SHEATH overrides the auto-arm until combat.
    bool m_suiRtsHold = false;
    // [SUI] Manual primary (owner 2026-09-01, "primary should always be user controlled"): the
    // commander drives this unit's actions himself from the Command View. RTS orders still land;
    // every autonomous decision (rotation, assist, grind, self-care, doctrine) is suspended and
    // brain errands are fenced like a conscript's. ORDER_AUTO or the free view coming down clears it.
    bool m_suiManual = false;
    // [SUI] Commander orders are STRICT about the navmesh (owner, 2026-09-02): a destination must
    // be a mesh point with a complete path, or the reachable end of a partial one; no seam
    // crossing, nudging or ring-scanning toward a spot the commander mis-clicked (an exterior
    // wall, a roof). Returns false and tells the commander when there is no path at all.
    bool SuiValidateOrderDest(float& x, float& y, float& z);
    WorldSession* SuiCommanderSession() const;
    bool m_suiOrderedAttackPass = false;   // an explicit ATTACK order passing the manual gate
    float m_suiFormationFacing = -1000.f;   // > -100 = face this way on arrival
    int8 m_suiSheathOverride = -1;          // -1 none; else the SheathState to keep

    // [SUI] Fix B — arrival-callback interrupt guard. StopMoving() Clears the MotionMaster, which
    // finalizes the in-flight point generator SYNCHRONOUSLY; the base PointMovementGenerator::
    // Finalize then fires MovementInform unconditionally — a FALSE arrival at the destination we
    // are abandoning, which otherwise re-paths the stale dest or emits a phantom TASK_COMPLETE.
    // Raised only across the interrupt; a genuine spline completion still informs normally.
    bool m_suiSuppressArrival = false;

    // [SUI] Fix A — RTS move coalescing (<=1 per unit per world tick, latest destination wins).
    // A flood of CMSG_SUI_ORDER move packets drains whole each tick; executing each inline runs a
    // full navmesh pathfind per packet and thrashes the spline, hanging a single unit. ORDER_MOVE
    // now only stashes the newest destination here; ConsumePendingSuiRtsMove issues it once per
    // UpdateAI tick. Other order types (attack/hold/formation/waypoint) stay immediate.
    bool  m_suiPendingMove = false;
    float m_suiPendingMoveX = 0.f;
    float m_suiPendingMoveY = 0.f;
    float m_suiPendingMoveZ = 0.f;
    void QueueSuiRtsMove(float x, float y, float z)
    {
        m_suiPendingMove = true;
        m_suiPendingMoveX = x;
        m_suiPendingMoveY = y;
        m_suiPendingMoveZ = z;
    }
    void ConsumePendingSuiRtsMove();   // defined in AiBotAIMovement.cpp; called each UpdateAI tick

    // [SUI] Conscription: this bot is enlisted in a commander's RTS army
    // (ORDER_CONSCRIPT, sent when the client assigns it to a control group).
    // The brain planner stands down — STATE carries conscripted:1 and mutating
    // bridge lines are dropped — while combat AI, doctrine reactions and
    // explicit RTS orders keep working. Empty = free. Cleared by ORDER_DISMISS
    // and by the commander's logout; deliberately survives SuiAbandonJourney
    // (possessing your own soldier must not muster it out).
    ObjectGuid m_suiConscriptedBy;
    bool IsSuiConscripted() const { return !m_suiConscriptedBy.IsEmpty(); }
    // Commander-injected bridge lines (RTS orders synthesized by SuiPossess and
    // SuiQueueWaypoint) must pass the conscription fence that drops brain lines.
    bool m_suiCommanderLine = false;
    void SuiInjectCommandLine(char const* line)
    {
        m_suiCommanderLine = true;
        BridgeProcessLine(line);
        m_suiCommanderLine = false;
    }

    // The REAL character's autonomy while its human drives a bot or the free camera
    // (SuiPossess Attach/DetachUnattendedAI). The character runs this same fleet AI —
    // enrolls with the brain via the normal HELLO/STATE bridge flow, follows the party
    // boss (possessed-first pre-pass), assists — with the fabricated-bot login work
    // skipped: no heal/skill-max/reagents/channel-join mutations on a real character.
    // In-party behaviour is the gate, and the bridge SELL_ITEMS wall refuses live-client
    // sessions, so it can never vendor. Ticks through Player::Update's m_AI slot.
    static AiBotAI* AttachToRealCharacter(Player* owner);

    // --- Combat (from BattleBotAI) ---
    bool CommanderRaidCast(Unit* target, uint32 spellId);
    bool CommanderRaidMove(float x, float y, float z);
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Unit* pExcept = nullptr) const;
    bool DrinkAndEat();
    bool UseMount();
    uint32 GetMountSpellId() const;
    float GetMaxAggroDistanceForMap() const;
    bool CheckForUnreachableTarget();
    void StopMoving();

    // --- Combat stalemate breaker (Session: in-combat no-damage deadlock) ---
    // HandleCombatStalemate runs per-tick while in combat: detects a no-damage-either-way
    // deadlock (bot on a navmesh seam / mob unreachable across geometry), nudges to break
    // it, and after AIBOT_STALEMATE_MAX_NUDGES force-disengages + ignores the offending guid.
    // IsCombatIgnored is consulted by Select*Target so a freshly-disengaged mob isn't
    // instantly re-acquired from the hostile-ref list. Returns true when it acted this tick.
    bool HandleCombatStalemate();
    bool IsCombatIgnored(uint32 guidLow) const;

    // --- Overpull discipline (solo death-spiral fix) ---
    // OverpullGuard: solo gate consulted BEFORE engaging a grind target — true = the target
    //   sits in too dense a cluster to pull alone, so hold instead of diving in.
    // HandleOverpullRetreat: per-tick in-combat — if more than the cap are on us, AttackStop
    //   and retreat away from the hostile centroid; short-ignore the attackers so we don't
    //   instantly re-acquire them mid-flee. Returns true when it acted (caller returns).
    bool OverpullGuard(Unit* target) const;
    // Level/HP-aware solo overpull cap: a low-level bot (small HP pool) tolerates far fewer
    // simultaneous attackers than a settled one. Replaces the flat AIBOT_OVERPULL_SOLO at the
    // solo call sites so a L1 flees the 2nd add instead of standing in a 3-mob pile and dying.
    uint32 SoloOverpullCap() const;
    bool HandleOverpullRetreat();
    bool HandleHearthCast();   // [HEARTH] drive the stranded-escape Hearthstone cast -> town port
    // PullReady: fight-INITIATION readiness (solo). Gates only the start of a new pull at
    // the grind dispatch + approach scan; never gates defense or an in-progress fight.
    bool PullReady() const;
    // [PULL] Proactive pull-and-retreat (FINDING_005). BeginPull tags the target then arms the
    // retreat-to-anchor; HandlePullRetreat drives it each in-combat tick and resumes normal
    // combat once the mob has been dragged clear of the camp.
    bool BeginPull(Unit* pTarget);
    bool HandlePullRetreat();

    // --- [TEAMPLAY] public seams for the stateless group-combat resolvers ---
    // TeamPlay (AiBotAITeamPlay.cpp) reads this AI but mutates nothing; these two accessors
    // (+ the public m_combatDirective, beside m_currentTask) are the ONLY surfaces it needs,
    // so it never reaches into protected combat internals. GetBotPlayer keeps the seam
    // base-access-agnostic (works whether or not the base exposes `me` publicly).
    Player* GetBotPlayer() const { return me; }
    bool    IsValidAssistTarget(Unit* pTarget) const;

    // [DOCTRINE] Re-resolve + swap m_doctrine for the current combat state (§2.2). Called once
    // per behaviour tick at the top of UpdateAI, before any acquisition/combat decision.
    void    RefreshDoctrine();

    // --- [PLAYERPARTY] Escort mode (2026-07-07) — a REAL player leads this bot's group ---
    // FindPartyBoss: the human this bot escorts — the group LEADER if it is a real (non-bot)
    // session, else the first real-player member; nullptr when the group has no real player
    // (or no group). THE detection primitive: ResolveDoctrine keys PlayerParty on it, the
    // STATE producer echoes it to C# (pparty), the Form/Disband guards refuse to touch a
    // player-led party, and the escort hook follows it.
    // DoPartyFollow: the escort spine (UpdateAI OOC hook) — engage the doctrine's party
    // focus if one resolves, else keep formation on the boss (MoveFollow with a per-bot
    // angle spread; same-map NearTeleportTo catch-up when left > AIBOT_PARTY_CATCHUP_TELEPORT
    // behind). Movement stays spine-owned — the doctrine only ever names targets.
    Player* FindPartyBoss() const;
    // FindEscortBoss: the human THIS bot keeps formation on (2026-07-08, multi-human split).
    // With ONE real player it is FindPartyBoss; with several, each bot deterministically picks
    // reals[GUIDLow % count] over the group's real players — the Group member list is a single
    // server-side object, so every bot iterates the identical order and the fleet splits
    // ~evenly across the humans with zero coordination and zero tick-to-tick flapping.
    // FindPartyBoss stays the DETECTION primitive (doctrine / pparty / invite / guards);
    // this one is only the FORMATION target (DoPartyFollow + the ppdist STATE echo).
    Player* FindEscortBoss() const;
    // True for the body a human left behind (AttachToRealCharacter). Such an AI is kept off
    // the brain bridge entirely by the theft wall, so any "stand down" that travels as a
    // STATE echo is invisible to it — the non-autonomy decree has to be enforced locally.
    bool IsUnattendedRealCharacter() const { return m_ownedDummyEntry != nullptr; }
    // [COMPANION] Any REAL account's character running this AI: the enrolled
    // unattended own character OR a summoned companion. Every fabricated-bot
    // mutation (spec/gear/skill repairs, whisper flag) and the brain bridge
    // gate on this, not on m_ownedDummyEntry alone.
    bool IsRealCharacter() const
    {
        return m_ownedDummyEntry != nullptr || (botEntry && botEntry->ownerAccountId != 0);
    }
    // [COMPANION] The ONE human this unit answers to, or nullptr for a shared
    // fleet bot: a companion -> its owner, an unattended own character -> its
    // own session's character (me), a conscript -> its conscriptor.
    Player* SuiBoundHuman() const;
    // The body a human is playing right now: the bot they possess, else themselves.
    static Player* SuiDrivenBodyOf(Player* human);
    // Set by SuiCompanion::Summon; consumed by TickArrival on the first map tick.
    ObjectGuid m_suiCompanionOwner;
    bool m_suiCompanionArrival = false;
    // [COMPANION] Death rule (owner 2026-09-02): the party gets first call on a
    // resurrection (healer out of combat, druid Rebirth in combat); failing that
    // the corpse "runs itself": the ghost waits at the body for as long as the
    // graveyard run would have taken (never less than the reclaim delay), then
    // pops in place at 50% once the party is out of combat.
    uint32 m_suiCompanionDeadMs = 0;
    uint32 m_suiCompanionSelfRezAtMs = 0;
    // Fleet-wide ally resurrection: healers rez a dead party member out of
    // combat; a druid Rebirths one in combat. True when a cast was started.
    bool SuiTryResurrectAlly();
    void    DoPartyFollow();

    // --- 18 pure virtual combat method overrides (verbatim from BattleBotAI) ---
    void UpdateInCombatAI() override;

    // --- [ROTATION] Custom rotation slate (2026-07-16, RotationSlate design 2026-05-11) ---
    // A C#-authored, priority-sorted instruction list pushed via LOAD_ROTATION. When present
    // it OWNS in-combat casting: UpdateInCombatAI runs the slate instead of the class switch,
    // and a 250ms sub-tick in UpdateAI evaluates it between behaviour ticks (the 1 Hz loop —
    // and its wand-autorepeat early-return, which freezes a wanding vanilla bot's spell
    // evaluation entirely — no longer caps ability throughput). Empty slate = vanilla class
    // AI, bit-for-bit: the OG behaviour is just the else branch. OOC class AI (buffs, food,
    // post-fight healing) is untouched either way.
    bool UpdateRotationSlate();                 // walk the slate, first castable match wins; true = slate present (combat handled)
    Unit* ResolveRotationTarget(uint8 kind);    // 0=SELF 1=CURRENT_TARGET 2=LOWEST_HP_PARTY
    uint8 GetCombatSpecTab() const override;
    CombatBotRoles GetCombatActiveRole() const;
    bool HasUsableSpecCombat() const;
    bool HasFastCombatPolicy() const;
    bool UpdateSpecCombatAI();
    bool UpdateSpecOutOfCombatAI();
    bool TrySpecSpell(Unit* target, uint32 firstRankSpellId);
    bool TrySpecSpell(Unit* target, SpellEntry const* spell);
    bool TrySpecAura(Unit* target, uint32 firstRankSpellId);
    bool TrySpecStackingAura(Unit* target, uint32 firstRankSpellId);
    bool HasAuraFromSpellChain(Unit const* target, uint32 firstRankSpellId) const;
    bool CanUseSpecAoE(Unit* center, float radius, uint32 minimumTargets = 2) const;
    Unit* SelectSafeSpecAdd(Unit const* primary) const;
    bool TrySpecInterrupt(Unit* target, std::initializer_list<uint32> spellIds);
    bool TrySpecTaunt(Unit* target);
    bool CommandSpecPet(Unit* target, bool mendHunterPet);
    bool UpdateSpecCombatWarrior(uint8 spec);
    bool UpdateSpecCombatPaladin(uint8 spec);
    bool UpdateSpecCombatHunter(uint8 spec);
    bool UpdateSpecCombatRogue(uint8 spec);
    bool UpdateSpecCombatPriest(uint8 spec);
    bool UpdateSpecCombatShaman(uint8 spec);
    bool UpdateSpecCombatMage(uint8 spec);
    bool UpdateSpecCombatWarlock(uint8 spec);
    bool UpdateSpecCombatDruid(uint8 spec);
    bool UpdateSpecOutOfCombat(uint8 playerClass, uint8 spec);
    void UpdateOutOfCombatAI() override;
    void UpdateInCombatAI_Paladin() override;
    void UpdateOutOfCombatAI_Paladin() override;
    void UpdateInCombatAI_Shaman() override;
    void UpdateOutOfCombatAI_Shaman() override;
    void UpdateInCombatAI_Hunter() override;
    void UpdateOutOfCombatAI_Hunter() override;
    void UpdateInCombatAI_Mage() override;
    void UpdateOutOfCombatAI_Mage() override;
    void UpdateInCombatAI_Priest() override;
    void UpdateOutOfCombatAI_Priest() override;
    void UpdateInCombatAI_Warlock() override;
    void UpdateOutOfCombatAI_Warlock() override;
    void UpdateInCombatAI_Warrior() override;
    void UpdateOutOfCombatAI_Warrior() override;
    void UpdateInCombatAI_Rogue() override;
    void UpdateOutOfCombatAI_Rogue() override;
    void UpdateInCombatAI_Druid() override;
    void UpdateOutOfCombatAI_Druid() override;

    // --- Phase 1 idle wander ---
    void DoRandomWander();

    // [SUI] Party spacing (owner 2026-08-28): an idle party bot standing inside
    // a fellow group member deliberately sidesteps to open ground. Returns true
    // when it issued the sidestep and owns the tick.
    bool DoPartyUnstack();

    // --- Auto-loot after kill ---
    void DoAutoLoot(ObjectGuid creatureGuid);
    void TryAutoEquipBags();
    void TryAutoEquip();
    float ScoreItem(ItemPrototype const* proto, uint8 slot) const;

    // --- TASK_GRIND: area grind behavior ---
    void BridgeHandleSetTask(const char* json);
    void BridgeHandleCombatDirective(const char* json);   // [TEAMPLAY] group focus-fire stamp
    void BridgeHandleResetCombatStuck(const char* json);  // protocol-v6 bounded combat-latch teardown
    void DoGrindPatrol();
   Unit* SelectGrindTarget(Unit* pExcept = nullptr) const;

    // --- §4 objective approach: enrich MOVE_TO → hand off to GRIND in place ---
    // ScanApproachTarget scans for m_currentTask.creatureEntry around the BOT (no
    // center-radius filter — the center is the deep loader coord). ConvertMoveToGrindInPlace
    // re-centers the grind on the bot's current position so SelectGrindTarget's center
    // filter passes for the local cluster, then leaves the grind to count kills + complete.
    Unit* ScanApproachTarget();
    void  ConvertMoveToGrindInPlace();

    // --- Path safety validation (Session 15) ---
    bool IsPathSafe(float destX, float destY, float destZ,
                    float &unsafeX, float &unsafeY, float &unsafeZ,
                    uint32 &dangerLevel);

    // --- Chunked pathing (Session 25) ---
    // Advances m_pathIndex forward through m_pathWaypoints by up to
    // AIBOT_PATH_CHUNK_DIST yards and issues MovePoint to that waypoint.
    // Returns true if a MovePoint was issued, false if path is complete.
    bool StartNextPathChunk();
    void ClearStoredPath();

    // --- TCP Bridge (Phase 2) ---
    void BridgeConnect();
    void BridgeDisconnect();
    void BridgeSend(const char* json);   // queues a complete JSON line (never truncates)
    void CircuitFlush();                 // [CIRCUIT] ship buffered probes (AiBotCircuit.cpp, 1 Hz)
    void BridgeFlush();                  // drains m_bridgeSendBuf; tolerates partial / EWOULDBLOCK writes
    void BridgeSendHello();
    void BridgeSendState();
    void BridgeSendEvent(const char* eventType, const char* data);
    void BridgeSendEvent(const char* eventType, const char* data, uint64 commandCbt);
    bool CancelPendingNpcInteraction(const char* source);
    void BridgeRecv();
    void BridgeProcessLine(const char* line);
    void BridgeHandleMoveTo(const char* json);
    void BridgeHandleTeleport(const char* json);   // generic live-bot teleport (assist + future hearth)
    void MoveToDestination(float destX, float destY, float destZ, bool stopCurrentMovement = true);
    bool FindNearestNavmeshPoint(float& outX, float& outY, float& outZ, float searchYards) const;
    bool FindNearestNavmeshPointNear(float queryX, float queryY, float queryZ, float& outX, float& outY, float& outZ, float searchYards) const;
    bool IsStartIsolated();   // [FINDING_020] 8-point probe: can the bot path anywhere from where it stands? (tags MOVE_FAILED start_isolated)
    void TeleportToWalkable(float x, float y, float z, float o, const char* tag);   // [FINDING_020] port to a coord, ring-nudge onto walkable floor if it's in a wall
    PointsArray SmoothPathCorners(PointsArray const& raw);
    bool ValidateWideBowCandidate(Vector3 const& anchorStart, Vector3 const& anchorEnd, PointsArray& candidate);
    // [GROUND] Snap a teleport/snap destination Z DOWN onto real terrain (the float-maroon
    // fix). Called before EVERY NearTeleportTo so a bot can't be planted on a floating
    // navmesh poly (Recast lays the walkable surface ABOVE the collision hull on steep
    // slopes). Snap-down-only so it can never shove a bot up into geometry.
    // maxDrop: refuse (and log) a downward snap larger than this. 0.0f = unlimited, which is the
    // pre-2026-07-20 behaviour every existing teleport call site keeps by omission.
    void ReGroundZ(float x, float y, float& z, const char* tag = nullptr, float maxDrop = 0.0f);
    // [GROUND] Re-ground EVERY point in a path before it reaches the spline. THE fly fix:
    // MovebyPath flies the array verbatim, and raw Detour points carry the navmesh poly surface,
    // which Recast lays ABOVE the collision hull. Before this, only accepted fillet candidates
    // were ever grounded — every straight-run waypoint (the overwhelming majority) went out raw.
    void GroundPathPoints(PointsArray& pts, const char* tag);
    // [TRACE] 4 Hz position sampler — the symptom half of the movement trace. Call once per
    // behaviour tick from UpdateAI with the same diff; it self-throttles to AIBOT_TRACE_SAMPLE_MS.
    void UpdateMovementTrace(uint32 diff);
    // [FINDING_019] Per-tick travel-stuck watchdog -- snaps a physically-wedged bot to navmesh.
    void UpdateTravelStuckWatchdog();
    // Pin run speed on a MovePoint so the spline never takes MoveSplineInit's velocity==0
    // inflated-spline fallback (~52yd/s glide + the model floating off slopes). The two travel
    // splines pin this already; every OTHER mover routes through here so none can forget.
    bool MovePointRun(uint32 pointId, float x, float y, float z, bool emitAutonomousRefusal = true);
    // [REMESH] "I am off the mesh" vs "the dest is unreachable": when the START has no polygon under it,
    // every calculate() is NOPATH regardless of dest. Walk a short straight step back onto the nearest
    // navmesh point instead of refusing (the refusal is what pinned bots in crevices for a day). Returns
    // true when a step was issued (caller retries its real hop next tick), false when the start is on
    // mesh (dest really is the problem) or the mesh is too far to walk to.
    bool TryRemeshStep(const char* why);
    // Navmesh-seam crossing (cave entry/exit). RecordNavBoundary stores the seam the
    // first time we teleport across inbound; FindNavBoundaryNear lets the outbound
    // leg find the recorded outside anchor to escape back to the connected mesh.
    void RecordNavBoundary(float innerX, float innerY, float innerZ);
    NavBoundary const* FindNavBoundaryNear(float x, float y, float scope) const;
    void BridgeHandleSayText(const char* json);
    void BridgeHandleQuestInteract(const char* json);
    void BridgeHandleAbandonQuest(const char* json);
    void BridgeHandleLearnSpell(const char* json);
    void BridgeHandleAttackTarget(const char* json);
    void BridgeHandleInteractNpc(const char* json);
    void BridgeHandleTakeFlight(const char* json);
    void BridgeHandleSellItems(const char* json);
    void BridgeHandleResurrect(const char* json);
    void BridgeHandleTrain(const char* json);
    void BridgeHandleUseGameObject(const char* json);
    void BridgeHandleQuestCast(const char* json);   // [CLASS-QUEST] cast a quest spell on a target creature
    void BridgeHandleFormGroup(const char* json);
    void BridgeHandleDisbandGroup(const char* json);
    void BridgeHandleSetEscort(const char* json);   // [FOLLOW-CMD] "{bot} follow {player}" — sets/clears m_escortOverrideName
    void BridgeHandleLoadRotation(const char* json); // [ROTATION] load/replace/clear the custom slate (pipe payload, resolves SpellEntry at load)
    void BridgeHandleApplyCombatLoadout(const char* json); // correlated, core-authoritative talent/profile/role/rotation mutation
    void BridgeHandleLoadRaidPlan(const char* json); // [RAID-PLAN] adopt this bot's raid-plan slice (PLAN_19 M-C; executes at M-D)
    void BridgeHandleCircuitTrace(const char* json); // [CIRCUIT] mode/ship toggle from C# (CIRCUIT_BOARD.md R6)
    void BridgeHandleRepairItems(const char* json);
    char const* GetTalentProfileStateName() const;
    char const* GetEffectiveRotationSource() const;
    char const* GetEffectiveRotationProfile() const;

    // --- Quest/combat/event helpers ---
    // `confirmed` is corpse proof. False means the creature had already left
    // the map, so the brain can measure inferred kills separately.
    void SendKillEvent(uint32 creatureEntry, uint32 creatureGuidLow, bool confirmed = true);
    void SendQuestUpdateEvent(uint32 questId, const char* status);
    void SendLevelUpEvent(uint32 newLevel);
    // C0 (§5.1): senderGuidLow = GUID low of the sending player (0 when non-player/unresolvable).
    // C# uses it for roster lookup (is-bot); the NAME remains the memory key (D3).
    void SendChatRecvEvent(const char* senderName, const char* message, const char* chatType, const char* channelName = nullptr, uint32 senderGuidLow = 0);

    // --- Task executor ---
    AiBotTaskData m_currentTask;

    // [TEAMPLAY] Live group combat stamp (read by TeamPlay::ResolveCombatTarget).
    // Public like the rest of this AI's state; stamped by BridgeHandleCombatDirective,
    // cleared on a 'none'/absent directive. Inactive (IsActive()==false) on every solo bot,
    // so the combat seam is a provable no-op unless the coordinator stamps it.
    CombatDirective m_combatDirective;

    // [RAID-PLAN] read seams + act entry points (PLAN_19 M-D). The adopted plan
    // itself lives with the rotation slate below; the doctrine resolver and the
    // EncounterPlay doctrine read through these.
    bool HasRaidPlan() const { return m_hasRaidPlan; }
    SuiRaidPlan const& GetRaidPlan() const { return m_raidPlan; }
    bool RaidPlanOnAddDuty() const;
    Unit* RaidPlanPreferredAdd();
    Unit* RaidPlanFindBoss();
    void UpdateRaidPlanTick();
    void RaidPlanMaintainAuras();
    void RaidPlanFormation();

    // [DOCTRINE] The active engagement doctrine (Layer D) — Solo / TeamAuto / Directed. One
    // instance owned by the bot, re-resolved each behaviour tick by RefreshDoctrine() and
    // swapped only when the kind changes. ALL group-fight transient state — the sticky-assist
    // memo and the B3 pull-hold counter that used to live here as members — now lives ON the
    // TeamAuto instance (AiBotDoctrineTeam.cpp), so a mode swap resets it by construction. The
    // in-combat target authority, OOC acquisition, and pull discipline all route through it.
    std::unique_ptr<IEngagementDoctrine> m_doctrine;
    DoctrineKind m_doctrineKind = DoctrineKind::Solo;

    // --- Victim Tracking ---
    uint32 m_lastVictimEntry = 0;
    uint32 m_lastVictimGuidLow = 0;

    // --- Loot state ---
    int32  m_lootTimer = 0;           // Countdown to auto-loot after kill (humanization delay)
    ObjectGuid m_lootTargetGuid;      // GUID of creature to loot
    uint32 ChooseQuestReward(Quest const* pQuest) const; // helper for choosing quest reward
    // Largest count the bot's ACTIVE quests still ask for of this item (0 = none). Only ReqItem
    // gather slots count; source items given on accept are excluded, so they are never surplus.
    // Defined in AiBotAILoot.cpp; used by the loot cap there and the sell surplus-trim in Bridge.
    uint32 QuestRequiredCountFor(uint32 itemId) const;

    // --- State ---
    ShortTimeTracker m_updateTimer;
    // [FINDING_019] travel-stuck watchdog reference (real-position stagnation, not the gameable
    // progress clock). See UpdateTravelStuckWatchdog / AIBOT_TRAVEL_STUCK_*.
    float  m_travelRefX = 0.0f;
    float  m_travelRefY = 0.0f;
    uint32 m_travelRefMs = 0;
    ShortTimeTracker m_rotationSubTick;   // [ROTATION] 250ms in-combat slate cadence (see AIBOT_ROTATION_SUBTICK_MS)
    bool m_reportedArmsWeaponMismatch = false;
    bool m_wasDead = false;
    uint32 m_lastCombatMs = 0;        // last in-combat tick; anchors the OOC form-drop grace
    bool m_loggedFirstUpdate = false;
    bool m_freshSpawn = false;
    bool m_possessed = false;         // SUI possession: autonomous behaviour suspended
    // Set by AttachToRealCharacter: inert PlayerBotEntry (never registered with
    // PlayerBotMgr) absorbing the base class's requestRemoval writes.
    std::unique_ptr<PlayerBotEntry> m_ownedDummyEntry;
    uint32 m_wanderTimer = 0;
    uint32 m_unstackTimer = 0;         // [SUI] party-spacing sidestep throttle
    uint32 m_suiFollowDiagTimer = 0;   // [SUI-DIAG] throttle for the party-follow decision log
    uint32 m_lastKnownLevel = 0;
    uint32 m_trackedQuestId = 0;

    // Spawn params
    uint8 m_spawnRace;
    uint8 m_spawnClass;
    uint32 m_spawnLevel;
    uint32 m_spawnMapId;
    uint32 m_spawnInstanceId;
    uint32 CountNearbyHostiles(Unit* pCandidate, float radius) const;
    uint32 CountNearbyBots(Unit* pCenter, float radius) const;   // [SPREAD] anti-convergence (FINDING_005)
    float m_spawnX, m_spawnY, m_spawnZ, m_spawnO;
    std::string m_spawnName;  // Persisted name from playerbot table (empty = generate random)

    // A far TeleportTo completes asynchronously. PORT_HOME can validate same-map landings
    // immediately, but a cross-continent grind-hub jump must wait until the worldport completes
    // before TeleportToWalkable can inspect the destination map's navmesh.
    bool   m_pendingWalkableLanding = false;
    uint32 m_pendingWalkableMap = 0;
    float  m_pendingWalkableX = 0.0f;
    float  m_pendingWalkableY = 0.0f;
    float  m_pendingWalkableZ = 0.0f;

    // --- Chunked pathing state (Session 25) ---
    // Stored mmap waypoints from PathInfo::calculate().
    // MovePoint is called to waypoints[m_pathIndex] in ~200yd chunks.
    // m_pathIndex is the NEXT waypoint to walk toward.
    std::vector<Vector3> m_pathWaypoints;
    uint32 m_pathIndex = 0;
    // A task replacement raises this before the old generator is cleared. It also remains raised
    // when combat defers that Clear, so a late command-A callback cannot read command B's task/cbt.
    // StopMoving releases it only after the synchronous MotionMaster::Clear finalizer returns.
    bool m_suppressTaskDestInform = false;
    uint32 m_lastMovePointRefusedMs = 0;
    uint32 m_lastRemeshStepMs = 0;   // [REMESH] rate limit for TryRemeshStep

    // INTERACT_NPC owns a distinct point id and outcome correlation. It may temporarily interrupt
    // an autonomous task, but its arrival can never masquerade as that task's TASK_COMPLETE.
    ObjectGuid m_pendingInteractNpcGuid;
    uint64 m_pendingInteractNpcCbt = 0;

    // [FINDING_017] Continuation progress gate: tracks distance-to-dest across
    // leg-exhausted continuations of one TASK_MOVE_TO dest. Re-armed when the
    // task dest changes; 3 consecutive continuations gaining <5yd → MOVE_FAILED
    // no_path instead of re-issuing forever (0-yd-partial livelock).
    float  m_contDestX = 0.0f;
    float  m_contDestY = 0.0f;
    float  m_contLastDist = -1.0f;
    uint32 m_contNoProgress = 0;

    // §4 approach-scan throttle: an enriched (objective) MOVE_TO scans for its target
    // mob every ~2-3s while walking. Reset to 0 by BridgeHandleMoveTo so the first scan
    // fires on the journey's first tick. Decremented in UpdateAI alongside m_wanderTimer.
    uint32 m_approachScanTimer = 0;


    // --- Combat stalemate breaker state ---
    // No-damage-either-way in-combat deadlock detection + escape. m_stalemateMs accumulates
    // while the deadlock holds; a nudge fires every AIBOT_STALEMATE_NUDGE_MS, and after
    // AIBOT_STALEMATE_MAX_NUDGES the bot force-disengages and ignores the guid for
    // AIBOT_STALEMATE_IGNORE_MS so Select*Target doesn't re-acquire it from the hostile-ref list.
    uint32 m_stalemateMs       = 0;   // accumulated no-damage-in-combat time
    uint32 m_stalemateHoldMs   = 0;   // mid-nudge hold (suppresses re-chase while the hop runs)
    uint8  m_stalemateNudges   = 0;   // nudges issued this stalemate
    uint8  m_stalemateDisengages = 0; // Stage-2 flee attempts before the hard-teleport escape
    uint32 m_lastHealth        = 0;   // bot hp last tick (detect damage taken)
    uint32 m_lastVictimHealth  = 0;   // victim hp last tick (detect damage dealt)
    ObjectGuid m_stalemateVictim;     // anchored stalemate target guid
    std::map<uint32, uint32> m_combatIgnore;  // victim guidLow -> ms remaining ignored

    // Protocol-v6 combat-still handshake. The reset preserves the planner task,
    // but parks autonomous reacquisition long enough for its correlated ACK and
    // a post-reset STATE to reach C# before the task can pull again.
    uint32 m_recoveryResetHoldMs = 0;

    // --- Overpull retreat state ---
    uint32 m_overpullFleeHoldMs = 0;   // mid-retreat hold (suppress re-chase while the hop runs)
    uint8  m_overpullFlees      = 0;   // retreats issued this combat (cap at AIBOT_OVERPULL_MAX_FLEES)
    uint32 m_lastAttackerCount  = 0;   // peak melee attackers this combat — stamped on the DEATH event

    // --- [HEARTH] Authentic stranded-escape hearth (PORT_HOME) state ---
    // Cast Hearthstone (8690) for the real cast bar + animation + built-in damage/move interrupt,
    // with the bot's homebind temporarily pointed at the escape town so the spell's own teleport
    // lands there. HandleHearthCast restores the homebind + re-anchors spawn on completion, or
    // aborts (bot stays; the C# brain re-issues) on interrupt.
    bool   m_hearthActive       = false;
    uint32 m_hearthElapsedMs    = 0;
    float  m_hearthX = 0.0f, m_hearthY = 0.0f, m_hearthZ = 0.0f;
    int    m_hearthMap          = -1;
    float  m_hearthSavedHomeX = 0.0f, m_hearthSavedHomeY = 0.0f, m_hearthSavedHomeZ = 0.0f;
    uint32 m_hearthSavedHomeMap = 0;
    uint16 m_hearthSavedArea    = 0;

    // --- [PULL] Proactive pull-and-retreat state (FINDING_005) ---
    // A fresh grind engagement TAGS the target then drags it to m_pullAnchor so only it (+ tight
    // neighbours) follows, instead of body-diving the camp. Solo + TeamAuto anchor both route
    // through BeginPull (the shared TASK_GRIND engage); followers never self-acquire so never pull.
    bool       m_pullActive       = false;   // a proactive pull is in progress
    bool       m_pullTagged       = false;   // target aggroed onto us → retreat phase
    ObjectGuid m_pullTargetGuid;             // the mob being pulled
    float      m_pullAnchorX = 0.0f, m_pullAnchorY = 0.0f, m_pullAnchorZ = 0.0f;  // retreat-to spot
    uint32     m_pullRetreatHoldMs = 0;      // mid-retreat hold (suppress re-chase while the hop runs)
    uint32     m_pullElapsedMs     = 0;      // safety-timeout accumulator (AIBOT_PULL_MAX_MS)
    // --- Recovery hysteresis latch (2026-07-05) ---
    // Set when HP/mana dips below AIBOT_EAT_ENTER_* OR when a pull is refused
    // under-resourced (PullReady false at an engage site); held until
    // AIBOT_EAT_EXIT_HP/_MANA. While set, the OOC eat block runs every tick (tasked or
    // not) and the solo grind dispatch never initiates. Survives combat interrupts —
    // the food aura breaks on aggro, the latch doesn't — so a mid-eat defense fight
    // resumes eating afterward instead of chaining into the next pull at 45%.
    bool m_eatRecoveryLatch = false;
    // --- Grind freeze-escape (livelock fix, generalized) ---
    // Consecutive TASK_GRIND ticks that produced no pull — EITHER OverpullGuard vetoed every
    // candidate (field denser than the solo cap) OR SelectGrindTarget found nothing valid (all
    // killed/tapped/grey/red, or empty — the "nothing matches" freeze under a crowd of bots).
    // At AIBOT_GRIND_FREEZE_DWELL we emit GRIND_BLOCKED so C# breaks the freeze with a single
    // unstick kill, then re-issues the objective. Reset to 0 on any successful pull.
    uint32 m_grindFreezeStreak = 0;

    // [SPREAD] Anti-convergence latch (FINDING_005): true while we're deferring a solo filler grind
    // out of an over-crowded camp. Only used to log the defer ONCE per episode (not every tick).
    bool m_spreadDeferred = false;

    // --- [PLAYERPARTY] Instance-follow dwell (2026-07-08) ---
    // Accumulates while the escort boss is on a DIFFERENT map (and not taxi-flying / on a
    // transport); at AIBOT_PARTY_INSTANCE_DWELL_MS DoPartyFollow TeleportTo's him — into his
    // instance OR back out, symmetrically. Reset on same-map / taxi / transport / no boss, so
    // a portal in-out or a boat ride can't thrash the fleet through loading screens.
    // TeleportTo skips areatrigger level checks — under-leveled companions WILL enter; the
    // only gate is the human. Instance-copy binding = verify-first-run (group binding should
    // land members in the boss's copy; if a parallel copy spawns, pass GetInstanceId()).
    uint32 m_bossOffMapMs = 0;

    // [FOLLOW-CMD] Escort override (2026-07-16): "{bot} follow {player}" in party chat.
    // Stored LOWERCASED by BridgeHandleSetEscort; empty = no override (the GUIDLow-modulo
    // auto split applies). FindEscortBoss prefers the named REAL player when he is present
    // in the group; if he isn't (offline / left / typo), it silently falls back to the auto
    // split — self-healing, never a stuck bot. Session-lifetime state, deliberately not
    // persisted: a fresh login starts on the auto split.
    std::string m_escortOverrideName;

    // --- [ROTATION] The loaded slate ---
    // Instructions arrive pre-sorted by priority (C# sorts; C++ preserves order). pSpell is
    // resolved once at load (sSpellMgr + me->HasSpell) so the 4 Hz tick never string-parses
    // or table-scans. Cleared by LOAD_ROTATION with empty data; survives goal changes and
    // party joins/leaves by design — the slate is per-BOT kit, not per-doctrine state.
    struct RotationInstruction
    {
        uint32 spellId = 0;
        SpellEntry const* pSpell = nullptr;   // resolved at load; null = unknown/unlearned (skipped, reported on the ack)
        uint8  target = 1;                    // 0=SELF 1=CURRENT_TARGET 2=LOWEST_HP_PARTY
        int    hpMin = 0;                     // resolved target's health % window, inclusive
        int    hpMax = 100;
        uint32 auraId = 0;                    // 0 = no aura condition
        bool   auraPresent = false;           // auraId != 0: cast only if target HAS (true) / LACKS (false) it
    };
    std::vector<RotationInstruction> m_rotation;
    std::string m_rotationProfile;            // observability: echoed in logs/acks
    uint32 m_rotationInstructionCount = 0;    // source entries, including rejected legacy LOAD_ROTATION rows
    uint32 m_rotationCastableCount = 0;       // entries installed after strict spell resolution
    uint32 m_combatConfigRevision = 0;        // live-session optimistic concurrency token
    std::string m_lastCombatLoadoutRequest;   // idempotency: duplicate request gets the original ACK
    std::string m_lastCombatLoadoutAck;

    // [RAID-PLAN] this bot's adopted slice of the raid plan (PLAN_19 M-C).
    // Stored by BridgeHandleLoadRaidPlan; the EncounterPlay doctrine and the
    // formation movement generator (M-D) are the consumers.
    SuiRaidPlan m_raidPlan;
    bool m_hasRaidPlan = false;
    ShortTimeTracker m_raidPlanSubTick;             // [RAID-PLAN] 500ms act cadence (M-D)
    std::map<uint32, uint32> m_raidPlanAuraReady;   // [RAID-PLAN] aura spellId -> ready-at ms
    ObjectGuid m_raidPlanChaseGuid;                 // [RAID-PLAN] last formation chase issued
    float m_raidPlanChaseAngle = -999.f;
    float m_raidPlanChaseDist = -1.f;


    // --- Graveyard self-rez state (race-free port→rez; no C# roundtrip) ---
    // We teleport the ghost, then resurrect it OURSELVES once the teleport has actually
    // landed — so the bot can never rez back in the death pocket (the old loop). While
    // m_pendingGraveyardRez is set, inbound RESURRECT commands are ignored (a stray plain
    // rez from C# can't rez us mid-teleport). m_graveRezWaitMs is a safety timeout: if the
    // teleport never confirms, rez anyway rather than ghost-stick forever.
    bool   m_pendingGraveyardRez = false;
    float  m_graveRezX = 0.0f, m_graveRezY = 0.0f, m_graveRezZ = 0.0f;
    uint32 m_graveRezMap = 0;
    uint32 m_graveRezWaitMs = 0;

    // --- Navmesh-seam crossing state (this bot's learned cave portals) ---
    // Overworld caves whose interior mesh doesn't stitch to terrain at the portal
    // make pathing the last few yards return NOPATH forever. We teleport across on
    // entry (CheckForUnreachableTarget already does this for the combat chase case)
    // and record the seam so the exit leg can teleport back to the connected mesh.
    // Per-bot, reset on reconnect; the durable/fleet-shared version persists these.
    std::vector<NavBoundary> m_navBoundaries;   // learned at runtime
    bool m_didBoundaryExit = false;             // one outbound seam-cross per MOVE_TO journey
    bool m_didNavmeshSnap = false;   // one off-mesh-start snap attempt per MOVE_TO journey
    float m_lastDispatchedDestX, m_lastDispatchedDestY, m_lastDispatchedDestZ;
    bool m_hasDispatchedDest;
    uint32 m_pathJourneySeed;

    // --- [TRACE] Movement trace state (2026-07-20) ---
    // m_traceSampleMs accumulates diff until AIBOT_TRACE_SAMPLE_MS. m_traceFloating latches an
    // open float incident so the log carries FLOAT-OPEN / FLOAT-CLOSE pairs (with duration and
    // peak dz) instead of a 4 Hz firehose of identical samples.
    uint32 m_traceSampleMs   = 0;
    bool   m_traceFloating   = false;
    uint32 m_traceFloatMs    = 0;
    float  m_traceFloatPeak  = 0.0f;

    // --- Bridge state ---
    BridgeSocket m_bridgeSocket;
    bool m_bridgeConnected = false;
    bool m_bridgeHelloSent = false;
    uint64 m_bridgeDispatchCbt = 0;      // exact cbt of the command handler currently on this stack
    uint32 m_bridgeStateTimer = 0;
    uint32 m_bridgeReconnectTimer = 0;
    uint32 m_bridgeReconnectDelay = BRIDGE_RECONNECT_BASE;
    char m_bridgeRecvBuf[BRIDGE_RECV_BUF_SIZE];
    int m_bridgeRecvLen = 0;

    // Outbound queue (Session 36). BridgeSend appends complete JSON lines here;
    // BridgeFlush drains it. Holds only whole lines, so a partial socket write
    // can never truncate a message on the wire. Per-bot, single-threaded with
    // the rest of this bot's AI — no locking needed.
    std::string m_bridgeSendBuf;
};

#endif
