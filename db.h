#ifndef DB_H
#define DB_H

#include <mysql/mysql.h>
#include <stdbool.h>

typedef struct {
    MYSQL *conn;
} db_t;

bool db_init(db_t *db, const char *host, const char *user, const char *pass, const char *dbname);
void db_close(db_t *db);

int db_register_user(db_t *db, const char *username, const char *password_hash);
int db_authenticate_user(db_t *db, const char *username, const char *password_hash, int *user_id);

#endif // DB_H
