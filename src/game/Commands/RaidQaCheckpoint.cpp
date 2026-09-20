// Temporary mission QA checkpoint. Capture and restore the named raid in place;
// combat remains entirely under the ordinary creature AI and raid executor.
#include "Chat.h"
#include "Log.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "InstanceData.h"
#include "Item.h"
#include "LootMgr.h"
#include "Bag.h"
#include "WorldSession.h"
#include "SpellAuras.h"
#include "MotionMaster.h"
#include "SuiCommanderRaid.h"
#include "AiBotAIMain.h"
#include "Server/Packets/Spell.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "TemporarySummon.h"
#include "json.hpp"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <set>
#include <regex>
#include <cmath>
#include <cstdio>

namespace RaidQaCheckpoint
{
using Json = nlohmann::json;
// A restore re-fights the captured creature. The native respawn (next world tick) lets an instance
// script re-roll the spawn's identity (Molten Core swaps Lava Annihilator and Firelord with even odds
// on every respawn), which would leave a different unit standing where the captured one was; the
// captured entry is re-asserted once the respawned body is alive again.
class CapturedIdentityEvent : public BasicEvent
{
public:
    CapturedIdentityEvent(Creature& owner, uint32 entry) : m_owner(owner), m_entry(entry) {}
    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        // The respawn lands on the world tick right after the restore; the event runs well after it.
        if (m_owner.IsInWorld() && m_owner.IsAlive() && m_owner.GetEntry() != m_entry)
        {
            m_owner.SetEntry(m_entry);
            m_owner.UpdateEntry(m_entry);
            m_owner.AIM_Initialize();
            m_owner.SetHealth(m_owner.GetMaxHealth());
            // Clients keep the template from the creature's creation packet: re-create the object at
            // every client so they see the captured identity too (the same trick Creature::Respawn uses).
            UnitVisibility const current = m_owner.GetVisibility();
            m_owner.SetVisibility(VISIBILITY_RESPAWN);
            m_owner.SetVisibility(current);
        }
        return true;
    }
private:
    Creature& m_owner;
    uint32 m_entry;
};
// A summoned encounter creature is called back a few seconds after the restore, once the owner's own in-place
// teleport to the captured pose has been acknowledged by the client; a synchronous summon found the owner where the
// previous fight left him and engaged him at once (v6, 2026-09-14).
class DeferredSummonEvent : public BasicEvent
{
public:
    DeferredSummonEvent(Player& owner, uint32 entry, float x, float y, float z, float o, bool state = false, uint32 unitFlags = 0, uint32 faction = 0, uint32 npcFlags = 0)
        : m_owner(owner), m_entry(entry), m_x(x), m_y(y), m_z(z), m_o(o), m_state(state), m_unitFlags(unitFlags), m_faction(faction), m_npcFlags(npcFlags) {}
    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
    {
        if (m_owner.IsInWorld())
        {
            Creature* fresh = m_owner.SummonCreature(m_entry, m_x, m_y, m_z, m_o, TEMPSUMMON_MANUAL_DESPAWN, 2 * HOUR * IN_MILLISECONDS);
            // The captured live state of a scripted summon (v8): the template gives a fresh body its spawn flags (Ragnaros
            // spawns immune to players and is only opened by his summoning event; a yielded Majordomo is a friendly gossip
            // NPC), so the restore puts back the flags, faction and NPC flags the captured body had.
            if (fresh && m_state)
            {
                fresh->SetUInt32Value(UNIT_FIELD_FLAGS, m_unitFlags);
                fresh->SetFactionTemplateId(m_faction);
                fresh->SetUInt32Value(UNIT_NPC_FLAGS, m_npcFlags);
            }
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI][raid-checkpoint] deferred summon entry=%u guid=%llu state=%u flags=%u faction=%u npcFlags=%u", m_entry, static_cast<unsigned long long>(fresh ? fresh->GetObjectGuid().GetRawValue() : 0ull), unsigned(m_state), m_unitFlags, m_faction, m_npcFlags);
        }
        return true;
    }
private:
    Player& m_owner;
    uint32 m_entry;
    float m_x, m_y, m_z, m_o;
    bool m_state;
    uint32 m_unitFlags, m_faction, m_npcFlags;
};
bool Member(uint32 id) { return id == 787 || (id >= 115 && id <= 142) || (id >= 150 && id <= 160); }
bool Label(std::string const& s) { return std::regex_match(s, std::regex("[A-Za-z0-9_-]{1,64}")); }
void Require(bool value, char const* reason) { if (!value) throw std::runtime_error(reason); }
Json Pose(Unit* u) { return Json::array({u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), u->GetOrientation()}); }
void ValidatePose(Json const& p)
{
    Require(p.is_array() && p.size() == 4, "invalid pose");
    for (auto const& n : p) Require(n.is_number() && std::isfinite(n.get<double>()) && std::fabs(n.get<double>()) < 17000, "invalid coordinate");
}
std::map<uint32, Player*> Roster(bool restoring = false, bool readingNormal = false)
{
    std::map<uint32, Player*> result;
    Player* owner = ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER, uint32(787)));
    Require(owner && owner->GetGroup() && owner->GetGroup()->GetMembersCount() == 40, "exact forty-member raid required");
    for (GroupReference* i = owner->GetGroup()->GetFirstMember(); i; i = i->next())
    {
        Player* p = i->getSource();
        std::ostringstream rosterState;
        rosterState << "raid member readiness: guid=" << (p ? p->GetGUIDLow() : 0);
        if (p) rosterState << " world=" << p->IsInWorld() << " combat=" << p->IsInCombat()
            << " teleport=" << p->IsBeingTeleported() << " frozen=" << p->IsSuiTacticallyFrozen()
            << " executor=" << SuiCommanderRaid::Owns(p) << " session=" << bool(p->GetSession())
            << " selfActor=" << bool(p->GetSession() && p->GetSession()->GetSuiActor() == p);
        Require(p && Member(p->GetGUIDLow()) && p->IsInWorld() && (restoring || !p->IsInCombat()) && !p->IsBeingTeleported() &&
            !p->IsSuiTacticallyFrozen() && (readingNormal || !SuiCommanderRaid::Owns(p)) && p->GetSession() && p->GetSession()->GetSuiActor() == p,
            rosterState.str().c_str());
        result.emplace(p->GetGUIDLow(), p);
    }
    Require(result.size() == 40, "wrong or duplicate roster");
    return result;
}
void Normal(Player* p, bool permitOwnerGm = false)
{
    Require((!p->IsGameMaster() || (permitOwnerGm && p->GetGUIDLow() == 787)) && !p->GetCheatOptions(), "normal rules required: GM/cheats enabled");
    Require(p->GetInvincibilityHpThreshold() == 0, "normal rules required: invincibility HP threshold is nonzero");
    if (p->GetClass() == CLASS_PRIEST) Require(p->GetRace() == RACE_DWARF && p->HasSpell(6346), "every priest must be dwarf with learned Fear Ward");
}

// Raid loot (owner rule, 2026-09-15): every accepted boss hands the raid one kill's worth of loot. The kill is rolled by the
// server's own creature loot template (the group, chance, reference and count processing a corpse gets); the drops are
// handed out and worn under the ordinary equip rules. Setup only - no creature, fight or progression is touched (v9).
Json RollKillLoot(Player* owner, uint32 entry)
{
    CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);
    Require(info && info->loot_id, "creature entry has no loot template");
    Loot loot(nullptr);
    Require(loot.FillLoot(info->loot_id, LootTemplates_Creature, nullptr, true, true), "loot template missing");
    Json items = Json::array(), quest = Json::array();
    for (LootItem const& item : loot.items)
        items.push_back({{"item", item.itemid}, {"count", uint32(item.count)}, {"randomProperty", item.randomPropertyId},
            {"condition", uint32(item.conditionId)}, {"allowedForOwner", item.AllowedForPlayer(owner, nullptr)}});
    for (LootItem const& item : loot.m_questItems) quest.push_back({{"item", item.itemid}, {"count", uint32(item.count)}});
    return {{"entry", entry}, {"lootId", info->loot_id}, {"items", items}, {"questItems", quest}};
}
// A boss whose kill hands its loot through a chest instead of a corpse (Majordomo's Cache of the Firelord, gameobject
// 179703): roll the server's own GAMEOBJECT loot template exactly as the creature roll above does (v11). Setup only - the
// live chest, its state and the instance are untouched.
Json RollObjectLoot(Player* owner, uint32 entry)
{
    GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(entry);
    Require(info && info->GetLootId(), "gameobject entry has no loot template");
    Loot loot(nullptr);
    Require(loot.FillLoot(info->GetLootId(), LootTemplates_Gameobject, nullptr, true, true), "loot template missing");
    Json items = Json::array(), quest = Json::array();
    for (LootItem const& item : loot.items)
        items.push_back({{"item", item.itemid}, {"count", uint32(item.count)}, {"randomProperty", item.randomPropertyId},
            {"condition", uint32(item.conditionId)}, {"allowedForOwner", item.AllowedForPlayer(owner, nullptr)}});
    for (LootItem const& item : loot.m_questItems) quest.push_back({{"item", item.itemid}, {"count", uint32(item.count)}});
    return {{"entry", entry}, {"lootId", info->GetLootId()}, {"items", items}, {"questItems", quest}};
}
Json EquipLoot(Player* p, uint32 entry, uint32 slot)
{
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
    Require(proto && proto->InventoryType != INVTYPE_NON_EQUIP && slot < EQUIPMENT_SLOT_END, "equippable item and equipment slot required");
    Require(p->IsAlive() && !p->IsInCombat(), "equip requires a living member out of combat");
    uint16 dest = 0;
    Require(p->CanEquipNewItem(uint8(slot), dest, entry, true) == EQUIP_ERR_OK && (dest & 0xFF) == slot, "ordinary equip rules refuse this item in this slot");
    Json previous = nullptr;
    if (Item* old = p->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot)))
    {
        ItemPosCountVec bags;
        Require(p->CanUnequipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, false) == EQUIP_ERR_OK &&
            p->CanStoreItem(NULL_BAG, NULL_SLOT, bags, old, false) == EQUIP_ERR_OK, "the replaced item cannot go to the bags");
        previous = {{"entry", old->GetEntry()}, {"itemGuid", old->GetGUIDLow()}};
        p->RemoveItem(INVENTORY_SLOT_BAG_0, uint8(slot), true);
        p->StoreItem(bags, old, true);
    }
    Require(p->CanEquipNewItem(uint8(slot), dest, entry, false) == EQUIP_ERR_OK, "equip refused after the slot was emptied");
    Item* item = p->EquipNewItem(dest, entry, true);
    Require(item, "equip failed");
    p->AutoUnequipOffhandIfNeed();
    p->SaveInventoryAndGoldToDB();
    return {{"entry", entry}, {"slot", slot}, {"itemGuid", item->GetGUIDLow()}, {"previous", previous}};
}
Json GrantLoot(Player* p, uint32 entry, uint32 count)
{
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
    Require(proto && count >= 1 && count <= 20, "known item and a count of 1-20 required");
    Item* item = p->StoreNewItemInInventorySlot(entry, count);
    Require(item, "no bag space for the granted item");
    p->SaveInventoryAndGoldToDB();
    return {{"entry", entry}, {"count", count}, {"itemGuid", item->GetGUIDLow()}};
}

Json PrepareBlessing(std::map<uint32, Player*> const& members, std::string const& token,
                     uint32 casterGuid, uint32 spellId, uint32 targetGuid)
{
    Require(members.count(casterGuid) && members.count(targetGuid), "blessing caster/target outside raid");
    Player* caster = members.at(casterGuid); Player* target = members.at(targetGuid);
    for (auto const& pair : members)
    {
        Normal(pair.second, true);
        Require(pair.second->IsAlive() && pair.second->GetMap() == caster->GetMap(), "blessing requires living raid in one instance");
    }
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    Require(spell && caster->HasActiveSpell(spellId) && !spell->IsPassiveSpell() &&
        Spells::GetSpellSpecific(spellId) == SPELL_BLESSING && spell->GetDuration() >= 60000,
        "ordinary learned maintenance blessing required");
    static std::map<std::string, Json> requests;
    Json identity = {{"caster", casterGuid}, {"spell", spellId}, {"target", targetGuid}};
    auto found = requests.find(token);
    if (found != requests.end()) Require(found->second.at("identity") == identity, "blessing token collision");
    else
    {
        Require(requests.size() < 4096, "preparation receipt capacity reached");
        Require(caster->IsSpellReady(spellId) && !caster->HasGCD(spell) &&
            !caster->IsNonMeleeSpellCasted(false, false, true), "blessing caster busy; no cast requested");
        uint32 reagent = spell->Reagent[0] > 0 ? uint32(spell->Reagent[0]) : 0;
        Json receipt = {{"identity", identity}, {"reagent", reagent},
            {"before", reagent ? caster->GetItemCount(reagent, false) : 0}};
        // Journal before the normal spell ingress; a retry observes this request,
        // never emits a second cast. The handler retains learned-spell, range,
        // LOS, GCD, mana, reagent and ordinary cast-result validation.
        requests.emplace(token, receipt);
        caster->SetStandState(UNIT_STAND_STATE_STAND);
        WorldPackets::Spell::CastSpell packet; packet.spellId = spellId;
        packet.targets.setUnitTarget(target);
        caster->GetSession()->HandleCastSpellOpcode(packet);
        found = requests.find(token);
    }
    Json receipt = found->second;
    uint32 reagent = receipt.at("reagent");
    receipt["after"] = reagent ? caster->GetItemCount(reagent, false) : 0;
    unsigned expected = 0, applied = 0;
    for (auto const& pair : members) if (pair.second->GetClass() == target->GetClass())
    { ++expected; applied += pair.second->HasAura(spellId); }
    receipt["expected"] = expected; receipt["applied"] = applied;
    return receipt;
}


Item* CheckpointCarriedItem(Player* player, uint32 entry)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetEntry() == entry) return item;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Bag* bag = dynamic_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            for (uint32 at = 0; at < bag->GetBagSize(); ++at)
                if (Item* item = bag->GetItemByPos(at)) if (item->GetEntry() == entry) return item;
    return nullptr;
}
Json PreparePetFeed(std::map<uint32, Player*> const& members, std::string const& token,
                    uint32 casterGuid, uint32 spellId, uint32 foodEntry)
{
    Require(members.count(casterGuid), "feed owner outside raid");
    Player* caster = members.at(casterGuid); Pet* pet = caster->GetPet();
    for (auto const& pair : members)
    {
        Normal(pair.second, true);
        Require(pair.second->IsAlive() && pair.second->GetMap() == caster->GetMap(), "feed requires living raid in one instance");
    }
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    Require(spell && caster->HasActiveSpell(spellId) && !spell->IsPassiveSpell() &&
        spell->Effect[0] == SPELL_EFFECT_FEED_PET && pet && pet->IsAlive() &&
        pet->GetPetType() == HUNTER_PET && pet->GetMap() == caster->GetMap(), "normal learned Feed Pet and living owned hunter pet required");
    static std::map<std::string, Json> requests;
    Json identity = {{"caster", casterGuid}, {"spell", spellId}, {"food", foodEntry}, {"pet", pet->GetObjectGuid().GetRawValue()}};
    auto found = requests.find(token);
    if (found != requests.end()) Require(found->second.at("identity") == identity, "feed token collision or pet changed");
    else
    {
        Require(requests.size() < 4096, "preparation receipt capacity reached");
        Require(caster->IsSpellReady(spellId) && !caster->HasGCD(spell) &&
            !caster->IsNonMeleeSpellCasted(false, false, true), "feed caster busy; no cast requested");
        Item* food = CheckpointCarriedItem(caster, foodEntry);
        Require(food, "carried pet food required");
        requests.emplace(token, Json{{"identity", identity}, {"before", caster->GetItemCount(foodEntry, false)}, {"happinessBefore", pet->GetPower(POWER_HAPPINESS)}});
        caster->SetStandState(UNIT_STAND_STATE_STAND);
        WorldPackets::Spell::CastSpell packet; packet.spellId = spellId;
        packet.targets.setItemTarget(food);
        caster->GetSession()->HandleCastSpellOpcode(packet);
        found = requests.find(token);
    }
    Json receipt = found->second;
    receipt["after"] = caster->GetItemCount(foodEntry, false);
    receipt["happiness"] = pet->GetPower(POWER_HAPPINESS);
    receipt["maxHappiness"] = pet->GetMaxPower(POWER_HAPPINESS);
    return receipt;
}

Json Gear(Player* p)
{
    Json result = Json::array();
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        result.push_back(item ? Json::array({slot, item->GetGUIDLow(), item->GetEntry(), item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)}) : Json::array({slot, 0, 0, 0}));
    }
    return result;
}
// Temporary pre-fight setup rule (owner, 2026-09-09): cosmetic slots never decide
// whether a saved raid may be restored. A shirt or tabard changes no combat fact.
Json GearIdentity(Json gear)
{
    for (auto& row : gear)
    {
        uint32 slot = row.at(0).get<uint32>();
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD) { row[1] = 0; row[2] = 0; row[3] = 0; }
    }
    return gear;
}
Json Supplies(Player* p)
{
    std::map<uint32, uint32> counts;
    auto add = [&](Item* item)
    {
        if (item && (item->GetProto()->Class == ITEM_CLASS_CONSUMABLE || item->GetProto()->Class == ITEM_CLASS_PROJECTILE ||
            item->GetProto()->Class == ITEM_CLASS_REAGENT)) counts[item->GetEntry()] += item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot) add(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Bag* bag = dynamic_cast<Bag*>(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            for (uint32 at = 0; at < bag->GetBagSize(); ++at) add(bag->GetItemByPos(at));
    Json result = Json::array();
    for (auto const& i : counts) result.push_back({{"item", i.first}, {"count", i.second}});
    return result;
}
Json Auras(Unit* u)
{
    Json result = Json::array();
    for (auto const& pair : u->GetSpellAuraHolderMap())
    {
        SpellAuraHolder* h = pair.second;
        if (h->IsPassive() || !h->IsPositive() || h->GetSpellProto()->IsChanneledSpell()) continue;
        // Area effects are recreated by the captured caster; never duplicate a foreign area emitter.
        Json effects = Json::array(); bool skip = false;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (Aura* a = h->GetAuraByEffectIndex(SpellEffectIndex(i)))
            {
                uint32 type = h->GetSpellProto()->EffectApplyAuraName[i];
                if ((a->IsAreaAura() && h->GetCasterGuid() != u->GetObjectGuid()) || type == SPELL_AURA_MOD_POSSESS ||
                    type == SPELL_AURA_MOD_CHARM || type == SPELL_AURA_BIND_SIGHT || type == SPELL_AURA_FAR_SIGHT || type == SPELL_AURA_AOE_CHARM) skip = true;
                effects.push_back({{"index", i}, {"amount", a->GetModifier()->m_amount}, {"period", a->GetModifier()->periodictime}});
            }
        if (!skip && !effects.empty()) result.push_back({{"spell", h->GetId()}, {"caster", h->GetCasterGuid().GetRawValue()},
            {"item", h->GetCastItemGuid().GetRawValue()}, {"stacks", h->GetStackAmount()}, {"charges", h->GetAuraCharges()},
            {"maxDuration", h->GetAuraMaxDuration()}, {"duration", h->GetAuraDuration()}, {"effects", effects}});
    }
    return result;
}
void RestoreAuras(Unit* u, Json const& auras)
{
    std::set<uint32> remove;
    for (auto const& pair : u->GetSpellAuraHolderMap()) if (!pair.second->IsPassive()) remove.insert(pair.second->GetId());
    for (uint32 spell : remove) u->RemoveAurasDueToSpell(spell);
    for (auto const& a : auras)
    {
        auto proto = sSpellMgr.GetSpellEntry(a.at("spell").get<uint32>());
        auto h = CreateSpellAuraHolder(proto, u, nullptr, nullptr);
        int32 maxDuration = a.at("maxDuration").get<int32>(), duration = a.at("duration").get<int32>();
        // Temporary pre-fight setup rule (owner, 2026-09-09): a restored buff is refreshed to
        // its full duration so a checkpoint captured hours ago never expires mid-fight.
        if (maxDuration > 0) duration = maxDuration;
        h->SetLoadedState(ObjectGuid(a.at("caster").get<uint64>()), ObjectGuid(a.at("item").get<uint64>()),
            a.at("stacks").get<uint32>(), a.at("charges").get<uint32>(), maxDuration, duration);
        for (auto const& e : a.at("effects"))
        {
            auto index = SpellEffectIndex(e.at("index").get<uint8>());
            auto aura = CreateAura(proto, index, nullptr, h, u);
            aura->SetLoadedState(e.at("amount").get<float>(), e.at("period").get<uint32>());
            h->AddAura(aura, index);
        }
        if (h->GetCasterGuid() != u->GetObjectGuid() && h->IsSingleTarget()) h->SetIsSingleTarget(false);
        Require(u->AddSpellAuraHolder(h), "aura restore failed; pull forbidden");
    }
}
Json PetSpells(Pet* pet)
{
    Json spells = Json::array();
    for (auto const& s : pet->m_petSpells)
        if (s.second.state != PETSPELL_REMOVED) spells.push_back({{"spell", s.first}, {"active", s.second.active}, {"type", s.second.type}});
    return spells;
}
void RestorePetLoyalty(Pet* pet, Json const& saved)
{
    if (pet->GetPetType() != HUNTER_PET) return;
    // Legacy snapshots omit loyalty. A recreated hunter starts at the native
    // initial level; do not invent the previous pet's advanced loyalty.
    uint32 loyalty = saved.value("loyalty", std::max(uint32(REBELLIOUS), pet->GetLoyaltyLevel()));
    Require(loyalty >= REBELLIOUS && loyalty <= BEST_FRIEND, "invalid saved hunter loyalty");
    pet->SetLoyaltyLevel(LoyaltyLevel(loyalty));
}
void RestorePetSpells(Pet* pet, Json const& spells)
{
    std::set<uint32> wanted;
    for (auto const& s : spells) wanted.insert(s.at("spell").get<uint32>());
    std::vector<uint32> extra;
    for (auto const& s : pet->m_petSpells) if (!wanted.count(s.first)) extra.push_back(s.first);
    for (uint32 s : extra) pet->RemoveSpell(s, false);
    for (auto const& s : spells)
    {
        uint32 spell = s.at("spell");
        pet->AddSpell(spell, ActiveStates(s.at("active").get<uint32>()), PETSPELL_NEW, PetSpellType(s.at("type").get<uint32>()));
        pet->ToggleAutocast(spell, s.at("active").get<uint32>() == ACT_ENABLED);
    }
    pet->CleanupActionBar();
}
Json Powers(Unit* u)
{
    Json result = Json::array();
    for (uint8 i = 0; i < MAX_POWERS; ++i) result.push_back(u->GetPower(::Powers(i)));
    return result;
}
// Explicit checkpoint recovery retires engagement memory as well as motion.
// The normal sticky-assist bridge otherwise recognizes a respawned same-GUID
// creature as the previous fight and immediately starts it again.
void ResetCheckpointJourney(AiBotAI* ai)
{
    ai->SuiAbandonJourney();
    ai->m_doctrineKind = ResolveDoctrine(*ai);
    ai->m_doctrine = MakeDoctrine(ai->m_doctrineKind);
    ai->m_lastVictimEntry = 0; ai->m_lastVictimGuidLow = 0;
    ai->m_lootTimer = 0; ai->m_lootTargetGuid.Clear();
    ai->m_suiUnlinked = true; ai->m_suiRtsHold = true;
    ai->SuiStopFollowForHold(); ai->StopMoving();
}
void RestoreVitals(Unit* u, Json const& row)
{
    u->SetHealth(u->GetMaxHealth());
    for (uint8 i = 0; i < MAX_POWERS; ++i) u->SetPower(::Powers(i), std::min(u->GetMaxPower(::Powers(i)), row.at("powers").at(i).get<uint32>()));
}
void ValidateAuras(Json const& auras)
{
    Require(auras.is_array() && auras.size() <= 128, "invalid auras");
    for (auto const& a : auras)
    {
        auto proto = sSpellMgr.GetSpellEntry(a.at("spell").get<uint32>());
        Require(proto && !proto->IsChanneledSpell() && a.at("stacks").get<uint32>() >= 1 && a.at("stacks").get<uint32>() <= 255, "invalid aura spell/stack");
        Require(a.at("effects").is_array() && a.at("effects").size() <= MAX_EFFECT_INDEX, "invalid aura effects");
        std::set<uint8> indexes;
        for (auto const& e : a.at("effects"))
        {
            auto i = e.at("index").get<uint8>();
            Require(i < MAX_EFFECT_INDEX && proto->EffectApplyAuraName[i] && indexes.insert(i).second, "invalid aura effect");
            Require(std::isfinite(e.at("amount").get<float>()), "invalid aura amount");
            e.at("period").get<uint32>();
        }
        a.at("caster").get<uint64>(); a.at("item").get<uint64>(); a.at("charges").get<uint32>();
        a.at("maxDuration").get<int32>(); a.at("duration").get<int32>();
    }
}
void Validate(Json const& data)
{
    Require(data.at("schema") == 1 && data.at("owner") == 787 && data.at("instance").get<uint32>() != 0, "wrong checkpoint schema/scope");
    Require(data.at("members").is_array() && data.at("members").size() == 40, "checkpoint must contain exact forty");
    std::set<uint32> ids;
    for (auto const& row : data.at("members"))
    {
        auto id = row.at("guid").get<uint32>();
        Require(Member(id) && ids.insert(id).second, "foreign/duplicate checkpoint member");
        ValidatePose(row.at("pose")); ValidateAuras(row.at("auras"));
        Require(row.at("powers").size() == MAX_POWERS, "invalid powers");
        for (auto const& power : row.at("powers")) power.get<uint32>();
        row.at("ammo").get<uint32>(); row.at("class").get<uint32>(); row.at("race").get<uint32>(); row.at("level").get<uint32>();
        Require(row.at("gear").is_array(), "invalid gear");
        std::set<uint32> items;
        for (auto const& item : row.at("supplies"))
        {
            uint32 entry = item.at("item").get<uint32>(), count = item.at("count").get<uint32>();
            auto p = sObjectMgr.GetItemPrototype(entry);
            Require(p && items.insert(entry).second && count <= 100000 && (p->Class == ITEM_CLASS_CONSUMABLE || p->Class == ITEM_CLASS_PROJECTILE || p->Class == ITEM_CLASS_REAGENT), "invalid checkpoint supply");
        }
        if (!row.at("pet").is_null())
        {
            auto const& pet = row.at("pet"); ValidateAuras(pet.at("auras"));
            pet.at("number").get<uint32>(); pet.at("entry").get<uint32>();
            if (pet.count("loyalty") && pet.at("type").get<uint32>() == uint32(HUNTER_PET)) Require(pet.at("loyalty").get<uint32>() >= REBELLIOUS && pet.at("loyalty").get<uint32>() <= BEST_FRIEND, "invalid saved hunter loyalty");
            if (pet.count("spells"))
            {
                Require(pet.at("spells").is_array() && pet.at("spells").size() <= 128 && pet.at("type").get<uint32>() <= 1 && pet.at("level").get<uint32>() <= 60, "invalid saved pet");
                pet.at("name").get<std::string>(); pet.at("createdBy").get<uint32>();
                for (auto const& s : pet.at("spells")) Require(sSpellMgr.GetSpellEntry(s.at("spell").get<uint32>()), "invalid pet spell");
            }
            Require(pet.at("powers").size() == MAX_POWERS, "invalid pet powers");
            for (auto const& p : pet.at("powers")) p.get<uint32>();
        }
    }
    Require(data.at("creatures").is_array() && !data.at("creatures").empty() && data.at("creatures").size() <= 32, "selected encounter creatures required");
    std::set<uint64> creatures;
    for (auto const& c : data.at("creatures"))
    {
        Require(creatures.insert(c.at("guid").get<uint64>()).second && c.at("entry").get<uint32>() > 0 && c.at("maxHealth").get<uint32>() > 0, "duplicate/invalid creature");
        ValidatePose(c.at("home"));
    }
    data.at("map").get<uint32>();
}
}

bool ChatHandler::HandleGroupQaCheckpointCommand(char* args)
{
    using namespace RaidQaCheckpoint;
    try
    {
        std::istringstream input(args ? args : ""); std::string mode, label; input >> mode >> label;
        Require(Label(label) && (mode == "capture" || mode == "capture-encounter" || mode == "restore" || mode == "restore-raid" || mode == "status" || mode == "return" || mode == "normal" || mode == "bless" || mode == "feed" || mode == "loot" || mode == "loot-object" || mode == "equip" || mode == "grant" || mode == "rebase" || mode == "respawn"), "usage: qacheckpoint capture|capture-encounter|restore|restore-raid|status|return|normal|bless|feed|loot|loot-object|equip|grant|rebase|respawn LABEL [creature raw GUIDs for capture]");
        bool const captureEncounter = mode == "capture-encounter";
        if (captureEncounter) mode = "capture";
        std::string path = "raid-qa-checkpoint-" + label + ".json";
        auto members = Roster(mode == "restore" || mode == "return" || mode == "normal", mode == "normal"); Player* owner = members.at(787);
        if (mode == "feed")
        {
            uint32 caster = 0, spell = 0, food = 0; std::string extra;
            Require(bool(input >> caster >> spell >> food) && !(input >> extra), "feed TOKEN CASTER SPELL FOOD required");
            Json receipt = PreparePetFeed(members, label, caster, spell, food);
            std::string message = "QA_PET_FEED " + label + " " + receipt.dump();
            SendSysMessage(message.c_str()); return true;
        }
        if (mode == "rebase")
        {
            // Owner roster and loot changes (2026-09-15, v10): a new immutable checkpoint that is the source checkpoint
            // with every member's worn gear taken from the live character. A member whose class/race/level changed (a
            // roster slot given to a new character) takes its buffs, supplies, ammo, powers and pet from a named template
            // member of the same live class in the source. Poses, creatures and instance state stay the source's.
            std::string source; Require(bool(input >> source) && Label(source) && source != label, "rebase NEW SOURCE [MEMBER:TEMPLATE ...] required");
            std::map<uint32, uint32> templates; std::string token;
            while (input >> token)
            {
                uint32 member = 0, templ = 0; char colon = 0; std::istringstream pair(token);
                Require(bool(pair >> member >> colon >> templ) && colon == ':' && Member(member) && Member(templ) && member != templ &&
                    templates.emplace(member, templ).second, "template pairs are MEMBER:TEMPLATE");
            }
            Require(!std::ifstream(path).good(), "immutable checkpoint already exists; use a fresh label");
            Json data;
            { std::ifstream file("raid-qa-checkpoint-" + source + ".json"); Require(bool(file), "source checkpoint not found"); file >> data; }
            Validate(data);
            std::map<uint32, Json> sourceRows;
            for (auto const& row : data.at("members")) sourceRows.emplace(row.at("guid").get<uint32>(), row);
            Json rebased = Json::array(), changed = Json::array();
            for (auto const& row : data.at("members"))
            {
                uint32 const id = row.at("guid").get<uint32>(); Player* p = members.at(id); Normal(p, true);
                Json next = row;
                bool const identity = p->GetClass() != row.at("class").get<uint32>() || p->GetRace() != row.at("race").get<uint32>() || p->GetLevel() != row.at("level").get<uint32>();
                Require(identity == (templates.count(id) == 1), "a MEMBER:TEMPLATE pair is required exactly for members whose class/race/level changed");
                if (identity)
                {
                    Json const& t = sourceRows.at(templates.at(id));
                    Require(t.at("class").get<uint32>() == p->GetClass() && t.at("level").get<uint32>() == p->GetLevel(), "template must match the member's live class and level");
                    for (char const* key : {"supplies", "ammo", "powers", "pet", "auras"}) next[key] = t.at(key);
                    uint64 const templateGuid = ObjectGuid(HIGHGUID_PLAYER, t.at("guid").get<uint32>()).GetRawValue();
                    for (auto& aura : next["auras"]) if (aura.at("caster").get<uint64>() == templateGuid) aura["caster"] = p->GetObjectGuid().GetRawValue();
                    next["class"] = p->GetClass(); next["race"] = p->GetRace(); next["level"] = p->GetLevel();
                }
                Json const gear = Gear(p);
                if (identity || GearIdentity(gear) != GearIdentity(row.at("gear"))) changed.push_back(id);
                next["gear"] = gear; rebased.push_back(next);
            }
            data["members"] = rebased; data["created"] = uint64(time(nullptr)); data["rebasedFrom"] = source;
            Validate(data);
            // The rebased characters keep the source's raid binding, as a capture binds them.
            if (Map* map = sMapMgr.FindMap(data.at("map").get<uint32>(), data.at("instance").get<uint32>()))
                if (auto state = dynamic_cast<DungeonPersistentState*>(map->GetPersistentState()))
                    for (auto const& pair : members)
                    {
                        auto bind = pair.second->GetBoundInstance(map->GetId());
                        Require(!bind || !bind->perm || bind->state == state, "different permanent raid binding; rebase refused");
                        pair.second->BindToInstance(state, true);
                    }
            std::ofstream out(path + ".tmp", std::ios::trunc); out << data.dump(2); out.close();
            Require(bool(out) && std::rename((path + ".tmp").c_str(), path.c_str()) == 0, "checkpoint file write failed");
            std::string message = "QA_CHECKPOINT rebase " + label + " from=" + source + " exact40 changed=" + changed.dump();
            SendSysMessage(message.c_str()); return true;
        }
        if (mode == "loot" || mode == "loot-object")
        {
            uint32 entry = 0; std::string extra;
            Require(bool(input >> entry) && !(input >> extra), "loot TOKEN CREATURE_ENTRY | loot-object TOKEN GAMEOBJECT_ENTRY required");
            std::string message = "QA_LOOT " + label + " " +
                (mode == "loot" ? RollKillLoot(owner, entry) : RollObjectLoot(owner, entry)).dump();
            SendSysMessage(message.c_str()); return true;
        }
        // Bring named static instance spawns back, by raw GUID (v12). Two states need it and neither has any other cure:
        // a spawn the INSTANCE SCRIPT pruned while its encounter was flagged done - Molten Core removes every Ancient
        // Core Hound once Magmadar is DONE, and a pruned spawn carries no respawn time, so nothing returns it - and a
        // spawn killed by GM staging setup, which is then gone for its whole database timer (2-6 h in Molten Core).
        // GM `.respawn` cannot do this: it walks the GRID (Cell::VisitGridObjects) and a despawned creature is out of
        // the grid though still addressable in the map's object store. This is the same reload-and-respawn the restore
        // path already performs for a checkpoint's own creatures, exposed for creatures no checkpoint holds yet, so a
        // pack can be captured and then fought normally. Setup only: no loot, no kill credit, no encounter state.
        if (mode == "respawn")
        {
            Map* map = owner->GetMap();
            Require(map && map->GetInstanceId(), "respawn must be issued by the owner inside the instance");
            Json done = Json::array();
            uint64 guid;
            while (input >> guid)
            {
                ObjectGuid id(guid);
                Creature* c = map->GetCreature(id);
                if (!c)
                {
                    Require(sObjectMgr.GetCreatureData(id.GetCounter()), "creature has no database spawn");
                    Creature* reloaded = new Creature;
                    if (!reloaded->LoadFromDB(id.GetCounter(), map)) { delete reloaded; Require(false, "creature could not be reloaded"); }
                    map->Add(reloaded); c = map->GetCreature(id);
                }
                Require(c && c->HasStaticDBSpawnData(), "respawn needs a static instance spawn");
                if (c->AI()) c->AI()->EnterEvadeMode();
                if (c->IsAlive()) c->ForcedDespawn();
                c->Respawn();
                done.push_back({{"guid", guid}, {"entry", c->GetEntry()}});
            }
            Require(input.eof(), "invalid creature GUID");
            std::string message = "QA_RESPAWN " + label + " " + done.dump();
            SendSysMessage(message.c_str()); return true;
        }
        if (mode == "equip" || mode == "grant")
        {
            uint32 member = 0, entry = 0, amount = 0; std::string extra;
            Require(bool(input >> member >> entry >> amount) && !(input >> extra), "equip TOKEN MEMBER ITEM SLOT | grant TOKEN MEMBER ITEM COUNT required");
            Require(members.count(member) == 1, "loot recipient must be a raid member");
            Player* p = members.at(member); Normal(p, true);
            Json receipt = mode == "equip" ? EquipLoot(p, entry, amount) : GrantLoot(p, entry, amount);
            receipt["member"] = member;
            std::string message = std::string(mode == "equip" ? "QA_EQUIP " : "QA_GRANT ") + label + " " + receipt.dump();
            SendSysMessage(message.c_str()); return true;
        }
        if (mode == "bless")
        {
            uint32 caster = 0, spell = 0, target = 0; std::string extra;
            Require(bool(input >> caster >> spell >> target) && !(input >> extra), "bless TOKEN CASTER SPELL TARGET required");
            Json receipt = PrepareBlessing(members, label, caster, spell, target);
            std::string message = "QA_BLESSING " + label + " " + receipt.dump();
            SendSysMessage(message.c_str());
            return true;
        }
        if (mode == "normal")
        {
            std::string extra; Require(!(input >> extra), "unexpected normal-gate arguments");
            unsigned priests = 0, pets = 0;
            for (auto const& pair : members)
            {
                Normal(pair.second, true); priests += pair.second->GetClass() == CLASS_PRIEST;
                if (Pet* pet = pair.second->GetPet())
                {
                    Require(pet->GetInvincibilityHpThreshold() == 0, "pet invincibility threshold is nonzero"); ++pets;
                }
            }
            Require(priests == 6 && pets == 7, "normal gate requires all six priests and seven pets");
            SendSysMessage("QA_NORMAL exact40 invincibility=0 cheats=0 priests=6 pets=7; owner GM staging permitted, GM-off acknowledgment required before combat");
            return true;
        }
        Json data; std::string summonedReceipt;
        struct PendingSummon { uint32 entry; float x, y, z, o; bool state; uint32 unitFlags, faction, npcFlags; }; std::vector<PendingSummon> pendingSummons;
        if (mode == "capture")
        {
            Require(!std::ifstream(path).good(), "immutable checkpoint already exists; use a fresh label");
            Map* map = owner->GetMap();
            Require(map && map->GetInstanceId(), "capture must be at the boss inside its instance");
            for (auto const& ref : map->GetPlayers()) Require(ref.getSource() && Member(ref.getSource()->GetGUIDLow()), "unrelated player in instance");
            data = {{"schema", 1}, {"owner", 787}, {"map", map->GetId()}, {"instance", map->GetInstanceId()}, {"created", uint64(time(nullptr))}, {"members", Json::array()}, {"creatures", Json::array()}};
            unsigned priests = 0;
            for (auto const& pair : members)
            {
                Player* p = pair.second; Normal(p);
                Require(p->GetMap() == map && p->IsAlive() && p->IsFullHealth(), "capture requires all forty alive/full in the same instance");
                priests += p->GetClass() == CLASS_PRIEST;
                Require(p->GetPowerType() != POWER_MANA || p->GetPower(POWER_MANA) == p->GetMaxPower(POWER_MANA), "capture requires full mana");
                Json pet = nullptr;
                if (Pet* q = p->GetPet())
                {
                    Require(q->IsAlive() && q->IsFullHealth() && !q->IsInCombat(), "pet must be alive/full/out of combat");
                    pet = {{"entry", q->GetEntry()}, {"number", q->GetCharmInfo()->GetPetNumber()}, {"type", q->GetPetType()}, {"loyalty", q->GetLoyaltyLevel()}, {"level", q->GetLevel()}, {"name", q->GetName()}, {"createdBy", q->GetUInt32Value(UNIT_CREATED_BY_SPELL)}, {"spells", PetSpells(q)}, {"powers", RaidQaCheckpoint::Powers(q)}, {"auras", Auras(q)}};
                }
                data["members"].push_back({{"guid", pair.first}, {"class", p->GetClass()}, {"race", p->GetRace()}, {"level", p->GetLevel()},
                    {"pose", Pose(p)}, {"gear", Gear(p)}, {"supplies", Supplies(p)}, {"ammo", p->GetUInt32Value(PLAYER_AMMO_ID)},
                    {"powers", RaidQaCheckpoint::Powers(p)}, {"auras", Auras(p)}, {"pet", pet}});
            }
            Require(priests == 6, "exact six dwarf priests required");
            uint64 guid;
            while (input >> guid)
            {
                Creature* c = map->GetCreature(ObjectGuid(guid));
                // A scripted encounter a player summons (Molten Core's Majordomo, called by the last rune) has no
                // database spawn: it is captured as a temporary summon and a restore re-summons it at its home.
                bool const summoned = c && c->IsTemporarySummon() && !c->HasStaticDBSpawnData();
                Require(c && !c->IsPet() && (c->HasStaticDBSpawnData() || summoned) && c->IsAlive() && c->IsFullHealth() && !c->IsInCombat(), "selected creature must be a full, idle static instance spawn or temporary summon");
                float x, y, z, o; c->GetRespawnCoord(x, y, z, &o);
                data["creatures"].push_back({{"guid", guid}, {"entry", c->GetEntry()}, {"maxHealth", c->GetMaxHealth()}, {"home", Json::array({x,y,z,o})}, {"summoned", summoned}});
                if (summoned) // the live state a fresh summon does not get from its template (v8)
                {
                    data["creatures"].back()["unitFlags"] = c->GetUInt32Value(UNIT_FIELD_FLAGS);
                    data["creatures"].back()["faction"] = c->GetFactionTemplateId();
                    data["creatures"].back()["npcFlags"] = c->GetUInt32Value(UNIT_NPC_FLAGS);
                }
            }
            Require(input.eof(), "invalid creature GUID");
            // An encounter checkpoint also records the instance script's own encounter/progression
            // state (its persistence string). Restoring it must bring the boss's linked adds and
            // flags back to the captured state, or the second pull fights a different encounter.
            // Plain trash checkpoints carry no state: restoring one later must not regress progression.
            if (captureEncounter) { InstanceData* instanceData = map->GetInstanceData(); Require(instanceData, "instance script state unavailable"); data["instanceState"] = std::string(instanceData->Save()); }
            Validate(data);
            auto state = dynamic_cast<DungeonPersistentState*>(map->GetPersistentState());
            Require(state, "raid instance state required");
            for (auto const& pair : members)
            {
                auto bind = pair.second->GetBoundInstance(map->GetId());
                Require(!bind || !bind->perm || bind->state == state, "different permanent raid binding; capture refused");
            }
            // Native raid bindings keep this same instance across a development restart.
            // Only the exact authorized characters are bound; no worldstate is copied.
            for (auto const& pair : members) pair.second->BindToInstance(state, true);
            std::ofstream out(path + ".tmp", std::ios::trunc); out << data.dump(2); out.close();
            Require(bool(out) && std::rename((path + ".tmp").c_str(), path.c_str()) == 0, "checkpoint file write failed");
        }
        else
        {
            std::string extra; Require(!(input >> extra), "unexpected checkpoint arguments");
            std::ifstream file(path); Require(bool(file), "checkpoint not found"); file >> data; Validate(data);
            // Instance ids are renumbered by resets and restarts. The saved id is a hint;
            // the owner's own instance of the saved map is the live authority.
            Map* map = sMapMgr.FindMap(data.at("map").get<uint32>(), data.at("instance").get<uint32>());
            if (!map && owner->GetMapId() == data.at("map").get<uint32>() && owner->GetMap() && owner->GetMap()->GetInstanceId()) map = owner->GetMap();
            Require(map, "checkpoint instance not loaded; move the owner onto the saved map first");
            data["instance"] = map->GetInstanceId();
            for (auto const& ref : map->GetPlayers()) Require(ref.getSource() && Member(ref.getSource()->GetGUIDLow()), "unrelated player in instance");
            // Validate every member and selected creature before the first mutation.
            for (auto const& row : data.at("members"))
            {
                Player* p = members.at(row.at("guid").get<uint32>()); Normal(p);
                Require(GearIdentity(Gear(p)) == GearIdentity(row.at("gear")) && p->GetClass() == row.at("class") && p->GetRace() == row.at("race") && p->GetLevel() == row.at("level"), "character gear/class/race/level changed; capture a new baseline");
                if (mode == "return")
                {
                    auto bind = p->GetBoundInstance(map->GetId());
                    Require(!bind || !bind->perm || bind->state == map->GetPersistentState(), "different permanent raid binding; return refused");
                }
                else Require(p->GetMap() == map, "member not in checkpoint instance; use return then restore");
                if (Pet* pet = p->GetPet())
                {
                    bool same = !row.at("pet").is_null() && pet->GetEntry() == row.at("pet").at("entry") &&
                        pet->GetPetType() == row.at("pet").value("type", p->GetClass() == CLASS_HUNTER ? uint32(HUNTER_PET) : uint32(SUMMON_PET));
                    if (!same || (mode != "restore" && mode != "return" && pet->IsInCombat()))
                        throw std::runtime_error("changed/fighting pet owner=" + std::to_string(p->GetGUIDLow()) + " entry=" + std::to_string(pet->GetEntry()) + " number=" + std::to_string(pet->GetCharmInfo()->GetPetNumber()) + " type=" + std::to_string(pet->GetPetType()));
                }
            }
            std::vector<Creature*> creatures;
            // Re-apply a captured encounter state before touching creatures: instance scripts remove
            // linked adds and trash on create/respawn once their boss is flagged done, so the flags
            // must precede both the reload of a pruned spawn and the respawn below.
            if ((mode == "restore" || mode == "return") && data.count("instanceState") && data.at("instanceState").is_string())
            {
                InstanceData* instanceData = map->GetInstanceData(); Require(instanceData, "instance script state unavailable");
                std::string const saved = data.at("instanceState").get<std::string>();
                instanceData->Load(saved.c_str()); instanceData->SaveToDB();
            }
            // Character-only recovery must not load, evade or respawn completed trash.
            if (mode != "restore-raid")
            for (auto const& row : data.at("creatures"))
            {
                // A saved instance may outlive its unloaded creature grid after
                // restart. Load the captured home before validating live identity.
                auto const& home = row.at("home");
                map->ForceLoadGridsAroundPosition(home[0].get<float>(), home[1].get<float>());
                ObjectGuid const guid(row.at("guid").get<uint64>());
                Creature* c = map->GetCreature(guid);
                if (row.value("summoned", false))
                {
                    // A summoned encounter: whatever the previous fight left of it (an evaded body, a yielded and
                    // relocated one) is unsummoned together with its own summons, and a fresh one is called at the
                    // captured home by the owner - the same call the instance script made. Its GUID changes on
                    // every restore, so the receipt names the new one.
                    Require(mode == "restore" || mode == "return", "a summoned creature is only restored by restore/return");
                    uint32 const entry = row.at("entry").get<uint32>();
                    // Every temporary summon of the entry near the captured home goes, not only the captured GUID: an
                    // instance script may summon its own copy (Molten Core re-fires the DONE runes on grid load after a
                    // restart), and each body takes its living summons with it. No evade first: a scripted Reset
                    // re-summons the adds on the body's next update, before the deferred unsummon removes it (v3/v4).
                    std::list<Creature*> bodies;
                    {
                        struct SummonedOfEntry { uint32 entry; bool operator()(Creature* other) const { return other->IsInWorld() && other->GetEntry() == entry && other->IsTemporarySummon(); } } bodyCheck{entry};
                        MaNGOS::CreatureListSearcher<SummonedOfEntry> bodySearcher(bodies, bodyCheck);
                        Cell::VisitGridObjects(home[0].get<float>(), home[1].get<float>(), map, bodySearcher, 300.f);
                    }
                    if (c && std::find(bodies.begin(), bodies.end(), c) == bodies.end()) bodies.push_back(c);
                    for (Creature* body : bodies)
                    {
                        std::list<Creature*> nearby;
                        MaNGOS::AnyUnitInObjectRangeCheck check(body, 200.f);
                        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
                        Cell::VisitGridObjects(body, searcher, 200.f);
                        for (Creature* other : nearby)
                            if (other != body && other->IsTemporarySummon() && static_cast<TemporarySummon*>(other)->GetSummonerGuid() == body->GetObjectGuid())
                                other->DespawnOrUnsummon();
                        body->DespawnOrUnsummon();
                    }
                    summonedReceipt += " unsummoned=" + std::to_string(bodies.size());
                    // The fresh summon waits until every member stands at its captured pose (below): summoned here it
                    // would engage whoever still stands where the previous fight ended (v5, 2026-09-14).
                    bool const state = row.count("unitFlags") && row.count("faction") && row.count("npcFlags");
                    pendingSummons.push_back({entry, home[0].get<float>(), home[1].get<float>(), home[2].get<float>(), home[3].get<float>(), state,
                        state ? row.at("unitFlags").get<uint32>() : 0u, state ? row.at("faction").get<uint32>() : 0u, state ? row.at("npcFlags").get<uint32>() : 0u});
                    continue;
                }
                if (!c && (mode == "restore" || mode == "return") && data.count("instanceState"))
                {
                    // The instance script pruned this static spawn while its encounter was done; the
                    // captured state is back in force, so reload the spawn from its database row.
                    Require(sObjectMgr.GetCreatureData(guid.GetCounter()), "pruned creature has no database spawn");
                    Creature* reloaded = new Creature;
                    if (!reloaded->LoadFromDB(guid.GetCounter(), map)) { delete reloaded; Require(false, "pruned creature could not be reloaded"); }
                    map->Add(reloaded); c = map->GetCreature(guid);
                }
                // A template with a level range re-rolls its level, and so its maximum health, on every
                // respawn (a restore respawns the captured creature), so the captured value identifies
                // only fixed-level spawns; guid, static spawn row and entry identify the rest.
                bool const fixedLevel = c && c->GetCreatureInfo()->level_min == c->GetCreatureInfo()->level_max;
                Require(c && c->HasStaticDBSpawnData() && c->GetEntry() == row.at("entry") && (mode == "restore" || mode == "return" || !c->IsInCombat()) && (!fixedLevel || c->GetMaxHealth() == row.at("maxHealth")), "selected creature unavailable, changed or still fighting");
                creatures.push_back(c);
            }
            // An invalidated pull may displace one member out of the instance.
            // Retire the captured fight before returning that member; the same
            // exact roster, exclusive instance and cleared-executor guards apply.
            if (mode == "restore" || mode == "return")
            {
                for (Creature* c : creatures)
                {
                    if (c->AI()) c->AI()->EnterEvadeMode();
                    // Evade can leave a living creature travelling home while
                    // the restored raid is already present. Retire that body
                    // before restoring players; native respawn resets it at home
                    // on the next world tick, including in-flight movement.
                    if (c->IsAlive()) c->ForcedDespawn();
                    c->Respawn();
                    // The respawn tick may re-roll the entry (instance script); keep the captured identity.
                    c->m_Events.AddEvent(new CapturedIdentityEvent(*c, c->GetEntry()), c->m_Events.CalculateTime(1500));
                }
            }
            if (mode == "return")
            {
                auto state = dynamic_cast<DungeonPersistentState*>(map->GetPersistentState());
                Require(state, "raid instance state required");
                for (auto const& pair : members) pair.second->BindToInstance(state, true);
                for (auto const& row : data.at("members"))
                {
                    Player* p = members.at(row.at("guid").get<uint32>()); auto const& pos = row.at("pose");
                    // Return only crosses instance boundaries. Re-teleporting
                    // the members already here floods the owner's socket with
                    // redundant roster/visibility snapshots during its worldport.
                    // The separate restore phase applies every saved local pose.
                    if (p->GetMap() == map) continue;
                    Require(p->TeleportTo(map->GetId(), pos[0], pos[1], pos[2], pos[3]), "return teleport failed; restore/pull forbidden");
                }
            }
            if (mode == "restore" || mode == "restore-raid")
            {
                for (auto const& row : data.at("members"))
                {
                    Player* p = members.at(row.at("guid").get<uint32>());
                    // Retire the previous fight's AI journey before resurrection.
                    // Unit movement alone does not clear queued AI attack/path intent.
                    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(p->AI()))
                        ResetCheckpointJourney(ai);
                    p->CombatStopWithPets(true); p->ClearTarget();
                    if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
                    // A member can be alive with full health yet still flagged a ghost (its spirit-release
                    // flags survive a resurrection that did not transition from a dead state): the client then
                    // reports it not-alive and a later stage's readiness gate never passes. Clear the ghost
                    // state of every restored member the way the ghost aura's own removal does (2026-09-14, v7).
                    p->RemoveSpellsCausingAura(SPELL_AURA_GHOST);
                    p->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
                    p->RemoveByteFlag(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_VIS_FLAG, UNIT_VIS_FLAGS_GHOST);
                    p->InterruptNonMeleeSpells(false); p->AttackStop(); p->GetMotionMaster()->Clear(); p->StopMoving();
                    p->DurabilityRepairAll(false, 0); p->RemoveAllCooldowns(); RestoreAuras(p, row.at("auras")); RestoreVitals(p, row);
                    for (auto const& supply : row.at("supplies"))
                    {
                        uint32 entry = supply.at("item"), wanted = supply.at("count"), have = p->GetItemCount(entry, false);
                        if (have < wanted) Require(p->StoreNewItemInInventorySlot(entry, wanted - have), "supply restore failed; pull forbidden");
                        if (have > wanted) p->DestroyItemCount(entry, have - wanted, true);
                    }
                    p->SetAmmo(row.at("ammo"));
                    auto const& pos = row.at("pose");
                    if (!row.at("pet").is_null())
                    {
                        if (!p->GetPet())
                        {
                            p->ResummonPetTemporaryUnSummonedIfAny();
                            if (!p->GetPet())
                            {
                                auto const& savedPet = row.at("pet");
                                p->EffectSummonPet(savedPet.value("createdBy", uint32(0)), savedPet.at("entry"), savedPet.value("level", p->GetLevel()));
                                Pet* recreated = p->GetPet(); Require(recreated, "native pet recreation failed; pull forbidden");
                                recreated->SetPetType(PetType(savedPet.value("type", p->GetClass() == CLASS_HUNTER ? uint32(HUNTER_PET) : uint32(SUMMON_PET))));
                                recreated->InitStatsForLevel(savedPet.value("level", p->GetLevel()), p);
                                if (recreated->GetPetType() == HUNTER_PET)
                                {
                                    recreated->SetPowerType(POWER_FOCUS);
                                    recreated->SetMaxPower(POWER_HAPPINESS, recreated->GetCreatePowers(POWER_HAPPINESS));
                                }
                                if (savedPet.count("name")) recreated->SetName(savedPet.at("name").get<std::string>());
                            }
                        }
                        Pet* pet = p->GetPet(); Require(pet, "pet missing; pull forbidden");
                        if (!pet->IsAlive()) pet->SetDeathState(ALIVE);
                        RestorePetLoyalty(pet, row.at("pet"));
                        if (row.at("pet").count("spells")) RestorePetSpells(pet, row.at("pet").at("spells"));
                        p->PetSpellInitialize();
                        pet->RemoveAllCooldowns(); RestoreAuras(pet, row.at("pet").at("auras")); RestoreVitals(pet, row.at("pet"));
                        pet->NearTeleportTo(pos[0], pos[1], pos[2], pos[3]);
                    }
                    // Load a missing pet before putting its owner in teleport transit:
                    // native LoadPetFromDB deliberately defers current pets during transit.
                    Require(p->NearTeleportTo(pos[0], pos[1], pos[2], pos[3]), "in-place teleport failed; pull forbidden");
                }
            }
        }
        for (auto const& pending : pendingSummons)
        {
            owner->m_Events.AddEvent(new DeferredSummonEvent(*owner, pending.entry, pending.x, pending.y, pending.z, pending.o, pending.state, pending.unitFlags, pending.faction, pending.npcFlags), owner->m_Events.CalculateTime(3000));
            summonedReceipt += " summoned=" + std::to_string(pending.entry) + ":deferred3000";
        }
        std::ostringstream out; out << "QA_CHECKPOINT " << mode << ' ' << label << " exact40 map=" << data.at("map") << " instance=" << data.at("instance") << " creatures=" << data.at("creatures").size() << summonedReceipt;
        // This is a setup receipt, never a fight result. Fresh client pre-pull gates remain mandatory.
        SendSysMessage(out.str().c_str()); return true;
    }
    catch (std::exception const& e)
    {
        std::string message = std::string("QA_CHECKPOINT_REJECT ") + e.what(); SendSysMessage(message.c_str()); return false;
    }
}
