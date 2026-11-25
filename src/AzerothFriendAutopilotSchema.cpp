#include "AzerothFriendAutopilotSchema.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>
#include <map>

namespace
{
    struct Column
    {
        const char* name;
        const char* definition;
    };

    struct Table
    {
        const char*         name;
        const char*         key;
        std::vector<Column> columns;
        const char*         indexes;
    };

    const std::vector<Table>& Layout()
    {
        static const std::vector<Table> tables = {
            {
                "azeroth_friend_autopilot", "bot_guid",
                {
                    { "bot_guid",       "BIGINT UNSIGNED NOT NULL PRIMARY KEY" },
                    { "mode",           "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "enrolled",       "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "source",         "VARCHAR(32) NOT NULL DEFAULT ''" },
                    { "style",          "VARCHAR(64) NOT NULL DEFAULT ''" },
                    { "outlook",        "VARCHAR(32) NOT NULL DEFAULT ''" },
                    { "profile",        "TEXT NULL" },
                    { "kills_total",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "deaths_total",   "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_total",   "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "dungeons_total", "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "doing",          "VARCHAR(64) NOT NULL DEFAULT ''" },
                    { "doing_since",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "decided_by",     "VARCHAR(16) NOT NULL DEFAULT ''" },
                    { "strategies",     "VARCHAR(2000) NOT NULL DEFAULT ''" },
                    { "last_results",   "VARCHAR(2000) NOT NULL DEFAULT ''" },
                    { "goal_kind",      "VARCHAR(24) NOT NULL DEFAULT ''" },
                    { "goal_target",    "VARCHAR(64) NOT NULL DEFAULT ''" },
                    { "goal_target_id", "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_value",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_baseline",  "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_set_at",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "goal_text",      "VARCHAR(255) NOT NULL DEFAULT ''" },
                    { "last_reason",    "VARCHAR(255) NOT NULL DEFAULT ''" },
                    { "baseline",       "VARCHAR(4000) NOT NULL DEFAULT ''" },
                    { "plan_until",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "last_plan_at",   "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "enrolled_at",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "updated_at",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                },
                "INDEX idx_enrolled (enrolled)"
            },
            {
                "azeroth_friend_autopilot_snapshots", "id",
                {
                    { "id",              "BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY" },
                    { "bot_guid",        "BIGINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "taken_at",        "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "level",           "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "xp",              "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "money",           "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "map_id",          "SMALLINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "zone_id",         "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "area_id",         "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "durability_pct",  "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "free_bag_slots",  "SMALLINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_active",   "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_rewarded", "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "kills_total",     "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "deaths_total",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "quests_total",    "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "professions",     "VARCHAR(255) NOT NULL DEFAULT ''" },
                },
                "INDEX idx_bot_time (bot_guid, taken_at)"
            },
            {
                "azeroth_friend_autopilot_events", "id",
                {
                    { "id",       "BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY" },
                    { "bot_guid", "BIGINT UNSIGNED NOT NULL DEFAULT 0" },
                    { "at",       "INT UNSIGNED NOT NULL DEFAULT 0" },
                    { "type",     "VARCHAR(32) NOT NULL DEFAULT ''" },
                    { "detail",   "VARCHAR(255) NOT NULL DEFAULT ''" },
                },
                "INDEX idx_bot_id (bot_guid, id)"
            },
        };
        return tables;
    }

    bool TableExists(const char* table)
    {
        std::string sql = "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '" + std::string(table) + "'";
        QueryResult r = CharacterDatabase.Query(sql.c_str());
        return r && (*r)[0].Get<uint64>() > 0;
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    struct Existing
    {
        uint64_t length   = 0;
        bool     required = false;
    };

    std::map<std::string, Existing> ColumnsOf(const char* table)
    {
        std::map<std::string, Existing> out;
        std::string sql = "SELECT column_name, IFNULL(character_maximum_length, 0), "
                          "CAST(is_nullable = 'NO' AND column_default IS NULL AND extra NOT LIKE '%auto_increment%' AS UNSIGNED) "
                          "FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = '" + std::string(table) + "'";
        if (QueryResult r = CharacterDatabase.Query(sql.c_str()))
        {
            do
            {
                Existing& e = out[Lower((*r)[0].Get<std::string>())];
                e.length   = (*r)[1].Get<uint64>();
                e.required = (*r)[2].Get<uint64>() != 0;
            } while (r->NextRow());
        }
        return out;
    }

    uint64_t VarcharLength(const char* definition)
    {
        const std::string d = definition;
        if (d.rfind("VARCHAR(", 0) != 0)
            return 0;
        const size_t close = d.find(')', 8);
        if (close == std::string::npos)
            return 0;
        return std::stoull(d.substr(8, close - 8));
    }
}

namespace AzerothFriendAutopilotSchema
{
    void EnsureTables()
    {
        for (const Table& t : Layout())
        {
            if (!TableExists(t.name))
            {
                std::ostringstream ss;
                ss << "CREATE TABLE IF NOT EXISTS `" << t.name << "` (";
                for (size_t i = 0; i < t.columns.size(); ++i)
                {
                    if (i > 0)
                        ss << ", ";
                    ss << "`" << t.columns[i].name << "` " << t.columns[i].definition;
                }
                if (t.indexes && *t.indexes)
                    ss << ", " << t.indexes;
                ss << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

                CharacterDatabase.DirectExecute(ss.str().c_str());
                LOG_INFO("module.azerothfriend", "Autopilot schema: created table '{}'.", t.name);
                continue;
            }

            const auto existing = ColumnsOf(t.name);
            if (existing.find(Lower(t.key)) == existing.end())
            {
                LOG_WARN("module.azerothfriend",
                    "Autopilot schema: table '{}' exists but has no '{}' column; skipping self-heal.",
                    t.name, t.key);
                continue;
            }

            for (const Column& c : t.columns)
            {
                auto it = existing.find(Lower(c.name));
                if (it == existing.end())
                {
                    std::string sql = "ALTER TABLE `" + std::string(t.name) + "` ADD COLUMN `" +
                                      std::string(c.name) + "` " + std::string(c.definition);
                    CharacterDatabase.DirectExecute(sql.c_str());
                    LOG_INFO("module.azerothfriend", "Autopilot schema: added missing column '{}.{}'.",
                             t.name, c.name);
                    continue;
                }

                const uint64_t want = VarcharLength(c.definition);
                if (want > 0 && it->second.length > 0 && it->second.length < want)
                {
                    std::string sql = "ALTER TABLE `" + std::string(t.name) + "` MODIFY COLUMN `" +
                                      std::string(c.name) + "` " + std::string(c.definition);
                    CharacterDatabase.DirectExecute(sql.c_str());
                    LOG_INFO("module.azerothfriend", "Autopilot schema: widened '{}.{}' ({} -> {} chars).",
                             t.name, c.name, it->second.length, want);
                }
            }
        }
    }
}
