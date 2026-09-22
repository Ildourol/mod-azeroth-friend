-- --------------------------------------------------------
-- mod-azeroth-friend: action execution layer
--
-- Adds the result/verification columns the executor writes for every step and the
-- catalogue table that mirrors mod-playerbots' own action list.
-- Every statement is idempotent (safe to re-run), and the module also self-heals
-- these objects at startup.
-- --------------------------------------------------------

ALTER TABLE `azeroth_friend_actions`
    ADD COLUMN IF NOT EXISTS `failure_reason` VARCHAR(255) DEFAULT NULL AFTER `status`;

ALTER TABLE `azeroth_friend_actions`
    ADD COLUMN IF NOT EXISTS `result_json` LONGTEXT DEFAULT NULL AFTER `failure_reason`;

ALTER TABLE `azeroth_friend_actions`
    ADD COLUMN IF NOT EXISTS `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP;

-- Steps that were in flight when the world restarted can never complete on their own.
UPDATE `azeroth_friend_actions`
SET `status` = 'interrupted',
    `failure_reason` = 'module restart while step in progress'
WHERE `status` = 'in_progress';

CREATE TABLE IF NOT EXISTS `azeroth_friend_action_catalog` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `action_name` VARCHAR(96) NOT NULL,
    `category` VARCHAR(48) NOT NULL DEFAULT 'general',
    `source` ENUM('curated', 'playerbots', 'strategy', 'command') NOT NULL DEFAULT 'curated',
    `params_schema` VARCHAR(128) DEFAULT NULL,
    `description` VARCHAR(255) DEFAULT NULL,
    `safe` TINYINT(1) NOT NULL DEFAULT 1,
    `exported_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `idx_catalog_unique` (`bot_guid`, `source`, `action_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
