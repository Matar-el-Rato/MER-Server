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

/* ── Test config (config.txt) ────────────────────────────────────────────────
 *
 * Supported keys (one per line, comments start with #):
 *
 *   golden_force=gun,handcuffs,fire_axe,...
 *       Forces the golden-square spin result in order, cycling when exhausted.
 *       Valid item names: gun  cigarette  magnifying_glass  handcuffs  fire_axe
 *
 * Example config.txt:
 *   golden_force=fire_axe,gun,handcuffs
 *
 * Config is (re)loaded at the start of every initiative_sequence so you can
 * edit it between games without restarting the server.
 * ─────────────────────────────────────────────────────────────────────────── */

#define MAX_GOLDEN_FORCE 64

static int g_golden_force[MAX_GOLDEN_FORCE];
static int g_golden_force_count = 0;
static int g_golden_force_idx   = 0;

static int item_name_to_idx(const char *name)
{
    if (strcmp(name, "gun")              == 0) return ITEM_GUN;
    if (strcmp(name, "cigarette")        == 0) return ITEM_CIGARETTE;
    if (strcmp(name, "magnifying_glass") == 0) return ITEM_MAGNIFYING_GLASS;
    if (strcmp(name, "handcuffs")        == 0) return ITEM_HANDCUFFS;
    if (strcmp(name, "fire_axe")         == 0) return ITEM_FIRE_AXE;
    return -1;
}

void config_load(const char *path)
{
    g_golden_force_count = 0;
    g_golden_force_idx   = 0;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        const char *key = "golden_force=";
        size_t klen = strlen(key);
        if (strncmp(line, key, klen) != 0) continue;

        char *p = line + klen;
        char *tok = strtok(p, ", \t\r\n");
        while (tok && g_golden_force_count < MAX_GOLDEN_FORCE) {
            int idx = item_name_to_idx(tok);
            if (idx >= 0)
                g_golden_force[g_golden_force_count++] = idx;
            else
                fprintf(stderr, "[config] unknown item '%s' — skipped\n", tok);
            tok = strtok(NULL, ", \t\r\n");
        }
        break;
    }
    fclose(f);

    if (g_golden_force_count > 0) {
        fprintf(stdout, "[config] golden_force loaded (%d items):", g_golden_force_count);
        static const char *NAMES[] = { "gun","cigarette","magnifying_glass","handcuffs","fire_axe" };
        for (int i = 0; i < g_golden_force_count; i++)
            fprintf(stdout, " %s", NAMES[g_golden_force[i]]);
        fprintf(stdout, "\n");
    }
}

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

/* Scoring on a completed match: winner +100, every other participant -50
 * (db_add_points floors at 0). Only call on a real finish — cancelled/abandoned
 * matches must not score. Reads the original roster from chair state, which keeps
 * every player's user_id even after disconnects. */
#define POINTS_WIN  100
#define POINTS_LOSS (-50)
static void award_match_points(int room_id, int winner_user_id,
                               db_t *db, pthread_mutex_t *db_mutex)
{
    int uids[MAX_ROOM_PLAYERS];
    int n = 0;
    pthread_mutex_lock(&g_chair_mutex);
    /* Chair slots are colour-indexed (0=blue,1=green,2=yellow,3=red) and SPARSE —
     * a 2-player match may sit in slots 0 and 3 — so scan every slot, not just
     * [0..player_count). Using player_count as the bound silently misses players
     * seated in the higher colour slots (the cause of points never changing). */
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
        int u = g_chair_state[room_id].slots[i].user_id;
        if (u > 0) uids[n++] = u;
    }
    pthread_mutex_unlock(&g_chair_mutex);

    pthread_mutex_lock(db_mutex);
    for (int i = 0; i < n; i++)
        db_add_points(db, uids[i], uids[i] == winner_user_id ? POINTS_WIN : POINTS_LOSS);
    pthread_mutex_unlock(db_mutex);
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
    if (ts->player_count == 0) { pthread_mutex_unlock(&g_turn_mutex); return; }
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

static bool do_eliminate_player(int room_id, int elim_user_id, int elim_slot,
                                 client_list_t *live, db_t *db,
                                 pthread_mutex_t *db_mutex, int match_id);

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

                if (lives <= 0) {
                    if (do_eliminate_player(room_id, user_id, slot,
                                            live, db, db_mutex, match_id))
                        return NULL; /* game over — no turn to advance */
                }
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
    static const int SHOT_ORDER[] = {0, 2, 1, 3}; /* blue, yellow, green, red (clockwise) */
    static const char *ITEM_NAMES[] = {
        "gun", "cigarette", "magnifying_glass", "handcuffs", "fire_axe"
    };

    static int seeded = 0;
    if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }

    /* Reload config every game so edits to config.txt take effect without restart. */
    config_load("config.txt");

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
        int item1, item2;
        if (g_golden_force_count > 0) {
            item1 = g_golden_force[g_golden_force_idx % g_golden_force_count];
            g_golden_force_idx++;
            item2 = g_golden_force[g_golden_force_idx % g_golden_force_count];
            g_golden_force_idx++;
        } else {
            item1 = rand() % NUM_ITEMS;
            item2 = (item1 + 1 + rand() % (NUM_ITEMS - 1)) % NUM_ITEMS;
        }
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

/* Returns the slot index for a given color string, or -1. */
static int slot_for_color(const char *color)
{
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++)
        if (strcmp(SLOT_COLORS[i], color) == 0) return i;
    return -1;
}

void handle_player_disconnected(int room_id, int disc_user_id, const char *color,
                                 client_list_t *live, db_t *db,
                                 pthread_mutex_t *db_mutex, int match_id)
{
    /* Verify the match is still the one we think it is. */
    pthread_mutex_lock(&g_game_mutex);
    int active  = g_game_state[room_id].active;
    int cur_mid = g_game_state[room_id].match_id;
    pthread_mutex_unlock(&g_game_mutex);
    if (!active || cur_mid != match_id) return;

    /* 1. Clear the disconnected player's pieces from board state. */
    int slot = slot_for_color(color);
    if (slot >= 0) {
        pthread_mutex_lock(&g_game_mutex);
        memset(g_game_state[room_id].piece_positions[slot], 0,
               sizeof(g_game_state[room_id].piece_positions[slot]));
        /* Also resolve any pending gun decision they were in the middle of. */
        g_game_state[room_id].pending_gun_choice[slot] = false;
        pthread_mutex_unlock(&g_game_mutex);
    }

    /* 2. Remove the player from the turn rotation and adjust current_idx.
     *
     * After the shift:
     *   disc_idx < current_idx  → current_idx-- (element before current removed)
     *   disc_idx == current_idx → set current_idx so advance_turn()'s +1
     *                             lands on the old next player
     *   disc_idx > current_idx  → no change needed
     */
    int was_current = 0;

    pthread_mutex_lock(&g_turn_mutex);
    turn_state_t *ts = &g_turn_state[room_id];

    int disc_idx = -1;
    for (int i = 0; i < ts->player_count; i++) {
        if (ts->turn_order[i] == disc_user_id) { disc_idx = i; break; }
    }

    if (disc_idx >= 0 && ts->player_count > 1) {
        was_current = (disc_idx == ts->current_idx);

        /* Shift remaining entries left. */
        for (int i = disc_idx; i < ts->player_count - 1; i++)
            ts->turn_order[i] = ts->turn_order[i + 1];
        ts->player_count--;

        if (was_current) {
            /* advance_turn() will do (current_idx + 1) % player_count.
             * We want it to land on disc_idx % new_player_count, so subtract 1. */
            ts->current_idx = (disc_idx - 1 + ts->player_count) % ts->player_count;
        } else if (disc_idx < ts->current_idx) {
            ts->current_idx--;
        }
    } else if (disc_idx >= 0) {
        /* Last player in the rotation — just clear, no turn to advance. */
        ts->player_count = 0;
    }

    pthread_mutex_unlock(&g_turn_mutex);

    /* 3. If it was their turn, cancel the running timer and hand off. */
    if (was_current) {
        turn_timer_cancel(room_id);
        advance_turn(room_id, disc_user_id, live, db, db_mutex, match_id);
    }

    char log[128];
    snprintf(log, sizeof(log),
             "[game] room %d: player %d (%s) disconnected — pieces cleared, "
             "turn rotation patched (was_current=%d)\n",
             room_id, disc_user_id, color, was_current);
    tlog(log);
}

/* Broadcast player_eliminated, mark the slot eliminated, clear pieces, and
 * remove the player from the turn rotation.
 * Returns true if the game is now over (last survivor found → game_over
 * broadcast sent, game marked inactive).  Caller must skip advance_turn. */
static bool do_eliminate_player(int room_id, int elim_user_id, int elim_slot,
                                 client_list_t *live, db_t *db,
                                 pthread_mutex_t *db_mutex, int match_id)
{
    char json[128];
    int  len = snprintf(json, sizeof(json),
        "{\"action\":\"" ACTION_PLAYER_ELIMINATED "\",\"user_id\":%d}", elim_user_id);
    broadcast_game_action_to_room(live, room_id, json, len);
    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, elim_user_id, ACTION_PLAYER_ELIMINATED, json);
    pthread_mutex_unlock(db_mutex);

    pthread_mutex_lock(&g_game_mutex);
    g_game_state[room_id].is_eliminated[elim_slot] = true;
    memset(g_game_state[room_id].piece_positions[elim_slot], 0,
           sizeof(g_game_state[room_id].piece_positions[elim_slot]));
    pthread_mutex_unlock(&g_game_mutex);

    pthread_mutex_lock(&g_turn_mutex);
    turn_state_t *ts = &g_turn_state[room_id];
    /* Elimination order IS the ranking: the number of players still in the match
     * (including the one being eliminated now) is this player's finish position.
     * First eliminated of N → N; the last survivor → 1 (set below). */
    int elim_position = ts->player_count;
    int disc_idx = -1;
    for (int i = 0; i < ts->player_count; i++)
        if (ts->turn_order[i] == elim_user_id) { disc_idx = i; break; }
    if (disc_idx >= 0 && ts->player_count > 1) {
        int was_current = (disc_idx == ts->current_idx);
        for (int i = disc_idx; i < ts->player_count - 1; i++)
            ts->turn_order[i] = ts->turn_order[i + 1];
        ts->player_count--;
        if (was_current)
            ts->current_idx = (disc_idx - 1 + ts->player_count) % ts->player_count;
        else if (disc_idx < ts->current_idx)
            ts->current_idx--;
    } else if (disc_idx >= 0) {
        ts->player_count = 0;
    }
    int survivor = (ts->player_count == 1) ? ts->turn_order[0] : -1;
    pthread_mutex_unlock(&g_turn_mutex);

    fprintf(stdout, "[game] room %d: player %d eliminated — pieces cleared, rotation patched\n",
            room_id, elim_user_id);

    /* Record the eliminated player's placement (history). */
    pthread_mutex_lock(db_mutex);
    db_set_finish_position(db, match_id, elim_user_id, elim_position);
    pthread_mutex_unlock(db_mutex);

    if (survivor >= 0) {
        len = snprintf(json, sizeof(json),
            "{\"action\":\"" ACTION_GAME_OVER "\","
            "\"winner_user_id\":%d,\"reason\":\"elimination\"}", survivor);
        broadcast_game_action_to_room(live, room_id, json, len);
        pthread_mutex_lock(db_mutex);
        db_log_event(db, match_id, survivor, ACTION_GAME_OVER, json);
        /* Survivor places 1st and the match is now FINISHED. */
        db_set_finish_position(db, match_id, survivor, 1);
        db_finish_match(db, match_id, survivor);
        pthread_mutex_unlock(db_mutex);
        award_match_points(room_id, survivor, db, db_mutex);
        pthread_mutex_lock(&g_game_mutex);
        g_game_state[room_id].active = false;
        pthread_mutex_unlock(&g_game_mutex);
        turn_timer_cancel(room_id);
        mark_match_ended(room_id);  /* match is FINISHED; don't let a later leave cancel it */
        fprintf(stdout, "[game] room %d: game over by elimination — winner user_id=%d\n",
                room_id, survivor);
        return true;
    }
    return false;
}

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

    /* If the magnifying glass peeked this roll, use those values and clear them. */
    if (gs->peeked[slot]) {
        die1 = gs->peeked_die1[slot];
        die2 = gs->peeked_die2[slot];
        gs->peeked[slot] = false;
    }

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

            if (lives <= 0) {
                if (do_eliminate_player(room_id, user_id, slot,
                                        live, db, db_mutex, match_id))
                    return; /* game over — no turn to advance */
            }
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

/* Defined below handle_use_gun; forward-declared so handle_move_piece can call it. */
static void finish_move_turn(int fd, int user_id, int match_id, int room_id, int slot,
                             int piece_id, int to_sq, bool is_doubles, int second_move_die,
                             int captured_count, bool scored_goal,
                             client_list_t *live, db_t *db, pthread_mutex_t *db_mutex);

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
    if (slot < 0) {
        turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
        return;
    }

    pthread_mutex_lock(&g_game_mutex);
    game_state_t *gs = &g_game_state[room_id];

    /* Validate dice or bonus pending. */
    bool bonus_move = (gs->pending_die1 == 0 && gs->pending_die2 == 0
                       && gs->pending_movements[slot] > 0);
    bool normal_move = (gs->pending_die1 > 0);
    if (!bonus_move && !normal_move) {
        pthread_mutex_unlock(&g_game_mutex);
        turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
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
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            return; /* can't use bonus to exit home */
        }
        to_sq = parchis_advance(slot, from_sq, steps);
        if (to_sq < 0) {
            pthread_mutex_unlock(&g_game_mutex);
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            return;
        }
        if (!parchis_path_clear(slot, from_sq, steps, gs->piece_positions)) {
            pthread_mutex_unlock(&g_game_mutex);
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            return;
        }
    } else if (from_sq == 0) {
        /* Exit from home — need a 5 die. */
        if (die1 != 5 && die2 != 5 && (die1 + die2) != 5) {
            pthread_mutex_unlock(&g_game_mutex);
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
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
        if (to_sq < 0) {
            pthread_mutex_unlock(&g_game_mutex);
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            return;
        }

        if (!parchis_path_clear(slot, from_sq, steps, gs->piece_positions)) {
            pthread_mutex_unlock(&g_game_mutex);
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            return;
        }
    }

    /* Verify landing is allowed.
     * A square (excluding goal) can hold at most 2 pieces (disallows 3-piece blockades). */
    bool blocked = false;
    if (!parchis_is_goal(to_sq)) {
        int total_occ = 0;
        for (int s = 0; s < MAX_ROOM_PLAYERS; s++) {
            for (int p = 0; p < 4; p++) {
                if (gs->piece_positions[s][p] == to_sq) total_occ++;
            }
        }
        if (total_occ >= 2) blocked = true;
    }
    if (blocked) {
        pthread_mutex_unlock(&g_game_mutex);
        turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
        return;
    }

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

    bool spawn_priority = (to_sq == PARCHIS_EXIT[slot]);

    /* ── Gun prompt: if the attacker owns the gun and lands on an enemy,
     *    suspend the capture and send a private choice prompt.              ── */
    if ((!on_safe || spawn_priority) && !parchis_is_goal(to_sq)) {
        pthread_mutex_lock(&g_game_mutex);
        bool has_gun  = g_game_state[room_id].has_item[slot][ITEM_GUN];
        int  victim_s = -1, victim_p = -1;
        if (has_gun) {
            for (int s = 0; s < MAX_ROOM_PLAYERS && victim_s < 0; s++) {
                if (s == slot) continue;
                for (int p = 0; p < 4; p++) {
                    if (g_game_state[room_id].piece_positions[s][p] == to_sq) {
                        victim_s = s; victim_p = p; break;
                    }
                }
            }
        }
        if (victim_s >= 0) {
            g_game_state[room_id].pending_gun_choice[slot]          = true;
            g_game_state[room_id].pending_gun_piece_moved[slot]     = piece_id;
            g_game_state[room_id].pending_gun_victim_slot[slot]     = victim_s;
            g_game_state[room_id].pending_gun_victim_piece[slot]    = victim_p;
            g_game_state[room_id].pending_gun_at_sq[slot]           = to_sq;
            g_game_state[room_id].pending_gun_is_doubles[slot]      = is_doubles;
            g_game_state[room_id].pending_gun_second_move_die[slot] = second_move_die;
            int victim_uid = g_chair_state[room_id].slots[victim_s].user_id;
            pthread_mutex_unlock(&g_game_mutex);

            char prompt[192];
            int  plen = snprintf(prompt, sizeof(prompt),
                "{\"action\":\"" ACTION_GUN_AVAILABLE "\","
                "\"target_user_id\":%d,\"target_piece_id\":%d,\"square\":%d}",
                victim_uid, victim_p, to_sq);
            send_game_action_to_fd(fd, prompt, plen);
            return;  /* turn flow resumes in handle_use_gun */
        }
        pthread_mutex_unlock(&g_game_mutex);
    }

    /* Captures — land on non-safe square, OR on own spawn square (priority eats enemies). */
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

    bool scored_goal = parchis_is_goal(to_sq);
    finish_move_turn(fd, user_id, match_id, room_id, slot,
                     piece_id, to_sq, is_doubles, second_move_die,
                     captured_count, scored_goal,
                     live, db, db_mutex);
}

/* ── Item handlers ─────────────────────────────────────────────────────────── */

static bool has_moveable_pieces_bonus(int slot, int pending_mvs, int positions[][4])
{
    for (int p = 0; p < 4; p++) {
        int from = positions[slot][p];
        if (from <= 0) continue;
        int to = parchis_advance(slot, from, pending_mvs);
        if (to < 0) continue;
        if (!parchis_path_clear(slot, from, pending_mvs, positions)) continue;
        if (!can_land(to, slot, positions)) continue;
        return true;
    }
    return false;
}

/* finish_move_turn: runs after all captures are resolved (goal, golden square,
 * turn flow).  Called from handle_move_piece and handle_use_gun. */
static void finish_move_turn(int fd, int user_id, int match_id, int room_id, int slot,
                             int piece_id, int to_sq, bool is_doubles, int second_move_die,
                             int captured_count, bool scored_goal,
                             client_list_t *live, db_t *db, pthread_mutex_t *db_mutex)
{
    static const char *ITEM_NAMES[] = {
        "gun", "cigarette", "magnifying_glass", "handcuffs", "fire_axe"
    };

    /* Goal scored? */
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

        if (finished >= 4) {
            char win_json[128];
            int  wlen = snprintf(win_json, sizeof(win_json),
                "{\"action\":\"" ACTION_GAME_OVER "\","
                "\"winner_user_id\":%d,\"reason\":\"race\"}", user_id);
            broadcast_game_action_to_room(live, room_id, win_json, wlen);
            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].active = false;
            pthread_mutex_unlock(&g_game_mutex);

            /* ── Record finished match + full placements (history) ──────────────
             * Winner places 1st; the rest are ranked by pieces-in-goal desc. */
            int hist_uid[MAX_ROOM_PLAYERS];
            int hist_fin[MAX_ROOM_PLAYERS];
            int hist_n = 0;
            pthread_mutex_lock(&g_chair_mutex);
            /* Colour-indexed slots are sparse — scan all of them, not [0..player_count). */
            for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
                int uid = g_chair_state[room_id].slots[i].user_id;
                if (uid > 0) hist_uid[hist_n++] = uid;
            }
            pthread_mutex_unlock(&g_chair_mutex);

            pthread_mutex_lock(&g_game_mutex);
            for (int i = 0; i < hist_n; i++) {
                int s = user_id_to_slot(room_id, hist_uid[i]);
                hist_fin[i] = (s >= 0) ? count_finished(room_id, s) : 0;
            }
            pthread_mutex_unlock(&g_game_mutex);

            pthread_mutex_lock(db_mutex);
            db_log_event(db, match_id, user_id, ACTION_GAME_OVER, win_json);
            db_set_finish_position(db, match_id, user_id, 1);
            /* Selection-sort the non-winners by pieces-in-goal desc → positions 2..N. */
            bool used[MAX_ROOM_PLAYERS] = { false };
            int  next_pos = 2;
            for (int placed = 1; placed < hist_n; placed++) {
                int best = -1;
                for (int i = 0; i < hist_n; i++) {
                    if (used[i] || hist_uid[i] == user_id) continue;
                    if (best == -1 || hist_fin[i] > hist_fin[best]) best = i;
                }
                if (best == -1) break;
                used[best] = true;
                db_set_finish_position(db, match_id, hist_uid[best], next_pos++);
            }
            db_finish_match(db, match_id, user_id);
            pthread_mutex_unlock(db_mutex);
            award_match_points(room_id, user_id, db, db_mutex);

            turn_timer_cancel(room_id);
            mark_match_ended(room_id);  /* match is FINISHED; don't let a later leave cancel it */
            fprintf(stdout, "[game] room %d: game over by race — winner user_id=%d\n",
                    room_id, user_id);
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
        bool owned[NUM_ITEMS];
        pthread_mutex_lock(&g_game_mutex);
        for (int k = 0; k < NUM_ITEMS; k++)
            owned[k] = g_game_state[room_id].has_item[slot][k];
        pthread_mutex_unlock(&g_game_mutex);

        int spins[20];
        int spin_count = 0;
        int face;

        if (g_golden_force_count > 0) {
            /* Cycle the forced list but skip items the player already owns. */
            int tries = 0;
            do {
                face = g_golden_force[g_golden_force_idx % g_golden_force_count];
                g_golden_force_idx++;
                tries++;
            } while (face < NUM_ITEMS && owned[face] && tries < g_golden_force_count);

            /* All forced items owned — fall back to first unowned item. */
            if (face < NUM_ITEMS && owned[face]) {
                face = 0;
                for (int k = 0; k < NUM_ITEMS; k++)
                    if (!owned[k]) { face = k; break; }
            }
            spins[spin_count++] = face;
        } else {
            do {
                face = rand() % 6;
                spins[spin_count++] = face;
            } while ((face == 5 || (face < NUM_ITEMS && owned[face])) && spin_count < 20);
            if (face == 5 || (face < NUM_ITEMS && owned[face])) {
                face = 0;
                for (int k = 0; k < NUM_ITEMS; k++)
                    if (!owned[k]) { face = k; break; }
            }
        }

        pthread_mutex_lock(&g_game_mutex);
        g_game_state[room_id].has_item[slot][face] = true;
        pthread_mutex_unlock(&g_game_mutex);

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

    // If there is a pending bonus move, check if any pieces can legally move it.
    // If not, auto-skip the bonus movement phase entirely.
    if (pending_mvs > 0) {
        pthread_mutex_lock(&g_game_mutex);
        bool has_bonus_moves = has_moveable_pieces_bonus(slot, pending_mvs, g_game_state[room_id].piece_positions);
        pthread_mutex_unlock(&g_game_mutex);

        if (!has_bonus_moves) {
            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].pending_movements[slot] = 0;
            pthread_mutex_unlock(&g_game_mutex);

            char skip_json[128];
            int  slen = snprintf(skip_json, sizeof(skip_json),
                "{\"action\":\"" ACTION_EXTRA_TURN "\","
                "\"user_id\":%d,\"reason\":\"bonus_skipped\",\"pending_movements\":0}",
                user_id);
            broadcast_game_action_to_room(live, room_id, skip_json, slen);

            pending_mvs = 0;
        }
    }

    if (second_move_die > 0) {
        pthread_mutex_lock(&g_game_mutex);
        int  moveable2[4];
        bool can_exit2;
        int  n2 = parchis_moveable_pieces(slot, second_move_die, 0,
                                          g_game_state[room_id].piece_positions,
                                          moveable2, &can_exit2);
        pthread_mutex_unlock(&g_game_mutex);

        if (n2 == 0) {
            pthread_mutex_lock(&g_game_mutex);
            g_game_state[room_id].pending_die1 = 0;
            bool five_reroll = g_game_state[room_id].pending_five_reroll[slot];
            if (five_reroll) g_game_state[room_id].pending_five_reroll[slot] = false;
            pthread_mutex_unlock(&g_game_mutex);

            if (pending_mvs > 0) {
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
                turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
            } else if (five_reroll) {
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
            turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
        }
    } else if (pending_mvs > 0) {
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
        turn_timer_start(room_id, match_id, user_id, live, db, db_mutex);
    } else if (is_doubles) {
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

static void handle_use_gun(int fd, int user_id, int match_id, int room_id,
                            const char *json, client_list_t *live,
                            db_t *db, pthread_mutex_t *db_mutex)
{
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

    if (!gs->pending_gun_choice[slot]) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    /* Read and clear pending state. */
    int  piece_id        = gs->pending_gun_piece_moved[slot];
    int  victim_slot     = gs->pending_gun_victim_slot[slot];
    int  victim_piece    = gs->pending_gun_victim_piece[slot];
    int  at_sq           = gs->pending_gun_at_sq[slot];
    bool is_doubles      = gs->pending_gun_is_doubles[slot];
    int  second_move_die = gs->pending_gun_second_move_die[slot];
    gs->pending_gun_choice[slot] = false;

    int victim_user_id = g_chair_state[room_id].slots[victim_slot].user_id;
    pthread_mutex_unlock(&g_game_mutex);

    /* Parse choice: "fire" = use the gun, anything else = skip (normal capture). */
    const char *cv = json_str_val(json, "choice");
    char choice[8] = "skip";
    if (cv) json_str_copy(cv, choice, sizeof(choice));

    int  captured_count = 0;
    char result_json[256];

    if (strcmp(choice, "fire") == 0) {
        /* Consume the gun. */
        pthread_mutex_lock(&g_game_mutex);
        gs->has_item[slot][ITEM_GUN] = false;
        pthread_mutex_unlock(&g_game_mutex);

        if (rand() & 1) {
            /* ── Kill: capture + victim loses a life ─────────────────────── */
            do_capture(room_id, user_id, victim_slot, victim_piece, at_sq,
                       live, db, db_mutex, match_id);
            captured_count = 1;
            pthread_mutex_lock(&g_game_mutex);
            gs->pending_movements[slot] += 20;
            pthread_mutex_unlock(&g_game_mutex);

            int rlen = snprintf(result_json, sizeof(result_json),
                "{\"action\":\"" ACTION_GUN_RESULT "\","
                "\"attacker_user_id\":%d,\"attacker_piece_id\":%d,"
                "\"target_user_id\":%d,"
                "\"target_piece_id\":%d,\"square\":%d,\"result\":\"kill\"}",
                user_id, piece_id, victim_user_id, victim_piece, at_sq);
            broadcast_game_action_to_room(live, room_id, result_json, rlen);

            pthread_mutex_lock(&g_game_mutex);
            gs->life_charges[victim_slot]--;
            int lives = gs->life_charges[victim_slot];
            pthread_mutex_unlock(&g_game_mutex);

            rlen = snprintf(result_json, sizeof(result_json),
                "{\"action\":\"" ACTION_LIFE_LOST "\","
                "\"user_id\":%d,\"lives_remaining\":%d,\"reason\":\"gun\"}",
                victim_user_id, lives);
            broadcast_game_action_to_room(live, room_id, result_json, rlen);
            pthread_mutex_lock(db_mutex);
            db_log_event(db, match_id, user_id, ACTION_GUN_RESULT, result_json);
            pthread_mutex_unlock(db_mutex);

            if (lives <= 0) {
                if (do_eliminate_player(room_id, victim_user_id, victim_slot,
                                        live, db, db_mutex, match_id))
                    return; /* game over — skip finish_move_turn / advance_turn */
            }

        } else {
            /* ── Misfire: no capture. Attacker's piece is sent back to base. ── */
            pthread_mutex_lock(&g_game_mutex);
            gs->piece_positions[slot][piece_id] = 0;  /* 0 = base */
            pthread_mutex_unlock(&g_game_mutex);

            int rlen = snprintf(result_json, sizeof(result_json),
                "{\"action\":\"" ACTION_GUN_RESULT "\","
                "\"attacker_user_id\":%d,\"attacker_piece_id\":%d,"
                "\"target_user_id\":%d,"
                "\"target_piece_id\":%d,\"square\":%d,\"result\":\"misfire\"}",
                user_id, piece_id, victim_user_id, victim_piece, at_sq);
            broadcast_game_action_to_room(live, room_id, result_json, rlen);
            pthread_mutex_lock(db_mutex);
            db_log_event(db, match_id, user_id, ACTION_GUN_RESULT, result_json);
            pthread_mutex_unlock(db_mutex);
        }
    } else {
        /* ── Skip: normal capture + bonus moves, gun is NOT consumed ──────── */
        do_capture(room_id, user_id, victim_slot, victim_piece, at_sq,
                   live, db, db_mutex, match_id);
        captured_count = 1;
        pthread_mutex_lock(&g_game_mutex);
        gs->pending_movements[slot] += 20;
        pthread_mutex_unlock(&g_game_mutex);
    }

    /* Continue normal turn flow. */
    finish_move_turn(fd, user_id, match_id, room_id, slot,
                     piece_id, at_sq, is_doubles, second_move_die,
                     captured_count, false /* scored_goal */,
                     live, db, db_mutex);
}

static void handle_use_cigarette(int fd, int user_id, int match_id, int room_id,
                                  const char *json, client_list_t *live,
                                  db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd; (void)json;

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

    /* No moveable pieces after the reroll → don't strand the player on the turn
     * clock until it expires (which also costs a life).  Mirror handle_roll_dice:
     * doubles grant another roll, otherwise the turn passes — which slides the
     * cubilete over to the next player. */
    if (n_moveable == 0) {
        if (die1 == die2) {
            pthread_mutex_lock(&g_game_mutex);
            int consec = gs->consecutive_doubles[slot];
            pthread_mutex_unlock(&g_game_mutex);

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

static void handle_use_magnifying_glass(int fd, int user_id, int match_id, int room_id,
                                         const char *json, client_list_t *live,
                                         db_t *db, pthread_mutex_t *db_mutex)
{
    (void)json;

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

    /* Must own the glass. The pre-roll window is enforced client-side;
     * pending_die1/die2 carry over from the previous turn so we cannot
     * use them to detect a same-turn roll here. */
    if (!gs->has_item[slot][ITEM_MAGNIFYING_GLASS]) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    /* Pre-roll the dice for this player's next roll. */
    int die1 = (rand() % 6) + 1;
    int die2 = (rand() % 6) + 1;

    gs->peeked[slot]      = true;
    gs->peeked_die1[slot] = die1;
    gs->peeked_die2[slot] = die2;
    gs->has_item[slot][ITEM_MAGNIFYING_GLASS] = false;

    pthread_mutex_unlock(&g_game_mutex);

    /* Send peeked values privately to the player only. */
    char msg[128];
    int  mlen = snprintf(msg, sizeof(msg),
        "{\"action\":\"" ACTION_MAGNIFYING_RESULT "\",\"die1\":%d,\"die2\":%d}",
        die1, die2);
    send_game_action_to_fd(fd, msg, mlen);

    pthread_mutex_lock(db_mutex);
    db_log_event(db, match_id, user_id, ACTION_MAGNIFYING_RESULT, msg);
    pthread_mutex_unlock(db_mutex);
}

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

/* Returns the ring square immediately before `sq` (with 68→1 wrap). 0 if out of range. */
static int ring_prev(int sq)
{
    if (sq < 1 || sq > 68) return 0;
    return sq == 1 ? 68 : sq - 1;
}

/* Returns the ring square immediately after `sq` (with 68→1 wrap). 0 if out of range. */
static int ring_next(int sq)
{
    if (sq < 1 || sq > 68) return 0;
    return sq == 68 ? 1 : sq + 1;
}

/* True if no piece (any color) sits on `sq`. */
static bool square_is_empty(int room_id, int sq)
{
    game_state_t *gs = &g_game_state[room_id];
    for (int s = 0; s < MAX_ROOM_PLAYERS; s++)
        for (int p = 0; p < 4; p++)
            if (gs->piece_positions[s][p] == sq) return false;
    return true;
}

static void handle_use_fire_axe(int fd, int user_id, int match_id, int room_id,
                                 const char *json, client_list_t *live,
                                 db_t *db, pthread_mutex_t *db_mutex)
{
    (void)fd; (void)match_id; (void)db; (void)db_mutex;

    int target_sq = json_int_val(json, "target_square", -1);
    if (target_sq < 1 || target_sq > 68) return;  /* ring squares only */

    int slot = user_id_to_slot(room_id, user_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_game_mutex);
    game_state_t *gs = &g_game_state[room_id];

    /* Must own the axe. */
    if (!gs->has_item[slot][ITEM_FIRE_AXE]) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    /* Collect exactly the two pieces sitting on target_sq. Same-color barriers and
     * mixed-color barriers on safe squares both qualify. */
    int hit_slot[2];
    int hit_piece[2];
    int hit_count = 0;
    for (int s = 0; s < MAX_ROOM_PLAYERS && hit_count <= 2; s++) {
        for (int p = 0; p < 4 && hit_count <= 2; p++) {
            if (gs->piece_positions[s][p] == target_sq) {
                if (hit_count < 2) {
                    hit_slot[hit_count]  = s;
                    hit_piece[hit_count] = p;
                }
                hit_count++;
            }
        }
    }
    if (hit_count != 2) {
        pthread_mutex_unlock(&g_game_mutex);
        return;  /* must be exactly 2 pieces to form a clean barrier */
    }

    /* Per design: prev/next squares must be empty to avoid edge-case interactions. */
    int prev_sq = ring_prev(target_sq);
    int next_sq = ring_next(target_sq);
    if (prev_sq == 0 || next_sq == 0) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }
    if (!square_is_empty(room_id, prev_sq) || !square_is_empty(room_id, next_sq)) {
        pthread_mutex_unlock(&g_game_mutex);
        return;
    }

    /* Randomly choose which of the two goes forward, the other goes backward. */
    int fwd_idx = rand() & 1;
    int bwd_idx = 1 - fwd_idx;

    int fwd_slot  = hit_slot[fwd_idx];
    int fwd_piece = hit_piece[fwd_idx];
    int bwd_slot  = hit_slot[bwd_idx];
    int bwd_piece = hit_piece[bwd_idx];

    gs->piece_positions[fwd_slot][fwd_piece] = next_sq;
    gs->piece_positions[bwd_slot][bwd_piece] = prev_sq;

    /* Consume the axe. */
    gs->has_item[slot][ITEM_FIRE_AXE] = false;

    pthread_mutex_unlock(&g_game_mutex);

    int fwd_user = g_chair_state[room_id].slots[fwd_slot].user_id;
    int bwd_user = g_chair_state[room_id].slots[bwd_slot].user_id;

    /* Broadcast the axe event with both displaced pieces. */
    char out[384];
    int  len = snprintf(out, sizeof(out),
        "{\"action\":\"" ACTION_FIRE_AXE_USED "\","
        "\"attacker_user_id\":%d,\"target_square\":%d,"
        "\"forward\":{\"user_id\":%d,\"piece_id\":%d,\"to_square\":%d},"
        "\"backward\":{\"user_id\":%d,\"piece_id\":%d,\"to_square\":%d}}",
        user_id, target_sq,
        fwd_user, fwd_piece, next_sq,
        bwd_user, bwd_piece, prev_sq);
    broadcast_game_action_to_room(live, room_id, out, len);

    /* Also broadcast a barrier_broken so existing client logic re-centers any
     * stragglers — though by construction nothing remains on target_sq. */
    char bar_json[128];
    int  blen = snprintf(bar_json, sizeof(bar_json),
        "{\"action\":\"" ACTION_BARRIER_BROKEN "\","
        "\"user_id\":%d,\"square\":%d,\"reason\":\"fire_axe\"}",
        user_id, target_sq);
    broadcast_game_action_to_room(live, room_id, bar_json, blen);
}

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
