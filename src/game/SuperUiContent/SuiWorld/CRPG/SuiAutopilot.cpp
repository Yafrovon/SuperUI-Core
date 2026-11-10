/*
 * SuiAutopilot.cpp — see SuiAutopilot.h. Line endings: LF (C++ repo convention).
 */

#include "CreatureGroups.h"
#include "SuiAutopilot.h"
#include "SuiRaidTelemetry.h"
#include "SuiRaidSupply.h"
#include "SuiRaidPlaybook.h"
#include "AiBotAIMain.h"
#include "Chat.h"
#include "Creature.h"
#include "GameObject.h"
#include "LootMgr.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "PathFinder.h"
#include "Player.h"
#include "Spell.h"
#include "Pet.h"
#include "Log.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Movement/WaypointManager.h"
#include "Config/Config.h"
#include "MapPersistentStateMgr.h"
#include "DBCStores.h"
#include "World.h"
#include <fstream>
#include <tuple>

#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace
{
    // Tuning. All of it is ordinary raiding practice, none of it knows an encounter.
    // How far the leader looks for the next pull: near first, then wider, so the route stays
    // local but the raid never stalls with packs left beyond the first ring.
    constexpr float kSearchRings[] = { 120.0f, 250.0f, 450.0f, 800.0f, 1200.0f };
    constexpr uint32 kEmptySearchBackoffMs = 10 * 1000;
    constexpr float kPullYards = 25.0f;         // start the pull from here (with line of sight)
    constexpr float kGatherYards = 40.0f;       // everyone this close before a pull
    constexpr float kRestHealthPct = 80.0f;     // rest until health and mana are back
    constexpr float kRestManaPct = 70.0f;
    constexpr uint32 kRestCapMs = 90 * 1000;    // never wait longer than this for stragglers
    constexpr uint32 kStuckMs = 20 * 1000;      // no approach progress for this long = skip it
    constexpr uint32 kWipesBeforePause = 3;     // the same creature killed the leader this often

    enum class Phase : uint8 { Idle, Rest, Approach, Pull, Return, Fight };
    constexpr float kCampYards = 40.0f;        // the raid stops this far from the pack
    constexpr float kGatheredYards = 20.0f;    // and gathers this close before the pull
    constexpr uint32 kGatherCapMs = 45 * 1000;

    struct Pilot
    {
        ObjectGuid leader;
        bool paused = false;
        Phase phase = Phase::Idle;
        ObjectGuid target;
        uint32 targetEntry = 0;
        uint32 restMs = 0;
        uint32 approachMs = 0;
        uint32 stuckMs = 0;
        float lastX = 0.0f, lastY = 0.0f;   // where the leader stood at the last progress check
        uint32 moveCooldownMs = 0;
        float bestDist = 1e9f;
        uint32 fightMs = 0;
        uint32 pulls = 0;
        uint32 fights = 0;
        uint32 searchBackoffMs = 0;
        float campX = 0.0f, campY = 0.0f, campZ = 0.0f;   // where the raid waits for the pull
        uint32 returnMs = 0;
        uint32 gatherMs = 0;
        uint32 patrolWaitMs = 0;
        // The pull plan: where the leader stands to pull (outside the pack's reach, in sight of
        // it) and where the raid camps (behind that, away from every other pack).
        bool planned = false;
        float pullX = 0.0f, pullY = 0.0f, pullZ = 0.0f;
        float planTX = 0.0f, planTY = 0.0f;
        uint32 pullShotMs = 0;
        uint32 assistWaitMs = 0;   // the pull shot held while a neighbour stands in the target's call for help
        float planDp = 0.0f, planDc = 0.0f, planRoute = 0.0f;   // the plan's clearances
        ObjectGuid crowder;                                        // the pack closest to the camp
        uint32 crowdHops = 0;
        uint32 crowdWaitMs = 0;
        uint32 planHoldMs = 0;
        bool planFresh = false;   // a new plan: the walk still under way heads for the target itself
        // Ambush of a patrolling target: the point on its walk where it is pulled.
        ObjectGuid ambushTarget;
        float ambushX = 0.0f, ambushY = 0.0f, ambushZ = 0.0f;
        uint32 ambushWaitMs = 0;
        // The last spot the leader stood on with no idle pack within reach: where the raid rests
        // and waits out patrols (2026-09-24: waiting 32 yd from an imp pack, a Surger patrol walked
        // into the raid and the imps joined).
        bool hasSafe = false;
        float safeX = 0.0f, safeY = 0.0f, safeZ = 0.0f;
        uint32 safeCheckMs = 0;
        bool retreating = false;   // the whole raid falls back with the leader (early aggro)
        std::map<ObjectGuid, uint32> skip;       // unreachable / stuck targets -> retry after (ms clock)
        uint32 prepNextMs = 0;
        bool offMesh = false;
        uint32 offMeshTries = 0;
        std::map<uint32, uint32> exploreSkip;   // spawn guid -> retry after
        std::map<uint32, uint32> usedObjects;   // scripted object spawn guid -> retry after (ms clock)
        uint32 useStep = 0;                      // 0 = walk to it, 1 = beside it: use
        uint32 exploreStallMs = 0;
        std::map<uint32, uint32> wipesByEntry;
        std::map<uint32, uint32> deferEntry;     // entry -> retry after (ms clock): it keeps winning
        struct Zone { float x, y, z, r; uint32 until; };
        std::vector<Zone> deferZones;            // areas that keep winning: everything there waits
        std::string lastEvent;
        bool leaderDied = false;   // the leader fell in this fight: the record still runs to its end
        bool review = true;        // pause after a boss fight or a wipe (owner 2026-11-03: review per boss)
        ObjectGuid noAmbush;       // a patrol whose ambush timed out: pulled where it walks
        std::map<uint32, uint32> gateNotedMs;   // boss entry -> last "waits for" note
        ObjectGuid timedBy;        // a patrolling boss the next pull is timed around (pulled while it is away)
        ObjectGuid passedFrom;     // the target a "stands beside the walk" hop left (never hopped back to)
        uint32 passHops = 0;
        uint32 timedWaitMs = 0;
        float timedLastNear = 0.0f;
        float timedLastT = 0.0f, timedLastC = 0.0f, timedLastM = 0.0f;   // his last distances: pack, camp, leader
        uint32 timedClearTicks = 0;   // ticks in a row he has been away and walking away
        bool lured = false;        // the plan is the playbook's lure: being seen at the camp is the point
        bool hasRear = false;      // timed pull: the raid waits here, the tanks at the camp
        float rearX = 0.0f, rearY = 0.0f, rearZ = 0.0f;
        std::map<ObjectGuid, uint32> notFirstUntil;   // patrolling boss whose ambush was too crowded: packs first
    };

    std::recursive_mutex gLock;
    std::map<ObjectGuid, Pilot> gPilots;

    // Cleared spawns (map, creature table guid), kept in a file beside mangosd.conf so a server
    // restart does not forget what the raid already killed.
    std::set<std::tuple<uint32, uint32, uint32>> gCleared;   // map, instance, spawn
    bool gClearedLoaded = false;

    std::string ClearedPath()
    {
        std::string conf = sConfig.GetFilename();
        size_t slash = conf.find_last_of('/');
        return (slash == std::string::npos ? std::string() : conf.substr(0, slash + 1)) + "sui-autopilot-cleared.txt";
    }

    void LoadCleared()
    {
        if (gClearedLoaded)
            return;
        gClearedLoaded = true;
        std::ifstream in(ClearedPath());
        uint32 map = 0, instance = 0, guid = 0;
        while (in >> map >> instance >> guid)
            gCleared.insert(std::make_tuple(map, instance, guid));
    }

    std::string DeferredPath()
    {
        std::string const cleared = ClearedPath();
        return cleared.substr(0, cleared.find_last_of('/') + 1) + "sui-autopilot-deferred.txt";
    }

    bool IsCleared(Creature const* c)
    {
        LoadCleared();
        uint32 const guid = c->HasStaticDBSpawnData() ? c->GetDBTableGUIDLow() : 0;
        return guid && gCleared.count(std::make_tuple(c->GetMapId(), c->GetInstanceId(), guid));
    }

    // A creature that really walks somewhere: a waypoint route reaching 35 yd or more from its
    // spawn. A short loop around its own adds is standing still for every pull decision (2026-11-03:
    // Garr's six-point loop around his Firesworn - ambushes abandoned and "waiting for Garr to walk
    // clear of the camp" for 20 minutes).
    bool Patrols(Creature const* c)
    {
        if (c->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE || !c->HasStaticDBSpawnData())
            return false;
        static std::map<uint32, bool> cache;   // spawn guid -> walks far
        auto it = cache.find(c->GetDBTableGUIDLow());
        if (it != cache.end())
            return it->second;
        bool far = false;
        float hx = 0.0f, hy = 0.0f, hz = 0.0f;
        c->GetRespawnCoord(hx, hy, hz);
        if (WaypointPath const* path = sWaypointMgr.GetDefaultPath(c->GetEntry(), c->GetDBTableGUIDLow()))
            for (auto const& node : *path)
                if ((node.second.x - hx) * (node.second.x - hx) + (node.second.y - hy) * (node.second.y - hy) >= 35.0f * 35.0f)
                    far = true;
        cache[c->GetDBTableGUIDLow()] = far;
        return far;
    }

    Player* Find(ObjectGuid guid)
    {
        return guid.IsEmpty() ? nullptr : sObjectMgr.GetPlayer(guid);
    }

    bool UsesMana(Player const* p)
    {
        return p->GetPowerType() == POWER_MANA && p->GetMaxPower(POWER_MANA) > 0;
    }

    struct Readiness
    {
        uint32 members = 0, dead = 0, far = 0, low = 0, unsettled = 0;
        bool inCombat = false;
        bool Ready() const { return !inCombat && dead == 0 && far == 0 && low == 0 && unsettled == 0; }
    };

    Readiness Assess(Player* leader)
    {
        Readiness r;
        Group* group = leader->GetGroup();
        if (!group)
            return r;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->getSource();
            if (!m || !m->IsInWorld() || m->GetMap() != leader->GetMap())
                continue;
            ++r.members;
            if (!m->IsAlive())
            {
                ++r.dead;
                continue;
            }
            if (m->IsInCombat() && (m->GetVictim() || !m->GetAttackers().empty()))
                r.inCombat = true;
            if (m != leader && !m->IsWithinDistInMap(leader, kGatherYards))
                ++r.far;
            if (m->GetHealthPercent() < kRestHealthPct ||
                (UsesMana(m) && m->GetPowerPercent(POWER_MANA) < kRestManaPct))
                ++r.low;
        }
        // A hostile nearby that is still fighting or walking home (evading) joins whatever is
        // pulled next: wait for it to settle (2026-09-24: the second Molten Giant of a pair,
        // evading after a wipe, walked into the Firelord pull).
        std::list<Creature*> around;
        MaNGOS::AnyUnitInObjectRangeCheck check(leader, 60.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
        Cell::VisitGridObjects(leader, searcher, 60.0f);
        for (Creature* c : around)
            if (c->IsAlive() && c->IsHostileTo(leader) && !c->IsTotem() && c->GetCreatureType() != CREATURE_TYPE_CRITTER &&
                (c->IsInCombat() || c->IsInEvadeMode()) && !(c->GetVictim() && group->IsMember(c->GetVictim()->GetObjectGuid())))
                ++r.unsettled;
        return r;
    }

    // Walked path length to a point, or a negative value when the navmesh has no way there.
    float PathLength(Player* me, float x, float y, float z)
    {
        PathInfo path(me);
        path.calculate(x, y, z, false);
        if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            return -1.0f;
        PointsArray const& points = path.getPath();
        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            float const dx = points[i].x - points[i - 1].x, dy = points[i].y - points[i - 1].y,
                dz = points[i].z - points[i - 1].z;
            length += std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        return length;
    }

    bool Pullable(Player* me, Creature* c, Pilot const& pilot, bool ignoreSkip = false)
    {
        return c && c->IsAlive() && c->IsInWorld() && !c->IsInCombat() && !c->IsTotem() &&
            !c->IsCivilian() && c->GetCreatureType() != CREATURE_TYPE_CRITTER &&
            c->IsHostileTo(me) && me->IsValidAttackTarget(c) &&
            c->GetUInt32Value(UNIT_NPC_FLAGS) == 0 && !c->IsGuard() &&   // never a flight master, vendor or guard
            !c->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_IMMUNE_TO_PLAYER) &&
            !(!ignoreSkip && pilot.skip.count(c->GetObjectGuid()) && WorldTimer::getMSTime() < pilot.skip.at(c->GetObjectGuid())) &&
            !IsCleared(c) &&
            !(pilot.deferEntry.count(c->GetEntry()) && WorldTimer::getMSTime() < pilot.deferEntry.at(c->GetEntry())) &&
            std::none_of(pilot.deferZones.begin(), pilot.deferZones.end(), [c](Pilot::Zone const& z)
            {
                return WorldTimer::getMSTime() < z.until && std::fabs(c->GetPositionZ() - z.z) < 25.0f &&
                    c->GetDistance2d(z.x, z.y) < z.r;
            });
    }

    bool SamePack(Creature* target, Creature* c);

    // Idle hostiles within r of `around` that belong to another pack than `target` (or to
    // none): what a fight at that spot would drag in.
    template <typename F> void ForEachRoutePoint(Creature* c, F&& visit);

    uint32 IdleNeighbours(Player* me, WorldObject* around, Creature* target, float r, float loopR = -1.0f)
    {
        if (loopR < 0.0f)
            loopR = r;
        // A walking pack counts where its loop takes it, not only where it stands now (2026-11-05: an
        // Ancient Core Hound held where it met the raid, nothing idle within 45 yd; Garr's pack walked its
        // loop back, a Firesworn came within 25 yd of the melee and the whole pack joined - all dead).
        float const look = r + 60.0f;
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(around, look);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(around, searcher, look);
        float const ax = around->GetPositionX(), ay = around->GetPositionY(), az = around->GetPositionZ();
        uint32 count = 0;
        for (Creature* c : near)
        {
            if (c == target || !c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me))
                continue;
            if (!around->IsWithinDistInMap(c, r))
            {
                // the group's walker leads the loop; a member keeps its place up to ~10 yd off it
                Creature* walker = c;
                if (CreatureGroup* g = c->GetCreatureGroup())
                    if (Creature* lead = c->GetMap()->GetCreature(g->GetLeaderGuid()))
                        walker = lead;
                float const slack = walker == c ? 0.0f : 10.0f;
                bool reaches = false;
                ForEachRoutePoint(walker, [&](float x, float y, float z)
                {
                    if (!reaches && std::fabs(z - az) < 25.0f &&
                        (x - ax) * (x - ax) + (y - ay) * (y - ay) <= (loopR + slack) * (loopR + slack))
                        reaches = true;
                });
                if (!reaches)
                    continue;
            }
            if (target && ((c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup()) ||
                           (c->HasStaticDBSpawnData() && SamePack(target, c))))
                continue;
            ++count;
        }
        return count;
    }

    // The nearest reachable hostile by walked distance: the eight nearest by straight line
    // are pathed and the shortest walk wins, so a pack across a lava gap loses to the one
    // at the end of the corridor.
    bool WaitsForEarlierBoss(Player* me, Pilot& pilot, Creature* c);
    bool BossGoal(Player* me, float& gx, float& gy, float& gz, uint32* entryOut = nullptr);

    Creature* PickNextWithin(Player* me, Pilot& pilot, float radius)
    {
        std::list<Creature*> nearby;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, radius);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
        Cell::VisitGridObjects(me, searcher, radius);

        // Headed for the next boss of the order: packs that lie away from it wait (2026-11-03: after a
        // restart at the entrance the raid explored Geddon's side, 600 yd from Magmadar, and pulled him).
        float gx = 0, gy = 0, gz = 0;
        bool const goal = BossGoal(me, gx, gy, gz) && me->GetDistance(gx, gy, gz) > 150.0f;
        float const meToGoal = goal ? me->GetDistance(gx, gy, gz) : 0.0f;
        // ...by the walk, not the straight line: MC winds, and a pack 490 yd west of the entrance was
        // "nearer Magmadar" than the raid (2026-11-05). On the way = a detour of at most 40 yd.
        float const goalWalk = goal ? PathLength(me, gx, gy, gz) : -1.0f;
        std::vector<std::pair<float, Creature*>> byLine;
        for (Creature* c : nearby)
            if (Pullable(me, c, pilot) && !WaitsForEarlierBoss(me, pilot, c) &&
                !(goal && c->GetDistance(gx, gy, gz) > meToGoal + 15.0f))   // (60 yd let each pick drift a
                // little farther: 2026-11-05 the raid cleared its way 580 yd west of Magmadar into Garr's room)
                byLine.emplace_back(me->GetDistance(c), c);
        std::sort(byLine.begin(), byLine.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
        if (byLine.size() > 40)
            byLine.resize(40);   // looked at in order until 8 usable ones are compared

        Creature* best = nullptr;
        float bestWalk = 1e9f;
        uint32 failed = 0, probed = 0;
        for (auto const& entry : byLine)
        {
            if (++probed > 8)
                break;
            float const walk = PathLength(me, entry.second->GetPositionX(),
                entry.second->GetPositionY(), entry.second->GetPositionZ());
            if (walk < 0.0f)
                ++failed;
        }
        bool reachesKnown = false;
        if (failed >= 3 && failed == std::min<uint32>(probed, byLine.size()))
        {
            // Off the mesh, or just eight unreachable creatures (standing in the lava)? A walk to
            // a known good spot - the last clear spot, else the instance entrance - tells.
            float kx = 0, ky = 0, kz = 0;
            bool known = false;
            if (pilot.hasSafe && me->GetDistance(pilot.safeX, pilot.safeY, pilot.safeZ) > 8.0f)
            {
                kx = pilot.safeX; ky = pilot.safeY; kz = pilot.safeZ; known = true;
            }
            else if (AreaTriggerTeleport const* at = sObjectMgr.GetMapEntranceTrigger(me->GetMapId()))
                if (at->destination.mapId == me->GetMapId())
                {
                    kx = at->destination.x; ky = at->destination.y; kz = at->destination.z; known = true;
                    if (me->GetDistance(kx, ky, kz) <= 8.0f)
                        reachesKnown = true;   // standing on the entrance: on the mesh
                }
            if (known && !reachesKnown && PathLength(me, kx, ky, kz) >= 0.0f)
                reachesKnown = true;
        }
        if (failed >= 3 && failed == std::min<uint32>(probed, byLine.size()) && !reachesKnown)
        {
            // No way to anything: the leader stands off the navmesh (a knockback, a ledge), not
            // every creature in the instance being unreachable. Nobody is skipped.
            pilot.offMesh = true;
            return nullptr;
        }
        uint32 usable = 0, crossing = 0, pathless = 0;
        for (auto const& entry : byLine)
        {
            if (usable >= 8)
                break;
            float const walk = PathLength(me, entry.second->GetPositionX(),
                entry.second->GetPositionY(), entry.second->GetPositionZ());
            if (walk >= 0.0f && !pilot.deferZones.empty())
            {
                // Reaching it means walking through an area that keeps beating the raid.
                PathInfo path(me);
                path.calculate(entry.second->GetPositionX(), entry.second->GetPositionY(), entry.second->GetPositionZ(), false);
                bool crosses = false;
                uint32 const nowMs = WorldTimer::getMSTime();
                for (auto const& pt : path.getPath())
                    for (auto const& z : pilot.deferZones)
                        if (nowMs < z.until && std::fabs(pt.z - z.z) < 25.0f &&
                            (pt.x - z.x) * (pt.x - z.x) + (pt.y - z.y) * (pt.y - z.y) < z.r * z.r)
                            crosses = true;
                if (crosses)
                {
                    ++crossing;
                    continue;
                }
            }
            if (walk < 0.0f)
            {
                ++pathless;
                // Short: from a bad spot (a ledge, the fight's end) even a boss 40 yd away has no
                // path (2026-09-25: Magmadar was skipped for 10 min right beside his last pack).
                // Far off, a long walk only outruns the path search: not skipped, just not this pick.
                if (entry.first < 150.0f)
                    pilot.skip[entry.second->GetObjectGuid()] = WorldTimer::getMSTime() + 2 * 60 * 1000;
                sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[AUTOPILOT] %s: no path to %s (entry %u) - skipped",
                    me->GetName(), entry.second->GetName(), entry.second->GetEntry());
                continue;
            }
            if (goalWalk > 0.0f && walk + entry.second->GetDistance(gx, gy, gz) > goalWalk + 40.0f)
                continue;
            ++usable;
            float const score = walk + 60.0f * IdleNeighbours(me, entry.second, entry.second, 25.0f);
            if (score < bestWalk)
            {
                bestWalk = score;
                best = entry.second;
            }
        }
        if (!best && radius >= 1200.0f)
        {
            uint32 inRange = 0;
            for (Creature* c : nearby)
                if (c->IsAlive() && c->IsHostileTo(me) && !c->IsInCombat())
                    ++inRange;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AUTOPILOT] %s: pick found nothing - %u idle hostiles in %.0f yd, %u pullable, %u behind a deferred area, %u without a path",
                me->GetName(), inRange, radius, uint32(byLine.size()), crossing, pathless);
        }
        return best;
    }

    // c is part of target's pack: linked in data (one creature group), or summoned (a boss's adds
    // have no spawn row). A neighbouring pack is not (2026-11-03: two Molten Giants 20 yd from a Lava
    // Annihilator made it "a pack bursting every 4 s", the raid fought at the pull spot beside them,
    // and the giants, two more Annihilators and two patrols joined).
    bool SamePack(Creature* target, Creature* c)
    {
        if (!c->HasStaticDBSpawnData())
            return true;
        if (target->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup())
            return true;
        // ...or a neighbour that never wanders out of target's call for help
        // (CreatureFamilyAssistanceRadius, measured between the bodies): it always comes too.
        float const reach = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (reach <= 0.0f || !target->HasStaticDBSpawnData())
            return false;
        float tx, ty, tz, cx, cy, cz;
        target->GetRespawnCoord(tx, ty, tz);
        c->GetRespawnCoord(cx, cy, cz);
        float const homes = std::sqrt((tx - cx) * (tx - cx) + (ty - cy) * (ty - cy)) -
            target->GetObjectBoundingRadius() - c->GetObjectBoundingRadius();
        return homes + c->GetWanderDistance() + target->GetWanderDistance() <= reach;
    }

    // The nearest idle hostile of another pack inside target's call for help right now (in reach and
    // in sight, as Creature::CallAssistance picks them): it joins the moment target is struck
    // (2026-11-05: a Firelord wandering 8-18 yd from a Lava Annihilator came with it, the tanks chased
    // it to the second Firelord, all 39 dead).
    Creature* AssistNeighbour(Player* me, Creature* target, float margin)
    {
        float const reach = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (reach <= 0.0f)
            return nullptr;
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(target, reach + margin + 15.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(target, searcher, reach + margin + 15.0f);
        Creature* best = nullptr;
        float bestD = 1e9f;
        for (Creature* c : near)
        {
            if (c == target || !c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me) || SamePack(target, c))
                continue;
            float const d = target->GetDistance(c);
            if (d <= reach + margin && d < bestD && target->IsWithinLOSInMap(c))
            {
                best = c;
                bestD = d;
            }
        }
        return best;
    }

    // The boss a pull would start: the creature itself when it is a boss (world-boss rank), or the
    // boss its pack is linked to (its guards share the boss's creature group).
    Creature* BossOf(Creature* c)
    {
        if (c->IsWorldBoss())
            return c;
        if (!c->GetCreatureGroup())
            return nullptr;
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(c, 50.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(c, searcher, 50.0f);
        for (Creature* o : near)
            if (o->IsAlive() && o->IsWorldBoss() && o->GetCreatureGroup() == c->GetCreatureGroup())
                return o;
        return nullptr;
    }

    // The playbook's order: a boss (and its pack) waits while a boss before it in its raid's order
    // is alive here (2026-11-03: Garr fought before Magmadar - the Ancient Core Hounds about his room
    // respawn every 18 min until Magmadar dies and walked into three attempts).
    bool WaitsForEarlierBoss(Player* me, Pilot& pilot, Creature* c)
    {
        Creature* boss = BossOf(c);
        if (!boss)
            return false;
        uint32 const earlier = SuiRaidPlaybook::EarlierAliveBoss(me->GetMap(), boss->GetEntry());
        if (!earlier)
            return false;
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32& noted = pilot.gateNotedMs[boss->GetEntry()];
        if (!noted || nowMs - noted > 120000)
        {
            noted = nowMs;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s waits - %s comes first in the playbook order",
                me->GetName(), boss->GetName(), SuiRaidPlaybook::Name(earlier).c_str());
        }
        return true;
    }

    // Spawn spots of bosses that wait for an earlier one (and so does their room).
    std::vector<std::tuple<float, float, float>> WaitingBossSpots(Player* me)
    {
        std::vector<std::tuple<float, float, float>> out;
        std::vector<uint32> const order = SuiRaidPlaybook::Order(me->GetMapId());
        bool earlierAlive = false;
        for (uint32 entry : order)
        {
            bool const dead = SuiRaidPlaybook::BossDead(me->GetMap(), entry);
            float x, y, z;
            if (!dead && earlierAlive && SuiRaidPlaybook::BossSpawn(me->GetMapId(), entry, x, y, z))
                out.emplace_back(x, y, z);
            earlierAlive = earlierAlive || !dead;
        }
        return out;
    }

    // Where the raid is headed: the spawn of the first boss of the playbook order still alive here.
    bool BossGoal(Player* me, float& gx, float& gy, float& gz, uint32* entryOut)
    {
        uint32 const first = SuiRaidPlaybook::FirstAliveBoss(me->GetMap());
        if (!first || !SuiRaidPlaybook::BossSpawn(me->GetMapId(), first, gx, gy, gz))
            return false;
        if (entryOut)
            *entryOut = first;
        return true;
    }

    // Bosses last: before a boss is pulled, every other pack within reach of its room goes
    // (2026-09-24: Gehennas was pulled with a Firelord 23 yd away and two more packs close by -
    // all of them joined). The nearest such pack to the raid is returned, or null.
    Creature* RoomTrash(Player* me, Pilot& pilot, Creature* boss)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(boss, 90.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(boss, searcher, 90.0f);
        Creature* best = nullptr;
        float bestD = 1e9f;
        for (Creature* c : near)
        {
            // A pack only put off for a while (an abandoned ambush) still stands in the room: the boss
            // waits for it (2026-11-03: the hound patrol's ambush was abandoned, Garr was pulled with
            // the hound still walking his room and it joined three of four attempts).
            if (c == boss || c->IsWorldBoss() || !Pullable(me, c, pilot, true) ||
                (c->GetCreatureGroup() && c->GetCreatureGroup() == boss->GetCreatureGroup()))
                continue;
            if (std::fabs(c->GetPositionZ() - boss->GetPositionZ()) > 20.0f)
                continue;
            float const d = me->GetDistance(c);
            if (d < bestD && PathLength(me, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ()) >= 0.0f)
            {
                bestD = d;
                best = c;
            }
        }
        if (best)
            pilot.skip.erase(best->GetObjectGuid());   // it is the next pull again
        return best;
    }

    // The first pullable pack the walk to `target` passes within reach of: a raid walking past
    // an idle pack pulls it (2026-09-24: a Core Hound 30 yd off the route jumped the leader on
    // the way to the Annihilator and the Firelord next to it joined). That pack is cleared first.
    Creature* InTheWay(Player* me, Pilot& pilot, Creature* target)
    {
        PathInfo path(me);
        path.calculate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
        if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            return nullptr;
        PointsArray const& pts = path.getPath();
        float length = 0.0f;
        for (size_t k = 1; k < pts.size(); ++k)
            length += (pts[k] - pts[k - 1]).length();
        float const scan = std::min(length + 30.0f, 250.0f);
        std::list<Creature*> nearby;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, scan);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
        Cell::VisitGridObjects(me, searcher, scan);
        std::vector<Creature*> others;
        // A pack only put off for a while still wakes when the raid walks past it (2026-11-03: a Core
        // Hound pack skipped as "no path" from 513 yd out was walked into on the way to the next one).
        for (Creature* c : nearby)
            if (c != target && Pullable(me, c, pilot, true) && !BossOf(c) &&
                !(c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup()))
                others.push_back(c);
        if (others.empty())
            return nullptr;
        for (size_t k = 1; k < pts.size(); ++k)
        {
            Vector3 const seg = pts[k] - pts[k - 1];
            float const len = seg.length();
            for (float s = 0.0f; s <= len; s += 4.0f)
            {
                Vector3 const q = pts[k - 1] + seg * (len > 0.0f ? s / len : 0.0f);
                // Close to the target already: what stands here is the target's neighbourhood,
                // which the pull plan handles.
                if (target->GetDistance(q.x, q.y, q.z) < 35.0f)
                    return nullptr;
                Creature* blocker = nullptr;
                float bestD = 26.0f;
                for (Creature* c : others)
                {
                    if (std::fabs(c->GetPositionZ() - q.z) > 15.0f)
                        continue;
                    float const d = c->GetDistance(q.x, q.y, q.z);
                    if (d < bestD)
                    {
                        bestD = d;
                        blocker = c;
                    }
                }
                if (blocker && PathLength(me, blocker->GetPositionX(), blocker->GetPositionY(),
                    blocker->GetPositionZ()) >= 0.0f)
                {
                    pilot.skip.erase(blocker->GetObjectGuid());   // it is the next pull again
                    return blocker;
                }
            }
        }
        return nullptr;
    }

    // A boss whose patrol route passes within reach of `c`: fighting `c` means fighting it too,
    // sooner or later, so it goes first.
    Creature* PatrollingBossNear(Player* me, Pilot& pilot, Creature* c)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(c, 320.0f);   // a whole patrol route away (Geddon walks 210 yd)
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(c, searcher, 320.0f);
        for (Creature* b : near)
        {
            if (b == c || !b->IsWorldBoss() || !Pullable(me, b, pilot))
                continue;
            // A boss that wanders about its spot (and blinks) joins every fight in its room: it goes
            // first too (2026-09-25: Shazzrah joined both packs 50 yd from him and wiped the raid).
            if (b->GetDefaultMovementType() == RANDOM_MOTION_TYPE)
            {
                if (b->GetDistance(c) < 70.0f)
                    return b;
                continue;
            }
            if (!Patrols(b))
                continue;
            // Wherever it is on its walk right now, it comes back (2026-09-25: Baron Geddon walked
            // into the raid's camp beside the Firewalker it was waiting to pull).
            if (!b->HasStaticDBSpawnData())
                continue;
            WaypointPath const* path = sWaypointMgr.GetDefaultPath(b->GetEntry(), b->GetDBTableGUIDLow());
            if (!path)
                continue;
            for (auto const& node : *path)
                if (std::fabs(node.second.z - c->GetPositionZ()) < 15.0f && c->GetDistance2d(node.second.x, node.second.y) < 45.0f)
                    return b;
        }
        return nullptr;
    }

    bool AmbushPoint(Player* me, Creature* target, float& qx, float& qy, float& qz, float& clearance);

    // A patrolling boss with no clear spot anywhere on its walk cannot be met first: the packs along
    // the walk go first, each pulled while he is away (2026-11-03: Lucifron walks 130 yd through
    // five Core Hound packs, his clearest spot 23 yd from one; met first, the raid waited beside two
    // packs, pulled both and he arrived at +82 s).
    float LureClearance(Player* me, Creature* boss, float lx, float ly, float lz, Creature** nearestOut = nullptr,
        float* nxOut = nullptr, float* nyOut = nullptr);

    bool TimeAroundWalker(Player* me, Pilot& pilot, Creature* walker, Creature* found)
    {
        float qx, qy, qz, clear = -1.0f;
        // 22 yd: creatures call for help 10 yd around them, and the ambush camp is planned well clear
        // of the rest (with the first hound pack down, Lucifron's clearest spot was 25 yd from the next).
        {
            // the playbook lures it: the boss goes first, met beside its walk (Baron Geddon runs his loop)
            float lx, ly, lz, cx, cy, cz;
            bool hasCamp = false;
            if (SuiRaidPlaybook::Lure(walker->GetEntry(), lx, ly, lz, hasCamp, cx, cy, cz))
            {
                float const clear = LureClearance(me, walker, lx, ly, lz);
                if (clear >= SuiRaidPlaybook::LureClearYards(walker->GetEntry()))
                    return false;
                pilot.timedBy = walker->GetObjectGuid();
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s's lure is %.0f yd from a pack - %s (entry %u) goes first, pulled while he is away",
                    me->GetName(), walker->GetName(), clear, found->GetName(), found->GetEntry());
                return true;
            }
        }
        auto nf = pilot.notFirstUntil.find(walker->GetObjectGuid());
        bool const crowdedBefore = nf != pilot.notFirstUntil.end() && WorldTimer::getMSTime() < nf->second;
        if (!crowdedBefore && AmbushPoint(me, walker, qx, qy, qz, clear) && clear >= 22.0f)
            return false;
        pilot.timedBy = walker->GetObjectGuid();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s has no clear spot on its walk (best %.0f yd from other packs) - %s (entry %u) goes first, pulled while he is away",
            me->GetName(), walker->GetName(), clear, found->GetName(), found->GetEntry());
        return true;
    }

    // A patrolling boss the playbook lures, with the lure clear of every idle pack: he is met there
    // first and his room's packs wait (2026-11-07: RoomTrash sent the raid at the packs on Baron
    // Geddon's 30 s loop - he ran into six such pulls; the clear lure was never used).
    bool LuresFirst(Player* me, Creature* boss)
    {
        float lx, ly, lz, cx, cy, cz;
        bool hasCamp = false;
        if (!Patrols(boss) || !SuiRaidPlaybook::Lure(boss->GetEntry(), lx, ly, lz, hasCamp, cx, cy, cz))
            return false;
        Creature* who = nullptr;
        float nx = 0.0f, ny = 0.0f;
        float const clear = LureClearance(me, boss, lx, ly, lz, &who, &nx, &ny);
        if (clear >= SuiRaidPlaybook::LureClearYards(boss->GetEntry()))
            return true;
        if (who)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s's lure is %.0f yd from %s (entry %u, spawn %u, motion %u) at (%.0f, %.0f), it stands at (%.0f, %.0f)",
                me->GetName(), boss->GetName(), clear, who->GetName(), who->GetEntry(), who->GetDBTableGUIDLow(),
                uint32(who->GetDefaultMovementType()), nx, ny, who->GetPositionX(), who->GetPositionY());
        return false;
    }

    // A lured boss is met at the lure, not where he runs right now: the walk there is what must exist
    // (2026-11-07: Baron Geddon's lure was clear, but no path to his running position at that moment
    // let the Flameguard pack beside his walk go first - he joined it, 39 dead).
    bool LurePathOk(Player* me, Creature* boss)
    {
        float lx, ly, lz, cx, cy, cz;
        bool hasCamp = false;
        if (!SuiRaidPlaybook::Lure(boss->GetEntry(), lx, ly, lz, hasCamp, cx, cy, cz))
            return false;
        me->UpdateAllowedPositionZ(lx, ly, lz);
        return PathLength(me, lx, ly, lz) >= 0.0f;
    }

    Creature* PickNext(Player* me, Pilot& pilot)
    {
        pilot.offMesh = false;
        pilot.timedBy.Clear();
        for (float radius : kSearchRings)
            if (pilot.offMesh)
                return nullptr;
            else if (Creature* found = PickNextWithin(me, pilot, radius))
            {
                Creature* walker = nullptr;
                if (true)   // room trash too: a patrolling boss comes back to it (Geddon, 2026-09-25)
                    if ((walker = PatrollingBossNear(me, pilot, found)) && WaitsForEarlierBoss(me, pilot, walker))
                    {
                        // its boss is not ours yet, and it walks past this pack: the pack waits too
                        pilot.skip[found->GetObjectGuid()] = WorldTimer::getMSTime() + 3 * 60 * 1000;
                        continue;
                    }
                    else if (walker && !TimeAroundWalker(me, pilot, walker, found))
                        if (PathLength(me, walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ()) >= 0.0f ||
                            LurePathOk(me, walker))
                        {
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s patrols past %s (entry %u) - the boss goes first",
                                me->GetName(), walker->GetName(), found->GetName(), found->GetEntry());
                            found = walker;
                            // The walk to it is still cleared of packs well away from it (2026-09-24:
                            // the raid walked 580 yd to Geddon and met two packs on the way).
                            for (int hop = 0; hop < 4; ++hop)
                            {
                                // ...but never a pack the boss walks past itself: he joins it whenever it
                                // is fought (2026-11-03: a Core Hound pack went first, Lucifron arrived at +41 s).
                                Creature* first = InTheWay(me, pilot, found);
                                if (!first || first->GetDistance(walker) < 60.0f || PatrollingBossNear(me, pilot, first) == walker)
                                    break;
                                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s (entry %u) stands in the way of %s - it goes first",
                                    me->GetName(), first->GetName(), first->GetEntry(), found->GetName());
                                found = first;
                            }
                            return found;
                        }
                if (Creature* boss = BossOf(found))
                    if (LuresFirst(me, boss))
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s's lure is clear - he is met there before his room's packs",
                            me->GetName(), boss->GetName());
                        found = boss;
                    }
                    else if (Creature* trash = RoomTrash(me, pilot, boss))
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s (entry %u) is cleared before %s",
                            me->GetName(), trash->GetName(), trash->GetEntry(), boss->GetName());
                        found = trash;
                        if (Patrols(boss))
                            pilot.timedBy = boss->GetObjectGuid();   // his room's packs are pulled while he walks away
                    }
                for (int hop = 0; hop < 4; ++hop)
                {
                    Creature* first = InTheWay(me, pilot, found);
                    if (!first)
                        break;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s (entry %u) stands in the way of %s - it goes first",
                        me->GetName(), first->GetName(), first->GetEntry(), found->GetName());
                    found = first;
                }
                return found;
            }
        return nullptr;
    }

    void Note(Pilot& pilot, Player* me, char const* fmt, ...);
    void Regroup(Player* leader);

    // The next scripted, locked button object of this instance still standing (an encounter's
    // rune): false when there is none.
    bool NextScriptedObject(Player* me, Pilot& pilot, uint32& guidOut, uint32& entryOut, float& x, float& y, float& z)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        std::vector<std::tuple<float, float, float>> aliveBosses;
        for (uint32 entry : SuiRaidPlaybook::Order(me->GetMapId()))
        {
            float bx, by, bz;
            if (!SuiRaidPlaybook::BossDead(me->GetMap(), entry) && SuiRaidPlaybook::BossSpawn(me->GetMapId(), entry, bx, by, bz))
                aliveBosses.emplace_back(bx, by, bz);
        }
        float best = 1e9f;
        auto worker = [&](GameObjectDataPair const& pair) -> bool
        {
            GameObjectData const& data = pair.second;
            if (data.position.mapId != me->GetMapId())
                return false;
            GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(data.id);
            if (!info || info->type != GAMEOBJECT_TYPE_BUTTON || !info->ScriptId || !info->button.lockId)
                return false;
            auto used = pilot.usedObjects.find(pair.first);
            if (used != pilot.usedObjects.end() && nowMs < used->second)
                return false;
            // An encounter's rune waits for its boss: it is doused only once he is dead, and walking to
            // it pulls him (2026-11-03: "nothing left to fight - going to use" the Rune of Mohn, Garr
            // pulled with the raid strung out behind the leader).
            for (auto const& boss : aliveBosses)
                if ((std::get<0>(boss) - data.position.x) * (std::get<0>(boss) - data.position.x) +
                    (std::get<1>(boss) - data.position.y) * (std::get<1>(boss) - data.position.y) < 90.0f * 90.0f)
                    return false;
            float const d = me->GetDistance(data.position.x, data.position.y, data.position.z);
            if (d < best)
            {
                best = d;
                guidOut = pair.first;
                entryOut = data.id;
                x = data.position.x;
                y = data.position.y;
                z = data.position.z;
            }
            return false;
        };
        sObjectMgr.DoGOData(worker);
        return best < 1e8f;
    }

    bool UseScriptedObjects(Player* me, Pilot& pilot)
    {
        if (!me->GetMap()->IsDungeon() || me->IsInCombat())
            return false;
        uint32 guid = 0, entry = 0;
        float x = 0, y = 0, z = 0;
        if (!NextScriptedObject(me, pilot, guid, entry, x, y, z))
            return false;
        if (pilot.useStep == 0 || me->GetDistance(x, y, z) > 8.0f)
        {
            Note(pilot, me, "nothing left to fight - going to use object %u (spawn %u)",
                entry, guid);
            me->NearTeleportTo(x + 2.0f, y, z + 0.5f, me->GetOrientation());   // setup move
            pilot.useStep = 1;
            return true;
        }
        pilot.useStep = 0;
        pilot.usedObjects[guid] = WorldTimer::getMSTime() + 10 * 60 * 1000;
        GameObject* go = me->FindNearestGameObject(entry, 10.0f);
        if (!go)
        {
            Note(pilot, me, "object %u is not here - skipped", entry);
            return true;
        }
        Note(pilot, me, "using %s", go->GetName());
        go->Use(me);
        Regroup(me);   // the raid comes along (setup move)
        return true;
    }

    // Only the grids around the raid are loaded: a pack 600 yd away does not exist in memory until
    // someone walks near it (2026-09-24: "nothing left to pull" at the entrance with Geddon, Shazzrah,
    // Golemagg and Sulfuron alive). The spawn table says where the rest of the instance stands: the
    // nearest live, uncleared, hostile spawn that is not deferred is where the raid walks next.
    bool ExploreTarget(Player* me, Pilot& pilot, float& x, float& y, float& z, uint32& entryOut, uint32& guidOut)
    {
        MapPersistentState* state = me->GetMap()->GetPersistentState();
        FactionTemplateEntry const* mine = me->GetFactionTemplateEntry();
        if (!mine)
            return false;
        time_t const now = time(nullptr);
        uint32 const nowMs = WorldTimer::getMSTime();
        LoadCleared();
        std::vector<std::tuple<float, float, float>> const waitingSpots = WaitingBossSpots(me);
        float gx = 0, gy = 0, gz = 0;
        uint32 goalEntry = 0;
        bool const goal = BossGoal(me, gx, gy, gz, &goalEntry);
        float const meToGoal = goal ? me->GetDistance(gx, gy, gz) : 0.0f;
        float best = 1e9f;
        for (auto const& pair : sObjectMgr.GetCreatureDataMap())
        {
            CreatureData const& data = pair.second;
            if (data.position.mapId != me->GetMapId())
                continue;
            uint32 const entry = data.creature_id[0];
            CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);
            if (!info || info->npc_flags || info->type == CREATURE_TYPE_CRITTER)
                continue;
            FactionTemplateEntry const* theirs = sObjectMgr.GetFactionTemplateEntry(info->faction);
            if (!theirs || !theirs->IsHostileTo(*mine))
                continue;
            if (state && state->GetCreatureRespawnTime(pair.first) > now)
                continue;   // dead in this instance
            if (gCleared.count(std::make_tuple(me->GetMapId(), me->GetInstanceId(), pair.first)))
                continue;
            if (pilot.exploreSkip.count(pair.first) && nowMs < pilot.exploreSkip.at(pair.first))
                continue;
            if (pilot.deferEntry.count(entry) && nowMs < pilot.deferEntry.at(entry))
                continue;
            float const px = data.position.x, py = data.position.y, pz = data.position.z;
            bool deferred = false;
            for (auto const& zone : pilot.deferZones)
                if (nowMs < zone.until && std::fabs(pz - zone.z) < 25.0f &&
                    (px - zone.x) * (px - zone.x) + (py - zone.y) * (py - zone.y) < zone.r * zone.r)
                    deferred = true;
            if (deferred)
                continue;
            float const d = me->GetDistance(px, py, pz);
            if (d < 90.0f || d >= best)
                continue;   // close ones are loaded already (and were not pullable)
            bool waiting = false;   // the room of a boss that is not ours yet
            for (auto const& spot : waitingSpots)
                if (std::fabs(pz - std::get<2>(spot)) < 25.0f &&
                    (px - std::get<0>(spot)) * (px - std::get<0>(spot)) + (py - std::get<1>(spot)) * (py - std::get<1>(spot)) < 60.0f * 60.0f)
                    waiting = true;
            if (waiting)
                continue;
            if (goal && std::sqrt((px - gx) * (px - gx) + (py - gy) * (py - gy)) > meToGoal - 15.0f)
                continue;   // not on the way to the next boss
            if (PathLength(me, px, py, pz) < 0.0f)
                continue;
            best = d;
            x = px;
            y = py;
            z = pz;
            entryOut = entry;
            guidOut = pair.first;
        }
        if (best > 1e8f && goal && meToGoal > 30.0f && PathLength(me, gx, gy, gz) >= 0.0f)
        {
            // Nothing left between here and the next boss: walk to the boss.
            x = gx;
            y = gy;
            z = gz;
            entryOut = goalEntry;
            guidOut = 0;
            return true;
        }
        return best < 1e8f;
    }

    void Note(Pilot& pilot, Player* me, char const* fmt, ...)
    {
        char buffer[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        pilot.lastEvent = buffer;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s  @(%.0f, %.0f)", me->GetName(), buffer,
            me->GetPositionX(), me->GetPositionY());
    }
}

bool SuiAutopilot::Enable(Player* leader, std::string& reply)
{
    if (!leader || !leader->IsInWorld())
    {
        reply = "leader not found in world";
        return false;
    }
    if (!dynamic_cast<AiBotAI*>(leader->AI()))
    {
        reply = "leader must be an AiBot (log it in with .bot add first)";
        return false;
    }
    if (!leader->GetGroup())
    {
        reply = "leader is not in a group";
        return false;
    }
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
        if (Player* other = Find(pair.first))
            if (other != leader && other->GetGroup() == leader->GetGroup())
            {
                reply = std::string("group already led by ") + other->GetName();
                return false;
            }
    Pilot& pilot = gPilots[leader->GetObjectGuid()];
    pilot = Pilot();
    pilot.leader = leader->GetObjectGuid();
    {
        std::ifstream in(DeferredPath());
        uint32 map = 0;
        float x = 0, y = 0, z = 0, r = 0;
        uint64 untilUnix = 0;
        uint64 const nowUnix = uint64(time(nullptr));
        while (in >> map >> x >> y >> z >> r >> untilUnix)
            if (map == leader->GetMapId() && untilUnix > nowUnix)
                pilot.deferZones.push_back({x, y, z, r, WorldTimer::getMSTime() + uint32(untilUnix - nowUnix) * 1000});
        std::ifstream entries(DeferredPath() + ".entries");
        uint32 entry = 0;
        while (entries >> entry >> untilUnix)
            if (untilUnix > nowUnix)
                pilot.deferEntry[entry] = WorldTimer::getMSTime() + uint32(untilUnix - nowUnix) * 1000;
    }
    reply = std::string("autopilot on, leader ") + leader->GetName();
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: enabled", leader->GetName());
    return true;
}

void SuiAutopilot::Disable(ObjectGuid leader)
{
    std::lock_guard<std::recursive_mutex> guard(gLock);
    gPilots.erase(leader);
}

void SuiAutopilot::DisableAll()
{
    std::lock_guard<std::recursive_mutex> guard(gLock);
    gPilots.clear();
    SuiRaidTelemetry::Track(nullptr);
}

void SuiAutopilot::SetPaused(ObjectGuid leader, bool paused)
{
    std::lock_guard<std::recursive_mutex> guard(gLock);
    auto it = gPilots.find(leader);
    if (it != gPilots.end())
    {
        it->second.paused = paused;
        if (!paused)
            it->second.wipesByEntry.clear();
    }
}

void SuiAutopilot::SetReview(ObjectGuid leader, bool on)
{
    std::lock_guard<std::recursive_mutex> guard(gLock);
    auto it = gPilots.find(leader);
    if (it != gPilots.end())
        it->second.review = on;
}

bool SuiAutopilot::IsLeader(Player const* player)
{
    if (!player)
        return false;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    return gPilots.count(player->GetObjectGuid()) != 0;
}

Player* SuiAutopilot::LeaderFor(Player const* member)
{
    if (!member || !member->GetGroup())
        return nullptr;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (leader && leader != member && leader->IsInWorld() &&
            leader->GetGroup() == member->GetGroup())
            return leader;
    }
    return nullptr;
}

void SuiAutopilot::NoteLeaderDeath(Player const* leader)
{
    if (!leader)
        return;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    auto it = gPilots.find(leader->GetObjectGuid());
    if (it == gPilots.end())
        return;
    Pilot& pilot = it->second;
    uint32 const wipes = ++pilot.wipesByEntry[pilot.targetEntry];
    char line[256];
    snprintf(line, sizeof(line), "leader died on entry %u (%u time(s))", pilot.targetEntry, wipes);
    pilot.lastEvent = line;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s", leader->GetName(), line);
    if (wipes >= kWipesBeforePause)
    {
        // Not beaten yet: the rest of the instance first, this pack (and the boss it belongs to)
        // again in 30 minutes, with whatever the other fights taught the raid.
        uint32 const until = WorldTimer::getMSTime() + 120 * 60 * 1000;
        pilot.deferEntry[pilot.targetEntry] = until;
        {
            std::ofstream entries(DeferredPath() + ".entries", std::ios::app);
            entries << pilot.targetEntry << ' ' << uint64(time(nullptr)) + 120 * 60 << '\n';
        }
        if (Creature* target = leader->GetMap()->GetCreature(pilot.target))
        {
            if (Creature* boss = BossOf(target))
                pilot.deferEntry[boss->GetEntry()] = until;
            // The whole area waits, not only that entry: its neighbours are what killed us too
            // (2026-09-24: Lucifron deferred, the next pick was his room's Core Hounds again).
            pilot.deferZones.push_back({target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), 80.0f, until});
        }
        pilot.deferZones.push_back({leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), 60.0f, until});
        // Kept across a server restart (wall clock).
        std::ofstream out(DeferredPath(), std::ios::app);
        uint64 const untilUnix = uint64(time(nullptr)) + 120 * 60;
        for (size_t i = pilot.deferZones.size() >= 2 ? pilot.deferZones.size() - 2 : 0; i < pilot.deferZones.size(); ++i)
            out << leader->GetMapId() << ' ' << pilot.deferZones[i].x << ' ' << pilot.deferZones[i].y << ' '
                << pilot.deferZones[i].z << ' ' << pilot.deferZones[i].r << ' ' << untilUnix << '\n';
        pilot.wipesByEntry[pilot.targetEntry] = 0;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AUTOPILOT] %s: %u deaths on entry %u - deferred 30 min, clearing elsewhere first",
            leader->GetName(), wipes, pilot.targetEntry);
    }
    // The fight goes on without the leader; its record closes when the raid is out of combat.
    if (pilot.phase != Phase::Fight)
    {
        pilot.phase = Phase::Fight;
        pilot.fightMs = 0;
        ++pilot.fights;
    }
    pilot.leaderDied = true;
    pilot.restMs = 0;   // a wipe starts a full rest: everyone revives, eats and rebuffs
    pilot.target.Clear();
}

namespace
{
    // Raid preparation between pulls - the owner's standing rule (2026-09-10): GM setup
    // restores buffs and role consumables, combat runs on normal rules. Class buffs come only
    // from classes the raid actually brings (a paladin per Greater Blessing), consumables by
    // role. Applied as the consumed item or cast would, and only when missing.
    void Prepare(Player* leader)
    {
        Group* group = leader->GetGroup();
        if (!group)
            return;
        uint32 priests = 0, mages = 0, druids = 0, paladins = 0;
        std::vector<Player*> members;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->getSource();
            if (!m || !m->IsInWorld() || !m->IsAlive() || m->GetMap() != leader->GetMap())
                continue;
            members.push_back(m);
            priests += m->GetClass() == CLASS_PRIEST;
            mages += m->GetClass() == CLASS_MAGE;
            druids += m->GetClass() == CLASS_DRUID;
            paladins += m->GetClass() == CLASS_PALADIN;
        }
        uint32 applied = 0;
        auto give = [&applied](Player* m, uint32 spell)
        {
            if (!m->HasAura(spell))
            {
                m->CastSpell(m, spell, true);
                ++applied;
            }
        };
        uint32 repaired = 0;
        // The quartermaster answers to the group's leader (whoever holds the lead right now).
        Player* quartermaster = sObjectMgr.GetPlayer(group->GetLeaderGuid());
        if (!quartermaster || !quartermaster->IsInWorld())
            quartermaster = leader;
        struct FlushOnExit { Player* l; ~FlushOnExit() { SuiRaidSupply::Flush(l); } } flush{quartermaster};
        for (Player* m : members)
        {
            // Repairs are part of the setup between pulls (2026-09-24: after a day of wipes every
            // weapon was at durability 0 - a broken item counts as unequipped, so rogues and
            // warriors could not use a single ability and fought bare-handed).
            if (m->DurabilityRepairAll(false, 0.0f))
                ++repaired;
            // The quartermaster's owner-editable loadout (ammunition for the leader's ranged pull
            // included): what a raid brings in its bags, topped up between pulls.
            AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
            CombatBotRoles const role = ai ? ai->GetCombatActiveRole() : ROLE_MELEE_DPS;
            // The member's role picks its loadout bucket (1 tank, 3 healer, 4 melee; 0 = by its weapon):
            // with 0 for everyone the healers took the casters' kit and never got their Demonic Runes
            // (2026-11-08, Majordomo: nine healers dry by 14-50 s, runes listed for them, none carried).
            if (!m->IsInCombat())
                SuiRaidSupply::Supply(quartermaster, m, role == ROLE_TANK ? 1u : role == ROLE_HEALER ? 3u :
                    role == ROLE_MELEE_DPS ? 4u : 0u);
            bool const mana = m->GetPowerType() == POWER_MANA;
            bool const tank = role == ROLE_TANK;
            bool const healer = role == ROLE_HEALER;
            bool const physical = !mana || m->GetClass() == CLASS_HUNTER || role == ROLE_MELEE_DPS;

            if (priests) give(m, 21564);                 // Prayer of Fortitude
            if (mages && mana) give(m, 23028);           // Arcane Brilliance
            if (druids) give(m, 21850);                  // Gift of the Wild
            if (paladins >= 1) give(m, 25898);           // Greater Blessing of Kings
            if (paladins >= 2 && !tank) give(m, 25895);  // Greater Blessing of Salvation
            if (paladins >= 3 && physical) give(m, 25916);   // Greater Blessing of Might
            if (paladins >= 4 && mana) give(m, 25918);       // Greater Blessing of Wisdom

            give(m, 17543);                              // Greater Fire Protection
            if (tank)
            {
                give(m, 17626);                          // Flask of the Titans
                give(m, 3593);                           // Elixir of Fortitude
                give(m, 11348);                          // Elixir of Superior Defense
                give(m, 25661);                          // Dirge's Kickin' Chimaerok Chops
            }
            else if (healer)
            {
                give(m, 17627);                          // Flask of Distilled Wisdom
                give(m, 24363);                          // Mageblood Potion
                give(m, 18194);                          // Nightfin Soup
            }
            else if (physical)
            {
                give(m, 17538);                          // Elixir of the Mongoose
                give(m, 11405);                          // Elixir of the Giants
                give(m, m->GetClass() == CLASS_WARRIOR ? 24799u : 18192u);   // food
            }
            else
            {
                give(m, 17628);                          // Flask of Supreme Power
                give(m, 17539);                          // Greater Arcane Elixir
                give(m, 18194);                          // Nightfin Soup
            }
        }
        if (applied || repaired)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: raid prep applied %u buff(s)/consumable(s), repaired %u member(s)",
                leader->GetName(), applied, repaired);
    }

    // Between pulls the raid regroups on the leader (owner 2026-09-23: revive and summon between
    // pulls is allowed setup). A member revived at the instance entrance mid-clear used to walk
    // into whatever still stood there, alone, and die again.
    // Lava or slime underfoot (2026-09-25: a Firewalker's camp was planned in lava and the raid
    // was summoned into it).
    bool Molten(Player* me, float x, float y, float z)
    {
        return (me->GetTerrain()->getLiquidStatus(x, y, z + 0.01f, MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME, nullptr) &
            (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK)) != 0;
    }

    void Regroup(Player* leader)
    {
        Group* group = leader->GetGroup();
        if (!group)
            return;
        uint32 revived = 0, summoned = 0;
        if (Molten(leader, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
            return;   // never onto a leader standing in lava
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* m = itr->getSource();
            if (!m || m == leader || !m->IsInWorld() || m->IsInCombat() || m->IsBeingTeleported())
                continue;
            float x = leader->GetPositionX(), y = leader->GetPositionY(), z = leader->GetPositionZ();
            if (m->GetMap() != leader->GetMap())
            {
                // Left outside (revived at the portal after a restart): brought back in.
                if (!m->IsAlive())
                {
                    m->ResurrectPlayer(1.0f);
                    m->SpawnCorpseBones();
                    ++revived;
                }
                m->TeleportTo(leader->GetMapId(), x, y, z, leader->GetOrientation());
                ++summoned;
                continue;
            }
            if (!m->IsAlive())
            {
                m->ResurrectPlayer(1.0f);
                m->SpawnCorpseBones();
                m->NearTeleportTo(x, y, z, leader->GetOrientation());
                ++revived;
            }
            else if (!m->IsWithinDistInMap(leader, kGatherYards))
            {
                m->NearTeleportTo(x, y, z, leader->GetOrientation());
                ++summoned;
            }
        }
        if (revived || summoned)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: regroup revived %u, summoned %u",
                leader->GetName(), revived, summoned);
    }

    struct Spot { float x, y, z; Creature* who = nullptr; };


    // The ground a patrolling creature walks (its waypoint route, sampled every 5 yd): a fight
    // anywhere near it is visited sooner or later (2026-09-24: Lucifron walks a 130 yd route
    // through the Core Hound packs and joined every hound fight ~25 s in).
    template <typename F>
    void ForEachRoutePoint(Creature* c, F&& visit)
    {
        if (c->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE || !c->HasStaticDBSpawnData())
            return;
        WaypointPath const* path = sWaypointMgr.GetDefaultPath(c->GetEntry(), c->GetDBTableGUIDLow());
        if (!path || path->empty())
            return;
        WaypointNode const* prev = nullptr;
        for (auto const& node : *path)
        {
            WaypointNode const& n = node.second;
            if (prev)
            {
                float const dx = n.x - prev->x, dy = n.y - prev->y;
                float const len = std::sqrt(dx * dx + dy * dy);
                for (float s = 5.0f; s < len; s += 5.0f)
                    visit(prev->x + dx * s / len, prev->y + dy * s / len, prev->z + (n.z - prev->z) * s / len);
            }
            visit(n.x, n.y, n.z);
            prev = &n;
        }
    }

    // How far a spot is from a patrol's walk (its route sampled every 5 yd, same floor).
    float RouteGap(Creature* w, float x, float y, float z)
    {
        float best = 1e9f;
        ForEachRoutePoint(w, [&](float rx, float ry, float rz)
        {
            if (std::fabs(rz - z) < 20.0f)
                best = std::min(best, std::sqrt((rx - x) * (rx - x) + (ry - y) * (ry - y)));
        });
        return best;
    }

    // The raid waits and fights this far from an idle boss's patrol walk: his reach plus the raid's
    // own spread (2026-11-04: after a timed pack fight the raid stood 25 yd off Baron Geddon's walk;
    // he came by, the pack beside his walk joined, wipe).
    float const kBossWalkGap = 50.0f;   // Geddon runs his walk (always-run, ~30 s a loop): no timing keeps him away

    // An idle patrolling boss whose walk passes within r of the spot.
    Creature* PatrolBossWalkNear(Player* me, float x, float y, float z, float r)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, 320.0f);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(me, searcher, 320.0f);
        for (Creature* b : near)
            if (b->IsAlive() && !b->IsInCombat() && b->IsWorldBoss() && b->IsHostileTo(me) && Patrols(b) &&
                RouteGap(b, x, y, z) < r)
                return b;
        return nullptr;
    }

    // Idle hostiles that are not the target's own pack, around the target: what a camp, a pull
    // spot or the raid's walk there must keep clear of.
    void ForeignIdle(Player* me, Creature* target, float r, std::vector<Spot>& out)
    {
        // A long patrol counts by its route, wherever it is now: 300 yd out it still walks through
        // the camp (2026-11-05: an Ancient Core Hound 285 yd from its spawn joined a Firelord fight at
        // +18 s; its walk passed 18 yd from the camp, but the hound was outside the 110 yd look).
        float const look = std::max(r, 320.0f);
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(target, look);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(target, searcher, look);
        float const tx = target->GetPositionX(), ty = target->GetPositionY();
        for (Creature* c : near)
        {
            if (c == target || !c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me))
                continue;
            if (c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup())
                continue;
            bool const inside = target->IsWithinDist(c, r, false);
            if (inside)
                out.push_back({c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), c});
            ForEachRoutePoint(c, [&](float x, float y, float z)
            {
                if (inside || (x - tx) * (x - tx) + (y - ty) * (y - ty) <= r * r)
                    out.push_back({x, y, z, c});
            });
        }
    }

    // How far c notices me from where it stands, measured between the bodies' centres (aggro radius
    // by level difference, plus both bounding radii) - the pull spot stays outside it.
    float AggroReach(Player* me, Creature* c)
    {
        return c->GetAttackDistance(me) + c->GetObjectBoundingRadius() + me->GetObjectBoundingRadius();
    }

    // How clear of idle packs (other than the boss's own) the playbook's lure spot is: below 45 yd the
    // fight there drags them in (2026-11-04: Baron Geddon met 38 yd from a Firewalker pack - it joined at
    // +4 s); the packs near it go first then, timed around him.
    float LureClearance(Player* me, Creature* boss, float lx, float ly, float lz, Creature** nearestOut,
        float* nxOut, float* nyOut)
    {
        std::vector<Spot> near;
        ForeignIdle(me, boss, boss->GetDistance(lx, ly, lz) + 80.0f, near);
        float best = 1e9f;
        for (Spot const& sp : near)
            if (sp.who && !(sp.who->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE &&
                            sp.x != sp.who->GetPositionX()))   // the creatures, not other walkers' routes
            {
                float const d = std::sqrt((sp.x - lx) * (sp.x - lx) + (sp.y - ly) * (sp.y - ly));
                if (d < best)
                {
                    best = d;
                    if (nearestOut)
                        *nearestOut = sp.who;
                    if (nxOut)
                        *nxOut = sp.x;
                    if (nyOut)
                        *nyOut = sp.y;
                }
            }
        return best;
    }

    float Nearest(std::vector<Spot> const& spots, float x, float y, float z)
    {
        float best = 1e9f;
        for (Spot const& s : spots)
        {
            if (std::fabs(s.z - z) > 25.0f)
                continue;   // another floor
            best = std::min(best, std::sqrt((s.x - x) * (s.x - x) + (s.y - y) * (s.y - y)));
        }
        return best;
    }

    bool Ground(Player* me, float x, float y, float& z)
    {
        float const before = z;
        me->UpdateAllowedPositionZ(x, y, z);
        return std::fabs(z - before) < 12.0f;
    }

    // The side of the pack to pull it from. Every direction around it is tried: a pull spot 28
    // yd out (a ranged pull from beyond its aggro reach, with sight of it) and a camp 50 yd out
    // behind it, both scored by their distance to every OTHER idle pack and by how close the
    // raid's walk to the camp passes other packs and the target. Ordinary raid-leading: pull
    // the edge pack toward open ground, never into the next one.
    bool PlanPull(Player* me, Creature* target, Pilot& pilot, Vector3 const* at = nullptr)
    {
        std::vector<Spot> foreign;
        if (at)
        {
            // Around the ambush point: what stands near it, and the target's own walk away from
            // it (the leader and the camp stay off the route it comes along).
            std::vector<Spot> wide;
            ForeignIdle(me, target, target->GetDistance(at->x, at->y, at->z) + 110.0f, wide);
            for (Spot const& s : wide)
                if ((s.x - at->x) * (s.x - at->x) + (s.y - at->y) * (s.y - at->y) <= 110.0f * 110.0f)
                    foreign.push_back(s);
            ForEachRoutePoint(target, [&](float x, float y, float z)
            {
                if ((x - at->x) * (x - at->x) + (y - at->y) * (y - at->y) > 25.0f * 25.0f)
                    foreign.push_back({x, y, z, nullptr});
            });
        }
        else
            ForeignIdle(me, target, 110.0f, foreign);
        // A pull timed around a patrolling boss: the stretch of his walk beside this pack is empty when
        // the pull goes (the timing sees to it); the rest of his walk still keeps the camp away.
        if (!pilot.timedBy.IsEmpty())
            foreign.erase(std::remove_if(foreign.begin(), foreign.end(), [&](Spot const& s)
            {
                return s.who && s.who->GetObjectGuid() == pilot.timedBy &&
                    (s.x - target->GetPositionX()) * (s.x - target->GetPositionX()) +
                    (s.y - target->GetPositionY()) * (s.y - target->GetPositionY()) < 25.0f * 25.0f;
            }), foreign.end());
        {
            // The leader's side too: the raid's walk to the camp starts here.
            std::list<Creature*> near;
            MaNGOS::AnyUnitInObjectRangeCheck check(me, 110.0f);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
            Cell::VisitGridObjects(me, searcher, 110.0f);
            for (Creature* c : near)
            {
                if (c == target || !c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                    c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me) ||
                    (at ? c->GetDistance(at->x, at->y, at->z) < 110.0f : target->IsWithinDist(c, 110.0f, false)))
                    continue;   // already counted from the target's side
                if (c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup())
                    continue;
                foreign.push_back({c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), c});
                ForEachRoutePoint(c, [&](float x, float y, float z) { foreign.push_back({x, y, z, c}); });
            }
        }
        float const tx = at ? at->x : target->GetPositionX(), ty = at ? at->y : target->GetPositionY(),
            tz = at ? at->z : target->GetPositionZ();
        // A pull timed around a patrolling boss camps off his walk: the fight is still on when he
        // comes back by (2026-11-04: camps 25 yd from Baron Geddon's walk, he joined twice).
        Creature* walker = (!at && !pilot.timedBy.IsEmpty()) ? me->GetMap()->GetCreature(pilot.timedBy) : nullptr;
        if (walker && (walker == target || !walker->IsAlive()))
            walker = nullptr;
        // Any pack beside such a walk is timed around him the same way, however it was picked (2026-11-04:
        // a pack reached by a "passing" hop got a camp 26 yd off Geddon's walk; he walked into it).
        if (!at && !walker)
            if (Creature* boss = PatrolBossWalkNear(me, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), 45.0f))
                if (boss != target && !SamePack(boss, target))
                {
                    walker = boss;
                    pilot.timedBy = boss->GetObjectGuid();
                    pilot.timedWaitMs = 0;
                    pilot.timedClearTicks = 0;
                    foreign.erase(std::remove_if(foreign.begin(), foreign.end(), [&](Spot const& sp)
                    {
                        return sp.who == boss && (sp.x - tx) * (sp.x - tx) + (sp.y - ty) * (sp.y - ty) < 25.0f * 25.0f;
                    }), foreign.end());
                }
        float bestScore = 1e9f, bestDp = 0, bestDc = 0, bestDr = 0;
        Spot bestP{}, bestC{};
        // The target's own pack: the pull spot stays out of every member's reach, not only the
        // target's (2026-09-25: a pack member 10 yd nearer woke on the leader standing at the spot
        // and the leader died running the pack back to the camp).
        std::vector<Spot> pack;
        if (!at)
        {
            pack.push_back({target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), target});
            if (CreatureGroup* g = target->GetCreatureGroup())
                for (auto const& m : g->GetMembers())
                    if (Creature* c = me->GetMap()->GetCreature(m.first))
                        if (c != target && c->IsAlive())
                            pack.push_back({c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), c});
        }
        // Why directions were refused (logged when no plan comes out), and a second, looser pass
        // before giving up: an unplanned pull camps where the leader happens to stand (2026-11-03:
        // 25 yd from a Firelord pair, 35 yd from the next pair - both joined, wipe).
        uint32 refused[11] = {};
        bool relaxed = false;
        for (int pass = 0; pass < 2 && bestScore > 1e8f; ++pass)
        for (int i = 0; i < 24; ++i)
        {
            relaxed = pass == 1;
            float const zTol = relaxed ? 8.0f : 4.0f, backMax = relaxed ? 70.0f : 45.0f, pastMin = relaxed ? 20.0f : 24.0f;
            float const campR = relaxed ? 58.0f : 50.0f;
            float const a = i * float(M_PI) / 12.0f, cx = std::cos(a), cy = std::sin(a);
            Spot P{tx + 28.0f * cx, ty + 28.0f * cy, tz}, C{tx + campR * cx, ty + campR * cy, tz};
            if (!Ground(me, P.x, P.y, P.z) || !Ground(me, C.x, C.y, C.z) ||
                Molten(me, P.x, P.y, P.z) || Molten(me, C.x, C.y, C.z))
            {
                ++refused[0];
                continue;
            }
            if (!pack.empty() && Nearest(pack, P.x, P.y, P.z) < 26.0f)
            {
                ++refused[1];
                continue;
            }
            // ...and out of every other idle creature's own reach: a big one notices from farther
            // than 26 yd (2026-11-05: a Lava Annihilator 27 yd from the pull spot woke on the leader
            // and joined the Firelord pull - wipe). A walker is met at its ambush point instead, planned
            // from there (2026-11-05: around Lucifron's spot 40 of 48 spots fell inside a hound's reach,
            // the fallback camp stood 6 yd from a pack - all dead).
            if (!at && !Patrols(target))
            {
                bool seen = false;
                for (Spot const& s : foreign)
                    if (s.who && s.x == s.who->GetPositionX() && s.y == s.who->GetPositionY() &&
                        std::fabs(s.z - P.z) < 25.0f &&
                        (s.x - P.x) * (s.x - P.x) + (s.y - P.y) * (s.y - P.y) <
                            (AggroReach(me, s.who) + 3.0f) * (AggroReach(me, s.who) + 3.0f))
                    {
                        seen = true;
                        break;
                    }
                if (seen)
                {
                    ++refused[1];
                    continue;
                }
            }
            if (walker && RouteGap(walker, C.x, C.y, C.z) < kBossWalkGap)
            {
                ++refused[9];
                continue;
            }
            if (at ? !me->GetMap()->isInLineOfSight(tx, ty, tz + 2.0f, P.x, P.y, P.z + 2.0f)
                   : !target->IsWithinLOS(P.x, P.y, P.z + 2.0f))
            {
                ++refused[2];
                continue;
            }
            // The camp sees the pull spot, on its level: the fight comes to the spot and the healers
            // heal it from the camp (2026-09-25: a camp on the ramp below Majordomo's platform left
            // every healer without sight of the tanks above).
            if (std::fabs(C.z - P.z) > zTol || !me->GetMap()->isInLineOfSight(C.x, C.y, C.z + 2.0f, P.x, P.y, P.z + 2.0f))
            {
                ++refused[3];
                continue;
            }
            PathInfo toCamp(me);
            toCamp.calculate(C.x, C.y, C.z, false);
            if (toCamp.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            {
                ++refused[4];
                continue;
            }
            PathInfo pullToCamp(me);
            pullToCamp.calculate(Vector3(P.x, P.y, P.z), Vector3(C.x, C.y, C.z), false);
            if (pullToCamp.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            {
                ++refused[5];
                continue;
            }
            // ...and the pack itself must be able to walk to the camp: a camp the creatures cannot
            // path to makes them evade after 24 s "target unreachable", and a linked pack resets
            // with its boss (2026-09-25: Sulfuron's priests, twice, their tank 0.8 yd away).
            if (!at)
            {
                PathInfo theirs(target);
                theirs.calculate(C.x, C.y, C.z, false);
                if (theirs.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                {
                    ++refused[6];
                    continue;
                }
            }
            float back = 0.0f;
            PointsArray const& bp = pullToCamp.getPath();
            for (size_t k = 1; k < bp.size(); ++k)
                back += (bp[k] - bp[k - 1]).length();
            if (back > backMax)
            {
                ++refused[7];
                continue;   // the run back must be short and direct
            }
            // The raid's walk to the camp: sampled every 4 yd, its closest pass by another pack
            // and by the target itself (walking the raid past the pack pulls it early).
            float route = 1e9f, walk = 0.0f, pastTarget = 1e9f;
            PointsArray const& rp = toCamp.getPath();
            for (size_t k = 1; k < rp.size(); ++k)
            {
                Vector3 const seg = rp[k] - rp[k - 1];
                float const len = seg.length();
                walk += len;
                for (float s = 0.0f; s <= len; s += 4.0f)
                {
                    Vector3 const q = rp[k - 1] + seg * (len > 0.0f ? s / len : 0.0f);
                    route = std::min(route, Nearest(foreign, q.x, q.y, q.z));
                    float const dt = std::sqrt((q.x - tx) * (q.x - tx) + (q.y - ty) * (q.y - ty));
                    if (std::fabs(q.z - tz) < 25.0f)
                    {
                        route = std::min(route, dt + 5.0f);   // the target's own reach, slightly relaxed
                        pastTarget = std::min(pastTarget, dt);
                    }
                    if (!pack.empty())
                        pastTarget = std::min(pastTarget, Nearest(pack, q.x, q.y, q.z));
                }
            }
            // A walk to the camp that passes inside the target's own reach is not a way round it
            // (2026-09-25: twice the leader walked 17 yd past a Firelord and died alone).
            if (pastTarget < pastMin)
            {
                ++refused[8];
                continue;
            }
            float const dP = Nearest(foreign, P.x, P.y, P.z), dC = Nearest(foreign, C.x, C.y, C.z);
            float const score = walk * 0.2f + 10.0f * std::max(0.0f, 32.0f - dP) +
                10.0f * std::max(0.0f, 48.0f - dC) + 10.0f * std::max(0.0f, 30.0f - route);
            if (score < bestScore)
            {
                bestScore = score;
                bestP = P;
                bestC = C;
                bestDp = dP;
                bestDc = dC;
                bestDr = route;
            }
        }
        // Corridor plan: no ring around the pack fits (MC's passages are narrow), so the pull goes
        // back the way the raid came - the pack's own path to the last clear spot. The pull spot is
        // the first point on it out of the pack's reach and in its sight, the camp the first point
        // 45 yd out that sees the pull spot (or the clear spot itself).
        bool corridor = false;
        if (bestScore > 1e8f && !at && pilot.hasSafe)
        {
            PathInfo back(target);
            back.calculate(pilot.safeX, pilot.safeY, pilot.safeZ, false);
            if (!(back.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
            {
                std::vector<Vector3> samples;
                PointsArray const& bp = back.getPath();
                for (size_t k = 1; k < bp.size(); ++k)
                {
                    Vector3 const seg = bp[k] - bp[k - 1];
                    float const len = seg.length();
                    for (float d = 0.0f; d < len; d += 2.0f)
                        samples.push_back(bp[k - 1] + seg * (len > 0.0f ? d / len : 0.0f));
                }
                if (!bp.empty())
                    samples.push_back(bp[bp.size() - 1]);
                int iP = -1, iC = -1;
                for (size_t k = 0; k < samples.size(); ++k)
                {
                    Vector3 const& q = samples[k];
                    float const dt = std::sqrt((q.x - tx) * (q.x - tx) + (q.y - ty) * (q.y - ty));
                    if (iP < 0)
                    {
                        if (dt >= 28.0f && (pack.empty() || Nearest(pack, q.x, q.y, q.z) >= 26.0f) &&
                            target->IsWithinLOS(q.x, q.y, q.z + 2.0f) && !Molten(me, q.x, q.y, q.z))
                            iP = int(k);
                        continue;
                    }
                    Vector3 const& P = samples[iP];
                    if (((dt >= 45.0f && (!walker || RouteGap(walker, q.x, q.y, q.z) >= kBossWalkGap)) || k + 1 == samples.size()) &&
                        std::fabs(q.z - P.z) <= 8.0f && !Molten(me, q.x, q.y, q.z) &&
                        me->GetMap()->isInLineOfSight(q.x, q.y, q.z + 2.0f, P.x, P.y, P.z + 2.0f))
                    {
                        iC = int(k);
                        break;
                    }
                }
                if (iP >= 0 && iC >= 0)
                {
                    Vector3 const& P = samples[iP];
                    Vector3 const& C = samples[iC];
                    bestP = {P.x, P.y, P.z, nullptr};
                    bestC = {C.x, C.y, C.z, nullptr};
                    bestDp = Nearest(foreign, P.x, P.y, P.z);
                    bestDc = Nearest(foreign, C.x, C.y, C.z);
                    bestDr = std::min(bestDp, bestDc);
                    bestScore = 0.0f;
                    corridor = true;
                }
            }
        }
        if (bestScore > 1e8f)
        {
            Note(pilot, me, "no plan for %s: refused ground/lava %u, pack reach %u, no sight %u, camp level/sight %u, no path to camp %u, pull-to-camp %u, pack-to-camp %u, run back long %u, walk past it %u, camp on %s's walk %u",
                target->GetName(), refused[0], refused[1], refused[2], refused[3], refused[4], refused[5], refused[6], refused[7], refused[8],
                walker ? walker->GetName() : "a boss", refused[9]);
            return false;
        }
        pilot.planned = true;
        pilot.planFresh = true;
        pilot.pullX = bestP.x; pilot.pullY = bestP.y; pilot.pullZ = bestP.z;
        pilot.campX = bestC.x; pilot.campY = bestC.y; pilot.campZ = bestC.z;
        // A timed pull: the tanks take the pack at the camp and the raid waits 16 yd to its side, on
        // the side away from the walk (2026-11-04: the whole raid stood at the camp; the four elites
        // the leader brought back ran into it - 16 dead in 5 s to Cone of Fire and Fire Blossom).
        pilot.hasRear = false;
        if (walker)
        {
            float dx = bestC.x - bestP.x, dy = bestC.y - bestP.y;
            float const len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f)
            {
                dx /= len;
                dy /= len;
                float bestGap = -1.0f;
                struct D2 { float x, y; };
                D2 const dirs[3] = { {-dy, dx}, {dy, -dx}, {dx, dy} };   // either side, then behind
                for (D2 const& d : dirs)
                {
                    float rx = bestC.x + 16.0f * d.x, ry = bestC.y + 16.0f * d.y, rz = bestC.z;
                    if (!Ground(me, rx, ry, rz) || Molten(me, rx, ry, rz) || std::fabs(rz - bestC.z) > 5.0f ||
                        !me->GetMap()->isInLineOfSight(rx, ry, rz + 2.0f, bestC.x, bestC.y, bestC.z + 2.0f))
                        continue;
                    // ...it also sees where the pack meets the leader on his run back (8 yd out from
                    // the camp toward the pull spot), and the camp is a short walk from it on the
                    // navmesh (2026-11-07: a rear spot in a hollow 4 yd below a camp in Lucifron's
                    // room had no way out - ten healers and eight melee stood there 113 s without
                    // sight of the fight 9 yd from the camp; the tanks died with 0 healing).
                    {
                        float const mx = bestC.x - 8.0f * dx, my = bestC.y - 8.0f * dy;
                        float mz = bestC.z;
                        me->UpdateAllowedPositionZ(mx, my, mz);
                        if (!me->GetMap()->isInLineOfSight(rx, ry, rz + 2.0f, mx, my, mz + 2.0f))
                            continue;
                        PathInfo walk(me);
                        walk.calculate(Vector3(rx, ry, rz), Vector3(bestC.x, bestC.y, bestC.z), false);
                        if (walk.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                            continue;
                        PointsArray const& pts = walk.getPath();
                        float len = 0.0f;
                        for (size_t k = 1; k < pts.size(); ++k)
                            len += (pts[k] - pts[k - 1]).length();
                        if (pts.empty() || len > 16.0f * 1.5f + 5.0f)
                            continue;
                    }
                    float const gap = RouteGap(walker, rx, ry, rz);
                    if (gap > bestGap && Nearest(foreign, rx, ry, rz) >= 30.0f)
                    {
                        bestGap = gap;
                        pilot.hasRear = true;
                        pilot.rearX = rx; pilot.rearY = ry; pilot.rearZ = rz;
                    }
                }
            }
        }
        pilot.planTX = tx; pilot.planTY = ty;
        pilot.planDp = bestDp; pilot.planDc = bestDc; pilot.planRoute = bestDr;
        pilot.crowder.Clear();
        float crowdD = 1e9f;
        for (Spot const& s : foreign)
        {
            float const d = std::sqrt((s.x - bestC.x) * (s.x - bestC.x) + (s.y - bestC.y) * (s.y - bestC.y));
            if (s.who && d < crowdD)
            {
                crowdD = d;
                pilot.crowder = s.who->GetObjectGuid();
            }
        }
        if (pilot.crowdWaitMs == 0)
            Note(pilot, me, "plan for %s%s: pull spot clear of other packs by %.0f yd, camp by %.0f yd, route by %.0f yd",
            target->GetName(), corridor ? " (corridor back to the clear spot)" : relaxed ? " (relaxed)" : "", std::min(bestDp, 999.0f), std::min(bestDc, 999.0f), std::min(bestDr, 999.0f));
        return true;
    }

    // [AMBUSH] The point of a patrol's walk farthest from every idle pack and every other patrol's
    // route: where it is pulled (2026-09-25: Baron Geddon's walk passes six packs; pulled where he
    // happened to be, he dragged a Firewalker pack in every time).
    bool AmbushPoint(Player* me, Creature* target, float& qx, float& qy, float& qz, float& clearance)
    {
        if (!Patrols(target))
            return false;
        std::vector<Spot> foreign;
        ForeignIdle(me, target, 320.0f, foreign);
        float best = -1.0f;
        ForEachRoutePoint(target, [&](float x, float y, float z)
        {
            float const d = Nearest(foreign, x, y, z);
            if (d > best + 0.5f)
            {
                best = d;
                qx = x;
                qy = y;
                qz = z;
            }
        });
        clearance = best;
        return best >= 0.0f;
    }

    // A ranged pull with whatever the leader carries in the ranged slot (a player's "shoot the
    // pack"); zero when it has nothing to shoot with.
    uint32 RangedPullSpell(Player* me)
    {
        Item* ranged = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged || ranged->IsBroken())
            return 0;
        uint32 spell = 0;
        switch (ranged->GetProto()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW: spell = 2480; break;
            case ITEM_SUBCLASS_WEAPON_GUN: spell = 7918; break;
            case ITEM_SUBCLASS_WEAPON_CROSSBOW: spell = 7919; break;
            case ITEM_SUBCLASS_WEAPON_THROWN: spell = 2764; break;
            case ITEM_SUBCLASS_WEAPON_WAND: spell = 5019; break;
            default: return 0;
        }
        return me->HasSpell(spell) ? spell : 0;
    }

    // Cleared spawns that came back within reach are executed: no fight, no loot (the kill is the
    // creature's own). Returns how many.
    uint32 ExecuteRespawns(Player* me, Pilot& pilot, float range)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, range);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(me, searcher, range);
        uint32 executed = 0;
        for (Creature* c : near)
        {
            if (!c->IsAlive() || c->IsInCombat() || !c->IsHostileTo(me) || c->IsWorldBoss() || !IsCleared(c))
                continue;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: respawned %s (entry %u, spawn %u) executed - already cleared",
                me->GetName(), c->GetName(), c->GetEntry(), c->GetDBTableGUIDLow());
            c->DealDamage(c, c->GetHealth(), nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            ++executed;
        }
        return executed;
    }

    // Rest and wait where nothing idle is within reach; false when already there (or no such
    // spot is known yet).
    bool MoveToSafe(Player* me, Pilot& pilot)
    {
        if (!pilot.hasSafe || (IdleNeighbours(me, me, nullptr, 40.0f) == 0 &&
            !PatrolBossWalkNear(me, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), kBossWalkGap)))
            return false;
        if (me->GetDistance(pilot.safeX, pilot.safeY, pilot.safeZ) <= 4.0f)
            return false;
        if (!me->IsMoving())
        {
            // The clear spot is only where it was clear; the walk back to it must be too (2026-11-07:
            // stepping off Baron Geddon's walk, the way back ran past a Firelord - it took the leader
            // at 0 yd, Geddon and a Firewalker pack joined, 39 dead).
            PathInfo path(me);
            path.calculate(pilot.safeX, pilot.safeY, pilot.safeZ, false);
            PointsArray const& pts = path.getPath();
            float const look = std::min(150.0f, me->GetDistance(pilot.safeX, pilot.safeY, pilot.safeZ) + 30.0f);
            std::list<Creature*> near;
            MaNGOS::AnyUnitInObjectRangeCheck check(me, look);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
            Cell::VisitGridObjects(me, searcher, look);
            Creature* blocker = nullptr;
            for (Creature* c : near)
            {
                if (!c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                    c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me))
                    continue;
                for (size_t k = 1; k < pts.size() && !blocker; ++k)
                {
                    Vector3 const d = pts[k] - pts[k - 1];
                    int const n = std::max(1, int(d.length() / 2.0f));
                    for (int j = 1; j <= n && !blocker; ++j)
                    {
                        Vector3 const p = pts[k - 1] + d * (float(j) / n);
                        if (std::fabs(p.z - c->GetPositionZ()) < 15.0f && c->GetDistance2d(p.x, p.y) < 25.0f)
                            blocker = c;
                    }
                }
                if (blocker)
                    break;
            }
            if (blocker)
            {
                if (pilot.lastEvent.rfind("the way to the clear spot", 0) != 0)
                    Note(pilot, me, "the way to the clear spot passes %s (entry %u) - staying here", blocker->GetName(),
                        blocker->GetEntry());
                return false;
            }
            if (pilot.lastEvent.rfind("backing off to a clear spot", 0) != 0)
                Note(pilot, me, "backing off to a clear spot -> (%.0f, %.0f)", pilot.safeX, pilot.safeY);
            me->GetMotionMaster()->MovePoint(0, pilot.safeX, pilot.safeY, pilot.safeZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
        }
        return true;
    }

    // Progress on a walk: the leader has moved 4 yd since the last check (a winding route gets
    // no closer in a straight line for a long time - the MC entrance ramp).
    bool Moved(Player* me, Pilot& pilot)
    {
        float const dx = me->GetPositionX() - pilot.lastX, dy = me->GetPositionY() - pilot.lastY;
        if (dx * dx + dy * dy < 16.0f)
            return false;
        pilot.lastX = me->GetPositionX();
        pilot.lastY = me->GetPositionY();
        return true;
    }

    // The pull, as a raid leader does it: walk to the pack alone while the raid holds its
    // camp, get its attention (the pack aggroes on approach, or a hit starts it), then run
    // back so the fight happens where the healers already stand - out of the pack's own
    // corner, and away from whatever patrols past it.
    bool DrivePull(AiBotAI* ai, Player* me, Pilot& pilot)
    {
        uint32 const step = AIBOT_UPDATE_INTERVAL;
        Creature* target = me->GetMap()->GetCreature(pilot.target);
        if (pilot.phase == Phase::Approach)
        {
            if (!target || !target->IsAlive())
            {
                pilot.phase = Phase::Idle;
                pilot.target.Clear();
                return false;
            }
            if (target->IsInCombat())
            {
                // It woke on the way in. Short of it, the raid falls back with the leader to the
                // last clear spot it walked through and fights there, not next to the packs
                // around the target (2026-09-24: a Lava Annihilator met on the walk dragged a
                // Destroyer, a Giant and a Firelord into the fight and wiped the raid).
                if (pilot.hasSafe && me->GetDistance(target) > 12.0f &&
                    me->GetDistance(pilot.safeX, pilot.safeY, pilot.safeZ) > 10.0f &&
                    me->GetDistance(pilot.safeX, pilot.safeY, pilot.safeZ) < 120.0f)
                {
                    Note(pilot, me, "%s (entry %u) woke on the way in - falling back to the last clear spot (leader %.0f,%.0f,%.0f target %.0f,%.0f,%.0f camp %.0f,%.0f planned %u)",
                        target->GetName(), target->GetEntry(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                        target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), pilot.campX, pilot.campY,
                        uint32(pilot.planned));
                    pilot.campX = pilot.safeX;
                    pilot.campY = pilot.safeY;
                    pilot.campZ = pilot.safeZ;
                    pilot.phase = Phase::Return;
                    pilot.returnMs = 0;
                    pilot.retreating = true;
                    ++pilot.pulls;
                    me->AttackStop();
                    me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ,
                        MOVE_PATHFINDING | MOVE_RUN_MODE);
                    return true;
                }
                // Close to it (or nowhere clear to go): fight it where we stand.
                pilot.phase = Phase::Fight;
                pilot.fightMs = 0;
                ++pilot.fights;
                return false;
            }
            if (!Pullable(me, target, pilot))
            {
                pilot.phase = Phase::Idle;
                pilot.target.Clear();
                return false;
            }
            float dist = me->GetDistance(target);
            bool const ambush = pilot.ambushTarget == pilot.target && !pilot.ambushTarget.IsEmpty();
            if (!ambush && pilot.planned && (std::fabs(target->GetPositionX() - pilot.planTX) > 15.0f ||
                std::fabs(target->GetPositionY() - pilot.planTY) > 15.0f))
                pilot.planned = false;   // it moved: plan again
            // The playbook's lure for this patrol: the leader and the tanks wait beside its walk where it
            // notices them as it runs by, the raid at the camp behind (2026-11-04: Baron Geddon runs his
            // loop in ~30 s past ten packs; no timing keeps him away and no ambush point is 32 yd clear).
            if (!pilot.planned && !ambush && Patrols(target) && pilot.noAmbush != pilot.target && dist <= 250.0f)
            {
                float lx, ly, lz, cx, cy, cz;
                bool hasCamp = false;
                if (SuiRaidPlaybook::Lure(target->GetEntry(), lx, ly, lz, hasCamp, cx, cy, cz) &&
                    LureClearance(me, target, lx, ly, lz) >= SuiRaidPlaybook::LureClearYards(target->GetEntry()))
                {
                    float qx = lx, qy = ly, qz = lz, best = 1e9f;
                    ForEachRoutePoint(target, [&](float x, float y, float z)
                    {
                        float const d = (x - lx) * (x - lx) + (y - ly) * (y - ly);
                        if (d < best)
                        {
                            best = d;
                            qx = x; qy = y; qz = z;
                        }
                    });
                    me->UpdateAllowedPositionZ(lx, ly, lz);
                    std::vector<Spot> foreign;
                    ForeignIdle(me, target, 140.0f, foreign);
                    // The camp: on the lure's level, in its sight, and a walk the boss can make (2026-11-04: a
                    // camp picked by hand stood on a rock 4 yd up - the healers had no sight of the tanks
                    // below and Geddon, unable to reach the one he wanted, evaded after 24 s). Clear of
                    // idle packs first; the playbook's own camp only when it passes the same checks.
                    auto campOk = [&](float x, float y, float& z)
                    {
                        if (!Ground(me, x, y, z) || std::fabs(z - lz) > 3.0f || Molten(me, x, y, z) ||
                            !me->GetMap()->isInLineOfSight(x, y, z + 2.0f, lx, ly, lz + 2.0f))
                            return false;
                        PathInfo mine(me);
                        mine.calculate(x, y, z, false);
                        PathInfo his(target);
                        his.calculate(x, y, z, false);
                        return !(mine.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) &&
                            !(his.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE));
                    };
                    bool campFound = false;
                    if (hasCamp)
                    {
                        float z = lz;
                        if (campOk(cx, cy, z))
                        {
                            cz = z;
                            campFound = true;
                        }
                    }
                    if (!campFound)
                    {
                        float bestScore = -1e9f;
                        for (float r : {18.0f, 22.0f, 26.0f})
                            for (int i = 0; i < 24; ++i)
                            {
                                float const a = i * float(M_PI) / 12.0f;
                                float x = lx + r * std::cos(a), y = ly + r * std::sin(a), z = lz;
                                if (!campOk(x, y, z))
                                    continue;
                                float const score = Nearest(foreign, x, y, z) - 0.5f * std::fabs(r - 22.0f);
                                if (score > bestScore)
                                {
                                    bestScore = score;
                                    cx = x; cy = y; cz = z;
                                    campFound = true;
                                }
                            }
                    }
                    if (!campFound)
                    {
                        Note(pilot, me, "%s: no camp in sight of the playbook lure (%.0f, %.0f) - the lure is skipped",
                            target->GetName(), lx, ly);
                        pilot.noAmbush = pilot.target;
                        return true;
                    }
                    pilot.planned = true;
                    pilot.planFresh = true;
                    pilot.pullX = lx; pilot.pullY = ly; pilot.pullZ = lz;
                    pilot.campX = cx; pilot.campY = cy; pilot.campZ = cz;
                    pilot.planTX = target->GetPositionX();
                    pilot.planTY = target->GetPositionY();
                    pilot.planDp = pilot.planDc = pilot.planRoute = 999.0f;
                    pilot.crowder.Clear();
                    pilot.hasRear = false;
                    pilot.timedBy.Clear();
                    pilot.ambushTarget = pilot.target;
                    pilot.ambushX = qx;
                    pilot.ambushY = qy;
                    pilot.ambushZ = qz;
                    pilot.ambushWaitMs = 0;
                    pilot.lured = true;
                    // The tanks wait at the camp (HoldForPull) and the boss chases the leader to them; the
                    // raid waits 16 yd to the side of the camp farthest from every idle pack (2026-11-04: with
                    // the tanks at the lure the fight stayed there, 37 yd from a Lava Annihilator - a hunter
                    // stepping out of an inferno woke it).
                    {
                        float dx = cx - lx, dy = cy - ly;
                        float const len = std::max(0.1f, std::sqrt(dx * dx + dy * dy));
                        dx /= len;
                        dy /= len;
                        struct D2 { float x, y; };
                        D2 const dirs[3] = { {-dy, dx}, {dy, -dx}, {dx, dy} };
                        float bestClear = -1.0f;
                        for (D2 const& d : dirs)
                        {
                            float rx = cx + 14.0f * d.x, ry = cy + 14.0f * d.y, rz = cz;
                            if (!campOk(rx, ry, rz) || !me->GetMap()->isInLineOfSight(rx, ry, rz + 2.0f, cx, cy, cz + 2.0f))
                                continue;
                            float const clearYd = Nearest(foreign, rx, ry, rz);
                            if (clearYd > bestClear)
                            {
                                bestClear = clearYd;
                                pilot.hasRear = true;
                                pilot.rearX = rx; pilot.rearY = ry; pilot.rearZ = rz;
                            }
                        }
                        if (pilot.hasRear)
                            Note(pilot, me, "the raid waits at (%.0f, %.0f), %.0f yd from the nearest idle pack; the tanks at the camp",
                                pilot.rearX, pilot.rearY, bestClear);
                    }
                    Note(pilot, me, "%s: playbook lure at (%.0f, %.0f), %.0f yd off its walk - the raid camps at (%.0f, %.0f)",
                        target->GetName(), lx, ly, std::sqrt(best), cx, cy);
                    return true;
                }
            }
            if (!pilot.planned && !ambush && Patrols(target) && pilot.noAmbush != pilot.target &&
                dist <= 250.0f && RangedPullSpell(me))
            {
                float qx = 0.0f, qy = 0.0f, qz = 0.0f, clear = 0.0f;
                if (AmbushPoint(me, target, qx, qy, qz, clear))
                {
                    Vector3 const at(qx, qy, qz);
                    if (PlanPull(me, target, pilot, &at))
                    {
                        // A boss fight spreads 25 yd round the tank (infernos, bombs, cones): its camp
                        // keeps 50 yd and the ambush point 40 yd from every idle pack (2026-11-04: Baron
                        // Geddon ambushed with a pack 41 yd from the camp - it joined at +31 s, a second
                        // one at +34 s, wipe with Geddon at 63 %).
                        bool const bossFight = target->IsWorldBoss();
                        bool const crowded = pilot.planRoute < 15.0f || pilot.planDc < (bossFight ? 40.0f : 20.0f) ||
                            (bossFight && clear < 28.0f);
                        // A crowded ambush (the walk or the camp beside another pack) clears that pack
                        // first (2026-11-03: Geddon's ambush route passed 1 yd from three packs, camp 6 yd
                        // from one - the raid walked through them and Geddon came on top).
                        if (crowded && pilot.crowdHops < 3)
                            if (Creature* crowd = me->GetMap()->GetCreature(pilot.crowder))
                                if (crowd != target && Pullable(me, crowd, pilot) &&
                                    !Patrols(crowd) && !BossOf(crowd) &&   // never another boss (2026-11-04: Shazzrah "crowded" Geddon's ambush)
                                    PathLength(me, crowd->GetPositionX(), crowd->GetPositionY(), crowd->GetPositionZ()) >= 0.0f)
                                {
                                    // A pack on the boss's own walk goes first too, pulled while he is away
                                    // (2026-11-04: left in place, Geddon's ambush route passed it at 2 yd, the
                                    // raid walked into it and Geddon arrived at +46 s with a second pack).
                                    bool const onHisWalk = PatrollingBossNear(me, pilot, crowd) == target;
                                    if (onHisWalk)
                                    {
                                        pilot.timedBy = target->GetObjectGuid();
                                        pilot.timedWaitMs = 0;
                                        pilot.timedClearTicks = 0;
                                    }
                                    ++pilot.crowdHops;
                                    Note(pilot, me, "%s (entry %u) crowds the ambush of %s (route %.0f yd, camp %.0f yd, ambush point %.0f yd) - it goes first%s",
                                        crowd->GetName(), crowd->GetEntry(), target->GetName(), pilot.planRoute, pilot.planDc,
                                        std::min(clear, 999.0f),
                                        onHisWalk ? ", pulled while he is away" : "");
                                    pilot.target = crowd->GetObjectGuid();
                                    pilot.targetEntry = crowd->GetEntry();
                                    pilot.planned = false;
                                    pilot.stuckMs = 0;
                                    pilot.bestDist = me->GetDistance(crowd);
                                    return true;
                                }
                        // Still crowded and nothing to clear first: no ambush through other packs - the packs
                        // along his walk go first, timed (2026-11-04: Geddon's route at 0 yd from a pack,
                        // walked into it, Geddon on top - three wipes).
                        if (crowded)
                        {
                            Note(pilot, me, "%s: no clear way to an ambush (route %.0f yd, camp %.0f yd, ambush point %.0f yd) - the packs on his walk go first",
                                target->GetName(), pilot.planRoute, pilot.planDc, std::min(clear, 999.0f));
                            pilot.notFirstUntil[pilot.target] = WorldTimer::getMSTime() + 10 * 60 * 1000;
                            pilot.phase = Phase::Idle;
                            pilot.target.Clear();
                            pilot.planned = false;
                            // and not standing here among them (2026-11-04: the raid stopped 6 yd from a
                            // Firewalker pack on Geddon's walk; it woke, Geddon came at +29 s)
                            me->StopMoving();
                            MoveToSafe(me, pilot);
                            return false;
                        }
                        pilot.ambushTarget = pilot.target;
                        pilot.ambushX = qx;
                        pilot.ambushY = qy;
                        pilot.ambushZ = qz;
                        pilot.ambushWaitMs = 0;
                        Note(pilot, me, "%s patrols: ambush where its walk is clearest (%.0f, %.0f), %.0f yd from other packs",
                            target->GetName(), qx, qy, std::min(clear, 999.0f));
                        return true;
                    }
                }
            }
            if (pilot.planHoldMs > step)
                pilot.planHoldMs -= step;
            else
                pilot.planHoldMs = 0;
            if (!pilot.planned && pilot.planHoldMs == 0 && dist <= 180.0f && PlanPull(me, target, pilot) &&
                (pilot.planDc < 35.0f || pilot.planDp < 22.0f || pilot.planRoute < 15.0f))
            {
                // Crowded: no side of this pack keeps the raid clear of the next one. A patrol
                // walks on - wait for it to be somewhere better. A pack that stands still has its
                // neighbour taken first (the one nearest the would-be camp).
                bool patrol =
                    Patrols(target);
                // The crowding is a patrol's route (a boss walking through the room): pull while it
                // is at the far end of its walk, not while it is near the camp.
                if (!patrol && !pilot.crowder.IsEmpty())
                    if (Creature* walker = me->GetMap()->GetCreature(pilot.crowder))
                        if (Patrols(walker))
                        {
                            // A boss walking through the room is never out of the way for long: it
                            // goes first (2026-09-24: Lucifron's route passes every Core Hound pack
                            // of his room within 20 yd; every hound fight had him join).
                            Creature* boss = BossOf(walker);
                            if (false && boss && boss != target && walker->GetDistance(target) < 60.0f &&
                                pilot.crowdHops < 3 && Pullable(me, boss, pilot) &&
                                PathLength(me, boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ()) >= 0.0f)
                            {
                                ++pilot.crowdHops;
                                Note(pilot, me, "%s patrols through the pull of %s - it goes first", boss->GetName(),
                                    target->GetName());
                                pilot.target = boss->GetObjectGuid();
                                pilot.targetEntry = boss->GetEntry();
                                pilot.planned = false;
                                pilot.crowdWaitMs = 0;
                                pilot.stuckMs = 0;
                                pilot.bestDist = me->GetDistance(boss);
                                return true;
                            }
                            if (walker->GetDistance(pilot.campX, pilot.campY, pilot.campZ) >= 70.0f &&
                                walker->GetDistance(target) >= 60.0f)
                                goto crowdedButClear;
                            // A trash patrol is killed first, at its own ambush, not waited out: it
                            // walks back past the camp mid-fight (2026-11-05: a Lava Surger's walk
                            // passed 3 yd from the imp pull; after the 120 s wait the raid went anyway,
                            // the Surger walked in and 17 imps joined - all 39 dead).
                            if (walker != target && walker->IsAlive() && !walker->IsInCombat() && !BossOf(walker) &&
                                pilot.crowdHops < 3 && Pullable(me, walker, pilot) &&
                                PathLength(me, walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ()) >= 0.0f)
                            {
                                ++pilot.crowdHops;
                                Note(pilot, me, "%s (entry %u) patrols through the pull of %s - it goes first",
                                    walker->GetName(), walker->GetEntry(), target->GetName());
                                pilot.target = walker->GetObjectGuid();
                                pilot.targetEntry = walker->GetEntry();
                                pilot.planned = false;
                                pilot.crowdWaitMs = 0;
                                pilot.stuckMs = 0;
                                pilot.bestDist = me->GetDistance(walker);
                                return true;
                            }
                            patrol = true;   // wait for it like any patrol
                        }
                if (patrol && pilot.crowdWaitMs < 120000)
                {
                    pilot.planned = false;
                    pilot.planHoldMs = 3000;
                    pilot.crowdWaitMs += 3000;
                    if (!MoveToSafe(me, pilot) && me->IsMoving())
                        me->StopMoving();
                    if (pilot.crowdWaitMs == 3000)
                        Note(pilot, me, "%s is walking through other packs - waiting for a clear spot", target->GetName());
                    return true;
                }
                Creature* crowder = pilot.crowder.IsEmpty() ? nullptr : me->GetMap()->GetCreature(pilot.crowder);
                if (!patrol && crowder && pilot.crowdHops < 3 && Pullable(me, crowder, pilot) && !BossOf(crowder) &&
                    PathLength(me, crowder->GetPositionX(), crowder->GetPositionY(), crowder->GetPositionZ()) >= 0.0f)
                {
                    ++pilot.crowdHops;
                    Note(pilot, me, "%s (entry %u) crowds the pull of %s - it goes first", crowder->GetName(),
                        crowder->GetEntry(), target->GetName());
                    pilot.target = crowder->GetObjectGuid();
                    pilot.targetEntry = crowder->GetEntry();
                    pilot.planned = false;
                    pilot.stuckMs = 0;
                    pilot.bestDist = me->GetDistance(crowder);
                    return true;
                }
            }
        crowdedButClear:
            if (!pilot.planned && pilot.planHoldMs > 0 && pilot.crowdWaitMs > 0 && pilot.crowdWaitMs < 120000)
            {
                MoveToSafe(me, pilot);   // still waiting out the patrol
                return true;
            }
            // An ambush waits for the patrol to be well away before the raid walks to its camp
            // (2026-09-25: Baron Geddon met the raid on its walk and dragged a pack in).
            if (pilot.planned && ambush && !pilot.lured &&
                (target->GetDistance(pilot.campX, pilot.campY, pilot.campZ) < 55.0f || dist < 55.0f))
            {
                if (!MoveToSafe(me, pilot) && me->IsMoving())
                    me->StopMoving();
                if ((pilot.ambushWaitMs += step) % 20000 < step)
                    Note(pilot, me, "waiting for %s to walk clear of the camp", target->GetName());
                if (pilot.ambushWaitMs > 300000)
                {
                    pilot.skip[pilot.target] = WorldTimer::getMSTime() + 5 * 60 * 1000;
                    pilot.ambushTarget.Clear();
                    pilot.phase = Phase::Idle;
                    pilot.target.Clear();
                    pilot.planned = false;
                    return false;
                }
                return true;
            }
            // Any walk, planned or not, stops at an idle pack of another pull 32 yd out: walking on, the
            // raid behind the leader passes inside its reach (2026-11-03: the walk to a Core Hound pack's
            // camp went past the next pack at 5 yd; both packs and Lucifron joined). The path search to
            // a far target gives up before the packs on its way are seen, so this is the last word.
            if (!ambush && dist > 32.0f)
            {
                std::list<Creature*> around;
                MaNGOS::AnyUnitInObjectRangeCheck check(me, 32.0f);
                MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
                Cell::VisitGridObjects(me, searcher, 32.0f);
                Creature* passing = nullptr;
                float best = 33.0f;
                Creature* timed = pilot.timedBy.IsEmpty() ? nullptr : me->GetMap()->GetCreature(pilot.timedBy);
                Creature* left = pilot.passedFrom.IsEmpty() ? nullptr : me->GetMap()->GetCreature(pilot.passedFrom);
                Creature* bossNear = nullptr;
                for (Creature* c : around)
                {
                    if (pilot.passHops >= 3)
                        break;
                    if (!Pullable(me, c, pilot, true) || SamePack(target, c) || std::fabs(c->GetPositionZ() - me->GetPositionZ()) >= 15.0f)
                        continue;
                    if (left && (c == left || SamePack(left, c)))
                        continue;   // never back to the pull it just stepped off (2026-11-03: Firewalker <-> Lava Elemental)
                    if (timed && (c == timed || (c->GetCreatureGroup() && c->GetCreatureGroup() == timed->GetCreatureGroup())))
                        continue;   // the walker the pull is timed around: the timing keeps him away
                    if (BossOf(c))
                    {
                        bossNear = c;   // a boss's pack is never "the next pull" by the way
                        continue;
                    }
                    if (me->GetDistance(c) < best)
                    {
                        best = me->GetDistance(c);
                        passing = c;
                    }
                }
                if (!passing && bossNear && !(timed && (bossNear == timed || SamePack(timed, bossNear))))
                {
                    // The walk passes a boss's pack: this pull waits, walking on would wake him.
                    Note(pilot, me, "the walk to %s passes %s (%.0f yd) - it waits", target->GetName(), bossNear->GetName(),
                        me->GetDistance(bossNear));
                    pilot.skip[pilot.target] = WorldTimer::getMSTime() + 3 * 60 * 1000;
                    pilot.phase = Phase::Idle;
                    pilot.target.Clear();
                    pilot.planned = false;
                    me->StopMoving();
                    MoveToSafe(me, pilot);
                    return false;
                }
                // The boss the pull is timed around comes near while the raid walks in: back to the clear spot.
                if (timed && timed->IsAlive() && !timed->IsInCombat() && me->GetDistance(timed) < 60.0f)
                {
                    if (pilot.lastEvent.find("comes near") == std::string::npos)
                        Note(pilot, me, "%s comes near (%.0f yd) on the walk to %s - backing off", timed->GetName(),
                            me->GetDistance(timed), target->GetName());
                    if (!MoveToSafe(me, pilot) && me->IsMoving())
                        me->StopMoving();
                    return true;
                }
                if (passing)
                {
                    Note(pilot, me, "%s (entry %u) stands %.0f yd from the walk to %s - it goes first", passing->GetName(),
                        passing->GetEntry(), best, target->GetName());
                    pilot.skip.erase(passing->GetObjectGuid());
                    pilot.passedFrom = pilot.target;
                    ++pilot.passHops;
                    pilot.target = passing->GetObjectGuid();
                    pilot.targetEntry = passing->GetEntry();
                    pilot.planned = false;
                    pilot.stuckMs = 0;
                    pilot.bestDist = best;
                    if (me->IsMoving())
                        me->StopMoving();
                    return true;
                }
            }
            if (pilot.planned)
            {
                if (me->GetDistance(pilot.campX, pilot.campY, pilot.campZ) <= 4.0f)
                {
                    me->StopMoving();
                    pilot.phase = Phase::Pull;
                    pilot.gatherMs = 0;
                    pilot.patrolWaitMs = 0;
                    pilot.stuckMs = 0;
                    pilot.pullShotMs = 0;
                    pilot.assistWaitMs = 0;
                    pilot.bestDist = me->GetDistance(pilot.pullX, pilot.pullY, pilot.pullZ);
                    return true;
                }
                pilot.approachMs += step;
                if (pilot.planFresh)
                {
                    // The walk under way was straight at the target (2026-09-25: the leader kept on it
                    // after the plan, passed 14 yd from a Flameguard pack and died alone).
                    pilot.planFresh = false;
                    me->StopMoving();
                    pilot.moveCooldownMs = 3000;
                    me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ, MOVE_PATHFINDING);
                    return true;
                }
                float const toCamp = me->GetDistance(pilot.campX, pilot.campY, pilot.campZ);
                if (toCamp < pilot.bestDist - 3.0f || Moved(me, pilot))
                {
                    pilot.bestDist = std::min(pilot.bestDist, toCamp);
                    pilot.stuckMs = 0;
                }
                else if ((pilot.stuckMs += step) >= kStuckMs)
                {
                    Note(pilot, me, "could not reach the camp for %s (entry %u) - skipped", target->GetName(),
                        target->GetEntry());
                    pilot.skip[pilot.target] = WorldTimer::getMSTime() + 10 * 60 * 1000;
                    pilot.phase = Phase::Idle;
                    pilot.target.Clear();
                    pilot.planned = false;
                    return false;
                }
                if (pilot.moveCooldownMs > step)
                    pilot.moveCooldownMs -= step;
                else if (!me->IsMoving())
                {
                    pilot.moveCooldownMs = 3000;
                    me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ, MOVE_PATHFINDING);
                }
                return true;
            }
            // Unplanned, the walk stops at the first idle hostile 30 yd out, whoever it is: a wide
            // pack's nearest member, not its chosen one, decides where its reach begins (2026-11-03:
            // walking at one Flame Imp, the leader passed the others at 1 yd - 25 imps and a Surger).
            // The target itself counts, sight or not: the Pull phase closes in for sight alone
            // (2026-11-07: no sight into the imp pit until 0 yd, so the walk went on into 20 imps
            // with the raid stacked on the leader - 39 dead in 50 s).
            Creature* nearIdle = nullptr;
            {
                std::list<Creature*> around;
                MaNGOS::AnyUnitInObjectRangeCheck check(me, 30.0f);
                MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
                Cell::VisitGridObjects(me, searcher, 30.0f);
                float best = 31.0f;
                for (Creature* c : around)
                    if (Pullable(me, c, pilot) && std::fabs(c->GetPositionZ() - me->GetPositionZ()) < 15.0f && me->GetDistance(c) < best)
                    {
                        best = me->GetDistance(c);
                        nearIdle = c;
                    }
                if (nearIdle && nearIdle != target)
                {
                    Note(pilot, me, "%s (entry %u) stands %.0f yd from the walk to %s - it is the pull", nearIdle->GetName(),
                        nearIdle->GetEntry(), best, target->GetName());
                    pilot.target = nearIdle->GetObjectGuid();
                    pilot.targetEntry = nearIdle->GetEntry();
                    target = nearIdle;
                    dist = me->GetDistance(target);
                }
            }
            if ((dist <= kCampYards && me->IsWithinLOSInMap(target)) || nearIdle)
            {
                // Close enough: this is the camp. The raid gathers here and holds while the
                // leader goes in alone.
                me->StopMoving();
                pilot.campX = me->GetPositionX();
                pilot.campY = me->GetPositionY();
                pilot.campZ = me->GetPositionZ();
                // ...and the pull spot too: a shot from here, not a walk into the pack (2026-09-25:
                // unplanned pulls walked the leader into Firewalker packs, "aggroed at 0 yd").
                pilot.pullX = pilot.campX;
                pilot.pullY = pilot.campY;
                pilot.pullZ = pilot.campZ;
                pilot.pullShotMs = 0;
                pilot.assistWaitMs = 0;
                pilot.phase = Phase::Pull;
                pilot.gatherMs = 0;
                pilot.patrolWaitMs = 0;
                pilot.stuckMs = 0;
                pilot.bestDist = dist;
                return true;
            }
            pilot.approachMs += step;
            if (dist < pilot.bestDist - 3.0f || Moved(me, pilot))
            {
                pilot.bestDist = std::min(pilot.bestDist, dist);
                pilot.stuckMs = 0;
            }
            else if ((pilot.stuckMs += step) >= kStuckMs)
            {
                Note(pilot, me, "no progress toward %s (entry %u) for %u s - skipped", target->GetName(),
                    target->GetEntry(), kStuckMs / 1000);
                pilot.skip[pilot.target] = WorldTimer::getMSTime() + 10 * 60 * 1000;
                pilot.phase = Phase::Idle;
                pilot.target.Clear();
                return false;
            }
            if (pilot.moveCooldownMs > step)
                pilot.moveCooldownMs -= step;
            else if (!me->IsMoving())
            {
                pilot.moveCooldownMs = 3000;
                me->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(),
                    target->GetPositionZ(), MOVE_PATHFINDING);
            }
            return true;
        }

        if (pilot.phase == Phase::Pull)
        {
            if (!target || !target->IsAlive())
            {
                pilot.phase = Phase::Idle;
                pilot.target.Clear();
                return false;
            }
            if (target->IsInCombat())
            {
                ++pilot.pulls;
                float const aggroDist = me->GetDistance(target);
                // A pack that noticed the leader up close is fought right here: running back
                // with it at your heels means every hit lands from behind, where a tank can
                // neither block, parry nor dodge (2026-09-24: the leader died in 2 s that way).
                // The raid is within 40 yd - ranged and healers are already in reach.
                // ...but only a lone mob: a pack of elites on the leader alone kills it before the
                // other tanks and the heals arrive (2026-09-25: three Flameguard-pack elites, 9 s).
                uint32 onMe = 0;
                for (Unit* a : me->GetAttackers())
                    if (a && a->IsAlive())
                        ++onMe;
                if (CreatureGroup* g = target->GetCreatureGroup())
                {
                    uint32 pack = 0;
                    for (auto const& m : g->GetMembers())
                        if (Creature* c = me->GetMap()->GetCreature(m.first))
                            if (c->IsAlive())
                                ++pack;
                    if (Creature* lead = me->GetMap()->GetCreature(g->GetLeaderGuid()))
                        if (lead->IsAlive() && !g->ContainsGuid(lead->GetObjectGuid()))
                            ++pack;
                    onMe = std::max(onMe, pack);
                }
                // A kind that bursts around itself every few seconds is met where it stands, away from
                // the raid: brought back to the camp, it exploded on every healer (2026-09-25, Shazzrah).
                // ...the target or any of its pack, counting how many of each kind burst together
                uint32 rhythm = 0;
                {
                    std::map<uint32, uint32> kinds;
                    kinds[target->GetEntry()] = 1;
                    // its pack: linked in data, or (a summoned boss's adds) standing around it
                    std::list<Creature*> around;
                    MaNGOS::AnyUnitInObjectRangeCheck check(target, 25.0f);
                    MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
                    Cell::VisitGridObjects(target, searcher, 25.0f);
                    for (Creature* c : around)
                        if (c != target && c->IsAlive() && c->IsHostileTo(me) && SamePack(target, c))
                            ++kinds[c->GetEntry()];
                    for (auto const& k : kinds)
                        if (SuiRaidTelemetry::EntryPulseRadius(k.first) > 0.0f)
                            if (uint32 const iv = SuiRaidTelemetry::EntryBurstInterval(k.first))
                            {
                                uint32 const together = iv / std::max<uint32>(k.second, 1u);
                                if (!rhythm || together < rhythm)
                                    rhythm = together;
                            }
                }
                if (rhythm > 0 && rhythm < 10000 && pilot.timedBy.IsEmpty())   // a timed pull always goes to its camp, off the walk
                {
                    // A lone burster is met where it stands; a pack of them is held at the pull spot,
                    // where the other tanks (at the camp, 22 yd back) reach it as soon as it does -
                    // charging nine of Majordomo's adds alone killed the leader in 15 s, three times.
                    uint32 pack = 0;
                    for (Unit* a2 : me->GetAttackers())
                        if (a2 && a2->IsAlive())
                            ++pack;
                    {
                        std::list<Creature*> around;
                        MaNGOS::AnyUnitInObjectRangeCheck check(target, 25.0f);
                        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
                        Cell::VisitGridObjects(target, searcher, 25.0f);
                        uint32 near = 1;
                        for (Creature* c : around)
                            if (c != target && c->IsAlive() && c->IsHostileTo(me) && SamePack(target, c))
                                ++near;
                        pack = std::max(pack, near);
                    }
                    Note(pilot, me, "pull %u: %s (entry %u) bursts every %u s - %s", pilot.pulls, target->GetName(),
                        target->GetEntry(), rhythm / 1000, pack > 2 ? "held at the pull spot with the tanks" : "met where it stands");
                    pilot.phase = Phase::Fight;
                    pilot.fightMs = 0;
                    ++pilot.fights;
                    ai->AttackStart(target);
                    if (pack <= 2)
                        me->GetMotionMaster()->MoveChase(target);
                    else if (me->IsMoving())
                        me->StopMoving();
                    return false;
                }
                // A big creature alone that woke far out (it reaches far and sees far) is not led
                // back either: it would follow into whatever the camp stands beside (2026-09-25:
                // Onyxia, woken at 47 yd, was brought to a camp among her eggs).
                bool const bigAlone = target->GetCombatReach() >= 8.0f && onMe <= 1;
                // 45 yd: a pack that walks (Garr and his Firesworn loop 25 yd about their spot) reaches a
                // fight held 30 yd from it - 2026-11-03, a hound held at its ambush spot, Garr arrived in 6 s.
                if (pilot.timedBy.IsEmpty() && (aggroDist < 15.0f || bigAlone) && onMe <= 1 && IdleNeighbours(me, me, target, 45.0f) == 0 &&
                    (bigAlone || me->GetDistance(pilot.campX, pilot.campY, pilot.campZ) < 30.0f))
                {
                    Note(pilot, me, "pull %u: %s (entry %u) aggroed at %.0f yd - holding it here",
                        pilot.pulls, target->GetName(), target->GetEntry(), aggroDist);
                    pilot.phase = Phase::Fight;
                    pilot.fightMs = 0;
                    ++pilot.fights;
                    ai->AttackStart(target);
                    return false;
                }
                {
                    // where the fight is going: the camp, and the nearest idle hostile of another pack to it
                    float nearestOther = -1.0f;
                    std::list<Creature*> near;
                    MaNGOS::AnyUnitInObjectRangeCheck check(me, 120.0f);
                    MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
                    Cell::VisitGridObjects(me, searcher, 120.0f);
                    for (Creature* c : near)
                        if (c != target && c->IsAlive() && !c->IsInCombat() && c->IsHostileTo(me) && !SamePack(target, c) &&
                            c->GetCreatureType() != CREATURE_TYPE_CRITTER)
                        {
                            float const d = c->GetDistance(pilot.campX, pilot.campY, pilot.campZ);
                            if (nearestOther < 0.0f || d < nearestOther)
                                nearestOther = d;
                        }
                    Note(pilot, me, "pull %u: %s (entry %u) aggroed at %.0f yd - back to camp (%.0f, %.0f) %.0f yd from it%s, nearest other idle hostile %.0f yd from the camp",
                        pilot.pulls, target->GetName(), target->GetEntry(), aggroDist, pilot.campX, pilot.campY,
                        target->GetDistance(pilot.campX, pilot.campY, pilot.campZ), pilot.planned ? " (planned)" : " (unplanned)", nearestOther);
                    if (Creature* walker = pilot.timedBy.IsEmpty() ? nullptr : me->GetMap()->GetCreature(pilot.timedBy))
                        Note(pilot, me, "timed around %s: he is %.0f yd from the pack at (%.0f, %.0f), the camp is %.0f yd off his walk",
                            walker->GetName(), walker->GetDistance(target), walker->GetPositionX(), walker->GetPositionY(),
                            RouteGap(walker, pilot.campX, pilot.campY, pilot.campZ));
                }
                pilot.phase = Phase::Return;
                pilot.returnMs = 0;
                me->AttackStop();
                me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ,
                    MOVE_PATHFINDING | MOVE_RUN_MODE);
                return true;
            }
            if (pilot.gatherMs < kGatherCapMs)
            {
                pilot.gatherMs += step;
                uint32 away = 0;
                if (Group* group = me->GetGroup())
                    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                        if (Player* m = itr->getSource())
                            if (m != me && m->IsAlive() && m->IsInWorld() && m->GetMap() == me->GetMap() &&
                                m->GetDistance(pilot.campX, pilot.campY, pilot.campZ) > kGatheredYards &&
                                !(pilot.hasRear && (!pilot.timedBy.IsEmpty() || pilot.lured) &&
                                  m->GetDistance(pilot.rearX, pilot.rearY, pilot.rearZ) <= 8.0f))
                                ++away;
                if (away > 0)
                {
                    // Still catching up (looting, a long walk): no pull without the healers
                    // (2026-09-24: pulled right after Gehennas with the healers 50-90 yd away
                    // looting him). Stragglers are summoned after 15 s (owner-approved setup).
                    if (pilot.gatherMs >= (pilot.planned ? 1000u : 15000u) && pilot.gatherMs % 5000 < step)
                        Regroup(me);
                    return true;
                }
                pilot.gatherMs = kGatherCapMs;
            }
            if (pilot.ambushTarget == pilot.target && !pilot.ambushTarget.IsEmpty())
            {
                uint32 const shot = RangedPullSpell(me);
                float const dist = me->GetDistance(target);
                float const toSpot = me->GetDistance(pilot.pullX, pilot.pullY, pilot.pullZ);
                float const atPoint = target->GetDistance(pilot.ambushX, pilot.ambushY, pilot.ambushZ);
                if (pilot.ambushWaitMs > 0 && pilot.phase == Phase::Pull && pilot.gatherMs == kGatherCapMs &&
                    pilot.pullShotMs == 0)
                {
                    pilot.ambushWaitMs = 0;   // the approach's own wait does not count here
                    pilot.pullShotMs = 1;
                }
                pilot.ambushWaitMs += step;
                if (pilot.ambushWaitMs == step)
                    Note(pilot, me, "waiting for %s to reach the ambush point", target->GetName());
                if (!shot || pilot.ambushWaitMs > 120000)   // 2026-11-03: a 7 min wait for a hound, then abandoned
                {
                    // Clear of other packs where it walks now: pulled there, not left to roam into the
                    // next fight (2026-11-03: an abandoned Ancient Core Hound joined Garr from the first
                    // second; the raid spent the fight dodging its cone).
                    if (shot && IdleNeighbours(me, target, target, 40.0f) == 0)
                    {
                        Note(pilot, me, "ambush of %s timed out - pulled where it walks", target->GetName());
                        pilot.noAmbush = pilot.target;
                        pilot.ambushTarget.Clear();
                        pilot.planned = false;
                        pilot.phase = Phase::Approach;
                        pilot.stuckMs = 0;
                        pilot.bestDist = me->GetDistance(target);
                        return true;
                    }
                    Note(pilot, me, "ambush of %s abandoned", target->GetName());
                    pilot.skip[pilot.target] = WorldTimer::getMSTime() + 5 * 60 * 1000;
                    pilot.ambushTarget.Clear();
                    pilot.phase = Phase::Idle;
                    pilot.target.Clear();
                    pilot.planned = false;
                    return false;
                }
                // It walks up to the leader on its own: give way toward the camp, never be found.
                if (dist < 16.0f)
                {
                    if (!me->IsMoving())
                        me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ,
                            MOVE_PATHFINDING | MOVE_RUN_MODE);
                    return true;
                }
                if (toSpot > 3.0f && dist > 22.0f)
                {
                    if (!me->IsMoving())
                        me->GetMotionMaster()->MovePoint(0, pilot.pullX, pilot.pullY, pilot.pullZ, MOVE_PATHFINDING);
                    return true;
                }
                bool const inPlace = atPoint <= 12.0f || pilot.ambushWaitMs > 300000;
                // Creatures only call for help 10 yd around them (CreatureFamilyAssistanceRadius): a
                // patrol shot 18 yd clear of the next pack comes alone. 32 yd never fired on a hound
                // whose whole walk passes 20-30 yd from Garr's pack (2026-11-03, five abandoned ambushes).
                // A pack's loop counts 12 yd out (plus a member's 10 yd of slack): 18 + 10 round Garr's short
                // loop, 26 yd from the point, held the shot at an Ancient Core Hound for 14 min of
                // abandoned 120 s waits (2026-11-07; his Firesworn stood 22-38 yd from it).
                if (inPlace && dist <= 30.0f && me->IsWithinLOSInMap(target) &&
                    IdleNeighbours(me, target, target, 18.0f, 12.0f) == 0)
                {
                    if (me->IsMoving())
                        me->StopMoving();
                    if (!me->IsNonMeleeSpellCasted(false))
                    {
                        me->SetFacingToObject(target);
                        SpellCastResult const res = me->CastSpell(target, shot, false);
                        if (res != SPELL_CAST_OK && res != SPELL_FAILED_NOT_READY && pilot.ambushWaitMs % 10000 < step)
                            Note(pilot, me, "ambush shot at %s failed (result %u)", target->GetName(), uint32(res));
                    }
                }
                return true;
            }
            // Timed around a patrolling boss: pulled only while he is well away from the pack, the camp
            // and the leader (75 yd: his walk back takes longer than the fight).
            if (!pilot.timedBy.IsEmpty() && pilot.timedWaitMs < 300000)
                if (Creature* walker = me->GetMap()->GetCreature(pilot.timedBy))
                    if (walker->IsAlive() && !walker->IsInCombat())
                    {
                        float const dT = walker->GetDistance(target), dM = me->GetDistance(walker),
                            dC = walker->GetDistance(pilot.campX, pilot.campY, pilot.campZ);
                        float const near = std::min({dT, dM, dC});
                        // ...and walking away: 75 yd and coming back is 20 s from the fight (2026-11-03,
                        // Lucifron arrived 29 s after a pull made at 75 yd). Away from the pack, the camp AND
                        // the leader, two ticks in a row: the nearest of the three alone flips from one to
                        // the other as he walks between them (2026-11-04: "receding" from the pack while he
                        // walked at the camp sent the leader out; Geddon met him on the way).
                        auto away = [](float d, float last) { return d >= 170.0f || d > last + 0.5f; };
                        bool const receding = away(dT, pilot.timedLastT) && away(dC, pilot.timedLastC) &&
                            away(dM, pilot.timedLastM);
                        bool const closing = dM < pilot.timedLastM - 0.5f;
                        pilot.timedLastNear = near;
                        pilot.timedLastT = dT;
                        pilot.timedLastC = dC;
                        pilot.timedLastM = dM;
                        // 2026-11-04: pulled at 110 yd while Geddon walked back, he arrived at +32 s.
                        bool const clear = near >= 170.0f || (near >= 75.0f && receding);
                        pilot.timedClearTicks = clear ? pilot.timedClearTicks + 1 : 0;
                        // Out of the camp the pull is under way: it goes on unless he comes at the leader, and
                        // then it is back to the camp - never standing still at a pull spot beside his walk
                        // (2026-11-04: the wait froze the leader 13 yd from Geddon's walk; he took him).
                        bool const out = me->GetDistance(pilot.campX, pilot.campY, pilot.campZ) > 6.0f;
                        if (out && dM < 60.0f && closing)
                        {
                            if (pilot.lastEvent.find("back to the camp before") == std::string::npos)
                                Note(pilot, me, "%s comes at the pull spot (%.0f yd) - back to the camp before he arrives",
                                    walker->GetName(), dM);
                            pilot.timedClearTicks = 0;
                            pilot.timedWaitMs += step;
                            // Turn round now: the walk out is still a move, and "not moving" kept it
                            // (2026-11-05: the note said "back to the camp", the leader walked on 14 yd
                            // to the Flameguard pull spot and Geddon took him there - wipe at 13 %).
                            bool toCamp = false;
                            if (me->IsMoving() && !me->movespline->Finalized())
                            {
                                G3D::Vector3 const dest = me->movespline->FinalDestination();
                                float const dx = dest.x - pilot.campX, dy = dest.y - pilot.campY;
                                toCamp = dx * dx + dy * dy < 4.0f * 4.0f;
                            }
                            if (!toCamp)
                                me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ,
                                    MOVE_PATHFINDING | MOVE_RUN_MODE);
                            return true;
                        }
                        if (!out && pilot.timedClearTicks < 2)
                        {
                            if (pilot.timedWaitMs % 10000 < step)
                                Note(pilot, me, "waiting for %s to walk away from %s (pack %.0f, camp %.0f, me %.0f yd; camp at %.0f, %.0f)",
                                    walker->GetName(), target->GetName(), dT, dC, dM, pilot.campX, pilot.campY);
                            pilot.timedWaitMs += step;
                            if (me->IsMoving())
                                me->StopMoving();
                            return true;
                        }
                    }
            // A patrol walking past (the target itself, or a foreign mob moving near it) is
            // waited out: pulled together they are two packs (2026-09-24: a Lava Surger 11 yd
            // from a Flame Imp pack joined and killed the melee).
            bool patrolNear = false;
            if (pilot.patrolWaitMs < 45000)
            {
                std::list<Creature*> near;
                MaNGOS::AnyUnitInObjectRangeCheck check(target, 35.0f);
                MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
                Cell::VisitGridObjects(target, searcher, 35.0f);
                for (Creature* c : near)
                    if (c != target && c->IsAlive() && !c->IsInCombat() && c->IsMoving() && c->IsHostileTo(me) &&
                        !(c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup()))
                        patrolNear = true;
            }
            if ((patrolNear || (target->IsMoving() && IdleNeighbours(me, target, target, 30.0f) > 0)) &&
                pilot.patrolWaitMs < 45000)
            {
                pilot.patrolWaitMs += step;
                if (me->IsMoving())
                    me->StopMoving();
                return true;   // let the patrol walk clear of the pack it is passing
            }
            float const dist = me->GetDistance(target);
            if (dist <= 7.0f)
            {
                me->Attack(target, true);   // it did not notice us: a hit starts the fight
                return true;
            }
            if (pilot.pullShotMs < 6000)
            {
                uint32 const shot = RangedPullSpell(me);
                float const toSpot = me->GetDistance(pilot.pullX, pilot.pullY, pilot.pullZ);
                if (shot && toSpot <= 3.0f && !(dist <= 29.0f && me->IsWithinLOSInMap(target)) && Patrols(target))
                {
                    // A walker comes back along its route: it is waited for at the spot, never chased
                    // (2026-11-05: the leader followed an Ancient Core Hound 65 yd off its pull spot
                    // into a Molten Destroyer and Giant - two Firelords and a second hound joined, all dead).
                    if (me->IsMoving())
                        me->StopMoving();
                    if ((pilot.stuckMs += step) >= 90000)
                    {
                        Note(pilot, me, "%s did not come back in reach of the pull spot - skipped", target->GetName());
                        pilot.skip[pilot.target] = WorldTimer::getMSTime() + 5 * 60 * 1000;
                        pilot.phase = Phase::Idle;
                        pilot.target.Clear();
                        pilot.planned = false;
                        pilot.stuckMs = 0;
                        return false;
                    }
                    return true;
                }
                if (shot && toSpot <= 3.0f && !(dist <= 29.0f && me->IsWithinLOSInMap(target)))
                {
                    // At the spot but the pack wandered out of sight or range: close in until it
                    // is in sight, then shoot (2026-09-24: walking all the way in dragged the fight
                    // 100 yd from the raid).
                    if (!me->IsMoving())
                        me->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(),
                            target->GetPositionZ(), MOVE_PATHFINDING);
                    if ((pilot.stuckMs += step) >= kStuckMs)
                    {
                        pilot.pullShotMs = 6000;
                        pilot.bestDist = 1e9f;
                        pilot.stuckMs = 0;
                    }
                    return true;
                }
                // A neighbour wandering inside its call for help comes with it: shot once it has
                // wandered out (60 s at most - one that never does is part of the pack, SamePack).
                if (shot && dist <= 29.0f && pilot.pullShotMs == 0 && pilot.assistWaitMs < 60000)
                    if (Creature* nb = AssistNeighbour(me, target, 1.5f))
                    {
                        if (me->IsMoving())
                            me->StopMoving();
                        if (pilot.assistWaitMs == 0)
                            Note(pilot, me, "%s (entry %u) stands %.0f yd from %s, inside its call for help - waiting for it to wander off",
                                nb->GetName(), nb->GetEntry(), target->GetDistance(nb), target->GetName());
                        pilot.assistWaitMs += step;
                        if (pilot.assistWaitMs >= 60000)
                            Note(pilot, me, "%s did not wander off %s in 60 s - pulled anyway", nb->GetName(), target->GetName());
                        return true;
                    }
                if (shot && dist <= 29.0f && me->IsWithinLOSInMap(target))
                {
                    if (me->IsMoving())
                        me->StopMoving();
                    if (!me->IsNonMeleeSpellCasted(false))
                    {
                        me->SetFacingToObject(target);
                        SpellCastResult res = me->CastSpell(target, shot, false);
                        if (res == SPELL_FAILED_NOT_READY)
                            res = SPELL_CAST_OK;   // the shot is still in flight: wait for it to land
                        if (res != SPELL_CAST_OK)
                            Note(pilot, me, "ranged pull of %s failed (result %u) - walking in", target->GetName(),
                                uint32(res));
                        pilot.pullShotMs = res == SPELL_CAST_OK ? pilot.pullShotMs + step : 6000;
                        if (res != SPELL_CAST_OK)
                            pilot.bestDist = 1e9f;
                    }
                    else
                        pilot.pullShotMs += step;
                    return true;
                }
                if (shot)
                {
                    if (toSpot < pilot.bestDist - 2.0f)
                    {
                        pilot.bestDist = toSpot;
                        pilot.stuckMs = 0;
                    }
                    else if ((pilot.stuckMs += step) >= kStuckMs)
                    {
                        pilot.pullShotMs = 6000;   // cannot reach the spot: walk in instead
                        pilot.bestDist = 1e9f;
                        pilot.stuckMs = 0;
                    }
                    if (!me->IsMoving())
                    {
                        if (pilot.lastEvent.rfind("out to the pull spot", 0) != 0)
                            Note(pilot, me, "out to the pull spot (%.0f, %.0f) for %s", pilot.pullX, pilot.pullY, target->GetName());
                        // a timed pull runs out: the patrol is back in 15 s (Geddon walked to at 14 s a pull spot 38 yd off)
                        me->GetMotionMaster()->MovePoint(0, pilot.pullX, pilot.pullY, pilot.pullZ,
                            pilot.timedBy.IsEmpty() ? MOVE_PATHFINDING : (MOVE_PATHFINDING | MOVE_RUN_MODE));
                    }
                    return true;
                }
            }
            if (dist < pilot.bestDist - 2.0f)
            {
                pilot.bestDist = dist;
                pilot.stuckMs = 0;
            }
            else if ((pilot.stuckMs += step) >= kStuckMs)
            {
                Note(pilot, me, "could not reach %s (entry %u) from the camp - skipped", target->GetName(),
                    target->GetEntry());
                pilot.skip[pilot.target] = WorldTimer::getMSTime() + 10 * 60 * 1000;
                pilot.phase = Phase::Idle;
                pilot.target.Clear();
                return false;
            }
            if (!me->IsMoving())
                me->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(),
                    target->GetPositionZ(), MOVE_PATHFINDING);
            return true;
        }

        // Return: run to the camp; the fight starts there.
        pilot.returnMs += step;
        float const toCamp = me->GetDistance(pilot.campX, pilot.campY, pilot.campZ);
        if (toCamp <= 4.0f || pilot.returnMs >= 15000)
        {
            pilot.retreating = false;
            pilot.phase = Phase::Fight;
            pilot.fightMs = 0;
            ++pilot.fights;
            Unit* first = me->GetAttackerForHelper();
            if (!first && target && target->IsAlive())
                first = target;
            if (first)
                ai->AttackStart(first);
            return false;
        }
        if (!me->IsMoving())
            me->GetMotionMaster()->MovePoint(0, pilot.campX, pilot.campY, pilot.campZ,
                MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }
}

bool SuiAutopilot::HoldForPull(Player const* member)
{
    if (!member || !member->GetGroup())
        return false;
    // The tanks walk out with the puller: a pack that reaches the pull spot meets all of them at
    // once instead of the puller alone (2026-09-25: nine of Majordomo's adds killed the puller in
    // 9-25 s while the other tanks ran up from the camp).
    bool tank = false;
    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(const_cast<Player*>(member)->AI()))
        tank = ai->GetCombatActiveRole() == ROLE_TANK;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (leader && leader != member && leader->GetGroup() == member->GetGroup())
        {
            // ...except a pull timed around a patrolling boss: the pull spot is beside his walk and the
            // pack must come to the camp off it (2026-11-04: two tanks met a pack at the pull spot 13 yd
            // from Baron Geddon's walk and held it there; he walked by at +14 s and joined).
            if (tank && pair.second.timedBy.IsEmpty() && !pair.second.lured)
                return false;
            // ...nor while the pack is already on the leader near the camp (see CampHold)
            if (tank && !leader->GetAttackers().empty() &&
                leader->GetDistance(pair.second.campX, pair.second.campY, pair.second.campZ) < 25.0f)
                return false;
            // (The raid used to hold while the leader walked to a planned camp alone: the leader
            // then died alone whenever something woke on the way - 2026-09-25, four times. The
            // follow arc is 4-6 yd behind it; the raid walks with it.)
            return (pair.second.phase == Phase::Pull && pair.second.gatherMs >= kGatherCapMs) ||
                (pair.second.phase == Phase::Return && !pair.second.retreating);
        }
    }
    return false;
}

bool SuiAutopilot::CampHold(Player const* member, float& x, float& y, float& z)
{
    if (!member || !member->GetGroup() || !member->IsAlive())
        return false;
    bool tank = false;
    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(const_cast<Player*>(member)->AI()))
        tank = ai->GetCombatActiveRole() == ROLE_TANK;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (!leader || leader == member || leader->GetGroup() != member->GetGroup())
            continue;
        Pilot const& p = pair.second;
        if (p.paused || p.retreating || (p.timedBy.IsEmpty() && !p.lured) || leader->GetMap() != member->GetMap())
            return false;
        // A tank never holds while the pack is on the leader near the camp: a rooted leader does not
        // reach it (2026-11-05: Fire Blossom held him 13 yd short with three elites on him; the other
        // tanks waited at the camp for the Return to end and he died at 12 s - all but two dead).
        if (tank && !leader->GetAttackers().empty() && leader->GetDistance(p.campX, p.campY, p.campZ) < 25.0f)
            return false;
        // ...and the first 20 s of the fight while the member's target has not come to the camp yet: a
        // Firewalker stands rooted casting its blossoms 30 yd out, beside the walk (2026-11-04: the tanks
        // ran out to it there, Geddon came by at +26 s).
        bool const early = p.phase == Phase::Fight && p.fightMs < 20000 && member->GetVictim() &&
            member->GetVictim()->GetDistance(p.campX, p.campY, p.campZ) > 15.0f;
        if (p.phase != Phase::Return && !early)
            return false;
        float const bx = (!tank && p.hasRear) ? p.rearX : p.campX, by = (!tank && p.hasRear) ? p.rearY : p.campY,
            bz = (!tank && p.hasRear) ? p.rearZ : p.campZ;
        if (member->GetDistance(bx, by, bz) > 60.0f)
            return false;   // somewhere else entirely: the ordinary fight
        float const ring = 1.5f + float(member->GetGUIDLow() % 3) * 1.5f;
        float const angle = float(member->GetGUIDLow() % 16) * float(M_PI) / 8.0f;
        x = bx + ring * std::cos(angle);
        y = by + ring * std::sin(angle);
        z = bz;
        member->UpdateAllowedPositionZ(x, y, z);
        return true;
    }
    return false;
}

bool SuiAutopilot::RearSpot(Player const* member, float& x, float& y, float& z)
{
    if (!member || !member->GetGroup() || member->IsInCombat())
        return false;
    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(const_cast<Player*>(member)->AI()))
        if (ai->GetCombatActiveRole() == ROLE_TANK)
            return false;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (!leader || leader == member || leader->GetGroup() != member->GetGroup())
            continue;
        Pilot const& p = pair.second;
        if (p.paused || !p.hasRear || (p.timedBy.IsEmpty() && !p.lured) || !p.planned || leader->GetMap() != member->GetMap())
            return false;
        bool const atCamp = p.phase == Phase::Pull || p.phase == Phase::Return ||
            (p.phase == Phase::Approach && leader->GetDistance(p.campX, p.campY, p.campZ) < 12.0f);
        if (!atCamp || member->GetDistance(p.rearX, p.rearY, p.rearZ) > 120.0f)
            return false;
        // a small fixed ring per member round the spot, as on the leader
        float const ring = 1.5f + float(member->GetGUIDLow() % 3) * 1.5f;
        float const angle = float(member->GetGUIDLow() % 16) * float(M_PI) / 8.0f;
        x = p.rearX + ring * std::cos(angle);
        y = p.rearY + ring * std::sin(angle);
        z = p.rearZ;
        member->UpdateAllowedPositionZ(x, y, z);
        return true;
    }
    return false;
}

bool SuiAutopilot::FightAnchor(Player const* member, float& x, float& y, float& z)
{
    if (!member || !member->GetGroup())
        return false;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (!leader || leader->GetGroup() != member->GetGroup() || pair.second.paused)
            continue;
        if (pair.second.phase != Phase::Fight && pair.second.phase != Phase::Return)
            return false;
        x = pair.second.campX;
        y = pair.second.campY;
        z = pair.second.campZ;
        return member->GetMap() == leader->GetMap() && member->GetDistance(x, y, z) < 120.0f;
    }
    return false;
}

int SuiAutopilot::StackOnLeader(Player const* member, float& x, float& y, float& z)
{
    if (!member || !member->GetGroup() || member->IsInCombat())
        return 0;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (!leader || leader == member || leader->GetGroup() != member->GetGroup() || pair.second.paused)
            continue;
        Phase const phase = pair.second.phase;
        bool const waiting = phase == Phase::Idle || phase == Phase::Rest ||
            (phase == Phase::Approach && !pair.second.planned);
        if (!waiting || leader->IsMoving() || leader->IsInCombat() || leader->GetMap() != member->GetMap() ||
            !member->IsWithinDistInMap(leader, 45.0f))
            return 0;
        // A fixed slot per member on a small ring (2026-09-24: a formation 20-30 yd wide woke the
        // packs beside a clear waiting spot - a Firelord and Geddon joined).
        float const ring = 3.0f + float(member->GetGUIDLow() % 3) * 1.5f;
        for (uint32 k = 0; k < 16; ++k)
        {
            float const angle = float((member->GetGUIDLow() + k * 5) % 16) * float(M_PI) / 8.0f;
            x = leader->GetPositionX() + ring * std::cos(angle);
            y = leader->GetPositionY() + ring * std::sin(angle);
            z = leader->GetPositionZ();
            member->UpdateAllowedPositionZ(x, y, z);
            if (member->GetTerrain()->getLiquidStatus(x, y, z + 0.01f, MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME, nullptr) &
                (LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER | LIQUID_MAP_WATER_WALK))
                continue;   // not a slot in lava
            if (member->GetDistance2d(x, y) <= 2.5f)
                return 1;
            return 2;
        }
        return 0;
    }
    return 0;
}

namespace
{
    // The fight is over: its record is written, and a boss fight or a wipe stops the autopilot for
    // review before the next pull (owner 2026-11-03: be involved, review per boss attempt).
    void EndFight(Player* me, Pilot& pilot, Readiness const& ready)
    {
        uint32 const flags = SuiRaidTelemetry::FlushFight(me, pilot.fights, pilot.fightMs / 1000, pilot.targetEntry,
            pilot.leaderDied);
        Note(pilot, me, "fight %u over after %u s (members %u, dead %u)%s", pilot.fights,
            pilot.fightMs / 1000, ready.members, ready.dead, pilot.leaderDied ? " - leader died" : "");
        pilot.phase = Phase::Rest;
        pilot.restMs = 0;
        pilot.target.Clear();
        pilot.leaderDied = false;
        if (pilot.review && (flags & (SuiRaidTelemetry::FIGHT_BOSS | SuiRaidTelemetry::FIGHT_WIPE)))
        {
            pilot.paused = true;
            Note(pilot, me, "PAUSED for review after fight %u (%s%s%s) - .autopilot resume %s", pilot.fights,
                (flags & SuiRaidTelemetry::FIGHT_BOSS) ? "boss " : "",
                (flags & SuiRaidTelemetry::FIGHT_BOSS_KILLED) ? "killed " : "",
                (flags & SuiRaidTelemetry::FIGHT_WIPE) ? "wipe" : "", me->GetName());
        }
    }

    // A fight the leader cannot drive (dead, or paused): it is still sampled and closed.
    void WatchFight(Player* me, Pilot& pilot)
    {
        pilot.fightMs += AIBOT_UPDATE_INTERVAL;
        if (pilot.fightMs % 1000 < AIBOT_UPDATE_INTERVAL)
            SuiRaidTelemetry::Sample(me);
        Readiness const ready = Assess(me);
        if (!ready.inCombat && !(me->IsAlive() && me->IsInCombat() && (me->GetVictim() || !me->GetAttackers().empty())))
            EndFight(me, pilot, ready);
    }
}

bool SuiAutopilot::TickLeader(AiBotAI* ai)
{
    Player* me = ai ? ai->GetBotPlayer() : nullptr;
    if (!me || !me->IsInWorld())
        return false;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    auto it = gPilots.find(me->GetObjectGuid());
    if (it == gPilots.end())
        return false;
    Pilot& pilot = it->second;
    uint32 const step = AIBOT_UPDATE_INTERVAL;
    if (!me->IsAlive() || pilot.paused)
    {
        if (pilot.phase == Phase::Fight && me->GetMap()->IsDungeon())
            WatchFight(me, pilot);
        return false;
    }

    SuiRaidTelemetry::Track(me->GetGroup());

    // Dungeons and raids only: out in the world (a leader revived at an instance portal) the
    // autopilot waits instead of picking fights with the neighbourhood.
    if (!me->GetMap()->IsDungeon())
    {
        // The raid is inside and the leader is not (revived at the portal after a restart): it
        // rejoins them - a setup move, like a summon.
        if (Group* group = me->GetGroup())
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                if (Player* m = itr->getSource())
                    if (m != me && m->IsInWorld() && m->IsAlive() && m->GetMap()->IsDungeon() && !me->IsBeingTeleported())
                    {
                        Note(pilot, me, "rejoining the raid inside %s", m->GetMap()->GetMapName());
                        me->TeleportTo(m->GetMapId(), m->GetPositionX(), m->GetPositionY(), m->GetPositionZ(),
                            m->GetOrientation());
                        return false;
                    }
        if (pilot.lastEvent != "outside an instance - waiting")
            Note(pilot, me, "outside an instance - waiting");
        pilot.phase = Phase::Idle;
        return false;
    }

    if ((pilot.safeCheckMs += step) >= 1000)
    {
        pilot.safeCheckMs = 0;
        // a room away too (2026-09-25: a respawned pack 150+ yd off walked into Geddon), and during a
        // fight as well (2026-11-05: a cleared Flame Imp pack - 5 min respawn - came back 25 yd from a
        // Core Hound fight; a healer stepping off a hound woke it, 11 dead)
        ExecuteRespawns(me, pilot, 300.0f);
        bool nearRoute = false;
        {
            std::list<Creature*> walkers;
            MaNGOS::AnyUnitInObjectRangeCheck check(me, 200.0f);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(walkers, check);
            Cell::VisitGridObjects(me, searcher, 200.0f);
            for (Creature* w : walkers)
                if (!nearRoute && w->IsAlive() && !w->IsInCombat() && w->IsHostileTo(me))
                    ForEachRoutePoint(w, [&](float x, float y, float z)
                    {
                        if (std::fabs(z - me->GetPositionZ()) < 20.0f &&
                            me->GetDistance2d(x, y) < (w->IsWorldBoss() ? kBossWalkGap + 3.0f : 35.0f))
                            nearRoute = true;
                    });
        }
        if (!me->IsInCombat() && !nearRoute && IdleNeighbours(me, me, nullptr, 50.0f) == 0)
        {
            pilot.hasSafe = true;
            pilot.safeX = me->GetPositionX();
            pilot.safeY = me->GetPositionY();
            pilot.safeZ = me->GetPositionZ();
        }
    }

    // The pull runs through combat: the pack aggroes on the leader during Approach and
    // chases it back to the camp during Return.
    // A fight the raid is already in (a pack that came to us, Lava Spawns still up) comes first:
    // no new pull walks out of it (2026-09-24: the leader shot a Firelord while the raid was
    // still fighting Lava Spawns, and three more packs joined).
    if (pilot.phase == Phase::Approach || pilot.phase == Phase::Pull)
    {
        Creature* target = me->GetMap()->GetCreature(pilot.target);
        if (!(target && target->IsInCombat()))
            if (Group* group = me->GetGroup())
                for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                    if (Player* m = itr->getSource())
                        if (m->IsInWorld() && m->IsAlive() && m->GetMap() == me->GetMap() && m->IsInCombat() &&
                            (m->GetVictim() || !m->GetAttackers().empty()))
                        {
                            Note(pilot, me, "the raid is fighting - pull of %s on hold",
                                target ? target->GetName() : "?");
                            if (me->IsMoving())
                                me->StopMoving();
                            pilot.phase = Phase::Fight;
                            pilot.fightMs = 0;
                            ++pilot.fights;
                            pilot.target.Clear();
                            return false;
                        }
    }
    if (pilot.phase == Phase::Approach || pilot.phase == Phase::Pull || pilot.phase == Phase::Return)
        return DrivePull(ai, me, pilot);

    Readiness const ready = Assess(me);
    if (ready.inCombat || (me->IsInCombat() && (me->GetVictim() || !me->GetAttackers().empty())))
    {
        if (pilot.phase != Phase::Fight)
        {
            pilot.phase = Phase::Fight;
            pilot.fightMs = 0;
            ++pilot.fights;
            pilot.campX = me->GetPositionX();   // the fight's anchor: where it found us
            pilot.campY = me->GetPositionY();
            pilot.campZ = me->GetPositionZ();
            // A fight the leader did not pull: name who is fighting what.
            if (Group* group = me->GetGroup())
                for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                    if (Player* m = itr->getSource())
                        if (m->IsInWorld() && m->IsAlive() && m->IsInCombat() &&
                            (m->GetVictim() || !m->GetAttackers().empty()))
                        {
                            Unit* v = m->GetVictim();
                            Unit* a = m->GetAttackerForHelper();
                            Pet* pet = m->GetPet();
                            Note(pilot, me, "fight %u started unpulled: %s in combat, victim %s, attacker %s, pet victim %s, %.0f yd from leader",
                                pilot.fights, m->GetName(), v ? v->GetName() : "-", a ? a->GetName() : "-",
                                pet && pet->GetVictim() ? pet->GetVictim()->GetName() : "-", m->GetDistance(me));
                            break;
                        }
        }
        pilot.fightMs += step;
        if (pilot.fightMs % 1000 < step)
            SuiRaidTelemetry::Sample(me);
        return false;   // the doctrine fights; the autopilot only drives between fights
    }

    if (pilot.phase == Phase::Fight)
    {
        EndFight(me, pilot, ready);
        if (pilot.paused)
            return false;
    }

    if (pilot.phase == Phase::Idle || pilot.phase == Phase::Rest)
    {
        MoveToSafe(me, pilot);
        if (Creature* walker = PatrolBossWalkNear(me, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), kBossWalkGap))
            if (me->GetDistance(walker) < 120.0f && MoveToSafe(me, pilot))
            {
                if (pilot.lastEvent.find("on the walk of") == std::string::npos)
                    Note(pilot, me, "on the walk of %s (%.0f yd from it, he is %.0f yd away) - off it to the clear spot first",
                        walker->GetName(), RouteGap(walker, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ()),
                        me->GetDistance(walker));
                return false;
            }
        if (pilot.phase != Phase::Rest && WorldTimer::getMSTime() >= pilot.prepNextMs)
        {
            pilot.prepNextMs = WorldTimer::getMSTime() + 20000;
            Prepare(me);
            Regroup(me);   // members left on another map come back too
        }
        if (!ready.Ready() && pilot.restMs < kRestCapMs)
        {
            pilot.phase = Phase::Rest;
            pilot.restMs += step;
            if (pilot.restMs % 5000 < step)
                Regroup(me);
            return false;   // eat, drink, rebuff and gather under the ordinary AI
        }
        if (!ready.Ready() && pilot.lastEvent.compare(0, 16, "rest cap reached") != 0 &&
            pilot.lastEvent.compare(0, 16, "exploring toward") != 0)
            Note(pilot, me, "rest cap reached (dead %u, far %u, low %u, unsettled %u) - moving on",
                ready.dead, ready.far, ready.low, ready.unsettled);

        if (pilot.searchBackoffMs > step)
        {
            pilot.searchBackoffMs -= step;
            return false;
        }
        Creature* next = PickNext(me, pilot);
        if (next)
            pilot.offMeshTries = 0;
        if (!next && pilot.offMesh)
        {
            if (pilot.lastEvent != "no path from here - stepping back to the last clear spot")
                Note(pilot, me, "no path from here - stepping back to the last clear spot");
            // Setup move (owner-approved): the last clear spot, else a short hop, else the entrance.
            ++pilot.offMeshTries;
            if (pilot.hasSafe && pilot.offMeshTries <= 2)
                me->NearTeleportTo(pilot.safeX, pilot.safeY, pilot.safeZ, me->GetOrientation());
            else if (pilot.offMeshTries <= 5)
            {
                float const ang = pilot.offMeshTries * 1.3f;
                float x = me->GetPositionX() + 6.0f * std::cos(ang), y = me->GetPositionY() + 6.0f * std::sin(ang);
                float z = me->GetPositionZ();
                me->UpdateAllowedPositionZ(x, y, z);
                me->NearTeleportTo(x, y, z, me->GetOrientation());
            }
            else if (AreaTriggerTeleport const* entrance = sObjectMgr.GetMapEntranceTrigger(me->GetMapId()))
            {
                if (entrance->destination.mapId == me->GetMapId())
                    me->NearTeleportTo(entrance->destination.x, entrance->destination.y, entrance->destination.z,
                        entrance->destination.o);
                pilot.offMeshTries = 0;
            }
            pilot.searchBackoffMs = 3000;
            return false;
        }
        float ex = 0.0f, ey = 0.0f, ez = 0.0f;
        uint32 exploreEntry = 0, exploreGuid = 0;
        if (next)
            pilot.exploreStallMs = 0;
        if (!next && UseScriptedObjects(me, pilot))
        {
            pilot.searchBackoffMs = 1500;
            return false;
        }
        if (!next && ExploreTarget(me, pilot, ex, ey, ez, exploreEntry, exploreGuid))
        {
            CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(exploreEntry);
            std::string const what = std::string("exploring toward ") + (info ? info->name : "?");
            if (pilot.lastEvent.compare(0, what.size(), what) != 0)
                Note(pilot, me, "%s (%.0f yd)", what.c_str(), me->GetDistance(ex, ey, ez));
            // Only far enough to bring it into view: stop 120 yd short and look again every second,
            // so the next pull is planned from outside its room (2026-09-24: walking all the way to
            // the spawn found a Lava Reaver at 44 yd, in the middle of Baron Geddon's room).
            float const d = me->GetDistance(ex, ey, ez);
            if (d > 125.0f)
            {
                PathInfo path(me);
                path.calculate(ex, ey, ez, false);
                PointsArray const& pts = path.getPath();
                float sx = ex, sy = ey, sz = ez;
                for (size_t k = 1; k < pts.size(); ++k)
                    if ((pts[k].x - ex) * (pts[k].x - ex) + (pts[k].y - ey) * (pts[k].y - ey) <= 120.0f * 120.0f)
                    {
                        sx = pts[k].x;
                        sy = pts[k].y;
                        sz = pts[k].z;
                        break;
                    }
                if (!me->IsMoving())
                    me->GetMotionMaster()->MovePoint(0, sx, sy, sz, MOVE_PATHFINDING);
            }
            else
            {
                if (me->IsMoving())
                    me->StopMoving();
                // In view and still nothing to pull there: that spawn is not what it looked like
                // on paper (an event creature, a friendly) - forget it for a while.
                if ((pilot.exploreStallMs += 1000) >= 15000)
                {
                    pilot.exploreSkip[exploreGuid] = WorldTimer::getMSTime() + 10 * 60 * 1000;
                    pilot.exploreStallMs = 0;
                }
            }
            pilot.searchBackoffMs = 1000;
            return false;
        }
        if (!next)
        {
            pilot.searchBackoffMs = kEmptySearchBackoffMs;
            if (pilot.lastEvent != "nothing left to pull in range")
                Note(pilot, me, "nothing left to pull in range");
            pilot.phase = Phase::Idle;
            pilot.restMs = 0;
            return false;
        }
        pilot.target = next->GetObjectGuid();
        pilot.targetEntry = next->GetEntry();
        pilot.timedWaitMs = 0;
        pilot.timedClearTicks = 0;
        pilot.lured = false;
        pilot.passedFrom.Clear();
        pilot.passHops = 0;
        pilot.phase = Phase::Approach;
        pilot.planned = false;
        pilot.ambushTarget.Clear();
        pilot.crowdHops = 0;
        pilot.crowdWaitMs = 0;
        pilot.approachMs = 0;
        pilot.stuckMs = 0;
        pilot.moveCooldownMs = 0;
        pilot.bestDist = me->GetDistance(next);
        pilot.campX = me->GetPositionX();
        pilot.campY = me->GetPositionY();
        pilot.campZ = me->GetPositionZ();
        Prepare(me);
        Note(pilot, me, "next: %s (entry %u, guid %u) at %.0f yd", next->GetName(),
            next->GetEntry(), next->GetGUIDLow(), pilot.bestDist);
    }

    return pilot.phase == Phase::Approach ? DrivePull(ai, me, pilot) : false;
}

std::string SuiAutopilot::Status()
{
    std::lock_guard<std::recursive_mutex> guard(gLock);
    if (gPilots.empty())
        return "autopilot: off";
    std::ostringstream out;
    for (auto const& pair : gPilots)
    {
        Pilot const& p = pair.second;
        Player* leader = Find(pair.first);
        Readiness const r = leader ? Assess(leader) : Readiness();
        out << "autopilot leader " << (leader ? leader->GetName() : "(offline)")
            << " members=" << r.members << " dead=" << r.dead << " far=" << r.far << " low=" << r.low
            << (p.paused ? " PAUSED" : "") << " phase=" << int(p.phase) << " pulls=" << p.pulls
            << " fights=" << p.fights << " skipped=" << p.skip.size() << (p.review ? " review" : "")
            << " last: " << p.lastEvent << "\n";
    }
    return out.str();
}

// .autopilot on <leader> | off [leader] | pause <leader> | resume <leader> | status
bool ChatHandler::HandleAutopilotCommand(char* args)
{
    char* verb = ExtractLiteralArg(&args);
    std::string const action = verb ? verb : "status";
    if (action == "status")
    {
        std::string const status = SuiAutopilot::Status();
        std::istringstream lines(status);
        std::string line;
        while (std::getline(lines, line))
            SendSysMessage(line.c_str());
        return true;
    }
    if (action == "why")
    {
        char* botName = ExtractLiteralArg(&args);
        uint32 spellId = 0;
        Player* bot = botName ? sObjectMgr.GetPlayer(botName) : nullptr;
        AiBotAI* ai = bot ? dynamic_cast<AiBotAI*>(bot->AI()) : nullptr;
        if (!ai || !ExtractUInt32(&args, spellId))
        {
            SendSysMessage("usage: .autopilot why <bot> <first-rank spell id>");
            SetSentErrorMessage(true);
            return false;
        }
        SpellEntry const* spell = ai->GetHighestKnownRank(spellId);
        Unit* target = bot->GetVictim();
        std::ostringstream out;
        out << bot->GetName() << " spell " << spellId << " -> "
            << (spell ? std::to_string(spell->Id) : std::string("NOT KNOWN"))
            << " victim " << (target ? target->GetName() : "(none)");
        if (spell && target)
        {
            uint32 const cost = Spell::CalculatePowerCost(spell, bot);
            out << " | ready=" << bot->IsSpellReady(spell->Id) << " gcd=" << bot->HasGCD(spell)
                << " targetAuraState=" << spell->TargetAuraState << ":" << (spell->TargetAuraState ? target->HasAuraState(AuraState(spell->TargetAuraState)) : 1)
                << " casterAuraState=" << spell->CasterAuraState << ":" << (spell->CasterAuraState ? bot->HasAuraState(AuraState(spell->CasterAuraState)) : 1)
                << " power=" << bot->GetPower(Powers(spell->powerType)) << "/" << cost
                << " immune=" << target->IsImmuneToSpell(spell, false)
                << " form=" << uint32(spell->GetErrorAtShapeshiftedCast(bot->GetShapeshiftForm()))
                << " range=" << spell->rangeIndex << " meleeReach=" << bot->CanReachWithMeleeSpellAttack(target)
                << " dist=" << bot->GetDistance(target) << " combatDist=" << bot->GetCombatDistance(target)
                << " facing=" << bot->IsFacingTarget(target)
                << " canTry=" << ai->CanTryToCastSpell(target, spell);
        }
        SendSysMessage(out.str().c_str());
        return true;
    }
    if (action == "off" && !*args)
    {
        SuiAutopilot::DisableAll();
        SendSysMessage("autopilot: all off");
        return true;
    }
    char* name = ExtractLiteralArg(&args);
    Player* leader = name ? sObjectMgr.GetPlayer(name) : nullptr;
    if (!leader)
    {
        SendSysMessage("usage: .autopilot on|off|pause|resume <leader name> | status");
        SetSentErrorMessage(true);
        return false;
    }
    if (action == "on")
    {
        std::string reply;
        bool const ok = SuiAutopilot::Enable(leader, reply);
        SendSysMessage(reply.c_str());
        if (!ok)
            SetSentErrorMessage(true);
        return ok;
    }
    // A new lockout for the group (what the weekly reset does): everyone outside first.
    if (action == "newlockout")
    {
        uint32 mapId = 0;
        Group* group = leader->GetGroup();
        if (!ExtractUInt32(&args, mapId) || !group || !sMapStorage.LookupEntry<MapEntry>(mapId))
        {
            SendSysMessage("usage: .autopilot newlockout <leader> <map id> (leader in a group, everyone outside)");
            SetSentErrorMessage(true);
            return false;
        }
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->getSource())
                if (m->GetMapId() == mapId)
                {
                    PSendSysMessage("%s is still inside map %u - move everyone out first", m->GetName(), mapId);
                    SetSentErrorMessage(true);
                    return false;
                }
        uint32 unbound = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->getSource())
                if (m->GetBoundInstance(mapId))
                {
                    m->UnbindInstance(mapId);
                    ++unbound;
                }
        group->UnbindInstance(mapId);
        {
            // the deferrals of the old lockout go with it
            std::lock_guard<std::recursive_mutex> guard(gLock);
            std::vector<std::string> rows;
            {
                std::ifstream in(DeferredPath());
                std::string line;
                while (std::getline(in, line))
                    if (line.compare(0, std::to_string(mapId).size() + 1, std::to_string(mapId) + " ") != 0)
                        rows.push_back(line);
            }
            std::ofstream out(DeferredPath(), std::ios::trunc);
            for (std::string const& row : rows)
                out << row << '\n';
            std::ofstream(DeferredPath() + ".entries", std::ios::trunc);
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: new lockout on map %u (%u member bind(s) and the group bind dropped)",
            leader->GetName(), mapId, unbound);
        PSendSysMessage("new lockout on map %u: %u member bind(s) and the group bind dropped", mapId, unbound);
        return true;
    }
    // Setup between pulls: equip items on a member by id, replacing what each slot holds - a new raid
    // member needs gear before its first pull and bots read no mail (2026-11-09, the off-tank warrior).
    if (action == "equip")
    {
        uint32 itemId = 0, equipped = 0;
        uint32 filled = 0; // slots this call already equipped: a second ring or trinket takes the paired slot
        while (ExtractUInt32(&args, itemId))
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
            uint8 slot = proto ? leader->FindEquipSlot(proto, NULL_SLOT, true) : uint8(NULL_SLOT);
            if ((slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_TRINKET1) && (filled & (1u << slot)))
                ++slot;
            if (slot == NULL_SLOT)
            {
                PSendSysMessage("%u: no equipment slot", itemId);
                continue;
            }
            filled |= 1u << slot;
            if (leader->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                leader->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            if (proto->InventoryType == INVTYPE_2HWEAPON && leader->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                leader->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
            uint16 dest = 0;
            InventoryResult const result = leader->CanEquipNewItem(slot, dest, itemId, false);
            if (result != EQUIP_ERR_OK || !leader->EquipNewItem(dest, itemId, true))
            {
                PSendSysMessage("%u: cannot equip (result %u)", itemId, uint32(result));
                continue;
            }
            ++equipped;
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %u item(s) equipped (setup)", leader->GetName(), equipped);
        PSendSysMessage("%s: %u item(s) equipped", leader->GetName(), equipped);
        return true;
    }
    // A boss chest is opened by a member: bots loot only corpses, so the Cache of the Firelord (a chest the
    // instance spawns when Majordomo yields) was never opened (2026-11-09). Same steps as Player::SendLoot's
    // chest branch; the items go to the member's bags, to be handed out with "give".
    if (action == "loot")
    {
        // the nearest chest spawn with a loot table on this map (grids away from the raid are not loaded)
        uint32 chestGuid = 0, chestEntry = 0;
        float cx = 0, cy = 0, cz = 0, best = 600.0f;
        auto chestWorker = [&](GameObjectDataPair const& pair) -> bool
        {
            GameObjectData const& data = pair.second;
            if (data.position.mapId != leader->GetMapId())
                return false;
            GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(data.id);
            if (!info || info->type != GAMEOBJECT_TYPE_CHEST || !info->GetLootId())
                return false;
            float const d = leader->GetDistance(data.position.x, data.position.y, data.position.z);
            if (d < best)
            {
                best = d;
                chestGuid = pair.first;
                chestEntry = data.id;
                cx = data.position.x;
                cy = data.position.y;
                cz = data.position.z;
            }
            return false;
        };
        sObjectMgr.DoGOData(chestWorker);
        if (!chestGuid)
        {
            SendSysMessage("no chest spawn within 600 yd");
            SetSentErrorMessage(true);
            return false;
        }
        if (best > 4.0f)
        {
            leader->NearTeleportTo(cx + 2.0f, cy, cz + 0.5f, leader->GetOrientation());   // setup move
            PSendSysMessage("%s moved to chest spawn %u (%.0f yd) - run again to open it", leader->GetName(), chestGuid, best);
            return true;
        }
        GameObject* chest = leader->GetMap()->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, chestEntry, chestGuid));
        // "restore": a chest the instance script spawned once (Majordomo's cache) is gone after a server restart,
        // with its encounter still done - our own deploy hid it, and its loot is rolled only when it is opened.
        char* restore = ExtractLiteralArg(&args);
        if (chest && !chest->isSpawned() && restore && std::string(restore) == "restore")
        {
            chest->SetRespawnTime(HOUR);
            chest->Refresh();
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: chest %s (spawn %u) shown again after the restart (setup)",
                leader->GetName(), chest->GetName(), chestGuid);
        }
        if (!chest || !chest->isSpawned() || chest->getLootState() != GO_READY)
        {
            PSendSysMessage("chest spawn %u is not here, not spawned or already opened", chestGuid);
            SetSentErrorMessage(true);
            return false;
        }
        Loot& loot = chest->loot;
        loot.clear();
        Group* group = leader->GetGroup();
        bool const groupRules = group && chest->GetGOInfo()->chest.groupLootRules;
        if (groupRules)
        {
            group->UpdateLooterGuid(chest, true);
            loot.SetTeam(group->GetTeam());
        }
        loot.FillLoot(chest->GetGOInfo()->GetLootId(), LootTemplates_Gameobject, leader, !groupRules, false);
        loot.GenerateMoneyLoot(chest->GetGOInfo()->MinMoneyLoot, chest->GetGOInfo()->MaxMoneyLoot);
        if (groupRules)
            group->UpdateLooterGuid(chest);
        std::ostringstream items;
        for (LootItem const& li : loot.items)
            if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(li.itemid))
                items << " " << li.itemid << " " << proto->Name1 << " (q" << proto->Quality << ") x" << uint32(li.count) << ";";
        if (loot.gold)
        {
            leader->ModifyMoney(int32(loot.gold));
            loot.gold = 0;
        }
        leader->AutoStoreLoot(loot);
        chest->SetLootState(GO_JUST_DEACTIVATED);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s opens %s (spawn %u):%s", leader->GetName(), chest->GetName(),
            chest->GetDBTableGUIDLow(), items.str().c_str());
        PSendSysMessage("%s opens %s:%s", leader->GetName(), chest->GetName(), items.str().c_str());
        return true;
    }
    // Our own loot handed to a member who can use it (bots trade nothing and read no mail): one item moves from
    // the first member's bags to the second's, who equips it if its own gear scoring says it is better.
    if (action == "give")
    {
        char* toName = ExtractLiteralArg(&args);
        Player* to = toName ? sObjectMgr.GetPlayer(toName) : nullptr;
        uint32 itemId = 0;
        if (!to || !ExtractUInt32(&args, itemId) || !leader->HasItemCount(itemId, 1, false))
        {
            SendSysMessage("usage: .autopilot give <from> <to> <item id> (the item in <from>'s bags)");
            SetSentErrorMessage(true);
            return false;
        }
        ItemPosCountVec dest;
        if (to->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK)
        {
            PSendSysMessage("%s has no room for %u", to->GetName(), itemId);
            SetSentErrorMessage(true);
            return false;
        }
        leader->DestroyItemCount(itemId, 1, true);
        Item* item = to->StoreNewItem(dest, itemId, true);
        if (item)
            to->SendNewItem(item, 1, true, false);
        if (AiBotAI* ai = dynamic_cast<AiBotAI*>(to->AI()))
            ai->TryAutoEquip();
        bool const worn = to->HasItemWithIdEquipped(itemId, 1);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s gives %u to %s (%s)", leader->GetName(), itemId, to->GetName(),
            worn ? "equipped" : "kept in bags");
        PSendSysMessage("%s gives %u to %s (%s)", leader->GetName(), itemId, to->GetName(), worn ? "equipped" : "kept in bags");
        return true;
    }
    // Setup: a new member joins the leader's group, an OFFLINE member making room when it is full
    // (2026-11-09: the off-tank warrior took the slot of Testwar, who stays offline).
    if (action == "join")
    {
        char* memberName = ExtractLiteralArg(&args);
        char* roomName = ExtractLiteralArg(&args);
        Player* member = memberName ? sObjectMgr.GetPlayer(memberName) : nullptr;
        Group* group = leader->GetGroup();
        if (group && roomName && group->IsFull())
        {
            ObjectGuid const room = sObjectMgr.GetPlayerGuidByName(roomName);
            if (!room.IsEmpty() && group->IsMember(room) && !sObjectMgr.GetPlayer(room))
            {
                group->RemoveMember(room, 0);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s (offline) leaves %s's group to make room (setup)", roomName, leader->GetName());
            }
        }
        if (!member || !group || member->GetGroup() == group || group->IsFull())
        {
            SendSysMessage("usage: .autopilot join <leader> <member> [offline member to make room] (leader in a group with a free slot, member online)");
            SetSentErrorMessage(true);
            return false;
        }
        if (Group* old = member->GetGroup())
            old->RemoveMember(member->GetObjectGuid(), 0);
        if (!group->AddMember(member->GetObjectGuid(), member->GetName()))
        {
            SendSysMessage("the group refused the member");
            SetSentErrorMessage(true);
            return false;
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s joins %s's group (setup)", member->GetName(), leader->GetName());
        PSendSysMessage("%s joined %s's group", member->GetName(), leader->GetName());
        return true;
    }
    if (action == "review")
    {
        char* mode = ExtractLiteralArg(&args);
        bool const on = !mode || std::string(mode) != "off";
        SuiAutopilot::SetReview(leader->GetObjectGuid(), on);
        PSendSysMessage("autopilot review stops %s: %s", on ? "on" : "off", leader->GetName());
        return true;
    }
    if (action == "off")
        SuiAutopilot::Disable(leader->GetObjectGuid());
    else if (action == "pause" || action == "resume")
        SuiAutopilot::SetPaused(leader->GetObjectGuid(), action == "pause");
    else
    {
        SendSysMessage("usage: .autopilot on|off|pause|resume <leader name> | status");
        SetSentErrorMessage(true);
        return false;
    }
    PSendSysMessage("autopilot %s: %s", action.c_str(), leader->GetName());
    return true;
}

void SuiAutopilot::RemapInstances(std::map<uint32, uint32> const& oldToNew)
{
    std::lock_guard<std::recursive_mutex> guard(gLock);
    std::vector<std::tuple<uint32, uint32, uint32>> kept;
    uint32 dropped = 0;
    {
        std::ifstream in(ClearedPath());
        uint32 map = 0, instance = 0, guid = 0;
        while (in >> map >> instance >> guid)
        {
            auto it = oldToNew.find(instance);
            if (it == oldToNew.end())
                ++dropped;
            else
                kept.emplace_back(map, it->second, guid);
        }
    }
    std::ofstream out(ClearedPath(), std::ios::trunc);
    for (auto const& row : kept)
        out << std::get<0>(row) << ' ' << std::get<1>(row) << ' ' << std::get<2>(row) << '\n';
    gCleared.clear();
    gClearedLoaded = false;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] cleared spawns renumbered with their instances: %u kept, %u dropped (instance gone)",
        uint32(kept.size()), dropped);
}

void SuiAutopilot::NoteKill(Unit* killer, Unit* victim)
{
    if (!killer || !victim || !victim->IsCreature())
        return;
    Creature* c = static_cast<Creature*>(victim);
    if (!c->HasStaticDBSpawnData() || !c->GetDBTableGUIDLow())
        return;
    // A creature that finishes itself after our fight (a pack whose last one down takes the rest
    // with it - Core Hounds) is our kill too: it came back after a restart and was fought again.
    Player* player = nullptr;
    if (killer == victim)
    {
        player = c->GetLootRecipient();
        if (!player && c->GetVictim())
            player = c->GetVictim()->GetCharmerOrOwnerPlayerOrPlayerItself();
    }
    else
        player = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!player || !player->GetGroup())
        return;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    if (gPilots.empty())
        return;
    bool ours = false;
    for (auto const& pair : gPilots)
        if (Player* leader = Find(pair.first))
            if (leader->GetGroup() == player->GetGroup())
                ours = true;
    if (!ours)
        return;
    LoadCleared();
    if (c->IsWorldBoss() ||
        !gCleared.insert(std::make_tuple(c->GetMapId(), c->GetInstanceId(), c->GetDBTableGUIDLow())).second)
        return;
    std::ofstream out(ClearedPath(), std::ios::app);
    out << c->GetMapId() << ' ' << c->GetInstanceId() << ' ' << c->GetDBTableGUIDLow() << '\n';
}
