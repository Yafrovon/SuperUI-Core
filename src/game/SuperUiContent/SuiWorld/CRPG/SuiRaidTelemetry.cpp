/*
 * SuiRaidTelemetry.cpp — see SuiRaidTelemetry.h. Line endings: LF (C++ repo convention).
 */

#include "SuiRaidTelemetry.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "Config/Config.h"
#include <fstream>
#include "Player.h"
#include "SpellEntry.h"
#include "Timer.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::atomic<Group const*> gGroup{ nullptr };
    std::mutex gLock;

    struct Hit
    {
        uint32 at;
        std::string source;
        uint32 amount;
        uint32 healthAfter;
    };

    struct Fight
    {
        uint32 startedAt = 0;
        std::map<std::string, uint64> damageBySource;     // "Molten Giant / Stomp"
        std::map<std::string, uint64> damageByVictim;     // "Sealnub (rogue)"
        std::map<std::string, uint64> healingByHealer;
        std::map<std::string, uint64> dealtByMember;      // raid damage done
        std::map<std::string, uint64> dealtToTarget;      // by creature name
        uint64 dealtTotal = 0;
        std::map<std::string, std::map<std::string, uint64>> dealtByAbility;   // member -> ability
        std::map<std::string, uint32> hitsByMember;                          // landed hits
        std::map<std::string, std::map<std::string, uint32>> castsByMember;   // "spell:outcome" -> n
        std::map<std::string, std::array<uint32, 6>> ticksByMember;
        std::map<ObjectGuid, std::deque<Hit>> recent;     // last hits per member
        std::map<ObjectGuid, std::deque<std::pair<uint32, uint32>>> healsIn;   // (time, amount)
        std::vector<std::string> deaths;
        std::set<ObjectGuid> hitByRaid;      // creatures the raid has struck this fight
        std::set<ObjectGuid> hitTheRaid;     // creatures that have struck the raid this fight
    } gFight;

    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return "warrior";
            case CLASS_PALADIN: return "paladin";
            case CLASS_HUNTER: return "hunter";
            case CLASS_ROGUE: return "rogue";
            case CLASS_PRIEST: return "priest";
            case CLASS_SHAMAN: return "shaman";
            case CLASS_MAGE: return "mage";
            case CLASS_WARLOCK: return "warlock";
            case CLASS_DRUID: return "druid";
            default: return "?";
        }
    }

    Player* Member(Unit* unit)
    {
        Group const* group = gGroup.load(std::memory_order_relaxed);
        if (!group || !unit || !unit->IsPlayer())
            return nullptr;
        Player* player = static_cast<Player*>(unit);
        return player->GetGroup() == group ? player : nullptr;
    }

    std::string SourceName(Unit* attacker, SpellEntry const* spell)
    {
        std::string name = attacker ? attacker->GetName() : "environment";
        name += " / ";
        name += spell ? spell->SpellName[0] : std::string("melee");
        return name;
    }

    std::string MemberLabel(Player const* p)
    {
        return std::string(p->GetName()) + " (" + ClassName(p->GetClass()) + ")";
    }

    template <typename Map>
    std::string Top(Map const& values, size_t count)
    {
        std::vector<std::pair<std::string, uint64>> rows(values.begin(), values.end());
        std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        std::ostringstream out;
        for (size_t i = 0; i < rows.size() && i < count; ++i)
            out << (i ? "; " : "") << rows[i].first << " " << rows[i].second;
        return out.str();
    }
}

void SuiRaidTelemetry::Track(Group const* group)
{
    gGroup.store(group, std::memory_order_relaxed);
}

namespace
{
    struct Pulse { float radius = 0.0f; uint32 last = 0; uint32 prev = 0; uint32 burst = 0; };
    std::map<uint32, float> gBurstInterval;   // creature entry -> smoothed ms between burst starts
    std::map<uint32, uint32> gBurstMaxHit;    // creature entry -> biggest single burst hit on a member
    std::map<ObjectGuid, Pulse> gPulses;
    std::map<uint32, float> gPulseAuras;   // aura spell seen on a creature while it pulsed -> radius
    std::map<uint32, float> gPulseEntries; // creature entry seen pulsing -> radius
    std::map<uint32, float> gConeEntries;  // creature entry seen breathing a frontal cone -> reach
    struct Burst { uint32 start = 0; uint32 seen = 0; };
    std::map<ObjectGuid, Burst> gBursts;   // creature -> when its current/last burst began
    std::mutex gPulseLock;
    bool gPulseLoaded = false;

    // Learned pulses survive a restart: "aura radius" lines beside mangosd.conf.
    std::string PulsePath()
    {
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-autopilot-pulses.txt";
    }

    void LoadPulses()
    {
        if (gPulseLoaded)
            return;
        gPulseLoaded = true;
        std::ifstream in(PulsePath());
        uint32 aura = 0;
        float radius = 0.0f;
        while (in >> aura >> radius)
            gPulseAuras[aura] = radius;
        std::ifstream kinds(PulsePath() + ".entries");
        uint32 entry = 0;
        while (kinds >> entry >> radius)
            gPulseEntries[entry] = radius;
        std::ifstream cones(PulsePath() + ".cones");
        while (cones >> entry >> radius)
            gConeEntries[entry] = radius;
        std::ifstream rhythm(PulsePath() + ".rhythm");
        while (rhythm >> entry >> radius)
            gBurstInterval[entry] = radius;   // last line per entry wins
    }

    // Reach of a spell's cone in front of its caster, else 0.
    float ConeRadius(SpellEntry const* spell)
    {
        float radius = 0.0f;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            uint32 const a = spell->EffectImplicitTargetA[i], b = spell->EffectImplicitTargetB[i];
            if (a != TARGET_ENUM_UNITS_ENEMY_IN_CONE_24 && b != TARGET_ENUM_UNITS_ENEMY_IN_CONE_24 &&
                a != TARGET_ENUM_UNITS_ENEMY_IN_CONE_54 && b != TARGET_ENUM_UNITS_ENEMY_IN_CONE_54)
                continue;
            if (SpellRadiusEntry const* entry = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                radius = std::max(radius, Spells::GetSpellRadius(entry));
        }
        return radius;
    }

    // Caller holds gPulseLock.
    void NotePulseEntry(Unit const* creature, float radius)
    {
        if (gPulseEntries.emplace(creature->GetEntry(), radius).second)
        {
            std::ofstream(PulsePath() + ".entries", std::ios::app) << creature->GetEntry() << ' ' << radius << '\n';
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] PULSE kind learned: %s (entry %u) pulses %.0f yd",
                creature->GetName(), creature->GetEntry(), radius);
        }
    }

    // Radius of a spell's damage area when that area is centred on its caster, else 0.
    float SelfCentredRadius(SpellEntry const* spell)
    {
        float radius = 0.0f;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (spell->Effect[i] != SPELL_EFFECT_SCHOOL_DAMAGE)
                continue;
            uint32 const a = spell->EffectImplicitTargetA[i], b = spell->EffectImplicitTargetB[i];
            bool const centred = (a == TARGET_LOCATION_CASTER_DEST || a == TARGET_LOCATION_CASTER_SRC ||
                a == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC) &&
                (b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC || b == TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC || b == 0);
            if (!centred)
                continue;
            if (SpellRadiusEntry const* entry = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
                radius = std::max(radius, Spells::GetSpellRadius(entry));
        }
        return radius;
    }
}

float SuiRaidTelemetry::PulseRadius(Unit const* creature)
{
    if (!creature)
        return 0.0f;
    std::lock_guard<std::mutex> guard(gPulseLock);
    LoadPulses();
    // Learned: an aura this creature wore while pulsing before - the pulse comes with it.
    if (!gPulseAuras.empty())
        for (auto const& entry : creature->GetSpellAuraHolderMap())
            if (entry.second)
            {
                auto learned = gPulseAuras.find(entry.second->GetId());
                if (learned != gPulseAuras.end())
                {
                    NotePulseEntry(creature, learned->second);
                    uint32 const now = WorldTimer::getMSTime();
                    Burst& b = gBursts[creature->GetObjectGuid()];
                    if (!b.seen || now - b.seen > 3000)
                        b.start = now;   // a new burst (the aura was not on it a moment ago)
                    b.seen = now;
                    return learned->second;
                }
            }
    auto it = gPulses.find(creature->GetObjectGuid());
    if (it == gPulses.end())
        return 0.0f;
    uint32 const now = WorldTimer::getMSTime();
    Pulse const& p = it->second;
    if (now - p.last > 1500 || !p.prev || p.last - p.prev > 2500)
        return 0.0f;
    return p.radius;
}

namespace
{
    struct Forecast { uint32 map; uint32 instance; float x, y, r; uint32 until; uint32 chain; };
    std::vector<Forecast> gForecasts;
    std::mutex gForecastLock;
}

namespace
{
    std::map<uint32, float> gSplashByEntry;
    std::mutex gSplashLock;
}

float SuiRaidTelemetry::EntrySplashRadius(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gSplashLock);
    auto it = gSplashByEntry.find(entry);
    return it == gSplashByEntry.end() ? 0.0f : it->second;
}

void SuiRaidTelemetry::OnCreatureCast(Unit* caster, SpellEntry const* spell)
{
    if (!caster || !spell || !caster->IsCreature() || !caster->GetMap()->IsDungeon())
        return;
    // a damaging burst around the unit it is cast at
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spell->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE &&
            (spell->EffectImplicitTargetA[i] == TARGET_LOCATION_CASTER_TARGET_POSITION ||
             spell->EffectImplicitTargetA[i] == TARGET_UNIT_ENEMY) &&
            spell->EffectImplicitTargetB[i] == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC)
            if (SpellRadiusEntry const* r = sSpellRadiusStore.LookupEntry(spell->EffectRadiusIndex[i]))
            {
                float const radius = Spells::GetSpellRadius(r);
                if (radius > 0.0f && radius <= 15.0f)
                {
                    std::lock_guard<std::mutex> guard(gSplashLock);
                    float& known = gSplashByEntry[caster->GetEntry()];
                    if (radius > known)
                    {
                        known = radius;
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] SPLASH learned: %s (%u) bursts %.0f yd around its target (%s)",
                            caster->GetName(), caster->GetEntry(), radius, spell->SpellName[0].c_str());
                    }
                }
            }
    bool atDb = false;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spell->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE &&
            (spell->EffectImplicitTargetA[i] == TARGET_LOCATION_DATABASE || spell->EffectImplicitTargetB[i] == TARGET_LOCATION_DATABASE))
            atDb = true;
    if (!atDb)
        return;
    uint32 const now = WorldTimer::getMSTime();
    std::lock_guard<std::mutex> guard(gForecastLock);
    gForecasts.erase(std::remove_if(gForecasts.begin(), gForecasts.end(),
        [now](Forecast const& f) { return now >= f.until; }), gForecasts.end());
    // Each link of the chain is cast in turn: a link whose own impact point is forecast already
    // belongs to a chain seen at its start.
    if (SpellTargetPosition const* first = sSpellMgr.GetSpellTargetPosition(spell->Id))
        for (Forecast const& f : gForecasts)
            if (f.map == caster->GetMapId() && f.instance == caster->GetInstanceId() &&
                std::fabs(f.x - first->x) < 1.0f && std::fabs(f.y - first->y) < 1.0f)
                return;
    uint32 id = spell->Id;
    uint32 added = 0;
    for (uint32 k = 0; k < 40 && id; ++k)
    {
        SpellEntry const* s = sSpellMgr.GetSpellEntry(id);
        if (!s)
            break;
        float radius = 0.0f;
        uint32 next = 0;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (s->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE)
                if (SpellRadiusEntry const* r = sSpellRadiusStore.LookupEntry(s->EffectRadiusIndex[i]))
                    radius = std::max(radius, Spells::GetSpellRadius(r));
            if (s->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL && s->EffectTriggerSpell[i])
                next = s->EffectTriggerSpell[i];
        }
        if (SpellTargetPosition const* pos = sSpellMgr.GetSpellTargetPosition(id))
            if (pos->mapId == caster->GetMapId() && radius > 0.0f)
            {
                gForecasts.push_back({caster->GetMapId(), caster->GetInstanceId(), pos->x, pos->y,
                    radius, now + 15000, spell->Id});
                ++added;
            }
        if (next == id)
            break;
        id = next;
    }
    if (added)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] FORECAST %s: %u impacts from %s (%u) for 15 s",
            caster->GetName(), added, spell->SpellName[0].c_str(), spell->Id);
}

void SuiRaidTelemetry::ForecastHazards(Unit const* around, float range, std::vector<std::tuple<float, float, float>>& out)
{
    uint32 const now = WorldTimer::getMSTime();
    std::lock_guard<std::mutex> guard(gForecastLock);
    for (Forecast const& f : gForecasts)
        if (now < f.until && f.map == around->GetMapId() && f.instance == around->GetInstanceId() &&
            around->GetDistance2d(f.x, f.y) < range + f.r)
            out.emplace_back(f.x, f.y, f.r);
}

uint32 SuiRaidTelemetry::EntryBurstMaxHit(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gPulseLock);
    auto it = gBurstMaxHit.find(entry);
    return it == gBurstMaxHit.end() ? 0 : it->second;
}

uint32 SuiRaidTelemetry::EntryBurstInterval(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gPulseLock);
    auto it = gBurstInterval.find(entry);
    return it == gBurstInterval.end() ? 0 : uint32(it->second);
}

uint32 SuiRaidTelemetry::PulseAge(Unit const* creature)
{
    if (!creature)
        return 0;
    std::lock_guard<std::mutex> guard(gPulseLock);
    auto it = gBursts.find(creature->GetObjectGuid());
    if (it == gBursts.end() || !it->second.start)
        return 0;
    return WorldTimer::getMSTime() - it->second.start;
}

float SuiRaidTelemetry::EntryConeRadius(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gPulseLock);
    LoadPulses();
    auto it = gConeEntries.find(entry);
    return it == gConeEntries.end() ? 0.0f : it->second;
}

float SuiRaidTelemetry::EntryPulseRadius(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gPulseLock);
    LoadPulses();
    auto it = gPulseEntries.find(entry);
    return it == gPulseEntries.end() ? 0.0f : it->second;
}

namespace
{
    struct SchoolHits { uint32 mask[6] = {0, 0, 0, 0, 0, 0}; uint32 at[6] = {0, 0, 0, 0, 0, 0}; uint8 next = 0; };
    std::map<ObjectGuid, SchoolHits> gSchoolHits;
    std::mutex gSchoolLock;
}

uint32 SuiRaidTelemetry::RecentDamageSchools(Unit const* victim)
{
    if (!victim)
        return 0;
    std::lock_guard<std::mutex> guard(gSchoolLock);
    auto it = gSchoolHits.find(victim->GetObjectGuid());
    if (it == gSchoolHits.end())
        return 0;
    uint32 const now = WorldTimer::getMSTime();
    uint32 mask = 0;
    for (uint8 i = 0; i < 6; ++i)
        if (it->second.at[i] && now - it->second.at[i] <= 6000)
            mask |= it->second.mask[i];
    return mask;
}

void SuiRaidTelemetry::OnDamage(Unit* attacker, Unit* victim, uint32 damage, SpellEntry const* spell)
{
    // What school is hurting this member (a protection potion answers it).
    if (spell && damage && victim && victim->IsPlayer() && attacker && attacker->IsCreature())
    {
        std::lock_guard<std::mutex> guard(gSchoolLock);
        SchoolHits& h = gSchoolHits[victim->GetObjectGuid()];
        h.mask[h.next] = uint32(spell->GetSpellSchoolMask());
        h.at[h.next] = WorldTimer::getMSTime();
        h.next = uint8((h.next + 1) % 6);
    }
    // Cones: a creature's frontal cone landing on a raid member - its kind breathes.
    if (attacker && attacker->IsCreature() && spell && victim && victim->IsPlayer() && Member(victim))
        if (float const cone = ConeRadius(spell))
        {
            std::lock_guard<std::mutex> guard(gPulseLock);
            LoadPulses();
            if (gConeEntries.emplace(attacker->GetEntry(), cone).second)
            {
                std::ofstream(PulsePath() + ".cones", std::ios::app) << attacker->GetEntry() << ' ' << cone << '\n';
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] CONE learned: %s (entry %u) breathes %.0f yd in front",
                    attacker->GetName(), attacker->GetEntry(), cone);
            }
        }
    // Pulses: a creature's self-centred area spell landing on a raid member.
    if (attacker && attacker->IsCreature() && spell && victim && victim->IsPlayer() && Member(victim))
        if (float const radius = SelfCentredRadius(spell))
        {
            std::lock_guard<std::mutex> guard(gPulseLock);
            Pulse& p = gPulses[attacker->GetObjectGuid()];
            uint32 const now = WorldTimer::getMSTime();
            uint32& maxHit = gBurstMaxHit[attacker->GetEntry()];
            maxHit = std::max(maxHit, damage);
            if (now - p.last > 400)   // one pulse hits many members in the same tick
            {
                // a burst starts after a quiet 2.5 s: its interval to the last start is the rhythm
                if (!p.last || now - p.last > 2500)
                {
                    if (p.burst && now - p.burst < 60000)
                    {
                        LoadPulses();
                        float& iv = gBurstInterval[attacker->GetEntry()];
                        float const before = iv;
                        iv = iv > 0.0f ? iv * 0.7f + float(now - p.burst) * 0.3f : float(now - p.burst);
                        // kept across restarts when it first appears or moves by a quarter
                        if (before <= 0.0f || std::fabs(iv - before) > before * 0.25f)
                            std::ofstream(PulsePath() + ".rhythm", std::ios::app) << attacker->GetEntry() << ' ' << uint32(iv) << '\n';
                    }
                    p.burst = now;
                }
                p.prev = p.last;
                p.last = now;
                // Any area around itself that reached the raid: casters keep out of it from now on
                // (an arcane explosion every few seconds is not a pulse, but it is the same circle).
                NotePulseEntry(attacker, radius);
                // Pulsing (two ticks in 2.5 s): remember the periodic auras it wears right now.
                if (p.prev && p.last - p.prev <= 2500)
                    for (auto const& entry : attacker->GetSpellAuraHolderMap())
                        if (SpellAuraHolder const* h = entry.second)
                            if (h->GetAuraMaxDuration() > 0 && h->GetAuraMaxDuration() <= 30000)   // a cast, not a standing shield
                            for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                                if (h->GetSpellProto()->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_TRIGGER_SPELL)
                                    if (LoadPulses(), gPulseAuras.emplace(h->GetId(), radius).second &&
                                        (std::ofstream(PulsePath(), std::ios::app) << h->GetId() << ' ' << radius << '\n', true))
                                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                            "[TELEMETRY] PULSE learned: %s's aura %u comes with a %.0f yd pulse",
                                            attacker->GetName(), h->GetId(), radius);
            }
            p.radius = radius;
        }
    // Damage the raid deals: the attacker (or its pet's owner) is a member, the victim a creature.
    if (attacker && victim && victim->IsCreature() && damage)
    {
        Player* dealer = Member(attacker->GetCharmerOrOwnerPlayerOrPlayerItself());
        // Who breaks a crowd control (a sheep hit by a pet, a flurry, a damage-over-time tick).
        if (dealer && victim->HasBreakableByDamageCrowdControlAura())
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] CC BROKEN %s (guid %u) by %s%s with %s for %u",
                victim->GetName(), victim->GetGUIDLow(), dealer->GetName(), attacker != dealer ? "'s pet" : "",
                spell ? spell->SpellName[0].c_str() : "melee", damage);
        if (dealer)
        {
            std::lock_guard<std::mutex> guard(gLock);
            if (!gFight.startedAt)
                gFight.startedAt = WorldTimer::getMSTime();
            gFight.dealtByMember[MemberLabel(dealer)] += damage;
            std::string ability = spell ? spell->SpellName[0] : std::string("melee/auto");
            if (attacker != dealer)
                ability = "pet " + ability;
            gFight.dealtByAbility[MemberLabel(dealer)][ability] += damage;
            ++gFight.hitsByMember[MemberLabel(dealer)];
            gFight.dealtToTarget[victim->GetName()] += damage;
            gFight.dealtTotal += damage;
            if (gFight.hitByRaid.insert(victim->GetObjectGuid()).second)
            {
                Unit* vv = victim->GetVictim();
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[TELEMETRY] FIRST HIT %s (entry %u, guid %u) by %s with %s from %.0f yd at +%us; it was %s, on %s",
                    victim->GetName(), victim->GetEntry(), victim->GetGUIDLow(), MemberLabel(dealer).c_str(),
                    ability.c_str(), attacker->GetDistance(victim), (WorldTimer::getMSTime() - gFight.startedAt) / 1000,
                    gFight.hitTheRaid.count(victim->GetObjectGuid()) ? "already fighting us" : "not yet on us",
                    vv ? vv->GetName() : "-");
            }
            return;
        }
    }
    Player* member = Member(victim);
    if (!member || damage == 0 || (attacker && Member(attacker)))
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (!gFight.startedAt)
        gFight.startedAt = WorldTimer::getMSTime();
    std::string const source = SourceName(attacker, spell);
    gFight.damageBySource[source] += damage;
    if (attacker && attacker->IsCreature() && gFight.hitTheRaid.insert(attacker->GetObjectGuid()).second)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[TELEMETRY] JOINED %s (entry %u, guid %u) first struck %s at +%us%s",
            attacker->GetName(), attacker->GetEntry(), attacker->GetGUIDLow(), MemberLabel(member).c_str(),
            (WorldTimer::getMSTime() - gFight.startedAt) / 1000,
            gFight.hitByRaid.count(attacker->GetObjectGuid()) ? " (after we hit it)" : " (unprovoked)");
    gFight.damageByVictim[MemberLabel(member)] += damage;
    std::deque<Hit>& hits = gFight.recent[member->GetObjectGuid()];
    uint32 const health = member->GetHealth();
    hits.push_back({ WorldTimer::getMSTime(), source, damage, health > damage ? health - damage : 0 });
    while (hits.size() > 6)
        hits.pop_front();
}

void SuiRaidTelemetry::OnHeal(Unit* healer, Unit* target, uint32 amount, SpellEntry const* spell)
{
    Player* from = Member(healer);
    if (!from || !Member(target) || amount == 0)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    (void)spell;
    gFight.healingByHealer[MemberLabel(from)] += amount;
    auto& in = gFight.healsIn[target->GetObjectGuid()];
    in.emplace_back(WorldTimer::getMSTime(), amount);
    while (in.size() > 40)
        in.pop_front();
}

void SuiRaidTelemetry::OnKill(Unit* killer, Unit* victim)
{
    Player* member = Member(victim);
    if (!member)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    uint32 const now = WorldTimer::getMSTime();
    std::ostringstream line;
    line << MemberLabel(member) << " max " << member->GetMaxHealth() << " killed by "
         << (killer ? killer->GetName() : "?") << " after "
         << (gFight.startedAt ? (now - gFight.startedAt) / 1000 : 0) << " s; last hits:";
    for (Hit const& hit : gFight.recent[member->GetObjectGuid()])
        line << " [" << int32(hit.at - now) / 100 / 10.0f << "s " << hit.source << " " << hit.amount
             << " -> " << hit.healthAfter << "]";
    uint32 healed = 0;
    for (auto const& h : gFight.healsIn[member->GetObjectGuid()])
        if (now - h.first <= 8000)
            healed += h.second;
    line << " | healed " << healed << " in last 8 s | healers:";
    if (Group* group = member->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* h = itr->getSource();
            if (!h || h == member || !(h->GetClass() == CLASS_PRIEST || h->GetClass() == CLASS_PALADIN ||
                h->GetClass() == CLASS_DRUID || h->GetClass() == CLASS_SHAMAN))
                continue;
            Spell* cast = h->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            line << " " << h->GetName() << ":" << (h->IsAlive() ? "" : "DEAD/") << uint32(h->GetDistance(member))
                 << "yd" << (h->IsInCombat() ? "/IC" : "") << (h->IsWithinLOSInMap(member) ? "" : "/noLOS")
                 << (cast ? "/casting " + std::to_string(cast->m_spellInfo->Id) : "")
                 << "/mana" << uint32(h->GetPowerPercent(POWER_MANA));
        }
    gFight.deaths.push_back(member->GetName());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] DEATH %s", line.str().c_str());
}

void SuiRaidTelemetry::OnCastAttempt(Unit* caster, uint32 spellId, int32 outcome)
{
    Player* member = Member(caster);
    if (!member)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    std::string key = outcome == -2 ? std::string("null") : std::to_string(spellId);
    key += outcome == 0 ? ":ok" : outcome == -1 ? ":gate" : outcome == -2 ? "" : ":r" + std::to_string(outcome);
    ++gFight.castsByMember[MemberLabel(member)][key];
}

void SuiRaidTelemetry::OnRotationTick(Unit* caster, uint8 why)
{
    Player* member = Member(caster);
    if (!member || why > 5)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    ++gFight.ticksByMember[MemberLabel(member)][why];
}

void SuiRaidTelemetry::FlushFight(Player const* leader, uint32 fightNumber, uint32 seconds)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (gFight.damageBySource.empty() && gFight.deaths.empty() && gFight.dealtTotal == 0)
    {
        gFight = Fight();
        return;
    }
    char const* name = leader ? leader->GetName() : "?";
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u (%u s) deaths %u: %s", fightNumber,
        seconds, uint32(gFight.deaths.size()), name);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u damage by source: %s", fightNumber,
        Top(gFight.damageBySource, 8).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u damage taken: %s", fightNumber,
        Top(gFight.damageByVictim, 10).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u healing done: %s", fightNumber,
        Top(gFight.healingByHealer, 12).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u damage done %llu (%.0f/s) to: %s", fightNumber,
        (unsigned long long)gFight.dealtTotal, seconds ? double(gFight.dealtTotal) / seconds : 0.0,
        Top(gFight.dealtToTarget, 6).c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u damage by member: %s", fightNumber,
        Top(gFight.dealtByMember, 40).c_str());
    for (auto const& row : gFight.dealtByAbility)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u abilities %s (%u hits): %s", fightNumber,
            row.first.c_str(), gFight.hitsByMember[row.first], Top(row.second, 6).c_str());
    for (auto const& row : gFight.ticksByMember)
    {
        std::map<std::string, uint64> casts;
        for (auto const& c : gFight.castsByMember[row.first])
            casts[c.first] = c.second;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[TELEMETRY] fight %u rotation %s ran %u notInCombat %u casting %u pullHold %u hold %u gate %u | casts: %s",
            fightNumber, row.first.c_str(), row.second[0], row.second[1], row.second[2], row.second[3],
            row.second[4], row.second[5], Top(casts, 10).c_str());
    }
    gFight = Fight();
}
