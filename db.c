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
