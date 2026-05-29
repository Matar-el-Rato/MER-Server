-- Database cleanup script for matarelrato-db
-- CAUTION: This will delete everything except the rooms definitions

USE `matarelrato-db`;

-- Disabling foreign key checks to avoid deletion order issues
SET FOREIGN_KEY_CHECKS = 0;

-- Emptying all game-related tables
-- TRUNCATE is faster than DELETE and resets auto_increment IDs to 1
TRUNCATE TABLE match_events;
TRUNCATE TABLE match_participants;
TRUNCATE TABLE matches;
TRUNCATE TABLE users;

-- Re-enabling foreign key checks
SET FOREIGN_KEY_CHECKS = 1;

