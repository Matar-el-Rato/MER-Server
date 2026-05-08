#ifndef GAME_ACTIONS_H
#define GAME_ACTIONS_H

#include "protocol.h"
#include "db.h"
#include <pthread.h>

#define MAX_JSON_PAYLOAD 512
#define MAX_COLOR_LEN    8   /* "yellow\0" */

/* Client → Server action strings (REQ_GAME_ACTION payload "action" field) */
#define ACTION_CHOOSE_CHAIR        "choose_chair"
#define ACTION_ROLL_DICE           "roll_dice"
#define ACTION_MOVE_PIECE          "move_piece"
#define ACTION_USE_GUN             "use_gun"
#define ACTION_USE_CIGARETTE       "use_cigarette"
#define ACTION_USE_MAGNIFYING_GLASS "use_magnifying_glass"
#define ACTION_USE_HANDCUFFS       "use_handcuffs"
#define ACTION_USE_FIRE_AXE        "use_fire_axe"

/* Server → All action strings (MSG_GAME_ACTION broadcast "action" field) */
#define ACTION_CHAIR_TAKEN         "chair_taken"
#define ACTION_CHAIR_VACATED       "chair_vacated"
#define ACTION_CHAIRS_LOCKED       "chairs_locked"
#define ACTION_INITIATIVE_SEQUENCE "initiative_sequence"
#define ACTION_TURN_START          "turn_start"
#define ACTION_DICE_RESULT         "dice_result"
#define ACTION_PIECE_MOVED         "piece_moved"
#define ACTION_BARRIER_FORMED      "barrier_formed"
#define ACTION_BARRIER_BROKEN      "barrier_broken"
#define ACTION_CAPTURE             "capture"
#define ACTION_GOAL_SCORED         "goal_scored"
#define ACTION_TRIPLE_DOUBLE       "triple_double_penalty"
#define ACTION_EXTRA_TURN          "extra_turn"
#define ACTION_HANDCUFF_SKIP       "handcuff_skip"
#define ACTION_TURN_END            "turn_end"
#define ACTION_GOLDEN_SQUARE       "golden_square_event"
#define ACTION_GUN_RESULT          "gun_result"
#define ACTION_CIGARETTE_RESULT    "cigarette_result"
#define ACTION_MAGNIFYING_USED     "magnifying_glass_used"
#define ACTION_HANDCUFFS_APPLIED   "handcuffs_applied"
#define ACTION_BARRIER_DESTROYED   "barrier_destroyed"
#define ACTION_LIFE_LOST           "life_lost"
#define ACTION_PLAYER_ELIMINATED   "player_eliminated"
#define ACTION_GAME_OVER           "game_over"

/* Server → Single client action strings (private MSG_GAME_ACTION) */
#define ACTION_GUN_AVAILABLE       "gun_available"
#define ACTION_FIRE_AXE_AVAILABLE  "fire_axe_available"
#define ACTION_PEEK_RESULT         "peek_result"

/* Slot index → color name (matches client SlotColorNames, lowercased). */
extern const char *SLOT_COLORS[MAX_ROOM_PLAYERS]; /* {"blue","green","yellow","red"} */

/* One chair slot entry.  user_id == 0 → slot is free. */
typedef struct {
    int  user_id;
    int  skin_id;
    char username[MAX_USERNAME];
} chair_slot_t;

/* Chair-selection state for one room's active match. */
typedef struct {
    chair_slot_t slots[MAX_ROOM_PLAYERS]; /* slots[0..3] */
    int          match_id;
    int          room_id;
    int          player_count; /* how many chairs must be filled before chairs_locked fires */
} chair_state_t;

/* Per-room state; index 0 unused, 1-NUM_ROOMS active.
 * Protected by g_chair_mutex. */
extern chair_state_t   g_chair_state[NUM_ROOMS + 1];
extern pthread_mutex_t g_chair_mutex;

/* Turn-order state for one room's active match. */
typedef struct {
    int turn_order[MAX_ROOM_PLAYERS]; /* user_ids, winner first */
    int player_count;
    int current_idx;                  /* whose turn it currently is */
} turn_state_t;

/* Per-room turn state; index 0 unused, 1-NUM_ROOMS active.
 * Protected by g_turn_mutex. */
extern turn_state_t    g_turn_state[NUM_ROOMS + 1];
extern pthread_mutex_t g_turn_mutex;

/* Zero-initialises the chair state for a room at match start. */
void chair_state_init(int room_id, int match_id, int player_count);

/* Clears a player's chair claim (if any). Writes the freed color name into
 * color_out (must be >= MAX_COLOR_LEN bytes). Returns 1 if a chair was
 * released, 0 if the user had not claimed one.
 * Safe to call while holding g_live.mutex — uses its own g_chair_mutex. */
int chair_state_remove_user(int room_id, int user_id, char *color_out);

/* Parses the "action" field from json and dispatches to the right handler.
 * live / db / db_mutex are passed in to avoid exposing server.c statics. */
void handle_game_action(int fd, int user_id, const char *username,
                        int match_id, int room_id,
                        const char *json, int json_len,
                        client_list_t *live,
                        db_t *db, pthread_mutex_t *db_mutex);

/* Sends a MSG_GAME_ACTION broadcast to every client whose room_id matches.
 * Acquires live->mutex internally; must NOT be called while holding it. */
void broadcast_game_action_to_room(client_list_t *live, int room_id,
                                   const char *json, int json_len);

#endif /* GAME_ACTIONS_H */
