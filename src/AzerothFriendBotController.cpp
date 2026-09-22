#include "AzerothFriendBotController.h"
#include "AzerothFriendAiControl.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendShared.h"
#include "AzerothFriendLiveState.h"
#include "AzerothFriendPlayerbotActions.h"
#include "ObjectAccessor.h"
#include "Map.h"
#include "Creature.h"
#include "GameObject.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "Chat.h"
#include "ChannelMgr.h"
#include "Group.h"
#include "SharedDefines.h"
#include "Log.h"
#include "QuestDef.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "Item.h"
#include "Bag.h"
#include "TradeData.h"
#include <cmath>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotMgr.h"
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
#endif

namespace AzerothFriendBotController
{
    bool MoveTo(Player* bot, float x, float y, float z, float range)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        if (std::isnan(x) || std::isnan(y) || std::isnan(z) || std::isinf(x) || std::isinf(y) || std::isinf(z))
            return false;

        float dist = std::sqrt(std::pow(x - bot->GetPositionX(), 2) +
                               std::pow(y - bot->GetPositionY(), 2) +
                               std::pow(z - bot->GetPositionZ(), 2));

        if (dist <= range)
            return true; // Already arrived

        // Movement is exclusively delegated to playerbots pathfinding so the
        // cognitive action cannot diverge from the bot's physical state.
        return AzerothFriendPlayerbotActions::MoveToCoords(bot, x, y, z, range);
    }

    bool Attack(Player* bot, uint32 targetGuid)
    {
        if (!bot || !bot->GetMap())
            return false;

        Creature* creature = AzerothFriendShared::FindCreatureByCounter(bot->GetMap(), targetGuid);
        Unit* target = creature;
        if (!target)
        {
            target = AzerothFriendShared::FindPlayerByCounter(targetGuid);
        }

        if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target) || bot->IsFriendlyTo(target))
            return false;

        bot->SetSelection(target->GetGUID());
        bot->SetFacingToObject(target);

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            // Remove any "wait for attack" strategy and zero the timer so attacks happen immediately on the fly
            ai->ChangeStrategy("-wait for attack", BOT_STATE_COMBAT);
            if (ai->GetAiObjectContext())
            {
                auto* waitVal = ai->GetAiObjectContext()->GetValue<uint8>("wait for attack time");
                if (waitVal)
                    waitVal->Set(0);
            }
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
            ai->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(target->GetGUID());
            ai->ChangeEngine(BOT_STATE_COMBAT);
            Event event("", "");
            if (ai->IsTank(bot))
                ai->DoSpecificAction("tank assist", event, true);
            else
                ai->DoSpecificAction("dps assist", event, true);
            return true;
        }
#endif
        return false;
    }

    bool CastSpell(Player* bot, uint32 spellId, uint32 targetGuid, Unit* resolvedTarget)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !spellId || !bot->HasSpell(spellId))
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsPassive() || bot->HasSpellCooldown(spellId))
            return false;

        Unit* target = resolvedTarget;
        if (!target && targetGuid && bot->GetMap())
        {
            Unit* cand = AzerothFriendShared::FindCreatureByCounter(bot->GetMap(), targetGuid);
            Unit* player = AzerothFriendShared::FindPlayerByCounter(targetGuid);
            if (cand && player)
                return false; // Bare low GUIDs cannot distinguish a player and creature.
            if (!cand)
                cand = player;
            if (!cand)
                return false;
            target = cand;
        }

        // Auto-resolve target if none was explicitly provided
        if (!target)
        {
            if (!spellInfo->IsPositive())
            {
                target = bot->GetSelectedUnit();
                if (!target && bot->GetGroup())
                {
                    Player* master = ObjectAccessor::FindConnectedPlayer(bot->GetGroup()->GetLeaderGUID());
                    if (master)
                        target = master->GetSelectedUnit();
                }
                if (!target)
                    target = bot->GetVictim();

                if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target) || bot->IsFriendlyTo(target))
                    return false;
            }
            else
            {
                Unit* selected = bot->GetSelectedUnit();
                if (selected && selected->IsAlive() && bot->IsFriendlyTo(selected) &&
                    spellInfo->CheckExplicitTarget(bot, selected) == SPELL_CAST_OK)
                {
                    target = selected;
                }
                else if (spellInfo->CheckExplicitTarget(bot, bot) == SPELL_CAST_OK)
                {
                    target = bot;
                }
                else if (bot->GetGroup())
                {
                    Player* master = ObjectAccessor::FindConnectedPlayer(bot->GetGroup()->GetLeaderGUID());
                    if (master && master->IsAlive() && spellInfo->CheckExplicitTarget(bot, master) == SPELL_CAST_OK)
                        target = master;
                }
            }
        }
        else if (spellInfo->IsPositive() && target == bot &&
                 spellInfo->CheckExplicitTarget(bot, bot) != SPELL_CAST_OK && bot->GetGroup())
        {
            Player* master = ObjectAccessor::FindConnectedPlayer(bot->GetGroup()->GetLeaderGUID());
            if (master && master->IsAlive() && spellInfo->CheckExplicitTarget(bot, master) == SPELL_CAST_OK)
                target = master;
        }

        if (!target || !target->IsInWorld() || target->GetMap() != bot->GetMap() ||
            !bot->IsWithinLOSInMap(target))
            return false;

        bot->SetFacingToObject(target);

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai && ai->CanCastSpell(spellId, target, false))
        {
            if (ai->CastSpell(spellId, target))
                return true;
        }
#endif
        return false;
    }

    uint32 ResolveSpellId(Player* bot, std::string const& spellName, uint8 rank)
    {
        if (spellName.empty())
            return 0;

        // Parse optional "Rank X" from string if rank == 0
        std::string cleanName = spellName;
        uint8 parsedRank = rank;
        auto rankPos = cleanName.find("Rank ");
        if (rankPos == std::string::npos)
            rankPos = cleanName.find("rank ");
        if (rankPos != std::string::npos)
        {
            std::string numStr = cleanName.substr(rankPos + 5);
            try {
                parsedRank = static_cast<uint8>(std::stoul(numStr));
            } catch (...) {}
            cleanName = cleanName.substr(0, rankPos);
            while (!cleanName.empty() && (cleanName.back() == ' ' || cleanName.back() == '(' || cleanName.back() == ':'))
                cleanName.pop_back();
        }

        auto StrEqualsCi = [](std::string const& a, std::string const& b) -> bool {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        };

        uint32 bestLearnedSpellId = 0;
        uint8 highestLearnedRank = 0;

        // 1. Search bot's learned spells
        if (bot)
        {
            for (auto const& pair : bot->GetSpellMap())
            {
                if (pair.second && pair.second->State != PLAYERSPELL_REMOVED && pair.second->Active)
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(pair.first);
                    if (info && info->SpellName[0] && StrEqualsCi(info->SpellName[0], cleanName))
                    {
                        uint8 r = info->GetRank();
                        if (parsedRank > 0)
                        {
                            if (r == parsedRank)
                                return info->Id;
                        }
                        if (r >= highestLearnedRank)
                        {
                            highestLearnedRank = r;
                            bestLearnedSpellId = info->Id;
                        }
                    }
                }
            }
        }

        // An explicitly requested rank must never silently become a different rank.
        return parsedRank ? 0 : bestLearnedSpellId;

    }

    bool Interact(Player* bot, uint32 targetGuid)
    {
        if (!bot || !bot->GetMap())
            return false;

        Creature* c = AzerothFriendShared::FindCreatureByCounter(bot->GetMap(), targetGuid);
        if (c)
        {
            float dist = bot->GetDistance(c);
            if (dist > 5.5f)
            {
                // Move closer first
                float angle = c->GetAngle(bot);
                float destX = c->GetPositionX() + std::cos(angle + M_PI) * 2.5f;
                float destY = c->GetPositionY() + std::sin(angle + M_PI) * 2.5f;
                float destZ = c->GetPositionZ();
                MoveTo(bot, destX, destY, destZ, 2.0f);
                return true;
            }

            bot->SetFacingToObject(c);
#if AF_HAS_PLAYERBOTS
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (ai)
            {
                WorldPacket p;
                p << c->GetGUID();
                Event event("gossip hello", p);
                return ai->DoSpecificAction("gossip hello", event);
            }
#endif
            return false;
        }

        GameObject* go = AzerothFriendShared::FindGameObjectByCounter(bot->GetMap(), targetGuid);
        if (go)
        {
            float dist = bot->GetDistance(go);
            if (dist > go->GetInteractionDistance())
            {
                float angle = go->GetAngle(bot);
                float destX = go->GetPositionX() + std::cos(angle + M_PI) * 2.0f;
                float destY = go->GetPositionY() + std::sin(angle + M_PI) * 2.0f;
                float destZ = go->GetPositionZ();
                MoveTo(bot, destX, destY, destZ, 2.0f);
                return true;
            }

            bot->SetFacingToObject(go);
#if AF_HAS_PLAYERBOTS
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (ai)
            {
                Event event("", go->GetGOInfo()->name);
                return ai->DoSpecificAction("use", event);
            }
#endif
            return false;
        }

        return false;
    }

    bool Loot(Player* bot)
    {
        if (!bot)
            return false;

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("", "");
            return ai->DoSpecificAction("loot", event);
        }
#endif
        return false;
    }

    bool AcceptQuest(Player* bot, uint32 questId)
    {
        if (!bot || !questId)
            return false;

        return AzerothFriendPlayerbotActions::DoAction(bot, "accept quest", std::to_string(questId));
    }

    bool TurnInQuest(Player* bot, uint32 questId)
    {
        if (!bot || !questId)
            return false;

        return AzerothFriendPlayerbotActions::DoAction(bot, "talk to quest giver", std::to_string(questId));
    }

    bool Say(Player* bot, std::string const& message, std::string const& channel)
    {
        if (!bot || !bot->IsInWorld() || message.empty())
            return false;

        // In-game speech normally belongs to mod-llm-chatter. The fallback exists so
        // a companion can still answer when chatter is absent or disabled.
        if (!sAzerothFriendConfig->sayFallback)
        {
            if (sAzerothFriendConfig->debug)
            {
                LOG_DEBUG("server.loading",
                          "[AzerothFriend] Speech delegated to mod-llm-chatter (SayFallback disabled) for '{}'",
                          bot->GetName());
            }
            return false;
        }

        (void)channel;
        return AzerothFriendPlayerbotActions::DoAction(bot, "say", message);
    }

    bool Follow(Player* bot, Player* master)
    {
        if (!bot)
            return false;

#if AF_HAS_PLAYERBOTS
        if (ExecutePlayerbotCommand(bot, "follow"))
            return true;
#endif

        (void)master;
        return false;
    }

    bool Stop(Player* bot)
    {
        if (!bot)
            return false;

#if AF_HAS_PLAYERBOTS
        if (ExecutePlayerbotCommand(bot, "stay"))
            return true;
#endif

        return false;
    }

    namespace
    {
        std::unordered_map<ObjectGuid, uint32> sLastEmoteTimestamps;
        constexpr uint32 MIN_EMOTE_INTERVAL_SEC = 3; // Anti-spam: max 1 emote per 3 seconds per bot
    }

    bool Emote(Player* bot, std::string const& emoteName)
    {
        if (!bot) return false;

        // Anti-spam: Gracefully suppress emote during combat without failing action plan
        if (bot->IsInCombat())
            return true;

        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        auto it = sLastEmoteTimestamps.find(bot->GetGUID());
        if (it != sLastEmoteTimestamps.end() && (now - it->second) < MIN_EMOTE_INTERVAL_SEC)
            return true; // Gracefully drop rapid spam emotes without failing action plan

        sLastEmoteTimestamps[bot->GetGUID()] = now;

        return AzerothFriendPlayerbotActions::DoAction(bot, "emote", emoteName);
    }

    bool AcceptDuel(Player* bot)
    {
        if (!bot) return false;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("accept duel", "");
            if (ai->DoSpecificAction("accept duel", event))
                return true;
        }
#endif
        if (bot->duel && bot->duel->State == DUEL_STATE_CHALLENGED)
        {
            time_t now = GameTime::GetGameTime().count();
            bot->duel->StartTime = now + 3;
            bot->duel->State = DUEL_STATE_COUNTDOWN;
            bot->SendDuelCountdown(3000);
            if (bot->duel->Opponent)
            {
                bot->duel->Opponent->duel->StartTime = now + 3;
                bot->duel->Opponent->duel->State = DUEL_STATE_COUNTDOWN;
                bot->duel->Opponent->SendDuelCountdown(3000);
            }
            return true;
        }
        return false;
    }

    bool DeclineDuel(Player* bot)
    {
        if (!bot) return false;
        if (bot->duel)
        {
            bot->DuelComplete(DUEL_INTERRUPTED);
            return true;
        }
        return false;
    }

    bool InitiateDuel(Player* bot, uint32 targetGuid)
    {
        if (!bot) return false;
        Player* target = AzerothFriendShared::FindPlayerByCounter(targetGuid);
        if (!target || !bot->IsWithinDistInMap(target, 30.0f))
            return false;

        // Challenge player to a duel via playerbot AI if available
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("cast custom spell", target->GetName() + " 7266");
            return ai->DoSpecificAction("cast custom spell", event);
        }
#endif
        return false;
    }

    bool AcceptTrade(Player* bot)
    {
        if (!bot) return false;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("accept trade", "");
            if (ai->DoSpecificAction("accept trade", event))
                return true;
        }
#endif
        TradeData* trade = bot->GetTradeData();
        if (trade)
        {
            trade->SetAccepted(true);
            return true;
        }
        return false;
    }

    bool CancelTrade(Player* bot)
    {
        if (!bot) return false;
        bot->TradeCancel(true);
        return true;
    }

    bool InitiateTrade(Player* bot, Player* target)
    {
        if (!bot || !target || bot == target || !target->IsInWorld())
            return false;

        if (bot->GetTrader() || target->GetTrader())
            return false;

        if (!bot->IsWithinDistInMap(target, 11.11f))
            return false;

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("trade", target->GetName());
            if (ai->DoSpecificAction("trade", event))
                return true;
        }
#endif

        if (bot->GetSession())
        {
            WorldPacket packet(CMSG_INITIATE_TRADE);
            packet << target->GetGUID();
            bot->GetSession()->HandleInitiateTradeOpcode(packet);
            return true;
        }
        return false;
    }

    std::string FormatItemHyperlink(ItemTemplate const* proto, uint32 count)
    {
        if (!proto)
            return "";

        char color[32];
        uint32 q = proto->Quality < MAX_ITEM_QUALITY ? proto->Quality : 0;
        std::snprintf(color, sizeof(color), "%08x", ItemQualityColors[q]);

        std::string itemName;
        ItemLocale const* locale = sObjectMgr->GetItemLocale(proto->ItemId);
        if (locale && locale->Name.size() > sWorld->GetDefaultDbcLocale())
            itemName = locale->Name[sWorld->GetDefaultDbcLocale()];

        if (itemName.empty())
            itemName = proto->Name1;

        std::ostringstream out;
        out << "|c" << color << "|Hitem:" << proto->ItemId << ":0:0:0:0:0:0:0"
            << "|h[" << itemName << "]|h|r";

        if (count > 1)
            out << " x" << count;

        return out.str();
    }

    bool TradeSetItem(Player* bot, std::string const& itemQuery, uint32 count,
                      int8 specificSlot, std::string* outItemLink)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        TradeData* pTrade = bot->GetTradeData();
        if (!pTrade || !bot->GetTrader())
            return false;

        if (itemQuery.empty())
            return false;

        // 1. Parse query to find item ID or clean item name
        uint32 targetItemId = 0;
        std::string cleanName = itemQuery;

        std::regex linkRegex(R"(\|Hitem:(\d+))");
        std::smatch linkMatch;
        if (std::regex_search(itemQuery, linkMatch, linkRegex) && linkMatch.size() > 1)
        {
            targetItemId = static_cast<uint32>(std::stoul(linkMatch[1].str()));
        }
        else if (std::all_of(itemQuery.begin(), itemQuery.end(), ::isdigit))
        {
            targetItemId = static_cast<uint32>(std::stoul(itemQuery));
        }
        else
        {
            if (cleanName.front() == '[') cleanName.erase(0, 1);
            if (!cleanName.empty() && cleanName.back() == ']') cleanName.pop_back();
            while (!cleanName.empty() && (cleanName.front() == ' ' || cleanName.front() == '\t')) cleanName.erase(0, 1);
            while (!cleanName.empty() && (cleanName.back() == ' ' || cleanName.back() == '\t')) cleanName.pop_back();
        }

        // 2. Search bot's inventory for a tradeable matching item
        Item* matchedItem = nullptr;

        auto CheckItemMatch = [&](Item* item) -> bool
        {
            if (!item)
                return false;
            if (pTrade->HasItem(item->GetGUID()))
                return false;
            if (!item->CanBeTraded(false, true) || item->IsSoulBound())
                return false;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return false;

            if (targetItemId != 0)
                return proto->ItemId == targetItemId;

            return AzerothFriendShared::ContainsCaseInsensitive(proto->Name1, cleanName);
        };

        // Check backpack
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (CheckItemMatch(it))
            {
                matchedItem = it;
                break;
            }
        }

        // Check equipped bags
        if (!matchedItem)
        {
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                if (Bag* bag = bot->GetBagByPos(b))
                {
                    uint32 bSlots = bag->GetBagSize();
                    for (uint32 s = 0; s < bSlots; ++s)
                    {
                        Item* it = bag->GetItemByPos(s);
                        if (CheckItemMatch(it))
                        {
                            matchedItem = it;
                            break;
                        }
                    }
                    if (matchedItem)
                        break;
                }
            }
        }

        if (!matchedItem)
            return false;

        // 3. Find target trade slot (0..5)
        int8 tradeSlot = -1;
        if (specificSlot >= 0 && specificSlot < TRADE_SLOT_TRADED_COUNT)
        {
            if (pTrade->GetItem(TradeSlots(specificSlot)) == nullptr)
                tradeSlot = specificSlot;
        }

        if (tradeSlot == -1)
        {
            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
            {
                if (pTrade->GetItem(TradeSlots(i)) == nullptr)
                {
                    tradeSlot = i;
                    break;
                }
            }
        }

        if (tradeSlot == -1)
            return false;

        // 4. Send CMSG_SET_TRADE_ITEM opcode packet
        if (bot->GetSession())
        {
            WorldPacket packet(CMSG_SET_TRADE_ITEM, 3);
            packet << (uint8)tradeSlot;
            packet << (uint8)matchedItem->GetBagSlot();
            packet << (uint8)matchedItem->GetSlot();
            bot->GetSession()->HandleSetTradeItemOpcode(packet);

            if (outItemLink)
                *outItemLink = FormatItemHyperlink(matchedItem->GetTemplate(), matchedItem->GetCount());

            return true;
        }

        return false;
    }

    bool TradeClearItem(Player* bot, std::string const& itemQuery, int8 slot)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        TradeData* pTrade = bot->GetTradeData();
        if (!pTrade || !bot->GetTrader())
            return false;

        int8 clearSlot = -1;
        if (slot >= 0 && slot < TRADE_SLOT_TRADED_COUNT)
        {
            if (pTrade->GetItem(TradeSlots(slot)) != nullptr)
                clearSlot = slot;
        }

        if (clearSlot == -1 && !itemQuery.empty())
        {
            uint32 targetItemId = 0;
            std::string cleanName = itemQuery;
            std::regex linkRegex(R"(\|Hitem:(\d+))");
            std::smatch linkMatch;
            if (std::regex_search(itemQuery, linkMatch, linkRegex) && linkMatch.size() > 1)
            {
                targetItemId = static_cast<uint32>(std::stoul(linkMatch[1].str()));
            }
            else if (std::all_of(itemQuery.begin(), itemQuery.end(), ::isdigit))
            {
                targetItemId = static_cast<uint32>(std::stoul(itemQuery));
            }
            else
            {
                if (cleanName.front() == '[') cleanName.erase(0, 1);
                if (!cleanName.empty() && cleanName.back() == ']') cleanName.pop_back();
                while (!cleanName.empty() && (cleanName.front() == ' ' || cleanName.front() == '\t')) cleanName.erase(0, 1);
                while (!cleanName.empty() && (cleanName.back() == ' ' || cleanName.back() == '\t')) cleanName.pop_back();
            }

            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
            {
                if (Item* tradedItem = pTrade->GetItem(TradeSlots(i)))
                {
                    ItemTemplate const* proto = tradedItem->GetTemplate();
                    if (proto)
                    {
                        if (targetItemId != 0 && proto->ItemId == targetItemId)
                        {
                            clearSlot = i;
                            break;
                        }
                        if (targetItemId == 0 && AzerothFriendShared::ContainsCaseInsensitive(proto->Name1, cleanName))
                        {
                            clearSlot = i;
                            break;
                        }
                    }
                }
            }
        }

        if (clearSlot == -1)
            return false;

        if (bot->GetSession())
        {
            WorldPacket packet(CMSG_CLEAR_TRADE_ITEM, 1);
            packet << (uint8)clearSlot;
            bot->GetSession()->HandleClearTradeItemOpcode(packet);
            return true;
        }

        return false;
    }

    bool TradeSetGold(Player* bot, uint32 copper, uint32* outActualCopper)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        TradeData* pTrade = bot->GetTradeData();
        if (!pTrade || !bot->GetTrader())
            return false;

        uint32 availableMoney = bot->GetMoney();
        uint32 clampedCopper = std::min(copper, availableMoney);

        if (bot->GetSession())
        {
            WorldPacket packet(CMSG_SET_TRADE_GOLD, 4);
            packet << (uint32)clampedCopper;
            bot->GetSession()->HandleSetTradeGoldOpcode(packet);

            if (outActualCopper)
                *outActualCopper = clampedCopper;
            return true;
        }

        return false;
    }

    std::string LinkItemsInChat(Player* bot, Player* receiver, std::string const& category, uint32 maxItems)
    {
        if (!bot || !bot->IsInWorld() || !receiver || !receiver->IsInWorld())
            return "";

        std::string cat = category;
        std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);

        std::vector<std::pair<ItemTemplate const*, uint32>> tradeableItems;
        std::unordered_set<uint32> seenItems;

        auto ProcessItem = [&](Item* it)
        {
            if (!it || !it->CanBeTraded(false, true) || it->IsSoulBound())
                return;
            ItemTemplate const* proto = it->GetTemplate();
            if (!proto)
                return;

            if (cat == "consumable" || cat == "food" || cat == "drink")
            {
                if (proto->Class != ITEM_CLASS_CONSUMABLE)
                    return;
            }
            else if (cat == "potion" || cat == "potions" || cat == "elixir")
            {
                if (proto->Class != ITEM_CLASS_CONSUMABLE || (proto->SubClass != ITEM_SUBCLASS_POTION && proto->SubClass != ITEM_SUBCLASS_ELIXIR))
                    return;
            }
            else if (cat == "trade" || cat == "trade_goods" || cat == "crafting" || cat == "reagent")
            {
                if (proto->Class != ITEM_CLASS_TRADE_GOODS && proto->Class != ITEM_CLASS_REAGENT)
                    return;
            }
            else if (cat == "equipment" || cat == "gear" || cat == "armor" || cat == "weapon")
            {
                if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
                    return;
            }

            if (seenItems.insert(proto->ItemId).second)
            {
                tradeableItems.push_back({proto, it->GetCount()});
            }
            else
            {
                for (auto& pair : tradeableItems)
                {
                    if (pair.first->ItemId == proto->ItemId)
                    {
                        pair.second += it->GetCount();
                        break;
                    }
                }
            }
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            ProcessItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            if (Bag* bag = bot->GetBagByPos(b))
            {
                uint32 bSlots = bag->GetBagSize();
                for (uint32 s = 0; s < bSlots; ++s)
                    ProcessItem(bag->GetItemByPos(s));
            }
        }

        if (tradeableItems.empty())
        {
            std::string emptyMsg = std::string("I don't have any tradeable items") + (cat.empty() || cat == "all" ? "." : " in that category.");
            bot->Whisper(emptyMsg, LANG_UNIVERSAL, receiver);
            return emptyMsg;
        }

        std::ostringstream summary;
        std::ostringstream currentLine;
        uint32 itemCount = 0;
        uint32 inLine = 0;

        for (auto const& pair : tradeableItems)
        {
            if (itemCount >= maxItems)
                break;

            std::string link = FormatItemHyperlink(pair.first, pair.second);
            if (inLine > 0)
                currentLine << ", ";
            currentLine << link;
            inLine++;
            itemCount++;

            if (inLine >= 3)
            {
                bot->Whisper(currentLine.str(), LANG_UNIVERSAL, receiver);
                if (!summary.str().empty())
                    summary << " ";
                summary << currentLine.str();
                currentLine.str("");
                inLine = 0;
            }
        }

        if (inLine > 0)
        {
            bot->Whisper(currentLine.str(), LANG_UNIVERSAL, receiver);
            if (!summary.str().empty())
                summary << " ";
            summary << currentLine.str();
        }

        if (tradeableItems.size() > maxItems)
        {
            std::string moreMsg = "(and " + std::to_string(tradeableItems.size() - maxItems) + " more items)";
            bot->Whisper(moreMsg, LANG_UNIVERSAL, receiver);
            summary << " " << moreMsg;
        }

        return summary.str();
    }

    bool Mount(Player* bot)
    {
        if (!bot || bot->IsInCombat() || bot->IsMounted())
            return false;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("mount", "");
            return ai->DoSpecificAction("mount", event);
        }
#endif
        return false;
    }

    bool Dismount(Player* bot)
    {
        if (!bot || !bot->IsMounted()) return false;
        bot->Dismount();
        return true;
    }

    bool EatDrink(Player* bot)
    {
        if (!bot || bot->IsInCombat()) return false;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            Event event("food", "");
            ai->DoSpecificAction("food", event);
            Event drinkEvent("drink", "");
            ai->DoSpecificAction("drink", drinkEvent);
            return true;
        }
#endif
        return false;
    }

    bool ExecutePlayerbotCommand(Player* bot, std::string const& command)
    {
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai && !command.empty())
        {
            Group* grp = bot->GetGroup();
            Player* master = grp ? ObjectAccessor::FindConnectedPlayer(grp->GetLeaderGUID()) : nullptr;
            ai->HandleCommand(CHAT_MSG_WHISPER, command, master ? master : bot);
            return true;
        }
#endif
        return false;
    }

    void TriggerStuckRecovery(Player* bot, float targetX, float targetY, float targetZ)
    {
        if (!bot)
            return;

        LOG_INFO("server.loading", "[AzerothFriend] Bot {} stuck during movement! Attempting recovery.", bot->GetName());

        // Recovery remains inside playerbots pathfinding. Teleporting or raw
        // MotionMaster movement would create a disconnected "phantom" action.
        AzerothFriendPlayerbotActions::MoveToCoords(bot, targetX, targetY, targetZ, 2.0f);
    }

    void AutoRepairAll(Player* bot)
    {
        if (bot)
            AzerothFriendPlayerbotActions::DoAction(bot, "repair");
    }

    uint32 AutoSellJunk(Player* bot)
    {
        if (!bot) return 0;

        return AzerothFriendPlayerbotActions::DoAction(bot, "sell", "*poor") ? 1u : 0u;
    }

    void AutoEquipUpgrades(Player* bot)
    {
        if (bot)
            AzerothFriendPlayerbotActions::DoAction(bot, "equip upgrade");
    }

    static std::unordered_set<uint32> s_claimedBots;
    static std::mutex s_claimMutex;

    bool ClaimBot(Player* bot)
    {
        if (!bot)
            return false;

        uint32 lowGuid = bot->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(s_claimMutex);
            s_claimedBots.insert(lowGuid);
        }

        // Update the value-only cache before the asynchronous SQL write. All
        // event producers and live-state publishing stop immediately.
        sAFLiveState->SetBotBridgeEnabledLocal(lowGuid, false);
        // The dispatcher renews this short lease while the durable bridge gate
        // remains disabled, making Claim indefinite without a wall-clock expiry.
        AzerothFriendAiControl::Acquire(bot, 300);

        if (sAzerothFriendConfig->debug)
        {
            LOG_DEBUG("server.loading", "[AzerothFriend] Claimed bot {} (GUID: {}) indefinitely (bridge disconnected)",
                      bot->GetName(), lowGuid);
        }

        return true;
    }

    void ReleaseBot(Player* bot)
    {
        if (!bot)
            return;

        uint32 lowGuid = bot->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(s_claimMutex);
            s_claimedBots.erase(lowGuid);
        }

        sAFLiveState->SetBotBridgeEnabledLocal(lowGuid, true);
        AzerothFriendAiControl::Release(bot, "manual release");

        if (sAzerothFriendConfig->debug)
        {
            LOG_DEBUG("server.loading", "[AzerothFriend] Released bot {} (GUID: {}) from claim",
                      bot->GetName(), lowGuid);
        }
    }

    bool IsBotClaimed(Player* bot)
    {
        if (!bot)
            return false;

        uint32 lowGuid = bot->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(s_claimMutex);
            if (s_claimedBots.find(lowGuid) != s_claimedBots.end())
                return true;
        }

        // Durable fallback: check the control cache. Default to NOT claimed
        // during warm-up or in SQL fallback so chat commands are not dropped.
        AzerothFriend::BotControlRecord control;
        if (sAFLiveState->GetBotControl(lowGuid, control))
            return !control.bridgeEnabled;

        return false;
    }

    std::string CollectSelfContext(Player* bot)
    {
        if (!bot)
            return "{}";

        std::ostringstream ss;
        ss << "{";

        // 1. Equipped Item Level
        uint32 avgIlvl = bot->GetAverageItemLevel();
        ss << "\"avg_item_level\":" << avgIlvl
           << ",\"avg_ilvl\":" << avgIlvl;

        // Count free/total bag slots and consumables
        uint32 freeSlots = 0;
        uint32 totalSlots = 0;
        uint32 waterCount = 0;
        uint32 foodCount = 0;
        uint32 potionsCount = 0;

        totalSlots += (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START);
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!item)
            {
                freeSlots++;
            }
            else if (ItemTemplate const* tmpl = item->GetTemplate())
            {
                if (tmpl->Class == ITEM_CLASS_CONSUMABLE)
                {
                    std::string nl = tmpl->Name1;
                    std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                    if (nl.find("water") != std::string::npos || nl.find("drink") != std::string::npos || nl.find("milk") != std::string::npos || nl.find("juice") != std::string::npos || nl.find("tea") != std::string::npos)
                        waterCount += item->GetCount();
                    else if (nl.find("potion") != std::string::npos || nl.find("elixir") != std::string::npos || nl.find("flask") != std::string::npos || nl.find("bandage") != std::string::npos)
                        potionsCount += item->GetCount();
                    else
                        foodCount += item->GetCount();
                }
            }
        }

        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            if (Bag* bag = bot->GetBagByPos(b))
            {
                uint32 bSlots = bag->GetBagSize();
                totalSlots += bSlots;
                freeSlots += bag->GetFreeSlots();
                for (uint32 s = 0; s < bSlots; ++s)
                {
                    if (Item* item = bag->GetItemByPos(s))
                    {
                        if (ItemTemplate const* tmpl = item->GetTemplate())
                        {
                            if (tmpl->Class == ITEM_CLASS_CONSUMABLE)
                            {
                                std::string nl = tmpl->Name1;
                                std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                                if (nl.find("water") != std::string::npos || nl.find("drink") != std::string::npos || nl.find("milk") != std::string::npos || nl.find("juice") != std::string::npos || nl.find("tea") != std::string::npos)
                                    waterCount += item->GetCount();
                                else if (nl.find("potion") != std::string::npos || nl.find("elixir") != std::string::npos || nl.find("flask") != std::string::npos || nl.find("bandage") != std::string::npos)
                                    potionsCount += item->GetCount();
                                else
                                    foodCount += item->GetCount();
                            }
                        }
                    }
                }
            }
        }

        ss << ",\"free_bag_slots\":" << freeSlots
           << ",\"total_bag_slots\":" << totalSlots
           << ",\"water_count\":" << waterCount
           << ",\"food_count\":" << foodCount
           << ",\"potions_count\":" << potionsCount;

        // Complete live spellbook; rank/learning changes invalidate the bridge cache.
        ss << ",\"combat_spells\":[";
        uint32 spellCount = 0;
        for (auto const& pair : bot->GetSpellMap())
        {
            if (pair.second && pair.second->State != PLAYERSPELL_REMOVED && pair.second->Active)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(pair.first);
                if (info && !info->IsPassive() && info->SpellLevel <= bot->GetLevel())
                {
                    if (spellCount > 0) ss << ",";
                    std::string rankStr = info->Rank[0] ? info->Rank[0] : "";
                    ss << "{\"id\":" << pair.first << ",\"name\":\""
                       << AzerothFriendShared::EscapeJsonString(info->SpellName[0] ? info->SpellName[0] : "")
                       << "\",\"rank\":\"" << AzerothFriendShared::EscapeJsonString(rankStr) << "\""
                       << ",\"rank_num\":" << static_cast<uint32>(info->GetRank())
                       << ",\"mana\":" << info->ManaCost
                       << ",\"cast_time\":" << info->CalcCastTime(bot)
                       << ",\"cd\":" << info->GetRecoveryTime() << "}";
                    ++spellCount;
                }
            }
        }
        ss << "]";

        // 3. Usable consumables in bags (potions, food, bandages)
        ss << ",\"bag_items\":[";
        uint32 itemCount = 0;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (item && item->GetTemplate())
            {
                ItemTemplate const* tmpl = item->GetTemplate();
                if (tmpl->Class == ITEM_CLASS_CONSUMABLE)
                {
                    if (itemCount > 0) ss << ",";
                    ss << "{\"id\":" << tmpl->ItemId << ",\"name\":\""
                       << AzerothFriendShared::EscapeJsonString(tmpl->Name1)
                       << "\",\"count\":" << item->GetCount() << "}";
                    if (++itemCount >= 10)
                        break;
                }
            }
        }
        ss << "]";

        // 4. Active Quests
        ss << ",\"active_quests\":[";
        uint32 questCount = 0;
        for (auto const& pair : bot->getQuestStatusMap())
        {
            if (pair.second.Status == QUEST_STATUS_INCOMPLETE)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(pair.first);
                if (q)
                {
                    if (questCount > 0) ss << ",";
                    ss << "{\"id\":" << q->GetQuestId() << ",\"title\":\""
                       << AzerothFriendShared::EscapeJsonString(q->GetTitle()) << "\"}";
                    if (++questCount >= 10)
                        break;
                }
            }
        }
        ss << "]";

        // 5. Money
        uint32 money = bot->GetMoney();
        ss << ",\"money\":{\"gold\":" << (money / 10000)
           << ",\"silver\":" << ((money % 10000) / 100)
           << ",\"copper\":" << (money % 100) << "}";

        // 6. Learned Professions
        static const std::vector<std::pair<uint32, const char*>> kProfessions = {
            { SKILL_BLACKSMITHING, "Blacksmithing" },
            { SKILL_LEATHERWORKING, "Leatherworking" },
            { SKILL_ALCHEMY, "Alchemy" },
            { SKILL_HERBALISM, "Herbalism" },
            { SKILL_MINING, "Mining" },
            { SKILL_TAILORING, "Tailoring" },
            { SKILL_ENGINEERING, "Engineering" },
            { SKILL_ENCHANTING, "Enchanting" },
            { SKILL_SKINNING, "Skinning" },
            { SKILL_JEWELCRAFTING, "Jewelcrafting" },
            { SKILL_INSCRIPTION, "Inscription" },
            { SKILL_COOKING, "Cooking" },
            { SKILL_FIRST_AID, "First Aid" },
            { SKILL_FISHING, "Fishing" }
        };

        ss << ",\"professions\":[";
        bool firstProf = true;
        for (auto const& prof : kProfessions)
        {
            if (bot->HasSkill(prof.first))
            {
                if (!firstProf) ss << ",";
                ss << "{\"id\":" << prof.first
                   << ",\"name\":\"" << prof.second << "\""
                   << ",\"value\":" << (uint32)bot->GetSkillValue(prof.first)
                   << ",\"max\":" << (uint32)bot->GetPureMaxSkillValue(prof.first) << "}";
                firstProf = false;
            }
        }
        ss << "]";

#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            ss << ",\"playerbot\":{";
            ss << "\"strategies_combat\":[";
            std::vector<std::string> cbtStrats = ai->GetStrategies(BOT_STATE_COMBAT);
            for (size_t i = 0; i < cbtStrats.size(); ++i)
            {
                if (i > 0) ss << ",";
                ss << "\"" << AzerothFriendShared::EscapeJsonString(cbtStrats[i]) << "\"";
            }
            ss << "],\"strategies_non_combat\":[";
            std::vector<std::string> ncStrats = ai->GetStrategies(BOT_STATE_NON_COMBAT);
            for (size_t i = 0; i < ncStrats.size(); ++i)
            {
                if (i > 0) ss << ",";
                ss << "\"" << AzerothFriendShared::EscapeJsonString(ncStrats[i]) << "\"";
            }
            ss << "]}";
        }
#endif

        ss << "}";

        return ss.str();
    }
}
