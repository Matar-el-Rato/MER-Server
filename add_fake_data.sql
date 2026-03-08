-- Sample data script for matarelrato-db
-- Adds dummy users, matches, and logs for testing

USE `matarelrato-db`;

-- 1. Add Fake Users
INSERT INTO users (username, password_hash, skin_id, points) VALUES 
('BoltyTheDog', 'hash1', 1, 1500),
('MacabrePlayer', 'hash2', 2, 800),
('ShadowHunter', 'hash3', 3, 2200),
('NoobMaster69', 'hash4', 0, 100);

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

-- 5. Add Chat Messages
INSERT INTO chat_messages (room_id, match_id, user_id, content) VALUES 
(1, 1, 1, 'GGEZ'),
(2, NULL, 3, 'Does anyone want to play in Room 2?'),
(2, 2, 4, 'Stop shooting me!');

-- 6. Update Room Occupancy
UPDATE rooms SET current_players = 2 WHERE id = 1;
UPDATE rooms SET current_players = 2 WHERE id = 2;
