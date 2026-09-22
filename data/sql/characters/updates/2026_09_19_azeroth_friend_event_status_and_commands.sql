-- --------------------------------------------------------
-- mod-azeroth-friend: Event status enum extension and command indexing
--
-- MAF-007: Adds 'superseded' and 'ignored_addon' to status enum in azeroth_friend_events
-- to prevent MySQL Error 1265 (Data truncated for column 'status') and bridge loop deadlock.
-- --------------------------------------------------------

ALTER TABLE `azeroth_friend_events`
    MODIFY COLUMN `status` ENUM('pending', 'processing', 'completed', 'expired', 'skipped', 'superseded', 'ignored_addon') NOT NULL DEFAULT 'pending';

-- Reset any stuck processing events
UPDATE `azeroth_friend_events`
SET `status` = 'skipped',
    `processed_at` = NOW()
WHERE `status` = 'processing' AND `created_at` < DATE_SUB(NOW(), INTERVAL 1 HOUR);
