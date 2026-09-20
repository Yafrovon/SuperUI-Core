#!/usr/bin/env python3
"""Read-only source-contract checks. These do not compile Core or simulate combat."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
read=lambda rel:(root/rel).read_text(encoding='utf-8')
core=read('src/game/SuperUiContent/SuiWorld/CRPG/SuiCommanderRaid.cpp')
definition=read('src/game/SuperUiContent/SuiWorld/CRPG/SuiEncounterDefinition.cpp')
spell=read('src/game/Spells/Spell.cpp')
unit=read('src/game/Objects/Unit.cpp')
ai=read('src/game/SuperUiContent/SuiBots/AiBotAIMain.cpp')
bridge=read('src/game/SuperUiContent/SuiBots/AiBotAIBridge.cpp')
possess=read('src/game/SuperUiContent/SuiWorld/CRPG/SuiPossess.cpp')
packet=read('src/game/Server/Packets/SuiControl.h')
spec=read('src/game/SuperUiContent/SuiBots/AiBotAISpecCombat.cpp')
guidance=core.split('    Advice Guidance(Plan const& plan,Assignment const& row)\n    {',1)[1].split('    void Hold(',1)[0]
socket=read('src/game/Server/WorldSocket.cpp')
misc=read('src/game/Handlers/MiscHandler.cpp')
stand=misc.split('void WorldSession::HandleStandStateChangeOpcode',1)[1].split('void WorldSession::HandleFriendListOpcode',1)[0]
creature=read('src/game/Objects/Creature.cpp')
checks={
 'required adds tracked by distinct GUID':'std::map<ObjectGuid,Objective> addObjectives' in core and 'plan.addObjectives[boss->GetObjectGuid()]' in core,
 'unpulled optional adds excluded':'boss->IsInCombat()&&InRoom(plan' in core,
 'every participating add must actually die':'complete&=RequiredAddsComplete(*plan)' in core and 'if(!pair.second.seen||!pair.second.dead)return false' in core,
 'required count cannot be inferred from one entry':'if(seen<requirement.count)return false' in core,
 'feigned death does not become actual objective death':'bool dead=!boss->IsAlive()' in core and 'HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_DEAD)' in core,
 'immediate actual death observed before scripted despawn':'s == JUST_DIED && SuiCommanderRaid::Watches(GetEntry())' in creature or 's==JUST_DIED&&SuiCommanderRaid::Watches(GetEntry())' in creature,
 'required add parser bounded':'requiredAdds' in definition and 'AddRequirement' in definition,

 'idle wand preserves matching normal autorepeat after healing gates':'repeat->m_targets.getUnitTargetGuid()==enemy->GetObjectGuid()' in core and core.index('HealingPatient(actor,plan,row,false)||!wand')<core.index('if(Spell* repeat=actor->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))'),
 'carried potions use ordinary item handler':'actor->GetSession()->HandleUseItemOpcode(request)' in core and 'actor->CanUseItem(item)!=EQUIP_ERR_OK' in core and 'actor->IsSpellReady(spell->Id,proto)' in core,
 'potion scan excludes bank and noncombat items':'slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END' in core and 'spell->IsNonCombatSpell()' in core and 'use.SpellCategory!=4' in core,
 'potion use leaves hazard evaluation active':'TryCarriedPotion(actor,row,diff,plan->damageSchools[plan->phase]); // Instant support does not replace hazard evaluation.' in core,
 'protection school derives from loaded hazard spell chains':'HazardDamageSchools(next.definition,phase.id)' in core and 'spell->GetSpellSchoolMask()' in core and 'spell->EffectTriggerSpell[effect]' in core,
 'protection preserves existing absorption and native item cooldown':'SPELL_AURA_SCHOOL_ABSORB' in core and '!actor->HasAura(spell->Id)' in core and 'actor->IsSpellReady(spell->Id,proto)' in core,
 'manual potion support is clocked and actor-scoped':'if(diff&&boss->IsInCombat())for(auto& row:plan.rows)if(row.manual&&!row.yielded)' in core and 'actor->GetSession()->GetSuiActor()!=actor' in core and 'if(!diff)return;' in core,
 'loadout preparation checks native space and ordinary bag equip':'CanStoreNewItem(NULL_BAG,NULL_SLOT,positions' in read('src/game/Commands/RaidQaCommands.cpp') and 'HandleAutoEquipItemOpcode(packet)' in read('src/game/Commands/RaidQaCommands.cpp') and 'QA_SUPPLY_REJECT general bag and empty slot required' in read('src/game/Commands/RaidQaCommands.cpp'),
 'protected healer can afford assigned healing':'Spell::CalculatePowerCost(spell,healer,nullptr,nullptr,false)>healer->GetPower(POWER_MANA)' in core,
 'stand handler uses acting body with normal guards':'GetSuiActor()' in stand and '_player' not in stand and 'IsSuiTacticallyFrozen' in stand and 'UNIT_FLAG_PREVENT_ANIM' in stand,
 'stand reply is mirrored':'case SMSG_STANDSTATE_UPDATE:' in possess,
 'urgent healing counts only timely claims':'horizon=risk.deadline?risk.deadline:actual<50?1750:0' in core and 'claim.remaining<=horizon' in core,
 'low tank health blocks idle wand regardless of reservations':'tank->GetHealthPercent()<50)||HealingPatient' in core,
 'critical healing precedes routine buffs after instant control protection':'if(plan.enteredCombat&&row.role==3&&Heal(ai,plan,row,true))return true;\n        if(cast())return true;' in core,
 'tank healer support chooses a living assigned fast healer':'row.role==3&&row.primary==tank->GetObjectGuid()&&!row.yielded' in core and 'castTime==fastest&&Raw(row.guid)<Raw(selected->GetObjectGuid())' in core,
 'tank healer target works in combat and stationary arming':'Target::TankHealer?TankHealer(plan,tank,rule.spell)' in core and 'Target::TankHealer?TankHealer(*plan,tank,rule.spell)' in core,
 'tank healer is restricted to authored support':'rule.target!=Target::TankHealer||(rule.action==Action::Cast&&rule.trigger==Trigger::Always)' in definition,

 'active damage never detours through routine formation':'if(MaintainFormation(ai,*plan,row))' not in core.split('// Phase/reflect/threat/safety stops above',1)[1],
 'healer formation preserves patient range and LOS':'seconds,safe,"formation"' in core and 'actor,usable,goal,waypoint' in core and 'return HealerStationAllowed(plan,row,p);' in core and 'range*range&&patient->IsWithinLOS' in core,
 'routine formation tolerates small spacing deviations':'if(FormationCost(plan,row,at)<=24.f)return false' in core,

 'marked isolation evacuates the actual marked actor':'else if(rule.action==Action::Isolate)' in core and 'for(auto const& peer:plan.rows)if(peer.guid!=row.guid)' in core,
 'impact aura retires only matching isolation warnings':'rule.action==Action::Isolate&&rule.missingAura&&rule.spell&&target&&target->HasAura(rule.spell)' in core and 'if(it->remaining<=diff||resolved)' in core,
 'isolation requires an observed marked cast or member aura':'rule.action!=Action::Isolate||((event||rule.trigger==Trigger::MemberAura)&&rule.target==Target::Marked&&rule.radius>=1&&(!rule.missingAura||rule.spell))' in definition,
 'support reservations gate every candidate caster':'!SupportCasterReserved(plan,p,rule)' in core and 'earlier<reserve.reserveCasters' in core,
 'support reservation membership ignores cooldown churn':'p->IsAlive()&&p->HasSpell(reserve.spell)' in core and 'reserve.priority<=action.priority' in core,

 'formation reserves current and intended peer positions':'float distance=std::min(std::sqrt(dx*dx+dy*dy),std::sqrt(ax*ax+ay*ay))' in core,
 'routine formation requires authored self spread and fully safe paths':'rule.trigger==Trigger::Always&&rule.target==Target::Self' in core and 'seconds,safe,"formation"' in core and 'if(!safe(at)||!FindClearRoute' in core,
 'healing and support precede routine formation':core.index('if(Heal(ai,*plan,row,false))')<core.index('if(MaintainFormation(ai,*plan,row))') and core.index('if(Support(ai,*plan,row,boss,false))')<core.index('if(MaintainFormation(ai,*plan,row))'),
 'manual guidance and ordinary station range share formation cost':guidance.count('FormationRoute(plan,row,actor,goal,advice.waypoint,seconds)')==2 and 'seconds,safe,reason,[&](Point p){return FormationCost(plan,row,p);}' in core and 'safe,"healing",[&](Point p){return FormationCost(plan,row,p);}' in core,

 'both drivers revalidate phase-scoped escapes and certified holds':'row.escapePhase!=phase||dx*dx+dy*dy<=2.25f' in core and 'CurrentPositionCertified(plan,row,actor,at,hazards))&&!EscapePending(row,at,plan.phase)' in core and '!PositionClear(plan,row,actor,at,hazards)||EscapePending(row,at,plan.phase)' in core,
 'revalidated escape destinations resist oscillation and remain reserved':'score+=20.f' in core and 'other.escapeGoalReady&&other.escapePhase==plan.phase?other.escapeGoal' in core and 'seconds,traverse,"combined",crowding)){remember();return true;}' in core,
 'authored support selects available caster and reserves target spell':'if(selected!=actor)return false' in core and 'p->IsSpellReady(rule.spell)' in core and 'claim.kind==18&&claim.target==target->GetObjectGuid()&&claim.spell==rule.spell' in core,
 'support approach is combat-only safe and yields to critical healing':'if(!plan.enteredCombat||rule.target==Target::Self)return false' in core and 'if(row.role==3&&Heal(ai,plan,row,true))return true' in core and 'rule.spell,"support",false' in core,
 'defensive support is distinct from incoming healing claims':'claimKind=duty==4?17:duty' in core and 'if(claim.kind==4&&claim.target==target&&(!horizon||claim.remaining<=horizon))' in core,
 'safe destinations include moving peers and authored spacing':'other.hasMove?other.lastMove' in core and 'spacing=std::max(spacing,rule.radius)' in core and 'seconds,traverse,"combined",crowding)' in core,
 'fallback minimizes splash without disguising route duration':'"priority",exposure)' in core and 'score+=300.f*penetration*penetration' in core and 'travelSeconds=found?bestTravel/' in core and 'if(InRoom(plan,at)&&isClear(at)&&(!acceptRoute||acceptRoute(at,0,PointsArray{})))' in core,
 'socket admits the bounded full raid definition only for its opcode':'header->cmd == CMSG_SUI_COMMANDER_RAID' in socket and '4u + 24u + 40u * 71u + 32768u : 0x2800u' in socket and 'header->size > maxIncomingSize' in socket,
 'observable boss position predicts an authored footprint':'bool triggered=BossNearActive(plan,rule,actor)' in core and 'rule.triggerRadius*rule.triggerRadius' in core and 'if(BossNearActive(plan,rule,actor))return true' in core and 'Trigger::BossNear' in definition,

 'manual tank shares complete safe contact routes':'route=CombatApproachRoute(plan,row,actor,boss,goal,advice.waypoint,seconds)' in core and 'if(!CombatApproachRoute(plan,row,actor,target,goal,waypoint,seconds))' in core,

 'cone avoidance includes the real cast target after a threat swap':'casting->m_spellInfo->Id==id' in core and 'casting->m_targets.getUnitTarget()' in core and 'bearings[1]=boss->GetAngle(castTarget)' in core and 'for(float bearing:bearings)' in core and 'm_casterUnit->SetInFront(m_targets.getUnitTarget())' in spell,

 'add assignments bound ranged commitment by actual target GUID':'if(plan->definition.addPolicy=="split"&&row.role==5&&row.target!=candidate->GetObjectGuid())' in core and 'other.target==candidate->GetObjectGuid()' in core and 'if(assigned>=2)continue' in core,
 'normal casts survive routine movement while autorepeat stays preemptible':core.count('if(actor->IsNonMeleeSpellCasted(false,false,true))return true;')>=3,
 'all add melee approaches use checked movement':'((stationaryMechanic&&mainTank)||(add&&row.role!=5))' in core and 'ai->AttackStart(target)' not in core,

 'established tank acquires station only after outward facing':'(dx*ox+dy*oy)/distance>=.98480775f' in core and 'tankStation=TankFacingStation(*plan,actor,boss,tankStation)' in core,

 'ranged roles approach the actual chosen target':'if(RecoverDamageRange(ai,*plan,row,target))return true;' in core,
 'ranged approach respects spell limits LOS combat reach and hazards':'SizeFactor::CombatReach)-actor->GetCombatReach()' in core and 'distance>=minimum&&distance<=maximum&&safe(p)&&enemy->IsWithinLOS' in core and 'seconds,safe,reason' in core and 'row.damage,"ranged",true' in core,

 'nearby emergency healing precedes range recovery':core.index('if(Heal(ai,*plan,row,true))')<core.index('if(RecoverHealingRange(ai,*plan,row,tank))'),

 'displaced actors may enter but never re-exit room bounds':'enteredRoom=InRoom(plan,at)' in core and 'if(enteredRoom&&!inside)' in core and '!enteredRoom||score>=best' in core,

 'combat approaches validate observed hazards and forecast escape':'FindClearRoute(plan,actor,contact,goal,waypoint,seconds,safe,"combat-approach",{},verify,target->GetPositionZ())' in core and 'return PositionClear(plan,row,actor,p,observed)' in core,
 'combat approaches use normal melee contact and arrival margin':'CanReachWithMeleeAutoAttackAtPosition(target,p.x,p.y,p.z,-1.5f)' in core,
 'ranged damage helps remove encounter adds':'if (row.role==2 || (plan->definition.addPolicy!="hold"&&(row.role==4 || row.role==5)))' in core,
 'priest threat recovery covers add attackers':'actor->GetClass()==CLASS_PRIEST&&!actor->GetAttackers().empty()' in core,

 'all active footprints share one execution escape':'auto hazards=PositionHazards(plan,row,actor,active,firstRule,impact);' in core and 'if(active&&(!PositionClear(plan,row,actor,at,hazards)||EscapePending(row,at,plan.phase)))' in core,
 'manual guidance uses the identical combined escape':core.count('CombinedEscape(plan,row,actor,hazards,goal,')==2,
 'strict escape preserves initially safe independent lanes':'initiallySafe[h.group]&&!OutsideHazard(p,h)' in core,
 'overlapping lane circles share their escape group':'inserted.first->second&=OutsideHazard(at,h)' in core,
 'fallback preserves authored highest-priority hazards':'h.priority==highest&&!OutsideHazard(p,h)' in core,
 'certified current safety precedes optional crowding optimization':'bool found=PositionClear(plan,row,actor,at,observed)&&certified(at,0,PointsArray{});' in core and 'if(found){goal=waypoint=at;seconds=0;}' in core,
 'ordinary route families share bounded forecast certificates':core.count('auto verify=ForecastVerifier(plan,row,actor,hazards);')>=7 and 'safe,"combat-approach",{},verify,target->GetPositionZ())' in core and 'safe,"manual-air"' in core,
 'guidance recognizes a certified current position':'||CurrentPositionCertified(plan,row,actor,at,hazards)' in core,
 'forecast timing comes from loaded matching cast footprints':'ForecastWarningSeconds' in core and 'std::is_permutation(rule.points.begin()' in core and 'spell->GetCastTime(boss)/1000.f' in core,
 'forecast certificates require complete paths and time after control':'ForecastRouteCertified' in core and '(warning-controlLock-1.f-(row.manual?2.5f:0.f))*actor->GetSpeed(MOVE_RUN)-travel' in core and 'PATHFIND_SHORTCUT|PATHFIND_NOT_USING_PATH|PATHFIND_DEST_FORCED' in core,
 'forecast proof work is bounded and unknown effects retain caution':'queries>=64' in core and 'if(depth>=8)return 1000' in core and 'SPELL_EFFECT_SCRIPT_EFFECT)lock=1000' in core,
 'safe mechanic arrival resumes authored support':'if(Move(ai,plan,row,goal,6))return true;' in core,
 'fallback guidance does not promise full safety':'advice.state=!compromised&&seconds<=.01f?1:!compromised&&impact' in core,
 'route failures expose bounded actual rejection counters':'[SUI][raid-route]' in core and 'std::chrono::seconds(5)' in core,
 'member aura sources are observed living raid members':'if(p->IsAlive())for(uint32 spell:rule.spells)if(p->HasAura(spell))' in core and 'if(source->IsAlive())' in core and 'if(source!=actor)' in core,
 'combined positioning preserves legal stationary role work':'if(active&&(rule.action==Action::AvoidPoints||((rule.action==Action::Spread||rule.action==Action::Isolate)&&rule.target==Target::Marked)))continue;' in core,
 'safe timed hazard permits stationary role work':'if(Clearance(at,lane)>rule.radius)return false;' in core,
 'marked spread actor keeps casting at its location':'if(rule.action==Action::Spread)return false;' in core,
 'ordinary station movement checks every active hazard':'if(duty==1&&(MeleeStationRequired(plan,row)||TimedPositionActive(plan,row)||FormationSpacing(plan,row)>0))return SafeStationMove' in core and 'nearStation,goal,waypoint,seconds,safe,"station"' in core,
 'timed hazard constrains tank acquisition and every add approach':'((stationaryMechanic&&mainTank)||(add&&row.role!=5))&&!actor->CanReachWithMeleeAutoAttack(target)' in core,
 'idle healer damage follows needed heals and station movement':core.index('if(Heal(ai,*plan,row,false))')<core.index('if(IdleWand(ai,*plan,row,boss,tank,*phase))') and core.index('if (Move(ai,*plan,row,air?row.air:row.ground,1))',core.index('if(Heal(ai,*plan,row,true))'))<core.index('if(IdleWand(ai,*plan,row,boss,tank,*phase))'),
 'needed heals stop Shoot before cast availability checks':'StopIdleWand(actor); // Healing demand preempts Shoot' in core and 'HealingPatient(actor,plan,row,false)||!wand' in core,
 'idle damage requires usable wand ordinary Shoot and immunity check':'GetWeaponForAttack(RANGED_ATTACK,true,true)' in core and 'ITEM_SUBCLASS_WEAPON_WAND' in core and 'HasSpell(AB_SPELL_SHOOT_WAND)' in core and 'enemy->IsImmuneToDamage' in core,
 'idle damage obeys ranged phase threat LOS and actual spell range':'!phase.ranged||!threatSafe' in core and 'Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(shoot->rangeIndex))' in core and '!actor->IsWithinLOSInMap(enemy)' in core,
 'idle damage cannot pull or chase':'!enemy->IsInCombat()' in core and 'actor->Attack(enemy,false)' in core and 'if(IdleWand(ai,*plan,row,boss,tank,*phase))' in core,
 'support spells preempt automatic wand fire':'if(spell&&!spell->IsAutoRepeatRangedSpell())me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL,true);' in core,

 'healers recover assigned patient range before opportunistic healing':core.index('if(RecoverHealingRange(ai,*plan,row,tank))')<core.index('if(Heal(ai,*plan,row,false))'),
 'target-relative routes pre-filter candidates at the target floor':'verify,target->GetPositionZ())' in core and core.count('verify,enemy->GetPositionZ())')==2 and 'verify,patient->GetPositionZ())' in core,
 'healing approach keeps cone clearance before reaching patient':'canTraverse&&!canTraverse(step)' in core and 'FindRoleRoute(plan,actor,reachablePatient,goal,waypoint,seconds,' in core,
 'healing range uses living patient fallback and ordinary LOS':'if(!patient||!patient->IsAlive())patient=fallback;' in core and 'HealingPositionAllowed(row,patient,p,2.f)' in core and 'range*range&&patient->IsWithinLOS(point.x,point.y,point.z)' in core,
 'cone geometry comes from loaded target radius and signed angle':'spell->EffectImplicitTargetA[i]==24||spell->EffectImplicitTargetB[i]==24' in core and 'Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))' in core and 'angle<0?3.14159265f:0' in core,
 'unsupported cone geometry rejected at Apply':'!cone||!std::isfinite(angle)||angle==0||std::fabs(angle)>6.28318531f' in core,
 'cone rules have bounded margin and explicit source':'rule.trigger==Trigger::Always&&rule.target==Target::Boss&&rule.radius>=1&&rule.radius<=10' in definition,
 'ordinary station cannot return to unsafe cone':'if(duty==1&&(!ConeStationAllowed(plan,row,p,point)||!HealerStationAllowed(plan,row,point)))' in core,
 'safe cone actors resume ordinary roles':'return false; // Safe healers and damage roles may keep doing their jobs.' in core,
 'cone escape and guidance share path solver':core.count('FindClearRoute(plan,actor,[&](Point p){return ConeStationAllowed(plan,row,actor,p);}')==2,
 'route candidates use bounded sorted lower bound':'ix<20' in core and 'iy<20' in core and 'if(candidate.first>=best)break;' in core and 'std::stable_sort(candidates.begin()' in core,
 'current boss victim does not rotate cone across raid':'if(!boss||boss->GetVictim()==actor)return true;' in core,
 'tanks acquire ordinary contact and threat before the hold station':'boss->GetVictim()!=actor || (!actor->CanReachWithMeleeAutoAttack(boss)&&!facingOutward)'  in core and 'GetCurrentMovementGeneratorType()!=CHASE_MOTION_TYPE' in core and core.index('GetCurrentMovementGeneratorType()!=CHASE_MOTION_TYPE')<core.index('tankStation=TankFacingStation(*plan,actor,boss,tankStation)'),

 'station hold requires retained threat near the authored outward station':'if(!stationaryMechanic&&boss->GetVictim()==actor&&!actor->CanReachWithMeleeAutoAttack(boss)&&' in core and 'sx*sx+sy*sy<=4.f&&dx*ox+dy*oy>0' in core and 'remaining<=2.f||sx*dx+sy*dy<=0' in core,
 'guidance cannot take control of the human':not re.search(r'\b(?:Move|Hold|Stop|CommanderRaidCast|TeleportTo|NearTeleportTo)\s*\(',guidance) and 'if(!row.manual)' in guidance,
 'guidance uses shared complete path solver':'EscapeRoute(plan,actor,lane,rule.radius,goal,advice.waypoint,seconds)' in guidance and 'EscapeRoute(plan,actor,lane,rule.radius,goal,waypoint,seconds)' in core,
 'impact deadline is separate from effect persistence':'uint32 remaining=0,impact=0' in core and 'it->impact=it->impact>diff?it->impact-diff:0' in core and 'std::min(castTime,rule.duration)' in core,
 'late movement remains best effort':'remaining/1000.f' not in core and 'advice.state=event.impact&&seconds+.25f<=event.impact/1000.f?2:4' in guidance,
 'server status carries actual boss and member state':'30 + (plan ? plan->rows.size() * 37' in core and 'boss->GetMaxHealth()' in core and 'actor->IsAlive()' in core and 'packet << row.guid << row.duty << row.target << actorFlags' in core,
 'manual guidance follows authored rule priority':'for(size_t ruleIndex=0;ruleIndex<plan.definition.rules.size();ruleIndex++)' in guidance and 'if(event.rule!=ruleIndex)continue' in guidance,

 'executor contains no Onyxia identity or spell branches':not re.search(r'Onyxia|onyxia|\b(?:10184|11262|249|6346|18392|18576)\b',core),
 'boss script has no encounter-specific observer hooks':'SuiCommanderRaid' not in read('src/scripts/kalimdor/dustwallow_marsh/onyxias_lair/boss_onyxia.cpp'),
 'shared spell-start and spell-go observations':'ObserveCast(m_casterUnit,m_spellInfo->Id,m_targets.getUnitTarget(),true,m_casttime)' in spell and 'ObserveCast(m_casterUnit,m_spellInfo->Id,m_targets.getUnitTarget(),false)' in spell,
 'triggered start observed before client cast-bar guard':spell.index('SuiCommanderRaid::ObserveCast(m_casterUnit,m_spellInfo->Id,m_targets.getUnitTarget(),true,m_casttime)')<spell.index('if (!m_IsTriggeredSpell)\n        {\n            // will show cast bar') and 'SuiCommanderRaid::ObserveCast' not in spell.split('void Spell::SendSpellStart()',1)[1].split('void Spell::SendSpellGo()',1)[0],
 'avoidance radius covers loaded spell effects':'if(Spells::GetSpellRadius(radius)>rule.radius)' in core,
 'flanks shorten within real reach and complete paths':'candidate<7' in core and 'CanReachWithMeleeAutoAttackAtPosition(boss,point.x,point.y,point.z)' in core and 'ex*ex+ey*ey+ez*ez>2.25f' in core,
 'flank cache tracks boss position and facing':'std::cos(facing-row.flankFacing)' in core and 'row.flankReady=false' in core,
 'unit observer runs after freeze guard':unit.index('if (IsSuiTacticallyFrozen())',unit.index('void Unit::Update('))<unit.index('SuiCommanderRaid::ObserveUnit'),
 'encounter arbitration precedes pending movement':ai.index('SuiCommanderRaid::Tick(this, diff)')<ai.index('ConsumePendingSuiRtsMove();',ai.index('void AiBotAI::UpdateAI')),
 'brain mutation yields to the encounter lease':'SuiCommanderRaid::Owns(me)' in bridge and 'BridgeSendEvent("ENCOUNTER_DROP",msgType)' in bridge and 'if(m_suiCommanderLine) SuiCommanderRaid::Yield(me)' in bridge,
 'explicit commander orders yield the assignment':'SuiCommanderRaid::Yield(pMember)' in possess,
 'group identity and command authority verified':'SuiCompanion::MayCommand(owner, actor)' in core and 'GetLeaderGuid()' in core,
 'no teleport execution':not re.search(r'\b(?:TeleportTo|NearTeleportTo)\s*\(',core),
 'normal learned spell validation':'me->HasSpell(id)' in core and 'CanTryToCastSpell(target,spell)' in core,
 'v4 packet exact length and bounded definition':'data.size()!=24+size_t(count)*71+length' in packet and 'length>32768' in packet and 'packet.version != 4' in core,
 'bounded rules teams and parser depth':'rules.size()<=64' in definition and 'teams.size()<=8' in definition and '++depth<=16' in definition,
 'unknown actions rejected':'else Need(false)' in definition,
 'human rows cannot acquire a bot lease':'!row.manual&&!row.yielded' in core and 'if (!row.manual && row.guid==actor->GetObjectGuid()) { plan=' in core,
 'human rows never stopped or prepared as bots':'if (row.manual || row.yielded) return' in core and 'if(row.manual)continue' in core,
 'manual identity is exactly the commander':'row.manual!=(row.guid==owner->GetObjectGuid())' in core and 'manual!=1' in core,
 'primary healing patient must exist':'row.role==3&&!unique.count(Raw(row.primary))' in core,
 'healing claims and interrupted-cast release':'Incoming(plan,p->GetObjectGuid(),horizon)' in core and '(it->kind==4||it->kind==18)&&it->remaining>500&&!caster->IsNonMeleeSpellCasted' in core,
 'emergency heals precede station movement':core.index('if(Heal(ai,*plan,row,true))')<core.index('if (Move(ai,*plan,row,air?row.air:row.ground,1))',core.index('if(Heal(ai,*plan,row,true))')),
 'support casts claim interrupt and dispel duties':'Claimed(plan,target->GetObjectGuid(),claimKind)' in core and 'IsValidDispelTarget(patient,spell)' in core,
 'multiple objectives require every observed death':'complete&=objective.seen&&objective.dead' in core and 'tankObjectives.count(objective)' in core,
 'add pickup scans every matching creature':'GetCreatureListWithEntryInGrid(candidates,entry,100)' in core and 'if(!tanked)distance-=60' in core,
 'hazard routes cannot re-enter and score actual path length':'if(escaped&&!clear)' in core and 'score>=best' in core and 'best=score;bestTravel=travel;goal=q;waypoint=q' in core,
 'ordinary power cost and learned heal fallbacks':'Spell::CalculatePowerCost(spell,actor,nullptr,nullptr,false)' in core and 'PLAYERSPELL_REMOVED' in core,
 'temporary class role is restored':'ai->UpdateInCombatAI();ai->m_role=oldRole' in core,
 'no autopull on arm':'row.duty=11; // Arming never walks' in core,
 'both scripted paladin heals are available':'spell->SpellName[0]=="Flash of Light"' in core,
 'healing ranks use deficit and ordinary bonuses':'SpellHealingBonusDone(patient,spell,index,base,HEAL)' in core and 'choice.useful' in core and 'a.adequate!=b.adequate' in core,
 'ranged roles reach the ordinary spec rotation':'row.role==5?ROLE_RANGE_DPS' in core and 'CommanderRaidCast(target,row.damage)' not in core,
 'threat hold cancels spells and pet casts':'actor->InterruptNonMeleeSpells(false);actor->AttackStop();' in core and 'pet->InterruptNonMeleeSpells(false);pet->AttackStop();' in core,
 'raid rotations do not fabricate ammunition':'SPELL_FAILED_NO_AMMO) && !SuiCommanderRaid::Owns(me)' in spec,
 'raid owns add CC and temporary role':'if (SuiCommanderRaid::Owns(me)) return nullptr;' in spec and 'if (SuiCommanderRaid::Owns(me)) return GetRole();' in spec,
 'instance and map binding':'plan.instance!=boss->GetInstanceId()' in core and 'plan.definition.map!=boss->GetMapId()' in core,
}
for name,passed in checks.items():print(('PASS ' if passed else 'FAIL ')+name)
if not all(checks.values()):raise SystemExit(1)
print(str(len(checks))+' source contracts passed. Core compilation and live behavior remain unverified.')
