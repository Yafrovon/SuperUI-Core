/*
 * SuiRaidPlaybook.cpp — see SuiRaidPlaybook.h. Line endings: LF (C++ repo convention).
 */

#include "SuiRaidPlaybook.h"
#include "Creature.h"
#include "Map.h"
#include "InstanceData.h"
#include "MapPersistentStateMgr.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Config/Config.h"
#include "json.hpp"
#include <sys/stat.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <array>
#include <set>

namespace
{
    using Json = nlohmann::json;

    struct Book
    {
        std::map<uint32, std::vector<uint32>> order;   // map id -> boss entries in kill order
        std::map<uint32, std::string> names;           // boss entry -> name
        struct LureSpot { float l[3]; bool hasCamp; float c[3]; float clearance; };
        std::map<uint32, LureSpot> lures;              // boss entry -> lure spot (+ camp)
        std::map<uint32, std::vector<uint32>> killOrder;   // boss entry -> add entries, first dies first
        std::map<uint32, std::array<float, 3>> summonAt;   // boss entry -> where a script summons it (no spawn row)
        std::map<uint32, uint32> doneData;                 // boss entry -> instance data index that reads DONE once it is beaten
        std::map<uint32, float> spread;                    // boss entry -> yards its ranged and healers keep apart
        std::set<uint32> standInPulse;                     // boss entries whose learned pulse circle is not kept clear
        std::map<uint32, std::vector<uint32>> prePotions;  // boss entry -> potion item entries drunk before it
        std::map<uint32, std::vector<uint32>> paladinAura; // boss entry -> aura spell ids, highest rank first
        std::map<uint32, SuiRaidPlaybook::Gossip> summonGossip;   // boss entry -> the gossip that summons it
    };

    std::mutex gLock;
    Book gBook;
    std::string gLoadedPath;
    time_t gLoadedMtime = 0;
    uint32 gCheckedMs = 0;

    std::string LivePath()
    {
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-raid-playbook.json";
    }

    // Caller holds gLock. Re-reads the file when it changed (checked at most every 5 s).
    void Refresh()
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        if (gCheckedMs && nowMs - gCheckedMs < 5000)
            return;
        gCheckedMs = nowMs;
        std::string path = LivePath();
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
        {
            path += ".dist";
            if (stat(path.c_str(), &st) != 0)
                return;
        }
        if (path == gLoadedPath && st.st_mtime == gLoadedMtime)
            return;
        gLoadedPath = path;
        gLoadedMtime = st.st_mtime;
        Book book;
        try
        {
            std::ifstream in(path);
            Json j;
            in >> j;
            if (j.count("raids") && j.at("raids").is_object())
                for (auto it = j.at("raids").begin(); it != j.at("raids").end(); ++it)
                {
                    uint32 const mapId = uint32(std::stoul(it.key()));
                    Json const& raid = it.value();
                    if (raid.count("order") && raid.at("order").is_array())
                        for (Json const& boss : raid.at("order"))
                        {
                            if (boss.is_number_unsigned())
                                book.order[mapId].push_back(boss.get<uint32>());
                            else if (boss.is_object() && boss.count("entry"))
                            {
                                uint32 const entry = boss.at("entry").get<uint32>();
                                book.order[mapId].push_back(entry);
                                if (boss.count("name"))
                                    book.names[entry] = boss.at("name").get<std::string>();
                            }
                        }
                }
            if (j.count("bosses") && j.at("bosses").is_object())
                for (auto it = j.at("bosses").begin(); it != j.at("bosses").end(); ++it)
                {
                    if (it.key().empty() || it.key()[0] == '_' || !it.value().is_object())
                        continue;
                    uint32 const entry = uint32(std::stoul(it.key()));
                    Json const& boss = it.value();
                    if (boss.count("name"))
                        book.names[entry] = boss.at("name").get<std::string>();
                    if (boss.count("lure") && boss.at("lure").is_object() && boss.at("lure").count("at"))
                    {
                        Json const& lure = boss.at("lure");
                        Book::LureSpot spot{};
                        for (int k = 0; k < 3; ++k)
                            spot.l[k] = lure.at("at").at(k).get<float>();
                        spot.hasCamp = lure.count("camp") && lure.at("camp").is_array();
                        spot.clearance = lure.count("clearance") && lure.at("clearance").is_number() ?
                            lure.at("clearance").get<float>() : 45.0f;
                        if (spot.hasCamp)
                            for (int k = 0; k < 3; ++k)
                                spot.c[k] = lure.at("camp").at(k).get<float>();
                        book.lures[entry] = spot;
                    }
                    if (boss.count("summonAt") && boss.at("summonAt").is_array() && boss.at("summonAt").size() == 3)
                        for (int k = 0; k < 3; ++k)
                            book.summonAt[entry][k] = boss.at("summonAt").at(k).get<float>();
                    if (boss.count("paladinAura") && boss.at("paladinAura").is_array())
                        for (Json const& aura : boss.at("paladinAura"))
                            if (aura.is_number_unsigned())
                                book.paladinAura[entry].push_back(aura.get<uint32>());
                    if (boss.count("prePotions") && boss.at("prePotions").is_array())
                        for (Json const& item : boss.at("prePotions"))
                            if (item.is_number_unsigned())
                                book.prePotions[entry].push_back(item.get<uint32>());
                    if (boss.count("standInPulse") && boss.at("standInPulse").is_boolean() && boss.at("standInPulse").get<bool>())
                        book.standInPulse.insert(entry);
                    if (boss.count("spread") && boss.at("spread").is_number())
                        book.spread[entry] = boss.at("spread").get<float>();
                    if (boss.count("doneData") && boss.at("doneData").is_number_unsigned())
                        book.doneData[entry] = boss.at("doneData").get<uint32>();
                    if (boss.count("summonGossip") && boss.at("summonGossip").is_object())
                    {
                        Json const& g = boss.at("summonGossip");
                        SuiRaidPlaybook::Gossip gossip{};
                        gossip.npc = g.at("npc").get<uint32>();
                        for (int k = 0; k < 3; ++k)
                            gossip.at[k] = g.at("at").at(k).get<float>();
                        gossip.selects = g.count("selects") ? g.at("selects").get<uint32>() : 1;
                        gossip.hasStand = g.count("standAt") && g.at("standAt").is_array() && g.at("standAt").size() == 3;
                        if (gossip.hasStand)
                            for (int k = 0; k < 3; ++k)
                                gossip.stand[k] = g.at("standAt").at(k).get<float>();
                        book.summonGossip[entry] = gossip;
                    }
                    if (boss.count("killOrder") && boss.at("killOrder").is_array())
                        for (Json const& add : boss.at("killOrder"))
                            if (add.is_number_unsigned())
                                book.killOrder[entry].push_back(add.get<uint32>());
                }
        }
        catch (std::exception const& e)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PLAYBOOK] %s unreadable (%s) - the last good copy stays", path.c_str(), e.what());
            return;
        }
        gBook = book;
        size_t bosses = 0;
        for (auto const& o : gBook.order)
            bosses += o.second.size();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PLAYBOOK] read %s: %u raid order(s), %u boss(es), %u lure(s), %u kill order(s)", path.c_str(),
            uint32(gBook.order.size()), uint32(bosses), uint32(gBook.lures.size()), uint32(gBook.killOrder.size()));
    }

    // Spawn rows (db guid, data) of an entry on a map - the world's spawn table is walked once per
    // (map, entry) and kept: the picker asks for every candidate pack.
    std::vector<std::pair<uint32, CreatureData const*>> Spawns(uint32 mapId, uint32 entry)
    {
        static std::mutex lock;
        static std::map<std::pair<uint32, uint32>, std::vector<std::pair<uint32, CreatureData const*>>> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto key = std::make_pair(mapId, entry);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
        std::vector<std::pair<uint32, CreatureData const*>> out;
        for (auto const& pair : sObjectMgr.GetCreatureDataMap())
            if (pair.second.position.mapId == mapId && pair.second.creature_id[0] == entry)
                out.emplace_back(pair.first, &pair.second);
        cache[key] = out;
        return out;
    }
}

std::vector<uint32> SuiRaidPlaybook::Order(uint32 mapId)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.order.find(mapId);
    return it == gBook.order.end() ? std::vector<uint32>() : it->second;
}

std::string SuiRaidPlaybook::Name(uint32 bossEntry)
{
    {
        std::lock_guard<std::mutex> guard(gLock);
        Refresh();
        auto it = gBook.names.find(bossEntry);
        if (it != gBook.names.end())
            return it->second;
    }
    if (CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(bossEntry))
        return info->name;
    return "entry " + std::to_string(bossEntry);
}

bool SuiRaidPlaybook::BossDead(Map const* map, uint32 bossEntry)
{
    if (!map)
        return false;
    // A boss that yields or is summoned by a script is judged by its encounter (2026-11-10: Majordomo never dies and
    // has no spawn row; with summonAt alone the raid walked back to his empty terrace after he was DONE).
    {
        std::lock_guard<std::mutex> guard(gLock);
        Refresh();
        auto done = gBook.doneData.find(bossEntry);
        if (done != gBook.doneData.end())
            return map->GetInstanceData() && const_cast<Map*>(map)->GetInstanceData()->GetData(done->second) == 3;   // DONE (EncounterState, scripts)
    }
    auto spawns = Spawns(map->GetId(), bossEntry);
    if (spawns.empty())
        return false;   // summoned by a script (Ragnaros): not ours to judge from spawn rows
    MapPersistentState* state = const_cast<Map*>(map)->GetPersistentState();
    time_t const now = time(nullptr);
    for (auto const& spawn : spawns)
    {
        if (state && state->GetCreatureRespawnTime(spawn.first) > now)
            continue;
        if (Creature* c = const_cast<Map*>(map)->GetCreature(ObjectGuid(HIGHGUID_UNIT, bossEntry, spawn.first)))
            if (!c->IsAlive())
                continue;
        return false;   // a spawn of it stands
    }
    return true;
}

uint32 SuiRaidPlaybook::EarlierAliveBoss(Map const* map, uint32 bossEntry)
{
    if (!map)
        return 0;
    std::vector<uint32> const order = Order(map->GetId());
    auto it = std::find(order.begin(), order.end(), bossEntry);
    if (it == order.end())
        return 0;
    for (auto e = order.begin(); e != it; ++e)
        if (!BossDead(map, *e))
            return *e;
    return 0;
}

uint32 SuiRaidPlaybook::FirstAliveBoss(Map const* map)
{
    if (!map)
        return 0;
    for (uint32 e : Order(map->GetId()))
        if (!BossDead(map, e))
            return e;
    return 0;
}

bool SuiRaidPlaybook::Lure(uint32 bossEntry, float& lx, float& ly, float& lz, bool& hasCamp, float& cx, float& cy, float& cz)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.lures.find(bossEntry);
    if (it == gBook.lures.end())
        return false;
    lx = it->second.l[0]; ly = it->second.l[1]; lz = it->second.l[2];
    hasCamp = it->second.hasCamp;
    cx = it->second.c[0]; cy = it->second.c[1]; cz = it->second.c[2];
    return true;
}

float SuiRaidPlaybook::LureClearYards(uint32 bossEntry)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.lures.find(bossEntry);
    return it == gBook.lures.end() ? 45.0f : it->second.clearance;
}

int SuiRaidPlaybook::KillOrder(uint32 bossEntry, uint32 addEntry)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.killOrder.find(bossEntry);
    if (it == gBook.killOrder.end())
        return -1;
    for (size_t i = 0; i < it->second.size(); ++i)
        if (it->second[i] == addEntry)
            return int(i);
    return -1;
}

bool SuiRaidPlaybook::BossSpawn(uint32 mapId, uint32 bossEntry, float& x, float& y, float& z)
{
    auto spawns = Spawns(mapId, bossEntry);
    if (spawns.empty())
    {
        // A boss a script summons has no spawn row: the playbook says where it appears, so the raid walks to its room
        // once the way there is clear (2026-11-10: Majordomo was summoned by the last rune and never visited - his
        // adds only spawn with a player near, and exploration knew only spawn rows).
        std::lock_guard<std::mutex> guard(gLock);
        Refresh();
        auto at = gBook.summonAt.find(bossEntry);
        if (at == gBook.summonAt.end())
            return false;
        x = at->second[0];
        y = at->second[1];
        z = at->second[2];
        return true;
    }
    x = spawns.front().second->position.x;
    y = spawns.front().second->position.y;
    z = spawns.front().second->position.z;
    return true;
}

bool SuiRaidPlaybook::SummonGossip(uint32 bossEntry, Gossip& out)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.summonGossip.find(bossEntry);
    if (it == gBook.summonGossip.end())
        return false;
    out = it->second;
    return true;
}

float SuiRaidPlaybook::SpreadYards(uint32 bossEntry)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.spread.find(bossEntry);
    return it == gBook.spread.end() ? 0.0f : it->second;
}

bool SuiRaidPlaybook::StandInPulse(uint32 bossEntry)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    return gBook.standInPulse.count(bossEntry) != 0;
}

std::vector<uint32> SuiRaidPlaybook::PrePotions(uint32 bossEntry)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.prePotions.find(bossEntry);
    return it == gBook.prePotions.end() ? std::vector<uint32>() : it->second;
}

std::vector<uint32> SuiRaidPlaybook::PaladinAura(uint32 bossEntry)
{
    std::lock_guard<std::mutex> guard(gLock);
    Refresh();
    auto it = gBook.paladinAura.find(bossEntry);
    return it == gBook.paladinAura.end() ? std::vector<uint32>() : it->second;
}
