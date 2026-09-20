#ifndef MANGOS_SUI_ENCOUNTER_DEFINITION_H
#define MANGOS_SUI_ENCOUNTER_DEFINITION_H
#include "Common.h"
#include <string>
#include <vector>
namespace SuiEncounter
{
    struct Point { float x=0,y=0,z=0; };
    struct Team { std::string name; Point anchor; };
    struct Phase
    {
        uint8 id=0; int priority=0, airborne=-1;
        float healthMin=0,healthMax=100,threatRatio=.8f;
        uint32 aura=0; bool alternate=false,melee=true,ranged=true;
    };
    enum class Action { AvoidCones, AvoidPoints, Spread, Stack, Move, StopDamage, Cast, Isolate };
    enum class Trigger { Always, CastStart, CastGo, BossAura, SelfAura, MemberAura, BossNear };
    enum class Target { Self, Boss, Tank, Marked, TankHealer };
    struct Rule
    {
        std::string id; int priority=0;
        Action action=Action::Move; Trigger trigger=Trigger::Always; Target target=Target::Self;
        uint8 phase=0,toggle=0; uint32 roles=62,spell=0,duration=0;
        uint32 reserveCasters=0;
        float radius=0,triggerRadius=0; bool missingAura=false; Point station;
        std::vector<uint32> spells,points;
    };
    struct AddRequirement {uint32 entry=0,count=0;};
    struct Definition
    {
        std::string id,name,coverage; uint32 map=0,boss=0,immuneSchools=0;
        Point low,high,tank; uint32 addTanks=0,healers=0,healerCount=0;
        std::vector<Team> teams; std::vector<Phase> phases;
        std::vector<Rule> rules; std::vector<uint32> adds,objectives;
        std::vector<AddRequirement> requiredAdds; std::string addPolicy="split";
    };
    bool Inside(Definition const& d, Point p);
    // Bounded data only: no scripts, expressions, SQL, commands or file references.
    bool Parse(std::string const& text,uint32 map,uint32 boss,Definition& out);
}
#endif
