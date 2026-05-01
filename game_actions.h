#ifndef GAME_ACTIONS_H
#define GAME_ACTIONS_H

#include "protocol.h"
#include "db.h"
#include <pthread.h>

#define MAX_JSON_PAYLOAD 512
#define MAX_COLOR_LEN    8   /* "yellow\0" */

/* Slot index → color name (matches client SlotColorNames, lowercased). */
extern const char *SLOT_COLORS[MAX_ROOM_PLAYERS]; /* {"blue","green","yellow","red"} */

/* One chair slot entry.  user_id == 0 → slot is free. */
typedef struct {
    int  user_id;
    char username[MAX_USERNAME];
} chair_slot_t;

/* Chair-selection state for one room's active match. */
typedef struct {
    chair_slot_t slots[MAX_ROOM_PLAYERS]; /* slots[0..3] */
    int          match_id;
    int          room_id;
} chair_state_t;

/* Per-room state; index 0 unused, 1-NUM_ROOMS active.
 * Protected by g_chair_mutex. */
extern chair_state_t   g_chair_state[NUM_ROOMS + 1];
extern pthread_mutex_t g_chair_mutex;

/* Zero-initialises the chair state for a room at match start. */
void chair_state_init(int room_id, int match_id);

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
