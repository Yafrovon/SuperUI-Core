/*
 * AiBotAIMain.cpp — Autonomous AI bot for VMaNGOS 1.12.1
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the engine entry points:
 * the lifecycle overrides (OnSessionLoaded / OnPlayerLogin / OnPacketReceived /
 * MovementInform) and the UpdateAI main loop that orchestrates everything else.
 *
 * The behaviour it drives lives in sibling TUs (all member functions of AiBotAI,
 * so they link across files transparently):
 *   combat   → AiBotAICombat.cpp     bridge → AiBotAIBridge.cpp
 *   movement → AiBotAIMovement.cpp    loot   → AiBotAILoot.cpp
 *   grind    → AiBotAIGrind.cpp
 */

#include "AiBotAIMain.h"
#include "SuiAutopilot.h"
#include "SuiRaidTelemetry.h"
#include "SuiCommanderRaid.h"
#include "SuiRaidPrepare.h"
#include "AiBotCircuit.h" // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "AiBotTalents.h"
#include "SuiHero.h"
#include "Server/Packets/Quest.h"   // shared-quest accept/decline reply packets (PLAN_20 P3)
#include "Server/Packet.h"   // NullClientPacket — the typed empty client packet the group accept/decline handlers take
#include "AiBotAITeamPlay.h"   // [TEAMPLAY] ResolveCombatTarget — the group focus-fire resolver
#include "SuiPossess.h"      // [SUI] possessed-bot-as-boss precedence in FindPartyBoss
#include "SuiCompanion.h"    // [COMPANION] owner binding for the boss rules + arrival tick
#include "DBCStores.h"        // WorldSafeLocsEntry for the companion self-run wait
#include "Player.h"
#include <cstring>
#include <cstdio>
#include <cctype>   // tolower — [FOLLOW-CMD] case-insensitive escort-name match
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

// A null victim pointer is not proof of a kill: it also clears on evade,
// leash, and our own AttackStop paths. Classify the remembered victim before
// crediting it, with one map lookup shared by credit and loot.
namespace
{
    enum class SuiKillVerdict : uint8
    {
        Confirmed = 0,
        TappedAway = 1,
        Refuted = 2,
        Unconfirmed = 3,
    };

    SuiKillVerdict SuiClassifyCorpse(Player* bot, Creature* creature)
    {
        if (!creature)
            return SuiKillVerdict::Unconfirmed;
        if (!creature->IsDead())
            return SuiKillVerdict::Refuted;
        if (creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !creature->IsTappedBy(bot))
            return SuiKillVerdict::TappedAway;
        return SuiKillVerdict::Confirmed;
    }

    SuiKillVerdict SuiClassifyVictimLoss(Player* bot, uint32 victimEntry, uint32 victimGuidLow,
                                         Creature*& outCreature)
    {
        outCreature = nullptr;
        if (!bot || !bot->GetMap() || victimEntry == 0)
            return SuiKillVerdict::Unconfirmed;
        outCreature = bot->GetMap()->GetCreature(
            ObjectGuid(HIGHGUID_UNIT, victimEntry, victimGuidLow));
        return SuiClassifyCorpse(bot, outCreature);
    }
}

// ============================================================
// LIFECYCLE
// ============================================================

void AiBotAI::OnPlayerLogin()
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] OnPlayerLogin for %s (guid %u)",
        me ? me->GetName() : "NULL",
        me ? me->GetGUIDLow() : 0);

    // Bots always accept whispers (the runtime equivalent of a GM's ".whispers on").
    // PlayerBotMgr sessions are synthetic (no realmd row) and may carry elevated
    // security; the core hides higher-security characters from lower-security
    // whisperers ("That player doesn't exist") UNLESS the target accepts whispers.
    // A social-layer bot must be whisperable by everyone, always.
    // A real character keeps its own whisper setting.
    if (!IsRealCharacter())
        me->SetAcceptWhispers(true);

    // [COMPANION] A summoned companion arrives through the restart path with
    // m_initialized false. Mirror AttachToRealCharacter: role + spell caches
    // only, none of the fabricated-bot first-tick spawn work (no SPAWNING flag,
    // no spec/gear/skill repairs), so the character stays exactly as its owner
    // left it.
    if (IsRealCharacter() && !m_ownedDummyEntry && !m_initialized)
    {
        AutoAssignRole();
        ResetSpellData();
        PopulateSpellData();
        m_freshSpawn = false;
        m_initialized = true;
        m_lastKnownLevel = me->GetLevel();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s (guid %u) companion character enrolled as fleet AI",
            me->GetName(), me->GetGUIDLow());
    }

    if (!m_initialized)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: first login, spawning flag set");
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
    }

        // Persist character to DB so it survives restarts
        me->SaveToDB();
}

// [SUI] Ctrl+RightClick waypoint chain. An idle bot starts the first leg right
// away through the normal bridge MOVE_TO path (chunked pathfinding included); a
// bot already walking an ordered leg appends, and arrival chains the next leg.
WorldSession* AiBotAI::SuiCommanderSession() const
{
    if (Player* possessor = SuiPossess::GetPossessor(me))
        return possessor->GetSession();
    if (!m_suiConscriptedBy.IsEmpty())
        if (Player* commander = sObjectMgr.GetPlayer(m_suiConscriptedBy))
            return commander->GetSession();
    // [COMPANION] A summoned alt answers to its owner's session.
    if (Player* owner = SuiCompanion::OwnerOf(me))
        return owner->GetSession();
    // A real character commanded from its own free view: its session IS the commander's.
    if (me && me->GetSession() && !me->GetSession()->GetBot())
        return me->GetSession();
    return nullptr;
}

// True when this damage dealer should hold its damage: its victim is an elite someone else in its
// group holds, and its own threat is up to 90 % of the holder's.
static bool ThreatHoldFor(Player* me, CombatBotRoles role)
{
    if (role != ROLE_MELEE_DPS && role != ROLE_RANGE_DPS)
        return false;
    Unit* victim = me->GetVictim();
    if (!victim || !victim->IsCreature() || !victim->IsAlive() || !victim->CanHaveThreatList() || !me->GetGroup())
        return false;
    Creature* foe = static_cast<Creature*>(victim);
    if (!foe->IsElite() && !foe->IsWorldBoss())
        return false;   // small adds die fast; nobody waits on them
    Unit* holder = foe->GetVictim();
    if (!holder || holder == me || !holder->IsPlayer() || !me->GetGroup()->IsMember(holder->GetObjectGuid()))
        return false;
    float const held = foe->GetThreatManager().getThreat(holder);
    float const mine = foe->GetThreatManager().getThreat(me);
    return mine >= held * 0.9f;
}

bool AiBotAI::SuiValidateOrderDest(float& x, float& y, float& z)
{
    if (!me || !me->IsInWorld())
        return false;
    PathInfo probe(me);
    probe.calculate(x, y, z);
    PathType const t = probe.getPathType();
    bool const airLine = (t & (PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) && !me->CanFly();
    if ((t & PATHFIND_NOPATH) || airLine)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-order: dest off the mesh or unreachable, refused");
        if (WorldSession* commander = SuiCommanderSession())
            ChatHandler(commander).PSendSysMessage("%s: no path to that spot.", me->GetName());
        return false;
    }
    if (t & PATHFIND_INCOMPLETE)
    {
        PointsArray const& pts = probe.getPath();
        if (pts.size() < 2)
        {
            if (WorldSession* commander = SuiCommanderSession())
                ChatHandler(commander).PSendSysMessage("%s: no path to that spot.", me->GetName());
            return false;
        }
        // Walk to where the mesh actually ends toward the click: the wall, not the far side of it.
        CB_HIT(me->GetGUIDLow(), "cpp-order: partial path, going to the reachable end");
        x = pts.back().x;
        y = pts.back().y;
        z = pts.back().z;
    }
    return true;
}

void AiBotAI::SuiQueueWaypoint(float x, float y, float z)
{
    if (!SuiValidateOrderDest(x, y, z))
        return;
    if (m_currentTask.type == TASK_MOVE_TO)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: waypoint appended to active leg");
        m_suiWaypoints.push_back({x, y, z});
        return;
    }
    char json[160];
    snprintf(json, sizeof(json),
        "{\"type\":\"MOVE_TO\",\"payload\":{\"mapId\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
        me->GetMapId(), x, y, z);
    SuiInjectCommandLine(json);   // commander line: passes the conscription fence
}

AiBotAI* AiBotAI::AttachToRealCharacter(Player* owner)
{
    if (!owner || !owner->IsInWorld())
    {
        CB_HIT(owner ? owner->GetGUIDLow() : 0, "cpp-main: attach refused, no owner in world");
        return nullptr;
    }

    AiBotAI* ai = new AiBotAI(owner->GetRace(), owner->GetClass(), owner->GetLevel(),
        owner->GetMapId(), owner->GetInstanceId(),
        owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(),
        owner->GetOrientation());
    ai->SetPlayer(owner);
    ai->m_ownedDummyEntry = std::make_unique<PlayerBotEntry>();
    ai->botEntry = ai->m_ownedDummyEntry.get();

    // The fabricated-bot login work must not touch a real character: no premade spec,
    // no auto-equip, no skill-maxing, no reagent grants, no heal, no channel joins.
    // Role + spell data are the AI-internal pieces UpdateAI needs from that block.
    ai->AutoAssignRole();
    ai->ResetSpellData();
    ai->PopulateSpellData();
    ai->m_freshSpawn = false;
    ai->m_initialized = true;
    ai->m_lastKnownLevel = owner->GetLevel();

    owner->SetAI(ai);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s (guid %u) real character enrolled as fleet AI",
        owner->GetName(), owner->GetGUIDLow());
    return ai;
}

bool AiBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] OnSessionLoaded GUID=%u race=%u class=%u level=%u name='%s'",
        entry->playerGUID, m_spawnRace, m_spawnClass, m_spawnLevel,
        m_spawnName.c_str());

    // Check if this character already exists in the DB (restart vs first spawn)
    auto result = CharacterDatabase.PQuery(
        "SELECT 1 FROM `characters` WHERE `guid` = '%u'", entry->playerGUID);

    if (result)
    {
        CB_HIT(entry->playerGUID, "cpp-main: existing character found, restart path");
        // [SUI] HARD WALL: never adopt a character owned by a REAL account. The
        // brain auto-register once swallowed an enrolled real character
        // (Tesfff, 2026-08-10); logging it in on a synthetic bot account lets
        // SaveToDB stamp that account over the owner and the character
        // vanishes from their account list. Whatever a registry row says, a
        // real-account character is refused here.
        if (auto acctResult = CharacterDatabase.PQuery(   // cb:fold decl-in-condition artifact, body probed
                "SELECT `account` FROM `characters` WHERE `guid` = '%u'", entry->playerGUID))
        {
            CB_HIT(entry->playerGUID, "cpp-main: checking character owner account");
            uint32 ownerAccount = acctResult->Fetch()[0].GetUInt32();
            if (LoginDatabase.PQuery(
                    "SELECT 1 FROM `account` WHERE `id` = '%u'", ownerAccount))
            {
                // [COMPANION] The one exception: the owner of that very account
                // asked for this character on their own live session
                // (SuiCompanion::Summon verified the account match server-side
                // and the session keeps the real account id, so SaveToDB cannot
                // re-stamp it). Anything else stays refused.
                if (entry->ownerAccountId != 0 && entry->ownerAccountId == ownerAccount)
                {
                    CB_HIT(entry->playerGUID, "cpp-main: companion of its own account, allowed");
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] guid %u is a companion of account %u: logging in on the real account",
                        entry->playerGUID, ownerAccount);
                }
                else
                {
                    CB_HIT(entry->playerGUID, "cpp-main: real account owner, spawn refused");
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                        "[AIBOT] REFUSING to spawn guid %u as a bot: character belongs to REAL account %u",
                        entry->playerGUID, ownerAccount);
                    return false;
                }
            }
        }

        // RESTART PATH: character exists with gear/spells/skills intact
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] Existing character found for GUID=%u, using LoginPlayer",
            entry->playerGUID);
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    // FIRST SPAWN PATH: no character exists, create fresh
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] No existing character for GUID=%u, spawning fresh",
        entry->playerGUID);

    CharacterDatabase.PExecute("DELETE FROM `character_spell` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_skills` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_reputation` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_homebind` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_action` WHERE `guid` = '%u'", entry->playerGUID);
    sObjectMgr.DeletePlayerFromCache(entry->playerGUID);

    m_freshSpawn = true;

    if (!SpawnNewPlayer(sess, m_spawnClass, m_spawnRace, m_spawnMapId,
        m_spawnInstanceId, m_spawnX, m_spawnY, m_spawnZ, m_spawnO, nullptr, m_spawnName))
    {
        CB_HIT(entry->playerGUID, "cpp-main: fresh spawn failed");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT] SpawnNewPlayer FAILED for GUID=%u", entry->playerGUID);
        return false;
    }

    // First-spawn bots skip OnPlayerLogin (that path is LoginPlayer-only), so its
    // SetAcceptWhispers never runs here — set it now so a fresh bot is whisperable
    // immediately. (This is the WHISPER half; the name-lookup half is fixed by creating
    // the character under m_spawnName above, so it registers correctly for /w AND /invite.)
    if (me)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: fresh spawn accepts whispers");
        me->SetAcceptWhispers(true);
    }

    if (!m_spawnName.empty() && me)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: renaming fresh spawn");
        std::string oldName = me->GetName();
        sObjectMgr.DeletePlayerFromCache(me->GetGUIDLow());
        me->SetName(m_spawnName);
        sObjectMgr.InsertPlayerInCache(me);
        entry->name = m_spawnName;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] Renamed %s -> %s (GUID=%u)",
            oldName.c_str(), m_spawnName.c_str(), me->GetGUIDLow());
    }

    if (m_spawnLevel > 1 && me)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-main: applying spawn level", m_spawnLevel);
        me->GiveLevel(m_spawnLevel);
        me->SetUInt32Value(PLAYER_XP, 0);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] Set level %u for %s (GUID=%u)",
            m_spawnLevel, me->GetName(), me->GetGUIDLow());
    }

    return true;
}

void AiBotAI::OnPacketReceived(WorldPacket const* packet)
{
    // ── Session 31: Smart need/greed loot roll ──
    // CombatBotBaseAI auto-passes on SMSG_LOOT_START_ROLL.
    // We intercept first and make an intelligent decision using ScoreItem.
    if (packet->GetOpcode() == SMSG_LOOT_START_ROLL)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: loot roll packet intercepted");
        if (!me || !me->IsInWorld() || !me->GetGroup())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: roll skipped, no group or not in world");
            CombatBotBaseAI::OnPacketReceived(packet);
            return;
        }
 
        try
        {
            WorldPacket pkt(*packet);
 
            // Packet layout (from Group::SendLootStartRoll):
            //   uint64 lootedTargetGUID
            //   uint32 itemSlot
            //   uint32 itemId
            //   uint32 randomSuffix (unused)
            //   uint32 randomPropId
            //   uint32 countDown
            uint64 rawGuid;
            uint32 itemSlot, itemId, randomSuffix, randomPropId, countDown;
            pkt >> rawGuid >> itemSlot >> itemId >> randomSuffix >> randomPropId >> countDown;
 
            ObjectGuid lootedTarget(rawGuid);
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
 
            RollVote vote = ROLL_GREED; // default: greed (always better than pass for vendor gold)
 
            if (proto)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: roll evaluating item", itemId);
                bool canEquip = false;
                bool isUpgrade = false;

                if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: roll item is gear, checking eligibility");
                    // Check class/race/level requirements
                    bool classOk = (proto->AllowableClass == 0 ||
                                    (proto->AllowableClass & me->GetClassMask()));
                    bool raceOk  = (proto->AllowableRace == 0 ||
                                    (proto->AllowableRace & me->GetRaceMask()));
                    bool levelOk = ((uint32)proto->RequiredLevel <= me->GetLevel());
 
                    if (classOk && raceOk && levelOk)
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-main: roll item equippable");
                        canEquip = true;
 
                        // Map InventoryType → equipment slot for ScoreItem comparison
                        uint8 targetSlot = 255;
                        switch (proto->InventoryType)
                        {
                            case INVTYPE_HEAD:           targetSlot = EQUIPMENT_SLOT_HEAD; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_NECK:           targetSlot = EQUIPMENT_SLOT_NECK; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_SHOULDERS:      targetSlot = EQUIPMENT_SLOT_SHOULDERS; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_CHEST:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_ROBE:           targetSlot = EQUIPMENT_SLOT_CHEST; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_WAIST:          targetSlot = EQUIPMENT_SLOT_WAIST; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_LEGS:           targetSlot = EQUIPMENT_SLOT_LEGS; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_FEET:           targetSlot = EQUIPMENT_SLOT_FEET; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_WRISTS:         targetSlot = EQUIPMENT_SLOT_WRISTS; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_HANDS:          targetSlot = EQUIPMENT_SLOT_HANDS; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_FINGER:         targetSlot = EQUIPMENT_SLOT_FINGER1; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_TRINKET:        targetSlot = EQUIPMENT_SLOT_TRINKET1; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_CLOAK:          targetSlot = EQUIPMENT_SLOT_BACK; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_WEAPON:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_2HWEAPON:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_WEAPONMAINHAND: targetSlot = EQUIPMENT_SLOT_MAINHAND; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_SHIELD:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_WEAPONOFFHAND:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_HOLDABLE:       targetSlot = EQUIPMENT_SLOT_OFFHAND; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_RANGED:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_THROWN:   // cb:fold mapping detail, slot carried by adjacent probe
                            case INVTYPE_RANGEDRIGHT:    targetSlot = EQUIPMENT_SLOT_RANGED; break;   // cb:fold mapping detail, slot carried by adjacent probe
                            default: CB_HITV(me->GetGUIDLow(), "cpp-main: roll slot unmappable", proto->InventoryType); break;
                        }

                        if (targetSlot != 255)
                        {
                            CB_HITV(me->GetGUIDLow(), "cpp-main: roll comparing scores for slot", targetSlot);
                            float newScore = ScoreItem(proto, targetSlot);
                            float oldScore = 0.0f;
 
                            Item* currentItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, targetSlot);
                            if (currentItem && currentItem->GetProto())
                            {
                                CB_HIT(me->GetGUIDLow(), "cpp-main: scoring currently equipped item");
                                oldScore = ScoreItem(currentItem->GetProto(), targetSlot);
                            }

                            if (newScore > oldScore)
                            {
                                CB_HIT(me->GetGUIDLow(), "cpp-main: item is an upgrade");
                                isUpgrade = true;
                            }
                        }
                    }
                }
 
                vote = (canEquip && isUpgrade) ? ROLL_NEED : ROLL_GREED;
 
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-ROLL] %s: [%s] (id=%u q=%u) → %s%s",
                    me->GetName(),
                    proto->Name1 ? proto->Name1 : "?",
                    itemId, proto->Quality,
                    vote == ROLL_NEED ? "NEED" : "GREED",
                    isUpgrade ? " (upgrade!)" : "");
            }
            else
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: roll unknown item, greed", itemId);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-ROLL] %s: unknown item %u → GREED",
                    me->GetName(), itemId);
            }
 
            // Submit vote via the same packet path as a real player clicking Need/Greed
            auto data = std::make_unique<WorldPackets::Loot::LootRoll>();
            data->lootedTarget = lootedTarget;
            data->itemSlot = itemSlot;
            data->rollType = vote;
            me->GetSession()->QueuePacket(std::move(data));
        }
        catch (...)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: roll parse failed, base class fallback");
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-ROLL] %s: parse failed, falling back to base class (pass)",
                me->GetName());
            CombatBotBaseAI::OnPacketReceived(packet);
        }
 
        return; // Don't let base class auto-pass
    }
 
    // ── Existing: Intercept incoming chat messages (say, whisper, channel) ──
    if (packet->GetOpcode() == SMSG_MESSAGECHAT)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: chat packet received");
        try
        {
            WorldPacket pkt(*packet); // copy so we can read
            uint8 chatType;
            uint32 lang;
            pkt >> chatType >> lang;
 
            if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_WHISPER || chatType == CHAT_MSG_PARTY)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: say whisper or party chat", chatType);
                ObjectGuid senderGuid;
                pkt >> senderGuid;

                // SAY, YELL, PARTY have a SECOND copy of senderGuid
                if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_YELL || chatType == CHAT_MSG_PARTY)
                {   // cb:fold parse detail, chat carried by adjacent probes
                    ObjectGuid dupGuid;
                    pkt >> dupGuid;
                }
 
                // ── C0 self-echo filter (D16, §5.1): a bot hears its own Say/Party broadcast.
                // Forwarding it would let the bot converse with itself (reply loop). Skip entirely.
                if (senderGuid == me->GetObjectGuid())
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: own chat echo, skipping");
                    CombatBotBaseAI::OnPacketReceived(packet);
                    return;
                }

                uint32 textLen;
                pkt >> textLen;
                if (textLen > 0 && textLen < 512)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: forwarding chat to bridge");
                    std::string message;
                    pkt >> message;

                    std::string senderName = "Unknown";
                    if (Player* pSender = sObjectMgr.GetPlayer(senderGuid))   // cb:fold decl-in-condition artifact, body probed
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-main: chat sender resolved");
                        senderName = pSender->GetName();
                    }

                    const char* typeStr = (chatType == CHAT_MSG_WHISPER) ? "whisper"
                                        : (chatType == CHAT_MSG_PARTY)   ? "party"
                                        : "say";
                    // sender_guid: GUID low when the sender is a player, else 0 (§5.1)
                    uint32 senderGuidLow = senderGuid.IsPlayer() ? senderGuid.GetCounter() : 0;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: CHAT_RECV [%s] from %s: %s",
                        me->GetName(), typeStr, senderName.c_str(), message.c_str());
                    SendChatRecvEvent(senderName.c_str(), message.c_str(), typeStr, nullptr, senderGuidLow);
                }
            }
            else if (chatType == CHAT_MSG_CHANNEL)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: channel chat received");
                std::string channelName;
                pkt >> channelName;
 
                uint32 playerRank;
                pkt >> playerRank;
 
                ObjectGuid senderGuid;
                pkt >> senderGuid;
 
                // ── C0 self-echo filter (D16, §5.1): channel Say echoes back to the speaker too.
                if (senderGuid == me->GetObjectGuid())
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: own channel echo, skipping");
                    CombatBotBaseAI::OnPacketReceived(packet);
                    return;
                }

                uint32 textLen;
                pkt >> textLen;
                if (textLen > 0 && textLen < 512)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: forwarding channel chat to bridge");
                    std::string message;
                    pkt >> message;
 
                    std::string senderName = "Unknown";
                    if (Player* pSender = sObjectMgr.GetPlayer(senderGuid))   // cb:fold decl-in-condition artifact, body probed
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-main: chat sender resolved");
                        senderName = pSender->GetName();
                    }
 
                    // sender_guid: GUID low when the sender is a player, else 0 (§5.1)
                    uint32 senderGuidLow = senderGuid.IsPlayer() ? senderGuid.GetCounter() : 0;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: CHAT_RECV [channel:%s] from %s: %s",
                        me->GetName(), channelName.c_str(), senderName.c_str(), message.c_str());
                    SendChatRecvEvent(senderName.c_str(), message.c_str(), "channel", channelName.c_str(), senderGuidLow);
                }
            }
        }
        catch (...)
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: chat parse failed, skipping");
            // Packet parse failed — not critical, just skip
        }
    }
 
    // ── [PLAYERPARTY] Group invite (2026-07-07): accept from a REAL player, decline the rest ──
    // A human /invite is the entire control plane for escort mode — no UI, no bridge command.
    // Accept iff the pending group's LEADER is a real (non-bot) session; decline otherwise so a
    // stray bot-sourced invite can't graft this bot into an unmanaged group (the god-bot's own
    // grouping is direct Group::Create/AddMember — BridgeHandleFormGroup — and never sends
    // invite packets, so declining here cannot touch TeamAuto formation). Handled EXPLICITLY,
    // before the base fall-through, so behaviour never depends on CombatBotBaseAI's own invite
    // policy. On accept, ResolveDoctrine sees the real-player group next behaviour tick and
    // swaps to PlayerParty; STATE echoes pparty=1 and C# stands down.
    if (packet->GetOpcode() == SMSG_GROUP_INVITE)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: group invite received");
        bool accept = false;
        if (Group* pInviteGroup = me->GetGroupInvite())   // cb:fold decl-in-condition artifact, body probed
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: resolving invite leader");
            if (Player* pLeader = sObjectMgr.GetPlayer(pInviteGroup->GetLeaderGuid()))   // cb:fold decl-in-condition artifact, body probed
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: invite leader resolved, checking realness");
                accept = pLeader->GetSession() && !pLeader->GetSession()->GetBot();
            }
        }

        if (accept)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: invite from real player, accepting");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PARTY] %s: group invite from a REAL player — accepting (escort mode arms next tick)",
                me->GetName());
            NullClientPacket data(CMSG_GROUP_ACCEPT);   // zero-payload typed packet (Server/Packet.h)
            me->GetSession()->HandleGroupAcceptOpcode(data);
            BridgeSendEvent("PARTY_JOIN", "");
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: invite not from real player, declining");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PARTY] %s: group invite is not from a real player — declining",
                me->GetName());
            NullClientPacket data(CMSG_GROUP_DECLINE);
            me->GetSession()->HandleGroupDeclineOpcode(data);
        }
        return;   // handled — never fall through to the base class's own invite policy
    }

    // ── Shared quests (PLAN_20 P3) ───────────────────────────────────────────
    // A party member sharing a quest sends the bot SMSG_QUESTGIVER_QUEST_DETAILS
    // with the SHARER's player guid as the giver. Nothing in the bot AI answered
    // it, so a shared quest died silently and left the sharer waiting.
    //
    // Two deviations from the group-invite block above, both forced:
    //  * we answer through QueuePacket, not a direct Handle... call. The send is
    //    synchronous inside HandlePushQuestToParty, which sets the share info
    //    AFTER the send -- accepting inline would run before that exists, kill
    //    the sharer's confirmation and strand m_questShareInfo at BUSY forever.
    //  * a details packet whose giver is a CREATURE is the bridge driving this
    //    bot through a normal questgiver. Leave it alone; only a PLAYER giver
    //    means "someone shared this with you".
    if (packet->GetOpcode() == SMSG_QUESTGIVER_QUEST_DETAILS)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: quest details packet");
        if (!me || packet->size() < 12)
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: quest details malformed, base class");
            CombatBotBaseAI::OnPacketReceived(packet);
            return;
        }
        ObjectGuid giverGuid;
        uint32 questId = 0;
        {
            WorldPacket copy(*packet);
            copy.rpos(0);
            copy >> giverGuid >> questId;
        }
        if (!giverGuid.IsPlayer() || !questId)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: NPC quest offer, bridge owns it");
            CombatBotBaseAI::OnPacketReceived(packet);
            return;   // an NPC offer -- the bridge owns that path
        }

        Player* pSharer = sObjectMgr.GetPlayer(giverGuid);
        bool fromRealPartyMember = pSharer && pSharer->GetSession() &&
            !pSharer->GetSession()->GetBot() &&
            me->GetGroup() && pSharer->GetGroup() == me->GetGroup();

        Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);
        bool accept = fromRealPartyMember && pQuest &&
            me->CanTakeQuest(pQuest, false) && me->CanAddQuest(pQuest, false);

        if (accept)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-main: accepting shared quest", questId);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-QUEST] %s: accepting quest %u shared by %s",
                me->GetName(), questId, pSharer->GetName());
            auto data = std::make_unique<WorldPackets::Quest::QuestgiverAcceptQuest>();
            data->guid = giverGuid;
            data->quest = questId;
            me->GetSession()->QueuePacket(std::move(data));
        }
        else if (fromRealPartyMember)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-main: declining shared quest", questId);
            // Decline explicitly. Silence would leave the sharer's share info on
            // this bot, and every later share to it would answer BUSY.
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-QUEST] %s: declining quest %u shared by %s (cannot take it)",
                me->GetName(), questId, pSharer->GetName());
            auto data = std::make_unique<WorldPackets::Quest::QuestPushResult>();
            data->guid = me->GetObjectGuid();
            data->msg = QUEST_PARTY_MSG_DECLINE_QUEST;
            me->GetSession()->QueuePacket(std::move(data));
        }
        return;
    }

    // Escort quests (QUEST_FLAGS_PARTY_ACCEPT) confirm separately. Safe to answer
    // inline-ish here because that path sets the share info BEFORE it sends.
    if (packet->GetOpcode() == SMSG_QUEST_CONFIRM_ACCEPT)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: escort quest confirm packet");
        if (me && packet->size() >= 4)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: quest confirm parse");
            uint32 questId = 0;
            {
                WorldPacket copy(*packet);
                copy.rpos(0);
                copy >> questId;
            }
            if (questId)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: confirming escort quest", questId);
                auto data = std::make_unique<WorldPackets::Quest::QuestConfirmAccept>();
                data->questId = questId;
                me->GetSession()->QueuePacket(std::move(data));
            }
        }
        return;
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void AiBotAI::MovementInform(uint32 MovementType, uint32 Data)
{
    if (MovementType == POINT_MOTION_TYPE)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-task: point motion inform", Data);
        if (Data == AIBOT_POINT_WANDER)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: wander point reached, idle");
            // Arrived at wander destination — go idle, will wander again after timer
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_STALEMATE_NUDGE)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: stalemate nudge landed, idle");
            // Stalemate hop landed — go idle; HandleCombatStalemate re-evaluates next tick.
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_OVERPULL_FLEE)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: overpull flee hop landed, idle");
            // Retreat hop landed — go idle; HandleOverpullRetreat re-evaluates next tick.
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_INTERACT_NPC)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: NPC interaction approach point reached");
            if (m_suiSuppressArrival)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: NPC approach cancellation callback suppressed");
                return;
            }

            uint64 const interactCbt = m_pendingInteractNpcCbt;
            ObjectGuid const npcGuid = m_pendingInteractNpcGuid;
            m_pendingInteractNpcGuid.Clear();
            m_pendingInteractNpcCbt = 0;
            if (npcGuid.IsEmpty())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: NPC approach arrived without an owner");
                return;
            }

            Creature* pNpc = me->GetMap()->GetCreature(npcGuid);
            if (!pNpc)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: NPC approach target disappeared");
                BridgeSendEvent("NPC_INTERACT_FAIL", "reason=npc_lost|source=interact_npc", interactCbt);
                return;
            }

            float const npcDist = me->GetDistance(pNpc);
            if (npcDist > 10.0f)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-task: NPC approach ended too far away", npcDist);
                char eventData[128];
                snprintf(eventData, sizeof(eventData),
                    "reason=too_far|source=interact_npc|dist=%.1f", npcDist);
                BridgeSendEvent("NPC_INTERACT_FAIL", eventData, interactCbt);
                return;
            }

            CB_HIT(me->GetGUIDLow(), "cpp-task: NPC approach arrived, interaction emitted");
            me->SetFacingToObject(pNpc);
            BridgeSendEvent("NPC_INTERACT", pNpc->GetName(), interactCbt);
        }
        else if (Data == AIBOT_POINT_TASK_DEST)
        {
            // [SUI] Fix B: ignore the arrival callback when it is really an interrupt. StopMoving()
            // finalizes the abandoned spline synchronously and the base Finalize fires this as a
            // phantom arrival at the stale dest; a genuine spline completion has the latch down.
            if (m_suiSuppressArrival)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: arrival suppressed (interrupt, not a real arrival)");
                return;
            }
            if (m_suppressTaskDestInform)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: stale task-dest finalizer suppressed");
                return;
            }
            CB_HIT(me->GetGUIDLow(), "cpp-task: task dest point reached");
            // [SUI] While a human drives this bot directly, the journey must not
            // chain under them: TryBegin's stop finalizes the old spline, Finalize
            // fires this callback, and the next chunk would re-open a movespline
            // that HandleMovementOpcodes favours over every packet the human sends.
            // Commanded-from-the-free-view stays exempt — waypoint chains are
            // supposed to chain there, and the parked client sends no movement.
            if (m_possessed && !SuiPossess::IsCommandedFromFreeView(me))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: possessed, journey chain suppressed");
                return;
            }

            // More chunks remaining in the current (possibly partial) leg?
            if (!m_pathWaypoints.empty() &&
                m_pathIndex < (uint32)m_pathWaypoints.size() - 1)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: starting next path chunk");
                StartNextPathChunk();
                return;
            }

            // This leg's chunks are exhausted. If it was a PARTIAL leg that stopped
            // short of the true destination, re-query and walk the next leg —
            // continuation, no distance ceiling. stopCurrentMovement = false because
            // we're inside the motion callback (MovePoint is safe here; a full
            // StopMoving()/MotionMaster::Clear() is not).
            if (m_currentTask.type == TASK_MOVE_TO)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: move leg exhausted, continuation check");
                float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);
                if (dist > 3.0f)
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-task: short of dest", dist);
                    // [FINDING_017] Progress gate on the continuation. A new task dest
                    // re-arms the tracker; three consecutive continuations that gain
                    // <5yd toward the SAME dest mean the leg cannot advance (degenerate
                    // partials or frontier oscillation) — report honest no_path so C#
                    // shelves it, instead of re-issuing forever with no bridge event.
                    // [FINDING_017 v2] Anchor the "new dest re-arms the tracker" test to a
                    // radius comfortably larger than the ring scan. The scan writes a point up
                    // to AIBOT_RING_SCAN_RADIUS off the true dest back into m_currentTask, so an
                    // exact-inequality test here reset m_contNoProgress every leg and the
                    // MOVE_FAILED no_path below never fired. Compare against the stored anchor
                    // with a tolerance > the ring radius: jitter counts as the same journey (so
                    // no-progress accumulates and the leg is shelved), while a genuinely new
                    // objective (a far jump) still re-arms.
                    float anchorDx = m_contDestX - m_currentTask.x;
                    float anchorDy = m_contDestY - m_currentTask.y;
                    if ((anchorDx * anchorDx + anchorDy * anchorDy) >
                        (AIBOT_CONT_SAME_DEST_EPSILON * AIBOT_CONT_SAME_DEST_EPSILON))
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-task: new dest, progress tracker re-armed");
                        m_contDestX = m_currentTask.x;
                        m_contDestY = m_currentTask.y;
                        m_contLastDist = -1.0f;
                        m_contNoProgress = 0;
                    }
                    if (m_contLastDist >= 0.0f && (m_contLastDist - dist) < 5.0f)
                    {
                        CB_HITV(me->GetGUIDLow(), "cpp-task: continuation gained little ground", m_contNoProgress);
                        ++m_contNoProgress;
                    }
                    else
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-task: continuation progressing, counter reset");
                        m_contNoProgress = 0;
                    }
                    m_contLastDist = dist;

                    if (m_contNoProgress >= 3)
                    {
                        CB_HITV(me->GetGUIDLow(), "cpp-task: no progress, MOVE_FAILED no_path", m_contNoProgress);
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-PATH] %s: leg exhausted %.0fyd from dest, no progress x%u — MOVE_FAILED no_path",
                            me->GetName(), dist, m_contNoProgress);

                        char eventData[256];
                        snprintf(eventData, sizeof(eventData),
                            "dest_x=%.1f|dest_y=%.1f|dest_z=%.1f|reason=no_path|start_isolated=%u",
                            m_currentTask.x, m_currentTask.y, m_currentTask.z,
                            IsStartIsolated() ? 1u : 0u);   // [FINDING_020] tag start-side failures
                        BridgeSendEvent("MOVE_FAILED", eventData, m_currentTask.commandCbt);

                        m_contLastDist = -1.0f;
                        m_contNoProgress = 0;
                        m_currentTask.Clear();
                        ClearStoredPath();
                        return;
                    }

                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: leg exhausted %.0fyd from dest — continuing journey",
                        me->GetName(), dist);
                    ClearStoredPath();
                    MoveToDestination(m_currentTask.x, m_currentTask.y, m_currentTask.z, false);
                    return;
                }
            }

            // §4: enriched objective MOVE_TO reached the deep coord with no scan hit
            // (mobs were deeper than scan range during the walk) — grind here in place
            // instead of falsely reporting "arrived" (merged step: a TASK_COMPLETE here
            // would mean objective-done with zero kills). Bare MOVE_TO emits "arrived".
            if (m_currentTask.type == TASK_MOVE_TO && m_currentTask.creatureEntry != 0)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: deep coord reached, grind in place");
                ConvertMoveToGrindInPlace();
                return;
            }

            // Path complete (arrived, or it was a short single-MovePoint path).
            // Stamp the exact arrival coord into the event so C# refreshes ctx.Pos NOW
            // instead of waiting up to one 5s STATE cycle — otherwise a just-arrived bot
            // still reads stale-far and the planner re-issues MOVE_TO instead of interacting.
            CB_HIT(me->GetGUIDLow(), "cpp-task: arrived at dest, TASK_COMPLETE");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s arrived at task destination", me->GetName());
            char arrBuf[96];
            snprintf(arrBuf, sizeof(arrBuf), "MOVE_TO arrived|x=%.1f|y=%.1f|z=%.1f",
                     me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
            BridgeSendEvent("TASK_COMPLETE", arrBuf, m_currentTask.commandCbt);
            m_currentTask.Clear();
            ClearStoredPath();

            // [SUI] Queued RTS waypoints: chain into the next leg from inside the
            // motion callback — MoveToDestination with stopCurrentMovement=false,
            // the same rule the PARTIAL-leg continuation above follows.
            if (!m_suiWaypoints.empty())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: chaining next queued waypoint");
                std::array<float, 3> next = m_suiWaypoints.front();
                m_suiWaypoints.pop_front();
                if (m_suiPatrolLoop)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: patrol loop, waypoint recycled");
                    m_suiWaypoints.push_back(next);   // patrol: the route cycles
                }
                m_currentTask.type = TASK_MOVE_TO;
                m_currentTask.x = next[0];
                m_currentTask.y = next[1];
                m_currentTask.z = next[2];
                MoveToDestination(next[0], next[1], next[2], false);
            }
            else if (m_suiFormationFacing > -100.f)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: formation slot reached, taking facing");
                // [SUI] Formation slot reached: take the ordered facing. The
                // stamp survives exactly one arrival; every new order clears it
                // through SuiClearWaypoints.
                me->SetFacingTo(m_suiFormationFacing);
                m_suiFormationFacing = -1000.f;
            }
        }
        else if (Data == AIBOT_POINT_GRIND_PATROL)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: grind patrol point reached, idle");
            me->GetMotionMaster()->MoveIdle();
            // Arrived at patrol point within grind area — will pick next target or patrol again
        }
    }
}

// ============================================================
// MAIN UPDATE LOOP — COMPLETE-METHOD REPLACEMENT
//
// Drop-in replacement for AiBotAI::UpdateAI in AiBotAIMain.cpp. Identical to the
// deployed overpull/§4/[GRAVE-SELFREZ] version EXCEPT the TASK_GRIND post-freeze block,
// which now SELF-UNSTICKS the objective grind instead of handing back GRIND_BLOCKED|
// overpull_dwell (the kobold-camp livelock fix).
//
// Why: a dense field of QUEST mobs vetoed every pull (OverpullGuard: >AIBOT_OVERPULL_SOLO
// within AIBOT_PULL_DENSITY_RADIUS). The objective grind then handed back overpull_dwell,
// which C# always answered with a forced single-kill detour + re-issue of the SAME
// objective → re-freeze on the SAME camp. One kill per ~3s roundtrip until the fail/
// deadline machinery shelved the quest and the bot wandered off to easier work, beside an
// ocean of valid mobs. Now BOTH grind modes self-unstick after AIBOT_GRIND_FREEZE_DWELL:
// SelectGrindTarget already returns the LEAST-clustered candidate, HandleOverpullRetreat is
// the in-combat backstop if too many pile on, and a genuinely unkillable field still trips
// the C# 120s no-kill deadline → durable shelve. The no_target handback (truly empty field)
// is UNCHANGED. Only the overpull_dwell objective handback is removed.
//
// [TEAMPLAY] 2026-07-02 (B2/B3, the focus-fire divergence closure): the sticky-assist fix
// (v1.1) bridged only the narrowest of THREE divergence windows. This pass closes the two
// structural ones: (B3) pull discipline — a follower whose assist seam can't resolve yet
// HOLDS its grind pull for a bounded dwell so the anchor pulls first (the fresh-engagement
// seeding gap: nothing could create the FIRST assist), applied at the grind dispatch and
// the enriched-MOVE_TO approach scan; (B2) mid-combat convergence — an engaged follower
// re-checks the seam every tick and SWITCHES to the anchor's resolved victim (the old
// valid-victim branch held course for the victim's whole life and never consulted the
// resolver, so the arrival fan-out persisted until kill boundaries coincided by luck).
// ============================================================

// [DOCTRINE] Re-resolve the engagement doctrine for the current combat state and swap the
// instance only when the kind changes (so TeamAuto's sticky memo + pull-hold counter reset by
// construction on a mode switch). Cheap; called once per behaviour tick from UpdateAI.
void AiBotAI::RefreshDoctrine()
{
    DoctrineKind const kind = ResolveDoctrine(*this);
    if (!m_doctrine || kind != m_doctrineKind)
    {
        CB_HITV(me ? me->GetGUIDLow() : 0, "cpp-main: doctrine changed, swapping", (int)kind);
        // [SUI] A Solo-era brain errand does not survive joining a human's party:
        // the planner stands down on pparty, but its last MOVE_TO kept walking the
        // bot to a dead objective — and a live TASK_MOVE_TO also gates
        // DoPartyFollow, so the new member marched away instead of forming up.
        if (kind == DoctrineKind::PlayerParty && m_doctrineKind == DoctrineKind::Solo)
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: joining player party, abandoning solo errand");
            SuiAbandonJourney();
        }
        char const* from = m_doctrine ? m_doctrine->Name() : "(none)";
        m_doctrine = MakeDoctrine(kind);
        m_doctrineKind = kind;

        // The doctrine instances own their own tactical memo, but the shared combat
        // mechanisms below keep their continuation state on AiBotAI.  An opt-out must
        // therefore clear the old doctrine's in-flight counters/holds here; otherwise a
        // disabled mechanism can still suppress the 4 Hz rotation or resume where it left
        // off if this bot later changes posture again.
        if (!m_doctrine->UseStalemateBreaker())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: doctrine disables stalemate breaker, state reset");
            m_stalemateMs = 0;
            m_stalemateHoldMs = 0;
            m_stalemateNudges = 0;
            m_stalemateDisengages = 0;
            m_lastHealth = 0;
            m_lastVictimHealth = 0;
            m_stalemateVictim.Clear();
        }
        if (!m_doctrine->UseOverpullRetreat())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: doctrine disables overpull retreat, state reset");
            m_overpullFleeHoldMs = 0;
            m_overpullFlees = 0;
        }
        if (!m_doctrine->UseTapRespect() && !m_combatIgnore.empty())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: doctrine disables tap respect, stale ignores cleared");
            // m_combatIgnore is shared by tap/stalemate/overpull.  A doctrine swap is a
            // clean authority boundary: discard entries created under the old policy and
            // let any still-enabled safety mechanism repopulate its own entries.
            m_combatIgnore.clear();
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-DOCTRINE] %s: %s -> %s%s",
            me ? me->GetName() : "?", from, m_doctrine ? m_doctrine->Name() : "?",
            m_combatDirective.IsActive() ? " (directive active)" : "");
    }
}

// ── [PLAYERPARTY] The escort primitives (2026-07-07) ──────────────────────────────────────

// The human this bot escorts: the group LEADER when it is a real (non-bot) session, else the
// first real-player member; nullptr when no real player is in the group (or no group). This
// is the ONE detection primitive — ResolveDoctrine keys the PlayerParty doctrine on it, the
// STATE producer echoes it (pparty), the bridge Form/Disband guards refuse on it, and the
// escort hook follows it. Bot sessions are identified by WorldSession::GetBot() (the
// PlayerBotEntry every PlayerBotMgr-owned session carries; real clients have none).
// [COMPANION] Multi-human law (owner 2026-09-02): every unit follows "the driven
// body of ITS human". A bound unit (companion / unattended own character /
// conscript) has exactly one human and follows nobody else; a shared fleet bot
// keeps the group rules (real leader first, else the deterministic split) and
// then follows THAT human's driven body. The old "first possessed member in
// iteration order" pre-pass was the single-human special case of this and
// dragged everyone after one human's toon once two humans possessed.
Player* AiBotAI::SuiDrivenBodyOf(Player* human)
{
    if (!human || !human->GetSession())
        return nullptr;
    if (Player* bot = SuiPossess::GetControlledBot(human->GetSession()))
        if (bot->IsInWorld())
            return bot;
    return human;
}

bool AiBotAI::SuiTryResurrectAlly()
{
    if (!me || !me->IsInWorld() || !me->IsAlive() || m_possessed || m_suiManual)
        return false;
    // Out of combat: the class resurrection spell (priest/paladin/shaman).
    // In combat: only a druid's Rebirth.
    SpellEntry const* spell = me->IsInCombat()
        ? (me->GetClass() == CLASS_DRUID ? m_spells.druid.pRebirth : nullptr)
        : m_resurrectionSpell;
    if (!spell)
        return false;
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;
    if (me->IsNonMeleeSpellCasted(false))
        return false;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsInWorld() || pMember->IsAlive())
            continue;
        if (pMember->IsRessurectRequested())
            continue;   // someone already offered
        if (pMember->GetMapId() != me->GetMapId() || pMember->GetInstanceId() != me->GetInstanceId())
            continue;
        if (!me->IsWithinLOSInMap(pMember) || !spell->IsTargetInRange(me, pMember))
            continue;
        if (!CanTryToCastSpell(pMember, spell))
            continue;
        if (DoCastSpell(pMember, spell) == SPELL_CAST_OK)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[AIBOT] %s resurrects %s (%s)",
                me->GetName(), pMember->GetName(), me->IsInCombat() ? "combat rez" : "out of combat");
            return true;
        }
    }
    return false;
}

Player* AiBotAI::SuiBoundHuman() const
{
    if (!me)
        return nullptr;
    if (Player* owner = SuiCompanion::OwnerOf(me))
        return owner;
    if (IsUnattendedRealCharacter())
        return me;
    if (!m_suiConscriptedBy.IsEmpty())
        if (Player* commander = sObjectMgr.GetPlayer(m_suiConscriptedBy))
            if (commander->GetSession() && !commander->GetSession()->GetBot())
                return commander;
    return nullptr;
}

// Resolve a bound unit's boss: the bound human's driven body, or hold. Returns
// true when the bound rule decided (boss may be nullptr = stand still).
static bool SuiBoundBoss(AiBotAI const* ai, Player* me, Group* pGroup, Player** boss)
{
    Player* human = ai->SuiBoundHuman();
    if (!human)
        return false;
    *boss = nullptr;
    if (human->GetGroup() != pGroup)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: bound human outside my group, holding");
        return true;
    }
    Player* body = AiBotAI::SuiDrivenBodyOf(human);
    if (!body || body == me)
    {
        // I AM what my human plays (or my own session drives nothing else): hold.
        CB_HIT(me->GetGUIDLow(), "cpp-main: bound human drives nothing else, holding");
        return true;
    }
    CB_HIT(me->GetGUIDLow(), "cpp-main: bound human's driven body is boss");
    *boss = body;
    return true;
}

Player* AiBotAI::FindPartyBoss() const
{
    if (!me || !me->IsInWorld())
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: no boss, bot not in world");
        return nullptr;
    }

    Group* pGroup = me->GetGroup();
    if (!pGroup)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: no boss, not grouped");
        return nullptr;
    }

    Player* boundBoss = nullptr;
    if (SuiBoundBoss(this, me, pGroup, &boundBoss))
        return boundBoss;

    Player* firstReal = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: boss scan member skipped, null or self");
            continue;
        }
        WorldSession* pSess = pMember->GetSession();
        if (!pSess || pSess->GetBot())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: boss scan member is bot session");
            continue;   // a bot session — not a boss
        }
        if (pMember->GetObjectGuid() == pGroup->GetLeaderGuid())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: real leader is boss");
            Player* body = SuiDrivenBodyOf(pMember);   // the leader is real — unambiguous
            return body && body != me ? body : pMember;
        }
        if (!firstReal)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: first real member remembered");
            firstReal = pMember;
        }
    }
    // [AUTOPILOT] No human in the group: the autopilot leader stands in for one, so the
    // whole escort (formation, assist, group tactics) runs unchanged under a bot leader.
    if (!firstReal)
        return SuiAutopilot::LeaderFor(me);
    // leader is a bot but a human is present — escort the human's driven body
    if (Player* body = SuiDrivenBodyOf(firstReal))
        return body != me ? body : firstReal;
    return firstReal;
}

// The human THIS bot keeps formation on (2026-07-08, multi-human split). Null iff the group
// holds no real player — the SAME truth value as FindPartyBoss, so pparty semantics are
// unchanged when the STATE producer keys on this instead. With one human it degenerates to
// him; with several, GUIDLow % count spreads the escort across them deterministically (the
// group's member list is one server-side object — every bot walks the identical order, so
// no sort and no coordination are needed, and the pick never flaps between ticks; it only
// reshuffles when membership actually changes, which is the right time to reshuffle).
Player* AiBotAI::FindEscortBoss() const
{
    if (!me || !me->IsInWorld())
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: no escort boss, bot not in world");
        return nullptr;
    }

    Group* pGroup = me->GetGroup();
    if (!pGroup)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: no escort boss, not grouped");
        return nullptr;
    }

    // [CHAIN] Explicit anchor wins (owner 2026-09-03): a real player anchors at their
    // driven body, a bot at itself. A stale or absent anchor falls through to the rules.
    if (!m_suiChainAnchor.IsEmpty())
    {
        Player* target = sObjectMgr.GetPlayer(m_suiChainAnchor);
        if (target && target != me && target->IsInWorld() && target->GetGroup() == pGroup)
        {
            bool const realTarget = target->GetSession() && !target->GetSession()->GetBot();
            Player* body = realTarget ? SuiDrivenBodyOf(target) : target;
            if (!body)
                body = target;
            if (body != me)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: explicit chain anchor is boss");
                return body;
            }
        }
    }

    // [COMPANION] Bound units answer to one human's driven body (see SuiBoundBoss).
    // Shared fleet bots fall through to the assigned-human split below and then
    // escort THAT human's driven body.
    Player* boundBoss = nullptr;
    if (SuiBoundBoss(this, me, pGroup, &boundBoss))
        return boundBoss;

    std::vector<Player*> reals;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: escort scan member skipped, null or self");
            continue;
        }
        WorldSession* pSess = pMember->GetSession();
        if (!pSess || pSess->GetBot())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: escort scan member is bot session");
            continue;   // a bot session — not a human
        }
        reals.push_back(pMember);
    }

    if (reals.empty())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: no real players, no escort boss");
        return SuiAutopilot::LeaderFor(me);   // [AUTOPILOT] the bot leader, or null
    }

    // [FOLLOW-CMD] Explicit assignment wins: "{bot} follow {player}" stored a lowercased
    // name; if that human is HERE, escort him. Not present (offline / left / typo) → fall
    // through to the auto split below, so a stale override can never strand the bot.
    if (!m_escortOverrideName.empty())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: follow override present, matching");
        for (Player* pReal : reals)
        {
            char const* n = pReal->GetName();
            size_t i = 0;
            for (; n[i] && i < m_escortOverrideName.size(); ++i)
                if ((char)tolower((unsigned char)n[i]) != m_escortOverrideName[i])
                    break;   // cb:fold hot per-char name compare detail
            if (!n[i] && i == m_escortOverrideName.size())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: follow override matched, boss chosen");
                Player* body = SuiDrivenBodyOf(pReal);   // full-length case-insensitive match
                return body && body != me ? body : pReal;
            }
        }
    }

    Player* assigned = reals[me->GetGUIDLow() % reals.size()];
    Player* body = SuiDrivenBodyOf(assigned);
    return body && body != me ? body : assigned;
}

// Keep formation on the boss (called from the escort hook, out of combat, after the engage
// attempt found nothing to fight). Movement is spine-owned, doctrine names targets only.
// [MULTI-HUMAN] "the boss" here = this bot's ASSIGNED human (FindEscortBoss) — with several
// real players in the party the escort splits across them deterministically.
//  - dead boss: stand vigil (his ghost is not a follow target; follow resumes on his rez);
//  - cross-map/instance boss or a detected distant port: hold and end the follow leg;
//  - ordinary same-map walking separation retains the existing catch-up path;
//  - otherwise: (re)issue MoveFollow only when the follow generator is not already driving,
//    with a per-guid angle so the escort fans out behind him instead of stacking.
// [SUI-TAXI] A hold must also END the walk: the follow generator issued while the boss
// stood beside us stays on the motion master and keeps chasing him under the gryphon
// (owner 2026-09-03: "my character was moving towards Stormwind"). Only a FOLLOW leg is
// stopped — an RTS order task keeps its own generator.
void AiBotAI::SuiStopFollowForHold()
{
    if (!me || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        return;
    me->StopMoving();
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();
}

void AiBotAI::DoPartyFollow()
{
    // [MULTI-HUMAN] Formation keys on the ASSIGNED human (FindEscortBoss), not the single
    // detection boss — with two real players the fleet splits ~evenly instead of stacking
    // on one. Catch-up and world-hold below inherit the same target, so each
    // half of the escort tracks ITS human even when the humans split up.
    // [SUI] Divinity-style chain: an UNLINKED member (broken off from the portrait
    // chain) never formation-follows — it stands where it was left. Combat assist
    // and the player-party stand-down stay live; only the follow leg is severed.
    if (m_suiUnlinked)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: unlinked, holding position");
        return;
    }

    // A toon the human is commanding from the sky stands where it was sent. Formation-following
    // would drag it back to the party the moment its ordered leg finished, which is the opposite
    // of driving it: you moved it there on purpose.
    if (SuiPossess::IsCommandedFromFreeView(me))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: commanded from free view, no follow");
        return;
    }

    // [SUI-TAXI] Cargo: a bot on a flight (party flight, or driven onto one and then
    // left) never pushes a follow generator over the flight generator.
    if (me->IsTaxiFlying())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: on a taxi, no follow");
        return;
    }

    // [AUTOPILOT] The raid holds its camp while the leader walks out to pull.
    if (SuiAutopilot::HoldForPull(me))
    {
        SuiStopFollowForHold();
        return;
    }
    // [AUTOPILOT] ...and stacks on the leader while the raid waits.
    {
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        int const stack = SuiAutopilot::StackOnLeader(me, sx, sy, sz);
        if (stack == 1)
            return;
        if (stack == 2)
        {
            if (!me->IsMoving() || me->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            {
                me->UpdateAllowedPositionZ(sx, sy, sz);
                me->GetMotionMaster()->MovePoint(0, sx, sy, sz, MOVE_PATHFINDING | MOVE_RUN_MODE);
            }
            return;
        }
    }

    Player* pBoss = FindEscortBoss();
    if (!pBoss || !pBoss->IsInWorld() || !pBoss->IsAlive())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: no live boss, no follow");
        return;
    }

    // [TACTICS] Stepped out of lava or fire: stay out while the anchor stands where it stood -
    // the formation slot is what put this member in it (2026-09-24: 30 bots burned to death in
    // lava beside a leader waiting out a patrol).
    if (m_hazardHoldUntil)
    {
        if (WorldTimer::getMSTime() < m_hazardHoldUntil &&
            pBoss->GetDistance2d(m_hazardAnchorX, m_hazardAnchorY) < 8.0f && me->GetDistance(pBoss) < 35.0f)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: holding clear of a ground hazard");
            return;
        }
        m_hazardHoldUntil = 0;
    }

    if (pBoss->GetMap() != me->GetMap())
    {
        // A map/instance boundary must not become a delayed automatic summon.
        m_bossOffMapMs = 0;
        if (!m_suiLandedHold)
        {
            m_suiLandedHold = true;
            SuiPossess::NotifyChainChanged(me);
        }
        SuiStopFollowForHold();
        return;
    }

    m_bossOffMapMs = 0;

    float const dist = me->GetDistance(pBoss);

    // Record the anchor every tick, flights included. A far body switch or
    // discontinuous movement of the same body leaves the party holding.
    ObjectGuid const bossGuid = pBoss->GetObjectGuid();
    bool const bossChanged = !m_suiLastBossGuid.IsEmpty() && m_suiLastBossGuid != bossGuid;
    bool const bossPorted = !bossChanged && !m_suiLastBossGuid.IsEmpty() &&
        pBoss->GetDistance(m_suiLastBossX, m_suiLastBossY, m_suiLastBossZ) > AIBOT_PARTY_CATCHUP_TELEPORT;
    m_suiLastBossGuid = bossGuid;
    m_suiLastBossX = pBoss->GetPositionX();
    m_suiLastBossY = pBoss->GetPositionY();
    m_suiLastBossZ = pBoss->GetPositionZ();
    if ((bossChanged || bossPorted) && dist > AIBOT_PARTY_CATCHUP_TELEPORT && !m_suiLandedHold)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: anchor hopped or ported away, holding");
        m_suiLandedHold = true;
        SuiPossess::NotifyChainChanged(me);
    }
    // Owner 2026-09-03: in direct control the rest of the party STAYS when the driven
    // body takes a flight. Hold while he flies; when he lands far away, keep holding
    // (the left-behind latch) rather than catch-up teleporting after the gryphon.
    if (pBoss->IsTaxiFlying())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: boss on a taxi, holding");
        m_suiBossFlewAway = true;
        SuiStopFollowForHold();
        return;
    }
    if (m_suiBossFlewAway)
    {
        m_suiBossFlewAway = false;
        if (dist > AIBOT_PARTY_CATCHUP_TELEPORT && !m_suiLandedHold)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: boss flew away, holding where we are");
            m_suiLandedHold = true;
            SuiPossess::NotifyChainChanged(me);
        }
    }

    // [SUI-TAXI] Left-behind hold (flight, distant body switch, or anchor port):
    // hold here. The boss coming within catch-up range releases it and formation resumes.
    if (m_suiLandedHold)
    {
        if (dist > AIBOT_PARTY_CATCHUP_TELEPORT)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: left-behind hold, party not here yet");
            SuiStopFollowForHold();
            return;
        }
        m_suiLandedHold = false;              // the anchor is back in range: the chain re-forms
        SuiPossess::NotifyChainChanged(me);
    }

    if (dist > AIBOT_PARTY_CATCHUP_TELEPORT)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-main: left behind, catch-up teleport", dist);
        float bx = pBoss->GetPositionX();
        float by = pBoss->GetPositionY();
        float bz = pBoss->GetPositionZ();
        ReGroundZ(bx, by, bz, "party-catchup");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PARTY] %s: left %.0fyd behind the boss — catch-up teleport to (%.1f, %.1f, %.1f)",
            me->GetName(), dist, bx, by, bz);
        me->NearTeleportTo(bx, by, bz, me->GetOrientation());
        return;
    }

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE &&
        dist > AIBOT_PARTY_FOLLOW_DIST + 1.0f)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: issuing follow behind boss");
        // Deterministic per-bot spread behind the boss: 8 slots around the rear
        // arc, PLUS a per-bot RANK stagger on the follow distance. One shared
        // radius put same-arc neighbours shoulder-in-shoulder for the whole
        // journey — most of the visual stacking WHILE MOVING (owner 2026-08-28).
        // guid/8 decorrelates the rank from the guid%8 arc slot.
        float const angle = M_PI_F + (float(me->GetGUIDLow() % 8) - 3.5f) * (M_PI_F / 8.0f);
        float const followDist =
            AIBOT_PARTY_FOLLOW_DIST + float((me->GetGUIDLow() / 8) % 3) * 1.25f;
        me->GetMotionMaster()->MoveFollow(pBoss, followDist, angle);
    }
}

// ============================================================
// MAIN UPDATE LOOP — COMPLETE-METHOD REPLACEMENT (2026-07-05, survival pass fixes 1+2)
//
// Identical to the deployed doctrine/self-unstick version EXCEPT:
//
//   [EAT-HYST]  The OOC eat gate is now a hysteresis LATCH. Old behaviour: with an active
//               task, DrinkAndEat was reachable only below 40% HP / 20% mana — the moment
//               HP crossed 41% the condition went false and the grind dispatch re-pulled,
//               so a tasked bot lived permanently in the 40-60% band (casters re-pulled at
//               21% mana). New: dipping below the floor latches recovery ON; the latch
//               releases only at 90% HP / 85% mana (the proven RezHealTarget shape).
//
//   [PULLGATE]  Solo fight-initiation floor at BOTH engage sites (grind dispatch +
//               approach scan): never START a fight below 70% HP / 50% mana — latch
//               recovery and stand down instead of patrolling INTO the field. Defense,
//               in-progress fights, and the TeamAuto/Directed doctrines are untouched
//               (the group has its own recovery protocols: GroupDefend / guard-heal /
//               the chain 40/40 gate). Recovery (~10-30s) is well inside the 120s
//               no-kill deadline, so no false shelves.
//
// The kobold-camp self-unstick, doctrine dispatch, [GRAVE-SELFREZ], and §4 blocks are
// carried verbatim from the deployed build.
// ============================================================
// Bridge connect/recv/state/flush on the 1 Hz cadence. Factored out of UpdateAI
// so the possessed short-circuit keeps the bridge (and the C# brain's view of
// this bot) alive while every behaviour tick is suspended.
void AiBotAI::UpdateBridgeTick()
{
    // [SUI] An enrolled REAL character never talks to the brain. The brain
    // auto-registers every HELLO into characters.playerbot for restart
    // persistence -- which adopted a real character into the fleet, respawned
    // it on a synthetic account after a restart, and stole it from its owner
    // (Tesfff, 2026-08-10). Doctrine + explicit CMSG_SUI_ORDER injections are
    // the whole control surface for an unattended real character.
    if (IsRealCharacter())
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: enrolled real character, brain suppressed");
        return;
    }

    if (!m_bridgeConnected)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: bridge disconnected");
        if (m_bridgeReconnectTimer <= AIBOT_UPDATE_INTERVAL)
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: reconnect timer elapsed, connecting");
            m_bridgeReconnectTimer = 0;
            BridgeConnect();
        }
        else
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: reconnect timer ticking");
            m_bridgeReconnectTimer -= AIBOT_UPDATE_INTERVAL;
        }
    }

    if (m_bridgeConnected)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: bridge tick, connected");
        if (!m_bridgeHelloSent && m_initialized)
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: sending HELLO to brain");
            BridgeSendHello();
        }

        BridgeRecv();

        if (m_bridgeHelloSent)
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: hello sent, state cadence");
            if (m_bridgeStateTimer <= AIBOT_UPDATE_INTERVAL)
            {
                CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: state interval elapsed, sending STATE");
                m_bridgeStateTimer = BRIDGE_STATE_INTERVAL;
                BridgeSendState();
            }
            else
            {
                CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: state timer ticking");
                m_bridgeStateTimer -= AIBOT_UPDATE_INTERVAL;
            }
        }

        BridgeFlush();   // Session 36: drain bytes deferred by a prior partial/blocked write

        CircuitFlush();  // [CIRCUIT] ship this second's buffered probes (no-op unless armed)
    }
}

// SUI possession toggle (SuiPossess.cpp). On: freeze in place — the human's
// client is about to take over the mover; any in-flight spline or task motion
// would fight it. Off: resume on a fresh 1s tick so the accumulated diff of a
// long possession doesn't fire one giant catch-up tick, and let RefreshDoctrine
// re-resolve naturally on that tick.
void AiBotAI::SetPossessed(bool on)
{
    if (m_possessed == on)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: possession unchanged, no-op");
        return;
    }
    m_possessed = on;
    if (on)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: possession begin, freezing");
        if (me)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: stopping movement for possession");
            StopMoving();
            me->GetMotionMaster()->MoveIdle();
        }
    }
    else
    {
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-main: possession end, resume on fresh tick");
        m_updateTimer.Reset(1000);
    }
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[AIBOT] %s possession %s",
        me ? me->GetName() : "<no player>", on ? "BEGIN" : "END");
}

// [FINDING_019] Travel-stuck watchdog. The C# wedge breaker trips on LastProgressUtc, which the
// SET_TASK-ack churn refreshes -> a geometry-wedged bot never trips it and never PORT_HOMEs
// (documented in GoalSelector.cs, never fixed). This watches REAL position: an active task dest,
// out of combat, not yet arrived, and no physical advance > AIBOT_TRAVEL_STUCK_RADIUS for
// AIBOT_TRAVEL_STUCK_MS (+ per-bot jitter to destagger a mass wedge) -> snap to the nearest navmesh
// poly (local un-stick) and drop the leg so C# re-plans a reachable one. No navmesh within reach =
// genuinely isolated dest: drop the leg (fleet no_path memory blacklists it) rather than port nowhere.
void AiBotAI::UpdateTravelStuckWatchdog()
{
    uint32 const now = WorldTimer::getMSTime();

    bool traveling = m_currentTask.type != TASK_IDLE && !me->IsInCombat();
    if (traveling)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: watchdog checking travel progress");
        float ddx = m_currentTask.x - me->GetPositionX();
        float ddy = m_currentTask.y - me->GetPositionY();
        float arrive = m_currentTask.radius > AIBOT_TRAVEL_STUCK_RADIUS ? m_currentTask.radius : AIBOT_TRAVEL_STUCK_RADIUS;
        if (ddx * ddx + ddy * ddy < arrive * arrive)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: within arrive radius, not stuck");
            traveling = false;   // at/near the dest -- not travel-stuck
        }
    }
    if (!traveling)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: not traveling, watchdog reset");
        m_travelRefX = me->GetPositionX(); m_travelRefY = me->GetPositionY(); m_travelRefMs = now;
        return;
    }

    float mx = me->GetPositionX() - m_travelRefX;
    float my = me->GetPositionY() - m_travelRefY;
    if (mx * mx + my * my > AIBOT_TRAVEL_STUCK_RADIUS * AIBOT_TRAVEL_STUCK_RADIUS)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: advancing, watchdog reset");
        m_travelRefX = me->GetPositionX(); m_travelRefY = me->GetPositionY(); m_travelRefMs = now;
        return;   // advancing normally
    }

    uint32 const threshold = AIBOT_TRAVEL_STUCK_MS + (me->GetGUIDLow() % 30) * 1000;
    uint32 const stuckMs = WorldTimer::getMSTimeDiff(m_travelRefMs, now);
    if (stuckMs < threshold)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-main: wedged, stuck dwell accruing", stuckMs);
        return;
    }

    float nx, ny, nz;
    if (FindNearestNavmeshPointNear(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                                    nx, ny, nz, AIBOT_TRAVEL_UNSTUCK_SEARCH))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: navmesh snap candidate found");
        float sdx = nx - me->GetPositionX(), sdy = ny - me->GetPositionY();
        if (sdx * sdx + sdy * sdy > 1.0f)   // a real relocation, not a no-op snap onto our own poly
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: travel stuck, snap to navmesh, drop leg");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-UNSTUCK] %s: travel-stuck %ums at (%.1f,%.1f,%.1f) -> snap %.1fyd to navmesh (%.1f,%.1f,%.1f), drop leg",
                me->GetName(), stuckMs, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                sqrtf(sdx * sdx + sdy * sdy), nx, ny, nz);
            me->NearTeleportTo(nx, ny, nz, me->GetOrientation());
            SuiAbandonJourney();
            m_travelRefX = nx; m_travelRefY = ny; m_travelRefMs = now;
            return;
        }
    }

    CB_HIT(me->GetGUIDLow(), "cpp-main: travel stuck, isolated dest, drop leg");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-UNSTUCK] %s: travel-stuck %ums at (%.1f,%.1f,%.1f) but no navmesh within %.0fyd -> drop leg (isolated dest)",
        me->GetName(), stuckMs, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
        AIBOT_TRAVEL_UNSTUCK_SEARCH);
    SuiAbandonJourney();
    m_travelRefX = me->GetPositionX(); m_travelRefY = me->GetPositionY(); m_travelRefMs = now;
}


// [HEARTH] Drive the stranded-escape Hearthstone cast to completion. While the cast runs we own
// the tick (no movement, so we don't self-interrupt). On natural completion the spell has already
// ported the bot to the escape town (its homebind was pointed there for the cast); we restore the
// real homebind and re-anchor the spawn fallback. Any early loss of the cast (damage / knockback /
// combat / death) is an interrupt: restore the homebind and let the C# brain re-issue the escape.
// Returns true while the hearth owns the tick (caller returns), false once done/aborted (resume AI).
bool AiBotAI::HandleHearthCast()
{
    if (!m_hearthActive)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: no hearth active");
        return false;
    }

    // Interruptible by design — pulled into combat or killed cancels the escape.
    if (!me->IsAlive() || me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: hearth aborted by combat or death");
        me->InterruptSpell(CURRENT_GENERIC_SPELL);
        me->SetHomebindInMemory(
            WorldLocation(m_hearthSavedHomeMap, m_hearthSavedHomeX, m_hearthSavedHomeY, m_hearthSavedHomeZ, 0.0f),
            m_hearthSavedArea);
        m_hearthActive = false;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s HEARTH aborted (alive=%d combat=%d) — brain re-issues the escape",
            me->GetName(), me->IsAlive() ? 1 : 0, me->IsInCombat() ? 1 : 0);
        return false;
    }

    m_hearthElapsedMs += AIBOT_UPDATE_INTERVAL;

    // Still channeling → own the tick (stationary; do not let normal AI issue a move that breaks it).
    if (me->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: hearth still casting, owning tick");
        return true;
    }

    // Cast is gone. Either it completed (spell teleported us to the town via the overridden homebind)
    // or it was interrupted before completion. Restore the real homebind either way.
    me->SetHomebindInMemory(
        WorldLocation(m_hearthSavedHomeMap, m_hearthSavedHomeX, m_hearthSavedHomeY, m_hearthSavedHomeZ, 0.0f),
        m_hearthSavedArea);
    m_hearthActive = false;

    bool const crossMapPort = ((int)me->GetMapId() != m_hearthMap) || me->IsBeingTeleported();
    float const dx = me->GetPositionX() - m_hearthX, dy = me->GetPositionY() - m_hearthY;
    bool const arrivedSameMap = ((int)me->GetMapId() == m_hearthMap) && (dx * dx + dy * dy) < 60.0f * 60.0f;

    if (crossMapPort || arrivedSameMap)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: hearth port confirmed, re-anchoring spawn");
        // Re-anchor the "spawn" fallback to the town we hearthed to (mirrors the old PORT_HOME).
        m_spawnMapId = (uint32)m_hearthMap;
        m_spawnX = m_hearthX; m_spawnY = m_hearthY; m_spawnZ = m_hearthZ; m_spawnO = me->GetOrientation();
        if ((int)me->GetMapId() != m_hearthMap)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: cross-map hearth, landing validation pending");
            // Cross-continent worldport is async — validate the landing on the arrival tick (same
            // guarantee as the fallback instant cross-map port).
            m_pendingWalkableLanding = true;
            m_pendingWalkableMap = (uint32)m_hearthMap;
            m_pendingWalkableX = m_hearthX; m_pendingWalkableY = m_hearthY; m_pendingWalkableZ = m_hearthZ;
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: same-map hearth, walkable teleport");
            TeleportToWalkable(m_hearthX, m_hearthY, m_hearthZ, me->GetOrientation(), "hearth-home");
            m_spawnX = me->GetPositionX(); m_spawnY = me->GetPositionY(); m_spawnZ = me->GetPositionZ();
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s HEARTH complete -> town (%.1f, %.1f, %.1f) map=%d",
            me->GetName(), m_hearthX, m_hearthY, m_hearthZ, m_hearthMap);
        return true;
    }

    CB_HITV(me->GetGUIDLow(), "cpp-main: hearth lost early, escape aborted", m_hearthElapsedMs);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s HEARTH interrupted @ %u ms — escape aborted (brain re-issues)",
        me->GetName(), m_hearthElapsedMs);
    return false;
}

void AiBotAI::UpdateAI(uint32 const diff)
{
    // Handle pending teleports from base class
    PlayerBotAI::UpdateAI(diff);

    // [COMPANION] First tick after AddToWorld: join the owner's party and
    // teleport beside the owner's driven body. Nothing else this tick — the
    // body may be mid-transfer afterwards.
    if (m_suiCompanionArrival)
    {
        SuiCompanion::TickArrival(this);
        return;
    }

    // [SUI] Fix A: drain at most one coalesced RTS move per tick (latest dest wins). Runs ahead of
    // the 1 Hz behaviour gate so an ordered move stays responsive, and before the possess
    // early-returns so a free-view-commanded body still gets its move.
    if (SuiCommanderRaid::Tick(this, diff)) return;
    // A commanded pre-pull preparation owns the tick the same way: the body stays put
    // and eats, drinks and buffs until it is raid-ready or interrupted.
    if (SuiRaidPrepare::Tick(this, diff)) return;
    ConsumePendingSuiRtsMove();

    // [ROTATION/SPEC] Combat sub-tick: external slates and validated built-in
    // spec policies evaluate at 4 Hz
    // AHEAD of the 1s behaviour gate — the 1 Hz loop can't weave a GCD, and its wand
    // autorepeat early-return silences a wanding bot's spell evaluation entirely. This
    // runs ONLY the cast attempt; every behaviour decision (tasks, doctrine, bridge,
    // loot, kill detection) stays on the 1s tick below. Legacy/fallback bots skip in
    // one branch and retain the original cadence.
    m_rotationSubTick.Update(diff);
    if (m_rotationSubTick.Passed())
    {   // cb:fold hot 4 Hz sub-tick cadence, cast decisions probed inside
        m_rotationSubTick.Reset(AIBOT_ROTATION_SUBTICK_MS);
        // [SUI] Manual primary: the commander swings and casts from the sky, so the body must keep
        // facing its victim on its own (a driven client turns its own character; this one has no
        // driver). 4 Hz, planted only - a chasing body already faces where it runs.
        if (m_suiManual && me && me->IsInWorld() && me->IsAlive() && !me->IsMoving())
            if (Unit* pVictim = me->GetVictim())
                if (!me->HasInArc(pVictim, 2 * M_PI_F / 3))
                    me->SetInFront(pVictim);
        if (!m_possessed && !m_suiManual && !m_recoveryResetHoldMs
            && HasFastCombatPolicy() && me && me->IsInWorld() && !me->IsBeingTeleported()
            && me->IsAlive() && (me->IsInCombat() || HealerOnDuty())
            && !me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)
            && !me->IsNonMeleeSpellCasted(false, false, true))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: combat sub-tick cast window open");
            if (!m_rotation.empty())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: external rotation slate cast");
                UpdateRotationSlate(); // absolute external override
            }
            // Built-in policies yield while the movement spine owns a pull or
            // escape hop.  Casting here can stop the isolated tag-and-drag or a
            // stalemate/overpull retreat before its 1 Hz handler advances it.
            // External LOAD_ROTATION remains the absolute override above.
            // [TACTICS] A tactical move in progress (closing for sight, out of fire, clear of a
            // melee hostile) is not cancelled by the next cast: DoCastSpell stops movement for any
            // cast-time spell, which pinned healers behind the rock they were walking out of.
            else if (m_repositionUntilMs && WorldTimer::getMSTime() < m_repositionUntilMs && me->IsMoving())
                SuiRaidTelemetry::OnRotationTick(me, 6);
            // [TACTICS] Threat discipline: a damage dealer does not take an elite off whoever holds
            // it - at 90 % of the holder's threat it waits (the game moves aggro at 110 % in melee
            // range, 130 % beyond). 2026-09-25: Onyxia turned to a warlock and a mage in the first
            // ten seconds, before her tank had built anything, and breathed on the raid.
            else if (ThreatHoldFor(me, GetCombatActiveRole()))
                SuiRaidTelemetry::OnRotationTick(me, 4);
            else if (!m_pullActive && !m_stalemateHoldMs && !m_overpullFleeHoldMs)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: built-in spec cast attempt");
                SuiRaidTelemetry::OnRotationTick(me, 0);
                UpdateSpecCombatAI();
            }
            else
                SuiRaidTelemetry::OnRotationTick(me, m_pullActive ? 3 : 4);
        }
        else if (me && me->IsInWorld() && me->IsAlive())
            SuiRaidTelemetry::OnRotationTick(me, !(me->IsInCombat() || HealerOnDuty()) ? 1 :
                me->IsNonMeleeSpellCasted(false, false, true) ? 2 : 5);
    }

    // [RAID-PLAN] Act cadence for the adopted raid plan (PLAN_19 M-D): formation
    // stations + maintained auras at 2 Hz, same guard set as the rotation
    // sub-tick. Behaviour DECISIONS stay on the 1s tick and the doctrine; this
    // only walks and wards.
    m_raidPlanSubTick.Update(diff);
    if (m_raidPlanSubTick.Passed())
    {   // cb:fold hot 2 Hz sub-tick cadence, act tick probed inside
        m_raidPlanSubTick.Reset(500);
        if (!m_possessed && !m_recoveryResetHoldMs && m_hasRaidPlan && me && me->IsInWorld() && !me->IsBeingTeleported()
            && me->IsAlive()
            && !me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: raid plan act tick");
            UpdateRaidPlanTick();
        }
    }

    // [TRACE] Movement trace sub-tick (2026-07-20) — the SYMPTOM half of the fly
    // instrumentation. Placed here for the same reason as the rotation sub-tick above: it must
    // run AHEAD of the 1s behaviour gate, because 1 Hz is ~7yd of travel per sample at run speed
    // — far too coarse to resolve a float that opens and closes inside one fillet arc. It
    // self-throttles to AIBOT_TRACE_SAMPLE_MS internally and returns on the first branch unless
    // this bot's name is in run/bin/aibot_trace.txt, so an unarmed fleet pays one string compare
    // per bot per 250ms and emits nothing. Pairs with the dispatch-time [AIBOT-TRACE] WP/PATH
    // lines from GroundPathPoints — cause and symptom land on one timeline in Server.log.
    if (!m_possessed)
        UpdateMovementTrace(diff);   // cb:fold hot per-update trace sub-tick, self-throttled inside

    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: behavior tick begins");
        m_updateTimer.Reset(AIBOT_UPDATE_INTERVAL);
    }
    else
        return;   // cb:fold hot per-update behavior gate

    if (!me->IsInWorld() || me->IsBeingTeleported())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: not in world or teleporting, tick skipped");
        return;
    }

    // Finish walkability validation for a cross-continent PORT_HOME after PlayerBotAI above
    // completes the asynchronous worldport. The original far teleport uses a real hostile-spawn
    // coordinate from the C# safety grid; this final pass re-grounds it and ring-nudges off any
    // disconnected navmesh pixel before autonomous quest/grind selection resumes.
    if (m_pendingWalkableLanding && me->GetMapId() == m_pendingWalkableMap)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: cross-map landing, validating walkable");
        float const requestedX = m_pendingWalkableX;
        float const requestedY = m_pendingWalkableY;
        m_pendingWalkableLanding = false;
        TeleportToWalkable(m_pendingWalkableX, m_pendingWalkableY, m_pendingWalkableZ,
            me->GetOrientation(), "port-home-world");
        m_spawnMapId = me->GetMapId();
        m_spawnX = me->GetPositionX();
        m_spawnY = me->GetPositionY();
        m_spawnZ = me->GetPositionZ();
        m_spawnO = me->GetOrientation();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: cross-map landing validated (%.1f,%.1f) -> walkable (%.1f,%.1f,%.1f)",
            me->GetName(), requestedX, requestedY, m_spawnX, m_spawnY, m_spawnZ);
    }

    // [SUI] A real player is driving this body (SuiPossess). Every autonomous
    // behaviour — tasks, doctrine, combat, loot, wander — is suspended; only the
    // bridge stays alive so the C# brain keeps seeing STATE (possessed:1) and
    // stands down. SetPossessed(false) resumes on a fresh 1s tick.
    // Commanded from the free view is the exception: autonomy still must not choose goals,
    // but the tick has to RUN or the ordered task has nothing to walk it. The doctrine that
    // results is PlayerParty (the possessor's own body resolves as boss), which is exactly the
    // wanted shape — no grind, no patrol, no wander, combat assist live, and the task machinery
    // reachable for the MOVE_TO the order just queued. DoPartyFollow is separately suppressed
    // for this bot so it holds the ground you sent it to instead of walking back to the party.
    if (m_possessed && !SuiPossess::IsCommandedFromFreeView(me))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: possessed, autonomy suspended, bridge only");
        UpdateBridgeTick();
        return;
    }

    // [SUI-TAXI] A bot on a flight is cargo until it lands: the flight generator owns
    // its motion, hostile refs are offline, and nothing autonomous may push a movement
    // generator over it (party flight, or a possessed flyer whose human hopped away).
    // The bridge stays alive so the brain keeps seeing STATE.
    if (me->IsTaxiFlying())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: on a taxi, autonomy suspended, bridge only");
        UpdateBridgeTick();
        return;
    }

    // [SUI] Manual primary: the commander's own keys drive this unit. Orders still land (the RTS
    // move drained above, ATTACK_TARGET through the bridge below); nothing autonomous runs.
    if (m_suiManual)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: manual primary, autonomy suspended, bridge only");
        // Body etiquette the AI used to own: weapons out while fighting, away when clear.
        // The commander has no sheath key for a unit he is not possessing.
        bool const fighting = me->IsInCombat() || me->GetVictim() != nullptr;
        if (fighting && me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
            me->SetSheath(SHEATH_STATE_MELEE);
        else if (!fighting && me->GetSheath() != SHEATH_STATE_UNARMED && m_suiSheathOverride < 0)
            me->SetSheath(SHEATH_STATE_UNARMED);
        UpdateBridgeTick();
        return;
    }

    // [HEARTH] Authentic stranded-escape hearth in progress: keep STATE flowing to the brain,
    // own the tick while the Hearthstone cast runs, fire the port on completion, and fall through
    // to normal AI if the cast just aborted (interrupt / combat / death). See HandleHearthCast.
    if (m_hearthActive)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: hearth active, driving cast");
        UpdateBridgeTick();
        if (HandleHearthCast())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: hearth owns tick");
            return;
        }
    }

    // Protocol-v6 combat-still reset hold. Keep the bridge alive (including
    // fresh STATE) but suspend every autonomous mover/target selector while C#
    // validates the reset result. This branch owns later hold ticks; the second
    // check below catches the command on the exact tick that adopts it.
    if (m_recoveryResetHoldMs)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combat-reset: recovery hold owns tick", m_recoveryResetHoldMs);
        UpdateBridgeTick();
        m_recoveryResetHoldMs = m_recoveryResetHoldMs > AIBOT_UPDATE_INTERVAL
            ? m_recoveryResetHoldMs - AIBOT_UPDATE_INTERVAL
            : 0;
        return;
    }

    // [FINDING_019] Backstop: un-stick a bot physically wedged in geometry while traveling. Runs on
    // the 1s behaviour tick, ahead of the task/doctrine machinery (which can not see the wedge).
    UpdateTravelStuckWatchdog();

    // Decrement wander/patrol timer
    if (m_wanderTimer > AIBOT_UPDATE_INTERVAL)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: wander timer ticking");
        m_wanderTimer -= AIBOT_UPDATE_INTERVAL;
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: wander timer at zero");
        m_wanderTimer = 0;
    }

    // [SUI] party-spacing sidestep throttle rides the same clock.
    if (m_unstackTimer > AIBOT_UPDATE_INTERVAL)
        m_unstackTimer -= AIBOT_UPDATE_INTERVAL;
    else
        m_unstackTimer = 0;

    // §4 approach-scan throttle
    if (m_approachScanTimer > AIBOT_UPDATE_INTERVAL)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: approach scan timer ticking");
        m_approachScanTimer -= AIBOT_UPDATE_INTERVAL;
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: approach scan timer at zero");
        m_approachScanTimer = 0;
    }

    // [ADDED] Combat-stalemate ignore set: tick down per-guid cooldowns
    for (auto it = m_combatIgnore.begin(); it != m_combatIgnore.end(); )
    {
        if (it->second <= AIBOT_UPDATE_INTERVAL)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: tapped-ignore expired");
            it = m_combatIgnore.erase(it);
        }
        else { CB_HIT(me->GetGUIDLow(), "cpp-main: tapped-ignore cooldown ticking"); it->second -= AIBOT_UPDATE_INTERVAL; ++it; }
    }

    // [DOCTRINE] Resolve which engagement doctrine governs this tick (Solo / TeamAuto / Directed)
    // and swap on change, before any acquisition or combat decision below consults m_doctrine.
    RefreshDoctrine();

    // --- Bridge: connect + recv + periodic state ---
    UpdateBridgeTick();

    if (m_recoveryResetHoldMs)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combat-reset: newly-adopted recovery hold owns tick", m_recoveryResetHoldMs);
        m_recoveryResetHoldMs = m_recoveryResetHoldMs > AIBOT_UPDATE_INTERVAL
            ? m_recoveryResetHoldMs - AIBOT_UPDATE_INTERVAL
            : 0;
        return;
    }

    // One-time log on first successful update tick
    if (!m_loggedFirstUpdate)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: first update tick, enabling save");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s (guid %u) UpdateAI active - class %u, level %u, zone %u, map %u",
            me->GetName(), me->GetGUIDLow(), me->GetClass(), me->GetLevel(),
            me->GetZoneId(), me->GetMapId());
        m_loggedFirstUpdate = true;
        me->SetSaveDisabled(false);
        me->SaveToDB();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s (guid %u) saved to DB", me->GetName(), me->GetGUIDLow());
    }

    // --- Initialization (once, on first update after login) ---
    if (!m_initialized)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: initializing bot on first tick");
        uint32 learnedTalentPoints = 0;
        uint32 learnedClassSpells = 0;
        uint32 learnedArmorSpells = 0;

        // Attached real characters deliberately bypass all fabricated-bot mutations.
        if (!IsRealCharacter())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: fabricated bot, running spawn repairs");
            AiBotTalents::RepairResult repair = AiBotTalents::EnsureProfileAndTalents(me, botEntry);
            learnedTalentPoints = repair.learnedPoints;
            if (repair.role != ROLE_INVALID)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: role taken from talent repair");
                m_role = repair.role;
            }

            // Quest/fundamental abilities unlock trainer chains, so repair the
            // curated class set after talents but before refreshing trainer and
            // item spells.  Attached real characters never enter this block.
            learnedClassSpells = LearnBotClassQuestSpells();
            if (m_freshSpawn || learnedTalentPoints || learnedClassSpells)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: refreshing trainer and item spells");
                LearnTrainerAndItemSpells();
            }
            learnedArmorSpells = LearnArmorProficiencies();

            if (learnedClassSpells || learnedArmorSpells)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,   // cb:fold logging-only spell-count report
                    "[AIBOT] %s learned lifecycle spells: class=%u armor=%u",
                    me->GetName(), learnedClassSpells, learnedArmorSpells);
        }

        if (m_freshSpawn)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: fresh spawn auto-equip");
            AutoEquipGear(PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR);
        }

        if (m_role == ROLE_INVALID)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: auto-assigning role");
            AutoAssignRole();
        }

        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        me->UpdateSkillsToMaxSkillsForLevel();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        if (urand(0, 1))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: random helm-cloak hide toggle");
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM);
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK);
        }

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        // Join zone-specific General and Trade channels so bots can hear channel chat
        // Channel names are zone-dependent: "General - Elwynn Forest", "Trade - Stormwind City", etc.
        if (me->GetSession())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: joining zone chat channels");
            const AreaEntry* zoneEntry = AreaEntry::GetById(newzone);
            if (zoneEntry && zoneEntry->Name)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: zone resolved, joining General and Trade");
                std::string zoneName = zoneEntry->Name;

                WorldPackets::Channel::JoinChannel joinGeneral;
                joinGeneral.channelName = "General - " + zoneName;
                joinGeneral.channelPassword = "";
                me->GetSession()->HandleJoinChannelOpcode(joinGeneral);

                WorldPackets::Channel::JoinChannel joinTrade;
                joinTrade.channelName = "Trade - " + zoneName;
                joinTrade.channelPassword = "";
                me->GetSession()->HandleJoinChannelOpcode(joinTrade);

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s joined General and Trade channels for zone %s",
                    me->GetName(), zoneName.c_str());
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: zone name unresolved, skipping channels");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s could not resolve zone name for zone %u, skipping channel join",
                    me->GetName(), newzone);
            }
        }

        if (m_freshSpawn || learnedTalentPoints || learnedClassSpells || learnedArmorSpells)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: init save to DB");
            me->SaveToDB();
        }
        m_initialized = true;
        m_lastKnownLevel = me->GetLevel();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s initialized - class %u, level %u, role %u",
            me->GetName(), me->GetClass(), me->GetLevel(), m_role);
        return;
    }

    // --- Death handling: ghost at corpse, wait for C# RESURRECT (or self-rez at a graveyard) ---
    if (me->IsDead())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: dead tick");
        if (!m_wasDead)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: death detected, ghosting at corpse");
            m_wasDead = true;

            float deathX = me->GetPositionX();
            float deathY = me->GetPositionY();
            float deathZ = me->GetPositionZ();
            uint32 deathMap = me->GetMapId();

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s died at (%.1f, %.1f, %.1f) map=%u — ghosting at corpse, waiting for C# RESURRECT",
                me->GetName(), deathX, deathY, deathZ, deathMap);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: stopping movement on death");
                StopMoving();
            }

            m_currentTask.Clear();

            // Clean up any existing corpse from a previous death
            if (Corpse* oldCorpse = me->GetCorpse())   // cb:fold decl-in-condition artifact, body probed
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: clearing previous corpse");
                me->SpawnCorpseBones();
            }

            // Ghost form — bot becomes translucent spirit, corpse drops
            me->BuildPlayerRepop();
            // NO RepopAtGraveyard — ghost stays right here at the death position

            // [COMPANION] Size the self-run: the time a spirit would spend running
            // back from the nearest graveyard, floored by the reclaim delay.
            // [SELF-REZ] Every other bot gets the same clock as a BACKSTOP (owner 2026-09-22:
            // a wiped raid stayed ghosts for four hours). Revival was delegated wholly to the
            // C# brain's MaintenancePlanner, and with the brain disabled - the raid setup - no
            // RESURRECT ever comes. A live brain rezzes in 15-22 s, long before this fires, so
            // its graveyard/loop handling still wins; the clamp keeps an instance death (whose
            // graveyard is on another map) from producing a meaningless run time.
            {
                bool const companion = SuiCompanion::IsCompanion(me);
                float runSeconds = 0.0f;
                if (WorldSafeLocsEntry const* grave = sObjectMgr.GetClosestGraveYard(
                        deathX, deathY, deathZ, deathMap, me->GetTeam()))
                {
                    if (grave->map_id == deathMap)
                    {
                        float const dx = grave->x - deathX, dy = grave->y - deathY;
                        runSeconds = sqrtf(dx * dx + dy * dy) / SuiCompanion::GHOST_RUN_SPEED;
                    }
                }
                uint32 const reclaimMs = me->GetCorpseReclaimDelay(false) * IN_MILLISECONDS;
                uint32 runMs = uint32(runSeconds * 1000.0f);
                if (!companion)
                    runMs = std::min(std::max(runMs, AIBOT_SELF_REZ_MIN_MS), AIBOT_SELF_REZ_MAX_MS);
                m_suiCompanionDeadMs = 0;
                m_suiCompanionSelfRezAtMs = std::max(reclaimMs, runMs);
                if (companion)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                        "[SUI-COMPANION] %s died; self-run pops in %u s unless the party rezzes first",
                        me->GetName(), m_suiCompanionSelfRezAtMs / 1000);
                    SuiCompanion::NotifyOwner(me, "%s has died.", me->GetName());
                }
            }

            char deathData[160];
            snprintf(deathData, sizeof(deathData),
                "x=%.1f|y=%.1f|z=%.1f|map=%u|attackers=%u",
                deathX, deathY, deathZ, deathMap, m_lastAttackerCount);
            BridgeSendEvent("DEATH", deathData);
            if (SuiAutopilot::IsLeader(me))
                SuiAutopilot::NoteLeaderDeath(me);

            return;
        }

        // [GRAVE-SELFREZ] Graveyard self-rez — the firing half of the zone-0 escape.
        if (m_pendingGraveyardRez)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: graveyard self-rez pending");
            if (SuiHero::BlocksResurrection(me))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: hero blocks resurrection, rez canceled");
                m_pendingGraveyardRez = false;
                m_graveRezWaitMs = 0;
                return;
            }

            if (m_graveRezWaitMs > AIBOT_UPDATE_INTERVAL)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: grave rez wait ticking");
                m_graveRezWaitMs -= AIBOT_UPDATE_INTERVAL;
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: grave rez wait at zero");
                m_graveRezWaitMs = 0;
            }

            float distToGrave = me->GetDistance2d(m_graveRezX, m_graveRezY);
            bool landed   = (me->GetMapId() == m_graveRezMap) && (distToGrave < 25.0f);
            bool timedOut = (m_graveRezWaitMs == 0);

            if (landed || timedOut)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: graveyard reached or timeout, self-rez", distToGrave);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s graveyard self-rez %s at (%.1f, %.1f) (dist=%.1f)",
                    me->GetName(), landed ? "confirmed" : "TIMEOUT",
                    me->GetPositionX(), me->GetPositionY(), distToGrave);

                m_pendingGraveyardRez = false;
                m_graveRezWaitMs = 0;

                me->ResurrectPlayer(0.5f);
                me->CombatStop(true);   // never resume rez in-combat (a ghost pulled nothing anyway)
                me->SpawnCorpseBones();

                BridgeSendEvent("RESPAWN", "");
                BridgeSendState();
                return;
            }

            return;
        }

        // [COMPANION] Nobody's brain will ever RESURRECT a companion. Wait out the
        // self-run (a party rez arriving meanwhile simply revives us), then pop
        // in place once the party has left combat — never mid-fight.
        // [SELF-REZ] The same wait is the backstop for every bot the brain does not revive.
        {
            bool const companion = SuiCompanion::IsCompanion(me);
            m_suiCompanionDeadMs += AIBOT_UPDATE_INTERVAL;
            if (m_suiCompanionDeadMs < m_suiCompanionSelfRezAtMs)
                return;
            bool partyInCombat = false;
            if (Group* pGroup = me->GetGroup())
                for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    if (Player* pMember = itr->getSource())
                        if (pMember->IsInWorld() && pMember->IsAlive() && pMember->IsInCombat())
                        {
                            partyInCombat = true;
                            break;
                        }
            if (partyInCombat)
                return;
            if (SuiHero::BlocksResurrection(me))
                return;
            // [SELF-REZ] A body revives at half health. Beside the pack that killed it, it
            // dies again within the minute (2026-09-22: 11 of 39 re-died at a Molten Core
            // Firelord pack whose Lava Spawns still stood on the corpses, then again to a
            // Lava Surger and an Ancient Core Hound PATROLLING through the corpses - "nothing
            // near right now" is no promise inside a raid). So inside an instance the backstop
            // does what a real corpse run does: the ghost goes to the entrance and revives
            // there (the proven ghost-port + m_pendingGraveyardRez path above). A healer's
            // Resurrect during the wait still wins. In the open world it revives in place
            // unless a hostile is close, and then keeps waiting for the pack to leash.
            if (me->GetMap()->IsDungeon())
            {
                AreaTriggerTeleport const* entrance = sObjectMgr.GetMapEntranceTrigger(me->GetMapId());
                if (entrance && entrance->destination.mapId == me->GetMapId())
                {
                    WorldLocation const& at = entrance->destination;
                    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                        "[AIBOT] %s: no RESURRECT after %u s - ghost to the instance entrance (%.1f, %.1f, %.1f) to revive",
                        me->GetName(), m_suiCompanionDeadMs / 1000, at.x, at.y, at.z);
                    me->NearTeleportTo(at.x, at.y, at.z, at.o);
                    m_pendingGraveyardRez = true;
                    m_graveRezX = at.x; m_graveRezY = at.y; m_graveRezZ = at.z;
                    m_graveRezMap = me->GetMapId(); m_graveRezWaitMs = 8000;
                    return;
                }
            }
            {
                std::list<Unit*> nearby;
                me->GetEnemyListInRadiusAround(me, AIBOT_SELF_REZ_SAFE_YARDS, nearby);
                Unit const* threat = nullptr;
                for (Unit* unit : nearby)
                    if (unit && unit->IsCreature() && unit->IsAlive() && unit->IsHostileTo(me) &&
                        static_cast<Creature*>(unit)->GetCreatureType() != CREATURE_TYPE_CRITTER)
                    {
                        threat = unit;
                        break;
                    }
                if (threat)
                {
                    if (m_suiCompanionDeadMs % 30000 < AIBOT_UPDATE_INTERVAL)
                        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                            "[AIBOT] %s: holding revival - %s (entry %u) is %.0f yd from the corpse",
                            me->GetName(), threat->GetName(), threat->GetEntry(), me->GetDistance(threat));
                    return;
                }
            }
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, companion
                    ? "[SUI-COMPANION] %s recovers its corpse in place"
                    : "[AIBOT] %s: no RESURRECT after %u s and the group is out of combat - recovering in place",
                me->GetName(), m_suiCompanionDeadMs / 1000);
            me->ResurrectPlayer(0.5f);
            me->CombatStop(true);
            me->SpawnCorpseBones();
            if (companion)
                SuiCompanion::NotifyOwner(me, "%s has recovered its body.", me->GetName());
            else
            {
                BridgeSendEvent("RESPAWN", "");
                BridgeSendState();
            }
            return;
        }

        // Subsequent ticks while dead: keep bridge alive, wait for RESURRECT.
        return;
    }
    else
    {   // cb:fold alive fall-through, revival probed inside
        if (m_wasDead)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: revived, resummon pet");
            m_wasDead = false;
            SummonPetIfNeeded();
            return;
        }
    }

    // [TACTICS] Nobody stands in fire on the ground.
    if (AvoidGroundHazard())
        return;

    // [TACTICS] Nobody but the tank stands in front of a breath.
    if (AvoidFrontalCone())
        return;

    // [TELEMETRY] Positions of tanks and healers in an instance fight, every 8 s (the geometry a
    // death line cannot show: who stood where, in lava, without sight of the tank).
    if (me->IsInCombat() && me->GetGroup() && me->GetMap()->IsDungeon() &&
        (GetCombatActiveRole() == ROLE_TANK || GetCombatActiveRole() == ROLE_HEALER) &&
        WorldTimer::getMSTime() >= m_posLogNextMs)
    {
        m_posLogNextMs = WorldTimer::getMSTime() + 8000;
        Player* tank = ActiveTank();
        Unit* v = me->GetVictim();
        bool const lava = (me->GetTerrain()->getLiquidStatus(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ() + 0.01f,
            MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME, nullptr) & (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)) != 0;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[POS] %s %s (%.1f, %.1f, %.1f)%s hp %u%% | tank %s %.0f yd%s | victim %s %.0f yd (%.1f, %.1f)%s",
            me->GetName(), GetCombatActiveRole() == ROLE_TANK ? "tank" : "healer",
            me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), lava ? " IN LAVA" : "", uint32(me->GetHealthPercent()),
            tank ? tank->GetName() : "-", tank ? me->GetDistance(tank) : -1.0f,
            tank && !me->IsWithinLOSInMap(tank) ? " NO SIGHT" : "",
            v ? v->GetName() : "-", v ? me->GetDistance(v) : -1.0f, v ? v->GetPositionX() : 0.0f, v ? v->GetPositionY() : 0.0f,
            v && !me->IsWithinLOSInMap(v) ? " NO SIGHT" : "");
    }

    // [TACTICS] Ranged and healers keep clear of the melee brawl.
    if (MaintainRangedSpacing())
        return;

    // [AUTOPILOT] The leader's drive (pick, walk out, pull, run back) runs ahead of the combat
    // handling below: a pull is in combat from the moment the pack notices the leader.
    if (SuiAutopilot::IsLeader(me) && SuiAutopilot::TickLeader(this))
        return;

    // [OVERPULL] Peak melee attackers this combat — stamped on the DEATH event above.
    if (me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: in combat, tracking attacker peak");
        uint32 atk = (uint32)me->GetAttackers().size();
        if (atk > m_lastAttackerCount)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-main: attacker peak raised", atk);
            m_lastAttackerCount = atk;
        }
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: out of combat, attacker peak reset");
        m_lastAttackerCount = 0;
    }


    // --- TASK_TAXI: in-flight — skip ALL behavior until we land ---
    if (m_currentTask.type == TASK_TAXI)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: taxi task tick");
        if (me->GetTaxi().empty() && !me->HasUnitState(UNIT_STATE_TAXI_FLIGHT))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-task: flight complete, task cleared", m_currentTask.taxiDestNode);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s: flight complete (arrived at node %u)",
                me->GetName(), m_currentTask.taxiDestNode);
            BridgeSendEvent("FLIGHT_COMPLETE", "", m_currentTask.commandCbt);
            m_currentTask.Clear();
            // Don't return — fall through to normal idle behavior this tick
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: still flying, behavior suspended");
            return; // still flying — don't wander, fight, eat, buff, etc.
        }
    }

    // --- Level-up detection ---
    if (m_lastKnownLevel > 0 && me->GetLevel() > m_lastKnownLevel)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-main: level-up detected", me->GetLevel());
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s leveled up: %u -> %u",
            me->GetName(), m_lastKnownLevel, me->GetLevel());
        if (!IsRealCharacter())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: level-up repairs for fabricated bot");
            AiBotTalents::RepairResult repair = AiBotTalents::EnsureProfileAndTalents(me, botEntry);
            if (repair.role != ROLE_INVALID)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: role from level-up repair");
                m_role = repair.role;
            }

            uint32 const learnedClassSpells = LearnBotClassQuestSpells();
            LearnTrainerAndItemSpells();
            uint32 const learnedArmorSpells = LearnArmorProficiencies();
            if (learnedClassSpells || learnedArmorSpells)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,   // cb:fold logging-only spell-count report
                    "[AIBOT] %s learned level-up lifecycle spells: class=%u armor=%u",
                    me->GetName(), learnedClassSpells, learnedArmorSpells);
        }

        SendLevelUpEvent(me->GetLevel());
        m_lastKnownLevel = me->GetLevel();

        // Refresh spell caches for both fabricated bots and attached real
        // characters, but never mutate an attached character's skills/items.
        ResetSpellData();
        PopulateSpellData();
        if (!IsRealCharacter())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: level-up reagents and skills refresh");
            AddAllSpellReagents();
            me->UpdateSkillsToMaxSkillsForLevel();
            me->SaveToDB();
        }
    }

    // --- Auto-loot timer ---
    if (m_lootTimer > 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: loot timer ticking");
        m_lootTimer -= (int32)AIBOT_UPDATE_INTERVAL;
        if (m_lootTimer <= 0)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: loot timer elapsed");
            m_lootTimer = 0;
            if (!m_lootTargetGuid.IsEmpty())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: firing auto-loot");
                DoAutoLoot(m_lootTargetGuid);
                m_lootTargetGuid.Clear();
            }
        }
    }

    // --- CC break ---
    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: crowd controlled, breaking");
        BreakCrowdControlEffects();
        return;
    }

    // --- Auto-repeat spell handling (Hunter Auto Shot / caster wands) ---
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: autorepeat spell active");
        bool const fastPolicy = HasFastCombatPolicy();
        if (!me->GetVictim())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: no victim, stopping autorepeat");
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        }
        else if (me->GetClass() == CLASS_HUNTER)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: hunter autorepeat check");
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: victim in melee, stopping auto shot");
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            }
            else if (!fastPolicy)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: legacy hunter combat AI");
                UpdateInCombatAI_Hunter();
            }
        }

        // Preserve the inherited behavior exactly for legacy/fallback bots.
        // A loaded slate or validated spec policy already owns its 250 ms cast
        // lane, so autorepeat must not freeze this one-second doctrine/task/
        // movement/retreat spine.  The guard below skips autorepeat but still
        // blocks real casts and channels.
        if (!fastPolicy)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: legacy autorepeat, tick ends");
            return;
        }
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: casting, tick ends");
        return;
    }

    if (me->GetTargetGuid() == me->GetObjectGuid())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: clearing self-target");
        me->ClearTarget();
    }

    Unit* pVictim = me->GetVictim();

    // Prevent chasing stealthed target
    if (pVictim && !pVictim->IsVisibleForOrDetect(me, me, false))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: victim stealthed, dropping chase");
        me->AttackStop();
        me->ClearTarget();
        me->StopMoving();
        if (pVictim = SelectAttackTarget(pVictim))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: reacquired after stealth drop");
            AttackStart(pVictim);
        }
        return;
    }

    // Finish any task replacement that combat forced us to defer. The old generator is cleared
    // under both cancellation guards before the new task can scan, move, or publish an outcome.
    if (!me->IsInCombat() && m_suppressTaskDestInform)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: applying deferred task preemption OOC");
        StopMoving();

        // SET_TASK GRIND also deferred its initial approach so combat remained untouched. Own the
        // first path refusal here, exactly as the synchronous handler does, instead of letting a
        // random patrol report only autonomous telemetry and burn the correlated WAIT deadline.
        if (m_currentTask.type == TASK_GRIND &&
            me->GetDistance2d(m_currentTask.x, m_currentTask.y) > m_currentTask.radius)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: deferred GRIND replacement needs initial approach");
            if (!MovePointRun(AIBOT_POINT_GRIND_PATROL,
                    m_currentTask.x, m_currentTask.y, m_currentTask.z, false))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: deferred GRIND approach NOPATH, terminal fail");
                uint64 const taskCbt = m_currentTask.commandCbt;
                char eventData[256];
                snprintf(eventData, sizeof(eventData),
                    "dest_x=%.1f|dest_y=%.1f|dest_z=%.1f|reason=no_path|source=set_task_approach|start_isolated=%u",
                    m_currentTask.x, m_currentTask.y, m_currentTask.z,
                    IsStartIsolated() ? 1u : 0u);
                m_currentTask.Clear();
                BridgeSendEvent("MOVE_FAILED", eventData, taskCbt);
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: deferred GRIND approach issued, motion owns tick");
                return;
            }
        }
    }

    // --- Out of combat: eat/drink — [EAT-HYST] hysteresis latch (2026-07-05) ---
    // OLD: one-way gate — with an active task DrinkAndEat was reachable only below 40% HP /
    // 20% mana, so the loop RELEASED the bot back into the grind at 41%/21%: a tasked bot
    // lived permanently in the fight-losing band, every objective, every camp.
    // NEW: dipping below the floor latches recovery ON; the latch releases only at 90% HP /
    // 85% mana (the shape the C# rez heal proved at 0.95/0.85). The food aura breaks on
    // aggro but the LATCH survives combat, so a mid-eat defense fight resumes eating
    // afterward instead of chaining into the next pull half-dead.
    if (!me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: OOC eat-drink evaluation");
        bool hasActiveTask = (m_currentTask.type == TASK_MOVE_TO ||
                              m_currentTask.type == TASK_GRIND);
        bool const manaUser = (me->GetPowerType() == POWER_MANA);
        float const hp = me->GetHealthPercent();
        float const mp = manaUser ? me->GetPowerPercent(POWER_MANA) : 100.0f;

        if (hp < AIBOT_EAT_ENTER_HP || (manaUser && mp < AIBOT_EAT_ENTER_MANA))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-main: eat latch set, below floor", hp);
            m_eatRecoveryLatch = true;
        }

        if (m_eatRecoveryLatch && hp >= AIBOT_EAT_EXIT_HP && mp >= AIBOT_EAT_EXIT_MANA)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: eat latch released, recovered");
            m_eatRecoveryLatch = false;
        }

        if (!hasActiveTask || m_eatRecoveryLatch)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: eat gate open");
            if (DrinkAndEat())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: eating or drinking, tick ends");
                return;
            }
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: standing up");
        me->SetStandState(UNIT_STAND_STATE_STAND);
    }

    // [SUI] Combat cancels a commanded sheath; otherwise the ORDER_SHEATH
    // override holds and the auto-arm below must not fight it every tick.
    if (m_suiSheathOverride >= 0 && me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: combat cancels sheath override");
        m_suiSheathOverride = -1;
    }
    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted() &&
        m_suiSheathOverride < 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: auto-arming melee sheath");
        me->SetSheath(SHEATH_STATE_MELEE);
    }

    // --- Out of combat behavior ---
    if (!me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: out of combat behavior");
        // [PULL] Out of combat = no pull in flight. Clear any stale pull state so it can never
        // linger across an OOC gap into an unrelated defense fight. BeginPull (below, in the
        // TASK_GRIND engage) re-sets it later THIS tick when we actually pull, then returns.
        m_pullActive = false;

        if (CheckForUnreachableTarget())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: unreachable target handled, tick ends");
            return;
        }

        // A dead party member outranks buffing and wandering.
        if (SuiTryResurrectAlly())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: resurrecting an ally, tick ends");
            return;
        }

        UpdateOutOfCombatAI();

        if (m_isBuffing)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: buffing, tick ends");
            return;
        }

        // Can enter combat from UpdateOutOfCombatAI().
        if (me->IsInCombat())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: OOC AI entered combat, tick ends");
            return;
        }

        if (me->IsNonMeleeSpellCasted())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: casting after OOC AI, tick ends");
            return;
        }


        // --- Kill detection (must run before combat/OOC branching) ---
        if (!pVictim && m_lastVictimEntry != 0)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: victim gone, kill check");
            Creature* pKillCreature = nullptr;
            SuiKillVerdict const verdict =
                SuiClassifyVictimLoss(me, m_lastVictimEntry, m_lastVictimGuidLow, pKillCreature);

            if (verdict == SuiKillVerdict::Refuted)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: kill refuted, victim still alive", m_lastVictimEntry);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: victim (entry=%u guid=%u) is alive — evade/leash/disengage, no kill credit",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else if (verdict == SuiKillVerdict::TappedAway)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: kill refused, corpse tapped by another");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: mob (entry=%u guid=%u) tapped by another — skipping kill credit",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else
            {
                bool const confirmed = (verdict == SuiKillVerdict::Confirmed);
                if (confirmed)
                    CB_HITV(me->GetGUIDLow(), "cpp-main: kill credited, corpse confirmed", m_lastVictimEntry);
                else
                    CB_HITV(me->GetGUIDLow(), "cpp-main: kill unconfirmed, creature absent", m_lastVictimEntry);

                SendKillEvent(m_lastVictimEntry, m_lastVictimGuidLow, confirmed);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: kill detected (entry=%u guid=%u confirmed=%u)",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow, confirmed ? 1u : 0u);

                if (confirmed)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: queueing auto-loot");
                    m_lootTargetGuid = pKillCreature->GetObjectGuid();
                    m_lootTimer = urand(1000, 2500);
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-LOOT] %s: loot timer started (%dms) for %s (entry=%u)",
                        me->GetName(), m_lootTimer, pKillCreature->GetName(), m_lastVictimEntry);
                }

                if (m_currentTask.type == TASK_GRIND)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: grind kill bookkeeping");
                    // wolf-meat fix (2026-06-30): MatchesObjectiveEntry checks the primary
                    // dispatched creatureEntry OR any tied item-drop alternate, not exact
                    // equality alone — so a kill on a tied local sibling (e.g. Timber Wolf
                    // when the dispatched entry was Young Wolf) still advances THIS leg's
                    // killCount instead of silently not counting toward the objective.
                    bool matches = (m_currentTask.creatureEntry == 0 ||
                                    m_currentTask.MatchesObjectiveEntry(m_lastVictimEntry));
                    if (matches)
                    {
                        CB_HITV(me->GetGUIDLow(), "cpp-task: objective kill counted", m_currentTask.killCount);
                        m_currentTask.killCount++;
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT] %s: GRIND kill %d/%d (entry=%u)",
                            me->GetName(), m_currentTask.killCount, m_currentTask.killGoal, m_lastVictimEntry);
                        if (m_currentTask.killGoal > 0 &&
                            m_currentTask.killCount >= m_currentTask.killGoal)
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-task: grind goal reached, TASK_COMPLETE");
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                "[AIBOT] %s: GRIND task complete (%d/%d kills)",
                                me->GetName(), m_currentTask.killCount, m_currentTask.killGoal);
                            BridgeSendEvent("TASK_COMPLETE", "GRIND finished", m_currentTask.commandCbt);
                            m_currentTask.Clear();
                        }
                    }
                }
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
        }
        else if (pVictim && pVictim->IsCreature())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: caching victim for kill detection");
            m_lastVictimEntry = static_cast<Creature*>(pVictim)->GetEntry();
            m_lastVictimGuidLow = pVictim->GetGUIDLow();
        }

        // A manual NPC approach temporarily owns OOC movement without stealing the autonomous
        // task or its cbt. Natural arrival clears it in MovementInform; combat or another mover may
        // interrupt the spline, in which case resume here before task/follow logic can overwrite it.
        if (!m_pendingInteractNpcGuid.IsEmpty())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: pending NPC interaction owns OOC movement");
            if (me->IsMoving())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: NPC interaction approach still moving");
                return;
            }

            uint64 const interactCbt = m_pendingInteractNpcCbt;
            Creature* pNpc = me->GetMap()->GetCreature(m_pendingInteractNpcGuid);
            if (!pNpc)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: pending NPC interaction target disappeared");
                m_pendingInteractNpcGuid.Clear();
                m_pendingInteractNpcCbt = 0;
                BridgeSendEvent("NPC_INTERACT_FAIL", "reason=npc_lost|source=interact_npc", interactCbt);
                return;
            }

            float const npcDist = me->GetDistance(pNpc);
            if (npcDist <= 10.0f)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-task: interrupted NPC approach is now in range", npcDist);
                m_pendingInteractNpcGuid.Clear();
                m_pendingInteractNpcCbt = 0;
                me->SetFacingToObject(pNpc);
                BridgeSendEvent("NPC_INTERACT", pNpc->GetName(), interactCbt);
                return;
            }

            float nx, ny, nz;
            pNpc->GetContactPoint(me, nx, ny, nz);
            if (!MovePointRun(AIBOT_POINT_INTERACT_NPC, nx, ny, nz, false))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: resumed NPC approach NOPATH, terminal fail");
                m_pendingInteractNpcGuid.Clear();
                m_pendingInteractNpcCbt = 0;
                char eventData[192];
                snprintf(eventData, sizeof(eventData),
                    "reason=no_path|source=interact_npc|dest_x=%.1f|dest_y=%.1f|dest_z=%.1f",
                    nx, ny, nz);
                BridgeSendEvent("NPC_INTERACT_FAIL", eventData, interactCbt);
            }
            return;
        }

        // ── [PLAYERPARTY] Escort mode (2026-07-07): a REAL player leads this group ──
        // The human is the coordinator; C++ owns the whole behaviour. Engage the doctrine's
        // party focus (boss's victim → boss's attacker → a party member's attacker → sticky;
        // NEVER a self-initiated pull), else keep formation on the boss. The return makes the
        // task machinery unreachable while escorting — no grind dispatch, no MOVE_TO resume,
        // no patrol, no wander — so a stale C# task from before the invite simply idles out
        // (C# stands down to Goal.Idle off the pparty STATE echo and SET_TASK IDLEs anyway).
        // Placed AFTER kill-detection so escort kills still fire KILL events + loot timers,
        // and AFTER the [EAT-HYST] block so a companion still sits to eat between fights.
        if (m_doctrineKind == DoctrineKind::PlayerParty)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: escort mode tick");
            if (Unit* pEscortTarget = m_doctrine->AcquireTarget(*this))   // cb:fold decl-in-condition artifact, body probed
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: party focus acquired, engaging");
                // A live party focus OWNS this tick. AttackStart() returns false once we are
                // ALREADY attacking this victim (Unit::Attack no-ops on the same target), so a
                // false return is success ("already engaged"), NOT "nothing to fight". We must
                // return either way -- falling through on false let DoPartyFollow() re-issue
                // MoveFollow(boss) every steady-state tick, overriding the active MoveChase and
                // yanking the bot back the instant it got past AIBOT_PARTY_FOLLOW_DIST+1 (4yd)
                // from the boss: melee companions engaged, turned around at ~4yd, and only ever
                // hit mobs that wandered into melee (Nico, 2026-08-19). The tick-1 MoveChase
                // persists on a false return and keeps closing to the boss's target.
                AttackStart(pEscortTarget);
                return;
            }

            // [HUB-ERRAND] YIELD (2026-07-08 §3): an active MOVE_TO while in a human's party
            // is by construction a deliberate C#-issued errand leg (the GoalSelector's
            // player-party hold stands everything else down; C# never sends otherwise), so
            // fall THROUGH to the task machinery below and let the leg walk/resume/arrive
            // exactly like a solo MOVE_TO. Doctrine stays PlayerParty throughout — a mob
            // jumping the bot at the vendor still gets the full escort ladder above. Every
            // OTHER task type keeps the pre-yield stand-down (a stale pre-invite GRIND still
            // idles out as before). Known cosmetic: 1-2 ticks of turn-toward-boss between
            // errand legs (the task clears on TASK_COMPLETE, follow resumes until the next
            // command lands).
            // [SUI-DIAG] Why is this member not forming up? Every "nobody follows me" report
            // ends at one of three answers and none of them were observable: a stale
            // TASK_MOVE_TO diverts past the follow call entirely, an unlinked member stands its
            // ground by design, or the boss simply did not resolve. Throttled to 5s per bot,
            // and only reachable in PlayerParty doctrine, so it is bounded to the human party
            // rather than the whole fleet.
            if (m_suiFollowDiagTimer > AIBOT_UPDATE_INTERVAL)
                m_suiFollowDiagTimer -= AIBOT_UPDATE_INTERVAL;   // cb:fold logging-only follow diagnostic cadence
            else
            {   // cb:fold logging-only follow diagnostic
                m_suiFollowDiagTimer = 5000;
                Player* pDiagBoss = FindEscortBoss();
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[SUI-FOLLOW] %s: task=%u unlinked=%u commanded=%u boss=%s dist=%.1f",
                    me->GetName(), uint32(m_currentTask.type), m_suiUnlinked ? 1u : 0u,
                    SuiPossess::IsCommandedFromFreeView(me) ? 1u : 0u,
                    pDiagBoss ? pDiagBoss->GetName() : "(none)",
                    pDiagBoss ? me->GetDistance(pDiagBoss) : -1.0f);
            }

            if (m_currentTask.type != TASK_MOVE_TO)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: escort follow, no errand leg");
                DoPartyFollow();
                return;
            }
            // fall through — the TASK_MOVE_TO resume/arrival machinery below drives the leg
        }

        // --- TASK_GRIND: proactive pull or patrol ---
        if (m_currentTask.type == TASK_GRIND)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: grind dispatch");
            // [PULLGATE] Fight-initiation floor (solo, 2026-07-05). Never START a fight
            // under-resourced: below the floor, latch recovery and stand down — and do NOT
            // patrol (DoGrindPatrol random-hops INTO the field, i.e. body-pulling at low
            // HP — the exact death class this closes). The eat block above owns every
            // subsequent tick until the latch releases at 90/85; recovery (~10-30s) is well
            // inside the C# 120s no-kill deadline, so no false shelves. Placed BEFORE
            // AcquireTarget so neither the pull nor the freeze/self-unstick escalation can
            // run while weak. TeamAuto/Directed pass through untouched — the group owns its
            // own recovery (GroupDefend / guard-heal / the chain 40/40 gate).
            if (m_doctrineKind == DoctrineKind::Solo && !PullReady())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: pull gate closed, recovering first");
                if (!m_eatRecoveryLatch)
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,   // cb:fold logging-only pull-gate report
                        "[AIBOT-PULLGATE] %s: refusing new pull at hp=%.0f%% mp=%.0f%% — recovering to %.0f/%.0f",
                        me->GetName(), me->GetHealthPercent(),
                        me->GetPowerType() == POWER_MANA ? me->GetPowerPercent(POWER_MANA) : 100.0f,
                        AIBOT_EAT_EXIT_HP, AIBOT_EAT_EXIT_MANA);
                m_eatRecoveryLatch = true;
                return;
            }

            // [SPREAD] Anti-convergence (FINDING_005): don't add to a dogpile. For a SOLO FILLER
            // grind (entry==0), if AIBOT_SPREAD_BOT_CAP+ other independent bots are already grinding
            // within AIBOT_SPREAD_RADIUS, patrol out instead of engaging — landing no kills here lets
            // the C# no-progress breaker relocate us to a less-crowded camp, so the fleet spreads
            // instead of 8 bots piling one camp into a 48-attacker overpull. Quests (shared mobs) and
            // groups (meant to stack + focus-fire) are exempt; the grid-search runs only for the solo
            // filler case. m_spreadDeferred logs the defer once per episode, not every tick.
            uint32 const nearbyBots =
                (m_doctrineKind == DoctrineKind::Solo && m_currentTask.creatureEntry == 0 && !me->GetGroup())
                ? CountNearbyBots(me, AIBOT_SPREAD_RADIUS) : 0;
            if (nearbyBots >= AIBOT_SPREAD_BOT_CAP)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-task: spread cap hit, patrolling out", nearbyBots);
                if (!m_spreadDeferred)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: spread defer episode begins");
                    m_spreadDeferred = true;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-SPREAD] %s: %u other bots grinding within %.0fyd (cap %u) — patrolling out to spread",
                        me->GetName(), nearbyBots, AIBOT_SPREAD_RADIUS, AIBOT_SPREAD_BOT_CAP);
                }
                DoGrindPatrol();
                return;
            }
            m_spreadDeferred = false;

            // [DOCTRINE] Acquisition + pull discipline are the doctrine's; the freeze / GRIND_BLOCKED
            // / patrol bookkeeping stays here in the spine. AcquireTarget returns the mob to pull, or
            // nullptr to hold/patrol. A nullptr that is a DELIBERATE TeamAuto wait-for-anchor hold (B3
            // — a follower letting the anchor pull first) is flagged by HoldingForTeam(): patrol
            // WITHOUT a freeze tick, so a hold can never bump m_grindFreezeStreak or fire a false
            // GRIND_BLOCKED|no_target. Solo returns its priority scan; the anchor / filler-detour /
            // dwell-expired follower all fall through to a real pull. (The whole group-fight decision
            // — resolver-first, the B3 dwell + its counter, sticky — now lives in AiBotDoctrineTeam.)
            if (Unit* pGrindTarget = m_doctrine->AcquireTarget(*this))   // cb:fold decl-in-condition artifact, body probed
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: grind target acquired");
                if (m_doctrine->HoldPull(*this, pGrindTarget))
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-task: pull held, overpull cap", m_grindFreezeStreak);
                    ++m_grindFreezeStreak;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-OVERPULL] %s: holding (%u/%u) — target %s in %u-deep cluster (solo cap %u) [will self-unstick]",
                        me->GetName(), m_grindFreezeStreak, AIBOT_GRIND_FREEZE_DWELL,
                        pGrindTarget->GetName(),
                        CountNearbyHostiles(pGrindTarget, AIBOT_PULL_DENSITY_RADIUS), AIBOT_OVERPULL_SOLO);

                    if (m_grindFreezeStreak >= AIBOT_GRIND_FREEZE_DWELL)
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-task: freeze escape, self-unstick pull");
                        m_grindFreezeStreak = 0;

                        // Self-unstick: the objective grind USED to hand back GRIND_BLOCKED|overpull_dwell
                        // so C# could "resync + re-derive" — but the brain ALWAYS answered with a forced
                        // single-kill detour + re-issue of the SAME objective, re-freezing on the SAME
                        // dense field (the kobold-camp livelock). A dense field of QUEST mobs is exactly
                        // where we want to grind, so take the pull right here: HoldPull handed us the
                        // least-clustered candidate, HandleOverpullRetreat is the in-combat backstop if too
                        // many pile on, and a field that genuinely can't be killed still trips the C# 120s
                        // no-kill deadline → durable shelve.
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-OVERPULL] %s: freeze-escape — self-unstick pull of %s (bypassing cap)",
                            me->GetName(), pGrindTarget->GetName());
                        AttackStart(pGrindTarget);
                        return;
                    }

                    DoGrindPatrol();
                    return;
                }

                // Pull cleared the cap → engage with a real PULL (FINDING_005): tag it, then drag
                // it back to open ground so the camp's neighbours don't dogpile. BeginPull owns the
                // approach + retreat; HandlePullRetreat drives it on the in-combat ticks that follow.
                m_grindFreezeStreak = 0;
                BeginPull(pGrindTarget);
                return;
            }

            // AcquireTarget returned nullptr. If that was a deliberate wait-for-anchor hold (B3),
            // patrol and DO NOT count a freeze — the doctrine is choosing to wait, not stuck.
            if (m_doctrine->HoldingForTeam())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: holding for anchor, patrolling");
                DoGrindPatrol();
                return;
            }

            // Genuinely no valid target this tick. Only an OBJECTIVE grind (entry!=0) hands back (quest
            // mobs all dead/tapped → C# resyncs + detours). An entry==0 grind just patrols: a filler with
            // no mobs is GrindPlanner's no-kills→reselect job, and a detour rides its own WAIT deadline.
            if (m_currentTask.creatureEntry != 0)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: objective grind, no valid target");
                if (++m_grindFreezeStreak >= AIBOT_GRIND_FREEZE_DWELL)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: no target dwell expired, GRIND_BLOCKED");
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-OVERPULL] %s: freeze-escape — no valid target %u ticks, signaling GRIND_BLOCKED",
                        me->GetName(), m_grindFreezeStreak);
                    m_grindFreezeStreak = 0;
                    char ev[160];
                    snprintf(ev, sizeof(ev),
                        "x=%.1f|y=%.1f|z=%.1f|entry=%u|reason=no_target",
                        m_currentTask.x, m_currentTask.y, m_currentTask.z,
                        m_currentTask.creatureEntry);
                    BridgeSendEvent("GRIND_BLOCKED", ev, m_currentTask.commandCbt);
                }
            }

            DoGrindPatrol();
            return;
        }

       // --- TASK_MOVE_TO: resume movement after interruption ---
        if (m_currentTask.type == TASK_MOVE_TO)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: move task tick");
            // ── §4 approach scan ──
            if (m_currentTask.creatureEntry != 0 && m_approachScanTimer == 0)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: approach scan firing");
                m_approachScanTimer = urand(2000, 3000);
                if (Unit* pMob = ScanApproachTarget())   // cb:fold decl-in-condition artifact, body probed
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-task: approach scan hit, grind handoff", m_currentTask.creatureEntry);
                    // [PULLGATE] (solo, 2026-07-05) Arriving at the field under-resourced:
                    // the scan just proved live mobs are AHEAD, so stop HERE — still outside
                    // the camp — latch recovery, and eat before the engage. The MOVE_TO
                    // resume logic below picks the journey back up once the latch releases,
                    // the scan re-fires at full HP, and the engage proceeds as normal.
                    if (m_doctrineKind == DoctrineKind::Solo && !PullReady())
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-task: field ahead but weak, stop and recover");
                        if (!m_eatRecoveryLatch)
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,   // cb:fold logging-only pull-gate report
                                "[AIBOT-PULLGATE] %s: field ahead (%s, %.0fyd) but hp=%.0f%% mp=%.0f%% — stopping to recover first",
                                me->GetName(), pMob->GetName(), me->GetDistance(pMob),
                                me->GetHealthPercent(),
                                me->GetPowerType() == POWER_MANA ? me->GetPowerPercent(POWER_MANA) : 100.0f);
                        StopMoving();
                        m_eatRecoveryLatch = true;
                        return;
                    }

                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: approach scan hit %s (entry=%u, %.0fyd) — handoff to GRIND",
                        me->GetName(), pMob->GetName(), m_currentTask.creatureEntry, me->GetDistance(pMob));
                    ConvertMoveToGrindInPlace();

                    // [DOCTRINE] The scan hit is the OTHER "group arrives at a camp" moment. A
                    // TeamAuto follower piles the ANCHOR'S resolved victim if there is one (via the
                    // doctrine's target authority), or HOLDS if the anchor isn't fighting yet — the
                    // grind dispatch's wait-for-anchor owns the wait from the very next tick. Solo /
                    // anchor: engage the scan hit under the pull veto, byte-for-byte the old engage.
                    Unit* pEngage = pMob;
                    if (m_combatDirective.IsActive() &&
                        m_combatDirective.anchorGuidLow != me->GetGUIDLow())
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-task: follower at scan hit, deferring to anchor");
                        if (Unit* pAssist = m_doctrine->MaintainTarget(*this, nullptr))   // cb:fold decl-in-condition artifact, body probed
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-task: assisting anchor victim");
                            pEngage = pAssist;
                        }
                        else
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-task: anchor not fighting, holding");
                            return;   // follower, anchor not fighting yet → hold (grind gate next tick)
                        }
                    }

                    // [OVERPULL] Same veto as the grind dispatch: convert to a local grind but
                    // don't dive a dense pack solo. The grind dispatch re-gates next tick.
                    if (!m_doctrine->HoldPull(*this, pEngage))
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-task: engaging scan hit");
                        AttackStart(pEngage);
                    }
                    return;
                }
            }

            // Still actively walking — let the motion generator finish
            if (me->IsMoving())
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: still walking, motion owns leg");
                return;
            }

            // Not moving. Either we arrived or we got interrupted.
            float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);

            if (dist > 3.0f)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-task: not arrived, resuming journey", dist);
                // Haven't arrived — resume movement.
                if (!m_pathWaypoints.empty() &&
                    m_pathIndex < (uint32)m_pathWaypoints.size() - 1)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: resuming chunked path");
                    // Resume the current leg's remaining chunks
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: resuming chunked path from waypoint %u/%u",
                        me->GetName(), m_pathIndex, (uint32)m_pathWaypoints.size());
                    StartNextPathChunk();
                }
                else
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: leg exhausted, next leg to dest");
                    // Current leg exhausted (or interrupted with no stored path) —
                    // compute the next leg toward the true dest and walk it.
                    MoveToDestination(m_currentTask.x, m_currentTask.y, m_currentTask.z);
                }
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: close enough to dest");
                // Close enough to the destination.
                // §4: an enriched objective MOVE_TO arrived at the deep coord with no
                // scan hit — grind here in place rather than emit a false "arrived"
                // TASK_COMPLETE (merged step: that would mean objective-done, zero kills).
                if (m_currentTask.creatureEntry != 0)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: objective auto-arrival, grind in place");
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: objective MOVE_TO auto-arrived (dist=%.1f) — GRIND in place",
                        me->GetName(), dist);
                    ConvertMoveToGrindInPlace();
                }
                else
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: auto-arrived at dest, TASK_COMPLETE");
                    // Stamp the exact arrival coord so C# refreshes ctx.Pos immediately
                    // (no 5s STATE-cycle stale read driving a needless MOVE_TO re-issue).
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: MOVE_TO auto-arrived (dist=%.1f, threshold=3.0)",
                        me->GetName(), dist);
                    char arrBuf[96];
                    snprintf(arrBuf, sizeof(arrBuf), "MOVE_TO arrived|x=%.1f|y=%.1f|z=%.1f",
                             me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
                    BridgeSendEvent("TASK_COMPLETE", arrBuf, m_currentTask.commandCbt);
                    m_currentTask.Clear();
                    ClearStoredPath();
                }
            }
            return;
        }

        // [SUI] Party spacing before any idle stroll: a bot standing inside a
        // fellow party member steps clear first. Runs for RTS-held and
        // conscripted bots too (a group ordered to one point is exactly when
        // they pile up), which is why it sits ABOVE DoRandomWander's bails.
        if (DoPartyUnstack())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: party unstack owns tick");
            return;
        }

        // --- Default idle wander ---
        DoRandomWander();
        return;
    }

    // [PULL] Proactive pull-and-retreat — drag a freshly-tagged mob to open ground before the
    // fight settles, so only it (+ tight neighbours) engage. Runs ahead of the stalemate/overpull
    // handlers and owns the tick while active. (FINDING_005)
    if (HandlePullRetreat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: pull retreat owns tick");
        return;
    }

    // [ADDED] Combat stalemate breaker — shared machinery, explicitly owned by doctrine.
    if (m_doctrine->UseStalemateBreaker() && HandleCombatStalemate())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: stalemate breaker owns tick");
        return;
    }

    // [OVERPULL] Retreat only when this doctrine permits abandoning the current fight.
    if (m_doctrine->UseOverpullRetreat() && HandleOverpullRetreat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: overpull retreat owns tick");
        return;
    }

    // Cache victim info before combat system clears it
    if (pVictim && pVictim->IsCreature())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: caching victim in combat");
        m_lastVictimEntry = static_cast<Creature*>(pVictim)->GetEntry();
        m_lastVictimGuidLow = pVictim->GetGUIDLow();
    }

    // --- Tap-respect: doctrine decides whether foreign ownership may veto this fight. ---
    if (m_doctrine->UseTapRespect() && pVictim && pVictim->IsCreature())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: tap-respect check");
        Creature* pVicCre = static_cast<Creature*>(pVictim);
        if (pVicCre->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) &&
            !pVicCre->IsTappedBy(me))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: victim tapped by another");
            // Is the tapper in my group? If so it's a focus-fire assist, not a steal.
            bool tapperInMyGroup = false;
            if (Group* pGroup = me->GetGroup())   // cb:fold decl-in-condition artifact, body probed
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: checking tapper group membership");
                if (Player* pTapper = pVicCre->GetLootRecipient())   // cb:fold decl-in-condition artifact, body probed
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: tapper membership resolved");
                    tapperInMyGroup = pGroup->IsMember(pTapper->GetObjectGuid());
                }
            }

            // [PARTY-TAP-EXEMPT] (2026-07-16) The human's fight is authoritative — even on a
            // stranger's tap. If any REAL player in my group is currently attacking this mob,
            // or the mob is beating on a group member (or a member's pet), disengaging is
            // abandoning MY party's fight, not declining a steal: the old gate AttackStopped
            // here while the doctrine ladder recommitted the same mob next tick — a 1 Hz
            // engage/stop thrash that reads at the keyboard as "barely pushing buttons".
            // The human owns the ninja-etiquette call; companions back his choice. Mobs no
            // group player is on remain vetoed (companions still don't initiate steals).
            bool partyOwnsThisFight = false;
            if (!tapperInMyGroup)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: tapper outside group, exemption check");
                if (Group* pGroup = me->GetGroup())   // cb:fold decl-in-condition artifact, body probed
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: scanning group for human on this mob");
                    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* pMember = itr->getSource();
                        if (!pMember || pMember == me || !pMember->IsInWorld())
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-main: tap scan member skipped, null or self");
                            continue;
                        }
                        WorldSession* pSess = pMember->GetSession();
                        if (!pSess || pSess->GetBot())
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-main: tap scan member is bot session");
                            continue;   // only a HUMAN's choice grants the exemption
                        }
                        if (pMember->GetVictim() == pVicCre)
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-main: human fighting this mob, exemption granted");
                            partyOwnsThisFight = true;
                            break;
                        }
                    }
                }
                if (!partyOwnsThisFight)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: checking mob victim ownership");
                    if (Unit* pMobVictim = pVicCre->GetVictim())   // cb:fold decl-in-condition artifact, body probed
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-main: mob has victim, resolving owner");
                        if (Player* pMobVictimOwner = pMobVictim->GetCharmerOrOwnerPlayerOrPlayerItself())   // cb:fold decl-in-condition artifact, body probed
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-main: mob victim owner resolved");
                            if (pMobVictimOwner == me ||
                                (me->GetGroup() && me->GetGroup()->IsMember(pMobVictimOwner->GetObjectGuid())))
                            {
                                CB_HIT(me->GetGUIDLow(), "cpp-main: mob attacking my party, exemption granted");
                                partyOwnsThisFight = true;
                            }
                        }
                    }
                }
                if (partyOwnsThisFight)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: exemption granted, veto healed");
                    m_combatIgnore.erase(pVicCre->GetGUIDLow());   // heal any earlier wrong veto
                }
            }

            if (!tapperInMyGroup && !partyOwnsThisFight)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: disengaging tapped mob, short-ignore");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: victim %u (guid=%u) tapped by another — disengaging (not mine)",
                    me->GetName(), pVicCre->GetEntry(), pVicCre->GetGUIDLow());

                me->AttackStop();
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: stopping chase on disengage");
                    StopMoving();
                }
                me->ClearTarget();

                // Short-ignore so Select*Target / SelectGrindTarget don't instantly re-acquire
                // this same tapped mob from the hostile-ref list next tick.
                m_combatIgnore[pVicCre->GetGUIDLow()] = AIBOT_TAPPED_IGNORE_MS;
                return;
            }
        }
    }

    // --- In combat: target validation + switch ---
    if (!pVictim || !IsValidHostileTarget(pVictim) ||
        !pVictim->IsWithinDist(me, VISIBILITY_DISTANCE_SMALL))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: victim invalid or lost, reselect path");
        bool victimDied = (pVictim && pVictim->IsDead() && pVictim->IsCreature());
        if (!victimDied && !pVictim && m_lastVictimEntry != 0 && !me->IsInCombat())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: inferring victim death");
            victimDied = true;
        }

        if (victimDied)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: combat victim died");
            uint32 killedEntry = pVictim ? static_cast<Creature*>(pVictim)->GetEntry() : m_lastVictimEntry;
            uint32 killedGuid = pVictim ? pVictim->GetGUIDLow() : m_lastVictimGuidLow;
            Creature* pKillCreature2 = nullptr;
            SuiKillVerdict const verdict2 = pVictim
                ? SuiClassifyCorpse(me, static_cast<Creature*>(pVictim))
                : SuiClassifyVictimLoss(me, m_lastVictimEntry, m_lastVictimGuidLow, pKillCreature2);
            if (pVictim)
                pKillCreature2 = static_cast<Creature*>(pVictim);

            if (verdict2 == SuiKillVerdict::Refuted)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-main: combat kill refuted, victim still alive", killedEntry);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: combat victim (entry=%u guid=%u) is alive — no kill credit",
                    me->GetName(), killedEntry, killedGuid);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else if (verdict2 == SuiKillVerdict::TappedAway)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: combat kill tapped, no credit");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: mob (entry=%u guid=%u) tapped by another — skipping kill credit (combat path)",
                    me->GetName(), killedEntry, killedGuid);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else
            {
                bool const confirmed2 = (verdict2 == SuiKillVerdict::Confirmed);
                if (confirmed2)
                    CB_HITV(me->GetGUIDLow(), "cpp-main: combat kill credited, corpse confirmed", killedEntry);
                else
                    CB_HITV(me->GetGUIDLow(), "cpp-main: combat kill unconfirmed, creature absent", killedEntry);

                SendKillEvent(killedEntry, killedGuid, confirmed2);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: combat kill detected (entry=%u guid=%u confirmed=%u)",
                    me->GetName(), killedEntry, killedGuid, confirmed2 ? 1u : 0u);

                // Queue auto-loot only when the corpse was actually confirmed.
                if (confirmed2)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-main: queueing loot, combat path");
                    m_lootTargetGuid = pKillCreature2->GetObjectGuid();
                    m_lootTimer = urand(1000, 2500);
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-LOOT] %s: loot timer started (%dms) for entry=%u (in-combat path)",
                        me->GetName(), m_lootTimer, killedEntry);
                }

                if (m_currentTask.type == TASK_GRIND)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: grind kill bookkeeping, combat path");
                    // wolf-meat fix (2026-06-30): same MatchesObjectiveEntry widening as the
                    // OOC kill-detect path above — primary or any tied alternate counts.
                    bool matches = (m_currentTask.creatureEntry == 0 ||
                                    m_currentTask.MatchesObjectiveEntry(killedEntry));
                    if (matches)
                    {
                        CB_HITV(me->GetGUIDLow(), "cpp-task: objective kill counted, combat path", m_currentTask.killCount);
                        m_currentTask.killCount++;
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT] %s: GRIND kill %d/%d (entry=%u)",
                            me->GetName(), m_currentTask.killCount, m_currentTask.killGoal, killedEntry);
                        if (m_currentTask.killGoal > 0 &&
                            m_currentTask.killCount >= m_currentTask.killGoal)
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-task: grind goal reached, combat path");
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                "[AIBOT] %s: GRIND task complete (%d/%d kills)",
                                me->GetName(), m_currentTask.killCount, m_currentTask.killGoal);
                            BridgeSendEvent("TASK_COMPLETE", "GRIND finished", m_currentTask.commandCbt);
                            m_currentTask.Clear();
                        }
                    }
                }

                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
        }

        // [DOCTRINE] Target authority on reselect (the flap fix). TeamAuto returns the anchor's
        // mob and the follower COMMITS to it — even if it's the same mob we just "lost" for
        // drifting past VISIBILITY_DISTANCE_SMALL (we were lagging; AttackStart re-chases and the
        // ChaseMovementGenerator closes the gap). No pExcept exclusion of the anchor's mob, and no
        // "not hitting me → drop" bail for a mob the anchor is tanking — those two, plus the SMALL
        // drop, are what produced the A/B/A/B flap. Solo / defer → nullptr → the legacy solo pick
        // + drop below run byte-for-byte. (Kill-credit above already ran regardless.)
        if (Unit* pFocus = m_doctrine->MaintainTarget(*this, pVictim))   // cb:fold decl-in-condition artifact, body probed
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: doctrine focus on reselect, committing");
            AttackStart(pFocus);
            return;
        }

        if (Unit* pNewVictim = SelectAttackTarget(pVictim))   // cb:fold decl-in-condition artifact, body probed
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: solo reselect found target");
            if (pVictim != pNewVictim)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: switching to new victim");
                AttackStart(pNewVictim);
                return;
            }
        }

        if (me->GetVictim() &&
           (me != me->GetVictim()->GetVictim()))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: victim not fighting me, dropping");
            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: stopping chase, victim dropped");
                StopMoving();
            }
            return;
        }
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: victim valid, holding course");
        // [DOCTRINE] Mid-combat convergence via the single target authority. TeamAuto returns the
        // anchor's mob; a follower SWITCHES to it and commits. There is NO VISIBILITY_DISTANCE_SMALL
        // gate here anymore — that gate (narrower than the doctrine's own NORMAL range check) is
        // exactly what made a lagging follower flap A/B/A/B, switching to a mob the outer validation
        // would reject the very next tick. Committing without it, the ChaseMovementGenerator closes
        // the gap. Solo / defer → nullptr → hold course exactly as before; the anchor gets nullptr
        // from the resolver (self), so it is untouched and the team converges on IT.
        if (Unit* pFocus = m_doctrine->MaintainTarget(*this, pVictim))   // cb:fold decl-in-condition artifact, body probed
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: mid-combat convergence check");
            if (pFocus != pVictim && AttackStart(pFocus))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-main: converging to anchor victim");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-DOCTRINE] %s: converge %s -> %s (%s)",
                    me->GetName(), pVictim->GetName(), pFocus->GetName(), m_doctrine->Name());
                return;
            }
        }

        if (MaintainTankFacing(pVictim))
            return;

        if (!me->HasInArc(pVictim, 2 * M_PI_F / 3) && !me->IsMoving())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: turning to face victim");
            me->SetInFront(pVictim);
            me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
        }

        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
           (m_role != ROLE_HEALER) &&
            IsValidHostileTarget(pVictim) &&
            AttackStart(pVictim))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: melee re-engage, tick ends");
            return;
        }
    }

    // Fast policies already ran ahead of this behaviour tick.  Do not dispatch
    // them again here at 1 Hz (which otherwise creates duplicate same-frame tries).
    if (me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-main: combat AI dispatch");
        // Druid combat rez: Rebirth a fallen party member before the rotation.
        if (SuiTryResurrectAlly())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: combat rez cast, tick ends");
            return;
        }
        if (!HasFastCombatPolicy())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: legacy combat AI tick");
            UpdateInCombatAI();
        }
        else if (me->GetVictim())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-main: fast policy, trinkets only");
            UseTrinketEffects();
        }
    }
}
