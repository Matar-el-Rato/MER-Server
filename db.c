#include "db.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool db_init(db_t *db, const char *host, const char *user, const char *pass, const char *dbname) {
    db->conn = mysql_init(NULL);
    if (db->conn == NULL) {
        fprintf(stderr, "mysql_init() failed\n");
        return false;
    }

    bool reconnect = true;
    mysql_options(db->conn, MYSQL_OPT_RECONNECT, &reconnect);

    if (mysql_real_connect(db->conn, host, user, pass, dbname, 0, NULL, 0) == NULL) {
        fprintf(stderr, "mysql_real_connect() failed: %s\n", mysql_error(db->conn));
        mysql_close(db->conn);
        return false;
    }

    return true;
}

void db_close(db_t *db) {
    if (db->conn) {
        mysql_close(db->conn);
    }
}

int db_register_user(db_t *db, const char *username, const char *password_hash) {
    mysql_ping(db->conn);
    char query[1024];
    snprintf(query, sizeof(query), 
             "INSERT INTO users (username, password_hash) VALUES ('%s', '%s')", 
             username, password_hash);

    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "Registration error: %s\n", mysql_error(db->conn));
        return -1; // Likely user exists or DB error
    }

    return 0;
}

int db_authenticate_user(db_t *db, const char *username, const char *password_hash, int *user_id, int *skin_id) {
    mysql_ping(db->conn);
    char query[1024];
    /* COALESCE returns 101 (default skin) when skin_id is NULL (new accounts). */
    snprintf(query, sizeof(query),
             "SELECT id, COALESCE(skin_id, 101) FROM users WHERE username='%s' AND password_hash='%s'",
             username, password_hash);

    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "Authentication error: %s\n", mysql_error(db->conn));
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(db->conn);
    if (res == NULL) return -1;

    MYSQL_ROW row = mysql_fetch_row(res);
    int status = -1;
    if (row) {
        *user_id  = atoi(row[0]);
        *skin_id  = atoi(row[1]);
        status = 0;
    }

    mysql_free_result(res);
    return status;
}

int db_get_skin_id(db_t *db, int user_id) {
    mysql_ping(db->conn);
    char query[128];
    snprintf(query, sizeof(query),
             "SELECT COALESCE(skin_id, 101) FROM users WHERE id=%d", user_id);

    if (mysql_query(db->conn, query)) return 101;

    MYSQL_RES *res = mysql_store_result(db->conn);
    if (!res) return 101;

    MYSQL_ROW row = mysql_fetch_row(res);
    int skin = row ? atoi(row[0]) : 101;
    mysql_free_result(res);
    return skin;
}

int db_update_skin(db_t *db, int user_id, int skin_id) {
    mysql_ping(db->conn);
    char query[512];
    snprintf(query, sizeof(query),
             "UPDATE users SET skin_id=%d WHERE id=%d",
             skin_id, user_id);

    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "Skin update error: %s\n", mysql_error(db->conn));
        return -1;
    }

    return 0;
}

int db_username_exists(db_t *db, const char *username) {
    mysql_ping(db->conn);
    char esc[2 * 50 + 1];
    mysql_real_escape_string(db->conn, esc, username, strlen(username));
    char query[256];
    snprintf(query, sizeof(query),
        "SELECT 1 FROM users WHERE username='%s' LIMIT 1", esc);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_username_exists error: %s\n", mysql_error(db->conn));
        return -1;
    }
    MYSQL_RES *res = mysql_store_result(db->conn);
    if (!res) return -1;
    int exists = mysql_fetch_row(res) ? 1 : 0;
    mysql_free_result(res);
    return exists;
}

int db_add_points(db_t *db, int user_id, int delta) {
    mysql_ping(db->conn);
    char query[256];
    /* GREATEST(0, ...) clamps the score so it can never drop below zero. */
    snprintf(query, sizeof(query),
        "UPDATE users SET points = GREATEST(0, points + (%d)) WHERE id=%d",
        delta, user_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_add_points error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

int db_create_match(db_t *db, int room_id) {
    mysql_ping(db->conn);
    char query[256];
    snprintf(query, sizeof(query),
        "INSERT INTO matches (room_id, status) VALUES (%d, 'WAITING')", room_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_create_match error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return (int)mysql_insert_id(db->conn);
}

int db_add_participants(db_t *db, int match_id, int *user_ids, int count) {
    mysql_ping(db->conn);
    for (int i = 0; i < count; i++) {
        char query[256];
        snprintf(query, sizeof(query),
            "INSERT INTO match_participants (match_id, user_id, turn_order) VALUES (%d, %d, %d)",
            match_id, user_ids[i], i);
        if (mysql_query(db->conn, query))
            fprintf(stderr, "db_add_participants error (user_id=%d): %s\n",
                user_ids[i], mysql_error(db->conn));
    }
    return 0;
}

int db_start_match(db_t *db, int match_id) {
    mysql_ping(db->conn);
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE matches SET status='PLAYING', start_time=NOW() WHERE id=%d", match_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_start_match error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

int db_cancel_match(db_t *db, int match_id) {
    mysql_ping(db->conn);
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE matches SET status='CANCELLED', end_time=NOW()"
        " WHERE id=%d AND status IN ('WAITING','PLAYING')",
        match_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_cancel_match error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

int db_finish_match(db_t *db, int match_id, int winner_user_id) {
    mysql_ping(db->conn);
    char query[256];
    /* Guard on status='PLAYING' so a finish can't overwrite a CANCELLED row
     * (e.g. if the last loser disconnected at the same moment the winner scored). */
    if (winner_user_id > 0)
        snprintf(query, sizeof(query),
            "UPDATE matches SET status='FINISHED', end_time=NOW(), winner_id=%d"
            " WHERE id=%d AND status='PLAYING'",
            winner_user_id, match_id);
    else
        snprintf(query, sizeof(query),
            "UPDATE matches SET status='FINISHED', end_time=NOW(), winner_id=NULL"
            " WHERE id=%d AND status='PLAYING'",
            match_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_finish_match error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

int db_set_finish_position(db_t *db, int match_id, int user_id, int position) {
    mysql_ping(db->conn);
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE match_participants SET finish_position=%d"
        " WHERE match_id=%d AND user_id=%d",
        position, match_id, user_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_set_finish_position error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

int db_set_chair(db_t *db, int match_id, int user_id, const char *color) {
    mysql_ping(db->conn);
    char escaped[32];
    mysql_real_escape_string(db->conn, escaped, color, strlen(color));
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE match_participants SET chair_color='%s'"
        " WHERE match_id=%d AND user_id=%d",
        escaped, match_id, user_id);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_set_chair error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

int db_log_event(db_t *db, int match_id, int user_id,
                 const char *event_type, const char *event_data) {
    mysql_ping(db->conn);
    char esc_type[64];
    char esc_data[1024];
    mysql_real_escape_string(db->conn, esc_type, event_type, strlen(event_type));
    mysql_real_escape_string(db->conn, esc_data, event_data, strlen(event_data));
    char query[1200];
    if (user_id > 0)
        snprintf(query, sizeof(query),
            "INSERT INTO match_events (match_id, user_id, event_type, event_data)"
            " VALUES (%d, %d, '%s', '%s')",
            match_id, user_id, esc_type, esc_data);
    else
        snprintf(query, sizeof(query),
            "INSERT INTO match_events (match_id, user_id, event_type, event_data)"
            " VALUES (%d, NULL, '%s', '%s')",
            match_id, esc_type, esc_data);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_log_event error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}

/* Appends `s` into out[*off..] as a JSON string body (no surrounding quotes),
 * escaping " \ and control chars. Never writes past out_cap. */
static void json_escape_append(char *out, size_t out_cap, size_t *off, const char *s) {
    for (; *s && *off + 8 < out_cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[(*off)++] = '\\';
            out[(*off)++] = (char)c;
        } else if (c < 0x20) {
            *off += (size_t)snprintf(out + *off, out_cap - *off, "\\u%04x", c);
        } else {
            out[(*off)++] = (char)c;
        }
    }
}

int db_get_match_history_json(db_t *db, const char *username, char *out, size_t out_cap) {
    mysql_ping(db->conn);

    char esc[2 * 50 + 1];
    mysql_real_escape_string(db->conn, esc, username, strlen(username));

    /* GROUP_CONCAT pairs: names tab-separated, positions comma-separated, both
     * ordered the same way so the i-th name lines up with the i-th position.
     * Tab is a safe name delimiter (registration rejects non-isprint chars). */
    char query[1024];
    snprintf(query, sizeof(query),
        "SELECT m.id, m.room_id, m.status, "
        "UNIX_TIMESTAMP(m.start_time), UNIX_TIMESTAMP(m.end_time), "
        "COALESCE(w.username,''), COUNT(DISTINCT mp.user_id), "
        "GROUP_CONCAT(u.username ORDER BY COALESCE(mp.finish_position,999), mp.turn_order SEPARATOR '\\t'), "
        "GROUP_CONCAT(COALESCE(mp.finish_position,0) ORDER BY COALESCE(mp.finish_position,999), mp.turn_order SEPARATOR ',') "
        "FROM matches m "
        "JOIN match_participants mp ON mp.match_id=m.id "
        "JOIN users u ON u.id=mp.user_id "
        "LEFT JOIN users w ON w.id=m.winner_id "
        "WHERE m.id IN (SELECT mp2.match_id FROM match_participants mp2 "
        "JOIN users u2 ON u2.id=mp2.user_id WHERE u2.username='%s') "
        "GROUP BY m.id ORDER BY m.start_time DESC LIMIT 50",
        esc);

    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_get_match_history_json error: %s\n", mysql_error(db->conn));
        return -1;
    }
    MYSQL_RES *res = mysql_store_result(db->conn);
    if (!res) return -1;

    size_t off = 0;
    out[off++] = '[';
    int count = 0;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        /* Leave headroom for one full match entry (header + up to 4 players). */
        if (off + 1024 >= out_cap) break;

        long long start = row[3] ? atoll(row[3]) : 0;
        long long end   = row[4] ? atoll(row[4]) : 0;
        long long dur   = (end > start) ? (end - start) : 0;

        if (count > 0) out[off++] = ',';

        off += (size_t)snprintf(out + off, out_cap - off,
            "{\"match_id\":%s,\"room_id\":%s,\"status\":\"%s\",\"start\":%lld,\"end\":%lld,"
            "\"duration\":%lld,\"winner\":\"",
            row[0] ? row[0] : "0", row[1] ? row[1] : "0", row[2] ? row[2] : "",
            start, end, dur);
        json_escape_append(out, out_cap, &off, row[5] ? row[5] : "");
        off += (size_t)snprintf(out + off, out_cap - off,
            "\",\"player_count\":%s,\"players\":[", row[6] ? row[6] : "0");

        const char *names = row[7] ? row[7] : "";
        const char *poss  = row[8] ? row[8] : "";
        int   first = 1;
        char  namebuf[32];
        while (*names && off + 128 < out_cap) {
            int n = 0;
            while (*names && *names != '\t' && n < (int)sizeof(namebuf) - 1)
                namebuf[n++] = *names++;
            namebuf[n] = '\0';
            if (*names == '\t') names++;

            int pos = atoi(poss);
            while (*poss && *poss != ',') poss++;
            if (*poss == ',') poss++;

            if (!first) out[off++] = ',';
            first = 0;
            off += (size_t)snprintf(out + off, out_cap - off, "{\"name\":\"");
            json_escape_append(out, out_cap, &off, namebuf);
            off += (size_t)snprintf(out + off, out_cap - off, "\",\"position\":%d}", pos);
        }
        off += (size_t)snprintf(out + off, out_cap - off, "]}");
        count++;
    }

    if (off + 2 < out_cap) out[off++] = ']';
    out[off < out_cap ? off : out_cap - 1] = '\0';

    mysql_free_result(res);
    return count;
}

int db_get_leaderboard_json(db_t *db, char *out, size_t out_cap) {
    mysql_ping(db->conn);

    if (mysql_query(db->conn,
            "SELECT username, points FROM users "
            "ORDER BY points DESC, username ASC LIMIT 100")) {
        fprintf(stderr, "db_get_leaderboard_json error: %s\n", mysql_error(db->conn));
        return -1;
    }
    MYSQL_RES *res = mysql_store_result(db->conn);
    if (!res) return -1;

    size_t off = 0;
    out[off++] = '[';
    int count = 0;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (off + 128 >= out_cap) break;
        if (count > 0) out[off++] = ',';
        off += (size_t)snprintf(out + off, out_cap - off, "{\"username\":\"");
        json_escape_append(out, out_cap, &off, row[0] ? row[0] : "");
        off += (size_t)snprintf(out + off, out_cap - off,
            "\",\"points\":%s}", row[1] ? row[1] : "0");
        count++;
    }

    if (off + 2 < out_cap) out[off++] = ']';
    out[off < out_cap ? off : out_cap - 1] = '\0';

    mysql_free_result(res);
    return count;
}
