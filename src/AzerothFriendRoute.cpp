#include "AzerothFriendRoute.h"
#include "AzerothFriendUtilities.h"
#include "Config.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "PathGenerator.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "Player.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    struct RouteConfig
    {
        uint32_t queriesPerVisit = 6;
        float    nodeSpacing     = 28.0f;
        float    waterCost       = 20.0f;
        uint32_t lookaheadNodes  = 12;
    };

    RouteConfig g_rc;

    constexpr float    ARRIVAL       = 6.0f;
    constexpr float    MIN_PROGRESS  = 2.5f;
    constexpr float    LEG_MAX       = 120.0f;
    constexpr float    LEG_MIN       = 6.0f;
    constexpr float    LOOP_RADIUS   = 18.0f;
    constexpr float    TURN_COSINE   = 0.94f;
    constexpr float    NODE_REACHED  = 8.0f;
    constexpr float    TILES_LOADED  = 90.0f;
    constexpr uint32_t MAX_STALLS    = 3;
    constexpr uint32_t MAX_STEP_OFFS = 4;
    constexpr float    HAND_AHEAD    = 55.0f;
    constexpr float    CORRIDOR_MAX  = 250.0f;
    constexpr float    CORRIDOR_MIN  = 40.0f;

    float D2(const AutopilotRoutePoint& a, const AutopilotRoutePoint& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    float D3(const AutopilotRoutePoint& a, const AutopilotRoutePoint& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    AutopilotRoutePoint At(Player* bot)
    {
        return { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    }

    void Filter(Player* bot, PathGenerator& generator)
    {
        AzerothFriendRoute::Filter(bot, generator);
    }

    bool TileLoaded(Map* map, float x, float y)
    {
        dtNavMesh const* mesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!mesh)
            return false;
        const float point[3] = { y, 0.0f, x };
        int tx = -1, ty = -1;
        mesh->calcTileLoc(point, &tx, &ty);
        return tx >= 0 && ty >= 0 && mesh->getTileAt(tx, ty, 0) != nullptr;
    }

    bool SnapToMesh(Map* map, AutopilotRoutePoint& p, float horizontal, float vertical)
    {
        dtNavMeshQuery const* query = map ? map->GetMapCollisionData().GetMMapData().GetNavMeshQuery() : nullptr;
        if (!query || !TileLoaded(map, p.x, p.y))
            return false;
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        const float center[3]  = { p.y, p.z, p.x };
        const float extents[3] = { horizontal, vertical, horizontal };
        dtPolyRef ref = 0;
        float nearest[3] = { 0.0f, 0.0f, 0.0f };
        if (dtStatusFailed(query->findNearestPoly(center, extents, &filter, &ref, nearest)) || !ref)
            return false;
        p = { nearest[2], nearest[0], nearest[1] };
        return true;
    }

    AutopilotRoutePoint Reseat(Map* map, const AutopilotRoutePoint& p, float walkingZ)
    {
        if (map)
        {
            const float ground = map->GetHeight(p.x, p.y, walkingZ + 6.0f, true, 80.0f);
            if (ground > INVALID_HEIGHT)
                return { p.x, p.y, ground };
        }
        return { p.x, p.y, walkingZ };
    }

    void RemoveLoops(std::vector<AutopilotRoutePoint>& points)
    {
        for (size_t i = 0; i + 2 < points.size(); ++i)
            for (size_t j = points.size() - 1; j > i + 1; --j)
                if (D2(points[i], points[j]) <= LOOP_RADIUS)
                {
                    points.erase(points.begin() + i + 1, points.begin() + j + 1);
                    break;
                }
    }

    void AddDense(AutopilotRoute& r, const AutopilotRoutePoint& p)
    {
        const float travelled = D2(r.lastKept, p);
        if (travelled < 0.01f)
            return;

        const float fx = (p.x - r.lastKept.x) / travelled;
        const float fy = (p.y - r.lastKept.y) / travelled;
        const bool turned = r.haveHeading && (fx * r.headingX + fy * r.headingY) < TURN_COSINE;

        if (travelled >= g_rc.nodeSpacing || (turned && travelled >= 8.0f))
        {
            r.nodes.push_back(p);
            r.lastKept    = p;
            r.headingX    = fx;
            r.headingY    = fy;
            r.haveHeading = true;
        }
    }

    void Anchor(Player* bot, AutopilotRoute& r)
    {
        if (D2(r.cursor, r.dest) < 150.0f)
        {
            r.anchorNote = "short walk: no road needed";
            return;
        }

        TravelNodeMap& nodeMap = TravelNodeMap::instance();
        std::shared_lock<std::shared_timed_mutex> lock(nodeMap.m_nMapMtx, std::try_to_lock);
        if (!lock.owns_lock())
        {
            r.anchorNote = "road network busy";
            return;
        }

        const WorldPosition from(bot);
        const WorldPosition to(r.map, r.dest.x, r.dest.y, r.dest.z);
        std::vector<TravelNode*> starts = nodeMap.getNodes(from, 600.0f);
        std::vector<TravelNode*> ends   = nodeMap.getNodes(to, 600.0f);
        if (starts.empty() || ends.empty())
        {
            r.anchorNote = starts.empty() ? "no road node within 600 yd of the bot"
                                          : "no road node within 600 yd of the destination";
            return;
        }
        if (starts.size() > 3) starts.resize(3);
        if (ends.size() > 3)   ends.resize(3);

        std::vector<TravelNode*> nodes;
        for (TravelNode* s : starts)
        {
            for (TravelNode* e : ends)
            {
                if (s == e)
                    continue;
                TravelNodeRoute found = nodeMap.getRoute(s, e, nullptr);
                if (!found.isEmpty())
                {
                    nodes = found.getNodes();
                    break;
                }
            }
            if (!nodes.empty())
                break;
        }
        if (nodes.size() < 2)
        {
            r.anchorNote = "no road route between the nearest nodes";
            return;
        }

        std::vector<AutopilotRoutePoint> points;
        auto keep = [&](const WorldPosition& w)
        {
            const AutopilotRoutePoint q{ w.GetPositionX(), w.GetPositionY(), w.GetPositionZ() };
            if (points.empty() || D2(points.back(), q) >= 120.0f)
                points.push_back(q);
        };

        for (size_t i = 0; i + 1 < nodes.size(); ++i)
        {
            if (nodes[i]->getMapId() != r.map || nodes[i + 1]->getMapId() != r.map)
                break;
            auto* links = nodes[i]->getLinks();
            auto link = links->find(nodes[i + 1]);
            if (link == links->end() || link->second->getPathType() != TravelNodePathType::walk)
                break;
            keep(*nodes[i]->getPosition());
            for (const WorldPosition& w : link->second->getPath())
                keep(w);
            if (i + 2 == nodes.size())
                keep(*nodes.back()->getPosition());
        }
        if (points.size() < 2)
        {
            r.anchorNote = "the road route is not a walk from here";
            return;
        }
        r.anchorNote = SafeFormat("following the road: {} nodes", nodes.size());
        r.anchors = std::move(points);
        r.anchor  = 0;
    }

    enum class Step { Progress, Exhausted, Done, Fail };

    bool StepOff(Player* bot, AutopilotRoute& r)
    {
        if (r.stepOffs >= MAX_STEP_OFFS)
            return false;
        Map* map = bot->GetMap();
        for (float radius : { 4.0f, 7.0f })
        {
            for (int i = 0; i < 8; ++i)
            {
                const float angle = float(i) * float(M_PI) / 4.0f;
                const AutopilotRoutePoint c = Reseat(map, { r.cursor.x + std::cos(angle) * radius,
                                                            r.cursor.y + std::sin(angle) * radius, r.cursor.z },
                                                     r.cursor.z);
                if (std::fabs(c.z - r.cursor.z) > 3.0f ||
                    !map->isInLineOfSight(r.cursor.x, r.cursor.y, r.cursor.z + 2.0f, c.x, c.y, c.z + 2.0f,
                                          bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                    continue;

                ++r.queries;
                PathGenerator probe(bot);
                Filter(bot, probe);
                probe.CalculatePath(c.x, c.y, c.z, c.x + 2.0f, c.y, c.z, false);
                if (probe.GetPathType() & (PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH))
                    continue;

                ++r.stepOffs;
                r.nodes.push_back(c);
                r.lastKept      = c;
                r.haveHeading   = false;
                r.cursor        = c;
                r.retryCorridor = true;
                return true;
            }
        }
        return false;
    }

    Step CorridorLeg(Player* bot, AutopilotRoute& r)
    {
        ++r.queries;
        PathGenerator generator(bot);
        generator.SetUseStraightPath(true);
        Filter(bot, generator);

        r.corridor.clear();
        r.corner = 0;

        while (r.anchor < r.anchors.size() &&
               (D2(r.cursor, r.anchors[r.anchor]) < 25.0f ||
                (r.anchor + 1 < r.anchors.size() &&
                 D2(r.cursor, r.anchors[r.anchor + 1]) < D2(r.cursor, r.anchors[r.anchor]))))
            ++r.anchor;
        const bool viaAnchor = r.anchor < r.anchors.size();
        const AutopilotRoutePoint goal = viaAnchor ? r.anchors[r.anchor] : r.dest;
        r.goal      = goal;
        r.goalIsEnd = !viaAnchor;

        AutopilotRoutePoint target = goal;
        const float toGoal = D2(r.cursor, goal);
        r.corridorToDest = !viaAnchor && toGoal <= r.corridorReach;
        if (toGoal > r.corridorReach)
        {
            const float t = r.corridorReach / toGoal;
            target = Reseat(bot->GetMap(), { r.cursor.x + (goal.x - r.cursor.x) * t,
                                             r.cursor.y + (goal.y - r.cursor.y) * t, r.cursor.z }, r.cursor.z);
        }

        SnapToMesh(bot->GetMap(), target, 20.0f, 30.0f);

        if (!generator.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, target.x, target.y, target.z, false))
        {
            r.why = "invalid coordinates";
            return Step::Fail;
        }

        const PathType type = generator.GetPathType();
        if (type & PATHFIND_NOT_USING_PATH)
        {
            PathGenerator probe(bot);
            probe.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, r.cursor.x + 2.0f, r.cursor.y, r.cursor.z, false);
            ++r.queries;
            if (probe.GetPathType() & PATHFIND_NOT_USING_PATH)
            {
                r.why = "no navmesh for this map";
                return Step::Fail;
            }

            if (r.corridorReach > CORRIDOR_MIN)
            {
                r.corridorReach = std::max(CORRIDOR_MIN, r.corridorReach * 0.5f);
                return Step::Progress;
            }
            if (D2(At(bot), r.cursor) >= TILES_LOADED)
            {
                r.waiting = true;
                return Step::Done;
            }
            r.why = "navmesh tile not loaded";
            return Step::Fail;
        }
        if (type & PATHFIND_NOPATH)
        {
            if (StepOff(bot, r))
                return Step::Progress;
            r.why = "no path from current position";
            return Step::Fail;
        }

        Movement::PointsArray const& points = generator.GetPath();
        if (points.size() < 2)
            return Step::Exhausted;

        for (size_t i = 1; i < points.size(); ++i)
        {
            const G3D::Vector3& p = points[i];
            r.corridor.push_back({ p.x, p.y, p.z });
        }
        RemoveLoops(r.corridor);
        r.legStart = r.cursor;
        r.legOpen  = true;
        r.reach    = LEG_MAX;
        r.corridorReach = CORRIDOR_MAX;
        return Step::Progress;
    }

    Step SmoothLeg(Player* bot, AutopilotRoute& r)
    {
        if (r.corner >= r.corridor.size())
            return Step::Exhausted;

        const AutopilotRoutePoint& corner = r.corridor[r.corner];
        const float toCorner = D2(r.cursor, corner);
        if (toCorner <= ARRIVAL)
        {
            ++r.corner;
            r.reach = LEG_MAX;
            return Step::Progress;
        }

        float step = std::min(r.reach, toCorner);
        AutopilotRoutePoint aim = corner;
        if (step < toCorner)
        {
            const float t = step / toCorner;
            aim = { r.cursor.x + (corner.x - r.cursor.x) * t,
                    r.cursor.y + (corner.y - r.cursor.y) * t,
                    r.cursor.z + (corner.z - r.cursor.z) * t };
        }
        aim = Reseat(bot->GetMap(), aim, r.cursor.z);

        ++r.queries;
        PathGenerator generator(bot);
        Filter(bot, generator);
        if (!generator.CalculatePath(r.cursor.x, r.cursor.y, r.cursor.z, aim.x, aim.y, aim.z, false))
        {
            r.why = "cannot compute walk path";
            return Step::Fail;
        }

        const PathType type = generator.GetPathType();
        if ((type & (PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH)) ||
            (type & PATHFIND_INCOMPLETE && generator.GetPath().size() < 2))
        {
            if (step > LEG_MIN * 2.0f)
            {
                r.reach = step * 0.5f;
                return Step::Progress;
            }
            if (StepOff(bot, r))
                return Step::Progress;
            r.why = "path blocked or terrain steep";
            return Step::Fail;
        }

        Movement::PointsArray const& points = generator.GetPath();
        if (points.size() < 2)
        {
            if (step > LEG_MIN * 2.0f)
            {
                r.reach = step * 0.5f;
                return Step::Progress;
            }
            return Step::Exhausted;
        }

        const G3D::Vector3& end = points.back();
        const AutopilotRoutePoint reached{ end.x, end.y, end.z };
        if (D2(r.cursor, reached) < MIN_PROGRESS)
        {
            if (step > LEG_MIN * 2.0f)
            {
                r.reach = step * 0.5f;
                return Step::Progress;
            }
            return Step::Exhausted;
        }

        for (size_t i = 1; i < points.size(); ++i)
            AddDense(r, { points[i].x, points[i].y, points[i].z });

        r.cursor = reached;
        if (D2(r.cursor, corner) <= ARRIVAL)
        {
            ++r.corner;
            r.reach = LEG_MAX;
        }
        else
            r.reach = std::min(LEG_MAX, step * 1.5f);

        if (r.corridorToDest && D2(r.cursor, r.dest) <= ARRIVAL)
        {
            if (D2(r.lastKept, r.dest) > 1.0f)
                r.nodes.push_back(r.dest);
            r.complete = true;
            return Step::Done;
        }
        return Step::Progress;
    }
}

namespace AzerothFriendRoute
{
    void LoadConfig()
    {
        g_rc.queriesPerVisit = sConfigMgr->GetOption<uint32>("AzerothFriend.Autopilot.Route.QueriesPerVisit", 6);
        g_rc.nodeSpacing     = sConfigMgr->GetOption<float>("AzerothFriend.Autopilot.Route.NodeSpacing", 28.0f);
        g_rc.waterCost       = sConfigMgr->GetOption<float>("AzerothFriend.Autopilot.Route.WaterCost", 20.0f);
    }

    void Filter(Player* bot, PathGenerator& generator)
    {
        generator.SetUseFilter(NAV_GROUND | NAV_WATER);
        generator.SetExcludeFilter(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        generator.SetAreaCost(NAV_WATER, std::max(10.0f, g_rc.waterCost));
    }

    bool ClimbsOnlyWhereWet(Player* bot, const Movement::PointsArray& points)
    {
        Map* map = bot->GetMap();
        for (size_t i = 1; i < points.size(); ++i)
        {
            const float dz = std::fabs(points[i].z - points[i - 1].z);
            const float dxy = std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
            if (dxy > 0.1f && dz / dxy > 0.8f) // steep
            {
                if (!map || !map->IsInWater(points[i].x, points[i].y, points[i].z, bot->GetCollisionHeight()))
                    return false;
            }
        }
        return true;
    }

    bool WalkableNear(Player* bot, float x, float y, AutopilotRoutePoint& out)
    {
        AutopilotRoutePoint p{ x, y, bot->GetPositionZ() };
        if (SnapToMesh(bot->GetMap(), p, 15.0f, 25.0f))
        {
            out = p;
            return true;
        }
        return false;
    }

    void Begin(Player* bot, uint32_t map, float x, float y, float z, AutopilotRoute& route)
    {
        route = AutopilotRoute();
        route.map = map;
        route.dest = { x, y, z };
        route.cursor = At(bot);
        route.lastKept = route.cursor;
        Anchor(bot, route);
    }

    void Rebuild(Player* bot, AutopilotRoute& route)
    {
        const uint32_t prevRebuilds = route.rebuilds;
        const AutopilotRoutePoint dest = route.dest;
        const uint32_t map = route.map;
        Begin(bot, map, dest.x, dest.y, dest.z, route);
        route.rebuilds = prevRebuilds + 1;
    }

    void Extend(Player* bot, AutopilotRoute& route)
    {
        if (route.complete || route.failed)
            return;

        route.waiting = false;
        uint32_t spent = 0;
        while (spent < g_rc.queriesPerVisit && !route.complete && !route.failed && !route.waiting)
        {
            if (route.retryCorridor || !route.legOpen || route.corner >= route.corridor.size())
            {
                route.retryCorridor = false;
                const Step s = CorridorLeg(bot, route);
                ++spent;
                if (s == Step::Fail)
                {
                    route.failed = true;
                    return;
                }
                if (s == Step::Done)
                    return;
                continue;
            }

            const Step s = SmoothLeg(bot, route);
            ++spent;
            if (s == Step::Fail)
            {
                route.failed = true;
                return;
            }
            if (s == Step::Done)
                return;
        }
    }

    bool Next(Player* bot, AutopilotRoute& route, AutopilotRoutePoint& out, size_t& index)
    {
        if (route.nodes.empty())
            return false;

        const AutopilotRoutePoint me = At(bot);
        while (route.next < route.nodes.size() && D2(me, route.nodes[route.next]) <= NODE_REACHED)
            ++route.next;

        if (route.next >= route.nodes.size())
            return false;

        size_t pick = route.next;
        for (size_t i = route.next + 1; i < route.nodes.size(); ++i)
        {
            if (D2(me, route.nodes[i]) <= HAND_AHEAD)
                pick = i;
            else
                break;
        }

        out = route.nodes[pick];
        index = pick;
        return true;
    }
}
