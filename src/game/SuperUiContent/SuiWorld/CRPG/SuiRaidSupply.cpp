#include "SuiRaidSupply.h"
#include "Player.h"
#include "Item.h"
#include "Bag.h"
#include "Pet.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Chat.h"
#include "Log.h"
#include "Config/Config.h"
#include "json.hpp"
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using Json = nlohmann::json;

    struct Want { uint32 item = 0; uint32 count = 0; };
    struct Policy
    {
        bool loaded = false, repair = false;
        std::string error;
        std::vector<Want> everyone, mana, tank, healer, physical, caster, petFood;
        uint32 ammoCount = 0, arrows = 0, bullets = 0;
    };
    struct Receipt
    {
        uint32 members = 0, items = 0, repaired = 0;
        std::vector<std::string> refused; // "Name: item" - no bag room or unknown item
        std::string error;
    };

    std::mutex mutex;
    std::map<ObjectGuid, Receipt> receipts; // by commander

    std::string PolicyPath()
    {
        std::string configured = sConfig.GetStringDefault("SuiRaidSupply.Policy", "sui-raid-supply.json");
        if (!configured.empty() && configured[0] == '/') return configured;
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + configured;
    }

    std::vector<Want> Wants(Json const& j, std::string const& key, std::string& error)
    {
        std::vector<Want> out;
        if (!j.count(key)) return out;
        if (!j.at(key).is_array()) { error = key + " must be an array"; return out; }
        for (auto const& row : j.at(key))
        {
            if (!row.is_object() || !row.count("item") || !row.count("count") ||
                !row.at("item").is_number_unsigned() || !row.at("count").is_number_unsigned())
            { error = key + " rows need unsigned item and count"; return {}; }
            Want want{ row.at("item").get<uint32>(), row.at("count").get<uint32>() };
            if (!want.item || !want.count || want.count > 200 || !sObjectMgr.GetItemPrototype(want.item))
            { error = key + ": unknown item or count out of 1..200: " + std::to_string(want.item); return {}; }
            out.push_back(want);
        }
        return out;
    }

    // Read the policy fresh for every order: the owner edits it without a restart.
    Policy Load()
    {
        Policy p;
        std::string path = PolicyPath();
        std::ifstream in(path);
        // A fresh install has only the template the build installs beside mangosd.conf.dist;
        // the defaults apply until the owner creates the live file to change them.
        if (!in) { in.open(path + ".dist"); }
        if (!in) { p.error = "policy file missing: " + path + " (or its .dist template)"; return p; }
        Json j;
        try { in >> j; }
        catch (std::exception const& e) { p.error = std::string("policy parse error: ") + e.what(); return p; }
        if (!j.is_object() || !j.count("schema") || !j.at("schema").is_number_unsigned() || j.at("schema").get<uint32>() != 1)
        { p.error = "policy schema 1 required"; return p; }
        std::string error;
        p.repair = j.value("repair", false);
        p.everyone = Wants(j, "everyone", error);
        p.mana = Wants(j, "mana", error);
        p.petFood = Wants(j, "petFood", error);
        if (j.count("roles"))
        {
            Json const& roles = j.at("roles");
            if (!roles.is_object()) error = "roles must be an object";
            else
            {
                p.tank = Wants(roles, "tank", error); p.healer = Wants(roles, "healer", error);
                p.physical = Wants(roles, "physical", error); p.caster = Wants(roles, "caster", error);
            }
        }
        if (j.count("ammo"))
        {
            Json const& ammo = j.at("ammo");
            if (!ammo.is_object()) error = "ammo must be an object";
            else
            {
                p.ammoCount = ammo.value("count", 0u); p.arrows = ammo.value("arrows", 0u); p.bullets = ammo.value("bullets", 0u);
                if (p.ammoCount > 4000 || (p.arrows && !sObjectMgr.GetItemPrototype(p.arrows)) || (p.bullets && !sObjectMgr.GetItemPrototype(p.bullets)))
                    error = "ammo: unknown item or count above 4000";
            }
        }
        if (!error.empty()) { p.error = error; return p; }
        p.loaded = true;
        return p;
    }

    uint32 Carried(Player* bot, uint32 item) { return bot->GetItemCount(item, false); }

    // Top the bot up to `count` of `item` across all its bags. Returns items granted; -1 when refused.
    int32 TopUp(Player* bot, Want const& want, Receipt& receipt)
    {
        uint32 have = Carried(bot, want.item);
        if (have >= want.count) return 0;
        uint32 missing = want.count - have;
        ItemPosCountVec dest;
        InventoryResult result = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, want.item, missing);
        if (result != EQUIP_ERR_OK)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(want.item);
            receipt.refused.push_back(std::string(bot->GetName()) + ": " + (proto && proto->Name1 ? std::string(proto->Name1) : std::to_string(want.item)));
            return -1;
        }
        Item* item = bot->StoreNewItem(dest, want.item, true, Item::GenerateItemRandomPropertyId(want.item));
        if (!item) { receipt.refused.push_back(std::string(bot->GetName()) + ": " + std::to_string(want.item)); return -1; }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI][raid-supply] actor=%u item=%u granted=%u now=%u",
            bot->GetGUIDLow(), want.item, missing, Carried(bot, want.item));
        return int32(missing);
    }

    void Grant(Player* bot, std::vector<Want> const& wants, Receipt& receipt)
    {
        for (Want const& want : wants)
        {
            int32 granted = TopUp(bot, want, receipt);
            if (granted > 0) receipt.items += uint32(granted);
        }
    }

    enum Bucket : uint8 { BucketPhysical, BucketCaster, BucketTank, BucketHealer };
    Bucket Resolve(Player* bot, uint32 role)
    {
        switch (role)
        {
            case 1: case 2: return BucketTank;
            case 3: return BucketHealer;
            case 4: return BucketPhysical;
            default: break;
        }
        // Ranged or unassigned: a body that shoots is physical; a body that casts is a caster.
        if (Item* ranged = bot->GetWeaponForAttack(RANGED_ATTACK))
        {
            uint32 subclass = ranged->GetProto()->SubClass;
            if (subclass == ITEM_SUBCLASS_WEAPON_BOW || subclass == ITEM_SUBCLASS_WEAPON_GUN || subclass == ITEM_SUBCLASS_WEAPON_CROSSBOW)
                return BucketPhysical;
        }
        return bot->GetPowerType() == POWER_MANA ? BucketCaster : BucketPhysical;
    }

    void Ammunition(Player* bot, Policy const& policy, Receipt& receipt)
    {
        if (!policy.ammoCount) return;
        Item* ranged = bot->GetWeaponForAttack(RANGED_ATTACK);
        if (!ranged) return;
        uint32 subclass = ranged->GetProto()->SubClass;
        uint32 fallback = subclass == ITEM_SUBCLASS_WEAPON_GUN ? policy.bullets
            : (subclass == ITEM_SUBCLASS_WEAPON_BOW || subclass == ITEM_SUBCLASS_WEAPON_CROSSBOW) ? policy.arrows : 0;
        if (!fallback) return;
        uint32 current = bot->GetUInt32Value(PLAYER_AMMO_ID);
        // Keep the ammunition the body already uses; only a body with none gets the policy default.
        uint32 entry = current ? current : fallback;
        int32 granted = TopUp(bot, Want{ entry, policy.ammoCount }, receipt);
        if (granted > 0) receipt.items += uint32(granted);
        if (!current && Carried(bot, entry)) bot->SetAmmo(entry);
    }

    void PetFood(Player* bot, Policy const& policy, Receipt& receipt)
    {
        Pet* pet = bot->GetPet();
        if (!pet || pet->GetPetType() != HUNTER_PET || policy.petFood.empty()) return;
        for (Want const& want : policy.petFood)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(want.item);
            if (!proto || !pet->HaveInDiet(proto) || !pet->GetCurrentFoodBenefitLevel(proto->ItemLevel)) continue;
            int32 granted = TopUp(bot, want, receipt);
            if (granted > 0) receipt.items += uint32(granted);
            return; // the first food the pet eats is its ration
        }
        receipt.refused.push_back(std::string(bot->GetName()) + ": no policy pet food in its pet's diet");
    }
}

namespace SuiRaidSupply
{
    void Supply(Player* commander, Player* bot, uint32 role)
    {
        if (!commander || !bot || !bot->IsInWorld()) return;
        std::lock_guard<std::mutex> guard(mutex);
        Receipt& receipt = receipts[commander->GetObjectGuid()];
        Group* group = commander->GetGroup();
        if (!group || !group->IsLeader(commander->GetObjectGuid())) { receipt.error = "only the group leader may supply the raid"; return; }
        if (!bot->IsAlive() || bot->IsInCombat()) { receipt.refused.push_back(std::string(bot->GetName()) + ": dead or in combat"); return; }
        Policy policy = Load();
        if (!policy.loaded) { receipt.error = policy.error; return; }
        ++receipt.members;
        Grant(bot, policy.everyone, receipt);
        if (bot->GetPowerType() == POWER_MANA) Grant(bot, policy.mana, receipt);
        switch (Resolve(bot, role))
        {
            case BucketTank: Grant(bot, policy.tank, receipt); break;
            case BucketHealer: Grant(bot, policy.healer, receipt); break;
            case BucketPhysical: Grant(bot, policy.physical, receipt); break;
            case BucketCaster: Grant(bot, policy.caster, receipt); break;
        }
        Ammunition(bot, policy, receipt);
        PetFood(bot, policy, receipt);
        if (policy.repair) { bot->DurabilityRepairAll(false, 0); ++receipt.repaired; }
    }

    void Flush(Player* commander)
    {
        if (!commander || !commander->GetSession()) return;
        Receipt receipt;
        {
            std::lock_guard<std::mutex> guard(mutex);
            auto found = receipts.find(commander->GetObjectGuid());
            if (found == receipts.end()) return;
            receipt = found->second;
            receipts.erase(found);
        }
        ChatHandler handler(commander->GetSession());
        if (!receipt.error.empty()) { handler.PSendSysMessage("Raid supply refused: %s", receipt.error.c_str()); return; }
        handler.PSendSysMessage("Raid supplied: %u members, %u items granted, %u repaired.", receipt.members, receipt.items, receipt.repaired);
        if (!receipt.refused.empty())
        {
            std::string text;
            for (size_t i = 0; i < receipt.refused.size() && i < 6; ++i) text += (i ? "; " : "") + receipt.refused[i];
            if (receipt.refused.size() > 6) text += "; +" + std::to_string(receipt.refused.size() - 6) + " more";
            handler.PSendSysMessage("Not supplied (no bag room or not in the policy): %s", text.c_str());
        }
    }
}
