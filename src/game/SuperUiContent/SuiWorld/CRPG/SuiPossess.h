/*
 * SuperUI CRPG/RTS possession core.
 *
 * A real client session takes direct control of a party AiBot: the session's
 * mover is re-pointed at the bot (stock mover machinery — MSG_MOVE_* then
 * drive the bot), the bot's autonomous AI is suspended, and owner-only data is
 * proxied back to the possessor (M3). Deliberately NOT built on the charm
 * aura machinery: ModPossess is mind-control semantics (faction swap, threat
 * wipe) which is wrong for driving your own party bot mid-fight. Only the
 * ordering law is borrowed: Camera::SetView BEFORE SetMover BEFORE
 * SetClientControl (see Unit::ModPossess).
 *
 * State model (no global registry):
 *   - possessor session:  WorldSession::m_suiControlledGuid
 *   - bot:                Unit::m_possessorGuid (SetPossessorGuid) — also
 *                         denies a second possessor and routes
 *                         GetConfirmedMover/UpdateControl correctly
 *   - bot AI:             AiBotAI::m_possessed (suspends behaviour + bridge
 *                         commands; bridge STATE keeps flowing with
 *                         possessed:1 so the C# brain stands down)
 */

#ifndef MANGOS_SUI_POSSESS_H
#define MANGOS_SUI_POSSESS_H

#include "Common.h"
#include "ObjectGuid.h"

#include <vector>

class Player;
class Unit;
class WorldSession;
class WorldPacket;
class Group;

namespace SuiPossess
{
    // SMSG_SUI_CONTROL_ACK result codes. 0 = granted; 1..15 denials answer a
    // CMSG_SUI_CONTROL_REQUEST; 16+ are releases — solicited or forced — and
    // must be accepted by the client in ANY control state.
    enum AckResult : uint8
    {
        ACK_OK                  = 0,
        DENY_NOT_FOUND          = 1,   // target missing / not a player / not visible / other map
        DENY_NOT_BOT            = 2,   // target is a real player or has no AiBotAI
        DENY_NOT_IN_GROUP       = 3,   // requester and target not in the same group
        DENY_BUSY               = 4,   // already possessed by someone
        DENY_TARGET_STATE       = 5,   // dead / taxi / transport / teleporting
        DENY_REQUESTER_STATE    = 6,   // requester is a bot, not self-mover, dead, on taxi…
        ACK_RELOCATING          = 7,   // outdoor transfer accepted; retry after ordinary streaming
        DENY_CROSS_INSTANCE     = 8,   // faction control never enters a foreign instance

        RELEASED                = 16,  // voluntary, back to own character
        RELEASED_FREECAM        = 17,  // voluntary, own character stays autonomous
        RELEASED_DEATH          = 18,  // possessed bot died
        RELEASED_TELEPORT       = 19,  // either side got teleported / changed map
        RELEASED_GROUP          = 20,  // group membership between the pair broke
        RELEASED_LOGOUT         = 21,  // possessor logging out, or the bot despawned
    };

    enum ReleaseMode : uint8
    {
        RELEASE_TO_SELF     = 0,
        RELEASE_TO_FREECAM  = 1,
    };

    enum OrderType : uint8              // CMSG_SUI_ORDER (M6)
    {
        ORDER_MOVE   = 0,
        ORDER_ATTACK = 1,
        ORDER_STOP   = 2,
        ORDER_MOVE_QUEUE = 3,   // append a waypoint; arrival chains the next leg
        ORDER_PATROL = 4,       // loop the queued waypoints until MOVE/STOP clears them
        ORDER_FOLLOW = 5,       // targetGuid = group member to escort; empty = auto split
        ORDER_LINK = 6,         // x >= 0.5 links the member into the chain; else unlinks
        // 7 is reserved: the Windows client already emits ORDER_AUTO_GROUP = 7
        // from its control-group palette; implementing it is separate work.
        ORDER_FORMATION_LINE   = 8,   // standing army: ranks of five facing the commander
        ORDER_FORMATION_CIRCLE = 9,   // evenly spaced ring around the anchor, facing out
        ORDER_SHEATH           = 10,  // x >= 0.5 draws weapons; else sheathes until combat
        ORDER_CONSCRIPT        = 11,  // enlist: the brain planner stands down for these bots
        ORDER_DISMISS          = 12,  // muster out: the brain resumes questing in place
        ORDER_MANUAL           = 13,  // the commander drives this unit's actions himself
        ORDER_AUTO             = 14,  // hand the unit's actions back to its AI
        ORDER_PREPARE          = 15,  // raid-ready from own spellbook and bags: buffs, elixirs, potion, food, pet (x = expected school mask)
        ORDER_SUPPLY           = 16,  // quartermaster: top bags up to the per-role policy loadout (x = encounter role 1-5, 0 = derive)
    };

    // SMSG_SUI_CONTROL_ROSTER member flags
    enum RosterFlags : uint8
    {
        ROSTER_CONTROLLABLE = 0x01,    // AiBot in your group you may possess
        ROSTER_POSSESSED    = 0x02,    // currently driven by a real player
        ROSTER_CONSCRIPTED  = 0x04,    // enlisted in someone's RTS army (brain off)
        ROSTER_COMPANION    = 0x08,    // one of YOUR summoned alts (owner sees this; others see 0)
    };

    // Chain state per roster row (owner 2026-09-03: "green or red chain, and WHO it's
    // chained to"). Server truth: the client draws exactly this.
    enum ChainState : uint8
    {
        CHAIN_LINKED     = 0,   // follows its anchor
        CHAIN_UNLINKED   = 1,   // broken by the human (ORDER_LINK 0): holds until re-linked
        CHAIN_WORLD_HOLD = 2,   // left behind by the world (landed alone, human hopped far,
                                // boss flew off): clears when the anchor is back in range
    };

    /// Attempt possession. Sends the ACK (grant or deny) itself.
    void HandleRequest(WorldSession* session, ObjectGuid targetGuid);

    /// Voluntary release (client asked). Safe to call when nothing is possessed.
    void HandleRelease(WorldSession* session, uint8 mode);

    // CMSG_SUI_CAM: reposition the freecam eye so grid/visibility streaming
    // follows the free camera instead of the abandoned body. No-op outside
    // the free view.
    void HandleCam(WorldSession* session, float x, float y, float z, bool active);

    // CMSG_SUI_ZONE_INTEL: the commander map census. Answers ONLY the asker with
    // per-zone bots/players counts plus the live positions of its own forces
    // (self + group). Client-driven polling; no server-side broadcast timer.
    void HandleZoneIntel(WorldSession* session, uint8 flags);

    /// RTS order from the free camera: move/attack/stop for the group's AiBots
    /// (explicit subject list, or empty = every controllable bot). Reuses the
    /// bridge command paths so ordered movement/attack behaves exactly like a
    /// brain-issued command.
    void HandleOrder(WorldSession* session, uint8 orderType,
        std::vector<ObjectGuid> const& subjects, ObjectGuid targetGuid,
        float x, float y, float z);

    /// Forced release with a reason code; no-op when the session possesses nothing.
    /// Server-initiated paths open the movement drain window (m_moveRejectTime) so
    /// in-flight bot-coordinate MSG_MOVE_* are not attributed to the distant own char.
    void ForceRelease(WorldSession* session, AckResult reason);

    // ── Hooks (called from core seams) ────────────────────────────────────────
    void OnPlayerRemovedFromGroup(Player* player);   // Group::RemoveMember / Disband
    /// Player::TeleportTo (either side of the pair). farTeleport=false is a same-map near
    /// teleport, which the possessed BOT survives (the possessor's client adopts it through the
    /// mirrored MSG_MOVE_TELEPORT_ACK); every other case breaks the pair.
    void OnPlayerTeleport(Player* player, bool farTeleport = true);
    void OnPlayerDeath(Player* player);              // Player::SetDeathState(JUST_DIED)
    void OnLogout(WorldSession* session);            // WorldSession::LogoutPlayer

    // ── Queries ───────────────────────────────────────────────────────────────
    /// The real player driving `bot` via SUI possession, or nullptr.
    Player* GetPossessor(Unit const* bot);
    // True only for the active possessor of this unit's owner/charmer.
    bool IsControlledOwnerOf(Unit const* unit, Player const* observer);
    // True when this bot is possessed AND its possessor is commanding from the free view
    // (evidenced by a live freecam eye). Such a bot takes RTS orders and executes them under
    // its own AI: the client that "owns" it is parked on a camera and will never move it.
    bool IsCommandedFromFreeView(Unit const* bot);
    /// True while this player's session has the free view up (live freecam eye). The
    /// driving client is parked on the camera then, so server-side facing help applies
    /// to whatever unit its casts act through (commanded bot or unattended own char).
    bool IsFreeViewUp(Player* player);
    /// Exact identity test for the invisible, server-created streaming helper
    /// registered to a live SUI free view.  Do not infer this from creature
    /// entry 15384: ordinary world triggers remain gameplay Units.
    bool IsFreecamEye(Unit const* unit);
    /// The bot this session is driving, or nullptr.
    Player* GetControlledBot(WorldSession const* session);
    bool IsSuiPossessed(Unit const* unit);
    /// FlightPathMovementGenerator::Finalize, final landing: a bot that flew without
    /// its human aboard holds where it landed (AiBotAI::m_suiLandedHold).
    void OnTaxiLanded(Player* player);
    // Re-push the driven bot's bags/facts to the commander after an inventory edit (ItemHandler).
    void ResnapshotControlled(WorldSession* session);

    // Cross-map faction control must retire the source-map free-camera eye
    // before Player::TeleportTo starts ordinary NEW_WORLD streaming.  The
    // return value records whether a failed transfer should restore that view.
    bool PrepareForRelocation(Player* player);
    void RestoreAfterFailedRelocation(Player* player, bool restoreFreeView);

    // ── Roster ────────────────────────────────────────────────────────────────
    /// Push SMSG_SUI_CONTROL_ROSTER to one real player (their current group view).
    void SendRoster(Player* realPlayer);
    /// Push the roster to every real-session member of a group. A member-facts
    /// era roster edge also re-pushes every party AiBot's bags + known spells
    /// to each real SUI member (party = full facts, faction = orders).
    void BroadcastRoster(Group* group);
    /// A chain edge (hold set/cleared, link toggled): re-push the roster so every SUI
    /// client in the group redraws the chain from server truth.
    void NotifyChainChanged(Player* member);

    // ── Party member facts (owner decision 2026-08-25) ────────────────────────
    /// CMSG_SUI_MEMBER_FACTS: push the inventory snapshot + known spells of
    /// party/raid AiBot members to the asking real player, no possession
    /// required. Empty subjects = every AiBot in the requester's group.
    /// Rate-limited per session, independent of movement. Faction-control
    /// authority is deliberately NOT sufficient — same-group membership is.
    void HandleMemberFacts(WorldSession* session,
        std::vector<ObjectGuid> const& subjects);

    // ── Party quest facts (PLAN_20 P1) ────────────────────────────────────────
    /// CMSG_SUI_QUEST_FACTS: push the quest logs of party/raid members to the
    /// asking real player. Empty subjects = the whole group AND the requester's
    /// own character — the latter is the only way a client can see quests it
    /// holds past the twenty update-field slots. Rate-limited per session,
    /// separately from the bag/spell pull.
    void HandleQuestFacts(WorldSession* session, uint8 flags,
        std::vector<ObjectGuid> const& subjects);

    // ── Party quest acts (PLAN_20 P3) ─────────────────────────────────────────
    /// SMSG_SUI_PARTY_QUEST_RESULT codes. Fine-grained on purpose: a party act
    /// must be able to say WHICH member was refused and WHY.
    enum PartyQuestResult : uint8
    {
        PARTY_QUEST_OK               = 0,
        PARTY_QUEST_DENIED           = 1,   // not on the party line / no authority
        PARTY_QUEST_REQUIREMENTS     = 2,   // level, prerequisites, race, class
        PARTY_QUEST_LOG_FULL         = 3,   // Quests.MaxHeld reached
        PARTY_QUEST_NO_QUEST         = 4,   // giver does not offer/end it, or not in their log
        PARTY_QUEST_TOO_FAR          = 5,   // outside share range, or cannot interact
        PARTY_QUEST_BAD_REWARD       = 6,   // reward index outside the quest's choices
        PARTY_QUEST_CANNOT_REWARD    = 7,   // bags full, not complete, already rewarded
        PARTY_QUEST_ALREADY_HELD     = 8,   // accept: already in their log (benign)
        PARTY_QUEST_ALREADY_REWARDED = 9,   // accept: already turned in
        PARTY_QUEST_NEEDS_CHOICE     = 10,  // turn-in: "auto" asked for, nobody to choose
        PARTY_QUEST_CANNOT_ABANDON   = 11,  // quest start items cannot be un-equipped
    };

    /// One subject of a party quest act. Deliberately a local POD rather than the
    /// wire packet's nested type: this header sees only Common.h and ObjectGuid.h,
    /// and HandleMemberFacts/HandleQuestFacts already keep the packet types on the
    /// session-shim side of the boundary.
    struct PartyQuestSubject
    {
        ObjectGuid guid;
        uint8 rewardChoice = 0;   // 255 = let the server choose
    };

    /// CMSG_SUI_PARTY_QUEST: act on a quest for an explicit set of party members.
    /// Every subject is authorized and answered individually.
    /// PLAN_20 P4a: take group leadership back from a companion bot, so a
    /// commander in a bot-led group can rearrange or break up their own party.
    void HandlePartyLead(WorldSession* session, uint8 action, ObjectGuid subject);

    /// PLAN_20 P5: what every party member would see over these questgivers'
    /// heads, so the world marker can wear an honest "(4)".
    void HandleGiverStatus(WorldSession* session,
        std::vector<ObjectGuid> const& givers);

    /// PLAN_20 Model B: per (quest, member) eligibility verdict at one giver, so the
    /// commander-view quest window draws each member's card. Ordered by how much
    /// needs doing so the client can colour without re-deriving.
    enum GiverQuestVerdict : uint8
    {
        GIVER_QUEST_CAN_TAKE       = 0,   // eligible to accept right now
        GIVER_QUEST_ON_IT          = 1,   // held, still working it
        GIVER_QUEST_READY          = 2,   // held and complete -> can turn in here
        GIVER_QUEST_DONE           = 3,   // already rewarded (non-repeatable)
        GIVER_QUEST_NEEDS_PREREQ   = 4,   // previous/chain/breadcrumb quest first
        GIVER_QUEST_LOW_LEVEL      = 5,
        GIVER_QUEST_WRONG_RACE_CLASS = 6,
        GIVER_QUEST_LOW_SKILL_REP  = 7,   // skill, reputation or condition
        GIVER_QUEST_LOG_FULL       = 8,   // Quests.MaxHeld reached
        GIVER_QUEST_CANT           = 9,   // ineligible for another reason
    };

    /// CMSG_SUI_GIVER_QUESTS: enumerate one giver's offered/ended quests and answer
    /// a verdict for every party member (self included), no possession required.
    void HandleGiverQuests(WorldSession* session, ObjectGuid giver);

    void HandlePartyQuest(WorldSession* session, uint8 action, uint32 questId,
        ObjectGuid npcGuid, std::vector<PartyQuestSubject> const& subjects);

    // SMSG_SUI_MEMBER_ITEM_MOVE_RESULT codes.
    enum MemberItemMoveResult : uint8
    {
        ITEM_MOVE_OK           = 0,
        ITEM_MOVE_DENIED       = 1,   // party line / authority / endpoints invalid
        ITEM_MOVE_NO_ITEM      = 2,   // nothing at that bag/slot any more
        ITEM_MOVE_TARGET_FULL  = 3,   // receiver cannot store it
        ITEM_MOVE_UNAVAILABLE  = 4,   // different map, or a live trade window
        ITEM_MOVE_REFUSED_ITEM = 5,   // conjured etc.
    };

    /// CMSG_SUI_MEMBER_ITEM_MOVE (Phase C v1, owner 2026-08-25): move one bag
    /// item between two party endpoints — the requester's own character or a
    /// party AiBot member — instantly, no trade window. Binding deliberately
    /// does NOT gate the move (this is the CRPG party's shared backpack, not
    /// the auction house); conjured items are refused. Both endpoints
    /// re-snapshot to every real SUI member of the group afterwards.
    void HandleMemberItemMove(WorldSession* session, ObjectGuid from,
        ObjectGuid to, uint8 bag, uint8 slot,
        bool inPlace = false, uint8 destBag = 0, uint8 destSlot = 0);

    // ── Owner-data mirror (M3) ────────────────────────────────────────────────
    /// Wrap whitelisted owner-only packets of a possessed bot's socket-less
    /// session into SMSG_SUI_PROXY toward the possessor. Called from
    /// WorldSession::SendPacket; no-op unless possessed and whitelisted.
    void MirrorOwnerPacket(WorldSession* botSession, WorldPacket const* packet);
}

#endif
