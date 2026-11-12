/* SuperUI macro rate hooks: per-level XP curve and the live rate readback
 * used by MangosSuperUI's Gameplay Tuning > Rates pages. */

#ifndef MANGOS_SUI_RATES_H
#define MANGOS_SUI_RATES_H

#include "Common.h"

#include <string>

namespace SuiRates
{
    // Parses Rate.XP.LevelCurve, e.g. "1-19:3 20-39:2 60:0.5" (bands separated by
    // spaces, commas or semicolons; a later band overrides an earlier one). Called
    // from World::LoadConfigSettings on boot and on .reload config. A malformed band
    // is logged and skipped, never fatal.
    void LoadLevelCurve(std::string const& spec);

    // XP multiplier for a player at this level; 1.0 when no band covers it.
    float LevelCurveRate(uint32 level);

    // The accepted bands, normalized ("1-19:3 20-39:2"); empty when none.
    std::string const& LevelCurveSpec();
}

#endif
