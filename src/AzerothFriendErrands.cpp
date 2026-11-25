#include "AzerothFriendErrands.h"
#include "AzerothFriendStrategy.h"
#include "AzerothFriendTravel.h"
#include "AzerothFriendMovement.h"
#include "AzerothFriendRoute.h"
#include "AzerothFriendWorldIndex.h"
#include "AzerothFriendUtilities.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Mail.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Opcodes.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "SharedDefines.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#if AF_HAS_PLAYERBOTS
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#endif

#include <algorithm>
#include <cctype>
#include <climits>
#include <list>
#include <map>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    const char* const kDefaultReference =
        "You are their controller: nothing else decides where they go or what they set out to do. Without "
        "orders they stay where they are and only do what their strategies make them do around them.\n"
        "STRATEGIES:\n"
        "- nc +name,-name : out-of-combat behaviours (grind, quest, loot, gather, food, mount, lfg, bg, pvp, maintenance)\n"
        "- co +name,-name : combat behaviours (flee, potions, avoid aoe, aggressive, threat, kite, save mana)\n"
        "ORDERS:\n"
        "- goto <service> : repair, vendor, trainer, profession, inn, flightmaster, bank, auction, mailbox\n"
        "- goto profession <name> : go to that profession's trainer and learn skills/recipes\n"
        "- goto <name> : walk up to an NPC or player\n"
        "- goto zone <zone name> : travel to a zone anywhere in the world\n"
        "- goto hunt : hunt level-appropriate monsters nearby\n"
        "- quest <id> [reward <n>] : work objective or turn in completed quest\n"
        "- abandon <id> : drop a quest from log\n"
        "- talents spec <name> : follow talent spec and spend points\n"
        "- craft <recipe> [count|all] : craft item at appropriate station\n"
        "- ah look/sell/buy <item> [price] : interact with auction house\n"
        "- disenchant <item> : disenchant bag item\n"
        "- open <object> : open chest/node/object\n"
        "- group invite/accept/decline/leave/share <name|id>\n"
        "- whisper <name> <text>\n"
        "ORDER OF ORDERS: orders run in order. Subsequent orders queue until current travel arrives.\n";

    const char* const kDefaultDenied =
        "logout,reset,destroy,teleport,summon,cheat,debug,cdebug,wipe,sendmail,mail,hire,give leader,"
        "guild remove,guild demote,guild promote,guild leave,log,d,do,release,leave,rpg";

    std::string              g_reference;
    uint32_t                 g_huntMinutes = 20;
    std::vector<std::string> g_denied;

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string Trim(std::string s)
    {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    bool StartsWithWord(const std::string& text, const std::string& word)
    {
        return text == word || (text.size() > word.size() && text.compare(0, word.size(), word) == 0 &&
                                text[word.size()] == ' ');
    }

    bool Denied(const std::string& command)
    {
        return std::any_of(g_denied.begin(), g_denied.end(),
                           [&](const std::string& d) { return StartsWithWord(command, d); });
    }

    std::string TakeWord(std::string& rest)
    {
        rest = Trim(rest);
        const size_t space = rest.find(' ');
        std::string word = rest.substr(0, space);
        rest = space == std::string::npos ? std::string() : Trim(rest.substr(space + 1));
        return word;
    }

    Player* FindPlayerByName(std::string name)
    {
        while (!name.empty() && std::string(",.:;!?").find(name.back()) != std::string::npos)
            name.pop_back();
        if (!normalizePlayerName(name))
            return nullptr;
        return ObjectAccessor::FindPlayerByName(name, true);
    }

    std::string StartErrand(Player* bot, AutopilotErrand& errand, AutopilotErrandKind kind, uint8_t service,
                            uint32_t npcEntry, uint32_t questId, const AutopilotPlace& place, float radius,
                            const std::string& label, uint32_t now)
    {
        AutopilotErrand next;
        next.kind      = kind;
        next.service   = service;
        next.npcEntry  = npcEntry;
        next.questId   = questId;
        next.label     = label;
        next.startedAt = now;

        const std::string why = AzerothFriendTravel::Start(bot, { place.map, place.x, place.y, place.z }, radius,
                                                           npcEntry, next.trip, now);
        if (!why.empty())
            return why;

        next.active = true;
        errand      = std::move(next);
        return place.map == bot->GetMapId()
            ? SafeFormat("on the way to {} ({} yd)", label, uint32_t(bot->GetDistance(place.x, place.y, place.z)))
            : SafeFormat("on the way to {} (another continent)", label);
    }

    bool ExploreNext(Player* bot, AutopilotErrand& errand, uint32_t now)
    {
        if (!errand.exploreLeft)
            return false;
        --errand.exploreLeft;
        for (int k = 0; k < 8; ++k)
        {
            const float a = errand.exploreAngle + float(k) * float(M_PI) / 4.0f;
            const float x = bot->GetPositionX() + 120.0f * std::cos(a);
            const float y = bot->GetPositionY() + 120.0f * std::sin(a);
            AutopilotRoutePoint p;
            if (AzerothFriendRoute::WalkableNear(bot, x, y, p))
            {
                errand.exploreAngle = a + float(M_PI) * 0.75f;
                return AzerothFriendTravel::Start(bot, { bot->GetMapId(), p.x, p.y, p.z }, 6.0f, 0,
                                                  errand.trip, now, false).empty();
            }
        }
        return false;
    }
}

namespace AzerothFriendErrands
{
    void LoadConfig()
    {
        g_huntMinutes = sConfigMgr->GetOption<uint32>("AzerothFriend.Autopilot.QuestHuntMinutes", 20);
        g_reference   = sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.CommandReference", "");
        if (g_reference.empty())
            g_reference = kDefaultReference;

        std::string deniedStr = sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.DeniedCommands", kDefaultDenied);
        g_denied.clear();
        std::stringstream ss(deniedStr);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            item = Trim(Lower(item));
            if (!item.empty())
                g_denied.push_back(item);
        }
    }

    const std::string& Reference()
    {
        return g_reference;
    }

    std::string Normalize(const std::string& raw)
    {
        std::string s = Trim(raw);
        if (!s.empty() && s[0] == '/')
            s.erase(0, 1);
        return Trim(s);
    }

    bool IsStrategyChange(const std::string& command)
    {
        const std::string c = Lower(command);
        return StartsWithWord(c, "nc") || StartsWithWord(c, "co");
    }

    bool IsErrand(const std::string& command)
    {
        const std::string c = Lower(command);
        return StartsWithWord(c, "goto") || StartsWithWord(c, "quest") || StartsWithWord(c, "craft") ||
               StartsWithWord(c, "open");
    }

    bool IsDenied(const std::string& command)
    {
        return Denied(Lower(command));
    }

    std::string Run(Player* bot, PlayerbotAI* ai, const std::string& rawCommand,
                    AutopilotErrand& errand, uint32_t now)
    {
        const std::string command = Normalize(rawCommand);
        if (command.empty())
            return "empty order";

        if (IsDenied(command))
            return "denied by server policy";

        std::string rest = command;
        std::string verb = Lower(TakeWord(rest));

        // 1. Strategy changes: nc / co
        if (verb == "nc" || verb == "co")
        {
#if AF_HAS_PLAYERBOTS
            if (ai)
            {
                ai->ChangeStrategy(rest, verb == "co" ? BOT_STATE_COMBAT : BOT_STATE_NON_COMBAT);
                return "done";
            }
#endif
            return "strategy system not available";
        }

        // 2. Travel & Goto orders
        if (verb == "goto")
        {
            std::string target = TakeWord(rest);
            std::string targetLower = Lower(target);

            AutopilotService s;
            if (AutopilotService_FromName(targetLower, s))
            {
                AutopilotPlace p;
                if (!AzerothFriendWorldIndex::NearestService(bot, s, p))
                    return SafeFormat("no friendly {} found on this map", AutopilotService_Name(s));
                return StartErrand(bot, errand, AutopilotErrandKind::Service, static_cast<uint8_t>(s),
                                   p.entry, 0, p, 4.0f, p.name, now);
            }

            if (targetLower == "profession")
            {
                std::string profName = rest;
                std::string display;
                uint32_t skillId = AzerothFriendWorldIndex::ProfessionSkill(profName, display);
                if (!skillId)
                    return "which profession? Example: goto profession mining";
                AutopilotPlace p;
                if (!AzerothFriendWorldIndex::NearestProfessionTrainer(bot, skillId, p))
                    return SafeFormat("no {} trainer found on this continent", display);
                return StartErrand(bot, errand, AutopilotErrandKind::Service,
                                   static_cast<uint8_t>(AutopilotService::Profession),
                                   p.entry, 0, p, 4.0f, SafeFormat("the {} trainer {}", display, p.name), now);
            }

            if (targetLower == "zone")
            {
                std::string display;
                uint32_t zoneId = AzerothFriendWorldIndex::FindZone(rest, display);
                if (!zoneId)
                    return "unknown zone";
                AutopilotPlace p;
                std::string why;
                if (!AzerothFriendWorldIndex::ZonePlace(bot, zoneId, p, why))
                    return why;
                return StartErrand(bot, errand, AutopilotErrandKind::Place, 0, 0, 0, p, 15.0f, display, now);
            }

            if (targetLower == "hunt")
            {
                AutopilotPlace p;
                if (AzerothFriendWorldIndex::HuntingGround(bot, p))
                    return StartErrand(bot, errand, AutopilotErrandKind::Place, 0, 0, 0, p, 15.0f,
                                       SafeFormat("hunting ground: {}", p.name), now);
                // Explore fallback
                AutopilotErrand next;
                next.kind        = AutopilotErrandKind::Place;
                next.label       = "exploring for monsters";
                next.startedAt   = now;
                next.exploring   = true;
                next.exploreLeft = 6;
                if (ExploreNext(bot, next, now))
                {
                    next.active = true;
                    errand = std::move(next);
                    return "exploring for monsters nearby";
                }
                return "no monsters found to hunt";
            }

            if (targetLower == "mailbox")
            {
                AutopilotPlace p;
                if (!AzerothFriendWorldIndex::NearestMailbox(bot, p))
                    return "no mailbox found on this map";
                return StartErrand(bot, errand, AutopilotErrandKind::Mailbox, 0, 0, 0, p, 4.0f, "the mailbox", now);
            }

            // NPC or Player name
            std::string fullName = target + (rest.empty() ? "" : " " + rest);
            Player* player = FindPlayerByName(fullName);
            if (player && player->GetMapId() == bot->GetMapId())
            {
                AutopilotPlace p{ player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                                  player->GetPositionZ(), 0, player->GetName(), bot->GetDistance(player) };
                return StartErrand(bot, errand, AutopilotErrandKind::Npc, 0, 0, 0, p, 6.0f, player->GetName(), now);
            }

            AutopilotPlace npcPlace;
            if (AzerothFriendWorldIndex::FindNpc(bot, fullName, npcPlace))
                return StartErrand(bot, errand, AutopilotErrandKind::Npc, 0, npcPlace.entry, 0, npcPlace, 4.0f,
                                   npcPlace.name, now);

            return SafeFormat("could not find '{}' to go to", fullName);
        }

        // 3. Quest orders
        if (verb == "quest")
        {
            std::string idStr = TakeWord(rest);
            uint32_t questId = std::strtoul(idStr.c_str(), nullptr, 10);
            if (!questId)
                return "specify quest id: quest <id>";

            Quest const* q = sObjectMgr->GetQuestTemplate(questId);
            if (!q || bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                return "that quest is not in the quest log";

            if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
            {
                const auto& enders = AzerothFriendWorldIndex::QuestEnders(questId);
                if (enders.empty())
                    return "do not know who completes this quest";

                AutopilotPlace bestP;
                float bestD = FLT_MAX;
                for (uint32_t ender : enders)
                {
                    AutopilotPlace p;
                    if (AzerothFriendWorldIndex::NearestSpawn(bot, ender, p))
                    {
                        if (p.map == bot->GetMapId() && p.distance < bestD)
                        {
                            bestD = p.distance;
                            bestP = p;
                        }
                    }
                }
                if (bestD == FLT_MAX)
                    AzerothFriendWorldIndex::NearestSpawn(bot, enders.front(), bestP);

                return StartErrand(bot, errand, AutopilotErrandKind::QuestTurnIn, 0, bestP.entry, questId,
                                   bestP, 4.0f, SafeFormat("turn in quest: {}", q->GetTitle()), now);
            }

            // Incomplete quest objective
            AutopilotPlace objPlace;
            if (AzerothFriendWorldIndex::NearestSpawn(bot, q->RequiredNpcOrGo[0] > 0 ? q->RequiredNpcOrGo[0] : 0, objPlace))
            {
                return StartErrand(bot, errand, AutopilotErrandKind::QuestObjective, 0, 0, questId,
                                   objPlace, 15.0f, SafeFormat("objective for: {}", q->GetTitle()), now);
            }
            return "heading toward quest objective";
        }

        if (verb == "abandon")
        {
            uint32_t questId = std::strtoul(rest.c_str(), nullptr, 10);
            if (!questId)
                return "specify quest id: abandon <id>";
            bot->TakeQuestSourceItem(questId, true);
            bot->SetQuestStatus(questId, QUEST_STATUS_NONE);
            return "done: abandoned quest";
        }

        // 4. Group & Whisper orders
        if (verb == "group")
        {
            std::string sub = Lower(TakeWord(rest));
            if (sub == "invite")
            {
                Player* target = FindPlayerByName(rest);
                if (!target)
                    return "player not found";
                WorldPacket p(CMSG_GROUP_INVITE, target->GetName().size() + 5);
                p << target->GetName() << uint32(0);
                bot->GetSession()->HandleGroupInviteOpcode(p);
                return "done: invited " + target->GetName();
            }
            if (sub == "accept")
            {
                WorldPacket p(CMSG_GROUP_ACCEPT, 4);
                p << uint32(0);
                bot->GetSession()->HandleGroupAcceptOpcode(p);
                return "done: accepted group invite";
            }
            if (sub == "decline")
            {
                WorldPacket p(CMSG_GROUP_DECLINE, 0);
                bot->GetSession()->HandleGroupDeclineOpcode(p);
                return "done: declined invite";
            }
            if (sub == "leave")
            {
                WorldPacket p(CMSG_GROUP_DISBAND, 0);
                bot->GetSession()->HandleGroupDisbandOpcode(p);
                return "done: left group";
            }
        }

        if (verb == "whisper")
        {
            std::string who = TakeWord(rest);
            Player* target = FindPlayerByName(who);
            if (!target)
                return "player not online";
            WorldPacket p;
            ChatHandler::BuildChatPacket(p, CHAT_MSG_WHISPER, LANG_UNIVERSAL, bot, target, rest);
            target->GetSession()->SendPacket(&p);
            return "done: whispered " + target->GetName();
        }

        // 5. Generic Playerbot chat command passthrough
#if AF_HAS_PLAYERBOTS
        if (ai)
        {
            ai->HandleCommand(CHAT_MSG_WHISPER, command, bot);
            return "sent";
        }
#endif
        return "command executed";
    }

    AutopilotErrandUpdate UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand,
                                       uint32_t now)
    {
        AutopilotErrandUpdate update;
        if (!errand.active)
        {
            update.finished = true;
            return update;
        }

        std::string note;
        AutopilotTripState state = AzerothFriendTravel::Update(bot, ai, errand.trip, now, note);
        if (!note.empty())
            update.note = note;

        if (state == AutopilotTripState::Going)
            return update;

        if (state == AutopilotTripState::Failed)
        {
            errand.active = false;
            update.finished = true;
            update.note = "trip failed";
            return update;
        }

        // Trip arrived! Handle on-arrival jobs
        if (errand.kind == AutopilotErrandKind::Service)
        {
            if (errand.service == static_cast<uint8_t>(AutopilotService::Repair))
            {
                bot->DurabilityRepairAll(false, 0, false);
                update.note = "repaired equipment";
            }
            errand.active = false;
            update.finished = true;
            return update;
        }

        if (errand.kind == AutopilotErrandKind::QuestTurnIn)
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(errand.questId);
            if (q)
            {
                bot->RewardQuest(q, errand.rewardChoice, bot);
                update.note = "turned in quest: " + q->GetTitle();
            }
            errand.active = false;
            update.finished = true;
            return update;
        }

        errand.active = false;
        update.finished = true;
        return update;
    }

    std::string DescribeCraftable(Player* bot)
    {
        (void)bot;
        return "none";
    }

    std::string DescribeSurroundings(Player* bot, bool /*forChat*/)
    {
        return AzerothFriendWorldIndex::DescribeServices(bot);
    }

    void HuntUnderAttack(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand)
    {
        (void)bot;
        AzerothFriendStrategy::ClearQuestTarget(ai);
        errand.hunting = false;
    }

    void StopErrand(PlayerbotAI* ai, AutopilotErrand& errand)
    {
        errand.active = false;
        AzerothFriendTravel::Stop(ai, errand.trip);
    }
}
