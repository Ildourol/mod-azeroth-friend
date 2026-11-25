#include "AzerothFriendStrategy.h"
#include "Log.h"
#include "Player.h"

#if AF_HAS_PLAYERBOTS
#include "AiObjectContext.h"
#include "NamedObjectContext.h"
#include "Creature.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "PlayerbotAI.h"
#include "Action.h"
#include "Map.h"
#include "Multiplier.h"
#include "Strategy.h"

#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "DKAiObjectContext.h"
#include "DruidAiObjectContext.h"
#include "HunterAiObjectContext.h"
#include "MageAiObjectContext.h"
#include "PaladinAiObjectContext.h"
#include "PriestAiObjectContext.h"
#include "RogueAiObjectContext.h"
#include "ShamanAiObjectContext.h"
#include "WarlockAiObjectContext.h"
#include "WarriorAiObjectContext.h"

namespace
{
    std::atomic<bool> g_noHandouts{ true };
    std::atomic<bool> g_modelGroups{ true };

    std::mutex                   g_travelMutex;
    std::unordered_set<uint64_t> g_travelling;

    std::mutex                   g_enrolledMutex;
    std::unordered_set<uint64_t> g_enrolledBots;

    bool IsTravelling(Player* bot)
    {
        std::lock_guard<std::mutex> lock(g_travelMutex);
        return g_travelling.count(bot->GetGUID().GetRawValue()) > 0;
    }

    bool IsBotEnrolledInternal(uint64_t botGuid)
    {
        std::lock_guard<std::mutex> lock(g_enrolledMutex);
        return g_enrolledBots.count(botGuid) > 0;
    }

    class AutopilotHoldMultiplier : public Multiplier
    {
    public:
        explicit AutopilotHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "af hold") { }

        float GetValue(Action* action) override
        {
            if (!action)
                return 1.0f;

            Player* bot = botAI->GetBot();
            if (!bot)
                return 1.0f;

            // Per-bot enrollment check: only enrolled bots are subject to hold multipliers
            if (!IsBotEnrolledInternal(bot->GetGUID().GetRawValue()))
                return 1.0f;

            const std::string name = action->getName();

            if (name == "attack anything" || name == "move random")
            {
                return IsTravelling(bot) ? 0.0f : 1.0f;
            }

            if (g_modelGroups.load(std::memory_order_relaxed) &&
                (name == "accept invitation" || name == "invite nearby" || name == "invite guild" ||
                 name == "leave far away"))
                return 0.0f;

            if (!g_noHandouts.load(std::memory_order_relaxed))
                return 1.0f;

            if (name == "auto maintenance on levelup" || name == "random bot update" || name == "lfg accept")
                return 0.0f;

            if (name == "auto release" || name == "release" || name == "spirit healer")
            {
                if (!bot->GetGroup() && !bot->InBattleground() && bot->GetMap() &&
                    !bot->GetMap()->Instanceable())
                    return 0.0f;
            }
            return 1.0f;
        }
    };

    class AutopilotMarkerStrategy : public Strategy
    {
    public:
        explicit AutopilotMarkerStrategy(PlayerbotAI* botAI) : Strategy(botAI) { }

        std::string const getName() override { return AUTOPILOT_STRATEGY_NAME; }
        uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }

        void InitMultipliers(std::vector<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new AutopilotHoldMultiplier(botAI));
        }
    };

    class AutopilotStrategyContext : public NamedObjectContext<Strategy>
    {
    public:
        AutopilotStrategyContext()
        {
            creators[AUTOPILOT_STRATEGY_NAME] = &AutopilotStrategyContext::autopilot;
        }

    private:
        static Strategy* autopilot(PlayerbotAI* botAI) { return new AutopilotMarkerStrategy(botAI); }
    };

    bool g_registered = false;

    void AddTo(SharedNamedObjectContextList<Strategy>& list)
    {
        if (list.creators.count(AUTOPILOT_STRATEGY_NAME) == 0)
            list.Add(new AutopilotStrategyContext());
    }

    bool In(SharedNamedObjectContextList<Strategy> const& list)
    {
        return list.creators.count(AUTOPILOT_STRATEGY_NAME) != 0;
    }
}
#endif

namespace AzerothFriendStrategy
{
    bool Register()
    {
#if AF_HAS_PLAYERBOTS
        AiObjectContext::BuildAllSharedContexts();

        AddTo(DKAiObjectContext::sharedStrategyContexts);
        AddTo(DruidAiObjectContext::sharedStrategyContexts);
        AddTo(HunterAiObjectContext::sharedStrategyContexts);
        AddTo(MageAiObjectContext::sharedStrategyContexts);
        AddTo(PaladinAiObjectContext::sharedStrategyContexts);
        AddTo(PriestAiObjectContext::sharedStrategyContexts);
        AddTo(RogueAiObjectContext::sharedStrategyContexts);
        AddTo(ShamanAiObjectContext::sharedStrategyContexts);
        AddTo(WarlockAiObjectContext::sharedStrategyContexts);
        AddTo(WarriorAiObjectContext::sharedStrategyContexts);

        g_registered =
            In(DKAiObjectContext::sharedStrategyContexts) &&
            In(DruidAiObjectContext::sharedStrategyContexts) &&
            In(HunterAiObjectContext::sharedStrategyContexts) &&
            In(MageAiObjectContext::sharedStrategyContexts) &&
            In(PaladinAiObjectContext::sharedStrategyContexts) &&
            In(PriestAiObjectContext::sharedStrategyContexts) &&
            In(RogueAiObjectContext::sharedStrategyContexts) &&
            In(ShamanAiObjectContext::sharedStrategyContexts) &&
            In(WarlockAiObjectContext::sharedStrategyContexts) &&
            In(WarriorAiObjectContext::sharedStrategyContexts);

        if (g_registered)
            LOG_INFO("module.azerothfriend", "Registered the '{}' playerbots strategy.",
                     AUTOPILOT_STRATEGY_NAME);
        else
            LOG_ERROR("module.azerothfriend",
                      "Could not register the '{}' playerbots strategy in every class context.",
                      AUTOPILOT_STRATEGY_NAME);

        return g_registered;
#else
        return false;
#endif
    }

    bool IsRegistered()
    {
#if AF_HAS_PLAYERBOTS
        return g_registered;
#else
        return false;
#endif
    }

    void SetNoHandouts(bool on)
    {
#if AF_HAS_PLAYERBOTS
        g_noHandouts.store(on, std::memory_order_relaxed);
#else
        (void)on;
#endif
    }

    bool NoHandouts()
    {
#if AF_HAS_PLAYERBOTS
        return g_noHandouts.load(std::memory_order_relaxed);
#else
        return true;
#endif
    }

    void SetModelGroups(bool on)
    {
#if AF_HAS_PLAYERBOTS
        g_modelGroups.store(on, std::memory_order_relaxed);
#else
        (void)on;
#endif
    }

    bool ModelGroups()
    {
#if AF_HAS_PLAYERBOTS
        return g_modelGroups.load(std::memory_order_relaxed);
#else
        return true;
#endif
    }

    void SetTravelling(uint64_t botGuid, bool on)
    {
#if AF_HAS_PLAYERBOTS
        std::lock_guard<std::mutex> lock(g_travelMutex);
        if (on)
            g_travelling.insert(botGuid);
        else
            g_travelling.erase(botGuid);
#else
        (void)botGuid; (void)on;
#endif
    }

    void SetQuestTarget(PlayerbotAI* ai, uint64_t targetGuid)
    {
#if AF_HAS_PLAYERBOTS
        if (ai)
            ai->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({ ObjectGuid(targetGuid) });
#else
        (void)ai; (void)targetGuid;
#endif
    }

    void ClearQuestTarget(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        if (ai)
            ai->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Reset();
#else
        (void)ai;
#endif
    }

    bool EngageQuestTarget(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        return ai && ai->DoSpecificAction("attack anything", Event(), true);
#else
        (void)ai;
        return false;
#endif
    }

    void LootObject(PlayerbotAI* ai, uint64_t objectGuid)
    {
#if AF_HAS_PLAYERBOTS
        if (!ai)
            return;
        if (LootObjectStack* stack = ai->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get())
            stack->Add(ObjectGuid(objectGuid));
        if (!ai->HasStrategy("loot", BOT_STATE_NON_COMBAT))
        {
            ai->DoSpecificAction("loot", Event(), true);
            ai->DoSpecificAction("open loot", Event(), true);
        }
#else
        (void)ai; (void)objectGuid;
#endif
    }

    bool OpenObject(PlayerbotAI* ai, uint64_t objectGuid)
    {
#if AF_HAS_PLAYERBOTS
        if (!ai)
            return false;
        if (LootObjectStack* stack = ai->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get())
            stack->Add(ObjectGuid(objectGuid));
        return ai->DoSpecificAction("open loot", Event(), true);
#else
        (void)ai; (void)objectGuid;
        return false;
#endif
    }

    void ClearDeathCount(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        if (!ai)
            return;
        auto* deaths = ai->GetAiObjectContext()->GetValue<uint32>("death count");
        if (deaths && deaths->Get())
            deaths->Set(0);
#else
        (void)ai;
#endif
    }

    void AutopilotStrategy_Apply(Player* bot)
    {
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            for (BotState state : { BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT, BOT_STATE_DEAD })
            {
                if (!ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, state))
                    ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, state);
            }
        }
#else
        (void)bot;
#endif
    }

    void AutopilotStrategy_Remove(Player* bot)
    {
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            for (BotState state : { BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT, BOT_STATE_DEAD })
            {
                if (ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, state))
                    ai->ChangeStrategy(std::string("-") + AUTOPILOT_STRATEGY_NAME, state);
            }
        }
#else
        (void)bot;
#endif
    }

    bool IsBotEnrolled(uint64_t botGuid)
    {
        return IsBotEnrolledInternal(botGuid);
    }

    void SetBotEnrolled(uint64_t botGuid, bool enrolled)
    {
        std::lock_guard<std::mutex> lock(g_enrolledMutex);
        if (enrolled)
            g_enrolledBots.insert(botGuid);
        else
            g_enrolledBots.erase(botGuid);
    }

    void ClearEnrolledBots()
    {
        std::lock_guard<std::mutex> lock(g_enrolledMutex);
        g_enrolledBots.clear();
    }

    std::string CaptureStrategies(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        if (!ai)
            return "";
        auto join = [](const std::vector<std::string>& v)
        {
            std::string s;
            for (const std::string& n : v)
            {
                if (n != AUTOPILOT_STRATEGY_NAME)
                    s += (s.empty() ? "" : ",") + n;
            }
            return s;
        };
        return "nc:" + join(ai->GetStrategies(BOT_STATE_NON_COMBAT)) + ";co:" +
               join(ai->GetStrategies(BOT_STATE_COMBAT));
#else
        (void)ai;
        return "";
#endif
    }

    void RestoreEngine(PlayerbotAI* ai, const std::string& baseline, BotState state)
    {
#if AF_HAS_PLAYERBOTS
        if (!ai)
            return;
        const std::string prefix = state == BOT_STATE_COMBAT ? "co:" : "nc:";
        std::unordered_set<std::string> want;
        bool found = false;

        std::stringstream ss(baseline);
        std::string part;
        while (std::getline(ss, part, ';'))
        {
            if (part.rfind(prefix, 0) == 0)
            {
                found = true;
                std::stringstream partStream(part.substr(3));
                std::string n;
                while (std::getline(partStream, n, ','))
                {
                    if (!n.empty())
                        want.insert(n);
                }
            }
        }
        if (!found)
            return;

        for (const std::string& n : ai->GetStrategies(state))
        {
            if (n != AUTOPILOT_STRATEGY_NAME && !want.count(n))
                ai->ChangeStrategy("-" + n, state);
        }
        for (const std::string& n : want)
        {
            if (!ai->HasStrategy(n, state))
                ai->ChangeStrategy("+" + n, state);
        }
#else
        (void)ai; (void)baseline; (void)state;
#endif
    }
}

