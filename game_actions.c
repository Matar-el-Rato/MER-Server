#include "game_actions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

const char *SLOT_COLORS[MAX_ROOM_PLAYERS] = {"blue", "green", "yellow", "red"};

chair_state_t  g_chair_state[NUM_ROOMS + 1];
pthread_mutex_t g_chair_mutex = PTHREAD_MUTEX_INITIALIZER;

void chair_state_init(int room_id, int match_id)
{
    pthread_mutex_lock(&g_chair_mutex);
    memset(&g_chair_state[room_id], 0, sizeof(chair_state_t));
    g_chair_state[room_id].match_id = match_id;
    g_chair_state[room_id].room_id  = room_id;
    pthread_mutex_unlock(&g_chair_mutex);
}

/* Returns a pointer to the first character of a JSON string value (after the
 * opening '"'), or NULL if the key is missing or the value is not a string. */
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

/* Copies a JSON string value into dst (up to maxlen-1 chars), stopping at
 * the closing '"' or end of string. */
static void json_str_copy(const char *start, char *dst, int maxlen)
{
    int i = 0;
    while (i < maxlen - 1 && start[i] != '"' && start[i] != '\0')
        dst[i] = start[i++];
    dst[i] = '\0';
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
    if (slot == -1) return; /* unknown color */

    char broadcast_json[256];
    int  claimed = 0;

    pthread_mutex_lock(&g_chair_mutex);

    /* Validate this packet belongs to the current match. */
    if (g_chair_state[room_id].match_id != match_id) {
        pthread_mutex_unlock(&g_chair_mutex);
        return;
    }

    /* Reject if this user already claimed any chair. */
    for (int i = 0; i < MAX_ROOM_PLAYERS; i++) {
        if (g_chair_state[room_id].slots[i].user_id == user_id) {
            pthread_mutex_unlock(&g_chair_mutex);
            return;
        }
    }

    /* Claim the slot if it's still free. */
    if (g_chair_state[room_id].slots[slot].user_id == 0) {
        g_chair_state[room_id].slots[slot].user_id = user_id;
        strncpy(g_chair_state[room_id].slots[slot].username, username, MAX_USERNAME - 1);
        g_chair_state[room_id].slots[slot].username[MAX_USERNAME - 1] = '\0';

        snprintf(broadcast_json, sizeof(broadcast_json),
            "{\"action\":\"chair_taken\",\"color\":\"%s\",\"user_id\":%d,\"username\":\"%s\"}",
            color, user_id, username);
        claimed = 1;
    }

    pthread_mutex_unlock(&g_chair_mutex);

    if (claimed) {
        broadcast_game_action_to_room(live, room_id, broadcast_json,
                                      (int)strlen(broadcast_json));

        pthread_mutex_lock(db_mutex);
        db_set_chair(db, match_id, user_id, color);
        db_log_event(db, match_id, user_id, "choose_chair", broadcast_json);
        pthread_mutex_unlock(db_mutex);
    }
}

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

    if (strcmp(action, "choose_chair") == 0) {
        handle_choose_chair(fd, user_id, username, match_id, room_id,
                            json, live, db, db_mutex);
    }
    /* future: initiative_roll, use_gun, use_cigarette, ... */
}

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
            send(live->entries[i].socket_fd, json, (size_t)json_len, MSG_NOSIGNAL);
        }
    }
    pthread_mutex_unlock(&live->mutex);
}
