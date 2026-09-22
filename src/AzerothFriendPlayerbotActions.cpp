#include "AzerothFriendPlayerbotActions.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendPlayerbot.h"
#include "Log.h"
#include <algorithm>
#include <set>
#include <sstream>

#if AF_HAS_PLAYERBOTS
    #include "MovementActions.h"
#endif

namespace AzerothFriendPlayerbotActions
{
    namespace
    {
        std::string Trim(std::string const& input)
        {
            size_t begin = input.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return "";

            size_t end = input.find_last_not_of(" \t\r\n");
            return input.substr(begin, end - begin + 1);
        }

        std::string ToLower(std::string input)
        {
            std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) { return std::tolower(c); });
            std::istringstream words(input);
            std::string normalized, word;
            while (words >> word)
                normalized += (normalized.empty() ? "" : " ") + word;
            return normalized;
        }

        bool MatchesList(std::string const& value, std::string const& csv)
        {
            if (value.empty() || csv.empty())
                return false;

            std::string needle = ToLower(Trim(value));
            std::stringstream ss(csv);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                std::string candidate = ToLower(Trim(item));
                if (!candidate.empty() && (candidate == needle || needle.rfind(candidate + " ", 0) == 0))
                    return true;
            }
            return false;
        }

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

#if AF_HAS_PLAYERBOTS
        // Playerbots' MovementAction keeps MoveNear/MoveTo protected; this exposes
        // them so companion steps can reuse the bot's own pathfinding and collision
        // handling instead of fighting it with raw MotionMaster points.
        class AfMoveAction : public MovementAction
        {
        public:
            explicit AfMoveAction(PlayerbotAI* ai) : MovementAction(ai, "af move") {}

            bool MoveToRange(uint32 mapId, float x, float y, float z, float range)
            {
                if (range > 0.5f)
                    return MoveNear(mapId, x, y, z, range, MovementPriority::MOVEMENT_FORCED);

                return MoveTo(mapId, x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED);
            }
        };
#endif
    }

    bool HasPlayerbotAi(Player* bot)
    {
        return GetAi(bot) != nullptr;
    }

    bool DoAction(Player* bot, std::string const& actionName, std::string const& param, bool silent)
    {
        if (!bot || !bot->IsInWorld() || actionName.empty())
            return false;

        if (IsDeniedAction(actionName))
        {
            LOG_WARN("server.loading", "[AzerothFriend] Refusing denied playerbot action '{}' for {}",
                     actionName, bot->GetName());
            return false;
        }

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai)
            return false;

        Event event("", param);
        bool result = ai->DoSpecificAction(actionName, event, silent);
        if (!result && sAzerothFriendConfig->debug)
        {
            LOG_DEBUG("server.loading", "[AzerothFriend] playerbot action '{}' (param '{}') returned false for {}",
                      actionName, param, bot->GetName());
        }
        return result;
#else
        (void)param;
        (void)silent;
        return false;
#endif
    }

    bool DoCommand(Player* bot, std::string const& command)
    {
        if (!bot || !bot->IsInWorld() || command.empty())
            return false;

        if (IsDeniedCommand(command))
        {
            LOG_WARN("server.loading", "[AzerothFriend] Refusing denied playerbot command '{}' for {}",
                     command, bot->GetName());
            return false;
        }

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai)
            return false;

        std::vector<std::string> parts;
        std::string const separator = sPlayerbotAIConfig.commandSeparator;
        if (!separator.empty() && command.find(separator) != std::string::npos)
        {
            std::vector<std::string> dest;
            split(dest, command, separator.c_str());
            parts = dest;
        }
        else
        {
            parts.push_back(command);
        }

        bool dispatched = false;
        for (std::string const& rawPart : parts)
        {
            std::string part = Trim(rawPart);
            if (part.empty())
                continue;

            if (IsDeniedCommand(part))
                return false;

            // Use the bot's human master if available with an active session,
            // which satisfies PlayerbotSecurity checks; fallback to bot.
            Player* sender = ai->GetMaster();
            if (!sender || !sender->GetSession())
                sender = bot;

            ai->HandleCommand(CHAT_MSG_WHISPER, part, sender);
            dispatched = true;
        }

        return dispatched;
#else
        return false;
#endif
    }

    bool IsSupportedAction(Player* bot, std::string const& actionName)
    {
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai || !ai->GetAiObjectContext() || actionName.empty())
            return false;

        return ai->GetAiObjectContext()->GetSupportedActions().count(actionName) > 0;
#else
        (void)bot;
        (void)actionName;
        return false;
#endif
    }

    bool IsDeniedAction(std::string const& actionName)
    {
        return MatchesList(actionName, sAzerothFriendConfig->deniedActions);
    }

    bool IsDeniedCommand(std::string const& command)
    {
        if (command.find('\n') != std::string::npos || command.find('\r') != std::string::npos)
            return true;
        std::string probe = ToLower(Trim(command));
        if (probe.empty())
            return false;

        // Unwrap native action execution aliases so they cannot bypass policy.
        if (probe.rfind("do ", 0) == 0)
            return IsDeniedAction(probe.substr(3)) || IsDeniedCommand(probe.substr(3));
        if (probe.find('\n') != std::string::npos || probe.find('\r') != std::string::npos)
            return true;
#if AF_HAS_PLAYERBOTS
        std::string separator = sPlayerbotAIConfig.commandSeparator;
        if (!separator.empty() && probe.find(separator) != std::string::npos)
        {
            size_t start = 0, end;
            while ((end = probe.find(separator, start)) != std::string::npos)
            {
                if (IsDeniedCommand(probe.substr(start, end - start)))
                    return true;
                start = end + separator.size();
            }
            return IsDeniedCommand(probe.substr(start));
        }
#endif

        if (MatchesList(probe, sAzerothFriendConfig->deniedCommands))
            return true;

        // Match the leading verb too ("guild remove X" -> "guild remove").
        size_t space = probe.find(' ');
        if (space != std::string::npos && MatchesList(probe.substr(0, space), sAzerothFriendConfig->deniedCommands))
            return true;

        return false;
    }

    std::vector<std::string> SupportedActions(Player* bot)
    {
        std::vector<std::string> result;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai || !ai->GetAiObjectContext())
            return result;

        std::set<std::string> const supported = ai->GetAiObjectContext()->GetSupportedActions();
        result.assign(supported.begin(), supported.end());
#else
        (void)bot;
#endif
        return result;
    }

    std::vector<std::string> SupportedStrategies(Player* bot)
    {
        std::vector<std::string> result;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai || !ai->GetAiObjectContext())
            return result;

        std::set<std::string> const supported = ai->GetAiObjectContext()->GetSupportedStrategies();
        result.assign(supported.begin(), supported.end());
#else
        (void)bot;
#endif
        return result;
    }

    bool MoveToCoords(Player* bot, float x, float y, float z, float range)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
            std::isinf(x) || std::isinf(y) || std::isinf(z))
        {
            return false;
        }

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (ai)
        {
            AfMoveAction mover(ai);
            if (mover.MoveToRange(bot->GetMapId(), x, y, z, range))
                return true;
        }
#endif
        return false;
    }

    bool FollowMaster(Player* bot)
    {
        if (!bot)
            return false;

        if (DoAction(bot, "follow"))
            return true;

        return ChangeStrategies(bot, "+follow,-stay", false);
    }

    bool StopMovement(Player* bot)
    {
        if (!bot)
            return false;
        return DoAction(bot, "stay") || ChangeStrategies(bot, "+stay,-follow", false);
    }

    bool ChangeStrategies(Player* bot, std::string const& spec, bool combat)
    {
        if (!bot || spec.empty())
            return false;

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai)
            return false;

        ai->ChangeStrategy(spec, combat ? BOT_STATE_COMBAT : BOT_STATE_NON_COMBAT);
        return true;
#else
        (void)combat;
        return false;
#endif
    }

    std::string Describe(Player* bot)
    {
        std::ostringstream ss;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = GetAi(bot);
        if (!ai)
        {
            ss << "no playerbot AI";
            return ss.str();
        }

        ss << "supported_actions=" << SupportedActions(bot).size()
           << " supported_strategies=" << SupportedStrategies(bot).size()
           << " command_separator='" << sPlayerbotAIConfig.commandSeparator << "'";
#else
        (void)bot;
        ss << "mod-playerbots not compiled in";
#endif
        return ss.str();
    }
}
