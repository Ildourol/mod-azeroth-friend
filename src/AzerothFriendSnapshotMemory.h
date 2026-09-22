#pragma once

#include "Common.h"
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace AzerothFriend
{
    struct SnapshotEntry
    {
        time_t timestamp = 0;
        std::string json;
    };

    class SnapshotMemory
    {
    public:
        static SnapshotMemory& Instance();

        void StoreSnapshot(uint32 botGuid, std::string const& json, uint32 maxSnapshots = 3, uint32 ttlMinutes = 30);
        std::string GetLatestSnapshot(uint32 botGuid);
        uint32 GetSnapshotCount(uint32 botGuid);
        uint32 GetTotalSnapshots();
        void ClearAll();
        void PruneExpired(uint32 ttlMinutes = 30);

    private:
        SnapshotMemory() = default;
        ~SnapshotMemory() = default;
        SnapshotMemory(SnapshotMemory const&) = delete;
        SnapshotMemory& operator=(SnapshotMemory const&) = delete;

        std::mutex _mutex;
        std::map<uint32, std::deque<SnapshotEntry>> _snapshots;
    };
}
