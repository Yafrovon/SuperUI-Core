/*
 * SuperUI real portals.
 *
 * This is deliberately a preparation/readiness side channel. It never calls
 * GameObject::Use and therefore cannot teleport a player or weaken the stock
 * CMSG_GAMEOBJ_USE checks. The client gets only enough authoritative metadata
 * to preload and render the far side of the six stock mage portals.
 */

#include "SuiPortal.h"

#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
    constexpr uint8 PORTAL_PROTOCOL_VERSION = WorldPackets::SuiPortal::SUI_PORTAL_PROTOCOL_VERSION;
    constexpr float PORTAL_PRELOAD_RADIUS = 150.0f;
    constexpr float PORTAL_HALF_WIDTH = 3.0f;
    constexpr float PORTAL_HALF_HEIGHT = 4.0f;
    constexpr float PORTAL_BOTTOM_CLEARANCE = 0.10f;
    constexpr float PORTAL_CROSSING_EPSILON = 0.35f;
    constexpr uint32 PORTAL_PREPARE_LEASE_MS = 30000;
    constexpr uint32 PORTAL_READY_LEASE_MS = 5000;
    constexpr uint32 PORTAL_DESCRIPTOR_REVISION = 1;
    // Descriptor flags are part of the cross-repo wire contract. Bit 4 is
    // reserved for a future server-verified bidirectional membrane.
    constexpr uint16 PORTAL_DESCRIPTOR_ONE_WAY = 0x0001;
    constexpr uint16 PORTAL_DESCRIPTOR_PARTY_ONLY = 0x0002;
    constexpr uint16 PORTAL_DESCRIPTOR_CLICK_FALLBACK = 0x0004;
    constexpr uint16 PORTAL_DESCRIPTOR_SAME_MAP_HINT = 0x0008;
    constexpr uint16 PORTAL_DESCRIPTOR_BIDIRECTIONAL = 0x0010;
    constexpr size_t PORTAL_DESCRIPTOR_WIRE_SIZE = 92;
    constexpr size_t PORTAL_STATE_WIRE_SIZE = 32;
    constexpr uint8 PORTAL_PREWARM_CATALOG_VERSION = 1;
    constexpr uint16 PORTAL_PREWARM_ROW_SIZE = 32;

    struct PortalCastDefinition
    {
        uint32 summonSpellId;
        uint32 portalEntry;
        uint32 teleportSpellId;
    };

    constexpr std::array<PortalCastDefinition, 6> PORTAL_CAST_DEFINITIONS = {{
        {10059, 176296, 17334}, // Stormwind
        {11416, 176497, 17607}, // Ironforge
        {11417, 176499, 17609}, // Orgrimmar
        {11418, 176501, 17611}, // Undercity
        {11419, 176498, 17608}, // Darnassus
        {11420, 176500, 17610}, // Thunder Bluff
    }};

    struct PortalFacts
    {
        GameObject* object = nullptr;
        GameObjectInfo const* info = nullptr;
        SpellTargetPosition const* destination = nullptr;
        uint32 teleportSpellId = 0;
    };

    enum class ValidationResult
    {
        Ok,
        Expired,
        Unsupported,
        Denied,
    };

    struct DescriptorPayload
    {
        SuiPortal::DescriptorResult result = SuiPortal::DESCRIPTOR_FAILED;
        uint16 flags = 0;
        uint32 requestId = 0;
        ObjectGuid portalGuid;
        uint32 generation = 0;
        uint32 revision = 0;
        uint64 ticket = 0;
        uint32 entry = 0;
        uint32 teleportSpellId = 0;
        uint32 remainingLifetimeMs = 0;
        float sourceX = 0.0f;
        float sourceY = 0.0f;
        float sourceZ = 0.0f;
        float sourceYaw = 0.0f;
        float halfWidth = 0.0f;
        float halfHeight = 0.0f;
        float crossingEpsilon = 0.0f;
        uint32 destinationMapId = 0;
        float destinationX = 0.0f;
        float destinationY = 0.0f;
        float destinationZ = 0.0f;
        float destinationOrientation = 0.0f;
    };

    bool IsStockPortalPair(uint32 entry, uint32 spellId)
    {
        switch (entry)
        {
            // gameobject_template.data0: the spell cast by using the portal GO.
            // These are distinct from the Mage's 356x self-teleport spells.
            case 176296: return spellId == 17334; // Stormwind
            case 176497: return spellId == 17607; // Ironforge
            case 176498: return spellId == 17608; // Darnassus
            case 176499: return spellId == 17609; // Orgrimmar
            case 176500: return spellId == 17610; // Thunder Bluff
            case 176501: return spellId == 17611; // Undercity
            default: return false;
        }
    }

    PortalCastDefinition const* FindPortalCastDefinition(uint32 summonSpellId,
        uint32 portalEntry = 0)
    {
        for (PortalCastDefinition const& definition : PORTAL_CAST_DEFINITIONS)
        {
            if (definition.summonSpellId == summonSpellId &&
                (!portalEntry || definition.portalEntry == portalEntry))
                return &definition;
        }
        return nullptr;
    }

    bool IsFiniteDestination(SpellTargetPosition const* destination)
    {
        return destination && std::isfinite(destination->x) &&
            std::isfinite(destination->y) && std::isfinite(destination->z) &&
            std::isfinite(destination->o);
    }

    bool ResolvePortalCastDefinition(PortalCastDefinition const& definition,
        SpellTargetPosition const*& destination)
    {
        destination = nullptr;
        SpellEntry const* summon = sSpellMgr.GetSpellEntry(definition.summonSpellId);
        if (!summon)
            return false;

        bool summonsExpectedEntry = false;
        for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        {
            if (summon->Effect[effect] == SPELL_EFFECT_TRANS_DOOR &&
                summon->EffectMiscValue[effect] > 0 &&
                uint32(summon->EffectMiscValue[effect]) == definition.portalEntry)
            {
                summonsExpectedEntry = true;
                break;
            }
        }
        if (!summonsExpectedEntry)
            return false;

        GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(definition.portalEntry);
        if (!info || info->type != GAMEOBJECT_TYPE_SPELLCASTER ||
            info->spellcaster.spellId != definition.teleportSpellId)
            return false;

        destination = sSpellMgr.GetSpellTargetPosition(definition.teleportSpellId);
        return IsFiniteDestination(destination);
    }

    bool IsPartyEligible(GameObject* portal, Player* player)
    {
        GameObjectInfo const* info = portal->GetGOInfo();
        if (!info || !info->spellcaster.partyOnly)
            return true;

        if (Unit* caster = portal->GetOwner())
        {
            if (caster->GetTypeId() != TYPEID_PLAYER)
                return false;
            return player->IsInSameRaidWith(static_cast<Player*>(caster));
        }

        uint32 ownerGroupId = portal->GetOwnerGroupId();
        Group* group = player->GetGroup();
        return ownerGroupId != 0 && group && group->GetId() == ownerGroupId;
    }

    ValidationResult ValidatePortal(WorldSession* session, ObjectGuid portalGuid, PortalFacts& facts)
    {
        Player* player = session->GetPlayer();
        if (!player || session->GetBot() || !player->IsInWorld() || !player->IsSelfMover())
            return ValidationResult::Denied;

        if (!portalGuid.IsGameObject() || !player->GetMap())
            return ValidationResult::Expired;

        GameObject* portal = player->GetMap()->GetGameObject(portalGuid);
        if (!portal || portal->IsDeleted() || !portal->isSpawned())
            return ValidationResult::Expired;

        GameObjectInfo const* info = portal->GetGOInfo();
        if (!info || portal->GetGoType() != GAMEOBJECT_TYPE_SPELLCASTER ||
            !IsStockPortalPair(portal->GetEntry(), info->spellcaster.spellId))
            return ValidationResult::Unsupported;

        SpellTargetPosition const* destination =
            sSpellMgr.GetSpellTargetPosition(info->spellcaster.spellId);
        if (!destination)
            return ValidationResult::Unsupported;

        // Preparation is intentionally wider than stock click distance: it is
        // what lets the client start loading while the player approaches. The
        // normal use handler still enforces IsAtInteractDistance at the click.
        if (!player->IsWithinDistInMap(portal, PORTAL_PRELOAD_RADIUS) ||
            portal->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT) ||
            !portal->PlayerCanUse(player) || !IsPartyEligible(portal, player))
            return ValidationResult::Denied;

        facts.object = portal;
        facts.info = info;
        facts.destination = destination;
        facts.teleportSpellId = info->spellcaster.spellId;
        return ValidationResult::Ok;
    }

    uint32 RemainingLifetimeMs(GameObject const* portal)
    {
        time_t expiresAt = portal->GetRespawnTime();
        time_t now = time(nullptr);
        if (!expiresAt)
            return std::numeric_limits<uint32>::max();
        if (expiresAt <= now)
            return 0;

        uint64 milliseconds = uint64(expiresAt - now) * 1000u;
        return uint32(std::min<uint64>(milliseconds,
            std::numeric_limits<uint32>::max() - 1u));
    }

    uint64 NewTicket()
    {
        uint64 ticket = (uint64(uint32(rand32())) << 32) | uint32(rand32());
        return ticket ? ticket : 1u;
    }

    bool LeaseExpired(uint32 issuedAtMs, uint32 leaseMs, uint32 nowMs)
    {
        return !leaseMs || WorldTimer::getMSTimeDiff(issuedAtMs, nowMs) >= leaseMs;
    }

    uint32 RemainingLeaseMs(uint32 issuedAtMs, uint32 leaseMs, uint32 nowMs)
    {
        if (LeaseExpired(issuedAtMs, leaseMs, nowMs))
            return 0;
        return leaseMs - WorldTimer::getMSTimeDiff(issuedAtMs, nowMs);
    }

    void SendDescriptor(WorldSession* session, DescriptorPayload const& descriptor)
    {
        if (!session->IsSuiCapable())
            return;

        WorldPacket data(SMSG_SUI_PORTAL_DESCRIPTOR, PORTAL_DESCRIPTOR_WIRE_SIZE);
        data << uint8(PORTAL_PROTOCOL_VERSION);
        data << uint8(descriptor.result);
        data << uint16(descriptor.flags);
        data << uint32(descriptor.requestId);
        data << uint64(descriptor.portalGuid.GetRawValue());
        data << uint32(descriptor.generation);
        data << uint32(descriptor.revision);
        data << uint64(descriptor.ticket);
        data << uint32(descriptor.entry);
        data << uint32(descriptor.teleportSpellId);
        data << uint32(descriptor.remainingLifetimeMs);
        data << float(descriptor.sourceX) << float(descriptor.sourceY) << float(descriptor.sourceZ);
        data << float(descriptor.sourceYaw);
        data << float(descriptor.halfWidth) << float(descriptor.halfHeight);
        data << float(descriptor.crossingEpsilon);
        data << uint32(descriptor.destinationMapId);
        data << float(descriptor.destinationX) << float(descriptor.destinationY) << float(descriptor.destinationZ);
        data << float(descriptor.destinationOrientation);
        MANGOS_ASSERT(data.size() == PORTAL_DESCRIPTOR_WIRE_SIZE);
        session->SendPacket(&data);
    }

    void SendState(WorldSession* session, SuiPortal::PortalState state,
        SuiPortal::StateReason reason, ObjectGuid portalGuid, uint32 generation,
        uint32 revision, uint64 ticket, uint32 leaseMs)
    {
        if (!session->IsSuiCapable())
            return;

        WorldPacket data(SMSG_SUI_PORTAL_STATE, PORTAL_STATE_WIRE_SIZE);
        data << uint8(PORTAL_PROTOCOL_VERSION);
        data << uint8(state);
        data << uint8(reason);
        data << uint8(0);
        data << uint64(portalGuid.GetRawValue());
        data << uint32(generation);
        data << uint32(revision);
        data << uint64(ticket);
        data << uint32(leaseMs);
        MANGOS_ASSERT(data.size() == PORTAL_STATE_WIRE_SIZE);
        session->SendPacket(&data);
    }

    SuiPortal::DescriptorResult ToDescriptorResult(ValidationResult result)
    {
        switch (result)
        {
            case ValidationResult::Ok: return SuiPortal::DESCRIPTOR_OK;
            case ValidationResult::Expired: return SuiPortal::DESCRIPTOR_EXPIRED;
            case ValidationResult::Unsupported: return SuiPortal::DESCRIPTOR_UNSUPPORTED;
            case ValidationResult::Denied: return SuiPortal::DESCRIPTOR_DENIED;
        }
        return SuiPortal::DESCRIPTOR_FAILED;
    }
}

void SuiPortal::WriteCapabilityTrailer(WorldPacket& data)
{
    std::array<SpellTargetPosition const*, PORTAL_CAST_DEFINITIONS.size()> destinations = {{}};
    bool catalogComplete = true;
    for (size_t i = 0; i < PORTAL_CAST_DEFINITIONS.size(); ++i)
    {
        if (!ResolvePortalCastDefinition(PORTAL_CAST_DEFINITIONS[i], destinations[i]))
            catalogComplete = false;
    }

    uint32 capabilities = CAPABILITY_REAL_PORTALS_V1 |
        CAPABILITY_FACTION_CONTROL_GROUPS_V1 |
        CAPABILITY_PARTY_MEMBER_FACTS_V1 |
        CAPABILITY_PARTY_ITEM_MOVE_V1 |
        CAPABILITY_PARTY_QUEST_FACTS_V1 |
        CAPABILITY_PARTY_QUEST_ACTS_V1 |
        CAPABILITY_PARTY_GIVER_STATUS_V1 |
        CAPABILITY_PARTY_LEAD_V1 |
        CAPABILITY_PARTY_GIVER_QUESTS_V1 |
        CAPABILITY_COMPANIONS_V1 |
        CAPABILITY_PARTY_TAXI_V1 |
        CAPABILITY_TACTICAL_FREEZE_V1 | CAPABILITY_COMMANDER_RAID_V1 |
        CAPABILITY_THREAT_METER_V1;
    if (catalogComplete)
        capabilities |= CAPABILITY_PORTAL_CAST_PREWARM_V1;

    data << uint32(CAPABILITIES_MAGIC);
    data << uint32(capabilities);
    if (!catalogComplete)
        return;

    data << uint8(PORTAL_PREWARM_CATALOG_VERSION);
    data << uint8(PORTAL_CAST_DEFINITIONS.size());
    data << uint16(PORTAL_PREWARM_ROW_SIZE);
    for (size_t i = 0; i < PORTAL_CAST_DEFINITIONS.size(); ++i)
    {
        PortalCastDefinition const& definition = PORTAL_CAST_DEFINITIONS[i];
        SpellTargetPosition const& destination = *destinations[i];
        data << uint32(definition.summonSpellId);
        data << uint32(definition.portalEntry);
        data << uint32(definition.teleportSpellId);
        data << uint32(destination.mapId);
        data << float(destination.x) << float(destination.y) << float(destination.z);
        data << float(destination.o);
    }
}

void SuiPortal::ConfigureSummonedPortalVisibility(GameObject* portal,
    uint32 summonSpellId)
{
    if (!portal || !FindPortalCastDefinition(summonSpellId, portal->GetEntry()))
        return;

    // Visibility modifiers only participate for active objects. Six temporary
    // portal objects are a bounded exception and the core already uses this
    // exact 200-yard tier for templates carrying spellcaster.large.
    portal->SetVisibilityModifier(VISIBILITY_DISTANCE_LARGE);
    portal->SetActiveObjectState(true);
}

void WorldSession::HandleSuiPortalPrepareOpcode(WorldPackets::SuiPortal::Prepare const& packet)
{
    // A response is legal only after this CMSG has opted the session in.
    SetSuiCapable(true);

    DescriptorPayload descriptor;
    descriptor.requestId = packet.requestId;
    descriptor.portalGuid = packet.portalGuid;

    if (!packet.exactSize)
    {
        descriptor.result = SuiPortal::DESCRIPTOR_FAILED;
        SendDescriptor(this, descriptor);
        return;
    }

    if (packet.version != PORTAL_PROTOCOL_VERSION)
    {
        descriptor.result = SuiPortal::DESCRIPTOR_UNSUPPORTED;
        SendDescriptor(this, descriptor);
        return;
    }

    if (packet.reserved != 0 || packet.flags != 0)
    {
        descriptor.result = SuiPortal::DESCRIPTOR_FAILED;
        SendDescriptor(this, descriptor);
        return;
    }

    PortalFacts facts;
    ValidationResult validation = ValidatePortal(this, packet.portalGuid, facts);
    descriptor.result = ToDescriptorResult(validation);
    if (validation != ValidationResult::Ok)
    {
        SendDescriptor(this, descriptor);
        return;
    }

    uint32 objectLifetimeMs = RemainingLifetimeMs(facts.object);
    if (objectLifetimeMs == 0)
    {
        descriptor.result = SuiPortal::DESCRIPTOR_EXPIRED;
        SendDescriptor(this, descriptor);
        return;
    }

    uint32 nowMs = WorldTimer::getMSTime();
    bool reuseCorrelation = m_suiPortalGuid == packet.portalGuid && m_suiPortalTicket != 0 &&
        !LeaseExpired(m_suiPortalIssuedAtMs, m_suiPortalLeaseMs, nowMs);
    if (!reuseCorrelation)
    {
        ++m_suiPortalGeneration;
        if (!m_suiPortalGeneration)
            ++m_suiPortalGeneration;
        m_suiPortalRevision = PORTAL_DESCRIPTOR_REVISION;
        m_suiPortalTicket = NewTicket();
    }

    m_suiPortalGuid = packet.portalGuid;
    m_suiPortalIssuedAtMs = nowMs;
    m_suiPortalLeaseMs = PORTAL_PREPARE_LEASE_MS;

    if (objectLifetimeMs != std::numeric_limits<uint32>::max())
        m_suiPortalLeaseMs = std::min(m_suiPortalLeaseMs, objectLifetimeMs);

    descriptor.flags = PORTAL_DESCRIPTOR_ONE_WAY | PORTAL_DESCRIPTOR_CLICK_FALLBACK;
    if (facts.info->spellcaster.partyOnly)
        descriptor.flags |= PORTAL_DESCRIPTOR_PARTY_ONLY;
    if (GetPlayer() && facts.destination->mapId == GetPlayer()->GetMapId())
        descriptor.flags |= PORTAL_DESCRIPTOR_SAME_MAP_HINT;
    descriptor.generation = m_suiPortalGeneration;
    descriptor.revision = m_suiPortalRevision;
    descriptor.ticket = m_suiPortalTicket;
    descriptor.entry = facts.object->GetEntry();
    descriptor.teleportSpellId = facts.teleportSpellId;
    descriptor.remainingLifetimeMs = objectLifetimeMs;
    descriptor.sourceX = facts.object->GetPositionX();
    descriptor.sourceY = facts.object->GetPositionY();
    descriptor.sourceZ = facts.object->GetPositionZ() + PORTAL_BOTTOM_CLEARANCE + PORTAL_HALF_HEIGHT;
    descriptor.sourceYaw = facts.object->GetOrientation();
    descriptor.halfWidth = PORTAL_HALF_WIDTH;
    descriptor.halfHeight = PORTAL_HALF_HEIGHT;
    descriptor.crossingEpsilon = PORTAL_CROSSING_EPSILON;
    descriptor.destinationMapId = facts.destination->mapId;
    descriptor.destinationX = facts.destination->x;
    descriptor.destinationY = facts.destination->y;
    descriptor.destinationZ = facts.destination->z;
    descriptor.destinationOrientation = facts.destination->o;
    SendDescriptor(this, descriptor);
}

void WorldSession::HandleSuiPortalReadyOpcode(WorldPackets::SuiPortal::Ready const& packet)
{
    SetSuiCapable(true);

    if (!packet.exactSize)
    {
        SendState(this, SuiPortal::STATE_FAILED, SuiPortal::REASON_BAD_PACKET,
            ObjectGuid(), 0, 0, 0, 0);
        return;
    }

    if (packet.version != PORTAL_PROTOCOL_VERSION)
    {
        SendState(this, SuiPortal::STATE_FAILED, SuiPortal::REASON_UNSUPPORTED_VERSION,
            packet.portalGuid, packet.generation, packet.revision, packet.ticket, 0);
        return;
    }

    if (packet.reserved != 0 || packet.result > SuiPortal::LOAD_FAILED)
    {
        SendState(this, SuiPortal::STATE_FAILED, SuiPortal::REASON_BAD_PACKET,
            packet.portalGuid, packet.generation, packet.revision, packet.ticket, 0);
        return;
    }

    bool correlated = packet.portalGuid == m_suiPortalGuid &&
        packet.generation == m_suiPortalGeneration &&
        packet.revision == m_suiPortalRevision &&
        packet.ticket != 0 && packet.ticket == m_suiPortalTicket;
    if (!correlated)
    {
        SendState(this, SuiPortal::STATE_BLOCKED, SuiPortal::REASON_CORRELATION,
            packet.portalGuid, packet.generation, packet.revision, packet.ticket, 0);
        return;
    }

    uint32 nowMs = WorldTimer::getMSTime();
    if (LeaseExpired(m_suiPortalIssuedAtMs, m_suiPortalLeaseMs, nowMs))
    {
        SendState(this, SuiPortal::STATE_EXPIRED, SuiPortal::REASON_LEASE_EXPIRED,
            packet.portalGuid, packet.generation, packet.revision, packet.ticket, 0);
        m_suiPortalGuid.Clear();
        m_suiPortalTicket = 0;
        m_suiPortalLeaseMs = 0;
        return;
    }

    if (packet.result == SuiPortal::LOAD_FAILED)
    {
        SendState(this, SuiPortal::STATE_FAILED, SuiPortal::REASON_CLIENT_LOAD_FAILED,
            packet.portalGuid, packet.generation, packet.revision, packet.ticket, 0);
        m_suiPortalGuid.Clear();
        m_suiPortalTicket = 0;
        m_suiPortalLeaseMs = 0;
        return;
    }

    PortalFacts facts;
    ValidationResult validation = ValidatePortal(this, packet.portalGuid, facts);
    if (validation != ValidationResult::Ok)
    {
        SuiPortal::PortalState state = validation == ValidationResult::Expired
            ? SuiPortal::STATE_EXPIRED : SuiPortal::STATE_REVOKED;
        SuiPortal::StateReason reason = validation == ValidationResult::Expired
            ? SuiPortal::REASON_OBJECT_UNAVAILABLE : SuiPortal::REASON_NOT_AUTHORIZED;
        SendState(this, state, reason, packet.portalGuid, packet.generation,
            packet.revision, packet.ticket, 0);
        m_suiPortalGuid.Clear();
        m_suiPortalTicket = 0;
        m_suiPortalLeaseMs = 0;
        return;
    }

    // Readiness is a short lease, not permission to teleport. Crossing/clicking
    // still goes through the unmodified stock game-object use handler.
    uint32 objectLifetimeMs = RemainingLifetimeMs(facts.object);
    uint32 readyLeaseMs = PORTAL_READY_LEASE_MS;
    if (objectLifetimeMs != std::numeric_limits<uint32>::max())
        readyLeaseMs = std::min(readyLeaseMs, objectLifetimeMs);
    readyLeaseMs = std::min(readyLeaseMs,
        RemainingLeaseMs(m_suiPortalIssuedAtMs, m_suiPortalLeaseMs, nowMs));

    if (readyLeaseMs == 0)
    {
        SendState(this, SuiPortal::STATE_EXPIRED, SuiPortal::REASON_LEASE_EXPIRED,
            packet.portalGuid, packet.generation, packet.revision, packet.ticket, 0);
        m_suiPortalGuid.Clear();
        m_suiPortalTicket = 0;
        m_suiPortalLeaseMs = 0;
        return;
    }

    m_suiPortalIssuedAtMs = nowMs;
    m_suiPortalLeaseMs = readyLeaseMs;
    SendState(this, SuiPortal::STATE_READY, SuiPortal::REASON_NONE,
        packet.portalGuid, packet.generation, packet.revision, packet.ticket,
        readyLeaseMs);
}
