#include "AzerothFriendMovement.h"
#include "AzerothFriendRoute.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Player.h"

#if AF_HAS_PLAYERBOTS
#include "PlayerbotAI.h"
#include "AiObjectContext.h"
#include "LastMovementValue.h"

namespace
{
    constexpr float SHORT_STEP       = 8.0f;
    constexpr float SHORT_STEP_CLIMB = 3.0f;

    LastMovement& LastMove(PlayerbotAI* ai)
    {
        return ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    }
}
#endif

namespace AzerothFriendMovement
{
    bool MoveTo(PlayerbotAI* ai, float x, float y, float z, bool generatePath)
    {
#if AF_HAS_PLAYERBOTS
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot || !bot->IsInWorld() || bot->IsInFlight() || bot->IsBeingTeleported())
            return false;

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        float length = bot->GetExactDist(x, y, z);
        if (!generatePath || length < 3.0f)
        {
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
        }
        else
        {
            auto usable = [&](PathGenerator& p, bool built)
            {
                if (!built || (p.GetPathType() & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH)) ||
                    p.GetPath().size() < 2)
                    return false;
                const float reach = (p.GetPath().back() - p.GetPath().front()).length();
                return reach >= std::min(3.0f, length * 0.5f);
            };

            PathGenerator checked(bot);
            AzerothFriendRoute::Filter(bot, checked);
            checked.SetSlopeCheck(true);
            bool built = checked.CalculatePath(x, y, z, false);
            PathGenerator plain(bot);
            PathGenerator* chosen = &checked;
            if (!usable(checked, built))
            {
                AzerothFriendRoute::Filter(bot, plain);
                const bool plainBuilt = plain.CalculatePath(x, y, z, false);
                if (usable(plain, plainBuilt) && AzerothFriendRoute::ClimbsOnlyWhereWet(bot, plain.GetPath()))
                {
                    built  = true;
                    chosen = &plain;
                }
            }
            PathGenerator& path = *chosen;
            if (!usable(path, built))
            {
                if (!built || length > SHORT_STEP || std::fabs(z - bot->GetPositionZ()) > SHORT_STEP_CLIMB ||
                    !bot->IsWithinLOS(x, y, z))
                    return false;
                bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
            }
            else
            {
                Movement::PointsArray points = path.GetPath();
                if (points.size() == 2)
                    points.insert(points.begin() + 1, (points[0] + points[1]) * 0.5f);
                length = 0.0f;
                for (size_t i = 1; i < points.size(); ++i)
                    length += (points[i] - points[i - 1]).length();
                const G3D::Vector3& end = points.back();
                x = end.x;
                y = end.y;
                z = end.z;
                bot->GetMotionMaster()->MoveSplinePath(&points);
            }
        }

        const float speed = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
        const float delay = length / speed * IN_MILLISECONDS + 500.0f;
        LastMove(ai).Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), delay, MovementPriority::MOVEMENT_NORMAL);
        return true;
#else
        (void)ai; (void)x; (void)y; (void)z; (void)generatePath;
        return false;
#endif
    }

    void Hold(PlayerbotAI* ai, uint32_t ms, bool overCombat)
    {
#if AF_HAS_PLAYERBOTS
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot)
            return;
        LastMove(ai).Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                         bot->GetOrientation(), float(ms),
                         overCombat ? MovementPriority::MOVEMENT_FORCED : MovementPriority::MOVEMENT_NORMAL);
#else
        (void)ai; (void)ms; (void)overCombat;
#endif
    }

    bool IsMoving(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        Player* bot = ai ? ai->GetBot() : nullptr;
        return bot && bot->isMoving();
#else
        (void)ai;
        return false;
#endif
    }

    void Stop(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        if (Player* bot = ai ? ai->GetBot() : nullptr)
            if (bot->isMoving() && !bot->IsInFlight())
                bot->StopMovingOnCurrentPos();
#else
        (void)ai;
#endif
    }

    bool YieldEscort(PlayerbotAI* ai)
    {
#if AF_HAS_PLAYERBOTS
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot || bot->IsInFlight())
            return false;

        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != ESCORT_MOTION_TYPE)
            return false;

        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        LastMove(ai).Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                         bot->GetOrientation(), 0.0f, MovementPriority::MOVEMENT_NORMAL);
        return true;
#else
        (void)ai;
        return false;
#endif
    }

    bool TalkTo(PlayerbotAI* ai, Creature* npc)
    {
#if AF_HAS_PLAYERBOTS
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot || !npc)
            return false;
        bot->SetTarget(npc->GetGUID());
        return ai->DoSpecificAction("talk to quest giver", Event(), true);
#else
        (void)ai; (void)npc;
        return false;
#endif
    }
}
