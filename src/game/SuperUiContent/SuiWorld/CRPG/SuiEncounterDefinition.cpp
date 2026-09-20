#include "SuiEncounterDefinition.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace
{
    using Json=nlohmann::json;
    void Need(bool value) { if(!value) throw std::runtime_error("Invalid encounter definition"); }
    void Keys(Json const& j,std::initializer_list<char const*> keys,std::initializer_list<char const*> optional={})
    {
        Need(j.is_object());
        size_t count=keys.size();
        for(auto key:keys) Need(j.count(key)!=0);
        for(auto key:optional) count+=j.count(key);
        Need(j.size()==count);
    }
    uint32 Number(Json const& j,uint32 maximum)
    { Need(j.is_number_integer()); auto value=j.get<int64>();Need(value>=0 && uint64(value)<=maximum);return uint32(value); }
    int Signed(Json const& j,int minimum,int maximum)
    { Need(j.is_number_integer());auto value=j.get<int64>();Need(value>=minimum && value<=maximum);return int(value); }
    float Real(Json const& j,float low,float high)
    { Need(j.is_number());auto v=j.get<double>();Need(std::isfinite(v) && v>=low && v<=high);return float(v); }
    bool Flag(Json const& j) { Need(j.is_boolean());return j.get<bool>(); }
    std::string Text(Json const& j,size_t limit)
    { Need(j.is_string());auto s=j.get<std::string>();Need(!s.empty() && s.size()<=limit);return s; }
    SuiEncounter::Point ReadPoint(Json const& j)
    { Need(j.is_array() && j.size()==3);return {Real(j[0],-100000,100000),Real(j[1],-100000,100000),Real(j[2],-100000,100000)}; }
    std::vector<uint32> Ids(Json const& j,size_t limit)
    { Need(j.is_array() && j.size()<=limit);std::vector<uint32> out;for(auto const& x:j) {auto v=Number(x,0x7fffffff);Need(v!=0);out.push_back(v);}return out; }
}
bool SuiEncounter::Inside(Definition const& d,Point p)
{
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z) &&
        p.x>=d.low.x&&p.x<=d.high.x&&p.y>=d.low.y&&p.y<=d.high.y&&p.z>=d.low.z&&p.z<=d.high.z;
}
bool SuiEncounter::Parse(std::string const& text,uint32 map,uint32 boss,Definition& out)
{
    try
    {
        Need(!text.empty() && text.size()<=32768);
        // Bound nesting before entering the parser (including hostile packet input).
        int depth=0;bool quoted=false,escaped=false;
        for(char c:text) { if(quoted) {if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
            else if(c=='"')quoted=true;else if(c=='{'||c=='[')Need(++depth<=16);else if(c=='}'||c==']')Need(--depth>=0); }
        Need(depth==0 && !quoted);
        Json j=Json::parse(text);Definition d;
        if(!j.count("requiredAdds"))j["requiredAdds"]=Json::array();
        if(!j.count("addPolicy"))j["addPolicy"]="split";
        Keys(j,{"schema","id","name","mapId","bossEntry","bounds","tankAnchor","teams","addTanksPerTeam","healersPerTeam","healerCount","addEntries","phases","rules","immuneSchools","objectives","coverage","requiredAdds","addPolicy"});
        Need(Number(j.at("schema"),1)==1);d.id=Text(j.at("id"),64);d.name=Text(j.at("name"),96);
        for(unsigned char c:d.id)Need((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_');
        d.map=Number(j.at("mapId"),0x7fffffff);d.boss=Number(j.at("bossEntry"),0x7fffffff);Need(d.map==map&&d.boss==boss&&boss!=0);
        d.immuneSchools=Number(j.at("immuneSchools"),127);d.coverage=Text(j.at("coverage"),16);
        Need(d.coverage=="authored"||d.coverage=="basic");
        d.objectives=Ids(j.at("objectives"),8);if(d.objectives.empty())d.objectives.push_back(d.boss);
        std::set<uint32> objectives(d.objectives.begin(),d.objectives.end());
        Need(objectives.size()==d.objectives.size()&&objectives.count(d.boss));
        auto const& bounds=j.at("bounds");Keys(bounds,{"min","max"});d.low=ReadPoint(bounds.at("min"));d.high=ReadPoint(bounds.at("max"));
        Need(d.high.x>d.low.x&&d.high.y>d.low.y&&d.high.z>d.low.z&&d.high.x-d.low.x<=400&&d.high.y-d.low.y<=400&&d.high.z-d.low.z<=400);
        d.tank=ReadPoint(j.at("tankAnchor"));Need(Inside(d,d.tank));
        auto const& teams=j.at("teams");Need(teams.is_array()&&!teams.empty()&&teams.size()<=8);
        for(auto const& t:teams) {Keys(t,{"name","anchor"});Team team{Text(t.at("name"),24),ReadPoint(t.at("anchor"))};Need(Inside(d,team.anchor));d.teams.push_back(team);}
        d.addTanks=Number(j.at("addTanksPerTeam"),4);d.healers=Number(j.at("healersPerTeam"),8);d.healerCount=Number(j.at("healerCount"),40);
        d.adds=Ids(j.at("addEntries"),32);
        d.addPolicy=Text(j.at("addPolicy"),16);Need(d.addPolicy=="split"||d.addPolicy=="focus"||d.addPolicy=="balance"||d.addPolicy=="hold");
        auto const& required=j.at("requiredAdds");Need(required.is_array()&&required.size()<=32);std::set<uint32> requiredIds;
        for(auto const& r:required){Keys(r,{"entry","count"});AddRequirement requirement{Number(r.at("entry"),0x7fffffff),Number(r.at("count"),64)};
            Need(requiredIds.insert(requirement.entry).second&&std::find(d.adds.begin(),d.adds.end(),requirement.entry)!=d.adds.end());d.requiredAdds.push_back(requirement);}

        auto const& phases=j.at("phases");Need(phases.is_array()&&!phases.empty()&&phases.size()<=16);
        std::set<uint8> phaseIds;bool fallback=false;
        for(auto const& p:phases)
        {
            Keys(p,{"id","name","priority","healthMin","healthMax","airborne","aura","alternate","melee","ranged","threatRatio"});Phase phase;
            phase.id=Number(p.at("id"),255);Need(phase.id&&phaseIds.insert(phase.id).second);Text(p.at("name"),64);
            phase.priority=Signed(p.at("priority"),-10000,10000);phase.healthMin=Real(p.at("healthMin"),0,100);phase.healthMax=Real(p.at("healthMax"),0,100);Need(phase.healthMin<phase.healthMax);
            phase.airborne=Signed(p.at("airborne"),-1,1);phase.aura=Number(p.at("aura"),0x7fffffff);phase.alternate=Flag(p.at("alternate"));
            phase.melee=Flag(p.at("melee"));phase.ranged=Flag(p.at("ranged"));phase.threatRatio=Real(p.at("threatRatio"),0,1);
            fallback|=phase.healthMin==0&&phase.healthMax==100&&phase.airborne==-1&&phase.aura==0;d.phases.push_back(phase);
        }
        Need(fallback);std::sort(d.phases.begin(),d.phases.end(),[](Phase const& a,Phase const& b){return a.priority!=b.priority?a.priority>b.priority:a.id<b.id;});
        auto const& rules=j.at("rules");Need(rules.is_array()&&rules.size()<=64);std::set<std::string> ruleIds;
        for(auto const& r:rules)
        {
            Keys(r,{"id","priority","action","trigger","spells","phase","roles","radius","durationMs","target","spell","missingAura","pointSpells","station","toggle"},{"triggerRadius","reserveCasters"});Rule rule;
            rule.id=Text(r.at("id"),64);Need(ruleIds.insert(rule.id).second);rule.priority=Signed(r.at("priority"),-10000,10000);
            auto action=Text(r.at("action"),24),trigger=Text(r.at("trigger"),24),target=Text(r.at("target"),24);
            if(action=="isolate")rule.action=Action::Isolate;else if(action=="avoidCones")rule.action=Action::AvoidCones;else if(action=="avoidPoints")rule.action=Action::AvoidPoints;else if(action=="spread")rule.action=Action::Spread;
            else if(action=="stack")rule.action=Action::Stack;else if(action=="move")rule.action=Action::Move;
            else if(action=="stopDamage")rule.action=Action::StopDamage;else if(action=="cast")rule.action=Action::Cast;else Need(false);
            if(trigger=="always")rule.trigger=Trigger::Always;else if(trigger=="castStart")rule.trigger=Trigger::CastStart;
            else if(trigger=="castGo")rule.trigger=Trigger::CastGo;else if(trigger=="bossAura")rule.trigger=Trigger::BossAura;
            else if(trigger=="selfAura")rule.trigger=Trigger::SelfAura;else if(trigger=="memberAura")rule.trigger=Trigger::MemberAura;else if(trigger=="bossNear")rule.trigger=Trigger::BossNear;else Need(false);
            if(target=="self")rule.target=Target::Self;else if(target=="boss")rule.target=Target::Boss;
            else if(target=="tank")rule.target=Target::Tank;else if(target=="marked")rule.target=Target::Marked;else if(target=="tankHealer")rule.target=Target::TankHealer;else Need(false);
            rule.spells=Ids(r.at("spells"),64);rule.points=Ids(r.at("pointSpells"),64);rule.phase=Number(r.at("phase"),255);Need(!rule.phase||phaseIds.count(rule.phase));
            rule.roles=Number(r.at("roles"),62);Need(rule.roles&&!(rule.roles&~62u));rule.radius=Real(r.at("radius"),0,100);
            rule.duration=Number(r.at("durationMs"),60000);rule.spell=Number(r.at("spell"),0x7fffffff);rule.missingAura=Flag(r.at("missingAura"));
            rule.station=ReadPoint(r.at("station"));rule.toggle=Number(r.at("toggle"),4);Need(rule.toggle!=3);
            rule.triggerRadius=r.count("triggerRadius")?Real(r.at("triggerRadius"),0,100):0;
            rule.reserveCasters=r.count("reserveCasters")?Number(r.at("reserveCasters"),8):0;
            Need(!rule.reserveCasters||(rule.action==Action::Cast&&rule.trigger==Trigger::Always&&rule.target!=Target::Self&&rule.target!=Target::Marked));
            Need(rule.trigger==Trigger::Always||rule.trigger==Trigger::BossNear||!rule.spells.empty());
            Need(rule.trigger!=Trigger::BossNear||(rule.action==Action::AvoidPoints&&rule.triggerRadius>=1));
            bool event=rule.trigger==Trigger::CastStart||rule.trigger==Trigger::CastGo;Need(!event||rule.duration>0);Need(rule.target!=Target::Marked||event||rule.trigger==Trigger::MemberAura);Need(rule.trigger!=Trigger::MemberAura||((rule.action==Action::Spread||rule.action==Action::Isolate)&&rule.target==Target::Marked&&rule.radius>=1));
            Need(rule.action!=Action::AvoidCones||(!rule.spells.empty()&&rule.trigger==Trigger::Always&&rule.target==Target::Boss&&rule.radius>=1&&rule.radius<=10));
            Need(rule.target!=Target::TankHealer||(rule.action==Action::Cast&&rule.trigger==Trigger::Always));
            Need(rule.action!=Action::Isolate||((event||rule.trigger==Trigger::MemberAura)&&rule.target==Target::Marked&&rule.radius>=1&&(!rule.missingAura||rule.spell)));
            Need(rule.action!=Action::AvoidPoints||(!rule.points.empty()&&rule.radius>=1));Need(rule.action!=Action::Cast||rule.spell!=0);
            Need(rule.action!=Action::Move||Inside(d,rule.station));d.rules.push_back(rule);
        }
        std::stable_sort(d.rules.begin(),d.rules.end(),[](Rule const& a,Rule const& b){return a.priority>b.priority;});
        out=std::move(d);return true;
    }
    catch(std::exception const&) {return false;}
}
