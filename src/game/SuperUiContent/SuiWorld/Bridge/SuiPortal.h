/*
 * Server-authoritative preparation/readiness handshake for MSUIClient's
 * rendered mage portals. The stock CMSG_GAMEOBJ_USE path remains untouched.
 */

#ifndef MANGOS_SUI_PORTAL_H
#define MANGOS_SUI_PORTAL_H

#include "Common.h"

class GameObject;
class WorldPacket;

namespace SuiPortal
{
    // Optional trailer on SMSG_SUI_CONTROL_ACK. A current MSUIClient sends a
    // zero-guid request through that old, backwards-compatible opcode before it
    // ever emits a portal opcode; old cores simply deny it with no trailer.
    constexpr uint32 CAPABILITIES_MAGIC = 0x31495553; // "SUI1" little-endian
    constexpr uint32 CAPABILITY_REAL_PORTALS_V1 = 1u << 0;
    constexpr uint32 CAPABILITY_PORTAL_CAST_PREWARM_V1 = 1u << 1;
    // Faction bot control groups (census + non-party possession + RTS orders)
    // are served by this core. Baseline SuperUI — advertised unconditionally.
    constexpr uint32 CAPABILITY_FACTION_CONTROL_GROUPS_V1 = 1u << 2;
    // Party member facts: party/raid AiBot inventory snapshots + known spells
    // pushed without possession (owner rule: party = full facts, faction =
    // orders). Advertised unconditionally; the client never pulls without it.
    constexpr uint32 CAPABILITY_PARTY_MEMBER_FACTS_V1 = 1u << 3;
    // Phase C v1: instant item moves between party members (the CRPG shared
    // backpack). Both endpoints re-snapshot after every accepted move.
    constexpr uint32 CAPABILITY_PARTY_ITEM_MOVE_V1 = 1u << 4;
    // PLAN_20 P1: party/raid quest logs pushed without possession, and the
    // requester's own quests held past the twenty update-field slots. Both ride
    // SMSG_SUI_QUEST_LOG — the subject guid says which.
    constexpr uint32 CAPABILITY_PARTY_QUEST_FACTS_V1 = 1u << 5;
    // PLAN_20 P3: accept / turn in / abandon on behalf of party members, with the
    // reward chosen per member. Also the id-addressed abandon that the vanilla
    // slot-indexed CMSG_QUESTLOG_REMOVE_QUEST cannot express.
    constexpr uint32 CAPABILITY_PARTY_QUEST_ACTS_V1 = 1u << 6;
    // Companions v1: CMSG/SMSG_SUI_COMPANION (summon/dismiss/list the owner's
    // own alts) and roster flag 0x08. The client must not send the opcode
    // until this bit is observed.
    constexpr uint32 CAPABILITY_COMPANIONS_V1 = 1u << 7;
    // Bit 7 is reserved for PLAN_20 P4 (party-vendor-v1) and left unclaimed even
    // though P5 shipped first.
    // PLAN_20 P5: per-member questgiver dialog status, so a world marker can wear
    // an honest "(4)". The client cannot compute this -- eligibility needs level,
    // prerequisites, race, class and exclusive groups it never receives for a
    // companion, and it is never told which quests an NPC offers at all.
    constexpr uint32 CAPABILITY_PARTY_GIVER_STATUS_V1 = 1u << 8;
    // PLAN_20 P4a: take group leadership back from a companion bot. Only ever
    // from a BOT -- taking it from a real player would be a griefing verb.
    constexpr uint32 CAPABILITY_PARTY_LEAD_V1 = 1u << 9;
    // PLAN_20 Model B (commander-view questing): a questgiver's offered/ended
    // quests plus a per-member eligibility verdict, so the free-view quest window
    // can draw each member's card without possession or the main at the NPC.
    constexpr uint32 CAPABILITY_PARTY_GIVER_QUESTS_V1 = 1u << 10;
    // Party flight (owner 2026-09-03): CMSG_SUI_PARTY_TAXI / SMSG_SUI_PARTY_TAXI_RESULT.
    // The Command View flies the whole commanded party from a flight master; the
    // client must not send the opcode until this bit is observed.
    constexpr uint32 CAPABILITY_PARTY_TAXI_V1 = 1u << 11;
    // Localized server-authoritative Command View freeze and five-action FIFO.
    constexpr uint32 CAPABILITY_COMMANDER_RAID_V1 = 1u << 13;
    constexpr uint32 CAPABILITY_TACTICAL_FREEZE_V1 = 1u << 12;

    // Append the backwards-compatible capability trailer and, when all six
    // server-authored destinations are available, the fixed-row cast-prewarm
    // catalog consumed before a summoned portal GameObject has a GUID.
    void WriteCapabilityTrailer(WorldPacket& data);

    // Summoned Mage portals need to reach an SUI client before the ordinary
    // continent visibility radius so its per-player 150-yard warm can begin.
    // This changes streaming visibility only; stock interaction range remains.
    void ConfigureSummonedPortalVisibility(GameObject* portal, uint32 summonSpellId);

    enum DescriptorResult : uint8
    {
        DESCRIPTOR_OK = 0,
        DESCRIPTOR_DENIED = 1,
        DESCRIPTOR_UNSUPPORTED = 2,
        DESCRIPTOR_EXPIRED = 3,
        DESCRIPTOR_FAILED = 4,
    };

    enum LoadResult : uint8
    {
        LOAD_READY = 0,
        LOAD_FAILED = 1,
    };

    enum PortalState : uint8
    {
        STATE_READY = 0,
        STATE_REVOKED = 1,
        STATE_BLOCKED = 2,
        STATE_ENTERING = 3,
        STATE_EXPIRED = 4,
        STATE_FAILED = 5,
    };

    enum StateReason : uint8
    {
        REASON_NONE = 0,
        REASON_BAD_PACKET = 1,
        REASON_UNSUPPORTED_VERSION = 2,
        REASON_CORRELATION = 3,
        REASON_LEASE_EXPIRED = 4,
        REASON_OBJECT_UNAVAILABLE = 5,
        REASON_NOT_AUTHORIZED = 6,
        REASON_CLIENT_LOAD_FAILED = 7,
    };
}

#endif
