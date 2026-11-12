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
#include "AiBotAIMain.h"
#include "Pet.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include <ctime>

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
        std::map<std::string, uint64> overhealByHealer;   // the part of each heal past the target's full health
        std::map<std::string, std::map<std::string, uint64>> healByAbility;   // healer -> "Heal r4 x12"
        std::map<std::string, std::map<std::string, uint32>> healCasts;       // healer -> spell -> landed
        std::map<std::string, uint64> dealtByMember;      // raid damage done
        std::map<std::string, uint64> dealtToTarget;      // by creature name
        uint64 dealtTotal = 0;
        std::map<std::string, std::map<std::string, uint64>> dealtByAbility;   // member -> ability
        std::map<std::string, uint32> hitsByMember;                          // landed hits
        std::map<std::string, std::map<std::string, uint32>> castsByMember;   // "spell:outcome" -> n
        std::map<std::string, std::array<uint32, 7>> ticksByMember;
        std::map<ObjectGuid, std::deque<Hit>> recent;     // last hits per member
        std::map<ObjectGuid, std::deque<std::pair<uint32, uint32>>> healsIn;   // (time, amount)
        std::vector<std::string> deaths;
        std::set<ObjectGuid> hitByRaid;      // creatures the raid has struck this fight
        std::set<ObjectGuid> aggroSeen;      // spawns seen entering combat before joining the fight
        std::set<ObjectGuid> hitTheRaid;     // creatures that have struck the raid this fight
        // The fight record (sui-autopilot-fights.jsonl): every hostile in it, how the raid spent
        // its time, and how each death came about - one JSON line per fight for review.
        struct Foe
        {
            uint32 entry = 0, rank = 0, maxHp = 0, firstAt = 0, diedAt = 0;
            std::string name;
            float hpPct = 100.0f, lowest = 100.0f;   // lowest: what a wipe's evade-to-full hides
            bool dead = false, unprovoked = false;
            bool corpse = false;   // already dead when first noted (a boss's corpse beside the next pull)
            uint64 taken = 0;
            uint32 onTank = 0, onOther = 0, swaps = 0;   // seconds its victim was a tank / not
            uint32 cc = 0;                               // seconds banished or sheeped (not counted above)
            std::map<std::string, uint32> swapWhy;       // world boss: why it turned from its target
            std::map<std::string, uint64> lowBy;         // elite: "member / ability" -> damage taken under 30 %
            ObjectGuid lastVictim;
            std::vector<uint32> hpTrace;                 // world-boss rank: hp % every 5 s
        };
        std::map<ObjectGuid, Foe> foes;
        // manaStart: mana % at its first sample (-1 = no mana); oom: seconds sampled under 10 % mana
        // (2026-11-04: healers cast 65 % of a wipe and healed 977/s in all - out of mana, or not?).
        struct Activity { uint8 role = 0; uint32 alive = 0, moving = 0, casting = 0, oom = 0; int32 manaStart = -1; std::map<std::string, uint32> repos; };
        std::map<std::string, Activity> activity;        // member label -> seconds sampled
        struct DeathRow { std::string who, killer, last; uint32 at = 0, healed8 = 0; uint8 role = 0; };
        std::vector<DeathRow> deathRows;
        std::vector<uint32> aliveTrace;                  // living members every 5 s
        std::map<std::string, uint64> riserLow;          // "member / ability" -> damage on a riser kept at its floor
        uint32 samples = 0;
    } gFight;

    uint8 RoleOf(Player const* p)
    {
        if (AiBotAI const* ai = dynamic_cast<AiBotAI const*>(const_cast<Player*>(p)->AI()))
            return uint8(ai->GetCombatActiveRole());
        return p->GetClass() == CLASS_WARRIOR ? uint8(ROLE_TANK) : uint8(ROLE_INVALID);
    }

    char const* RoleName(uint8 role)
    {
        switch (role)
        {
            case ROLE_TANK: return "tank";
            case ROLE_HEALER: return "healer";
            case ROLE_MELEE_DPS: return "melee";
            case ROLE_RANGE_DPS: return "ranged";
            default: return "?";
        }
    }

    std::string Json(std::string const& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (uint8(c) >= 0x20)
                out += c;
        }
        return out + "\"";
    }

    // A hostile taking part in this fight (caller holds gLock).
    Fight::Foe& NoteFoe(Unit* c, bool unprovoked)
    {
        Fight::Foe& f = gFight.foes[c->GetObjectGuid()];
        if (!f.entry)
        {
            f.entry = c->GetEntry();
            f.name = c->GetName();
            f.rank = c->IsCreature() ? static_cast<Creature*>(c)->GetCreatureInfo()->rank : 0;
            f.maxHp = c->GetMaxHealth();
            f.corpse = !c->IsAlive();
            f.firstAt = gFight.startedAt ? (WorldTimer::getMSTime() - gFight.startedAt) / 1000 : 0;
            f.unprovoked = unprovoked;
            // Who woke it, and from where: a spawned creature's place, how far it left home, its
            // victim and the nearest member (2026-11-03: a second Firelord pair 35 yd from the
            // fight was found on a healer, and nothing said how it got there).
            if (c->IsCreature() && static_cast<Creature*>(c)->HasStaticDBSpawnData())
            {
                Creature* cr = static_cast<Creature*>(c);
                float hx = 0.0f, hy = 0.0f, hz = 0.0f;
                cr->GetRespawnCoord(hx, hy, hz);
                Unit* v = cr->GetVictim();
                Player* nearest = nullptr;
                float nearestDist = 1e9f;
                if (Group const* group = gGroup.load(std::memory_order_relaxed))
                    for (GroupReference* itr = const_cast<Group*>(group)->GetFirstMember(); itr; itr = itr->next())
                        if (Player* m = itr->getSource())
                            if (m->IsInWorld() && m->IsAlive() && m->GetMap() == cr->GetMap() && cr->GetDistance(m) < nearestDist)
                            {
                                nearestDist = cr->GetDistance(m);
                                nearest = m;
                            }
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[TELEMETRY] WOKE %s (entry %u, spawn %u) at +%us (%.0f, %.0f, %.0f) %.0f yd from home, %s, victim %s %.0f yd, nearest member %s %.0f yd",
                    cr->GetName(), cr->GetEntry(), cr->GetDBTableGUIDLow(), f.firstAt, cr->GetPositionX(), cr->GetPositionY(),
                    cr->GetPositionZ(), cr->GetDistance(hx, hy, hz), unprovoked ? "struck us first" : "we struck first",
                    v ? v->GetName() : "-", v ? cr->GetDistance(v) : -1.0f, nearest ? nearest->GetName() : "-",
                    nearest ? nearestDist : -1.0f);
            }
        }
        return f;
    }

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
    std::map<uint32, float> gBurstEntries; // creature entry seen bursting as it died -> radius
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
        std::ifstream bursts(PulsePath() + ".bursts");
        while (bursts >> entry >> radius)
            gBurstEntries[entry] = radius;
        for (auto const& b : gBurstEntries)
            gPulseEntries.erase(b.first);   // a death burst read as a pulse before the two were told apart
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

float SuiRaidTelemetry::EntryDeathBurstRadius(uint32 entry)
{
    std::lock_guard<std::mutex> guard(gPulseLock);
    LoadPulses();
    auto it = gBurstEntries.find(entry);
    return it == gBurstEntries.end() ? 0.0f : it->second;
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
    // Death bursts: the same circle cast by a creature that has just died (a Firesworn's eruption)
    // is no pulse - nothing comes again; it is a blast when that kind dies.
    if (attacker && attacker->IsCreature() && !attacker->IsAlive() && spell && victim && victim->IsPlayer() && Member(victim))
        if (float const radius = SelfCentredRadius(spell))
        {
            std::lock_guard<std::mutex> guard(gPulseLock);
            LoadPulses();
            if (gBurstEntries.emplace(attacker->GetEntry(), radius).second)
            {
                std::ofstream(PulsePath() + ".bursts", std::ios::app) << attacker->GetEntry() << ' ' << radius << '\n';
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] DEATH BURST learned: %s (entry %u) bursts %.0f yd as it dies",
                    attacker->GetName(), attacker->GetEntry(), radius);
            }
            if (gPulseEntries.erase(attacker->GetEntry()))
            {
                std::ofstream out(PulsePath() + ".entries", std::ios::trunc);
                for (auto const& e : gPulseEntries)
                    out << e.first << ' ' << e.second << '\n';
            }
        }
    // Pulses: a creature's self-centred area spell landing on a raid member.
    if (attacker && attacker->IsCreature() && attacker->IsAlive() && spell && victim && victim->IsPlayer() && Member(victim))
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
            // A pack that gets back up is held at a floor until all of it is there: who hits one at the
            // floor while its kin still stands above it (the hit that starts the pack's clock early).
            if (victim->IsAlive() && victim->GetHealthPercent() <= 20.0f && AiBotAI::IsKnownRiser(victim->GetEntry()) &&
                !victim->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD))
            {
                std::list<Creature*> kin;
                victim->GetCreatureListWithEntryInGrid(kin, victim->GetEntry(), 100.0f);
                for (Creature* k : kin)
                    if (k != victim && k->IsAlive() && k->IsInCombat() && k->GetHealthPercent() > 25.0f &&
                        !k->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD))
                    {
                        gFight.riserLow[MemberLabel(dealer) + " / " + ability] += damage;
                        break;
                    }
            }
            gFight.dealtTotal += damage;
            Fight::Foe& foe = NoteFoe(victim, false);
            foe.taken += damage;
            // who finished it: damage taken under 30 % (2026-11-09, Majordomo: an Elite held at the kill
            // order's floor died anyway, 4th, and two Healers woke immune - by what?)
            if (victim->GetHealthPercent() < 30.0f && victim->IsCreature() && static_cast<Creature*>(victim)->IsElite())
                foe.lowBy[MemberLabel(dealer) + " / " + ability] += damage;
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
    // Self damage is the ground (Player::EnvironmentalDamage: lava, fire, falling) or a spell cast by an
    // object - a game object's fire patch reaches DealDamage as the victim hitting itself
    // (SpellCaster::DealDamage). Deaths read "killed by himself, no last hit" without it (2026-11-03,
    // Magmadar: twenty such deaths).
    bool const ground = attacker == victim;
    if (!member || damage == 0 || (attacker && Member(attacker) && !ground))
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (!gFight.startedAt)
        gFight.startedAt = WorldTimer::getMSTime();
    std::string const source = !ground ? SourceName(attacker, spell) :
        spell ? "object or self / " + std::string(spell->SpellName[0]) : std::string("ground (lava/fire/fall)");
    gFight.damageBySource[source] += damage;
    if (attacker && attacker->IsCreature())
        NoteFoe(attacker, !gFight.hitByRaid.count(attacker->GetObjectGuid()));
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
    std::string const label = MemberLabel(from);
    gFight.healingByHealer[label] += amount;
    // (called before the health changes: the room left is what the heal can still fill)
    uint32 const room = target->GetMaxHealth() > target->GetHealth() ? target->GetMaxHealth() - target->GetHealth() : 0;
    if (amount > room)
        gFight.overhealByHealer[label] += amount - room;
    if (spell)
    {
        // which rank landed how often (2026-11-05: six priests cast 60-74 % of a fight and healed
        // 120-430/s each while four paladins healed 2,400/s)
        std::string ability = spell->SpellName[0];
        if (!spell->Rank[0].empty())
            ability += " " + spell->Rank[0];
        gFight.healByAbility[label][ability] += amount;
        ++gFight.healCasts[label][ability];
    }
    auto& in = gFight.healsIn[target->GetObjectGuid()];
    in.emplace_back(WorldTimer::getMSTime(), amount);
    while (in.size() > 40)
        in.pop_front();
}

void SuiRaidTelemetry::OnKill(Unit* killer, Unit* victim)
{
    if (victim && victim->IsCreature() && gGroup.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto f = gFight.foes.find(victim->GetObjectGuid());
        if (f != gFight.foes.end() && !f->second.dead)
        {
            f->second.dead = true;
            f->second.hpPct = 0.0f;
            f->second.diedAt = gFight.startedAt ? (WorldTimer::getMSTime() - gFight.startedAt) / 1000 : 0;
            if (!f->second.lowBy.empty())
            {
                std::vector<std::pair<uint64, std::string>> rows;
                for (auto const& by : f->second.lowBy)
                    rows.emplace_back(by.second, by.first);
                std::sort(rows.rbegin(), rows.rend());
                std::ostringstream line;
                for (size_t i = 0; i < rows.size() && i < 10; ++i)
                    line << (i ? "; " : "") << rows[i].second << " " << rows[i].first;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] KILL %s (guid %u) at +%us, under 30 %% by: %s",
                    victim->GetName(), victim->GetGUIDLow(), f->second.diedAt, line.str().c_str());
            }
        }
        return;
    }
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
    {
        Fight::DeathRow row;
        row.who = member->GetName();
        row.role = RoleOf(member);
        row.killer = killer ? killer->GetName() : "?";
        row.at = gFight.startedAt ? (now - gFight.startedAt) / 1000 : 0;
        row.healed8 = healed;
        std::deque<Hit> const& hits = gFight.recent[member->GetObjectGuid()];
        if (!hits.empty())
            row.last = hits.back().source + " " + std::to_string(hits.back().amount);
        gFight.deathRows.push_back(row);
    }
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
    if (!member || why > 6)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    ++gFight.ticksByMember[MemberLabel(member)][why];
}

void SuiRaidTelemetry::OnReposition(Unit* who, char const* why)
{
    Player* member = Member(who);
    if (!member || !why)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (gFight.startedAt)
        ++gFight.activity[MemberLabel(member)].repos[why];
}

void SuiRaidTelemetry::Sample(Player const* leader)
{
    if (!leader || !leader->IsInWorld() || !leader->GetGroup() || leader->GetGroup() != gGroup.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (!gFight.startedAt)
        return;
    bool const every5 = gFight.samples++ % 5 == 0;
    uint32 alive = 0;
    for (GroupReference* itr = const_cast<Group*>(leader->GetGroup())->GetFirstMember(); itr; itr = itr->next())
    {
        Player const* m = itr->getSource();
        if (!m || !m->IsInWorld() || !m->IsInMap(leader) || !m->IsAlive())
            continue;
        ++alive;
        Fight::Activity& a = gFight.activity[MemberLabel(m)];
        a.role = RoleOf(m);
        ++a.alive;
        a.moving += m->IsMoving();
        a.casting += m->IsNonMeleeSpellCasted(false, false, true);
        if (m->GetPowerType() == POWER_MANA && m->GetMaxPower(POWER_MANA) > 0)
        {
            int32 const pct = int32(m->GetPowerPercent(POWER_MANA));
            if (a.manaStart < 0)
                a.manaStart = pct;
            a.oom += pct < 10;
        }
    }
    if (every5)
        gFight.aliveTrace.push_back(alive);
    // A spawn in combat that has not struck or been struck yet: who stood nearest when it woke
    // (2026-11-05: a Lava Reaver 32 yd from every held mob arrived on a mage 25 yd from its home,
    // and no member's logged step had come within its reach).
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(leader, 80.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(leader, searcher, 80.0f);
        for (Creature* c : near)
        {
            if (!c->IsAlive() || !c->IsInCombat() || !c->HasStaticDBSpawnData() || gFight.foes.count(c->GetObjectGuid()) ||
                !gFight.aggroSeen.insert(c->GetObjectGuid()).second)
                continue;
            float hx = 0.0f, hy = 0.0f, hz = 0.0f;
            c->GetRespawnCoord(hx, hy, hz);
            Unit const* nearest = nullptr;
            float nearestHome = 1e9f;
            for (GroupReference* itr = const_cast<Group*>(leader->GetGroup())->GetFirstMember(); itr; itr = itr->next())
                if (Player* m = itr->getSource())
                    if (m->IsInWorld() && m->IsAlive() && m->GetMap() == c->GetMap())
                    {
                        Unit const* bodies[2] = {m, m->GetPet()};
                        for (Unit const* b : bodies)
                            if (b && b->IsAlive() && b->GetDistance(hx, hy, hz) < nearestHome)
                            {
                                nearestHome = b->GetDistance(hx, hy, hz);
                                nearest = b;
                            }
                    }
            Unit const* v = c->GetVictim();
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[TELEMETRY] AGGRO %s (entry %u, spawn %u) at +%us (%.0f, %.0f) %.0f yd from home, victim %s %.0f yd, nearest to its home %s%s %.0f yd",
                c->GetName(), c->GetEntry(), c->GetDBTableGUIDLow(), (WorldTimer::getMSTime() - gFight.startedAt) / 1000,
                c->GetPositionX(), c->GetPositionY(), c->GetDistance(hx, hy, hz), v ? v->GetName() : "-",
                v ? c->GetDistance(v) : -1.0f, nearest ? nearest->GetName() : "-",
                nearest && !nearest->IsPlayer() ? " (pet)" : "", nearestHome);
        }
    }
    for (auto& pair : gFight.foes)
    {
        Fight::Foe& f = pair.second;
        if (f.dead)
            continue;
        Creature* c = leader->GetMap()->GetCreature(pair.first);
        if (!c || !c->IsAlive())
            continue;
        f.hpPct = c->GetHealthPercent();
        f.lowest = std::min(f.lowest, f.hpPct);
        if (every5 && f.rank == CREATURE_ELITE_WORLDBOSS)
            f.hpTrace.push_back(uint32(f.hpPct + 0.5f));
        bool controlled = c->HasBreakableByDamageCrowdControlAura();
        for (auto const& entry : c->GetSpellAuraHolderMap())
            if (entry.second && entry.second->HasMechanic(MECHANIC_BANISH))
                controlled = true;
        if (controlled)
        {
            ++f.cc;
            continue;
        }
        Unit* v = c->GetVictim();
        Player const* pv = v && v->IsPlayer() ? static_cast<Player const*>(v) : nullptr;
        if (!pv)
            continue;
        if (RoleOf(pv) == ROLE_TANK)
            ++f.onTank;
        else
            ++f.onOther;
        if (!f.lastVictim.IsEmpty() && f.lastVictim != pv->GetObjectGuid())
        {
            ++f.swaps;
            // A boss turning away from its target: why (the old one feared, dead, or simply out-threatened).
            if (f.rank == CREATURE_ELITE_WORLDBOSS)
            {
                Player const* old = leader->GetMap()->GetPlayer(f.lastVictim);
                char const* why = !old ? "gone" : !old->IsAlive() ? "dead" : old->HasAuraType(SPELL_AURA_MOD_FEAR) ? "feared" :
                    old->HasAuraType(SPELL_AURA_MOD_CHARM) ? "charmed" : old->HasAuraType(SPELL_AURA_MOD_STUN) ? "stunned" : "threat";
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] SWAP %s at +%us: %s (%s) -> %s (%s)",
                    c->GetName(), (WorldTimer::getMSTime() - gFight.startedAt) / 1000, old ? old->GetName() : "?", why,
                    pv->GetName(), RoleName(RoleOf(pv)));
                ++f.swapWhy[why];
            }
        }
        f.lastVictim = pv->GetObjectGuid();
    }
}

uint32 SuiRaidTelemetry::FlushFight(Player const* leader, uint32 fightNumber, uint32 seconds, uint32 targetEntry, bool leaderDied)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (gFight.damageBySource.empty() && gFight.deaths.empty() && gFight.dealtTotal == 0)
    {
        gFight = Fight();
        return 0;
    }
    // The record: what was fought, how it ended, who died of what, how the raid spent its time.
    uint32 flags = leaderDied ? FIGHT_WIPE : 0;
    uint32 members = 0, alive = 0;
    if (leader && leader->GetGroup())
        for (GroupReference* itr = const_cast<Group*>(leader->GetGroup())->GetFirstMember(); itr; itr = itr->next())
            if (Player const* m = itr->getSource())
                if (m->IsInWorld() && m->IsInMap(leader))
                {
                    ++members;
                    alive += m->IsAlive();
                }
    if (members && alive * 2 < members)
        flags |= FIGHT_WIPE;
    for (auto& pair : gFight.foes)
    {
        Fight::Foe& f = pair.second;
        if (leader && !f.dead)
        {
            Creature* c = leader->GetMap()->GetCreature(pair.first);
            if (!c || !c->IsAlive())
                f.dead = true, f.hpPct = 0.0f;
            else
                f.hpPct = c->GetHealthPercent();
        }
        // (a boss already dead when this fight first saw it is no boss fight: 2026-11-04/05, every
        // trash fight after Geddon and after Golemagg paused for review as "boss killed")
        if (f.rank == CREATURE_ELITE_WORLDBOSS && !f.corpse)
            flags |= FIGHT_BOSS | (f.dead ? FIGHT_BOSS_KILLED : 0);
    }
    {
        time_t const now = time(nullptr);
        char when[32];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
        std::ostringstream j;
        j << "{\"t\":" << Json(when) << ",\"fight\":" << fightNumber << ",\"leader\":" << Json(leader ? leader->GetName() : "?")
          << ",\"map\":" << (leader ? leader->GetMapId() : 0) << ",\"instance\":" << (leader ? leader->GetInstanceId() : 0)
          << ",\"secs\":" << seconds << ",\"target\":" << targetEntry
          << ",\"outcome\":" << Json((flags & FIGHT_WIPE) ? "wipe" : (flags & FIGHT_BOSS) && !(flags & FIGHT_BOSS_KILLED) ? "boss-alive" : "win")
          << ",\"leaderDied\":" << (leaderDied ? "true" : "false")
          << ",\"memberCount\":" << members << ",\"alive\":" << alive << ",\"dealt\":" << gFight.dealtTotal << ",\"foes\":[";
        bool first = true;
        for (auto const& pair : gFight.foes)
        {
            Fight::Foe const& f = pair.second;
            j << (first ? "" : ",") << "{\"entry\":" << f.entry << ",\"guid\":" << pair.first.GetCounter() << ",\"name\":" << Json(f.name)
              << ",\"rank\":" << f.rank << ",\"maxHp\":" << f.maxHp << ",\"hp\":" << uint32(f.hpPct + 0.5f) << ",\"low\":" << uint32(f.dead ? 0.0f : f.lowest + 0.5f)
              << ",\"dead\":" << (f.dead ? "true" : "false") << ",\"diedAt\":" << f.diedAt << ",\"firstAt\":" << f.firstAt
              << ",\"unprovoked\":" << (f.unprovoked ? "true" : "false") << ",\"taken\":" << f.taken
              << ",\"onTank\":" << f.onTank << ",\"onOther\":" << f.onOther << ",\"swaps\":" << f.swaps << ",\"cc\":" << f.cc << ",\"swapWhy\":{";
            {
                bool firstWhy = true;
                for (auto const& w : f.swapWhy)
                {
                    j << (firstWhy ? "" : ",") << Json(w.first) << ":" << w.second;
                    firstWhy = false;
                }
            }
            j << "},\"hpTrace\":[";
            for (size_t i = 0; i < f.hpTrace.size(); ++i)
                j << (i ? "," : "") << f.hpTrace[i];
            j << "]}";
            first = false;
        }
        j << "],\"deaths\":[";
        for (size_t i = 0; i < gFight.deathRows.size(); ++i)
        {
            Fight::DeathRow const& d = gFight.deathRows[i];
            j << (i ? "," : "") << "{\"who\":" << Json(d.who) << ",\"role\":" << Json(RoleName(d.role)) << ",\"at\":" << d.at
              << ",\"killer\":" << Json(d.killer) << ",\"last\":" << Json(d.last) << ",\"healed8\":" << d.healed8 << "}";
        }
        j << "],\"members\":[";
        first = true;
        for (auto const& pair : gFight.activity)
        {
            Fight::Activity const& a = pair.second;
            auto dealt = gFight.dealtByMember.find(pair.first);
            auto healed = gFight.healingByHealer.find(pair.first);
            auto over = gFight.overhealByHealer.find(pair.first);
            auto taken = gFight.damageByVictim.find(pair.first);
            j << (first ? "" : ",") << "{\"who\":" << Json(pair.first) << ",\"role\":" << Json(RoleName(a.role))
              << ",\"alive\":" << a.alive << ",\"moving\":" << a.moving << ",\"casting\":" << a.casting
              << ",\"manaStart\":" << a.manaStart << ",\"oom\":" << a.oom
              << ",\"dealt\":" << (dealt != gFight.dealtByMember.end() ? dealt->second : 0)
              << ",\"healed\":" << (healed != gFight.healingByHealer.end() ? healed->second : 0)
              << ",\"overheal\":" << (over != gFight.overhealByHealer.end() ? over->second : 0)
              << ",\"taken\":" << (taken != gFight.damageByVictim.end() ? taken->second : 0) << ",\"repos\":{";
            bool firstWhy = true;
            for (auto const& why : a.repos)
            {
                j << (firstWhy ? "" : ",") << Json(why.first) << ":" << why.second;
                firstWhy = false;
            }
            j << "}}";
            first = false;
        }
        j << "],\"aliveTrace\":[";
        for (size_t i = 0; i < gFight.aliveTrace.size(); ++i)
            j << (i ? "," : "") << gFight.aliveTrace[i];
        j << "],\"riserLow\":[";
        {
            std::vector<std::pair<std::string, uint64>> rows(gFight.riserLow.begin(), gFight.riserLow.end());
            std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            for (size_t i = 0; i < rows.size() && i < 12; ++i)
                j << (i ? "," : "") << "[" << Json(rows[i].first) << "," << rows[i].second << "]";
        }
        j << "],\"sources\":[";
        {
            std::vector<std::pair<std::string, uint64>> rows(gFight.damageBySource.begin(), gFight.damageBySource.end());
            std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            for (size_t i = 0; i < rows.size() && i < 12; ++i)
                j << (i ? "," : "") << "[" << Json(rows[i].first) << "," << rows[i].second << "]";
        }
        j << "]}";
        std::string const conf = sConfig.GetFilename();
        size_t const slash = conf.find_last_of('/');
        std::ofstream((slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-autopilot-fights.jsonl",
            std::ios::app) << j.str() << '\n';
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
    for (auto const& row : gFight.healByAbility)
    {
        std::map<std::string, uint64> named;
        for (auto const& a : row.second)
            named[a.first + " x" + std::to_string(gFight.healCasts[row.first][a.first])] = a.second;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] fight %u heals %s: %s", fightNumber,
            row.first.c_str(), Top(named, 6).c_str());
    }
    for (auto const& row : gFight.ticksByMember)
    {
        std::map<std::string, uint64> casts;
        for (auto const& c : gFight.castsByMember[row.first])
            casts[c.first] = c.second;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[TELEMETRY] fight %u rotation %s ran %u notInCombat %u casting %u pullHold %u hold %u gate %u repos %u | casts: %s",
            fightNumber, row.first.c_str(), row.second[0], row.second[1], row.second[2], row.second[3],
            row.second[4], row.second[5], row.second[6], Top(casts, 10).c_str());
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[TELEMETRY] RECORD fight %u written (flags %u)", fightNumber, flags);
    gFight = Fight();
    return flags;
}
