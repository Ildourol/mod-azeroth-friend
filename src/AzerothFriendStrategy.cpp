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

    bool IsTravelling(Player* bot)
    {
        std::lock_guard<std::mutex> lock(g_travelMutex);
        return g_travelling.count(bot->GetGUID().GetRawValue()) > 0;
    }

    class AutopilotHoldMultiplier : public Multiplier
    {
    public:
        explicit AutopilotHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "af hold") { }

        float GetValue(Action* action) override
        {
            if (!action)
                return 1.0f;
            const std::string name = action->getName();

            if (name == "attack anything" || name == "move random")
            {
                Player* bot = botAI->GetBot();
                return bot && IsTravelling(bot) ? 0.0f : 1.0f;
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
                Player* bot = botAI->GetBot();
                if (bot && !bot->GetGroup() && !bot->InBattleground() && bot->GetMap() &&
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
            ai->ChangeStrategy(std::string("+") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
#else
        (void)bot;
#endif
    }

    void AutopilotStrategy_Remove(Player* bot)
    {
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->ChangeStrategy(std::string("-") + AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
#else
        (void)bot;
#endif
    }
}

