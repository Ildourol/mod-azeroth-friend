-- ---------------------------------------------------------------------------
-- mod-azeroth-friend: durable per-bot Claim / bridge connection gate
-- ---------------------------------------------------------------------------
-- Idempotent and safe on populated databases. Existing companions remain
-- connected by default; no personality, goal, character or playerbot data is
-- rewritten.

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_bots'
      AND COLUMN_NAME = 'bridge_enabled');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_bots` ADD COLUMN `bridge_enabled` TINYINT(1) NOT NULL DEFAULT 1 AFTER `enabled`',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_bots'
      AND INDEX_NAME = 'idx_bridge_enabled');
SET @sql := IF(@has_idx = 0,
    'ALTER TABLE `azeroth_friend_bots` ADD INDEX `idx_bridge_enabled` (`enabled`, `bridge_enabled`)',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
