/*
 * SuperUI CRPG/RTS possession core. See SuiPossess.h for the state model.
 *
 * Ordering law (borrowed from Unit::ModPossess): Camera::SetView must precede
 * SetClientControl or the client ignores the control packets; SetMover must
 * precede the client's CMSG_SET_ACTIVE_MOVER confirmation or
 * HandleSetActiveMoverOpcode snaps the client mover back.
 */

#include "SuiPossess.h"
#include "SuiCommanderRaid.h"
#include "SuiRaidPrepare.h"
#include "SuiRaidSupply.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <unordered_map>
#include <set>
#include <cmath>
#include <mutex>

#include "AiBotAIMain.h"
#include "Bag.h"
#include "Chat.h"
#include "Group.h"
#include "MasterPlayer.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "PlayerBotMgr.h"
#include "Server/WorldSession.h"
#include "SuiFactionControl.h"
#include "SuiCompanion.h"
#include "SuiTacticalFreeze.h"
#include "SuperUiContent/SuiWorld/Bridge/SuiPortal.h"
#include "SuiWorldState.h"
#include "World.h"
#include "Maps/Map.h"
#include "Weather.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "ReputationMgr.h"

namespace SuiPossess
{

static void SendSnapshot(WorldSession* to, Player* bot);   // defined with the M4 block below
static void SendMemberSpells(WorldSession* to, Player* bot);   // member-facts block below
static void PushMemberFactsTo(Player* realPlayer);             // member-facts block below
static void PushMemberQuestsTo(Player* realPlayer);            // quest-facts block below
static Creature* FreecamEyeOf(Player* player);
static void EnsureFreecamEye(Player* player);
static void RemoveFreecamEye(Player* player);

static void SendAck(WorldSession* session, ObjectGuid guid, AckResult result, Player* positionOf)
{
    // Custom SMSGs only ever go to clients that have spoken SUI (MSUIClient);
    // a stock 1.12 client driving a GM-command possession must not receive
    // opcodes beyond its table.
    if (!session->IsSuiCapable())
        return;
    // 25-byte ACK prefix + 8-byte SUI1 trailer + 4-byte catalog header +
    // six 32-byte prewarm rows. WorldPacket grows dynamically, but reserving
    // the negotiated maximum avoids reallocating every capability probe.
    WorldPacket data(SMSG_SUI_CONTROL_ACK, 25 + 8 + 4 + 6 * 32);
    data << uint64(guid.GetRawValue());
    data << uint8(result);
    if (positionOf)
        data << positionOf->GetPositionX() << positionOf->GetPositionY()
             << positionOf->GetPositionZ() << positionOf->GetOrientation();
    else
        data << 0.0f << 0.0f << 0.0f << 0.0f;
    // Existing clients consume only the fixed 25-byte ACK. The portal helper
    // appends the self-identifying capability suffix and its optional cast-warm
    // catalog; older clients safely ignore both.
    SuiPortal::WriteCapabilityTrailer(data);
    session->SendPacket(&data);
}

Player* GetControlledBot(WorldSession const* session)
{
    ObjectGuid guid = session->GetSuiControlledGuid();
    return guid.IsEmpty() ? nullptr : sObjectMgr.GetPlayer(guid);
}

Player* GetPossessor(Unit const* bot)
{
    ObjectGuid possessorGuid = bot->GetPossessorGuid();
    if (possessorGuid.IsEmpty() || !possessorGuid.IsPlayer())
        return nullptr;
    Player* possessor = sObjectMgr.GetPlayer(possessorGuid);
    if (!possessor || !possessor->GetSession())
        return nullptr;
    // Distinguish SUI possession from spell (charm-based) possession.
    return possessor->GetSession()->GetSuiControlledGuid() == bot->GetObjectGuid()
        ? possessor : nullptr;
}

bool IsSuiPossessed(Unit const* unit)
{
    return GetPossessor(unit) != nullptr;
}

bool IsControlledOwnerOf(Unit const* unit, Player const* observer)
{
    if (!unit || !observer || !observer->GetSession())
        return false;
    Player* controlled = GetControlledBot(observer->GetSession());
    return controlled && GetPossessor(controlled) == observer &&
        (unit->GetOwnerGuid() == controlled->GetObjectGuid() ||
         unit->GetCharmerGuid() == controlled->GetObjectGuid());
}

// Re-evaluate objects already streamed before a body change. Only this observer
// receives the refresh; ordinary object updates handle newly streamed objects.
static void RefreshActorVisibility(WorldSession* session, ObjectGuid previousActor)
{
    Player* observer = session->GetPlayer();
    Player* actor = session->GetSuiActor();
    if (!observer || !observer->IsInWorld() || !actor)
        return;
    ObjectGuidSet visible;
    {
        std::shared_lock<std::shared_timed_mutex> guard(observer->m_visibleGUIDs_lock);
        visible = observer->m_visibleGUIDs;
    }
    UpdateData data;
    for (ObjectGuid const& guid : visible)
    {
        if (!guid.IsUnit())
            continue;
        Unit* unit = observer->GetMap()->GetUnit(guid);
        if (!unit)
            continue;
        UpdateMask mask;
        mask.SetCount(unit->GetValuesCount());
        if (unit->GetTypeId() == TYPEID_UNIT)
            mask.SetBit(UNIT_NPC_FLAGS);
        ObjectGuid const owner = unit->GetOwnerGuid();
        ObjectGuid const charmer = unit->GetCharmerGuid();
        bool const affectedPet = unit->GetTypeId() == TYPEID_UNIT &&
            (owner == previousActor || charmer == previousActor ||
             owner == actor->GetObjectGuid() || charmer == actor->GetObjectGuid());
        if (affectedPet)
        {
            // includingEmpty sends zero training points too. The serializer masks
            // owner-only values back to zero when this observer loses access.
            unit->BuildValuesUpdateBlockForPlayerWithFlags(data, observer, UF_FLAG_OWNER_ONLY, true);
            mask.SetBit(UNIT_FIELD_HEALTH);
            mask.SetBit(UNIT_FIELD_MAXHEALTH);
        }
        if (mask.HasData())
            unit->BuildValuesUpdateBlockForPlayer(data, mask, observer);
    }
    if (data.HasData())
        data.Send(session);
}

static AiBotAI* BotAiOf(Player* bot)
{
    return bot ? dynamic_cast<AiBotAI*>(bot->AI()) : nullptr;
}

/// A body whose human just moved to an anchor beyond catch-up range (another map, or
/// far on this one) holds where it stands instead of catch-up teleporting after it
/// (owner 2026-09-03: "the rest stay"). DoPartyFollow releases the hold when the
/// anchor comes back within range.
static void HoldIfLeftBehind(Player* body, Player* anchor)
{
    AiBotAI* ai = BotAiOf(body);
    if (!ai || !body || !anchor)
        return;
    bool const isFar = body->GetMapId() != anchor->GetMapId() ||
        body->GetInstanceId() != anchor->GetInstanceId() ||
        body->GetDistance(anchor) > AIBOT_PARTY_CATCHUP_TELEPORT;
    if (!isFar)
        return;
    ai->m_suiLandedHold = true;
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s left behind by %s: holding here",
        body->GetName(), anchor->GetName());
    NotifyChainChanged(body);
}

void OnTaxiLanded(Player* player)
{
    // A possessed flyer is driven; the human decides where it goes next. An
    // unpossessed one (its human hopped away mid-air, or a party flight whose
    // human did not board) stays put — the catch-up teleport in DoPartyFollow
    // used to yank it straight back to the party the moment it landed.
    AiBotAI* ai = BotAiOf(player);
    if (!ai || IsSuiPossessed(player))
        return;
    ai->m_suiLandedHold = true;
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-TAXI] %s landed without its human: holding here",
        player->GetName());
    NotifyChainChanged(player);
}

// ── Own-character autonomy (M5, design corrected 2026-08-10) ─────────────────
// While the human drives a bot or the free camera, their real character runs
// the SAME fleet AI as every SuperUI bot (AiBotAI): it enrolls with the C#
// brain through the normal HELLO/STATE bridge flow, follows the party boss
// (possessed-first pre-pass in FindPartyBoss, so no anchor plumbing), and
// assists in combat. In-party behaviour is the gate — group break force-
// releases and detaches — and the bridge SELL_ITEMS wall refuses live-client
// sessions, so it can never vendor. Never stomps a foreign AI (mind control);
// deletion is explicit — this AI is owned here, not by PlayerBotMgr.

static void AttachUnattendedAI(Player* owner, Player* /*anchor*/)
{
    if (!owner || !owner->IsInWorld())
        return;
    if (dynamic_cast<AiBotAI*>(owner->AI()))
        return;   // already enrolled (bot switch / freecam transition)
    if (owner->AI())
        return;   // charmed/controlled by something else — leave it alone
    AiBotAI::AttachToRealCharacter(owner);
}

static void DetachUnattendedAI(Player* owner)
{
    if (!owner)
        return;
    AiBotAI* ai = dynamic_cast<AiBotAI*>(owner->AI());
    if (!ai)
        return;
    // Only the AI THIS file fabricated is ours to free. AttachToRealCharacter
    // stamps m_ownedDummyEntry on it; a PlayerBot's AiBotAI instead belongs to
    // PlayerBotEntry::ai (a unique_ptr installed on the Player by
    // PlayerBotMgr::OnPlayerInWorld) -- which is exactly why PlayerBotAI::Remove()
    // overrides PlayerAI::Remove() to detach WITHOUT deleting. Freeing a bot's AI
    // here leaves entry->ai dangling but non-null, so the rest of LogoutPlayer
    // calls straight through it (WorldSession::SendPacket ->
    // GetBot()->ai->OnPacketReceived) and PlayerBotMgr frees it a second time on
    // teardown: that is the ".kick <bot>" / ".bot delete <bot>" SIGSEGV.
    // Same law the core already spells out in Player::SetControlledBy and
    // Player::RemoveTemporaryAI ("Careful not to delete bot ai").
    // A bot only ever reaches here via OnLogout (the despawn edge) and needs none
    // of the manual-control cleanup below, so bail out whole.
    if (!ai->IsUnattendedRealCharacter())
        return;
    if (WorldSession* session = owner->GetSession())
        if (PlayerBotEntry* entry = session->GetBot())
            if (entry->ai.get() == ai)
                return;
    owner->SetAI(nullptr);
    delete ai;
    owner->StopMoving();
    owner->GetMotionMaster()->Clear(false, true);
    owner->GetMotionMaster()->MoveIdle();
    owner->AttackStop();
    // The AI era walked this body by server splines the real client never
    // confirms (no CMSG_MOVE_SPLINE_DONE in MSUIClient); a pending flag left
    // set here discards every movement packet of the returning driver.
    owner->SetSplineDonePending(false);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s back under manual control", owner->GetName());
}

static void ParkUnattendedBody(Player* owner)
{
    if (!owner)
        return;
    // A faction-wide target need not share the owner's group. The generic
    // unattended AiBotAI has no explicit anchor in that case and may select
    // solo doctrine/quests while the human is driving a distant army unit.
    // Remove only our attached real-character AI and park the owner exactly at
    // the relocation destination until release restores manual control.
    DetachUnattendedAI(owner);
    owner->StopMoving();
    owner->GetMotionMaster()->Clear(false, true);
    owner->GetMotionMaster()->MoveIdle();
    owner->SetSplineDonePending(false);
    owner->AttackStop();
    owner->CombatStop(true);
}

/// Everything except the ACK — shared by the wire handler and the GM command.
static AckResult TryBegin(WorldSession* session, ObjectGuid targetGuid, Player** grantedBot)
{
    Player* possessor = session->GetPlayer();
    if (!possessor || !possessor->IsInWorld() || session->GetBot())
        return DENY_REQUESTER_STATE;
    if (!session->GetSuiControlledGuid().IsEmpty())
        return DENY_REQUESTER_STATE;    // release first
    if (possessor->IsDead() || possessor->IsTaxiFlying() || possessor->GetTransport() ||
        possessor->IsBeingTeleported() || !possessor->IsSelfMover())
        return DENY_REQUESTER_STATE;

    Player* bot = sObjectMgr.GetPlayer(targetGuid);
    if (!bot || !bot->IsInWorld())
        return DENY_NOT_FOUND;
    // Authority is target-local as well as requester-local: an observer outside
    // somebody else's field cannot seize a latched bot and rewrite its AI,
    // motion, camera or control state behind that lock.
    if (bot->IsSuiTacticallyFrozen())
        return DENY_TARGET_STATE;
    if (!bot->GetSession() || !bot->GetSession()->GetBot())
        return DENY_NOT_BOT;
    AiBotAI* ai = BotAiOf(bot);
    if (!ai)
        return DENY_NOT_BOT;
    // [COMPANION] Another human's summoned alt is a real player to you.
    if (!SuiCompanion::MayCommand(possessor, bot))
        return DENY_NOT_BOT;
    Group* group = possessor->GetGroup();
    bool factionAuthorized = SuiFactionControl::CanControl(possessor, bot);
    bool const partyAuthorized = group && group == bot->GetGroup();
    if (!partyAuthorized && !factionAuthorized)
        return DENY_NOT_IN_GROUP;
    if (!bot->GetPossessorGuid().IsEmpty())
        return DENY_BUSY;
    if (bot->IsDead() || bot->IsTaxiFlying() || bot->GetTransport() || bot->IsBeingTeleported())
        return DENY_TARGET_STATE;
    // Camera::SetView requires the target on the same map. Faction control can
    // explicitly relocate the owner's body outdoors; the client retries this
    // request only after normal world streaming has made the target visible.
    bool const sameMapInstance = bot->GetMapId() == possessor->GetMapId() &&
        bot->GetInstanceId() == possessor->GetInstanceId();
    bool const visible = sameMapInstance && possessor->IsInVisibleList(bot);
    // [SUI] A PARTY member out of streaming range on the SAME map is granted in place
    // (owner 2026-09-03: hopping Stormwind → Westfall dragged the main along with a
    // loading screen). Camera::SetView needs only the same map: far-sight streams the
    // bot's surroundings to the client, whose grant handler waits for the entity and
    // loads the destination tiles behind a curtain. Cross-map hops still relocate.
    if (!visible && !(partyAuthorized && sameMapInstance))
    {
        if (!factionAuthorized)
            return DENY_NOT_FOUND;
        SuiFactionControl::RelocateResult relocate =
            SuiFactionControl::TryRelocate(possessor, bot);
        if (relocate == SuiFactionControl::RELOCATE_INSTANCE_DENIED)
            return DENY_CROSS_INSTANCE;
        if (relocate == SuiFactionControl::RELOCATE_ACCEPTED)
        {
            if (grantedBot)
                *grantedBot = bot;
            return ACK_RELOCATING;
        }
        return DENY_NOT_FOUND;
    }

    // ── Grant ──
    // This request may originate from Commander free view (including the GM
    // diagnostic path). Retire its streaming eye only after every denial gate
    // has passed so a rejected request leaves the existing view untouched.
    RemoveFreecamEye(possessor);
    ai->SetPossessed(true);
    // Stop whatever the AI had it doing FIRST. A bot taken mid-stride keeps its movespline,
    // and HandleMovementOpcodes drops every client movement packet while one is unfinalized
    // (HandleMovementOpcodes: if movespline is not Finalized, return) — so the human would
    // drive a body that
    // ignores them and keeps walking to wherever the AI was already sending it. Since bots are
    // almost always following the party when you take them, that is the normal case, not an
    // edge one: it reads as "I move but nothing happens, and I snap back to the group".
    bot->StopMoving();
    bot->GetMotionMaster()->Clear(false, true);
    bot->GetMotionMaster()->MoveIdle();
    // StopMoving on a moving Player launches a stop spline, and Launch marks every
    // player spline "done pending" until a client confirms it — which this client
    // never will (MSUIClient does not speak CMSG_MOVE_SPLINE_DONE). Together with
    // any stale flag from the bot's AI era, that blocked HandleMovementOpcodes
    // wholesale: the human drove a body whose every packet was discarded. Clear
    // it LAST, after the stop above.
    bot->SetSplineDonePending(false);
    // The bot's brain-era journey does not survive a human taking the body: a
    // live TASK_MOVE_TO gates DoPartyFollow and resumes walking on release.
    ai->SuiAbandonJourney();
    ai->m_suiLandedHold = false;     // the human is back on this body
    // A half-open loot window would strand the loot session (loot is
    // player-scoped); mirror ModPossess's force-release.
    if (ObjectGuid lootGuid = bot->GetLootGuid())
        bot->GetSession()->DoLootRelease(lootGuid);

    possessor->GetCamera().SetView(bot);
    bot->SetPossessorGuid(possessor->GetObjectGuid());
    session->SetSuiControlledGuid(bot->GetObjectGuid());
    possessor->SetMover(bot);
    possessor->SetClientControl(bot, 1);

    // The abandoned real character follows and assists whoever the human drives.
    if (partyAuthorized)
    {
        AttachUnattendedAI(possessor, bot);
        HoldIfLeftBehind(possessor, bot);   // a far hop: the main stays where it is
    }
    else
        ParkUnattendedBody(possessor);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s now possesses bot %s",
        possessor->GetName(), bot->GetName());
    RefreshActorVisibility(session, possessor->GetObjectGuid());
    if (grantedBot)
        *grantedBot = bot;
    return ACK_OK;
}

/// Shared release path. Safe against a despawned bot or a tearing-down session.
// ── Freecam eye ──────────────────────────────────────────────────────────────
// The free view must LOAD what it overflies. Visibility/grid streaming follows
// the player's Camera, so while in the free view the camera rides an invisible
// World Trigger summon (active object: keeps its grid ticking) that the client
// repositions with CMSG_SUI_CAM. Torn down on every path that leaves the view.
static std::unordered_map<uint64, uint64> s_freecamEyes;
static std::recursive_mutex s_freecamEyesMutex;

static Creature* FreecamEyeOf(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(s_freecamEyesMutex);
    auto it = s_freecamEyes.find(player->GetObjectGuid().GetRawValue());
    if (it == s_freecamEyes.end())
        return nullptr;
    return player->GetMap()->GetCreature(ObjectGuid(it->second));
}

static void EnsureFreecamEye(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(s_freecamEyesMutex);
    if (!player || !player->IsInWorld())
        return;
    if (Creature* existing = FreecamEyeOf(player))
    {
        player->GetCamera().SetView(existing);
        return;
    }
    Creature* eye = player->SummonCreature(15384 /* World Trigger, invisible model */,
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), 0.0f,
        TEMPSUMMON_MANUAL_DESPAWN, 0, true /* active object */);
    if (!eye)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[SUI] %s: freecam eye summon failed",
            player->GetName());
        return;
    }
    eye->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
    s_freecamEyes[player->GetObjectGuid().GetRawValue()] = eye->GetObjectGuid().GetRawValue();
    player->GetCamera().SetView(eye);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s: freecam eye up", player->GetName());
}

static void RemoveFreecamEye(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(s_freecamEyesMutex);
    if (!player)
        return;
    auto it = s_freecamEyes.find(player->GetObjectGuid().GetRawValue());
    if (it == s_freecamEyes.end())
        return;
    if (player->IsInWorld())
        if (Creature* eye = player->GetMap()->GetCreature(ObjectGuid(it->second)))
        {
            // Give the view back to whatever the player is actually looking through. Landing
            // out of the free view while still possessing must return it to the BOT — a blind
            // ResetView would point the camera at the abandoned body and stop streaming the
            // world around the character being driven.
            Player* stillDriving = nullptr;
            if (WorldSession* session = player->GetSession())
            {
                ObjectGuid driven = session->GetSuiControlledGuid();
                if (!driven.IsEmpty())
                    stillDriving = sObjectMgr.GetPlayer(driven);
            }
            if (stillDriving && stillDriving->IsInWorld())
                player->GetCamera().SetView(stillDriving);
            else
                player->GetCamera().ResetView();
            eye->AddObjectToRemoveList();
        }
    s_freecamEyes.erase(it);
}

// A camera/mover handoff need not change either body's physical zone. Refresh
// after the control ack through the body's own session so the owner mirror tags it.
static void SendBodyWeather(Player* body)
{
    if (!body || !body->IsInWorld() || !sWorld.getConfig(CONFIG_BOOL_WEATHER))
        return;
    Weather* weather = body->GetMap()->GetWeatherSystem()->FindOrCreateWeather(body->GetZoneId());
    weather->SendWeatherUpdateToPlayer(body);
}

static bool DoRelease(WorldSession* session, AckResult reason, bool serverInitiated)
{
    ObjectGuid botGuid = session->GetSuiControlledGuid();
    if (botGuid.IsEmpty())
        return false;
    session->SetSuiControlledGuid(ObjectGuid());

    Player* possessor = session->GetPlayer();
    if (Player* bot = sObjectMgr.GetPlayer(botGuid))
    {
        // A loot window the commander opened AS the bot must not outlive the
        // possession (the AI would inherit a half-open loot session). Release it
        // while the possessor is still set so the release frame mirrors back.
        if (ObjectGuid lootGuid = bot->GetLootGuid())
            if (bot->GetSession())
                bot->GetSession()->DoLootRelease(lootGuid);
        bot->SetPossessorGuid(ObjectGuid());
        if (AiBotAI* ai = BotAiOf(bot))
            ai->SetPossessed(false);
        if (possessor)
            HoldIfLeftBehind(bot, possessor);   // released far from the human: stays put
    }
    if (possessor)
    {
        possessor->GetCamera().ResetView();
        possessor->SetMover(nullptr);           // resolves to self
        possessor->SetClientControl(possessor, 1);
        // Manual control resumes — except into the free camera, where the own
        // character stays autonomous. Its anchor remains the just-released bot
        // (still a valid group member); it only re-points on the next possess.
        if (reason != RELEASED_FREECAM)
        {
            DetachUnattendedAI(possessor);
            RemoveFreecamEye(possessor);
        }
        else
            EnsureFreecamEye(possessor);
    }
    if (serverInitiated)
        // In-flight MSG_MOVE_* still carry bot coordinates; without the drain
        // window they would be attributed to the own character standing far
        // away (GetConfirmedMover fallback) and trip anticheat/teleport.
        session->RejectMovementPacketsFor(1000);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s released bot %s (reason %u)",
        possessor ? possessor->GetName() : "<logging out>",
        botGuid.GetString().c_str(), uint32(reason));

    SendAck(session, possessor ? possessor->GetObjectGuid() : ObjectGuid(), reason, possessor);
    RefreshActorVisibility(session, botGuid);
    // The client resets its pet bar on the release ack; hand the own character's
    // pet bar back after it (no pet → nothing sent, bar stays empty).
    if (possessor && possessor->IsInWorld())
    {
        possessor->PetSpellInitialize();
        SendBodyWeather(possessor);
    }
    if (possessor && possessor->GetGroup())
        BroadcastRoster(possessor->GetGroup());
    return true;
}

void HandleRequest(WorldSession* session, ObjectGuid targetGuid)
{
    session->SetSuiCapable(true);
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
    {
        SendAck(session, targetGuid, DENY_REQUESTER_STATE, nullptr);
        return;
    }
    Player* bot = nullptr;
    AckResult result = TryBegin(session, targetGuid, &bot);
    SendAck(session, targetGuid, result, bot);
    if (result == ACK_OK && bot)
    {
        // Owner data follows the grant, deliberately routed through the BOT's
        // own (socket-less) session: the SendPacket mirror wraps each packet in
        // SMSG_SUI_PROXY with the right source guid. Mid-session re-sends of
        // both packets are proven safe in this fork (SpecCommands .testbars).
        bot->SendInitialSpells();
        bot->SendProficiency(ITEM_CLASS_WEAPON, bot->GetWeaponProficiency());
        bot->SendProficiency(ITEM_CLASS_ARMOR, bot->GetArmorProficiency());
        if (MasterPlayer* master = bot->GetSession()->GetMasterPlayer())
            master->SendInitialActionButtons();
        SendSnapshot(session, bot);
        bot->GetReputationMgr().SendInitialReputations();
        // The driven body's pet bar (hunter/warlock companions): SMSG_PET_SPELLS on
        // the bot's session mirrors through the proxy. No pet → nothing is sent and
        // the client's control-change reset leaves the bar empty.
        bot->PetSpellInitialize();
        SendBodyWeather(bot);
        if (Group* group = session->GetPlayer()->GetGroup())
            BroadcastRoster(group);
    }
}

bool IsCommandedFromFreeView(Unit const* bot)
{
    Player* possessor = GetPossessor(bot);
    return possessor != nullptr && FreecamEyeOf(possessor) != nullptr;
}

bool IsFreeViewUp(Player* player)
{
    return player != nullptr && FreecamEyeOf(player) != nullptr;
}

bool IsFreecamEye(Unit const* unit)
{
    if (!unit)
        return false;
    std::lock_guard<std::recursive_mutex> guard(s_freecamEyesMutex);
    uint64 const guid = unit->GetObjectGuid().GetRawValue();
    for (auto const& pair : s_freecamEyes)
        if (pair.second == guid)
            return true;
    return false;
}

bool PrepareForRelocation(Player* player)
{
    bool const restoreFreeView = FreecamEyeOf(player) != nullptr;
    RemoveFreecamEye(player);
    return restoreFreeView;
}

void RestoreAfterFailedRelocation(Player* player, bool restoreFreeView)
{
    if (restoreFreeView)
        EnsureFreecamEye(player);
}

void HandleCam(WorldSession* session, float x, float y, float z, bool active)
{
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
        return;
    // The free view came down. Drop the eye — which also ends IsCommandedFromFreeView, handing
    // a possessed bot back to the client that is once again really driving it.
    if (!active)
    {
        SuiTacticalFreeze::ReleaseOwnedBy(session, SuiTacticalFreeze::FREEZE_RELEASED_VIEW);
        // Landing. Tear the eye down FIRST so the commanded-remotely waiver is
        // already off when the stop below finalizes the bot's spline — otherwise
        // MovementInform still reads "commanded" and chains the next task leg
        // under the feet of the client that is about to really drive it.
        RemoveFreecamEye(player);
        // Manual primaries are a Command View thing: hand every unit's actions back to its AI.
        {
            auto handBack = [](Player* p)
            {
                if (!p)
                    return;
                if (AiBotAI* ai = dynamic_cast<AiBotAI*>(p->AI()))
                    ai->m_suiManual = false;
            };
            handBack(player);
            if (Group* group = player->GetGroup())
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                    handBack(itr->getSource());
        }
        // The command era walked the bot by server splines this client never
        // confirms; stop it where it stands and drop the pending flag, or every
        // movement packet from its returning driver is silently discarded.
        if (Player* bot = GetControlledBot(session))
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear(false, true);
            bot->GetMotionMaster()->MoveIdle();
            bot->SetSplineDonePending(false);
        }
        return;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return;
    // ENSURE, not just move. HandleRequest tears the eye down ("possess overrides the
    // free-camera view"), which was true while possessing meant leaving the free view — it
    // does not any more: the client now commands a toon with the camera still up, and kept
    // flying with nothing streaming the world in around it. The client only ever sends
    // CMSG_SUI_CAM while its free view is up, so rebuilding here makes the eye's existence
    // track the client's real camera mode instead of its possession state.
    EnsureFreecamEye(player);
    if (Creature* eye = FreecamEyeOf(player))
        eye->NearTeleportTo(x, y, z, 0.0f);
}

void HandleRelease(WorldSession* session, uint8 mode)
{
    session->SetSuiCapable(true);
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
    {
        // The driven body is the field anchor and cannot change mid-lock. Command
        // View exit calls ReleaseOwnedBy before it reaches this ordinary path.
        SendAck(session, session->GetPlayer() ? session->GetPlayer()->GetObjectGuid() : ObjectGuid(),
            DENY_REQUESTER_STATE, session->GetSuiActor());
        return;
    }
    AckResult reason = mode == RELEASE_TO_FREECAM ? RELEASED_FREECAM : RELEASED;
    if (!DoRelease(session, reason, false))
    {
        // Nothing possessed: the freecam enter/leave path from the own character.
        if (Player* player = session->GetPlayer())
        {
            if (mode == RELEASE_TO_FREECAM)
            {
                Player* anchor = nullptr;
                if (Group* group = player->GetGroup())
                    anchor = sObjectMgr.GetPlayer(group->GetLeaderGuid());
                AttachUnattendedAI(player, anchor && anchor != player ? anchor : nullptr);
                EnsureFreecamEye(player);
            }
            else
            {
                DetachUnattendedAI(player);
                RemoveFreecamEye(player);
            }
        }
        // Ack so the client state machine resolves either way.
        SendAck(session, session->GetPlayer() ? session->GetPlayer()->GetObjectGuid() : ObjectGuid(),
            reason, session->GetPlayer());
    }
}

void ForceRelease(WorldSession* session, AckResult reason)
{
    DoRelease(session, reason, true);
}

// ── Conscription ─────────────────────────────────────────────────────────────
// A bot assigned to a client control group is ENLISTED: the C# brain planner
// stands down for it (STATE conscripted:1 plus the bridge fence) while combat
// AI, doctrine reactions and explicit RTS orders keep working. The registry
// exists so a commander's logout can muster out the whole army; group state is
// session-local on the client, so the server must not depend on a farewell
// packet arriving.

static std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> s_conscriptsByCommander;

static void ForgetConscript(ObjectGuid commander, ObjectGuid bot)
{
    auto it = s_conscriptsByCommander.find(commander);
    if (it == s_conscriptsByCommander.end())
        return;
    std::vector<ObjectGuid>& roster = it->second;
    roster.erase(std::remove(roster.begin(), roster.end(), bot), roster.end());
    if (roster.empty())
        s_conscriptsByCommander.erase(it);
}

static void Conscript(Player* commander, Player* pMember, AiBotAI* ai)
{
    ObjectGuid const commanderGuid = commander->GetObjectGuid();
    if (ai->IsSuiConscripted() && ai->m_suiConscriptedBy != commanderGuid)
        ForgetConscript(ai->m_suiConscriptedBy, pMember->GetObjectGuid());   // takeover
    ai->m_suiConscriptedBy = commanderGuid;
    std::vector<ObjectGuid>& roster = s_conscriptsByCommander[commanderGuid];
    if (std::find(roster.begin(), roster.end(), pMember->GetObjectGuid()) == roster.end())
        roster.push_back(pMember->GetObjectGuid());
    // The planner's errand dies here (RefreshDoctrine precedent); the march
    // stops unless combat AI is mid-fight. Enlisted = at attention.
    ai->SuiAbandonJourney();
    ai->m_suiRtsHold = true;
    if (!pMember->IsInCombat())
        ai->StopMoving();
}

static void Dismiss(Player* pMember, AiBotAI* ai)
{
    if (!ai->IsSuiConscripted())
        return;
    ForgetConscript(ai->m_suiConscriptedBy, pMember->GetObjectGuid());
    ai->m_suiConscriptedBy.Clear();
    // Muster out: tactical latches die with the journey (SuiAbandonJourney
    // clears hold and the sheath override) and the brain resumes questing in
    // place — the next STATE shows conscripted:0 and the planner re-tasks from
    // wherever the war left the bot.
    ai->SuiAbandonJourney();
}

static void DismissConscriptsOf(Player* commander)
{
    auto it = s_conscriptsByCommander.find(commander->GetObjectGuid());
    if (it == s_conscriptsByCommander.end())
        return;
    std::vector<ObjectGuid> roster = std::move(it->second);
    s_conscriptsByCommander.erase(it);
    for (ObjectGuid guid : roster)
        if (Player* bot = sObjectMgr.GetPlayer(guid))
            if (AiBotAI* ai = dynamic_cast<AiBotAI*>(bot->AI()))
                if (ai->m_suiConscriptedBy == commander->GetObjectGuid())
                {
                    ai->m_suiConscriptedBy.Clear();
                    ai->SuiAbandonJourney();
                }
}

// [SUI] Lay a formation around the anchor and walk every subject to its slot.
// LINE is a standing army: ranks of five, 2.5 yd files and 3 yd ranks, all
// facing the commander. CIRCLE spaces the set evenly on a ring, everyone
// facing outward. The packet coordinate is the anchor; a zero coordinate means
// "form up where you stand" (the squad's centroid). Slots ride the normal
// MoveToDestination path (chunked pathfinding, in-combat deferral) and the
// slot facing is applied by MovementInform on arrival.
static void DispatchFormation(Player* player,
    std::vector<std::pair<Player*, AiBotAI*>> const& subjects,
    bool circle, float x, float y, float z)
{
    float const pi = 3.14159265f;
    auto normalize = [pi](float o)
    {
        while (o < 0.f) o += 2.f * pi;
        while (o >= 2.f * pi) o -= 2.f * pi;
        return o;
    };

    float ax = x, ay = y, az = z;
    if (ax == 0.0f && ay == 0.0f)
    {
        az = 0.0f;
        for (auto const& entry : subjects)
        {
            ax += entry.first->GetPositionX();
            ay += entry.first->GetPositionY();
            az += entry.first->GetPositionZ();
        }
        float const inv = 1.0f / float(subjects.size());
        ax *= inv; ay *= inv; az *= inv;
    }

    // The army faces its commander; a commander standing on the anchor keeps
    // their own facing as the parade direction.
    float face = player->GetOrientation();
    float const cdx = player->GetPositionX() - ax;
    float const cdy = player->GetPositionY() - ay;
    if (cdx * cdx + cdy * cdy > 1.0f)
        face = atan2f(cdy, cdx);

    size_t const n = subjects.size();
    float const fileSpacing = 2.5f, rankSpacing = 3.0f, arcSpacing = 3.0f;
    float const radius = std::max(2.5f, float(n) * arcSpacing / (2.0f * pi));
    size_t const files = n < 5 ? n : 5;

    for (size_t i = 0; i < n; ++i)
    {
        AiBotAI* ai = subjects[i].second;
        float sx, sy, slotFace;
        if (circle)
        {
            float const theta = face + 2.0f * pi * float(i) / float(n);
            sx = ax + radius * cosf(theta);
            sy = ay + radius * sinf(theta);
            slotFace = theta;   // everyone looks outward
        }
        else
        {
            float const fileOffset =
                (float(i % files) - float(files - 1) * 0.5f) * fileSpacing;
            float const rankOffset = float(i / files) * rankSpacing;
            // Files span the commander's right hand; ranks stack away from them.
            sx = ax + cosf(face - pi * 0.5f) * fileOffset - cosf(face) * rankOffset;
            sy = ay + sinf(face - pi * 0.5f) * fileOffset - sinf(face) * rankOffset;
            slotFace = face;
        }

        ai->SuiClearWaypoints();
        ai->m_currentTask.Clear();
        ai->m_currentTask.type = TASK_MOVE_TO;
        ai->m_currentTask.x = sx;
        ai->m_currentTask.y = sy;
        ai->m_currentTask.z = az;
        ai->MoveToDestination(sx, sy, az);
        // After SuiClearWaypoints — it wipes the facing stamp.
        ai->m_suiFormationFacing = normalize(slotFace);
    }
}

void HandleOrder(WorldSession* session, uint8 orderType,
    std::vector<ObjectGuid> const& subjects, ObjectGuid targetGuid,
    float x, float y, float z)
{
    session->SetSuiCapable(true);
    Player* player = session->GetPlayer();
    if (!player || session->GetBot())
        return;
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
        return; // only CMSG_SUI_TACTICAL_QUEUE may add actions while time is locked
    // Solo is legal: the unattended own character (freecam with no party) must
    // obey RTS orders too — a group only widens the orderable set. This gate
    // silently ate every order a partyless owner clicked from the free view.
    Group* group = player->GetGroup();
    bool const freeView = FreecamEyeOf(player) != nullptr;

    // Enrollment is always explicit: the privileged empty-list party expansion
    // stays a nudge and must never conscript or dismiss anyone.
    if ((orderType == ORDER_CONSCRIPT || orderType == ORDER_DISMISS ||
         orderType == ORDER_MANUAL || orderType == ORDER_AUTO) && subjects.empty())
        return;

    // Preflight the complete subject set before changing any AI so a
    // multi-selection cannot partially apply when one commanded body is held
    // by another real player's field. The typed tactical queue is the sole
    // authoring path for a frozen subject.
    if (!subjects.empty())
    {
        for (ObjectGuid guid : subjects)
            if (Player* subject = sObjectMgr.GetPlayer(guid))
                if (subject->IsSuiTacticallyFrozen())
                    return;
    }
    else if (Group* subjectGroup = player->GetGroup())
    {
        for (GroupReference* itr = subjectGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* subject = itr->getSource();
            if (subject && subject->IsSuiTacticallyFrozen() &&
                SuiCompanion::MayCommand(player, subject))
                return;
        }
    }
    if (!targetGuid.IsEmpty())
        if (Unit* target = player->GetMap()->GetUnit(targetGuid))
            if (target->IsSuiTacticallyFrozen())
                return;

    // Formation slots depend on the whole ordered set, so those subjects are
    // collected here and laid out after the expansion loop below.
    std::vector<std::pair<Player*, AiBotAI*>> formationSubjects;
    std::set<Group*> changedChainGroups;

    // [SUI] Owner 2026-08-28: bots must not STACK in transit or on arrival. A
    // multi-subject move/waypoint order fans out around the clicked point —
    // slot 0 takes the point itself, later slots ring it at 2 yd spacing.
    // Slots go by assignment order, which is stable per subject list, so a
    // waypoint CHAIN keeps every bot in its own lane leg after leg. The offset
    // destination rides the same task pathfinding a formation slot does.
    uint32 moveSlot = 0;
    auto fannedDest = [&moveSlot](float cx, float cy, float& fx, float& fy)
    {
        float const pi = 3.14159265f;
        uint32 const slot = moveSlot++;
        if (slot == 0) { fx = cx; fy = cy; return; }
        uint32 const ring = (slot - 1) / 6 + 1;
        uint32 const pos = (slot - 1) % 6;
        float const angle = float(pos) * (pi / 3.0f) + float(ring - 1) * (pi / 6.0f);
        float const radius = 2.0f * float(ring);
        fx = cx + cosf(angle) * radius;
        fy = cy + sinf(angle) * radius;
    };

    auto orderBot = [&](Player* pMember)
    {
        if (!pMember || pMember->IsSuiTacticallyFrozen())
            return;
        // Empty-list expansion retains the real party/own-character law. An
        // explicit non-group subject may additionally use the faction-control
        // grant, but only from a live Free View and only while streamed in the
        // same map/instance. SuiFactionControl revalidates genuine AiBot identity
        // and team server-side; a friendly-looking or forged client GUID is not
        // authority.
        bool const partyAuthorized = pMember == player ||
            (group && pMember->GetGroup() == group);
        bool const factionAuthorized = freeView &&
            SuiFactionControl::CanControl(player, pMember) &&
            pMember->GetMapId() == player->GetMapId() &&
            pMember->GetInstanceId() == player->GetInstanceId() &&
            player->IsInVisibleList(pMember);
        if (!partyAuthorized && !factionAuthorized)
            return;
        if (factionAuthorized &&
            (pMember->IsDead() || pMember->IsTaxiFlying() ||
             pMember->GetTransport() || pMember->IsBeingTeleported()))
            return;
        // AI-attached is the real gate: fabricated bots always are; the human's
        // own character only while unattended (possession/freecam), which is
        // exactly when it must obey RTS orders alongside the bots. A manually
        // driven character has no AiBotAI, and the possessed bot stays excluded.
        AiBotAI* ai = dynamic_cast<AiBotAI*>(pMember->AI());
        if (!ai)
            return;
        // [COMPANION] Ownership closure: another human's character — attended,
        // unattended, or a summoned alt — takes no orders from you.
        if (!SuiCompanion::MayCommand(player, pMember))
            return;
        if (ai->IsPossessed())
        {
            // Normally excluded: possession makes the CLIENT the bot's mover, and a
            // server-side MOVE_TO would fight the movement stream coming the other way.
            // From the FREE VIEW that conflict cannot arise — the client's controller is the
            // detached camera, its movement stream is parked, and the possessed bot receives
            // no client input at all. So the toon you are commanding stays orderable, which
            // is the whole point of clicking it: halo, bars, and a right-click that moves it.
            // The freecam eye is the server's evidence of that camera mode (the client sends
            // CMSG_SUI_CAM only while the free view is up, and HandleCam keeps the eye alive).
            Player* possessor = GetPossessor(pMember);
            if (possessor != player || !freeView)
                return;
        }

        // Reuse the bridge command paths verbatim — ordered behaviour is then
        // bit-identical to a brain-issued MOVE_TO / ATTACK_TARGET, including the
        // chunked pathfinding and the in-combat MOVE_TO deferral. The PlayerParty
        // escort loop yields to an active TASK_MOVE_TO, so orders are not
        // formation-snapped on the next tick.
        // [SUI] Every explicit order is discipline: the idle wander stands down
        // until the journey is abandoned (doctrine change or possession).
        // Preparation and supply are not movement orders: they neither stand the
        // wander down nor yield an applied raid plan row.
        bool const logistics = orderType == ORDER_PREPARE || orderType == ORDER_SUPPLY;
        if (orderType != ORDER_AUTO && !logistics)
            ai->m_suiRtsHold = true;

        if (!logistics)
            SuiCommanderRaid::Yield(pMember);
        char json[192];
        switch (orderType)
        {
            case ORDER_MOVE:
            {
                // [SUI] Fix A: coalesced — stash the newest dest; UpdateAI issues it once next
                // tick (SuiClearWaypoints + MOVE_TO happen there), so a right-click flood can no
                // longer trigger a full pathfind + spline restart per packet on a single unit.
                float fx, fy;
                fannedDest(x, y, fx, fy);
                ai->QueueSuiRtsMove(fx, fy, z);
                break;
            }
            case ORDER_ATTACK:
                if (targetGuid.IsCreature())
                {
                    // Carry the ENTRY too. A vmangos creature ObjectGuid is
                    // (HIGHGUID_UNIT, entry, counter) and Map::GetCreature matches on all
                    // three, so a counter-only payload can never be resolved on the far side
                    // — every RTS attack order died as "creature guid N not found on map".
                    snprintf(json, sizeof(json),
                        "{\"type\":\"ATTACK_TARGET\",\"payload\":{\"guid\":%u,\"entry\":%u}}",
                        targetGuid.GetCounter(), targetGuid.GetEntry());
                    ai->SuiInjectCommandLine(json);
                }
                break;
            case ORDER_MOVE_QUEUE:
            {
                float fx, fy;
                fannedDest(x, y, fx, fy);
                ai->SuiQueueWaypoint(fx, fy, z);
                break;
            }
            case ORDER_FOLLOW:
            {
                // Chain to ANY group member (owner 2026-09-03: "chain 2 players and 2
                // others — not just main to main"). The anchor lives on the AI
                // (FindEscortBoss honours it first: a real player anchors at their driven
                // body, a bot at itself); an empty/unknown target clears back to the group
                // rules. Following IS linking: an unlink or a world hold is lifted. The
                // bridge escort override still names a REAL target so the brain agrees.
                Player* followTarget = targetGuid.IsEmpty() ? nullptr
                    : sObjectMgr.GetPlayer(targetGuid);
                bool const validTarget = followTarget && followTarget != pMember &&
                    followTarget->IsInWorld() && followTarget->GetGroup() == pMember->GetGroup();
                ai->m_suiChainAnchor = validTarget ? followTarget->GetObjectGuid() : ObjectGuid();
                ai->m_suiUnlinked = false;
                ai->m_suiLandedHold = false;
                bool const realTarget = validTarget && followTarget->GetSession() &&
                    !followTarget->GetSession()->GetBot();
                snprintf(json, sizeof(json),
                    "{\"type\":\"SET_ESCORT\",\"payload\":{\"player_name\":\"%s\"}}",
                    realTarget ? followTarget->GetName() : "");
                ai->SuiInjectCommandLine(json);
                if (Group* changed = pMember->GetGroup()) changedChainGroups.insert(changed);
                break;
            }
            case ORDER_LINK:
                // Divinity-style chain toggle. Linked members keep formation on the
                // driven character; an unlinked member stands its ground from here.
                ai->m_suiUnlinked = x < 0.5f;
                if (ai->m_suiUnlinked)
                {
                    ai->StopMoving();
                    pMember->GetMotionMaster()->Clear(false, true);
                    pMember->GetMotionMaster()->MoveIdle();
                }
                else
                    // An explicit re-link is the human saying "come": it also lifts a
                    // world hold (the catch-up rule then walks or ports it over).
                    ai->m_suiLandedHold = false;
                if (Group* changed = pMember->GetGroup()) changedChainGroups.insert(changed);
                break;
            case ORDER_PATROL:
            {
                // Convert the queued chain into a loop. The coordinate in the
                // packet joins the route as its final point (a bare patrol
                // click with no chain gives a two-point there-and-back).
                float fx, fy;
                fannedDest(x, y, fx, fy);
                ai->SuiQueueWaypoint(fx, fy, z);
                ai->m_suiPatrolLoop = true;
                break;
            }
            case ORDER_STOP:
                ai->SuiClearWaypoints();
                ai->StopMoving();
                pMember->AttackStop();
                ai->m_currentTask.type = TASK_IDLE;
                pMember->GetMotionMaster()->MoveIdle();
                break;
            case ORDER_FORMATION_LINE:
            case ORDER_FORMATION_CIRCLE:
                // Cross-map subjects cannot join this anchor's geometry.
                if (pMember->GetMapId() == player->GetMapId() &&
                    pMember->GetInstanceId() == player->GetInstanceId())
                    formationSubjects.emplace_back(pMember, ai);
                break;
            case ORDER_SHEATH:
            {
                SheathState const want =
                    x >= 0.5f ? SHEATH_STATE_MELEE : SHEATH_STATE_UNARMED;
                ai->m_suiSheathOverride = int8(want);
                pMember->SetSheath(want);
                break;
            }
            case ORDER_CONSCRIPT:
                Conscript(player, pMember, ai);
                break;
            case ORDER_DISMISS:
                Dismiss(pMember, ai);
                break;
            case ORDER_MANUAL:
                // [SUI] Manual primary (owner 2026-09-01): "primary should always be user
                // controlled". Autonomy off from the next tick; a fight the AI already began
                // is left to the engine's auto-attack and the commander's own keys.
                ai->m_suiManual = true;
                break;
            case ORDER_AUTO:
                ai->m_suiManual = false;
                break;
            case ORDER_PREPARE:
                SuiRaidPrepare::Begin(player, pMember, ai, uint32(std::max(0.f, x)));
                break;
            case ORDER_SUPPLY:
                SuiRaidSupply::Supply(player, pMember, uint32(std::max(0.f, x)));
                break;
            default:
                break;
        }
    };

    if (!subjects.empty())
        for (ObjectGuid guid : subjects)
            orderBot(sObjectMgr.GetPlayer(guid));
    else if (group)
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            orderBot(itr->getSource());
    else
        orderBot(player);   // empty subject list solo = the own character
    if (orderType == ORDER_SUPPLY)
        SuiRaidSupply::Flush(player); // one receipt per supply order

    // One roster/facts publication per affected group, not per ordered member.
    // Bulk raid link/follow orders otherwise enqueue N full-raid snapshots and
    // can disconnect the commander with a full socket send queue.
    for (Group* changed : changedChainGroups)
        BroadcastRoster(changed);

    if (!formationSubjects.empty())
        DispatchFormation(player, formationSubjects,
            orderType == ORDER_FORMATION_CIRCLE, x, y, z);
}

// ── Hooks ────────────────────────────────────────────────────────────────────

void OnPlayerRemovedFromGroup(Player* player)
{
    if (!player)
        return;
    SuiCompanion::OnPlayerRemovedFromGroup(player);   // out of the owner's party = dismissed
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_GROUP);
    else if (player->GetSession() && !player->GetSession()->GetSuiControlledGuid().IsEmpty())
        ForceRelease(player->GetSession(), RELEASED_GROUP);
}

void OnPlayerTeleport(Player* player, bool farTeleport)
{
    if (!player)
        return;
    // A tactical field has a fixed body-sampled center.  Near teleports move
    // that body just as surely as far teleports, so either kind must thaw an
    // owned/anchored lock before normal possession teleport handling resumes.
    SuiTacticalFreeze::ReleaseForPlayer(player,
        SuiTacticalFreeze::FREEZE_RELEASED_MAP_CHANGE);
    if (Player* possessor = GetPossessor(player))
    {
        // The driven bot's near teleport (area-trigger portal, script, GM) rides the
        // mirrored MSG_MOVE_TELEPORT_ACK: the possessor's client snaps its controller and
        // acks for the bot (PlayerBotAI stands down its own ack while possessed).
        if (!farTeleport)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s near-teleports while driven by %s: possession kept",
                player->GetName(), possessor->GetName());
            return;
        }
        ForceRelease(possessor->GetSession(), RELEASED_TELEPORT);
    }
    else if (player->GetSession() && !player->GetSession()->GetSuiControlledGuid().IsEmpty())
    {
        // The MAIN near-teleports while its human drives a bot: that is the chain's
        // catch-up after a port of the driven body (POSSESS_LAW 4.3). The main's client
        // adopts it on the streamed entity and acks (HandleMoveTeleportAck accepts the
        // session player's own ack while the mover is the bot). Far teleports still break.
        if (!farTeleport)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s (driving) near-teleports: possession kept", player->GetName());
            return;
        }
        ForceRelease(player->GetSession(), RELEASED_TELEPORT);
    }
}

void OnPlayerDeath(Player* player)
{
    SuiTacticalFreeze::ReleaseForPlayer(player,
        SuiTacticalFreeze::FREEZE_RELEASED_DEATH);
    // Only the possessed bot's death breaks possession; the possessor's own
    // character dying under AI is surfaced client-side, not force-released.
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_DEATH);
}

void OnLogout(WorldSession* session)
{
    SuiTacticalFreeze::ReleaseOwnedBy(session,
        SuiTacticalFreeze::FREEZE_RELEASED_LOGOUT);
    // Session was possessing someone → clean release.
    DoRelease(session, RELEASED_LOGOUT, true);
    if (Player* player = session->GetPlayer())
    {
        SuiTacticalFreeze::ReleaseForPlayer(player,
            SuiTacticalFreeze::FREEZE_RELEASED_LOGOUT);
        // The commander leaves: the whole army musters out and the brain
        // resumes questing everyone in place. Group state is session-local on
        // the client, so logout is the release edge the server must own.
        DismissConscriptsOf(player);
        // Freecam logout: the unattended AI is owned here, not by a bot entry —
        // reclaim it before Player teardown.
        DetachUnattendedAI(player);
        RemoveFreecamEye(player);
        // Session's player IS a possessed bot (bot despawn path) → release its human.
        if (Player* possessor = GetPossessor(player))
            ForceRelease(possessor->GetSession(), RELEASED_LOGOUT);
    }
}

// ── Roster ───────────────────────────────────────────────────────────────────

void SendRoster(Player* realPlayer)
{
    WorldSession* session = realPlayer->GetSession();
    if (!session || session->GetBot() || !session->IsSuiCapable())
        return;

    WorldPacket data(SMSG_SUI_CONTROL_ROSTER, 1 + 18 * MAX_RAID_SIZE);
    Group* group = realPlayer->GetGroup();
    if (!group)
    {
        data << uint8(0);
        session->SendPacket(&data);
        return;
    }

    size_t countPos = data.wpos();
    data << uint8(0);
    uint8 count = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld())
            continue;
        uint8 flags = 0;
        if (member->GetSession() && member->GetSession()->GetBot() && BotAiOf(member) &&
            SuiCompanion::MayCommand(realPlayer, member))
            flags |= ROSTER_CONTROLLABLE;
        if (SuiCompanion::IsOwnedBy(realPlayer, member))
            flags |= ROSTER_COMPANION;
        if (IsSuiPossessed(member))
            flags |= ROSTER_POSSESSED;
        uint8 chain = CHAIN_LINKED;
        ObjectGuid anchor;
        if (AiBotAI* memberAi = dynamic_cast<AiBotAI*>(member->AI()))
        {
            if (memberAi->IsSuiConscripted())
                flags |= ROSTER_CONSCRIPTED;
            // Chain truth (owner 2026-09-03): the human's unlink wins over a world hold,
            // and the anchor is whoever the formation actually keys on right now.
            if (memberAi->m_suiUnlinked)
                chain = CHAIN_UNLINKED;
            else if (memberAi->m_suiLandedHold)
                chain = CHAIN_WORLD_HOLD;
            if (Player* boss = memberAi->FindEscortBoss())
                anchor = boss->GetObjectGuid();
        }
        data << uint64(member->GetObjectGuid().GetRawValue());
        data << flags;
        data << chain;                                    // row v2 (2026-09-03): 18 bytes
        data << uint64(anchor.GetRawValue());
        ++count;
    }
    data.put<uint8>(countPos, count);
    session->SendPacket(&data);
}

void NotifyChainChanged(Player* member)
{
    if (member && member->IsInWorld())
        BroadcastRoster(member->GetGroup());
}

void BroadcastRoster(Group* group)
{
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (member && member->GetSession() && !member->GetSession()->GetBot())
        {
            SendRoster(member);
            // Party = full facts: the same roster edge re-pushes every party
            // AiBot's bags + known spells to this SUI human (no possession).
            PushMemberFactsTo(member);
            // ...and, since PLAN_20 P1, every party member's quest log.
            PushMemberQuestsTo(member);
        }
    }
}

} // namespace SuiPossess

// ── Owner-data mirror (M3) + inventory/talent snapshot (M4) ──────────────────

namespace SuiPossess
{

static void AppendSnapshotItem(WorldPacket& data, uint8 bag, uint8 slot, Item* item)
{
    // The client queries these templates the moment the snapshot lands, and the
    // anti-datamining gate (HandleItemQuerySingleOpcode: ItemPrototype::Discovered)
    // answers "no such item" for anything neither the startup scan nor a runtime
    // Item::Create has marked — fabricated bots' gear can be exactly that after a
    // restart. The client caches the refusal permanently: nameless icon-less bags
    // with working stack counts. An item in a possessed party member's bags is
    // discovered by any honest reading of the rule.
    if (ItemPrototype const* proto = item->GetProto())
        proto->Discovered = true;
    data << uint8(bag);
    data << uint8(slot);
    data << uint64(item->GetObjectGuid().GetRawValue());
    data << uint32(item->GetEntry());
    data << uint32(item->GetCount());
    // Quivers and ammo pouches (ITEM_CLASS_QUIVER) are Bag objects too: the object
    // type is decided by INVTYPE_BAG (NewItemOrBag), which is exactly what IsBag()
    // tests. The old Class == ITEM_CLASS_CONTAINER test typed a possessed hunter's
    // quiver as a plain item, so the client could neither open it nor list its
    // arrows (MSUIClient issue #40).
    uint8 bagSlots = 0;
    if (item->IsBag())
        bagSlots = (uint8)((Bag*)item)->GetBagSize();
    data << bagSlots;
}

/// Read-only bags + talent points for the possessed bot, pushed once per grant.
/// bag 255 = character-held (equipment 0-18, bag slots 19-22, backpack 23-38,
/// keyring 81+ — one contiguous slot numbering); bag 19-22 = inside that
/// equipped bag. Bag rows precede their contents by construction.
static void SendSnapshot(WorldSession* to, Player* bot)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_SNAPSHOT, 4096);
    data << uint64(bot->GetObjectGuid().GetRawValue());
    data << uint32(bot->GetUInt32Value(PLAYER_CHARACTER_POINTS1));
    data << uint32(bot->GetMoney());
    size_t countPos = data.wpos();
    data << uint16(0);
    uint16 count = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    for (uint8 i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    // Snapshot v4: the bank. PLAYER_BANK_SLOT_1.. / PLAYER_BANK_BAG_SLOT_1.. are
    // owner-only fields, so the commander's BankFrame drew a driven bot's bank
    // empty. Same contiguous slot numbering (bank items 39-62, bank bags 63-68);
    // bank-bag contents follow below with bag = that bank bag slot. Rows, not a
    // trailer, so any client that files bag-255 rows by slot gets them for free.
    for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_BAG_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    for (uint8 bagSlot = BANK_SLOT_BAG_START; bagSlot < BANK_SLOT_BAG_END; ++bagSlot)
        if (Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            if (bagItem->IsBag()) // containers AND quivers/ammo pouches (issue #40)
                {
                    Bag* pBag = (Bag*)bagItem;
                    for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                        if (Item* item = pBag->GetItemByPos((uint8)j))
                        {
                            AppendSnapshotItem(data, bagSlot, (uint8)j, item);
                            ++count;
                        }
                }
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            if (bagItem->IsBag()) // containers AND quivers/ammo pouches (issue #40)
                {
                    Bag* pBag = (Bag*)bagItem;
                    for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                        if (Item* item = pBag->GetItemByPos((uint8)j))
                        {
                            AppendSnapshotItem(data, bagSlot, (uint8)j, item);
                            ++count;
                        }
                }

    data.put<uint16>(countPos, count);

    // ── Snapshot v2: the paper-doll stat block. These UNIT_FIELD values are
    // owner-only on the vanilla wire (never streamed for another player), so a
    // possessed bot's character sheet rendered all zeros without them. Raw field
    // values, mirrored verbatim into the same fields client-side; the client
    // reads the block only when present, so the growth is compatible both ways.
    for (int i = 0; i < 5; ++i)
        data << bot->GetUInt32Value(UNIT_FIELD_STAT0 + i);
    for (int i = 0; i < 7; ++i)
        data << bot->GetUInt32Value(UNIT_FIELD_RESISTANCES + i);
    data << bot->GetUInt32Value(UNIT_FIELD_ATTACK_POWER);
    data << bot->GetUInt32Value(UNIT_FIELD_ATTACK_POWER_MODS);
    data << bot->GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER);
    data << bot->GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS);
    data << bot->GetUInt32Value(UNIT_FIELD_BASEATTACKTIME);
    data << bot->GetUInt32Value(UNIT_FIELD_BASEATTACKTIME + 1);   // offhand
    data << bot->GetUInt32Value(UNIT_FIELD_RANGEDATTACKTIME);
    data << bot->GetFloatValue(UNIT_FIELD_MINDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE);
    data << bot->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE);

    // ── Snapshot v3: the buyback shelf. The twelve PLAYER_VENDOR_BUYBACK_SLOT
    // guids, their prices/timestamps and the Item objects they point at are all
    // owner-only, so the commander's Merchant Buyback tab rendered empty for a
    // driven bot while its bags worked. u8 count, then per occupied slot:
    // u8 index (0-11), u64 itemGuid, u32 entry, u32 stack, u32 price,
    // u32 timestamp. Appended after the v2 stat block; older clients stop
    // reading before this and are unharmed.
    size_t buybackCountPos = data.wpos();
    data << uint8(0);
    uint8 buybackCount = 0;
    for (uint32 i = 0; i < BUYBACK_SLOT_END - BUYBACK_SLOT_START; ++i)
    {
        Item* item = bot->GetItemFromBuyBackSlot(BUYBACK_SLOT_START + i);
        if (!item)
            continue;
        // Same anti-datamining waiver as AppendSnapshotItem: an item on a party
        // member's buyback shelf is discovered by any honest reading of the rule.
        if (ItemPrototype const* proto = item->GetProto())
            proto->Discovered = true;
        data << uint8(i);
        data << uint64(item->GetObjectGuid().GetRawValue());
        data << uint32(item->GetEntry());
        data << uint32(item->GetCount());
        data << uint32(bot->GetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + i));
        data << uint32(bot->GetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1 + i));
        ++buybackCount;
    }
    data.put<uint8>(buybackCountPos, buybackCount);

    to->SendPacket(&data);
}

// ── Party member facts (owner decision 2026-08-25) ───────────────────────────
// Party = full facts, faction = orders: every party/raid AiBot member's bags
// (SMSG_SUI_SNAPSHOT, byte-identical to the possession wire) and known spells
// (SMSG_SUI_MEMBER_SPELLS) go to the party's real SUI clients WITHOUT
// possession. Pushed on every roster edge (BroadcastRoster) and pulled by
// CMSG_SUI_MEMBER_FACTS when a client panel opens. Live cooldowns/casts stay
// possession-only (the proxy wire); inventory dirty-hooks are deliberately
// deferred — the client stamps snapshot age and re-pulls on panel open.

/// The party/raid line itself: the subject must be an AiBot in the SAME group
/// as the requester. Faction-control authority is deliberately NOT sufficient.
static bool IsMemberFactsSubject(Player* requester, Player* member)
{
    if (!requester || !member || requester == member)
        return false;
    Group* group = requester->GetGroup();
    if (!group || member->GetGroup() != group)
        return false;
    return member->GetSession() && member->GetSession()->GetBot() && BotAiOf(member) &&
        SuiCompanion::MayCommand(requester, member);   // [COMPANION] owner-only alts
}

/// u64 guid, u16 count, u32 spellIds[] — the active spellbook under the same
/// filter SendInitialSpells applies, minus cooldowns (possession-only).
static void SendMemberSpells(WorldSession* to, Player* bot)
{
    if (!to->IsSuiCapable())
        return;
    PlayerSpellMap const& spells = bot->GetSpellMap();
    WorldPacket data(SMSG_SUI_MEMBER_SPELLS, 8 + 2 + 4 * spells.size());
    data << uint64(bot->GetObjectGuid().GetRawValue());
    size_t countPos = data.wpos();
    data << uint16(0);
    uint16 count = 0;
    for (auto const& spell : spells)
    {
        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;
        if (!spell.second.active || spell.second.disabled)
            continue;
        data << uint32(spell.first);
        ++count;
    }
    data.put<uint16>(countPos, count);
    to->SendPacket(&data);
}

static void SendMemberFacts(WorldSession* to, Player* bot)
{
    SendSnapshot(to, bot);
    SendMemberSpells(to, bot);
}

// [SUI] Re-push the driven bot's owner-only facts (bags/coinage/stats + spells) to the commander
// after the commander changed the bot's bags via ItemHandler (swap/split/equip through GetSuiActor).
// The bot has no client session, so its private item fields never stream to the commander otherwise
// — without this the rearrange/equip would silently not show. Public: ItemHandler calls it.
void ResnapshotControlled(WorldSession* session)
{
    if (Player* bot = GetControlledBot(session))
        SendMemberFacts(session, bot);
}

/// Fan the whole group's AiBot facts out to one real SUI member.
static void PushMemberFactsTo(Player* realPlayer)
{
    WorldSession* session = realPlayer ? realPlayer->GetSession() : nullptr;
    if (!session || session->GetBot() || !session->IsSuiCapable())
        return;
    Group* group = realPlayer->GetGroup();
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (member && member->IsInWorld() && IsMemberFactsSubject(realPlayer, member))
            SendMemberFacts(session, member);
    }
}

void HandleMemberFacts(WorldSession* session, std::vector<ObjectGuid> const& subjects)
{
    // Only MSUIClient speaks this opcode — same latch as HandleRequest.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot())
        return;
    // Rate-limited separately from movement: one pull per second is plenty for
    // panel-open refreshes; the roster-edge pushes cover everything else.
    uint32 nowMs = WorldTimer::getMSTime();
    if (session->GetSuiMemberFactsPullMs() &&
        WorldTimer::getMSTimeDiff(session->GetSuiMemberFactsPullMs(), nowMs) < 1000)
        return;
    session->SetSuiMemberFactsPullMs(nowMs);

    if (subjects.empty())
    {
        PushMemberFactsTo(requester);
        return;
    }
    for (ObjectGuid guid : subjects)
    {
        Player* member = sObjectMgr.GetPlayer(guid);
        if (member && member->IsInWorld() && IsMemberFactsSubject(requester, member))
            SendMemberFacts(session, member);
    }
}

// -- Party quest facts (PLAN_20 P1) -------------------------------------------
// Owner decision 2026-08-25: real per-character quest logs, merged in the view.
// The party line is unchanged (IsMemberFactsSubject) with ONE widening: the
// requester's own character is a legal subject, because a client cannot see its
// own quests held past the twenty update-field slots any other way.

static constexpr uint8 QUEST_FACTS_INCLUDE_TIMERS = 0x01;
static constexpr uint8 QUEST_FACTS_KNOWN_FLAGS = QUEST_FACTS_INCLUDE_TIMERS;

/// The update-field log slot for a quest, or MAX_QUEST_LOG_SIZE when the quest
/// is held without one. Player::FindQuestSlot is private and this file is not a
/// friend, so the identical scan runs through the public field accessor. Today
/// every held quest has a slot; the sentinel is on the wire from day one so the
/// client needs no second packet shape when PLAN_20 P2 lifts the cap.
static uint16 QuestLogSlotOf(Player* player, uint32 questId)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (player->GetUInt32Value(
                PLAYER_QUEST_LOG_1_1 + slot * MAX_QUEST_OFFSET + QUEST_ID_OFFSET) == questId)
            return slot;
    return MAX_QUEST_LOG_SIZE;
}

/// The quest line itself. Self is allowed (own overflow quests); everyone else
/// must clear the same party-line predicate the bag/spell facts use, so faction
/// authority stays insufficient here too.
static bool IsQuestFactsSubject(Player* requester, Player* member)
{
    return requester && member &&
        (requester == member || IsMemberFactsSubject(requester, member));
}

/// u64 subject, u8 flags, u16 heldCap, u16 count, then fixed-stride entries:
/// the original 19 bytes (u32 quest, u8 status, u8 entryFlags, u8 slot,
/// u8 objectives[4], u16 items[4]), plus an optional trailing u32 absolute Unix
/// deadline when the requester opted into timer extension 0x01.
///
/// The counters are the SERVER-side truth (m_creatureOrGOcount / m_itemcount),
/// never the packed update-field mirror: a party member's slots were never
/// streamed to this client, and a quest held past the twenty slots has no
/// mirror at all. Item progress especially -- vanilla reads the player's own
/// bags for that, which is structurally unavailable for anyone else.
static void SendMemberQuests(WorldSession* to, Player* member,
                             std::set<uint32> const* unionHeld = nullptr)
{
    if (!to->IsSuiCapable())
        return;
    QuestStatusMap& quests = member->GetQuestStatusMap();
    bool const includeTimers = (to->GetSuiQuestFactsFlags() & QUEST_FACTS_INCLUDE_TIMERS) != 0;
    uint32 const entryBytes = includeTimers ? 23 : 19;
    WorldPacket data(SMSG_SUI_QUEST_LOG, 8 + 1 + 2 + 2 + entryBytes * quests.size());
    data << uint64(member->GetObjectGuid().GetRawValue());
    data << uint8(includeTimers ? QUEST_FACTS_INCLUDE_TIMERS : 0);
    // How many quests this character may HOLD. Not knowable client-side once it
    // stops being the update-field slot count, and the quest log prints it.
    data << uint16(sWorld.getConfig(CONFIG_UINT32_MAX_QUEST_HELD));
    size_t countPos = data.wpos();
    data << uint16(0);
    uint16 count = 0;
    for (auto const& pair : quests)
    {
        QuestStatusData const& status = pair.second;
        // ONE predicate for 'does this character hold this quest', shared with
        // _LoadQuestStatus and every credit scan. This used to be a THIRD, subtly
        // different copy that dropped every m_rewarded quest unconditionally --
        // and AddQuest never clears m_rewarded on re-accept, so a re-accepted
        // REPEATABLE quest earned credit server-side while being structurally
        // invisible to the client. VMaNGOS also leaves a turned-in quest at
        // QUEST_STATUS_COMPLETE forever, which is why m_status alone is not the
        // test -- IsHeldQuestStatus owns both halves of that rule now.
        if (!Player::IsHeldQuestStatus(pair.first, status))
            continue;

        uint16 slot = QuestLogSlotOf(member, pair.first);
        uint8 entryFlags = 0;
        if (status.m_status == QUEST_STATUS_COMPLETE)
            entryFlags |= 0x01;
        if (status.m_status == QUEST_STATUS_FAILED)
            entryFlags |= 0x02;
        if (slot >= MAX_QUEST_LOG_SIZE)
            entryFlags |= 0x04;             // held without a log slot

        data << uint32(pair.first);
        data << uint8(status.m_status);
        data << uint8(entryFlags);
        data << uint8(slot >= MAX_QUEST_LOG_SIZE ? 255 : uint8(slot));
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            data << uint8(std::min<uint32>(status.m_creatureOrGOcount[i], 255));
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            data << uint16(std::min<uint32>(status.m_itemcount[i], 65535));
        if (includeTimers)
            data << uint32(status.m_timer
                ? sWorld.GetGameTime() + status.m_timer / IN_MILLISECONDS : 0);
        ++count;
    }
    // Rewarded (already turned-in) quests that ANOTHER party member still holds:
    // report them so the party log can show "completed" for the finisher, without
    // dumping this character's whole quest history. IsHeldQuestStatus excludes a
    // plain rewarded quest, so this is a separate, union-gated pass.
    if (unionHeld)
    {
        for (auto const& pair : quests)
        {
            QuestStatusData const& status = pair.second;
            if (Player::IsHeldQuestStatus(pair.first, status))
                continue;                       // a held/re-held quest already went out above
            if (!status.m_rewarded)
                continue;
            if (unionHeld->find(pair.first) == unionHeld->end())
                continue;                       // nobody in the party still holds it
            data << uint32(pair.first);
            data << uint8(QUEST_STATUS_COMPLETE);
            data << uint8(0x01 | 0x08);         // complete + rewarded
            data << uint8(255);                 // no update-field log slot
            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                data << uint8(0);
            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                data << uint16(0);
            if (includeTimers)
                data << uint32(0);
            ++count;
        }
    }
    data.put<uint16>(countPos, count);
    to->SendPacket(&data);
}

// The union of quest ids any group member (including the requester) currently
// HOLDS. Rewarded-quest reporting is gated on this so the wire stays bounded to
// the quests the party log already shows, never a character's full history.
static void BuildGroupHeldQuestUnion(Player* requester, std::set<uint32>& out)
{
    auto addHeld = [&out](Player* p)
    {
        if (!p)
            return;
        for (auto const& pair : p->GetQuestStatusMap())
            if (Player::IsHeldQuestStatus(pair.first, pair.second))
                out.insert(pair.first);
    };
    if (!requester)
        return;
    addHeld(requester);
    if (Group* group = requester->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            addHeld(itr->getSource());
}

/// Fan the whole group's quest logs out to one real SUI member, including that
/// member's own log.
static void PushMemberQuestsTo(Player* realPlayer)
{
    WorldSession* session = realPlayer ? realPlayer->GetSession() : nullptr;
    if (!session || session->GetBot() || !session->IsSuiCapable())
        return;
    std::set<uint32> unionHeld;
    BuildGroupHeldQuestUnion(realPlayer, unionHeld);
    SendMemberQuests(session, realPlayer, &unionHeld);
    Group* group = realPlayer->GetGroup();
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (member && member->IsInWorld() && IsMemberFactsSubject(realPlayer, member))
            SendMemberQuests(session, member, &unionHeld);
    }
}

void HandleQuestFacts(WorldSession* session, uint8 flags,
                      std::vector<ObjectGuid> const& subjects)
{
    // Only MSUIClient speaks this opcode -- same latch as HandleRequest.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot() || (flags & ~QUEST_FACTS_KNOWN_FLAGS) != 0)
        return;
    // Remember the opt-in so roster-edge and party-act pushes keep the negotiated
    // entry shape instead of briefly replacing timed facts with legacy rows.
    session->SetSuiQuestFactsFlags(flags);
    uint32 nowMs = WorldTimer::getMSTime();
    if (session->GetSuiQuestFactsPullMs() &&
        WorldTimer::getMSTimeDiff(session->GetSuiQuestFactsPullMs(), nowMs) < 1000)
        return;
    session->SetSuiQuestFactsPullMs(nowMs);

    if (subjects.empty())
    {
        PushMemberQuestsTo(requester);
        return;
    }
    std::set<uint32> unionHeld;
    BuildGroupHeldQuestUnion(requester, unionHeld);
    for (ObjectGuid guid : subjects)
    {
        Player* member = sObjectMgr.GetPlayer(guid);
        if (member && member->IsInWorld() && IsQuestFactsSubject(requester, member))
            SendMemberQuests(session, member, &unionHeld);
    }
}

// -- Party lead claim (PLAN_20 P4a) -------------------------------------------
// BridgeHandleFormGroup has bots create their OWN groups (Group::Create with the
// bot as leader) and add members to them, so "an AiBot holds the lead" is a state
// the fleet produces by design, not an accident. Vanilla then offers no way out:
// HandleGroupSetLeaderOpcode requires group->IsLeader(GetPlayer()) before it will
// promote anyone, and refuses player == GetPlayer() outright -- so a commander in
// a bot-led group can neither promote themselves nor rearrange the party.
//
// This is the way back, and it is deliberately narrow. Leadership is taken ONLY
// from an AiBot in the requester's own group; from a real player it is refused,
// because a verb that seizes lead from a human is a griefing verb whatever the
// intent behind it. Vanilla's handler is left untouched.
enum SuiPartyLeadResult
{
    SUI_LEAD_OK                  = 0,
    SUI_LEAD_NOT_IN_GROUP        = 1,
    SUI_LEAD_ALREADY_LEADER      = 2,
    SUI_LEAD_LEADER_IS_PLAYER    = 3,
    SUI_LEAD_SUBJECT_NOT_IN_GROUP = 4,
    SUI_LEAD_SUBJECT_NOT_SELF    = 5,
    SUI_LEAD_NO_SUBJECT          = 6,
    SUI_LEAD_BAD_ACTION          = 7,
};

static void SendPartyLeadResult(WorldSession* to, uint8 action, ObjectGuid subject, uint8 result)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_PARTY_LEAD_RESULT, 10);
    data << uint8(action);
    data << uint64(subject.GetRawValue());
    data << uint8(result);
    to->SendPacket(&data);
}

void HandlePartyLead(WorldSession* session, uint8 action, ObjectGuid subject)
{
    // Only MSUIClient speaks this opcode -- same latch as HandleRequest.
    session->SetSuiCapable(true);
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_BAD_ACTION);
        return;
    }
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot())
        return;

    if (action != 1)
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_BAD_ACTION);
        return;
    }
    if (subject.IsEmpty())
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_NO_SUBJECT);
        return;
    }
    // v1 claims the lead for yourself only. Promoting one bot over another is a
    // separate decision with its own failure modes; it is not smuggled in here.
    if (subject != requester->GetObjectGuid())
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_SUBJECT_NOT_SELF);
        return;
    }

    Group* group = requester->GetGroup();
    if (!group)
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_NOT_IN_GROUP);
        return;
    }
    if (group->IsLeader(requester->GetObjectGuid()))
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_ALREADY_LEADER);
        return;
    }

    // The one rule that matters. IsMemberFactsSubject is the established test for
    // "an AiBot in my group I am allowed to act on" -- reused rather than
    // restated, because a second copy of an authorization rule is how the two
    // quietly stop agreeing.
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    if (!leader || !IsMemberFactsSubject(requester, leader))
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_LEADER_IS_PLAYER);
        return;
    }
    if (leader->IsSuiTacticallyFrozen())
    {
        SendPartyLeadResult(session, action, subject, SUI_LEAD_BAD_ACTION);
        return;
    }

    group->ChangeLeader(requester->GetObjectGuid());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[SUI-LEAD] %s took group leadership from bot %s",
        requester->GetName(), leader->GetName());
    SendPartyLeadResult(session, action, subject, SUI_LEAD_OK);
}

// -- Party questgiver status (PLAN_20 P5) -------------------------------------
// Owner decision 5: keep the exact vanilla !/? art, font and yellow, and hang a
// parenthesised numeral over it -- (4) when four of your group can take what
// this NPC offers.
//
// This lives on the server because the client cannot compute it and must not
// guess it. Vanilla SMSG_QUESTGIVER_STATUS answers for the asking session and
// nobody else; eligibility turns on level, prerequisites, race, class and
// exclusive groups the client never receives for a companion; and the client is
// never told which quests an NPC offers or ends in the first place. A wrong
// number over an NPC's head is worse than no number.
//
// The verdict is produced by exactly the path CMSG_QUESTGIVER_STATUS_QUERY uses
// -- the script hook first, the core rule as the fallback, and the same
// hostility gate -- so a scripted questgiver answers identically for a
// companion and for you.
static uint8 GiverStatusFor(WorldSession* session, Player* member, Object* questgiver)
{
    uint32 dialogStatus = DIALOG_STATUS_NONE;
    switch (questgiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            Creature* creature = static_cast<Creature*>(questgiver);
            if (creature->IsHostileTo(member))   // not show quest status to enemies
                return DIALOG_STATUS_NONE;
            dialogStatus = sScriptMgr.GetDialogStatus(member, creature);
            if (dialogStatus > 6)
                dialogStatus = session->GetDialogStatus(member, creature, DIALOG_STATUS_NONE);
            break;
        }
        case TYPEID_GAMEOBJECT:
        {
            GameObject* go = static_cast<GameObject*>(questgiver);
            dialogStatus = sScriptMgr.GetDialogStatus(member, go);
            if (dialogStatus > 6)
                dialogStatus = session->GetDialogStatus(member, go, DIALOG_STATUS_NONE);
            break;
        }
        default:
            return DIALOG_STATUS_NONE;
    }
    return dialogStatus > 7 ? uint8(DIALOG_STATUS_NONE) : uint8(dialogStatus);
}

void HandleGiverStatus(WorldSession* session,
    std::vector<ObjectGuid> const& givers)
{
    // Only MSUIClient speaks this opcode -- same latch as HandleRequest.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot() || givers.empty())
        return;
    uint32 nowMs = WorldTimer::getMSTime();
    if (session->GetSuiGiverStatusPullMs() &&
        WorldTimer::getMSTimeDiff(session->GetSuiGiverStatusPullMs(), nowMs) < 1000)
        return;
    session->SetSuiGiverStatusPullMs(nowMs);

    // Resolve the countable members once, not once per questgiver.
    std::vector<Player*> members;
    if (Group* group = requester->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != requester && member->IsInWorld() &&
                IsMemberFactsSubject(requester, member))
                members.push_back(member);
        }
    }

    std::vector<std::pair<std::pair<ObjectGuid, ObjectGuid>, uint8>> entries;
    entries.reserve(givers.size() * (members.size() + 1));

    for (ObjectGuid giverGuid : givers)
    {
        // Resolved against the REQUESTER: they are the one looking at the marker,
        // and a companion across the zone has no view of this NPC to resolve from.
        Object* questgiver =
            requester->GetObjectByTypeMask(giverGuid, TYPEMASK_CREATURE_OR_GAMEOBJECT);
        if (!questgiver)
            continue;

        // The requester's own row is always emitted, even at NONE. It is what
        // tells the client "this giver was answered for" -- without it a giver
        // whose whole party went to NONE would simply be absent from the reply
        // and the client would keep showing the previous answer forever.
        entries.push_back({ { giverGuid, requester->GetObjectGuid() },
            GiverStatusFor(session, requester, questgiver) });

        for (Player* member : members)
        {
            uint8 status = GiverStatusFor(session, member, questgiver);
            if (status == DIALOG_STATUS_NONE)
                continue;                       // absent means zero; do not pay for it
            entries.push_back({ { giverGuid, member->GetObjectGuid() }, status });
        }
    }

    if (!session->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_GIVER_STATUS, 3 + 17 * entries.size());
    data << uint8(0);                           // flags, reserved
    data << uint16(entries.size());
    for (auto const& entry : entries)
    {
        data << uint64(entry.first.first.GetRawValue());
        data << uint64(entry.first.second.GetRawValue());
        data << uint8(entry.second);
    }
    session->SendPacket(&data);
}

// -- Giver quests + per-member eligibility (PLAN_20 Model B) -------------------
// The commander-view quest window needs, for one NPC, the quests it offers or ends
// and each member's verdict — none of which vanilla ever tells the client. The
// verdict order mirrors CanTakeQuest's own check order so the first failing reason
// is the one reported, exactly as the giver frame would have refused it.

static uint8 QuestEligibilityFor(Player* member, Quest const* q, bool ends)
{
    uint32 id = q->GetQuestId();
    QuestStatus st = member->GetQuestStatus(id);
    bool held = (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_COMPLETE);

    if (member->GetQuestRewardStatus(id) && !q->IsRepeatable())
        return SuiPossess::GIVER_QUEST_DONE;
    if (held)
    {
        // READY becomes a "Turn in" button AT THIS GIVER on the client, so it
        // may only be said by a giver that actually ENDS the quest. At a
        // start-only giver a held-complete quest is still just "on it" — its
        // turn-in point is somewhere else (owner report 2026-08-27: the modal
        // offered a turn-in at the quest's ACCEPT NPC).
        if (st == QUEST_STATUS_COMPLETE && ends)
            return SuiPossess::GIVER_QUEST_READY;
        return SuiPossess::GIVER_QUEST_ON_IT;
    }
    if (ends)
        return SuiPossess::GIVER_QUEST_CANT;   // ends it, but this member does not hold it

    // Giver STARTS it and the member does not hold it: can they take it, and if not,
    // which check is the one that refuses them?
    if (member->CanTakeQuest(q, false))
        return SuiPossess::GIVER_QUEST_CAN_TAKE;
    if (!member->SatisfyQuestLevel(q, false))
        return SuiPossess::GIVER_QUEST_LOW_LEVEL;
    if (!member->SatisfyQuestRace(q, false) || !member->SatisfyQuestClass(q, false))
        return SuiPossess::GIVER_QUEST_WRONG_RACE_CLASS;
    if (!member->SatisfyQuestPreviousQuest(q, false) ||
        !member->SatisfyQuestPrevChain(q, false) ||
        !member->SatisfyQuestBreadcrumbQuest(q, false) ||
        !member->SatisfyQuestDependentBreadcrumbQuests(q, false))
        return SuiPossess::GIVER_QUEST_NEEDS_PREREQ;
    if (!member->SatisfyQuestSkill(q, false) ||
        !member->SatisfyQuestReputation(q, false) ||
        !member->SatisfyQuestCondition(q, false))
        return SuiPossess::GIVER_QUEST_LOW_SKILL_REP;
    if (!member->SatisfyQuestLog(false))
        return SuiPossess::GIVER_QUEST_LOG_FULL;
    return SuiPossess::GIVER_QUEST_CANT;
}

void HandleGiverQuests(WorldSession* session, ObjectGuid giver)
{
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot() || giver.IsEmpty())
        return;

    // Resolved against the REQUESTER: they are the one looking at the NPC.
    Object* questgiver =
        requester->GetObjectByTypeMask(giver, TYPEMASK_CREATURE_OR_GAMEOBJECT);
    if (!questgiver)
        return;

    uint32 entry = questgiver->GetEntry();
    QuestRelationsMapBounds starts, ends;
    if (questgiver->GetTypeId() == TYPEID_UNIT)
    {
        starts = sObjectMgr.GetCreatureQuestRelationsMapBounds(entry);
        ends = sObjectMgr.GetCreatureQuestInvolvedRelationsMapBounds(entry);
    }
    else
    {
        starts = sObjectMgr.GetGOQuestRelationsMapBounds(entry);
        ends = sObjectMgr.GetGOQuestInvolvedRelationsMapBounds(entry);
    }

    // Members = self first, then every party AiBot we may answer for.
    std::vector<Player*> members;
    members.push_back(requester);
    if (Group* group = requester->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != requester && member->IsInWorld() &&
                IsMemberFactsSubject(requester, member))
                members.push_back(member);
        }

    // De-duplicated quest list with a relation flag (bit0 starts, bit1 ends).
    std::map<uint32, uint8> relById;
    for (QuestRelationsMap::const_iterator itr = starts.first; itr != starts.second; ++itr)
        relById[itr->second] |= 0x01;
    for (QuestRelationsMap::const_iterator itr = ends.first; itr != ends.second; ++itr)
        relById[itr->second] |= 0x02;

    // Owner 2026-08-27: present the quests in DO-ORDER — a chain's steps kept
    // together and in sequence — rather than raw quest-id order. The chain data
    // (PrevQuestId) never reaches the client, so the ordering is decided here:
    // walk each quest's PrevQuestId links up to its chain root, then sort by the
    // root's level (where the storyline begins), root id, and depth within the
    // chain. Standalone quests are one-step chains and interleave naturally.
    struct GiverQuestRow
    {
        uint32 questId;
        uint8 relation;
        uint32 rootLevel;
        uint32 rootId;
        uint32 depth;
    };
    std::vector<GiverQuestRow> rows;
    rows.reserve(relById.size());
    for (auto const& kv : relById)
    {
        Quest const* q = sObjectMgr.GetQuestTemplate(kv.first);
        if (!q || !q->IsActive())
            continue;
        uint32 rootId = kv.first;
        uint32 depth = 0;
        for (Quest const* step = q; step && depth < 32; ++depth)
        {
            int32 prevSigned = step->GetPrevQuestId();
            uint32 prev = prevSigned ? uint32(std::abs(prevSigned)) : 0;
            if (!prev || prev == rootId)
                break;                          // no earlier step, or a data cycle
            Quest const* prevQuest = sObjectMgr.GetQuestTemplate(prev);
            if (!prevQuest)
                break;
            rootId = prev;
            step = prevQuest;
        }
        Quest const* root = sObjectMgr.GetQuestTemplate(rootId);
        uint32 rootLevel = root ? root->GetQuestLevel() : q->GetQuestLevel();
        rows.push_back({ kv.first, kv.second, rootLevel, rootId, depth });
    }
    std::sort(rows.begin(), rows.end(),
        [](GiverQuestRow const& a, GiverQuestRow const& b)
    {
        if (a.rootLevel != b.rootLevel) return a.rootLevel < b.rootLevel;
        if (a.rootId != b.rootId) return a.rootId < b.rootId;
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.questId < b.questId;
    });

    // Body: per quest -> u32 questId, u8 relation, u8 memberCount,
    //       then per member -> u64 guid, u8 verdict.
    ByteBuffer body;
    uint16 questCount = 0;
    for (GiverQuestRow const& row : rows)
    {
        Quest const* q = sObjectMgr.GetQuestTemplate(row.questId);
        if (!q)
            continue;
        bool startsIt = (row.relation & 0x01) != 0;
        bool endsIt = (row.relation & 0x02) != 0;

        body << uint32(row.questId);
        body << uint8(row.relation);
        size_t countPos = body.wpos();
        body << uint8(0);
        uint8 memberCount = 0;
        for (Player* member : members)
        {
            QuestStatus st = member->GetQuestStatus(row.questId);
            bool holds = (st == QUEST_STATUS_INCOMPLETE || st == QUEST_STATUS_COMPLETE);
            uint8 verdict;
            if (endsIt && holds)
                verdict = QuestEligibilityFor(member, q, true);
            else if (startsIt)
                verdict = QuestEligibilityFor(member, q, false);
            else
                continue;   // a quest this giver only ENDS and the member does not hold
            body << uint64(member->GetObjectGuid().GetRawValue());
            body << uint8(verdict);
            ++memberCount;
        }
        body.put<uint8>(countPos, memberCount);
        ++questCount;
    }

    if (!session->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_GIVER_QUESTS, 1 + 8 + 2 + body.size());
    data << uint8(0);                       // flags, reserved
    data << uint64(giver.GetRawValue());
    data << uint16(questCount);
    if (body.size() > 0)
        data.append(body.contents(), body.size());
    session->SendPacket(&data);
}

// -- Party quest acts (PLAN_20 P3) --------------------------------------------
// Owner decision 2026-08-25: accept and turn in for the party in one gesture,
// with the reward CHOSEN PER BOT BY THE PLAYER. Every subject is authorized and
// answered individually -- a party act that collapsed five outcomes into one
// "failed" would be worse than no party act at all.

static void SendPartyQuestResult(WorldSession* to, uint8 action, uint32 questId,
    std::vector<std::pair<ObjectGuid, uint8>> const& outcomes)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_PARTY_QUEST_RESULT, 6 + 9 * outcomes.size());
    data << uint8(action);
    data << uint32(questId);
    data << uint8(outcomes.size());
    for (auto const& outcome : outcomes)
    {
        data << uint64(outcome.first.GetRawValue());
        data << uint8(outcome.second);
    }
    to->SendPacket(&data);
}

/// Is this subject close enough to be included in a party act?
///
/// The requester is TALKING to the giver, so they answer to the ordinary
/// interaction rule. Companions answer to QUEST_SHARE_DISTANCE measured from the
/// REQUESTER -- which is exactly vanilla's own rule for "this party member is
/// close enough to be shared with" (HandlePushQuestToParty). Using
/// INTERACTION_DISTANCE for everyone would be unusable: five bodies cannot all
/// stand within five yards of one NPC.
static bool IsPartyQuestInRange(Player* requester, Player* subject, Object* giver)
{
    if (!requester || !subject)
        return false;
    if (subject == requester)
        return giver && requester->CanInteractWithQuestGiver(giver);
    if (subject->GetMapId() != requester->GetMapId())
        return false;
    // The requester's CanInteractWithQuestGiver already covers alive, ghost,
    // taxi and lost-control. A companion used to get only the two positional
    // tests, which made a corpse a legal subject -- and RewardQuest would then
    // hand XP, money and items to a dead player, a state the game cannot
    // otherwise produce.
    if (!subject->IsAlive() || subject->IsTaxiFlying())
        return false;
    return requester->IsWithinDist(subject, QUEST_SHARE_DISTANCE, true, SizeFactor::None);
}

void HandlePartyQuest(WorldSession* session, uint8 action, uint32 questId,
    ObjectGuid npcGuid, std::vector<PartyQuestSubject> const& subjects)
{
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot() || subjects.empty())
        return;
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
    {
        std::vector<std::pair<ObjectGuid, uint8>> denied;
        denied.reserve(subjects.size());
        for (PartyQuestSubject const& subject : subjects)
            denied.push_back({ subject.guid, PARTY_QUEST_DENIED });
        SendPartyQuestResult(session, action, questId, denied);
        return;
    }

    Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);
    if (!pQuest)
        return;

    std::vector<std::pair<ObjectGuid, uint8>> outcomes;
    outcomes.reserve(subjects.size());

    // Abandon needs no giver at all; accept and turn-in must prove this object
    // really offers/ends the quest, exactly as the real handlers do.
    Object* pGiver = nullptr;
    if (action != 3)
    {
        pGiver = requester->GetObjectByTypeMask(npcGuid, TYPEMASK_CREATURE_OR_GAMEOBJECT);
        bool offers = pGiver &&
            (action == 1 ? pGiver->HasQuest(questId) : pGiver->HasInvolvedQuest(questId));
        if (!offers)
        {
            for (PartyQuestSubject const& subject : subjects)
                outcomes.push_back({ subject.guid, PARTY_QUEST_NO_QUEST });
            SendPartyQuestResult(session, action, questId, outcomes);
            return;
        }
    }

    std::vector<Player*> touched;

    for (PartyQuestSubject const& entry : subjects)
    {
        Player* subject = (entry.guid == requester->GetObjectGuid())
            ? requester : sObjectMgr.GetPlayer(entry.guid);
        if (!subject || !subject->IsInWorld() || !IsQuestFactsSubject(requester, subject))
        {
            outcomes.push_back({ entry.guid, PARTY_QUEST_DENIED });
            continue;
        }
        if (subject->IsSuiTacticallyFrozen())
        {
            outcomes.push_back({ entry.guid, PARTY_QUEST_DENIED });
            continue;
        }
        // Only the requester's own refusals may raise vanilla UI errors; a
        // companion's refusal is reported in the result packet, by name.
        bool const isSelf = (subject == requester);
        AiBotAI* ai = isSelf ? nullptr : BotAiOf(subject);
        uint8 result = PARTY_QUEST_OK;

        if (action == 3)                                   // ---- ABANDON ----
        {
            if (subject->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                result = PARTY_QUEST_NO_QUEST;
            else
            {
                subject->RemoveQuestById(questId);
                // RemoveQuestById can silently refuse when a quest start item
                // cannot be un-equipped, so confirm rather than assume.
                result = subject->GetQuestStatus(questId) == QUEST_STATUS_NONE
                    ? PARTY_QUEST_OK : PARTY_QUEST_CANNOT_ABANDON;
                if (result == PARTY_QUEST_OK && ai)
                {
                    if (ai->m_trackedQuestId == questId)
                        ai->m_trackedQuestId = 0;
                    ai->SendQuestUpdateEvent(questId, "abandoned");
                }
            }
        }
        else if (!IsPartyQuestInRange(requester, subject, pGiver))
        {
            result = PARTY_QUEST_TOO_FAR;
        }
        else if (action == 1)                              // ---- ACCEPT ----
        {
            QuestStatus status = subject->GetQuestStatus(questId);
            if (status != QUEST_STATUS_NONE)
            {
                // VMaNGOS parks a turned-in quest at COMPLETE forever, so
                // m_rewarded is the bit that separates "has it" from "did it".
                QuestStatusMap& map = subject->GetQuestStatusMap();
                QuestStatusMap::const_iterator itr = map.find(questId);
                result = (itr != map.end() && itr->second.m_rewarded)
                    ? PARTY_QUEST_ALREADY_REWARDED : PARTY_QUEST_ALREADY_HELD;
            }
            else if (!subject->CanTakeQuest(pQuest, isSelf))
                result = PARTY_QUEST_REQUIREMENTS;
            else if (!subject->CanAddQuest(pQuest, isSelf))
                result = PARTY_QUEST_LOG_FULL;
            else
            {
                // The real giver object is what fires OnQuestAccept and the
                // quest start scripts; passing nullptr would suppress both.
                subject->AddQuest(pQuest, pGiver);
                if (subject->CanCompleteQuest(questId))
                    subject->CompleteQuest(questId);
                // The bridge's accept path omits this; the real handler does not.
                if (pQuest->GetSrcSpell() > 0)
                    subject->CastSpell(subject, pQuest->GetSrcSpell(), true);
                if (isSelf && subject->PlayerTalkClass)
                    subject->PlayerTalkClass->CloseGossip();
                if (ai)
                {
                    ai->m_trackedQuestId = questId;
                    ai->SendQuestUpdateEvent(questId, "accepted");
                }
            }
        }
        else                                               // ---- TURN IN ----
        {
            if (subject->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                result = PARTY_QUEST_NO_QUEST;
            else
            {
                uint32 reward = entry.rewardChoice;
                if (reward == 255)
                {
                    // "Auto" means the spec-aware pick the fleet already uses.
                    // Our own character has no such chooser, so it leans on the
                    // client picker - but ONLY when the quest actually offers a
                    // choice. A fixed/no-reward quest (e.g. 783 "A Threat Within")
                    // has nothing to pick, so auto-for-self there is a plain 0, not
                    // a refusal the client could never satisfy (no picker is shown).
                    if (!ai)
                    {
                        if (pQuest->GetRewChoiceItemsCount() > 0)
                            result = PARTY_QUEST_NEEDS_CHOICE;
                        else
                            reward = 0;
                    }
                    else
                        reward = ai->ChooseQuestReward(pQuest);
                }
                if (result == PARTY_QUEST_OK)
                {
                    // The array bound is the floor, not the rule. Vanilla own
                    // handler stops at QUEST_REWARD_CHOICES_COUNT too, which lets
                    // an index inside the array but past THIS quest choices reach
                    // RewardQuest, where RewChoiceItemId[reward] == 0 means the
                    // quest is rewarded and the chosen item silently never lands.
                    uint32 const choiceCount = pQuest->GetRewChoiceItemsCount();
                    if (reward >= QUEST_REWARD_CHOICES_COUNT ||
                        (choiceCount > 0 && reward >= choiceCount))
                        result = PARTY_QUEST_BAD_REWARD;
                    else if (!subject->CanRewardQuest(pQuest, reward, isSelf))
                        result = PARTY_QUEST_CANNOT_REWARD;
                    else
                    {
                        // RewardQuest is the one consumer that needs the
                        // narrower type; everything else takes Object*.
                        subject->RewardQuest(pQuest, reward,
                            pGiver ? pGiver->ToWorldObject() : nullptr, true);
                        if (ai)
                        {
                            ai->m_trackedQuestId = 0;
                            ai->TryAutoEquipBags();
                            ai->TryAutoEquip();
                            ai->SendQuestUpdateEvent(questId, "rewarded");
                        }
                        else if (Quest const* next = subject->GetNextQuest(npcGuid, pQuest))
                            subject->PlayerTalkClass->SendQuestGiverQuestDetails(
                                next, npcGuid, true);
                    }
                }
            }
        }

        if (result == PARTY_QUEST_OK || result == PARTY_QUEST_ALREADY_HELD)
            touched.push_back(subject);
        outcomes.push_back({ entry.guid, result });
    }

    SendPartyQuestResult(session, action, questId, outcomes);

    // Push fresh quest logs for everyone whose log actually moved, rather than
    // making the client re-pull (which is rate-limited to one per second).
    std::set<uint32> unionHeld;
    BuildGroupHeldQuestUnion(session->GetPlayer(), unionHeld);
    for (Player* subject : touched)
        SendMemberQuests(session, subject, &unionHeld);
}

// ── Party item move (Phase C v1, owner 2026-08-25) ───────────────────────────
// The CRPG shared backpack: a real SUI player moves one bag item between two
// party endpoints — its own character or a party AiBot — with no trade window.
// The mechanics are the proven trade-completion sequence (CanStoreItem →
// MoveItemFromInventory → MoveItemToInventory, both helpers own the DB side).

static void SendMemberItemMoveResult(WorldSession* to, uint8 result,
    ObjectGuid fromGuid, ObjectGuid toGuid)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_MEMBER_ITEM_MOVE_RESULT, 17);
    data << uint8(result);
    data << uint64(fromGuid.GetRawValue());
    data << uint64(toGuid.GetRawValue());
    to->SendPacket(&data);
}

/// An endpoint of a party item move: the requester's own character, or a
/// party AiBot member. The party line itself — never faction authority.
static Player* ResolveItemMoveEndpoint(Player* requester, ObjectGuid guid)
{
    if (guid == requester->GetObjectGuid())
        return requester;
    Player* member = sObjectMgr.GetPlayer(guid);
    return member && member->IsInWorld() && IsMemberFactsSubject(requester, member)
        ? member : nullptr;
}

void HandleMemberItemMove(WorldSession* session, ObjectGuid fromGuid,
    ObjectGuid toGuid, uint8 bag, uint8 slot,
    bool inPlace, uint8 destBag, uint8 destSlot)
{
    // Only MSUIClient speaks this opcode — same latch as HandleRequest.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || session->GetBot())
        return;
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
    {
        SendMemberItemMoveResult(session, ITEM_MOVE_UNAVAILABLE, fromGuid, toGuid);
        return;
    }

    Player* from = ResolveItemMoveEndpoint(requester, fromGuid);

    // In-place rearrange within ONE owner (Ctrl+F party browser same-owner drag):
    // swap src<->dest on that owner's own bags directly, no possession. from == to.
    if (inPlace)
    {
        if (!from)
        {
            SendMemberItemMoveResult(session, ITEM_MOVE_DENIED, fromGuid, toGuid);
            return;
        }
        if (from->IsSuiTacticallyFrozen())
        {
            SendMemberItemMoveResult(session, ITEM_MOVE_UNAVAILABLE, fromGuid, toGuid);
            return;
        }
        if (from->GetTradeData())
        {
            SendMemberItemMoveResult(session, ITEM_MOVE_UNAVAILABLE, fromGuid, toGuid);
            return;
        }
        if (!from->GetItemByPos(bag, slot))
        {
            SendMemberItemMoveResult(session, ITEM_MOVE_NO_ITEM, fromGuid, toGuid);
            return;
        }
        // Same primitive the CMSG_SWAP_ITEM handler ends with; SwapItem validates
        // the destination internally (empty slot, swap, or same-stack merge).
        uint16 srcPos = (uint16(bag) << 8) | slot;
        uint16 dstPos = (uint16(destBag) << 8) | destSlot;
        from->SwapItem(srcPos, dstPos);
        SendMemberItemMoveResult(session, ITEM_MOVE_OK, fromGuid, toGuid);
        // Re-snapshot the one owner to every real SUI member; an own-char owner
        // also updated through the ordinary owner wire that SwapItem drove.
        if (Group* group = requester->GetGroup())
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                WorldSession* memberSession = member ? member->GetSession() : nullptr;
                if (!memberSession || memberSession->GetBot() || !memberSession->IsSuiCapable())
                    continue;
                if (IsMemberFactsSubject(member, from))
                    SendMemberFacts(memberSession, from);
            }
        return;
    }

    Player* to = ResolveItemMoveEndpoint(requester, toGuid);
    if (!from || !to || from == to)
    {
        SendMemberItemMoveResult(session, ITEM_MOVE_DENIED, fromGuid, toGuid);
        return;
    }
    if (from->IsSuiTacticallyFrozen() || to->IsSuiTacticallyFrozen())
    {
        SendMemberItemMoveResult(session, ITEM_MOVE_UNAVAILABLE, fromGuid, toGuid);
        return;
    }
    // Same map only (no distance gate — party logistics is deliberately
    // BG3-style); a live trade window on either endpoint would fight the
    // mutation mid-commit.
    if (from->GetMapId() != to->GetMapId() ||
        from->GetTradeData() || to->GetTradeData())
    {
        SendMemberItemMoveResult(session, ITEM_MOVE_UNAVAILABLE, fromGuid, toGuid);
        return;
    }
    Item* item = from->GetItemByPos(bag, slot);
    if (!item)
    {
        SendMemberItemMoveResult(session, ITEM_MOVE_NO_ITEM, fromGuid, toGuid);
        return;
    }
    // Binding deliberately does NOT gate the move — this is the party's shared
    // backpack, not the auction house. A conjured item would evaporate on its
    // new owner's next login, so it is the one refusal.
    if (ItemPrototype const* proto = item->GetProto())
        if (proto->Flags & ITEM_FLAG_CONJURED)
        {
            SendMemberItemMoveResult(session, ITEM_MOVE_REFUSED_ITEM, fromGuid, toGuid);
            return;
        }

    ItemPosCountVec dest;
    if (to->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
    {
        SendMemberItemMoveResult(session, ITEM_MOVE_TARGET_FULL, fromGuid, toGuid);
        return;
    }

    from->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    to->MoveItemToInventory(dest, item, true, true);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s moved item %u x%u from %s to %s",
        requester->GetName(), item->GetEntry(), item->GetCount(),
        from->GetName(), to->GetName());

    SendMemberItemMoveResult(session, ITEM_MOVE_OK, fromGuid, toGuid);
    // Fresh facts for BOTH ends to every real SUI member of the group — the
    // client columns update from these pushes, never from optimism. Own-char
    // endpoints update through the ordinary owner wire instead.
    Group* group = requester->GetGroup();
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        WorldSession* memberSession = member ? member->GetSession() : nullptr;
        if (!memberSession || memberSession->GetBot() || !memberSession->IsSuiCapable())
            continue;
        if (IsMemberFactsSubject(member, from))
            SendMemberFacts(memberSession, from);
        if (IsMemberFactsSubject(member, to))
            SendMemberFacts(memberSession, to);
    }
}

void MirrorOwnerPacket(WorldSession* botSession, WorldPacket const* packet)
{
    // Whitelist first: this sits on every socket-less SendPacket, keep it cheap.
    // Mirroring everything would double-deliver broadcasts the possessor already
    // receives; these are the strictly owner-only spell/bar/cooldown packets.
    switch (packet->GetOpcode())
    {
        // Owner-only replies from the driven bot; client unwraps with source isolation.
        case SMSG_GOSSIP_POI:
        case SMSG_ITEM_COOLDOWN:
        case SMSG_CANCEL_AUTO_REPEAT:
        case SMSG_CANCEL_COMBAT:
        case MSG_CHANNEL_START:
        case MSG_CHANNEL_UPDATE:
        case SMSG_UPDATE_AURA_DURATION:
        case SMSG_INVENTORY_CHANGE_FAILURE:
        case SMSG_START_MIRROR_TIMER:
        case SMSG_PAUSE_MIRROR_TIMER:
        case SMSG_STOP_MIRROR_TIMER:
        case SMSG_FEIGN_DEATH_RESISTED:
        case SMSG_SET_FORCED_REACTIONS:
        case SMSG_ACTION_BUTTONS:
        case SMSG_INITIAL_SPELLS:
        case SMSG_SET_FLAT_SPELL_MODIFIER:
        case SMSG_SET_PCT_SPELL_MODIFIER:
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        case SMSG_SPELL_COOLDOWN:
        case SMSG_COOLDOWN_EVENT:
        case SMSG_CLEAR_COOLDOWN:
        case SMSG_CAST_RESULT:
        case SMSG_STANDSTATE_UPDATE:
        // [SUI] P4b: the NPC-interaction reply frames of a driven bot. The quest
        // and gossip handler family now runs as GetSuiActor() (the possessed bot),
        // so these are built from the BOT's quest state and must reach the
        // commander's client, which unwraps them in ApplySuiProxy exactly as it
        // does the spell/bar packets above. Only ever mirrored while the bot is
        // possessed (GetPossessor gates below) — an autonomous bot mirrors nothing.
        // Vendor/trainer reply frames are deliberately NOT here yet: their action
        // handlers still act on _player, so a mirrored list would be misleading.
        case SMSG_GOSSIP_MESSAGE:
        case SMSG_GOSSIP_COMPLETE:
        case SMSG_QUESTGIVER_STATUS:
        case SMSG_QUESTGIVER_QUEST_LIST:
        case SMSG_QUESTGIVER_QUEST_DETAILS:
        case SMSG_QUESTGIVER_REQUEST_ITEMS:
        case SMSG_QUESTGIVER_OFFER_REWARD:
        case SMSG_QUESTGIVER_QUEST_INVALID:
        case SMSG_QUESTGIVER_QUEST_FAILED:
        case SMSG_QUESTLOG_FULL:
        case SMSG_QUESTUPDATE_COMPLETE:
        case SMSG_QUESTUPDATE_FAILED:
        case SMSG_QUESTUPDATE_FAILEDTIMER:
        case SMSG_QUESTUPDATE_ADD_ITEM:
        case SMSG_QUESTUPDATE_ADD_KILL:
        case SMSG_QUESTGIVER_QUEST_COMPLETE:
        // [SUI] P4b vendor/trainer/repair: the driven bot's shop and trainer reply
        // frames. Reached here only when built on the bot's socket-less session (a
        // gossip "browse goods" / "train" option, or an internal SendBuyError); the
        // direct vendor/trainer-frame requests already answer on the commander's own
        // socket. Either way the commander's ApplySuiProxy routes them into the same
        // vendor/trainer parsers. Inventory/coin edits refresh via ResnapshotControlled.
        case SMSG_LIST_INVENTORY:
        case SMSG_SELL_ITEM:
        case SMSG_BUY_ITEM:
        case SMSG_BUY_FAILED:
        case SMSG_TRAINER_LIST:
        case SMSG_TRAINER_BUY_SUCCEEDED:
        case SMSG_TRAINER_BUY_FAILED:
        // [SUI] trade: the partner's session addresses the BOT (TradeData::Update →
        // m_player->GetSession()->SendUpdateTrade, HandleBeginTrade's OPEN_WINDOW, the
        // completion verdict). The handlers themselves run as GetSuiActor() on the
        // commander's session; these are the halves that come back through the bot.
        case SMSG_TRADE_STATUS:
        case SMSG_TRADE_STATUS_EXTENDED:
        // [SUI] mail: new-mail notices are pushed at the RECEIVER's session (the bot's). The
        // mailbox verbs themselves run as GetSuiActor() on the commander's session.
        case SMSG_RECEIVED_MAIL:
        case MSG_QUERY_NEXT_MAIL_TIME:
        // [SUI] loot: LootHandler runs as GetSuiActor(), and Player::SendLoot /
        // SendLootRelease / SendNotifyLootItemRemoved / SendNotifyLootMoneyRemoved /
        // SendLootMoneyNotify all emit on the LOOTER's session — the bot's. Item
        // pushes: Player::SendNewItem skips the possessor in its group broadcast so
        // this mirrored copy is the commander's only one.
        case SMSG_LOOT_RESPONSE:
        case SMSG_LOOT_RELEASE_RESPONSE:
        case SMSG_LOOT_REMOVED:
        case SMSG_LOOT_CLEAR_MONEY:
        case SMSG_LOOT_MONEY_NOTIFY:
        case SMSG_ITEM_PUSH_RESULT:
        // [SUI] pet: PetHandler runs as GetSuiActor(); the pet bar, mode, feedback and
        // cast-fail frames address the pet OWNER's session — the bot's.
        case SMSG_PET_SPELLS:
        case SMSG_PET_MODE:
        case SMSG_PET_ACTION_SOUND:
        case SMSG_PET_UNLEARN_CONFIRM:
        case SMSG_PET_ACTION_FEEDBACK:
        case SMSG_PET_CAST_FAILED:
        // [SUI] taxi: TaxiHandler runs as GetSuiActor(); the activation verdict
        // leaves on the FLYER's session (Player::ActivateTaxiPathTo). The map,
        // node status and discovery frames answer on the commander's socket from
        // the direct handlers, but a gossip "I need a ride" (Player::OnGossipSelect
        // → GetSession()->SendTaxiMenu / SendLearnNewTaxiNode) builds them on the
        // BOT's session — without these the option silently did nothing.
        case SMSG_ACTIVATETAXIREPLY:
        case SMSG_SHOWTAXINODES:
        case SMSG_TAXINODE_STATUS:
        case SMSG_NEW_TAXI_PATH:
        // [SUI] near teleport of the driven bot (Player::TeleportTo same map): the
        // possessor's client adopts it on its controller and answers the ack.
        case MSG_MOVE_TELEPORT_ACK:
        // [SUI] gossip-answered frames of the routed families (owner 2026-09-03:
        // "a gossip reply that lands nowhere doesn't count as functioning").
        // Player::OnGossipSelect runs as the driven bot and answers on ITS session:
        // banker → SendShowBank, stable master → SendStablePet, trainer "unlearn
        // talents" → SendTalentWipeConfirm, innkeeper → the Bind spell's confirm,
        // auctioneer → SendAuctionHello. The direct frame requests already answer
        // on the commander's socket. SMSG_BINDPOINTUPDATE is deliberately NOT
        // mirrored: it is the bot's hearth data and the client keeps one hearth.
        case SMSG_SHOW_BANK:
        case MSG_LIST_STABLED_PETS:
        case MSG_TALENT_WIPE_CONFIRM:
        case SMSG_INITIALIZE_FACTIONS:
        case SMSG_SET_FACTION_VISIBLE:
        case SMSG_SET_FACTION_STANDING:
        case SMSG_SET_FACTION_ATWAR:
        case SMSG_WEATHER:
        case SMSG_BINDER_CONFIRM:
        case SMSG_SET_PROFICIENCY:
        case SMSG_PLAYERBOUND:
        case MSG_AUCTION_HELLO:
            break;
        default:
            return;
    }

    Player* bot = botSession->GetPlayer();
    if (!bot)
        return;
    Player* possessor = GetPossessor(bot);
    if (!possessor || !possessor->GetSession() || !possessor->GetSession()->IsSuiCapable())
        return;

    WorldPacket data(SMSG_SUI_PROXY, 8 + 2 + packet->size());
    data << uint64(bot->GetObjectGuid().GetRawValue());
    data << uint16(packet->GetOpcode());
    if (packet->size() > 0)
        data.append(packet->contents(), packet->size());
    possessor->GetSession()->SendPacket(&data);
}

} // namespace SuiPossess

// ── Zone intel (commander map) ────────────────────────────────────────────────

void SuiPossess::HandleZoneIntel(WorldSession* session, uint8 /*flags*/)
{
    // Any CMSG_SUI_* proves the sender is MSUIClient (same opportunistic mark
    // as the other handlers); the reply below goes only to the asker.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || !requester->IsInWorld() || session->GetBot())
        return;

    // Census: every in-world player bucketed by CACHED zone id. GetCachedZoneId
    // is a plain field read at most one zone-tick stale; WorldObject::GetZoneId
    // is a terrain query and must never run in this loop.
    std::unordered_map<uint32, std::pair<uint16, uint16>> census;   // zone -> {bots, players}
    {
        HashMapHolder<Player>::ReadGuard g(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld())
                continue;
            uint32 zone = p->GetCachedZoneId();
            if (!zone)
                continue;                    // mid-login, zone not resolved yet
            auto& c = census[zone];
            uint16& bucket = p->IsBot() ? c.first : c.second;
            if (bucket < 0xFFFF)
                ++bucket;                    // unattended real characters count as players
        }
    }

    // The asker's own forces: self + every in-world group member, with live
    // positions. The client has no position data for unstreamed members — this
    // block is what places the unit markers on the zone map.
    std::vector<Player*> units;
    units.push_back(requester);
    if (Group* group = requester->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member->IsInWorld() && member != requester)
                units.push_back(member);
        }
    if (units.size() > 250)
        units.resize(250);                   // u8 count; far above any raid size

    // Both blocks carry an explicit row stride so a future server can append
    // per-row facts while an older client skips them (see SUI_WIRE_PROTOCOL.md).
    WorldPacket data(SMSG_SUI_ZONE_INTEL, 3 + census.size() * 9 + 2 + units.size() * 29);
    data << uint16(census.size());
    data << uint8(9);                        // zone row stride (R1: +controller byte)
    for (auto const& kv : census)
    {
        data << uint32(kv.first);
        data << uint16(kv.second.first);
        data << uint16(kv.second.second);
        data << uint8(0);                    // controller (0 until territory R3; 0x80 = contested)
    }
    data << uint8(units.size());
    data << uint8(29);                       // unit row stride
    for (Player* u : units)
    {
        uint8 unitFlags = 0;
        if (u->IsAlive()) unitFlags |= 1;
        if (u->IsBot())   unitFlags |= 2;
        data << uint64(u->GetObjectGuid().GetRawValue());
        data << uint32(u->GetMapId());
        data << uint32(u->GetCachedZoneId());
        data << float(u->GetPositionX());
        data << float(u->GetPositionY());
        data << float(u->GetPositionZ());
        data << unitFlags;
    }
    session->SendPacket(&data);
}

// ── Wire handlers ────────────────────────────────────────────────────────────

Player* WorldSession::GetSuiActor()
{
    // The unit the session's gameplay input acts as: the possessed bot while
    // driving one, else the session's own player. Threaded through the
    // cast/target/melee handler family — bit-identical when not possessing.
    if (!m_suiControlledGuid.IsEmpty())
        if (Player* bot = SuiPossess::GetControlledBot(this))
            return bot;
    return _player;
}

void WorldSession::HandleSuiControlRequestOpcode(WorldPackets::SuiControl::ControlRequest const& packet)
{
    SuiPossess::HandleRequest(this, packet.targetGuid);
}

void WorldSession::HandleSuiControlReleaseOpcode(WorldPackets::SuiControl::ControlRelease const& packet)
{
    SuiPossess::HandleRelease(this, packet.mode);
}

void WorldSession::HandleSuiOrderOpcode(WorldPackets::SuiControl::Order const& packet)
{
    SuiPossess::HandleOrder(this, packet.orderType, packet.subjects,
        packet.targetGuid, packet.x, packet.y, packet.z);
}

void WorldSession::HandleSuiCamOpcode(WorldPackets::SuiControl::Cam const& packet)
{
    SuiPossess::HandleCam(this, packet.x, packet.y, packet.z, packet.active != 0);
}

void WorldSession::HandleSuiTacticalFreezeOpcode(
    WorldPackets::SuiControl::TacticalFreeze const& packet)
{
    SuiTacticalFreeze::HandleFreeze(this, packet.exactSize, packet.version,
        packet.requestId, packet.desiredActive, packet.lockId);
}

void WorldSession::HandleSuiTacticalQueueOpcode(
    WorldPackets::SuiControl::TacticalQueue const& packet)
{
    std::vector<SuiTacticalFreeze::QueueRecord> records;
    records.reserve(packet.records.size());
    for (WorldPackets::SuiControl::TacticalQueue::Record const& wire : packet.records)
    {
        SuiTacticalFreeze::QueueRecord row;
        row.actorGuid = wire.actorGuid;
        row.actionId = wire.actionId;
        row.actionKind = wire.actionKind;
        row.targetGuid = wire.targetGuid;
        row.x = wire.x;
        row.y = wire.y;
        row.z = wire.z;
        row.spellId = wire.spellId;
        records.push_back(row);
    }
    SuiTacticalFreeze::HandleQueue(this, packet.exactSize, packet.version,
        packet.lockId, packet.requestId, packet.operation, records);
}

void WorldSession::HandleSuiZoneIntelOpcode(WorldPackets::SuiControl::ZoneIntel const& packet)
{
    SuiPossess::HandleZoneIntel(this, packet.flags);
}

void WorldSession::HandleSuiMemberFactsOpcode(WorldPackets::SuiControl::MemberFacts const& packet)
{
    // Wire discipline: a body whose length does not match its count is refused
    // outright rather than best-effort parsed.
    if (!packet.exactSize)
        return;
    SuiPossess::HandleMemberFacts(this, packet.subjects);
}

void WorldSession::HandleSuiMemberItemMoveOpcode(
    WorldPackets::SuiControl::MemberItemMove const& packet)
{
    if (!packet.exactSize)
        return;
    SuiPossess::HandleMemberItemMove(this, packet.from, packet.to,
        packet.bag, packet.slot, packet.inPlace, packet.destBag, packet.destSlot);
}

void WorldSession::HandleSuiQuestFactsOpcode(
    WorldPackets::SuiControl::QuestFacts const& packet)
{
    if (!packet.exactSize)
        return;
    SuiPossess::HandleQuestFacts(this, packet.flags, packet.subjects);
}

void WorldSession::HandleSuiPartyLeadOpcode(
    WorldPackets::SuiControl::PartyLead const& packet)
{
    if (!packet.exactSize)
        return;
    SuiPossess::HandlePartyLead(this, packet.action, packet.subject);
}

void WorldSession::HandleSuiGiverStatusOpcode(
    WorldPackets::SuiControl::GiverStatus const& packet)
{
    if (!packet.exactSize)
        return;
    SuiPossess::HandleGiverStatus(this, packet.givers);
}

void WorldSession::HandleSuiGiverQuestsOpcode(
    WorldPackets::SuiControl::GiverQuests const& packet)
{
    if (!packet.exactSize)
        return;
    SuiPossess::HandleGiverQuests(this, packet.giver);
}

void WorldSession::HandleSuiPartyQuestOpcode(
    WorldPackets::SuiControl::PartyQuest const& packet)
{
    if (!packet.exactSize)
        return;
    if (packet.action < 1 || packet.action > 3)
        return;
    // The wire type stays on this side of the namespace boundary; SuiPossess.h
    // sees only Common.h and ObjectGuid.h.
    std::vector<SuiPossess::PartyQuestSubject> subjects;
    subjects.reserve(packet.subjects.size());
    for (auto const& subject : packet.subjects)
        subjects.push_back({ subject.guid, subject.rewardChoice });
    SuiPossess::HandlePartyQuest(this, packet.action, packet.questId,
        packet.npcGuid, subjects);
}

// ── GM commands (stock-client testable: .sui possess <name> / .sui release) ──

bool ChatHandler::HandleSuiWorldStateCommand(char* args)
{
    if (char* arg = ExtractLiteralArg(&args))
    {
        PSendSysMessage("SUI worldstate is boot-latched; '%s' was not applied.", arg);
        return true;
    }
    PSendSysMessage("SUI worldstate: %s",
        SuiWorldState::RtsWorldState() ? "RTS MATCH" : "vanilla");
    return true;
}

bool ChatHandler::HandleSuiPossessCommand(char* args)
{
    Player* requester = m_session ? m_session->GetPlayer() : nullptr;
    if (!requester)
        return false;

    ObjectGuid targetGuid;
    if (char* name = ExtractLiteralArg(&args))
    {
        std::string playerName = name;
        if (Player* target = sObjectMgr.GetPlayer(playerName.c_str()))
            targetGuid = target->GetObjectGuid();
    }
    else if (Player* selected = GetSelectedPlayer())
        targetGuid = selected->GetObjectGuid();

    if (targetGuid.IsEmpty())
    {
        SendSysMessage("[SUI] usage: .sui possess <botname> (or select the bot)");
        SetSentErrorMessage(true);
        return false;
    }

    Player* bot = nullptr;
    SuiPossess::AckResult result = SuiPossess::TryBegin(m_session, targetGuid, &bot);
    if (result == SuiPossess::ACK_OK)
        PSendSysMessage("[SUI] possessing %s — WASD drives the bot, .sui release to stop",
            bot ? bot->GetName() : "?");
    else
        PSendSysMessage("[SUI] possess denied (code %u)", uint32(result));
    return true;
}

bool ChatHandler::HandleSuiReleaseCommand(char* /*args*/)
{
    if (!m_session)
        return false;
    if (m_session->GetSuiControlledGuid().IsEmpty())
    {
        SendSysMessage("[SUI] not possessing anything");
        return true;
    }
    // Voluntary GM release still opens the drain window: a stock client has no
    // pending-state machine parking its movement stream first.
    SuiPossess::ForceRelease(m_session, SuiPossess::RELEASED);
    SendSysMessage("[SUI] released");
    return true;
}
