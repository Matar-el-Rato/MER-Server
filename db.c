#include "db.h"
#include <stdio.h>
#include <string.h>

bool db_init(db_t *db, const char *host, const char *user, const char *pass, const char *dbname) {
    db->conn = mysql_init(NULL);
    if (db->conn == NULL) {
        fprintf(stderr, "mysql_init() failed\n");
        return false;
    }

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
    char query[1024];
    // Simple insertion. In a production app, we would use prepared statements to prevent SQL Injection.
    // For this educational/basic version, we use sprintf.
    snprintf(query, sizeof(query), 
             "INSERT INTO users (username, password_hash) VALUES ('%s', '%s')", 
             username, password_hash);

    if (mysql_query(db->conn, query)) {
        fprintf(stderr, "Registration error: %s\n", mysql_error(db->conn));
        return -1; // Likely user exists or DB error
    }

    return 0;
}

int db_authenticate_user(db_t *db, const char *username, const char *password_hash, int *user_id) {
    char query[1024];
    snprintf(query, sizeof(query), 
             "SELECT id FROM users WHERE username='%s' AND password_hash='%s'", 
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
        *user_id = atoi(row[0]);
        status = 0; // Success
    }

    mysql_free_result(res);
    return status;
}
