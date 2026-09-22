#ifndef MOD_AZEROTH_FRIEND_LIVESTATE_H
#define MOD_AZEROTH_FRIEND_LIVESTATE_H

#include "Common.h"
#include "DatabaseEnv.h"
#include "AsyncCallbackProcessor.h"
#include "Player.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace boost { namespace asio { class io_context; } }

namespace AzerothFriend
{
    // ---------------------------------------------------------------------
    // Value-only snapshots. The world thread never touches a Player* here:
    // everything is copied under the lock as plain values.
    // ---------------------------------------------------------------------
    enum class LiveSection : uint8
    {
        Vitals = 0,
        Surroundings = 1,
        Owner = 2,
        Control = 3,
        Capabilities = 4,
        Outcomes = 5
    };

    char const* LiveSectionName(LiveSection section);

    struct PendingPublish
    {
        uint32 botGuid = 0;
        uint32 controlRevision = 0;
        int64 observationTs = 0;
        LiveSection section = LiveSection::Vitals;
        std::string payloadJson;
    };

    // Durable control/goal record, refreshed asynchronously (never queried by
    // the world thread) and used for telemetry plus the control publish section.
    struct BotControlRecord
    {
        uint32 botGuid = 0;
        std::string botName;
        std::string mode = "companion";
        std::string actionMode = "travel";
        std::string personality;
        int32 affinity = 0;
        std::string currentGoal;
        std::string longTermGoal;
        bool autonomyEnabled = false;
        std::string goalStatus = "paused";
        std::string goalProgress;
        std::string goalResult;
        uint32 controlRevision = 0;
        uint32 masterGuid = 0;
        bool enabled = true;
        bool bridgeEnabled = true;
        uint32 capabilityRevision = 0;
        time_t refreshedAt = 0;
    };

    struct TelemetryCounters
    {
        uint32 coProcessed = 0;
        uint32 sensoryReused = 0;
        uint32 tokensSaved = 0;
        time_t refreshedAt = 0;
    };

    struct BotActionSummary
    {
        std::string text;
        time_t at = 0;
    };

    // Diagnostics the bridge reports back over the live-state socket, so the
    // addon can render cache freshness, context size and summary timestamps
    // without doing any maths of its own.
    struct BridgeDiagnostics
    {
        uint32 contextTokens = 0;
        uint64 cacheBytes = 0;
        uint32 cacheBots = 0;
        int64 summaryTimestamp = 0;
        std::string lastDiagnostic;
        time_t receivedAt = 0;
    };

    struct LiveStateStatus
    {
        bool running = false;
        bool bridgeConnected = false;
        bool bridgeAuthorized = false;
        std::string sessionId;
        uint64 framesSent = 0;
        uint64 framesRejected = 0;
        uint64 refreshesRequested = 0;
        uint32 queuedSections = 0;
        time_t lastClientActivity = 0;
        time_t lastDiagnostics = 0;
    };

    /**
     * RAM-first live state service.
     *
     * Two responsibilities, both explicitly separated by thread:
     *   1. World-thread side: publish value snapshots, answer cache reads, and
     *      pump asynchronous SQL callbacks. No socket or synchronous SQL I/O.
     *   2. Worker-thread side: serialize frames, transmit them over a loopback
     *      TCP connection and read bridge frames (auth/refresh/diag). All socket
     *      handlers run exclusively on that single worker thread, which is the
     *      threading model Boost.Asio documents for an io_context with one
     *      thread.
     */
    class LiveStateService
    {
    public:
        static LiveStateService& Instance();

        // ------------------------------------------------------------ lifecycle
        void Start();
        void Stop();
        bool IsRunning() const;

        // ------------------------------------------------------- world-thread API
        // Publish a value snapshot. Serialization is expected to have happened
        // already; the call only copies strings under a short lock.
        void Publish(uint32 botGuid, LiveSection section, std::string const& payloadJson,
                     uint32 controlRevision);

        // A complete baseline has to be re-sent (new connection, gap, recovery).
        bool ConsumeFullRefreshRequest(uint32 botGuid);
        bool IsFullRefreshRequested(uint32 botGuid) const;
        void ClearFullRefreshSentinel();
        void RequestFullRefresh(uint32 botGuid);
        void RequestFullRefreshAll();

        // Async SQL: schedule a query whose callback runs on the world thread
        // during PumpCallbacks(). Callbacks must only capture values.
        void SubmitAsyncQuery(std::string const& sql, std::function<void(QueryResult)> callback);
        void PumpCallbacks();

        // ------------------------------------------------------- cached reads
        void RefreshBotControlAsync();
        bool GetBotControl(uint32 botGuid, BotControlRecord& out) const;
        std::vector<BotControlRecord> GetControlledBots() const;
        // Owner commands update this value synchronously in RAM before their
        // asynchronous SQL write, closing the claim/release race window.
        void SetBotBridgeEnabledLocal(uint32 botGuid, bool enabled, bool bumpRevision = true);
        bool IsBotBridgeEnabled(uint32 botGuid) const;
        void RefreshTelemetryAsync();
        TelemetryCounters GetTelemetry() const;
        void RefreshActivePlansAsync();
        bool HasActivePlan(uint32 botGuid) const;
        uint32 GetActivePlanSteps(uint32 botGuid) const;
        void RefreshThoughtAsync(uint32 botGuid);
        std::string GetThought(uint32 botGuid) const;
        void RefreshHistoryAsync(uint32 botGuid);
        std::string GetHistory(uint32 botGuid) const;

        // Action outcomes feed the addon and the zero-token summary source list.
        void PublishActionOutcome(uint32 botGuid, uint32 controlRevision, std::string const& resultText);

        // ------------------------------------------------------- status/diagnostics
        LiveStateStatus GetStatus() const;
        BridgeDiagnostics GetBridgeDiagnostics() const;

        // Transport tuning accessors used by the worker thread.
        std::string const& liveStateHost() const;
        uint32 liveStatePort() const;
        uint32 frameCapBytes() const;
        uint32 publishIntervalMs() const;
        uint32 heartbeatSeconds() const;
        uint32 queueLimit() const;
        bool secretMatches(std::string const& provided) const;

    private:
        LiveStateService() = default;
        // Declared out-of-line: the io_context member is an incomplete type here.
        ~LiveStateService();
        LiveStateService(LiveStateService const&) = delete;
        LiveStateService& operator=(LiveStateService const&) = delete;

        // -------------------------------------------------- world-thread internals
        void PublishLocked(PendingPublish&& entry);

        mutable std::mutex _publishMutex;
        std::map<uint32, std::map<uint8, PendingPublish>> _dirty;
        std::map<uint32, bool> _fullRefreshRequested;
        uint32 _queuedSections = 0;

        mutable std::mutex _cacheMutex;
        std::unordered_map<uint32, BotControlRecord> _botControl;
        // Last owner-issued Claim/Release decision. Keeping this small local
        // override prevents an older async SELECT result from reopening the
        // bridge while its ordered UPDATE is still crossing the DB worker.
        std::unordered_map<uint32, bool> _bridgeOverrides;
        TelemetryCounters _telemetry;
        std::unordered_map<uint32, bool> _activePlans;
        std::unordered_map<uint32, uint32> _activePlanSteps;
        std::unordered_map<uint32, std::string> _thoughts;
        std::unordered_map<uint32, std::string> _history;
        BridgeDiagnostics _diagnostics;
        std::atomic<bool> _botControlRefreshInFlight{ false };
        std::atomic<bool> _telemetryRefreshInFlight{ false };
        std::atomic<bool> _activePlanRefreshInFlight{ false };

        // Async DB callbacks are invoked by PumpCallbacks() on the world thread.
        QueryCallbackProcessor _callbacks;

        // ----------------------------------------------------- worker internals
        // Everything below is touched by the io_context worker thread only,
        // except the two atomics and the publish queues above.
        std::unique_ptr<boost::asio::io_context> _io;
        std::unique_ptr<class LiveStateTransportWorker> _worker;
        std::thread _thread;
        std::atomic<bool> _running{ false };

        // Transport status mirror, written by the worker, read by the world thread.
        mutable std::mutex _statusMutex;
        LiveStateStatus _status;

        friend class LiveStateTransportWorker;
    };
}

// Address-of the reference keeps the `sAFLiveState->X()` call style used by the
// other module singletons (sAFEnvironment, sAFDispatcher).
#define sAFLiveState (&AzerothFriend::LiveStateService::Instance())

#endif // MOD_AZEROTH_FRIEND_LIVESTATE_H
