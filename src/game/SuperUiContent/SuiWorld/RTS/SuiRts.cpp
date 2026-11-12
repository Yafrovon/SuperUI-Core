/* SuperUI RTS immutable boot rules and module facade. */

#include "SuiRts.h"

#include "SuiHero.h"
#include "SuiHonor.h"
#include "SuiWorldState.h"
#include "SuiTacticalFreeze.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

#include "Chat.h"
#include "Database/DatabaseEnv.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "Server/WorldSession.h"
#include "World.h"

namespace SuiRts
{
namespace
{
    std::unordered_map<std::string, std::string> s_kv;
    std::atomic<int64> s_honorPool[2];
    std::atomic<bool> s_poolDirty{false};
    int64 s_botCap[2] = { -1, -1 };
    bool s_loaded = false;
    bool s_honorEnabled = false;
    bool s_heroesEnabled = false;
    bool s_territoryEnabled = false;
    bool s_dungeonsEnabled = false;
    bool s_factionControlEnabled = false;
    uint32 s_flushMs = 30000;
    uint32 s_flushTimer = 0;

    int64 CheckedAddHonor(uint8 teamIdx, int64 amount)
    {
        std::atomic<int64>& pool = s_honorPool[teamIdx & 1];
        int64 current = pool.load(std::memory_order_relaxed);
        if (amount <= 0)
            return current;

        int64 const maximum = std::numeric_limits<int64>::max();
        while (true)
        {
            int64 const desired = current > maximum - amount ? maximum : current + amount;
            if (pool.compare_exchange_weak(current, desired,
                std::memory_order_relaxed, std::memory_order_relaxed))
            {
                if (desired != current)
                    s_poolDirty.store(true, std::memory_order_relaxed);
                if (desired == maximum && current > maximum - amount)
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                        "[SUI-RTS] honor pool saturated for team %u", teamIdx & 1);
                return desired;
            }
        }
    }

    void ApplyRates()
    {
        static struct { char const* key; eConfigFloatValues index; } const RATES[] =
        {
            { "rate.xp_kill",              CONFIG_FLOAT_RATE_XP_KILL },
            { "rate.xp_kill_elite",        CONFIG_FLOAT_RATE_XP_KILL_ELITE },
            { "rate.xp_quest",             CONFIG_FLOAT_RATE_XP_QUEST },
            { "rate.drop_money",           CONFIG_FLOAT_RATE_DROP_MONEY },
            { "rate.drop_item_poor",       CONFIG_FLOAT_RATE_DROP_ITEM_POOR },
            { "rate.drop_item_normal",     CONFIG_FLOAT_RATE_DROP_ITEM_NORMAL },
            { "rate.drop_item_uncommon",   CONFIG_FLOAT_RATE_DROP_ITEM_UNCOMMON },
            { "rate.drop_item_rare",       CONFIG_FLOAT_RATE_DROP_ITEM_RARE },
            { "rate.drop_item_epic",       CONFIG_FLOAT_RATE_DROP_ITEM_EPIC },
            { "rate.drop_item_legendary",  CONFIG_FLOAT_RATE_DROP_ITEM_LEGENDARY },
            { "rate.drop_item_artifact",   CONFIG_FLOAT_RATE_DROP_ITEM_ARTIFACT },
            { "rate.drop_item_referenced", CONFIG_FLOAT_RATE_DROP_ITEM_REFERENCED },
        };
        for (auto const& rate : RATES)
        {
            auto itr = s_kv.find(rate.key);
            if (itr == s_kv.end())
                continue;
            float value = float(atof(itr->second.c_str()));
            if (value <= 0.0f)
                continue;
            sWorld.setConfig(rate.index, value);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[SUI-RTS] boot rate override: %s = %.2f", rate.key, value);
        }
    }

    void FlushState()
    {
        if (!SuiWorldState::RtsWorldState() || !s_poolDirty.exchange(false))
            return;
        for (uint8 team = 0; team < 2; ++team)
        {
            long pool = long(s_honorPool[team].load(std::memory_order_relaxed));
            CharacterDatabase.DirectPExecute(
                "UPDATE `superui_faction` SET `honor_pool`=%ld WHERE `team`=%u", pool, team);
        }
    }
}

std::string GetKV(std::string const& key, std::string const& def)
{
    auto itr = s_kv.find(key);
    return itr != s_kv.end() ? itr->second : def;
}

float GetKVFloat(std::string const& key, float def)
{
    auto itr = s_kv.find(key);
    return itr != s_kv.end() ? float(atof(itr->second.c_str())) : def;
}

int64 GetKVInt(std::string const& key, int64 def)
{
    auto itr = s_kv.find(key);
    return itr != s_kv.end() ? int64(strtoll(itr->second.c_str(), nullptr, 10)) : def;
}

void ReapplyRateOverrides()
{
    if (!s_loaded || !SuiWorldState::RtsWorldState())
        return;
    ApplyRates();
}

void LoadRuleset()
{
    if (s_loaded)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[SUI-RTS] ruleset reload refused: mode and modules are boot-latched");
        return;
    }
    s_loaded = true;
    s_honorPool[0].store(0, std::memory_order_relaxed);
    s_honorPool[1].store(0, std::memory_order_relaxed);

    // MMO boot is deliberately read-only and does not even probe RTS module
    // tables. LoadWorldState already established the immutable mode latch.
    if (!SuiWorldState::RtsWorldState())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[SUI-RTS] vanilla boot: all match modules inert");
        return;
    }

    if (auto result = CharacterDatabase.Query(
        "SELECT `key`,`value` FROM `superui_worldstate`"))
    {
        do
        {
            Field* fields = result->Fetch();
            s_kv[fields[0].GetCppString()] = fields[1].GetCppString();
        } while (result->NextRow());
    }

    s_honorEnabled = GetKVInt("honor.enabled", 0) == 1;
    bool heroesRequested = GetKVInt("hero.enabled", 0) == 1;
    s_heroesEnabled = heroesRequested && s_honorEnabled;
    if (heroesRequested && !s_honorEnabled)
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[SUI-RTS] hero module disabled: hero.enabled requires honor.enabled");
    s_territoryEnabled = GetKVInt("territory.enabled", 0) == 1;
    s_dungeonsEnabled = GetKVInt("dungeon.enabled", 0) == 1;
    s_factionControlEnabled = GetKVInt("control.faction_bots", 0) == 1;
    s_botCap[0] = GetKVInt("bots.cap.alliance", -1);
    s_botCap[1] = GetKVInt("bots.cap.horde", -1);
    int64 flush = GetKVInt("state.flush_ms", 30000);
    s_flushMs = uint32(std::max<int64>(1000, std::min<int64>(3600000, flush)));

    ApplyRates();
    if (s_honorEnabled)
        SuiHonor::LoadRuleset();
    if (s_heroesEnabled && !SuiHero::LoadRuleset())
    {
        s_heroesEnabled = false;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[SUI-RTS] hero module disabled: rules or native aura spells are invalid");
    }

    if (s_honorEnabled || s_heroesEnabled)
        if (auto result = CharacterDatabase.Query(
            "SELECT `team`,`honor_pool` FROM `superui_faction`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint8 team = uint8(fields[0].GetUInt32());
                if (team < 2)
                {
                    int64 loaded = fields[1].GetInt64();
                    if (loaded < 0)
                    {
                        loaded = 0;
                        s_poolDirty.store(true, std::memory_order_relaxed);
                        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                            "[SUI-RTS] negative honor pool normalized to zero for team %u", team);
                    }
                    s_honorPool[team].store(loaded, std::memory_order_relaxed);
                }
            } while (result->NextRow());
        }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[SUI-RTS] boot latch: honor=%u heroes=%u territory=%u dungeons=%u faction-control=%u; "
        "bot caps A=%ld H=%ld; pools A=%ld H=%ld",
        HonorEnabled(), HeroesEnabled(), TerritoryEnabled(), DungeonsEnabled(),
        FactionControlEnabled(), long(s_botCap[0]), long(s_botCap[1]),
        long(s_honorPool[0].load()), long(s_honorPool[1].load()));
}

void Tick(uint32 diff)
{
    if (!SuiWorldState::RtsWorldState())
        return;
    if (HeroesEnabled())
        SuiHero::Tick();
    s_flushTimer += diff;
    if (s_flushTimer >= s_flushMs)
    {
        s_flushTimer = 0;
        FlushState();
    }
}

void Shutdown()
{
    if (!SuiWorldState::RtsWorldState())
        return;
    if (HeroesEnabled())
        SuiHero::Shutdown();
    FlushState();
}

bool HonorEnabled() { return SuiWorldState::RtsWorldState() && s_honorEnabled; }
bool HeroesEnabled() { return SuiWorldState::RtsWorldState() && s_heroesEnabled; }
bool TerritoryEnabled() { return SuiWorldState::RtsWorldState() && s_territoryEnabled; }
bool DungeonsEnabled() { return SuiWorldState::RtsWorldState() && s_dungeonsEnabled; }
bool FactionControlEnabled() { return SuiWorldState::RtsWorldState() && s_factionControlEnabled; }

int64 HonorPool(uint8 teamIdx)
{
    return s_honorPool[teamIdx & 1].load(std::memory_order_relaxed);
}

void AddHonor(uint8 teamIdx, int64 amount)
{
    if (!HonorEnabled() || amount <= 0)
        return;
    CheckedAddHonor(teamIdx, amount);
}

bool TrySpendHonor(uint8 teamIdx, int64 amount, int64* poolAfter)
{
    std::atomic<int64>& pool = s_honorPool[teamIdx & 1];
    int64 current = pool.load(std::memory_order_relaxed);
    if (!HeroesEnabled() || !HonorEnabled() || amount < 0)
    {
        if (poolAfter)
            *poolAfter = current;
        return false;
    }
    while (current >= amount)
    {
        int64 desired = current - amount;
        if (pool.compare_exchange_weak(current, desired,
            std::memory_order_relaxed, std::memory_order_relaxed))
        {
            s_poolDirty.store(true, std::memory_order_relaxed);
            if (poolAfter)
                *poolAfter = desired;
            return true;
        }
    }
    if (poolAfter)
        *poolAfter = current;
    return false;
}

void RefundHonor(uint8 teamIdx, int64 amount, int64* poolAfter)
{
    if (!HeroesEnabled() || !HonorEnabled() || amount <= 0)
    {
        if (poolAfter)
            *poolAfter = s_honorPool[teamIdx & 1].load(std::memory_order_relaxed);
        return;
    }
    int64 after = CheckedAddHonor(teamIdx, amount);
    if (poolAfter)
        *poolAfter = after;
}

int64 BotCap(uint8 teamIdx)
{
    return SuiWorldState::RtsWorldState() ? s_botCap[teamIdx & 1] : -1;
}

void OnUnitKill(Unit* killer, Unit* victim)
{
    if (!SuiWorldState::RtsWorldState())
        return;
    if (HonorEnabled())
        SuiHonor::OnUnitKill(killer, victim);
    if (HeroesEnabled())
        SuiHero::OnUnitKill(victim);
}

void OnPlayerWorldEnter(Player* player)
{
    if (HeroesEnabled())
        SuiHero::OnPlayerWorldEnter(player);
}

void HandleRtsState(WorldSession* session, uint8 /*flags*/)
{
    session->SetSuiCapable(true);
    bool rts = SuiWorldState::RtsWorldState();
    uint8 modules = 0;
    if (HonorEnabled())          modules |= 0x01;
    if (HeroesEnabled())         modules |= 0x02;
    if (TerritoryEnabled())      modules |= 0x04;
    if (DungeonsEnabled())       modules |= 0x08;
    if (FactionControlEnabled()) modules |= 0x10;

    WorldPacket data(SMSG_SUI_RTS_STATE, 64);
    data << uint8(rts ? 1 : 0) << modules << uint8(26);
    for (uint8 team = 0; team < 2; ++team)
    {
        data << uint64(rts ? uint64(HonorPool(team)) : 0);
        data << uint32(0) << uint32(0) << uint32(0);
        data << uint16(0);
        data << uint16(rts ? SuiHero::Fielded(team) : 0);
        data << uint16(rts ? SuiHero::SlotCap(team) : 0);
    }

    std::vector<SuiHero::Snapshot> heroes;
    if (HeroesEnabled())
        SuiHero::SnapshotRows(heroes);
    data << uint8(std::min<size_t>(255, heroes.size())) << uint8(12);
    for (size_t i = 0; i < heroes.size() && i < 255; ++i)
    {
        SuiHero::Snapshot const& hero = heroes[i];
        data << uint64(ObjectGuid(HIGHGUID_PLAYER, hero.GuidLow).GetRawValue());
        data << hero.Team << hero.Level << uint8(hero.Dead ? 1 : 0) << uint8(0);
    }
    data << uint8(0) << uint8(7);
    session->SendPacket(&data);
}

void HandleRtsAction(WorldSession* session, uint8 action, uint64 subjectGuid)
{
    session->SetSuiCapable(true);
    Player* player = session->GetPlayer();
    uint8 team = player && player->GetTeam() == HORDE ? 1 : 0;
    int64 poolAfter = HonorPool(team);
    ObjectGuid requested(subjectGuid);
    Player* subject = requested.IsPlayer()
        ? sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, requested.GetCounter())) : nullptr;
    uint8 result = SuiTacticalFreeze::IsSessionGameplayFrozen(session) ||
        (subject && subject->IsSuiTacticallyFrozen())
        ? uint8(4) : SuiHero::HandleAction(player, action, subjectGuid, poolAfter);

    WorldPacket data(SMSG_SUI_RTS_ACTION_RESULT, 18);
    data << action << result;
    uint64 normalized = requested.IsPlayer()
        ? ObjectGuid(HIGHGUID_PLAYER, requested.GetCounter()).GetRawValue() : subjectGuid;
    data << normalized << uint64(poolAfter);
    session->SendPacket(&data);
}
}

void WorldSession::HandleSuiRtsStateOpcode(WorldPackets::SuiRts::RtsState const& packet)
{
    SuiRts::HandleRtsState(this, packet.flags);
}

void WorldSession::HandleSuiRtsActionOpcode(WorldPackets::SuiRts::RtsAction const& packet)
{
    SuiRts::HandleRtsAction(this, packet.action, packet.subjectGuid);
}

bool ChatHandler::HandleSuiRtsCommand(char* args)
{
    if (char* sub = ExtractLiteralArg(&args))
    {
        std::string command = sub;
        if (command == "reload")
        {
            SendSysMessage("SUI RTS rules are boot-latched; runtime reload is disabled.");
            return true;
        }
        if (command == "heroes")
        {
            std::vector<SuiHero::Snapshot> heroes;
            SuiHero::SnapshotRows(heroes);
            PSendSysMessage("SUI RTS heroes: %u", uint32(heroes.size()));
            for (SuiHero::Snapshot const& hero : heroes)
            {
                Player* online = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, hero.GuidLow));
                PSendSysMessage("  %s guid=%u team=%u level=%u %s",
                    online ? online->GetName() : "<offline>", hero.GuidLow,
                    hero.Team, hero.Level, hero.Dead ? "dead" : "alive");
            }
            return true;
        }
        if (command == "honor")
        {
            char* op = ExtractLiteralArg(&args);
            char* side = ExtractLiteralArg(&args);
            int32 amount = 0;
            if (!op || std::string(op) != "add" || !side || !ExtractInt32(&args, amount) || amount <= 0)
            {
                SendSysMessage("Usage: .sui rts honor add alliance|horde <positive amount>");
                return false;
            }
            std::string faction = side;
            if (faction != "alliance" && faction != "horde")
            {
                SendSysMessage("Faction must be alliance or horde.");
                return false;
            }
            uint8 team = faction == "horde" ? 1 : 0;
            int64 before = SuiRts::HonorPool(team);
            SuiRts::AddHonor(team, amount);
            PSendSysMessage("SUI RTS diagnostic Honor %s: %ld -> %ld",
                faction.c_str(), long(before), long(SuiRts::HonorPool(team)));
            return true;
        }
        if (command != "status")
        {
            SendSysMessage("Usage: .sui rts [status|heroes|honor add alliance|horde <amount>]");
            return false;
        }
    }

    PSendSysMessage("SUI RTS worldstate: %s", SuiWorldState::RtsWorldState() ? "RTS MATCH" : "vanilla");
    PSendSysMessage("  modules: honor=%u heroes=%u territory=%u dungeons=%u faction-control=%u",
        SuiRts::HonorEnabled(), SuiRts::HeroesEnabled(), SuiRts::TerritoryEnabled(),
        SuiRts::DungeonsEnabled(), SuiRts::FactionControlEnabled());
    PSendSysMessage("  honor pools: alliance=%ld horde=%ld",
        long(SuiRts::HonorPool(0)), long(SuiRts::HonorPool(1)));
    PSendSysMessage("  bot caps: alliance=%ld horde=%ld (-1 = uncapped)",
        long(SuiRts::BotCap(0)), long(SuiRts::BotCap(1)));
    return true;
}
