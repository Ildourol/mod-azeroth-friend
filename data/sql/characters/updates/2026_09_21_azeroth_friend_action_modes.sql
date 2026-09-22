-- ---------------------------------------------------------------------------
-- AzerothFriend Idempotent Migration: Action Modes & Behavioral Postures
-- Adds `action_mode` ENUM('combat', 'travel', 'idle', 'social') to azeroth_friend_bots.
-- ---------------------------------------------------------------------------

SET @col_exists = (
    SELECT COUNT(*)
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'azeroth_friend_bots'
      AND COLUMN_NAME = 'action_mode'
);

SET @query = IF(@col_exists = 0,
    'ALTER TABLE `azeroth_friend_bots` ADD COLUMN `action_mode` ENUM(\'combat\', \'travel\', \'idle\', \'social\') NOT NULL DEFAULT \'travel\' AFTER `bridge_enabled`',
    'SELECT "Column action_mode already exists in azeroth_friend_bots"'
);

PREPARE stmt FROM @query;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
