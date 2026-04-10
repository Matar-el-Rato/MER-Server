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
    `points` INT DEFAULT 0,
    `created_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- 2. Rooms
-- Tracks the 3 physical server rooms.
-- current_players field allows the server to check for capacity before letting someone join.
CREATE TABLE IF NOT EXISTS `rooms` (
    `id` INT PRIMARY KEY,
    `max_players` INT DEFAULT 4,
    `current_players` INT DEFAULT 0
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
    `user_id` INT NOT NULL,
    `event_type` VARCHAR(30) NOT NULL,
    `event_data` JSON NOT NULL,
    `timestamp` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (`match_id`) REFERENCES `matches`(`id`) ON DELETE CASCADE,
    FOREIGN KEY (`user_id`) REFERENCES `users`(`id`)
) ENGINE=InnoDB;

-- 6. Chat Messages
-- Chat messages registered.
-- match_id is NULL for lobby chat, but populated for in-game "trash talk".
CREATE TABLE IF NOT EXISTS `chat_messages` (
    `id` BIGINT AUTO_INCREMENT PRIMARY KEY,
    `room_id` INT NOT NULL,
    `match_id` INT DEFAULT NULL,
    `user_id` INT NOT NULL,
    `content` TEXT NOT NULL,
    `sent_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (`room_id`) REFERENCES `rooms`(`id`),
    FOREIGN KEY (`match_id`) REFERENCES `matches`(`id`) ON DELETE CASCADE,
    FOREIGN KEY (`user_id`) REFERENCES `users`(`id`)
) ENGINE=InnoDB;

-- Initialization: Create the 3 local rooms.
-- ON DUPLICATE KEY UPDATE: makes the script safe to run multiple times.
-- If the room exists, it updates max_players but DOES NOT reset current_players.
INSERT INTO `rooms` (`id`, `max_players`, `current_players`) 
VALUES (1, 4, 0), (2, 4, 0), (3, 4, 0)
ON DUPLICATE KEY UPDATE `max_players` = VALUES(`max_players`);
