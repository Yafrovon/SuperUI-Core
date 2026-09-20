// Mission QA only: character preparation through ordinary gameplay methods.
// No encounter IDs, encounter mutation, SQL, save restore or combat execution.
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Item.h"
#include "Bag.h"
#include "WorldSession.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SuiCommanderRaid.h"
#include <map>
#include <set>
#include <sstream>
#include <regex>
#include <cmath>
#include <vector>

namespace
{
    bool QaMember(uint32 id) { return id==787||(id>=115&&id<=142)||(id>=150&&id<=160); }
    std::map<uint32,Player*> QaRoster()
    {
        std::map<uint32,Player*> members;
        Player* owner=ObjectAccessor::FindPlayer(ObjectGuid(HIGHGUID_PLAYER,uint32(787)));
        if(!owner||!owner->GetGroup()||owner->GetGroup()->GetMembersCount()!=40)return {};
        for(GroupReference* i=owner->GetGroup()->GetFirstMember();i;i=i->next())
        {
            Player* p=i->getSource();
            if(!p||!QaMember(p->GetGUIDLow())||p->IsInCombat()||p->IsBeingTeleported()||
                p->IsSuiTacticallyFrozen()||SuiCommanderRaid::Owns(p)||!p->GetSession()||
                p->GetSession()->GetSuiActor()!=p)return {};
            members.emplace(p->GetGUIDLow(),p);
        }
        if(members.size()!=40)return {};
        return members;
    }
    Item* QaCarriedItem(Player* p,uint32 entry)
    {
        for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)
            if(Item* item=p->GetItemByPos(INVENTORY_SLOT_BAG_0,slot))if(item->GetEntry()==entry)return item;
        for(uint8 slot=INVENTORY_SLOT_BAG_START;slot<INVENTORY_SLOT_BAG_END;++slot)
            if(Bag* bag=dynamic_cast<Bag*>(p->GetItemByPos(INVENTORY_SLOT_BAG_0,slot)))
                for(uint32 at=0;at<bag->GetBagSize();++at)
                    if(Item* item=bag->GetItemByPos(at))if(item->GetEntry()==entry)return item;
        return nullptr;
    }
    bool QaSupplies(ChatHandler* handler,std::string const& request)
    {
        auto reject=[&](char const* reason){handler->SendSysMessage(reason);return false;};
        auto members=QaRoster();
        if(members.empty())return reject("QA_SUPPLY_REJECT roster, combat, transit, possession or active plan");
        for(auto const& pair:members)if(!pair.second->IsAlive()||pair.second->IsNonMeleeSpellCasted(false,false,true))
            return reject("QA_SUPPLY_REJECT dead or casting member");
        std::istringstream input(request);std::string mode,token,part;input>>mode>>token;
        if((mode!="stock"&&mode!="bag"&&mode!="inspect")||!std::regex_match(token,std::regex("[A-Za-z0-9_-]{1,64}")))return false;
        struct Receipt{std::string request,text;bool success=false;};
        static std::map<std::string,Receipt> receipts;
        auto prior=receipts.find(token);
        if(prior!=receipts.end())
        {
            if(prior->second.request!=request)return reject("QA_SUPPLY_REJECT token collision");
            handler->SendSysMessage(prior->second.text.c_str());return prior->second.success;
        }
        struct Row{Player* player;uint32 item,value,before;uint16 destination;};
        std::vector<Row> rows;std::set<uint32> seen;
        while(input>>part)
        {
            std::smatch match;
            if(!std::regex_match(part,match,std::regex("([0-9]{1,10}):([0-9]{1,10}):([0-9]{1,10})")))return false;
            uint64 guid=std::stoull(match[1]),entry=std::stoull(match[2]),value=std::stoull(match[3]);
            if(guid>UINT32_MAX||entry>UINT32_MAX||value>UINT32_MAX||!members.count(guid)||!seen.insert(guid).second||rows.size()>=8)
                return reject("QA_SUPPLY_REJECT actor, duplicate or range");
            Player* player=members.at(guid);auto proto=sObjectMgr.GetItemPrototype(entry);
            if(!proto||player->CanUseItem(proto)!=EQUIP_ERR_OK)return reject("QA_SUPPLY_REJECT item eligibility");
            uint32 before=player->GetItemCount(entry,false);uint16 destination=0;
            if(mode=="bag")
            {
                if(proto->Class!=ITEM_CLASS_CONTAINER||proto->SubClass!=0||proto->InventoryType!=INVTYPE_BAG||!proto->ContainerSlots||
                    value<INVENTORY_SLOT_BAG_START||value>=INVENTORY_SLOT_BAG_END||player->GetItemByPos(INVENTORY_SLOT_BAG_0,value))
                    return reject("QA_SUPPLY_REJECT general bag and empty slot required");
                Item* carried=QaCarriedItem(player,entry);
                if(carried&&(!dynamic_cast<Bag*>(carried)||!static_cast<Bag*>(carried)->IsEmpty()))
                    return reject("QA_SUPPLY_REJECT carried bag contains items");
                if(player->CanEquipItem(NULL_SLOT,destination,proto,carried,false)!=EQUIP_ERR_OK||
                    destination!=uint16((INVENTORY_SLOT_BAG_0<<8)|value)||player->GetItemByPos(destination))
                    return reject("QA_SUPPLY_REJECT normal equip destination differs");
                if(!carried)
                {
                    ItemPosCountVec positions;
                    if(player->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,1)!=EQUIP_ERR_OK)
                        return reject("QA_SUPPLY_REJECT no space for ordinary bag grant");
                }
            }
            else
            {
                if(proto->Class!=ITEM_CLASS_CONSUMABLE||proto->InventoryType!=INVTYPE_NON_EQUIP)
                    return reject("QA_SUPPLY_REJECT consumable required");
                if(mode=="stock")
                {
                    if(!value||value>1000)return reject("QA_SUPPLY_REJECT stock target");
                    if(before<value)
                    {
                        ItemPosCountVec positions;
                        if(player->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,value-before)!=EQUIP_ERR_OK)
                            return reject("QA_SUPPLY_REJECT insufficient native capacity");
                    }
                }
                else if((value&&!sSpellMgr.GetSpellEntry(value))||!sSpellMgr.GetSpellEntry(proto->Spells[0].SpellId))
                    return reject("QA_SUPPLY_REJECT unknown aura or use spell");
            }
            rows.push_back({player,uint32(entry),uint32(value),before,destination});
        }
        if(rows.empty())return false;
        if(mode!="inspect"&&receipts.size()>=4096)return reject("QA_SUPPLY_REJECT receipt capacity");
        // Every row is validated before any grant; one item per distinct actor prevents overbooking.
        std::ostringstream output;output<<"QA_SUPPLIES "<<token<<" mode="<<mode;bool success=true;
        for(auto const& row:rows)
        {
            Player* player=row.player;
            if(mode=="stock")
            {
                if(row.before<row.value&&!player->StoreNewItemInInventorySlot(row.item,row.value-row.before))success=false;
                uint32 after=player->GetItemCount(row.item,false);success&=after>=row.value;
                output<<"\nQA_STOCK "<<player->GetGUIDLow()<<' '<<row.item<<' '<<row.before<<' '<<after<<' '<<row.value;
            }
            else if(mode=="bag")
            {
                Item* item=QaCarriedItem(player,row.item);
                if(!item)item=player->StoreNewItemInInventorySlot(row.item,1);
                if(!item){success=false;output<<"\nQA_BAG_INCOMPLETE "<<player->GetGUIDLow();continue;}
                uint32 guid=item->GetGUIDLow();
                WorldPackets::Item::AutoEquipItem packet;packet.srcbag=item->GetBagSlot();packet.srcslot=item->GetSlot();
                player->GetSession()->HandleAutoEquipItemOpcode(packet);
                Item* equipped=player->GetItemByPos(row.destination);
                bool observed=equipped&&equipped->GetGUIDLow()==guid&&equipped->GetEntry()==row.item;
                success&=observed;
                output<<"\nQA_BAG "<<player->GetGUIDLow()<<' '<<row.item<<' '<<guid<<' '<<row.value<<' '<<observed;
            }
            else
            {
                auto proto=sObjectMgr.GetItemPrototype(row.item);auto spell=sSpellMgr.GetSpellEntry(proto->Spells[0].SpellId);
                output<<"\nQA_SUPPLY "<<player->GetGUIDLow()<<' '<<row.item<<' '<<player->GetItemCount(row.item,false)<<' '
                    <<row.value<<' '<<(!row.value||player->HasAura(row.value))<<' '<<player->IsSpellReady(spell->Id,proto);
            }
        }
        std::string text=output.str();
        if(mode!="inspect")receipts.emplace(token,Receipt{request,text,success});
        handler->SendSysMessage(text.c_str());return success;
    }

}

bool ChatHandler::HandleGroupQaElixirsCommand(char* args)
{
    std::string const requestText=args?args:"";
    std::string const mode=requestText.substr(0,requestText.find(' '));
    if(mode=="stock"||mode=="bag"||mode=="inspect")return QaSupplies(this,requestText);
    auto members=QaRoster();
    if(members.empty()){SendSysMessage("QA_BATCH_REJECT roster, combat, transit, possession or active plan");return false;}
    std::string request=args?args:"",token,part;std::istringstream in(request);
    in>>token;
    if(!std::regex_match(token,std::regex("[A-Za-z0-9_-]{1,64}")))return false;
    // Repeat delivery returns its first result; it never consumes another item.
    struct ObservedUse{uint32 guid,item,aura,before;};
    struct Receipt{std::string request,sealed;std::vector<ObservedUse> uses;};
    static std::map<std::string,Receipt> receipts;
    auto observe=[&](Receipt& receipt)
    {
        if(!receipt.sealed.empty())return receipt.sealed;
        std::ostringstream out;out<<"QA_BATCH "<<token;bool complete=!receipt.uses.empty();
        for(auto const& use:receipt.uses)
        {
            Player* p=members.at(use.guid);uint32 after=p->GetItemCount(use.item,false);bool aura=p->HasAura(use.aura);
            complete&=use.before>0&&after==use.before-1&&aura;
            out<<"\nQA_ITEM "<<use.guid<<' '<<use.item<<' '<<use.aura<<' '<<use.before<<' '<<after<<' '<<aura;
        }
        if(complete)receipt.sealed=out.str();
        return out.str();
    };
    auto prior=receipts.find(token);
    if(prior!=receipts.end())
    {
        if(prior->second.request!=request){SendSysMessage("QA_BATCH_REJECT token reused with different request");return false;}
        SendSysMessage(observe(prior->second).c_str());return true;
    }
    for(auto const& pair:members)if(!pair.second->IsAlive()||pair.second->IsNonMeleeSpellCasted(false,false,true))
    {SendSysMessage("QA_BATCH_REJECT dead or casting member");return false;}
    if(receipts.size()>=4096){SendSysMessage("QA_BATCH_REJECT receipt capacity");return false;}
    struct Use{Player* p;uint32 item,aura;};std::vector<Use> uses;std::set<uint32> seen;
    while(in>>part)
    {
        std::smatch match;
        if(!std::regex_match(part,match,std::regex("([0-9]{1,10}):([0-9]{1,10}):([0-9]{1,10})")))return false;
        uint64 g=std::stoull(match[1]),item=std::stoull(match[2]),aura=std::stoull(match[3]);
        if(g>UINT32_MAX||item>UINT32_MAX||aura>UINT32_MAX||!members.count(g)||!seen.insert(g).second||uses.size()>=8)return false;
        auto proto=sObjectMgr.GetItemPrototype(item);Player* p=members[g];
        if(!proto||proto->Class!=ITEM_CLASS_CONSUMABLE||proto->InventoryType!=INVTYPE_NON_EQUIP||
            proto->Spells[0].SpellId!=aura||proto->Spells[0].SpellTrigger!=ITEM_SPELLTRIGGER_ON_USE||
            proto->Spells[0].SpellCharges!=-1||p->IsShapeShifted())
        {SendSysMessage("QA_BATCH_REJECT invalid consumable or form");return false;}
        if(p->CanUseItem(proto)!=EQUIP_ERR_OK){SendSysMessage("QA_BATCH_REJECT item eligibility");return false;}
        uses.push_back({p,uint32(item),uint32(aura)});
    }
    if(uses.empty())return false;
    auto& receipt=receipts[token];receipt.request=request;
    for(auto const& use:uses)
    {
        Player* p=use.p;Item* item=QaCarriedItem(p,use.item);
        if(!item)item=p->StoreNewItemInInventorySlot(use.item,1);
        if(!item){receipt.sealed="QA_BATCH_INCOMPLETE "+token+" inventory";SendSysMessage(receipt.sealed.c_str());return false;}
        uint32 before=p->GetItemCount(use.item,false);
        receipt.uses.push_back({p->GetGUIDLow(),use.item,use.aura,before});
        p->SetStandState(UNIT_STAND_STATE_STAND);
        WorldPackets::Spell::UseItem packet;
        packet.bagIndex=item->GetBagSlot();packet.slot=item->GetSlot();packet.spellSlot=0;
        auto spellInfo=sSpellMgr.GetSpellEntry(use.aura);
        if(spellInfo&&(spellInfo->AllowedTargetMask&TARGET_FLAG_UNIT))packet.targets.setUnitTarget(p);
        p->GetSession()->HandleUseItemOpcode(packet); // All normal item and spell checks apply.

    }
    SendSysMessage(observe(receipt).c_str());return true;
}

bool ChatHandler::HandleGroupQaRepairCommand(char* args)
{
    bool recover=false,relocate=false;float x=0,y=0,z=0;std::string mode,extra;std::istringstream input(args?args:"");
    if(input>>mode)
    {
        if(mode=="recover-here") {if(input>>extra)return false;recover=true;}
        else
        {
            if(mode!="recover"||!(input>>x>>y>>z)||(input>>extra)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||
                std::fabs(x)>17000||std::fabs(y)>17000||std::fabs(z)>17000)return false;
            recover=true;relocate=true;
        }
    }
    auto members=QaRoster();
    if(members.empty()){SendSysMessage("QA_REPAIR_REJECT roster, combat, transit, possession or active plan");return false;}
    for(auto const& pair:members)if(!recover&&!pair.second->IsAlive())
    {SendSysMessage("QA_REPAIR_REJECT dead member");return false;}
    for(auto const& pair:members)
    {
        Player* p=pair.second;
        if(recover)
        {
            if(!p->IsAlive()){p->ResurrectPlayer(1.f);p->SpawnCorpseBones();}
            // Only the explicit legacy coordinate mode relocates. In-place recovery
            // preserves the current instance, completed creatures and loot.
            if(relocate&&!p->TeleportTo(0,x,y,z,0)){SendSysMessage("QA_REPAIR_INCOMPLETE teleport refused");return false;}
        }
        p->DurabilityRepairAll(false,0);p->RemoveAllCooldowns();
        if(recover){p->SetHealth(p->GetMaxHealth());if(p->GetPowerType()==POWER_MANA)p->SetPower(POWER_MANA,p->GetMaxPower(POWER_MANA));}
    }
    SendSysMessage(recover&&!relocate?"QA_REPAIR exact40 recovered in place, repaired and cooldowns cleared":"QA_REPAIR exact40 repaired and cooldowns cleared");return true;
}
