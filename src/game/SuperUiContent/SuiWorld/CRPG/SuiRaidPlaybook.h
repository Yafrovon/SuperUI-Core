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

    // Is this boss entry dead in `map` (bosses.<entry>.doneData: that instance data reads DONE; else the
    // instance's respawn store, or a loaded corpse)?
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

    // A boss a friendly NPC summons through its gossip (bosses.<entry>.summonGossip): the NPC entry, where
    // it stands, and how many times the first option is chosen (menu after menu) to start the summon.
    // standAt (hasStand): where the raid waits for the summoned boss and pulls it from, else beside the NPC.
    struct Gossip { uint32 npc; float at[3]; uint32 selects; bool hasStand; float stand[3]; };
    bool SummonGossip(uint32 bossEntry, Gossip& out);

    // How far apart the ranged and healers stand while this boss is in the fight (bosses.<entry>.spread), else 0: for a
    // burst the boss does not cast itself, so the learned splash never sees it (Ragnaros: Might of Ragnaros summons a
    // trigger on a random mana user that bursts 20 yd).
    float SpreadYards(uint32 bossEntry);

    // bosses.<entry>.standInPulse: the raid does not keep clear of this boss's learned pulse circle (a light, slow burst
    // whose circle is as wide as the casters' reach).
    bool StandInPulse(uint32 bossEntry);

    // bosses.<entry>.prePotions: item entries every member drinks (if it carries one and does not wear its aura) while
    // the raid waits for this boss (Ragnaros: Greater Fire Protection Potion, from the quartermaster's supply).
    std::vector<uint32> PrePotions(uint32 bossEntry);

    // bosses.<entry>.paladinAura: aura spell ids, highest rank first, every paladin of the raid holds for this boss.
    std::vector<uint32> PaladinAura(uint32 bossEntry);
}

#endif
