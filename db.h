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
int db_authenticate_user(db_t *db, const char *username, const char *password_hash, int *user_id, int *skin_id);
int db_update_skin(db_t *db, int user_id, int skin_id);

/* Match lifecycle */
int db_create_match(db_t *db, int room_id);                                       /* INSERT WAITING match, returns match_id or -1 */
int db_add_participants(db_t *db, int match_id, int *user_ids, int count);        /* INSERT match_participants rows */
int db_start_match(db_t *db, int match_id);                                       /* status → PLAYING, start_time = NOW() */
int db_cancel_match(db_t *db, int match_id);                                      /* status → CANCELLED, end_time = NOW() */

#endif // DB_H
