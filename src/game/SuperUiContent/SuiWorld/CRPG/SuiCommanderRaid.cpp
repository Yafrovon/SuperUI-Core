#include "SuiCommanderRaid.h"
#include "SuiEncounterDefinition.h"
#include "Creature.h"
#include <atomic>
#include "SuiPossess.h"
#include "SuiCompanion.h"
#include "AiBotAIMain.h"
#include "Player.h"
#include "Item.h"
#include "Bag.h"
#include "Pet.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "WaypointManager.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "Log.h"
#include "PathFinder.h"
#include "GameObject.h"
#include "DynamicObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

// Pre-combat objective rebinding: a candidate must be nearer the tank anchor by more than this
// before it replaces a live, in-room bound unit (wander is a few yards; dead bodies never bind).
static constexpr float kBindScanRange=400.f;
// Pre-combat patrol engagement: a waypoint patroller is approached only once its route brings it
// within this distance of the tank anchor (the survey clearance distance: nearer than this a raid
// standing at the anchor is inside the patroller's detection footprint anyway).
static constexpr float kPatrolEngageYards=36.f;
#include <algorithm>
#include <functional>
#include <chrono>
#include <limits>
#include <cmath>
#include <map>
#include <list>
#include <set>
#include <mutex>
#include <chrono>

// One authoritative plan per commander. Access is serialized because map updates
// and session packet processing need not execute on the same worker thread.
namespace
{
    using namespace SuiEncounter;
    struct SlowRaidScope
    {
        char const* operation; uint32 actor;
        std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
        ~SlowRaidScope()
        {
            auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
            if(elapsed>=25) sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-slow] operation=%s actor=%u elapsedMs=%lld",operation,actor,static_cast<long long>(elapsed));
        }
    };
    struct Claim { ObjectGuid actor,target; uint8 kind=0; uint32 remaining=0,amount=0,spell=0; };
    struct Objective { ObjectGuid guid; bool seen=false,dead=false,engaged=false,yielded=false; uint32 entry=0,maxHealth=0; std::chrono::steady_clock::time_point yieldedSince{}; };
    // An objective has yielded when it is alive, out of combat after the raid fought it, and no longer an enemy of the
    // raid: flagged immune to players / non-attackable, or no longer hostile to the owner. An evading boss keeps its
    // hostility and stays attackable, so an evade never reads as a yield.
    static constexpr uint32 kYieldStableMs=1000;
    static bool ObjectiveYielded(Unit const* unit,Player const* /*owner*/)
    {
        // Flags only: a hostility test against the owner would read a GM-mode owner as "not hostile" (v35/v37).
        if(!unit||!unit->IsAlive()||unit->IsInCombat())return false;
        return unit->HasFlag(UNIT_FIELD_FLAGS,UNIT_FLAG_IMMUNE_TO_PLAYER|UNIT_FLAG_NON_ATTACKABLE_2|UNIT_FLAG_NOT_ATTACKABLE_1);
    }
    struct Event { size_t rule=0; ObjectGuid target; uint32 remaining=0,impact=0; };
    struct Assignment
    {
        ObjectGuid guid,primary;
        bool manual=false;
        uint32 focus=0,interrupt=0,dispel=0,taunt=0,defensive=0;
        uint8 role = 0, team = 0, duty = 0;
        Point ground, air, lastMove, flank, flankOrigin;
        mutable Point escapeGoal;
        mutable bool escapeGoalReady=false;
        mutable uint8 escapePhase=0;
        float flankFacing=0;
        bool flankReady=false;
        uint32 heal = 0, damage = 0, tick = 0, bridgeTick = 0, potionTick = 0;
        uint32 feignRecoveryMs = 0;
        bool yielded = false, hasMove = false, hazardMove = false;
        bool healMovementInterrupted = false;
        ObjectGuid target;
        Point holdGoal; bool holdReady = false; // the add tank's committed add hold ground (v46)
    };
    struct TankContact
    {
        ObjectGuid primary, acting;
        std::chrono::steady_clock::time_point lostSince{};
    };
    struct Plan
    {
        ObjectGuid owner, boss;
        uint32 revision = 0, instance = 0;
        uint8 phase=0; Definition definition; std::vector<Event> events;
        uint8 state = 0, flags = 0;
        bool enteredCombat = false;
        std::vector<Claim> claims;
        std::map<uint32,Objective> objectives;
        std::map<ObjectGuid,Objective> addObjectives;
        std::vector<Assignment> rows;
        std::map<ObjectGuid,TankContact> tankContacts;
        std::map<uint8,uint32> damageSchools;
        struct ControlLease { ObjectGuid controller; uint32 spell=0; std::chrono::steady_clock::time_point cast{},expires{}; };
        std::map<ObjectGuid,ControlLease> controls; // add -> the member holding it under a control spell
        std::map<uint32,float> auraFootprints; // self periodic aura spell -> the area-damage radius its pulses were observed with
        std::map<uint32,float> burstFootprints; // creature entry -> the largest instant caster-centred burst radius it was observed casting
        std::map<ObjectGuid,std::chrono::steady_clock::time_point> releases; // add -> the last direct damage cast that broke its control (v45)
        ObjectGuid breaker; std::chrono::steady_clock::time_point breakerAt{}; // the member elected to break the released add's control (v45)
        std::map<ObjectGuid,bool> controllable; std::chrono::steady_clock::time_point controllableAt{}; // add -> a raid damage member can hold it (v45)
        std::map<ObjectGuid,ObjectGuid> addTankOf; // uncontrollable add -> the add tank assigned to hold it (v53)
    };
    std::recursive_mutex mutex;
    std::map<uint64, Plan> plans;
    std::atomic<uint32> watchedEntries[128]{};
    uint64 Raw(ObjectGuid guid) { return guid.GetRawValue(); }
    bool RequiredAddsComplete(Plan const& plan)
    {
        for(auto const& requirement:plan.definition.requiredAdds)
        {
            uint32 seen=0;
            for(auto const& pair:plan.addObjectives)if(pair.second.entry==requirement.entry)
            {
                if(!pair.second.seen||!pair.second.dead)return false;
                ++seen;
            }
            if(seen<requirement.count)return false;
        }
        return true;
    }
    void RefreshWatches()
    {
        size_t slot=0;
        for(auto const& item:plans) if(item.second.state==2)
        {
            for(uint32 entry:item.second.definition.objectives)if(slot<128)watchedEntries[slot++].store(entry);
            for(auto const& requirement:item.second.definition.requiredAdds)if(slot<128)watchedEntries[slot++].store(requirement.entry);
        }
        while(slot<128)watchedEntries[slot++].store(0);
    }
    bool InRoom(Plan const& plan,Point p) {return Inside(plan.definition,p);}
    Player* Member(Plan const& plan, ObjectGuid guid)
    {
        Player* p = sObjectMgr.GetPlayer(guid);
        return p && p->IsInWorld() && p->GetMapId() == plan.definition.map && p->GetInstanceId() == plan.instance ? p : nullptr;
    }
    // Native crowd control (2026-09-13): the mechanics that take an enemy out of the fight without moving it.
    // A ground phase with melee damage switched off (adds that burst on death, 2026-09-13) still has its tanks in
    // melee contact; only airborne/alternate phases park the tanks too.
    bool MeleeAllowed(Phase const& phase,uint8 role)
    {
        return phase.melee||(role<=2&&phase.airborne!=1&&!phase.alternate);
    }
    bool ControlMechanic(uint32 mechanic)
    {
        return mechanic==MECHANIC_BANISH||mechanic==MECHANIC_POLYMORPH||mechanic==MECHANIC_SHACKLE||mechanic==MECHANIC_SLEEP||mechanic==MECHANIC_SAPPED;
    }
    // The member's best known control spell that can hold this target: hostile single-target, a control mechanic, a
    // duration, and a creature-type mask that covers the target (the game's own CheckTargetCreatureType rule).
    SpellEntry const* ControlSpellFor(Player* actor,Unit* target)
    {
        SpellEntry const* best=nullptr;
        for(auto const& known:actor->GetSpellMap())
        {
            if(known.second.state==PLAYERSPELL_REMOVED||known.second.disabled||!known.second.active)continue;
            SpellEntry const* spell=sSpellMgr.GetSpellEntry(known.first);
            if(!spell||!ControlMechanic(spell->Mechanic)||spell->GetDuration()<5000)continue;
            if(spell->EffectImplicitTargetA[0]!=TARGET_UNIT_ENEMY)continue;
            if(spell->TargetCreatureType&&!(spell->TargetCreatureType&target->GetCreatureTypeMask()))continue;
            // A control the unit is immune to (template mechanic/school immunity, live immunity auras) is no control
            // option: claiming it would stop the raid's damage on the add for a cast that cannot land (2026-09-14, v40).
            if(target->IsImmuneToSpell(spell,false))continue;
            if(!best||spell->GetDuration()>best->GetDuration())best=spell;
        }
        return best;
    }
    // The compiled definition lists its required adds in the global doctrine's kill order (summoners, healers,
    // casters, ranged, melee; weakest first within a class). The rank of a unit is the index of its entry in that
    // list; an entry the definition does not require ranks after every required one.
    float KillRank(Plan const& plan,Unit const* unit)
    {
        for(size_t i=0;i<plan.definition.requiredAdds.size();i++)if(plan.definition.requiredAdds[i].entry==unit->GetEntry())return float(i);
        return float(plan.definition.requiredAdds.size());
    }
    // The raid kills a before b: earlier kill rank, then lower health, then lower GUID - one order every member shares.
    bool KillsBefore(Plan const& plan,Unit const* a,Unit const* b)
    {
        float const ra=KillRank(plan,a),rb=KillRank(plan,b);
        if(ra!=rb)return ra<rb;
        if(a->GetHealthPercent()!=b->GetHealthPercent())return a->GetHealthPercent()<b->GetHealthPercent();
        return a->GetObjectGuid().GetRawValue()<b->GetObjectGuid().GetRawValue();
    }
    // The raid's current kill target among the required adds: the uncontrolled, in-combat, in-room add that dies
    // first in the kill order (KillsBefore). Nobody flees its burst while killing it (v44). Const: no lease read.
    bool HeldByControl(Unit const* unit)
    {
        for(auto const& pair:unit->GetSpellAuraHolderMap())
            if(pair.second&&pair.second->GetSpellProto()&&ControlMechanic(pair.second->GetSpellProto()->Mechanic))return true;
        return false;
    }
    Creature* KillTargetAdd(Plan const& plan,Player* actor)
    {
        if(plan.definition.adds.empty())return nullptr;
        std::list<Creature*> found;
        for(uint32 entry:plan.definition.adds)actor->GetCreatureListWithEntryInGrid(found,entry,100);
        Creature* loose=nullptr,*held=nullptr;
        for(Creature* add:found)
        {
            if(!add->IsAlive()||!add->IsInCombat()||add->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD))continue;
            if(!InRoom(plan,{add->GetPositionX(),add->GetPositionY(),add->GetPositionZ()}))continue;
            if(HeldByControl(add)){if(!held||KillsBefore(plan,add,held))held=add;continue;}
            if(!loose||KillsBefore(plan,add,loose))loose=add;
        }
        // A held add ranked before every uncontrolled one is the add the raid releases next (v45): it is the kill target.
        if(held&&(!loose||KillRank(plan,held)<KillRank(plan,loose)))return held;
        return loose;
    }
    bool ControlledAdd(Plan& plan,Unit* target,std::chrono::steady_clock::time_point now)
    {
        auto lease=plan.controls.find(target->GetObjectGuid());
        if(lease!=plan.controls.end())
        {
            bool const landing=now-lease->second.cast<std::chrono::seconds(3);
            if(now<lease->second.expires&&(landing||target->HasAura(lease->second.spell)))return true;
            plan.controls.erase(lease);
        }
        for(auto const& pair:target->GetSpellAuraHolderMap())
            if(pair.second&&pair.second->GetSpellProto()&&ControlMechanic(pair.second->GetSpellProto()->Mechanic))return true;
        return false;
    }
    void SuspendControlledTargetDamage(Plan& plan,Unit* target)
    {
        auto stop=[&](Unit* attacker){
            if(!attacker)return;
            if(attacker->GetVictim()==target)attacker->AttackStop();
            for(auto slot:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL,CURRENT_AUTOREPEAT_SPELL})
                if(auto cast=attacker->GetCurrentSpell(slot))
                    if(cast->m_targets.getUnitTargetGuid()==target->GetObjectGuid()&&!cast->m_spellInfo->IsPositiveSpell(attacker,target))
                        attacker->InterruptSpell(slot,true);
        };
        for(auto const& member:plan.rows)if(!member.manual&&!member.yielded)if(Player* actor=Member(plan,member.guid))
        {
            stop(actor);
            if(Pet* pet=actor->GetPet()){if(pet->GetVictim()==target&&pet->GetCharmInfo())pet->GetCharmInfo()->SetIsCommandAttack(false);stop(pet);}
        }
    }
    // Whether a living damage member of the raid can hold this unit under one of its control spells (cached two seconds).
    bool RaidCanControl(Plan& plan,Unit* unit,std::chrono::steady_clock::time_point now)
    {
        if(now-plan.controllableAt>std::chrono::seconds(2)){plan.controllable.clear();plan.controllableAt=now;}
        auto found=plan.controllable.find(unit->GetObjectGuid());
        if(found!=plan.controllable.end())return found->second;
        bool can=false;
        for(auto const& other:plan.rows)
            if(!other.manual&&!other.yielded&&(other.role==4||other.role==5))
                if(Player* member=Member(plan,other.guid))if(member->IsAlive()&&ControlSpellFor(member,unit)){can=true;break;}
        plan.controllable[unit->GetObjectGuid()]=can;
        return can;
    }
    // A ready, affordable, in-range, line-of-sight direct damage spell from the member's own book that lands within
    // 1.5 s on this one target (no control mechanic, no area): the fastest one.
    SpellEntry const* ReleaseSpellFor(Player* actor,Unit* target)
    {
        SpellEntry const* best=nullptr;
        for(auto const& known:actor->GetSpellMap())
        {
            if(known.second.state==PLAYERSPELL_REMOVED||known.second.disabled||!known.second.active)continue;
            SpellEntry const* spell=sSpellMgr.GetSpellEntry(known.first);
            if(!spell||spell->IsPassiveSpell()||ControlMechanic(spell->Mechanic)||spell->EffectImplicitTargetA[0]!=TARGET_UNIT_ENEMY)continue;
            bool damage=false,area=false;
            for(uint8 i=0;i<MAX_EFFECT_INDEX;++i)
            {
                damage|=spell->Effect[i]==SPELL_EFFECT_SCHOOL_DAMAGE;
                area|=spell->EffectRadiusIndex[i]!=0||spell->EffectChainTarget[i]>1;
            }
            if(!damage||area||spell->GetCastTime(actor)>1500||!actor->IsSpellReady(known.first))continue;
            if(Spell::CalculatePowerCost(spell,actor,nullptr,nullptr,false)>actor->GetPower(Powers(spell->powerType)))continue;
            if(target->IsImmuneToSpell(spell,false))continue;
            if(!actor->IsWithinDist(target,Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex)),true)||!actor->IsWithinLOSInMap(target))continue;
            if(!best||spell->GetCastTime(actor)<best->GetCastTime(actor))best=spell;
        }
        return best;
    }
    // A control spell under a reflect aura lands on the caster half the time (Majordomo's Magic Reflection on his adds).
    bool ReflectsSpells(Unit const* target)
    {return target->HasAuraType(SPELL_AURA_REFLECT_SPELLS)||target->HasAuraType(SPELL_AURA_REFLECT_SPELLS_SCHOOL);}
    // Re-casting a control on the unit it already holds (v51): the ordinary cast helper refuses any spell whose aura the
    // target already carries, so the v43 refresh never landed and every held add broke free together when the first
    // leases lapsed (majordomo-0914m-01: four sheeps cast at t=3 all ended at t~50). The game itself refreshes the aura.
    // A control claim or refresh preempts the member's own damage cast (v64): the control helpers wait for an idle caster, and a
    // controller chain-casting damage is never idle - no add was polymorphed in the first 20 s of majordomo-0915q-04 (all ten
    // mages on damage duty) and the three loose healer adds killed the melee in 25 s; the same wait lets a held add's refresh
    // lapse. Heals, controls and casts at a friendly unit are never interrupted.
    void PreemptDamageCast(Player* actor)
    {
        for(auto slot:{CURRENT_GENERIC_SPELL,CURRENT_CHANNELED_SPELL})
            if(Spell* cast=actor->GetCurrentSpell(slot))
            {
                SpellEntry const* info=cast->m_spellInfo;
                bool heal=false;
                for(uint8 i=0;i<MAX_EFFECT_INDEX;++i)heal|=info->Effect[i]==SPELL_EFFECT_HEAL;
                if(heal||ControlMechanic(info->Mechanic))continue;
                if(Unit* aimed=cast->m_targets.getUnitTarget())if(!aimed->IsHostileTo(actor))continue;
                actor->InterruptSpell(slot,false);
            }
    }
    bool RefreshControl(AiBotAI* ai,Unit* target,uint32 id)
    {
        Player* actor=ai->GetBotPlayer();
        SpellEntry const* spell=sSpellMgr.GetSpellEntry(id);
        if(!spell||!actor->HasSpell(id)||actor->IsMoving()||!actor->IsSpellReady(id))return false;
        if(target->IsImmuneToSpell(spell,false))return false;
        if(!actor->IsWithinDist(target,Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex)),true)||!actor->IsWithinLOSInMap(target))return false;
        PreemptDamageCast(actor);
        if(actor->IsNonMeleeSpellCasted(false,false,true))return false;
        actor->SetFacingToObject(target);
        return actor->CastSpell(target,spell,false)==SPELL_CAST_OK;
    }
    // The controlled add ranked before every uncontrolled fighting add is the raid's next kill target (v42), but the
    // class rotations never damage a unit under breakable control and its controller would refresh it forever: the
    // raid stood 15-40 s on a sheeped healer in 8 of 15 v42-v44 Majordomo losses (route-0914v-executus-04: 25 s at
    // 100 % with 20 attackers). v45: one elected damage member - the lease holder first, then GUID order - breaks it
    // with a direct damage cast, and the lease is dropped so nobody re-controls it while it ranks first.
    bool BreakControl(AiBotAI* ai,Plan& plan,Assignment& row,Creature* release,std::chrono::steady_clock::time_point now)
    {
        auto recent=plan.releases.find(release->GetObjectGuid());
        if(recent!=plan.releases.end()&&now-recent->second<std::chrono::seconds(3))return false; // one landing break at a time
        if(now-plan.breakerAt>=std::chrono::milliseconds(500))
        {
            plan.breakerAt=now;plan.breaker.Clear();
            ObjectGuid controller;
            auto lease=plan.controls.find(release->GetObjectGuid());
            if(lease!=plan.controls.end())controller=lease->second.controller;
            auto able=[&](Assignment const& other)
            {
                if(other.manual||other.yielded||(other.role!=4&&other.role!=5))return false;
                Player* member=Member(plan,other.guid);
                return member&&member->IsAlive()&&!member->IsNonMeleeSpellCasted(false,false,true)&&ReleaseSpellFor(member,release)!=nullptr;
            };
            for(auto const& other:plan.rows)if(other.guid==controller&&able(other)){plan.breaker=other.guid;break;}
            if(plan.breaker.IsEmpty())
                for(auto const& other:plan.rows)
                    if(able(other)&&(plan.breaker.IsEmpty()||other.guid.GetRawValue()<plan.breaker.GetRawValue()))plan.breaker=other.guid;
        }
        if(plan.breaker!=row.guid)return false;
        Player* actor=ai->GetBotPlayer();
        SpellEntry const* spell=ReleaseSpellFor(actor,release);
        if(!spell||!ai->CommanderRaidCast(release,spell->Id))return false;
        plan.releases[release->GetObjectGuid()]=now;plan.breakerAt={};
        plan.controls.erase(release->GetObjectGuid());
        sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-release] actor=%u target=%llu entry=%u spell=%u",
            actor->GetGUIDLow(),static_cast<unsigned long long>(Raw(release->GetObjectGuid())),release->GetEntry(),spell->Id);
        row.target=release->GetObjectGuid();row.duty=5;
        return true;
    }
    // A damage member with a control spell holds one fighting add the raid has not taken yet. Adds are taken
    // from the end of the kill order (control goes on what dies last), GUID order within a rank; a controller
    // keeps its own add (re-cast on expiry) and never takes a second; the boss, or failing that the last
    // uncontrolled add, stays an active objective so the fight always has a target.
    bool NativeCrowdControl(AiBotAI* ai,Plan& plan,Assignment& row,Unit* boss)
    {
        if(row.role!=4&&row.role!=5)return false;
        Player* actor=ai->GetBotPlayer();if(!actor||!actor->IsAlive()||plan.definition.adds.empty())return false;
        auto const now=std::chrono::steady_clock::now();
        std::list<Creature*> found;
        for(uint32 entry:plan.definition.adds)actor->GetCreatureListWithEntryInGrid(found,entry,100);
        found.sort([](Creature* a,Creature* b){return a->GetObjectGuid().GetRawValue()<b->GetObjectGuid().GetRawValue();});
        found.unique([](Creature* a,Creature* b){return a->GetObjectGuid()==b->GetObjectGuid();});
        std::vector<Creature*> fighting;
        for(Creature* add:found)if(add->IsAlive()&&add->IsInCombat()&&!add->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)&&InRoom(plan,{add->GetPositionX(),add->GetPositionY(),add->GetPositionZ()}))fighting.push_back(add);
        if(fighting.empty())return false;
        std::stable_sort(fighting.begin(),fighting.end(),[&](Creature* a,Creature* b){return KillRank(plan,a)>KillRank(plan,b);}); // last in the kill order is claimed first
        bool const bossActive=boss&&boss->IsAlive()&&boss->IsInCombat()&&std::find(fighting.begin(),fighting.end(),boss)==fighting.end();
        size_t controlled=0;Creature* mine=nullptr;
        for(Creature* add:fighting)
        {
            if(!ControlledAdd(plan,add,now))continue;
            ++controlled;
            auto lease=plan.controls.find(add->GetObjectGuid());
            if(lease!=plan.controls.end()&&lease->second.controller==actor->GetObjectGuid())mine=add;
        }
        size_t const limit=bossActive?fighting.size():fighting.size()-1;
        // The released add (v45): controlled, ranked before every uncontrolled fighting add. Broken, never refreshed.
        Creature* release=nullptr;float looseRank=1e9f;
        for(Creature* add:fighting)if(!ControlledAdd(plan,add,now))looseRank=std::min(looseRank,KillRank(plan,add));
        for(Creature* add:fighting)if(KillRank(plan,add)<looseRank&&ControlledAdd(plan,add,now)&&(!release||KillsBefore(plan,add,release)))release=add;
        if(release&&BreakControl(ai,plan,row,release,now))return true;
        if(mine&&mine==release)return false;
        // Own add still held: refresh the control inside the last six seconds of its lease (2026-09-14, v43 - a lapsed
        // sheep drops the add onto its top-threat member, a healer, with the tank's lead at zero); otherwise nothing
        // to do (the rotation may not target it - see the candidate loop).
        if(mine)
        {
            auto lease=plan.controls.find(mine->GetObjectGuid());
            if(lease!=plan.controls.end()&&lease->second.expires-now<std::chrono::seconds(6)&&now-lease->second.cast>=std::chrono::seconds(3))
            {
                // Wait out a reflect window unless the hold is about to end anyway (v51).
                if(ReflectsSpells(mine)&&lease->second.expires-now>std::chrono::milliseconds(1500))return false;
                if(!RefreshControl(ai,mine,lease->second.spell))return false;
                SpellEntry const* spell=sSpellMgr.GetSpellEntry(lease->second.spell);
                lease->second.cast=now;lease->second.expires=now+std::chrono::milliseconds(std::max(5000,spell?spell->GetDuration():5000));
                row.target=mine->GetObjectGuid();row.duty=14;return true;
            }
            return false;
        }
        // Own lease expired on a still-fighting add: take it again before anyone else.
        for(auto const& lease:plan.controls)if(lease.second.controller==actor->GetObjectGuid())return false;
        if(controlled>=limit)return false;
        // Control goes on what dies last, never on the raid's kill target: the first uncontrolled fighting add in the
        // kill order. Its siblings of the same rank are control targets (three healers stand sheeped while the fourth
        // is killed); when it dies the damage rotation releases the next in the order (2026-09-14, v42 - v41 excluded
        // the whole first rank, so with polymorph-immune late ranks nothing was ever controlled and eight adds walked
        // the raid).
        Creature* killTarget=nullptr;
        for(Creature* add:fighting)if(!ControlledAdd(plan,add,now)&&(!killTarget||KillsBefore(plan,add,killTarget)))killTarget=add;
        for(Creature* add:fighting)
        {
            if(ControlledAdd(plan,add,now))continue;
            if(add==killTarget)continue;
            if(ReflectsSpells(add))continue; // claim it once the reflect window ends (v51)
            SpellEntry const* spell=ControlSpellFor(actor,add);
            if(!spell)continue;
            bool claimed=false;
            for(auto const& lease:plan.controls)if(lease.first==add->GetObjectGuid()){claimed=true;break;}
            if(claimed)continue;
            // Lower-GUID controllers claim first: every controller sees the same ordered lists.
            bool someoneElse=false;
            for(auto const& other:plan.rows)
            {
                if(other.guid==row.guid||other.manual||other.yielded||(other.role!=4&&other.role!=5)||other.guid.GetRawValue()>=row.guid.GetRawValue())continue;
                bool busy=false;for(auto const& lease:plan.controls)if(lease.second.controller==other.guid){busy=true;break;}
                if(busy)continue;
                Player* candidate=Member(plan,other.guid);
                if(candidate&&candidate->IsAlive()&&ControlSpellFor(candidate,add)){someoneElse=true;break;}
            }
            if(someoneElse)continue;
            SuspendControlledTargetDamage(plan,add);
            if(!actor->IsMoving()&&actor->IsSpellReady(spell->Id)&&actor->IsWithinLOSInMap(add)&&
                actor->IsWithinDist(add,Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex)),true))
                PreemptDamageCast(actor); // v64: the claim does not wait for the damage rotation to go idle
            if(!ai->CommanderRaidCast(add,spell->Id))return false; // Not ready (range, cooldown, casting): the ordinary rotation resumes this tick.
            Plan::ControlLease lease;lease.controller=actor->GetObjectGuid();lease.spell=spell->Id;lease.cast=now;
            lease.expires=now+std::chrono::milliseconds(std::max(5000,spell->GetDuration()));
            plan.controls[add->GetObjectGuid()]=lease;
            row.target=add->GetObjectGuid();row.duty=14;return true;
        }
        return false;
    }
    // A tank-capable member: an assigned tank role, or a melee row carrying a learned taunt.
    bool TankCapable(Assignment const& row)
    {
        return !row.yielded&&(row.role<=2||(row.taunt&&row.role==4));
    }
    // An objective in combat that nobody holds: its victim is not a tank-capable member of the plan (a threat reset
    // that landed it on a caster, a dead tank, a pet). Until a tank has it back it walks the raid.
    bool LooseObjective(Plan const& plan,Unit* boss)
    {
        if(!boss||!boss->IsAlive()||!boss->IsInCombat())return false;
        Unit* victim=boss->GetVictim();
        if(!victim)return true;
        for(auto const& row:plan.rows)if(row.guid==victim->GetObjectGuid())return !TankCapable(row);
        return true;
    }
    Player* SelectActingTank(Plan& plan,Unit* boss,bool fighting,std::chrono::steady_clock::time_point now)
    {
        Player* primary=nullptr;Player* backup=nullptr;Player* fallback=nullptr;
        for(auto const& row:plan.rows)
        {
            // A living taunt-capable member of any role is the last resort once every assigned tank is
            // dead: without one the damage roles hold their threat ratio against nobody and the boss
            // walks the raid until it evades (a single-spawn boss derives no add tanks at all).
            bool const assigned=(row.role==1&&row.focus==boss->GetEntry())||row.role==2;
            if(!assigned&&!row.taunt)continue;
            Player* member=Member(plan,row.guid);
            if(!member||!member->IsAlive()||row.yielded||
                (!row.manual&&SuiPossess::IsSuiPossessed(member)&&!SuiPossess::IsCommandedFromFreeView(member)))continue;
            // Inspect the entire roster: the primary normally precedes every backup.
            Player*& selected=!assigned?fallback:row.role==1?primary:backup;
            if(!selected||Raw(member->GetObjectGuid())<Raw(selected->GetObjectGuid()))selected=member;
        }
        if(!primary&&!backup)backup=fallback;
        // A leftover add already on a tank-role member is tanked by that member: the threat discipline of the
        // damage roles must follow the tank that actually holds it, not the lowest-GUID add tank.
        // The tank-capable member the objective actually attacks - a leftover add on an add tank, a pickup after a
        // threat reset - is the acting tank while it holds it (2026-09-14, v36): the damage roles' threat discipline
        // and the healers' range follow the holder, not the assigned primary walking back in from where the reset
        // left it.
        Player* holder=nullptr;
        for(auto const& row:plan.rows)if(TankCapable(row))if(Player* member=Member(plan,row.guid))
            if(member->IsAlive()&&boss->GetVictim()==member){holder=member;break;}
        if(!primary&&holder)backup=holder;
        auto& contact=plan.tankContacts[boss->GetObjectGuid()];
        ObjectGuid primaryGuid=primary?primary->GetObjectGuid():ObjectGuid();
        if(contact.primary!=primaryGuid){contact.primary=primaryGuid;contact.lostSince={};}
        bool melee=false;
        for(auto const& phase:plan.definition.phases)if(phase.id==plan.phase){melee=MeleeAllowed(phase,1);break;}
        Player* tank=primary?primary:backup;
        if(!primary||!backup||!fighting||!boss->IsInCombat()||!melee||
            primary->CanReachWithMeleeAutoAttack(boss)||boss->CanReachWithMeleeAutoAttack(primary))contact.lostSince={};
        else if(contact.lostSince==std::chrono::steady_clock::time_point{})contact.lostSince=now;
        else if(now-contact.lostSince>=std::chrono::milliseconds(4000))tank=backup;
        if(holder&&holder!=primary&&fighting&&boss->IsInCombat())tank=holder;
        ObjectGuid acting=tank?tank->GetObjectGuid():ObjectGuid();
        if(contact.acting!=acting)
        {
            sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-acting-tank] objective=%u tank=%u previous=%u",boss->GetEntry(),acting.GetCounter(),contact.acting.GetCounter());
            contact.acting=acting;
        }
        return tank;
    }

    bool RecoverFeignDeath(Player* actor, Assignment& row, uint32 diff)
    {
        // Voluntary threat shedding must end before the generic lost-control
        // gate, which otherwise suppresses this actor for the whole aura duration.
        if (!actor->HasUnitState(UNIT_STATE_FEIGN_DEATH) || !actor->HasAuraType(SPELL_AURA_FEIGN_DEATH))
        { row.feignRecoveryMs=0;return false; }
        row.feignRecoveryMs += std::min(diff, uint32(1500)-row.feignRecoveryMs);
        if (row.feignRecoveryMs<1500) {row.duty=8;return true;}
        actor->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
        row.feignRecoveryMs=0;row.target.Clear();row.hasMove=false;
        // Re-enter the ordinary rules, target selection, and threat gates.
        return false;
    }
    bool Commandable(Player* owner, Player* actor)
    {
        return owner && actor && owner->GetGroup() && actor->GetGroup() == owner->GetGroup()
            && actor->GetSession() && actor->GetSession()->GetBot()
            && SuiCompanion::MayCommand(owner, actor) && dynamic_cast<AiBotAI*>(actor->AI());
    }
    void Stop(Assignment& row, Plan const& plan)
    {
        if (row.manual || row.yielded) return;
        if (Player* p = Member(plan, row.guid))
        {
            // Never stop a manually driven body, even when an old plan still names it.
            if ((SuiPossess::IsSuiPossessed(p) && !SuiPossess::IsCommandedFromFreeView(p)) || p->IsSuiTacticallyFrozen()) return;
            p->InterruptNonMeleeSpells(false); p->AttackStop();
            p->StopMoving(); p->GetMotionMaster()->MoveIdle();
            if (Pet* pet = p->GetPet()) pet->AttackStop();
        }
        row.hasMove = false; row.escapeGoalReady=false; row.duty = 0; row.target.Clear();
    }
    struct Advice { uint8 state=0; uint16 rule=0; uint32 impact=0; Point waypoint{}; };
    Advice Guidance(Plan const& plan,Assignment const& row);
    void Reply(WorldSession* session, uint32 request, uint8 result, Plan const* plan)
    {
    SlowRaidScope slow{"reply",0};
        Unit* boss=nullptr;
        if(plan&&!plan->boss.IsEmpty())if(Player* owner=Member(*plan,plan->owner))boss=owner->GetMap()->GetUnit(plan->boss);
        Objective const* terminal=nullptr;
        if(plan){auto it=plan->objectives.find(plan->definition.boss);if(it!=plan->objectives.end()&&it->second.seen&&it->second.dead)terminal=&it->second;}
        // The protocol-4 client parses only the three low bits (it drops any status above 7 - the v35 wedge); the yield
        // bits are logged with the reply for the review adapters instead of being sent.
        uint8 const bossFlags=boss?uint8(1|(boss->IsAlive()?2:0)|(boss->IsInCombat()?4:0)):(terminal?1:0);
        uint8 const yieldFlags=boss?uint8((boss->HasFlag(UNIT_FIELD_FLAGS,UNIT_FLAG_IMMUNE_TO_PLAYER|UNIT_FLAG_NON_ATTACKABLE_2|UNIT_FLAG_NOT_ATTACKABLE_1)?16:0)):0;
        WorldPacket packet(SMSG_SUI_COMMANDER_RAID, 30 + (plan ? plan->rows.size() * 37 : 0));
        packet << uint8(4) << request << uint32(plan ? plan->revision : 0) << result
            << uint8(plan ? plan->state : 0) << uint8(plan?plan->phase:0) << uint64(plan?Raw(plan->boss):0)
            << uint8(plan ? plan->rows.size() : 0)
            << uint32(boss?boss->GetHealth():0) << uint32(boss?boss->GetMaxHealth():terminal?terminal->maxHealth:0) << bossFlags;
        if (plan) for (auto const& row : plan->rows)
        {
            Advice const advice=Guidance(*plan,row);
            Player* actor=Member(*plan,row.guid);
            uint8 const actorFlags=actor?uint8(1|(actor->IsAlive()?2:0)|(actor->IsInCombat()?4:0)):0;
            packet << row.guid << row.duty << row.target << actorFlags << advice.state << advice.rule << advice.impact
                << advice.waypoint.x << advice.waypoint.y << advice.waypoint.z;
        }
        session->SendPacket(&packet);
        sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-reply] request=%u result=%u state=%u bossFlags=%u yieldFlags=%u rows=%u",request,unsigned(result),unsigned(plan?plan->state:0),unsigned(bossFlags),unsigned(yieldFlags),unsigned(plan?plan->rows.size():0));
    }
    bool IsHeal(SpellEntry const* spell)
    {
        if(!spell || spell->IsPassiveSpell())return false;
        // Assigned healing must land on the selected ally, never a self-only racial.
        bool friendly=false;for(uint32 target:spell->EffectImplicitTargetA)if(target==21)friendly=true;
        if(!friendly)return false;
        // Holy Light uses SCRIPT_EFFECT in this client's actual spell data.
        for(uint32 effect:spell->Effect)if(effect==SPELL_EFFECT_HEAL)return true;
        return (spell->SpellName[0]=="Holy Light" || spell->SpellName[0]=="Flash of Light") &&
            spell->Effect[0]==SPELL_EFFECT_SCRIPT_EFFECT;
    }
    bool Learned(Player* actor, uint32 id, bool heal)
    {
        SpellEntry const* spell = id ? sSpellMgr.GetSpellEntry(id) : nullptr;
        if (!spell || !actor->HasSpell(id) || spell->IsPassiveSpell()) return false;
        if(heal)return IsHeal(spell);
        for (uint32 effect : spell->Effect)if(effect==SPELL_EFFECT_SCHOOL_DAMAGE)return true;
        return spell->IsAutoRepeatRangedSpell();
    }
    bool ObjectivePrioritySafe(uint8 role,bool attackable,float objectiveHealthPercent,bool required,
        bool ordinaryRank,float addMaximum,float victimHealth,float victimMaximum,float pressure,bool casting)
    {
        return (role==4||role==5)&&attackable&&objectiveHealthPercent>0&&objectiveHealthPercent<=35.f&&
            !required&&ordinaryRank&&victimMaximum>0&&addMaximum<=victimMaximum&&
            victimHealth>=victimMaximum*.5f&&!casting&&std::isfinite(pressure)&&pressure>=0&&
            victimHealth-pressure>victimMaximum*.35f;
    }
    bool MinorAddCanWait(Plan const& plan,Assignment const& row,Phase const& phase,Unit* objective,Creature* add)
    {
        bool const attackable=row.role==5?phase.ranged:phase.melee;
        if((row.role!=4&&row.role!=5)||!attackable||objective->GetHealthPercent()>35.f)return false;
        bool required=std::find(plan.definition.objectives.begin(),plan.definition.objectives.end(),add->GetEntry())!=plan.definition.objectives.end();
        for(auto const& requirement:plan.definition.requiredAdds)required|=requirement.entry==add->GetEntry();
        Unit* victim=add->GetVictim();
        Player* patient=victim?Member(plan,victim->GetObjectGuid()):nullptr;
        if(!patient||!patient->IsAlive())return false;
        std::set<Unit*> attackers(patient->GetAttackers().begin(),patient->GetAttackers().end());
        attackers.insert(add);float pressure=0;bool casting=false;unsigned inspected=0;
        for(Unit* enemy:attackers)
        {
            if(++inspected>64)return false;
            if(!enemy||!enemy->IsAlive()||!enemy->IsInCombat()||!enemy->IsHostileTo(patient))continue;
            // The objective has its own tank/threat/healing gates; estimate the
            // additional pressure from all other current attackers together.
            if(std::find(plan.definition.objectives.begin(),plan.definition.objectives.end(),enemy->GetEntry())!=plan.definition.objectives.end())continue;
            casting|=enemy->IsNonMeleeSpellCasted(false,false,true);
            pressure+=std::max(0.f,enemy->GetFloatValue(UNIT_FIELD_MAXDAMAGE))*
                std::ceil(3000.f/std::max(1u,enemy->GetAttackTime(BASE_ATTACK)));
            if(enemy->HaveOffhandWeapon())pressure+=std::max(0.f,enemy->GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE))*
                std::ceil(3000.f/std::max(1u,enemy->GetAttackTime(OFF_ATTACK)));
        }
        // Loaded maximum unmitigated, noncritical swings; reevaluate each tick.
        // This is a priority estimate, not a guarantee against future abilities.
        return ObjectivePrioritySafe(row.role,attackable,objective->GetHealthPercent(),required,
            add->GetCreatureInfo()->rank==CREATURE_ELITE_NORMAL,add->GetMaxHealth(),
            patient->GetHealth(),patient->GetMaxHealth(),pressure,casting);
    }

    bool SupportLearned(Player* actor,uint32 id,uint8 kind)
    {
        if(!id)return true;
        auto spell=sSpellMgr.GetSpellEntry(id);
        if(!spell||!actor->HasSpell(id)||spell->IsPassiveSpell())return false;
        for(size_t i=0;i<3;i++)
            if((kind==13&&spell->Effect[i]==SPELL_EFFECT_INTERRUPT_CAST)||
                (kind==14&&spell->Effect[i]==SPELL_EFFECT_DISPEL)||
                (kind==2&&(spell->Effect[i]==SPELL_EFFECT_ATTACK_ME||spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_TAUNT))||
                (kind==4&&spell->EffectApplyAuraName[i]==SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN&&spell->EffectBasePoints[i]<0))return true;
        return false;
    }
    bool Claimed(Plan const& plan,ObjectGuid target,uint8 kind)
    {
        for(auto const& claim:plan.claims)if(claim.kind==kind&&claim.target==target)return true;
        return false;
    }
    uint32 Incoming(Plan const& plan,ObjectGuid target,uint32 horizon=0)
    {
        uint32 total=0;for(auto const& claim:plan.claims)if(claim.kind==4&&claim.target==target&&(!horizon||claim.remaining<=horizon))total+=claim.amount;
        return total;
    }
    bool SupportCast(AiBotAI* ai,Plan& plan,Assignment& row,Unit* target,uint32 spell,uint8 duty)
    {
        uint8 const claimKind=duty==4?17:duty; // Defensive support never collides with incoming-heal claims.
        if(!target||!spell||Claimed(plan,target->GetObjectGuid(),claimKind))return false;
        if(!ai->CommanderRaidCast(target,spell))return false;
        plan.claims.push_back({row.guid,target->GetObjectGuid(),claimKind,750,0});
        row.duty=duty;row.target=target->GetObjectGuid();return true;
    }
    void StopIdleWand(Player* actor)
    {
        actor->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true);
        actor->AttackStop();
    }
    struct DirectDamageForecast { float amount=0; uint32 deadline=0; };
    DirectDamageForecast ForecastDirectDamage(Plan const& plan,Player* patient)
    {
        DirectDamageForecast risk;
        std::set<Unit*> enemies(patient->GetAttackers().begin(),patient->GetAttackers().end());
        for(auto const& objective:plan.objectives)
            if(Unit* enemy=patient->GetMap()->GetUnit(objective.second.guid))enemies.insert(enemy);
        unsigned inspected=0;
        for(Unit* enemy:enemies)
        {
            if(++inspected>64)break;
            if(!enemy||!enemy->IsAlive()||!enemy->IsInCombat()||!enemy->IsHostileTo(patient))continue;
            Spell* cast=enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if(!cast||cast->getState()!=SPELL_STATE_PREPARING||cast->IsChanneled()||
                cast->m_targets.getUnitTargetGuid()!=patient->GetObjectGuid())continue;
            uint32 const remaining=cast->GetCastedTime(); // Core's countdown, not original cast duration.
            if(!remaining||remaining>3000)continue;
            auto spell=cast->m_spellInfo;
            float damage=0;
            for(uint8 i=0;i<3;i++)if(spell->Effect[i]==SPELL_EFFECT_SCHOOL_DAMAGE)
            {
                auto index=SpellEffectIndex(i);
                float base=std::max(0,cast->m_currentBasePoints[i]+std::max(0,spell->EffectDieSides[i]-1));
                base=enemy->SpellDamageBonusDone(patient,spell,index,base,SPELL_DIRECT_DAMAGE);
                damage+=std::max(0.f,patient->SpellDamageBonusTaken(enemy,spell,index,base,SPELL_DIRECT_DAMAGE));
            }
            // Noncritical, pre-resistance estimate for observed direct casts only.
            // Scripted/triggered effects and ground areas are not certified harmless.
            if(damage<=0)continue;
            risk.amount+=damage;
            if(!risk.deadline||remaining<risk.deadline)risk.deadline=remaining;
        }
        return risk;
    }
    bool BurstUrgent(float health,float maximum,float timelyHealing,float damage)
    {
        return maximum>0&&damage>0&&health+timelyHealing-damage<maximum*.35f;
    }
    void ObserveHealer(Plan const& plan,Assignment const& row,Player* actor)
    {
        if(row.role!=3||!plan.enteredCombat||plan.state!=2)return;
        static std::map<uint64,std::chrono::steady_clock::time_point> last;
        auto now=std::chrono::steady_clock::now();auto& previous=last[Raw(row.guid)];
        if(now-previous<std::chrono::seconds(1))return;previous=now;
        Player* primary=Member(plan,row.primary);Unit* target=actor->GetMap()->GetUnit(row.target);
        Spell* cast=actor->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-healer] actor=%u health=%u mana=%u duty=%u moving=%u control=%u cast=%u castState=%u remaining=%u castTarget=%llu primary=%llu primaryHealth=%u primaryDistance=%.2f primaryLos=%u target=%llu targetHealth=%u targetDistance=%.2f targetLos=%u incoming=%u",
            actor->GetGUIDLow(),actor->GetHealth(),actor->GetPower(POWER_MANA),unsigned(row.duty),unsigned(actor->IsMoving()),
            unsigned(actor->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)),cast?cast->m_spellInfo->Id:0u,
            cast?unsigned(cast->getState()):0u,cast?cast->GetCastedTime():0u,
            static_cast<unsigned long long>(cast?Raw(cast->m_targets.getUnitTargetGuid()):0),
            static_cast<unsigned long long>(Raw(row.primary)),primary?primary->GetHealth():0u,primary?actor->GetDistance(primary):-1.f,
            unsigned(primary&&actor->IsWithinLOSInMap(primary)),static_cast<unsigned long long>(Raw(row.target)),target?target->GetHealth():0u,
            target?actor->GetDistance(target):-1.f,unsigned(target&&actor->IsWithinLOSInMap(target)),Incoming(plan,row.target));
    }
    Player* HealingPatient(Player* actor,Plan const& plan,Assignment const& row,bool emergencyOnly)
    {
        Player* patient=nullptr;float best=100000;
        for(auto const& candidate:plan.rows)
        {
            Player* p=Member(plan,candidate.guid);
            if(!p||!p->IsAlive()||!p->GetMaxHealth()||!actor->IsWithinDistInMap(p,40)||!actor->IsWithinLOSInMap(p))continue;
            float actual=p->GetHealthPercent();
            // A slow reserved heal cannot cover an urgent patient before it lands.
            // Still coordinate heals arriving within one ordinary fast-heal cast.
            auto const risk=ForecastDirectDamage(plan,p);
            uint32 const horizon=risk.deadline?risk.deadline:actual<50?1750:0;
            float projected=100.f*(p->GetHealth()+Incoming(plan,p->GetObjectGuid(),horizon))/p->GetMaxHealth();
            bool const urgent=BurstUrgent(p->GetHealth(),p->GetMaxHealth(),Incoming(plan,p->GetObjectGuid(),horizon),risk.amount);
            if(projected>=(urgent?99.f:92.f)||(emergencyOnly&&actual>=50&&!urgent))continue;
            float score=projected-100.f*risk.amount/p->GetMaxHealth()-(candidate.guid==row.primary?22:0)-(candidate.role<=2?12:0)-(candidate.team==row.team?6:0);
            if(actual<30)score-=50;
            if(score<best){best=score;patient=p;}
        }
        return patient;
    }
    bool Heal(AiBotAI* ai,Plan& plan,Assignment& row,bool emergencyOnly)
    {
        Player* actor=ai->GetBotPlayer();
        Player* patient=HealingPatient(actor,plan,row,emergencyOnly);
        if(!patient)return false;
        StopIdleWand(actor); // Healing demand preempts Shoot even if mana or cooldown prevents a cast.
        row.target=patient->GetObjectGuid();row.duty=4;
        if(actor->IsNonMeleeSpellCasted(false,false,true))return true;
        // A critical heal outranks returning to a station. Movement mechanics already ran.
        if(actor->IsMoving()){actor->StopMoving();actor->GetMotionMaster()->MoveIdle();row.hasMove=false;}
        std::vector<SpellEntry const*> choices;
        if(auto chosen=sSpellMgr.GetSpellEntry(row.heal))choices.push_back(chosen);
        for(auto const& learned:actor->GetSpellMap())
        {
            if(learned.second.state==PLAYERSPELL_REMOVED||learned.second.disabled||learned.first==row.heal)continue;
            auto spell=sSpellMgr.GetSpellEntry(learned.first);if(IsHeal(spell))choices.push_back(spell);
        }
        // Rank choice belongs to the healing role, not the encounter. Estimate
        // ordinary noncritical healing with actual caster/patient bonuses. Claims
        // reserve only the remaining deficit so parallel healers do not pile on.
        struct Candidate { SpellEntry const* spell; uint32 cost, amount, cast; float useful; bool adequate; };
        std::vector<Candidate> ranked;
        auto const risk=ForecastDirectDamage(plan,patient);
        uint32 const horizon=risk.deadline?risk.deadline:patient->GetHealthPercent()<50?1750:0;
        uint32 const incoming=Incoming(plan,patient->GetObjectGuid(),horizon);
        bool const urgent=emergencyOnly||row.healMovementInterrupted||BurstUrgent(patient->GetHealth(),patient->GetMaxHealth(),incoming,risk.amount);
        float const missing=std::max(0.f,float(patient->GetMaxHealth())-patient->GetHealth()-incoming);
        float const minimum=std::min(missing,patient->GetMaxHealth()*(urgent?.25f:.15f));
        for(auto spell:choices)
        {
            uint32 cost=Spell::CalculatePowerCost(spell,actor,nullptr,nullptr,false);
            if(spell->powerType==POWER_MANA&&cost>actor->GetPower(POWER_MANA))continue;
            float amount=0;
            for(uint8 i=0;i<3;i++)if(spell->Effect[i]==SPELL_EFFECT_HEAL||spell->Effect[i]==SPELL_EFFECT_SCRIPT_EFFECT)
            {
                auto index=SpellEffectIndex(i);
                float base=std::max(0,spell->CalculateSimpleValue(index));
                base=actor->SpellHealingBonusDone(patient,spell,index,base,HEAL);
                amount+=patient->SpellHealingBonusTaken(actor,spell,index,base,HEAL);
            }
            if(amount<=0)continue;
            ranked.push_back({spell,cost,uint32(amount),spell->GetCastTime(actor),std::min(amount,missing),amount>=minimum});
        }
        float const rescue=std::max(0.f,risk.amount-float(patient->GetHealth())-incoming+patient->GetMaxHealth()*.1f);
        std::sort(ranked.begin(),ranked.end(),[urgent,risk,row,rescue](Candidate const& a,Candidate const& b)
        {
            bool const timelyA=!risk.deadline||a.cast+250<risk.deadline,timelyB=!risk.deadline||b.cast+250<risk.deadline;
            if(urgent&&timelyA!=timelyB)return timelyA;
            // If both can beat an observed impact, first choose enough healing
            // to survive it. Otherwise urgent care delivers the earliest heal,
            // using the largest useful rank at that speed. A percentage-size
            // cutoff must not force a slow cast when no heal can beat the impact.
            if(urgent&&risk.deadline&&timelyA&&timelyB&&(a.useful>=rescue)!=(b.useful>=rescue))return a.useful>=rescue;
            if(urgent&&a.cast!=b.cast)return a.cast<b.cast;
            if(urgent&&a.useful!=b.useful)return a.useful>b.useful;
            if(a.adequate!=b.adequate)return a.adequate;
            float const as=a.useful/std::max(1u,a.cost),bs=b.useful/std::max(1u,b.cost);
            if(as!=bs)return as>bs;
            if(a.cast!=b.cast)return a.cast<b.cast;
            if((a.spell->Id==row.heal)!=(b.spell->Id==row.heal))return a.spell->Id==row.heal;
            return a.spell->Id<b.spell->Id;
        });
        for(auto const& choice:ranked)
        {
            if(!ai->CommanderRaidCast(patient,choice.spell->Id))continue;
            // One fast replacement after an actual interrupted heal. Another
            // interruption renews this request; a rejected cast cannot consume it.
            row.healMovementInterrupted=false;
            plan.claims.push_back({row.guid,patient->GetObjectGuid(),4,std::max(500u,choice.cast+250),uint32(choice.useful)});
            sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-heal-cast] actor=%u target=%u health=%u maximum=%u spell=%u cast=%u amount=%u cost=%u incoming=%u urgent=%u",
                actor->GetGUIDLow(),patient->GetGUIDLow(),patient->GetHealth(),patient->GetMaxHealth(),choice.spell->Id,choice.cast,choice.amount,choice.cost,incoming,unsigned(urgent));
            return true;
        }
        row.duty=actor->GetPowerPercent(POWER_MANA)<15?15:16;
        return false;
    }
    bool IdleWand(AiBotAI* ai,Plan& plan,Assignment& row,Unit* enemy,Player* tank,Phase const& phase)
    {
        Player* actor=ai->GetBotPlayer();
        Item* wand=actor->GetWeaponForAttack(RANGED_ATTACK,true,true);
        auto shoot=sSpellMgr.GetSpellEntry(AB_SPELL_SHOOT_WAND);
        bool const threatSafe=phase.threatRatio<=0 || (tank&&enemy&&enemy->GetVictim()==tank&&
            enemy->GetThreatManager().getThreat(actor)<=enemy->GetThreatManager().getThreat(tank)*phase.threatRatio);
        // Share patient selection with Heal: an unsuccessful needed heal is not idle time.
        // Never chase for damage, spend mana, or start combat from an armed plan.
        if(!enemy||!enemy->IsAlive()||!enemy->IsInCombat()||!phase.ranged||!threatSafe||
            actor->IsMoving()||actor->IsNonMeleeSpellCasted(false,false,true)||
            (tank&&tank->IsAlive()&&tank->GetHealthPercent()<50)||HealingPatient(actor,plan,row,false)||!wand||wand->GetProto()->SubClass!=ITEM_SUBCLASS_WEAPON_WAND||
            !shoot||!actor->HasSpell(AB_SPELL_SHOOT_WAND)||!actor->IsWithinLOSInMap(enemy)||
            !actor->IsWithinDistInMap(enemy,Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(shoot->rangeIndex)))||
            enemy->IsImmuneToDamage(SpellSchoolMask(1u<<wand->GetProto()->Damage[0].DamageType)))
        {StopIdleWand(actor);return false;}
        if(actor->GetVictim()!=enemy)
        {StopIdleWand(actor);if(!actor->Attack(enemy,false))return false;}
        actor->SetInFront(enemy);
        // Leave an existing normal autorepeat running. Reissuing Shoot on each
        // planner tick resets its work and attempts duplicate cooldown entries.
        if(Spell* repeat=actor->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            if(repeat->m_spellInfo->Id==AB_SPELL_SHOOT_WAND&&repeat->m_targets.getUnitTargetGuid()==enemy->GetObjectGuid())
            {row.target=enemy->GetObjectGuid();row.duty=5;return true;}
        if(!ai->CommanderRaidCast(enemy,AB_SPELL_SHOOT_WAND))
        {StopIdleWand(actor);return false;}
        row.target=enemy->GetObjectGuid();row.duty=5;
        return true;
    }
    uint32 HazardDamageSchools(Definition const& definition,uint8 phase)
    {
        std::vector<uint32> pending;std::set<uint32> visited;uint32 schools=0;
        for(auto const& rule:definition.rules)
        {
            if(rule.phase&&rule.phase!=phase)continue;
            if(rule.action!=Action::AvoidCones&&rule.action!=Action::AvoidPoints&&rule.action!=Action::Isolate)continue;
            pending.insert(pending.end(),rule.spells.begin(),rule.spells.end());
            pending.insert(pending.end(),rule.points.begin(),rule.points.end());
            if(rule.spell)pending.push_back(rule.spell);
        }
        while(!pending.empty())
        {
            uint32 id=pending.back();pending.pop_back();
            if(!id||!visited.insert(id).second)continue;
            auto spell=sSpellMgr.GetSpellEntry(id);if(!spell)continue;
            for(uint8 effect=0;effect<3;++effect)
            {
                bool const damage=spell->Effect[effect]==SPELL_EFFECT_SCHOOL_DAMAGE||
                    ((spell->Effect[effect]==SPELL_EFFECT_APPLY_AURA||spell->Effect[effect]==SPELL_EFFECT_PERSISTENT_AREA_AURA)&&
                     (spell->EffectApplyAuraName[effect]==SPELL_AURA_PERIODIC_DAMAGE||spell->EffectApplyAuraName[effect]==SPELL_AURA_PERIODIC_DAMAGE_PERCENT));
                if(damage)schools|=uint32(spell->GetSpellSchoolMask());
                if(spell->EffectTriggerSpell[effect])pending.push_back(spell->EffectTriggerSpell[effect]);
            }
        }
        return schools;
    }
    void TryCarriedPotion(Player* actor,Assignment& row,uint32 diff,uint32 damageSchools)
    {
        if(!diff)return; // Read-only zero-diff observation cannot issue an item use.
        if(row.potionTick>diff){row.potionTick-=diff;return;}
        row.potionTick=1000;
        if(!actor||!actor->IsAlive()||!actor->IsInCombat()||actor->IsSuiTacticallyFrozen()||
            actor->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)||!actor->GetSession()||
            actor->GetSession()->GetSuiActor()!=actor||actor->IsNonMeleeSpellCasted(false,false,true))return;
        // Match the existing manual driver's healing threshold; bots retain theirs.
        bool const hurt=actor->GetHealthPercent()<(row.manual?50:40);
        bool const lowMana=actor->GetMaxPower(POWER_MANA)>0&&actor->GetPowerPercent(POWER_MANA)<35;
        if(!hurt&&!lowMana&&!damageSchools)return;
        Item* best=nullptr;uint8 bestSpell=0,bestKind=0;float bestScore=0;uint32 bestSchool=0;
        auto inspect=[&](Item* item)
        {
            if(!item||item->IsInTrade()||actor->CanUseItem(item)!=EQUIP_ERR_OK)return;
            auto proto=item->GetProto();
            if(proto->Class!=ITEM_CLASS_CONSUMABLE)return;
            for(uint8 i=0;i<MAX_ITEM_PROTO_SPELLS;i++)
            {
                auto const& use=proto->Spells[i];auto spell=sSpellMgr.GetSpellEntry(use.SpellId);
                if(use.SpellTrigger!=ITEM_SPELLTRIGGER_ON_USE||use.SpellCategory!=4||!spell||spell->IsNonCombatSpell()||
                    spell->GetCastTime(actor)!=0||!actor->IsSpellReady(spell->Id,proto))continue;
                for(uint8 effect=0;effect<3;effect++)
                {
                    float const amount=std::max(0,spell->CalculateSimpleValue(SpellEffectIndex(effect)));
                    uint8 kind=0;float score=0;uint32 school=0;
                    if(hurt&&spell->Effect[effect]==SPELL_EFFECT_HEAL)
                    {kind=3;score=std::min(amount,float(actor->GetMaxHealth()-actor->GetHealth()));}
                    else if(lowMana&&spell->Effect[effect]==SPELL_EFFECT_ENERGIZE&&spell->EffectMiscValue[effect]==POWER_MANA)
                    {kind=2;score=std::min(amount,float(actor->GetMaxPower(POWER_MANA)-actor->GetPower(POWER_MANA)));}
                    else if(spell->Effect[effect]==SPELL_EFFECT_APPLY_AURA&&spell->EffectApplyAuraName[effect]==SPELL_AURA_SCHOOL_ABSORB&&
                        (uint32(spell->EffectMiscValue[effect])&damageSchools)&&!actor->HasAura(spell->Id))
                    {kind=1;score=amount;school=uint32(spell->EffectMiscValue[effect])&damageSchools;}
                    // Urgent healing, then mana, precede preventive absorption on their shared cooldown.
                    if(score>0&&(kind>bestKind||(kind==bestKind&&score>bestScore)))
                    {best=item;bestSpell=i;bestKind=kind;bestScore=score;bestSchool=school;}
                }
            }
        };
        for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;slot++)inspect(actor->GetItemByPos(INVENTORY_SLOT_BAG_0,slot));
        for(uint8 bagSlot=INVENTORY_SLOT_BAG_START;bagSlot<INVENTORY_SLOT_BAG_END;bagSlot++)
            if(Bag* bag=dynamic_cast<Bag*>(actor->GetItemByPos(INVENTORY_SLOT_BAG_0,bagSlot)))
                for(uint32 slot=0;slot<bag->GetBagSize();slot++)inspect(bag->GetItemByPos(slot));
        if(!best)return;
        WorldPackets::Spell::UseItem request;
        request.bagIndex=best->GetBagSlot();request.slot=best->GetSlot();request.spellSlot=bestSpell;
        auto useInfo=sSpellMgr.GetSpellEntry(best->GetProto()->Spells[bestSpell].SpellId);
        if(useInfo&&(useInfo->AllowedTargetMask&TARGET_FLAG_UNIT))request.targets.setUnitTarget(actor);
        uint32 const entry=best->GetEntry(),before=best->GetCount(),spell=best->GetProto()->Spells[bestSpell].SpellId;ObjectGuid const guid=best->GetObjectGuid();
        // Normal ownership, cooldown, form, trade, targeting, effects and consumption checks apply.
        actor->GetSession()->HandleUseItemOpcode(request);
        Item* after=actor->GetItemByPos(request.bagIndex,request.slot);
        uint32 const count=after&&after->GetObjectGuid()==guid?after->GetCount():0;
        if(count<before)sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"[SUI][raid-potion] actor=%u item=%u before=%u after=%u spell=%u purpose=%s schoolMask=%u",
            actor->GetGUIDLow(),entry,before,count,spell,bestKind==3?"healing":bestKind==2?"mana":"school-absorb",bestSchool);
    }
    // A learned dispel whose implicit target is the enemy (an enrage/magic removal) against a removable
    // positive aura on the fought unit. The spell comes from the actor's own book and the aura's dispel
    // type from live spell data; nothing names a boss or a buff.
    uint32 EnemyAuraRemoval(Player* actor,Unit* enemy)
    {
        if(!enemy||!enemy->IsAlive())return 0;
        for(auto const& known:actor->GetSpellMap())
        {
            if(known.second.state==PLAYERSPELL_REMOVED||known.second.disabled)continue;
            SpellEntry const* spell=sSpellMgr.GetSpellEntry(known.first);
            if(!spell||spell->IsPassiveSpell()||!actor->IsSpellReady(known.first))continue;
            uint32 mask=0;bool enemyTarget=false;
            for(uint8 i=0;i<MAX_EFFECT_INDEX;++i)if(spell->Effect[i]==SPELL_EFFECT_DISPEL)
            {mask|=Spells::GetDispellMask(DispelType(spell->EffectMiscValue[i]));enemyTarget|=spell->EffectImplicitTargetA[i]==TARGET_UNIT_ENEMY;}
            if(!mask||!enemyTarget)continue;
            bool removable=false;
            for(auto const& pair:enemy->GetSpellAuraHolderMap())
            {
                auto holder=pair.second;auto aura=holder->GetSpellProto();
                if(!holder->IsPositive()||aura->Dispel>=32||!(mask&(1u<<aura->Dispel)))continue;
                removable=true;break;
            }
            if(!removable)continue;
            if(!actor->IsWithinLOSInMap(enemy)||!actor->IsWithinDist(enemy,Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex)),true))continue;
            return known.first;
        }
        return 0;
    }
    bool Support(AiBotAI* ai,Plan& plan,Assignment& row,Unit* enemy,bool tank)
    {
        Player* actor=ai->GetBotPlayer();
        if(uint32 removal=EnemyAuraRemoval(actor,enemy))if(SupportCast(ai,plan,row,enemy,removal,15))return true;
        if(actor->GetHealthPercent()<35&&SupportCast(ai,plan,row,actor,row.defensive,4))return true;
        if(enemy&&enemy->IsNonMeleeSpellCasted(false,false,true)&&SupportCast(ai,plan,row,enemy,row.interrupt,13))return true;
        if(tank&&enemy&&enemy->GetVictim()!=actor&&SupportCast(ai,plan,row,enemy,row.taunt,2))return true;
        if(row.dispel)
        {
            auto spell=sSpellMgr.GetSpellEntry(row.dispel);
            // Tanks first (the boss's debuff on the tank is the raid's problem), healers next, everyone else after.
            if(spell)for(int tier=0;tier<3;++tier)for(auto const& candidate:plan.rows)
            {
                int const rank=candidate.role<=2?0:candidate.role==3?1:2;
                if(rank!=tier)continue;
                Player* patient=Member(plan,candidate.guid);
                if(patient&&patient->IsAlive()&&ai->IsValidDispelTarget(patient,spell)&&SupportCast(ai,plan,row,patient,row.dispel,14))return true;
            }
        }
        return false;
    }
    bool BossNearActive(Plan const& plan,Rule const& rule,Player* actor)
    {
        if(rule.trigger!=Trigger::BossNear)return false;
        Unit* boss=actor->GetMap()->GetUnit(plan.boss);
        if(!boss||!boss->IsAlive()||!boss->IsInCombat())return false;
        float dx=boss->GetPositionX()-rule.station.x,dy=boss->GetPositionY()-rule.station.y,dz=boss->GetPositionZ()-rule.station.z;
        // Observable position only; do not inspect script RNG or future state.
        return dx*dx+dy*dy+dz*dz<=rule.triggerRadius*rule.triggerRadius;
    }
    bool TimedPositionActive(Plan const& plan,Assignment const& row)
    {
        for(auto const& event:plan.events)
        {
            Rule const& rule=plan.definition.rules[event.rule];
            if((rule.action==Action::AvoidPoints||rule.action==Action::Spread||rule.action==Action::Isolate)&&
                (rule.roles&(1u<<row.role))&&(!rule.phase||rule.phase==plan.phase)&&
                (!rule.toggle||(plan.flags&rule.toggle)))return true;
        }
        for(auto const& rule:plan.definition.rules)
            if((rule.roles&(1u<<row.role))&&(!rule.phase||rule.phase==plan.phase)&&
                (!rule.toggle||(plan.flags&rule.toggle)))
                if(Player* actor=Member(plan,row.guid))if(BossNearActive(plan,rule,actor))return true;
        for(auto const& rule:plan.definition.rules)
            if(rule.trigger==Trigger::MemberAura&&(rule.roles&(1u<<row.role))&&
                (!rule.phase||rule.phase==plan.phase)&&(!rule.toggle||(plan.flags&rule.toggle)))
                for(auto const& member:plan.rows)if(Player* p=Member(plan,member.guid))
                    if(p->IsAlive())for(uint32 spell:rule.spells)if(p->HasAura(spell))return true;
        return false;
    }
    void StopMechanicMovement(Player* actor,Assignment& row)
    {
        // Reaching safety ends movement, not healing, support, or stationary damage.
        if(actor->IsMoving()||row.hasMove){actor->StopMoving();actor->GetMotionMaster()->MoveIdle();}
        row.hasMove=false;
    }
    bool ConeStationAllowed(Plan const&,Assignment const&,Player*,Point);
    float FormationSpacing(Plan const&,Assignment const&);
    bool SafeStationMove(AiBotAI*,Plan const&,Assignment&,Point);
    float InstantBurstSpellRadius(SpellEntry const* spell,float* damage=nullptr);
    // A patient standing inside a stand-off region (a tank on a boss with an instant caster-centred burst) must be
    // healed from outside it: the healers' working range floor is that radius plus a small margin. Loaded spell list
    // and template slots of the attackers (the observed-burst memory needs the plan and is not needed here).
    float StandoffFloor(Player* patient)
    {
        float floor=0;
        if(!patient)return 0;
        for(Unit* attacker:patient->GetAttackers())
        {
            if(!attacker||!attacker->IsAlive()||attacker->GetTypeId()!=TYPEID_UNIT)continue;
            CreatureInfo const* info=static_cast<Creature const*>(attacker)->GetCreatureInfo();
            if(CreatureSpellsList const* list=sObjectMgr.GetCreatureSpellsList(info->spell_list_id))
                for(auto const& entry:*list)floor=std::max(floor,InstantBurstSpellRadius(sSpellMgr.GetSpellEntry(entry.spellId)));
            for(uint32 spell:info->spells)if(spell)floor=std::max(floor,InstantBurstSpellRadius(sSpellMgr.GetSpellEntry(spell)));
        }
        return floor>0?floor+4.f:0.f;
    }
    float HealingWorkingRange(Assignment const& row,float floorRange=0.f)
    {
        auto spell=sSpellMgr.GetSpellEntry(row.heal);
        float maximum=spell?Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex)):0.f;
        // Leave movement slack instead of camping at the absolute spell limit.
        // This conservative base range does not assume range-increasing talents.
        float base=std::max(0.f,std::min(maximum-2.f,maximum*.8f));
        // ...unless the patient stands inside a stand-off region: then the slack gives way to the spell limit -2.
        return std::max(base,std::min(maximum-2.f,floorRange));
    }
    bool HealingPositionAllowed(Assignment const& row,Player* patient,Point point,float arrivalMargin=0.f)
    {
        if(!patient||!patient->IsAlive())return false;
        float range=HealingWorkingRange(row,StandoffFloor(patient))-arrivalMargin;
        float dx=point.x-patient->GetPositionX(),dy=point.y-patient->GetPositionY(),dz=point.z-patient->GetPositionZ();
        return range>0&&dx*dx+dy*dy+dz*dz<=range*range&&patient->IsWithinLOS(point.x,point.y,point.z);
    }
    bool HealerStationAllowed(Plan const& plan,Assignment const& row,Point point)
    {return row.role!=3||HealingPositionAllowed(row,Member(plan,row.primary),point);}
    bool MeleeStationRequired(Plan const& plan,Assignment const& row)
    {
        if(row.role!=4)return false;
        for(auto const& phase:plan.definition.phases)if(phase.id==plan.phase)return phase.melee;
        return false;
    }
    void RememberInterruptedHeal(Player* actor,Assignment& row)
    {
        Spell* cast=actor->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if(row.role==3&&cast&&cast->getState()==SPELL_STATE_PREPARING&&
            cast->GetCastedTime()>0&&IsHeal(cast->m_spellInfo))
            row.healMovementInterrupted=true;
    }
    bool Move(AiBotAI* ai, Plan const& plan, Assignment& row, Point point, uint8 duty, bool hazardMove=false, float arrivalTolerance=1.5f)
    {
        Player* p = ai->GetBotPlayer();
        // Keep the proven safe location until timed hazards expire. Avoidance
        // movement (duties6/7) remains authoritative; ordinary roles may cast here.
        if(duty==1&&(MeleeStationRequired(plan,row)||TimedPositionActive(plan,row)||FormationSpacing(plan,row)>0))return SafeStationMove(ai,plan,row,point);
        // A safe actor may keep casting; ordinary station recovery must not send
        // it straight back into a currently unsafe boss-facing sector.
        if(duty==1&&(!ConeStationAllowed(plan,row,p,point)||!HealerStationAllowed(plan,row,point)))
        {
            if(row.hasMove){p->StopMoving();p->GetMotionMaster()->MoveIdle();row.hasMove=false;}
            return false;
        }
        // Project onto the current floor rather than imposing an authored flat Z.
        p->UpdateAllowedPositionZ(point.x, point.y, point.z);
        float dx = point.x - p->GetPositionX(), dy = point.y - p->GetPositionY();
        if (dx*dx + dy*dy <= arrivalTolerance*arrivalTolerance)
        {
            if (row.hasMove) { p->StopMoving();p->GetMotionMaster()->MoveIdle(); }
            row.hasMove = false; return false;
        }
        row.duty = duty;
        row.hazardMove = hazardMove; // Purpose is independent of the displayed movement duty.
        if (row.hasMove && p->IsMoving() && std::fabs(point.x-row.lastMove.x) < 1 && std::fabs(point.y-row.lastMove.y) < 1) return true;
        RememberInterruptedHeal(p,row);
        p->InterruptNonMeleeSpells(false);
        row.lastMove = point;
        row.hasMove = ai->CommanderRaidMove(point.x, point.y, point.z);
        if (!row.hasMove) row.duty = 12;
        return true;
    }
    Point TankStationOutward(Plan const& plan,Unit* boss,Point station)
    {
        float ox=station.x-(plan.definition.low.x+plan.definition.high.x)*.5f;
        float oy=station.y-(plan.definition.low.y+plan.definition.high.y)*.5f;
        float bx=station.x-boss->GetPositionX(),by=station.y-boss->GetPositionY();
        // Acquire the actual anchor after lateral displacement. Once the enemy
        // has passed it, preserve the outward side instead of crossing back.
        if(bx*ox+by*oy>0){ox=bx;oy=by;}
        return {ox,oy,0};
    }
    Point TankFacingStation(Plan const& plan,Player* actor,Unit* boss,Point station)
    {
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        Point axis=TankStationOutward(plan,boss,station);
        float ox=axis.x,oy=axis.y;
        float length=std::sqrt(ox*ox+oy*oy);if(length<.1f)return at;ox/=length;oy/=length;
        float dx=at.x-boss->GetPositionX(),dy=at.y-boss->GetPositionY();
        float distance=std::sqrt(dx*dx+dy*dy);
        // The caller verified retained threat and either contact or outward
        // facing. Backing toward the station may cross our attack radius.
        if(distance>=1.5f&&(dx*ox+dy*oy)/distance>=.98480775f)
        {
            float sx=station.x-at.x,sy=station.y-at.y,remaining=std::hypot(sx,sy);
            if(remaining<=2.f||sx*dx+sy*dy<=0)return at;
            float step=std::min(2.f,remaining-2.f)/remaining;
            return {at.x+sx*step,at.y+sy*step,at.z};
        }
        // Cross directly when acquiring the opposite side; reserve tangential
        // correction for an established tank within 120 degrees of the intended direction.
        if(distance>.01f&&(dx*ox+dy*oy)/distance<-.5f)
            return {boss->GetPositionX()+ox*2.f,boss->GetPositionY()+oy*2.f,at.z};
        // Turn tangentially at the actual separation. Do not pull a tank
        // inward, or force a close tank outward to a fraction of maximum reach.
        float contact=std::max(1.f,actor->GetCombatReachToTarget(boss,false,0,true)-.75f);
        float rx=distance>.01f?dx/distance:ox,ry=distance>.01f?dy/distance:oy;
        float angle=std::atan2(rx*oy-ry*ox,rx*ox+ry*oy);
        angle=std::max(-.34906585f,std::min(.34906585f,angle));
        float c=std::cos(angle),s=std::sin(angle);
        float radius=std::min(contact,std::max(std::max(1.5f,distance),distance/c));
        return {boss->GetPositionX()+(rx*c-ry*s)*radius,boss->GetPositionY()+(rx*s+ry*c)*radius,at.z};
    }
    // Voluntary tank positioning must not outrun every living healer. Use the
    // same conservative spell range as healer movement, including real LOS.
    // Contact acquisition and urgent hazard routes remain owned by their callers.
    bool TankStepHasHealingCoverage(Plan const& plan,Player* actor,Point next)
    {
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        bool any=false;float current=1e30f,proposed=1e30f;
        for(auto const& healer:plan.rows)
        {
            if(healer.role!=3||healer.yielded)continue;
            Player* body=Member(plan,healer.guid);
            float range=HealingWorkingRange(healer);
            if(!body||!body->IsAlive()||range<=0)continue;
            any=true;
            auto gap=[&](Point point)
            {
                float dx=point.x-body->GetPositionX(),dy=point.y-body->GetPositionY(),dz=point.z-body->GetPositionZ();
                return std::sqrt(dx*dx+dy*dy+dz*dz)/range;
            };
            float before=gap(at),after=gap(next);
            if(after<=1.f&&body->IsWithinLOS(next.x,next.y,next.z))return true;
            current=std::min(current,before);proposed=std::min(proposed,after);
        }
        // An uncovered tank can move back toward support. With no surviving
        // healer, do not deadlock normal tank combat on impossible coverage.
        return !any||proposed+.001f<current;
    }
    Point CoveredTankStep(Plan const& plan,Player* actor,Point goal)
    {
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        float dx=goal.x-at.x,dy=goal.y-at.y,dz=goal.z-at.z;
        float length=std::sqrt(dx*dx+dy*dy+dz*dz);
        if(length>2.f){float scale=2.f/length;goal={at.x+dx*scale,at.y+dy*scale,at.z+dz*scale};}
        return TankStepHasHealingCoverage(plan,actor,goal)?goal:at;
    }
    bool FlankStation(Plan const& plan,Assignment& row,Player* actor,Unit* boss,Point& station)
    {
        float const facing=boss->GetOrientation();
        float const dx=boss->GetPositionX()-row.flankOrigin.x,dy=boss->GetPositionY()-row.flankOrigin.y;
        if(row.flankReady&&row.duty!=12&&dx*dx+dy*dy<.5625f&&std::cos(facing-row.flankFacing)>.9961947f)
        {station=row.flank;return true;}
        row.flankReady=false;
        float const angle=facing+(row.team%2==0?1.5707963f:-1.5707963f);
        float const reach=std::max(3.f,boss->GetCombatReach()+actor->GetCombatReach()-1.f);
        // Keep the flank angle but shorten the radius when a wall blocks the outer station.
        // Seven candidates, floor/range/LOS validation and complete navigation only.
        for(int candidate=0;candidate<7;candidate++)
        {
            float const radius=reach-(reach-3.f)*candidate/6.f;
            // This destination is around the target, potentially uphill from the actor.
            // A downward floor query from the actor's height can start below that floor.
            Point point{boss->GetPositionX()+std::cos(angle)*radius,boss->GetPositionY()+std::sin(angle)*radius,boss->GetPositionZ()};
            actor->UpdateAllowedPositionZ(point.x,point.y,point.z);
            if(!InRoom(plan,point)||!actor->CanReachWithMeleeAutoAttackAtPosition(boss,point.x,point.y,point.z)||
                !boss->IsWithinLOS(point.x,point.y,point.z))continue;
            PathInfo path(actor);path.calculate(point.x,point.y,point.z);
            if(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE)||path.getPath().empty())continue;
            auto const& end=path.getPath().back();
            float const ex=end.x-point.x,ey=end.y-point.y,ez=end.z-point.z;
            if(ex*ex+ey*ey+ez*ez>2.25f)continue;
            row.flank=point;row.flankOrigin={boss->GetPositionX(),boss->GetPositionY(),boss->GetPositionZ()};
            row.flankFacing=facing;row.flankReady=true;station=point;return true;
        }
        return false;
    }
    std::vector<Point> HazardPoints(Plan const& plan,Rule const& rule)
    {
        std::vector<Point> points;
        for(uint32 id:rule.points) if(auto p=sSpellMgr.GetSpellTargetPosition(id))
            if(p->mapId==plan.definition.map) points.push_back({p->x,p->y,p->z});
        return points;
    }
    float Clearance(Point p,std::vector<Point> const& lane)
    {
        float best=100000;
        for(Point q:lane) {float x=p.x-q.x,y=p.y-q.y;best=std::min(best,x*x+y*y);}
        return std::sqrt(best);
    }
    Point FirstRouteTurn(Point at,PointsArray const& path)
    {
        Point waypoint=at;float ux=0,uy=0,progress=0;bool direction=false;
        // Smooth navigation emits short points even along a straight segment.
        // Deliver that whole segment, stopping at its first horizontal turn.
        // Never replace a bent certified route with a chord to its destination.
        for(auto const& node:path)
        {
            float dx=node.x-at.x,dy=node.y-at.y,length=std::sqrt(dx*dx+dy*dy);
            if(length<.01f)continue;
            if(!direction){ux=dx/length;uy=dy/length;direction=true;}
            float along=dx*ux+dy*uy,cross=dx*uy-dy*ux;
            if(std::fabs(cross)>.001f||along<progress-.001f)break;
            waypoint={node.x,node.y,node.z};progress=along;
        }
        return waypoint;
    }
    bool FindClearRoute(Plan const& plan,Player* actor,std::function<bool(Point)> const& isClear,
        Point& goal,Point& waypoint,float& travelSeconds,std::function<bool(Point)> const& canTraverse = {},char const* reason="route",
        std::function<float(Point)> const& destinationCost = {},
        std::function<bool(Point,float,PointsArray const&)> const& acceptRoute = {},float floorReference=NAN)
    {
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        float best=100000,bestTravel=0;bool found=false;
        auto cost=[&](Point p){return destinationCost?std::max(0.f,destinationCost(p)):0.f;};
        if(InRoom(plan,at)&&isClear(at)&&(!acceptRoute||acceptRoute(at,0,PointsArray{}))){best=cost(at);goal=waypoint=at;found=true;}
        unsigned floorReject=0,pathReject=0,roomReject=0,traverseReject=0,reenterReject=0;
        // Bounded candidate count, independent of the authored room's size.
        auto const& d=plan.definition;
        std::vector<std::pair<float,Point>> candidates;
        for(int ix=1;ix<20;ix++)for(int iy=1;iy<20;iy++)
        {
            Point q{d.low.x+(d.high.x-d.low.x)*ix/20,d.low.y+(d.high.y-d.low.y)*iy/20,std::isfinite(floorReference)?floorReference:at.z};
            if(!isClear(q))continue;
            float dx=q.x-at.x,dy=q.y-at.y;candidates.push_back({std::sqrt(dx*dx+dy*dy)+cost(q),q});
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](auto const& a,auto const& b){return a.first<b.first;});
        for(auto const& candidate:candidates)
        {
            if(candidate.first>=best)break;
            Point q=candidate.second;
            actor->UpdateAllowedPositionZ(q.x,q.y,q.z);if(!InRoom(plan,q)||!isClear(q)){++floorReject;continue;}
            PathInfo path(actor);path.calculate(q.x,q.y,q.z);
            if(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE)||path.getPath().empty()){++pathReject;continue;}
            if(acceptRoute&&(path.getPathType()&(PATHFIND_SHORTCUT|PATHFIND_NOT_USING_PATH|PATHFIND_DEST_FORCED))){++pathReject;continue;}
            auto const& end=path.getPath().back();
            float const ex=end.x-q.x,ey=end.y-q.y,ez=end.z-q.z;
            if(ex*ex+ey*ey+ez*ez>2.25f)continue;
            // Check the entire sampled route. Once clear of the footprint, never re-enter it.
            Point previous=at;bool escaped=isClear(at),safe=true,enteredRoom=InRoom(plan,at);float travel=0;
            for(auto const& node:path.getPath())
            {
                Point next{node.x,node.y,node.z};float sx=next.x-previous.x,sy=next.y-previous.y,sz=next.z-previous.z;
                float length=std::sqrt(sx*sx+sy*sy+sz*sz);travel+=length;
                int samples=std::max(1,int(std::ceil(length)));if(samples>1000){safe=false;break;}
                for(int sample=1;sample<=samples;sample++)
                {
                    float f=float(sample)/samples;Point step{previous.x+sx*f,previous.y+sy*f,previous.z+sz*f};
                    bool const inside=InRoom(plan,step);
                    if(enteredRoom&&!inside){++roomReject;safe=false;break;}
                    enteredRoom|=inside; // An actor displaced outside may return by an ordinary complete path.
                    if(canTraverse&&!canTraverse(step)){++traverseReject;safe=false;break;}
                    bool clear=isClear(step);
                    if(escaped&&!clear){++reenterReject;safe=false;break;}escaped|=clear;
                }
                if(!safe)break;previous=next;
            }
            float const score=travel+cost(q);
            if(!safe||!escaped||!enteredRoom||score>=best)continue;
            if(acceptRoute&&!acceptRoute(q,travel,path.getPath()))continue;
            best=score;bestTravel=travel;goal=q;waypoint=q;found=true;
            waypoint=FirstRouteTurn(at,path.getPath());
        }
        if(!found)
        {
            static std::map<uint64,std::chrono::steady_clock::time_point> last;
            auto now=std::chrono::steady_clock::now();auto& previous=last[Raw(actor->GetObjectGuid())];
            if(now-previous>=std::chrono::seconds(5))
            {
                previous=now;
                sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI][raid-route] actor=%llu kind=%s pos=%.2f,%.2f,%.2f candidates=%u floor=%u path=%u bounds=%u traversal=%u reenter=%u",
                    static_cast<unsigned long long>(Raw(actor->GetObjectGuid())),reason,at.x,at.y,at.z,
                    unsigned(candidates.size()),floorReject,pathReject,roomReject,traverseReject,reenterReject);
            }
        }
        travelSeconds=found?bestTravel/std::max(.1f,actor->GetSpeed(MOVE_RUN)):0;
        return found;
    }
    bool EscapeRoute(Plan const& plan,Player* actor,std::vector<Point> const& lane,float radius,
        Point& goal,Point& waypoint,float& travelSeconds)
    {
        return FindClearRoute(plan,actor,[&](Point p){return Clearance(p,lane)>radius+1;},goal,waypoint,travelSeconds);
    }
    bool ConeStationAllowed(Plan const& plan,Assignment const& row,Player* actor,Point p)
    {
        if(plan.state!=2||plan.boss.IsEmpty())return true;
        Unit* boss=actor->GetMap()->GetUnit(plan.boss);
        // A boss faces its current victim. Circling that victim just rotates the
        // hazard across everybody else; holding permits a tank to acquire it.
        if(!boss||boss->GetVictim()==actor)return true;
        for(auto const& rule:plan.definition.rules)
        {
            if(rule.action!=Action::AvoidCones||!(rule.roles&(1u<<row.role))||
                (rule.phase&&rule.phase!=plan.phase)||(rule.toggle&&!(plan.flags&rule.toggle)))continue;
            for(uint32 id:rule.spells)
            {
                auto spell=sSpellMgr.GetSpellEntry(id);if(!spell)return false;
                float radius=0;
                // Core fills implicit cone targets 24 and 54 through the same PUSH_IN_CONE
                // notifier (Spell.cpp), whose arc is sSpellMgr.GetSpellCone(id) for every spell
                // outside Spell.cpp's own per-id list under target 54; both are the same cone here.
                for(size_t i=0;i<3;i++)if(spell->Effect[i]&&
                    (spell->EffectImplicitTargetA[i]==24||spell->EffectImplicitTargetB[i]==24||spell->EffectImplicitTargetA[i]==54||spell->EffectImplicitTargetB[i]==54))
                    radius=std::max(radius,Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i])));
                float const angle=sSpellMgr.GetSpellCone(id);
                float const dx=p.x-boss->GetPositionX(),dy=p.y-boss->GetPositionY();
                float bearings[2]={boss->GetOrientation(),boss->GetOrientation()};
                // Spell::cast turns an NPC toward its original explicit target
                // immediately before FillTargetMap, even after a threat swap.
                // Keep both the current and that impending cone clear.
                if(Spell* casting=boss->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                    if(casting->m_spellInfo->Id==id)
                        if(Unit* castTarget=casting->m_targets.getUnitTarget())
                            if(castTarget!=boss)bearings[1]=boss->GetAngle(castTarget);
                float const half=std::min(3.14159265f,std::fabs(angle)*.5f+.08726646f);
                for(float bearing:bearings)
                {
                    float const facing=bearing+(angle<0?3.14159265f:0);
                    float const alignment=std::cos(std::atan2(dy,dx)-facing);
                    if(alignment<std::cos(half))continue;
                    float const distance=std::hypot(dx,dy);
                    float const insideAngle=std::max(0.f,half-std::acos(std::max(-1.f,std::min(1.f,alignment))));
                    // A distant point in front is not a stable station if the
                    // caster can close its range before this actor can leave
                    // the cone. Reserve ordinary advice age AND minimum travel
                    // to its nearest angular edge, using native movement speeds.
                    // This is only planning clearance; spell hit tests are unchanged.
                    float const escapeDistance=distance*std::sin(std::min(insideAngle,1.57079633f));
                    float const actorSpeed=actor->GetSpeed(MOVE_RUN);
                    float const escapeSeconds=actorSpeed>.01f?escapeDistance/actorSpeed:1000.f;
                    float const margin=radius+rule.radius+boss->GetSpeed(MOVE_RUN)*(2.5f+escapeSeconds);
                    if(distance<=margin)return false;
                }
            }
        }
        return true;
    }
    // Timing is read from the effective loaded spell, not an encounter-ID table.
    float ForecastWarningSeconds(Plan const& plan,Rule const& forecast,Player* actor)
    {
        Unit* boss=actor->GetMap()->GetUnit(plan.boss);if(!boss)return 0;
        float warning=std::numeric_limits<float>::max();bool found=false;
        for(auto const& rule:plan.definition.rules)
        {
            if(rule.trigger!=Trigger::CastStart||rule.action!=Action::AvoidPoints||rule.radius<forecast.radius||
                rule.points.size()!=forecast.points.size()||!std::is_permutation(rule.points.begin(),rule.points.end(),forecast.points.begin())||
                (rule.phase&&rule.phase!=plan.phase)||(rule.toggle&&!(plan.flags&rule.toggle)))continue;
            for(uint32 id:rule.spells)
            {
                auto spell=sSpellMgr.GetSpellEntry(id);if(!spell)return 0;
                warning=std::min(warning,spell->GetCastTime(boss)/1000.f);found=true;
            }
        }
        return found?warning:0;
    }
    float SpellMovementLockSeconds(uint32 id,std::set<uint32>& visited,unsigned depth=0)
    {
        if(visited.count(id))return 1000;
        if(depth>=8)return 1000; // Unresolved trigger depth cannot grant escape time.
        visited.insert(id);auto spell=sSpellMgr.GetSpellEntry(id);if(!spell)return 1000;
        float lock=0,children=0;
        for(unsigned i=0;i<3;++i)
        {
            if(!spell->Effect[i])continue;
            if(spell->Effect[i]==SPELL_EFFECT_SCRIPT_EFFECT)lock=1000;
            if(Spells::IsEffectAppliesAura(spell->Effect[i]))
                switch(spell->EffectApplyAuraName[i])
                {
                    case SPELL_AURA_MOD_CONFUSE:case SPELL_AURA_MOD_FEAR:
                    case SPELL_AURA_MOD_STUN:case SPELL_AURA_MOD_ROOT:
                        lock=std::max(lock,spell->GetDuration()<=0?1000.f:spell->GetDuration()/1000.f);break;
                    default:break;
                }
            if(spell->EffectTriggerSpell[i])children+=SpellMovementLockSeconds(spell->EffectTriggerSpell[i],visited,depth+1);
        }
        return std::min(1000.f,lock+children);
    }
    float ObservedMovementLockSeconds(Plan const& plan,Player* actor)
    {
        float lock=0;
        auto inspect=[&](uint32 spell){std::set<uint32> visited;lock=std::max(lock,SpellMovementLockSeconds(spell,visited));};
        for(auto const& event:plan.events)if(event.target==actor->GetObjectGuid())
            for(uint32 spell:plan.definition.rules[event.rule].spells)inspect(spell);
        for(auto const& rule:plan.definition.rules)if(rule.trigger==Trigger::MemberAura)
            for(uint32 spell:rule.spells)if(actor->HasAura(spell))inspect(spell);
        return lock;
    }
    struct PositionHazard { Point point; float radius; size_t group; int priority; bool forecast=false; float warning=0; };
    // Hostile trap objects (spawned area-damage objects) are exclusion regions while they exist.
    // The region comes from the live object and its template/spell data only: no object, spell or
    // encounter identity is named. Friendly traps and objects whose spell does no damage are ignored.
    static constexpr float kObjectHazardRange=80.f;
    static constexpr int kObjectHazardPriority=90;
    struct TrapObjectsInRange
    {
        WorldObject const* origin;float range;
        bool operator()(GameObject* go)const
        {
            GameObjectInfo const* info=go->GetGOInfo();
            return info&&info->type==GAMEOBJECT_TYPE_TRAP&&go->isSpawned()&&origin->IsWithinDist(go,range,false);
        }
    };
    struct AreaAurasInRange
    {
        WorldObject const* origin;float range;
        bool operator()(DynamicObject* area)const
        {
            return area->GetType()==DYNAMIC_OBJECT_AREA_SPELL&&area->IsInWorld()&&origin->IsWithinDist(area,range,false);
        }
        bool operator()(WorldObject*)const {return false;}
    };
    bool HarmfulAreaSpell(SpellEntry const* spell,float& radius)
    {
        bool harmful=false;radius=0;
        for(uint8 i=0;i<MAX_EFFECT_INDEX;++i)
        {
            // A persistent area aura (rain of fire, a burning ground) carries its periodic damage on the
            // persistent-area effect itself, not on an apply-aura effect: both effect kinds are harmful.
            bool const aura=spell->Effect[i]==SPELL_EFFECT_APPLY_AURA||spell->Effect[i]==SPELL_EFFECT_PERSISTENT_AREA_AURA;
            bool damage=spell->Effect[i]==SPELL_EFFECT_SCHOOL_DAMAGE||
                (aura&&(spell->EffectApplyAuraName[i]==SPELL_AURA_PERIODIC_DAMAGE||spell->EffectApplyAuraName[i]==SPELL_AURA_PERIODIC_DAMAGE_PERCENT));
            if(!damage)continue;
            harmful=true;
            if(auto entry=sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))radius=std::max(radius,Spells::GetSpellRadius(entry));
        }
        return harmful;
    }
    // Idle hostile creatures (2026-09-13): a creature that is alive, hostile, able to attack and not in combat is
    // an exclusion region of its own aggro reach while it stands there - the group the raid has not pulled. It
    // applies to bot rows once the fight has begun; the encounter's objective itself and anything already fighting
    // are ordinary targets. Live world only: no entry, name or coordinate.
    static constexpr float kIdleHostileRange=60.f;
    static constexpr float kIdleHostileMargin=3.f;
    static constexpr int kIdleHostilePriority=85;
    struct IdleHostilesInRange
    {
        WorldObject const* origin;float range;
        bool operator()(Creature* creature)const
        {
            return creature->IsInWorld()&&creature->IsAlive()&&!creature->IsInCombat()&&!creature->IsPet()&&
                !creature->IsCivilian()&&creature->CanInitiateAttack()&&origin->IsWithinDist(creature,range,false);
        }
    };
    void IdleHostileHazards(Plan const& plan,Assignment const& row,Player* actor,std::vector<PositionHazard>& hazards,bool& active)
    {
        if(!plan.enteredCombat)return; // the manual tank's escapes and stations avoid unpulled groups as well; its pre-combat pull walk is not guided here
        std::list<Creature*> creatures;
        CellPair pair(MaNGOS::ComputeCellPair(actor->GetPositionX(),actor->GetPositionY()));Cell cell(pair);cell.SetNoCreate();
        IdleHostilesInRange check{actor,kIdleHostileRange};
        MaNGOS::CreatureListSearcher<IdleHostilesInRange> searcher(creatures,check);
        TypeContainerVisitor<MaNGOS::CreatureListSearcher<IdleHostilesInRange>,GridTypeMapContainer> visitor(searcher);
        cell.Visit(pair,visitor,*actor->GetMap(),*actor,kIdleHostileRange);
        Point const at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        for(Creature* creature:creatures)
        {
            if(creature->GetObjectGuid()==plan.boss||!creature->IsHostileTo(actor))continue;
            float const reach=creature->GetAttackDistance(actor)+kIdleHostileMargin;
            auto region=[&](Point p)
            {
                hazards.push_back({p,reach,plan.definition.rules.size()+hazards.size(),kIdleHostilePriority});
                if(std::hypot(p.x-at.x,p.y-at.y)<reach)active=true; // passive unless the actor already stands inside
            };
            region({creature->GetPositionX(),creature->GetPositionY(),creature->GetPositionZ()});
            // A waypoint patroller will be everywhere on its loaded route: the route is the region.
            if(creature->GetDefaultMovementType()==WAYPOINT_MOTION_TYPE)
                if(WaypointPath const* path=sWaypointMgr.GetDefaultPath(creature->GetEntry(),creature->GetGUIDLow()))
                    for(auto const& node:*path)region({node.second.x,node.second.y,node.second.z});
        }
    }
    // Unit-carried periodic aura hazards (2026-09-13): a hostile creature under a SELF-cast periodic-trigger aura is
    // a moving exclusion region while the aura lasts, when its pulses do area damage around the carrier. The radius
    // is the aura's loaded periodic child when that child damages an area directly, otherwise the area-damage pulses
    // this plan has already observed the carrier cast on itself under the aura (a scripted pulse behind a dummy child).
    static constexpr int kUnitAuraPriority=95;
    bool SelfPeriodicAura(SpellEntry const* spell,uint32& child)
    {
        if(!spell)return false;
        for(uint8 i=0;i<MAX_EFFECT_INDEX;++i)
            if(spell->Effect[i]==SPELL_EFFECT_APPLY_AURA&&spell->EffectApplyAuraName[i]==SPELL_AURA_PERIODIC_TRIGGER_SPELL&&spell->EffectImplicitTargetA[i]==TARGET_UNIT_CASTER)
            {child=spell->EffectTriggerSpell[i];return true;}
        return false;
    }
    float SelfPeriodicAuraFootprint(Plan const& plan,Unit const* unit,SpellAuraHolder const* holder)
    {
        SpellEntry const* spell=holder->GetSpellProto();uint32 child=0;
        if(!spell||holder->GetCasterGuid()!=unit->GetObjectGuid()||!SelfPeriodicAura(spell,child))return 0;
        float radius=0;
        if(SpellEntry const* pulse=sSpellMgr.GetSpellEntry(child))if(!HarmfulAreaSpell(pulse,radius))radius=0;
        auto learned=plan.auraFootprints.find(spell->Id);
        if(learned!=plan.auraFootprints.end())radius=std::max(radius,learned->second);
        return radius;
    }
    struct LivingCreaturesInRange
    {
        WorldObject const* origin;float range;
        bool operator()(Creature* creature)const{return creature->IsInWorld()&&creature->IsAlive()&&origin->IsWithinDist(creature,range,false);}
    };
    void UnitAuraHazards(Plan const& plan,Player* actor,std::vector<PositionHazard>& hazards,bool& active)
    {
        std::list<Creature*> creatures;
        CellPair pair(MaNGOS::ComputeCellPair(actor->GetPositionX(),actor->GetPositionY()));Cell cell(pair);cell.SetNoCreate();
        LivingCreaturesInRange check{actor,kObjectHazardRange};
        MaNGOS::CreatureListSearcher<LivingCreaturesInRange> searcher(creatures,check);
        TypeContainerVisitor<MaNGOS::CreatureListSearcher<LivingCreaturesInRange>,GridTypeMapContainer> visitor(searcher);
        cell.Visit(pair,visitor,*actor->GetMap(),*actor,kObjectHazardRange);
        for(Creature* creature:creatures)
        {
            if(!creature->IsHostileTo(actor))continue;
            for(auto const& held:creature->GetSpellAuraHolderMap())
            {
                float const radius=held.second?SelfPeriodicAuraFootprint(plan,creature,held.second):0.f;
                if(radius<=0)continue;
                hazards.push_back({{creature->GetPositionX(),creature->GetPositionY(),creature->GetPositionZ()},radius+1.f,plan.definition.rules.size()+hazards.size(),kUnitAuraPriority});
                active=true;
            }
        }
    }
    // Stand-off (2026-09-13): the raiding rule "ranged and healers stay out of the boss's area effect". A caster-centred
    // area damage burst the unit prepares in at most kStandoffCastMs cannot be left by anyone already inside it, so the
    // healer and ranged rows keep its radius from every living, fighting hostile that carries one in its loaded spell
    // list; melee and tanks accept it under healing. Live world only: object-manager spell list, loaded effects and radii.
    static constexpr uint32 kStandoffCastMs=1500;
    static constexpr int kStandoffPriority=88;
    // The radius of a caster-centred SCHOOL_DAMAGE burst prepared in at most kStandoffCastMs; 0 for any other spell.
    // `damage` receives the burst's loaded base points (its bite before resistances and taken-damage modifiers).
    float InstantBurstSpellRadius(SpellEntry const* spell,float* damage)
    {
        if(!spell)return 0;
        SpellCastTimesEntry const* cast=sSpellCastTimesStore.LookupEntry(spell->CastingTimeIndex);
        if(cast&&cast->CastTime>int32(kStandoffCastMs))return 0;
        float radius=0;
        for(uint8 i=0;i<MAX_EFFECT_INDEX;++i)
            if(spell->Effect[i]==SPELL_EFFECT_SCHOOL_DAMAGE&&spell->EffectImplicitTargetA[i]==TARGET_LOCATION_CASTER_SRC&&spell->EffectImplicitTargetB[i]==TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC)
                if(auto e=sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                {
                    float const r=Spells::GetSpellRadius(e);
                    if(r>radius){radius=r;if(damage)*damage=float(spell->EffectBasePoints[i]+1);}
                }
        return radius;
    }
    // Loaded spell list, the template's own spell slots, and the bursts this plan has watched the entry cast.
    float InstantBurstRadius(Plan const& plan,Creature const* unit,float* damage=nullptr)
    {
        float radius=0,bite=0;CreatureInfo const* info=unit->GetCreatureInfo();
        auto consider=[&](SpellEntry const* spell){float d=0;float r=InstantBurstSpellRadius(spell,&d);if(r>0){radius=std::max(radius,r);bite=std::max(bite,d);}};
        if(CreatureSpellsList const* list=sObjectMgr.GetCreatureSpellsList(info->spell_list_id))
            for(auto const& entry:*list)consider(sSpellMgr.GetSpellEntry(entry.spellId));
        for(uint32 spell:info->spells)if(spell)consider(sSpellMgr.GetSpellEntry(spell));
        auto learned=plan.burstFootprints.find(unit->GetEntry());
        if(learned!=plan.burstFootprints.end())radius=std::max(radius,learned->second);
        if(damage)*damage=bite;
        return radius;
    }
    // A melee damage member sits a burst out when one hit takes this share of its own health: it cannot be healed
    // through a burst every few seconds on top of the tank. Tank roles are never held back. At 20 % the six rogues sat
    // out a 924-point burst (shazzrah-0913f-01): the kill slowed from 41 % to 14 % per 30 s and the blinks won; a
    // quarter-health hit is healable, a third is not.
    static constexpr float kMeleeStandoffFraction=.35f;
    void StandoffHazards(Plan const& plan,Assignment const& row,Player* actor,std::vector<PositionHazard>& hazards,bool& active)
    {
        if(row.role!=3&&row.role!=4&&row.role!=5)return;
        std::list<Creature*> creatures;
        CellPair pair(MaNGOS::ComputeCellPair(actor->GetPositionX(),actor->GetPositionY()));Cell cell(pair);cell.SetNoCreate();
        LivingCreaturesInRange check{actor,kObjectHazardRange};
        MaNGOS::CreatureListSearcher<LivingCreaturesInRange> searcher(creatures,check);
        TypeContainerVisitor<MaNGOS::CreatureListSearcher<LivingCreaturesInRange>,GridTypeMapContainer> visitor(searcher);
        cell.Visit(pair,visitor,*actor->GetMap(),*actor,kObjectHazardRange);
        Point const at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        Creature* const killTarget=KillTargetAdd(plan,actor); // the add the raid is finishing: do not flee its burst (v44)
        for(Creature* creature:creatures)
        {
            if(!creature->IsInCombat()||!creature->IsHostileTo(actor))continue;
            // Only its attackers stand in a loose kill target's footprint (v57): a healer does not damage it, so keeping out
            // of its burst costs the kill nothing. A released controlled add is held by nobody and walks onto its threat
            // leader on the ranged and healer ground (majordomo-0915j: its bursts were 27-30 % of the healers' intake).
            if(creature==killTarget&&row.role!=3) // finishing it stops its burst; fleeing a loose one only prolongs it (v44)
            {
                // ...but once a tank holds it the kill target stands still, and the ranged and healers keep its footprint
                // from their own range like any other burst carrier (v49).
                bool onTank=false;
                if(Unit* victim=creature->GetVictim())
                    for(auto const& other:plan.rows)if(other.role<=2&&other.guid==victim->GetObjectGuid()){onTank=true;break;}
                if(!onTank)continue;
            }
            float bite=0;float const radius=InstantBurstRadius(plan,creature,&bite);
            if(radius<=0)continue;
            if(row.role==4&&bite<kMeleeStandoffFraction*float(actor->GetMaxHealth()))continue; // a melee member can take this one
            // Its victim cannot outrun a burst its attacker carries along: running kites the unit through the raid
            // while the tanks chase it (2026-09-14, v38). The victim holds its ground for the pickup instead.
            if(creature->GetVictim()==actor)continue;
            Point const p{creature->GetPositionX(),creature->GetPositionY(),creature->GetPositionZ()};
            hazards.push_back({p,radius+1.f,plan.definition.rules.size()+hazards.size(),kStandoffPriority});
            if(std::hypot(p.x-at.x,p.y-at.y)<radius+1.f)active=true; // inside (a blink into the team): leave it
        }
    }
    void ObjectHazards(Plan const& plan,Player* actor,std::vector<PositionHazard>& hazards,bool& active)
    {
        std::list<GameObject*> objects;
        CellPair pair(MaNGOS::ComputeCellPair(actor->GetPositionX(),actor->GetPositionY()));Cell cell(pair);cell.SetNoCreate();
        TrapObjectsInRange check{actor,kObjectHazardRange};
        MaNGOS::GameObjectListSearcher<TrapObjectsInRange> searcher(objects,check);
        TypeContainerVisitor<MaNGOS::GameObjectListSearcher<TrapObjectsInRange>,GridTypeMapContainer> visitor(searcher);
        cell.Visit(pair,visitor,*actor->GetMap(),*actor,kObjectHazardRange);
        for(GameObject* go:objects)
        {
            GameObjectInfo const* info=go->GetGOInfo();
            // A trap with no proximity trigger and no re-use cooldown is a one-shot scripted burst: its spell went off when
            // the script used it on creation and the object is inert afterwards (v46). Ragnaros' lava bursts (35 yd spell,
            // radius 0, cooldown 0, three per wave) turned the whole chamber into standing exclusion regions and the raid
            // fled out of healing range for the whole fight (rag-manual-01: median member 52 yd from the objective).
            if(info->trap.radius==0&&info->trap.cooldown==0)continue;
            SpellEntry const* spell=sSpellMgr.GetSpellEntry(info->trap.spellId);
            float spellRadius=0;
            if(!spell||!HarmfulAreaSpell(spell,spellRadius))continue;
            Unit* owner=go->GetOwner();
            if(owner?!owner->IsHostileTo(actor):!go->IsHostileTo(actor))continue;
            float radius=std::max(float(info->trap.radius),spellRadius)+1.f;
            hazards.push_back({{go->GetPositionX(),go->GetPositionY(),go->GetPositionZ()},radius,plan.definition.rules.size()+hazards.size(),kObjectHazardPriority});
            active=true;
        }
        // Hostile persistent area auras (a dynamic object placed by a spell: rain of fire, a burning ground)
        // are exclusion regions while they exist, from the live object and its spell data only: the region is
        // the object's own radius and it disappears with the object. Friendly and harmless areas are ignored.
        std::list<WorldObject*> areas;
        AreaAurasInRange areaCheck{actor,kObjectHazardRange};
        MaNGOS::WorldObjectListSearcher<AreaAurasInRange> areaSearcher(areas,areaCheck);
        TypeContainerVisitor<MaNGOS::WorldObjectListSearcher<AreaAurasInRange>,GridTypeMapContainer> areaVisitor(areaSearcher);
        cell.Visit(pair,areaVisitor,*actor->GetMap(),*actor,kObjectHazardRange);
        for(WorldObject* object:areas)
        {
            DynamicObject* area=static_cast<DynamicObject*>(object);
            SpellEntry const* spell=sSpellMgr.GetSpellEntry(area->GetSpellId());
            float spellRadius=0;
            if(!spell||!HarmfulAreaSpell(spell,spellRadius))continue;
            if(!area->IsHostileTo(actor))continue;
            float radius=std::max(area->GetRadius(),spellRadius)+1.f;
            hazards.push_back({{area->GetPositionX(),area->GetPositionY(),area->GetPositionZ()},radius,plan.definition.rules.size()+hazards.size(),kObjectHazardPriority});
            active=true;
        }
    }
    // A tank-role member (the manual main tank included) that one of the encounter's units - the objective or a
    // listed add - is attacking right now: it is the unit's anchor and must not be moved by an isolate.
    bool HoldingFightUnit(Plan const& plan,Assignment const& row,Player* actor)
    {
        if(row.role>2)return false;
        for(Unit* attacker:actor->GetAttackers())
        {
            if(!attacker||!attacker->IsAlive()||attacker->GetTypeId()!=TYPEID_UNIT)continue;
            uint32 const entry=attacker->GetEntry();
            if(attacker->GetObjectGuid()==plan.boss)return true;
            if(std::find(plan.definition.objectives.begin(),plan.definition.objectives.end(),entry)!=plan.definition.objectives.end())return true;
            if(std::find(plan.definition.adds.begin(),plan.definition.adds.end(),entry)!=plan.definition.adds.end())return true;
        }
        return false;
    }
    std::vector<PositionHazard> PositionHazards(Plan const& plan,Assignment const& row,Player* actor,
        bool& active,uint16& firstRule,uint32& impact)
    {
        std::vector<PositionHazard> hazards;
        for(size_t i=0;i<plan.definition.rules.size();i++)
        {
            Rule const& rule=plan.definition.rules[i];
            if(!(rule.roles&(1u<<row.role))||(rule.phase&&rule.phase!=plan.phase)||
                (rule.toggle&&!(plan.flags&rule.toggle)))continue;
            if(rule.action!=Action::AvoidPoints&&!((rule.action==Action::Spread||rule.action==Action::Isolate)&&rule.target==Target::Marked))continue;
            std::vector<ObjectGuid> targets;bool triggered=BossNearActive(plan,rule,actor);
            for(auto const& event:plan.events)if(event.rule==i)
            {targets.push_back(event.target);triggered=true;if(event.impact&&(impact==0||event.impact<impact))impact=event.impact;}
            if(rule.trigger==Trigger::MemberAura)
                for(auto const& member:plan.rows)if(Player* p=Member(plan,member.guid))
                    if(p->IsAlive())for(uint32 spell:rule.spells)if(p->HasAura(spell))
                    {targets.push_back(member.guid);triggered=true;break;}
            if(!triggered)continue;
            if(!active)firstRule=uint16(i);active=true;
            if(rule.action==Action::AvoidPoints)
            {
                bool forecast=rule.trigger==Trigger::BossNear;
                float warning=forecast?ForecastWarningSeconds(plan,rule,actor):0;
                for(Point p:HazardPoints(plan,rule))hazards.push_back({p,rule.radius+1,i,rule.priority,forecast,warning});
            }
            else for(ObjectGuid guid:targets)
                if(Unit* source=actor->GetMap()->GetUnit(guid))if(source->IsAlive())
                {
                    if(source!=actor)
                        hazards.push_back({{source->GetPositionX(),source->GetPositionY(),source->GetPositionZ()},rule.radius+1,i,rule.priority});
                    else if(rule.action==Action::Isolate)
                    {
                        // A marked actor runs out before impact instead of asking
                        // every neighbour to vacate while it continues casting -
                        // unless it is a tank holding one of the fight's units: the unit would follow it out of
                        // the healers' range and line of sight, and the peers already keep the radius from it.
                        if(!HoldingFightUnit(plan,row,actor))
                        for(auto const& peer:plan.rows)if(peer.guid!=row.guid)
                            if(Player* p=Member(plan,peer.guid))if(p->IsAlive())
                                hazards.push_back({{p->GetPositionX(),p->GetPositionY(),p->GetPositionZ()},rule.radius+1,i,rule.priority});
                    }
                }
        }
        ObjectHazards(plan,actor,hazards,active);
        UnitAuraHazards(plan,actor,hazards,active);
        IdleHostileHazards(plan,row,actor,hazards,active);
        StandoffHazards(plan,row,actor,hazards,active);
        return hazards;
    }
    bool OutsideHazard(Point p,PositionHazard const& h)
    {float dx=p.x-h.point.x,dy=p.y-h.point.y;return dx*dx+dy*dy>=h.radius*h.radius;}
    // Add hold ground (v45): the raiding rule "tank the exploding adds away from the melee". An add tank that holds
    // instant caster-centred burst carriers (listed or learned footprints) keeps them where their bursts reach only
    // itself: the footprint plus a margin clear of the objective and its tank, of the raid's kill target (the melee
    // stand on it), of the other add tanks and of every team anchor (the healers' and ranged's ground), within healing
    // reach of its own team anchor and outside hostile objects. The held units follow their victim, so the tank walks
    // them there. Majordomo, every v44 pull: four elites, the objective, both add tanks, the kill-target healer and all
    // eight rogues stood inside five yards and every Blast Wave hit all of them (rogues dead by t=31 even in the win).
    static constexpr float kAddHoldMargin=4.f;
    static constexpr float kAddHoldAnchorMargin=2.f;
    static constexpr float kAddHoldReach=30.f;
    static constexpr float kObjectiveRescueReach=20.f; // v56: the farthest an add tank walks (its add in tow) to taunt a loose objective
    static constexpr float kAddHoldSlack=2.f;
    static constexpr float kAddHoldArrival=3.f;
    // The ground is chosen once and kept (v46): majordomo-0914h-01 re-chose it every tick against the wandering kill
    // target and the other tank's moves, both add tanks walked for 60 s without threat and died with the elites loose.
    // A committed ground is given up only when it breaks a fixed constraint: the tank anchor, a team anchor, the own
    // anchor's reach or a hostile object. The moving units (objective, its tank, kill target, other add tanks' grounds)
    // only shape the choice.
    bool AddHoldRoute(Plan const& plan,Assignment& row,Player* actor,Unit* boss,Player* tank,Point& goal,Point& waypoint,float& seconds)
    {
        if(row.team>=plan.definition.teams.size()){row.holdReady=false;return false;}
        std::list<Creature*> found;
        for(uint32 entry:plan.definition.adds)actor->GetCreatureListWithEntryInGrid(found,entry,100);
        float radius=0;
        for(Creature* add:found)
            if(add->IsAlive()&&add->IsInCombat()&&!add->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)&&add->GetVictim()==actor)
                radius=std::max(radius,InstantBurstRadius(plan,add));
        if(radius<=0){row.holdReady=false;return false;}
        std::vector<std::pair<Point,float>> fixed,moving; // a point and the least distance the held units keep from it
        fixed.push_back({plan.definition.tank,radius+kAddHoldMargin});
        for(auto const& team:plan.definition.teams)fixed.push_back({team.anchor,radius+kAddHoldAnchorMargin});
        auto unit=[&](Unit* u)
        {if(u&&u!=actor&&u->IsAlive()&&u->GetVictim()!=actor)moving.push_back({{u->GetPositionX(),u->GetPositionY(),u->GetPositionZ()},radius+kAddHoldMargin});};
        unit(boss);unit(tank);unit(KillTargetAdd(plan,actor));
        for(auto const& other:plan.rows)
            if(other.role==2&&other.guid!=row.guid)
                if(Player* peer=Member(plan,other.guid))if(peer->IsAlive())
                    moving.push_back({other.holdReady?other.holdGoal:Point{peer->GetPositionX(),peer->GetPositionY(),peer->GetPositionZ()},radius+kAddHoldMargin});
        Point const own=plan.definition.teams[row.team].anchor;
        std::vector<PositionHazard> hazards;bool active=false;
        ObjectHazards(plan,actor,hazards,active);
        auto clear=[&](std::vector<std::pair<Point,float>> const& keep,Point p,float slack)
        {
            for(auto const& k:keep)
            {
                float const least=std::max(0.f,k.second-slack),dx=p.x-k.first.x,dy=p.y-k.first.y;
                if(dx*dx+dy*dy<least*least)return false;
            }
            return true;
        };
        auto fixedAllowed=[&](Point p,float slack)
        {
            if(!clear(fixed,p,slack))return false;
            float const ox=p.x-own.x,oy=p.y-own.y;
            if(ox*ox+oy*oy>(kAddHoldReach+slack)*(kAddHoldReach+slack))return false;
            for(auto const& h:hazards)if(!OutsideHazard(p,h))return false;
            return true;
        };
        Point const at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        auto walk=[&](){float dx=row.holdGoal.x-at.x,dy=row.holdGoal.y-at.y;if(dx*dx+dy*dy<=kAddHoldArrival*kAddHoldArrival)return false;goal=waypoint=row.holdGoal;return true;};
        if(row.holdReady&&fixedAllowed(row.holdGoal,kAddHoldSlack))return walk();
        row.holdReady=false;
        if(fixedAllowed(at,0)&&clear(moving,at,0)){row.holdGoal=at;row.holdReady=true;return false;} // standing on good ground already
        if(!FindClearRoute(plan,actor,[&](Point p){return fixedAllowed(p,0)&&clear(moving,p,0);},goal,waypoint,seconds,{},"add-hold"))return false;
        row.holdGoal=goal;row.holdReady=true;
        return walk();
    }
    // Bring the objective to its tank (v48): an objective whose threat reset put it on a member that cannot tank it is
    // walked by that member to the objective's own tank instead of being fought where it landed, so the tank takes it
    // back seconds sooner. Majordomo's Teleport resets his threat every 20-30 s while the owner runs back from the trap:
    // majordomo-0914j-02 lost three priests (t=46-51) and add tank 115 (t=63) under him.
    // v49: the same for the required adds. A released kill-target healer followed its top-threat damage dealer into the
    // ranged and healer ground and its 20 yd Shadow Shock hit them there (majordomo-0914k-01: Shadow Shock on the raid
    // 55k -> 130k, mages/warlocks/hunters/healers 3-4k each). An add control can hold goes to the objective's tank (the
    // add tanks hold what control cannot); an uncontrollable add goes to the nearest add tank.
    static constexpr float kBringToTankYards=6.f;
    bool BringObjectiveToTank(AiBotAI* ai,Plan& plan,Assignment& row)
    {
        if(row.manual||row.role<3||TankCapable(row))return false;
        Player* actor=ai->GetBotPlayer();
        auto const now=std::chrono::steady_clock::now();
        Player* mainTank=nullptr;
        for(auto const& other:plan.rows)
            if(other.role==1)if(Player* holder=Member(plan,other.guid))if(holder->IsAlive()&&holder!=actor){mainTank=holder;break;}
        std::vector<std::pair<Unit*,bool>> units; // unit, is an objective
        for(uint32 entry:plan.definition.objectives)
        {
            auto objective=plan.objectives.find(entry);
            if(objective!=plan.objectives.end()&&!objective->second.guid.IsEmpty())units.push_back({actor->GetMap()->GetUnit(objective->second.guid),true});
        }
        bool uncontrollable=false;
        for(auto const& pair:plan.addObjectives)
            if(Unit* add=actor->GetMap()->GetUnit(pair.first))if(add->IsAlive()&&add->IsInCombat())
            {
                units.push_back({add,false});
                uncontrollable|=!RaidCanControl(plan,add,now);
            }
        for(auto const& candidate:units)
        {
            Unit* unit=candidate.first;
            if(!unit||!unit->IsAlive()||!unit->IsInCombat()||unit->GetVictim()!=actor)continue;
            Player* tank=nullptr;
            if(candidate.second)
            {
                for(auto const& other:plan.rows)
                    if(other.role==1&&other.focus==unit->GetEntry())
                        if(Player* holder=Member(plan,other.guid))if(holder->IsAlive()&&holder!=actor){tank=holder;break;}
            }
            else if(uncontrollable&&RaidCanControl(plan,unit,now))tank=mainTank;
            else
            {
                float best=1e9f;
                for(auto const& other:plan.rows)
                    if(other.role==2)if(Player* holder=Member(plan,other.guid))
                        if(holder->IsAlive()&&holder!=actor&&actor->GetDistance(holder)<best){best=actor->GetDistance(holder);tank=holder;}
            }
            if(!tank)tank=mainTank;
            if(!tank||actor->GetDistance(tank)<=kBringToTankYards)return false;
            row.target=unit->GetObjectGuid();
            return Move(ai,plan,row,{tank->GetPositionX(),tank->GetPositionY(),tank->GetPositionZ()},6,false,kBringToTankYards);
        }
        return false;
    }
    bool PositionClear(Plan const& plan,Assignment const& row,Player* actor,Point p,std::vector<PositionHazard> const& hazards)
    {
        if(!ConeStationAllowed(plan,row,actor,p))return false;
        for(auto const& h:hazards)if(!OutsideHazard(p,h))return false;
        return true;
    }
    bool EscapePending(Assignment const& row,Point at,uint8 phase)
    {
        if(!row.escapeGoalReady)return false;
        float dx=at.x-row.escapeGoal.x,dy=at.y-row.escapeGoal.y;
        if(row.escapePhase!=phase||dx*dx+dy*dy<=2.25f){row.escapeGoalReady=false;return false;}
        return true;
    }
    float FormationSpacing(Plan const& plan,Assignment const& row)
    {
        float spacing=0;
        for(auto const& rule:plan.definition.rules)
            if(rule.action==Action::Spread&&rule.trigger==Trigger::Always&&rule.target==Target::Self&&
                (rule.roles&(1u<<row.role))&&(!rule.phase||rule.phase==plan.phase)&&
                (!rule.toggle||(plan.flags&rule.toggle)))spacing=std::max(spacing,rule.radius);
        return spacing;
    }
    float DestinationCrowding(Plan const& plan,Assignment const& row,Point p)
    {
        float spacing=0;
        for(auto const& rule:plan.definition.rules)
            if(rule.action==Action::Spread&&(rule.roles&(1u<<row.role))&&
                (!rule.phase||rule.phase==plan.phase)&&(!rule.toggle||(plan.flags&rule.toggle)))
                spacing=std::max(spacing,rule.radius);
        float formation=FormationSpacing(plan,row);
        if(formation>0)spacing=formation;
        if(spacing<=0)return 0;
        float score=0;
        for(auto const& other:plan.rows)if(other.guid!=row.guid&&!other.yielded)
            if(Player* member=Member(plan,other.guid))if(member->IsAlive())
            {
                // A moving peer owns its intended destination. Treating every
                // mover only at its current position sends the whole raid to one point.
                Point q=other.escapeGoalReady&&other.escapePhase==plan.phase?other.escapeGoal:
                    other.hasMove?other.lastMove:Point{member->GetPositionX(),member->GetPositionY(),member->GetPositionZ()};
                float dx=p.x-q.x,dy=p.y-q.y;
                float ax=p.x-member->GetPositionX(),ay=p.y-member->GetPositionY();
                // Reserve destinations without pretending a travelling peer has
                // already vacated its current position. This also protects idle clusters.
                float distance=std::min(std::sqrt(dx*dx+dy*dy),std::sqrt(ax*ax+ay*ay));
                score+=(formation>0?12.f:4.f)*std::max(0.f,spacing-distance);
            }
        return score;
    }
    // A possible footprint may be occupied only while a complete ordinary
    // evacuation path is proved inside its parsed warning budget. Every part of
    // the proposed movement can use its remaining suffix plus that evacuation;
    // checking total travel is conservative and avoids straight-line shortcuts.
    bool ForecastRouteCertified(Plan const& plan,Assignment const& row,Player* actor,
        std::vector<PositionHazard> const& hazards,Point goal,float travel,PointsArray const& route,
        float controlLock,unsigned& queries)
    {
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        std::set<size_t> groups;
        for(auto const& h:hazards)if(h.forecast)groups.insert(h.group);
        for(size_t group:groups)
        {
            auto outside=[&](Point p){for(auto const& h:hazards)if(h.group==group&&!OutsideHazard(p,h))return false;return true;};
            bool entered=!outside(at);Point previous=at;
            for(auto const& node:route)
            {
                Point next{node.x,node.y,node.z};float dx=next.x-previous.x,dy=next.y-previous.y,dz=next.z-previous.z;
                int n=std::max(1,int(std::ceil(std::sqrt(dx*dx+dy*dy+dz*dz))));if(n>1000)return false;
                for(int i=1;i<=n&&!entered;++i){float f=float(i)/n;entered=!outside({previous.x+dx*f,previous.y+dy*f,previous.z+dz*f});}
                previous=next;
            }
            if(!entered&&outside(goal))continue;
            float warning=0;for(auto const& h:hazards)if(h.group==group){warning=h.warning;break;}
            // Manual guidance is accepted by the client for up to 2.5 seconds.
            // Reserve that entire advice age before the ordinary one-second
            // execution margin; a future escape cannot assume instant input.
            float budget=(warning-controlLock-1.f-(row.manual?2.5f:0.f))*actor->GetSpeed(MOVE_RUN)-travel;
            if(budget<0)return false;
            if(outside(goal))continue;
            auto safe=[&](Point p)
            {
                if(!InRoom(plan,p)||!ConeStationAllowed(plan,row,actor,p))return false;
                for(auto const& h:hazards)if(!h.forecast&&!OutsideHazard(p,h))return false;
                return true;
            };
            std::vector<std::pair<float,Point>> exits;
            auto const& d=plan.definition;
            for(int ix=1;ix<20;++ix)for(int iy=1;iy<20;++iy)
            {
                Point q{d.low.x+(d.high.x-d.low.x)*ix/20,d.low.y+(d.high.y-d.low.y)*iy/20,goal.z};
                float dx=q.x-goal.x,dy=q.y-goal.y,dist=std::sqrt(dx*dx+dy*dy);
                if(dist<=budget&&outside(q)&&safe(q))exits.push_back({dist,q});
            }
            std::stable_sort(exits.begin(),exits.end(),[](auto const& a,auto const& b){return a.first<b.first;});
            bool certified=false;unsigned tried=0;
            for(auto const& exit:exits)
            {
                if(tried++>=12||queries>=64)break;++queries;
                Point q=exit.second;actor->UpdateAllowedPositionZ(q.x,q.y,q.z);if(!outside(q)||!safe(q))continue;
                PathInfo path(actor);path.calculate(Vector3(goal.x,goal.y,goal.z),Vector3(q.x,q.y,q.z));
                if(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE|PATHFIND_SHORTCUT|PATHFIND_NOT_USING_PATH|PATHFIND_DEST_FORCED)||path.getPath().empty())continue;
                auto const& end=path.getPath().back();Point finish{end.x,end.y,end.z};
                float ex=finish.x-q.x,ey=finish.y-q.y,ez=finish.z-q.z;
                if(ex*ex+ey*ey+ez*ez>2.25f||!outside(finish)||!safe(finish))continue;
                Point previous=goal;float length=0;bool valid=true;
                for(auto const& node:path.getPath())
                {
                    Point next{node.x,node.y,node.z};float dx=next.x-previous.x,dy=next.y-previous.y,dz=next.z-previous.z;
                    float segment=std::sqrt(dx*dx+dy*dy+dz*dz);length+=segment;
                    if(length>budget){valid=false;break;}
                    int n=std::max(1,int(std::ceil(segment)));if(n>1000){valid=false;break;}
                    for(int i=1;i<=n;++i){float f=float(i)/n;if(!safe({previous.x+dx*f,previous.y+dy*f,previous.z+dz*f})){valid=false;break;}}
                    if(!valid)break;previous=next;
                }
                if(valid){certified=true;break;}
            }
            if(!certified)return false;
        }
        return true;
    }
    std::vector<PositionHazard> ObservedHazards(std::vector<PositionHazard> const& hazards)
    {
        std::vector<PositionHazard> observed;
        for(auto const& h:hazards)if(!h.forecast)observed.push_back(h);
        return observed;
    }
    std::function<bool(Point,float,PointsArray const&)> ForecastVerifier(Plan const& plan,Assignment const& row,
        Player* actor,std::vector<PositionHazard> const& hazards)
    {
        if(std::none_of(hazards.begin(),hazards.end(),[](PositionHazard const& h){return h.forecast;}))return {};
        // This predicate is consumed synchronously by the complete-route solver.
        return [&plan,&row,actor,&hazards,control=ObservedMovementLockSeconds(plan,actor),queries=0u]
            (Point p,float travel,PointsArray const& path) mutable
        {return ForecastRouteCertified(plan,row,actor,hazards,p,travel,path,control,queries);};
    }
    bool CurrentPositionCertified(Plan const& plan,Assignment const& row,Player* actor,
        Point at,std::vector<PositionHazard> const& hazards)
    {
        auto observed=ObservedHazards(hazards);
        if(!PositionClear(plan,row,actor,at,observed))return false;
        auto verify=ForecastVerifier(plan,row,actor,hazards);
        return !verify||verify(at,0,PointsArray{});
    }
    bool CombinedEscape(Plan const& plan,Assignment const& row,Player* actor,std::vector<PositionHazard> const& hazards,
        Point& goal,Point& waypoint,float& seconds,bool* compromised=nullptr)
    {
        if(compromised)*compromised=false;
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        std::map<size_t,bool> initiallySafe;
        int highest=std::numeric_limits<int>::min();
        for(auto const& h:hazards)
        {
            auto inserted=initiallySafe.emplace(h.group,true);
            inserted.first->second&=OutsideHazard(at,h);highest=std::max(highest,h.priority);
        }
        auto crowding=[&](Point p)
        {
            float score=DestinationCrowding(plan,row,p);
            // Revalidate every route, but do not reverse a still-valid escape
            // merely because peers moved. Hazard exposure always remains dominant.
            if(EscapePending(row,at,plan.phase))
            {float dx=p.x-row.escapeGoal.x,dy=p.y-row.escapeGoal.y;if(dx*dx+dy*dy>1.f)score+=20.f;}
            return score;
        };
        auto remember=[&](){row.escapeGoal=goal;row.escapeGoalReady=true;row.escapePhase=plan.phase;};
        auto traverse=[&](Point p)
        {
            // A multi-point lane is one footprint. Crossing its overlapping
            // circles while escaping is not entry into another independent hazard.
            for(auto const& h:hazards)if(initiallySafe[h.group]&&!OutsideHazard(p,h))return false;
            return !ConeStationAllowed(plan,row,actor,at)||ConeStationAllowed(plan,row,actor,p);
        };
        if(std::any_of(hazards.begin(),hazards.end(),[](PositionHazard const& h){return h.forecast&&h.warning>0;}))
        {
            auto observed=ObservedHazards(hazards);
            auto observedTraverse=[&](Point p)
            {
                for(auto const& h:observed)if(initiallySafe[h.group]&&!OutsideHazard(p,h))return false;
                return !ConeStationAllowed(plan,row,actor,at)||ConeStationAllowed(plan,row,actor,p);
            };
            unsigned queries=0;float lock=ObservedMovementLockSeconds(plan,actor);
            auto started=std::chrono::steady_clock::now();
            auto certified=[&](Point p,float travel,PointsArray const& path)
            {return ForecastRouteCertified(plan,row,actor,hazards,p,travel,path,lock,queries);};
            // Safety precedes optional formation optimization. A certified
            // current hold must not keep cancelling healing or add protection
            // merely because another point would be less crowded.
            bool found=PositionClear(plan,row,actor,at,observed)&&certified(at,0,PointsArray{});
            if(found){goal=waypoint=at;seconds=0;}
            else found=FindClearRoute(plan,actor,[&](Point p){return PositionClear(plan,row,actor,p,observed);},goal,waypoint,seconds,
                observedTraverse,"forecast-deadline",crowding,certified);
            static std::map<uint64,std::chrono::steady_clock::time_point> last;
            auto finished=std::chrono::steady_clock::now();auto& previous=last[Raw(actor->GetObjectGuid())];
            if(finished-previous>=std::chrono::seconds(5))
            {
                previous=finished;
                sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-forecast] actor=%llu certified=%u queries=%u control=%.2f travel=%.2f micros=%lld",
                    static_cast<unsigned long long>(Raw(actor->GetObjectGuid())),unsigned(found),queries,lock,seconds,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(finished-started).count()));
            }
            if(found){remember();return true;}
        }
        if(FindClearRoute(plan,actor,[&](Point p){return PositionClear(plan,row,actor,p,hazards);},goal,waypoint,seconds,traverse,"combined",crowding)){remember();return true;}
        if(hazards.empty())return false;
        // If every constraint cannot be satisfied, retain the authored highest
        // priority footprint and accept a lower-priority risk instead of freezing
        // in a lethal lane. This is a best-effort route, never reported as safe.
        auto urgent=[&](Point p)
        {
            if(!ConeStationAllowed(plan,row,actor,p))return false;
            for(auto const& h:hazards)if(h.priority==highest&&!OutsideHazard(p,h))return false;
            return true;
        };
        auto exposure=[&](Point p)
        {
            float score=crowding(p);
            for(auto const& h:hazards)if(h.priority<highest)
            {
                float dx=p.x-h.point.x,dy=p.y-h.point.y;
                float penetration=std::max(0.f,1.f-std::sqrt(dx*dx+dy*dy)/h.radius);
                score+=300.f*penetration*penetration;
            }
            return score;
        };
        // Being clear of the lethal lane is not a reason to stand in stacked
        // splash. Minimize remaining exposure while preserving that lane over
        // the complete route; a nonzero residual risk stays explicitly compromised.
        if(!FindClearRoute(plan,actor,urgent,goal,waypoint,seconds,{},"priority",exposure))return false;
        remember();if(compromised)*compromised=true;return true;
    }
    float FormationCost(Plan const& plan,Assignment const& row,Point p)
    {return FormationSpacing(plan,row)>0?DestinationCrowding(plan,row,p):0.f;}
    bool FormationRoute(Plan const& plan,Assignment const& row,Player* actor,Point& goal,Point& waypoint,float& seconds)
    {
        if(FormationSpacing(plan,row)<=0)return false;
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        // Small deviations from preferred spacing do not justify cancelling
        // productive positioning. Marked spell hazards remain urgent in RunRules.
        if(FormationCost(plan,row,at)<=24.f)return false;
        bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        auto verify=ForecastVerifier(plan,row,actor,hazards);
        auto observed=ObservedHazards(hazards);
        auto safe=[&](Point p){return PositionClear(plan,row,actor,p,observed);};
        auto usable=[&](Point p)
        {
            if(!safe(p))return false;
            return HealerStationAllowed(plan,row,p);
        };
        // Routine spreading never takes a compromised escape or crosses a hazard,
        // and a healer's destination must retain the assigned patient's reach.
        if(!safe(at)||!FindClearRoute(plan,actor,usable,goal,waypoint,seconds,safe,"formation",
            [&](Point p){return FormationCost(plan,row,p);},verify))return false;
        float dx=goal.x-at.x,dy=goal.y-at.y;
        return dx*dx+dy*dy>2.25f&&FormationCost(plan,row,goal)+2.f<FormationCost(plan,row,at);
    }
    bool MaintainFormation(AiBotAI* ai,Plan const& plan,Assignment& row)
    {
        Player* actor=ai->GetBotPlayer();
        if(actor->IsNonMeleeSpellCasted(false,false,true))return false;
        Point goal,waypoint;float seconds=0;
        if(FormationRoute(plan,row,actor,goal,waypoint,seconds))return Move(ai,plan,row,goal,7);
        if(row.duty==7&&FormationSpacing(plan,row)>0)StopMechanicMovement(actor,row);
        return false;
    }
    bool CombatApproachRoute(Plan const& plan,Assignment const& row,Player* actor,Unit* target,
        Point& goal,Point& waypoint,float& seconds)
    {
        bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        auto verify=ForecastVerifier(plan,row,actor,hazards);
        auto observed=ObservedHazards(hazards);
        auto safe=[&](Point p){return PositionClear(plan,row,actor,p,observed);};
        auto contact=[&](Point p){return safe(p)&&actor->CanReachWithMeleeAutoAttackAtPosition(target,p.x,p.y,p.z,-1.5f);};
        // Role movement must keep the complete route safe, not merely its endpoint.
        // The contact margin accounts for Move's ordinary1.5-yard arrival tolerance.
        // Candidates are pre-filtered at the target's floor height: a raid staged on a lower level (a
        // tunnel mouth below the pull floor) otherwise finds no contact, range or line-of-sight
        // candidate at all and holds while its tank fights alone (the melee station already does this).
        return FindClearRoute(plan,actor,contact,goal,waypoint,seconds,safe,"combat-approach",{},verify,target->GetPositionZ());
    }
    bool SafeCombatApproach(AiBotAI* ai,Plan const& plan,Assignment& row,Unit* target)
    {
        Player* actor=ai->GetBotPlayer();Point goal,waypoint;float seconds=0;
        if(!CombatApproachRoute(plan,row,actor,target,goal,waypoint,seconds))
        {StopMechanicMovement(actor,row);actor->AttackStop();row.duty=12;return true;}
        row.target=target->GetObjectGuid();
        if(Move(ai,plan,row,goal,6))return true;
        return !actor->CanReachWithMeleeAutoAttack(target);
    }
    bool FindRoleRoute(Plan const& plan,Player* actor,std::function<bool(Point)> const& destination,
        Point& goal,Point& waypoint,float& seconds,std::function<bool(Point)> const& safe,char const* reason,
        std::function<float(Point)> const& cost,std::function<bool(Point,float,PointsArray const&)> const& accept,float floorReference=NAN)
    {
        if(FindClearRoute(plan,actor,destination,goal,waypoint,seconds,safe,reason,cost,accept,floorReference))return true;
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        auto distance=[](Point a,Point b){float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return std::sqrt(x*x+y*y+z*z);};
        auto price=[&](Point p){return cost?std::max(0.f,cost(p)):0.f;};
        std::vector<Point> goals,vias;
        for(int x=1;x<20;++x)for(int y=1;y<20;++y)
        {
            Point p{plan.definition.low.x+(plan.definition.high.x-plan.definition.low.x)*x/20,
                plan.definition.low.y+(plan.definition.high.y-plan.definition.low.y)*y/20,std::isfinite(floorReference)?floorReference:at.z};
            actor->UpdateAllowedPositionZ(p.x,p.y,p.z);
            if(!InRoom(plan,p)||!safe(p))continue;
            if(destination(p))goals.push_back(p);else if(distance(at,p)>2.f)vias.push_back(p);
        }
        if(goals.empty())return false;
        // Costs are immutable during this synchronous search. Compute each once,
        // preserving the stable ordering and the exact bounded path checks below.
        std::vector<float> goalPrices;goalPrices.reserve(goals.size());
        for(Point end:goals)goalPrices.push_back(price(end));
        std::vector<std::pair<float,Point>> rankedVias;rankedVias.reserve(vias.size());
        for(Point via:vias)
        {
            float tail=100000;
            for(size_t i=0;i<goals.size();++i)tail=std::min(tail,distance(via,goals[i])+goalPrices[i]);
            rankedVias.push_back({distance(at,via)+tail,via});
        }
        std::stable_sort(rankedVias.begin(),rankedVias.end(),[](auto const& a,auto const& b){return a.first<b.first;});
        vias.clear();
        for(size_t i=0;i<std::min(size_t(128),rankedVias.size());++i)vias.push_back(rankedVias[i].second);
        // Cheap geometric rejection is only a prefilter. Actual navmesh legs below
        // must still pass every sample, endpoint, bounds and forecast check.
        auto segment=[&](Point a,Point b)
        {
            int samples=std::max(1,int(std::ceil(distance(a,b))));if(samples>1000)return false;
            for(int i=1;i<=samples;++i){float f=float(i)/samples;
                if(!safe({a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f}))return false;}
            return true;
        };
        auto complete=[&](PathInfo const& path,Point end)
        {
            if(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE|PATHFIND_SHORTCUT|PATHFIND_NOT_USING_PATH|PATHFIND_DEST_FORCED)||path.getPath().empty())return false;
            auto p=path.getPath().back();return distance({p.x,p.y,p.z},end)<=1.5f;
        };
        unsigned queries=0;
        for(Point via:vias)
        {
            if(queries>=64)break;
            if(!segment(at,via))continue;
            std::vector<std::pair<float,Point>> rankedEnds;rankedEnds.reserve(goals.size());
            for(size_t i=0;i<goals.size();++i)rankedEnds.push_back({distance(via,goals[i])+goalPrices[i],goals[i]});
            std::stable_sort(rankedEnds.begin(),rankedEnds.end(),[](auto const& a,auto const& b){return a.first<b.first;});
            std::vector<Point> ends;
            for(size_t i=0;i<std::min(size_t(8),rankedEnds.size());++i)ends.push_back(rankedEnds[i].second);
            ends.erase(std::remove_if(ends.begin(),ends.end(),[&](Point end){return !segment(via,end);}),ends.end());
            if(ends.empty())continue;
            PathInfo first(actor);++queries;first.calculate(via.x,via.y,via.z);
            if(!complete(first,via))continue;
            // Start the next leg at the actual endpoint, never an assumed point.
            auto join=first.getPath().back();Point pivot{join.x,join.y,join.z};
            for(Point end:ends)
            {
                if(queries>=64)break;
                if(!segment(pivot,end))continue;
                PathInfo second(actor);++queries;second.calculate({pivot.x,pivot.y,pivot.z},{end.x,end.y,end.z});
                if(!complete(second,end))continue;
                PointsArray path=first.getPath();path.insert(path.end(),second.getPath().begin(),second.getPath().end());
                Point previous=at;bool entered=InRoom(plan,at),arrived=destination(at),valid=true;float travel=0;
                for(auto const& node:path)
                {
                    Point next{node.x,node.y,node.z};float length=distance(previous,next);travel+=length;
                    int samples=std::max(1,int(std::ceil(length)));if(samples>1000){valid=false;break;}
                    for(int i=1;i<=samples;++i)
                    {
                        float f=float(i)/samples;Point p{previous.x+(next.x-previous.x)*f,previous.y+(next.y-previous.y)*f,previous.z+(next.z-previous.z)*f};
                        bool inside=InRoom(plan,p),reached=destination(p);
                        if((entered&&!inside)||!safe(p)||(arrived&&!reached)){valid=false;break;}
                        entered|=inside;arrived|=reached;
                    }
                    if(!valid)break;previous=next;
                }
                if(!valid||!entered||!arrived||(accept&&!accept(end,travel,path)))continue;
                goal=end;waypoint=FirstRouteTurn(at,path);seconds=travel/std::max(.1f,actor->GetSpeed(MOVE_RUN));
                static std::map<uint64,std::chrono::steady_clock::time_point> last;
                auto now=std::chrono::steady_clock::now();auto& previousLog=last[Raw(actor->GetObjectGuid())];
                if(now-previousLog>=std::chrono::seconds(5))
                {
                    previousLog=now;
                    sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-role-detour] actor=%u kind=%s via=%.2f,%.2f goal=%.2f,%.2f seconds=%.2f queries=%u",
                        actor->GetGUIDLow(),reason,pivot.x,pivot.y,end.x,end.y,seconds,queries);
                }
                return true;
            }
        }
        return false; // Bounded search exhaustion is not a safety certificate.
    }
    bool SafeStationMove(AiBotAI* ai,Plan const& plan,Assignment& row,Point station)
    {
        Player* actor=ai->GetBotPlayer();bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        auto verify=ForecastVerifier(plan,row,actor,hazards);
        auto observed=ObservedHazards(hazards);
        auto safe=[&](Point p){return PositionClear(plan,row,actor,p,observed);};
        bool const melee=MeleeStationRequired(plan,row);
        Unit* target=melee?actor->GetMap()->GetUnit(plan.boss):nullptr;
        auto nearStation=[&](Point p)
        {
            float dx=p.x-station.x,dy=p.y-station.y;
            return dx*dx+dy*dy<=64&&safe(p)&&HealerStationAllowed(plan,row,p)&&
                (!melee||(target&&actor->CanReachWithMeleeAutoAttackAtPosition(target,p.x,p.y,p.z,-.2f)&&target->IsWithinLOS(p.x,p.y,p.z)));
        };
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        if(nearStation(at)&&(!melee||!verify||verify(at,0,PointsArray{})))
        {StopMechanicMovement(actor,row);return false;}
        Point goal,waypoint;float seconds=0;
        if(melee)
        {
            // Ordinary flank approach obeys the same complete hazard/forecast route
            // checks as escape. Being near an anchor does not establish melee contact.
            if(!FindRoleRoute(plan,actor,nearStation,goal,waypoint,seconds,safe,"melee-station",
                [&](Point p){return FormationCost(plan,row,p);},verify,station.z))
            {StopMechanicMovement(actor,row);actor->AttackStop();row.duty=12;return true;}
            row.target=target->GetObjectGuid();
            return Move(ai,plan,row,waypoint,6,false,.2f);
        }
        if(!FindClearRoute(plan,actor,nearStation,goal,waypoint,seconds,safe,"station",[&](Point p){return FormationCost(plan,row,p);},verify))
        {StopMechanicMovement(actor,row);return false;}
        return Move(ai,plan,row,goal,6);
    }
    bool RecoverSpellRange(AiBotAI* ai,Plan const& plan,Assignment& row,Unit* enemy,uint32 spellId,char const* reason,bool holdOnFailure,bool restoreFormation=false)
    {
        Player* actor=ai->GetBotPlayer();auto spell=sSpellMgr.GetSpellEntry(spellId);
        if(!enemy||!spell)return false;
        auto range=sSpellRangeStore.LookupEntry(spell->rangeIndex);
        float maximum=Spells::GetSpellMaxRange(range)-2.f;
        float minimum=Spells::GetSpellMinRange(range);if(minimum>0)minimum+=2.f;
        if(maximum<=minimum)return false;
        bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        auto verify=ForecastVerifier(plan,row,actor,hazards);
        auto observed=ObservedHazards(hazards);
        auto safe=[&](Point p){return PositionClear(plan,row,actor,p,observed);};
        auto canShoot=[&](Point p)
        {
            // Spell::CheckRange uses combat distance. Base range with a2yard
            // arrival margin is conservative; actual casts still apply spell mods.
            float distance=std::max(0.f,enemy->GetDistance(p.x,p.y,p.z,SizeFactor::CombatReach)-actor->GetCombatReach());
            return distance>=minimum&&distance<=maximum&&safe(p)&&enemy->IsWithinLOS(p.x,p.y,p.z);
        };
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        if(canShoot(at))
        {
            // Between casts, repair dangerous crowding without giving up the
            // current target's spell range or line of sight. Optional spreading
            // never suppresses damage when no certified improvement exists.
            float cost=FormationCost(plan,row,at);
            if(restoreFormation&&cost>24.f)
            {
                Point goal,waypoint;float seconds=0;
                if(FindRoleRoute(plan,actor,canShoot,goal,waypoint,seconds,safe,"ranged-formation",
                    [&](Point p){return FormationCost(plan,row,p);},verify,enemy->GetPositionZ()))
                {
                    float dx=goal.x-at.x,dy=goal.y-at.y;
                    if(dx*dx+dy*dy>2.25f&&FormationCost(plan,row,goal)+2.f<cost)
                    {row.target=enemy->GetObjectGuid();return Move(ai,plan,row,waypoint,7,false,.2f);}
                }
            }
            StopMechanicMovement(actor,row);return false;
        }
        Point goal,waypoint;float seconds=0;
        if(!FindRoleRoute(plan,actor,canShoot,goal,waypoint,seconds,safe,reason,[&](Point p){return FormationCost(plan,row,p);},verify,enemy->GetPositionZ()))
        {if(holdOnFailure){StopMechanicMovement(actor,row);row.duty=12;}return holdOnFailure;}
        row.target=enemy->GetObjectGuid();return Move(ai,plan,row,waypoint,6,false,.2f);
    }
    bool RecoverDamageRange(AiBotAI* ai,Plan const& plan,Assignment& row,Unit* enemy)
    {return RecoverSpellRange(ai,plan,row,enemy,row.damage,"ranged",true,true);}
    bool RecoverHealingRange(AiBotAI* ai,Plan const& plan,Assignment& row,Player* fallback)
    {
        Player* actor=ai->GetBotPlayer();
        bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        auto verify=ForecastVerifier(plan,row,actor,hazards);
        auto observed=ObservedHazards(hazards);
        auto safe=[&](Point p){return PositionClear(plan,row,actor,p,observed);};
        Player* patient=Member(plan,row.primary);
        if(!patient||!patient->IsAlive())patient=fallback;
        if(!patient||!patient->IsAlive())return false;
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        if(HealingPositionAllowed(row,patient,at)){StopMechanicMovement(actor,row);return false;}
        // Two yards of arrival hysteresis covers the ordinary movement tolerance.
        auto reachablePatient=[&](Point p){return safe(p)&&HealingPositionAllowed(row,patient,p,2.f);};
        // Recompute the complete approach when positions or hazards change.
        Point goal,waypoint;float seconds=0;
        // Range is a destination requirement. Every approach sample must also
        // respect currently active cones, even before healing range is reached.
        if(!FindRoleRoute(plan,actor,reachablePatient,goal,waypoint,seconds,
            safe,"healing",[&](Point p){return FormationCost(plan,row,p);},verify,patient->GetPositionZ()))
        {
            // The room grid is coarse (a twentieth of the room per cell): a thin annulus between a stand-off region
            // and the working range holds no grid point. Sample the ring around the patient directly.
            float const ring=HealingWorkingRange(row,StandoffFloor(patient))-2.f;Point chosen;float bestDistance=100000;bool found=false;
            for(int step=0;step<24&&ring>0;++step)
            {
                float const angle=step*(2.f*float(M_PI)/24.f);
                Point p{patient->GetPositionX()+ring*std::cos(angle),patient->GetPositionY()+ring*std::sin(angle),patient->GetPositionZ()};
                actor->UpdateAllowedPositionZ(p.x,p.y,p.z);
                if(!InRoom(plan,p)||!reachablePatient(p))continue;
                PathInfo path(actor);path.calculate(p.x,p.y,p.z);
                if(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE)||path.getPath().empty())continue;
                float dx=p.x-at.x,dy=p.y-at.y,d=std::sqrt(dx*dx+dy*dy)+FormationCost(plan,row,p);
                if(d<bestDistance){bestDistance=d;chosen=p;found=true;}
            }
            if(!found)return false;
            row.target=patient->GetObjectGuid();
            return Move(ai,plan,row,chosen,6,false,.2f);
        }
        row.target=patient->GetObjectGuid();
        return Move(ai,plan,row,waypoint,6,false,.2f);
    }
    bool TankHoldAllowed(Plan const& plan,Player* actor,Unit* boss)
    {
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        return InRoom(plan,at)&&actor->CanReachWithMeleeAutoAttack(boss)&&
            actor->IsWithinLOS(boss->GetPositionX(),boss->GetPositionY(),boss->GetPositionZ());
    }
    Advice Guidance(Plan const& plan,Assignment const& row)
    {
    SlowRaidScope slow{"guidance",uint32(Raw(row.guid))};
        Advice advice;
        Player* actor=Member(plan,row.guid);
        if(plan.state!=2||!actor||!actor->IsAlive()||row.yielded)return advice;
        bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        if(active)
        {
            Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
            advice.rule=firstRule;advice.impact=impact;advice.waypoint=at;
            if((PositionClear(plan,row,actor,at,hazards)||CurrentPositionCertified(plan,row,actor,at,hazards))&&!EscapePending(row,at,plan.phase))
            {
                advice.state=1;
                if(row.manual)
                {
                    Point goal;float seconds=0;
                    if(FormationRoute(plan,row,actor,goal,advice.waypoint,seconds))
                    {
                        for(size_t i=0;i<plan.definition.rules.size();++i)
                        {auto const& rule=plan.definition.rules[i];if(rule.action==Action::Spread&&rule.trigger==Trigger::Always&&rule.target==Target::Self&&
                            (rule.roles&(1u<<row.role))&&(!rule.phase||rule.phase==plan.phase)&&(!rule.toggle||(plan.flags&rule.toggle))){advice.rule=uint16(i);break;}}
                        advice.state=2;return advice;
                    }
                }
                // A safe manual tank still needs a safe approach after landing.
                // Share the bot contact solver instead of freezing for an entire
                // fireball duration while ordinary healer threat owns the boss.
                if(row.manual&&row.role==1)
                {
                    Phase const* phase=nullptr;
                    for(auto const& p:plan.definition.phases)if(p.id==plan.phase){phase=&p;break;}
                    Unit* boss=actor->GetMap()->GetUnit(plan.boss);
                    Point goal;float seconds=0;bool route=false;
                    if(phase&&MeleeAllowed(*phase,row.role)&&boss&&boss->IsAlive()&&boss->IsInCombat()&&!actor->CanReachWithMeleeAutoAttack(boss))
                        route=CombatApproachRoute(plan,row,actor,boss,goal,advice.waypoint,seconds);
                    else if(phase&&!MeleeAllowed(*phase,row.role))
                    {
                        auto verify=ForecastVerifier(plan,row,actor,hazards);
                        auto observed=ObservedHazards(hazards);
                        auto safe=[&](Point p){return PositionClear(plan,row,actor,p,observed);};
                        auto nearAir=[&](Point p){float dx=p.x-row.air.x,dy=p.y-row.air.y;return dx*dx+dy*dy<=64&&safe(p);};
                        if(!nearAir(at))route=FindClearRoute(plan,actor,nearAir,goal,advice.waypoint,seconds,safe,"manual-air",[&](Point p){return FormationCost(plan,row,p);},verify);
                    }
                    if(route)advice.state=2;
                    else advice.waypoint=at;
                }
                return advice;
            }
            if(!row.manual){advice.state=row.hasMove&&row.duty==6?2:3;if(advice.state==2)advice.waypoint=row.lastMove;return advice;}
            Point goal;float seconds=0;bool compromised=false;
            if(!CombinedEscape(plan,row,actor,hazards,goal,advice.waypoint,seconds,&compromised)){advice.state=3;return advice;}
            advice.state=!compromised&&seconds<=.01f?1:!compromised&&impact&&seconds+.25f<=impact/1000.f?2:4;return advice;
        }
        row.escapeGoalReady=false;
        if(row.manual&&row.role==1&&!plan.enteredCombat)
        {
            // Pre-combat approach: the client walks the owner straight at the bound objective, which a
            // tunnel or ridge stops; route it along the navmesh instead (the same contact solver the
            // bots use). The client follows a movement advice only under a movement rule of the
            // definition, so one is referenced; a definition without any leaves the straight walk.
            Unit* boss=actor->GetMap()->GetUnit(plan.boss);int movementRule=-1;
            for(size_t i=0;i<plan.definition.rules.size()&&movementRule<0;++i)
            {auto const& rule=plan.definition.rules[i];if(rule.action==Action::Spread||rule.action==Action::AvoidPoints||rule.action==Action::AvoidCones||rule.action==Action::Isolate)movementRule=int(i);}
            if(boss&&boss->IsAlive()&&!boss->IsInCombat()&&movementRule>=0&&!actor->CanReachWithMeleeAutoAttack(boss))
            {
                // The navmesh path to the unit itself; the client walks straight at the advised point
                // and re-reads it every status, so the point is the first node a few yards ahead
                // (a smooth path opens with sub-yard nodes, which fall under the client's move
                // threshold and would leave it jittering in place). Straight-line walking from the
                // owner to that node stays inside the corridor the path threads.
                Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
                // A unit on a waypoint patrol is engaged where its route passes the tank anchor, never
                // followed along it: the route runs through the groups it patrols past, and a puller
                // who chases it engages whatever stands there (one group per pull). Until the
                // patroller is within engage reach of the anchor - or of the tank once the approach
                // has begun, so a unit walking away is not released mid-approach - the tank walks to
                // the anchor and holds there. A spawn that idles or wanders at home is approached
                // directly, as before.
                Point const anchor=plan.definition.tank;
                bool const patrolling=boss->GetTypeId()==TYPEID_UNIT&&static_cast<Creature*>(boss)->GetDefaultMovementType()==WAYPOINT_MOTION_TYPE;
                float const fromAnchor=std::hypot(boss->GetPositionX()-anchor.x,boss->GetPositionY()-anchor.y);
                float const fromTank=std::hypot(boss->GetPositionX()-at.x,boss->GetPositionY()-at.y);
                // (2026-09-13, v26) The tank walks out to a patroller once it is within engage reach of the anchor
                // or the tank, meets it, and the combat guidance below then drags it back to the anchor.
                bool const engage=!patrolling||fromAnchor<=kPatrolEngageYards||fromTank<=kPatrolEngageYards;
                Point const destination=engage?Point{boss->GetPositionX(),boss->GetPositionY(),boss->GetPositionZ()}:anchor;
                if(!engage&&std::hypot(destination.x-at.x,destination.y-at.y)<=2.5f)
                {advice.waypoint=at;advice.rule=uint16(movementRule);advice.state=1;return advice;}
                PathInfo path(actor);path.calculate(destination.x,destination.y,destination.z);
                if(!(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE))&&!path.getPath().empty())
                {
                    Point ahead=at;bool chosen=false;
                    for(auto const& node:path.getPath())
                    {
                        float dx=node.x-at.x,dy=node.y-at.y;
                        if(std::sqrt(dx*dx+dy*dy)>=2.5f){ahead={node.x,node.y,node.z};chosen=true;break;}
                    }
                    if(!chosen){auto const& last=path.getPath().back();ahead={last.x,last.y,last.z};}
                    if(InRoom(plan,ahead)){advice.waypoint=ahead;advice.rule=uint16(movementRule);advice.state=2;return advice;}
                }
            }
        }
        if(row.manual&&row.role==1&&plan.enteredCombat)
        {
            // (2026-09-13) The manual tank brings its target home: with the objective on the tank and the tank away
            // from the anchor (a pull that met it elsewhere, an escape that ended), the advice walks the tank back
            // along the navmesh and the target follows; escapes and cone advice above keep their precedence.
            Unit* boss=actor->GetMap()->GetUnit(plan.boss);int movementRule=-1;
            for(size_t i=0;i<plan.definition.rules.size()&&movementRule<0;++i)
            {auto const& rule=plan.definition.rules[i];if(rule.action==Action::Spread||rule.action==Action::AvoidPoints||rule.action==Action::AvoidCones||rule.action==Action::Isolate)movementRule=int(i);}
            Point const station=plan.definition.tank;
            Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
            if(boss&&boss->IsAlive()&&boss->GetVictim()==actor&&movementRule>=0&&std::hypot(station.x-at.x,station.y-at.y)>4.f)
            {
                PathInfo path(actor);path.calculate(station.x,station.y,station.z);
                if(!(path.getPathType()&(PATHFIND_NOPATH|PATHFIND_INCOMPLETE))&&!path.getPath().empty())
                {
                    Point ahead=at;bool chosen=false;
                    for(auto const& node:path.getPath())
                    {
                        float dx=node.x-at.x,dy=node.y-at.y;
                        if(std::sqrt(dx*dx+dy*dy)>=2.5f){ahead={node.x,node.y,node.z};chosen=true;break;}
                    }
                    if(!chosen){auto const& last=path.getPath().back();ahead={last.x,last.y,last.z};}
                    if(InRoom(plan,ahead)){advice.waypoint=ahead;advice.rule=uint16(movementRule);advice.state=2;return advice;}
                }
            }
        }
        if(row.manual)
        {
            Point goal;float seconds=0;
            if(FormationRoute(plan,row,actor,goal,advice.waypoint,seconds))
                    {
                        for(size_t i=0;i<plan.definition.rules.size();++i)
                        {auto const& rule=plan.definition.rules[i];if(rule.action==Action::Spread&&rule.trigger==Trigger::Always&&rule.target==Target::Self&&
                            (rule.roles&(1u<<row.role))&&(!rule.phase||rule.phase==plan.phase)&&(!rule.toggle||(plan.flags&rule.toggle))){advice.rule=uint16(i);break;}}
                        advice.state=2;return advice;
                    }
        }
        for(size_t ruleIndex=0;ruleIndex<plan.definition.rules.size();ruleIndex++)
        {
            Rule const& candidate=plan.definition.rules[ruleIndex];
            if(candidate.action==Action::AvoidCones&&(candidate.roles&(1u<<row.role))&&
                (!candidate.phase||candidate.phase==plan.phase)&&(!candidate.toggle||(plan.flags&candidate.toggle)))
            {
                Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
                if(ConeStationAllowed(plan,row,actor,at))continue;
                advice.rule=uint16(ruleIndex);advice.waypoint=at;
                if(!row.manual){advice.state=row.hasMove&&row.duty==6?2:3;if(advice.state==2)advice.waypoint=row.lastMove;return advice;}
                Point goal;float seconds=0;
                advice.state=FindClearRoute(plan,actor,[&](Point p){return ConeStationAllowed(plan,row,actor,p);},goal,advice.waypoint,seconds)?2:3;
                return advice;
            }
            for(auto const& event:plan.events)
            {
                if(event.rule!=ruleIndex)continue;
                Rule const& rule=plan.definition.rules[event.rule];
                if(rule.action!=Action::AvoidPoints||!(rule.roles&(1u<<row.role))||
                    (rule.phase&&rule.phase!=plan.phase)||(rule.toggle&&!(plan.flags&rule.toggle)))continue;
                advice.rule=uint16(event.rule);advice.impact=event.impact;
                auto lane=HazardPoints(plan,rule);
                Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
                advice.waypoint=at;
                if(lane.size()!=rule.points.size()){advice.state=3;return advice;}
                if(Clearance(at,lane)>rule.radius){advice.state=1;return advice;}
                if(!row.manual)
                {
                    advice.state=row.hasMove&&row.duty==6?2:3;
                    if(advice.state==2)advice.waypoint=row.lastMove;
                    return advice;
                }
                Point goal;float seconds=0;
                if(!EscapeRoute(plan,actor,lane,rule.radius,goal,advice.waypoint,seconds)){advice.state=3;return advice;}
                advice.state=event.impact&&seconds+.25f<=event.impact/1000.f?2:4;
                return advice;
            }
        }
        // Ordinary station acquisition comes after all hazard escape decisions.
        // The reserved rule index denotes a generic positional hold, never an
        // encounter rule or permission to override an active hazard route.
        if(row.manual&&row.role==1)
        {
            Unit* boss=actor->GetMap()->GetUnit(plan.boss);
            bool melee=false;for(auto const& phase:plan.definition.phases)if(phase.id==plan.phase)melee=MeleeAllowed(phase,row.role);
            if(melee&&boss&&boss->IsAlive()&&boss->GetVictim()==actor&&TankHoldAllowed(plan,actor,boss))
            {
                Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
                Point goal=TankFacingStation(plan,actor,boss,row.ground);
                Point step=CoveredTankStep(plan,actor,goal);
                float wanted=std::hypot(goal.x-at.x,goal.y-at.y),allowed=std::hypot(step.x-at.x,step.y-at.y);
                if(wanted>.2f&&allowed<=.2f){advice.state=1;advice.rule=65535;advice.waypoint=at;}
            }
        }
        return advice;
    }
    void Hold(Player* actor,Assignment& row)
    {
        RememberInterruptedHeal(actor,row);
        actor->InterruptNonMeleeSpells(false);actor->AttackStop();actor->StopMoving();actor->GetMotionMaster()->MoveIdle();
        if(Pet* pet=actor->GetPet())pet->AttackStop();row.hasMove=false;
    }
    bool SupportCasterReserved(Plan const& plan,Player* actor,Rule const& action)
    {
        for(auto const& reserve:plan.definition.rules)
        {
            if(!reserve.reserveCasters||reserve.priority<=action.priority||reserve.spell!=action.spell||
                (reserve.phase&&reserve.phase!=plan.phase)||(reserve.toggle&&!(plan.flags&reserve.toggle)))continue;
            std::vector<uint64> eligible;
            for(auto const& candidate:plan.rows)
                if(!candidate.manual&&!candidate.yielded&&(reserve.roles&(1u<<candidate.role)))
                    if(Player* p=Member(plan,candidate.guid))if(p->IsAlive()&&p->HasSpell(reserve.spell))
                        eligible.push_back(Raw(candidate.guid));
            uint64 guid=Raw(actor->GetObjectGuid());
            if(std::find(eligible.begin(),eligible.end(),guid)==eligible.end())continue;
            // Stable ownership through cooldowns, casting and crowd control.
            // Readiness must not rotate a spent support slot onto a new caster
            // and let every lower-priority buff consume the entire pool.
            size_t earlier=std::count_if(eligible.begin(),eligible.end(),[&](uint64 id){return id<guid;});
            if(earlier<reserve.reserveCasters)return true;
        }
        return false;
    }
    bool InstantControlProtection(SpellEntry const* spell,Player* actor)
    {
        if(!spell||spell->GetCastTime(actor)!=0)return false;
        for(uint8 effect=0;effect<3;++effect)
            if(spell->Effect[effect]==SPELL_EFFECT_APPLY_AURA&&spell->EffectApplyAuraName[effect]==SPELL_AURA_MECHANIC_IMMUNITY)
                switch(spell->EffectMiscValue[effect])
                {
                    case MECHANIC_CHARM:case MECHANIC_DISORIENTED:case MECHANIC_FEAR:case MECHANIC_ROOT:
                    case MECHANIC_SILENCE:case MECHANIC_STUN:case MECHANIC_SLEEP:case MECHANIC_POLYMORPH:
                        return true;
                    default:break;
                }
        return false;
    }
    bool AuthoredSupportCast(AiBotAI* ai,Plan& plan,Assignment& row,Rule const& rule,Unit* target)
    {
        Player* actor=ai->GetBotPlayer();auto spell=sSpellMgr.GetSpellEntry(rule.spell);
        if(!target||!spell||(rule.missingAura&&target->HasAura(rule.spell)))return false;
        for(auto const& claim:plan.claims)
            if(claim.kind==18&&claim.target==target->GetObjectGuid()&&claim.spell==rule.spell)return false;
        auto available=[&](Player* p)
        {
            return p&&p->IsAlive()&&!SupportCasterReserved(plan,p,rule)&&p->HasSpell(rule.spell)&&p->IsSpellReady(rule.spell)&&!p->HasGCD(spell)&&
                !p->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)&&!p->IsNonMeleeSpellCasted(false,false,true)&&
                (spell->powerType!=POWER_MANA||Spell::CalculatePowerCost(spell,p,nullptr,nullptr,false)<=p->GetPower(POWER_MANA));
        };
        if(!available(actor))return false;
        if(rule.target!=Target::Self)
        {
            Player* selected=nullptr;float best=100000;
            for(auto const& candidate:plan.rows)
                if(!candidate.manual&&!candidate.yielded&&(rule.roles&(1u<<candidate.role)))
                    if(Player* p=Member(plan,candidate.guid))if(available(p))
                    {
                        float score=p->GetDistance(target)+(p->IsWithinLOSInMap(target)?0.f:1000.f);
                        if(score<best){best=score;selected=p;}
                    }
            if(selected!=actor)return false;
        }
        auto cast=[&]()
        {
            if(!ai->CommanderRaidCast(target,rule.spell))return false;
            plan.claims.push_back({row.guid,target->GetObjectGuid(),18,std::max(750u,spell->GetCastTime(actor)+250),0,rule.spell});
            row.duty=4;row.target=target->GetObjectGuid();return true;
        };
        // An authored instant control immunity closes a protection gap before
        // starting another emergency heal. Normal casting, GCD and target checks
        // still decide success; never interrupt a committed heal for routine buffs.
        if(plan.enteredCombat&&rule.missingAura&&InstantControlProtection(spell,actor))
        {
            if(cast())return true;
            // Only the selected, ready provider takes this route. Otherwise
            // repeated emergency raid heals can indefinitely postpone closing
            // an observed control-protection gap outside the shorter buff range.
            // Committed casts, ordinary cooldowns and complete safe paths still win.
            if(rule.target!=Target::Self&&RecoverSpellRange(ai,plan,row,target,rule.spell,"support",false))return true;
        }
        if(plan.enteredCombat&&row.role==3&&Heal(ai,plan,row,true))return true;
        if(cast())return true;
        // Arming remains stationary. In combat, critical healing outranks travel
        // for a buff, and a blocked support route must not suppress normal healing.
        if(!plan.enteredCombat||rule.target==Target::Self)return false;
        if(row.role==3&&Heal(ai,plan,row,true))return true;
        if(RecoverSpellRange(ai,plan,row,target,rule.spell,"support",false))return true;
        return cast(); // A safe in-range moving caster may just have stopped.
    }
    bool ExecuteRule(AiBotAI* ai,Plan& plan,Assignment& row,Rule const& rule,Unit* target,uint32 /*remaining*/=0)
    {
        Player* actor=ai->GetBotPlayer();
        if(rule.action==Action::AvoidCones)
        {
            Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
            if(ConeStationAllowed(plan,row,actor,at))
            {
                if(row.hasMove&&row.hazardMove&&row.duty==6){actor->StopMoving();actor->GetMotionMaster()->MoveIdle();row.hasMove=false;}
                return false; // Safe healers and damage roles may keep doing their jobs.
            }
            if(row.hasMove&&row.duty==6&&ConeStationAllowed(plan,row,actor,row.lastMove))return Move(ai,plan,row,row.lastMove,6,true);
            Point goal,waypoint;float seconds=0;
            if(!FindClearRoute(plan,actor,[&](Point p){return ConeStationAllowed(plan,row,actor,p);},goal,waypoint,seconds))
            {Hold(actor,row);row.duty=12;return true;}
            return Move(ai,plan,row,goal,6,true);
        }
        if(rule.action==Action::Cast)
        {
            return AuthoredSupportCast(ai,plan,row,rule,target);
        }
        if(rule.action==Action::StopDamage) {Hold(actor,row);row.duty=8;return true;}
        if(rule.action==Action::Move) {if(!Move(ai,plan,row,rule.station,1))Hold(actor,row);return true;}
        if(rule.action==Action::Spread || rule.action==Action::Stack)
        {
            if(!target)return false;
            if(target==actor)
            {
                if(rule.action==Action::Spread)return false; // Keep casting; ordinary movement remains constrained.
                Hold(actor,row);row.duty=7;return true;
            }
            float dx=actor->GetPositionX()-target->GetPositionX(),dy=actor->GetPositionY()-target->GetPositionY();
            float length=std::sqrt(dx*dx+dy*dy);
            if(rule.action==Action::Spread && length>=rule.radius)return false;
            if(rule.action==Action::Stack && length<=rule.radius) {Hold(actor,row);row.duty=7;return true;}
            if(length<.1f) {float angle=float(Raw(actor->GetObjectGuid())%16)*.392699f;dx=std::cos(angle);dy=std::sin(angle);length=1;}
            float distance=rule.action==Action::Spread?rule.radius+2:rule.radius*.5f;
            Point goal{target->GetPositionX()+dx/length*distance,target->GetPositionY()+dy/length*distance,actor->GetPositionZ()};
            if(InRoom(plan,goal))Move(ai,plan,row,goal,7,true);else {Hold(actor,row);row.duty=12;}
            return true;
        }
        auto lane=HazardPoints(plan,rule);
        if(lane.size()!=rule.points.size()) {Hold(actor,row);row.duty=12;return true;}
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        if(Clearance(at,lane)>rule.radius)return false; // Safe work may resume; check remaining active rules.
        if(row.hasMove&&row.duty==6&&Clearance(row.lastMove,lane)>rule.radius+1)return Move(ai,plan,row,row.lastMove,6,true);
        Point goal,waypoint;float seconds=0;
        // Keep escaping even if the warning budget is already exhausted. Holding
        // in a damage footprint turns a late route into repeated guaranteed hits.
        if(!EscapeRoute(plan,actor,lane,rule.radius,goal,waypoint,seconds))
        {Hold(actor,row);row.duty=12;return true;}
        return Move(ai,plan,row,goal,6,true);
    }
    Player* TankHealer(Plan const& plan,Player* tank,uint32 protectionSpell)
    {
        if(!tank)return nullptr;
        Player* selected=nullptr;uint32 fastest=std::numeric_limits<uint32>::max();bool selectedProtects=false;
        for(auto const& row:plan.rows)if(row.role==3&&row.primary==tank->GetObjectGuid()&&!row.yielded)
            if(Player* healer=Member(plan,row.guid))if(healer->IsAlive())
                if(auto spell=sSpellMgr.GetSpellEntry(row.heal))
                {
                    if(spell->powerType==POWER_MANA&&Spell::CalculatePowerCost(spell,healer,nullptr,nullptr,false)>healer->GetPower(POWER_MANA))continue;
                    uint32 castTime=spell->GetCastTime(healer);
                    // Protect an assigned healer who can maintain this same
                    // protection on the tank; prefer fast healing within that pool.
                    // Readiness/movement must not rotate recipients every tick.
                    bool protects=InstantControlProtection(sSpellMgr.GetSpellEntry(protectionSpell),healer)&&healer->HasSpell(protectionSpell);
                    if(!selected||(protects&&!selectedProtects)||(protects==selectedProtects&&
                        (castTime<fastest||(castTime==fastest&&Raw(row.guid)<Raw(selected->GetObjectGuid())))))
                    {selected=healer;fastest=castTime;selectedProtects=protects;}
                }
        return selected;
    }
    bool HasRemovableControl(Player* patient,SpellEntry const* dispel)
    {
        uint32 mask=0;
        for(uint8 effect=0;effect<3;++effect)if(dispel->Effect[effect]==SPELL_EFFECT_DISPEL)
            mask|=Spells::GetDispellMask(DispelType(dispel->EffectMiscValue[effect]));
        for(auto const& pair:patient->GetSpellAuraHolderMap())
        {
            auto holder=pair.second;auto aura=holder->GetSpellProto();
            if(holder->IsPositive()||aura->Dispel>=32||!(mask&(1u<<aura->Dispel)))continue;
            for(uint8 effect=0;effect<3;++effect)
                if(holder->GetAuraByEffectIndex(SpellEffectIndex(effect)))
                    switch(aura->EffectApplyAuraName[effect])
                    {
                        case SPELL_AURA_MOD_FEAR:case SPELL_AURA_MOD_CONFUSE:case SPELL_AURA_MOD_STUN:
                        case SPELL_AURA_MOD_ROOT:case SPELL_AURA_MOD_SILENCE:case SPELL_AURA_MOD_PACIFY_SILENCE:
                            return true;
                        default:break;
                    }
        }
        return false;
    }
    bool UrgentControlDispel(AiBotAI* ai,Plan& plan,Assignment& row)
    {
        Player* actor=ai->GetBotPlayer();auto spell=sSpellMgr.GetSpellEntry(row.dispel);
        if(!spell||!actor->HasSpell(row.dispel)||!actor->IsSpellReady(row.dispel)||
            actor->IsNonMeleeSpellCasted(false,false,true)||
            (spell->powerType==POWER_MANA&&Spell::CalculatePowerCost(spell,actor,nullptr,nullptr,false)>actor->GetPower(POWER_MANA)))return false;
        // The caster's own escape remains first. Do not chase an ally or interrupt
        // a committed heal; rank reachable control removals before routine work.
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        if(!ConeStationAllowed(plan,row,actor,at))return false;
        struct Candidate{Player* patient;float score;};std::vector<Candidate> ranked;
        for(auto const& candidate:plan.rows)
        {
            Player* patient=Member(plan,candidate.guid);
            if(!patient||!patient->IsAlive()||candidate.yielded||!patient->IsFriendlyTo(actor)||
                Claimed(plan,candidate.guid,14)||!HasRemovableControl(patient,spell)||
                !ai->IsValidDispelTarget(patient,spell)||!actor->IsWithinLOSInMap(patient)||
                !actor->IsWithinDistInMap(patient,Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex))))continue;
            bool active=false;uint16 rule=0;uint32 impact=0;
            auto hazards=PositionHazards(plan,candidate,patient,active,rule,impact);
            Point point{patient->GetPositionX(),patient->GetPositionY(),patient->GetPositionZ()};
            bool danger=!ConeStationAllowed(plan,candidate,patient,point)||(active&&!PositionClear(plan,candidate,patient,point,hazards));
            float score=(danger?1000.f:0.f)+(candidate.role==3?200.f:candidate.role<=2?150.f:0.f)-actor->GetDistance(patient);
            ranked.push_back({patient,score});
        }
        std::sort(ranked.begin(),ranked.end(),[](Candidate const& a,Candidate const& b)
            {return a.score!=b.score?a.score>b.score:Raw(a.patient->GetObjectGuid())<Raw(b.patient->GetObjectGuid());});
        for(auto const& candidate:ranked)
        {
            if(actor->IsMoving()){actor->StopMoving();actor->GetMotionMaster()->MoveIdle();row.hasMove=false;}
            if(!SupportCast(ai,plan,row,candidate.patient,row.dispel,14))continue;
            sLog.Out(LOG_BASIC,LOG_LVL_MINIMAL,"[SUI][raid-control-dispel] actor=%u target=%u spell=%u score=%.1f",
                actor->GetGUIDLow(),candidate.patient->GetGUIDLow(),row.dispel,candidate.score);
            return true;
        }
        return false;
    }
    bool RunRules(AiBotAI* ai,Plan& plan,Assignment& row,Unit* boss,Player* tank)
    {
        Player* actor=ai->GetBotPlayer();
        bool active=false;uint16 firstRule=0;uint32 impact=0;
        auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);
        Point at{actor->GetPositionX(),actor->GetPositionY(),actor->GetPositionZ()};
        if(!active)row.escapeGoalReady=false;
        if(active&&(!PositionClear(plan,row,actor,at,hazards)||EscapePending(row,at,plan.phase)))
        {
            Point goal,waypoint;float seconds=0;
            if(!CombinedEscape(plan,row,actor,hazards,goal,waypoint,seconds))
            {Hold(actor,row);row.duty=12;return true;}
            if(Move(ai,plan,row,goal,6,true))return true;
        }
        if(UrgentControlDispel(ai,plan,row))return true;
        for(size_t i=0;i<plan.definition.rules.size();i++)
        {
            Rule const& rule=plan.definition.rules[i];
            if(!(rule.roles&(1u<<row.role)) || (rule.phase&&rule.phase!=plan.phase) || (rule.toggle&&!(plan.flags&rule.toggle)))continue;
            if(rule.action==Action::Spread&&rule.trigger==Trigger::Always&&rule.target==Target::Self)continue;
            // These active constraints were solved together above, never sequentially.
            if(active&&(rule.action==Action::AvoidPoints||((rule.action==Action::Spread||rule.action==Action::Isolate)&&rule.target==Target::Marked)))continue;
            std::vector<std::pair<ObjectGuid,uint32>> targets;
            if(rule.trigger==Trigger::CastStart||rule.trigger==Trigger::CastGo)
            {for(auto const& event:plan.events)if(event.rule==i)targets.push_back({event.target,event.remaining});}
            else
            {
                bool active=rule.trigger==Trigger::Always;
                Unit* subject=rule.trigger==Trigger::SelfAura?actor:boss;
                if(!active)for(uint32 spell:rule.spells)if(subject->HasAura(spell)) {active=true;break;}
                if(active)targets.push_back({ObjectGuid(),0});
            }
            for(auto const& observed:targets)
            {
                ObjectGuid marked=observed.first;
                Unit* target=rule.target==Target::Self?actor:rule.target==Target::Boss?boss:rule.target==Target::Tank?tank:rule.target==Target::TankHealer?TankHealer(plan,tank,rule.spell):actor->GetMap()->GetUnit(marked);
                if(ExecuteRule(ai,plan,row,rule,target,observed.second))return true;
            }
        }
        return false;
    }

}

void WorldSession::HandleSuiCommanderRaidOpcode(WorldPackets::SuiControl::CommanderRaid const& packet)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    Player* owner = GetPlayer();
    sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-request] owner=%u op=%u request=%u revision=%u",owner?owner->GetGUIDLow():0u,unsigned(packet.operation),packet.requestId,packet.revision);
    if (!owner || GetBot()) { Reply(this,packet.requestId,2,nullptr); return; }
    auto it = plans.find(Raw(owner->GetObjectGuid()));
    Plan* old = it == plans.end() ? nullptr : &it->second;
    auto reject = [&](uint8 result) { Reply(this,packet.requestId,result,old); };
    if (!packet.exactSize || packet.version != 4 || packet.operation > 4 || packet.flags > 7) { reject(1); return; }
    if (packet.operation == 0) { reject(0); return; }
    if (!owner->GetGroup() || owner->GetGroup()->GetLeaderGuid() != owner->GetObjectGuid() || !SuiPossess::IsFreeViewUp(owner)) { reject(2); return; }
    if (packet.revision != (old ? old->revision : 0)) { reject(6); return; }
    if (owner->IsSuiTacticallyFrozen()) { reject(7); return; }
    if (packet.operation == 4)
    {
        if (old) { for (auto& row : old->rows) Stop(row,*old); old->rows.clear(); old->events.clear(); old->state=0; ++old->revision; RefreshWatches(); }
        reject(0); return;
    }
    if (packet.operation == 3)
    {
        if (!old || old->rows.empty()) { reject(8); return; }
        for (auto& row : old->rows) Stop(row,*old);
        old->state=3; old->events.clear(); ++old->revision; RefreshWatches(); reject(0); return;
    }
    if (owner->GetMapId() != packet.mapId) { reject(3); return; }
    if (packet.operation == 2)
    {
        if (!old || old->rows.empty() || old->instance != owner->GetInstanceId() || old->definition.map!=packet.mapId || old->definition.boss!=packet.bossEntry) { reject(8); return; }
        for (auto& row : old->rows)
        {
            Player* actor=Member(*old,row.guid);
            if (!actor || !actor->IsAlive() || (row.manual ? actor!=owner : !Commandable(owner,actor))) { reject(4); return; }
            if (actor->IsSuiTacticallyFrozen()) { reject(7); return; }
        }
        for (auto& row : old->rows)
        {
            if(row.manual)continue;
            row.yielded=false;row.hasMove=false;row.flankReady=false;
            if(Player* p=Member(*old,row.guid)) if(auto ai=dynamic_cast<AiBotAI*>(p->AI())) ai->SuiAbandonJourney();
        }
        old->tankContacts.clear();old->claims.clear();old->objectives.clear();old->addObjectives.clear();old->enteredCombat=false;old->boss.Clear();old->phase=0;old->events.clear();old->state=2; ++old->revision; RefreshWatches(); reject(0); return;
    }
    if (old && old->state == 2) { reject(10); return; }
    if (packet.records.empty()) { reject(1); return; }
    // Limit live command plans and validate all data before changing ownership.
    if(!old && plans.size()>=16) {reject(1);return;}
    Plan next;
    if(!SuiEncounter::Parse(packet.definition,packet.mapId,packet.bossEntry,next.definition)) {reject(1);return;}
    for(auto const& rule:next.definition.rules)
    {
        if(rule.spell && !sSpellMgr.GetSpellEntry(rule.spell)) {reject(5);return;}
        for(uint32 spell:rule.spells)if(!sSpellMgr.GetSpellEntry(spell)) {reject(5);return;}
        if(rule.action==Action::AvoidCones)for(uint32 id:rule.spells)
        {
            auto spell=sSpellMgr.GetSpellEntry(id);bool cone=false;
            if(spell)for(size_t i=0;i<3;i++)if(spell->Effect[i]&&
                (spell->EffectImplicitTargetA[i]==24||spell->EffectImplicitTargetB[i]==24||spell->EffectImplicitTargetA[i]==54||spell->EffectImplicitTargetB[i]==54)&&
                Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))>0)cone=true;
            float const angle=sSpellMgr.GetSpellCone(id);
            if(!cone||!std::isfinite(angle)||angle==0||std::fabs(angle)>6.28318531f){reject(1);return;}
        }
        for(uint32 spell:rule.points)
        {
            auto point=sSpellMgr.GetSpellTargetPosition(spell);
            auto entry=sSpellMgr.GetSpellEntry(spell);
            if(!point||point->mapId!=packet.mapId||!entry) {reject(1);return;}
            // Authored avoidance must cover the server's actual effect footprint.
            if(rule.action==Action::AvoidPoints)for(uint8 effect=0;effect<3;effect++)
                if(entry->Effect[effect]&&entry->EffectRadiusIndex[effect])
                    if(auto radius=sSpellRadiusStore.LookupEntry(entry->EffectRadiusIndex[effect]))
                        if(Spells::GetSpellRadius(radius)>rule.radius) {reject(1);return;}
        }
    }
    for(auto const& phase:next.definition.phases)next.damageSchools[phase.id]=HazardDamageSchools(next.definition,phase.id);
    next.owner=owner->GetObjectGuid(); next.instance=owner->GetInstanceId();
    next.revision=(old ? old->revision : 0)+1; next.state=1; next.flags=packet.flags;
    std::set<uint64> unique; uint32 tanks=0, manual=0, adds[8]={}, healers[8]={};
    std::set<uint32> tankObjectives;
    for (auto const& input : packet.records)
    {
        Assignment row; row.guid=input.guid;row.role=input.role;row.team=input.team;row.manual=input.manual!=0;row.primary=input.primary;
        row.focus=input.focus;row.interrupt=input.interrupt;row.dispel=input.dispel;row.taunt=input.taunt;row.defensive=input.defensive;
        if(input.manual>1 || (row.manual!=(row.guid==owner->GetObjectGuid()))) {reject(1);return;}
        if(row.manual)++manual;
        row.ground={input.gx,input.gy,input.gz};row.air={input.ax,input.ay,input.az};row.heal=input.heal;row.damage=input.damage;
        // Add tanks stand with the puller when the encounter has adds: a linked pull lands every add on the tank
        // anchor at once, and a taunt from 13-20 yd behind arrives after the puller is dead (Garr, 2026-09-13).
        if(row.role==2&&!row.manual&&!next.definition.adds.empty())
        {
            Point const anchor=next.definition.tank;float const side=(adds[row.team&7]%2?1.f:-1.f)*3.5f;
            Point beside{anchor.x+side,anchor.y+3.5f*(1+adds[row.team&7]/2),anchor.z};
            if(InRoom(next,beside))row.ground=beside;
        }
        if (!unique.insert(Raw(row.guid)).second || row.role<1 || row.role>5 || row.team>=next.definition.teams.size() || !InRoom(next,row.ground) || !InRoom(next,row.air)) { reject(1); return; }
        Player* actor=Member(next,row.guid);
        if (!actor || !actor->IsAlive() || (row.manual ? actor!=owner : !Commandable(owner,actor))) { reject(4); return; }
        if (actor->IsSuiTacticallyFrozen()) { reject(7); return; }
        if (!row.manual && ((row.role==3 && !Learned(actor,row.heal,true)) || (row.role==5 && !Learned(actor,row.damage,false)) ||
            !SupportLearned(actor,row.interrupt,13)||!SupportLearned(actor,row.dispel,14)||!SupportLearned(actor,row.taunt,2)||!SupportLearned(actor,row.defensive,4))) { reject(5); return; }
        if(!row.manual&&row.role==5)
        {auto damage=sSpellMgr.GetSpellEntry(row.damage);if(damage&&(damage->GetSpellSchoolMask()&next.definition.immuneSchools)) {reject(5);return;}}
        if (row.role==1) {++tanks;tankObjectives.insert(row.focus);}
        if (row.role==2) ++adds[row.team];
        if (row.role==3) ++healers[row.team];
        next.rows.push_back(row);
    }
    if(manual!=1||tanks!=next.definition.objectives.size()||tankObjectives.size()!=tanks) {reject(9);return;}
    for(uint32 objective:next.definition.objectives)if(!tankObjectives.count(objective)) {reject(9);return;}
    for(auto const& row:next.rows)if(row.role==3&&!unique.count(Raw(row.primary))) {reject(9);return;}
    uint32 totalHealers=0;for(uint32 count:healers)totalHealers+=count;
    if(totalHealers<next.definition.healerCount){reject(9);return;}
    for(size_t team=0;team<next.definition.teams.size();team++)
        if(adds[team]<next.definition.addTanks||healers[team]<next.definition.healers) {reject(9);return;}
    // One actor cannot be leased to two commanders, including an older paused plan.
    for(auto const& pair:plans)if(pair.first!=Raw(next.owner)&&pair.second.state!=0)
        for(auto const& row:pair.second.rows)if(unique.count(Raw(row.guid))) {reject(4);return;}
    plans[Raw(next.owner)]=std::move(next);
    Reply(this,packet.requestId,0,&plans[Raw(owner->GetObjectGuid())]);
}

bool SuiCommanderRaid::Owns(Unit* actor)
{
    if(!actor)return false;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for(auto const& pair:plans)if(pair.second.state==2)
        for(auto const& row:pair.second.rows)if(row.guid==actor->GetObjectGuid()&&!row.manual&&!row.yielded)return true;
    return false;
}
bool SuiCommanderRaid::Watches(uint32 entry)
{
    for(auto const& watched:watchedEntries)if(watched.load()==entry)return entry!=0;
    return false;
}
void SuiCommanderRaid::ObserveUnit(Unit* boss,uint32 diff)
{
    if(!boss)return;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for(auto& pair:plans)
    {
        Plan& plan=pair.second;
        if(plan.state!=2||plan.definition.map!=boss->GetMapId()||plan.instance!=boss->GetInstanceId())continue;
        bool primary=std::find(plan.definition.objectives.begin(),plan.definition.objectives.end(),boss->GetEntry())!=plan.definition.objectives.end();
        bool required=std::any_of(plan.definition.requiredAdds.begin(),plan.definition.requiredAdds.end(),[&](AddRequirement const& r){return r.entry==boss->GetEntry();});
        if(required)
        {
            auto found=plan.addObjectives.find(boss->GetObjectGuid());
            if(found!=plan.addObjectives.end()||(boss->IsInCombat()&&InRoom(plan,{boss->GetPositionX(),boss->GetPositionY(),boss->GetPositionZ()})&&plan.addObjectives.size()<256))
            {
                auto& add=plan.addObjectives[boss->GetObjectGuid()];bool dead=!boss->IsAlive();
                if(!add.seen||add.dead!=dead)sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-objective] owner=%llu entry=%u guid=%llu dead=%u",static_cast<unsigned long long>(Raw(plan.owner)),boss->GetEntry(),static_cast<unsigned long long>(Raw(boss->GetObjectGuid())),dead?1:0);
                add.guid=boss->GetObjectGuid();add.entry=boss->GetEntry();add.maxHealth=boss->GetMaxHealth();add.seen=true;add.dead=dead;
            }
        }
        if(primary)
        {
            auto& objective=plan.objectives[boss->GetEntry()];
            if(objective.guid!=boss->GetObjectGuid())
            {
                // An objective entry can have many spawns on the map. Binding happens only
                // inside the surveyed room and, until the fight starts, prefers the candidate
                // nearest the tank anchor; a bound unit already fighting is never replaced.
                // The room gate is a binding rule only: an already bound unit is observed
                // wherever it goes (airborne phases leave the surveyed volume).
                Point const here{boss->GetPositionX(),boss->GetPositionY(),boss->GetPositionZ()};
                if(!InRoom(plan,here))continue;
                // A dead spawn of the objective entry (an earlier group's body awaiting its
                // database respawn) is never a new binding: it cannot be pulled, and observing it
                // must not move a binding the client has already sealed.
                if(!boss->IsAlive())continue;
                if(!objective.guid.IsEmpty())
                {
                    Unit* bound=boss->GetMap()->GetUnit(objective.guid);
                    bool const boundFighting=bound&&bound->IsAlive()&&bound->IsInCombat();
                    if(plan.enteredCombat||boundFighting)continue;
                    bool const boundInRoom=bound&&InRoom(plan,{bound->GetPositionX(),bound->GetPositionY(),bound->GetPositionZ()});
                    // A living bound unit inside the room keeps the binding: the client seals it and walks
                    // the tank to it, and the nearest member was chosen at binding time (below), so a
                    // distance re-evaluation could only flip it on wander after the seal.
                    if(bound&&bound->IsAlive()&&boundInRoom)continue;
                }
                // Spawns are observed one at a time, so a binding decided per observed unit can land
                // on a member that is not the nearest of its group. Any new binding therefore goes to
                // the nearest live in-room spawn of the entry to the tank anchor, evaluated over the
                // whole grid list at once: the client walks the tank to pull range of the bound unit,
                // so a farther packmate costs the tank a deep walk away from its healers.
                Point const anchor=plan.definition.tank;
                Unit* target=boss;float targetDistance=std::hypot(here.x-anchor.x,here.y-anchor.y);
                {
                    std::list<Creature*> spawns;boss->GetCreatureListWithEntryInGrid(spawns,boss->GetEntry(),kBindScanRange);
                    for(Creature* spawn:spawns)
                    {
                        if(!spawn->IsAlive()||!InRoom(plan,{spawn->GetPositionX(),spawn->GetPositionY(),spawn->GetPositionZ()}))continue;
                        float const distance=std::hypot(spawn->GetPositionX()-anchor.x,spawn->GetPositionY()-anchor.y);
                        if(distance<targetDistance){target=spawn;targetDistance=distance;}
                    }
                }
                if(!objective.guid.IsEmpty())
                    sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-objective-bind] owner=%llu entry=%u from=%llu to=%llu toDistance=%.1f",
                        static_cast<unsigned long long>(Raw(plan.owner)),boss->GetEntry(),static_cast<unsigned long long>(Raw(objective.guid)),
                        static_cast<unsigned long long>(Raw(target->GetObjectGuid())),targetDistance);
                boss=target;
            }
            objective.guid=boss->GetObjectGuid();objective.entry=boss->GetEntry();objective.maxHealth=boss->GetMaxHealth();objective.seen=true;objective.dead=!boss->IsAlive();
            if(boss->GetEntry()==plan.definition.boss)plan.boss=boss->GetObjectGuid();
        }
        if(!primary&&!required)continue;
        ObjectGuid clock=plan.boss;
        bool primaryClock=false;
        for(uint32 entry:plan.definition.objectives)
        {auto found=plan.objectives.find(entry);if(found!=plan.objectives.end()&&found->second.seen&&!found->second.dead){clock=found->second.guid;primaryClock=true;break;}}
        if(!primaryClock)for(auto const& add:plan.addObjectives)if(add.second.seen&&!add.second.dead){clock=add.first;break;}
        if(boss->GetObjectGuid()!=clock)continue;
        for(auto it=plan.claims.begin();it!=plan.claims.end();)
        {
            Player* caster=Member(plan,it->actor);
            if(it->remaining<=diff||!caster||!caster->IsAlive()||((it->kind==4||it->kind==18)&&it->remaining>500&&!caster->IsNonMeleeSpellCasted(false,false,true)))it=plan.claims.erase(it);
            else {it->remaining-=diff;++it;}
        }
        for(auto const& phase:plan.definition.phases)
            if(boss->GetHealthPercent()>=phase.healthMin&&boss->GetHealthPercent()<=phase.healthMax&&
                (phase.airborne==-1||(phase.airborne==1)==boss->IsLevitating())&&(!phase.aura||boss->HasAura(phase.aura)))
            {plan.phase=phase.id;break;}
        for(auto it=plan.events.begin();it!=plan.events.end();)
        {
            Rule const& rule=plan.definition.rules[it->rule];
            Unit* target=boss->GetMap()->GetUnit(it->target);
            // MissingAura on an isolation warning is its observed end condition.
            // The independent member-aura rule owns post-impact splash protection.
            bool resolved=rule.action==Action::Isolate&&rule.missingAura&&rule.spell&&target&&target->HasAura(rule.spell);
            if(it->remaining<=diff||resolved)it=plan.events.erase(it);
            else {it->remaining-=diff;it->impact=it->impact>diff?it->impact-diff:0;++it;}
        }
        if(!boss->IsInCombat())plan.events.clear();
        // The elected objective clock runs manual rows once; bot rows keep their AI cadence.
        if(diff&&boss->IsInCombat())for(auto& row:plan.rows)if(row.manual&&!row.yielded)
            if(Player* actor=Member(plan,row.guid))TryCarriedPotion(actor,row,diff,plan.damageSchools[plan.phase]);
    }
}
void SuiCommanderRaid::ObserveCast(Unit* boss,uint32 spell,Unit* target,bool start,uint32 castTime)
{
    if(!boss||!Watches(boss->GetEntry()))return;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for(auto& pair:plans)
    {
        Plan& plan=pair.second;
        if(plan.state!=2||plan.definition.map!=boss->GetMapId()||plan.instance!=boss->GetInstanceId()||(std::find(plan.definition.objectives.begin(),plan.definition.objectives.end(),boss->GetEntry())==plan.definition.objectives.end()&&!plan.addObjectives.count(boss->GetObjectGuid())))continue;
        auto seen=plan.objectives.find(boss->GetEntry());
        if(seen!=plan.objectives.end()&&!seen->second.guid.IsEmpty()&&seen->second.guid!=boss->GetObjectGuid())continue;
        if(!start)
        {
            // A caster-centred burst the unit casts (a scripted spell outside its list and slots) teaches its entry's
            // stand-off radius to this plan from the first cast on.
            if(float burst=InstantBurstSpellRadius(sSpellMgr.GetSpellEntry(spell)))
            {float& known=plan.burstFootprints[boss->GetEntry()];known=std::max(known,burst);}
            // A pulse of area damage the objective casts on itself while carrying a self periodic aura teaches that
            // aura's footprint to this plan (a scripted pulse whose loaded periodic child is a dummy effect).
            float radius=0;SpellEntry const* pulse=sSpellMgr.GetSpellEntry(spell);
            if(pulse&&(!target||target==boss)&&HarmfulAreaSpell(pulse,radius)&&radius>0)
                for(auto const& held:boss->GetSpellAuraHolderMap())
                {
                    uint32 child=0;
                    if(held.second&&held.second->GetCasterGuid()==boss->GetObjectGuid()&&SelfPeriodicAura(held.second->GetSpellProto(),child))
                    {float& known=plan.auraFootprints[held.second->GetSpellProto()->Id];known=std::max(known,radius);}
                }
        }
        for(size_t i=0;i<plan.definition.rules.size();i++)
        {
            auto const& rule=plan.definition.rules[i];
            if(rule.trigger!=(start?Trigger::CastStart:Trigger::CastGo)||std::find(rule.spells.begin(),rule.spells.end(),spell)==rule.spells.end())continue;
            ObjectGuid guid=target?target->GetObjectGuid():ObjectGuid();bool refreshed=false;
            for(auto& event:plan.events)if(event.rule==i&&event.target==guid) {event.remaining=rule.duration;event.impact=start?std::min(castTime,rule.duration):0;refreshed=true;break;}
            if(!refreshed&&plan.events.size()<128)plan.events.push_back({i,guid,rule.duration,start?std::min(castTime,rule.duration):0});
        }
    }
}
void SuiCommanderRaid::Yield(Unit* actor)
{
    if (!actor) return;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto& pair : plans) for (auto& row : pair.second.rows)
        if (!row.manual && row.guid==actor->GetObjectGuid()) { row.yielded=true;row.duty=9;row.hasMove=false; }
}

bool SuiCommanderRaid::Tick(AiBotAI* ai,uint32 diff)
{
    SlowRaidScope slow{"tick",ai->GetBotPlayer()?ai->GetBotPlayer()->GetGUIDLow():0};
    Player* actor=ai->GetBotPlayer();
    if (!actor || !actor->IsInWorld()) return false;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    Plan* plan=nullptr;Assignment* assignment=nullptr;
    for (auto& pair : plans) if (pair.second.state==2)
        for (auto& row : pair.second.rows) if (!row.manual && row.guid==actor->GetObjectGuid()) { plan=&pair.second;assignment=&row;break; }
    if (!plan) return false;
    Assignment& row=*assignment;
    Player* owner=Member(*plan,plan->owner);
    if (!owner || !Commandable(owner,actor) || owner->GetGroup()->GetLeaderGuid()!=owner->GetObjectGuid())
    { Stop(row,*plan);row.yielded=true;row.duty=10;return false; }
    if (row.yielded || ai->m_suiManual || (ai->IsPossessed() && !SuiPossess::IsCommandedFromFreeView(actor))) { row.yielded=true;row.duty=9;return false; }
    if (!actor->IsAlive())
    {
        // Nobody left standing: end the plan so the session sees a terminal state instead of a fight that never
        // resolves (the living-actor update below is what normally observes the end of combat).
        bool anyoneAlive=false;
        for(auto const& other:plan->rows)if(Player* member=Member(*plan,other.guid))if(member->IsAlive()){anyoneAlive=true;break;}
        if(!anyoneAlive&&plan->enteredCombat){for(auto& other:plan->rows)Stop(other,*plan);plan->state=3;plan->enteredCombat=false;plan->claims.clear();++plan->revision;RefreshWatches();}
        row.duty=10;return true;
    }
    ObserveHealer(*plan,row,actor);
    if (actor->IsSuiTacticallyFrozen()) return true;
    if (RecoverFeignDeath(actor,row,diff)) return true;
    if (actor->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)) return true;
    row.bridgeTick+=diff;
    if(row.bridgeTick>=1000) { row.bridgeTick=0; ai->UpdateBridgeTick(); }
    row.tick+=diff;
    if (row.tick<250) return true;
    row.tick=0;
    Unit* boss=nullptr;bool fighting=false,complete=true,yielded=false;
    auto const nowTick=std::chrono::steady_clock::now(); // `owner` is the plan owner already resolved above
    for(uint32 entry:plan->definition.objectives)
    {
        auto& objective=plan->objectives[entry];
        Unit* unit=objective.guid.IsEmpty()?nullptr:actor->GetMap()->GetUnit(objective.guid);
        if(!unit&&!objective.dead)unit=actor->FindNearestCreature(entry,400);
        if(unit) {ObserveUnit(unit,0);if(unit->IsAlive())
            {fighting|=unit->IsInCombat();if(!boss||entry==row.focus)boss=unit;}}
        // The yield predicate: the raid fought this objective (it was in combat while the plan was), then it left
        // combat alive as a non-enemy; the state must hold for kYieldStableMs before it counts.
        if(unit&&unit->IsAlive()&&unit->IsInCombat()&&plan->enteredCombat)objective.engaged=true;
        bool const yieldsNow=objective.engaged&&plan->enteredCombat&&ObjectiveYielded(unit,owner);
        if(yieldsNow&&!objective.yielded){objective.yielded=true;objective.yieldedSince=nowTick;}
        else if(!yieldsNow)objective.yielded=false;
        bool const yieldStable=objective.yielded&&nowTick-objective.yieldedSince>=std::chrono::milliseconds(kYieldStableMs);
        yielded|=yieldStable;
        complete&=objective.seen&&objective.dead||yieldStable; // (seen and dead) or a stable yield (which was observed)
    }
    for(auto const& requirement:plan->definition.requiredAdds)
    {
        std::list<Creature*> required;actor->GetCreatureListWithEntryInGrid(required,requirement.entry,400);
        for(Creature* creature:required)ObserveUnit(creature,0);
        for(auto const& pair:plan->addObjectives)if(pair.second.entry==requirement.entry)
        {
            if(Unit* unit=actor->GetMap()->GetUnit(pair.first))if(unit->IsAlive())
            {
                fighting|=unit->IsInCombat();
                // Once the objectives are dead the fight continues on the required adds: never fall back onto
                // an add held under a control spell (immune or asleep) while an uncontrolled one is fighting.
                bool const held=ControlledAdd(*plan,unit,std::chrono::steady_clock::now());
                auto onTank=[&](Unit* u){for(auto const& r:plan->rows)if(r.role<=2)if(Player* h=Member(*plan,r.guid))if(h->IsAlive()&&u->GetVictim()==h)return true;return false;};
                bool const bossHeld=boss&&boss->IsInCombat()&&ControlledAdd(*plan,boss,std::chrono::steady_clock::now());
                if(!boss||(bossHeld&&!held)||(!held&&!bossHeld&&onTank(unit)&&!onTank(boss)))boss=unit;
            }
        }
    }
    complete&=RequiredAddsComplete(*plan);
    if(complete&&plan->enteredCombat)
    {
        unsigned long long elapsedMs=0;
        for(auto const& pair:plan->objectives)if(pair.second.yielded)elapsedMs=std::max<unsigned long long>(elapsedMs,std::chrono::duration_cast<std::chrono::milliseconds>(nowTick-pair.second.yieldedSince).count());
        sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-completion] owner=%llu guid=%llu predicate=%s elapsedMs=%llu",
            static_cast<unsigned long long>(Raw(plan->owner)),static_cast<unsigned long long>(Raw(plan->boss)),yielded?"friendlySurrender":"death",elapsedMs);
        for(auto& other:plan->rows)Stop(other,*plan);
        plan->state=4;plan->claims.clear();++plan->revision;RefreshWatches();return true;
    }
    if(!boss){row.duty=11;return true;}
    Player* tank=SelectActingTank(*plan,boss,fighting,std::chrono::steady_clock::now());
    if (!fighting)
    {
        // A yielding objective (alive, out of combat, no longer an enemy) is completing, not evading: hold the plan
        // through the stability interval instead of stopping it.
        bool yielding=false;for(auto const& pair:plan->objectives)yielding|=pair.second.yielded;
        if(yielding){Hold(actor,row);row.duty=12;return true;}
        if (plan->enteredCombat)
        {
            for(auto& other:plan->rows) Stop(other,*plan);
            plan->state=3;plan->enteredCombat=false;plan->claims.clear();++plan->revision;RefreshWatches();return true;
        }
        if(row.role==3&&Heal(ai,*plan,row,false))return true;
        for(auto const& rule:plan->definition.rules)
            if(rule.action==Action::Cast&&rule.trigger==Trigger::Always&&rule.phase==0&&rule.missingAura&&
                (rule.roles&(1u<<row.role))&&(!rule.toggle||(plan->flags&rule.toggle))&&
                (rule.target==Target::Self||rule.target==Target::Tank||rule.target==Target::TankHealer))
                if(ExecuteRule(ai,*plan,row,rule,rule.target==Target::Self?actor:rule.target==Target::TankHealer?TankHealer(*plan,tank,rule.spell):tank))return true;
        row.duty=11; // Arming never walks the group through an enemy aggro radius.
        return true;
    }
    plan->enteredCombat=true;
    Phase const* phase=nullptr;
    for(auto const& candidate:plan->definition.phases)if(candidate.id==plan->phase) {phase=&candidate;break;}
    if(!phase) {row.duty=10;return true;}
    bool const air=phase->alternate;
    TryCarriedPotion(actor,row,diff,plan->damageSchools[plan->phase]); // Instant support does not replace hazard evaluation.
    if(RunRules(ai,*plan,row,boss,tank))return true;
    if(NativeCrowdControl(ai,*plan,row,boss))return true;
    if(BringObjectiveToTank(ai,*plan,row))return true;
    Unit* add=nullptr;
    if (row.role==2 || (plan->definition.addPolicy!="hold"&&(row.role==4 || row.role==5)))
    if (row.role!=4||phase->melee) // A melee damage member takes adds only while the phase allows melee at all (melee off = the melee keep their stations; an add that bursts on death is burned by the ranged and the add tanks take the burst).
    {
        float best=100000;
        std::list<Creature*> candidates;
        for(uint32 entry:plan->definition.adds)actor->GetCreatureListWithEntryInGrid(candidates,entry,100);
        // A controlled add that stands before every uncontrolled fighting add in the kill order is the next kill
        // target: the damage that breaks its control is the kill order at work, not a wasted control (v42).
        Creature* release=nullptr;float looseRank=1e9f;auto const nowTick=std::chrono::steady_clock::now();
        for(Creature* candidate:candidates)
            if(candidate->IsAlive()&&candidate->IsInCombat()&&!candidate->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)&&!ControlledAdd(*plan,candidate,nowTick))looseRank=std::min(looseRank,KillRank(*plan,candidate));
        for(Creature* candidate:candidates)
            if(candidate->IsAlive()&&candidate->IsInCombat()&&!candidate->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)&&KillRank(*plan,candidate)<looseRank&&ControlledAdd(*plan,candidate,nowTick)&&(!release||KillsBefore(*plan,candidate,release)))release=candidate;
        // Add tanks hold what control cannot (v45): while a fighting add no raid control spell can hold is up, an add tank
        // leaves the controllable ones to the controllers and the damage rotation - its threat on them sends every released
        // kill target back into its held adds' bursts (route-0914v-executus-08: each released healer went to an add tank).
        bool holdUncontrollable=false;
        if(row.role==2)
            for(Creature* candidate:candidates)
                if(candidate->IsAlive()&&candidate->IsInCombat()&&!candidate->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)&&!RaidCanControl(*plan,candidate,nowTick))
                {holdUncontrollable=true;break;}
        // Tank assignments (v53): every add the add tanks take is assigned to one living add tank when it is first seen - the
        // least loaded, GUID order breaking ties - and moves only when that tank dies. An add tank takes its own adds and
        // never another tank's: the v47 balancing and the v48 loose-first race walked both add tanks onto the same ground
        // (majordomo-0914p-01: both 7 yd apart under all four elites, both dead at t=41).
        if(row.role==2)
        {
            std::vector<ObjectGuid> tanks;
            for(auto const& assigned:plan->rows)if(assigned.role==2)if(Player* holder=Member(*plan,assigned.guid))if(holder->IsAlive())tanks.push_back(assigned.guid);
            std::sort(tanks.begin(),tanks.end(),[](ObjectGuid a,ObjectGuid b){return a.GetRawValue()<b.GetRawValue();});
            std::vector<Creature*> taken;
            for(Creature* candidate:candidates)
                if(candidate->IsAlive()&&candidate->IsInCombat()&&!candidate->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)&&
                    !ControlledAdd(*plan,candidate,nowTick)&&!(holdUncontrollable&&RaidCanControl(*plan,candidate,nowTick)))taken.push_back(candidate);
            std::sort(taken.begin(),taken.end(),[](Creature* a,Creature* b){return a->GetObjectGuid().GetRawValue()<b->GetObjectGuid().GetRawValue();});
            for(auto it=plan->addTankOf.begin();it!=plan->addTankOf.end();)
            {
                bool const addLive=std::any_of(taken.begin(),taken.end(),[&](Creature* c){return c->GetObjectGuid()==it->first;});
                bool const tankLive=std::find(tanks.begin(),tanks.end(),it->second)!=tanks.end();
                if(!addLive||!tankLive)it=plan->addTankOf.erase(it);else ++it;
            }
            auto tankDistance=[&](ObjectGuid tankGuid,Creature* add){Player* holder=Member(*plan,tankGuid);return holder?holder->GetDistance(add):100000.f;};
            for(Creature* add:taken)
                if(!plan->addTankOf.count(add->GetObjectGuid())&&!tanks.empty())
                {
                    // Least loaded first; the nearest of the equally loaded tanks takes it (v55 - GUID order sent a tank
                    // across the room to an add another tank stood beside).
                    ObjectGuid least;unsigned fewest=~0u;float nearest=100000.f;
                    for(ObjectGuid tankGuid:tanks)
                    {
                        unsigned held=0;for(auto const& pair:plan->addTankOf)if(pair.second==tankGuid)++held;
                        float const reach=tankDistance(tankGuid,add);
                        if(held<fewest||(held==fewest&&reach<nearest)){fewest=held;least=tankGuid;nearest=reach;}
                    }
                    plan->addTankOf[add->GetObjectGuid()]=least;
                }
            // Rescue (v55): an add beating on a member who is not a tank belongs to the nearest living add tank once that
            // tank stands 10 yd closer than the assigned one - the assigned tank's walk lets the add kill healers while
            // another add tank stands beside it (five-tank Majordomo losses: healer melee intake 3-8x the win's).
            for(Creature* add:taken)
            {
                Unit* victim=add->GetVictim();
                if(!victim||victim->GetTypeId()!=TYPEID_PLAYER)continue;
                bool victimTank=false;
                for(auto const& other:plan->rows)if(other.role<=2&&other.guid==victim->GetObjectGuid()){victimTank=true;break;}
                if(victimTank)continue;
                auto assigned=plan->addTankOf.find(add->GetObjectGuid());
                if(assigned==plan->addTankOf.end())continue;
                ObjectGuid rescuer;float nearest=100000.f;
                for(ObjectGuid tankGuid:tanks){float const reach=tankDistance(tankGuid,add);if(reach<nearest){nearest=reach;rescuer=tankGuid;}}
                if(!rescuer.IsEmpty()&&rescuer!=assigned->second&&tankDistance(assigned->second,add)>nearest+10.f)
                {
                    sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-add-rescue] add=%u entry=%u victim=%u from=%u to=%u reach=%.1f",
                        add->GetGUIDLow(),add->GetEntry(),victim->GetGUIDLow(),assigned->second.GetCounter(),rescuer.GetCounter(),nearest);
                    assigned->second=rescuer;
                }
            }
        }
        for(Creature* candidate:candidates)
            {
                if(!candidate->IsAlive()||!candidate->IsInCombat()||candidate->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD))continue;
                if(candidate!=release&&ControlledAdd(*plan,candidate,nowTick))continue; // Held by a control spell: not a target.
                if(row.role==2&&holdUncontrollable&&RaidCanControl(*plan,candidate,nowTick))continue;
                if(row.role==2)
                {
                    auto assigned=plan->addTankOf.find(candidate->GetObjectGuid());
                    if(assigned!=plan->addTankOf.end()&&assigned->second!=row.guid)continue; // another add tank's add (v53)
                }
                if(MinorAddCanWait(*plan,row,*phase,boss,candidate))continue; // Preserve objective DPS before retaining an optional add assignment.
                if(plan->definition.addPolicy=="split"&&row.role==5&&row.target==candidate->GetObjectGuid()){add=candidate;break;}
                if(plan->definition.addPolicy=="split"&&row.role==5&&row.target!=candidate->GetObjectGuid())
                {
                    unsigned assigned=0;
                    for(auto const& other:plan->rows)
                        if(other.role==5&&other.guid!=row.guid&&other.target==candidate->GetObjectGuid())
                            if(Player* member=Member(*plan,other.guid))if(member->IsAlive())++assigned;
                    // Commit up to two ranged actors per add; retain existing
                    // assignments until it dies instead of retargeting the raid.
                    if(assigned>=2)continue;
                }
                float teamDistance=100000;size_t nearestTeam=0;
                for(size_t team=0;team<plan->definition.teams.size();team++)
                {
                    Point anchor=plan->definition.teams[team].anchor;
                    float dx=candidate->GetPositionX()-anchor.x,dy=candidate->GetPositionY()-anchor.y,score=dx*dx+dy*dy;
                    if(score<teamDistance) {teamDistance=score;nearestTeam=team;}
                }
                float distance=actor->GetDistance(candidate)+(nearestTeam==row.team?0:40);
                if(row.role==2)
                {
                    bool tanked=false;
                    // Only an add tank counts as holding an add: an add beating on the boss's own tank is loose
                    // (a linked pull lands every add on the puller; the main tank must be relieved, not "secured").
                    for(auto const& assigned:plan->rows)if(assigned.role==2&&candidate->GetVictim()==Member(*plan,assigned.guid)){tanked=true;break;}
                    // Collect loose adds attacking healers before padding threat on a secured add.
                    // Whatever the distance (v48): once the add tanks hold their adds apart, a loose add 30 yd away on the
                    // other team's side scored above the tank's own held adds and walked the raid for the whole healer
                    // phase (majordomo-0914j-01: rogues, priests and warlocks dead under one loose elite).
                    if(!tanked)distance=actor->GetDistance(candidate)*.1f+(nearestTeam==row.team?0.f:5.f);
                    if(!tanked)distance-=60;
                    else if(candidate->GetVictim()!=actor)
                        distance=actor->GetDistance(candidate)-45; // my assigned add on another add tank: take it over (v53)
                    else
                    {
                        // Several adds on one tank are held by rotating the threat lead (tab-sunder): the held add
                        // whose lead over its next attacker is smallest is hit next; the current target keeps a small
                        // preference so the rotation does not flap (2026-09-14, v43 - one add got the whole rotation
                        // and the other three went to the healers on healing threat once the shout expired).
                        float const lead=candidate->GetThreatManager().getThreat(actor);float next=0;
                        for(HostileReference* ref:candidate->GetThreatManager().getThreatList())
                            if(ref&&ref->getTarget()!=actor)next=std::max(next,ref->getThreat());
                        distance=std::max(0.f,lead-next)*.05f-(row.target==candidate->GetObjectGuid()?10.f:0.f);
                    }
                }
                else if(!row.target.IsEmpty()&&candidate->GetObjectGuid()==row.target)distance-=10;
                if(row.role!=2&&plan->definition.addPolicy=="focus")distance=KillRank(*plan,candidate)*1000.f+candidate->GetHealthPercent()+float(candidate->GetObjectGuid().GetRawValue()%997u)*.0001f; // kill order first, then whatever is already lowest, then one shared tie-break (v42)
                if(row.role!=2&&plan->definition.addPolicy=="balance")
                {
                    unsigned assigned=0;for(auto const& other:plan->rows)if(other.guid!=row.guid&&(other.role==4||other.role==5)&&other.target==candidate->GetObjectGuid())
                        if(Player* member=Member(*plan,other.guid))if(member->IsAlive())++assigned;
                    distance=-candidate->GetHealthPercent()*3.f+assigned*25.f+actor->GetDistance(candidate)*.01f;
                }
                if(candidate->IsInCombat()&&distance<best) {best=distance;add=candidate;}
            }
    }

    if (row.role==3)
    {
        // Real heals/channels finish unless encounter safety already interrupted them.
        // Autorepeat is excluded, so healing demand still preempts idle wanding.
        if(actor->IsNonMeleeSpellCasted(false,false,true))return true;
        if(actor->GetClass()==CLASS_PRIEST&&!actor->GetAttackers().empty())
        {
            StopIdleWand(actor);
            if(!actor->IsNonMeleeSpellCasted(false,false,true)&&ai->TrySpecSpell(actor,586))return true; // Fade
        }
        if(Heal(ai,*plan,row,true))return true;
        if(RecoverHealingRange(ai,*plan,row,tank))return true;
        if(Support(ai,*plan,row,boss,false))return true;
        if(Heal(ai,*plan,row,false))return true;
        if(MaintainFormation(ai,*plan,row))return true;
        if (Move(ai,*plan,row,air?row.air:row.ground,1)) return true;
        if(IdleWand(ai,*plan,row,boss,tank,*phase))return true;
        row.duty=actor->GetPowerPercent(POWER_MANA)<15?15:4;
        return true;
    }
    // Objective rescue (v56): an objective beating on a member who cannot tank (a threat reset landed it on a healer) is
    // taunted off by the add tank standing nearest to it, whatever that tank holds, once it is within a short walk and its
    // taunt is ready; the held add follows its tank for those few yards. The loose-objective pickup above needs a tank
    // with no add of its own, and with an add tank per add nobody qualified: v55 Majordomo losses took 8-9k melee from the
    // objective on healers in the first 90 s (the win with the cleanest opening took 1k).
    if(row.role==2&&row.taunt&&boss&&boss->GetVictim()&&LooseObjective(*plan,boss)&&actor->IsSpellReady(row.taunt)&&
        actor->GetDistance(boss)<=kObjectiveRescueReach)
    {
        Player* nearest=nullptr;float reach=100000.f;
        for(auto const& other:plan->rows)
            if(other.role==2&&other.taunt&&!other.yielded)
                if(Player* holder=Member(*plan,other.guid))
                    if(holder->IsAlive()&&holder->IsSpellReady(other.taunt)&&holder->GetDistance(boss)<reach){reach=holder->GetDistance(boss);nearest=holder;}
        if(nearest==actor)
        {
            if(actor->CanReachWithMeleeAutoAttack(boss))
            {
                if(SupportCast(ai,*plan,row,boss,row.taunt,2))
                {
                    sLog.Out(LOG_BASIC,LOG_LVL_BASIC,"[SUI][raid-objective-rescue] tank=%u objective=%u victim=%u",actor->GetGUIDLow(),boss->GetEntry(),boss->GetVictim()->GetGUIDLow());
                    return true;
                }
            }
            else if(SafeCombatApproach(ai,*plan,row,boss))return true;
        }
    }
    Unit* target=add?add:boss;
    // A loose objective is picked up by every taunt-capable melee or add-tank row that has no add of its own, as if
    // it were the acting tank: close, taunt in reach, build threat; the damage-role threat gate never holds a
    // taunt-capable member back from an objective nobody holds (2026-09-14, v36).
    bool const pickup=actor!=tank&&row.taunt&&(row.role==2||row.role==4)&&!add&&LooseObjective(*plan,boss);
    bool const mainTank=(actor==tank||pickup) && MeleeAllowed(*phase,row.role);
    if(mainTank){add=nullptr;target=boss;}
    // Hold add policy: the adds are the add tanks' business until the objectives are dead, and a melee member never
    // closes on one afterwards either (an add that bursts on death is finished from range); it keeps formation.
    if(row.role==4&&plan->definition.addPolicy=="hold"&&target&&!mainTank)
    {
        bool requiredAdd=false;
        for(auto const& requirement:plan->definition.requiredAdds)if(requirement.entry==target->GetEntry()){requiredAdd=true;break;}
        if(requiredAdd){actor->InterruptNonMeleeSpells(false);actor->AttackStop();if(Pet* pet=actor->GetPet()){pet->AttackStop();if(pet->GetCharmInfo())pet->GetCharmInfo()->SetIsCommandAttack(false);}if(MaintainFormation(ai,*plan,row))return true;Move(ai,*plan,row,air?row.air:row.ground,1);row.duty=8;return true;}
    }
    if(target->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)){Hold(actor,row);row.duty=8;return true;}
    if(Support(ai,*plan,row,target,mainTank||(add&&row.role==2)))return true;
    if (!add && ((row.role==5 && !phase->ranged) || (row.role!=5 && !MeleeAllowed(*phase,row.role)))) { actor->InterruptNonMeleeSpells(false);actor->AttackStop();if(Pet* pet=actor->GetPet()){pet->InterruptNonMeleeSpells(false);pet->AttackStop();if(pet->GetCharmInfo())pet->GetCharmInfo()->SetIsCommandAttack(false);}if(MaintainFormation(ai,*plan,row))return true;Move(ai,*plan,row,air?row.air:row.ground,1);row.duty=8;return true; }
    if (!add && phase->threatRatio>0 && !mainTank && (!tank || boss->GetVictim()!=tank ||
        boss->GetThreatManager().getThreat(actor) > boss->GetThreatManager().getThreat(tank)*phase->threatRatio))
    {
        actor->InterruptNonMeleeSpells(false);actor->AttackStop();
        if(Pet* pet=actor->GetPet()){pet->InterruptNonMeleeSpells(false);pet->AttackStop();if(pet->GetCharmInfo())pet->GetCharmInfo()->SetIsCommandAttack(false);}
        // Threat tools are class behavior. Taunt is not a damage-role rotation.
        if(actor->GetClass()==CLASS_ROGUE)ai->TrySpecSpell(boss,1966); // Feint
        row.duty=8;return true;
    }
    // Phase/reflect/threat/safety stops above still preempt casts. Routine range
    // and station movement must not cancel an otherwise valid cast or channel.
    if(actor->IsNonMeleeSpellCasted(false,false,true))return true;
    // Active damage keeps valid spell range or melee contact. Shared route
    // costs spread any needed approach; routine formation must not undo it.
    bool const stationaryMechanic=TimedPositionActive(*plan,row);
    // Add hold ground (v45): an add tank that holds its target walks its burst carriers clear of the fight.
    if(row.role==2&&!row.manual&&add&&add->GetVictim()==actor&&!stationaryMechanic)
    {
        Point holdGoal,holdWaypoint;float holdSeconds=0;
        if(AddHoldRoute(*plan,row,actor,boss,tank,holdGoal,holdWaypoint,holdSeconds))
        {row.target=add->GetObjectGuid();if(Move(ai,*plan,row,holdGoal,6))return true;}
    }
    if(((stationaryMechanic&&mainTank)||(add&&row.role!=5))&&!actor->CanReachWithMeleeAutoAttack(target))
    {if(SafeCombatApproach(ai,*plan,row,target))return true;}
    if (mainTank)
    {
        Point tankStation=row.ground;
        for(auto const& primary:plan->rows) if(primary.role==1&&primary.focus==boss->GetEntry()) { tankStation=primary.ground;break; }
        float sx=tankStation.x-actor->GetPositionX(),sy=tankStation.y-actor->GetPositionY();
        Point stationAxis=TankStationOutward(*plan,boss,tankStation);
        float ox=stationAxis.x,oy=stationAxis.y;
        float dx=actor->GetPositionX()-boss->GetPositionX(),dy=actor->GetPositionY()-boss->GetPositionY();
        // Keep the acquired station after knockback while threat is retained.
        // A lost target still requires ordinary pursuit and threat acquisition.
        if(!stationaryMechanic&&boss->GetVictim()==actor&&!actor->CanReachWithMeleeAutoAttack(boss)&&
            sx*sx+sy*sy<=4.f&&dx*ox+dy*oy>0)
        {Hold(actor,row);row.target=boss->GetObjectGuid();row.duty=2;actor->SetInFront(boss);return true;}
        float outwardLength=std::hypot(ox,oy),separation=std::hypot(dx,dy);
        bool facingOutward=outwardLength>.1f&&separation>=1.5f&&
            (dx*ox+dy*oy)/(outwardLength*separation)>=.98480775f;
        if (!stationaryMechanic&&(boss->GetVictim()!=actor || (!actor->CanReachWithMeleeAutoAttack(boss)&&!facingOutward)))
        {
            if (actor->GetVictim()!=boss || actor->GetMotionMaster()->GetCurrentMovementGeneratorType()!=CHASE_MOTION_TYPE)
                ai->AttackStart(boss);
            row.hasMove=false;
        }
        else
        {
            if (actor->GetVictim()!=boss) actor->Attack(boss,true);
            tankStation=TankFacingStation(*plan,actor,boss,tankStation);
            if(!stationaryMechanic&&boss->GetVictim()==actor)tankStation=CoveredTankStep(*plan,actor,tankStation);
            if (Move(ai,*plan,row,tankStation,1,false,.2f)) return true;
        }
    }
    else if(row.role==5)
    {
        if(RecoverDamageRange(ai,*plan,row,target))return true;
    }
    else if (!add)
    {
        Point station=air?row.air:row.ground;
        if (phase->melee && row.role==4)
        {
            if(!FlankStation(*plan,row,actor,boss,station))
            {Hold(actor,row);row.duty=12;return true;}
        }
        if (Move(ai,*plan,row,station,1)) return true;
    }
    row.target=target->GetObjectGuid();row.duty=mainTank?2:add?3:5;
    if (actor->GetVictim()!=target)
        actor->Attack(target,true);
    actor->SetInFront(target);
    if(!actor->IsMoving()&&!actor->IsNonMeleeSpellCasted(false,false,true))
    {
        auto oldRole=ai->m_role;
        ai->m_role=mainTank||row.role==2?ROLE_TANK:row.role==5?ROLE_RANGE_DPS:ROLE_MELEE_DPS;
        // The ordinary class/spec rotation owns abilities and resources. The
        // encounter already chose target, movement, mechanics and threat budget.
        ai->UpdateInCombatAI();ai->m_role=oldRole;
    }
    return true;
}

bool AiBotAI::CommanderRaidCast(Unit* target,uint32 id)
{
    if (!target || !id || !me->HasSpell(id) || me->IsMoving() || me->IsNonMeleeSpellCasted(false,false,true)) return false;
    SpellEntry const* spell=sSpellMgr.GetSpellEntry(id);
    if(spell&&target->IsImmuneToSpell(spell,target==me))return false;
    // Normal heals, dispels and encounter support must preempt automatic wand fire.
    if(spell&&!spell->IsAutoRepeatRangedSpell())me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true);
    if(spell&&spell->IsAutoRepeatRangedSpell()&&me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))return true;
    return spell && CanTryToCastSpell(target,spell) && DoCastSpell(target,spell)==SPELL_CAST_OK;
}
bool AiBotAI::CommanderRaidMove(float x,float y,float z)
{
    return MovePointRun(0x52414944,x,y,z);
}
