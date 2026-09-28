#ifndef MANGOS_SUI_RAID_TELEMETRY_H
#define MANGOS_SUI_RAID_TELEMETRY_H
#include <tuple>
#include <vector>

/*
 * SuiRaidTelemetry — what happened in a fight, for the group the autopilot is driving.
 * Damage taken (by source and spell), healing done (by healer), and every death with the
 * hits that led to it, written as [TELEMETRY] lines. The hooks sit on Unit::DealDamage,
 * Unit::Kill and SpellCaster::DealHeal; the only cost outside a tracked group is one atomic
 * pointer compare.
 */

#include "Common.h"

class Group;
class Unit;
class Player;
struct SpellEntry;

namespace SuiRaidTelemetry
{
    void Track(Group const* group);          // null stops tracking
    void OnDamage(Unit* attacker, Unit* victim, uint32 damage, SpellEntry const* spell);
    // School mask of the spell damage that hit this unit in the last 6 s (0 = none).
    uint32 RecentDamageSchools(Unit const* victim);
    // A creature pulsing area damage around itself right now (its self-centred area spell hit the
    // raid twice within 2.5 s, the last time under 1.5 s ago): the area's radius, else 0.
    float PulseRadius(Unit const* creature);
    // The pulse radius a creature of this kind has been seen to carry (0 = never seen pulsing).
    float EntryPulseRadius(uint32 entry);
    // The reach of a frontal cone a creature of this kind has hit the raid with (0 = never seen).
    float EntryConeRadius(uint32 entry);
    // The burst radius of a spell this kind casts at a unit that bursts around it (0 = none seen).
    float EntrySplashRadius(uint32 entry);
    // Milliseconds since this creature's last pulsing burst began (0 = never seen bursting).
    uint32 PulseAge(Unit const* creature);
    // Typical milliseconds between the starts of this kind's area bursts (0 = not known yet).
    uint32 EntryBurstInterval(uint32 entry);
    // The biggest single hit this kind's area burst has landed on a member (0 = not seen).
    uint32 EntryBurstMaxHit(uint32 entry);
    // A creature began a spell: if it opens a chain of impacts at fixed spots, remember them.
    void OnCreatureCast(Unit* caster, SpellEntry const* spell);
    // Forecast impact circles (x, y, radius) on this map instance still to come.
    void ForecastHazards(Unit const* around, float range, std::vector<std::tuple<float, float, float>>& out);
    void OnHeal(Unit* healer, Unit* target, uint32 amount, SpellEntry const* spell);
    void OnKill(Unit* killer, Unit* victim);
    void FlushFight(Player const* leader, uint32 fightNumber, uint32 seconds);
    // outcome: -2 null spell, -1 refused by the bot's own gate, else the SpellCastResult.
    void OnCastAttempt(Unit* caster, uint32 spellId, int32 outcome);
    // why: 0 ran, 1 not in combat, 2 casting, 3 pull hold, 4 stalemate/flee hold, 5 other gate.
    void OnRotationTick(Unit* caster, uint8 why);
}

#endif
