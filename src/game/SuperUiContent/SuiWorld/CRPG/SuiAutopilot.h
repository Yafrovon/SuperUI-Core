#ifndef MANGOS_SUI_AUTOPILOT_H
#define MANGOS_SUI_AUTOPILOT_H

/*
 * SuiAutopilot — a bot raid/party leader that runs content with no human present
 * (owner 2026-09-23: "run MC, drive the whole thing ... raid the entirety of classic and all
 * dungeons").
 *
 * One AiBot (normally the main tank) is named leader with `.autopilot on <name>`. Every other
 * bot in its group escorts it exactly as it would escort a human (FindPartyBoss /
 * FindEscortBoss fall back to the leader when the group holds no real player), so formation,
 * assist and the group tactics are the SAME code a human-led party runs. The leader itself:
 *   - fights whatever engages the group (the PlayerParty doctrine, leader as its own anchor);
 *   - out of combat, waits until the group is gathered, alive and rested;
 *   - then picks the nearest reachable hostile, walks to it and pulls it;
 *   - stops itself (paused) after repeated wipes on the same creature, and logs why.
 * Nothing here knows a boss, an instance or a spell: the next target is simply the nearest
 * living hostile the navmesh can reach. [AUTOPILOT] log lines are the evidence channel.
 */

#include "Common.h"
#include "ObjectGuid.h"
#include <string>

class AiBotAI;
class Player;
class Unit;

namespace SuiAutopilot
{
    // Leader must be an AiBot in a group. Returns false with a reason otherwise.
    bool Enable(Player* leader, std::string& reply);
    void Disable(ObjectGuid leader);
    void DisableAll();
    void SetPaused(ObjectGuid leader, bool paused);
    bool IsLeader(Player const* player);
    // The enabled, living, in-world leader of member's group (never member itself), or null.
    Player* LeaderFor(Player const* member);
    // Out-of-combat driving for the leader. True = the tick was consumed (moving / pulling).
    bool TickLeader(AiBotAI* ai);
    // True while the member's leader walks out to pull and back: the raid holds its camp.
    bool HoldForPull(Player const* member);
    // While the raid waits (rest, a patrol, a gather) and the leader stands still, members stack
    // within a few yards of it instead of spreading into a wide follow formation. 0 = does not
    // apply, 1 = in place (hold), 2 = move to (x, y, z).
    int StackOnLeader(Player const* member, float& x, float& y, float& z);
    // Where the member's raid is fighting from (its camp), while it fights; false otherwise.
    bool FightAnchor(Player const* member, float& x, float& y, float& z);
    // Leader death notification from the bot's death handling (wipe accounting).
    void NoteLeaderDeath(Player const* leader);
    // A kill by a member of an autopilot group: the spawn counts as cleared. A cleared spawn
    // that respawns is executed without a fight (owner 2026-09-23: "if a pack is cleared, its
    // cleared. If it respawns, save time and gm execute them").
    void NoteKill(Unit* killer, Unit* victim);
    std::string Status();
}

#endif
