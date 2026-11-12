/* SuperUI macro rate hooks. See SuiRates.h. */

#include "SuiRates.h"
#include "SuiWorldState.h"
#include "Chat.h"
#include "Log.h"
#include "World.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace
{
    // Indexed by player level (0 unused). Written only by config (re)load on the
    // world thread; read lock-free from map threads, exactly like sWorld's rates.
    std::array<float, 256> s_levelCurve = []
    {
        std::array<float, 256> curve;
        curve.fill(1.0f);
        return curve;
    }();
    std::string s_levelCurveSpec;

    bool ParseBand(std::string const& band, uint32& first, uint32& last, float& rate)
    {
        std::size_t const colon = band.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= band.size())
            return false;

        std::string const range = band.substr(0, colon);
        std::size_t const dash = range.find('-');
        char* end = nullptr;
        unsigned long lo = std::strtoul(range.c_str(), &end, 10);
        unsigned long hi = lo;
        if (dash == std::string::npos)
        {
            if (*end != '\0')
                return false;
        }
        else
        {
            if (end != range.c_str() + dash)
                return false;
            hi = std::strtoul(range.c_str() + dash + 1, &end, 10);
            if (*end != '\0')
                return false;
        }

        double const value = std::strtod(band.c_str() + colon + 1, &end);
        if (*end != '\0' || !std::isfinite(value) || value < 0.0 || value > 1000.0)
            return false;
        if (lo < 1 || hi < lo || hi > 255)
            return false;

        first = uint32(lo);
        last = uint32(hi);
        rate = float(value);
        return true;
    }
}

namespace SuiRates
{
void LoadLevelCurve(std::string const& spec)
{
    s_levelCurve.fill(1.0f);
    s_levelCurveSpec.clear();

    std::string normalized = spec;
    for (char& c : normalized)
        if (c == ',' || c == ';' || c == '"')
            c = ' ';

    std::istringstream bands(normalized);
    std::string band;
    while (bands >> band)
    {
        uint32 first = 0, last = 0;
        float rate = 1.0f;
        if (!ParseBand(band, first, last, rate))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[SUI-RATES] Rate.XP.LevelCurve: ignoring malformed band '%s' (want LO-HI:RATE or LVL:RATE)",
                band.c_str());
            continue;
        }

        for (uint32 level = first; level <= last; ++level)
            s_levelCurve[level] = rate;

        char text[48];
        if (first == last)
            snprintf(text, sizeof(text), "%u:%g", first, rate);
        else
            snprintf(text, sizeof(text), "%u-%u:%g", first, last, rate);
        if (!s_levelCurveSpec.empty())
            s_levelCurveSpec += ' ';
        s_levelCurveSpec += text;
    }

    if (!s_levelCurveSpec.empty())
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI-RATES] XP level curve: %s", s_levelCurveSpec.c_str());
}

float LevelCurveRate(uint32 level)
{
    return level < s_levelCurve.size() ? s_levelCurve[level] : 1.0f;
}

std::string const& LevelCurveSpec()
{
    return s_levelCurveSpec;
}
}

// .sui rates — prints the in-memory rate values as key=value lines. MangosSuperUI
// reads this over RA to show what the running core actually uses (as opposed to
// what mangosd.conf says), and treats "hooks=1" as proof this build has the hooks.
bool ChatHandler::HandleSuiRatesCommand(char* /*args*/)
{
    static struct { char const* key; eConfigFloatValues index; } const RATES[] =
    {
        { "Rate.XP.Kill",                       CONFIG_FLOAT_RATE_XP_KILL },
        { "Rate.XP.Kill.Elite",                 CONFIG_FLOAT_RATE_XP_KILL_ELITE },
        { "Rate.XP.Quest",                      CONFIG_FLOAT_RATE_XP_QUEST },
        { "Rate.XP.Explore",                    CONFIG_FLOAT_RATE_XP_EXPLORE },
        { "Rate.XP.Personal.Min",               CONFIG_FLOAT_RATE_XP_PERSONAL_MIN },
        { "Rate.XP.Personal.Max",               CONFIG_FLOAT_RATE_XP_PERSONAL_MAX },
        { "Rate.Rest.InGame",                   CONFIG_FLOAT_RATE_REST_INGAME },
        { "Rate.Rest.Offline.InTavernOrCity",   CONFIG_FLOAT_RATE_REST_OFFLINE_IN_TAVERN_OR_CITY },
        { "Rate.Rest.Offline.InWilderness",     CONFIG_FLOAT_RATE_REST_OFFLINE_IN_WILDERNESS },
        { "Rate.Reputation.Gain",               CONFIG_FLOAT_RATE_REPUTATION_GAIN },
        { "Rate.Reputation.Kill",               CONFIG_FLOAT_RATE_REPUTATION_KILL },
        { "Rate.Reputation.Quest",              CONFIG_FLOAT_RATE_REPUTATION_QUEST },
        { "Rate.Reputation.Spell",              CONFIG_FLOAT_RATE_REPUTATION_SPELL },
        { "Rate.Reputation.LowLevel.Kill",      CONFIG_FLOAT_RATE_REPUTATION_LOWLEVEL_KILL },
        { "Rate.Reputation.LowLevel.Quest",     CONFIG_FLOAT_RATE_REPUTATION_LOWLEVEL_QUEST },
        { "Rate.Honor.Kill",                    CONFIG_FLOAT_RATE_HONOR_KILL },
        { "Rate.Honor.Bonus",                   CONFIG_FLOAT_RATE_HONOR_BONUS },
        { "Rate.Honor.Quest",                   CONFIG_FLOAT_RATE_HONOR_QUEST },
        { "Rate.Honor.Dishonor",                CONFIG_FLOAT_RATE_HONOR_DISHONOR },
        { "Rate.Honor.RankPoints",              CONFIG_FLOAT_RATE_HONOR_RANK_POINTS },
        { "RpDecay",                            CONFIG_FLOAT_RP_DECAY },
    };

    PSendSysMessage("SUI rates: hooks=1 rts=%u", SuiWorldState::RtsWorldState() ? 1u : 0u);
    for (auto const& rate : RATES)
        PSendSysMessage("%s=%g", rate.key, sWorld.getConfig(rate.index));
    PSendSysMessage("Rate.XP.LevelCurve=%s", SuiRates::LevelCurveSpec().c_str());
    return true;
}
