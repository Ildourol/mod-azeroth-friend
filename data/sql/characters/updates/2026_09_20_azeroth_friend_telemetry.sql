-- Run against the characters database after the base schema. MySQL/MariaDB compatible.
-- The Python bridge creates this table lazily; the C++ `.af status` reader expects it.
-- Idempotent so databases created before this migration pick it up without a reset.
CREATE TABLE IF NOT EXISTS `azeroth_friend_telemetry` (
    `id` INT UNSIGNED NOT NULL,
    `co_processed_events` INT UNSIGNED NOT NULL DEFAULT 0,
    `sensory_reused_events` INT UNSIGNED NOT NULL DEFAULT 0,
    `tokens_saved` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
