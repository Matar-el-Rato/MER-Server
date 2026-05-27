/* =============================================================
 * game_actions.c — all in-game action handlers
 * ============================================================= */

#include "game_actions.h"
#include "parchis_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

const char *SLOT_COLORS[MAX_ROOM_PLAYERS] = {"blue", "green", "yellow", "red"};

chair_state_t   g_chair_state[NUM_ROOMS + 1];
pthread_mutex_t g_chair_mutex = PTHREAD_MUTEX_INITIALIZER;

turn_state_t    g_turn_state[NUM_ROOMS + 1];
pthread_mutex_t g_turn_mutex = PTHREAD_MUTEX_INITIALIZER;

game_state_t    g_game_state[NUM_ROOMS + 1];
pthread_mutex_t g_game_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── JSON helpers ──────────────────────────────────────────────────────────── */

static const char *json_str_val(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return NULL;
    return p + 1;
}

static int json_int_val(const char *json, const char *key, int default_val)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return default_val;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9')) return atoi(p);
    return default_val;
}

static void json_str_copy(const char *start, char *dst, int maxlen)
{
    int i = 0;
    while (i < maxlen - 1 && start[i] != '"' && start[i] != '\0') {
        dst[i] = start[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── Public helpers ────────────────────────────────────────────────────────── */

int user_id_to_slot(int room_id, int user_id)
{
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++)
        if (g_chair_state[room_id].slots[i].user_id == user_id) return i;
    return -1;
}

/* How many finished pieces (at goal) does slot have? */
static int count_finished(int room_id, int slot)
{
    int n = 0;
    for (int p = 0; p < 4; p++)
        if (parchis_is_goal(g_game_state[room_id].piece_positions[slot][p])) n++;
    return n;
}

/* Broadcast a piece-level capture: send victim piece home and report it. */
static void do_capture(int room_id, int attacker_user_id, int victim_slot, int victim_piece,
                        int square, client_list_t *live, db_t *db, pthread_mutex_t *db_mutex,
                        int match_id)
{
    game_state_t *gs = &g_game_state[room_id];
    gs->piece_positions[victim_slot][victim_piece] = 0;

    int victim_user_id = g_chair_state[room_id].slots[victim_slot].user_id;

    char json[256];
    int  len = snprintf(json, sizeof(json),
        "{\"action\":\"" ACTION_CAPTURE "\","
        "\"attacker_user_id\":%d,\"victim_user_id\":%d,"
        "\"victim_piece_id\":%d,\"square\":%d,\"bonus_movements\":20}",
        attacker_user_id, victim_user_id, victim_piece, square);
    broadcast_game_action_to_room(live, room_id, json, len);
    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, attacker_user_id, ACTION_CAPTURE, json);
    pthread_mutex_unlock(db_mutex);
}

/* Advance turn to the next player, broadcasting turn_end + turn_start. */
static void advance_turn(int room_id, int current_user_id, client_list_t *live,
                          db_t *db, pthread_mutex_t *db_mutex, int match_id)
{
    pthread_mutex_lock(&g_turn_mutex);
    turn_state_t *ts = &g_turn_state[room_id];
    ts->current_idx  = (ts->current_idx + 1) % ts->player_count;
    int next_uid     = ts->turn_order[ts->current_idx];
    pthread_mutex_unlock(&g_turn_mutex);

    char json[128];
    int  len = snprintf(json, sizeof(json),
        "{\"action\":\"" ACTION_TURN_END "\",\"user_id\":%d,\"next_user_id\":%d}",
        current_user_id, next_uid);
    broadcast_game_action_to_room(live, room_id, json, len);

    /* Reset the next player's consecutive doubles if it's a clean turn. */
    int next_slot = user_id_to_slot(room_id, next_uid);
    if (next_slot >= 0) {
        pthread_mutex_lock(&g_game_mutex);
        /* Check handcuff */
        bool handcuffed = g_game_state[room_id].is_handcuffed[next_slot];
        if (handcuffed) g_game_state[room_id].is_handcuffed[next_slot] = false;
        pthread_mutex_unlock(&g_game_mutex);

        if (handcuffed) {
            len = snprintf(json, sizeof(json),
                "{\"action\":\"" ACTION_HANDCUFF_SKIP "\",\"user_id\":%d}", next_uid);
            broadcast_game_action_to_room(live, room_id, json, len);
            /* Skip to the player after next. */
            advance_turn(room_id, next_uid, live, db, db_mutex, match_id);
            return;
        }
    }

    len = snprintf(json, sizeof(json),
        "{\"action\":\"" ACTION_TURN_START "\",\"user_id\":%d,"
        "\"consecutive_doubles\":%d,\"pending_movements\":0}",
        next_uid,
        next_slot >= 0 ? g_game_state[room_id].consecutive_doubles[next_slot] : 0);
    broadcast_game_action_to_room(live, room_id, json, len);

    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, next_uid, ACTION_TURN_START, json);
    pthread_mutex_unlock(db_mutex);

    turn_timer_start(room_id, match_id, next_uid, live, db, db_mutex);
}

/* ── Turn timer ──────────────────────────────────────────────────────────────
 *
 * One detached pthread per room.  Cancel is implemented by bumping a per-room
 * generation counter (g_timer_gen).  The running thread polls every 100 ms and
 * exits silently if its captured generation no longer matches the current one.
 * This avoids any join / condvar complexity and is safe even when the timer
 * thread itself fires advance_turn() which re-arms the timer for the next player.
 * ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int             gen;
    int             room_id;
    int             match_id;
    int             user_id;
    client_list_t  *live;
    db_t           *db;
    pthread_mutex_t *db_mutex;
} turn_timer_args_t;

static volatile int    g_timer_gen[NUM_ROOMS + 1];  /* bump = cancel           */
static pthread_mutex_t g_timer_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *turn_timer_thread(void *arg)
{
    turn_timer_args_t *a = (turn_timer_args_t *)arg;

    /* Copy everything to locals; free the heap args immediately. */
    int             room_id  = a->room_id;
    int             match_id = a->match_id;
    int             user_id  = a->user_id;
    int             gen      = a->gen;
    client_list_t  *live     = a->live;
    db_t           *db       = a->db;
    pthread_mutex_t *db_mutex = a->db_mutex;
    free(a);

    /* Stages: { sleep_ms, seconds_remaining_to_broadcast }
     * remaining == 0 means this stage ends in expiry. */
    static const struct { int ms; int remaining; } STAGES[] = {
        { 10000, 20 },
        { 10000, 10 },
        {  5000,  5 },
        {  5000,  0 },
    };

    for (int s = 0; s < 4; s++) {
        /* Poll in 100 ms chunks so cancellation is noticed quickly. */
        int chunks = STAGES[s].ms / 100;
        for (int t = 0; t < chunks; t++) {
            struct timespec ts = { 0, 100000000L };
            nanosleep(&ts, NULL);
            if (g_timer_gen[room_id] != gen) return NULL;
        }
        /* One final check after the sleep loop to close the 100ms window
         * between loop exit and the broadcast/expiry action. */
        if (g_timer_gen[room_id] != gen) return NULL;

        if (STAGES[s].remaining > 0) {
            /* Warning broadcast. */
            char json[128];
            int  len = snprintf(json, sizeof(json),
                "{\"action\":\"" ACTION_TURN_TIMER_WARNING "\","
                "\"user_id\":%d,\"seconds_remaining\":%d}",
                user_id, STAGES[s].remaining);
            broadcast_game_action_to_room(live, room_id, json, len);

        } else {
            /* Expiry: claim the generation under the mutex to prevent a
             * simultaneous cancel from also applying a penalty. */
            pthread_mutex_lock(&g_timer_mutex);
            if (g_timer_gen[room_id] != gen) {
                pthread_mutex_unlock(&g_timer_mutex);
                return NULL;
            }
            g_timer_gen[room_id]++;   /* consume this generation */
            pthread_mutex_unlock(&g_timer_mutex);

            /* Guard: skip penalty if the game is no longer active or belongs to a different match. */
            pthread_mutex_lock(&g_game_mutex);
            int active   = g_game_state[room_id].active;
            int cur_mid  = g_game_state[room_id].match_id;
            pthread_mutex_unlock(&g_game_mutex);
            if (!active || cur_mid != match_id) return NULL;

            /* Broadcast expiry. */
            char json[256];
            int  len = snprintf(json, sizeof(json),
                "{\"action\":\"" ACTION_TURN_TIMER_EXPIRED "\",\"user_id\":%d}",
                user_id);
            broadcast_game_action_to_room(live, room_id, json, len);

            /* Deduct one life. */
            int slot = user_id_to_slot(room_id, user_id);
            if (slot >= 0) {
                pthread_mutex_lock(&g_game_mutex);
                g_game_state[room_id].life_charges[slot]--;
                int lives = g_game_state[room_id].life_charges[slot];
                pthread_mutex_unlock(&g_game_mutex);

                len = snprintf(json, sizeof(json),
                    "{\"action\":\"" ACTION_LIFE_LOST "\","
                    "\"user_id\":%d,\"lives_remaining\":%d,\"reason\":\"timeout\"}",
                    user_id, lives);
                broadcast_game_action_to_room(live, room_id, json, len);

                pthread_mutex_lock(db_mutex);
                db_log_event(db, match_id, user_id, ACTION_LIFE_LOST, json);
                pthread_mutex_unlock(db_mutex);
            }

            /* Skip to the next player (re-arms the timer for them). */
            advance_turn(room_id, user_id, live, db, db_mutex, match_id);
        }
    }
    return NULL;
}

void turn_timer_cancel(int room_id)
{
    pthread_mutex_lock(&g_timer_mutex);
    g_timer_gen[room_id]++;
    pthread_mutex_unlock(&g_timer_mutex);
    /* Running thread notices within ≤100 ms and exits silently. */
}

void turn_timer_start(int room_id, int match_id, int user_id,
                      client_list_t *live, db_t *db, pthread_mutex_t *db_mutex)
{
    pthread_mutex_lock(&g_timer_mutex);
    g_timer_gen[room_id]++;           /* cancel any timer already running    */
    int gen = g_timer_gen[room_id];   /* capture generation for new thread   */
    pthread_mutex_unlock(&g_timer_mutex);

    turn_timer_args_t *args = malloc(sizeof(*args));
    if (!args) return;
    args->gen      = gen;
    args->room_id  = room_id;
    args->match_id = match_id;
    args->user_id  = user_id;
    args->live     = live;
    args->db       = db;
    args->db_mutex = db_mutex;

    pthread_t t;
    if (pthread_create(&t, NULL, turn_timer_thread, args) != 0) {
        free(args);
        return;
    }
    pthread_detach(t);
}

/* ── Initiative sequence ───────────────────────────────────────────────────── */

static void handle_initiative_sequence(client_list_t *live, int room_id, int match_id,
                                        db_t *db, pthread_mutex_t *db_mutex)
{
    static const int SHOT_ORDER[] = {2, 0, 3, 1}; /* yellow, blue, red, green */
    static const char *ITEM_NAMES[] = {
        "gun", "cigarette", "magnifying_glass", "handcuffs", "fire_axe"
    };

    static int seeded = 0;
    if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }

    int player_ids[MAX_ROOM_PLAYERS];
    int player_count = 0;

    pthread_mutex_lock(&g_chair_mutex);
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
        int slot = SHOT_ORDER[i];
        if (g_chair_state[room_id].slots[slot].user_id != 0)
            player_ids[player_count++] = g_chair_state[room_id].slots[slot].user_id;
    }
    pthread_mutex_unlock(&g_chair_mutex);

    if (player_count == 0) return;

    int winner_idx = rand() % player_count;

    int turn_order[MAX_ROOM_PLAYERS];
    int turn_count = 0;
    turn_order[turn_count++] = player_ids[winner_idx];
    for (int i = 0; i < player_count; i++)
        if (i != winner_idx) turn_order[turn_count++] = player_ids[i];

    pthread_mutex_lock(&g_turn_mutex);
    memset(&g_turn_state[room_id], 0, sizeof(turn_state_t));
    g_turn_state[room_id].player_count = turn_count;
    g_turn_state[room_id].current_idx  = 0;
    for (int i = 0; i < turn_count; i++)
        g_turn_state[room_id].turn_order[i] = turn_order[i];
    pthread_mutex_unlock(&g_turn_mutex);

    /* Kill any lingering timer from a previous match before touching game state. */
    turn_timer_cancel(room_id);

    /* Initialise game state for this room. */
    pthread_mutex_lock(&g_game_mutex);
    memset(&g_game_state[room_id], 0, sizeof(game_state_t));
    g_game_state[room_id].active       = true;
    g_game_state[room_id].match_id     = match_id;
    g_game_state[room_id].room_id      = room_id;
    g_game_state[room_id].player_count = turn_count;
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++)
        g_game_state[room_id].life_charges[i] = 3;
    parchis_random_golden_squares(g_game_state[room_id].golden_squares);
    pthread_mutex_unlock(&g_game_mutex);

    /* Build initiative_sequence JSON. */
    char json[1024];
    int  pos = 0;
    pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                    "{\"action\":\"" ACTION_INITIATIVE_SEQUENCE "\",\"shots\":[");
    for (int i = 0; i <= winner_idx; i++)
        pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                        "%s{\"user_id\":%d,\"result\":\"%s\"}",
                        i ? "," : "", player_ids[i],
                        i == winner_idx ? "bang" : "click");

    pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                    "],\"winner_user_id\":%d,\"turn_order\":[%d",
                    turn_order[0], turn_order[0]);
    for (int i = 1; i < turn_count; i++)
        pos += snprintf(json + pos, sizeof(json) - (size_t)pos, ",%d", turn_order[i]);

    pos += snprintf(json + pos, sizeof(json) - (size_t)pos, "],\"item_grants\":[");
    pthread_mutex_lock(&g_game_mutex);
    for (int i = 0; i < player_count; i++) {
        int item1 = rand() % NUM_ITEMS;
        int item2 = (item1 + 1 + rand() % (NUM_ITEMS - 1)) % NUM_ITEMS;
        pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                        "%s{\"user_id\":%d,\"items\":[\"%s\"",
                        i ? "," : "", player_ids[i], ITEM_NAMES[item1]);
        if (i == winner_idx)
            pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                            ",\"%s\"", ITEM_NAMES[item2]);
        pos += snprintf(json + pos, sizeof(json) - (size_t)pos, "]}");

        /* Record granted items so golden_square_event won't re-grant them. */
        int islot = user_id_to_slot(room_id, player_ids[i]);
        if (islot >= 0) {
            g_game_state[room_id].has_item[islot][item1] = true;
            if (i == winner_idx)
                g_game_state[room_id].has_item[islot][item2] = true;
        }
    }
    pthread_mutex_unlock(&g_game_mutex);

    /* Include golden squares so clients can mark them on the board. */
    pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                    "],\"golden_squares\":[%d,%d,%d,%d]}",
                    g_game_state[room_id].golden_squares[0],
                    g_game_state[room_id].golden_squares[1],
                    g_game_state[room_id].golden_squares[2],
                    g_game_state[room_id].golden_squares[3]);

    broadcast_game_action_to_room(live, room_id, json, pos);
    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, 0, ACTION_INITIATIVE_SEQUENCE, json);
    pthread_mutex_unlock(db_mutex);

    /* First turn_start for the initiative winner. */
    char turn_json[128];
    int  tlen = snprintf(turn_json, sizeof(turn_json),
                         "{\"action\":\"" ACTION_TURN_START "\",\"user_id\":%d,"
                         "\"consecutive_doubles\":0,\"pending_movements\":0}",
                         turn_order[0]);
    broadcast_game_action_to_room(live, room_id, turn_json, tlen);
}

/* ── Chair selection ───────────────────────────────────────────────────────── */

void chair_state_init(int room_id, int match_id, int player_count)
{
    pthread_mutex_lock(&g_chair_mutex);
    memset(&g_chair_state[room_id], 0, sizeof(chair_state_t));
    g_chair_state[room_id].match_id     = match_id;
    g_chair_state[room_id].room_id      = room_id;
    g_chair_state[room_id].player_count = player_count;
    pthread_mutex_unlock(&g_chair_mutex);
}

int chair_state_remove_user(int room_id, int user_id, char *color_out)
{
    int found = 0;
    pthread_mutex_lock(&g_chair_mutex);
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
        if (g_chair_state[room_id].slots[i].user_id == user_id) {
            strncpy(color_out, SLOT_COLORS[i], MAX_COLOR_LEN - 1);
            color_out[MAX_COLOR_LEN - 1] = '\0';
            memset(&g_chair_state[room_id].slots[i], 0, sizeof(chair_slot_t));
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_chair_mutex);
    return found;
}

static void handle_choose_chair(int fd, int user_id, const char *username,
                                 int match_id, int room_id,
                                 const char *json,
                                 client_list_t *live,
                                 db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd;

    const char *cv = json_str_val(json, "color");
    if (!cv) return;

    char color[MAX_COLOR_LEN];
    json_str_copy(cv, color, sizeof(color));

    int slot = -1;
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
        if (strcmp(SLOT_COLORS[i], color) == 0) { slot = i; break; }
    }
    if (slot == -1) return;

    int skin_id = 101;
    pthread_mutex_lock(&live->mutex);
    for (int i = 0; i < live->count; i++) {
        if (live->entries[i].user_id == user_id) {
            skin_id = live->entries[i].skin_id;
            break;
        }
    }
    pthread_mutex_unlock(&live->mutex);

    char broadcast_json[256];
    int  claimed      = 0;
    int  all_seated   = 0;
    char locked_json[512];

    pthread_mutex_lock(&g_chair_mutex);

    if (g_chair_state[room_id].match_id != match_id) {
        pthread_mutex_unlock(&g_chair_mutex);
        return;
    }
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
        if (g_chair_state[room_id].slots[i].user_id == user_id) {
            pthread_mutex_unlock(&g_chair_mutex);
            return;
        }
    }
    if (g_chair_state[room_id].slots[slot].user_id == 0) {
        g_chair_state[room_id].slots[slot].user_id = user_id;
        g_chair_state[room_id].slots[slot].skin_id = skin_id;
        strncpy(g_chair_state[room_id].slots[slot].username, username, MAX_USERNAME - 1);
        g_chair_state[room_id].slots[slot].username[MAX_USERNAME - 1] = '\0';

        snprintf(broadcast_json, sizeof(broadcast_json),
            "{\"action\":\"chair_taken\",\"color\":\"%s\",\"user_id\":%d,\"username\":\"%s\",\"skin_id\":%d}",
            color, user_id, username, skin_id);
        claimed = 1;

        int filled = 0;
        int player_count = g_chair_state[room_id].player_count;
        for (int i = 0; i < MAX_ROOM_PLAYERS; i++)
            if (g_chair_state[room_id].slots[i].user_id != 0) filled++;

        if (filled >= player_count) {
            int pos = 0;
            pos += snprintf(locked_json + pos, sizeof(locked_json) - (size_t)pos,
                            "{\"action\":\"chairs_locked\",\"assignments\":[");
            int first = 1;
            for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
                if (g_chair_state[room_id].slots[i].user_id == 0) continue;
                pos += snprintf(locked_json + pos, sizeof(locked_json) - (size_t)pos,
                    "%s{\"user_id\":%d,\"username\":\"%s\",\"skin_id\":%d,\"chair\":\"%s\"}",
                    first ? "" : ",",
                    g_chair_state[room_id].slots[i].user_id,
                    g_chair_state[room_id].slots[i].username,
                    g_chair_state[room_id].slots[i].skin_id,
                    SLOT_COLORS[i]);
                first = 0;
            }
            snprintf(locked_json + pos, sizeof(locked_json) - (size_t)pos, "]}");
            all_seated = 1;
        }
    }
    pthread_mutex_unlock(&g_chair_mutex);

    if (claimed) {
        broadcast_game_action_to_room(live, room_id, broadcast_json,
                                      (int)strlen(broadcast_json));
        pthread_mutex_lock(db_mutex);
        db_set_chair(db, match_id, user_id, color);
        db_log_event(db, match_id, user_id, "choose_chair", broadcast_json);
        pthread_mutex_unlock(db_mutex);

        if (all_seated) {
            broadcast_game_action_to_room(live, room_id, locked_json,
                                          (int)strlen(locked_json));
            pthread_mutex_lock(db_mutex);
            db_log_event(db, match_id, 0, "chairs_locked", locked_json);
            pthread_mutex_unlock(db_mutex);
            handle_initiative_sequence(live, room_id, match_id, db, db_mutex);
        }
    }
}

/* ── Roll dice ─────────────────────────────────────────────────────────────── */

static void handle_roll_dice(int fd, int user_id,
                              int match_id, int room_id,
                              const char *json,
                              client_list_t *live,
                              db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd;

    /* Client sends die values from physical dice roll. */
    int die1 = json_int_val(json, "die1", 0);
    int die2 = json_int_val(json, "die2", 0);
    if (die1 < 1 || die1 > 6 || die2 < 1 || die2 > 6) return;

    /* Validate it's this player's turn. */
    pthread_mutex_lock(&g_turn_mutex);
    turn_state_t *ts = &g_turn_state[room_id];
    if (ts->player_count == 0 || ts->turn_order[ts->current_idx] != user_id) {
        pthread_mutex_unlock(&g_turn_mutex);
        return;
    }
    pthread_mutex_unlock(&g_turn_mutex);

    int slot = user_id_to_slot(room_id, user_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_game_mutex);
    game_state_t *gs = &g_game_state[room_id];

    /* Track consecutive doubles. */
    bool is_doubles = (die1 == die2);
    if (is_doubles) gs->consecutive_doubles[slot]++;
    else            gs->consecutive_doubles[slot] = 0;

    int consec = gs->consecutive_doubles[slot];

    /* Triple double: penalise without actually rolling. */
    if (consec >= 3) {
        gs->consecutive_doubles[slot] = 0;
        gs->pending_die1 = 0;
        gs->pending_die2 = 0;

        /* Pick a random active (non-home, non-goal) piece. */
        int victim_piece = -1;
        for (int p = 0; p < 4; p++) {
            int pos = gs->piece_positions[slot][p];
            if (pos != 0 && !parchis_is_goal(pos)) { victim_piece = p; break; }
        }
        pthread_mutex_unlock(&g_game_mutex);

        if (victim_piece >= 0) {
            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].piece_positions[slot][victim_piece] = 0;
            pthread_mutex_unlock(&g_game_mutex);

            char pen_json[128];
            int  plen = snprintf(pen_json, sizeof(pen_json),
                "{\"action\":\"" ACTION_TRIPLE_DOUBLE "\",\"user_id\":%d,\"piece_id\":%d}",
                user_id, victim_piece);
            broadcast_game_action_to_room(live, room_id, pen_json, plen);

            /* Life lost */
            pthread_mutex_lock(&g_game_mutex);
            gs->life_charges[slot]--;
            int lives = gs->life_charges[slot];
            pthread_mutex_unlock(&g_game_mutex);

            snprintf(pen_json, sizeof(pen_json),
                "{\"action\":\"" ACTION_LIFE_LOST "\",\"user_id\":%d,\"lives_remaining\":%d,\"reason\":\"triple_double\"}",
                user_id, lives);
            broadcast_game_action_to_room(live, room_id, pen_json, (int)strlen(pen_json));
        }

        advance_turn(room_id, user_id, live, db, db_mutex, match_id);
        return;
    }

    /* Store dice for handle_move_piece to consume. */
    gs->pending_die1               = die1;
    gs->pending_die2               = die2;
    gs->pending_doubles_reroll[slot] = false; /* clear any stale flag from a previous turn */

    /* Compute moveable pieces. */
    int moveable[4];
    bool can_exit = false;
    int  n_moveable = parchis_moveable_pieces(slot, die1, die2,
                                               gs->piece_positions,
                                               moveable, &can_exit);

    pthread_mutex_unlock(&g_game_mutex);

    /* Build dice_result JSON with moveable_pieces array. */
    char result_json[256];
    int  rpos = 0;
    rpos += snprintf(result_json + rpos, sizeof(result_json) - (size_t)rpos,
                     "{\"action\":\"" ACTION_DICE_RESULT "\","
                     "\"user_id\":%d,\"die1\":%d,\"die2\":%d,\"total\":%d,"
                     "\"is_doubles\":%s,\"consecutive_doubles\":%d,"
                     "\"can_exit_house\":%s,\"moveable_pieces\":[",
                     user_id, die1, die2, die1 + die2,
                     is_doubles ? "true" : "false", consec,
                     can_exit   ? "true" : "false");
    for (int i = 0; i < n_moveable; i++)
        rpos += snprintf(result_json + rpos, sizeof(result_json) - (size_t)rpos,
                         "%s%d", i ? "," : "", moveable[i]);
    rpos += snprintf(result_json + rpos, sizeof(result_json) - (size_t)rpos, "]}");

    broadcast_game_action_to_room(live, room_id, result_json, rpos);

    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, user_id, ACTION_ROLL_DICE, result_json);
    pthread_mutex_unlock(db_mutex);

    /* No moveable pieces → doubles grant a reroll; otherwise auto-pass. */
    if (n_moveable == 0) {
        if (is_doubles) {
            char extra_json[128];
            int  elen = snprintf(extra_json, sizeof(extra_json),
                "{\"action\":\"" ACTION_EXTRA_TURN "\","
                "\"user_id\":%d,\"reason\":\"doubles\",\"pending_movements\":0}",
                user_id);
            broadcast_game_action_to_room(live, room_id, extra_json, elen);

            char ts_json[128];
            int  tslen = snprintf(ts_json, sizeof(ts_json),
                "{\"action\":\"" ACTION_TURN_START "\",\"user_id\":%d,"
                "\"consecutive_doubles\":%d,\"pending_movements\":0}",
                user_id, consec);
            broadcast_game_action_to_room(live, room_id, ts_json, tslen);

            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
        } else {
            advance_turn(room_id, user_id, live, db, db_mutex, match_id);
        }
    }
}

/* ── Move piece ────────────────────────────────────────────────────────────── */

static void handle_move_piece(int fd, int user_id,
                               int match_id, int room_id,
                               const char *json,
                               client_list_t *live,
                               db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd;

    int piece_id = json_int_val(json, "piece_id", -1);
    if (piece_id < 0 || piece_id > 3) return;

    /* Validate it's this player's turn. */
    pthread_mutex_lock(&g_turn_mutex);
    turn_state_t *ts = &g_turn_state[room_id];
    if (ts->player_count == 0 || ts->turn_order[ts->current_idx] != user_id) {
        pthread_mutex_unlock(&g_turn_mutex);
        return;
    }
    pthread_mutex_unlock(&g_turn_mutex);

    /* Player is committing a move — stop the turn clock. */
    turn_timer_cancel(room_id);

    int slot = user_id_to_slot(room_id, user_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_game_mutex);
    game_state_t *gs = &g_game_state[room_id];

    /* Validate dice or bonus pending. */
    bool bonus_move = (gs->pending_die1 == 0 && gs->pending_die2 == 0
                       && gs->pending_movements[slot] > 0);
    bool normal_move = (gs->pending_die1 > 0);
    if (!bonus_move && !normal_move) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    int from_sq      = gs->piece_positions[slot][piece_id];
    int die1         = gs->pending_die1;
    int die2         = gs->pending_die2;
    bool is_doubles  = (die1 == die2 && die1 > 0);
    int to_sq;
    bool is_exit     = false;
    int total_steps  = 0; /* included in piece_moved so the client can animate bounces */
    int exit_remainder  = 0; /* non-5 die kept after a 5+X exit                        */
    int second_move_die = 0; /* 0=none, else=die value for the second move after exit   */

    if (bonus_move) {
        /* Extra move using pending_movements only. */
        int steps = gs->pending_movements[slot];
        total_steps = steps;
        if (from_sq == 0) {
            pthread_mutex_unlock(&g_game_mutex);
            return; /* can't use bonus to exit home */
        }
        to_sq = parchis_advance(slot, from_sq, steps);
        if (to_sq < 0) { pthread_mutex_unlock(&g_game_mutex); return; }
        if (!parchis_path_clear(slot, from_sq, steps, gs->piece_positions)) {
            pthread_mutex_unlock(&g_game_mutex);
            return;
        }
    } else if (from_sq == 0) {
        /* Exit from home — need a 5 die. */
        if (die1 != 5 && die2 != 5 && (die1 + die2) != 5) {
            pthread_mutex_unlock(&g_game_mutex);
            return;
        }
        to_sq       = PARCHIS_EXIT[slot];
        is_exit     = true;
        total_steps = 5;
        /* Compute remaining die for 5+X case (not 5+5, not total-of-5). */
        if      (die1 == 5 && die2 > 0 && die2 != 5) exit_remainder = die2;
        else if (die2 == 5 && die1 > 0 && die1 != 5) exit_remainder = die1;
    } else {
        int steps   = die1 + die2;
        total_steps = steps;
        to_sq = parchis_advance(slot, from_sq, steps);
        if (to_sq < 0) { pthread_mutex_unlock(&g_game_mutex); return; }

        if (!parchis_path_clear(slot, from_sq, steps, gs->piece_positions)) {
            pthread_mutex_unlock(&g_game_mutex);
            return;
        }
    }

    /* Verify landing is allowed.
     * Own spawn square: native color always has priority (will eat enemies).
     * Safe square: 2+ non-mover pieces of any colors form a blocking barrier.
     * Non-safe square: same-color enemy barrier blocks landing. */
    bool blocked = false;
    if (to_sq != PARCHIS_EXIT[slot]) {
        if (parchis_is_safe(to_sq, slot)) {
            int occ = 0;
            for (int s = 0; s < MAX_ROOM_PLAYERS; s++) {
                if (s == slot) continue;
                for (int p = 0; p < 4; p++)
                    if (gs->piece_positions[s][p] == to_sq) occ++;
            }
            blocked = (occ >= 2);
        } else {
            for (int s = 0; s < MAX_ROOM_PLAYERS; s++) {
                if (s == slot) continue;
                if (parchis_is_barrier(to_sq, s, gs->piece_positions)) { blocked = true; break; }
            }
        }
    }
    if (blocked) { pthread_mutex_unlock(&g_game_mutex); return; }

    /* Apply the move. */
    gs->piece_positions[slot][piece_id] = to_sq;

    /* Consume dice / bonus.
     * 5+5: if exit taken, keep second 5 for a second move and owe a reroll after.
     *       if no exit, is_doubles stays true → grants re-roll like regular doubles.
     * 5+X: if exit taken with the 5, keep X for a second move (no reroll). */
    if (bonus_move) {
        gs->pending_movements[slot] = 0;
    } else if (die1 == 5 && die2 == 5) {
        if (is_exit) {
            /* Keep the second 5; a reroll is owed after both 5s are consumed. */
            gs->pending_die1 = 5;
            gs->pending_die2 = 0;
            gs->pending_five_reroll[slot] = true;
            second_move_die = 5;
            is_doubles = false;
        } else {
            gs->pending_die1 = 0;
            gs->pending_die2 = 0;
            /* is_doubles stays true → non-exit 5+5 grants a re-roll like regular doubles */
        }
    } else if (is_exit && exit_remainder > 0) {
        /* 5+X exit: keep the non-5 die for a second move (no reroll). */
        gs->pending_die1 = exit_remainder;
        gs->pending_die2 = 0;
        second_move_die = exit_remainder;
        is_doubles = false;
    } else {
        gs->pending_die1 = 0;
        gs->pending_die2 = 0;
    }

    bool on_safe = parchis_is_safe(to_sq, slot);

    /* Check barrier formed/broken before capture check. */
    int pieces_at_to = 0;
    for (int p = 0; p < 4; p++)
        if (gs->piece_positions[slot][p] == to_sq) pieces_at_to++;
    int pieces_at_from = 0;
    for (int p = 0; p < 4; p++)
        if (gs->piece_positions[slot][p] == from_sq) pieces_at_from++;

    bool barrier_formed = (pieces_at_to >= 2);
    bool barrier_broken = (from_sq > 0 && pieces_at_from == 1);  /* was 2, now 1 */

    pthread_mutex_unlock(&g_game_mutex);

    /* Broadcast piece_moved. */
    char moved_json[256];
    int  mlen = snprintf(moved_json, sizeof(moved_json),
        "{\"action\":\"" ACTION_PIECE_MOVED "\","
        "\"user_id\":%d,\"piece_id\":%d,\"from\":%d,\"to\":%d,"
        "\"steps\":%d,\"is_exit\":%s,\"on_safe_square\":%s}",
        user_id, piece_id, from_sq, to_sq, total_steps,
        is_exit   ? "true" : "false",
        on_safe   ? "true" : "false");
    broadcast_game_action_to_room(live, room_id, moved_json, mlen);
    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, user_id, ACTION_MOVE_PIECE, moved_json);
    pthread_mutex_unlock(db_mutex);

    /* Barrier formed? */
    if (barrier_formed && to_sq > 0 && !parchis_is_goal(to_sq)) {
        int ids[2]; int n = 0;
        for (int p = 0; p < 4 && n < 2; p++)
            if (gs->piece_positions[slot][p] == to_sq) ids[n++] = p;
        char bar_json[128];
        int  blen = snprintf(bar_json, sizeof(bar_json),
            "{\"action\":\"" ACTION_BARRIER_FORMED "\","
            "\"user_id\":%d,\"square\":%d,\"piece_ids\":[%d,%d]}",
            user_id, to_sq, ids[0], n > 1 ? ids[1] : ids[0]);
        broadcast_game_action_to_room(live, room_id, bar_json, blen);
    }

    /* Barrier broken at from_sq? */
    if (barrier_broken && from_sq > 0 && !parchis_is_goal(from_sq)) {
        char bar_json[128];
        int  blen = snprintf(bar_json, sizeof(bar_json),
            "{\"action\":\"" ACTION_BARRIER_BROKEN "\","
            "\"user_id\":%d,\"square\":%d,\"reason\":\"moved\"}",
            user_id, from_sq);
        broadcast_game_action_to_room(live, room_id, bar_json, blen);
    }

    /* Captures — land on non-safe square, OR on own spawn square (priority eats enemies). */
    bool spawn_priority = (to_sq == PARCHIS_EXIT[slot]);
    int  captured_count = 0;
    if ((!on_safe || spawn_priority) && !parchis_is_goal(to_sq)) {
        for (int s = 0; s < MAX_ROOM_PLAYERS; s++) {
            if (s == slot) continue;
            for (int p = 0; p < 4; p++) {
                pthread_mutex_lock(&g_game_mutex);
                bool at_sq = (g_game_state[room_id].piece_positions[s][p] == to_sq);
                pthread_mutex_unlock(&g_game_mutex);
                if (!at_sq) continue;

                do_capture(room_id, user_id, s, p, to_sq, live, db, db_mutex, match_id);
                captured_count++;

                pthread_mutex_lock(&g_game_mutex);
                g_game_state[room_id].pending_movements[slot] += 20;
                pthread_mutex_unlock(&g_game_mutex);
            }
        }
    }

    /* Goal scored? */
    bool scored_goal = parchis_is_goal(to_sq);
    if (scored_goal) {
        pthread_mutex_lock(&g_game_mutex);
        int finished = count_finished(room_id, slot);
        g_game_state[room_id].pending_movements[slot] += 10;
        pthread_mutex_unlock(&g_game_mutex);

        char goal_json[128];
        int  glen = snprintf(goal_json, sizeof(goal_json),
            "{\"action\":\"" ACTION_GOAL_SCORED "\","
            "\"user_id\":%d,\"piece_id\":%d,\"pieces_in_goal\":%d,\"bonus_movements\":10}",
            user_id, piece_id, finished);
        broadcast_game_action_to_room(live, room_id, goal_json, glen);
        pthread_mutex_lock(db_mutex);
        db_log_event(db, match_id, user_id, ACTION_GOAL_SCORED, goal_json);
        pthread_mutex_unlock(db_mutex);

        /* Win condition: all 4 pieces in goal. */
        if (finished >= 4) {
            char win_json[128];
            int  wlen = snprintf(win_json, sizeof(win_json),
                "{\"action\":\"" ACTION_GAME_OVER "\","
                "\"winner_user_id\":%d,\"reason\":\"race\"}", user_id);
            broadcast_game_action_to_room(live, room_id, win_json, wlen);

            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].active = false;
            pthread_mutex_unlock(&g_game_mutex);
            turn_timer_cancel(room_id);
            return;
        }
    }

    /* Golden square? */
    pthread_mutex_lock(&g_game_mutex);
    bool is_golden = false;
    for (int q = 0; q < 4; q++)
        if (g_game_state[room_id].golden_squares[q] == to_sq) { is_golden = true; break; }
    pthread_mutex_unlock(&g_game_mutex);

    if (is_golden && !parchis_is_goal(to_sq)) {
        static const char *ITEM_NAMES[] = {
            "gun", "cigarette", "magnifying_glass", "handcuffs", "fire_axe"
        };
        /* Snapshot which items this player already owns. */
        bool owned[NUM_ITEMS];
        pthread_mutex_lock(&g_game_mutex);
        for (int k = 0; k < NUM_ITEMS; k++)
            owned[k] = g_game_state[room_id].has_item[slot][k];
        pthread_mutex_unlock(&g_game_mutex);

        /* 6-sided spin: faces 0-4 = items, face 5 = reroll.
           Also reroll if the item is already owned. Cap at 20 spins. */
        int spins[20];
        int spin_count = 0;
        int face;
        do {
            face = rand() % 6;
            spins[spin_count++] = face;
        } while ((face == 5 || (face < NUM_ITEMS && owned[face])) && spin_count < 20);

        /* Safety cap: all owned or only got rerolls — find first unowned item. */
        if (face == 5 || (face < NUM_ITEMS && owned[face])) {
            face = 0;
            for (int k = 0; k < NUM_ITEMS; k++)
                if (!owned[k]) { face = k; break; }
        }

        /* Record the grant. */
        pthread_mutex_lock(&g_game_mutex);
        g_game_state[room_id].has_item[slot][face] = true;
        pthread_mutex_unlock(&g_game_mutex);

        /* Build spins JSON array. "magnifying_glass"×20 + wrapper ≈ 520 bytes max. */
        char gold_json[640];
        int  gpos = 0;
        gpos += snprintf(gold_json + gpos, sizeof(gold_json) - (size_t)gpos,
            "{\"action\":\"" ACTION_GOLDEN_SQUARE "\","
            "\"user_id\":%d,\"square\":%d,\"spins\":[",
            user_id, to_sq);
        for (int i = 0; i < spin_count; i++) {
            const char *spin_name = (spins[i] == 5) ? "reroll" : ITEM_NAMES[spins[i]];
            gpos += snprintf(gold_json + gpos, sizeof(gold_json) - (size_t)gpos,
                "%s\"%s\"", i ? "," : "", spin_name);
        }
        gpos += snprintf(gold_json + gpos, sizeof(gold_json) - (size_t)gpos,
            "],\"final_item\":\"%s\"}", ITEM_NAMES[face]);

        broadcast_game_action_to_room(live, room_id, gold_json, gpos);
        pthread_mutex_lock(db_mutex);
        db_log_event(db, match_id, user_id, ACTION_GOLDEN_SQUARE, gold_json);
        pthread_mutex_unlock(db_mutex);
    }

    /* Determine turn flow. */
    pthread_mutex_lock(&g_game_mutex);
    int pending_mvs = g_game_state[room_id].pending_movements[slot];
    pthread_mutex_unlock(&g_game_mutex);

    if (pending_mvs > 0) {
        /* Extra move from capture/goal bonus.
         * If this was a doubles move, remember the reroll — it fires after the bonus is used. */
        if (is_doubles) {
            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].pending_doubles_reroll[slot] = true;
            pthread_mutex_unlock(&g_game_mutex);
        }
        const char *reason = (captured_count > 0 && !scored_goal) ? "capture_bonus"
                           : (scored_goal && captured_count == 0)  ? "goal_bonus"
                           : "capture_bonus";
        char extra_json[128];
        int  elen = snprintf(extra_json, sizeof(extra_json),
            "{\"action\":\"" ACTION_EXTRA_TURN "\","
            "\"user_id\":%d,\"reason\":\"%s\",\"pending_movements\":%d}",
            user_id, reason, pending_mvs);
        broadcast_game_action_to_room(live, room_id, extra_json, elen);
    } else if (second_move_die > 0) {
        /* Second move after exit (5+5 or 5+X): offer remaining die to the mover. */
        pthread_mutex_lock(&g_game_mutex);
        int  moveable2[4];
        bool can_exit2;
        int  n2 = parchis_moveable_pieces(slot, second_move_die, 0,
                                          g_game_state[room_id].piece_positions,
                                          moveable2, &can_exit2);
        pthread_mutex_unlock(&g_game_mutex);

        if (n2 == 0) {
            /* Nothing to move — clear pending die, check for owed reroll. */
            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].pending_die1 = 0;
            bool five_reroll = g_game_state[room_id].pending_five_reroll[slot];
            if (five_reroll) g_game_state[room_id].pending_five_reroll[slot] = false;
            pthread_mutex_unlock(&g_game_mutex);
            if (five_reroll) {
                char extra_json[128];
                int  elen = snprintf(extra_json, sizeof(extra_json),
                    "{\"action\":\"" ACTION_EXTRA_TURN "\","
                    "\"user_id\":%d,\"reason\":\"doubles\",\"pending_movements\":0}",
                    user_id);
                broadcast_game_action_to_room(live, room_id, extra_json, elen);
                char ts_json[128];
                int  tslen = snprintf(ts_json, sizeof(ts_json),
                    "{\"action\":\"" ACTION_TURN_START "\",\"user_id\":%d,"
                    "\"consecutive_doubles\":%d,\"pending_movements\":0}",
                    user_id, g_game_state[room_id].consecutive_doubles[slot]);
                broadcast_game_action_to_room(live, room_id, ts_json, tslen);
                turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            } else {
                advance_turn(room_id, user_id, live, db, db_mutex, match_id);
            }
        } else {
            char dice2_json[256];
            int  d2len = 0;
            d2len += snprintf(dice2_json + d2len, sizeof(dice2_json) - (size_t)d2len,
                "{\"action\":\"" ACTION_DICE_RESULT "\","
                "\"user_id\":%d,\"die1\":%d,\"die2\":0,\"total\":%d,"
                "\"is_doubles\":false,\"consecutive_doubles\":0,"
                "\"can_exit_house\":%s,\"moveable_pieces\":[",
                user_id, second_move_die, second_move_die,
                can_exit2 ? "true" : "false");
            for (int i = 0; i < n2; i++)
                d2len += snprintf(dice2_json + d2len, sizeof(dice2_json) - (size_t)d2len,
                    "%s%d", i ? "," : "", moveable2[i]);
            d2len += snprintf(dice2_json + d2len, sizeof(dice2_json) - (size_t)d2len, "]}");
            send_game_action_to_fd(fd, dice2_json, d2len);
        }
    } else if (is_doubles) {
        /* Regular doubles (never 5+5): player rolls again. */
        char extra_json[128];
        int  elen = snprintf(extra_json, sizeof(extra_json),
            "{\"action\":\"" ACTION_EXTRA_TURN "\","
            "\"user_id\":%d,\"reason\":\"doubles\",\"pending_movements\":0}",
            user_id);
        broadcast_game_action_to_room(live, room_id, extra_json, elen);

        /* Re-send turn_start so client re-enables roll. */
        char ts_json[128];
        int  tslen = snprintf(ts_json, sizeof(ts_json),
            "{\"action\":\"" ACTION_TURN_START "\",\"user_id\":%d,"
            "\"consecutive_doubles\":%d,\"pending_movements\":0}",
            user_id, g_game_state[room_id].consecutive_doubles[slot]);
        broadcast_game_action_to_room(live, room_id, ts_json, tslen);

        /* Fresh 60 s for the same player's bonus roll. */
        turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
    } else {
        /* Check for deferred doubles reroll (past bonus) or 5+5-exit reroll. */
        pthread_mutex_lock(&g_game_mutex);
        bool deferred    = g_game_state[room_id].pending_doubles_reroll[slot];
        if (deferred)    g_game_state[room_id].pending_doubles_reroll[slot] = false;
        bool five_reroll = g_game_state[room_id].pending_five_reroll[slot];
        if (five_reroll) g_game_state[room_id].pending_five_reroll[slot] = false;
        int deferred_consec = g_game_state[room_id].consecutive_doubles[slot];
        pthread_mutex_unlock(&g_game_mutex);

        if (deferred || five_reroll) {
            char extra_json[128];
            int  elen = snprintf(extra_json, sizeof(extra_json),
                "{\"action\":\"" ACTION_EXTRA_TURN "\","
                "\"user_id\":%d,\"reason\":\"doubles\",\"pending_movements\":0}",
                user_id);
            broadcast_game_action_to_room(live, room_id, extra_json, elen);

            char ts_json[128];
            int  tslen = snprintf(ts_json, sizeof(ts_json),
                "{\"action\":\"" ACTION_TURN_START "\",\"user_id\":%d,"
                "\"consecutive_doubles\":%d,\"pending_movements\":0}",
                user_id, deferred_consec);
            broadcast_game_action_to_room(live, room_id, ts_json, tslen);

            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
        } else {
            advance_turn(room_id, user_id, live, db, db_mutex, match_id);
        }
    }
}

/* ── Item stubs ────────────────────────────────────────────────────────────── */

static void handle_use_gun(int fd, int user_id, int match_id, int room_id,
                            const char *json, client_list_t *live,
                            db_t *db, pthread_mutex_t *db_mutex)
{ (void)fd;(void)user_id;(void)match_id;(void)room_id;(void)json;(void)live;(void)db;(void)db_mutex; }

static void handle_use_cigarette(int fd, int user_id, int match_id, int room_id,
                                  const char *json, client_list_t *live,
                                  db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd; (void)json; (void)match_id; (void)db; (void)db_mutex;

    /* Must be this player's turn. */
    pthread_mutex_lock(&g_turn_mutex);
    turn_state_t *ts = &g_turn_state[room_id];
    if (ts->player_count == 0 || ts->turn_order[ts->current_idx] != user_id) {
        pthread_mutex_unlock(&g_turn_mutex);
        return;
    }
    pthread_mutex_unlock(&g_turn_mutex);

    int slot = user_id_to_slot(room_id, user_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_game_mutex);
    game_state_t *gs = &g_game_state[room_id];

    /* Must own the cigarette and dice must be pending (roll already happened). */
    if (!gs->has_item[slot][ITEM_CIGARETTE] ||
        (gs->pending_die1 == 0 && gs->pending_die2 == 0)) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    /* Generate new dice, replace the pending values. */
    int die1 = (rand() % 6) + 1;
    int die2 = (rand() % 6) + 1;
    gs->pending_die1 = die1;
    gs->pending_die2 = die2;

    /* Consume the item. */
    gs->has_item[slot][ITEM_CIGARETTE] = false;

    /* Compute moveable pieces with the new dice. */
    int  moveable[4];
    bool can_exit  = false;
    int  n_moveable = parchis_moveable_pieces(slot, die1, die2,
                                               gs->piece_positions,
                                               moveable, &can_exit);
    pthread_mutex_unlock(&g_game_mutex);

    /* Broadcast cigarette_result to all room clients. */
    char out[256];
    int  rpos = snprintf(out, sizeof(out),
        "{\"action\":\"" ACTION_CIGARETTE_RESULT "\","
        "\"user_id\":%d,\"die1\":%d,\"die2\":%d,\"total\":%d,"
        "\"moveable_pieces\":[",
        user_id, die1, die2, die1 + die2);
    for (int i = 0; i < n_moveable; i++)
        rpos += snprintf(out + rpos, sizeof(out) - (size_t)rpos,
                         "%s%d", i ? "," : "", moveable[i]);
    rpos += snprintf(out + rpos, sizeof(out) - (size_t)rpos, "]}");

    broadcast_game_action_to_room(live, room_id, out, rpos);
}

static void handle_use_magnifying_glass(int fd, int user_id, int match_id, int room_id,
                                         const char *json, client_list_t *live,
                                         db_t *db, pthread_mutex_t *db_mutex)
{ (void)fd;(void)user_id;(void)match_id;(void)room_id;(void)json;(void)live;(void)db;(void)db_mutex; }

static void handle_use_handcuffs(int fd, int user_id, int match_id, int room_id,
                                  const char *json, client_list_t *live,
                                  db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd; (void)match_id; (void)db; (void)db_mutex;

    int target_user_id = json_int_val(json, "target_user_id", -1);
    if (target_user_id < 0) return;

    int slot        = user_id_to_slot(room_id, user_id);
    int target_slot = user_id_to_slot(room_id, target_user_id);
    if (slot < 0 || target_slot < 0 || slot == target_slot) return;

    pthread_mutex_lock(&g_game_mutex);
    game_state_t *gs = &g_game_state[room_id];

    /* Validate: attacker must own the item; target must not already be cuffed. */
    if (!gs->has_item[slot][ITEM_HANDCUFFS] || gs->is_handcuffed[target_slot]) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    /* Consume item and mark target as handcuffed. */
    gs->has_item[slot][ITEM_HANDCUFFS] = false;
    gs->is_handcuffed[target_slot]     = true;
    pthread_mutex_unlock(&g_game_mutex);

    /* Broadcast to all room clients. */
    char out[128];
    int  len = snprintf(out, sizeof(out),
        "{\"action\":\"" ACTION_HANDCUFFS_APPLIED "\","
        "\"attacker_user_id\":%d,\"target_user_id\":%d}",
        user_id, target_user_id);
    broadcast_game_action_to_room(live, room_id, out, len);
}

static void handle_use_fire_axe(int fd, int user_id, int match_id, int room_id,
                                 const char *json, client_list_t *live,
                                 db_t *db, pthread_mutex_t *db_mutex)
{ (void)fd;(void)user_id;(void)match_id;(void)room_id;(void)json;(void)live;(void)db;(void)db_mutex; }

/* ── Dispatch ──────────────────────────────────────────────────────────────── */

void handle_game_action(int fd, int user_id, const char *username,
                        int match_id, int room_id,
                        const char *json, int json_len,
                        client_list_t *live,
                        db_t *db, pthread_mutex_t *db_mutex)
{
    (void)json_len;

    const char *av = json_str_val(json, "action");
    if (!av) return;

    char action[32];
    json_str_copy(av, action, sizeof(action));

    if      (strcmp(action, ACTION_CHOOSE_CHAIR)         == 0)
        handle_choose_chair(fd, user_id, username, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_ROLL_DICE)            == 0)
        handle_roll_dice(fd, user_id, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_MOVE_PIECE)           == 0)
        handle_move_piece(fd, user_id, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_USE_GUN)              == 0)
        handle_use_gun(fd, user_id, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_USE_CIGARETTE)        == 0)
        handle_use_cigarette(fd, user_id, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_USE_MAGNIFYING_GLASS) == 0)
        handle_use_magnifying_glass(fd, user_id, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_USE_HANDCUFFS)        == 0)
        handle_use_handcuffs(fd, user_id, match_id, room_id, json, live, db, db_mutex);
    else if (strcmp(action, ACTION_USE_FIRE_AXE)         == 0)
        handle_use_fire_axe(fd, user_id, match_id, room_id, json, live, db, db_mutex);
}

/* ── Transport ─────────────────────────────────────────────────────────────── */

void broadcast_game_action_to_room(client_list_t *live, int room_id,
                                   const char *json, int json_len)
{
    uint8_t  header[3];
    uint16_t be_len = htons((uint16_t)json_len);
    header[0] = MSG_GAME_ACTION;
    memcpy(&header[1], &be_len, 2);

    pthread_mutex_lock(&live->mutex);
    for (int i = 0; i < live->count; i++) {
        if (live->entries[i].room_id == room_id) {
            send(live->entries[i].socket_fd, header, sizeof(header), MSG_NOSIGNAL);
            send(live->entries[i].socket_fd, json,  (size_t)json_len, MSG_NOSIGNAL);
        }
    }
    pthread_mutex_unlock(&live->mutex);
}

void send_game_action_to_fd(int fd, const char *json, int json_len)
{
    uint8_t  header[3];
    uint16_t be_len = htons((uint16_t)json_len);
    header[0] = MSG_GAME_ACTION;
    memcpy(&header[1], &be_len, 2);
    send(fd, header, sizeof(header), MSG_NOSIGNAL);
    send(fd, json,   (size_t)json_len, MSG_NOSIGNAL);
}
