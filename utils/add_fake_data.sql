-- Sample data script for matarelrato-db
-- Adds dummy users, matches, and logs for testing

USE `matarelrato-db`;

-- 1. Add Fake Users
INSERT INTO users (username, password_hash, skin_id, points) VALUES
('BoltyTheDog', 'hash1', 1, 650),
('MacabrePlayer', 'hash2', 2, 450),
('ShadowHunter', 'hash3', 3, 800),
('NoobMaster69', 'hash4', 0, 0);

-- 2. Add Fake Matches
-- Room 1: Finished Match
INSERT INTO matches (room_id, status, start_time, end_time, winner_id) 
VALUES (1, 'FINISHED', '2026-03-08 10:00:00', '2026-03-08 10:25:00', 1);

-- Room 2: Active Match
INSERT INTO matches (room_id, status, start_time) 
VALUES (2, 'PLAYING', NOW());

-- 3. Add Participants
-- Match 1 (Finished)
INSERT INTO match_participants (match_id, user_id, turn_order, finish_position) VALUES 
(1, 1, 1, 1),
(1, 2, 2, 2);

-- Match 2 (Active)
INSERT INTO match_participants (match_id, user_id, turn_order) VALUES 
(2, 3, 1),
(2, 4, 3);

-- 4. Add Match Events
INSERT INTO match_events (match_id, user_id, event_type, event_data) VALUES 
(1, 1, 'DICE_ROLL', '{"value": 6}'),
(1, 2, 'SMOKE', '{"duration": 5.5}'),
(2, 3, 'SHOOT', '{"target_id": 4, "result": "MISS"}');

