#ifndef MANGOS_SUI_RAID_PREPARE_H
#define MANGOS_SUI_RAID_PREPARE_H
#include "Common.h"
class AiBotAI;
class Player;

// ORDER_PREPARE (CMSG_SUI_ORDER type 15): a commanded bot gets raid-ready the way a
// player does before a pull - it buffs the group from its own spellbook, drinks the
// elixirs and flasks it carries, pre-drinks a school-absorb potion, eats until it is
// well fed and feeds its pet. Nothing is granted or created: only learned spells and
// carried items are used, through the ordinary cast and item-use paths with their
// normal costs, reagents, cooldowns and consumption. No item, spell, class or boss
// identity appears here; every choice is read from the spell and item data.
namespace SuiRaidPrepare
{
    // schoolMask: the damage schools the raid expects (SpellSchoolMask bits). 0 lets the
    // carried potions decide: a single carried absorb school is drunk, several are not.
    bool Begin(Player* commander, Player* bot, AiBotAI* ai, uint32 schoolMask);
    // Returns true only while preparation owns this actor's AI tick.
    bool Tick(AiBotAI* ai, uint32 diff);
    bool Preparing(Player const* bot);
}
#endif
