#include "AzerothFriendCommand.h"
#include "AzerothFriendActionDispatcher.h"
#include "AzerothFriendActionRegistry.h"
#include "AzerothFriendAiControl.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendShared.h"
#include "AzerothFriendBotController.h"
#include "AzerothFriendPlayerbotActions.h"
#include "AzerothFriendSnapshotMemory.h"
#include "AzerothFriendEnvironment.h"
#include "AzerothFriendLiveState.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace AzerothFriend
{
    namespace
    {
        std::string TakeWord(std::string& input)
        {
            size_t begin = input.find_first_not_of(' ');
            if (begin == std::string::npos)
            {
                input.clear();
                return "";
            }
            size_t end = input.find(' ', begin);
            std::string word = input.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            input = end == std::string::npos ? "" : input.substr(end + 1);
            return word;
        }

        // FRIEND_* payloads use '|' as their field separator. Contract text is
        // operator-facing data, so flatten it without allowing a description or
        // compact parameter expression to create a forged telemetry field.
        std::string AddonField(std::string value)
        {
            for (char& c : value)
            {
                if (c == '|' || c == '\r' || c == '\n')
                    c = (c == '|') ? '/' : ' ';
            }
            return value;
        }

        /// Value-only reply helper: the callback captures a player GUID, never a
        /// ChatHandler or Player*, and resolves the recipient freshly.
        uint32 RecipientGuid(ChatHandler* handler)
        {
            if (!handler || !handler->GetSession())
                return 0;
            Player* player = handler->GetSession()->GetPlayer();
            return player ? player->GetGUID().GetCounter() : 0;
        }

        void SendToPlayer(uint32 playerGuid, std::string const& message)
        {
            if (!playerGuid || message.empty())
                return;
            Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(playerGuid));
            if (player && player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
        }

        /// Runs a diagnostic query off the world thread and renders the reply on
        /// the world thread when the value-only callback is pumped.
        void AsyncSendQuery(ChatHandler* handler, std::string const& sql,
                            std::function<std::string(QueryResult)> render)
        {
            uint32 recipient = RecipientGuid(handler);
            if (!recipient)
                return;
            sAFLiveState->SubmitAsyncQuery(sql, [recipient, render](QueryResult result)
            {
                SendToPlayer(recipient, render(result));
            });
        }

        void HandleStatus(ChatHandler* handler)
        {
            handler->SendSysMessage("=== AzerothFriend Background System Status ===");
            handler->SendSysMessage(sAzerothFriendConfig->enable ? "Module State: |cff00ff00ENABLED|r" : "Module State: |cffff0000DISABLED|r");
            handler->SendSysMessage(sAzerothFriendConfig->debug ? "Debug Logging: |cff00ff00ACTIVE (Verbose)|r" : "Debug Logging: |cffaaaaaaOFF|r");

            std::ostringstream tickInfo;
            tickInfo << "Action Tick Frequency: " << sAzerothFriendConfig->tickIntervalMs << " ms"
                     << " | Max Entities/Prompt: " << sAzerothFriendConfig->maxVisibleEntities
                     << " | Multi-Action Batching: " << (sAzerothFriendConfig->multiActionPlanEnable ? "ON" : "OFF");
            handler->SendSysMessage(tickInfo.str().c_str());

            // Storage mode and RAM ring buffer status
            std::ostringstream storageInfo;
            if (sAzerothFriendConfig->storageMode == 1)
            {
                storageInfo << "Snapshot Storage: |cff00ccffRAM Ring Buffer (In-Memory)|r"
                            << " | Active Snapshots: " << SnapshotMemory::Instance().GetTotalSnapshots()
                            << " | TTL: " << sAzerothFriendConfig->snapshotTtlMinutes << "m";
            }
            else
            {
                storageInfo << "Snapshot Storage: |cffffcc00SQL Persistent Database|r";
            }
            storageInfo << " | Self-Context: " << (sAzerothFriendConfig->selfContextEnable ? "|cff00ff00ON|r" : "|cffaaaaaaOFF|r");
            handler->SendSysMessage(storageInfo.str().c_str());

            std::ostringstream mindsetInfo;
            mindsetInfo << "Mindset Latency: " << (sAzerothFriendConfig->mindsetEnable ? "|cff00ff00ON|r" : "|cffaaaaaaOFF|r")
                        << " | Commitment Window: " << sAzerothFriendConfig->mindsetCommitmentSeconds << "s"
                        << " | Opportunistic: " << (sAzerothFriendConfig->mindsetAllowOpportunistic ? "|cff00ff00ON|r" : "|cffaaaaaaOFF|r");
            handler->SendSysMessage(mindsetInfo.str().c_str());

            // mod-llm-chatter Token Synergy Telemetry (RAM counters; the bridge
            // flushes them to SQL every 10 seconds when dirty).
            if (sAFLiveState->IsRunning())
            {
                AzerothFriend::TelemetryCounters counters = sAFLiveState->GetTelemetry();
                std::ostringstream telInfo;
                telInfo << "Chatter Token Synergy: |cff00ff00ACTIVE|r"
                        << " | Co-Processed: " << counters.coProcessed
                        << " | Sensory Reused: " << counters.sensoryReused
                        << " | Tokens Saved: |cff00ff00~" << counters.tokensSaved << "|r";
                handler->SendSysMessage(telInfo.str().c_str());
            }
            else
            {
                QueryResult telRes = CharacterDatabase.Query(
                    "SELECT co_processed_events, sensory_reused_events, tokens_saved FROM azeroth_friend_telemetry WHERE id = 1");
                if (telRes)
                {
                    Field* tf = telRes->Fetch();
                    std::ostringstream telInfo;
                    telInfo << "Chatter Token Synergy: |cff00ff00ACTIVE|r"
                            << " | Co-Processed: " << tf[0].Get<uint32>()
                            << " | Sensory Reused: " << tf[1].Get<uint32>()
                            << " | Tokens Saved: |cff00ff00~" << tf[2].Get<uint32>() << "|r";
                    handler->SendSysMessage(telInfo.str().c_str());
                }
                else
                {
                    handler->SendSysMessage("Chatter Token Synergy: |cffaaaaaaREADY (Pending telemetry sync)|r");
                }
            }

            // Queue statistics are diagnostics: they run asynchronously and reply
            // when the value-only callback is pumped on the world thread.
            AsyncSendQuery(handler,
                "SELECT status, COUNT(*) FROM azeroth_friend_events GROUP BY status",
                [](QueryResult result) -> std::string
                {
                    uint32 pending = 0, processing = 0, completed = 0, failed = 0;
                    if (result)
                    {
                        do
                        {
                            Field* f = result->Fetch();
                            std::string st = f[0].Get<std::string>();
                            uint32 count = f[1].Get<uint32>();
                            if (st == "pending") pending = count;
                            else if (st == "processing") processing = count;
                            else if (st == "completed") completed = count;
                            else if (st == "failed") failed = count;
                        } while (result->NextRow());
                    }
                    std::ostringstream evInfo;
                    evInfo << "Events: |cffffcc00Pending: " << pending << "|r"
                           << " | |cff00ccffProcessing: " << processing << "|r"
                           << " | |cff00ff00Completed: " << completed << "|r"
                           << " | |cffff0000Failed: " << failed << "|r";
                    return evInfo.str();
                });

            AsyncSendQuery(handler,
                "SELECT status, COUNT(*) FROM azeroth_friend_actions GROUP BY status",
                [](QueryResult result) -> std::string
                {
                    uint32 queued = 0, running = 0, executed = 0, interrupted = 0, failed = 0;
                    if (result)
                    {
                        do
                        {
                            Field* f = result->Fetch();
                            std::string st = f[0].Get<std::string>();
                            uint32 count = f[1].Get<uint32>();
                            if (st == "pending") queued = count;
                            else if (st == "in_progress") running = count;
                            else if (st == "completed") executed = count;
                            else if (st == "interrupted") interrupted = count;
                            else if (st == "failed") failed = count;
                        } while (result->NextRow());
                    }
                    std::ostringstream actInfo;
                    actInfo << "Actions (C++ Dispatcher): |cffffcc00Pending: " << queued << "|r"
                            << " | |cff00ccffRunning: " << running << "|r"
                            << " | |cff00ff00Completed: " << executed << "|r"
                            << " | |cffff8800Interrupted: " << interrupted << "|r"
                            << " | |cffff0000Failed: " << failed << "|r";
                    return actInfo.str();
                });

            AsyncSendQuery(handler,
                "SELECT COUNT(*) FROM azeroth_friend_bots",
                [](QueryResult result) -> std::string
                {
                    uint32 total = result ? result->Fetch()[0].Get<uint32>() : 0;
                    return "Registered Bot Companions in DB: " + std::to_string(total);
                });

            // Live-state transport health (RAM, no SQL).
            AzerothFriend::LiveStateStatus status = sAFLiveState->GetStatus();
            AzerothFriend::BridgeDiagnostics diagnostics = sAFLiveState->GetBridgeDiagnostics();
            std::ostringstream ramInfo;
            ramInfo << "RAM-First Transport: "
                    << (status.running ? (status.bridgeAuthorized ? "|cff00ff00CONNECTED|r" : "|cffffcc00LISTENING|r")
                                       : "|cffaaaaaaDISABLED|r")
                    << " | Session: " << (status.sessionId.empty() ? "n/a" : status.sessionId)
                    << " | Frames: " << status.framesSent
                    << " | Refresh requests: " << status.refreshesRequested
                    << " | Context tokens: " << diagnostics.contextTokens
                    << "/" << sAzerothFriendConfig->contextMaxInputTokens
                    << " | RAM bytes: " << diagnostics.cacheBytes;
            handler->SendSysMessage(ramInfo.str().c_str());
        }

        void HandleBots(ChatHandler* handler)
        {
            AsyncSendQuery(handler,
                "SELECT bot_guid, bot_name, mode, affinity, personality, current_goal FROM azeroth_friend_bots",
                [](QueryResult res) -> std::string
                {
                    std::ostringstream out;
                    out << "=== Registered AzerothFriend Bot Companions ===";
                    if (!res)
                    {
                        out << "\nNo companion bots registered in `azeroth_friend_bots` table.";
                        return out.str();
                    }

                    do
                    {
                        Field* f = res->Fetch();
                        uint32 guid = f[0].Get<uint32>();
                        std::string name = f[1].Get<std::string>();
                        std::string mode = f[2].Get<std::string>();
                        int32 affinity = f[3].Get<int32>();
                        std::string goal = f[5].Get<std::string>();

                        Player* bot = AzerothFriendShared::FindPlayerByCounter(guid);
                        bool online = (bot && bot->IsInWorld());
                        bool claimed = (bot && AzerothFriendBotController::IsBotClaimed(bot));

                        out << "\n"
                            << (online ? "|cff00ff00[ONLINE]|r " : "|cff888888[OFFLINE]|r ")
                            << (claimed ? "|cffff8800[CLAIMED]|r " : "")
                            << "|cffffffff" << name << "|r (GUID: " << guid << ")"
                            << " | Mode: " << mode
                            << " | Affinity: " << (affinity >= 0 ? "+" : "") << affinity
                            << " | Goal: " << (goal.empty() ? "None" : goal);
                    } while (res->NextRow());
                    return out.str();
                });
        }

        void HandleEvents(ChatHandler* handler, std::string limitStr)
        {
            uint32 limit = 5;
            if (!limitStr.empty())
            {
                try { limit = std::stoul(limitStr); } catch (...) {}
            }
            if (limit == 0 || limit > 50) limit = 5;

            AsyncSendQuery(handler,
                ("SELECT id, bot_guid, event_type, priority, status, created_at "
                 "FROM azeroth_friend_events ORDER BY id DESC LIMIT " + std::to_string(limit)),
                [limit](QueryResult res) -> std::string
                {
                    std::ostringstream out;
                    out << "=== Recent Events in Background Pipeline (Last " << limit << ") ===";
                    if (!res)
                    {
                        out << "\nNo events found in `azeroth_friend_events`.";
                        return out.str();
                    }

                    do
                    {
                        Field* f = res->Fetch();
                        uint64 id = f[0].Get<uint64>();
                        uint32 botGuid = f[1].Get<uint32>();
                        std::string evType = f[2].Get<std::string>();
                        uint8 prio = f[3].Get<uint8>();
                        std::string status = f[4].Get<std::string>();
                        std::string created = f[5].Get<std::string>();

                        std::string color = (status == "completed") ? "|cff00ff00" :
                                            (status == "processing") ? "|cff00ccff" :
                                            (status == "pending") ? "|cffffcc00" : "|cffff0000";

                        out << "\n#" << id << " Bot " << botGuid << " | Type: " << evType
                            << " (Prio " << (uint32)prio << ") | Status: " << color << status << "|r"
                            << " | Time: " << created;
                    } while (res->NextRow());
                    return out.str();
                });
        }

        void HandleActions(ChatHandler* handler, std::string limitStr)
        {
            uint32 limit = 5;
            if (!limitStr.empty())
            {
                try { limit = std::stoul(limitStr); } catch (...) {}
            }
            if (limit == 0 || limit > 50) limit = 5;

            AsyncSendQuery(handler,
                ("SELECT id, bot_guid, action_type, status, created_at, completed_at, "
                 "COALESCE(failure_reason, ''), COALESCE(result_json, '') "
                 "FROM azeroth_friend_actions ORDER BY id DESC LIMIT " + std::to_string(limit)),
                [limit](QueryResult res) -> std::string
                {
                    std::ostringstream out;
                    out << "=== Recent Actions in Background Pipeline (Last " << limit << ") ===";
                    if (!res)
                    {
                        out << "\nNo actions found in `azeroth_friend_actions`.";
                        return out.str();
                    }

                    do
                    {
                        Field* f = res->Fetch();
                        uint64 id = f[0].Get<uint64>();
                        uint32 botGuid = f[1].Get<uint32>();
                        std::string actType = f[2].Get<std::string>();
                        std::string status = f[3].Get<std::string>();
                        std::string created = f[4].Get<std::string>();
                        std::string finished = f[5].IsNull() ? "" : f[5].Get<std::string>();
                        std::string reason = f[6].Get<std::string>();
                        std::string result = f[7].Get<std::string>();

                        std::string color = (status == "completed") ? "|cff00ff00" :
                                            (status == "in_progress") ? "|cff00ccff" :
                                            (status == "pending") ? "|cffffcc00" :
                                            (status == "interrupted") ? "|cffff8800" : "|cffff0000";

                        out << "\n#" << id << " Bot " << botGuid << " | Action: " << actType
                            << " | Status: " << color << status << "|r"
                            << " | Created: " << created
                            << (finished.empty() ? "" : " | Finished: " + finished);
                        if (!result.empty())
                            out << " | Result: " << (result.size() > 120 ? result.substr(0, 120) + "..." : result);
                        if (!reason.empty())
                            out << " | Reason: " << reason;
                    } while (res->NextRow());
                    return out.str();
                });
        }

        void HandleCatalog(ChatHandler* handler, std::string rest)
        {
            std::string first = TakeWord(rest);
            bool const describe = first == "describe";
            std::string filter = describe ? TakeWord(rest) : first;
            std::string botName = TakeWord(rest);

            if (filter == "all" || filter == "*")
                filter.clear();

            Player* bot = nullptr;
            if (!botName.empty())
                bot = ObjectAccessor::FindPlayerByName(botName.c_str());

            if (!bot && !filter.empty())
            {
                // If user called `.af catalog <bot>` without filter
                Player* maybeBot = ObjectAccessor::FindPlayerByName(filter.c_str());
                if (maybeBot && (sAzerothFriendConfig->IsBotControlled(maybeBot->GetName()) ||
                                 AzerothFriendPlayerbotActions::IsPlayerbot(maybeBot)))
                {
                    bot = maybeBot;
                    filter.clear();
                }
            }

            if (!bot)
            {
                Player* self = handler->GetPlayer();
                if (self)
                {
                    if (self->GetSelectedPlayer() &&
                        (sAzerothFriendConfig->IsBotControlled(self->GetSelectedPlayer()->GetName()) ||
                         AzerothFriendPlayerbotActions::IsPlayerbot(self->GetSelectedPlayer())))
                    {
                        bot = self->GetSelectedPlayer();
                    }
                    else if (self->GetGroup())
                    {
                        for (GroupReference* itr = self->GetGroup()->GetFirstMember(); itr != nullptr; itr = itr->next())
                        {
                            Player* member = itr->GetSource();
                            if (member && member != self &&
                                (sAzerothFriendConfig->IsBotControlled(member->GetName()) ||
                                 AzerothFriendPlayerbotActions::IsPlayerbot(member)))
                            {
                                bot = member;
                                break;
                            }
                        }
                    }
                }
            }

            if (!bot)
            {
                for (const auto& bName : sAzerothFriendConfig->controlledBots)
                {
                    bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                    if (bot)
                        break;
                }
            }

            if (!bot && sAFLiveState)
            {
                auto controlled = sAFLiveState->GetControlledBots();
                for (auto const& c : controlled)
                {
                    bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(c.botGuid));
                    if (bot)
                        break;
                }
            }

            std::string summary = sAFDispatcher->ExportCatalog(bot);
            handler->SendSysMessage(("=== AzerothFriend Action Catalogue (" + summary + ") ===").c_str());

            Player* recipient = handler->GetPlayer();
            std::string botNameTag = bot ? bot->GetName() : (recipient ? recipient->GetName() : "companion");

            if (describe)
            {
                AzerothFriendActionRegistry::ActionInfo const* info = AzerothFriendActionRegistry::Find(filter);
                if (!info)
                {
                    if (recipient)
                    {
                        AzerothFriendShared::SendTelemetry(bot ? bot : recipient, "FRIEND_API",
                            "Bot: " + botNameTag + "|View: error|Error: Unknown capability '" +
                            AddonField(filter) + "'", recipient);
                    }
                    handler->SendSysMessage(("Unknown capability '" + filter + "'.").c_str());
                    return;
                }

                if (recipient)
                {
                    std::ostringstream packet;
                    packet << "Bot: " << botNameTag
                           << "|View: contract"
                           << "|ApiVersion: " << (uint32)info->apiVersion
                           << "|CapabilityRevision: 1"
                           << "|Name: " << AddonField(info->name)
                           << "|Category: " << AddonField(info->category)
                           << "|Description: " << AddonField(info->description)
                           << "|Authority: " << AddonField(info->authority)
                           << "|BindingKind: " << AddonField(info->bindingKind)
                           << "|NativeBinding: " << AddonField(info->playerbotAction.empty() ? "none" : info->playerbotAction)
                           << "|Completion: " << AddonField(info->completionPolicy)
                           << "|Preconditions: " << AddonField(info->preconditions)
                           << "|Parameters: " << AddonField(info->params.empty() ? "none" : info->params)
                           << "|ParameterSchema: " << AddonField(info->paramsJsonSchema)
                           << "|ResultSchema: " << AddonField(info->resultJsonSchema);
                    AzerothFriendShared::SendTelemetry(bot ? bot : recipient, "FRIEND_API", packet.str(), recipient);
                }

                std::ostringstream detail;
                detail << "|cff00ff00" << info->name << "|r v" << (uint32)info->apiVersion
                       << " [" << info->category << "]\n"
                       << info->description << "\n"
                       << "Authority: " << info->authority << " | Binding: " << info->bindingKind
                       << " | Completion: " << info->completionPolicy << "\n"
                       << "Preconditions: " << info->preconditions << "\n"
                       << "Parameters: " << info->paramsJsonSchema << "\n"
                       << "Result: " << info->resultJsonSchema;
                handler->SendSysMessage(detail.str().c_str());
                return;
            }

            uint32 shown = 0;
            uint32 catalogCount = 0;
            bool chatTruncated = false;
            std::ostringstream catalog;
            for (AzerothFriendActionRegistry::ActionInfo const& info : AzerothFriendActionRegistry::Curated())
            {
                if (!filter.empty() && info.name.find(filter) == std::string::npos &&
                    info.category.find(filter) == std::string::npos)
                    continue;

                if (catalogCount)
                    catalog << ';';
                catalog << AddonField(info.name) << '~' << AddonField(info.category) << '~'
                        << AddonField(info.authority);
                ++catalogCount;

                if (shown < 40)
                {
                    std::ostringstream ss;
                    ss << "|cff00ff00" << info.name << "|r(" << info.params << ") [" << info.category << "] - "
                       << info.description;
                    handler->SendSysMessage(ss.str().c_str());
                    ++shown;
                }
                else
                    chatTruncated = true;
            }

            if (chatTruncated)
                handler->SendSysMessage("... (chat list truncated; the addon receives the complete filtered index)");

            if (recipient && bot)
            {
                std::ostringstream packet;
                packet << "Bot: " << botNameTag
                       << "|View: catalog"
                       << "|ApiVersion: 1"
                       << "|CapabilityRevision: 1"
                       << "|Filter: " << AddonField(filter.empty() ? "all" : filter)
                       << "|Shown: " << catalogCount
                       << "|Truncated: NO"
                       << "|Catalog: " << catalog.str();
                AzerothFriendShared::SendTelemetry(bot, "FRIEND_API", packet.str(), recipient);
            }
            else if (recipient)
            {
                std::ostringstream packet;
                packet << "Bot: " << botNameTag
                       << "|View: catalog"
                       << "|ApiVersion: 1"
                       << "|CapabilityRevision: 1"
                       << "|Filter: " << AddonField(filter.empty() ? "all" : filter)
                       << "|Shown: " << catalogCount
                       << "|Truncated: NO"
                       << "|Catalog: " << catalog.str();
                AzerothFriendShared::SendTelemetry(recipient, "FRIEND_API", packet.str(), recipient);
            }

            handler->SendSysMessage("Tip: .af catalog describe <capability> [bot] shows its Bot API contract.");
            handler->SendSysMessage("Tip: .af run <bot> <action> <json params> fires a single catalogue action.");
        }

        void HandleThinking(ChatHandler* handler, std::string rest)
        {
            std::string tier = TakeWord(rest);
            std::transform(tier.begin(), tier.end(), tier.begin(), ::tolower);
            if (tier.empty())
            {
                handler->SendSysMessage(("Current thinking cadence: " + sAzerothFriendConfig->thinkingCadence +
                                        " (" + std::to_string(sAzerothFriendConfig->GetThinkingCadenceSeconds()) + "s)").c_str());
                return;
            }

            if (tier != "low" && tier != "normal" && tier != "high")
            {
                handler->SendSysMessage("Usage: .af thinking [low|normal|high]");
                return;
            }

            sAzerothFriendConfig->SetThinkingCadence(tier);
            handler->SendSysMessage(("Thinking cadence set to: " + tier +
                                    " (" + std::to_string(sAzerothFriendConfig->GetThinkingCadenceSeconds()) + "s)").c_str());

            // Broadcast updated mindset telemetry to player/addon
            Player* self = handler->GetPlayer();
            if (self)
            {
                Player* bot = nullptr;
                if (self->GetSelectedPlayer())
                    bot = self->GetSelectedPlayer();
                else if (self->GetGroup())
                {
                    for (GroupReference* itr = self->GetGroup()->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* member = itr->GetSource();
                        if (member && member != self &&
                            (sAzerothFriendConfig->IsBotControlled(member->GetName()) ||
                             AzerothFriendPlayerbotActions::IsPlayerbot(member)))
                        {
                            bot = member;
                            break;
                        }
                    }
                }
                if (!bot)
                {
                    for (const auto& bName : sAzerothFriendConfig->controlledBots)
                    {
                        bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                        if (bot) break;
                    }
                }
                if (bot)
                {
                    std::ostringstream mindsetPayload;
                    mindsetPayload << "Bot: " << bot->GetName()
                                   << "|Claimed: " << (AzerothFriendBotController::IsBotClaimed(bot) ? "CLAIMED" : "RELEASED")
                                   << "|Bridge: " << (AzerothFriendBotController::IsBotClaimed(bot) ? "DISCONNECTED" : "CONNECTED")
                                   << "|CommitmentWindow: " << sAzerothFriendConfig->mindsetCommitmentSeconds << "s"
                                   << "|ThinkingCadence: " << sAzerothFriendConfig->thinkingCadence;
                    AzerothFriendShared::SendTelemetry(bot, "FRIEND_MINDSET", mindsetPayload.str(), self);
                }
            }
        }

        void HandleRun(ChatHandler* handler, std::string rest)
        {
            std::string botName = TakeWord(rest);
            std::string actionName = TakeWord(rest);
            if (botName.empty() || actionName.empty())
            {
                handler->SendSysMessage("Usage: .af run <bot> <action> [json params]");
                return;
            }

            Player* bot = ObjectAccessor::FindPlayerByName(botName.c_str());
            if (!bot)
            {
                handler->SendSysMessage(("Bot '" + botName + "' is not online.").c_str());
                return;
            }

            std::string paramsJson = rest.empty() ? "{}" : rest;
            std::string message;
            if (sAFDispatcher->ExecuteManualAction(bot, actionName, paramsJson, message))
                handler->SendSysMessage(("OK: " + message).c_str());
            else
                handler->SendSysMessage(("FAILED: " + message).c_str());
        }

        void HandleDiag(ChatHandler* handler, std::string rest)
        {
            std::string botName = TakeWord(rest);
            Player* bot = nullptr;
            if (!botName.empty())
                bot = ObjectAccessor::FindPlayerByName(botName.c_str());

            if (!bot)
            {
                Player* self = handler->GetPlayer();
                if (self && self->GetSelectedPlayer())
                    bot = self->GetSelectedPlayer();
            }

            if (!bot)
            {
                for (const auto& bName : sAzerothFriendConfig->controlledBots)
                {
                    bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                    if (bot)
                        break;
                }
            }

            if (!bot)
            {
                handler->SendSysMessage("No companion bot found (usage: .af diag <bot>).");
                return;
            }

            handler->SendSysMessage("=== AzerothFriend Action Diagnostics ===");
            handler->SendSysMessage(AzerothFriendAiControl::Describe(bot).c_str());
            handler->SendSysMessage(AzerothFriendPlayerbotActions::Describe(bot).c_str());

            AsyncSendQuery(handler,
                ("SELECT action_type, status, COALESCE(failure_reason, ''), COALESCE(result_json, '') "
                 "FROM azeroth_friend_actions WHERE bot_guid = " +
                 std::to_string(bot->GetGUID().GetCounter()) + " ORDER BY id DESC LIMIT 5"),
                [](QueryResult res) -> std::string
                {
                    if (!res)
                        return "No action history for this bot yet.";
                    std::ostringstream out;
                    do
                    {
                        Field* f = res->Fetch();
                        out << "\nlast: " << f[0].Get<std::string>() << " -> " << f[1].Get<std::string>();
                        std::string reason = f[2].Get<std::string>();
                        std::string result = f[3].Get<std::string>();
                        if (!result.empty())
                            out << " result=" << (result.size() > 90 ? result.substr(0, 90) + "..." : result);
                        if (!reason.empty())
                            out << " reason=" << reason;
                    } while (res->NextRow());
                    return out.str();
                });
        }

        void HandleClaim(ChatHandler* handler, std::string rest)
        {
            std::string botName = TakeWord(rest);
            Player* bot = nullptr;
            if (!botName.empty())
                bot = ObjectAccessor::FindPlayerByName(botName.c_str());

            if (!bot)
            {
                Player* self = handler->GetPlayer();
                if (self && self->GetSelectedPlayer() &&
                    sAzerothFriendConfig->IsBotControlled(self->GetSelectedPlayer()->GetName()))
                {
                    bot = self->GetSelectedPlayer();
                }
                else
                {
                    for (const auto& bName : sAzerothFriendConfig->controlledBots)
                    {
                        bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                        if (bot) break;
                    }
                }
            }

            if (!bot)
            {
                handler->SendSysMessage("No companion bot found online. Usage: .af claim [botname]");
                return;
            }

            AzerothFriendBotController::ClaimBot(bot);

            // Mutual cancellation: claiming companion cancels/disables autonomy
            std::string id = std::to_string(bot->GetGUID().GetCounter());
            // Enqueue-only persistence: control changes are written in issue order
            // without blocking the world thread, and the local generation is
            // invalidated immediately so in-flight steps cannot execute.
            CharacterDatabase.Execute(("UPDATE azeroth_friend_bots SET bridge_enabled=0, autonomy_enabled=0, goal_status='paused', "
                "control_revision=control_revision+1 WHERE bot_guid=" + id).c_str());
            CharacterDatabase.Execute(("UPDATE azeroth_friend_events SET status='superseded', processed_at=NOW() "
                "WHERE bot_guid=" + id + " AND status='pending'").c_str());
            sAFLiveState->RequestFullRefresh(bot->GetGUID().GetCounter());
            sAFLiveState->RefreshBotControlAsync();
            sAFDispatcher->InterruptBotPlan(bot->GetGUID().GetCounter(), "Bot claimed: bridge disconnected", false);

            handler->SendSysMessage(("Bot " + bot->GetName() +
                " claimed until release (AzerothFriend bridge disconnected; autonomy cancelled).").c_str());
            AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, handler->GetPlayer());
        }

        void HandleRelease(ChatHandler* handler, std::string rest)
        {
            std::string botName = TakeWord(rest);
            Player* bot = nullptr;
            if (!botName.empty())
                bot = ObjectAccessor::FindPlayerByName(botName.c_str());

            if (!bot)
            {
                Player* self = handler->GetPlayer();
                if (self && self->GetSelectedPlayer() &&
                    sAzerothFriendConfig->IsBotControlled(self->GetSelectedPlayer()->GetName()))
                {
                    bot = self->GetSelectedPlayer();
                }
                else
                {
                    for (const auto& bName : sAzerothFriendConfig->controlledBots)
                    {
                        bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                        if (bot) break;
                    }
                }
            }

            if (!bot)
            {
                handler->SendSysMessage("No companion bot found online. Usage: .af release [botname]");
                return;
            }

            AzerothFriendBotController::ReleaseBot(bot);
            std::string id = std::to_string(bot->GetGUID().GetCounter());
            CharacterDatabase.Execute(("UPDATE azeroth_friend_bots SET bridge_enabled=1, autonomy_enabled=0, "
                "goal_status='paused', control_revision=control_revision+1 WHERE bot_guid=" + id).c_str());
            sAFLiveState->RequestFullRefresh(bot->GetGUID().GetCounter());
            sAFLiveState->RefreshBotControlAsync();
            handler->SendSysMessage(("Bot " + bot->GetName() +
                " released and reconnected to AzerothFriend with autonomy still off.").c_str());
            AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, handler->GetPlayer());
        }

        void HandleInspect(ChatHandler* handler, std::string rest)
        {
            std::string botName = TakeWord(rest);
            Player* bot = nullptr;
            if (!botName.empty())
            {
                bot = ObjectAccessor::FindPlayerByName(botName.c_str());
            }

            if (!bot)
            {
                // If targeting a bot, inspect target
                Player* self = handler->GetPlayer();
                if (self && self->GetSelectedPlayer() && sAzerothFriendConfig->IsBotControlled(self->GetSelectedPlayer()->GetName()))
                {
                    bot = self->GetSelectedPlayer();
                }
                else
                {
                    // Search first online controlled bot
                    for (const auto& bName : sAzerothFriendConfig->controlledBots)
                    {
                        bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                        if (bot) break;
                    }
                }
            }

            if (!bot)
            {
                std::string primary = sAzerothFriendConfig->GetPrimaryBotName();
                handler->SendSysMessage(("No online controlled companion bot found (configured primary: '" + primary + "'). Usage: .af inspect <botname>").c_str());
                return;
            }

            AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, handler->GetPlayer());
            handler->SendSysMessage(("Broadcasted zero-token live context & telemetry for bot " + bot->GetName()).c_str());
        }

        void HandleTest(ChatHandler* handler, std::string rest)
        {
            std::string botName = TakeWord(rest);
            if (botName.empty())
            {
                handler->SendSysMessage("Usage: .af test <botname> [event_type] [message_or_payload]");
                return;
            }

            Player* bot = ObjectAccessor::FindPlayerByName(botName.c_str());
            uint32 botGuid = 0;
            if (bot)
                botGuid = bot->GetGUID().GetCounter();
            else
            {
                // Offline companions are resolved from the RAM control cache;
                // the SQL fallback is asynchronous and value-only.
                AsyncSendQuery(handler,
                    ("SELECT bot_guid FROM azeroth_friend_bots WHERE bot_name = '" +
                     AzerothFriendShared::EscapeSqlString(botName) + "' LIMIT 1"),
                    [botName](QueryResult res) -> std::string
                    {
                        if (!res)
                            return "Unknown bot companion '" + botName + "'. Not found in `azeroth_friend_bots`.";
                        uint32 guid = res->Fetch()[0].Get<uint32>();
                        AzerothFriendShared::QueueEvent(guid, "player_command", 1, 0, "Console",
                                                        "{\"command\":\"test_diagnostic\",\"text\":\"Greetings from GM test command!\"}");
                        return "Enqueued test event for bot " + botName + " (GUID: " + std::to_string(guid) + "). Watch background bridge logs!";
                    });
                return;
            }

            if (!botGuid)
            {
                handler->SendSysMessage(("Unknown bot companion '" + botName + "'. Not found online or in `azeroth_friend_bots`.").c_str());
                return;
            }

            std::string eventType = TakeWord(rest);
            if (eventType.empty())
                eventType = "player_command";

            std::string payload = rest;
            if (payload.empty())
                payload = "{\"command\":\"test_diagnostic\",\"text\":\"Greetings from GM test command!\"}";
            else if (payload.front() != '{')
                payload = "{\"command\":\"" + AzerothFriendShared::EscapeJsonString(payload) + "\"}";

            Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            uint32 issuerGuid = issuer ? issuer->GetGUID().GetCounter() : 0;
            std::string issuerName = issuer ? issuer->GetName() : "Console";

            AzerothFriendShared::QueueEvent(botGuid, eventType, 1, issuerGuid, issuerName, payload);

            std::string msg = "Enqueued test event '" + eventType + "' for bot " + botName + " (GUID: " + std::to_string(botGuid) + "). Watch background bridge logs!";
            handler->SendSysMessage(msg.c_str());
        }

        void HandleDebugToggle(ChatHandler* handler, std::string arg)
        {
            std::string opt = TakeWord(arg);
            if (opt == "on" || opt == "1")
            {
                sAzerothFriendConfig->debug = true;
                handler->SendSysMessage("AzerothFriend debug logging: |cff00ff00ENABLED|r (live)");
            }
            else if (opt == "off" || opt == "0")
            {
                sAzerothFriendConfig->debug = false;
                handler->SendSysMessage("AzerothFriend debug logging: |cffaaaaaaDISABLED|r (live)");
            }
            else
            {
                sAzerothFriendConfig->debug = !sAzerothFriendConfig->debug;
                handler->SendSysMessage(sAzerothFriendConfig->debug
                    ? "AzerothFriend debug logging: |cff00ff00ENABLED|r (live)"
                    : "AzerothFriend debug logging: |cffaaaaaaDISABLED|r (live)");
            }
        }
        Player* AuthorizedBot(ChatHandler* handler, std::string const& name = "")
        {
            Player* issuer = handler->GetPlayer();
            Player* bot = name.empty() ? nullptr : ObjectAccessor::FindPlayerByName(name.c_str());
            if (name.empty() && issuer && issuer->GetSelectedPlayer() &&
                sAzerothFriendConfig->IsBotControlled(issuer->GetSelectedPlayer()->GetName()))
                bot = issuer->GetSelectedPlayer();
            if (name.empty() && !bot)
                bot = ObjectAccessor::FindPlayerByName(sAzerothFriendConfig->GetPrimaryBotName().c_str());
            if (!AzerothFriendShared::CanControl(issuer, bot))
            {
                if (issuer)
                    AzerothFriendShared::SendAddonError(issuer, "Companion unavailable or not owned by you.", bot ? bot->GetName() : name);
                else
                    handler->SendSysMessage("Companion unavailable or not owned by you.");
                return nullptr;
            }
            return bot;
        }

        void HandleGoal(ChatHandler* handler, std::string rest, bool autonomy)
        {
            std::string op = TakeWord(rest);
            std::string botName;
            if (sAzerothFriendConfig->IsBotControlled(op))
            {
                botName = op;
                op = TakeWord(rest);
            }
            Player* bot = AuthorizedBot(handler, botName);
            if (!bot)
                return;
            std::string id = std::to_string(bot->GetGUID().GetCounter());
            if (op.empty() || op == "show" || op == "status")
            {
                AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, handler->GetPlayer());
                return;
            }
            std::string assignments;
            if (autonomy && (op == "on" || op == "off"))
                assignments = op == "on" ? "bridge_enabled=1,autonomy_enabled=1,goal_status='active'" :
                    "autonomy_enabled=0,goal_status='paused'";
            else if (!autonomy && op == "set" && !rest.empty() && rest.size() <= 255)
                assignments = "current_goal='" + AzerothFriendShared::EscapeSqlString(rest) +
                    "',goal_status='paused',autonomy_enabled=0,goal_progress='',goal_result=''";
            else if (!autonomy && (op == "longterm" || op == "long") && !rest.empty() && rest.size() <= 2000)
                // The enduring purpose. Changing it does not pause the loop: the
                // companion re-derives its next short-term goal from it.
                assignments = "long_term_goal='" + AzerothFriendShared::EscapeSqlString(rest) + "'";
            else if (!autonomy && op == "pause")
                assignments = "goal_status='paused',autonomy_enabled=0";
            else if (!autonomy && op == "resume")
                assignments = "bridge_enabled=1,goal_status='active',autonomy_enabled=1";
            else if (!autonomy && op == "complete")
                assignments = "goal_status='completed',autonomy_enabled=0,goal_result='Confirmed by owner'";
            else if (!autonomy && op == "clear")
                assignments = "current_goal='',long_term_goal=NULL,goal_status='paused',autonomy_enabled=0,"\
                    "goal_progress='',goal_result=''";
            else
            {
                if (Player* issuer = handler->GetPlayer())
                    AzerothFriendShared::SendAddonError(issuer, "Usage: .af autonomy [bot] on|off|status; .af goal [bot] set <short term>|longterm <overall purpose>|show|pause|resume|complete|clear", bot->GetName());
                else
                    handler->SendSysMessage("Usage: .af autonomy [bot] on|off|status; .af goal [bot] set <short term>|"
                                            "longterm <overall purpose>|show|pause|resume|complete|clear");
                return;
            }
            if (op == "on" || op == "resume")
            {
                // Mutual cancellation: enabling autonomy releases the companion claim
                if (AzerothFriendBotController::IsBotClaimed(bot))
                {
                    AzerothFriendBotController::ReleaseBot(bot);
                    handler->SendSysMessage(("Companion claim released on " + bot->GetName() + " for autonomous goal execution.").c_str());
                }

                // Either goal is enough: with only a long-term goal the companion
                // derives its own short-term objective. Read from the RAM control
                // cache so the command handler never blocks the world thread.
                AzerothFriend::BotControlRecord control;
                bool haveControl = sAFLiveState->GetBotControl(bot->GetGUID().GetCounter(), control);
                if (haveControl && control.currentGoal.empty() && control.longTermGoal.empty())
                {
                    if (Player* issuer = handler->GetPlayer())
                        AzerothFriendShared::SendAddonError(issuer, "Set a short-term or long-term goal before enabling autonomy.", bot->GetName());
                    else
                        handler->SendSysMessage("Set a short-term or long-term goal before enabling autonomy.");
                    return;
                }
                if (!haveControl)
                {
                    // Cache still warming: refresh and let the operator retry; the
                    // write below is still safe because it is the owner's command.
                    sAFLiveState->RefreshBotControlAsync();
                }
            }

            // Enqueue-only persistence, in issue order. No blocking write.
            CharacterDatabase.Execute(("UPDATE azeroth_friend_bots SET " + assignments +
                ",control_revision=control_revision+1 WHERE bot_guid=" + id).c_str());
            sAFLiveState->RequestFullRefresh(bot->GetGUID().GetCounter());
            sAFLiveState->RefreshBotControlAsync();
            sAFDispatcher->InterruptBotPlan(bot->GetGUID().GetCounter(), "Owner changed goal/autonomy", false);
            handler->SendSysMessage("Companion goal/autonomy updated.");
            AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, handler->GetPlayer());
        }

        void HandleMode(ChatHandler* handler, std::string rest)
        {
            std::string mode = TakeWord(rest);
            std::string botName;
            if (sAzerothFriendConfig->IsBotControlled(mode))
            {
                botName = mode;
                mode = TakeWord(rest);
            }

            Player* bot = AuthorizedBot(handler, botName);
            if (!bot)
                return;

            std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::tolower(c); });
            if (mode != "combat" && mode != "travel" && mode != "idle" && mode != "social" &&
                mode != "follow" && mode != "stay" && mode != "grind" && mode != "rpg")
            {
                if (Player* issuer = handler->GetPlayer())
                    AzerothFriendShared::SendAddonError(issuer, "Usage: .af mode [bot] <combat|travel|idle|social>", bot->GetName());
                else
                    handler->SendSysMessage("Usage: .af mode [bot] <combat|travel|idle|social>");
                return;
            }

            // Canonicalize aliases
            if (mode == "follow") mode = "travel";
            else if (mode == "stay") mode = "idle";
            else if (mode == "grind") mode = "combat";
            else if (mode == "rpg") mode = "social";

            uint32 botGuid = bot->GetGUID().GetCounter();
            AzerothFriendAiControl::ApplyMode(bot, mode);

            // Enqueue-only persistence
            std::string id = std::to_string(botGuid);
            CharacterDatabase.Execute(("UPDATE azeroth_friend_bots SET action_mode='" +
                AzerothFriendShared::EscapeSqlString(mode) +
                "', control_revision=control_revision+1 WHERE bot_guid=" + id).c_str());

            sAFLiveState->RequestFullRefresh(botGuid);
            sAFLiveState->RefreshBotControlAsync();
            sAFDispatcher->InterruptBotPlan(botGuid, "Owner changed action mode", false);

            handler->SendSysMessage(("Companion " + bot->GetName() + " action mode set to: " + mode).c_str());
            AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, handler->GetPlayer());
        }

        void HandleAction(ChatHandler* handler, std::string rest)
        {
            std::string actionName = TakeWord(rest);
            if (actionName.empty())
            {
                if (Player* issuer = handler->GetPlayer())
                    AzerothFriendShared::SendAddonError(issuer, "Usage: .af action <combat|travel|idle|social|follow|stop|eat_drink>");
                else
                    handler->SendSysMessage("Usage: .af action <combat|travel|idle|social|follow|stop|eat_drink>");
                return;
            }

            std::string loweredAction = actionName;
            std::transform(loweredAction.begin(), loweredAction.end(), loweredAction.begin(), [](unsigned char c) { return std::tolower(c); });
            if (loweredAction == "combat" || loweredAction == "travel" || loweredAction == "idle" || loweredAction == "social")
            {
                HandleMode(handler, loweredAction + (rest.empty() ? "" : " " + rest));
                return;
            }

            Player* self = handler->GetPlayer();
            Player* bot = nullptr;

            if (self && self->GetSelectedPlayer() && sAzerothFriendConfig->IsBotControlled(self->GetSelectedPlayer()->GetName()))
            {
                bot = self->GetSelectedPlayer();
            }
            else
            {
                for (const auto& bName : sAzerothFriendConfig->controlledBots)
                {
                    bot = ObjectAccessor::FindPlayerByName(bName.c_str());
                    if (bot) break;
                }
            }

            if (!bot)
            {
                if (self)
                    AzerothFriendShared::SendAddonError(self, "No controlled companion bot found online.");
                else
                    handler->SendSysMessage("No controlled companion bot found online.");
                return;
            }

            if (!AzerothFriendShared::CanControl(self, bot))
            {
                if (self)
                    AzerothFriendShared::SendAddonError(self, "You do not own this companion.", bot->GetName());
                else
                    handler->SendSysMessage("You do not own this companion.");
                return;
            }
            AzerothFriendShared::InvalidateControl(bot);
            sAFDispatcher->InterruptBotPlan(bot->GetGUID().GetCounter(), "Owner action", false);

            if (actionName == "follow")
            {
                if (AzerothFriendBotController::Follow(bot, self))
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + " to follow you.").c_str());
                else
                    handler->SendSysMessage(("Companion " + bot->GetName() + " rejected the follow action.").c_str());
            }
            else if (actionName == "stop" || actionName == "hold" || actionName == "stay")
            {
                if (AzerothFriendBotController::Stop(bot))
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + " to hold position.").c_str());
                else
                    handler->SendSysMessage(("Companion " + bot->GetName() + " rejected the hold action.").c_str());
            }
            else if (actionName == "eat_drink" || actionName == "rest")
            {
                if (AzerothFriendBotController::EatDrink(bot))
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + " to rest (eat/drink).").c_str());
                else
                    handler->SendSysMessage(("Companion " + bot->GetName() + " rejected the rest action.").c_str());
            }
            else if (actionName == "attack")
            {
                if (AzerothFriendPlayerbotActions::DoCommand(bot, "attack"))
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + " to attack target.").c_str());
                else
                    handler->SendSysMessage(("Companion " + bot->GetName() + " rejected the attack action.").c_str());
            }
            else if (actionName == "loot" || actionName == "loot_all")
            {
                if (AzerothFriendPlayerbotActions::DoCommand(bot, "loot all"))
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + " to loot.").c_str());
                else
                    handler->SendSysMessage(("Companion " + bot->GetName() + " rejected the loot action.").c_str());
            }
            else if (actionName == "flee")
            {
                if (AzerothFriendPlayerbotActions::DoCommand(bot, "flee"))
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + " to flee.").c_str());
                else
                    handler->SendSysMessage(("Companion " + bot->GetName() + " rejected the flee action.").c_str());
            }
            else
            {
                std::string fullCmd = actionName;
                if (!rest.empty())
                    fullCmd += " " + rest;

                if (AzerothFriendPlayerbotActions::DoCommand(bot, fullCmd))
                {
                    handler->SendSysMessage(("Ordered companion " + bot->GetName() + ": " + fullCmd).c_str());
                }
                else
                {
                    handler->SendSysMessage(("Rejected action '" + fullCmd + "' for companion " + bot->GetName()).c_str());
                }
            }
        }
    }

    class AzerothFriendCommandScript final : public AllCommandScript
    {
    public:
        AzerothFriendCommandScript() : AllCommandScript("AzerothFriendCommandScript") {}

        bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
        {
            if (cmdStr.empty())
                return true;

            std::string_view s = cmdStr;
            if (s.front() == '.' || s.front() == '/')
                s.remove_prefix(1);

            size_t spacePos = s.find(' ');
            std::string command(s.substr(0, spacePos));
            std::string arguments = (spacePos != std::string_view::npos) ? std::string(s.substr(spacePos + 1)) : "";

            if (command != "af" && command != "friend")
                return true;

            std::string rest = arguments;
            std::string subcommand = TakeWord(rest);

            if (subcommand == "goal" || subcommand == "autonomy")
            {
                HandleGoal(&handler, rest, subcommand == "autonomy");
                return false;
            }
            if (subcommand == "cast")
            {
                Player* bot = AuthorizedBot(&handler);
                if (bot && !rest.empty())
                {
                    AzerothFriendShared::InvalidateControl(bot);
                    sAFDispatcher->InterruptBotPlan(bot->GetGUID().GetCounter(), "Owner spell request", false);
                    Player* issuer = handler.GetPlayer();
                    AzerothFriendShared::QueueEvent(bot->GetGUID().GetCounter(), "player_command", 1,
                        issuer ? issuer->GetGUID().GetCounter() : 0, issuer ? issuer->GetName() : "Console",
                        "{\"text\":\"cast " + AzerothFriendShared::EscapeJsonString(rest) + "\",\"channel\":\"command\"}");
                    handler.SendSysMessage("Spell request queued.");
                }
                return false;
            }
            if (subcommand == "run" || subcommand == "claim" || subcommand == "release" ||
                subcommand == "inspect" || subcommand == "sync" || subcommand == "context" || subcommand == "diag")
            {
                std::string args = rest;
                Player* bot = AuthorizedBot(&handler, TakeWord(args));
                if (!bot)
                    return false;
                // Handlers must resolve the same bot that passed authorization.
                rest = bot->GetName() + (args.empty() ? "" : " " + args);
                if (subcommand == "run")
                {
                    AzerothFriendShared::InvalidateControl(bot);
                    sAFDispatcher->InterruptBotPlan(bot->GetGUID().GetCounter(), "Owner control command", false);
                }
            }

            bool const console = !handler.GetSession();
            bool const gameMaster = console || handler.GetSession()->GetSecurity() >= SEC_GAMEMASTER;
            if (!gameMaster)
            {
                if (subcommand != "action" && subcommand != "mode" && subcommand != "inspect" && subcommand != "sync" &&
                    subcommand != "context" && subcommand != "claim" && subcommand != "release" &&
                    subcommand != "status" && subcommand != "reset" && subcommand != "run" &&
                    subcommand != "catalog" && subcommand != "diag" && subcommand != "thinking")
                {
                    if (Player* issuer = handler.GetPlayer())
                        AzerothFriendShared::SendAddonError(issuer, "You are not authorized to execute administrative AzerothFriend commands.");
                    else
                        handler.SendSysMessage("You are not authorized to execute administrative AzerothFriend commands.");
                    return false;
                }
            }

            if (subcommand.empty() || subcommand == "status")
            {
                HandleStatus(&handler);
                return false;
            }
            if (subcommand == "mode")
            {
                HandleMode(&handler, rest);
                return false;
            }
            if (subcommand == "thinking")
            {
                HandleThinking(&handler, rest);
                return false;
            }
            if (subcommand == "action")
            {
                HandleAction(&handler, rest);
                return false;
            }
            if (subcommand == "reset")
            {
                handler.SendSysMessage("To reset the AzerothFriend UI window layout, type /af reset in chat.");
                return false;
            }
            if (subcommand == "debug")
            {
                HandleDebugToggle(&handler, rest);
                return false;
            }
            if (subcommand == "bots")
            {
                HandleBots(&handler);
                return false;
            }
            if (subcommand == "events")
            {
                HandleEvents(&handler, rest);
                return false;
            }
            if (subcommand == "actions")
            {
                HandleActions(&handler, rest);
                return false;
            }
            if (subcommand == "catalog")
            {
                HandleCatalog(&handler, rest);
                return false;
            }
            if (subcommand == "run")
            {
                HandleRun(&handler, rest);
                return false;
            }
            if (subcommand == "diag")
            {
                HandleDiag(&handler, rest);
                return false;
            }
            if (subcommand == "claim")
            {
                HandleClaim(&handler, rest);
                return false;
            }
            if (subcommand == "release")
            {
                HandleRelease(&handler, rest);
                return false;
            }
            if (subcommand == "inspect" || subcommand == "sync" || subcommand == "context")
            {
                HandleInspect(&handler, rest);
                return false;
            }
            if (subcommand == "test")
            {
                HandleTest(&handler, rest);
                return false;
            }

            handler.SendSysMessage("=== AzerothFriend Commands ===");
            handler.SendSysMessage(".af status              - Monitor background bridge, RAM/SQL status, and tokens saved");
            handler.SendSysMessage(".af inspect [bot]       - Broadcast zero-token live context & mindset telemetry to Addon");
            handler.SendSysMessage(".af goal [bot] set <text>|show|pause|resume|complete|clear - Manage the focused goal");
            handler.SendSysMessage(".af autonomy [bot] on|off|status - Opt-in self-directed operation (needs a goal)");
            handler.SendSysMessage(".af thinking [low|normal|high] - Configure thinking cadence tier");
            handler.SendSysMessage(".af cast <spell> [target] - Natural-language spell request resolved against learned spells");
            handler.SendSysMessage(".af debug [on|off]      - Toggle verbose debug telemetry in real-time");
            handler.SendSysMessage(".af bots                - List registered companion bots and claim statuses");
            handler.SendSysMessage(".af claim [bot]         - Disconnect bot from AzerothFriend until release");
            handler.SendSysMessage(".af release <bot>       - Reconnect claimed bot with autonomy still off");
            handler.SendSysMessage(".af events [limit]      - Inspect recent events pipeline in database");
            handler.SendSysMessage(".af actions [limit]     - Inspect recent actions queue in database");
            handler.SendSysMessage(".af catalog [filter] [bot] | describe <capability> [bot] - List/export/inspect Bot API v1");
            handler.SendSysMessage(".af run <bot> <action> [json] - Fire one catalogue action immediately");
            handler.SendSysMessage(".af diag [bot]          - Lease state, playerbot AI and last action results");
            handler.SendSysMessage(".af context|sync [bot]  - Dump the live context payload the bridge receives");
            handler.SendSysMessage(".af reset               - Print the addon window layout reset hint");
            handler.SendSysMessage(".af test <bot> [type]   - Trigger manual test event to verify AI bridge");
            return false;
        }
    };

    void RegisterAzerothFriendCommand()
    {
        new AzerothFriendCommandScript();
    }
}
