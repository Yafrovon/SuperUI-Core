/*
 * SuiRaidPlaybook.h — per-raid and per-boss DATA for the raid autopilot (owner decision 2026-11-03:
 * "a per-boss data playbook keyed by creature entry, on top of the generic autopilot; code only adds
 * reusable primitives"). The file is sui-raid-playbook.json beside mangosd.conf (falls back to the
 * .dist template the build installs there); it is re-read when it changes, no restart needed.
 *
 * What lives here is what a raid leader reads off a guide, never a script: the order the bosses die
 * in (a roadmap), and per boss entry the assignments its fight wants. Line endings: LF.
 */

#ifndef SUI_RAID_PLAYBOOK_H
#define SUI_RAID_PLAYBOOK_H

#include "Common.h"
#include <string>
#include <vector>

class Map;

namespace SuiRaidPlaybook
{
    // Boss entries of a raid map in kill order (empty when the playbook has no order for it).
    std::vector<uint32> Order(uint32 mapId);

    // Is this boss entry dead in `map` (the instance's respawn store, or a loaded corpse)?
    bool BossDead(Map const* map, uint32 bossEntry);

    // The first boss before `bossEntry` in its map's order that is still alive in `map`, else 0.
    // 0 too when `bossEntry` is not in the order.
    uint32 EarlierAliveBoss(Map const* map, uint32 bossEntry);

    // The first boss of the order still alive in `map`, else 0.
    uint32 FirstAliveBoss(Map const* map);

    // Spawn position of a boss entry on a map (its first spawn row); false when it has none.
    bool BossSpawn(uint32 mapId, uint32 bossEntry, float& x, float& y, float& z);

    // Display name the playbook gives a boss entry (for logs), else "entry N".
    std::string Name(uint32 bossEntry);

    // A lure for a patrolling boss (bosses.<entry>.lure): the spot beside its walk where the leader and
    // the tanks wait for it to notice them as it passes, and optionally where the raid camps. False
    // when the playbook has none for this entry.
    bool Lure(uint32 bossEntry, float& lx, float& ly, float& lz, bool& hasCamp, float& cx, float& cy, float& cz);
    // How far every idle pack must stand from the lure before it is used (bosses.<entry>.lure.clearance,
    // default 45 yd); nearer packs go first, timed around the boss.
    float LureClearYards(uint32 bossEntry);

    // Where an add entry stands in its boss's kill order (bosses.<entry>.killOrder, add entries, the
    // first dies first): 0, 1, ...; -1 when the boss has no list or the add is not in it.
    int KillOrder(uint32 bossEntry, uint32 addEntry);
}

#endif
