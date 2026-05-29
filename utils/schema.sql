-- matarelrato-db initialization script

-- Creates the database if it doesn't exist. This is the entry point for the "Matar el Rato" backend.
CREATE DATABASE IF NOT EXISTS `matarelrato-db`;
USE `matarelrato-db`;

-- 1. Users
-- Stores permanent account data. 
-- Points and skin_id are nullable but default to 0 for a fresh start, will also implement password hashing to make it secure.
CREATE TABLE IF NOT EXISTS `users` (
    `id` INT AUTO_INCREMENT PRIMARY KEY,
    `username` VARCHAR(50) NOT NULL UNIQUE,
    `password_hash` VARCHAR(256) NOT NULL,
    `skin_id` INT DEFAULT 101,
    -- Every account starts with 500 points; +100 for a win, -50 for a loss (floored at 0).
    `points` INT NOT NULL DEFAULT 500,
    `created_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- 2. Rooms
-- Exists only as the FK target for matches.room_id. Room capacity / occupancy is
-- tracked in-memory by the server (MAX_ROOM_PLAYERS + the live client list), not here.
CREATE TABLE IF NOT EXISTS `rooms` (
    `id` INT PRIMARY KEY
) ENGINE=InnoDB;

-- 3. Matches
-- Records a single playthrough session.
-- FOREIGN KEY (room_id): ensures a match can only exist within a valid room.
-- FOREIGN KEY (winner_id): ensures the winner is a real user in the system.
CREATE TABLE IF NOT EXISTS `matches` (
    `id` INT AUTO_INCREMENT PRIMARY KEY,
    `room_id` INT NOT NULL,
    `status` ENUM('WAITING', 'PLAYING', 'FINISHED', 'CANCELLED') DEFAULT 'WAITING',
    `start_time` TIMESTAMP NULL DEFAULT NULL,
    `end_time` TIMESTAMP NULL DEFAULT NULL,
    `winner_id` INT NULL,
    FOREIGN KEY (`room_id`) REFERENCES `rooms`(`id`),
    FOREIGN KEY (`winner_id`) REFERENCES `users`(`id`)
) ENGINE=InnoDB;

-- 4. Match Participants
-- Links users to matches
-- PRIMARY KEY (match_id, user_id): ensures a player cannot join the same match twice.
-- ON DELETE CASCADE: if a match record is deleted, the participants link is automatically removed.
CREATE TABLE IF NOT EXISTS `match_participants` (
    `match_id` INT NOT NULL,
    `user_id` INT NOT NULL,
    `turn_order` INT NOT NULL,
    `chair_color` VARCHAR(10) DEFAULT NULL,
    `finish_position` INT DEFAULT NULL,
    PRIMARY KEY (`match_id`, `user_id`),
    FOREIGN KEY (`match_id`) REFERENCES `matches`(`id`) ON DELETE CASCADE,
    FOREIGN KEY (`user_id`) REFERENCES `users`(`id`)
) ENGINE=InnoDB;

-- 5. Match Events
-- The protocol of the game registered.
-- ON DELETE CASCADE: ensures logs are wiped if the match is deleted.
CREATE TABLE IF NOT EXISTS `match_events` (
    `id` BIGINT AUTO_INCREMENT PRIMARY KEY,
    `match_id` INT NOT NULL,
    -- NULL for match-wide events with no specific author (initiative_sequence,
    -- chairs_locked, ...). db_log_event in db.c inserts NULL when user_id <= 0.
    `user_id` INT NULL,
    `event_type` VARCHAR(30) NOT NULL,
    `event_data` JSON NOT NULL,
    `timestamp` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (`match_id`) REFERENCES `matches`(`id`) ON DELETE CASCADE,
    FOREIGN KEY (`user_id`) REFERENCES `users`(`id`) ON DELETE SET NULL
) ENGINE=InnoDB;

-- Initialization: Create the 3 local rooms.
-- INSERT IGNORE makes the script safe to run multiple times.
INSERT IGNORE INTO `rooms` (`id`) VALUES (1), (2), (3);
