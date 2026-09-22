#include "AzerothFriendAiControl.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendPlayerbot.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace AzerothFriendAiControl
{
    namespace
    {
        std::unordered_map<uint32, LeaseInfo> _leases;
        std::mutex _mutex;

        PlayerbotAI* GetAi(Player* bot)
        {
#if AF_HAS_PLAYERBOTS
            if (!bot)
                return nullptr;
            return PlayerbotsMgr::instance().GetPlayerbotAI(bot);
#else
            (void)bot;
            return nullptr;
#endif
        }

        std::string JoinStrategies(std::vector<std::string> const& strategies)
        {
            std::ostringstream ss;
            for (size_t i = 0; i < strategies.size(); ++i)
            {
                if (strategies[i].empty())
                    continue;

                if (ss.tellp() > 0)
                    ss << ",";

                ss << "+" << strategies[i];
            }
            return ss.str();
        }

        void SuspendAutonomy(PlayerbotAI* ai, std::string const& strategyMode)
        {
#if AF_HAS_PLAYERBOTS
            if (!ai)
                return;

            if (strategyMode == "clear")
            {
                for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
                    ai->ClearStrategies(static_cast<BotState>(i));
                return;
            }

            if (strategyMode == "passive_stay")
            {
                for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
                    ai->ChangeStrategy("+passive,+stay", static_cast<BotState>(i));
                return;
            }

            // Default "suspend": stop all roaming/questing in the non-combat engine
            // while keeping the class combat rotation fully functional.
            ai->ClearStrategies(BOT_STATE_NON_COMBAT);
            ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
#else
            (void)ai;
            (void)strategyMode;
#endif
        }

        void ApplyLeaseMode(PlayerbotAI* ai, std::string const& mode)
        {
#if AF_HAS_PLAYERBOTS
            if (mode == "combat" || mode == "grind")
            {
                ai->ClearStrategies(BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+grind,+loot,+gather", BOT_STATE_NON_COMBAT);
                ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
                return;
            }

            if (mode == "travel" || mode == "follow")
            {
                ai->SelectiveResetStrategies(BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+travel,+follow,-stay,-emote", BOT_STATE_NON_COMBAT);
                ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
                return;
            }

            if (mode == "idle" || mode == "stay" || mode == "hold")
            {
                ai->SelectiveResetStrategies(BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+stay,-follow", BOT_STATE_NON_COMBAT);
                ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
                return;
            }

            if (mode == "social" || mode == "rpg")
            {
                ai->SelectiveResetStrategies(BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+rpg,-grind", BOT_STATE_NON_COMBAT);
                ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
                return;
            }

            if (mode == "guard")
            {
                ai->ClearStrategies(BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+guard", BOT_STATE_NON_COMBAT);
                ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
                return;
            }

            if (mode == "default")
            {
                ai->SelectiveResetStrategies(BOT_STATE_NON_COMBAT);
                ai->SelectiveResetStrategies(BOT_STATE_COMBAT);
                return;
            }

            // "hold" (or anything unknown): companion owns the bot.
            SuspendAutonomy(ai, sAzerothFriendConfig->leaseStrategyMode);
#else
            (void)ai;
            (void)mode;
#endif
        }
    }

    bool Acquire(Player* bot, uint32 durationSeconds, std::string* reason)
    {
        if (!bot || !bot->IsInWorld())
        {
            if (reason)
                *reason = "bot_not_in_world";
            return false;
        }

        uint32 botGuid = bot->GetGUID().GetCounter();
        time_t now = time(nullptr);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _leases.find(botGuid);
            if (it != _leases.end() && it->second.leased && now < it->second.expires)
            {
                it->second.expires = now + durationSeconds;
                it->second.durationSeconds = durationSeconds;
                return true;
            }
        }

        if (!sAzerothFriendConfig->leaseEnable)
        {
            if (reason)
                *reason = "lease_disabled";
            return false;
        }

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai)
        {
            if (reason)
                *reason = "no_playerbot_ai";
            return false;
        }

        LeaseInfo lease;
        lease.leased = true;
        lease.expires = now + durationSeconds;
        lease.durationSeconds = durationSeconds;
        lease.mode = "hold";
        lease.nonCombat = ai->GetStrategies(BOT_STATE_NON_COMBAT);
        lease.combat = ai->GetStrategies(BOT_STATE_COMBAT);
        lease.dead = ai->GetStrategies(BOT_STATE_DEAD);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _leases[botGuid] = lease;
        }

        SuspendAutonomy(ai, sAzerothFriendConfig->leaseStrategyMode);

        // Stop any movement already queued by the bot's own AI.
        if (bot->GetMotionMaster())
        {
            bot->GetMotionMaster()->Clear(false);
            bot->StopMoving();
        }

        if (sAzerothFriendConfig->debug)
        {
            LOG_DEBUG("server.loading",
                      "[AzerothFriend] Lease acquired for {} ({}s, mode: {}, snapshot: {} NC / {} CBT strategies)",
                      bot->GetName(), durationSeconds, sAzerothFriendConfig->leaseStrategyMode,
                      lease.nonCombat.size(), lease.combat.size());
        }
        return true;
#else
        if (reason)
            *reason = "playerbots_not_compiled";
        return false;
#endif
    }

    bool ApplyMode(Player* bot, std::string const& mode, std::string* reason)
    {
        if (!bot)
        {
            if (reason)
                *reason = "no_bot";
            return false;
        }

        if (!Acquire(bot, sAzerothFriendConfig->leaseRefreshSeconds, reason))
            return false;

        uint32 botGuid = bot->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _leases.find(botGuid);
            if (it != _leases.end())
                it->second.mode = mode;
        }

#if AF_HAS_PLAYERBOTS
        ApplyLeaseMode(GetAi(bot), mode);
#endif
        return true;
    }

    void Release(Player* bot, std::string const& reason, bool retainStrategies)
    {
        if (!bot)
            return;

        uint32 botGuid = bot->GetGUID().GetCounter();
        LeaseInfo lease;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _leases.find(botGuid);
            if (it == _leases.end())
                return;

            lease = it->second;
            _leases.erase(it);
        }

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (ai && lease.leased)
        {
            // If the lease was released due to a mode handoff (e.g. follow, stay, grind),
            // do NOT wipe and revert the active strategies. Retain the newly applied mode as baseline.
            bool isModeHandoff = retainStrategies;
            if (!isModeHandoff)
            {
                BotState states[BOT_STATE_MAX] = { BOT_STATE_COMBAT, BOT_STATE_NON_COMBAT, BOT_STATE_DEAD };
                std::vector<std::string> const* snapshots[BOT_STATE_MAX] = { &lease.combat, &lease.nonCombat, &lease.dead };

                for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
                {
                    ai->ClearStrategies(states[i]);
                    std::string const spec = JoinStrategies(*snapshots[i]);
                    if (!spec.empty())
                        ai->ChangeStrategy(spec, states[i]);
                }
            }

            if (sAzerothFriendConfig->debug)
            {
                LOG_DEBUG("server.loading", "[AzerothFriend] Lease released for {} ({}) - strategies {}",
                          bot->GetName(), reason.empty() ? "plan finished" : reason,
                          isModeHandoff ? "retained" : "restored");
            }
        }
#else
        (void)lease;
#endif
    }

    bool IsLeased(Player* bot)
    {
        if (!bot)
            return false;

        uint32 botGuid = bot->GetGUID().GetCounter();
        time_t now = time(nullptr);
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _leases.find(botGuid);
        if (it == _leases.end() || !it->second.leased)
            return false;

        return now < it->second.expires;
    }

    bool IsLeasedGuid(uint32 botGuid)
    {
        time_t now = time(nullptr);
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _leases.find(botGuid);
        return it != _leases.end() && it->second.leased && now < it->second.expires;
    }

    std::string GetMode(Player* bot)
    {
        if (!bot)
            return "none";

        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _leases.find(bot->GetGUID().GetCounter());
        if (it == _leases.end())
            return "none";

        return it->second.mode;
    }

    std::string Describe(Player* bot)
    {
        if (!bot)
            return "no bot";

        uint32 botGuid = bot->GetGUID().GetCounter();
        time_t now = time(nullptr);

        std::ostringstream ss;
        ss << "bot=" << bot->GetName() << " guid=" << botGuid;

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        ss << " | playerbot_ai=" << (ai ? "yes" : "no");
        if (ai)
        {
            ss << " | strategies NC=" << ai->GetStrategies(BOT_STATE_NON_COMBAT).size()
               << " CBT=" << ai->GetStrategies(BOT_STATE_COMBAT).size()
               << " | supported_actions="
               << (ai->GetAiObjectContext() ? ai->GetAiObjectContext()->GetSupportedActions().size() : 0);
        }
#else
        ss << " | playerbot_ai=not_compiled";
#endif

        uint32 leaseCount = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _leases.find(botGuid);
            if (it != _leases.end() && it->second.leased)
            {
                leaseCount = static_cast<uint32>(now < it->second.expires ? (it->second.expires - now) : 0);
                ss << " | lease=MODE:" << it->second.mode
                   << " remaining=" << leaseCount << "s"
                   << " snapshot(NC=" << it->second.nonCombat.size()
                   << ",CBT=" << it->second.combat.size() << ")";
            }
            else
            {
                ss << " | lease=none";
            }
        }

        return ss.str();
    }

    bool IsInCombatSupremacy(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        if (bot->IsInCombat())
            return true;

#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GetAi(bot))
        {
            if (Player* master = ai->GetMaster())
            {
                if (master->IsInWorld() && master->IsInCombat())
                    return true;
            }
        }
#endif
        return false;
    }

    bool CheckOwnerTether(Player* bot, float maxDistance)
    {
        if (!bot || !bot->IsInWorld())
            return true;

#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GetAi(bot))
        {
            if (Player* master = ai->GetMaster())
            {
                if (!master->IsInWorld())
                    return true;

                // Failsafe 1: If master is mounted, companion should not engage in slow gathering
                if (master->IsMounted())
                    return false;

                // Failsafe 2: If master is in a different map, tether is broken
                if (master->GetMap() != bot->GetMap())
                    return false;

                // Failsafe 3: Distance check
                float dist = bot->GetDistance(master);
                if (dist > maxDistance)
                    return false;
            }
        }
#endif
        return true;
    }

    namespace
    {
        struct MasterIdleState
        {
            float lastX = 0.0f;
            float lastY = 0.0f;
            time_t stationarySince = 0;
        };

        std::unordered_map<uint32, MasterIdleState> _masterIdleStates;
        std::mutex _idleMutex;
    }

    void NotifyMasterMoved(uint32 masterGuid)
    {
        if (!masterGuid)
            return;
        std::lock_guard<std::mutex> lock(_idleMutex);
        _masterIdleStates.erase(masterGuid);
    }

    uint8 GetDowntimeRoutine(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return 0;

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai)
            return 0;

        Player* master = ai->GetMaster();
        if (!master || !master->IsInWorld() || master->IsInCombat() || bot->IsInCombat())
            return 0;

        uint32 masterGuid = master->GetGUID().GetCounter();
        time_t now = time(nullptr);

        {
            std::lock_guard<std::mutex> lock(_idleMutex);
            auto& state = _masterIdleStates[masterGuid];
            float moved = std::sqrt(std::pow(master->GetPositionX() - state.lastX, 2) +
                                    std::pow(master->GetPositionY() - state.lastY, 2));

            if (moved > 3.0f || state.stationarySince == 0)
            {
                state.lastX = master->GetPositionX();
                state.lastY = master->GetPositionY();
                state.stationarySince = now;
                return 0;
            }

            // Must be stationary for at least 30 seconds
            if (now - state.stationarySince < 30)
                return 0;

            // 1 = Inn/City rest (tavern), 2 = Wilderness campfire
            if (master->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
                return 1;

            if (now - state.stationarySince >= 45)
                return 2;
        }
#endif
        return 0;
    }

    void ReleaseExpired()
    {
        time_t now = time(nullptr);
        std::vector<uint32> expired;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (auto const& pair : _leases)
            {
                if (pair.second.leased && now >= pair.second.expires)
                    expired.push_back(pair.first);
            }
        }

        for (uint32 botGuid : expired)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
            Release(bot, "lease expired");
        }
    }

    void ReleaseAll()
    {
        std::vector<uint32> guids;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (auto const& pair : _leases)
                guids.push_back(pair.first);
        }

        for (uint32 botGuid : guids)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
            Release(bot, "module shutdown");
        }
    }
}
