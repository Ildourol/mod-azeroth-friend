#ifndef MOD_AZEROTH_FRIEND_TRAVEL_H
#define MOD_AZEROTH_FRIEND_TRAVEL_H

#include "AzerothFriendRoute.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;
#if AF_HAS_PLAYERBOTS
class PlayerbotAI;
#else
class PlayerbotAI {};
#endif

struct AutopilotTravelPoint
{
    uint32_t map = 0;
    float    x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class AutopilotLegType : uint8_t
{
    Walk,
    Fly,
    Board,
    Ride,
    Approach,
    Trigger,
    Portal
};

struct AutopilotLeg
{
    AutopilotLegType      type = AutopilotLegType::Walk;
    AutopilotTravelPoint  to;
    float                 radius = 6.0f;
    uint32_t              entry  = 0;
    std::vector<uint32_t> taxiPath;
    AutopilotTravelPoint  arrive;
    AutopilotTravelPoint  land;
    std::string           label;
};

struct AutopilotTrip
{
    bool                 active = false;
    AutopilotTravelPoint dest;
    float                arriveRadius = 25.0f;
    uint32_t             npcEntry = 0;

    std::vector<AutopilotLeg> legs;
    size_t               leg = 0;
    uint32_t             legStartedAt = 0;
    uint32_t             plans = 0;
    bool                 noFlight = false;
    bool                 flown = false;

    // Walk leg state.
    bool                 routed = false;
    AutopilotRoute       route;
    size_t               issued = SIZE_MAX;
    float                best = 0.0f;
    size_t               lastReached = 0;
    uint32_t             bestAt = 0;

    // Vehicle state.
    bool                 tookOff = false;
    bool                 boarded = false;
    bool                 stepping = false;
    bool                 deckProbed = false;
    bool                 deckFound  = false;
    AutopilotTravelPoint deck;

    float                tx = 0.0f, ty = 0.0f;
    uint32_t             tSampleAt = 0;

    bool                 interrupted = false;

    size_t               crossing = SIZE_MAX;
    std::vector<size_t>  avoid;
};

enum class AutopilotTripState : uint8_t { Going, Arrived, Failed };

namespace AzerothFriendTravel
{
    void LoadConfig();
    void Build();
    std::string Start(Player* bot, const AutopilotTravelPoint& dest, float arriveRadius,
                      uint32_t npcEntry, AutopilotTrip& trip, uint32_t now, bool allowFlights = true);
    AutopilotTripState Update(Player* bot, PlayerbotAI* ai, AutopilotTrip& trip, uint32_t now,
                              std::string& note);
    void Stop(PlayerbotAI* ai, AutopilotTrip& trip);
    std::string Describe(Player* bot, const AutopilotTrip& trip);
    bool NeedsFrequentStep(const AutopilotTrip& trip);
}

#endif // MOD_AZEROTH_FRIEND_TRAVEL_H
