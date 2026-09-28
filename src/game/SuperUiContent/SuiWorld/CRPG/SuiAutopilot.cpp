/*
 * SuiAutopilot.cpp — see SuiAutopilot.h. Line endings: LF (C++ repo convention).
 */

#include "CreatureGroups.h"
#include "SuiAutopilot.h"
#include "SuiRaidTelemetry.h"
#include "SuiRaidSupply.h"
#include "AiBotAIMain.h"
#include "Chat.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
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

    bool Pullable(Player* me, Creature* c, Pilot const& pilot)
    {
        return c && c->IsAlive() && c->IsInWorld() && !c->IsInCombat() && !c->IsTotem() &&
            !c->IsCivilian() && c->GetCreatureType() != CREATURE_TYPE_CRITTER &&
            c->IsHostileTo(me) && me->IsValidAttackTarget(c) &&
            c->GetUInt32Value(UNIT_NPC_FLAGS) == 0 && !c->IsGuard() &&   // never a flight master, vendor or guard
            !c->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_IMMUNE_TO_PLAYER) &&
            !(pilot.skip.count(c->GetObjectGuid()) && WorldTimer::getMSTime() < pilot.skip.at(c->GetObjectGuid())) &&
            !IsCleared(c) &&
            !(pilot.deferEntry.count(c->GetEntry()) && WorldTimer::getMSTime() < pilot.deferEntry.at(c->GetEntry())) &&
            std::none_of(pilot.deferZones.begin(), pilot.deferZones.end(), [c](Pilot::Zone const& z)
            {
                return WorldTimer::getMSTime() < z.until && std::fabs(c->GetPositionZ() - z.z) < 25.0f &&
                    c->GetDistance2d(z.x, z.y) < z.r;
            });
    }

    // Idle hostiles within r of `around` that belong to another pack than `target` (or to
    // none): what a fight at that spot would drag in.
    uint32 IdleNeighbours(Player* me, WorldObject* around, Creature* target, float r)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(around, r);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(around, searcher, r);
        uint32 count = 0;
        for (Creature* c : near)
        {
            if (c == target || !c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me))
                continue;
            if (target && c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup())
                continue;
            ++count;
        }
        return count;
    }

    // The nearest reachable hostile by walked distance: the eight nearest by straight line
    // are pathed and the shortest walk wins, so a pack across a lava gap loses to the one
    // at the end of the corridor.
    Creature* PickNextWithin(Player* me, Pilot& pilot, float radius)
    {
        std::list<Creature*> nearby;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, radius);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
        Cell::VisitGridObjects(me, searcher, radius);

        std::vector<std::pair<float, Creature*>> byLine;
        for (Creature* c : nearby)
            if (Pullable(me, c, pilot))
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
                pilot.skip[entry.second->GetObjectGuid()] = WorldTimer::getMSTime() + 2 * 60 * 1000;
                sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[AUTOPILOT] %s: no path to %s (entry %u) - skipped",
                    me->GetName(), entry.second->GetName(), entry.second->GetEntry());
                continue;
            }
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
            if (c == boss || c->IsWorldBoss() || !Pullable(me, c, pilot) ||
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
        for (Creature* c : nearby)
            if (c != target && Pullable(me, c, pilot) && !BossOf(c) &&
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
                    return blocker;
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
            if (b->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE)
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

    Creature* PickNext(Player* me, Pilot& pilot)
    {
        pilot.offMesh = false;
        for (float radius : kSearchRings)
            if (pilot.offMesh)
                return nullptr;
            else if (Creature* found = PickNextWithin(me, pilot, radius))
            {
                Creature* walker = nullptr;
                if (true)   // room trash too: a patrolling boss comes back to it (Geddon, 2026-09-25)
                    if ((walker = PatrollingBossNear(me, pilot, found)))
                        if (PathLength(me, walker->GetPositionX(), walker->GetPositionY(), walker->GetPositionZ()) >= 0.0f)
                        {
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s patrols past %s (entry %u) - the boss goes first",
                                me->GetName(), walker->GetName(), found->GetName(), found->GetEntry());
                            found = walker;
                            // The walk to it is still cleared of packs well away from it (2026-09-24:
                            // the raid walked 580 yd to Geddon and met two packs on the way).
                            for (int hop = 0; hop < 4; ++hop)
                            {
                                Creature* first = InTheWay(me, pilot, found);
                                if (!first || first->GetDistance(walker) < 60.0f)
                                    break;
                                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s (entry %u) stands in the way of %s - it goes first",
                                    me->GetName(), first->GetName(), first->GetEntry(), found->GetName());
                                found = first;
                            }
                            return found;
                        }
                if (Creature* boss = BossOf(found))
                    if (Creature* trash = RoomTrash(me, pilot, boss))
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s (entry %u) is cleared before %s",
                            me->GetName(), trash->GetName(), trash->GetEntry(), boss->GetName());
                        found = trash;
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
            if (PathLength(me, px, py, pz) < 0.0f)
                continue;
            best = d;
            x = px;
            y = py;
            z = pz;
            entryOut = entry;
            guidOut = pair.first;
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
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AUTOPILOT] %s: %s", me->GetName(), buffer);
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
    SuiRaidTelemetry::FlushFight(leader, pilot.fights, pilot.fightMs / 1000);
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
    pilot.phase = Phase::Idle;
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
            if (!m->IsInCombat())
                SuiRaidSupply::Supply(quartermaster, m, 0);
            AiBotAI* ai = dynamic_cast<AiBotAI*>(m->AI());
            CombatBotRoles const role = ai ? ai->GetCombatActiveRole() : ROLE_MELEE_DPS;
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

    // Idle hostiles that are not the target's own pack, around the target: what a camp, a pull
    // spot or the raid's walk there must keep clear of.
    void ForeignIdle(Player* me, Creature* target, float r, std::vector<Spot>& out)
    {
        std::list<Creature*> near;
        MaNGOS::AnyUnitInObjectRangeCheck check(target, r);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(near, check);
        Cell::VisitGridObjects(target, searcher, r);
        for (Creature* c : near)
        {
            if (c == target || !c->IsAlive() || c->IsInCombat() || c->IsTotem() || c->IsCivilian() ||
                c->GetCreatureType() == CREATURE_TYPE_CRITTER || !c->IsHostileTo(me))
                continue;
            if (c->GetCreatureGroup() && c->GetCreatureGroup() == target->GetCreatureGroup())
                continue;
            out.push_back({c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), c});
            ForEachRoutePoint(c, [&](float x, float y, float z) { out.push_back({x, y, z, c}); });
        }
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
        for (int i = 0; i < 24; ++i)
        {
            float const a = i * float(M_PI) / 12.0f, cx = std::cos(a), cy = std::sin(a);
            Spot P{tx + 28.0f * cx, ty + 28.0f * cy, tz}, C{tx + 50.0f * cx, ty + 50.0f * cy, tz};
            if (!Ground(me, P.x, P.y, P.z) || !Ground(me, C.x, C.y, C.z) ||
                Molten(me, P.x, P.y, P.z) || Molten(me, C.x, C.y, C.z))
                continue;
            if (!pack.empty() && Nearest(pack, P.x, P.y, P.z) < 26.0f)
                continue;
            if (at ? !me->GetMap()->isInLineOfSight(tx, ty, tz + 2.0f, P.x, P.y, P.z + 2.0f)
                   : !target->IsWithinLOS(P.x, P.y, P.z + 2.0f))
                continue;
            // The camp sees the pull spot, on its level: the fight comes to the spot and the healers
            // heal it from the camp (2026-09-25: a camp on the ramp below Majordomo's platform left
            // every healer without sight of the tanks above).
            if (std::fabs(C.z - P.z) > 4.0f || !me->GetMap()->isInLineOfSight(C.x, C.y, C.z + 2.0f, P.x, P.y, P.z + 2.0f))
                continue;
            PathInfo toCamp(me);
            toCamp.calculate(C.x, C.y, C.z, false);
            if (toCamp.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                continue;
            PathInfo pullToCamp(me);
            pullToCamp.calculate(Vector3(P.x, P.y, P.z), Vector3(C.x, C.y, C.z), false);
            if (pullToCamp.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                continue;
            // ...and the pack itself must be able to walk to the camp: a camp the creatures cannot
            // path to makes them evade after 24 s "target unreachable", and a linked pack resets
            // with its boss (2026-09-25: Sulfuron's priests, twice, their tank 0.8 yd away).
            if (!at)
            {
                PathInfo theirs(target);
                theirs.calculate(C.x, C.y, C.z, false);
                if (theirs.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                    continue;
            }
            float back = 0.0f;
            PointsArray const& bp = pullToCamp.getPath();
            for (size_t k = 1; k < bp.size(); ++k)
                back += (bp[k] - bp[k - 1]).length();
            if (back > 45.0f)
                continue;   // the run back must be short and direct
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
            if (pastTarget < 24.0f)
                continue;
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
        if (bestScore > 1e8f)
            return false;
        pilot.planned = true;
        pilot.planFresh = true;
        pilot.pullX = bestP.x; pilot.pullY = bestP.y; pilot.pullZ = bestP.z;
        pilot.campX = bestC.x; pilot.campY = bestC.y; pilot.campZ = bestC.z;
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
            Note(pilot, me, "plan for %s: pull spot clear of other packs by %.0f yd, camp by %.0f yd, route by %.0f yd",
            target->GetName(), std::min(bestDp, 999.0f), std::min(bestDc, 999.0f), std::min(bestDr, 999.0f));
        return true;
    }

    // [AMBUSH] The point of a patrol's walk farthest from every idle pack and every other patrol's
    // route: where it is pulled (2026-09-25: Baron Geddon's walk passes six packs; pulled where he
    // happened to be, he dragged a Firewalker pack in every time).
    bool AmbushPoint(Player* me, Creature* target, float& qx, float& qy, float& qz, float& clearance)
    {
        if (target->GetDefaultMovementType() != WAYPOINT_MOTION_TYPE)
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
        if (!pilot.hasSafe || IdleNeighbours(me, me, nullptr, 40.0f) == 0)
            return false;
        if (me->GetDistance(pilot.safeX, pilot.safeY, pilot.safeZ) <= 4.0f)
            return false;
        if (!me->IsMoving())
        {
            if (pilot.lastEvent != "backing off to a clear spot")
                Note(pilot, me, "backing off to a clear spot");
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
            float const dist = me->GetDistance(target);
            bool const ambush = pilot.ambushTarget == pilot.target && !pilot.ambushTarget.IsEmpty();
            if (!ambush && pilot.planned && (std::fabs(target->GetPositionX() - pilot.planTX) > 15.0f ||
                std::fabs(target->GetPositionY() - pilot.planTY) > 15.0f))
                pilot.planned = false;   // it moved: plan again
            if (!pilot.planned && !ambush && target->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE &&
                dist <= 250.0f && RangedPullSpell(me))
            {
                float qx = 0.0f, qy = 0.0f, qz = 0.0f, clear = 0.0f;
                if (AmbushPoint(me, target, qx, qy, qz, clear))
                {
                    Vector3 const at(qx, qy, qz);
                    if (PlanPull(me, target, pilot, &at))
                    {
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
                    target->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE;
                // The crowding is a patrol's route (a boss walking through the room): pull while it
                // is at the far end of its walk, not while it is near the camp.
                if (!patrol && !pilot.crowder.IsEmpty())
                    if (Creature* walker = me->GetMap()->GetCreature(pilot.crowder))
                        if (walker->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
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
            if (pilot.planned && ambush &&
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
            if (dist <= kCampYards && me->IsWithinLOSInMap(target))
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
                        if (c != target && c->IsAlive() && c->IsHostileTo(me))
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
                if (rhythm > 0 && rhythm < 10000)
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
                            if (c != target && c->IsAlive() && c->IsHostileTo(me))
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
                if ((aggroDist < 15.0f || bigAlone) && onMe <= 1 && IdleNeighbours(me, me, target, 30.0f) == 0 &&
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
                Note(pilot, me, "pull %u: %s (entry %u) aggroed at %.0f yd - back to camp", pilot.pulls,
                    target->GetName(), target->GetEntry(), aggroDist);
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
                                m->GetDistance(pilot.campX, pilot.campY, pilot.campZ) > kGatheredYards)
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
                if (!shot || pilot.ambushWaitMs > 420000)
                {
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
                if (inPlace && dist <= 30.0f && me->IsWithinLOSInMap(target) &&
                    IdleNeighbours(me, target, target, 32.0f) == 0)
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
                        me->GetMotionMaster()->MovePoint(0, pilot.pullX, pilot.pullY, pilot.pullZ, MOVE_PATHFINDING);
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
    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(const_cast<Player*>(member)->AI()))
        if (ai->GetCombatActiveRole() == ROLE_TANK)
            return false;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    for (auto const& pair : gPilots)
    {
        Player* leader = Find(pair.first);
        if (leader && leader != member && leader->GetGroup() == member->GetGroup())
            // (The raid used to hold while the leader walked to a planned camp alone: the leader
            // then died alone whenever something woke on the way - 2026-09-25, four times. The
            // follow arc is 4-6 yd behind it; the raid walks with it.)
            return (pair.second.phase == Phase::Pull && pair.second.gatherMs >= kGatherCapMs) ||
                (pair.second.phase == Phase::Return && !pair.second.retreating);
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

bool SuiAutopilot::TickLeader(AiBotAI* ai)
{
    Player* me = ai ? ai->GetBotPlayer() : nullptr;
    if (!me || !me->IsInWorld() || !me->IsAlive())
        return false;
    std::lock_guard<std::recursive_mutex> guard(gLock);
    auto it = gPilots.find(me->GetObjectGuid());
    if (it == gPilots.end() || it->second.paused)
        return false;
    Pilot& pilot = it->second;
    uint32 const step = AIBOT_UPDATE_INTERVAL;

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
        if (!me->IsInCombat())
            ExecuteRespawns(me, pilot, 300.0f);   // a room away too (2026-09-25: a respawned pack 150+ yd off walked into Geddon)
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
                        if (std::fabs(z - me->GetPositionZ()) < 20.0f && me->GetDistance2d(x, y) < 35.0f)
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
        return false;   // the doctrine fights; the autopilot only drives between fights
    }

    if (pilot.phase == Phase::Fight)
    {
        Note(pilot, me, "fight %u over after %u s (members %u, dead %u)", pilot.fights,
            pilot.fightMs / 1000, ready.members, ready.dead);
        SuiRaidTelemetry::FlushFight(me, pilot.fights, pilot.fightMs / 1000);
        pilot.phase = Phase::Rest;
        pilot.restMs = 0;
        pilot.target.Clear();
    }

    if (pilot.phase == Phase::Idle || pilot.phase == Phase::Rest)
    {
        MoveToSafe(me, pilot);
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
            << " fights=" << p.fights << " skipped=" << p.skip.size()
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
