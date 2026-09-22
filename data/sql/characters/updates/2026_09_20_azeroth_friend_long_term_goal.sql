-- Run against the characters database after the base schema. MySQL/MariaDB compatible.
-- Adds the long-term goal: the companion's enduring purpose, which the autonomy loop
-- uses to derive its own short-term goal (the existing current_goal column).
-- Idempotent so databases created before this migration pick it up without a reset.
DROP PROCEDURE IF EXISTS af_add_long_term_goal;
DELIMITER //
CREATE PROCEDURE af_add_long_term_goal()
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'azeroth_friend_bots'
          AND COLUMN_NAME = 'long_term_goal') THEN
        ALTER TABLE azeroth_friend_bots ADD COLUMN long_term_goal TEXT NULL AFTER current_goal;
    END IF;
END//
DELIMITER ;
CALL af_add_long_term_goal();
DROP PROCEDURE af_add_long_term_goal;
