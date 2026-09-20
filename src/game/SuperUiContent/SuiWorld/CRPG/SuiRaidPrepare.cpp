#include "SuiRaidPrepare.h"
#include "AiBotAIMain.h"
#include "Player.h"
#include "Item.h"
#include "Bag.h"
#include "Pet.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "WorldSession.h"
#include "Chat.h"
#include "Log.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{
    enum Stage : uint8 { StageBuffs, StageElixirs, StagePotion, StageFood, StagePet, StageDone };

    struct Preparation
    {
        ObjectGuid commander;
        uint32 schoolMask = 0;
        Stage stage = StageBuffs;
        uint32 waitMs = 0;       // settle time (category cooldown, cast, eating) before the next action
        uint32 elapsedMs = 0;
        uint32 stageMs = 0;
        uint32 buffsCast = 0, elixirsUsed = 0, potionsUsed = 0;
        uint32 wellFedSpell = 0; // the aura the chosen food promises once eaten long enough
        bool ate = false, fed = false, petFed = false;
        std::set<uint64> triedBuffs;   // (spell, member) pairs that refused once; never spun on
        std::set<uint32> usedSpells;   // consumable use spells already issued this preparation
    };
    struct Batch
    {
        std::set<ObjectGuid> pending;
        uint32 ordered = 0, finished = 0, buffs = 0, elixirs = 0, potions = 0, fed = 0, petsFed = 0;
        std::vector<std::string> notFed, interrupted;
    };

    std::recursive_mutex mutex;
    std::map<ObjectGuid, Preparation> preparations; // by bot
    std::map<ObjectGuid, Batch> batches;            // by commander

    uint32 const kMaxPreparationMs = 150000;
    uint32 const kMaxStageMs = 60000;
    uint32 const kElixirSettleMs = 3500;  // elixirs and flasks share a short item category cooldown
    uint32 const kPotionSettleMs = 1500;
    uint32 const kFoodSettleMs = 20000;   // eating must run long enough for the well-fed trigger to fire
    uint32 const kPetSettleMs = 2000;
    uint32 const kBuffMinDurationMs = 5 * MINUTE * IN_MILLISECONDS;      // a raid buff, not a proc or a cooldown
    uint32 const kConsumableMinDurationMs = 15 * MINUTE * IN_MILLISECONDS; // an elixir, flask or absorb potion
    uint32 const kPotionCategoryCooldownMs = 60000;                       // the shared potion cooldown class
    float const kBuffRangeYards = 30.f;

    template <typename Fn> void ForEachCarried(Player* actor, Fn fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            fn(actor->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = dynamic_cast<Bag*>(actor->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot)))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    fn(bag->GetItemByPos(uint8(slot)));
    }

    bool AurasOnly(SpellEntry const* spell)
    {
        for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
            if (spell->Effect[e] != 0 && spell->Effect[e] != SPELL_EFFECT_APPLY_AURA)
                return false;
        return true;
    }

    // A carried consumable's on-use spell, if the item is usable by this actor right now.
    struct CarriedUse { Item* item = nullptr; uint8 spellSlot = 0; SpellEntry const* spell = nullptr; _ItemSpell const* use = nullptr; };
    template <typename Pred> void ForEachUsable(Player* actor, Pred pred, std::vector<CarriedUse>& out)
    {
        ForEachCarried(actor, [&](Item* item)
        {
            if (!item || item->IsInTrade() || actor->CanUseItem(item) != EQUIP_ERR_OK) return;
            ItemPrototype const* proto = item->GetProto();
            if (proto->Class != ITEM_CLASS_CONSUMABLE) return;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                _ItemSpell const& use = proto->Spells[i];
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(use.SpellId);
                if (use.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE || !spell) continue;
                if (!pred(proto, use, spell)) continue;
                out.push_back({ item, i, spell, &use });
            }
        });
    }

    // Ordinary item use through the session: ownership, cooldown, form, targeting and
    // consumption checks all apply exactly as for a player's click.
    bool UseCarried(Player* actor, CarriedUse const& carried)
    {
        WorldPackets::Spell::UseItem request;
        request.bagIndex = carried.item->GetBagSlot();
        request.slot = carried.item->GetSlot();
        request.spellSlot = carried.spellSlot;
        if (carried.spell->AllowedTargetMask & TARGET_FLAG_UNIT) request.targets.setUnitTarget(actor);
        uint32 const before = carried.item->GetCount();
        ObjectGuid const guid = carried.item->GetObjectGuid();
        uint32 const entry = carried.item->GetEntry(), spell = carried.spell->Id;
        actor->GetSession()->HandleUseItemOpcode(request);
        Item* after = actor->GetItemByPos(request.bagIndex, request.slot);
        uint32 const count = after && after->GetObjectGuid() == guid ? after->GetCount() : 0;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI][raid-prepare] actor=%u item=%u spell=%u before=%u after=%u",
            actor->GetGUIDLow(), entry, spell, before, count);
        // The decrement is asynchronous for a cast with a cast time; a request that was not
        // refused outright counts as issued. The aura, not the count, decides the outcome.
        return true;
    }

    bool HasAuraNamed(Unit const* unit, SpellEntry const* like)
    {
        for (auto const& holder : unit->GetSpellAuraHolderMap())
            if (SpellEntry const* active = holder.second ? holder.second->GetSpellProto() : nullptr)
                if (active->SpellName[0] == like->SpellName[0]) return true;
        return false;
    }

    // ── stages ────────────────────────────────────────────────────────────────

    // One raid buff per call: the highest learned rank of any long friendly aura in the
    // actor's own spellbook, on the first group member who lacks it. Rank, chain and
    // refresh rules are the AI's ordinary buff-target rules.
    bool CastOneBuff(AiBotAI* ai, Player* actor, Preparation& p)
    {
        if (actor->IsNonMeleeSpellCasted(false, false, true)) return true;
        std::vector<SpellEntry const*> candidates;
        for (auto const& known : actor->GetSpellMap())
        {
            if (known.second.state == PLAYERSPELL_REMOVED || !known.second.active || known.second.disabled) continue;
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(known.first);
            if (!spell || spell->IsPassiveSpell() || !spell->IsPositiveSpell() || !AurasOnly(spell)) continue;
            if (spell->EffectImplicitTargetA[0] != TARGET_UNIT_FRIEND) continue;
            if (spell->GetDuration() < int32(kBuffMinDurationMs)) continue;
            if (spell->RecoveryTime > 10000 || spell->CategoryRecoveryTime > 10000) continue; // an ability, not a buff
            candidates.push_back(spell);
        }
        std::sort(candidates.begin(), candidates.end(), [](SpellEntry const* a, SpellEntry const* b)
        {
            uint8 const ra = sSpellMgr.GetSpellRank(a->Id), rb = sSpellMgr.GetSpellRank(b->Id);
            return ra != rb ? ra > rb : a->Id < b->Id;
        });
        Group* group = actor->GetGroup();
        if (!group) return false;
        for (SpellEntry const* spell : candidates)
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (!member || !member->IsAlive() || member->IsGameMaster() || member->GetMap() != actor->GetMap()) continue;
                uint64 const key = (uint64(spell->Id) << 32) | member->GetGUIDLow();
                if (p.triedBuffs.count(key)) continue;
                if (!actor->IsWithinDist(member, kBuffRangeYards) || !actor->IsWithinLOSInMap(member)) continue;
                if (!ai->IsValidBuffTarget(member, spell) || !ai->CanTryToCastSpell(member, spell)) continue;
                p.triedBuffs.insert(key);
                if (ai->DoCastSpell(member, spell) != SPELL_CAST_OK) continue;
                ++p.buffsCast;
                p.waitMs = spell->GetCastTime(actor) + 600;
                return true;
            }
        return false;
    }

    // One elixir or flask per call: a carried consumable whose use is a long positive
    // aura-only spell on a short item category (not the potion class) that the actor lacks.
    bool DrinkOneElixir(Player* actor, Preparation& p)
    {
        std::vector<CarriedUse> uses;
        ForEachUsable(actor, [&](ItemPrototype const*, _ItemSpell const& use, SpellEntry const* spell)
        {
            return spell->IsPositiveSpell() && AurasOnly(spell) && spell->GetDuration() >= int32(kConsumableMinDurationMs) &&
                use.SpellCategoryCooldown < int32(kPotionCategoryCooldownMs) && use.SpellCooldown < int32(kPotionCategoryCooldownMs) &&
                !actor->HasAura(spell->Id) && !p.usedSpells.count(spell->Id);
        }, uses);
        for (CarriedUse const& carried : uses)
        {
            if (!actor->IsSpellReady(carried.spell->Id, carried.item->GetProto())) continue;
            p.usedSpells.insert(carried.spell->Id);
            if (!UseCarried(actor, carried)) continue;
            ++p.elixirsUsed;
            p.waitMs = kElixirSettleMs;
            return true;
        }
        return false;
    }

    // A pre-fight school-absorb potion. The commander's expected schools choose it; without
    // them, exactly one carried absorb school is read as the raid's intent and several as none.
    bool DrinkProtection(Player* actor, Preparation& p)
    {
        std::vector<CarriedUse> uses;
        ForEachUsable(actor, [&](ItemPrototype const*, _ItemSpell const& use, SpellEntry const* spell)
        {
            if (!spell->IsPositiveSpell() || spell->GetDuration() < int32(kConsumableMinDurationMs)) return false;
            if (use.SpellCategoryCooldown < int32(kPotionCategoryCooldownMs) && use.SpellCooldown < int32(kPotionCategoryCooldownMs)) return false;
            for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
                if (spell->Effect[e] == SPELL_EFFECT_APPLY_AURA && spell->EffectApplyAuraName[e] == SPELL_AURA_SCHOOL_ABSORB) return true;
            return false;
        }, uses);
        if (uses.empty()) return false;
        auto schools = [](SpellEntry const* spell)
        {
            uint32 mask = 0;
            for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
                if (spell->Effect[e] == SPELL_EFFECT_APPLY_AURA && spell->EffectApplyAuraName[e] == SPELL_AURA_SCHOOL_ABSORB)
                    mask |= uint32(spell->EffectMiscValue[e]);
            return mask;
        };
        // Already shielded against any of the wanted schools: nothing to do.
        for (auto const& holder : actor->GetSpellAuraHolderMap())
            if (SpellEntry const* active = holder.second ? holder.second->GetSpellProto() : nullptr)
                if (uint32 mask = schools(active))
                    if (!p.schoolMask || (mask & p.schoolMask)) return false;
        std::set<uint32> carriedMasks;
        for (CarriedUse const& carried : uses) carriedMasks.insert(schools(carried.spell));
        CarriedUse const* chosen = nullptr;
        for (CarriedUse const& carried : uses)
        {
            uint32 const mask = schools(carried.spell);
            bool const wanted = p.schoolMask ? (mask & p.schoolMask) != 0 : carriedMasks.size() == 1;
            if (!wanted || !actor->IsSpellReady(carried.spell->Id, carried.item->GetProto()) || p.usedSpells.count(carried.spell->Id)) continue;
            chosen = &carried; break;
        }
        if (!chosen) return false;
        p.usedSpells.insert(chosen->spell->Id);
        if (!UseCarried(actor, *chosen)) return false;
        ++p.potionsUsed;
        p.waitMs = kPotionSettleMs;
        return true;
    }

    // Eat: carried food whose use is the seated regeneration aura with a periodic trigger -
    // the trigger IS the well-fed buff. Already fed (any food's buff of that name) means skip.
    bool Eat(Player* actor, Preparation& p)
    {
        struct Food { CarriedUse carried; SpellEntry const* wellFed; uint32 itemLevel; };
        std::vector<Food> foods;
        std::vector<CarriedUse> uses;
        ForEachUsable(actor, [&](ItemPrototype const*, _ItemSpell const&, SpellEntry const* spell)
        {
            if (!spell->HasAuraInterruptFlag(AURA_INTERRUPT_STANDING_CANCELS)) return false;
            bool regen = false; uint32 trigger = 0;
            for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
            {
                if (spell->Effect[e] != SPELL_EFFECT_APPLY_AURA) continue;
                if (spell->EffectApplyAuraName[e] == SPELL_AURA_MOD_REGEN || spell->EffectApplyAuraName[e] == SPELL_AURA_OBS_MOD_HEALTH) regen = true;
                if (spell->EffectApplyAuraName[e] == SPELL_AURA_PERIODIC_TRIGGER_SPELL && spell->EffectTriggerSpell[e]) trigger = spell->EffectTriggerSpell[e];
            }
            return regen && trigger != 0 && sSpellMgr.GetSpellEntry(trigger) != nullptr;
        }, uses);
        for (CarriedUse const& carried : uses)
            for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
                if (carried.spell->EffectApplyAuraName[e] == SPELL_AURA_PERIODIC_TRIGGER_SPELL && carried.spell->EffectTriggerSpell[e])
                    foods.push_back({ carried, sSpellMgr.GetSpellEntry(carried.spell->EffectTriggerSpell[e]), carried.item->GetProto()->ItemLevel });
        if (foods.empty()) return false;
        for (Food const& food : foods)
            if (HasAuraNamed(actor, food.wellFed)) { p.fed = true; return false; }
        std::sort(foods.begin(), foods.end(), [](Food const& a, Food const& b) { return a.itemLevel > b.itemLevel; });
        for (Food const& food : foods)
        {
            if (!actor->IsSpellReady(food.carried.spell->Id, food.carried.item->GetProto())) continue;
            if (actor->IsNonMeleeSpellCasted(false, false, true)) return true;
            if (!UseCarried(actor, food.carried)) continue;
            p.ate = true;
            p.wellFedSpell = food.wellFed->Id;
            p.waitMs = kFoodSettleMs;
            return true;
        }
        return false;
    }

    // Feed the pet with carried food in its diet through the actor's own Feed Pet spell.
    bool FeedPet(Player* actor, Preparation& p)
    {
        Pet* pet = actor->GetPet();
        if (!pet || !pet->IsAlive() || pet->GetPetType() != HUNTER_PET || pet->GetHappinessState() == HAPPY) return false;
        SpellEntry const* feed = nullptr;
        for (auto const& known : actor->GetSpellMap())
        {
            if (known.second.state == PLAYERSPELL_REMOVED || !known.second.active || known.second.disabled) continue;
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(known.first);
            if (spell && spell->Effect[0] == SPELL_EFFECT_FEED_PET) { feed = spell; break; }
        }
        if (!feed || !actor->IsSpellReady(feed->Id) || actor->IsNonMeleeSpellCasted(false, false, true)) return false;
        Item* best = nullptr;
        ForEachCarried(actor, [&](Item* item)
        {
            if (!item || item->IsInTrade()) return;
            ItemPrototype const* proto = item->GetProto();
            if (proto->Class != ITEM_CLASS_CONSUMABLE || !proto->FoodType || !pet->HaveInDiet(proto)) return;
            if (!pet->GetCurrentFoodBenefitLevel(proto->ItemLevel)) return;
            if (!best || proto->ItemLevel > best->GetProto()->ItemLevel) best = item;
        });
        if (!best) return false;
        actor->SetStandState(UNIT_STAND_STATE_STAND);
        WorldPackets::Spell::CastSpell packet;
        packet.spellId = feed->Id;
        packet.targets.setItemTarget(best);
        actor->GetSession()->HandleCastSpellOpcode(packet);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI][raid-prepare] actor=%u feed-pet food=%u", actor->GetGUIDLow(), best->GetEntry());
        p.petFed = true;
        p.waitMs = kPetSettleMs;
        return true;
    }

    void Advance(Player* actor, Preparation& p)
    {
        if (p.stage == StageFood && actor->IsSittingDown()) actor->SetStandState(UNIT_STAND_STATE_STAND);
        p.stage = Stage(p.stage + 1);
        p.stageMs = 0;
        p.waitMs = 0;
    }

    void Finish(Player* actor, Preparation const& p, bool interrupted)
    {
        if (actor->IsSittingDown()) actor->SetStandState(UNIT_STAND_STATE_STAND);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI][raid-prepare] actor=%u finished stage=%u buffs=%u elixirs=%u potions=%u fed=%u pet=%u interrupted=%u ms=%u",
            actor->GetGUIDLow(), unsigned(p.stage), p.buffsCast, p.elixirsUsed, p.potionsUsed, unsigned(p.fed), unsigned(p.petFed), unsigned(interrupted), p.elapsedMs);
        auto found = batches.find(p.commander);
        if (found == batches.end()) return;
        Batch& batch = found->second;
        batch.pending.erase(actor->GetObjectGuid());
        ++batch.finished;
        batch.buffs += p.buffsCast; batch.elixirs += p.elixirsUsed; batch.potions += p.potionsUsed;
        if (p.fed) ++batch.fed; else batch.notFed.push_back(actor->GetName());
        if (p.petFed) ++batch.petsFed;
        if (interrupted) batch.interrupted.push_back(actor->GetName());
        if (!batch.pending.empty()) return;
        // The batch is complete: one receipt line to the commander, then forget it.
        if (Player* commander = sObjectMgr.GetPlayer(p.commander))
            if (commander->GetSession())
            {
                auto names = [](std::vector<std::string> const& list)
                {
                    std::string text;
                    for (size_t i = 0; i < list.size() && i < 6; ++i) text += (i ? ", " : "") + list[i];
                    if (list.size() > 6) text += ", +" + std::to_string(list.size() - 6);
                    return text;
                };
                ChatHandler handler(commander->GetSession());
                handler.PSendSysMessage("Raid prepared: %u of %u bots - %u buffs cast, %u elixirs, %u potions, %u well fed, %u pets fed.",
                    batch.finished, batch.ordered, batch.buffs, batch.elixirs, batch.potions, batch.fed, batch.petsFed);
                if (!batch.notFed.empty()) handler.PSendSysMessage("Not well fed: %s", names(batch.notFed).c_str());
                if (!batch.interrupted.empty()) handler.PSendSysMessage("Preparation interrupted: %s", names(batch.interrupted).c_str());
            }
        batches.erase(found);
    }
}

namespace SuiRaidPrepare
{
    bool Begin(Player* commander, Player* bot, AiBotAI* ai, uint32 schoolMask)
    {
        if (!commander || !bot || !ai || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() ||
            ai->IsPossessed() || bot->IsSuiTacticallyFrozen() || bot->IsBeingTeleported() || !bot->GetSession())
            return false;
        std::lock_guard<std::recursive_mutex> guard(mutex);
        if (preparations.count(bot->GetObjectGuid())) return true;
        // Stand still for the whole preparation: eating cancels on movement and a buff
        // round should not chase a walking formation.
        ai->SuiClearWaypoints();
        ai->StopMoving();
        bot->AttackStop();
        bot->GetMotionMaster()->Clear(false, true);
        bot->GetMotionMaster()->MoveIdle();
        Preparation p;
        p.commander = commander->GetObjectGuid();
        p.schoolMask = schoolMask;
        preparations[bot->GetObjectGuid()] = p;
        Batch& batch = batches[p.commander];
        batch.pending.insert(bot->GetObjectGuid());
        ++batch.ordered;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI][raid-prepare] actor=%u begin commander=%u schools=%u",
            bot->GetGUIDLow(), commander->GetGUIDLow(), schoolMask);
        return true;
    }

    bool Preparing(Player const* bot)
    {
        if (!bot) return false;
        std::lock_guard<std::recursive_mutex> guard(mutex);
        return preparations.count(bot->GetObjectGuid()) != 0;
    }

    bool Tick(AiBotAI* ai, uint32 diff)
    {
        Player* actor = ai ? ai->GetBotPlayer() : nullptr;
        if (!actor || !actor->IsInWorld()) return false;
        std::lock_guard<std::recursive_mutex> guard(mutex);
        auto it = preparations.find(actor->GetObjectGuid());
        if (it == preparations.end()) return false;
        Preparation& p = it->second;
        p.elapsedMs += diff;
        p.stageMs += diff;
        bool const interrupted = !actor->IsAlive() || actor->IsInCombat() || ai->IsPossessed() ||
            actor->IsSuiTacticallyFrozen() || actor->IsBeingTeleported() || p.elapsedMs > kMaxPreparationMs;
        if (interrupted)
        {
            Finish(actor, p, true);
            preparations.erase(it);
            return false;
        }
        if (p.waitMs > diff)
        {
            p.waitMs -= diff;
            // Eating ends the moment the promised buff lands; standing early loses it.
            if (p.stage == StageFood && p.ate && !p.fed && p.wellFedSpell && actor->HasAura(p.wellFedSpell))
            { p.fed = true; p.waitMs = 0; }
            return true;
        }
        p.waitMs = 0;
        if (p.stageMs > kMaxStageMs) Advance(actor, p);
        switch (p.stage)
        {
            case StageBuffs:   if (!CastOneBuff(ai, actor, p)) Advance(actor, p); break;
            case StageElixirs: if (!DrinkOneElixir(actor, p)) Advance(actor, p); break;
            case StagePotion:  if (!DrinkProtection(actor, p)) Advance(actor, p); break;
            case StageFood:
                if (p.ate) { if (p.wellFedSpell && actor->HasAura(p.wellFedSpell)) p.fed = true; Advance(actor, p); }
                else if (!Eat(actor, p)) Advance(actor, p);
                break;
            case StagePet:     if (!FeedPet(actor, p)) Advance(actor, p); break;
            case StageDone:
                Finish(actor, p, false);
                preparations.erase(it);
                return false;
        }
        return true;
    }
}
