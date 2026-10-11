#include "AzerothFriendTravel.h"
#include "AzerothFriendMovement.h"
#include "AzerothFriendRoute.h"
#include "AzerothFriendUtilities.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "ModelIgnoreFlags.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "WorldSession.h"

#if AF_HAS_PLAYERBOTS
#include "PlayerbotAI.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    struct TravelConfig
    {
        bool     flights        = true;
        float    flightMinYards = 700.0f;
        uint32_t boatWaitMin    = 20;
        uint32_t rideMaxMin     = 40;
        uint32_t stuckSeconds   = 45;
    };
    constexpr uint32_t kMaxPlans = 8;
    TravelConfig g_tc;

    constexpr float kDirectWalk  = 60.0f;
    constexpr float kDocked      = 40.0f;
    constexpr float kTouch       = 4.0f;

    struct Dock
    {
        uint32_t map = 0;
        float    x = 0.0f, y = 0.0f, z = 0.0f;
        float    lx = 0.0f, ly = 0.0f, lz = 0.0f;
        bool     hasLand = false;
        uint32_t allianceOnly = 0;
        uint32_t hordeOnly    = 0;
    };

    enum class CrossKind : uint8_t { Transport, Trigger, Portal };

    struct Crossing
    {
        CrossKind            kind  = CrossKind::Transport;
        uint32_t             entry = 0;
        std::string          name;
        size_t               from = 0, to = 0;
        AutopilotTravelPoint enter;
        AutopilotTravelPoint exit;
        bool                 alliance = true, horde = true;

        uint32_t FromMap() const;
        uint32_t ToMap() const;
    };

    std::vector<Dock>     g_docks;
    std::vector<Crossing> g_crossings;

    uint32_t Crossing::FromMap() const { return kind == CrossKind::Transport ? g_docks[from].map : enter.map; }
    uint32_t Crossing::ToMap() const   { return kind == CrossKind::Transport ? g_docks[to].map : exit.map; }

    bool IsContinent(uint32_t map) { return map == 0 || map == 1 || map == 530 || map == 571; }

    float Dist2D(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    bool DockOk(const Dock& d, TeamId team)
    {
        if (!d.hasLand)
            return false;
        return team == TEAM_ALLIANCE ? !(d.hordeOnly && !d.allianceOnly) : !(d.allianceOnly && !d.hordeOnly);
    }

    bool Usable(const Crossing& c, TeamId team)
    {
        if (c.kind == CrossKind::Transport)
            return DockOk(g_docks[c.from], team) && DockOk(g_docks[c.to], team);
        return team == TEAM_ALLIANCE ? c.alliance : c.horde;
    }

    size_t AddDock(uint32_t map, float x, float y, float z)
    {
        for (size_t i = 0; i < g_docks.size(); ++i)
            if (g_docks[i].map == map && Dist2D(g_docks[i].x, g_docks[i].y, x, y) < 30.0f)
                return i;
        Dock d;
        d.map = map;
        d.x = x;
        d.y = y;
        d.z = z;
        g_docks.push_back(d);
        return g_docks.size() - 1;
    }

    AutopilotTravelPoint Start(const Crossing& c)
    {
        if (c.kind == CrossKind::Transport)
        {
            const Dock& d = g_docks[c.from];
            return { d.map, d.lx, d.ly, d.lz };
        }
        return c.enter;
    }

    std::unordered_map<uint32_t, int> HopsTo(uint32_t target, TeamId team)
    {
        std::unordered_map<uint32_t, int> dist;
        dist[target] = 0;
        std::deque<uint32_t> queue{ target };
        while (!queue.empty())
        {
            const uint32_t map = queue.front();
            queue.pop_front();
            for (const Crossing& c : g_crossings)
            {
                if (c.ToMap() != map || dist.count(c.FromMap()) || !Usable(c, team))
                    continue;
                dist[c.FromMap()] = dist[map] + 1;
                queue.push_back(c.FromMap());
            }
        }
        return dist;
    }

    const Crossing* FirstHop(Player* bot, uint32_t targetMap, const std::vector<size_t>& avoid)
    {
        const std::unordered_map<uint32_t, int> hops = HopsTo(targetMap, bot->GetTeamId());
        auto it = hops.find(bot->GetMapId());
        if (it == hops.end())
            return nullptr;

        const int targetDist = it->second;
        const Crossing* best = nullptr;
        float bestDist = 0.0f;
        for (size_t i = 0; i < g_crossings.size(); ++i)
        {
            const Crossing& c = g_crossings[i];
            if (c.FromMap() != bot->GetMapId() || !Usable(c, bot->GetTeamId()))
                continue;
            if (std::find(avoid.begin(), avoid.end(), i) != avoid.end())
                continue;

            auto next = hops.find(c.ToMap());
            if (next == hops.end() || next->second >= targetDist)
                continue;

            const AutopilotTravelPoint s = Start(c);
            const float d = bot->GetExactDist2d(s.x, s.y);
            if (!best || d < bestDist)
            {
                best     = &c;
                bestDist = d;
            }
        }
        return best;
    }

    MotionTransport* StandingOn(Player* bot, uint32_t entry)
    {
        Map* map = bot->GetMap();
        if (!map)
            return nullptr;
        Transport* t = map->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                                               bot->GetPositionZ(), nullptr);
        return t && t->GetEntry() == entry ? t->ToMotionTransport() : nullptr;
    }

    bool DockedAt(AutopilotTrip& trip, MotionTransport* t, const AutopilotTravelPoint& stop, uint32_t now)
    {
        if (!t || t->GetMapId() != stop.map ||
            Dist2D(t->GetPositionX(), t->GetPositionY(), stop.x, stop.y) > kDocked)
        {
            trip.tSampleAt = 0;
            return false;
        }

        const float x = t->GetPositionX(), y = t->GetPositionY();
        const bool still = trip.tSampleAt && now > trip.tSampleAt && Dist2D(x, y, trip.tx, trip.ty) < 0.5f;
        if (!trip.tSampleAt || now > trip.tSampleAt)
        {
            trip.tx        = x;
            trip.ty        = y;
            trip.tSampleAt = now;
        }
        return still;
    }

    bool Deck(Player* bot, MotionTransport* t, float boardZ, AutopilotTravelPoint& out)
    {
        if (!t->m_model)
            return false;

        const float top    = t->GetPositionZ() + 40.0f;
        const float bottom = t->GetPositionZ() - 20.0f;

        bool  found     = false;
        float bestScore = 0.0f;
        for (float r : { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 16.0f })
        {
            for (int k = 0; k < (r == 0.0f ? 1 : 8); ++k)
            {
                const float a = float(k) * float(M_PI) / 4.0f;
                const float x = t->GetPositionX() + r * std::cos(a);
                const float y = t->GetPositionY() + r * std::sin(a);

                float from  = top;
                float above = FLT_MAX;
                for (int layer = 0; layer < 8 && from > bottom; ++layer)
                {
                    float dist = from - bottom;
                    const G3D::Ray ray(G3D::Vector3(x, y, from), G3D::Vector3(0.0f, 0.0f, -1.0f));
                    if (!t->m_model->intersectRay(ray, dist, false, bot->GetPhaseMask(), VMAP::ModelIgnoreFlags::Nothing))
                        break;
                    const float z = from - dist;
                    const float headroom = above - z;
                    above = z;
                    from  = z - 0.5f;

                    if (headroom < 2.2f || std::fabs(z - boardZ) > 8.0f)
                        continue;
                    if (bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), x, y, z + 0.5f, nullptr) != t)
                        continue;

                    const float score = std::fabs(z - boardZ) * 10.0f + bot->GetExactDist2d(x, y);
                    if (!found || score < bestScore)
                    {
                        out       = { t->GetMapId(), x, y, z + 0.5f };
                        bestScore = score;
                        found     = true;
                    }
                }
            }
        }
        return found;
    }

    std::string MapName(uint32_t map)
    {
        if (MapEntry const* m = sMapStore.LookupEntry(map))
            return m->name[0];
        return std::to_string(map);
    }

    std::string TaxiNodeName(uint32_t node)
    {
        if (TaxiNodesEntry const* n = sTaxiNodesStore.LookupEntry(node))
            return n->name[0];
        return "?";
    }

    float DistTo(Player* bot, const AutopilotTravelPoint& p)
    {
        return bot->GetMapId() == p.map ? bot->GetExactDist2d(p.x, p.y) : FLT_MAX;
    }

    void ResetLeg(AutopilotTrip& trip, uint32_t now)
    {
        trip.legStartedAt = now;
        trip.routed       = false;
        trip.route        = AutopilotRoute();
        trip.issued       = SIZE_MAX;
        trip.best         = FLT_MAX;
        trip.lastReached  = 0;
        trip.bestAt       = now;
        trip.tookOff      = false;
        trip.boarded      = false;
        trip.stepping     = false;
        trip.deckProbed   = false;
        trip.deckFound    = false;
        trip.tSampleAt    = 0;
    }

    bool KnowsNode(Player* bot, uint32_t node)
    {
        return bot->isTaxiCheater() || bot->m_taxi.IsTaximaskNodeKnown(node);
    }

    bool PlanFlight(Player* bot, const AutopilotTravelPoint& dest, AutopilotLeg& walk, AutopilotLeg& fly)
    {
#if AF_HAS_PLAYERBOTS
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
        if (!fm || !fm->taxiNodeId || fm->pos.GetMapId() != bot->GetMapId())
            return false;

        const float total  = DistTo(bot, dest);
        const float toFm   = bot->GetExactDist2d(fm->pos.GetPositionX(), fm->pos.GetPositionY());
        const uint8 mount  = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;

        uint32_t bestNode = 0;
        float    bestDist = 0.0f;
        for (uint32_t i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* n = sTaxiNodesStore.LookupEntry(i);
            if (!n || n->map_id != dest.map || !n->MountCreatureID[mount] || !KnowsNode(bot, n->ID))
                continue;
            const float d = Dist2D(n->x, n->y, dest.x, dest.y);
            if (!bestNode || d < bestDist)
            {
                bestNode = n->ID;
                bestDist = d;
            }
        }

        if (!bestNode || bestNode == fm->taxiNodeId || toFm + bestDist > total * 0.6f)
            return false;

        std::vector<uint32_t> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, bestNode);
        if (path.size() < 2)
            return false;
        for (size_t i = 1; i < path.size(); ++i)
            if (!KnowsNode(bot, path[i]))
                return false;

        walk.type   = AutopilotLegType::Walk;
        walk.to     = { fm->pos.GetMapId(), fm->pos.GetPositionX(), fm->pos.GetPositionY(), fm->pos.GetPositionZ() };
        walk.radius = kTouch;
        walk.label  = "the flight master";

        fly.type     = AutopilotLegType::Fly;
        fly.entry    = fm->templateEntry;
        fly.to       = walk.to;
        fly.taxiPath = std::move(path);
        fly.label    = TaxiNodeName(bestNode);
        return true;
#else
        (void)bot; (void)dest; (void)walk; (void)fly;
        return false;
#endif
    }

    std::string Plan(Player* bot, AutopilotTrip& trip, uint32_t now)
    {
        trip.legs.clear();
        trip.leg = 0;
        trip.crossing = SIZE_MAX;
        ResetLeg(trip, now);
        if (++trip.plans > kMaxPlans)
            return "too many changes of plan on the way";

        if (bot->GetMapId() != trip.dest.map)
        {
            const Crossing* c = FirstHop(bot, trip.dest.map, trip.avoid);
            if (!c)
                return SafeFormat("no boat, zeppelin or portal {} can take leads from {} toward {}",
                                  bot->GetTeamId() == TEAM_ALLIANCE ? "the Alliance" : "the Horde",
                                  MapName(bot->GetMapId()), MapName(trip.dest.map));

            const size_t crossingIndex = size_t(c - g_crossings.data());

            {
                AutopilotLeg walk, fly;
                const AutopilotTravelPoint start = Start(*c);
                if (g_tc.flights && !trip.noFlight && !trip.flown && DistTo(bot, start) > g_tc.flightMinYards &&
                    PlanFlight(bot, start, walk, fly))
                {
                    trip.legs = { walk, fly };
                    return "";
                }
            }

            if (c->kind != CrossKind::Transport)
            {
                trip.crossing = crossingIndex;
                AutopilotLeg walk;
                walk.type   = AutopilotLegType::Walk;
                walk.to     = c->enter;
                walk.radius = c->kind == CrossKind::Trigger ? 3.0f : kTouch;
                walk.label  = c->name;

                AutopilotLeg go;
                go.type   = c->kind == CrossKind::Trigger ? AutopilotLegType::Trigger : AutopilotLegType::Portal;
                go.entry  = c->entry;
                go.to     = c->enter;
                go.arrive = c->exit;
                go.label  = c->name;

                trip.legs = { walk, go };
                return "";
            }

            const Dock& from = g_docks[c->from];
            const Dock& to   = g_docks[c->to];
            trip.crossing = crossingIndex;

            AutopilotLeg walk;
            walk.type   = AutopilotLegType::Walk;
            walk.to     = { from.map, from.lx, from.ly, from.lz };
            walk.radius = 6.0f;
            walk.label  = SafeFormat("the dock for {}", c->name);

            AutopilotLeg board;
            board.type   = AutopilotLegType::Board;
            board.entry  = c->entry;
            board.to     = { from.map, from.x, from.y, from.z };
            board.label  = c->name;

            AutopilotLeg ride;
            ride.type   = AutopilotLegType::Ride;
            ride.entry  = c->entry;
            ride.arrive = { to.map, to.x, to.y, to.z };
            ride.land   = { to.map, to.lx, to.ly, to.lz };
            ride.label  = c->name;

            trip.legs = { walk, board, ride };
            return "";
        }

        // Same map.
        if (g_tc.flights && !trip.noFlight && !trip.flown && DistTo(bot, trip.dest) > g_tc.flightMinYards)
        {
            AutopilotLeg walk, fly;
            if (PlanFlight(bot, trip.dest, walk, fly))
            {
                trip.legs = { walk, fly };
                return "";
            }
        }

        AutopilotLeg walk;
        walk.type   = AutopilotLegType::Walk;
        walk.to     = trip.dest;
        walk.radius = trip.arriveRadius;
        walk.label  = "the destination";
        trip.legs.push_back(walk);

        if (trip.npcEntry)
        {
            AutopilotLeg touch;
            touch.type   = AutopilotLegType::Approach;
            touch.entry  = trip.npcEntry;
            touch.to     = trip.dest;
            touch.radius = kTouch;
            touch.label  = "talking distance";
            trip.legs.push_back(touch);
        }
        return "";
    }

    enum class Step { Going, Replan, Arrived, Failed };

    Step StepLeg(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, uint32_t now, std::string& note)
    {
        if (trip.leg >= trip.legs.size())
            return Step::Arrived;

        AutopilotLeg& leg = trip.legs[trip.leg];
        switch (leg.type)
        {
            case AutopilotLegType::Walk:
            {
                const float dist = DistTo(bot, leg.to);
                if (dist <= leg.radius)
                    return Step::Arrived;

                if (dist < trip.best - 1.0f)
                {
                    trip.best   = dist;
                    trip.bestAt = now;
                }
                else if (now - trip.bestAt > g_tc.stuckSeconds)
                {
                    if (trip.route.rebuilds < 2)
                    {
                        AzerothFriendRoute::Rebuild(bot, trip.route);
                        trip.issued = SIZE_MAX;
                        trip.bestAt = now;
                        note = "stuck: recalculating route";
                        return Step::Going;
                    }
                    note = "stuck";
                    return Step::Failed;
                }

                if (dist <= kDirectWalk)
                {
                    if (!AzerothFriendMovement::IsMoving(ai))
                        AzerothFriendMovement::MoveTo(ai, leg.to.x, leg.to.y, leg.to.z, true);
                    return Step::Going;
                }

                if (!trip.routed)
                {
                    AzerothFriendRoute::Begin(bot, leg.to.map, leg.to.x, leg.to.y, leg.to.z, trip.route);
                    trip.routed = true;
                }
                AzerothFriendRoute::Extend(bot, trip.route);
                if (trip.route.failed)
                {
                    note = trip.route.why;
                    return Step::Failed;
                }

                AutopilotRoutePoint next;
                size_t nodeIdx = 0;
                if (AzerothFriendRoute::Next(bot, trip.route, next, nodeIdx) && nodeIdx != trip.issued)
                {
                    if (AzerothFriendMovement::MoveTo(ai, next.x, next.y, next.z, true))
                        trip.issued = nodeIdx;
                }
                return Step::Going;
            }

            case AutopilotLegType::Approach:
            {
                Creature* c = bot->GetNPCIfCanInteractWith(ObjectGuid(HighGuid::Unit, leg.entry, 0), UNIT_NPC_FLAG_NONE);
                if (!c)
                {
                    // Search near
                    float bestD = FLT_MAX;
                    Creature* bestC = nullptr;
                    CellCoord p(Acore::ComputeCellCoord(bot->GetPositionX(), bot->GetPositionY()));
                    Cell cell(p);
                    cell.SetNoLoader();
                    Map* map = bot->GetMap();
                    if (map)
                    {
                        for (auto const& [guid, creature] : map->GetCreatureBySpawnIdStore())
                        {
                            if (creature && creature->GetEntry() == leg.entry && creature->IsAlive())
                            {
                                float d = bot->GetDistance(creature);
                                if (d < bestD)
                                {
                                    bestD = d;
                                    bestC = creature;
                                }
                            }
                        }
                    }
                    c = bestC;
                }
                if (!c)
                    return Step::Arrived;

                if (bot->GetDistance(c) <= kTouch)
                {
                    AzerothFriendMovement::Stop(ai);
                    return Step::Arrived;
                }

                if (!AzerothFriendMovement::IsMoving(ai))
                    AzerothFriendMovement::MoveTo(ai, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), true);
                return Step::Going;
            }

            case AutopilotLegType::Fly:
            {
                if (bot->IsInFlight())
                {
                    trip.tookOff = true;
                    return Step::Going;
                }
                if (trip.tookOff)
                {
                    note = SafeFormat("landed at {}", leg.label);
                    trip.flown = true;
                    return Step::Replan;
                }

                if (now - trip.legStartedAt > 30)
                {
                    trip.noFlight = true;
                    note = "flight master unavailable; walking";
                    return Step::Replan;
                }

                Creature* fm = nullptr;
                for (auto const& [guid, creature] : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    if (creature && creature->GetEntry() == leg.entry && creature->IsAlive())
                    {
                        fm = creature;
                        break;
                    }
                }
                if (fm && bot->GetDistance(fm) <= kTouch)
                {
                    if (bot->ActivateTaxiPathTo(leg.taxiPath, fm))
                    {
                        trip.tookOff = true;
                        note = SafeFormat("flying to {}", leg.label);
                        return Step::Going;
                    }
                }
                return Step::Going;
            }

            case AutopilotLegType::Board:
            {
                MotionTransport* t = StandingOn(bot, leg.entry);
                if (t)
                {
                    note = SafeFormat("boarded {}", leg.label);
                    return Step::Arrived;
                }

                if (now - trip.legStartedAt > g_tc.boatWaitMin * 60)
                {
                    note = SafeFormat("tired of waiting for {}", leg.label);
                    return Step::Failed;
                }

                MotionTransport* docked = nullptr;
                Map* map = bot->GetMap();
                if (map)
                {
                    Transport* tr = map->GetTransportForPos(bot->GetPhaseMask(), leg.to.x, leg.to.y, leg.to.z, nullptr);
                    if (tr && tr->GetEntry() == leg.entry)
                        docked = tr->ToMotionTransport();
                }

                if (!DockedAt(trip, docked, leg.to, now))
                {
                    AzerothFriendMovement::Hold(ai, 1500, false);
                    return Step::Going;
                }

                if (!trip.deckProbed)
                {
                    trip.deckProbed = true;
                    trip.deckFound  = Deck(bot, docked, bot->GetPositionZ(), trip.deck);
                }
                if (!trip.deckFound)
                {
                    AzerothFriendMovement::Hold(ai, 1500, false);
                    return Step::Going;
                }

                AzerothFriendMovement::MoveTo(ai, trip.deck.x, trip.deck.y, trip.deck.z, false);
                return Step::Going;
            }

            case AutopilotLegType::Ride:
            {
                MotionTransport* t = StandingOn(bot, leg.entry);
                if (!t)
                {
                    if (DistTo(bot, leg.land) <= 15.0f)
                    {
                        note = SafeFormat("arrived on {}", leg.label);
                        return Step::Replan;
                    }
                    if (trip.boarded)
                    {
                        note = SafeFormat("fell off {}", leg.label);
                        return Step::Replan;
                    }
                }
                else
                    trip.boarded = true;

                if (now - trip.legStartedAt > g_tc.rideMaxMin * 60)
                {
                    note = SafeFormat("ride on {} timed out", leg.label);
                    return Step::Failed;
                }

                if (t && DockedAt(trip, t, leg.arrive, now))
                {
                    AzerothFriendMovement::MoveTo(ai, leg.land.x, leg.land.y, leg.land.z, false);
                    trip.stepping = true;
                    return Step::Going;
                }

                AzerothFriendMovement::Hold(ai, 2000, true);
                return Step::Going;
            }

            case AutopilotLegType::Trigger:
            {
                if (bot->GetMapId() == leg.arrive.map)
                {
                    note = SafeFormat("stepped through {}", leg.label);
                    return Step::Replan;
                }
                if (now - trip.legStartedAt > 30)
                {
                    note = SafeFormat("{} did not trigger teleport", leg.label);
                    return Step::Failed;
                }
                return Step::Going;
            }

            case AutopilotLegType::Portal:
            {
                if (bot->GetMapId() == leg.arrive.map)
                {
                    note = SafeFormat("used portal {}", leg.label);
                    return Step::Replan;
                }
                if (now - trip.legStartedAt > 30)
                {
                    note = SafeFormat("portal {} failed to transport", leg.label);
                    return Step::Failed;
                }

                GameObject* go = nullptr;
                for (auto const& [guid, obj] : bot->GetMap()->GetGameObjectBySpawnIdStore())
                {
                    if (obj && obj->GetEntry() == leg.entry)
                    {
                        go = obj;
                        break;
                    }
                }
                if (go && bot->GetDistance(go) <= kTouch)
                    go->Use(bot);
                return Step::Going;
            }
        }
        return Step::Going;
    }
}

namespace AzerothFriendTravel
{
    void LoadConfig()
    {
        g_tc.flights        = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Travel.Flights", true);
        g_tc.flightMinYards = sConfigMgr->GetOption<float>("AzerothFriend.Autopilot.Travel.FlightMinYards", 700.0f);
        g_tc.boatWaitMin    = sConfigMgr->GetOption<uint32>("AzerothFriend.Autopilot.Travel.BoatWaitMinutes", 20);
        g_tc.rideMaxMin     = sConfigMgr->GetOption<uint32>("AzerothFriend.Autopilot.Travel.RideMaxMinutes", 40);
        g_tc.stuckSeconds   = sConfigMgr->GetOption<uint32>("AzerothFriend.Autopilot.Travel.StuckSeconds", 45);
    }

    void Build()
    {
        g_docks.clear();
        g_crossings.clear();

        for (auto const& [entry, transport] : sTransportMgr->GetTransportTemplates())
        {
            if (transport.keyFrames.size() < 2)
                continue;

            std::vector<size_t> stops;
            for (auto const& kf : transport.keyFrames)
            {
                if (kf.IsStopFrame() && IsContinent(kf.Node->MapID))
                {
                    size_t d = AddDock(kf.Node->MapID, kf.Node->X, kf.Node->Y, kf.Node->Z);
                    stops.push_back(d);
                }
            }

            for (size_t i = 0; i < stops.size(); ++i)
            {
                size_t next = (i + 1) % stops.size();
                if (g_docks[stops[i]].map != g_docks[stops[next]].map)
                {
                    Crossing c;
                    c.kind  = CrossKind::Transport;
                    c.entry = entry;
                    c.name  = transport.name;
                    c.from  = stops[i];
                    c.to    = stops[next];
                    g_crossings.push_back(c);
                }
            }
        }

        for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
        {
            if (IsContinent(teleport.target_mapId))
            {
                AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
                if (at && IsContinent(at->map) && at->map != teleport.target_mapId)
                {
                    Crossing c;
                    c.kind     = CrossKind::Trigger;
                    c.entry    = triggerId;
                    c.name     = "the Dark Portal";
                    c.enter    = { at->map, at->x, at->y, at->z };
                    c.exit     = { teleport.target_mapId, teleport.target_X, teleport.target_Y, teleport.target_Z };
                    c.alliance = true;
                    c.horde    = true;
                    g_crossings.push_back(c);
                }
            }
        }

        LOG_INFO("module.azerothfriend", "Autopilot Travel: indexed {} intercontinental crossings and {} docks.",
                 g_crossings.size(), g_docks.size());
    }

    std::string Start(Player* bot, const AutopilotTravelPoint& dest, float arriveRadius,
                      uint32_t npcEntry, AutopilotTrip& trip, uint32_t now, bool allowFlights)
    {
        trip = AutopilotTrip();
        trip.active       = true;
        trip.dest         = dest;
        trip.arriveRadius = arriveRadius;
        trip.npcEntry     = npcEntry;
        trip.noFlight     = !allowFlights;

        std::string err = Plan(bot, trip, now);
        if (!err.empty())
            trip.active = false;
        return err;
    }

    AutopilotTripState Update(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, uint32_t now,
                              std::string& note)
    {
        if (!trip.active)
            return AutopilotTripState::Failed;

        Step s = StepLeg(bot, ai, trip, now, note);
        if (s == Step::Arrived)
        {
            ++trip.leg;
            if (trip.leg >= trip.legs.size())
            {
                trip.active = false;
                AzerothFriendMovement::Stop(ai);
                return AutopilotTripState::Arrived;
            }
            ResetLeg(trip, now);
            return AutopilotTripState::Going;
        }

        if (s == Step::Replan)
        {
            std::string err = Plan(bot, trip, now);
            if (!err.empty())
            {
                note = err;
                trip.active = false;
                return AutopilotTripState::Failed;
            }
            return AutopilotTripState::Going;
        }

        if (s == Step::Failed)
        {
            if (trip.crossing != SIZE_MAX)
            {
                trip.avoid.push_back(trip.crossing);
                std::string err = Plan(bot, trip, now);
                if (err.empty())
                {
                    note = "crossing unavailable; trying alternate route";
                    return AutopilotTripState::Going;
                }
            }
            trip.active = false;
            AzerothFriendMovement::Stop(ai);
            return AutopilotTripState::Failed;
        }

        return AutopilotTripState::Going;
    }

    void Stop(PlayerbotAI* ai, AutopilotTrip& trip)
    {
        trip.active = false;
        AzerothFriendMovement::Stop(ai);
    }

    std::string Describe(Player* bot, const AutopilotTrip& trip)
    {
        if (!trip.active || trip.leg >= trip.legs.size())
            return "idle";

        const AutopilotLeg& l = trip.legs[trip.leg];
        switch (l.type)
        {
            case AutopilotLegType::Walk:     return SafeFormat("walking to {}", l.label);
            case AutopilotLegType::Approach: return SafeFormat("stepping up to {}", l.label);
            case AutopilotLegType::Fly:      return bot->IsInFlight() ? SafeFormat("flying to {}", l.label) : SafeFormat("taking flight to {}", l.label);
            case AutopilotLegType::Board:    return SafeFormat("waiting to board {}", l.label);
            case AutopilotLegType::Ride:     return SafeFormat("riding {}", l.label);
            case AutopilotLegType::Trigger:  return SafeFormat("stepping through {}", l.label);
            case AutopilotLegType::Portal:   return SafeFormat("taking portal {}", l.label);
        }
        return "travelling";
    }

    bool NeedsFrequentStep(const AutopilotTrip& trip)
    {
        if (!trip.active || trip.leg >= trip.legs.size())
            return false;
        const AutopilotLegType t = trip.legs[trip.leg].type;
        return t == AutopilotLegType::Board || t == AutopilotLegType::Ride;
    }
}
