#ifndef MANGOS_SUI_RAID_SUPPLY_H
#define MANGOS_SUI_RAID_SUPPLY_H
#include "Common.h"
class Player;

// ORDER_SUPPLY (CMSG_SUI_ORDER type 16): the raid quartermaster. A commanded bot's bags are
// topped up to the per-role loadout in the owner-editable policy file `sui-raid-supply.json`
// beside mangosd.conf (consumables, ammunition, pet food, repairs). Top-up only: nothing is
// removed. The order's `x` is the encounter role the commander assigned (1 tank, 2 add tank,
// 3 healer, 4 melee, 5 ranged; 0 = unassigned, derived from the body). Every item identity
// lives in the policy file; the code only knows roles, power types and weapon classes.
namespace SuiRaidSupply
{
    void Supply(Player* commander, Player* bot, uint32 role);
    // One receipt to the commander for everything supplied since the last flush.
    void Flush(Player* commander);
}
#endif
