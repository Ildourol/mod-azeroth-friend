-- ---------------------------------------------------------------------------
-- mod-azeroth-friend: RAM-first context, structured summaries and retention
-- ---------------------------------------------------------------------------
-- Idempotent. Never drops or rewrites companion personality, goals, inventory,
-- levels or playerbot records. Safe to apply repeatedly on a populated database.
--
-- New responsibilities:
--   * azeroth_friend_summaries           -> zero-token structured summaries
--   * azeroth_friend_consumer_checkpoints-> incremental chatter ingestion cursors
--   * queue metadata                     -> control revision, expiry, dedup keys
--   * guarded indexes                    -> pending selection, active plans,
--                                          completion cleanup, bot/goal summaries
-- ---------------------------------------------------------------------------

-- 1. Structured summaries (goal identity + revision + source outcome ids).
CREATE TABLE IF NOT EXISTS `azeroth_friend_summaries` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` INT UNSIGNED NOT NULL,
    `goal_identity` VARCHAR(32) NOT NULL DEFAULT '',
    `revision` INT UNSIGNED NOT NULL DEFAULT 0,
    `window_start` TIMESTAMP NULL DEFAULT NULL,
    `window_end` TIMESTAMP NULL DEFAULT NULL,
    `content` TEXT NOT NULL,
    `payload_json` LONGTEXT DEFAULT NULL,
    `source_outcome_ids` LONGTEXT DEFAULT NULL,
    `summary_key` VARCHAR(64) NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `idx_summary_key` (`summary_key`),
    KEY `idx_summary_bot_goal` (`bot_guid`, `goal_identity`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 2. Incremental chatter consumer checkpoints (bounded overlap, source dedup).
CREATE TABLE IF NOT EXISTS `azeroth_friend_consumer_checkpoints` (
    `consumer` VARCHAR(32) NOT NULL,
    `last_event_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `last_message_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `overlap_seconds` INT UNSIGNED NOT NULL DEFAULT 30,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`consumer`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 3. Queue metadata: control revision, expiry and reliable deduplication keys.
SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND COLUMN_NAME = 'control_revision');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD COLUMN `control_revision` INT UNSIGNED NULL DEFAULT NULL',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND COLUMN_NAME = 'expires_at');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD COLUMN `expires_at` TIMESTAMP NULL DEFAULT NULL',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND COLUMN_NAME = 'dedup_key');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD COLUMN `dedup_key` VARCHAR(64) NULL DEFAULT NULL',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_events'
      AND COLUMN_NAME = 'expires_at');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_events` ADD COLUMN `expires_at` TIMESTAMP NULL DEFAULT NULL',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_events'
      AND COLUMN_NAME = 'dedup_key');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_events` ADD COLUMN `dedup_key` VARCHAR(64) NULL DEFAULT NULL',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4. Guarded indexes supporting the new access patterns.
SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_events'
      AND INDEX_NAME = 'idx_events_dedup');
SET @sql := IF(@has_idx = 0,
    'ALTER TABLE `azeroth_friend_events` ADD INDEX `idx_events_dedup` (`bot_guid`, `dedup_key`, `created_at`)',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND INDEX_NAME = 'idx_actions_active');
SET @sql := IF(@has_idx = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD INDEX `idx_actions_active` (`bot_guid`, `status`, `plan_id`, `step_index`)',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND INDEX_NAME = 'idx_actions_completion');
SET @sql := IF(@has_idx = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD INDEX `idx_actions_completion` (`status`, `completed_at`)',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND INDEX_NAME = 'idx_actions_dedup');
SET @sql := IF(@has_idx = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD INDEX `idx_actions_dedup` (`bot_guid`, `dedup_key`)',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 5. Catalogue cache table: runtime creation was removed so migrations own the
--    schema. Guarded because the table also exists in the installation schema.
CREATE TABLE IF NOT EXISTS `azeroth_friend_action_catalog` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `action_name` VARCHAR(96) NOT NULL,
    `category` VARCHAR(48) NOT NULL DEFAULT 'general',
    `source` ENUM('curated','playerbots','strategy','command') NOT NULL DEFAULT 'curated',
    `params_schema` VARCHAR(128) DEFAULT NULL,
    `description` VARCHAR(255) DEFAULT NULL,
    `safe` TINYINT(1) NOT NULL DEFAULT 1,
    `exported_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `idx_catalog_unique` (`bot_guid`, `source`, `action_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 6. Pre-upgrade rows in azeroth_friend_memory stay untouched: they remain
--    diagnostic history and are excluded from automatic prompts. No DELETE,
--    UPDATE or structural change is applied to companion or character data.
