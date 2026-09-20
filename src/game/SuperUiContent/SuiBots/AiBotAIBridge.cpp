#include "SuiCommanderRaid.h"
/*
 * AiBotAIBridge.cpp — the C# <-> C++ TCP bridge for the autonomous AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the bridge domain:
 *   - the non-blocking TCP transport (connect / disconnect / send / flush / recv)
 *   - HELLO / STATE / EVENT senders
 *   - the file-local minimal JSON extractors (JsonExtractFloat/Int/UInt64/String)
 *   - BridgeProcessLine dispatch + every BridgeHandle* command handler
 *   - the C++→C# event senders (SendKillEvent / SendQuestUpdateEvent / SendLevelUpEvent /
 *     SendChatRecvEvent)
 *
 * The JsonExtract* statics are defined before BridgeProcessLine and every handler that
 * uses them (statics are file-local). All AiBotAI methods link across the sibling TUs;
 * cross-TU members called from here (MoveToDestination / ReGroundZ / ClearStoredPath /
 * StopMoving from Movement, ChooseQuestReward / TryAutoEquip(Bags) from Loot) are defined
 * in those siblings.
 */

#include "AiBotAIMain.h"
#include "AiBotTalents.h"
#include "SuiHero.h"
#include "SuiPossess.h"      // [SUI] free-view command waiver on the possessed drop
#include "AiBotCircuit.h" // [CIRCUIT] probe macros + batch flush (CIRCUIT_BOARD.md)
#include "Player.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cctype>   // tolower — [FOLLOW-CMD] lowercases the stored escort-override name
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

// [WIN-COMPAT] MSVC/Windows lacks the POSIX ssize_t and strtok_r used by the TCP
// bridge below. Map them to their Windows equivalents so this TU compiles on MSVC.
#ifdef _MSC_VER
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif
#ifdef _WIN32
#define strtok_r strtok_s
#endif

// ============================================================
// TCP BRIDGE — Phase 2
// ============================================================

void AiBotAI::BridgeConnect()
{
    if (m_bridgeConnected)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: connect skipped, already connected");
        return;
    }

    m_bridgeSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_bridgeSocket == BRIDGE_INVALID_SOCKET)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: socket create failed");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: socket() failed", me->GetName());
        return;
    }
 
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BRIDGE_PORT);
    inet_pton(AF_INET, BRIDGE_HOST, &addr.sin_addr);
 
    if (connect(m_bridgeSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: connect failed, retry after ms", m_bridgeReconnectDelay);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: connect failed (will retry in %ums)",
            me->GetName(), m_bridgeReconnectDelay);
        BRIDGE_CLOSE_SOCKET(m_bridgeSocket);
        m_bridgeSocket = BRIDGE_INVALID_SOCKET;
        m_bridgeReconnectTimer = m_bridgeReconnectDelay;
        // Exponential backoff
        m_bridgeReconnectDelay = std::min(m_bridgeReconnectDelay * 2, (uint32)BRIDGE_RECONNECT_MAX);
        return;
    }
 
    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(m_bridgeSocket, FIONBIO, &mode);
#else
    int flags = fcntl(m_bridgeSocket, F_GETFL, 0);
    fcntl(m_bridgeSocket, F_SETFL, flags | O_NONBLOCK);
#endif
 
    m_bridgeConnected = true;
    m_bridgeHelloSent = false;
    m_bridgeReconnectDelay = BRIDGE_RECONNECT_BASE;
    m_bridgeRecvLen = 0;
    m_bridgeSendBuf.clear();   // Session 36: never carry a stale queue across connections
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: connected to %s:%d",
        me->GetName(), BRIDGE_HOST, BRIDGE_PORT);
}


void AiBotAI::BridgeDisconnect()
{
    if (m_bridgeSocket != BRIDGE_INVALID_SOCKET)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: socket closed, disconnected");
        BRIDGE_CLOSE_SOCKET(m_bridgeSocket);
        m_bridgeSocket = BRIDGE_INVALID_SOCKET;
    }
    m_bridgeConnected = false;
    m_bridgeHelloSent = false;
    m_bridgeRecvLen = 0;
    m_bridgeSendBuf.clear();   // Session 36: drop queued bytes; they belong to the dead socket
}

void AiBotAI::BridgeSend(const char* json)
{
    if (!m_bridgeConnected || m_bridgeSocket == BRIDGE_INVALID_SOCKET)
        return; // cb:fold socket plumbing

    size_t len = strlen(json);
 
    // Safety valve: if C# stops reading, the kernel send buffer fills and our
    // queue grows. Past the cap, drop the OLDEST data (resynced to a line
    // boundary so we never resume mid-message) rather than grow unbounded.
    if (m_bridgeSendBuf.size() + len + 1 > BRIDGE_SEND_BUF_MAX)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: send queue overflow, dropping oldest", m_bridgeSendBuf.size());
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: send queue over %u bytes — dropping oldest (C# slow or down?)",
            me->GetName(), (uint32)BRIDGE_SEND_BUF_MAX);
        m_bridgeSendBuf.erase(0, m_bridgeSendBuf.size() / 2);
        size_t nl = m_bridgeSendBuf.find('\n');
        if (nl != std::string::npos)
            m_bridgeSendBuf.erase(0, nl + 1);   // cb:fold buffer realign detail (next whole line)
        else
            m_bridgeSendBuf.clear(); // cb:fold buffer realign detail
    }
 
    // Queue the whole line. JSON-lines protocol → one '\n' terminator.
    m_bridgeSendBuf.append(json, len);
    m_bridgeSendBuf.push_back('\n');
 
    // Common case: drain right away so event latency stays low.
    BridgeFlush();
}

void AiBotAI::BridgeFlush()
{
    if (!m_bridgeConnected || m_bridgeSocket == BRIDGE_INVALID_SOCKET)
        return; // cb:fold socket plumbing

    while (!m_bridgeSendBuf.empty())
    {
        ssize_t sent = send(m_bridgeSocket,
                            m_bridgeSendBuf.data(),
                            m_bridgeSendBuf.size(), 0);
 
        if (sent > 0)
        {   // cb:fold socket plumbing
            m_bridgeSendBuf.erase(0, (size_t)sent);
            continue; // keep draining until empty or the socket pushes back
        }
 
        if (sent == 0)
        {   // cb:fold socket plumbing, disconnect probe carries the outcome
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: send returned 0 (peer closed), disconnecting", me->GetName());
            BridgeDisconnect();
            return;
        }
 
        // sent < 0
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return; // cb:fold socket plumbing (kernel buffer full, retry next tick)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; // cb:fold socket plumbing (kernel buffer full, retry next tick)
        if (errno == EINTR)
            continue; // cb:fold socket plumbing (EINTR, retry immediately)
#endif
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: send error, disconnecting", me->GetName());
        BridgeDisconnect();
        return;
    }
}

void AiBotAI::BridgeSendHello()
{
    if (!m_bridgeConnected || m_bridgeHelloSent)
        return; // cb:fold connection lifecycle gate, fires every tick in steady state

    char const* specProfile = botEntry
        ? AiBotTalents::GetProfileName(me->GetClass(), botEntry->specTab)
        : "unassigned";
    char json[1024];
    snprintf(json, sizeof(json),
        "{\"type\":\"HELLO\",\"payload\":{"
        "\"guid\":%u,\"bridgeProtocol\":%u,\"circuitEpoch\":\"%s\",\"name\":\"%s\",\"race\":%u,\"classId\":%u,"
        "\"level\":%u,\"specTab\":%u,\"specProfile\":\"%s\",\"activeRole\":%u,"
        "\"talentProfileState\":\"%s\",\"rotationSource\":\"%s\","
        "\"rotationProfile\":\"%s\",\"rotationInstructionCount\":%u,"
        "\"rotationCastableCount\":%u,\"combatConfigRevision\":%u,"
        "\"mapId\":%u,\"zoneId\":%u,"
        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
        me->GetGUIDLow(), uint32(BRIDGE_PROTOCOL_VERSION), CbCircuit::Epoch(), me->GetName(), me->GetRace(), me->GetClass(),
        me->GetLevel(), botEntry ? uint32(botEntry->specTab) : 255u,
        specProfile, uint32(m_role), GetTalentProfileStateName(),
        GetEffectiveRotationSource(), GetEffectiveRotationProfile(),
        m_rotationInstructionCount, m_rotationCastableCount, m_combatConfigRevision,
        me->GetMapId(), me->GetZoneId(),
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());

    BridgeSend(json);

    // Restore tracked quest from server quest log (restart recovery)
    if (m_trackedQuestId == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: no tracked quest, restoring from server log");
        const auto& questMap = me->GetQuestStatusMap();
        for (const auto& pair : questMap)
        {
            if (pair.second.m_status == QUEST_STATUS_INCOMPLETE ||
                pair.second.m_status == QUEST_STATUS_COMPLETE)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-bridge: tracked quest restored", pair.first);
                m_trackedQuestId = pair.first;
                break;  // take the first active quest
            }
        }
    }

    m_bridgeHelloSent = true;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: HELLO sent (guid=%u)", me->GetName(), me->GetGUIDLow());

    // [CIRCUIT] fresh connection: re-ship the probe-site manifest so the C#
    // side can decode this session's batches even after either side restarted.
    CbCircuit::ResetManifest(me->GetGUIDLow());
}

char const* AiBotAI::GetTalentProfileStateName() const
{
    if (!botEntry)
        return "unavailable"; // cb:fold state field mapping, carried by HELLO/STATE payload
    switch (botEntry->talentProfileState)
    {
        case PB_TALENT_PROFILE_USABLE: return "usable"; // cb:fold state field mapping
        case PB_TALENT_PROFILE_CONFLICT: return "conflict"; // cb:fold state field mapping
        case PB_TALENT_PROFILE_INVALID: return "invalid"; // cb:fold state field mapping
        case PB_TALENT_PROFILE_DISABLED: return "disabled"; // cb:fold state field mapping
        case PB_TALENT_PROFILE_ERROR: return "error"; // cb:fold state field mapping
        default: return "unchecked"; // cb:fold state field mapping
    }
}

char const* AiBotAI::GetEffectiveRotationSource() const
{
    if (!m_rotation.empty())
        return "custom"; // cb:fold state field mapping
    return HasUsableSpecCombat() ? "builtin_spec" : "legacy_class";
}

char const* AiBotAI::GetEffectiveRotationProfile() const
{
    if (!m_rotation.empty())
        return m_rotationProfile.c_str(); // cb:fold state field mapping
    if (HasUsableSpecCombat() && botEntry)
        return AiBotTalents::GetProfileName(me->GetClass(), botEntry->specTab); // cb:fold state field mapping
    return "legacy_class";
}

// ============================================================
// BridgeSendState — REPLACEMENT (retire-the-pull build)
//
// REPLACES: the entire existing AiBotAI::BridgeSendState() in AiBotAIBridge.cpp.
//
// WHAT CHANGED vs the previous version (three things, all inside this one method —
// no header change, no new members):
//   1. Builds the FULL quest-log snapshot (every non-rewarded active log entry, with
//      status + mob/item counts) into a bounded std::string `questBlob` — the exact
//      same loop and pipe format BridgeHandleQueryQuestStatus used for QUEST_STATUS_ALL.
//   2. Emits it as a new STATE field  "quests":"<blob>"  so C# ctx.QuestLog becomes a
//      continuously-maintained mirror of the player's quest log on the 5s heartbeat —
//      no QUERY_QUEST_STATUS round-trip, no request/reply cache to go stale/partial/empty.
//   3. Bumps the STATE buffer  char json[1536] -> char json[4096]  to fit the blob.
//      The blob is JSON-safe by construction (only digits, ':', ',', '|'), so it needs
//      no escaping inside the STATE string, and the 20-slot quest-log cap keeps it well
//      under the buffer.
//
// The held-task echo fields (taskKind/taskActivity/taskCreature/taskDest*/taskKills) and
// every other STATE field are byte-for-byte unchanged.
//
// QUERY_QUEST_STATUS / QUEST_STATUS_ALL are retired on every path. Group planning waits for
// the next authoritative STATE heartbeat instead of reviving the contradictory pull handler.
// ============================================================
void AiBotAI::BridgeSendState()
{
    if (!m_bridgeConnected)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: state skipped, not connected");
        return;
    }

    const char* taskStr = "IDLE";
    if (me->IsDead())
        taskStr = "DEAD"; // cb:fold state field mapping
    else if (me->IsInCombat()) // cb:fold state field mapping, carried by STATE taskState
        taskStr = "COMBAT"; // cb:fold state field mapping
    else if (m_currentTask.type == TASK_GRIND) // cb:fold state field mapping
        taskStr = "GRINDING"; // cb:fold state field mapping
    else if (m_currentTask.type == TASK_TAXI) // cb:fold state field mapping
        taskStr = "FLYING"; // cb:fold state field mapping
    else if (m_currentTask.type == TASK_MOVE_TO) // cb:fold state field mapping
        taskStr = "MOVING"; // cb:fold state field mapping
    else if (me->IsMoving()) // cb:fold state field mapping
        taskStr = "MOVING"; // cb:fold state field mapping

    uint32 freeSlots = 0;
    for (int fi = INVENTORY_SLOT_ITEM_START; fi < INVENTORY_SLOT_ITEM_END; ++fi)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
            ++freeSlots; // cb:fold slot tally detail
    for (int fi = INVENTORY_SLOT_BAG_START; fi < INVENTORY_SLOT_BAG_END; ++fi)
        if (Bag* pFBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi)) // cb:fold decl-if condition artifact
            if (pFBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pFBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER) // cb:fold slot tally detail
                for (uint32 fj = 0; fj < pFBag->GetBagSize(); ++fj) // cb:fold slot tally detail
                    if (!me->GetItemByPos(fi, fj))
                        ++freeSlots; // cb:fold slot tally detail
    uint32 totalSlots = (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START);
    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i)) // cb:fold decl-if condition artifact
            if (pBag->GetProto()->Class == ITEM_CLASS_CONTAINER && // cb:fold slot tally detail
                pBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                totalSlots += pBag->GetBagSize(); // cb:fold slot tally detail
    }

    // --- Min equipped durability % (feeds the C# repair trigger) ---
    // 100 = no damageable gear or all full. The lowest slot drives the decision;
    // a single 0-durability weapon tanks damage output, so min (not average) is right.
    // Mirrors the durability read in BridgeHandleRepairItems exactly.
    uint32 minDurabilityPct = 100;
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
        if (!item)
            continue; // cb:fold durability tally detail, carried by STATE durability
        uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (maxDur == 0)
            continue;   // cb:fold durability tally detail (rings/trinkets/etc. have none)
        uint32 dur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        uint32 pct = (dur * 100) / maxDur;
        if (pct < minDurabilityPct)
            minDurabilityPct = pct; // cb:fold durability tally detail
    }

    // --- [PLAYERPARTY] escort echo (2026-07-07) + boss range (2026-07-08) ---
    // pparty: 1 = this bot's group contains a REAL player (FindPartyBoss resolves) → C# stands
    // down to the player-party hold and C++ owns the whole behaviour. ppdist: distance to that
    // boss — -1 no boss, 99999 boss on ANOTHER map (instance/boat), else 3D yards. Feeds the
    // C# hub-errand abort guard (boss >150yd / off-map → drop the rounds, resume follow).
    // Same cheap group walk, once per 5s STATE.
    // [MULTI-HUMAN] Both key on FindEscortBoss — null iff no real player is in the group
    // (the SAME truth value as FindPartyBoss, so pparty's meaning is unchanged), but ppdist
    // now measures the bot's OWN assigned human, which is exactly who the hub-errand abort
    // guard should watch: the bot returns to ITS human, not necessarily the leader.
    uint32 pparty = 0u;
    int ppdist = -1;
    if (Player* pBoss = FindEscortBoss()) // cb:fold decl-if condition artifact, body probed
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: escort boss in group, pparty set");
        pparty = 1u;
        ppdist = (pBoss->GetMapId() == me->GetMapId()) ? (int)me->GetDistance(pBoss) : 99999;
    }
    else if (me->GetSession() && !me->GetSession()->GetBot())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: real char in free view, pparty hold");
        // [SUI] Enrolled REAL character with no boss to key on: its human is in
        // the free camera. Owner decree (2026-08-10): the personal party is
        // non-autonomous unless made explicit later — echo pparty=1 so the C#
        // brain holds Idle; movement comes only from CMSG_SUI_ORDER commands.
        pparty = 1u;
    }

    // --- Active quest status from server (authoritative) ---
    uint32 questStatus = 0;  // 0 = no tracked quest
    if (m_trackedQuestId > 0)
        questStatus = (uint32)me->GetQuestStatus(m_trackedQuestId); // cb:fold state field detail, carried by STATE questStatus

    // --- §4 held-task echo: the kind C++ is ACTUALLY running + within-objective activity ---
    // Independent of taskStr above (display/status). The committed task KIND stays GRIND/MOVE_TO
    // through a fight, which is exactly what the C# kind-match needs. All derived from existing
    // state — no new members, no new tracking.
    const char* taskKindStr;
    switch (m_currentTask.type)
    {
        case TASK_GRIND:   taskKindStr = "GRIND";   break; // cb:fold state field mapping, carried by STATE taskKind
        case TASK_MOVE_TO: taskKindStr = "MOVE_TO"; break; // cb:fold state field mapping
        case TASK_TAXI:    taskKindStr = "MOVE_TO"; break; // cb:fold state field mapping (in transit = a kind of travel)
        default:           taskKindStr = "IDLE";    break; // cb:fold state field mapping
    }

    // Within-objective activity — the headway signal C# reacts to (not a wall clock). Order is a
    // priority ladder: combat trumps everything; eating/drinking is a stationary recover; movement
    // is travel; a stationary grind is "between targets" (searching). Anything else = idle.
    const char* activityStr = "idle";
    if (me->IsInCombat())
        activityStr = "engaged"; // cb:fold state field mapping
    else if (me->HasAura(AB_SPELL_FOOD) || me->HasAura(AB_SPELL_DRINK)) // cb:fold state field mapping, carried by STATE taskActivity
        activityStr = "recovering"; // cb:fold state field mapping
    else if (me->IsMoving()) // cb:fold state field mapping
        activityStr = "traveling"; // cb:fold state field mapping
    else if (m_currentTask.type == TASK_GRIND) // cb:fold state field mapping
        activityStr = "searching";   // cb:fold state field mapping (grind between targets, NOT a clock stall)
    // else: idle

    // --- Full quest-log snapshot (RETIRES the QUERY_QUEST_STATUS pull) ---
    // Pushed on EVERY STATE so C# ctx.QuestLog is a continuously-maintained mirror of the player's
    // quest log — never a request/reply cache that can go stale, partial, or empty. Same pipe format
    // the old QUEST_STATUS_ALL used, so the C# parser is reused verbatim:
    //   questId:status:mob0,mob1,mob2,mob3:item0,item1,item2,item3 | questId:...
    // status: 1=COMPLETE, 3=INCOMPLETE (VMaNGOS enum). Only non-rewarded active log entries are
    // included (rewarded/turned-in are excluded — that is how a turn-in legitimately drops out). The
    // blob is built as a bounded std::string (the 20-slot log fits well under the buffer) and its
    // characters are JSON-safe (digits, ':', ',', '|'), so it needs no escaping inside the STATE string.
    std::string questBlob;
    {
        const auto& qMap = me->GetQuestStatusMap();
        for (const auto& pair : qMap)
        {
            const auto& qData = pair.second;

            if (qData.m_rewarded)
                continue;   // cb:fold quest blob detail (turned in, not an active log entry)
            if (qData.m_status != QUEST_STATUS_INCOMPLETE &&
                qData.m_status != QUEST_STATUS_COMPLETE)
                continue;   // cb:fold quest blob detail (skip QUEST_STATUS_NONE / UNAVAILABLE)

            if (!questBlob.empty())
                questBlob += "|"; // cb:fold quest blob detail

            char qEntry[128];
            snprintf(qEntry, sizeof(qEntry), "%u:%u:%u,%u,%u,%u:%u,%u,%u,%u",
                pair.first,
                (uint32)qData.m_status,
                qData.m_creatureOrGOcount[0], qData.m_creatureOrGOcount[1],
                qData.m_creatureOrGOcount[2], qData.m_creatureOrGOcount[3],
                qData.m_itemcount[0], qData.m_itemcount[1],
                qData.m_itemcount[2], qData.m_itemcount[3]);
            questBlob += qEntry;
        }
    }

    char json[4096];
    char const* specProfile = botEntry
        ? AiBotTalents::GetProfileName(me->GetClass(), botEntry->specTab)
        : "unassigned";
    snprintf(json, sizeof(json),
        "{\"type\":\"STATE\",\"payload\":{"
        "\"guid\":%u,\"health\":%u,\"maxHealth\":%u,"
        "\"mana\":%u,\"maxMana\":%u,\"level\":%u,"
        "\"specTab\":%u,\"specProfile\":\"%s\",\"activeRole\":%u,"
        "\"talentProfileState\":\"%s\",\"rotationSource\":\"%s\","
        "\"rotationProfile\":\"%s\",\"rotationInstructionCount\":%u,"
        "\"rotationCastableCount\":%u,\"combatConfigRevision\":%u,"
        "\"mapId\":%u,\"zoneId\":%u,"
        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
        "\"inCombat\":%s,\"isDead\":%s,"
        "\"targetGuid\":%u,\"taskState\":\"%s\","
        "\"freeSlots\":%u,\"totalSlots\":%u,\"copper\":%u,\"durability\":%u,\"pparty\":%u,\"ppdist\":%d,"
        "\"taskKind\":\"%s\",\"taskActivity\":\"%s\","
        "\"taskCreature\":%u,\"taskDestX\":%.2f,\"taskDestY\":%.2f,\"taskDestZ\":%.2f,\"taskKills\":%d,"
        "\"quests\":\"%s\","
        "\"questId\":%u,\"questStatus\":%u,\"possessed\":%u,\"conscripted\":%u}}",
        me->GetGUIDLow(),
        me->GetHealth(), me->GetMaxHealth(),
        me->GetPower(POWER_MANA), me->GetMaxPower(POWER_MANA),
        me->GetLevel(),
        botEntry ? uint32(botEntry->specTab) : 255u, specProfile, uint32(m_role),
        GetTalentProfileStateName(), GetEffectiveRotationSource(), GetEffectiveRotationProfile(),
        m_rotationInstructionCount, m_rotationCastableCount, m_combatConfigRevision,
        me->GetMapId(), me->GetZoneId(),
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
        me->IsInCombat() ? "true" : "false",
        me->IsDead() ? "true" : "false",
        me->GetTargetGuid().IsEmpty() ? 0 : me->GetTargetGuid().GetCounter(),
        taskStr,
        freeSlots, totalSlots, me->GetMoney(), minDurabilityPct, pparty, ppdist,
        taskKindStr, activityStr,
        m_currentTask.creatureEntry, m_currentTask.x, m_currentTask.y, m_currentTask.z, m_currentTask.killCount,
        questBlob.c_str(),
        m_trackedQuestId, questStatus, m_possessed ? 1u : 0u,
        IsSuiConscripted() ? 1u : 0u);

    BridgeSend(json);
}

void AiBotAI::BridgeSendEvent(const char* eventType, const char* data)
{
    // A two-argument outcome is synchronous when called from a command handler and
    // unsolicited otherwise; m_bridgeDispatchCbt is nonzero only for that dispatch stack.
    BridgeSendEvent(eventType, data, m_bridgeDispatchCbt);
}

void AiBotAI::BridgeSendEvent(const char* eventType, const char* data, uint64 commandCbt)
{
    if (!m_bridgeConnected)
    {
        CB_HITN(me->GetGUIDLow(), "cpp-bridge: event dropped, not connected", eventType);
        return;
    }

    // Async owners pass their persisted cbt explicitly, including an exact zero for legacy work.
    // That prevents a cancellation callback from borrowing the cbt of its interrupting command.
    char json[768];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"cbt\":" UI64FMTD ",\"payload\":{"
        "\"guid\":%u,\"event\":\"%s\",\"data\":\"%s\"}}",
        commandCbt, me->GetGUIDLow(), eventType, data);

    BridgeSend(json);
}

bool AiBotAI::CancelPendingNpcInteraction(const char* source)
{
    if (m_pendingInteractNpcGuid.IsEmpty())
        return false; // cb:fold no pending manual approach to cancel

    // A newer motion owner must retire the older INTERACT_NPC before it clears or replaces the
    // point generator. Otherwise its suppressed cancellation callback leaves this owner alive and
    // the OOC resume block can mistake the newer command's spline for the old NPC approach.
    CB_HIT(me->GetGUIDLow(), "cpp-task: pending NPC interaction preempted by newer motion owner");
    uint64 const interactCbt = m_pendingInteractNpcCbt;
    m_pendingInteractNpcGuid.Clear();
    m_pendingInteractNpcCbt = 0;
    char eventData[128];
    snprintf(eventData, sizeof(eventData),
        "reason=preempted|source=%s", source);
    BridgeSendEvent("NPC_INTERACT_FAIL", eventData, interactCbt);
    return true;
}

void AiBotAI::BridgeRecv()
{
    if (!m_bridgeConnected || m_bridgeSocket == BRIDGE_INVALID_SOCKET)
        return; // cb:fold socket plumbing

    // Read available data into buffer (non-blocking)
    int space = BRIDGE_RECV_BUF_SIZE - m_bridgeRecvLen - 1;
    if (space <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: recv buffer overflow, cleared");
        // Buffer full with no newline — discard
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: recv buffer overflow, clearing", me->GetName());
        m_bridgeRecvLen = 0;
        return;
    }

    ssize_t n = recv(m_bridgeSocket, m_bridgeRecvBuf + m_bridgeRecvLen, space, 0);
    if (n > 0)
    {   // cb:fold socket plumbing, dispatch probe carries each processed line
        m_bridgeRecvLen += n;
        m_bridgeRecvBuf[m_bridgeRecvLen] = '\0';

        // Process complete lines
        char* start = m_bridgeRecvBuf;
        char* newline;
        while ((newline = strchr(start, '\n')) != nullptr)
        {
            *newline = '\0';
            if (newline > start) // skip empty lines
                BridgeProcessLine(start); // cb:fold empty line skip, dispatch probe carries each line
            start = newline + 1;
        }

        // Shift remaining partial data to front
        int remaining = m_bridgeRecvLen - (int)(start - m_bridgeRecvBuf);
        if (remaining > 0 && start != m_bridgeRecvBuf)
            memmove(m_bridgeRecvBuf, start, remaining); // cb:fold socket plumbing
        m_bridgeRecvLen = remaining;
    }
    else if (n == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: server closed connection");
        // Clean disconnect
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: server closed connection", me->GetName());
        BridgeDisconnect();
    }
    else
    {   // cb:fold socket plumbing (EAGAIN empty poll path)
        // n < 0: check if it's just EAGAIN/EWOULDBLOCK (no data yet)
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK)
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: recv error, disconnecting");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: recv error, disconnecting", me->GetName());
            BridgeDisconnect();
        }
    }
}

// Minimal JSON field extraction — no library needed.
// Finds "key":value in a flat JSON object. Works for our simple payloads.
static bool JsonExtractFloat(const char* json, const char* key, float& out)
{
    // Search for "key": pattern
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false; // cb:fold json parse detail
    p += strlen(pattern);
    while (*p == ' ') p++;
    out = (float)atof(p);
    return true;
}

static bool JsonExtractInt(const char* json, const char* key, int& out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false; // cb:fold json parse detail
    p += strlen(pattern);
    while (*p == ' ') p++;
    out = atoi(p);
    return true;
}

static bool JsonExtractUInt64(const char* json, const char* key, uint64& out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false; // cb:fold json parse detail
    p += strlen(pattern);
    while (*p == ' ') p++;
    return sscanf(p, UI64FMTD, &out) == 1; // cb:fold json parse detail
}

static bool JsonExtractString(const char* json, const char* key, char* out, int maxLen)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false; // cb:fold json parse detail
    p += strlen(pattern);
    const char* end = strchr(p, '"');
    if (!end) return false; // cb:fold json parse detail
    int len = std::min((int)(end - p), maxLen - 1);
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// Destructive/admin commands must reject truncation rather than quietly applying
// a prefix of the caller's value. Existing legacy handlers keep their tolerant
// extractor; APPLY_COMBAT_LOADOUT uses this strict variant exclusively.
static bool JsonExtractStringStrict(const char* json, const char* key, char* out, int maxLen)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false; // cb:fold json parse detail
    p += strlen(pattern);
    const char* end = strchr(p, '"');
    if (!end || end - p >= maxLen)
        return false; // cb:fold json parse detail (strict length refusal)
    int len = int(end - p);
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool JsonExtractBool(const char* json, const char* key, bool& out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false; // cb:fold json parse detail
    p += strlen(pattern);
    while (*p == ' ') ++p;
    if (strncmp(p, "true", 4) == 0 || *p == '1')
    {   // cb:fold json parse detail
        out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0 || *p == '0')
    {   // cb:fold json parse detail
        out = false;
        return true;
    }
    return false;
}

static bool IsSafeBridgeToken(const char* value, bool allowEmpty = false)
{
    if (!value || (!allowEmpty && !*value))
        return false; // cb:fold token check detail, refusal carried by callers invalid_request ack
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
        if (!std::isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != ':')
            return false; // cb:fold token check detail
    return true;
}

void AiBotAI::BridgeProcessLine(const char* line)
{
    // Extract "type" field
    char msgType[32] = {0};
    if (!JsonExtractString(line, "type", msgType, sizeof(msgType)))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: line rejected, no type field");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: no 'type' in message", me->GetName());
        return;
    }

    // Protocol v4 promotes cbt from trace-only metadata to behavioral ownership. Preserve a
    // prior value in case a locally-injected command ever nests dispatch on this same stack.
    uint64 cbt = 0;
    JsonExtractUInt64(line, "cbt", cbt);
    uint64 const previousDispatchCbt = m_bridgeDispatchCbt;
    m_bridgeDispatchCbt = cbt;
    if (cbt > 0)
        CB_HITV(me->GetGUIDLow(), "cpp-chain: command adopted", static_cast<double>(cbt));
    CB_HITN(me->GetGUIDLow(), "cpp-bridge: dispatch", msgType);

    // Encounter execution owns combat and movement until an explicit commander order yields it.
    // Continue receiving liveness/diagnostics so stale autonomous commands are drained.
    if (SuiCommanderRaid::Owns(me) && strcmp(msgType,"PING")!=0 && strcmp(msgType,"CIRCUIT_TRACE")!=0)
    {
        if(m_suiCommanderLine) SuiCommanderRaid::Yield(me);
        else
        {
            BridgeSendEvent("ENCOUNTER_DROP",msgType);
            m_bridgeDispatchCbt=previousDispatchCbt;
            return;
        }
    }

    // [SUI] While a real player drives this bot every mutating command would
    // execute under the human's feet (MOVE_TO would yank the mover). Drop with
    // an event so the C# supervisor sees an explicit answer, not a stall.
    // ...unless the human is COMMANDING it from the free view, where there are no feet to
    // execute under: the client's controller is the camera, its movement stream is parked, and
    // an RTS order is the only thing that can move this bot at all. Dropping the order here is
    // what made a commanded toon the one party member that ignored a move.
    if (m_possessed && strcmp(msgType, "PING") != 0 &&
        strcmp(msgType, "APPLY_COMBAT_LOADOUT") != 0 &&
        !SuiPossess::IsCommandedFromFreeView(me))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "[AIBOT-BRIDGE] %s: dropped %s (possessed)",
            me->GetName(), msgType);
        CB_HITN(me->GetGUIDLow(), "cpp-bridge: dropped (possessed)", msgType);
        BridgeSendEvent("POSSESSED_DROP", msgType);
        m_bridgeDispatchCbt = previousDispatchCbt;
        return;
    }

    // [SUI] An enlisted bot belongs to its commander, not the planner: brain
    // lines are dropped with an explicit event so the C# supervisor sees an
    // answer, not a stall. PING stays (liveness); COMBAT_DIRECTIVE, LOAD_ROTATION
    // and LOAD_RAID_PLAN stay — conscripts keep their combat AI, and those are
    // combat configuration, not errands (an enlisted bot is exactly the one you
    // raid-plan with). Commander-injected RTS orders arrive via
    // SuiInjectCommandLine and pass the fence.
    if ((IsSuiConscripted() || m_suiManual) && !m_suiCommanderLine &&
        strcmp(msgType, "PING") != 0 && strcmp(msgType, "COMBAT_DIRECTIVE") != 0 &&
        strcmp(msgType, "LOAD_ROTATION") != 0 && strcmp(msgType, "APPLY_COMBAT_LOADOUT") != 0 &&
        strcmp(msgType, "LOAD_RAID_PLAN") != 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "[AIBOT-BRIDGE] %s: dropped %s (conscripted)",
            me->GetName(), msgType);
        CB_HITN(me->GetGUIDLow(), "cpp-bridge: dropped (conscripted)", msgType);
        BridgeSendEvent("CONSCRIPTED_DROP", msgType);
        m_bridgeDispatchCbt = previousDispatchCbt;
        return;
    }

    if (strcmp(msgType, "MOVE_TO") == 0)
        BridgeHandleMoveTo(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "TELEPORT_TO") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleTeleport(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "SAY_TEXT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleSayText(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "QUEST_INTERACT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleQuestInteract(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "ABANDON_QUEST") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleAbandonQuest(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "LEARN_SPELL") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleLearnSpell(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "ATTACK_TARGET") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleAttackTarget(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "INTERACT_NPC") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleInteractNpc(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "SET_TASK") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleSetTask(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "COMBAT_DIRECTIVE") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleCombatDirective(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "RESET_COMBAT_STUCK") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleResetCombatStuck(line); // cb:fold dispatch, carried by dispatch probe
     else if (strcmp(msgType, "TAKE_FLIGHT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleTakeFlight(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "SELL_ITEMS") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleSellItems(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "REPAIR_AT_NPC") == 0) // cb:fold dispatch, carried by dispatch probe
       BridgeHandleRepairItems(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "RESURRECT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleResurrect(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "TRAIN_AT_NPC") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleTrain(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "USE_GAMEOBJECT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleUseGameObject(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "QUEST_CAST") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleQuestCast(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "FORM_GROUP") == 0) // cb:fold dispatch, carried by dispatch probe
       BridgeHandleFormGroup(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "DISBAND_GROUP") == 0) // cb:fold dispatch, carried by dispatch probe
       BridgeHandleDisbandGroup(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "SET_ESCORT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleSetEscort(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "LOAD_ROTATION") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleLoadRotation(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "APPLY_COMBAT_LOADOUT") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleApplyCombatLoadout(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "LOAD_RAID_PLAN") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleLoadRaidPlan(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "CIRCUIT_TRACE") == 0) // cb:fold dispatch, carried by dispatch probe
        BridgeHandleCircuitTrace(line); // cb:fold dispatch, carried by dispatch probe
    else if (strcmp(msgType, "PING") == 0) // cb:fold dispatch, carried by dispatch probe
        { /* do nothing, keepalive */ } // cb:fold keepalive no-op, dispatch probe carries it
    else
    {
        CB_HITN(me->GetGUIDLow(), "cpp-bridge: unknown command", msgType);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: unknown command '%s'", me->GetName(), msgType);
    }
    m_bridgeDispatchCbt = previousDispatchCbt;
}

// ============================================================
// Protocol v6: RESET_COMBAT_STUCK
//
// This is not a general "leave combat" command. C# sends it only after a
// continuous, fresh-STATE proof that the bot has remained inside a tiny anchor
// while in combat, without a real kill, for the full stuck window. Revalidate
// that proof here before touching core combat state. The operation preserves
// the planner task/directive, emits an exact-cbt terminal result, immediately
// ships STATE, and parks autonomous reacquisition for five seconds so C# can
// require fresh out-of-combat truth before its existing safe escape runs.
// ============================================================
void AiBotAI::BridgeHandleResetCombatStuck(const char* json)
{
    auto fail = [this](const char* reason)
    {
        char data[128];
        snprintf(data, sizeof(data), "reason=%s", reason);
        BridgeSendEvent("COMBAT_RESET_FAIL", data);
        BridgeSendState();
    };

    // A socket cannot exist without its AiBotAI actor in normal operation, but
    // do not dereference a torn-down actor merely to manufacture a failure line.
    if (!me)
        return; // cb:fold actor teardown race; C# bounded WAIT owns the outcome

    if (m_bridgeDispatchCbt == 0)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0u, "cpp-combat-reset: rejected, missing cbt");
        fail("missing_cbt");
        return;
    }
    if (!me->IsInWorld() || !me->IsAlive() || me->IsBeingTeleported() ||
        me->HasUnitState(UNIT_STATE_TAXI_FLIGHT) || me->GetTransport() || m_hearthActive)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0u, "cpp-combat-reset: rejected, actor unavailable");
        fail("actor_unavailable");
        return;
    }
    if (me->GetGroup())
    {
        // Combat-still recovery is intentionally solo-only. Stopping combat for
        // one member of either a player-led or bot-only party would violate the
        // group's shared combat/movement owner and could split the formation.
        CB_HIT(me->GetGUIDLow(), "cpp-combat-reset: rejected, grouped actor");
        fail("grouped");
        return;
    }

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    float anchorX = 0.0f, anchorY = 0.0f, anchorZ = 0.0f, radius = 0.0f;
    int anchorMap = -1, stillSeconds = 0, wedgeStreak = 0;
    bool const complete =
        JsonExtractFloat(payload, "anchor_x", anchorX) &&
        JsonExtractFloat(payload, "anchor_y", anchorY) &&
        JsonExtractFloat(payload, "anchor_z", anchorZ) &&
        JsonExtractFloat(payload, "radius", radius) &&
        JsonExtractInt(payload, "anchor_map", anchorMap) &&
        JsonExtractInt(payload, "still_seconds", stillSeconds) &&
        JsonExtractInt(payload, "wedge_streak", wedgeStreak);

    if (!complete || !std::isfinite(anchorX) || !std::isfinite(anchorY) ||
        !std::isfinite(anchorZ) || !std::isfinite(radius) ||
        radius < 1.0f || radius > 10.0f || stillSeconds < AIBOT_COMBAT_RESET_MIN_STILL_SEC ||
        stillSeconds > 86400 || wedgeStreak < 6)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat-reset: rejected, invalid proof payload");
        fail("bad_proof");
        return;
    }
    if (anchorMap != (int)me->GetMapId())
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combat-reset: rejected, anchor map changed", anchorMap);
        fail("map_changed");
        return;
    }

    float const dx = me->GetPositionX() - anchorX;
    float const dy = me->GetPositionY() - anchorY;
    float const dz = std::fabs(me->GetPositionZ() - anchorZ);
    if (dx * dx + dy * dy > radius * radius || dz > 10.0f)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combat-reset: rejected, bot moved outside proof anchor", std::sqrt(dx * dx + dy * dy));
        fail("moved");
        return;
    }

    if (me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat-reset: validated latch, clearing core combat state");

        uint32 const resetVictimGuid = me->GetVictim()
            ? me->GetVictim()->GetGUIDLow()
            : m_lastVictimGuidLow;
        if (resetVictimGuid)
            m_combatIgnore[resetVictimGuid] = AIBOT_STALEMATE_IGNORE_MS; // cb:fold bounded reacquire quarantine

        // Tear down only tactical/motion continuation state. The strategic task
        // and combat directive deliberately survive; the C# owner will either
        // port after fresh OOC proof or let the same task resume after the hold.
        m_suppressTaskDestInform = true;
        CancelPendingNpcInteraction("combat_stuck_reset");
        // Stop controlled combat too. ACKing only the owner while a pet,
        // guardian, or charm remains engaged lets that dependent immediately
        // relatch the owner before the fresh post-reset STATE reaches C#.
        me->CombatStopWithPets(true);
        me->GetHostileRefManager().deleteReferences();
        me->ClearTarget();
        StopMoving();
        ClearStoredPath();

        m_stalemateMs = 0;
        m_stalemateHoldMs = 0;
        m_stalemateNudges = 0;
        m_stalemateDisengages = 0;
        m_lastHealth = 0;
        m_lastVictimHealth = 0;
        m_stalemateVictim.Clear();

        m_overpullFleeHoldMs = 0;
        m_overpullFlees = 0;
        m_lastAttackerCount = 0;

        m_pullActive = false;
        m_pullTagged = false;
        m_pullTargetGuid.Clear();
        m_pullRetreatHoldMs = 0;
        m_pullElapsedMs = 0;

        // AttackStop/CombatStop intentionally made the old victim disappear;
        // clear kill inference so the next OOC tick cannot mint a false KILL.
        m_lastVictimEntry = 0;
        m_lastVictimGuidLow = 0;
        m_recoveryResetHoldMs = AIBOT_COMBAT_RESET_HOLD_MS;
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat-reset: latch already clear, idempotent ACK");
    }

    if (me->IsInCombat() || !me->GetTargetGuid().IsEmpty() || me->IsMoving())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat-reset: postcondition failed, no ACK");
        fail("postcondition");
        return;
    }

    m_recoveryResetHoldMs = AIBOT_COMBAT_RESET_HOLD_MS;
    char ack[192];
    snprintf(ack, sizeof(ack),
        "result=cleared|map=%u|x=%.1f|y=%.1f|still_seconds=%d|wedge_streak=%d",
        me->GetMapId(),
        me->GetPositionX(), me->GetPositionY(), stillSeconds, wedgeStreak);
    CB_HIT(me->GetGUIDLow(), "cpp-combat-reset: correlated ACK + fresh STATE queued");
    BridgeSendEvent("COMBAT_RESET_ACK", ack);
    BridgeSendState();
}

// ============================================================
// BridgeHandleTeleport — generic live-bot teleport primitive
//
// C# drives the teleport-assist: when a final-approach MOVE_TO to a trainer /
// vendor / repair NPC no_path's twice while the bot is already in the vicinity
// (a nav-dead pocket — building interior, bad mesh stitch at the NPC), C# saves
// the bot's current on-mesh position as a return anchor and sends TELEPORT_TO the
// NPC. The bot does its business at real proximity, then C# sends TELEPORT_TO the
// saved anchor to put it back where it came from. All round-trip state lives in C#
// (BotContext) — this handler is the dumb primitive: relocate a LIVE bot on the
// same map and ack.
//
// Uses NearTeleportTo (same-map instant relocation) — NOT the cross-map far-port,
// which defers behind a loading screen / IsBeingTeleported(). Cross-map is refused
// (the assist is always same-map). An optional max_dist caps the hop as a safety
// rail so a bad C# coord can't fling a live bot across the zone (the assist sends
// max_dist≈50; 0/absent = no cap, for a future long-range hearth).
//
// PLACEMENT: after BridgeHandleMoveTo in AiBotAI.cpp
// DISPATCH:  BridgeProcessLine → else if (strcmp(msgType,"TELEPORT_TO")==0) BridgeHandleTeleport(line);
// HEADER:    void BridgeHandleTeleport(const char* json);
// ============================================================
void AiBotAI::BridgeHandleTeleport(const char* json)
{
    if (!me || !me->IsInWorld())
    {
        CB_HIT(me ? me->GetGUIDLow() : 0u, "cpp-move: teleport ignored, not in world");
        return;
    }

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    int mapId = (int)me->GetMapId();   // default = current map (a missing mapId is same-map)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float o = me->GetOrientation();    // default = keep facing
    float maxDist = 0.0f;              // 0 = no cap

    JsonExtractInt(payload, "mapId", mapId);
    bool haveX = JsonExtractFloat(payload, "x", x);
    bool haveY = JsonExtractFloat(payload, "y", y);
    bool haveZ = JsonExtractFloat(payload, "z", z);
    JsonExtractFloat(payload, "o", o);
    JsonExtractFloat(payload, "max_dist", maxDist);

    if (!haveX || !haveY || !haveZ)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-move: teleport bad payload, missing xyz, TELEPORT_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO bad payload (missing x/y/z)", me->GetName());
        BridgeSendEvent("TELEPORT_FAIL", "reason=bad_payload");
        return;
    }

    // Dead bots are owned by the death-recovery path (ghost-walk / graveyard port).
    // A teleport-assist must never fire on a ghost.
    if (me->IsDead() || me->GetDeathState() == DEAD)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-move: teleport refused, bot is dead, TELEPORT_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO refused — bot is dead", me->GetName());
        BridgeSendEvent("TELEPORT_FAIL", "reason=dead");
        return;
    }

    // Same-map only. NearTeleportTo is a same-map relocation; cross-map is the deferred
    // far-port (loading screen) and is out of scope for the assist.
    if ((uint32)mapId != me->GetMapId())
    {
        CB_HITV(me->GetGUIDLow(), "cpp-move: teleport refused, cross map, TELEPORT_FAIL", mapId);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO cross-map refused (current=%u target=%d)",
            me->GetName(), me->GetMapId(), mapId);
        BridgeSendEvent("TELEPORT_FAIL", "reason=cross_map");
        return;
    }

    // Safety rail: refuse a hop beyond the caller's cap (assist sends ~50yd). 0 = no cap.
    float dist = me->GetDistance2d(x, y);
    if (maxDist > 0.0f && dist > maxDist)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-move: teleport refused, hop beyond max dist, TELEPORT_FAIL", dist);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO refused — %.1fyd > max_dist %.1f (to %.1f,%.1f,%.1f)",
            me->GetName(), dist, maxDist, x, y, z);
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=too_far|dist=%.1f|max_dist=%.1f", dist, maxDist);
        BridgeSendEvent("TELEPORT_FAIL", buf);
        return;
    }

    // [GROUND] Re-ground the C#-supplied Z. The assist sends NPC spawn coords (already
    // grounded → no-op); the hearth sends homebind/hub coords — grounding here is what
    // keeps a hearth from re-floating a bot at a new coord. x/y untouched, so the 15yd
    // NPC-find at the assist target is unaffected.
    ReGroundZ(x, y, z, "teleport");

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TELEPORT] %s: TELEPORT_TO (%.1f,%.1f,%.1f) o=%.2f map=%d — %.1fyd hop",
        me->GetName(), x, y, z, o, mapId, dist);

    // Clean reset: stop the failed approach, drop the stored path + task, then relocate.
    // The MOVE_TO that no_path'd is finished; nothing in-flight needs to survive (the
    // interaction that follows — TRAIN/SELL/REPAIR — doesn't read m_currentTask).
    CancelPendingNpcInteraction("teleport_to");
    StopMoving();
    ClearStoredPath();
    m_currentTask.Clear();

    // Fresh journey state so the NEXT real MOVE_TO re-arms its one-shot recoveries from
    // the new start poly.
    m_didBoundaryExit = false;
    m_didNavmeshSnap  = false;

    // Suppress the idle wander for a few seconds so the bot holds at the NPC until C#
    // fires the interaction (task is now IDLE; without this DoRandomWander could hop 15yd
    // off the trainer before TRAIN_AT_NPC arrives — recoverable via its 50yd fallback, but
    // cleaner to just stand still).
    m_wanderTimer = 5000;

    me->NearTeleportTo(x, y, z, o);

    // Ack carries the (now grounded) REQUESTED target — C# updates ctx.Pos to it so the
    // planner sees DistToTarget≈0 and fires the interaction next tick. Actual landed pos
    // logged so a teleport that didn't take is visible on the first run.
    char ack[160];
    snprintf(ack, sizeof(ack), "x=%.1f|y=%.1f|z=%.1f|map=%u", x, y, z, me->GetMapId());
    BridgeSendEvent("TELEPORT_ACK", ack);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TELEPORT] %s: NearTeleportTo issued — requested (%.1f,%.1f,%.1f), now reads (%.1f,%.1f,%.1f) map=%u",
        me->GetName(), x, y, z,
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetMapId());
}

// ============================================================
// BridgeHandleMoveTo — REPLACEMENT (wolf-meat fix, 2026-06-30)
//
// REPLACES: the entire existing AiBotAI::BridgeHandleMoveTo(const char* json) in
// AiBotAIBridge.cpp.
//
// ONLY CHANGE vs the previous version: three new optional fields parsed from the
// payload — alt_entry1/2/3 — stashed onto m_currentTask.altCreatureEntries[0..2].
// Everything else (mapId/x/y/z, the existing creature_entry/kill_count/grind_radius
// enrichment, the arrival-jitter block, the cross-map guard, the journey-state resets,
// the call into MoveToDestination) is byte-for-byte unchanged.
//
// WHY: an item-drop objective can have more than one creature that satisfies it with
// no real priority between them (Young Wolf + Timber Wolf both drop Tough Wolf Meat at
// the same drop chance, both spawning in the same field near the giver). C# now ships
// up to 3 tied alternates alongside the primary creature_entry. They do NOT change
// where the bot walks (the dispatch coordinate is still creature_entry's resolved
// point) — they widen what counts as a valid hit once the bot is out there:
// ScanApproachTarget's valid-kill union, SelectGrindTarget's match checks, and the
// kill-credit check in UpdateAI all now match m_currentTask.MatchesObjectiveEntry(...)
// instead of `entry == m_currentTask.creatureEntry`. Absent fields parse as 0 (JsonExtractInt's
// untouched-on-miss behavior) — a pre-this-fix C# brain or a kill-objective leg that never
// sends these keys leaves altCreatureEntries all 0, which MatchesObjectiveEntry treats as
// "no alternates" — today's exact behavior, byte-for-byte.
//
// REQUIRES: AiBotTaskData::altCreatureEntries[MAX_ALT_ENTRIES] + MatchesObjectiveEntry()
// added to AiBotAIMain.h (struct AiBotTaskData) — see that header's 2026-06-30 edit.
// ============================================================
void AiBotAI::BridgeHandleMoveTo(const char* json)
{
    float x = 0, y = 0, z = 0;
    int mapId = 0;

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    JsonExtractInt(payload, "mapId", mapId);
    JsonExtractFloat(payload, "x", x);
    JsonExtractFloat(payload, "y", y);
    JsonExtractFloat(payload, "z", z);

    // ── §4 optional objective enrichment ──
    // A kill-objective MOVE_TO carries creature_entry / grind_radius / kill_count so
    // C++ engages the mob during the approach (ScanApproachTarget) and hands off to
    // GRIND in place — never marching to the deep loader coord. Absent → all stay 0 →
    // a plain MOVE_TO (repositioning / travel / gather): arrives, emits TASK_COMPLETE.
    int entry = 0, killCount = 0;
    float grindRadius = 0.0f;
    JsonExtractInt(payload, "creature_entry", entry);
    JsonExtractInt(payload, "kill_count", killCount);
    JsonExtractFloat(payload, "grind_radius", grindRadius);

    // ── wolf-meat fix: tied item-drop alternates (2026-06-30) ──
    // Absent on a plain MOVE_TO and on every kill-objective leg — stays 0, meaning
    // "no alternate" (AiBotTaskData::MatchesObjectiveEntry only matches a non-zero slot).
    int altEntry1 = 0, altEntry2 = 0, altEntry3 = 0;
    JsonExtractInt(payload, "alt_entry1", altEntry1);
    JsonExtractInt(payload, "alt_entry2", altEntry2);
    JsonExtractInt(payload, "alt_entry3", altEntry3);

    if ((uint32)mapId != me->GetMapId())
    {
        CB_HITV(me->GetGUIDLow(), "cpp-move: rejected, cross-map", mapId);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: cross-map move not supported (current=%u, target=%d)",
            me->GetName(), me->GetMapId(), mapId);
        return;
    }
    CB_HITV(me->GetGUIDLow(), "cpp-move: MOVE_TO accepted", entry);

    // ── Arrival jitter (never path to the EXACT dest coord) ──
    // Pathing precisely TO certain coords (NPC spawn points, loader coords) lands the bot on a
    // bad poly / seam edge and trips the off-mesh strand (Ujekawab @ (-8933.5,-136.5): NOPATH
    // from on top of the dest). So for a PLAIN travel MOVE_TO we resolve the request to a
    // VALIDATED point 0.2–2.0yd off the real coord at a random angle: sample the ring, path-check
    // each candidate, take the first that isn't NOPATH. This dodges the bad poly AND fans bots out
    // so they don't stack on one pixel. Bounded tries → if the whole neighborhood is unmeshed we
    // fall back to the exact coord and let MoveToDestination's no_path / off-mesh recovery own it.
    //
    // SKIPPED for an enriched-objective MOVE_TO (entry != 0): that coord is a grind hint that
    // converts to grind-in-place at the mouth — it is never an exact-arrival target, so no jitter.
    if (entry == 0 && !me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-move: plain travel, sampling arrival jitter ring");
        float jx = x, jy = y, jz = z;
        bool found = false;
        for (int t = 0; t < AIBOT_ARRIVE_JITTER_TRIES; ++t)
        {
            float ang  = frand(0.0f, 2.0f * M_PI_F);
            float dist = frand(AIBOT_ARRIVE_JITTER_MIN, AIBOT_ARRIVE_JITTER_MAX);
            float cx = x + dist * cosf(ang);
            float cy = y + dist * sinf(ang);
            float cz = z;
            ReGroundZ(cx, cy, cz, "arrive-jitter");   // keep the candidate on the floor before path-checking

            PathInfo probe(me);
            probe.calculate(cx, cy, cz);
            if (!(probe.getPathType() & PATHFIND_NOPATH))
            {
                CB_HITV(me->GetGUIDLow(), "cpp-move: pathable jitter point found", t);
                jx = cx; jy = cy; jz = cz;
                found = true;
                break;
            }
        }

        if (found)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-move: jitter applied to dest coord");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: MOVE_TO jittered (%.1f,%.1f) -> (%.1f,%.1f) %.1fyd off (dodging exact-coord poly)",
                me->GetName(), x, y, jx, jy, me->GetDistance2d(jx, jy) > 0 ? hypotf(jx - x, jy - y) : 0.0f);
            x = jx; y = jy; z = jz;
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-move: no pathable jitter point, using exact coord");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: MOVE_TO jitter found no pathable ring point in %d tries — using exact (%.1f,%.1f)",
                me->GetName(), AIBOT_ARRIVE_JITTER_TRIES, x, y);
        }
    }

    m_currentTask.isRtsCommand = m_suiCommanderLine;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: MOVE_TO map=%d (%.1f, %.1f, %.1f)%s",
        me->GetName(), mapId, x, y, z, entry ? " [objective]" : "");

    // Preempt command A before assigning command B's task/cbt. MotionMaster::Clear finalizes
    // point movement synchronously and calls MovementInform; both arrival guards make that
    // cancellation callback a no-op. Combat keeps its existing defer-without-stopping behavior:
    // the task-dest guard remains raised until the first out-of-combat tick performs the Clear.
    // Normal combat movement is deferred, but commander-issued RTS movement
    // is allowed to override the combat movement lock.
    m_suppressTaskDestInform = true;
    CancelPendingNpcInteraction("move_to");
    if (!me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-move: preempting prior task before cbt replacement");
        StopMoving();
    }
    ClearStoredPath();
    bool const isRts = m_suiCommanderLine;
    m_currentTask.Clear();

    // Stash the objective hint on the task BEFORE MoveToDestination. MoveToDestination
    // only writes type/x/y/z, so these persist through every continuation leg until the
    // approach scan or an arrival hands off to GRIND (re-centering on the bot).
    m_currentTask.commandCbt    = m_bridgeDispatchCbt;
    m_currentTask.isRtsCommand = isRts;
    m_currentTask.creatureEntry = (uint32)entry;
    m_currentTask.radius        = grindRadius;
    m_currentTask.killGoal      = killCount;
    m_currentTask.killCount     = 0;
    m_currentTask.altCreatureEntries[0] = (uint32)altEntry1;
    m_currentTask.altCreatureEntries[1] = (uint32)altEntry2;
    m_currentTask.altCreatureEntries[2] = (uint32)altEntry3;
    m_approachScanTimer         = 0;   // scan on the first tick of this journey

    // One MOVE_TO = one journey to (x,y,z). MoveToDestination walks the first leg;
    // MovementInform / the UpdateAI resume block continue it past each partial-path
    // horizon until arrival. Distance is no longer a failure mode.
    m_didBoundaryExit = false;   // fresh journey — allow one outbound seam-cross
    m_didNavmeshSnap  = false;   // fresh journey — allow one off-mesh-start snap
    MoveToDestination(x, y, z);
}
 

// ============================================================
// BridgeHandleLoadRotation — [ROTATION] load/replace/clear the custom slate (2026-07-16).
//
// Payload: {"profile":"priest_smite_v1","data":"spellId:prio:target:hpMin:hpMax:aura:present|..."}
// The pipe format is the house wire idiom (quest log, TRAIN_ACK). C# pre-sorts by
// priority; C++ preserves order and drops the priority field after parse (it exists on
// the wire only so a human can read a captured payload). Empty/absent data CLEARS the
// slate — vanilla class AI resumes next tick, the RotationSlate else-branch contract.
//
// SpellEntry resolution happens HERE, once: unknown spell IDs and spells the bot has
// not learned parse to pSpell=null and are skipped at tick time; the ROTATION_ACK
// reports loaded vs skipped counts so C# can log a bad profile loudly instead of the
// bot silently doing less than the profile says.
// ============================================================
void AiBotAI::BridgeHandleLoadRotation(const char* json)
{
    char profileBuf[64] = {0};
    JsonExtractString(json, "profile", profileBuf, sizeof(profileBuf));

    char dataBuf[2048] = {0};
    JsonExtractString(json, "data", dataBuf, sizeof(dataBuf));

    m_rotation.clear();
    m_rotationProfile.clear();
    m_rotationInstructionCount = 0;
    m_rotationCastableCount = 0;

    if (dataBuf[0] == '\0')
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: empty data, slate cleared, ROTATION_ACK");
        ++m_combatConfigRevision;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-ROTATION] %s: slate CLEARED — built-in spec policy (or legacy fallback) resumes",
            me->GetName());
        char ack[192];
        snprintf(ack, sizeof(ack), "profile=|loaded=0|skipped=0|revision=%u", m_combatConfigRevision);
        BridgeSendEvent("ROTATION_ACK", ack);
        BridgeSendState();
        return;
    }

    uint32 loaded = 0, skipped = 0;
    char* saveptr = nullptr;
    for (char* seg = strtok_r(dataBuf, "|", &saveptr); seg != nullptr; seg = strtok_r(nullptr, "|", &saveptr))
    {
        ++m_rotationInstructionCount;
        uint32 spellId = 0, auraId = 0;
        int prio = 0, target = 1, hpMin = 0, hpMax = 100, present = 0;
        if (sscanf(seg, "%u:%d:%d:%d:%d:%u:%d", &spellId, &prio, &target, &hpMin, &hpMax, &auraId, &present) != 7)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: malformed rotation instruction, skipped");
            ++skipped;
            continue;
        }

        SpellEntry const* pSpell = spellId ? sSpellMgr.GetSpellEntry(spellId) : nullptr;
        if (!pSpell || !me->HasSpell(spellId) || target < 0 || target > 2 ||
            hpMin < 0 || hpMax > 100 || hpMin > hpMax ||
            (auraId && !sSpellMgr.GetSpellEntry(auraId)))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: invalid or unlearned spell, skipped", spellId);
            ++skipped;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-ROTATION] %s: rejected invalid/unlearned spell %u in profile '%s'",
                me->GetName(), spellId, profileBuf);
            continue;
        }

        RotationInstruction inst;
        inst.spellId = spellId;
        inst.pSpell = pSpell;
        inst.target = (uint8)target;
        inst.hpMin = hpMin;
        inst.hpMax = hpMax;
        inst.auraId = auraId;
        inst.auraPresent = (present != 0);
        m_rotation.push_back(inst);
        ++loaded;
    }

    m_rotationCastableCount = loaded;
    if (loaded)
        m_rotationProfile = profileBuf; // cb:fold ack detail, ROTATION_ACK carries profile
    ++m_combatConfigRevision;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-ROTATION] %s: slate '%s' loaded — %u castable, %u skipped (of %u instructions)",
        me->GetName(), profileBuf, loaded, skipped, (uint32)m_rotation.size());

    char ack[192];
    snprintf(ack, sizeof(ack), "profile=%s|loaded=%u|skipped=%u|revision=%u",
        loaded ? profileBuf : "", loaded, skipped, m_combatConfigRevision);
    BridgeSendEvent("ROTATION_ACK", ack);
    BridgeSendState();
}

// One correlated, core-authoritative operation for the web build workshop.
// Talent/profile/role and the live rotation move together on this bot's world
// thread; SuperUI never edits character_spell directly.
void AiBotAI::BridgeHandleApplyCombatLoadout(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    char requestId[48] = {0};
    char rotationMode[24] = {0};
    char rotationProfile[64] = {0};
    char rotationData[2048] = {0};
    int expectedRevision = -1;
    int specTab = -1;
    int activeRole = 0;
    bool resetTalents = false;

    bool const requestOk = JsonExtractStringStrict(payload, "requestId", requestId, sizeof(requestId)) &&
        IsSafeBridgeToken(requestId) &&
        JsonExtractInt(payload, "expectedRevision", expectedRevision) &&
        JsonExtractInt(payload, "specTab", specTab) &&
        JsonExtractInt(payload, "activeRole", activeRole) &&
        JsonExtractBool(payload, "resetTalents", resetTalents) &&
        JsonExtractStringStrict(payload, "rotationMode", rotationMode, sizeof(rotationMode)) &&
        JsonExtractStringStrict(payload, "rotationProfile", rotationProfile, sizeof(rotationProfile)) &&
        JsonExtractStringStrict(payload, "rotationData", rotationData, sizeof(rotationData));

    auto sendAck = [&](char const* status, char const* code, uint32 learned)
    {
        char const* profile = (me && botEntry)
            ? AiBotTalents::GetProfileName(me->GetClass(), botEntry->specTab)
            : "unassigned";
        uint32 skipped = m_rotationInstructionCount > m_rotationCastableCount
            ? m_rotationInstructionCount - m_rotationCastableCount : 0;
        char ack[480];
        snprintf(ack, sizeof(ack),
            "requestId=%s|status=%s|code=%s|revision=%u|specTab=%u|profile=%s|role=%u|"
            "talentState=%s|learned=%u|rotationSource=%s|rotationProfile=%s|loaded=%u|skipped=%u|reset=%u",
            requestId, status, code, m_combatConfigRevision,
            botEntry ? uint32(botEntry->specTab) : 255u, profile, uint32(m_role),
            GetTalentProfileStateName(), learned, GetEffectiveRotationSource(),
            GetEffectiveRotationProfile(), m_rotationCastableCount, skipped,
            resetTalents ? 1u : 0u);
        if (requestId[0])
        {   // cb:fold ack replay cache bookkeeping
            m_lastCombatLoadoutRequest = requestId;
            m_lastCombatLoadoutAck = ack;
        }
        BridgeSendEvent("COMBAT_LOADOUT_ACK", ack);
    };

    if (!requestOk)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: invalid request payload, error ack");
        // Never reflect an untrusted token into the event's JSON data string.
        requestId[0] = '\0';
        sendAck("error", "invalid_request", 0);
        return;
    }

    if (m_lastCombatLoadoutRequest == requestId && !m_lastCombatLoadoutAck.empty())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: duplicate request, replaying cached ack");
        BridgeSendEvent("COMBAT_LOADOUT_ACK", m_lastCombatLoadoutAck.c_str());
        return;
    }

    if (expectedRevision < 0 || uint32(expectedRevision) != m_combatConfigRevision)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: stale revision, error ack", expectedRevision);
        sendAck("error", "stale_revision", 0);
        return;
    }
    if (!me || !botEntry || IsRealCharacter() || !m_initialized)
    {
        CB_HIT(me ? me->GetGUIDLow() : 0u, "cpp-combatcfg: bot not managed, error ack");
        sendAck("error", "not_managed", 0);
        return;
    }
    if (specTab < 0 || specTab > 2)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: invalid spec tab, error ack", specTab);
        sendAck("error", "invalid_profile", 0);
        return;
    }
    CombatBotRoles requestedRole = CombatBotRoles(activeRole);
    if (!AiBotTalents::IsProfileRoleAllowed(me->GetClass(), uint8(specTab), requestedRole))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: role not allowed for profile, error ack", activeRole);
        sendAck("error", "invalid_role", 0);
        return;
    }
    if (!resetTalents && botEntry->specTab != uint8(specTab))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: spec change without reset, error ack");
        sendAck("error", "reset_required", 0);
        return;
    }

    bool const useSpecRotation = strcmp(rotationMode, "SPEC") == 0 ||
        strcmp(rotationMode, "spec_default") == 0;
    bool const useCustomRotation = strcmp(rotationMode, "CUSTOM") == 0 ||
        strcmp(rotationMode, "custom") == 0;
    if (!useSpecRotation && !useCustomRotation)
    {
        CB_HITN(me->GetGUIDLow(), "cpp-combatcfg: invalid rotation mode, error ack", rotationMode);
        sendAck("error", "invalid_rotation_mode", 0);
        return;
    }

    std::vector<RotationInstruction> stagedRotation;
    uint32 stagedInstructionCount = 0;
    if (useCustomRotation)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: staging custom rotation");
        if (!IsSafeBridgeToken(rotationProfile) || !rotationData[0])
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bad rotation profile or data, error ack");
            sendAck("error", "invalid_rotation", 0);
            return;
        }

        char* saveptr = nullptr;
        for (char* seg = strtok_r(rotationData, "|", &saveptr); seg != nullptr; seg = strtok_r(nullptr, "|", &saveptr))
        {
            if (++stagedInstructionCount > 64)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: rotation too large, error ack", stagedInstructionCount);
                sendAck("error", "rotation_too_large", 0);
                return;
            }

            uint32 spellId = 0, auraId = 0;
            int prio = 0, target = 1, hpMin = 0, hpMax = 100, present = 0;
            if (sscanf(seg, "%u:%d:%d:%d:%d:%u:%d", &spellId, &prio, &target,
                    &hpMin, &hpMax, &auraId, &present) != 7 ||
                !spellId || prio < 0 || target < 0 || target > 2 ||
                hpMin < 0 || hpMax > 100 || hpMin > hpMax ||
                (present != 0 && present != 1) ||
                !sSpellMgr.GetSpellEntry(spellId) ||
                (auraId && !sSpellMgr.GetSpellEntry(auraId)))
            {
                CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: invalid rotation instruction, error ack", spellId);
                sendAck("error", "invalid_rotation", 0);
                return;
            }

            // A non-reset operation cannot change the known spell set, so reject
            // an unlearned entry before touching even role metadata. A rebuild is
            // re-resolved after the new talent prefix has been purchased.
            if (!resetTalents && !me->HasSpell(spellId))
            {
                CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: rotation spell unlearned, error ack", spellId);
                sendAck("error", "rotation_spell_unlearned", 0);
                return;
            }

            RotationInstruction inst;
            inst.spellId = spellId;
            inst.pSpell = sSpellMgr.GetSpellEntry(spellId);
            inst.target = uint8(target);
            inst.hpMin = hpMin;
            inst.hpMax = hpMax;
            inst.auraId = auraId;
            inst.auraPresent = present != 0;
            stagedRotation.push_back(inst);
        }
        if (stagedRotation.empty())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: rotation parsed empty, error ack");
            sendAck("error", "invalid_rotation", 0);
            return;
        }
    }

    if (m_possessed)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot possessed, error ack");
        sendAck("error", "bot_possessed", 0);
        return;
    }
    if (!me->IsAlive())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot dead, error ack");
        sendAck("error", "bot_dead", 0);
        return;
    }
    if (me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot in combat, error ack");
        sendAck("error", "bot_in_combat", 0);
        return;
    }
    if (me->IsNonMeleeSpellCasted(false, false, true))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot casting, error ack");
        sendAck("error", "bot_casting", 0);
        return;
    }
    if (me->IsBeingTeleported())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot teleporting, error ack");
        sendAck("error", "bot_teleporting", 0);
        return;
    }
    if (!me->GetTaxi().empty() || me->HasUnitState(UNIT_STATE_TAXI_FLIGHT))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot on taxi, error ack");
        sendAck("error", "bot_on_taxi", 0);
        return;
    }
    if (me->InBattleGround())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: bot in battleground, error ack");
        sendAck("error", "bot_in_battleground", 0);
        return;
    }

    AiBotTalents::TalentSnapshot beforeTalents;
    if (!AiBotTalents::CaptureSnapshot(me, botEntry, beforeTalents))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: talent snapshot failed, error ack");
        sendAck("error", "snapshot_failed", 0);
        return;
    }
    std::vector<RotationInstruction> const beforeRotation = m_rotation;
    std::string const beforeRotationProfile = m_rotationProfile;
    uint32 const beforeInstructionCount = m_rotationInstructionCount;
    uint32 const beforeCastableCount = m_rotationCastableCount;
    CombatBotRoles const beforeRole = m_role;

    // Clear cached talent spell pointers before the free reset. They are restored
    // only after the corresponding talent snapshot has been restored.
    m_rotation.clear();
    m_rotationProfile.clear();
    m_rotationInstructionCount = 0;
    m_rotationCastableCount = 0;

    AiBotTalents::ApplyResult applied = AiBotTalents::ApplyProfileAndRole(
        me, botEntry, uint8(specTab), requestedRole, resetTalents);

    auto refreshLifecycle = [&]()
    {
        LearnBotClassQuestSpells();
        LearnTrainerAndItemSpells();
        LearnArmorProficiencies();
        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        me->UpdateSkillsToMaxSkillsForLevel();
        SummonPetIfNeeded();
    };

    if (applied.status != AiBotTalents::TALENT_APPLY_OK)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: talent apply failed, error ack", applied.status);
        if (applied.status != AiBotTalents::TALENT_APPLY_ROLLBACK_FAILED)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: restoring previous rotation state");
            m_rotation = beforeRotation;
            m_rotationProfile = beforeRotationProfile;
            m_rotationInstructionCount = beforeInstructionCount;
            m_rotationCastableCount = beforeCastableCount;
        }
        m_role = beforeRole;
        if (applied.resetPerformed)
            refreshLifecycle(); // cb:fold lifecycle refresh, apply status probed above
        me->SaveToDB();
        sendAck("error", AiBotTalents::GetApplyStatusCode(applied.status), applied.learnedPoints);
        BridgeSendState();
        return;
    }

    m_role = applied.role;
    if (applied.resetPerformed)
        refreshLifecycle(); // cb:fold lifecycle refresh after reset

    if (useCustomRotation)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: adopting staged rotation after apply");
        bool allKnown = true;
        for (RotationInstruction& inst : stagedRotation)
        {
            inst.pSpell = me->HasSpell(inst.spellId) ? sSpellMgr.GetSpellEntry(inst.spellId) : nullptr;
            if (!inst.pSpell)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: staged spell unknown after apply", inst.spellId);
                allKnown = false;
                break;
            }
        }

        if (!allKnown)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: unlearned spell after apply, rolling back");
            bool const rolledBack = AiBotTalents::RestoreSnapshot(me, botEntry, beforeTalents);
            m_role = beforeRole;
            refreshLifecycle();
            if (rolledBack)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: snapshot rollback ok, rotation restored");
                m_rotation = beforeRotation;
                m_rotationProfile = beforeRotationProfile;
                m_rotationInstructionCount = beforeInstructionCount;
                m_rotationCastableCount = beforeCastableCount;
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: rollback failed, rotation cleared");
                m_rotation.clear();
                m_rotationProfile.clear();
                m_rotationInstructionCount = 0;
                m_rotationCastableCount = 0;
            }
            me->SaveToDB();
            sendAck("error", rolledBack ? "rotation_spell_unlearned" : "rollback_failed", applied.learnedPoints);
            BridgeSendState();
            return;
        }

        m_rotation.swap(stagedRotation);
        m_rotationProfile = rotationProfile;
        m_rotationInstructionCount = stagedInstructionCount;
        m_rotationCastableCount = stagedInstructionCount;
    }

    me->SaveToDB();
    ++m_combatConfigRevision;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-LOADOUT] %s request=%s applied profile=%s role=%u reset=%u rotation=%s revision=%u",
        me->GetName(), requestId, AiBotTalents::GetProfileName(me->GetClass(), botEntry->specTab),
        uint32(m_role), resetTalents ? 1u : 0u, GetEffectiveRotationSource(), m_combatConfigRevision);
    sendAck("ok", "ok", applied.learnedPoints);
    BridgeSendState();
}

// ============================================================
// BridgeHandleSetEscort — [FOLLOW-CMD] "{bot} follow {player}" (2026-07-16)
//
// Sets/clears the escort override consumed by FindEscortBoss. C# recognizes the
// addressed party-chat command deterministically (BotBridgeService CHAT_RECV) and
// sends {"player_name": "Athren"} — or "" to revert to the GUIDLow-modulo auto
// split ("{bot} follow auto" / bare "{bot} follow"). The name is stored LOWERCASED;
// resolution against group members is case-insensitive at follow time, and a name
// that never resolves (offline / left / typo) silently falls back to the auto
// split, so a bad command can never strand a bot. Fire-and-forget: no ack event —
// C# speaks the party-chat confirmation itself.
// ============================================================
void AiBotAI::BridgeHandleSetEscort(const char* json)
{
    char nameBuf[64] = {0};
    JsonExtractString(json, "player_name", nameBuf, sizeof(nameBuf));   // absent/empty = clear

    std::string lowered;
    for (char const* p = nameBuf; *p; ++p)
        lowered.push_back((char)tolower((unsigned char)*p));

    m_escortOverrideName = lowered;
    if (CancelPendingNpcInteraction("set_escort") && !me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: escort order tears down preempted NPC approach");
        StopMoving();
    }
    if (m_escortOverrideName.empty())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: escort override cleared, auto split resumes");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-PARTY] %s: escort override CLEARED (auto split)", me->GetName());
    }
    else
    {
        CB_HITN(me->GetGUIDLow(), "cpp-group: escort override set", nameBuf);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-PARTY] %s: escort override -> '%s'", me->GetName(), nameBuf);
    }
}

void AiBotAI::BridgeHandleSayText(const char* json)
{
    char text[256] = {0};
    char target[64] = {0};
    char channel[64] = {0};
    int chatType = 0;

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    JsonExtractString(payload, "text", text, sizeof(text));
    JsonExtractInt(payload, "chatType", chatType);
    JsonExtractString(payload, "target", target, sizeof(target));
    JsonExtractString(payload, "channel", channel, sizeof(channel));

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: SAY_TEXT type=%d target=%s channel=%s: %s",
        me->GetName(), chatType, target, channel, text);

    if (strlen(text) == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: SAY_TEXT empty text, ignored");
        return;
    }

    if (chatType == 7 && strlen(target) > 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: whisper path");
        // WHISPER — build and send whisper packet directly to target player
        // Player class doesn't have Whisper(); we use ChatHandler::BuildChatPacket
        Player* pTarget = sObjectMgr.GetPlayer(target);
        if (pTarget && pTarget->GetSession())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: whisper sent to target");
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, text, LANG_UNIVERSAL,
                CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName(),
                pTarget->GetObjectGuid());
            pTarget->GetSession()->SendPacket(&data);

            // Send WHISPER_INFORM back to self so bot's chat log shows it
            WorldPacket data2;
            ChatHandler::BuildChatPacket(data2, CHAT_MSG_WHISPER_INFORM, text, LANG_UNIVERSAL,
                CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName(),
                pTarget->GetObjectGuid());
            if (me->GetSession())
                me->GetSession()->SendPacket(&data2); // cb:fold self-echo packet detail
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: whisper target not found, fallback say");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: WHISPER target '%s' not found, falling back to SAY",
                me->GetName(), target);
            me->Say(text, LANG_UNIVERSAL);
        }
    }
    else if (chatType == 14 && strlen(channel) > 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: channel chat path");
        // CHANNEL — send to named channel via Channel::Say
        if (ChannelMgr* cMgr = channelMgr(me->GetTeam())) // cb:fold decl-if condition artifact, body probed
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: channel mgr resolved");
            // GetJoinChannel returns existing or creates — channel should already exist
            if (Channel* chn = cMgr->GetJoinChannel(std::string(channel))) // cb:fold decl-if condition artifact, body probed
            {
                CB_HIT(me->GetGUIDLow(), "cpp-bridge: channel say sent");
                chn->Say(me->GetObjectGuid(), text, LANG_UNIVERSAL, true);
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-bridge: channel not found, fallback say");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: channel '%s' not found, falling back to SAY",
                    me->GetName(), channel);
                me->Say(text, LANG_UNIVERSAL);
            }
        }
    }
    else if (chatType == 6)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: yell");
        me->Yell(text, LANG_UNIVERSAL);
    }
    else if (chatType == 1)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: party chat path");
        // PARTY — wire int 1 == CHAT_MSG_PARTY (0x01, VERIFIED SharedDefines.h).
        // Mirrors the whisper branch's packet-building idiom. Broadcast VERIFIED Group.h:
        //   void BroadcastPacket(WorldPacket* packet, bool ignorePlayersInBGRaid, int group=-1, ObjectGuid ignore = ObjectGuid());
        // Self receives its own line (like a real client); the C0 self-echo filter in
        // OnPacketReceived keeps it from re-entering the coordinator as a stimulus.
        if (Group* pGroup = me->GetGroup()) // cb:fold decl-if condition artifact, body probed
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: party chat broadcast");
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text, LANG_UNIVERSAL,
                CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName());
            pGroup->BroadcastPacket(&data, false);
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: party requested but ungrouped, fallback say");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: PARTY requested but ungrouped, falling back to SAY",
                me->GetName());
            me->Say(text, LANG_UNIVERSAL);
        }
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: plain say");
        me->Say(text, LANG_UNIVERSAL);
    }
}

// ============================================================
// PHASE 2.5: Quest / Combat / Interaction bridge commands
// ============================================================

void AiBotAI::BridgeHandleQuestInteract(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    int questId = 0, npcEntry = 0;
    char action[16] = {0};
    JsonExtractInt(payload, "quest_id", questId);
    JsonExtractInt(payload, "npc_entry", npcEntry);
    JsonExtractString(payload, "action", action, sizeof(action));

    if (questId <= 0 || npcEntry <= 0 || action[0] == '\0')
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: QUEST_INTERACT bad payload", questId);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT bad payload: action='%s' quest=%d npc=%d",
            me->GetName(), action, questId, npcEntry);
        return;
    }

    // ── Find the specific NPC by creature_template entry within 15 yards ──
    Creature* pNpc = nullptr;
    {
        std::list<Creature*> creatureList;
        me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);
        float bestDist = 999.0f;
        for (auto* pCreature : creatureList)
        {
            if (pCreature && pCreature->IsAlive())
            {   // cb:fold nearest candidate scan
                float d = me->GetDistance(pCreature);
                if (d < bestDist)
                {   // cb:fold nearest candidate scan
                    bestDist = d;
                    pNpc = pCreature;
                }
            }
        }
    }

    if (!pNpc)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: giver not found within 15yd, QUEST_INTERACT_FAIL", npcEntry);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT npc entry %d not found within 15yd",
            me->GetName(), npcEntry);

        char buf[128];
        snprintf(buf, sizeof(buf), "npc_not_found|quest_id=%d|npc_entry=%d", questId, npcEntry);
        BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
        return;
    }

    Quest const* pQuest = sObjectMgr.GetQuestTemplate((uint32)questId);
    if (!pQuest)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: quest template missing, QUEST_INTERACT_FAIL", questId);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT quest %d not found in quest_template",
            me->GetName(), questId);

        char buf[128];
        snprintf(buf, sizeof(buf), "quest_not_found|quest_id=%d", questId);
        BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
        return;
    }

    // ══════════════════════════════════════════════
    //  ACCEPT
    // ══════════════════════════════════════════════
    if (strcmp(action, "accept") == 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: accept requested", questId);

       // ── Idempotent accept (already-in-log) — REVISED 2026-06-30 ──────────────
        // GetQuestStatus() alone can't tell "still actively held" from "rewarded an
        // hour ago" — VMaNGOS sticks a turned-in quest's status at COMPLETE forever;
        // m_rewarded is the bit that actually means done (BridgeSendState's quest
        // blob already gates on it). Without this check, a stray re-ACCEPT for an
        // already-rewarded quest fell into the "idempotent ACK" branch below, told
        // C# the accept succeeded, and let it dispatch a brand-new objective for a
        // quest that will never again be turn-in-able — the no-op double-grind bug.
        QuestStatus existingStatus = me->GetQuestStatus((uint32)questId);
        if (existingStatus != QUEST_STATUS_NONE)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-quest: quest already known to log, checking rewarded", existingStatus);
            const auto& qMap = me->GetQuestStatusMap();
            auto it = qMap.find((uint32)questId);
            bool alreadyRewarded = (it != qMap.end()) && it->second.m_rewarded;

            if (alreadyRewarded)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-quest: already rewarded, accept refused, QUEST_INTERACT_FAIL", questId);
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[AIBOT-BRIDGE] %s: accept quest %d refused — already rewarded (turned in)",
                    me->GetName(), questId);
                char buf[64];
                snprintf(buf, sizeof(buf), "already_rewarded|quest_id=%d", questId);
                BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
                return;
            }

            // Genuinely still held (the case this guard was originally built for) —
            // a reward-granted chain follow-up or a re-pick that lost the Accepted
            // flag. ACK it like a normal accept so C# reconciles the batch.
            m_trackedQuestId = (uint32)questId;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: accept quest %d already in log (status=%u) — idempotent ACK",
                me->GetName(), questId, (uint32)existingStatus);
            char buf[64];
            snprintf(buf, sizeof(buf), "%d", questId);
            BridgeSendEvent("QUEST_ACCEPT_ACK", buf);
            return;
        }

        if (!me->CanTakeQuest(pQuest, false))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-quest: CanTakeQuest failed, QUEST_INTERACT_FAIL", questId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT accept quest %d — CanTakeQuest failed",
                me->GetName(), questId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-DEBUG] %s: quest %d FAILED — "
                "Status=%d ExclGrp=%d Class=%d Race=%d Level=%d Skill=%d "
                "Cond=%d Rep=%d PrevQ=%d Timed=%d NextC=%d PrevC=%d "
                "Bread=%d DepBread=%d Active=%d",
                me->GetName(), questId,
                me->SatisfyQuestStatus(pQuest, false) ? 1 : 0,
                me->SatisfyQuestExclusiveGroup(pQuest, false) ? 1 : 0,
                me->SatisfyQuestClass(pQuest, false) ? 1 : 0,
                me->SatisfyQuestRace(pQuest, false) ? 1 : 0,
                me->SatisfyQuestLevel(pQuest, false) ? 1 : 0,
                me->SatisfyQuestSkill(pQuest, false) ? 1 : 0,
                me->SatisfyQuestCondition(pQuest, false) ? 1 : 0,
                me->SatisfyQuestReputation(pQuest, false) ? 1 : 0,
                me->SatisfyQuestPreviousQuest(pQuest, false) ? 1 : 0,
                me->SatisfyQuestTimed(pQuest, false) ? 1 : 0,
                me->SatisfyQuestNextChain(pQuest, false) ? 1 : 0,
                me->SatisfyQuestPrevChain(pQuest, false) ? 1 : 0,
                me->SatisfyQuestBreadcrumbQuest(pQuest, false) ? 1 : 0,
                me->SatisfyQuestDependentBreadcrumbQuests(pQuest, false) ? 1 : 0,
                pQuest->IsActive() ? 1 : 0);

            char buf[128];
            snprintf(buf, sizeof(buf), "requirements_not_met|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        if (!me->CanAddQuest(pQuest, true))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-quest: quest log full, QUEST_INTERACT_FAIL", questId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT accept quest %d — CanAddQuest failed (log full?)",
                me->GetName(), questId);

            char buf[128];
            snprintf(buf, sizeof(buf), "quest_log_full|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        me->AddQuest(pQuest, pNpc);
        m_trackedQuestId = (uint32)questId;

        // Session 27: zero-objective delivery/talk quests need explicit CompleteQuest so
        // CanRewardQuest passes at turn-in.
        if (pQuest->GetReqCreatureOrGOcount() == 0 && pQuest->GetReqItemsCount() == 0
            && me->GetQuestStatus((uint32)questId) != QUEST_STATUS_COMPLETE)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-quest: no objectives, marked complete for turn-in", questId);
            me->CompleteQuest((uint32)questId);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: quest %d has no objectives — marked COMPLETE for turn-in",
                me->GetName(), questId);
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: accepted quest %d '%s' from %s (entry %d)",
            me->GetName(), questId, pQuest->GetTitle().c_str(),
            pNpc->GetName(), npcEntry);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", questId);
        BridgeSendEvent("QUEST_ACCEPT_ACK", buf);
        SendQuestUpdateEvent(questId, "accepted");
    }
    // ══════════════════════════════════════════════
    //  COMPLETE (turn-in for reward)
    // ══════════════════════════════════════════════
    else if (strcmp(action, "complete") == 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: complete requested", questId);
        QuestStatus status = me->GetQuestStatus((uint32)questId);
        if (status == QUEST_STATUS_NONE)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-quest: not in log, QUEST_INTERACT_FAIL", questId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT complete quest %d — not in quest log",
                me->GetName(), questId);

            char buf[128];
            snprintf(buf, sizeof(buf), "quest_not_in_log|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        // Pick the best CHOICE reward (gear upgrade by score, else highest vendor value).
        // Fixed rewards are granted regardless; this index only selects among "pick one" items.
        uint32 rewardChoice = ChooseQuestReward(pQuest);

        if (!me->CanRewardQuest(pQuest, rewardChoice, false))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-quest: CanRewardQuest failed, QUEST_INTERACT_FAIL", questId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT complete quest %d — CanRewardQuest failed (status=%u choice=%u)",
                me->GetName(), questId, (uint32)status, rewardChoice);

            char buf[128];
            snprintf(buf, sizeof(buf), "cannot_reward|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        // RewardQuest: canonical reward path (XP, money, rep, items, spell cast). The chosen
        // index now reflects ScoreItem, not a blind 0.
        me->RewardQuest(pQuest, rewardChoice, pNpc, true);
        m_trackedQuestId = 0;
        TryAutoEquipBags();
        TryAutoEquip();

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: completed quest %d '%s' at %s (entry %d, rewardChoice=%u)",
            me->GetName(), questId, pQuest->GetTitle().c_str(),
            pNpc->GetName(), npcEntry, rewardChoice);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", questId);
        BridgeSendEvent("QUEST_COMPLETE_ACK", buf);
        SendQuestUpdateEvent(questId, "rewarded");
    }
    else
    {
        CB_HITN(me->GetGUIDLow(), "cpp-quest: unknown action", action);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT unknown action '%s'",
            me->GetName(), action);
    }
}

void AiBotAI::BridgeHandleAbandonQuest(const char* json)
{
    int questId = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    JsonExtractInt(payload, "quest_id", questId);

    if (questId <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-quest: ABANDON_QUEST missing quest_id");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ABANDON_QUEST missing quest_id", me->GetName());
        return;
    }

    QuestStatus status = me->GetQuestStatus(questId);
    if (status == QUEST_STATUS_NONE)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: abandon target not in log", questId);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ABANDON_QUEST quest %d not in log", me->GetName(), questId);
        return;
    }

    // PLAN_20 P2: the full abandon, not just the status poke -- this used to
    // leave the update-field slot occupied, the timed-quest registration live
    // and the quest items in the bags.
    me->RemoveQuestById(questId);
    if (m_trackedQuestId == (uint32)questId)
        m_trackedQuestId = 0; // cb:fold tracked quest bookkeeping

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: abandoned quest %d", me->GetName(), questId);
    SendQuestUpdateEvent(questId, "abandoned");
}

void AiBotAI::BridgeHandleLearnSpell(const char* json)
{
    int spellId = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    JsonExtractInt(payload, "spell_id", spellId);

    if (spellId <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-train: LEARN_SPELL missing spell_id");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: LEARN_SPELL missing spell_id", me->GetName());
        return;
    }

    if (me->HasSpell(spellId))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-train: spell already known, ignored", spellId);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: LEARN_SPELL already knows %d", me->GetName(), spellId);
        return;
    }

    me->LearnSpell(spellId, false);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: learned spell %d", me->GetName(), spellId);
}

void AiBotAI::BridgeHandleTrain(const char* json)
{
    int npcEntry = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    JsonExtractInt(payload, "npc_entry", npcEntry);

    if (npcEntry <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-train: missing npc_entry, TRAIN_FAIL");
        BridgeSendEvent("TRAIN_FAIL", "reason=missing_npc_entry");
        return;
    }

    // Stop movement so we don't walk away between search and train
    StopMoving();

    // ── Diagnostic: search at 15yd first, then wider if not found ──
    std::list<Creature*> creatureList;
    me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TRAIN] %s: searching for trainer entry %d — found %zu creatures within 15yd, bot at (%.1f, %.1f, %.1f)",
        me->GetName(), npcEntry, creatureList.size(),
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());

    // Log every creature found (even if it fails the trainer check)
    for (auto* c : creatureList)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s:   candidate: '%s' entry=%u guid=%u alive=%d npc_flags=0x%X dist=%.1f (%.1f,%.1f,%.1f)",
            me->GetName(), c->GetName(), c->GetEntry(), c->GetGUIDLow(),
            c->IsAlive() ? 1 : 0,
            c->GetUInt32Value(UNIT_NPC_FLAGS),
            me->GetDistance(c),
            c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());
    }

    Creature* pTrainer = nullptr;
    float bestDist = 999.0f;
    for (auto* c : creatureList)
    {
        if (c && c->IsAlive() && (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_TRAINER))
        {   // cb:fold nearest candidate scan
            float d = me->GetDistance(c);
            if (d < bestDist)
            {   // cb:fold nearest candidate scan
                bestDist = d;
                pTrainer = c;
            }
        }
    }

    // ── Fallback: wider search if narrow failed ──
    if (!pTrainer)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-train: 15yd search failed, widening to 50yd");
        std::list<Creature*> wideList;
        me->GetCreatureListWithEntryInGrid(wideList, (uint32)npcEntry, 50.0f);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s: 15yd search failed — wide search (50yd) found %zu creatures",
            me->GetName(), wideList.size());

        for (auto* c : wideList)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-TRAIN] %s:   wide: '%s' entry=%u alive=%d flags=0x%X dist3d=%.1f pos=(%.1f,%.1f,%.1f)",
                me->GetName(), c->GetName(), c->GetEntry(), c->GetGUIDLow(),
                c->IsAlive() ? 1 : 0,
                c->GetUInt32Value(UNIT_NPC_FLAGS),
                me->GetDistance(c),
                c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());

            if (c && c->IsAlive() && (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_TRAINER))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-train: trainer found in wide search");
                pTrainer = c;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-TRAIN] %s: found trainer in wide search at dist=%.1f — using it",
                    me->GetName(), me->GetDistance(c));
                break;
            }
        }
    }

    if (!pTrainer)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-train: trainer not found within 50yd, TRAIN_FAIL", npcEntry);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s: trainer entry %d not found within 50yd",
            me->GetName(), npcEntry);
        BridgeSendEvent("TRAIN_FAIL", "reason=trainer_not_found");
        return;
    }

    if (!pTrainer->IsTrainerOf(me, false))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-train: not a trainer for this class, TRAIN_FAIL", npcEntry);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s: NPC %d ('%s') is not a trainer for this class",
            me->GetName(), npcEntry, pTrainer->GetName());
        BridgeSendEvent("TRAIN_FAIL", "reason=wrong_class");
        return;
    }

    TrainerSpellData const* cSpells = pTrainer->GetTrainerSpells();
    TrainerSpellData const* tSpells = pTrainer->GetTrainerTemplateSpells();

    if (!cSpells && !tSpells)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-train: trainer has no spell lists, TRAIN_FAIL");
        BridgeSendEvent("TRAIN_FAIL", "reason=no_spells");
        return;
    }

    int totalLearned = 0;
    uint32 totalCost = 0;
    bool learnedAnything;

    do
    {
        learnedAnything = false;

        auto processSpellList = [&](TrainerSpellData const* spells)
        {
            if (!spells) return; // cb:fold empty spell list guard, TRAIN_ACK carries counts
            for (auto const& itr : spells->spellList)
            {
                TrainerSpell const* tSpell = &itr.second;
                if (me->GetTrainerSpellState(tSpell) != TRAINER_SPELL_GREEN)
                    continue; // cb:fold per-spell eligibility scan, TRAIN_ACK carries counts

                SpellEntry const* spellEntry = sSpellMgr.GetSpellEntry(tSpell->spell);
                if (!spellEntry) continue; // cb:fold per-spell eligibility scan

                uint32 triggerSpell = spellEntry->EffectTriggerSpell[0];
                if (!triggerSpell) continue; // cb:fold per-spell eligibility scan

                if (sSpellMgr.IsPrimaryProfessionFirstRankSpell(triggerSpell))
                    continue; // cb:fold per-spell eligibility scan

                if (!me->IsSpellFitByClassAndRace(triggerSpell))
                    continue; // cb:fold per-spell eligibility scan

                uint32 spellCost = tSpell->spellCost;
                if (me->GetMoney() < spellCost)
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-train: cannot afford spell, skipped", spellCost);
                    continue;
                }

                me->ModifyMoney(-(int32)spellCost);
                me->LearnSpell(triggerSpell, false);

                totalCost += spellCost;
                totalLearned++;
                learnedAnything = true;

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-TRAIN] %s: learned spell %u (cost=%u copper)",
                    me->GetName(), triggerSpell, spellCost);
            }
        };

        processSpellList(cSpells);
        processSpellList(tSpells);

    } while (learnedAnything);

    if (totalLearned > 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-train: spells learned, refreshing spell data", totalLearned);
        ResetSpellData();
        PopulateSpellData();
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "learned=%d|cost=%u|gold=%u",
        totalLearned, totalCost, me->GetMoney());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TRAIN] %s: complete — %d spells, %u copper spent, %u remaining",
        me->GetName(), totalLearned, totalCost, me->GetMoney());

    BridgeSendEvent("TRAIN_ACK", buf);
}

void AiBotAI::BridgeHandleAttackTarget(const char* json)
{
    int guidLow = 0;
    int entry = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    bool const haveGuid = JsonExtractInt(payload, "guid", guidLow);
    bool const haveEntry = JsonExtractInt(payload, "entry", entry);

    if (!haveGuid || !haveEntry || guidLow <= 0 || entry <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: ATTACK_TARGET bad creature identity, terminal fail");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: ATTACK_TARGET bad payload (entry=%d guid=%d)",
            me->GetName(), entry, guidLow);
        char eventData[128];
        snprintf(eventData, sizeof(eventData),
            "reason=bad_payload|entry=%d|guid=%d", entry, guidLow);
        BridgeSendEvent("ATTACK_TARGET_FAIL", eventData);
        return;
    }

    // Creature identity is (HIGHGUID_UNIT, entry, counter). Map::GetCreature compares the complete
    // ObjectGuid, so accepting only guidLow creates an identity that cannot resolve reliably.
    Creature* pCreature = me->GetMap()->GetCreature(
        ObjectGuid(HIGHGUID_UNIT, uint32(entry), uint32(guidLow)));

    if (!pCreature)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: attack target not found, terminal fail", guidLow);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: ATTACK_TARGET creature entry %d guid %d not found on map",
            me->GetName(), entry, guidLow);
        char eventData[128];
        snprintf(eventData, sizeof(eventData),
            "reason=not_found|entry=%d|guid=%d", entry, guidLow);
        BridgeSendEvent("ATTACK_TARGET_FAIL", eventData);
        return;
    }

    if (!IsValidHostileTarget(pCreature))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: attack target not hostile, terminal fail", guidLow);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: ATTACK_TARGET entry %d guid %d is not a valid hostile target",
            me->GetName(), entry, guidLow);
        char eventData[128];
        snprintf(eventData, sizeof(eventData),
            "reason=not_hostile|entry=%d|guid=%d", entry, guidLow);
        BridgeSendEvent("ATTACK_TARGET_FAIL", eventData);
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: attacking %s (guid %d)",
        me->GetName(), pCreature->GetName(), guidLow);
    if (CancelPendingNpcInteraction("attack_target"))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: attack order tears down preempted NPC approach");
        // AttackStart is a no-op for the already-selected victim. Retire that
        // attack first so an NPC point spline cannot survive underneath a
        // same-target explicit attack order.
        me->AttackStop(false);
        StopMoving();
    }
    m_suiOrderedAttackPass = true;      // the commander's explicit order passes the manual gate
    AttackStart(pCreature);
    m_suiOrderedAttackPass = false;
}

void AiBotAI::BridgeHandleInteractNpc(const char* json)
{
    int guidLow = 0;
    int entry = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    bool const haveGuid = JsonExtractInt(payload, "guid", guidLow);
    bool const haveEntry = JsonExtractInt(payload, "entry", entry);

    if (!haveGuid || !haveEntry || guidLow <= 0 || entry <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: INTERACT_NPC bad creature identity, terminal fail");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: INTERACT_NPC bad payload (entry=%d guid=%d)",
            me->GetName(), entry, guidLow);
        char eventData[128];
        snprintf(eventData, sizeof(eventData),
            "reason=bad_payload|entry=%d|guid=%d", entry, guidLow);
        BridgeSendEvent("NPC_INTERACT_FAIL", eventData);
        return;
    }

    // INTERACT_NPC uses the same complete creature identity as ATTACK_TARGET. Never guess an
    // entry-less unit ObjectGuid from a low counter; it can select nothing and used to fail silently.
    Creature* pCreature = me->GetMap()->GetCreature(
        ObjectGuid(HIGHGUID_UNIT, uint32(entry), uint32(guidLow)));

    if (!pCreature)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: interact npc not found, terminal fail", guidLow);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: INTERACT_NPC creature entry %d guid %d not found",
            me->GetName(), entry, guidLow);
        char eventData[128];
        snprintf(eventData, sizeof(eventData),
            "reason=not_found|entry=%d|guid=%d", entry, guidLow);
        BridgeSendEvent("NPC_INTERACT_FAIL", eventData);
        return;
    }

    // A newer manual interaction supersedes an older approach. StopMoving's SUI latch suppresses
    // that point generator's synchronous cancellation callback; clear its owner before continuing.
    if (!m_pendingInteractNpcGuid.IsEmpty())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-bridge: replacing pending NPC interaction approach");
        CancelPendingNpcInteraction("interact_npc");
        StopMoving();
    }

    float dist = me->GetDistance(pCreature);
    if (dist > 10.0f)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: npc too far, moving closer first", dist);
        // Too far — move closer first, then interact on arrival
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: INTERACT_NPC moving to %s (dist=%.1f)",
            me->GetName(), pCreature->GetName(), dist);
        // This is a temporary manual approach, not an autonomous-task arrival. Guard the old task
        // finalizer, keep the task state available for later resume, and use a distinct point id.
        m_suppressTaskDestInform = true;
        StopMoving();
        m_pendingInteractNpcGuid = pCreature->GetObjectGuid();
        m_pendingInteractNpcCbt = m_bridgeDispatchCbt;
        float nx, ny, nz;
        pCreature->GetContactPoint(me, nx, ny, nz);
        if (!MovePointRun(AIBOT_POINT_INTERACT_NPC, nx, ny, nz, false))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-bridge: INTERACT_NPC approach NOPATH, terminal fail");
            uint64 const interactCbt = m_pendingInteractNpcCbt;
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

    // Face the NPC
    me->SetFacingToObject(pCreature);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: interacting with %s (guid %d)",
        me->GetName(), pCreature->GetName(), guidLow);
    BridgeSendEvent("NPC_INTERACT", pCreature->GetName());
}

void AiBotAI::BridgeHandleSetTask(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    char taskType[32] = {0};
    JsonExtractString(payload, "task", taskType, sizeof(taskType));

    if (strcmp(taskType, "GRIND") == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: SET_TASK GRIND adopted");
        m_suppressTaskDestInform = true;
        CancelPendingNpcInteraction("set_task_grind");
        if (!me->IsInCombat())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: preempting prior task for OOC GRIND replacement");
            StopMoving();
        }
        ClearStoredPath();
        m_currentTask.Clear();
        m_currentTask.type = TASK_GRIND;
        m_currentTask.commandCbt = m_bridgeDispatchCbt;
        JsonExtractFloat(payload, "x", m_currentTask.x);
        JsonExtractFloat(payload, "y", m_currentTask.y);
        JsonExtractFloat(payload, "z", m_currentTask.z);
        JsonExtractFloat(payload, "radius", m_currentTask.radius);

        int entry = 0, goal = 0;
        JsonExtractInt(payload, "creature_entry", entry);
        JsonExtractInt(payload, "kill_count", goal);
        m_currentTask.creatureEntry = (uint32)entry;
        m_currentTask.killGoal = goal;
        m_currentTask.killCount = 0;

        if (m_currentTask.radius < 10.0f)
            m_currentTask.radius = 40.0f;  // cb:fold default radius clamp (sane default)

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: SET_TASK GRIND entry=%u goal=%d at (%.1f,%.1f,%.1f) r=%.0f",
            me->GetName(), m_currentTask.creatureEntry,
            m_currentTask.killGoal,
            m_currentTask.x, m_currentTask.y, m_currentTask.z,
            m_currentTask.radius);

        // Immediately move to grind area if not already there
        float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);
        if (!me->IsInCombat() && dist > m_currentTask.radius)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-task: outside grind radius, moving to area", dist);
            if (!MovePointRun(AIBOT_POINT_GRIND_PATROL,
                    m_currentTask.x, m_currentTask.y, m_currentTask.z, false))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: initial grind approach NOPATH, terminal fail");
                uint64 const taskCbt = m_currentTask.commandCbt;
                char eventData[256];
                snprintf(eventData, sizeof(eventData),
                    "dest_x=%.1f|dest_y=%.1f|dest_z=%.1f|reason=no_path|source=set_task_approach|start_isolated=%u",
                    m_currentTask.x, m_currentTask.y, m_currentTask.z,
                    IsStartIsolated() ? 1u : 0u);
                m_currentTask.Clear();
                BridgeSendEvent("MOVE_FAILED", eventData, taskCbt);
            }
        }
    }
    else if (strcmp(taskType, "IDLE") == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: SET_TASK IDLE, task cleared");
        m_suppressTaskDestInform = true;
        CancelPendingNpcInteraction("set_task_idle");
        if (!me->IsInCombat())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: preempting prior task for OOC IDLE replacement");
            StopMoving();
        }
        ClearStoredPath();
        m_currentTask.Clear();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: SET_TASK IDLE (clearing task)", me->GetName());
    }
    else if (strcmp(taskType, "PORT_HOME") == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-task: PORT_HOME requested");
        // [ESCAPE] (FINDING_010) Living-bot stranded escape. The C# wedge-streak escalation asks
        // for a port to the racial start when a bot has proven it can neither kill nor quest where
        // it stands (Everlook L18 / Badlands L21 census — the park→local-relocate ladder shuffles
        // it ~50yd forever). The DEAD path is FINDING_008's ghost port; this one is alive-only and
        // refuses in combat (threat/aggro transfer). Same-map uses the proven NearTeleportTo seam;
        // cross-continent uses the full Player::TeleportTo far-teleport (bots are real in-core
        // players, same machinery as any player worldport).
        float hx = 0.0f, hy = 0.0f, hz = 0.0f; int hmap = -1;
        bool haveHome = JsonExtractFloat(payload, "home_x", hx) && JsonExtractFloat(payload, "home_y", hy) &&
                        JsonExtractFloat(payload, "home_z", hz) && JsonExtractInt(payload, "home_map", hmap);
        if (haveHome && hmap >= 0 && me->IsAlive() && !me->IsInCombat())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: stranded escape accepted, hearth attempt");
            float fx = me->GetPositionX(), fy = me->GetPositionY(), fz = me->GetPositionZ();
            uint32 fmap = me->GetMapId();
            m_suppressTaskDestInform = true;
            CancelPendingNpcInteraction("port_home");
            StopMoving();
            ClearStoredPath();
            m_currentTask.Clear();

            // [HEARTH] Instead of the old instant TeleportTo, cast a real Hearthstone (8690) so the
            // escape shows the authentic ~10s cast bar + animation and is interrupted by damage /
            // movement like a player's. We point the bot's homebind at the escape town for the cast,
            // so the spell's OWN teleport (TeleportToHomebind) lands it there; HandleHearthCast (in
            // UpdateAI) restores the homebind + re-anchors spawn on completion, or aborts on interrupt.
            WorldLocation const& oldHome = me->GetHomebindLoc();
            m_hearthSavedHomeMap = oldHome.mapId;
            m_hearthSavedHomeX = oldHome.x; m_hearthSavedHomeY = oldHome.y; m_hearthSavedHomeZ = oldHome.z;
            m_hearthSavedArea = me->GetHomeBindAreaId();
            me->SetHomebindInMemory(WorldLocation((uint32)hmap, hx, hy, hz, 0.0f), 0);

            me->CastSpell(me, AIBOT_HEARTH_SPELL_ID, false);
            if (me->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: hearth cast begun");
                m_hearthX = hx; m_hearthY = hy; m_hearthZ = hz; m_hearthMap = hmap;
                m_hearthElapsedMs = 0;
                m_hearthActive = true;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s HEARTH cast begun (stranded escape): (%.1f, %.1f, %.1f) map=%u -> (%.1f, %.1f, %.1f) map=%d",
                    me->GetName(), fx, fy, fz, fmap, hx, hy, hz, hmap);
            }
            else
            {
                CB_HIT(me->GetGUIDLow(), "cpp-task: hearth unavailable, instant port fallback");
                // Hearth could not start (silenced / spell unavailable) — restore the homebind and
                // fall back to the proven instant port so the escape still happens (never a regress
                // to "stuck forever").
                me->SetHomebindInMemory(
                    WorldLocation(m_hearthSavedHomeMap, m_hearthSavedHomeX, m_hearthSavedHomeY, m_hearthSavedHomeZ, 0.0f),
                    m_hearthSavedArea);
                m_hearthActive = false;
                if ((int)me->GetMapId() == hmap)
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: fallback same map teleport");
                    TeleportToWalkable(hx, hy, hz, me->GetOrientation(), "port-home");
                    hx = me->GetPositionX(); hy = me->GetPositionY(); hz = me->GetPositionZ();
                }
                else
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-task: fallback cross map teleport");
                    m_pendingWalkableLanding = true;
                    m_pendingWalkableMap = (uint32)hmap;
                    m_pendingWalkableX = hx;
                    m_pendingWalkableY = hy;
                    m_pendingWalkableZ = hz;
                    me->TeleportTo((uint32)hmap, hx, hy, hz, me->GetOrientation());
                }
                m_spawnMapId = (uint32)hmap;
                m_spawnX = hx; m_spawnY = hy; m_spawnZ = hz; m_spawnO = me->GetOrientation();
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s PORT_HOME (instant fallback, hearth unavailable): (%.1f, %.1f, %.1f) map=%u -> (%.1f, %.1f, %.1f) map=%d",
                    me->GetName(), fx, fy, fz, fmap, hx, hy, hz, hmap);
            }
        }
        else
        {
            CB_HIT(me->GetGUIDLow(), "cpp-task: PORT_HOME refused, missing home or dead or in combat");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s PORT_HOME refused (home=%d map=%d alive=%d combat=%d)",
                me->GetName(), haveHome ? 1 : 0, hmap, me->IsAlive() ? 1 : 0, me->IsInCombat() ? 1 : 0);
        }
    }
    else
    {
        CB_HITN(me->GetGUIDLow(), "cpp-task: unknown task type", taskType);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: SET_TASK unknown task type '%s'", me->GetName(), taskType);
    }
}

// ============================================================
// BridgeHandleCombatDirective — the per-member combat stamp (group focus-fire seam)
//
// The god-bot coordinator (C#) stamps each grouped member a combat directive each tick.
// v1 carries ONE mode — assist — plus the anchor's low GUID:
//   {"type":"COMBAT_DIRECTIVE","payload":{"mode":"assist","anchor_guid":123}}
//   • mode=="assist" + anchor_guid>0 → focus-fire the anchor's live victim (resolved in
//     TeamPlay::ResolveCombatTarget; the anchor itself — anchor_guid==self — falls through
//     to normal selection, so the team assists IT).
//   • mode=="none" / absent / anchor_guid<=0 → clear (revert to solo selection).
//
// FORWARD-TOLERANT BY CONSTRUCTION: parsed with JsonExtract*, the same idiom as every other
// inbound command in this file. A later key (role / interrupt_guid / move_to_guid) from a
// newer C# brain is simply not looked up here — a new key, never a contract break.
//
// No ack — a fire-and-forget stamp (like SET_TASK). Liveness IS the re-stamp cadence
// (the coordinator re-stamps every brain tick, idempotent; ungroup/anchor-death → mode=none).
// ============================================================
void AiBotAI::BridgeHandleCombatDirective(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    char mode[16] = {0};
    JsonExtractString(payload, "mode", mode, sizeof(mode));

    int anchorGuid = 0;
    JsonExtractInt(payload, "anchor_guid", anchorGuid);

    if (strcmp(mode, "assist") == 0 && anchorGuid > 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-combatcfg: assist directive adopted, anchor", anchorGuid);
        m_combatDirective.mode          = COMBAT_MODE_ASSIST;
        m_combatDirective.anchorGuidLow = (uint32)anchorGuid;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TEAMPLAY] %s: COMBAT_DIRECTIVE mode=assist anchor=%u%s",
            me->GetName(), m_combatDirective.anchorGuidLow,
            ((uint32)anchorGuid == me->GetGUIDLow()) ? " (self — I am the anchor)" : "");
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combatcfg: combat directive cleared");
        m_combatDirective.Clear();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TEAMPLAY] %s: COMBAT_DIRECTIVE cleared (mode='%s')",
            me->GetName(), mode[0] ? mode : "none");
    }
}

void AiBotAI::BridgeHandleTakeFlight(const char* json)
{
    int sourceNode = 0, destNode = 0;
 
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
 
    JsonExtractInt(payload, "sourceNode", sourceNode);
    JsonExtractInt(payload, "destNode", destNode);
 
    if (sourceNode <= 0 || destNode <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-flight: missing source or dest node, FLIGHT_FAILED");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT missing sourceNode or destNode", me->GetName());
        BridgeSendEvent("FLIGHT_FAILED", "missing sourceNode or destNode");
        return;
    }
 
    if (me->IsDead())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-flight: rejected, bot is dead, FLIGHT_FAILED");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT rejected — bot is dead", me->GetName());
        BridgeSendEvent("FLIGHT_FAILED", "bot is dead");
        return;
    }
 
    if (me->IsInCombat())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-flight: rejected, in combat, FLIGHT_FAILED");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT rejected — bot is in combat", me->GetName());
        BridgeSendEvent("FLIGHT_FAILED", "bot is in combat");
        return;
    }
 
    // Validate source node exists
    TaxiNodesEntry const* srcNode = sObjectMgr.GetTaxiNodeEntry((uint32)sourceNode);
    if (!srcNode)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-flight: invalid source node, FLIGHT_FAILED", sourceNode);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT invalid sourceNode %d", me->GetName(), sourceNode);
        BridgeSendEvent("FLIGHT_FAILED", "invalid sourceNode");
        return;
    }
 
    // Validate destination node exists
    TaxiNodesEntry const* dstNode = sObjectMgr.GetTaxiNodeEntry((uint32)destNode);
    if (!dstNode)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-flight: invalid dest node, FLIGHT_FAILED", destNode);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT invalid destNode %d", me->GetName(), destNode);
        BridgeSendEvent("FLIGHT_FAILED", "invalid destNode");
        return;
    }
 
    // Validate a path exists between source and dest
    uint32 pathId = 0, pathCost = 0;
    sObjectMgr.GetTaxiPath((uint32)sourceNode, (uint32)destNode, pathId, pathCost);
    if (!pathId)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-flight: no taxi path between nodes, FLIGHT_FAILED", destNode);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT no path from %d to %d", me->GetName(), sourceNode, destNode);
        BridgeSendEvent("FLIGHT_FAILED", "no path between nodes");
        return;
    }
 
    // Ensure bot knows both taxi nodes (unlock them)
    me->GetTaxi().SetTaximaskNode((uint32)sourceNode);
    me->GetTaxi().SetTaximaskNode((uint32)destNode);
 
    // Check if bot can afford the flight
    if (me->GetMoney() < pathCost)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-flight: not enough money, FLIGHT_FAILED", pathCost);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT not enough money (have %u, need %u copper)",
            me->GetName(), me->GetMoney(), pathCost);
 
        char failJson[256];
        snprintf(failJson, sizeof(failJson),
            "{\"type\":\"EVENT\",\"cbt\":" UI64FMTD ",\"payload\":{"
            "\"guid\":%u,\"event\":\"FLIGHT_FAILED\","
            "\"reason\":\"not_enough_money\","
            "\"have\":%u,\"need\":%u,\"cost\":%u}}",
            m_bridgeDispatchCbt, me->GetGUIDLow(), me->GetMoney(), pathCost, pathCost);
        BridgeSend(failJson);
        return;
    }
 
    // Must be on same map as source node
    if (srcNode->map_id != me->GetMapId())
    {
        CB_HITV(me->GetGUIDLow(), "cpp-flight: source node on different map, FLIGHT_FAILED", srcNode->map_id);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT source node %d is on map %u, bot is on map %u",
            me->GetName(), sourceNode, srcNode->map_id, me->GetMapId());
        BridgeSendEvent("FLIGHT_FAILED", "source node on different map");
        return;
    }
 
    // Stop any current movement/combat
    m_suppressTaskDestInform = true;
    CancelPendingNpcInteraction("take_flight");
    StopMoving();
    ClearStoredPath();
    if (me->IsMounted())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-flight: dismounting before taxi");
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    }
 
 
    // Set task state so UpdateAI doesn't interfere during flight
    m_currentTask.Clear();
    m_currentTask.type = TASK_TAXI;
    m_currentTask.commandCbt = m_bridgeDispatchCbt;
    m_currentTask.taxiSourceNode = (uint32)sourceNode;
    m_currentTask.taxiDestNode = (uint32)destNode;
 
    // Build the node vector and activate the flight path
    // nocheck = true skips the "do you know this node" validation
    std::vector<uint32> nodes;
    nodes.push_back((uint32)sourceNode);
    nodes.push_back((uint32)destNode);
 
    bool success = me->ActivateTaxiPathTo(nodes, nullptr, 0, true);
 
    if (success)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-flight: taxi path activated, FLIGHT_STARTED", destNode);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT activated path %u → %u (cost %u copper)",
            me->GetName(), sourceNode, destNode, pathCost);
        BridgeSendEvent("FLIGHT_STARTED", "");
    }
    else
    {
        CB_HIT(me->GetGUIDLow(), "cpp-flight: ActivateTaxiPathTo failed, FLIGHT_FAILED");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT ActivateTaxiPathTo failed (%d → %d)",
            me->GetName(), sourceNode, destNode);
        m_currentTask.Clear();
        BridgeSendEvent("FLIGHT_FAILED", "ActivateTaxiPathTo returned false");
    }
}

// ============================================================
// BridgeHandleSellItems — v4 (2026-07-28: consumable policy — food dumps, weak pots sell)
//
// CHANGE from v3:
//   The v2/v3 "keep up to MAX_KEEP_PER_CONSUMABLE(40) of each item-id" rule kept every
//   distinct food/drink/potion stack, so a bag with 30 different half-stacks freed nothing.
//   Replaced with a role split:
//     - FOOD & DRINK (subclass FOOD): the bot has unlimited food (autonomous DrinkAndEat
//       conjures/uses its own), so looted food is never needed → SELL ALL, any level.
//     - POTIONS / elixirs / scrolls / bandages: not consumed by the bot YET, but keep the
//       level-appropriate ones staged for when potion-use lands. Sell only ranks the bot has
//       outgrown — RequiredLevel more than AIBOT_CONSUMABLE_STALE_LEVELS below current level.
//
// CHANGES from v2/v3 (retained): gear sold on UPGRADE status not rarity (v3); non-upgrade bags
// vendorable; quest/consumable protections; "nothing_to_sell" flag in SELL_ACK.
//
// REQUIRES: AIBOT_SELL_KEEP_UPGRADE_LEVELS 5, AIBOT_CONSUMABLE_STALE_LEVELS 12  in AiBotAIMain.h
// ============================================================

void AiBotAI::BridgeHandleSellItems(const char* json)
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
    {
        CB_HIT(me ? me->GetGUIDLow() : 0u, "cpp-vendor: sell ignored, dead or not in world");
        return;
    }

    // [SUI] Never autosell a REAL account's character. AiBotAI is only ever
    // attached to socket-less bot sessions, and possessed bots reject bridge
    // commands upstream — but this is the requirement's hard wall: any unit
    // whose session has a live client keeps its inventory untouchable.
    if (me->GetSession() && !me->GetSession()->GetBot())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-vendor: refused, real account character, SELL_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s: refused — real account character", me->GetName());
        BridgeSendEvent("SELL_FAIL", "reason=real_account_protected");
        return;
    }

    int npcEntry = 0, keepQuality = 0;
    JsonExtractInt(json, "npc_entry", npcEntry);
    JsonExtractInt(json, "keep_quality", keepQuality);
    if (npcEntry <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-vendor: SELL_ITEMS missing npc_entry, SELL_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s: SELL_ITEMS missing npc_entry", me->GetName());
        BridgeSendEvent("SELL_FAIL", "reason=missing_npc_entry");
        return;
    }
    if (keepQuality <= 0) keepQuality = 2; // cb:fold default quality clamp

    // --- Find vendor NPC ---
    std::list<Creature*> creatureList;
    me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);

    Creature* pVendor = nullptr;
    float bestDist = 999.0f;
    for (Creature* c : creatureList)
    {
        if (c && c->IsAlive() && (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_VENDOR))
        {   // cb:fold nearest candidate scan
            float d = me->GetDistance(c);
            if (d < bestDist)
            {   // cb:fold nearest candidate scan
                bestDist = d;
                pVendor = c;
            }
        }
    }
    if (!pVendor)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-vendor: vendor not found within 15yd, SELL_FAIL", npcEntry);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s: no vendor with entry %d within 15yd", me->GetName(), npcEntry);
        BridgeSendEvent("SELL_FAIL", "reason=vendor_not_found");
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-SELL] %s: === BEGIN selling at %s (entry=%u) keepQuality=%d ===",
        me->GetName(), pVendor->GetName(), pVendor->GetEntry(), keepQuality);

    // --- Build set of quest-required item IDs ---
    std::set<uint32> questItemIds;
    const auto& questMap = me->GetQuestStatusMap();
    for (const auto& pair : questMap)
    {
        if (pair.second.m_status != QUEST_STATUS_INCOMPLETE && pair.second.m_status != QUEST_STATUS_COMPLETE)
            continue; // cb:fold quest item set build detail
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(pair.first);
        if (!pQuest) continue; // cb:fold quest item set build detail
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
        {
            if (pQuest->ReqItemId[j] > 0)
                questItemIds.insert(pQuest->ReqItemId[j]); // cb:fold quest item set build detail
        }
        if (pQuest->GetSrcItemId() > 0)
            questItemIds.insert(pQuest->GetSrcItemId()); // cb:fold quest item set build detail
    }

    // Find the largest equipped bag size (for bag-selling: only sell bags
    // that are NOT bigger than any equipped bag, i.e. not an upgrade)
    uint32 largestEquippedBagSize = 0;
    for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        Item* pBagItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
        if (!pBagItem) continue; // cb:fold bag size scan detail
        ItemPrototype const* bp = pBagItem->GetProto();
        if (bp && bp->Class == ITEM_CLASS_CONTAINER && bp->SubClass == ITEM_SUBCLASS_CONTAINER)
        {   // cb:fold bag size scan detail
            if (bp->ContainerSlots > largestEquippedBagSize)
                largestEquippedBagSize = bp->ContainerSlots; // cb:fold bag size scan detail
        }
    }
    // Also check if there are any empty bag equip slots (if so, ANY bag is an upgrade)
    bool hasEmptyBagSlot = false;
    for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b))
        {   // cb:fold bag slot scan detail
            hasEmptyBagSlot = true;
            break;
        }
    }

    uint32 totalCopper = 0;
    uint32 soldCount = 0;

    // --- Vendor surplus quest-gather items (over-loot cleanup) ---
    for (uint32 qItemId : questItemIds)
    {
        uint32 need = QuestRequiredCountFor(qItemId);
        if (need == 0)
            continue; // cb:fold surplus scan detail
        uint32 have = me->GetItemCount(qItemId, false);
        if (have <= need)
            continue; // cb:fold surplus scan detail
        uint32 surplus = have - need;
        ItemPrototype const* sp = sObjectMgr.GetItemPrototype(qItemId);
        uint32 money = (sp ? sp->SellPrice : 0) * surplus;
        if (money)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: quest surplus vendored", qItemId);
            me->ModifyMoney((int32)money);
            totalCopper += money;
        }
        me->DestroyItemCount(qItemId, surplus, true);
        soldCount += surplus;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s:   quest-surplus [%s] (id=%u) kept %u, sold %u for %uc",
            me->GetName(), sp && sp->Name1 ? sp->Name1 : "?", qItemId, need, surplus, money);
    }

    // --- Sell helper: checks one item, sells if appropriate ---
    auto trySellItem = [&](uint8 bag, uint8 slot)
    {
        Item* pItem = me->GetItemByPos(bag, slot);
        if (!pItem) return; // cb:fold empty slot scan

        ItemPrototype const* proto = pItem->GetProto();
        if (!proto) return; // cb:fold missing proto guard

        // Keep: no sell price (hearthstone, etc.)
        if (proto->SellPrice == 0)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, no sell price", proto->ItemId);
            return;
        }
        // Keep: quest-class items
        if (proto->Class == ITEM_CLASS_QUEST)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, quest class item", proto->ItemId);
            return;
        }
        if (proto->StartQuest > 0)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, item starts a quest", proto->ItemId);
            return;
        }
        if (proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, quest bound item", proto->ItemId);
            return;
        }
        if (questItemIds.count(proto->ItemId) > 0)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, active quest objective item", proto->ItemId);
            return;
        }

        // --- BAGS: sell unequipped bags that aren't upgrades ---
        if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: loose bag policy check", proto->ItemId);
            if (hasEmptyBagSlot)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep bag, empty equip slot exists", proto->ItemId);
                return;
            }
            if (proto->ContainerSlots > largestEquippedBagSize)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep bag, bigger than any equipped", proto->ItemId);
                return;
            }
            // else: duplicate/downgrade bag → sell
        }
        // --- CONSUMABLES: food dumps, weak potions sell, current-rank potions kept (v4) ---
        else if (proto->Class == ITEM_CLASS_CONSUMABLE)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: consumable policy check", proto->ItemId);
            // Food & drink (subclass FOOD): the bot has unlimited food (autonomous DrinkAndEat),
            // so looted food is never needed → sell all, any level. (§verify subclass FOOD == 5.)
            if (proto->SubClass == 5)   // 5 = Food & Drink (1.12 consumable subclass DBC id; this fork defines no ITEM_SUBCLASS_FOOD name)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: food or drink, selling all", proto->ItemId);
                // fall through to sell
            }
            else
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: potion staleness check", proto->ItemId);
                // Potions / elixirs / scrolls / bandages — not consumed by the bot YET. Keep the
                // level-appropriate ones staged for when potion-use lands; sell only ranks the bot
                // has outgrown. This is the "don't sell EVERYTHING" guard — the current tier stays.
                if (me->GetLevel() <= proto->RequiredLevel + AIBOT_CONSUMABLE_STALE_LEVELS)
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep potion, level appropriate", proto->ItemId);
                    return;   // level-appropriate → keep
                }
                // else: superseded rank → sell (fall through)
            }
        }
        // --- GEAR (weapons/armor): UPGRADE-AWARE, quality-blind (v3) ---
        else if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: gear policy check", proto->ItemId);
            // Keep 1: epics+ — rare, high value, never vendor-trash a purple.
            if (proto->Quality >= (uint32)ITEM_QUALITY_EPIC)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, epic or better", proto->ItemId);
                return;
            }

            uint16 dest = 0;
            InventoryResult can = me->CanEquipItem(NULL_SLOT, dest, pItem, true);
            if (can == EQUIP_ERR_OK)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: usable now, upgrade score check", proto->ItemId);
                // Usable now — sell UNLESS it out-scores what's worn (safety net for a real upgrade
                // that slipped past TryAutoEquip when bags were full).
                uint8 tgt = (uint8)(dest & 0xFF);
                Item* worn = me->GetItemByPos(INVENTORY_SLOT_BAG_0, tgt);
                float newS = ScoreItem(proto, tgt);
                float oldS = worn ? ScoreItem(worn->GetProto(), tgt) : 0.0f;
                if (newS > oldS)
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, genuine upgrade over worn", proto->ItemId);
                    return;   // Keep 2: genuine upgrade
                }
                // else: usable non-upgrade → sell
            }
            else
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: not equippable, grow-into check", proto->ItemId);
                // Keep 3: grow-into upgrade — right class/proficiency, under-level, usable within
                // AIBOT_SELL_KEEP_UPGRADE_LEVELS. (RequiredLevel — verify the field name on this core.)
                if (proto->RequiredLevel > me->GetLevel() &&
                    proto->RequiredLevel <= me->GetLevel() + AIBOT_SELL_KEEP_UPGRADE_LEVELS)
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, grow-into upgrade", proto->ItemId);
                    return;
                }
                // else: never-usable (wrong class/prof/race) or too far off → sell
            }
        }
        // --- RECIPES: temporary sell-by-default policy until bots participate in the economy ---
        else if (proto->Class == ITEM_CLASS_RECIPE)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: recipe policy, selling until economy enabled", proto->ItemId);
            // Autonomous bots do not currently learn professions, craft, trade, mail, or list items
            // on the auction house. Retaining recipes therefore has no simulated utility and has
            // filled bags fleet-wide. Sell every sellable recipe for now, regardless of rarity.
            //
            // TODO(economy): reintroduce recipe retention when bots participate in the full simulated
            // economy. Consider known/learnable recipes, profession plans, trading, and auction-house
            // value instead of restoring the generic rarity threshold.
            // fall through to sell
        }
        // --- OTHER MISC / trade goods: keep the quality threshold (conservative) ---
        else
        {
            CB_HITV(me->GetGUIDLow(), "cpp-vendor: misc quality threshold check", proto->ItemId);
            if (proto->Quality >= (uint32)keepQuality)
            {
                CB_HITV(me->GetGUIDLow(), "cpp-vendor: keep, misc above quality threshold", proto->ItemId);
                return;
            }
        }

        uint32 money = proto->SellPrice * pItem->GetCount();

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s:   selling [%s] (id=%u q=%u x%u) for %uc",
            me->GetName(), proto->Name1 ? proto->Name1 : "?",
            proto->ItemId, proto->Quality, pItem->GetCount(), money);

        // DestroyItem handles RemoveItem + RemoveFromUpdateQueueOf + SetState
        // in the correct order. Must grab money BEFORE destroy invalidates pItem.
        me->ModifyMoney((int32)money);
        me->DestroyItem(bag, slot, true);

        totalCopper += money;
        soldCount++;
    };

    // --- Iterate backpack ---
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        trySellItem(INVENTORY_SLOT_BAG_0, (uint8)i);

    // --- Iterate extra bags ---
    for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
        if (!pBag || pBag->GetProto()->Class != ITEM_CLASS_CONTAINER)
            continue; // cb:fold bag iterate detail
        for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            trySellItem((uint8)b, (uint8)j);
    }

    uint32 freeSlots = 0;
    for (int fi = INVENTORY_SLOT_ITEM_START; fi < INVENTORY_SLOT_ITEM_END; ++fi)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
            ++freeSlots; // cb:fold slot tally detail
    for (int fi = INVENTORY_SLOT_BAG_START; fi < INVENTORY_SLOT_BAG_END; ++fi)
        if (Bag* pFBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi)) // cb:fold decl-if condition artifact
            if (pFBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pFBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER) // cb:fold slot tally detail
                for (uint32 fj = 0; fj < pFBag->GetBagSize(); ++fj) // cb:fold slot tally detail
                    if (!me->GetItemByPos(fi, fj))
                        ++freeSlots; // cb:fold slot tally detail

    // Build SELL_ACK — include "nothing_to_sell" flag when bags are full but
    // nothing was vendorable, so C# can set a cooldown and break the loop.
    char eventData[196];
    if (soldCount == 0 && freeSlots == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-vendor: nothing to sell and bags full, SELL_ACK flagged");
        snprintf(eventData, sizeof(eventData),
            "sold=0|copper_earned=0|free_slots=0|copper_total=%u|nothing_to_sell=1",
            me->GetMoney());
    }
    else
    {
        CB_HITV(me->GetGUIDLow(), "cpp-vendor: sell complete, SELL_ACK", soldCount);
        snprintf(eventData, sizeof(eventData),
            "sold=%u|copper_earned=%u|free_slots=%u|copper_total=%u",
            soldCount, totalCopper, freeSlots, me->GetMoney());
    }

    BridgeSendEvent("SELL_ACK", eventData);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-SELL] %s: === DONE === sold %u items for %uc, %u free slots",
        me->GetName(), soldCount, totalCopper, freeSlots);
}

// ============================================================
// BridgeHandleRepairItems — Session 32
//
// C# sends REPAIR_AT_NPC after SELL_ACK when the vendor has
// UNIT_NPC_FLAG_REPAIR. Repairs all equipped gear + bags.
//
// Pattern mirrors BridgeHandleSellItems: find NPC by entry
// within 15yd, validate flag, do work, emit ACK/FAIL.
//
// DISPATCH:  Add to BridgeProcessLine:
//              else if (strcmp(msgType, "REPAIR_AT_NPC") == 0)
//                  BridgeHandleRepairItems(line);
// HEADER:    Add to AiBotAI.h:
//              void BridgeHandleRepairItems(const char* json);
// ============================================================
 
void AiBotAI::BridgeHandleRepairItems(const char* json)
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
    {
        CB_HIT(me ? me->GetGUIDLow() : 0u, "cpp-vendor: repair ignored, dead or not in world");
        return;
    }

    int npcEntry = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    JsonExtractInt(payload, "npc_entry", npcEntry);
 
    if (npcEntry <= 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-vendor: REPAIR_AT_NPC missing npc_entry, REPAIR_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: REPAIR_AT_NPC missing npc_entry", me->GetName());
        BridgeSendEvent("REPAIR_FAIL", "reason=missing_npc_entry");
        return;
    }
 
    // --- Find repair NPC within 15yd ---
    std::list<Creature*> creatureList;
    me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);
 
    Creature* pRepairNpc = nullptr;
    float bestDist = 999.0f;
    for (Creature* c : creatureList)
    {
        if (c && c->IsAlive() &&
            (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_REPAIR))
        {   // cb:fold nearest candidate scan
            float d = me->GetDistance(c);
            if (d < bestDist)
            {   // cb:fold nearest candidate scan
                bestDist = d;
                pRepairNpc = c;
            }
        }
    }
 
    if (!pRepairNpc)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-vendor: repair npc not found within 15yd, REPAIR_FAIL", npcEntry);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: no repair NPC with entry %d within 15yd",
            me->GetName(), npcEntry);
        BridgeSendEvent("REPAIR_FAIL", "reason=npc_not_found");
        return;
    }
 
    // --- Check if bot has any damaged gear before attempting repair ---
    bool hasDamage = false;
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
        if (item && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0 &&
            item->GetUInt32Value(ITEM_FIELD_DURABILITY) <
            item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
        {   // cb:fold durability scan detail
            hasDamage = true;
            break;
        }
    }
 
    if (!hasDamage)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-vendor: no damaged gear, REPAIR_ACK zero cost");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: no damaged gear — nothing to repair", me->GetName());
        char eventData[128];
        snprintf(eventData, sizeof(eventData), "cost=0|copper_total=%u", me->GetMoney());
        BridgeSendEvent("REPAIR_ACK", eventData);
        return;
    }
 
    // --- Repair all gear ---
    // DurabilityRepairAll(bool cost, float discountMod) → returns total copper spent
    //   cost=true: deduct gold from player
    //   discountMod=1.0: no faction reputation discount
    //   Returns 0 if can't afford any repairs
    // [FREE-REPAIR 2026-08-21, user directive] Bots earn ~no gold and were stranding at 0%
    // durability with 1c in the bank (REPAIR_FAIL not_enough_gold, seen live on Orincat). They
    // still walk the whole repair MOTION (route to the NPC, interact) — but the repair is FREE:
    // cost=false repairs every item WITHOUT deducting money, so a damaged bot that reaches a
    // repair NPC always leaves fully repaired. (The not_enough_gold branch below is now dead.)
    uint32 totalCost = me->DurabilityRepairAll(false, 1.0f);
 
    if (false && totalCost == 0 && hasDamage)   // [FREE-REPAIR] dead branch: repair never charges now
    {   // cb:fold dead branch, free repair never charges
        // Gear is damaged but nothing was repaired — can't afford
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: gear is damaged but can't afford repair (gold=%u)",
            me->GetName(), me->GetMoney());
        BridgeSendEvent("REPAIR_FAIL", "reason=not_enough_gold");
        return;
    }
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-REPAIR] %s: repaired all gear at %s (entry=%u) — cost %uc, %uc remaining",
        me->GetName(), pRepairNpc->GetName(), pRepairNpc->GetEntry(),
        totalCost, me->GetMoney());
 
    char eventData[128];
    snprintf(eventData, sizeof(eventData), "cost=%u|copper_total=%u", totalCost, me->GetMoney());
    BridgeSendEvent("REPAIR_ACK", eventData);
}
 

void AiBotAI::BridgeHandleUseGameObject(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    int goEntryInt = 0;
    JsonExtractInt(payload, "go_entry", goEntryInt);
    uint32 goEntry = (uint32)goEntryInt;

    if (!goEntry)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-go: bad payload, missing go_entry, USE_GO_FAIL");
        BridgeSendEvent("USE_GO_FAIL", "reason=bad_payload");
        return;
    }

    // Find nearest spawned GO of this entry within 15yd
    std::list<GameObject*> goList;
    me->GetGameObjectListWithEntryInGrid(goList, goEntry, 15.0f);

    GameObject* obj = nullptr;
    float closestDist = 999.0f;
    for (auto* go : goList)
    {
        if (!go->isSpawned()) continue; // cb:fold nearest candidate scan
        if (go->getLootState() != GO_READY) continue; // cb:fold nearest candidate scan
        float dist = me->GetDistance(go);
        if (dist < closestDist)
        {   // cb:fold nearest candidate scan
            closestDist = dist;
            obj = go;
        }
    }

    if (!obj)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-go: gameobject not found ready, USE_GO_FAIL", goEntry);
        char buf[128];
        snprintf(buf, sizeof(buf), "reason=not_found|go_entry=%u", goEntry);
        BridgeSendEvent("USE_GO_FAIL", buf);
        return;
    }

    if (closestDist > 10.0f)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-go: gameobject too far, USE_GO_FAIL", closestDist);
        char buf[128];
        snprintf(buf, sizeof(buf), "reason=too_far|go_entry=%u|dist=%d", goEntry, (int)closestDist);
        BridgeSendEvent("USE_GO_FAIL", buf);
        return;
    }

    // [GO-INTERACT CREDIT] Credit any GO-interact quest objective for this GO (a negative
    // ReqCreatureOrGO). Looting alone NEVER fires this — a pure-interact GO (lever, brazier,
    // fire) advances only via CastedCreatureOrGO. Harmless for non-interact GOs: if no quest
    // in the log requires this GO entry, it's a no-op.
    // NOTE: verify the CastedCreatureOrGO signature against your Player.h (this core:
    //   void Player::CastedCreatureOrGO(uint32 entry, ObjectGuid guid, uint32 spell_id)).
    me->CastedCreatureOrGO(obj->GetEntry(), obj->GetObjectGuid(), 0);

    // Get loot template ID from GO info
    uint32 lootId = obj->GetGOInfo()->GetLootId();
    if (!lootId)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-go: no loot table, interact only, USE_GO_ACK", goEntry);
        // No loot — still a valid use (some GOs give quest credit on interact)
        obj->SetLootState(GO_JUST_DEACTIVATED);
        char buf[128];
        snprintf(buf, sizeof(buf), "go_entry=%u|items=", goEntry);
        BridgeSendEvent("USE_GO_ACK", buf);
        return;
    }

    // Generate loot from gameobject_loot_template
    Loot& loot = obj->loot;
    loot.clear();
    loot.FillLoot(lootId, LootTemplates_Gameobject, me, false);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GO] %s: USE_GAMEOBJECT entry=%u lootId=%u → %zu items, %u copper",
        me->GetName(), goEntry, lootId, loot.items.size(), loot.gold);

    // Take gold
    uint32 gold = loot.gold;
    if (gold > 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-go: loot gold taken", gold);
        me->ModifyMoney((int32)gold);
        me->LootMoney((int32)gold, &loot);
        loot.gold = 0;
    }

    // Take items — same pattern as DoAutoLoot
    me->AutoStoreLoot(loot);

    // Build loot summary for bridge events
    std::string itemStr;
    uint32 itemsLooted = 0;
    for (size_t i = 0; i < loot.items.size(); ++i)
    {
        LootItem& item = loot.items[i];
        if (item.is_looted)
        {   // cb:fold loot summary format detail, carried by LOOT event
            itemsLooted++;
            if (!itemStr.empty()) itemStr += ","; // cb:fold loot summary format detail
            itemStr += std::to_string(item.itemid) + ":" + std::to_string(item.count);
        }
    }

    // Despawn — GO will respawn on its timer
    loot.clear();
    obj->SetLootState(GO_JUST_DEACTIVATED);

    // Auto-equip bags first, then gear
    TryAutoEquipBags();
    TryAutoEquip();

    // Emit LOOT event — same format as DoAutoLoot so C# quest item tracking works unchanged
    std::string lootData = "gold=" + std::to_string(gold);
    if (!itemStr.empty())
        lootData += "|items=" + itemStr; // cb:fold loot summary format detail
    BridgeSendEvent("LOOT", lootData.c_str());

    // Emit USE_GO_ACK so C# QuestingDomain knows the interaction succeeded
    std::string ackData = "go_entry=" + std::to_string(goEntry) + "|items=" + itemStr;
    BridgeSendEvent("USE_GO_ACK", ackData.c_str());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GO] %s: === DONE === %u items stored, %u copper, GO despawned",
        me->GetName(), itemsLooted, gold);
}

// ============================================================
// BridgeHandleQuestCast — [CLASS-QUEST] cast a quest spell on a target creature.
//
// Drives "cast spell S on creature C" objectives (quest_template ReqSpellCast) — the
// priest heal-an-NPC, and the same shape for other class-quest "do X to this NPC" steps.
//   payload: { spell_id, entry (creature_template to find nearby) | guid, count?, radius? }
//
// The bot casts through the REAL spell path (me->CastSpell), so — because the bot is a
// real Player — the core's quest hook should credit the objective exactly as it does for a
// human. This handler only STARTS the cast + ACKs; the C# planner confirms completion by
// re-syncing the quest log on the next STATE (same as a grind leg).
//
// If a live test shows the cast alone does NOT credit (the same surprise USE_GAMEOBJECT had
// with GO-interact), uncomment the explicit CastedCreatureOrGO line below.
//
// DISPATCH: BridgeProcessLine -> else if (strcmp(msgType,"QUEST_CAST")==0) BridgeHandleQuestCast(line);
// HEADER:   void BridgeHandleQuestCast(const char* json);
// ============================================================
void AiBotAI::BridgeHandleQuestCast(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    int spellIdInt = 0, entryInt = 0, guidInt = 0, countInt = 1;
    float radius = 15.0f;
    JsonExtractInt(payload, "spell_id", spellIdInt);
    JsonExtractInt(payload, "entry", entryInt);
    JsonExtractInt(payload, "guid", guidInt);
    JsonExtractInt(payload, "count", countInt);
    JsonExtractFloat(payload, "radius", radius);

    uint32 spellId = (uint32)spellIdInt;
    if (!spellId)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-quest: QUEST_CAST missing spell_id, QUEST_CAST_FAIL");
        BridgeSendEvent("QUEST_CAST_FAIL", "reason=bad_payload");
        return;
    }
    if (radius <= 0.0f || radius > 60.0f)
        radius = 15.0f; // cb:fold radius clamp detail

    // --- Resolve the target: by guid if the planner gave one, else nearest ALIVE of `entry`. ---
    Creature* pTarget = nullptr;
    if (guidInt > 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: cast target resolved by guid", guidInt);
        pTarget = me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, uint32(guidInt)));
    }
    else if (entryInt > 0)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: cast target by nearest entry", entryInt);
        std::list<Creature*> creatureList;
        me->GetCreatureListWithEntryInGrid(creatureList, (uint32)entryInt, radius);
        float closest = 999.0f;
        for (auto* c : creatureList)
        {
            if (!c || !c->IsAlive())
                continue; // cb:fold nearest candidate scan
            float d = me->GetDistance(c);
            if (d < closest)
            {   // cb:fold nearest candidate scan
                closest = d;
                pTarget = c;
            }
        }
    }

    if (!pTarget)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: cast target not found, QUEST_CAST_FAIL", entryInt);
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=target_not_found|entry=%d|spell=%u", entryInt, spellId);
        BridgeSendEvent("QUEST_CAST_FAIL", buf);
        return;
    }

    // --- Range / LOS guard: the planner should have walked us in first; don't cast blind. ---
    float dist = me->GetDistance(pTarget);
    if (dist > 30.0f || !me->IsWithinLOSInMap(pTarget))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: target too far or no LOS, QUEST_CAST_FAIL", dist);
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=too_far|entry=%d|spell=%u|dist=%d", entryInt, spellId, (int)dist);
        BridgeSendEvent("QUEST_CAST_FAIL", buf);
        return;
    }

    // --- Cast. Trigger the cast when the bot doesn't "know" the spell (item-granted quest
    //     spells, e.g. a provided rod); otherwise cast it for real so cast time / cost apply.
    //     Called as a plain statement so it compiles whether CastSpell returns void or a result. ---
    // A planner/slate bug can hand spell=0 or an id the DBCs do not know;
    // casting it only spams "unknown spell id 0" in the core log. Refuse
    // honestly through the same failure event instead.
    if (!spellId || !sSpellMgr.GetSpellEntry(spellId))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-quest: unknown spell id, QUEST_CAST_FAIL", spellId);
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=bad_spell|entry=%d|spell=%u", entryInt, spellId);
        BridgeSendEvent("QUEST_CAST_FAIL", buf);
        return;
    }

    CancelPendingNpcInteraction("quest_cast");
    StopMoving();
    me->SetFacingToObject(pTarget);

    bool triggered = !me->HasSpell(spellId);
    me->CastSpell(pTarget, spellId, triggered);

    // If the live test shows the cast alone doesn't credit the objective, uncomment:
    // me->CastedCreatureOrGO(pTarget->GetEntry(), pTarget->GetObjectGuid(), spellId);

    char ack[160];
    snprintf(ack, sizeof(ack), "entry=%u|spell=%u|guid=%u",
        pTarget->GetEntry(), spellId, pTarget->GetGUIDLow());
    BridgeSendEvent("QUEST_CAST_ACK", ack);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-QCAST] %s: cast spell %u on %s (entry=%u, dist=%.1f) — objective should credit on cast complete",
        me->GetName(), spellId, pTarget->GetName(), pTarget->GetEntry(), dist);
}

// ============================================================
// METHOD 2: BridgeHandleFormGroup — NEW METHOD
//
// Add after BridgeHandleUseGameObject in AiBotAI.cpp.
// Creates a WoW Group with this bot as leader, adds followers.
// Uses NEED_BEFORE_GREED so only eligible players see roll windows.
// ============================================================
 
void AiBotAI::BridgeHandleFormGroup(const char* json)
{
    if (m_bridgeDispatchCbt == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: FORM_GROUP missing command cbt");
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=missing_cbt");
        return;
    }

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    int leaderGuid = 0;
    if (!JsonExtractInt(payload, "leader_guid", leaderGuid)
        || leaderGuid <= 0
        || uint32(leaderGuid) != me->GetGUIDLow())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: FORM_GROUP leader identity mismatch");
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=leader_mismatch");
        return;
    }

    // Parse member_guids array: "member_guids":[5,8,12]
    const char* arrStart = strstr(payload, "\"member_guids\"");
    if (!arrStart)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: missing member_guids, FORM_GROUP_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: FORM_GROUP missing member_guids", me->GetName());
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=missing_member_guids");
        return;
    }

    arrStart = strchr(arrStart, '[');
    if (!arrStart)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: malformed member_guids open, FORM_GROUP_FAIL");
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=missing_array_open");
        return;
    }
    arrStart++; // skip '['

    const char* arrEnd = strchr(arrStart, ']');
    if (!arrEnd)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: malformed member_guids close, FORM_GROUP_FAIL");
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=missing_array_close");
        return;
    }

    // Extract GUIDs from comma-separated list
    std::vector<uint32> memberGuids;
    const char* p = arrStart;
    while (p < arrEnd)
    {
        while (p < arrEnd && (*p == ' ' || *p == ',')) p++;
        if (p >= arrEnd) break; // cb:fold guid list parse detail
        uint32 guid = (uint32)atoi(p);
        if (guid == 0)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-group: malformed member guid, FORM_GROUP_FAIL");
            BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=invalid_member_guid");
            return;
        }
        memberGuids.push_back(guid);
        while (p < arrEnd && *p != ',') p++;
    }

    if (memberGuids.empty() || memberGuids.size() > 4)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-group: invalid requested group size, FORM_GROUP_FAIL", memberGuids.size() + 1);
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: FORM_GROUP invalid size %u",
            me->GetName(), uint32(memberGuids.size() + 1));
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=invalid_group_size");
        return;
    }

    for (size_t i = 0; i < memberGuids.size(); ++i)
    {
        if (memberGuids[i] == me->GetGUIDLow())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-group: leader repeated as follower, FORM_GROUP_FAIL");
            BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=leader_in_members");
            return;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (memberGuids[j] == memberGuids[i])
            {
                CB_HITV(me->GetGUIDLow(), "cpp-group: duplicate requested member, FORM_GROUP_FAIL", memberGuids[i]);
                BridgeSendEvent("FORM_GROUP_FAIL", "reason=bad_payload|detail=duplicate_member");
                return;
            }
        }
    }

    std::vector<uint32> requestedGuids;
    requestedGuids.push_back(me->GetGUIDLow());
    requestedGuids.insert(requestedGuids.end(), memberGuids.begin(), memberGuids.end());
    std::string memberList;
    char guidText[16];
    for (size_t i = 0; i < requestedGuids.size(); ++i)
    {
        snprintf(guidText, sizeof(guidText), i == 0 ? "%u" : ",%u", requestedGuids[i]);
        memberList += guidText; // cb:fold exact ACK payload construction
    }
    char successData[192];
    snprintf(successData, sizeof(successData), "leader_guid=%u|member_guids=%s",
        me->GetGUIDLow(), memberList.c_str());

    // [PLAYERPARTY] Never let the god-bot yank this bot out of a REAL player's party
    // (2026-07-07). A stale C# coordinator decision must not disband a human's escort
    // mid-quest — the human outranks the coordinator, always. C# also stands down off the
    // pparty STATE echo, so this refusal is the belt to that suspender.
    if (FindPartyBoss())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: refused, in real player party, FORM_GROUP_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: FORM_GROUP refused — this bot is in a REAL player's party",
            me->GetName());
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=in_player_party");
        return;
    }

    // An exact replay is the reconciliation path for an ACK lost after the core committed.
    // It is idempotently successful; a different existing topology remains fail-closed.
    if (Group* existingGroup = me->GetGroup()) // cb:fold decl-if condition artifact, body probed
    {
        std::vector<uint32> liveGuids;
        bool liveMembersAreBots = true;
        for (const auto& memberSlot : existingGroup->GetMemberSlots())
        {
            liveGuids.push_back(memberSlot.guid.GetCounter()); // cb:fold persistent exact-set enumeration
            Player* liveMember = sObjectMgr.GetPlayer(memberSlot.guid);
            if (!liveMember || !liveMember->GetSession() || !liveMember->GetSession()->GetBot())
            {
                CB_HITV(me->GetGUIDLow(), "cpp-group: existing topology has offline/non-bot member, replay refused",
                    memberSlot.guid.GetCounter());
                liveMembersAreBots = false;
            }
        }

        bool topologyMatches = liveMembersAreBots
            && existingGroup->GetLeaderGuid() == me->GetObjectGuid()
            && liveGuids.size() == requestedGuids.size();
        for (uint32 requestedGuid : requestedGuids)
        {
            bool found = false;
            for (uint32 liveGuid : liveGuids)
                if (liveGuid == requestedGuid)
                { // cb:fold bounded set comparison
                    found = true;
                    break;
                }
            if (!found)
                topologyMatches = false; // cb:fold exact-set accumulation, final decision probed below
        }

        if (topologyMatches)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: exact FORM_GROUP replay acknowledged", requestedGuids.size());
            BridgeSendEvent("FORM_GROUP_ACK", successData);
            return;
        }

        CB_HIT(me->GetGUIDLow(), "cpp-group: different existing topology, FORM_GROUP_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: FORM_GROUP refused — leader already has a different group", me->GetName());
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=leader_already_grouped");
        return;
    }

    // Preflight every requested follower before creating or mutating a Group. That makes every
    // validation failure side-effect free and prevents the old partial-success ACK.
    std::vector<Player*> members;
    for (uint32 memberGuid : memberGuids)
    {
        Player* pMember = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, memberGuid));
        if (!pMember)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: requested member not online, FORM_GROUP_FAIL", memberGuid);
            char eventData[96];
            snprintf(eventData, sizeof(eventData),
                "reason=member_not_found|member_guid=%u", memberGuid);
            BridgeSendEvent("FORM_GROUP_FAIL", eventData);
            return;
        }
        if (!pMember->GetSession() || !pMember->GetSession()->GetBot())
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: requested member is not a bot, FORM_GROUP_FAIL", memberGuid);
            char eventData[96];
            snprintf(eventData, sizeof(eventData),
                "reason=member_not_bot|member_guid=%u", memberGuid);
            BridgeSendEvent("FORM_GROUP_FAIL", eventData);
            return;
        }
        if (pMember->GetGroup())
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: requested member already grouped, FORM_GROUP_FAIL", memberGuid);
            char eventData[96];
            snprintf(eventData, sizeof(eventData),
                "reason=member_already_grouped|member_guid=%u", memberGuid);
            BridgeSendEvent("FORM_GROUP_FAIL", eventData);
            return;
        }
        members.push_back(pMember);
    }

    // Create a new Group with this bot as leader
    Group* group = new Group;
    if (!group->Create(me->GetObjectGuid(), me->GetName()))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: Group Create failed, FORM_GROUP_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: Group::Create failed", me->GetName());
        delete group;
        BridgeSendEvent("FORM_GROUP_FAIL", "reason=create_failed");
        return;
    }

    // Group::Create initializes membership but does not publish the object to the core's
    // authoritative group registry. Mirror the canonical VMaNGOS lifecycle before adding
    // followers; every rollback below unregisters and deletes this same owned object.
    sObjectMgr.AddGroup(group);

    // NEED_BEFORE_GREED: only eligible players see the roll window.
    // StartLootRoll checks CanUseItem — priests won't roll on plate, etc.
    group->SetLootMethod(NEED_BEFORE_GREED);

    // All preflighted players were ungrouped. If the core nevertheless rejects one AddMember,
    // disband the newly-created group immediately; no pre-existing topology was destroyed.
    for (size_t i = 0; i < members.size(); ++i)
    {
        Player* pMember = members[i];
        uint32 const memberGuid = memberGuids[i];
        if (!group->AddMember(pMember->GetObjectGuid(), pMember->GetName()))
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: AddMember failed, rolling back group", memberGuid);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-GROUP] %s: FORM_GROUP AddMember failed for %s (GUID %u)",
                me->GetName(), pMember->GetName(), memberGuid);
            group->Disband();
            sObjectMgr.RemoveGroup(group);
            delete group;
            char eventData[96];
            snprintf(eventData, sizeof(eventData),
                "reason=add_member_failed|member_guid=%u", memberGuid);
            BridgeSendEvent("FORM_GROUP_FAIL", eventData);
            return;
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: added %s (GUID %u) to group",
            me->GetName(), pMember->GetName(), memberGuid);
    }

    CB_HITV(me->GetGUIDLow(), "cpp-group: exact requested group formed", memberGuids.size() + 1);
    BridgeSendEvent("FORM_GROUP_ACK", successData);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GROUP] %s: group formed — %u members, loot=NEED_BEFORE_GREED",
        me->GetName(), uint32(memberGuids.size() + 1));
}

// ============================================================
// METHOD 3: BridgeHandleDisbandGroup — NEW METHOD
//
// Add after BridgeHandleFormGroup.
// ============================================================
 
void AiBotAI::BridgeHandleDisbandGroup(const char* json)
{
    if (m_bridgeDispatchCbt == 0)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP missing command cbt");
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=missing_cbt");
        return;
    }

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail

    int leaderGuid = 0;
    bool const haveLeader = JsonExtractInt(payload, "leader_guid", leaderGuid);
    const char* arrStart = strstr(payload, "\"member_guids\"");
    if (!haveLeader || leaderGuid <= 0 || !arrStart)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP missing expected topology");
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=missing_expected_topology");
        return;
    }

    arrStart = strchr(arrStart, '[');
    const char* arrEnd = arrStart ? strchr(arrStart + 1, ']') : nullptr;
    if (!arrStart || !arrEnd)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP malformed expected member array");
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=malformed_member_guids");
        return;
    }
    ++arrStart;

    std::vector<uint32> expectedGuids;
    const char* p = arrStart;
    while (p < arrEnd)
    {
        while (p < arrEnd && (*p == ' ' || *p == ',')) p++;
        if (p >= arrEnd) break; // cb:fold expected guid list parse detail
        uint32 guid = (uint32)atoi(p);
        if (guid == 0)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP invalid expected member guid");
            BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=invalid_member_guid");
            return;
        }
        expectedGuids.push_back(guid);
        while (p < arrEnd && *p != ',') p++;
    }

    if (uint32(leaderGuid) != me->GetGUIDLow() || expectedGuids.size() < 2 || expectedGuids.size() > 5)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP invalid expected leader or size");
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=invalid_expected_topology");
        return;
    }

    bool expectedHasLeader = false;
    for (size_t i = 0; i < expectedGuids.size(); ++i)
    {
        if (expectedGuids[i] == me->GetGUIDLow())
            expectedHasLeader = true; // cb:fold bounded expected-set validation
        for (size_t j = 0; j < i; ++j)
        {
            if (expectedGuids[j] == expectedGuids[i])
            {
                CB_HITV(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP duplicate expected member", expectedGuids[i]);
                BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=duplicate_member");
                return;
            }
        }
    }
    if (!expectedHasLeader)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP expected set omits leader");
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=bad_payload|detail=leader_not_in_members");
        return;
    }

    std::string expectedList;
    char guidText[16];
    for (size_t i = 0; i < expectedGuids.size(); ++i)
    {
        snprintf(guidText, sizeof(guidText), i == 0 ? "%u" : ",%u", expectedGuids[i]);
        expectedList += guidText; // cb:fold exact ACK payload construction
    }
    char successData[224];
    snprintf(successData, sizeof(successData),
        "leader_guid=%u|member_guids=%s", me->GetGUIDLow(), expectedList.c_str());

    Group* group = me->GetGroup();
    if (!group)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP topology already absent");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: DISBAND_GROUP but not in a group", me->GetName());
        BridgeSendEvent("GROUP_DISBANDED", successData);
        return;
    }

    // [PLAYERPARTY] Never disband a REAL player's party from the wire (2026-07-07) — the
    // human formed it, only the human unforms it. Same rationale as the FORM_GROUP guard.
    if (FindPartyBoss())
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: disband refused, real player party, GROUP_DISBAND_FAIL");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: DISBAND_GROUP refused — this is a REAL player's party",
            me->GetName());
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=in_player_party");
        return;
    }

    std::vector<uint32> liveGuids;
    bool liveMembersAreBots = true;
    for (const auto& memberSlot : group->GetMemberSlots())
    {
        liveGuids.push_back(memberSlot.guid.GetCounter()); // cb:fold persistent exact-set enumeration
        Player* liveMember = sObjectMgr.GetPlayer(memberSlot.guid);
        if (!liveMember || !liveMember->GetSession() || !liveMember->GetSession()->GetBot())
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP offline/non-bot member, refusing",
                memberSlot.guid.GetCounter());
            liveMembersAreBots = false;
        }
    }

    bool topologyMatches = liveMembersAreBots && liveGuids.size() == expectedGuids.size();
    for (uint32 expectedGuid : expectedGuids)
    {
        bool found = false;
        for (uint32 liveGuid : liveGuids)
            if (liveGuid == expectedGuid)
            { // cb:fold bounded set comparison
                found = true;
                break;
            }
        if (!found)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP expected member absent", expectedGuid);
            topologyMatches = false;
        }
    }
    if (!topologyMatches)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-group: DISBAND_GROUP live topology mismatch, refusing");
        BridgeSendEvent("GROUP_DISBAND_FAIL", "reason=topology_mismatch");
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GROUP] %s: disbanding group", me->GetName());

    group->Disband();
    sObjectMgr.RemoveGroup(group);
    delete group;
    CB_HITV(me->GetGUIDLow(), "cpp-group: exact expected group disbanded", expectedGuids.size());
    BridgeSendEvent("GROUP_DISBANDED", successData);
}

void AiBotAI::BridgeHandleResurrect(const char* json)
{
    if (!me)
    {
        CB_HIT(0u, "cpp-rez: resurrect ignored, no player");
        return;
    }

    if (SuiHero::BlocksResurrection(me))
    {
        CB_HIT(me->GetGUIDLow(), "cpp-rez: held for paid hero revive");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[SUI-RTS] %s: automatic resurrection held for paid hero revive", me->GetName());
        return;
    }

    // A graveyard self-rez is already in flight: we've teleported the ghost and will rez it
    // ourselves once the teleport lands (see UpdateAI). Ignore ANY rez command until then —
    // a stray plain RESURRECT here would rez us mid-teleport, back in the death pocket (the loop).
    if (m_pendingGraveyardRez)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-rez: ignored, graveyard self-rez in flight");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s: RESURRECT ignored — graveyard self-rez in flight", me->GetName());
        return;
    }

    if (!me->IsDead() && me->GetDeathState() != DEAD)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-rez: bot not dead, RESPAWN echo");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: RESURRECT but bot is not dead, ignoring", me->GetName());
        BridgeSendEvent("RESPAWN", "");  // tell C# we're alive anyway
        return;
    }

    int atGraveyard = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json; // cb:fold payload fallback detail
    JsonExtractInt(payload, "at_graveyard", atGraveyard);

    // ── Death-loop / death-march escape: GHOST PORT, then SELF-rez once it lands ──
    // We port the *ghost* (non-combatable — zero aggro) with NearTeleportTo, stay dead, arm
    // m_pendingGraveyardRez, and emit GRAVEYARD_PORT (tells C# the port was accepted so it
    // stops its deadline). UpdateAI then resurrects us the moment the teleport has actually
    // applied — NOT on a C# roundtrip — so we can never rez in the kill pocket before moving.
    if (atGraveyard)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-rez: graveyard ghost port requested");
        float dx = me->GetPositionX(), dy = me->GetPositionY(), dz = me->GetPositionZ();
        uint32 mapId = me->GetMapId();

        // [HEARTH] (FINDING_008) Optional racial-start override. A persistent death loop the nearest
        // graveyard can't break (the graveyard is adjacent to the killer camp — SneakyShock's 307-death
        // Wetlands loop) ports the ghost to the bot's RACIAL START instead. C# only sends this when the
        // home is on THIS map (same-map NearTeleportTo — identical proven ghost-port seam as below); a
        // cross-continent home simply omits it and falls through to the normal graveyard logic. Reuses
        // the whole m_pendingGraveyardRez self-rez path, so the ghost is invulnerable in transit.
        float hx = 0.0f, hy = 0.0f, hz = 0.0f; int hmap = -1;
        if (JsonExtractFloat(payload, "home_x", hx) && JsonExtractFloat(payload, "home_y", hy) &&
            JsonExtractFloat(payload, "home_z", hz) && JsonExtractInt(payload, "home_map", hmap) &&
            hmap == (int)mapId)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-rez: racial start override, porting ghost home");
            ReGroundZ(hx, hy, hz, "rez-home");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s HEARTH port (ghost): (%.1f, %.1f, %.1f) -> racial start (%.1f, %.1f, %.1f) map=%u",
                me->GetName(), dx, dy, dz, hx, hy, hz, mapId);
            me->NearTeleportTo(hx, hy, hz, me->GetOrientation());
            m_pendingGraveyardRez = true;
            m_graveRezX = hx; m_graveRezY = hy; m_graveRezZ = hz;
            m_graveRezMap = mapId; m_graveRezWaitMs = 8000;
            BridgeSendEvent("GRAVEYARD_PORT", "");
            return;
        }

        // Primary: zone-linked nearest graveyard for the death position.
        WorldSafeLocsEntry const* grave = sObjectMgr.GetClosestGraveYard(dx, dy, dz, mapId, me->GetTeam());

        // [AIBOT-NEWBIE-GRAVE] Starter-valley graveyard gap: the starter SUB-zones (Coldridge
        // Valley, Northshire, Shadowglen, Valley of Trials, Deathknell, Camp Narache, ...) have NO
        // game_graveyard_zone link in the world DB, so the stock GetClosestGraveYard above falls back
        // to the nearest MAIN-zone graveyard and flings a dead newbie clear out of its valley (a
        // Coldridge death -> Kharanos, ~1300yd, observed live). For a low-level bot, if its racial
        // START — the playercreateinfo spawn, which sits in the newbie valley by its spirit healer —
        // is CLOSER to the death than the graveyard the core picked, rez THERE instead. Race-correct
        // for every start (GetPlayerInfo by race/class); self-limiting: a newbie that genuinely died
        // out by the main-zone graveyard keeps the stock pick (that one is then the closer of the two).
        bool  newbieGrave = false;
        float ngx = 0.0f, ngy = 0.0f, ngz = 0.0f;
        if (me->GetLevel() <= AIBOT_NEWBIE_GRAVE_LEVEL)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-rez: low level, racial start distance check");
            if (PlayerInfo const* startInfo = sObjectMgr.GetPlayerInfo(me->GetRace(), me->GetClass())) // cb:fold decl-if condition artifact
            {   // cb:fold newbie grave eval detail, decision probed below
                if ((int)startInfo->mapId == mapId)
                {   // cb:fold newbie grave eval detail
                    float const rsdx = startInfo->positionX - dx, rsdy = startInfo->positionY - dy;
                    float const rsDistSq = rsdx * rsdx + rsdy * rsdy;
                    float const gvDistSq = grave
                        ? (grave->x - dx) * (grave->x - dx) + (grave->y - dy) * (grave->y - dy)
                        : 1e18f;
                    if (rsDistSq < gvDistSq)
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-rez: racial start closer, overriding graveyard pick");
                        newbieGrave = true;
                        ngx = startInfo->positionX; ngy = startInfo->positionY; ngz = startInfo->positionZ;
                    }
                }
            }
        }

        // Fallback: the death pos resolves to a zone with NO graveyard link (areaId 0 /
        // out-of-bounds pocket — the Odugi/Haxixaw 200+ death loop). GetClosestGraveYard
        // returns null there, and the OLD code rez'd in place => infinite re-death. Probe
        // outward in rings; a probe point that lands in a REAL adjacent zone makes
        // GetClosestGraveYard return that zone's nearest valid, same-faction, level-
        // appropriate graveyard. Keep the hit closest to the actual death position.
        if (!grave && !newbieGrave)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-rez: no graveyard at death pos, probing rings");
            static const float kRings[] = { 150.0f, 350.0f, 600.0f, 1000.0f };
            float bestSq = 0.0f;
            for (float r : kRings)
            {
                for (int a = 0; a < 8; ++a)
                {
                    float ang = (float)a * (M_PI_F / 4.0f);
                    float px = dx + r * cosf(ang);
                    float py = dy + r * sinf(ang);
                    WorldSafeLocsEntry const* g =
                        sObjectMgr.GetClosestGraveYard(px, py, dz, mapId, me->GetTeam());
                    if (!g)
                        continue; // cb:fold ring probe scan detail
                    float gdx = g->x - dx, gdy = g->y - dy;
                    float dsq = gdx * gdx + gdy * gdy;
                    if (!grave || dsq < bestSq) { grave = g; bestSq = dsq; } // cb:fold ring probe scan detail
                }
                if (grave)
                    break;   // cb:fold ring probe scan detail (nearest ring with any hit wins)
            }
            if (grave)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-rez: ring probe found valid graveyard");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s graveyard port: death pos in dead zone (areaId 0?) — "
                    "probe found valid graveyard %.0fyd away", me->GetName(), sqrtf(bestSq));
            }
        }

        // Last resort: still nothing AND spawn point is on this map — port to spawn
        // (always a valid, level-appropriate starter loc). Never rez-in-place into the pocket.
        if (!grave && !newbieGrave && m_spawnMapId == mapId)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-rez: no graveyard found, porting ghost to spawn");
            // [GROUND] Spawn coords are authored, but ground them anyway so a bad spawn row
            // can't float the ghost. Local copy → snap → stash the grounded value so the
            // UpdateAI landed-check measures against the real target.
            float sx = m_spawnX, sy = m_spawnY, sz = m_spawnZ;
            ReGroundZ(sx, sy, sz, "rez-spawn");

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s graveyard port: no graveyard near death pos — porting ghost to spawn (%.1f,%.1f,%.1f)",
                me->GetName(), sx, sy, sz);
            me->NearTeleportTo(sx, sy, sz, m_spawnO);
            m_pendingGraveyardRez = true;
            m_graveRezX = sx; m_graveRezY = sy; m_graveRezZ = sz;
            m_graveRezMap = mapId; m_graveRezWaitMs = 8000;
            BridgeSendEvent("GRAVEYARD_PORT", "");
            return;
        }

        if (!grave && !newbieGrave)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-rez: no destination on map, rez in place, stuck risk");
            // Genuinely nowhere on this map (cross-map spawn + no graveyard) — degrade to the
            // old in-place rez, but log loudly so this rare case is visible if it ever bites.
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT] %s graveyard port: NO valid destination on map %u — rez in place (STUCK RISK)",
                me->GetName(), mapId);
            me->ResurrectPlayer(0.5f);
            me->CombatStop(true);
            me->SpawnCorpseBones();
            BridgeSendEvent("RESPAWN", "");
            BridgeSendState();
            return;
        }

        // [GROUND] Graveyard rows are authored on the ground (no-op in the normal case), but
        // snapping catches a bad/edge graveyard entry. A newbie override (racial start) wins over
        // the stock pick — see [AIBOT-NEWBIE-GRAVE] above. const grave → local copy.
        float gx, gy, gz;
        if (newbieGrave) { gx = ngx; gy = ngy; gz = ngz; } // cb:fold coord pick detail, override probed above
        else             { gx = grave->x; gy = grave->y; gz = grave->z; } // cb:fold coord pick detail
        ReGroundZ(gx, gy, gz, newbieGrave ? "rez-newbie-start" : "rez-grave");

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s graveyard port (ghost): (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f) map=%u",
            me->GetName(), dx, dy, dz, gx, gy, gz, mapId);

        me->NearTeleportTo(gx, gy, gz, me->GetOrientation());

        // Ghost stays DEAD on purpose; the teleport applies next tick. Arm the self-rez:
        // UpdateAI resurrects us once we've actually arrived at the graveyard. GRAVEYARD_PORT
        // just tells C# the port was accepted (stop its deadline) — the RESPAWN follows from us.
        m_pendingGraveyardRez = true;
        m_graveRezX = gx; m_graveRezY = gy; m_graveRezZ = gz;
        m_graveRezMap = mapId; m_graveRezWaitMs = 8000;
        BridgeSendEvent("GRAVEYARD_PORT", "");
        return;
    }

    // ── Plain rez (at the bot's current pos — which, post-port, IS the graveyard) ──
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s resurrecting at (%.1f, %.1f, %.1f)",
        me->GetName(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());

    me->ResurrectPlayer(0.5f);
    me->CombatStop(true);   // insurance: death cleared combat; never resume rez in-combat
    me->SpawnCorpseBones();

    BridgeSendEvent("RESPAWN", "");
    BridgeSendState();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s resurrected at 50%% HP", me->GetName());
}


// ============================================================
// EVENT SENDERS — C++ → C# notifications
// ============================================================

void AiBotAI::SendKillEvent(uint32 creatureEntry, uint32 creatureGuidLow, bool confirmed)
{
    if (!m_bridgeConnected)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: kill event dropped, not connected", creatureEntry);
        return;
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"KILL\","
        "\"creature_entry\":%u,\"creature_guid\":%u,\"confirmed\":%u}}",
        me->GetGUIDLow(), creatureEntry, creatureGuidLow, confirmed ? 1u : 0u);
    BridgeSend(json);
}

void AiBotAI::SendQuestUpdateEvent(uint32 questId, const char* status)
{
    if (!m_bridgeConnected)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: quest update event dropped, not connected", questId);
        return;
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"QUEST_UPDATE\","
        "\"quest_id\":%u,\"status\":\"%s\"}}",
        me->GetGUIDLow(), questId, status);
    BridgeSend(json);
}

void AiBotAI::SendLevelUpEvent(uint32 newLevel)
{
    if (!m_bridgeConnected)
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: levelup event dropped, not connected", newLevel);
        return;
    }

    char json[128];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"LEVEL_UP\",\"new_level\":%u}}",
        me->GetGUIDLow(), newLevel);
    BridgeSend(json);
}

// ── C0 (§5.1): minimal JSON string escape for outbound chat text. Chat is the one
// bridge lane carrying arbitrary player-typed text; a `"` or `\` in a message corrupts
// the newline-delimited JSON framing. Escapes `"` `\`; control chars (<0x20) become a
// space (in-game chat can't contain meaningful ones). Truncates safely on small dst.
static void JsonEscapeInto(char* dst, size_t dstSize, const char* src)
{
    size_t o = 0;
    if (!dst || dstSize == 0) return; // cb:fold json escape detail
    for (const char* p = src ? src : ""; *p && o + 2 < dstSize; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
        {   // cb:fold json escape detail
            dst[o++] = '\\';
            dst[o++] = (char)c; // cb:fold json escape detail
        }
        else if (c < 0x20)
            dst[o++] = ' '; // cb:fold json escape detail
        else
            dst[o++] = (char)c; // cb:fold json escape detail
    }
    dst[o] = '\0';
}

void AiBotAI::SendChatRecvEvent(const char* senderName, const char* message, const char* chatType, const char* channelName, uint32 senderGuidLow)
{
    if (!m_bridgeConnected)
    {
        CB_HITN(me->GetGUIDLow(), "cpp-bridge: chat event dropped, not connected", chatType);
        return;
    }

    // Escaped copies (§5.1): sender, message, channel_name are player-influenced text.
    // chatType is compiler-controlled ("say"/"whisper"/"channel"/"party") — no escape needed.
    char senderEsc[128];
    char messageEsc[1024];   // 511-char message cap upstream; worst case doubles under escaping
    char channelEsc[128];
    JsonEscapeInto(senderEsc, sizeof(senderEsc), senderName);
    JsonEscapeInto(messageEsc, sizeof(messageEsc), message);
    JsonEscapeInto(channelEsc, sizeof(channelEsc), channelName ? channelName : "");

    char json[1024];
    int n = snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"CHAT_RECV\","
        "\"sender\":\"%s\",\"sender_guid\":%u,\"message\":\"%s\","
        "\"chat_type\":\"%s\",\"channel_name\":\"%s\"}}",
        me->GetGUIDLow(), senderEsc, senderGuidLow, messageEsc, chatType, channelEsc);

    // A truncated line is broken JSON that poisons the newline framing — drop it instead.
    if (n < 0 || n >= (int)sizeof(json))
    {
        CB_HITV(me->GetGUIDLow(), "cpp-bridge: chat event dropped, escaped payload too large", n);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: CHAT_RECV dropped — escaped payload exceeds buffer (%d)",
            me->GetName(), n);
        return;
    }
    BridgeSend(json);
}

// ============================================================
// BridgeHandleLoadRaidPlan — [RAID-PLAN] adopt this bot's slice of the raid plan
// (PLAN_19 M-C; MSUIClient docs/plans/PLAN_19_RAID_DOCTRINE_PIPELINE.md).
//
// Payload: flat JSON, lists as house pipe strings — see RaidPlanLaw.cpp header
// for the exact wire shape. Validate-before-adopt: SuiParseRaidPlan builds the
// whole plan in a temporary; a refused payload leaves the previous plan standing
// and the RAID_PLAN_ACK carries the reason. Per-section diagnostics ride the ack
// so C# can log a bad plan loudly instead of the bot silently doing less than
// the plan says (the LOAD_ROTATION lesson, applied from day one).
//
// M-C stores and acks; the EncounterPlay doctrine + formation movement that
// EXECUTE m_raidPlan are M-D and read it from here.
// ============================================================
void AiBotAI::BridgeHandleLoadRaidPlan(const char* json)
{
    char err[192] = {0};
    SuiRaidPlan parsed;
    SuiRaidPlanDiag diag;
    if (!SuiParseRaidPlan(json, parsed, diag, err, sizeof(err)))
    {
        CB_HITN(me->GetGUIDLow(), "cpp-combatcfg: raid plan refused, previous stands", err);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-RAIDPLAN] %s: payload REFUSED — %s (previous plan stands)",
            me->GetName(), err);
        char ack[256];
        snprintf(ack, sizeof(ack), "ok=0|error=%s", err);
        BridgeSendEvent("RAID_PLAN_ACK", ack);
        return;
    }

    m_raidPlan = parsed;
    m_hasRaidPlan = true;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-RAIDPLAN] %s: plan '%s' (%s) adopted — job=%u side=%u class=%u "
        "assign=%d auras=%d addctl=%d targets=%d avoid=%d skipped=%d",
        me->GetName(), parsed.name.c_str(), parsed.encounterKey.c_str(),
        (uint32)parsed.job, (uint32)parsed.side, parsed.classId,
        diag.assignments, diag.auras, diag.addControl,
        diag.phaseTargets, diag.avoids, diag.skipped);

    char ack[256];
    snprintf(ack, sizeof(ack),
        "ok=1|plan=%s|assign=%d|auras=%d|addctl=%d|targets=%d|avoid=%d|skipped=%d",
        parsed.name.c_str(), diag.assignments, diag.auras, diag.addControl,
        diag.phaseTargets, diag.avoids, diag.skipped);
    BridgeSendEvent("RAID_PLAN_ACK", ack);
}


// ============================================================
// [CIRCUIT] BridgeHandleCircuitTrace — the one-switch control line (R6).
// Payload: {"mode":0|1} global recording mode; {"ship":0|1} this bot's
// armed-for-disk flag. C# pushes current state on HELLO and on every toggle.
// ============================================================
void AiBotAI::BridgeHandleCircuitTrace(const char* json)
{
    int mode = -1, ship = -1;
    if (JsonExtractInt(json, "mode", mode) && mode >= 0)
        CbCircuit::SetMode(mode); // cb:fold circuit control plumbing
    if (JsonExtractInt(json, "ship", ship) && ship >= 0)
        CbCircuit::SetShip(me->GetGUIDLow(), ship != 0); // cb:fold circuit control plumbing
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[AIBOT-CIRCUIT] %s: mode=%d ship=%d",
        me->GetName(), CbCircuit::g_mode, ship);
}
