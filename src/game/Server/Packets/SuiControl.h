/*
 * SuperUI CRPG/RTS control extension — client packets.
 *
 * These opcodes exist only between MSUIClient and this fork (see
 * docs/SUI_WIRE_PROTOCOL.md). Payloads are little-endian and fixed-layout;
 * guids travel as raw uint64 (never packed).
 */

#ifndef MANGOSSERVER_PACKETS_SUI_CONTROL_H
#define MANGOSSERVER_PACKETS_SUI_CONTROL_H

#include "Packet.h"
#include "ObjectGuid.h"

#include <vector>

namespace WorldPackets
{
    namespace SuiControl
    {
        class ControlRequest final : public ClientPacket
        {
        public:
            ObjectGuid targetGuid;

            explicit ControlRequest() : ClientPacket(CMSG_SUI_CONTROL_REQUEST) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> targetGuid;
            }
        };

        class ControlRelease final : public ClientPacket
        {
        public:
            uint8 mode = 0;             // SuiPossess::ReleaseMode

            explicit ControlRelease() : ClientPacket(CMSG_SUI_CONTROL_RELEASE) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> mode;
            }
        };

        class Order final : public ClientPacket
        {
        public:
            uint8 orderType = 0;        // SuiPossess::OrderType
            std::vector<ObjectGuid> subjects;   // empty = every controllable bot in the group
            ObjectGuid targetGuid;      // attack orders
            float x = 0, y = 0, z = 0;  // move orders

            explicit Order() : ClientPacket(CMSG_SUI_ORDER) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> orderType;
                uint8 count = 0;
                recv_data >> count;
                subjects.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> subjects[i];
                recv_data >> targetGuid;
                recv_data >> x >> y >> z;
            }
        };

        class Cam final : public ClientPacket
        {
        public:
            float x = 0, y = 0, z = 0;  // free-camera position (raw WoW map coords)
            // Is the free view UP? The client cannot leave it silently: the server keys the
            // streaming eye and the commanded-remotely waiver off this. Optional on the wire so
            // a sender predating the flag still reads as "up", which is what it meant.
            uint8 active = 1;

            explicit Cam() : ClientPacket(CMSG_SUI_CAM) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> x >> y >> z;
                if (recv_data.rpos() < recv_data.size())
                    recv_data >> active;
            }
        };

        class ZoneIntel final : public ClientPacket
        {
        public:
            uint8 flags = 0;   // reserved; an empty body is legal (older sender)

            explicit ZoneIntel() : ClientPacket(CMSG_SUI_ZONE_INTEL) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.rpos() < recv_data.size())
                    recv_data >> flags;
            }
        };

        class MemberFacts final : public ClientPacket
        {
        public:
            uint8 flags = 0;                    // reserved
            std::vector<ObjectGuid> subjects;   // empty = every party/raid AiBot member
            bool exactSize = false;             // wire discipline: reject sloppy lengths

            explicit MemberFacts() : ClientPacket(CMSG_SUI_MEMBER_FACTS) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() < 2)
                {
                    recv_data.rfinish();
                    return;
                }
                uint8 count = 0;
                recv_data >> flags >> count;
                if (recv_data.size() != 2u + 8u * count)
                {
                    recv_data.rfinish();
                    return;
                }
                exactSize = true;
                subjects.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> subjects[i];
            }
        };

        class QuestFacts final : public ClientPacket
        {
        public:
            // 0x01 opts into absolute Unix quest deadlines in response entries.
            uint8 flags = 0;
            // empty = the whole group AND the requester's own character; the
            // latter is how a client learns about quests it holds past the
            // twenty update-field slots.
            std::vector<ObjectGuid> subjects;
            bool exactSize = false;             // wire discipline: reject sloppy lengths

            explicit QuestFacts() : ClientPacket(CMSG_SUI_QUEST_FACTS) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() < 2)
                {
                    recv_data.rfinish();
                    return;
                }
                uint8 count = 0;
                recv_data >> flags >> count;
                if (recv_data.size() != 2u + 8u * count)
                {
                    recv_data.rfinish();
                    return;
                }
                exactSize = true;
                subjects.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> subjects[i];
            }
        };

        /// PLAN_20 P3: accept / turn in / abandon a quest for an EXPLICIT set of
        /// party members. There is deliberately no whole-party shorthand -- who
        /// acts must always be visible to the player who ordered it.
        class PartyQuest final : public ClientPacket
        {
        public:
            struct Subject
            {
                ObjectGuid guid;
                uint8 rewardChoice = 0;   // 255 = let the server choose
            };

            uint8 action = 0;             // 1 accept, 2 turn-in, 3 abandon
            uint32 questId = 0;
            ObjectGuid npcGuid;           // questgiver; empty for abandon
            std::vector<Subject> subjects;
            bool exactSize = false;       // wire discipline: reject sloppy lengths

            explicit PartyQuest() : ClientPacket(CMSG_SUI_PARTY_QUEST) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() < 14)
                {
                    recv_data.rfinish();
                    return;
                }
                uint8 count = 0;
                recv_data >> action >> questId >> npcGuid >> count;
                if (recv_data.size() != 14u + 9u * count)
                {
                    recv_data.rfinish();
                    return;
                }
                exactSize = true;
                subjects.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> subjects[i].guid >> subjects[i].rewardChoice;
            }
        };

        /// PLAN_20 P4a: take group leadership. The fleet's own grouping makes a
        /// bot the leader (BridgeHandleFormGroup calls Group::Create with the bot
        /// as leader), and vanilla's HandleGroupSetLeaderOpcode requires you to
        /// ALREADY lead in order to promote anyone -- and refuses a self-target
        /// outright. Without this the commander cannot rearrange their own party.
        class PartyLead final : public ClientPacket
        {
        public:
            uint8 action = 0;                   // 1 = claim
            ObjectGuid subject;
            bool exactSize = false;             // wire discipline: reject sloppy lengths

            explicit PartyLead() : ClientPacket(CMSG_SUI_PARTY_LEAD) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() != 9)
                {
                    recv_data.rfinish();
                    return;
                }
                recv_data >> action >> subject;
                exactSize = true;
            }
        };

        /// PLAN_20 P5: what would each party member see over these questgivers'
        /// heads? The giver list is exactly what the client is drawing markers
        /// for; there is deliberately no whole-zone shorthand, because the
        /// server's work must stay proportional to what it was asked about.
        class GiverStatus final : public ClientPacket
        {
        public:
            uint8 flags = 0;                    // reserved
            std::vector<ObjectGuid> givers;
            bool exactSize = false;             // wire discipline: reject sloppy lengths

            static uint8 const MaxGivers = 64;

            explicit GiverStatus() : ClientPacket(CMSG_SUI_GIVER_STATUS) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() < 2)
                {
                    recv_data.rfinish();
                    return;
                }
                uint8 count = 0;
                recv_data >> flags >> count;
                if (!count || count > MaxGivers || recv_data.size() != 2u + 8u * count)
                {
                    recv_data.rfinish();
                    return;
                }
                exactSize = true;
                givers.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> givers[i];
            }
        };

        /// PLAN_20 Model B: one questgiver's quests + per-member eligibility. The
        /// party is derived server-side from the requester's group; the client sends
        /// only the giver it right-clicked from the free-view camera.
        class GiverQuests final : public ClientPacket
        {
        public:
            uint8 flags = 0;                    // reserved
            ObjectGuid giver;
            bool exactSize = false;             // wire discipline: reject sloppy lengths

            explicit GiverQuests() : ClientPacket(CMSG_SUI_GIVER_QUESTS) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() != 9)      // u8 flags + u64 giver
                {
                    recv_data.rfinish();
                    return;
                }
                exactSize = true;
                recv_data >> flags >> giver;
            }
        };

        class MemberItemMove final : public ClientPacket
        {
        public:
            static constexpr size_t WIRE_SIZE = 19;           // cross-member give
            static constexpr size_t WIRE_SIZE_IN_PLACE = 21;  // in-place rearrange (dest follows)
            static constexpr uint8 FLAG_IN_PLACE = 0x01;

            uint8 flags = 0;            // bit0 = in-place rearrange within one owner
            ObjectGuid from;
            ObjectGuid to;
            uint8 bag = 0;              // 255 = character-held, 19-22 = equipped bag
            uint8 slot = 0;
            uint8 destBag = 0;          // in-place only: destination bag
            uint8 destSlot = 0;         // in-place only: destination slot
            bool inPlace = false;
            bool exactSize = false;     // wire discipline: reject sloppy lengths

            explicit MemberItemMove() : ClientPacket(CMSG_SUI_MEMBER_ITEM_MOVE) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                size_t sz = recv_data.size();
                if (sz != WIRE_SIZE && sz != WIRE_SIZE_IN_PLACE)
                {
                    exactSize = false;
                    recv_data.rfinish();
                    return;
                }
                recv_data >> flags >> from >> to >> bag >> slot;
                inPlace = (flags & FLAG_IN_PLACE) != 0;
                if (sz == WIRE_SIZE_IN_PLACE)
                {
                    recv_data >> destBag >> destSlot;
                    exactSize = inPlace;   // 21 bytes must carry the in-place flag
                }
                else
                    exactSize = !inPlace;  // 19 bytes must not
            }
        };

        /// Companions: summon / dismiss one of the requester's OWN characters, or
        /// ask for the account list. Exactly 9 bytes; the guid is 0 for a list.
        class Companion final : public ClientPacket
        {
        public:
            uint8 action = 0;               // SuiCompanion::Action
            ObjectGuid guid;
            bool exactSize = false;         // wire discipline: reject sloppy lengths

            explicit Companion() : ClientPacket(CMSG_SUI_COMPANION) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() != 9)
                {
                    recv_data.rfinish();
                    return;
                }
                recv_data >> action >> guid;
                exactSize = true;
            }
        };

        /// Party flight (owner 2026-09-03, Command View): the commander's whole
        /// commanded party takes a taxi from a flight master. u8 flags, u64
        /// flightMaster, u8 count, u32 × count nodes (source first, destination
        /// last) — exactly 10 + 4·count bytes, 2..8 nodes. See SuiTaxi.h.
        class PartyTaxi final : public ClientPacket
        {
        public:
            uint8 flags = 0;                // SuiTaxi::Flags
            ObjectGuid flightMaster;
            std::vector<uint32> nodes;
            bool exactSize = false;         // wire discipline: reject sloppy lengths

            explicit PartyTaxi() : ClientPacket(CMSG_SUI_PARTY_TAXI) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.size() < 10)
                {
                    recv_data.rfinish();
                    return;
                }
                uint8 count = 0;
                recv_data >> flags >> flightMaster >> count;
                if (count < 2 || count > 8 || recv_data.size() != 10u + 4u * count)
                {
                    recv_data.rfinish();
                    return;
                }
                nodes.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> nodes[i];
                exactSize = true;
            }
        };

        // Command View tactical freeze v1.  Unlike older opportunistic SUI
        // packets, every tactical body carries an explicit leading version and
        // is accepted only at its exact fixed/count-derived length.
        class TacticalFreeze final : public ClientPacket
        {
        public:
            static constexpr size_t WIRE_SIZE = 14;

            uint8 version = 0;
            uint32 requestId = 0;
            uint8 desiredActive = 0;
            uint64 lockId = 0;
            bool exactSize = false;

            explicit TacticalFreeze() : ClientPacket(CMSG_SUI_TACTICAL_FREEZE) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                size_t const size = recv_data.size();
                if (size >= 1)
                    recv_data >> version;
                if (size >= 5)
                    recv_data >> requestId;
                if (size != WIRE_SIZE)
                {
                    recv_data.rfinish();
                    return;
                }
                recv_data >> desiredActive >> lockId;
                exactSize = true;
            }
        };

        class CommanderRaid final : public ClientPacket
        {
        public:
            struct Record
            {
                ObjectGuid guid;
                uint8 role=0, team=0, manual=0;
                ObjectGuid primary;
                float gx=0,gy=0,gz=0,ax=0,ay=0,az=0;
                uint32 heal=0,damage=0,focus=0,interrupt=0,dispel=0,taunt=0,defensive=0;
            };
            uint8 version=0,operation=0,flags=0;
            uint32 requestId=0,revision=0,bossEntry=0,mapId=0;
            std::string definition;
            bool exactSize=false;
            std::vector<Record> records;
            explicit CommanderRaid() : ClientPacket(CMSG_SUI_COMMANDER_RAID) {}
            void ReadFromWorldPacket(WorldPacket& data) override
            {
                if (data.size()<24) { data.rfinish();return; }
                uint8 count=0; uint32 length=0;
                data >> version >> requestId >> revision >> operation >> bossEntry >> flags >> count >> mapId >> length;
                if (count>40 || length>32768 || data.size()!=24+size_t(count)*71+length || (operation!=1 && (count!=0 || length!=0)))
                { data.rfinish();return; }
                records.resize(count);
                for (auto& r:records) data >> r.guid >> r.role >> r.team >> r.manual >> r.gx >> r.gy >> r.gz
                    >> r.ax >> r.ay >> r.az >> r.heal >> r.damage >> r.primary >> r.focus >> r.interrupt >> r.dispel >> r.taunt >> r.defensive;
                definition.resize(length);
                for(uint32 i=0;i<length;i++) {uint8 value=0;data>>value;definition[i]=char(value);}
                exactSize=true;
            }
        };

        class TacticalQueue final : public ClientPacket
        {
        public:
            struct Record
            {
                ObjectGuid actorGuid;
                uint32 actionId = 0;
                uint8 actionKind = 0;
                ObjectGuid targetGuid;
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                uint32 spellId = 0;
            };

            static constexpr size_t HEADER_SIZE = 15;
            static constexpr size_t RECORD_SIZE = 37;
            static constexpr uint8 MAX_RECORDS = 40;

            uint8 version = 0;
            uint64 lockId = 0;
            uint32 requestId = 0;
            uint8 operation = 0;
            std::vector<Record> records;
            bool exactSize = false;

            explicit TacticalQueue() : ClientPacket(CMSG_SUI_TACTICAL_QUEUE) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                size_t const size = recv_data.size();
                if (size >= 1)
                    recv_data >> version;
                if (size >= 9)
                    recv_data >> lockId;
                if (size >= 13)
                    recv_data >> requestId;
                if (size < HEADER_SIZE)
                {
                    recv_data.rfinish();
                    return;
                }
                uint8 count = 0;
                recv_data >> operation >> count;
                if (!count || count > MAX_RECORDS || size != HEADER_SIZE + RECORD_SIZE * count)
                {
                    recv_data.rfinish();
                    return;
                }
                records.resize(count);
                for (Record& row : records)
                    recv_data >> row.actorGuid >> row.actionId >> row.actionKind >> row.targetGuid
                              >> row.x >> row.y >> row.z >> row.spellId;
                exactSize = true;
            }
        };

        class ForceRoster final : public ClientPacket
        {
        public:
            static constexpr size_t WIRE_SIZE = 14;

            uint8 flags = 0;
            uint32 requestId = 0;
            uint32 zoneId = 0;
            uint32 afterGuidLow = 0;
            uint8 limit = 0;
            bool exactSize = false;

            explicit ForceRoster() : ClientPacket(CMSG_SUI_FORCE_ROSTER) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                exactSize = recv_data.size() == WIRE_SIZE;
                if (!exactSize)
                {
                    recv_data.rfinish();
                    return;
                }

                recv_data >> flags >> requestId >> zoneId >> afterGuidLow >> limit;
            }
        };
    }
}

#endif
