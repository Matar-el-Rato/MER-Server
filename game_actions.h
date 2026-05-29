#ifndef GAME_ACTIONS_H
#define GAME_ACTIONS_H

#include "protocol.h"
#include "db.h"
#include <pthread.h>
#include <stdbool.h>

#define MAX_JSON_PAYLOAD 2048
#define MAX_COLOR_LEN    8   /* "yellow\0" */

/* Client → Server action strings (REQ_GAME_ACTION payload "action" field) */
#define ACTION_CHOOSE_CHAIR         "choose_chair"
#define ACTION_ROLL_DICE            "roll_dice"
#define ACTION_MOVE_PIECE           "move_piece"
#define ACTION_USE_GUN              "use_gun"
#define ACTION_USE_CIGARETTE        "use_cigarette"
#define ACTION_USE_MAGNIFYING_GLASS "use_magnifying_glass"
#define ACTION_USE_HANDCUFFS        "use_handcuffs"
#define ACTION_USE_FIRE_AXE         "use_fire_axe"

/* Server → All action strings (MSG_GAME_ACTION broadcast "action" field) */
#define ACTION_CHAIR_TAKEN          "chair_taken"
#define ACTION_CHAIR_VACATED        "chair_vacated"
#define ACTION_CHAIRS_LOCKED        "chairs_locked"
#define ACTION_INITIATIVE_SEQUENCE  "initiative_sequence"
#define ACTION_TURN_START           "turn_start"
#define ACTION_DICE_RESULT          "dice_result"
#define ACTION_PIECE_MOVED          "piece_moved"
#define ACTION_BARRIER_FORMED       "barrier_formed"
#define ACTION_BARRIER_BROKEN       "barrier_broken"
#define ACTION_CAPTURE              "capture"
#define ACTION_GOAL_SCORED          "goal_scored"
#define ACTION_TRIPLE_DOUBLE        "triple_double_penalty"
#define ACTION_EXTRA_TURN           "extra_turn"
#define ACTION_HANDCUFF_SKIP        "handcuff_skip"
#define ACTION_TURN_END             "turn_end"
#define ACTION_GOLDEN_SQUARE        "golden_square_event"
#define ACTION_GUN_RESULT           "gun_result"
#define ACTION_CIGARETTE_RESULT     "cigarette_result"
#define ACTION_MAGNIFYING_USED      "magnifying_glass_used"
#define ACTION_MAGNIFYING_RESULT    "magnifying_glass_result"
#define ACTION_HANDCUFFS_APPLIED    "handcuffs_applied"
#define ACTION_FIRE_AXE_USED        "fire_axe_used"
#define ACTION_BARRIER_DESTROYED    "barrier_destroyed"
#define ACTION_LIFE_LOST            "life_lost"
#define ACTION_PLAYER_ELIMINATED    "player_eliminated"
#define ACTION_GAME_OVER            "game_over"
#define ACTION_TURN_TIMER_WARNING   "turn_timer_warning"
#define ACTION_TURN_TIMER_EXPIRED   "turn_timer_expired"

/* Turn timer: 60 s total, warnings at 30 / 10 / 5 s remaining. */
#define TURN_TIMER_SECS             60

/* Server → Single client action strings (private MSG_GAME_ACTION) */
#define ACTION_GUN_AVAILABLE        "gun_available"
#define ACTION_FIRE_AXE_AVAILABLE   "fire_axe_available"
#define ACTION_PEEK_RESULT          "peek_result"

/* Item IDs: 0=gun 1=cigarette 2=magnifying_glass 3=handcuffs 4=fire_axe */
#define ITEM_GUN              0
#define ITEM_CIGARETTE        1
#define ITEM_MAGNIFYING_GLASS 2
#define ITEM_HANDCUFFS        3
#define ITEM_FIRE_AXE         4
#define NUM_ITEMS             5

/* Slot index → color name (matches client SlotColorNames, lowercased).
 * slot 0=blue, 1=green, 2=yellow, 3=red */
extern const char *SLOT_COLORS[MAX_ROOM_PLAYERS];

/* ── Data structures ─────────────────────────────────────────────────────── */

typedef struct {
    int  user_id;
    int  skin_id;
    char username[MAX_USERNAME];
} chair_slot_t;

typedef struct {
    chair_slot_t slots[MAX_ROOM_PLAYERS];
    int          match_id;
    int          room_id;
    int          player_count;
} chair_state_t;

typedef struct {
    int turn_order[MAX_ROOM_PLAYERS];
    int player_count;
    int current_idx;
} turn_state_t;

/* Full in-memory game state for one active room. */
typedef struct {
    bool active;                          /* true once match starts        */
    int  match_id;
    int  room_id;
    int  player_count;

    /* Board state */
    int  piece_positions[MAX_ROOM_PLAYERS][4]; /* [slot][piece_id], 0=home  */
    int  golden_squares[4];                    /* one per quadrant          */

    /* Current-turn state */
    int  pending_die1;                    /* stored after roll_dice        */
    int  pending_die2;
    int  pending_movements[MAX_ROOM_PLAYERS];    /* bonus steps from captures/goals */
    bool pending_doubles_reroll[MAX_ROOM_PLAYERS]; /* doubles reroll deferred past bonus moves */
    bool pending_five_reroll[MAX_ROOM_PLAYERS];    /* grant reroll after 5+5 exit+second move  */

    /* Per-player persistent state */
    int  consecutive_doubles[MAX_ROOM_PLAYERS];
    int  life_charges[MAX_ROOM_PLAYERS];       /* starts at 3               */
    bool is_eliminated[MAX_ROOM_PLAYERS];
    bool is_handcuffed[MAX_ROOM_PLAYERS];
    bool has_item[MAX_ROOM_PLAYERS][NUM_ITEMS];

    /* Magnifying-glass pre-roll */
    bool peeked[MAX_ROOM_PLAYERS];
    int  peeked_die1[MAX_ROOM_PLAYERS];
    int  peeked_die2[MAX_ROOM_PLAYERS];

    /* Pending gun choice: set when attacker lands on an enemy while holding the gun.
     * Turn flow is suspended until the attacker sends use_gun {choice: fire|skip}. */
    bool pending_gun_choice[MAX_ROOM_PLAYERS];
    int  pending_gun_piece_moved[MAX_ROOM_PLAYERS];    /* attacker's piece_id         */
    int  pending_gun_victim_slot[MAX_ROOM_PLAYERS];
    int  pending_gun_victim_piece[MAX_ROOM_PLAYERS];
    int  pending_gun_at_sq[MAX_ROOM_PLAYERS];          /* landing square              */
    bool pending_gun_is_doubles[MAX_ROOM_PLAYERS];
    int  pending_gun_second_move_die[MAX_ROOM_PLAYERS];
} game_state_t;

/* Per-room state arrays; index 0 unused, 1-NUM_ROOMS active. */
extern chair_state_t   g_chair_state[NUM_ROOMS + 1];
extern pthread_mutex_t g_chair_mutex;

extern turn_state_t    g_turn_state[NUM_ROOMS + 1];
extern pthread_mutex_t g_turn_mutex;

extern game_state_t    g_game_state[NUM_ROOMS + 1];
extern pthread_mutex_t g_game_mutex;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Reads config.txt and applies testing overrides. Called automatically at each
 * initiative_sequence; call manually if you need an earlier load. */
void config_load(const char *path);

void chair_state_init(int room_id, int match_id, int player_count);

int chair_state_remove_user(int room_id, int user_id, char *color_out);

void handle_game_action(int fd, int user_id, const char *username,
                        int match_id, int room_id,
                        const char *json, int json_len,
                        client_list_t *live,
                        db_t *db, pthread_mutex_t *db_mutex);

/* Broadcasts a MSG_GAME_ACTION to every client in the room. */
void broadcast_game_action_to_room(client_list_t *live, int room_id,
                                   const char *json, int json_len);

/* Called when a player disconnects mid-match.
 * Clears their pieces from board state, removes them from the turn rotation,
 * and (if it was their turn) cancels the timer and starts the next player's turn. */
void handle_player_disconnected(int room_id, int disc_user_id, const char *color,
                                 client_list_t *live, db_t *db,
                                 pthread_mutex_t *db_mutex, int match_id);

/* Sends a MSG_GAME_ACTION to a single client (private message). */
void send_game_action_to_fd(int fd, const char *json, int json_len);

/* Returns the slot index (0-3) for a given user_id in a room, or -1. */
int user_id_to_slot(int room_id, int user_id);

/* Turn timer: start a 60-second countdown for the given player's turn.
 * Broadcasts warnings at 30/10/5 s remaining; on expiry removes one life
 * and advances the turn. Automatically cancels any prior timer for the room. */
void turn_timer_start(int room_id, int match_id, int user_id,
                      client_list_t *live, db_t *db, pthread_mutex_t *db_mutex);

/* Cancel the running turn timer for a room (player acted in time). */
void turn_timer_cancel(int room_id);

#endif /* GAME_ACTIONS_H */
