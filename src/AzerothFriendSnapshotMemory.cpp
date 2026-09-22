#include "AzerothFriendSnapshotMemory.h"
#include <algorithm>
#include <ctime>

namespace AzerothFriend
{
    SnapshotMemory& SnapshotMemory::Instance()
    {
        static SnapshotMemory instance;
        return instance;
    }

    void SnapshotMemory::StoreSnapshot(uint32 botGuid, std::string const& json, uint32 maxSnapshots, uint32 ttlMinutes)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        time_t now = std::time(nullptr);

        auto& queue = _snapshots[botGuid];
        queue.push_back({ now, json });

        // Maintain ring buffer limit
        while (queue.size() > maxSnapshots)
        {
            queue.pop_front();
        }

        // Purge expired entries for this bot
        time_t cutoff = now - (ttlMinutes * 60);
        while (!queue.empty() && queue.front().timestamp < cutoff)
        {
            queue.pop_front();
        }
    }

    std::string SnapshotMemory::GetLatestSnapshot(uint32 botGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _snapshots.find(botGuid);
        if (it != _snapshots.end() && !it->second.empty())
        {
            return it->second.back().json;
        }
        return "{}";
    }

    uint32 SnapshotMemory::GetSnapshotCount(uint32 botGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _snapshots.find(botGuid);
        if (it != _snapshots.end())
        {
            return static_cast<uint32>(it->second.size());
        }
        return 0;
    }

    uint32 SnapshotMemory::GetTotalSnapshots()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        uint32 total = 0;
        for (auto const& pair : _snapshots)
        {
            total += static_cast<uint32>(pair.second.size());
        }
        return total;
    }

    void SnapshotMemory::ClearAll()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _snapshots.clear();
    }

    void SnapshotMemory::PruneExpired(uint32 ttlMinutes)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        time_t now = std::time(nullptr);
        time_t cutoff = now - (ttlMinutes * 60);

        for (auto it = _snapshots.begin(); it != _snapshots.end();)
        {
            auto& queue = it->second;
            while (!queue.empty() && queue.front().timestamp < cutoff)
            {
                queue.pop_front();
            }
            if (queue.empty())
            {
                it = _snapshots.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
