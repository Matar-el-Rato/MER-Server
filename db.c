#include "db.h"
#include <stdio.h>
#include <string.h>

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
    snprintf(query, sizeof(query),
        "INSERT INTO match_events (match_id, user_id, event_type, event_data)"
        " VALUES (%d, %d, '%s', '%s')",
        match_id, user_id, esc_type, esc_data);
    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "db_log_event error: %s\n", mysql_error(db->conn));
        return -1;
    }
    return 0;
}
