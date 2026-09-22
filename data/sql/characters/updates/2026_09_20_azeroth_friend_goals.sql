-- Run against the characters database after the base schema. MySQL/MariaDB compatible.
-- INFORMATION_SCHEMA guards make this migration repeatable without resetting data.
DROP PROCEDURE IF EXISTS af_add_goal_column;
DELIMITER //
CREATE PROCEDURE af_add_goal_column(IN col_name VARCHAR(64), IN col_definition TEXT)
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_bots'
          AND COLUMN_NAME = col_name) THEN
        SET @af_ddl = CONCAT('ALTER TABLE azeroth_friend_bots ADD COLUMN ', col_name, ' ', col_definition);
        PREPARE af_stmt FROM @af_ddl;
        EXECUTE af_stmt;
        DEALLOCATE PREPARE af_stmt;
    END IF;
END//
DELIMITER ;
CALL af_add_goal_column('autonomy_enabled', 'TINYINT UNSIGNED NOT NULL DEFAULT 0');
CALL af_add_goal_column('goal_status', 'VARCHAR(16) NOT NULL DEFAULT ''paused''');
CALL af_add_goal_column('goal_progress', 'TEXT NULL');
CALL af_add_goal_column('goal_result', 'TEXT NULL');
CALL af_add_goal_column('control_revision', 'BIGINT UNSIGNED NOT NULL DEFAULT 0');
DROP PROCEDURE af_add_goal_column;
