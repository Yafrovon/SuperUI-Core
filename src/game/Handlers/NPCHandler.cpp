/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "GossipDef.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "Pet.h"
#include "Spell.h"
#include "Chat.h"
#include "CharacterDatabaseCache.h"
#include "SuiHero.h"
#include "SuiPossess.h"      // [SUI] P4b: GetSuiActor + ResnapshotControlled for possessed-bot trainer/repair
#include "SuiTacticalFreeze.h"

enum StableResultCode
{
    STABLE_ERR_MONEY        = 0x01,                         // "you don't have enough money"
    STABLE_ERR_STABLE       = 0x06,                         // currently used in most fail cases
    STABLE_SUCCESS_STABLE   = 0x08,                         // stable success
    STABLE_SUCCESS_UNSTABLE = 0x09,                         // unstable/swap success
    STABLE_SUCCESS_BUY_SLOT = 0x0A,                         // buy slot success
};

void WorldSession::HandleTabardVendorActivateOpcode(WorldPackets::Npc::TabardVendorActivate const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_TABARDDESIGNER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleTabardVendorActivateOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    SendTabardVendorActivate(packet.guid);
}

void WorldSession::SendTabardVendorActivate(ObjectGuid guid)
{
    WorldPacket data(MSG_TABARDVENDOR_ACTIVATE, 8);
    data << ObjectGuid(guid);
    SendPacket(&data);
}

void WorldSession::HandleBankerActivateOpcode(WorldPackets::Npc::BankerActivate const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    // [SUI] The driven bot opens ITS bank: CheckBanker ranges from the actor and
    // SendShowBank latches the banker on the actor (Player::CanUseBank reads it).
    // The bank contents ride the snapshot (bank rows), the frame opens on this socket.
    Player* pActor = GetSuiActor();
    if (!CheckBanker(packet.guid))
        return;

    // remove fake death
    if (pActor->HasUnitState(UNIT_STATE_FEIGN_DEATH))
        pActor->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    SendShowBank(packet.guid);
}

void WorldSession::SendShowBank(ObjectGuid guid)
{
    WorldPacket data(SMSG_SHOW_BANK, 8);
    data << ObjectGuid(guid);
    GetSuiActor()->m_currentBankerGuid = guid;   // [SUI] the driven bot is the one at the banker
    SendPacket(&data);
}

void WorldSession::HandleTrainerListOpcode(WorldPackets::Npc::TrainerList const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    SendTrainerList(packet.guid);
}

static void SendTrainerSpellHelper(WorldPacket& data, TrainerSpell const* tSpell, uint32 triggerSpell, TrainerSpellState state, float fDiscountMod, bool can_learn_primary_prof)
{
    SpellEntry const* triggerInfo = sSpellMgr.GetSpellEntry(triggerSpell);
    uint32 spellLevel = 0;
    if (tSpell->reqLevel)
        spellLevel = tSpell->reqLevel;
    else if (triggerInfo)
        spellLevel = triggerInfo->spellLevel;
    else
        return;

    bool primary_prof_first_rank = sSpellMgr.IsPrimaryProfessionFirstRankSpell(triggerSpell);

    SpellChainNode const* chain_node = sSpellMgr.GetSpellChainNode(triggerSpell);

    data << uint32(tSpell->spell);
    data << uint8(state == TRAINER_SPELL_GREEN_DISABLED ? TRAINER_SPELL_GREEN : state);
    data << uint32(tSpell->spellCost * fDiscountMod + 0.5f);

    data << uint32(primary_prof_first_rank && can_learn_primary_prof ? 1 : 0);
    // primary prof. learn confirmation dialog
    data << uint32(primary_prof_first_rank ? 1 : 0);    // must be equal prev. field to have learn button in enabled state
    data << uint8(spellLevel);
    data << uint32(tSpell->reqSkill);
    data << uint32(tSpell->reqSkillValue);
    // Nostalrius: le client veut spellreq1, spellreq2 avec spellreq2 != 0 seulement si spellreq1 != 0.
    if (chain_node)
    {
        if (chain_node->req)
        {
            data << uint32(chain_node->req);
            data << uint32(chain_node->prev);
        }
        else
        {
            data << uint32(chain_node->prev);
            data << uint32(0);
        }
    }
    else
        data << uint32(0) << uint32(0);
    data << uint32(0);
}

void WorldSession::SendTrainerList(ObjectGuid guid)
{
    // [SUI] P4b: list the DRIVEN bot's trainable spells (its class/race/skill/level,
    // its already-known set), not the parked commander's. Built on GetSuiActor();
    // the packet still sends on `this` — the commander's socket directly when this
    // ran from the trainer frame, or the bot's socket (→ MirrorOwnerPacket) when it
    // ran from a gossip "train" option. Unpossessed, pActor == _player, unchanged.
    Player* pActor = GetSuiActor();
    Creature* unit = pActor->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_TRAINER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: SendTrainerList - %s not found or you can't interact with him.", guid.GetString().c_str());
        return;
    }

    // trainer list loaded at check;
    if (!unit->IsTrainerOf(pActor, true))
        return;

    CreatureInfo const* ci = unit->GetCreatureInfo();
    if (!ci)
        return;

    TrainerSpellData const* cSpells = unit->GetTrainerSpells();
    TrainerSpellData const* tSpells = unit->GetTrainerTemplateSpells();

    if (!cSpells && !tSpells)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: SendTrainerList - Training spells not found for %s", guid.GetString().c_str());
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    uint32 maxcount = (cSpells ? cSpells->spellList.size() : 0) + (tSpells ? tSpells->spellList.size() : 0);
    uint32 trainer_type = cSpells && cSpells->trainerType ? cSpells->trainerType : (tSpells ? tSpells->trainerType : 0);

    std::string strTitle;
    if (TrainerGreetingLocale const* trainerGreeting = sObjectMgr.GetTrainerGreetingLocale(guid.GetEntry()))
    {
        int locale_idx = GetSessionDbLocaleIndex();

        if ((int32)trainerGreeting->Content.size() > locale_idx + 1 && !trainerGreeting->Content[locale_idx + 1].empty())
            strTitle = trainerGreeting->Content[locale_idx + 1];
        else
            strTitle = trainerGreeting->Content[0];
    }
    else
    {
        strTitle = GetMangosString(LANG_NPC_TAINER_HELLO);
    }

    WorldPacket data(SMSG_TRAINER_LIST, 8 + 4 + 4 + maxcount * 38 + strTitle.size() + 1);
    data << ObjectGuid(guid);
    data << uint32(trainer_type);

    size_t count_pos = data.wpos();
    data << uint32(maxcount);

    // reputation discount
    float fDiscountMod = pActor->GetReputationPriceDiscount(unit);
    bool can_learn_primary_prof = pActor->GetFreePrimaryProfessionPoints() > 0;

    uint32 count = 0;

    if (cSpells)
    {
        for (const auto& itr : cSpells->spellList)
        {
            TrainerSpell const* tSpell = &itr.second;

            uint32 triggerSpell = sSpellMgr.GetSpellEntry(tSpell->spell)->EffectTriggerSpell[0];

            if (!pActor->IsSpellFitByClassAndRace(triggerSpell))
                continue;

            TrainerSpellState state = pActor->GetTrainerSpellState(tSpell);

            SendTrainerSpellHelper(data, tSpell, triggerSpell, state, fDiscountMod, can_learn_primary_prof);

            ++count;
        }
    }

    if (tSpells)
    {
        for (const auto& itr : tSpells->spellList)
        {
            TrainerSpell const* tSpell = &itr.second;

            uint32 triggerSpell = sSpellMgr.GetSpellEntry(tSpell->spell)->EffectTriggerSpell[0];

            if (!pActor->IsSpellFitByClassAndRace(triggerSpell))
                continue;

            TrainerSpellState state = pActor->GetTrainerSpellState(tSpell);

            SendTrainerSpellHelper(data, tSpell, triggerSpell, state, fDiscountMod, can_learn_primary_prof);

            ++count;
        }
    }

    data << strTitle;

    data.put<uint32>(count_pos, count);
    SendPacket(&data);
}

void WorldSession::SendTrainingSuccess(ObjectGuid guid, uint32 spellId)
{
    WorldPacket data(SMSG_TRAINER_BUY_SUCCEEDED, 12);
    data << ObjectGuid(guid);
    data << uint32(spellId);                                // should be same as in packet from client
    SendPacket(&data);
}

void WorldSession::SendTrainingFailure(ObjectGuid guid, uint32 serviceId, uint32 errorCode)
{
    WorldPacket data(SMSG_TRAINER_BUY_FAILED, 16);
    data << ObjectGuid(guid);
    data << uint32(serviceId);
    data << uint32(errorCode);
    SendPacket(&data);
}

void WorldSession::HandleTrainerBuySpellOpcode(WorldPackets::Npc::TrainerBuySpell const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: Received CMSG_TRAINER_BUY_SPELL Trainer: %s, learn spell id is: %u", packet.guid.GetString().c_str(), packet.spellId);

    // [SUI] P4b: the DRIVEN bot learns and pays. Acts on GetSuiActor(); the
    // learned spell reaches the commander as the already-mirrored SMSG_LEARNED_SPELL,
    // and a re-snapshot updates the bot's shown purse. SendTraining* still go on
    // `this` (the commander's socket) so the failure toast is seen directly.
    Player* pActor = GetSuiActor();
    bool suiActing = pActor != _player;
    Creature* unit = pActor->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_TRAINER);

    if (!unit || !unit->IsTrainerOf(pActor, true) || !unit->IsWithinLOSInMap(pActor))
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleTrainerBuySpellOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    // Check if the spell is present in the trainer's spell list.
    TrainerSpellData const* cSpells = unit->GetTrainerSpells();
    TrainerSpellData const* tSpells = unit->GetTrainerTemplateSpells();

    if (!cSpells && !tSpells)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        return;
    }

    // Try to find the spell in npc_trainer.
    TrainerSpell const* trainer_spell = cSpells ? cSpells->Find(packet.spellId) : nullptr;

    // Not found, try find it in npc_trainer_template.
    if (!trainer_spell && tSpells)
        trainer_spell = tSpells->Find(packet.spellId);

    // Not found anywhere, cheating?
    if (!trainer_spell)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
        return;
    }

    // Can't be learned, cheat? Or double learn with lags...
    if (pActor->GetTrainerSpellState(trainer_spell) != TRAINER_SPELL_GREEN)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_NOT_ENOUGH_SKILL);
        return;
    }

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(trainer_spell->spell);

    // Apply reputation discount.
    uint32 nSpellCost = uint32(trainer_spell->spellCost * pActor->GetReputationPriceDiscount(unit) + 0.5f);

    // Check money requirement.
    if (pActor->GetMoney() < nSpellCost)
    {
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_NOT_ENOUGH_MONEY);
        return;
    }

    // All is good. Spell can be learned if we reach this point.
    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    Spell* spell;
    if (proto->SpellVisual == 222)
        spell = new Spell(pActor, proto, false);
    else
        spell = new Spell(unit, proto, false);

    SpellCastTargets targets;
    targets.setUnitTarget(pActor);

    uint32 const learnedSpellId = proto->EffectTriggerSpell[0];
    SpellCastResult cast_result = spell->prepare(std::move(targets));
    spell->update(1); // Update the spell right now. Prevents desynch => take twice the money if you click really fast.

    // prepare() only reports that the wrapper cast was admitted. The cast can
    // still finish without executing SPELL_EFFECT_LEARN_SPELL, which used to
    // charge the player and report success while leaving the trainer row green.
    // Trainer data is validated at load time to use a learn-spell wrapper, so
    // make the taught spell authoritative before committing the purchase.
    if (cast_result == SPELL_CAST_OK && learnedSpellId && !pActor->HasSpell(learnedSpellId))
        pActor->LearnSpell(learnedSpellId, false);

    // Commit only after the taught spell is present on the acting player.
    if (cast_result == SPELL_CAST_OK && learnedSpellId && pActor->HasSpell(learnedSpellId))
    {
        pActor->ModifyMoney(-int32(nSpellCost));
        SendTrainingSuccess(packet.guid, packet.spellId);
        if (suiActing)
            SuiPossess::ResnapshotControlled(this);
    }
    else
        SendTrainingFailure(packet.guid, packet.spellId, TRAIN_FAIL_UNAVAILABLE);
}

void WorldSession::HandleGossipHelloOpcode(WorldPackets::Npc::GossipHello const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.npcGuid))
        return;

    // [SUI] P4b: while driving a party bot, run the whole NPC interaction as the
    // BOT (its quest state, its bags, its trainer eligibility) and let the reply
    // ride the possession proxy back to the commander. Unpossessed, GetSuiActor()
    // is _player and this is bit-identical.
    Player* actor = GetSuiActor();
    Creature* pCreature = actor->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_NONE);
    if (!pCreature)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleGossipHelloOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    actor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    actor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    if (!pCreature->HasExtraFlag(CREATURE_FLAG_EXTRA_NO_MOVEMENT_PAUSE))
        pCreature->PauseOutOfCombatMovement();

    if (pCreature->IsSpiritGuide())
        pCreature->SendAreaSpiritHealerQueryOpcode(actor);

    if (!sScriptMgr.OnGossipHello(actor, pCreature))
    {
        actor->PrepareGossipMenu(pCreature, pCreature->GetDefaultGossipMenuId());
        actor->SendPreparedGossip(pCreature);
    }
}

void WorldSession::HandleGossipSelectOptionOpcode(WorldPackets::Npc::GossipSelectOption const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    Player* actor = GetSuiActor();   // [SUI] P4b: the driven bot owns this menu
    bool const isCoded = actor->PlayerTalkClass->GossipOptionCoded(packet.gossipListId);
    if (isCoded && packet.code.empty())
        return;  // coded option requires a code from the client

    actor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    actor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    uint32 sender = actor->PlayerTalkClass->GossipOptionSender(packet.gossipListId);
    uint32 action = actor->PlayerTalkClass->GossipOptionAction(packet.gossipListId);

    // Only forward a non-null code to scripts for coded gossip options.
    const char* code = (isCoded && !packet.code.empty()) ? packet.code.c_str() : nullptr;

    if (packet.guid.IsAnyTypeCreature())
    {
        Creature* pCreature = actor->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_NONE);

        if (!pCreature)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleGossipSelectOptionOpcode - %s not found or you can't interact with it.", packet.guid.GetString().c_str());
            return;
        }

        if (!pCreature->HasExtraFlag(CREATURE_FLAG_EXTRA_NO_MOVEMENT_PAUSE))
            pCreature->PauseOutOfCombatMovement();

        if (!sScriptMgr.OnGossipSelect(actor, pCreature, sender, action, code))
            actor->OnGossipSelect(pCreature, packet.gossipListId);
    }
    else if (packet.guid.IsGameObject())
    {
        GameObject* pGo = actor->GetGameObjectIfCanInteractWith(packet.guid);

        if (!pGo)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleGossipSelectOptionOpcode - %s not found or you can't interact with it.", packet.guid.GetString().c_str());
            return;
        }

        if (!sScriptMgr.OnGossipSelect(actor, pGo, sender, action, code))
            actor->OnGossipSelect(pGo, packet.gossipListId);
    }
}

void WorldSession::HandleSpiritHealerActivateOpcode(WorldPackets::Npc::SpiritHealerActivate const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_SPIRITHEALER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleSpiritHealerActivateOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    GetPlayer()->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    SendSpiritResurrect();
}

void WorldSession::SendSpiritResurrect()
{
    if (SuiHero::BlocksResurrection(_player))
        return;

    _player->ResurrectPlayer(0.5f, true);

    _player->DurabilityLossAll(0.25f, true);

    // get corpse nearest graveyard
    WorldSafeLocsEntry const* corpseGrave = nullptr;
    Corpse* corpse = _player->GetCorpse();
    if (corpse)
        corpseGrave = sObjectMgr.GetClosestGraveYard(
                          corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), corpse->GetMapId(), _player->GetTeam());

    // now can spawn bones
    _player->SpawnCorpseBones();

    // teleport to nearest from corpse graveyard, if different from nearest to player ghost
    if (corpseGrave)
    {
        WorldSafeLocsEntry const* ghostGrave = sObjectMgr.GetClosestGraveYard(
                _player->GetPositionX(), _player->GetPositionY(), _player->GetPositionZ(), _player->GetMapId(), _player->GetTeam());

        float orientation = _player->GetOrientation();

        // World of Warcraft Client Patch 1.8.0 (2005-10-11)
        // - All graveyards that needed adjustment were changed so that a
        //   character's spirit comes into the world facing toward the Spirit Healer.
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
        if (float facing = sObjectMgr.GetWorldSafeLocFacing(corpseGrave->ID))
            orientation = facing;
#endif

        if (corpseGrave != ghostGrave)
            _player->TeleportTo(corpseGrave->map_id, corpseGrave->x, corpseGrave->y, corpseGrave->z, orientation);
        // or update at original position
        else
        {
            _player->GetCamera().UpdateVisibilityForOwner();
            _player->UpdateObjectVisibility();
        }
    }
    // or update at original position
    else
    {
        _player->GetCamera().UpdateVisibilityForOwner();
        _player->UpdateObjectVisibility();
    }
}

void WorldSession::HandleBinderActivateOpcode(WorldPackets::Npc::BinderActivate const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.npcGuid))
        return;

    // [SUI] the driven bot binds ITS hearth at the innkeeper
    Player* pActor = GetSuiActor();

    if (!pActor->IsInWorld() || !pActor->IsAlive())
        return;

    Creature* unit = pActor->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_INNKEEPER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleBinderActivateOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    SendBindPoint(unit);
}

void WorldSession::SendBindPoint(Creature* npc)
{
    // [SUI] the Bind spell targets the driven bot; its confirm/bound frames mirror back
    Player* pActor = GetSuiActor();

    // prevent set homebind to instances in any case
    if (pActor->GetMap()->Instanceable())
        return;

    // send spell for bind 3286 bind magic
    npc->CastSpell(pActor, 3286, true);                    // Bind

    pActor->PlayerTalkClass->CloseGossip();
}

void WorldSession::HandleListStabledPetsOpcode(WorldPackets::Npc::ListStabledPets const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.npcGuid))
        return;

    // [SUI] the driven bot's stable
    Player* pActor = GetSuiActor();

    if (!pActor || pActor->GetClass() != CLASS_HUNTER)
        return;
    Creature* unit = pActor->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_STABLEMASTER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleListStabledPetsOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    SendStablePet(packet.npcGuid);
}

void WorldSession::SendStablePet(ObjectGuid guid)
{
    // [SUI] the driven bot's pets; the list answers on this socket (or mirrors from a gossip pick)
    Player* pActor = GetSuiActor();

    if (!CheckStableMaster(guid))
        return;
    WorldPacket data(MSG_LIST_STABLED_PETS, 200);           // guess size
    data << guid;

    Pet* pet = pActor->GetPet();

    size_t wpos = data.wpos();
    data << uint8(0);                                       // place holder for slot show number

    data << uint8(pActor->m_stableSlots);

    uint8 num = 0;                                          // counter for place holder

    // not let move dead pet in slot
    if (pet && pet->IsAlive() && pet->GetPetType() == HUNTER_PET)
    {
        data << uint32(pet->GetCharmInfo()->GetPetNumber());
        data << uint32(pet->GetEntry());
        data << uint32(pet->GetLevel());
        data << pet->GetName();                             // petname
        data << uint32(pet->GetLoyaltyLevel());             // loyalty
        data << uint8(0x01);                                // client slot 1 == current pet (0)
        ++num;
    }
    // Pet may be despawned if owner went far away from pet for example.
    else if (CharacterPetCache const* currentPetData = sCharacterDatabaseCache.GetCharacterPetByOwner(pActor->GetGUIDLow()))
    {
        data << uint32(currentPetData->id);
        data << uint32(currentPetData->entry);
        data << uint32(currentPetData->level);
        data << currentPetData->name;                           // petname
        data << uint32(currentPetData->loyalty);                // loyalty
        data << uint8(0x01);                                    // client slot 1 == current pet (0)
        ++num;
    }
    CharPetMap const& pets = sCharacterDatabaseCache.GetCharPetsMap();
    CharPetMap::const_iterator myPets = pets.find(pActor->GetGUIDLow());
    if (myPets != pets.end())
        for (const auto it : myPets->second)
            if (it->slot >= PET_SAVE_FIRST_STABLE_SLOT && it->slot <= PET_SAVE_LAST_STABLE_SLOT)
            {
                data << uint32(it->id);                 // pet number
                data << uint32(it->entry);              // creature entry
                data << uint32(it->level);              // level
                data << it->name;                       // name
                data << uint32(it->loyalty);            // loyalty
                data << uint8(it->slot + 1);            // slot
                ++num;
            }

    data.put<uint8>(wpos, num);                             // set real data to placeholder
    SendPacket(&data);
}

void WorldSession::SendStableResult(uint8 res)
{
    WorldPacket data(SMSG_STABLE_RESULT, 1);
    data << uint8(res);
    SendPacket(&data);
}

bool WorldSession::CheckStableMaster(ObjectGuid guid)
{
    Player* pActor = GetSuiActor();
    if (!pActor || pActor->GetClass() != CLASS_HUNTER)
        return false;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, guid))
        return false;

    // spell case or GM
    if (guid == GetPlayer()->GetObjectGuid())
    {
        // command case will return only if player have real access to command
        if (!ChatHandler(GetPlayer()).FindCommand("stable"))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "%s attempt open stable in cheating way.", guid.GetString().c_str());
            return false;
        }
    }
    // stable master case
    else
    {
        if (!pActor->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_STABLEMASTER))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "Stablemaster %s not found or you can't interact with him.", guid.GetString().c_str());
            return false;
        }
    }

    return true;
}

void WorldSession::HandleStablePet(WorldPackets::Npc::StablePet const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;
    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.npcGuid))
        return;

    // [SUI] the driven bot stables ITS pet
    Player* pActor = GetSuiActor();

    if (!pActor->IsAlive())
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    Pet* pet = pActor->GetPet();

    // can't place in stable dead pet
    if (!pet || !pet->IsAlive() || pet->GetPetType() != HUNTER_PET)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    uint32 free_slot = PET_SAVE_FIRST_STABLE_SLOT;

    // Find free slot for pet
    bool usedSlots[PET_SAVE_LAST_STABLE_SLOT - PET_SAVE_FIRST_STABLE_SLOT + 1] = {false};
    CharPetMap const& pets = sCharacterDatabaseCache.GetCharPetsMap();
    CharPetMap::const_iterator myPets = pets.find(pActor->GetGUIDLow());
    if (myPets != pets.end())
        for (const auto it : myPets->second)
            if (it->slot >= PET_SAVE_FIRST_STABLE_SLOT && it->slot <= PET_SAVE_LAST_STABLE_SLOT)
                usedSlots[it->slot - PET_SAVE_FIRST_STABLE_SLOT] = true;

    for (free_slot = PET_SAVE_FIRST_STABLE_SLOT; free_slot <= PET_SAVE_LAST_STABLE_SLOT && usedSlots[free_slot - PET_SAVE_FIRST_STABLE_SLOT]; ++free_slot);

    if (free_slot <= pActor->m_stableSlots)
    {
        pet->Unsummon(PetSaveMode(free_slot), pActor);
        SendStableResult(STABLE_SUCCESS_STABLE);
    }
    else
        SendStableResult(STABLE_ERR_STABLE);

    // [SUI] Owner-only facts of the driven bot changed: re-push the snapshot.
    if (pActor != _player)
        SuiPossess::ResnapshotControlled(this);
}

void WorldSession::HandleUnstablePet(WorldPackets::Npc::UnstablePet const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    // [SUI] the driven bot takes ITS pet out
    Player* pActor = GetSuiActor();

    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    CharacterPetCache const* petData = sCharacterDatabaseCache.GetCharacterPetCacheByOwnerAndId(pActor->GetGUIDLow(), packet.petNumber);

    if (!petData || petData->slot < PET_SAVE_FIRST_STABLE_SLOT || petData->slot > PET_SAVE_LAST_STABLE_SLOT)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    uint32 creatureId = petData->entry;
    CreatureInfo const* creatureInfo = sObjectMgr.GetCreatureTemplate(creatureId);
    if (!creatureInfo || !creatureInfo->IsTameable())
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    // Player may have a pet, but unsummoned currently (too far away from owner ...). Do not erase this pet!
    Pet* pet = pActor->GetPet();
    if (pet || sCharacterDatabaseCache.GetCharacterPetByOwner(pActor->GetGUIDLow()))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    Pet* newpet = new Pet(HUNTER_PET);
    if (!newpet->LoadPetFromDB(pActor, creatureId, packet.petNumber))
    {
        delete newpet;
        newpet = nullptr;
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    SendStableResult(STABLE_SUCCESS_UNSTABLE);

    // [SUI] Owner-only facts of the driven bot changed: re-push the snapshot.
    if (pActor != _player)
        SuiPossess::ResnapshotControlled(this);
}

void WorldSession::HandleBuyStableSlot(WorldPackets::Npc::BuyStableSlot const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    // [SUI] the driven bot buys ITS stable slot with ITS coin
    Player* pActor = GetSuiActor();

    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    if (pActor->m_stableSlots < MAX_PET_STABLES)
    {
        StableSlotPricesEntry const* SlotPrice = sStableSlotPricesStore.LookupEntry(pActor->m_stableSlots + 1);
        if (pActor->GetMoney() >= SlotPrice->Price)
        {
            ++pActor->m_stableSlots;
            pActor->ModifyMoney(-int32(SlotPrice->Price));
            SendStableResult(STABLE_SUCCESS_BUY_SLOT);
        }
        else
            SendStableResult(STABLE_ERR_MONEY);
    }
    else
        SendStableResult(STABLE_ERR_STABLE);

    // [SUI] Owner-only facts of the driven bot changed: re-push the snapshot.
    if (pActor != _player)
        SuiPossess::ResnapshotControlled(this);
}

void WorldSession::HandleStableRevivePet(NullClientPacket const& /*packet*/)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    // [SUI] the driven bot's pet
    Player* pActor = GetSuiActor();

}

void WorldSession::HandleStableSwapPet(WorldPackets::Npc::StableSwapPet const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    // [SUI] the driven bot swaps ITS pets
    Player* pActor = GetSuiActor();

    if (!CheckStableMaster(packet.npcGuid))
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    Pet* pet = pActor->GetPet();

    if (!pet || !pet->IsAlive() || pet->GetPetType() != HUNTER_PET)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    // find swapped pet slot in stable
    CharacterPetCache const* swappedPet = sCharacterDatabaseCache.GetCharacterPetCacheByOwnerAndId(pActor->GetGUIDLow(), packet.petNumber);
    if (!swappedPet)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    uint32 slot        = swappedPet->slot;
    uint32 creature_id = swappedPet->entry;

    if (!creature_id)
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    CreatureInfo const* creatureInfo = sObjectMgr.GetCreatureTemplate(creature_id);
    if (!creatureInfo || !creatureInfo->IsTameable())
    {
        SendStableResult(STABLE_ERR_STABLE);
        return;
    }

    pet->Unsummon(PetSaveMode(slot), pActor);

    // summon unstabled pet
    Pet* newpet = new Pet;
    if (!newpet->LoadPetFromDB(pActor, creature_id, packet.petNumber))
    {
        delete newpet;
        SendStableResult(STABLE_ERR_STABLE);
    }
    else
        SendStableResult(STABLE_SUCCESS_UNSTABLE);

    // [SUI] Owner-only facts of the driven bot changed: re-push the snapshot.
    if (pActor != _player)
        SuiPossess::ResnapshotControlled(this);
}

void WorldSession::HandleRepairItemOpcode(WorldPackets::Npc::RepairItem const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.npcGuid))
        return;

    // [SUI] P4b: repair the DRIVEN bot's gear (its durability, its purse). Acts on
    // GetSuiActor(); re-snapshots the bot afterward so the commander sees the mended
    // durability and reduced coinage. Unpossessed, pActor == _player, unchanged.
    Player* pActor = GetSuiActor();
    bool suiActing = pActor != _player;
    Creature* unit = pActor->GetNPCIfCanInteractWith(packet.npcGuid, UNIT_NPC_FLAG_REPAIR);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleRepairItemOpcode - %s not found or you can't interact with him.", packet.npcGuid.GetString().c_str());
        return;
    }

    pActor->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pActor->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    // reputation discount
    float discountMod = pActor->GetReputationPriceDiscount(unit);

    if (packet.itemGuid)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "ITEM: %s repair of %s", packet.npcGuid.GetString().c_str(), packet.itemGuid.GetString().c_str());
        if (Item* item = pActor->GetItemByGuid(packet.itemGuid))
            pActor->DurabilityRepair(item->GetPos(), true, discountMod);
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "ITEM: %s repair all items", packet.npcGuid.GetString().c_str());
        pActor->DurabilityRepairAll(true, discountMod);
    }

    if (suiActing)
        SuiPossess::ResnapshotControlled(this);
}
