-- ---------------------------------------------------------------------------
-- mod-azeroth-friend: versioned Playerbot Semantic Bot API v1
-- ---------------------------------------------------------------------------
-- Idempotent and additive. No companion, character, inventory, goal or
-- playerbot data is modified. Queue provenance prevents model-authored params
-- from elevating an autonomous action to owner authority.

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND COLUMN_NAME = 'origin_event_id');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD COLUMN `origin_event_id` BIGINT UNSIGNED NULL DEFAULT NULL AFTER `control_revision`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND COLUMN_NAME = 'authority');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD COLUMN `authority` ENUM(''autonomous'',''owner_command'',''gm_manual'') NOT NULL DEFAULT ''autonomous'' AFTER `origin_event_id`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_actions'
      AND COLUMN_NAME = 'origin_source_guid');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_actions` ADD COLUMN `origin_source_guid` INT UNSIGNED NULL DEFAULT NULL AFTER `authority`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Catalogue v1 metadata. LONGTEXT is used for schemas to retain compatibility
-- with MariaDB installations where JSON is an alias with stricter DDL behavior.
SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'api_version');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `api_version` TINYINT UNSIGNED NOT NULL DEFAULT 1 AFTER `safe`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'params_schema_json');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `params_schema_json` LONGTEXT NULL AFTER `api_version`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'result_schema_json');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `result_schema_json` LONGTEXT NULL AFTER `params_schema_json`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'binding_kind');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `binding_kind` VARCHAR(32) NOT NULL DEFAULT ''semantic_adapter'' AFTER `result_schema_json`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'native_binding');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `native_binding` VARCHAR(128) NULL AFTER `binding_kind`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'authority');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `authority` ENUM(''autonomous'',''owner_command'',''gm_manual'') NOT NULL DEFAULT ''autonomous'' AFTER `native_binding`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'preconditions');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `preconditions` VARCHAR(255) NULL AFTER `authority`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'completion_policy');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `completion_policy` VARCHAR(32) NOT NULL DEFAULT ''dispatch_acknowledged'' AFTER `preconditions`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_action_catalog'
      AND COLUMN_NAME = 'capability_revision');
SET @sql := IF(@has_col = 0,
    'ALTER TABLE `azeroth_friend_action_catalog` ADD COLUMN `capability_revision` INT UNSIGNED NOT NULL DEFAULT 1 AFTER `completion_policy`',
    'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
