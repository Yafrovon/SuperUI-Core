#ifndef MANGOS_SUI_COMMANDER_RAID_H
#define MANGOS_SUI_COMMANDER_RAID_H
#include "Common.h"
class AiBotAI;
class Unit;
namespace SuiCommanderRaid
{
    // Returns true only while this actor's encounter executor owns its tick.
    bool Tick(AiBotAI* ai, uint32 diff);
    void Yield(Unit* actor);
    bool Owns(Unit* actor);
    bool Watches(uint32 entry);
    void ObserveUnit(Unit* boss,uint32 diff);
    void ObserveCast(Unit* boss,uint32 spell,Unit* target,bool start,uint32 castTime=0);
}
#endif
