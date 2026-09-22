#include "AzerothFriendLiveState.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendShared.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "AsyncCallbackProcessor.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace AzerothFriend
{
    namespace
    {
        constexpr uint32 PROTOCOL_VERSION = 1;
        constexpr size_t LENGTH_PREFIX_BYTES = 4;

        std::string ExtractJsonString(std::string const& json, std::string const& key)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
            std::smatch match;
            if (std::regex_search(json, match, pattern) && match.size() > 1)
                return match[1].str();
            return "";
        }

        uint64 ExtractJsonUint64(std::string const& json, std::string const& key, uint64 fallback = 0)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
            std::smatch match;
            if (std::regex_search(json, match, pattern) && match.size() > 1)
            {
                try
                {
                    return static_cast<uint64>(std::stoull(match[1].str()));
                }
                catch (...)
                {
                }
            }
            return fallback;
        }

        std::string MakeSessionId()
        {
            std::random_device device;
            std::mt19937_64 generator(device());
            std::uniform_int_distribution<uint64> distribution;
            std::ostringstream ss;
            ss << std::hex << std::setw(16) << std::setfill('0') << distribution(generator);
            return ss.str();
        }

        uint32 LengthPrefixToUint(std::array<unsigned char, LENGTH_PREFIX_BYTES> const& bytes)
        {
            return (static_cast<uint32>(bytes[0]) << 24) |
                   (static_cast<uint32>(bytes[1]) << 16) |
                   (static_cast<uint32>(bytes[2]) << 8) |
                   static_cast<uint32>(bytes[3]);
        }

        std::array<unsigned char, LENGTH_PREFIX_BYTES> UintToLengthPrefix(uint32 value)
        {
            return {
                static_cast<unsigned char>((value >> 24) & 0xFF),
                static_cast<unsigned char>((value >> 16) & 0xFF),
                static_cast<unsigned char>((value >> 8) & 0xFF),
                static_cast<unsigned char>(value & 0xFF)
            };
        }

        std::string PrefixFrame(std::string const& body)
        {
            std::array<unsigned char, LENGTH_PREFIX_BYTES> prefix =
                UintToLengthPrefix(static_cast<uint32>(body.size()));
            std::string frame;
            frame.resize(LENGTH_PREFIX_BYTES);
            std::memcpy(&frame[0], prefix.data(), LENGTH_PREFIX_BYTES);
            frame += body;
            return frame;
        }

        int64 NowSeconds()
        {
            return static_cast<int64>(std::time(nullptr));
        }

        /**
         * Cancel a timer without depending on a Boost version.
         *
         * Boost 1.87 removed the deprecated `cancel(error_code&)` overloads; the
         * no-argument overload is noexcept for waitable timers there, while older
         * Boost releases require the error_code form.
         */
        template <typename Timer>
        void CancelTimer(Timer& timer)
        {
#if BOOST_VERSION >= 108700
            timer.cancel();
#else
            boost::system::error_code ignored;
            timer.cancel(ignored);
#endif
        }
    }

    char const* LiveSectionName(LiveSection section)
    {
        switch (section)
        {
            case LiveSection::Vitals:       return "vitals";
            case LiveSection::Surroundings: return "surroundings";
            case LiveSection::Owner:        return "owner";
            case LiveSection::Control:      return "control";
            case LiveSection::Capabilities: return "capabilities";
            case LiveSection::Outcomes:     return "outcomes";
        }
        return "vitals";
    }

    /**
     * Loopback transport worker. Every socket handler runs on the single
     * io_context thread started by LiveStateService::Start().
     */
    class LiveStateTransportWorker
    {
    public:
        LiveStateTransportWorker(boost::asio::io_context& io, LiveStateService& service)
            : _io(io), _service(service), _acceptor(io), _socket(io),
              _publishTimer(io), _heartbeatTimer(io), _authTimer(io)
        {
            _sessionId = MakeSessionId();
        }

        void Start()
        {
            boost::system::error_code error;
            boost::asio::ip::tcp::endpoint endpoint(
                boost::asio::ip::make_address(_service.liveStateHost(), error),
                static_cast<unsigned short>(_service.liveStatePort()));
            if (error)
            {
                LOG_ERROR("server.loading", "[AzerothFriend] Live-state listener address '{}' is invalid",
                          _service.liveStateHost());
                return;
            }

            _acceptor.open(endpoint.protocol(), error);
            if (!error)
                _acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);
            if (!error)
                _acceptor.bind(endpoint, error);
            if (!error)
                _acceptor.listen(boost::asio::socket_base::max_listen_connections, error);
            if (error)
            {
                LOG_ERROR("server.loading", "[AzerothFriend] Live-state listener could not bind {}:{} ({})",
                          _service.liveStateHost(), _service.liveStatePort(), error.message());
                return;
            }

            {
                std::lock_guard<std::mutex> lock(_service._statusMutex);
                _service._status.running = true;
                _service._status.sessionId = _sessionId;
            }

            LOG_INFO("server.loading", "[AzerothFriend] Live-state listener bound to {}:{} (session {})",
                     _service.liveStateHost(), _service.liveStatePort(), _sessionId);

            DoAccept();
            SchedulePublishTimer();
            ScheduleHeartbeatTimer();
        }

        void Stop()
        {
            boost::system::error_code ignored;
            CancelTimer(_publishTimer);
            CancelTimer(_heartbeatTimer);
            CancelTimer(_authTimer);
            _acceptor.close(ignored);
            _socket.close(ignored);
        }

    private:
        // ------------------------------------------------------------- accepting
        void DoAccept()
        {
            _acceptor.async_accept(_socket, [this](boost::system::error_code error)
            {
                if (error)
                {
                    if (error != boost::asio::error::operation_aborted && _acceptor.is_open())
                        DoAccept();
                    return;
                }

                // One bridge connection at a time: a new socket replaces the old.
                _authorized = false;
                _writeQueue.clear();
                _body.clear();
                _lastActivity.store(NowSeconds());
                {
                    std::lock_guard<std::mutex> lock(_service._statusMutex);
                    _service._status.bridgeConnected = true;
                    _service._status.bridgeAuthorized = false;
                    _service._status.lastClientActivity = NowSeconds();
                }

                SendHello();
                ScheduleAuthTimeout();
                ReadHeader();
            });
        }

        void SendHello()
        {
            std::ostringstream frame;
            frame << "{\"v\":" << PROTOCOL_VERSION
                  << ",\"type\":\"hello\""
                  << ",\"session\":\"" << _sessionId << "\""
                  << ",\"server_session\":\"" << _sessionId << "\""
                  << ",\"seq\":" << (++_helloSequence)
                  << ",\"ts\":" << NowSeconds() << "}";
            QueueFrame(frame.str());
        }

        void ScheduleAuthTimeout()
        {
            _authTimer.expires_after(std::chrono::seconds(5));
            _authTimer.async_wait([this](boost::system::error_code error)
            {
                if (error || _authorized)
                    return;
                LOG_WARN("server.loading", "[AzerothFriend] Live-state connection dropped: no valid authentication within 5s");
                DropClient("authentication timeout");
            });
        }

        void DropClient(std::string const& reason)
        {
            boost::system::error_code ignored;
            CancelTimer(_authTimer);
            _socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            _socket.close(ignored);
            _authorized = false;
            _isWriting = false;
            _writeQueue.clear();
            {
                std::lock_guard<std::mutex> lock(_service._statusMutex);
                _service._status.bridgeConnected = false;
                _service._status.bridgeAuthorized = false;
            }
            if (!reason.empty())
                LOG_DEBUG("server.loading", "[AzerothFriend] Live-state client disconnected ({})", reason);

            if (_acceptor.is_open())
                DoAccept();
        }

        // ------------------------------------------------------------------ reads
        void ReadHeader()
        {
            boost::asio::async_read(_socket, boost::asio::buffer(_lengthBytes),
                [this](boost::system::error_code error, std::size_t /*bytes*/)
                {
                    if (error)
                    {
                        DropClient("read header");
                        return;
                    }

                    uint32 length = LengthPrefixToUint(_lengthBytes);
                    if (length == 0 || length > _service.frameCapBytes())
                    {
                        {
                            std::lock_guard<std::mutex> lock(_service._statusMutex);
                            ++_service._status.framesRejected;
                        }
                        LOG_WARN("server.loading", "[AzerothFriend] Rejected oversized live-state frame ({} bytes, cap {})",
                                 length, _service.frameCapBytes());
                        DropClient("oversized frame");
                        return;
                    }

                    _body.assign(length, 0);
                    boost::asio::async_read(_socket, boost::asio::buffer(_body),
                        [this](boost::system::error_code bodyError, std::size_t /*bodyBytes*/)
                        {
                            if (bodyError)
                            {
                                DropClient("read body");
                                return;
                            }
                            _lastActivity.store(NowSeconds());
                            {
                                std::lock_guard<std::mutex> lock(_service._statusMutex);
                                _service._status.lastClientActivity = NowSeconds();
                            }
                            HandleInboundFrame(std::string(_body.begin(), _body.end()));
                            ReadHeader();
                        });
                });
        }

        void HandleInboundFrame(std::string const& json)
        {
            std::string type = ExtractJsonString(json, "type");
            if (type == "auth")
            {
                std::string provided = ExtractJsonString(json, "secret");
                if (!_service.secretMatches(provided))
                {
                    {
                        std::lock_guard<std::mutex> lock(_service._statusMutex);
                        ++_service._status.framesRejected;
                    }
                    // The secret itself is never logged.
                    LOG_WARN("server.loading", "[AzerothFriend] Live-state authentication failed; connection refused");
                    std::string error = "{\"v\":" + std::to_string(PROTOCOL_VERSION) +
                        ",\"type\":\"error\",\"session\":\"" + _sessionId + "\",\"seq\":1,\"reason\":\"auth_failed\"}";
                    QueueFrame(error);
                    WriteNext();
                    DropClient("authentication failed");
                    return;
                }

                _authorized = true;
                {
                    std::lock_guard<std::mutex> lock(_service._statusMutex);
                    _service._status.bridgeAuthorized = true;
                }
                LOG_INFO("server.loading", "[AzerothFriend] Live-state bridge authenticated (session {})", _sessionId);

                // The bridge needs a complete baseline after (re)connecting.
                _service.RequestFullRefreshAll();
                DrainDirty();
                return;
            }

            if (!_authorized)
            {
                DropClient("frame before authentication");
                return;
            }

            if (type == "refresh")
            {
                {
                    std::lock_guard<std::mutex> lock(_service._statusMutex);
                    ++_service._status.refreshesRequested;
                }
                _service.RequestFullRefreshAll();
                return;
            }

            if (type == "diag")
            {
                std::lock_guard<std::mutex> lock(_service._cacheMutex);
                _service._diagnostics.contextTokens = static_cast<uint32>(ExtractJsonUint64(json, "context_tokens"));
                _service._diagnostics.cacheBytes = ExtractJsonUint64(json, "cache_bytes");
                _service._diagnostics.cacheBots = static_cast<uint32>(ExtractJsonUint64(json, "cache_bots"));
                _service._diagnostics.summaryTimestamp = static_cast<int64>(ExtractJsonUint64(json, "summary_ts"));
                _service._diagnostics.lastDiagnostic = ExtractJsonString(json, "diagnostic");
                _service._diagnostics.receivedAt = NowSeconds();
                {
                    std::lock_guard<std::mutex> statusLock(_service._statusMutex);
                    _service._status.lastDiagnostics = _service._diagnostics.receivedAt;
                }
                return;
            }

            {
                std::lock_guard<std::mutex> lock(_service._statusMutex);
                ++_service._status.framesRejected;
            }
        }

        // ----------------------------------------------------------------- writes
        void QueueFrame(std::string const& json)
        {
            if (json.size() > _service.frameCapBytes())
            {
                LOG_WARN("server.loading", "[AzerothFriend] Dropped oversized live-state frame ({} bytes)", json.size());
                return;
            }
            _writeQueue.push_back(PrefixFrame(json));
            if (_writeQueue.size() > _service.queueLimit())
            {
                // Bounded queue: drop the oldest replaceable state frame. The
                // bridge detects the sequence gap and asks for a full refresh.
                _writeQueue.pop_front();
            }
        }

        void WriteNext()
        {
            if (_isWriting || _writeQueue.empty())
                return;

            _isWriting = true;
            boost::asio::async_write(_socket, boost::asio::buffer(_writeQueue.front()),
                [this](boost::system::error_code error, std::size_t /*bytes*/)
                {
                    _isWriting = false;
                    if (error)
                    {
                        DropClient("write");
                        return;
                    }
                    _writeQueue.pop_front();
                    {
                        std::lock_guard<std::mutex> lock(_service._statusMutex);
                        ++_service._status.framesSent;
                    }
                    WriteNext();
                });
        }

        // ------------------------------------------------------- periodic timers
        void SchedulePublishTimer()
        {
            _publishTimer.expires_after(std::chrono::milliseconds(_service.publishIntervalMs()));
            _publishTimer.async_wait([this](boost::system::error_code error)
            {
                if (error)
                    return;
                DrainDirty();
                SchedulePublishTimer();
            });
        }

        void ScheduleHeartbeatTimer()
        {
            _heartbeatTimer.expires_after(std::chrono::seconds(_service.heartbeatSeconds()));
            _heartbeatTimer.async_wait([this](boost::system::error_code error)
            {
                if (error)
                    return;
                if (_authorized)
                {
                    std::ostringstream frame;
                    frame << "{\"v\":" << PROTOCOL_VERSION
                          << ",\"type\":\"heartbeat\""
                          << ",\"session\":\"" << _sessionId << "\""
                          << ",\"bot\":0"
                          << ",\"seq\":" << (++_heartbeatSequence)
                          << ",\"ts\":" << NowSeconds() << "}";
                    QueueFrame(frame.str());
                    WriteNext();
                }
                ScheduleHeartbeatTimer();
            });
        }

        /**
         * Coalesce replaceable state updates: only the newest payload per
         * (bot, section) is transmitted. Commands and executable plans are never
         * routed through this channel - they stay in the durable SQL queues.
         */
        void DrainDirty()
        {
            if (!_authorized)
                return;

            std::map<uint32, std::map<uint8, PendingPublish>> pending;
            {
                std::lock_guard<std::mutex> lock(_service._publishMutex);
                pending.swap(_service._dirty);
                _service._queuedSections = 0;
                {
                    std::lock_guard<std::mutex> statusLock(_service._statusMutex);
                    _service._status.queuedSections = 0;
                }
            }

            for (auto& botEntry : pending)
            {
                for (auto& sectionEntry : botEntry.second)
                {
                    PendingPublish const& publish = sectionEntry.second;
                    uint64 sequence = ++_botSequence[publish.botGuid];
                    std::ostringstream frame;
                    frame << "{\"v\":" << PROTOCOL_VERSION
                          << ",\"type\":\"state\""
                          << ",\"session\":\"" << _sessionId << "\""
                          << ",\"bot\":" << publish.botGuid
                          << ",\"seq\":" << sequence
                          << ",\"ts\":" << publish.observationTs
                          << ",\"revision\":" << publish.controlRevision
                          << ",\"section\":\"" << LiveSectionName(publish.section) << "\""
                          << ",\"payload\":" << publish.payloadJson
                          << "}";
                    QueueFrame(frame.str());
                }
            }

            if (!_writeQueue.empty())
                WriteNext();
        }

        boost::asio::io_context& _io;
        LiveStateService& _service;
        boost::asio::ip::tcp::acceptor _acceptor;
        boost::asio::ip::tcp::socket _socket;
        boost::asio::steady_timer _publishTimer;
        boost::asio::steady_timer _heartbeatTimer;
        boost::asio::steady_timer _authTimer;
        std::array<unsigned char, LENGTH_PREFIX_BYTES> _lengthBytes{};
        std::vector<unsigned char> _body;
        std::deque<std::string> _writeQueue;
        std::unordered_map<uint32, uint64> _botSequence;
        uint64 _heartbeatSequence = 0;
        uint64 _helloSequence = 0;
        std::string _sessionId;
        bool _authorized = false;
        bool _isWriting = false;
        std::atomic<time_t> _lastActivity{ 0 };
    };

    // -------------------------------------------------------------------------
    // LiveStateService
    // -------------------------------------------------------------------------
    LiveStateService::~LiveStateService()
    {
        // The static instance is destroyed at exit; joining the worker here keeps
        // the socket from outliving the process teardown.
        Stop();
    }

    LiveStateService& LiveStateService::Instance()
    {
        static LiveStateService instance;
        return instance;
    }

    void LiveStateService::Start()
    {
        if (!sAzerothFriendConfig->liveStateEnable)
            return;
        if (sAzerothFriendConfig->liveStateSecret.empty())
        {
            // No shared secret means no authenticated bridge connection can exist.
            // Leave the service stopped so the module keeps the SQL compatibility
            // path instead of publishing into a listener nobody can use.
            LOG_WARN("server.loading", "[AzerothFriend] Live-state transport not started: "
                     "AzerothFriend.LiveState.Secret is empty. Running in SQL compatibility mode.");
            return;
        }
        if (_running.load())
            return;

        _io = std::make_unique<boost::asio::io_context>();
        _worker = std::make_unique<LiveStateTransportWorker>(*_io, *this);
        _worker->Start();

        bool listenerReady = false;
        {
            std::lock_guard<std::mutex> lock(_statusMutex);
            listenerReady = _status.running;
        }
        if (!listenerReady)
        {
            // The listener could not bind (port in use, unusable address): leave
            // the service stopped so the dispatcher keeps its rollback path.
            LOG_ERROR("server.loading", "[AzerothFriend] Live-state listener unavailable; falling back to SQL compatibility mode.");
            _worker.reset();
            _io.reset();
            return;
        }

        _running.store(true);
        _thread = std::thread([this]()
        {
            try
            {
                _io->run();
            }
            catch (std::exception const& error)
            {
                LOG_ERROR("server.loading", "[AzerothFriend] Live-state worker stopped: {}", error.what());
            }
        });

        // Prime the world RAM caches from asynchronous reads (no blocking I/O).
        RefreshBotControlAsync();
        RefreshTelemetryAsync();
    }

    std::string const& LiveStateService::liveStateHost() const
    {
        return sAzerothFriendConfig->liveStateHost;
    }

    uint32 LiveStateService::liveStatePort() const
    {
        return sAzerothFriendConfig->liveStatePort;
    }

    uint32 LiveStateService::frameCapBytes() const
    {
        return sAzerothFriendConfig->liveStateFrameCapBytes ? sAzerothFriendConfig->liveStateFrameCapBytes : (256 * 1024);
    }

    uint32 LiveStateService::publishIntervalMs() const
    {
        return sAzerothFriendConfig->liveStatePublishIntervalMs ? sAzerothFriendConfig->liveStatePublishIntervalMs : 250;
    }

    uint32 LiveStateService::heartbeatSeconds() const
    {
        return sAzerothFriendConfig->liveStateHeartbeatSeconds ? sAzerothFriendConfig->liveStateHeartbeatSeconds : 2;
    }

    uint32 LiveStateService::queueLimit() const
    {
        return sAzerothFriendConfig->liveStateQueueLimit ? sAzerothFriendConfig->liveStateQueueLimit : 1024;
    }

    bool LiveStateService::secretMatches(std::string const& provided) const
    {
        std::string const& expected = sAzerothFriendConfig->liveStateSecret;
        if (expected.empty())
            return false;
        if (provided.size() != expected.size())
            return false;
        // Constant-time comparison: never leak characters through timing.
        unsigned char diff = 0;
        for (size_t i = 0; i < expected.size(); ++i)
            diff |= static_cast<unsigned char>(provided[i] ^ expected[i]);
        return diff == 0;
    }

    void LiveStateService::Stop()
    {
        if (!_running.exchange(false))
            return;

        if (_worker)
            _worker->Stop();
        if (_io)
            _io->stop();
        if (_thread.joinable())
            _thread.join();

        _worker.reset();
        _io.reset();

        std::lock_guard<std::mutex> lock(_publishMutex);
        _dirty.clear();
        _queuedSections = 0;

        std::lock_guard<std::mutex> statusLock(_statusMutex);
        _status = LiveStateStatus{};
    }

    bool LiveStateService::IsRunning() const
    {
        return _running.load();
    }

    // ---------------------------------------------------------- world-thread API
    void LiveStateService::Publish(uint32 botGuid, LiveSection section, std::string const& payloadJson,
                                   uint32 controlRevision)
    {
        if (!botGuid || payloadJson.empty() || !IsBotBridgeEnabled(botGuid))
            return;

        PendingPublish entry;
        entry.botGuid = botGuid;
        entry.section = section;
        entry.payloadJson = payloadJson;
        entry.controlRevision = controlRevision;
        entry.observationTs = NowSeconds();
        PublishLocked(std::move(entry));
    }

    void LiveStateService::PublishLocked(PendingPublish&& entry)
    {
        std::lock_guard<std::mutex> lock(_publishMutex);
        auto& sections = _dirty[entry.botGuid];
        auto const key = static_cast<uint8>(entry.section);
        if (sections.find(key) == sections.end())
            ++_queuedSections;
        sections[key] = std::move(entry);

        std::lock_guard<std::mutex> statusLock(_statusMutex);
        _status.queuedSections = _queuedSections;
    }

    bool LiveStateService::ConsumeFullRefreshRequest(uint32 botGuid)
    {
        std::lock_guard<std::mutex> lock(_publishMutex);
        auto it = _fullRefreshRequested.find(botGuid);
        if (it == _fullRefreshRequested.end() || !it->second)
            return false;
        it->second = false;
        return true;
    }

    void LiveStateService::RequestFullRefresh(uint32 botGuid)
    {
        std::lock_guard<std::mutex> lock(_publishMutex);
        _fullRefreshRequested[botGuid] = true;
    }

    bool LiveStateService::IsFullRefreshRequested(uint32 botGuid) const
    {
        std::lock_guard<std::mutex> lock(_publishMutex);
        auto bot = _fullRefreshRequested.find(botGuid);
        auto sentinel = _fullRefreshRequested.find(0);
        return (bot != _fullRefreshRequested.end() && bot->second) ||
               (sentinel != _fullRefreshRequested.end() && sentinel->second);
    }

    void LiveStateService::ClearFullRefreshSentinel()
    {
        std::lock_guard<std::mutex> lock(_publishMutex);
        auto sentinel = _fullRefreshRequested.find(0);
        if (sentinel != _fullRefreshRequested.end())
            sentinel->second = false;
    }

    void LiveStateService::RequestFullRefreshAll()
    {
        std::lock_guard<std::mutex> lock(_publishMutex);
        for (auto& entry : _fullRefreshRequested)
            entry.second = true;
        _fullRefreshRequested[0] = true; // 0 = "every bot" sentinel
    }

    // ------------------------------------------------------------- async SQL
    void LiveStateService::SubmitAsyncQuery(std::string const& sql, std::function<void(QueryResult)> callback)
    {
        if (sql.empty() || !callback)
            return;
        _callbacks.AddCallback(
            CharacterDatabase.AsyncQuery(sql).WithCallback(std::move(callback)));
    }

    void LiveStateService::PumpCallbacks()
    {
        // Runs on the world thread: callbacks only touch value snapshots.
        _callbacks.ProcessReadyCallbacks();
    }

    void LiveStateService::RefreshBotControlAsync()
    {
        if (_botControlRefreshInFlight.exchange(true))
            return;

        std::string sql =
            "SELECT bot_guid, bot_name, enabled, bridge_enabled, mode, personality, affinity, current_goal, "
            "COALESCE(long_term_goal,''), autonomy_enabled, goal_status, COALESCE(goal_progress,''), "
            "COALESCE(goal_result,''), control_revision, COALESCE(master_guid,0), COALESCE(action_mode,'travel') "
            "FROM azeroth_friend_bots WHERE enabled = 1";

        SubmitAsyncQuery(sql, [this](QueryResult result)
        {
            _botControlRefreshInFlight.store(false);
            if (!result)
                return;

            std::unordered_map<uint32, BotControlRecord> refreshed;
            time_t now = std::time(nullptr);
            do
            {
                Field* fields = result->Fetch();
                BotControlRecord record;
                record.botGuid = fields[0].Get<uint32>();
                record.botName = fields[1].Get<std::string>();
                record.enabled = fields[2].Get<bool>();
                record.bridgeEnabled = fields[3].Get<bool>();
                record.mode = fields[4].Get<std::string>();
                record.personality = fields[5].Get<std::string>();
                record.affinity = fields[6].Get<int32>();
                record.currentGoal = fields[7].Get<std::string>();
                record.longTermGoal = fields[8].Get<std::string>();
                record.autonomyEnabled = fields[9].Get<bool>();
                record.goalStatus = fields[10].Get<std::string>();
                record.goalProgress = fields[11].Get<std::string>();
                record.goalResult = fields[12].Get<std::string>();
                record.controlRevision = fields[13].Get<uint32>();
                record.masterGuid = fields[14].Get<uint32>();
                record.actionMode = fields[15].Get<std::string>();
                record.refreshedAt = now;
                refreshed[record.botGuid] = record;
            } while (result->NextRow());

            std::lock_guard<std::mutex> lock(_cacheMutex);
            for (auto& entry : refreshed)
            {
                auto bridgeOverride = _bridgeOverrides.find(entry.first);
                if (bridgeOverride != _bridgeOverrides.end())
                    entry.second.bridgeEnabled = bridgeOverride->second;
                auto current = _botControl.find(entry.first);
                if (current == _botControl.end() || current->second.controlRevision <= entry.second.controlRevision)
                    _botControl[entry.first] = entry.second;
            }
        });
    }

    bool LiveStateService::GetBotControl(uint32 botGuid, BotControlRecord& out) const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto it = _botControl.find(botGuid);
        if (it == _botControl.end())
            return false;
        out = it->second;
        return true;
    }

    std::vector<BotControlRecord> LiveStateService::GetControlledBots() const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        std::vector<BotControlRecord> bots;
        bots.reserve(_botControl.size());
        for (auto const& entry : _botControl)
            bots.push_back(entry.second);
        return bots;
    }

    void LiveStateService::SetBotBridgeEnabledLocal(uint32 botGuid, bool enabled, bool bumpRevision)
    {
        if (!botGuid)
            return;

        {
            std::lock_guard<std::mutex> lock(_cacheMutex);
            BotControlRecord& record = _botControl[botGuid];
            record.botGuid = botGuid;
            record.enabled = true;
            record.bridgeEnabled = enabled;
            _bridgeOverrides[botGuid] = enabled;
            if (!enabled)
            {
                record.autonomyEnabled = false;
                record.goalStatus = "paused";
            }
            if (bumpRevision)
                ++record.controlRevision;
            record.refreshedAt = std::time(nullptr);
        }

        if (!enabled)
        {
            // Do not let observations queued immediately before Claim leak to the
            // bridge after the owner has disconnected this bot.
            std::lock_guard<std::mutex> lock(_publishMutex);
            auto it = _dirty.find(botGuid);
            if (it != _dirty.end())
            {
                uint32 removed = static_cast<uint32>(it->second.size());
                _queuedSections = removed < _queuedSections ? _queuedSections - removed : 0;
                _dirty.erase(it);
                std::lock_guard<std::mutex> statusLock(_statusMutex);
                _status.queuedSections = _queuedSections;
            }
        }
    }

    bool LiveStateService::IsBotBridgeEnabled(uint32 botGuid) const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto local = _bridgeOverrides.find(botGuid);
        if (local != _bridgeOverrides.end())
            return local->second;
        auto it = _botControl.find(botGuid);
        // Fail closed during cache warm-up. This is what makes a durable Claim
        // survive a worldserver restart without leaking events or live frames.
        return it != _botControl.end() && it->second.bridgeEnabled;
    }

    void LiveStateService::RefreshTelemetryAsync()
    {
        if (_telemetryRefreshInFlight.exchange(true))
            return;

        SubmitAsyncQuery(
            "SELECT co_processed_events, sensory_reused_events, tokens_saved FROM azeroth_friend_telemetry WHERE id = 1",
            [this](QueryResult result)
            {
                _telemetryRefreshInFlight.store(false);
                if (!result)
                    return;
                Field* fields = result->Fetch();
                std::lock_guard<std::mutex> lock(_cacheMutex);
                _telemetry.coProcessed = fields[0].Get<uint32>();
                _telemetry.sensoryReused = fields[1].Get<uint32>();
                _telemetry.tokensSaved = fields[2].Get<uint32>();
                _telemetry.refreshedAt = std::time(nullptr);
            });
    }

    TelemetryCounters LiveStateService::GetTelemetry() const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        return _telemetry;
    }

    void LiveStateService::RefreshActivePlansAsync()
    {
        if (_activePlanRefreshInFlight.exchange(true))
            return;

        // Exact pending/in-progress lookup replaces the old "latest five actions"
        // heuristic for active-plan detection.
        SubmitAsyncQuery(
            "SELECT bot_guid, COUNT(*) FROM azeroth_friend_actions "
            "WHERE status IN ('pending','in_progress') GROUP BY bot_guid",
            [this](QueryResult result)
            {
                _activePlanRefreshInFlight.store(false);
                std::unordered_map<uint32, bool> plans;
                std::unordered_map<uint32, uint32> steps;
                if (result)
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        uint32 botGuid = fields[0].Get<uint32>();
                        uint32 count = fields[1].Get<uint32>();
                        plans[botGuid] = count > 0;
                        steps[botGuid] = count;
                    } while (result->NextRow());
                }

                std::lock_guard<std::mutex> lock(_cacheMutex);
                _activePlans = std::move(plans);
                _activePlanSteps = std::move(steps);
            });
    }

    bool LiveStateService::HasActivePlan(uint32 botGuid) const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto it = _activePlans.find(botGuid);
        return it != _activePlans.end() && it->second;
    }

    uint32 LiveStateService::GetActivePlanSteps(uint32 botGuid) const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto it = _activePlanSteps.find(botGuid);
        return it == _activePlanSteps.end() ? 0 : it->second;
    }

    void LiveStateService::RefreshThoughtAsync(uint32 botGuid)
    {
        if (!botGuid)
            return;
        std::string sql = "SELECT COALESCE(last_thought,'') FROM azeroth_friend_state WHERE bot_guid = " +
            std::to_string(botGuid);
        SubmitAsyncQuery(sql, [this, botGuid](QueryResult result)
        {
            if (!result)
                return;
            std::string thought = result->Fetch()[0].Get<std::string>();
            std::lock_guard<std::mutex> lock(_cacheMutex);
            _thoughts[botGuid] = std::move(thought);
        });
    }

    std::string LiveStateService::GetThought(uint32 botGuid) const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto it = _thoughts.find(botGuid);
        return it == _thoughts.end() ? std::string() : it->second;
    }

    void LiveStateService::RefreshHistoryAsync(uint32 botGuid)
    {
        if (!botGuid)
            return;
        std::string sql = "SELECT action_type, status, COALESCE(failure_reason,''), COALESCE(thought,'') "
            "FROM azeroth_friend_actions WHERE bot_guid = " + std::to_string(botGuid) +
            " ORDER BY id DESC LIMIT 5";
        SubmitAsyncQuery(sql, [this, botGuid](QueryResult result)
        {
            std::ostringstream recent;
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    std::string thought = fields[3].Get<std::string>();
                    if (thought.size() > 240)
                        thought = thought.substr(0, 240);
                    recent << fields[0].Get<std::string>() << " -> " << fields[1].Get<std::string>() << " "
                           << fields[2].Get<std::string>() << "\n" << thought << "\n\n";
                } while (result->NextRow());
            }
            std::lock_guard<std::mutex> lock(_cacheMutex);
            _history[botGuid] = recent.str();
        });
    }

    std::string LiveStateService::GetHistory(uint32 botGuid) const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        auto it = _history.find(botGuid);
        return it == _history.end() ? std::string() : it->second;
    }

    void LiveStateService::PublishActionOutcome(uint32 botGuid, uint32 controlRevision, std::string const& resultText)
    {
        if (!botGuid)
            return;
        std::string payload = "{\"latest\":\"" + AzerothFriendShared::EscapeJsonString(resultText) +
            "\",\"at\":" + std::to_string(NowSeconds()) + "}";
        Publish(botGuid, LiveSection::Outcomes, payload, controlRevision);
    }

    LiveStateStatus LiveStateService::GetStatus() const
    {
        std::lock_guard<std::mutex> lock(_statusMutex);
        return _status;
    }

    BridgeDiagnostics LiveStateService::GetBridgeDiagnostics() const
    {
        std::lock_guard<std::mutex> lock(_cacheMutex);
        return _diagnostics;
    }
}
