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
int db_get_skin_id(db_t *db, int user_id); /* returns 101 on any error */
int db_update_skin(db_t *db, int user_id, int skin_id);

/* Points: add delta (may be negative) to a user's score, floored at 0. */
int db_add_points(db_t *db, int user_id, int delta);

/* Returns 1 if a user with this username exists, 0 if not, -1 on query error. */
int db_username_exists(db_t *db, const char *username);

/* Match lifecycle */
int db_create_match(db_t *db, int room_id);                                       /* INSERT WAITING match, returns match_id or -1 */
int db_add_participants(db_t *db, int match_id, int *user_ids, int count);        /* INSERT match_participants rows */
int db_start_match(db_t *db, int match_id);                                       /* status → PLAYING, start_time = NOW() */
int db_cancel_match(db_t *db, int match_id);                                      /* status → CANCELLED, end_time = NOW() */
int db_finish_match(db_t *db, int match_id, int winner_user_id);                  /* status → FINISHED, end_time = NOW(), winner_id (NULL if <=0) */
int db_set_finish_position(db_t *db, int match_id, int user_id, int position);    /* UPDATE match_participants SET finish_position */

/* Game actions */
int db_set_chair(db_t *db, int match_id, int user_id, const char *color);        /* UPDATE match_participants SET chair_color */
int db_log_event(db_t *db, int match_id, int user_id,
                 const char *event_type, const char *event_data);                 /* INSERT match_events row */

/* Match history */
/* Builds a JSON array of every match `username` participated in (newest first,
 * capped at 50) into `out`. Returns the match count, or -1 on query error. */
int db_get_match_history_json(db_t *db, const char *username, char *out, size_t out_cap);

/* Builds a JSON array of the top players by points (desc, capped at 100) into
 * `out`. Returns the row count, or -1 on query error. */
int db_get_leaderboard_json(db_t *db, char *out, size_t out_cap);

#endif // DB_H
