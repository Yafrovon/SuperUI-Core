#ifndef MANGOS_SUI_COMMANDER_RAID_H
#define MANGOS_SUI_COMMANDER_RAID_H
#include "Common.h"
class AiBotAI;
class Unit;
class Creature;
namespace SuiCommanderRaid
{
    // Returns true only while this actor's encounter executor owns its tick.
    bool Tick(AiBotAI* ai, uint32 diff);
    void Yield(Unit* actor);
    bool Owns(Unit* actor);
    bool Watches(uint32 entry);
    void ObserveUnit(Unit* boss,uint32 diff);
    // True for a temporary summon whose summoner is a creature this executor already watches.
    // Its own Update then routes it through ObserveUnit, which is how a summoned add becomes
    // visible to the plan without any per-tick grid sweep.
    bool WatchesSummonerOf(Creature* creature);
    void ObserveCast(Unit* boss,uint32 spell,Unit* target,bool start,uint32 castTime=0);
    // True when the armed plan keeps this melee member off the enemy (a held required add):
    // a class splash (cleave, blade flurry, consecration) must not reach it either.
    bool ForbidsSplash(Unit* actor,Unit* enemy);
}
#endif
