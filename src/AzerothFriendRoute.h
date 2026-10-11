#ifndef MOD_AZEROTH_FRIEND_ROUTE_H
#define MOD_AZEROTH_FRIEND_ROUTE_H

#include "MoveSplineInitArgs.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;
class PathGenerator;

struct AutopilotRoutePoint
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct AutopilotRoute
{
    uint32_t            map = 0;
    AutopilotRoutePoint dest;

    // Pass 1: corners of the current corridor leg (XY guidance).
    std::vector<AutopilotRoutePoint> corridor;
    size_t   corner        = 0;
    bool     legOpen       = false;
    bool     retryCorridor = false;
    uint32_t stepOffs      = 0;
    uint32_t stalls        = 0;
    float    corridorReach = 250.0f;
    std::vector<AutopilotRoutePoint> anchors;
    size_t   anchor         = 0;
    AutopilotRoutePoint goal;
    bool     goalIsEnd      = true;
    bool     corridorToDest = false;
    AutopilotRoutePoint legStart;

    // Pass 2: where the walk has got to, and the step being tried.
    AutopilotRoutePoint cursor;
    float    reach = 0.0f;
    AutopilotRoutePoint lastKept;
    float    headingX = 0.0f, headingY = 0.0f;
    bool     haveHeading = false;

    // What the bot walks.
    std::vector<AutopilotRoutePoint> nodes;
    size_t   next = 0;

    bool        complete = false;
    bool        failed   = false;
    std::string why;
    std::string anchorNote;

    bool     waiting  = false;
    uint32_t queries  = 0;
    uint32_t rebuilds = 0;

    bool Empty() const { return nodes.empty() && complete; }
};

namespace AzerothFriendRoute
{
    void LoadConfig();
    void Filter(Player* bot, PathGenerator& generator);
    bool ClimbsOnlyWhereWet(Player* bot, const Movement::PointsArray& points);
    bool WalkableNear(Player* bot, float x, float y, AutopilotRoutePoint& out);
    void Begin(Player* bot, uint32_t map, float x, float y, float z, AutopilotRoute& route);
    void Rebuild(Player* bot, AutopilotRoute& route);
    void Extend(Player* bot, AutopilotRoute& route);
    bool Next(Player* bot, AutopilotRoute& route, AutopilotRoutePoint& out, size_t& index);
}

#endif // MOD_AZEROTH_FRIEND_ROUTE_H
