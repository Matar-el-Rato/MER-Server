#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>

void report_error(MYSQL *conn) {
    printf("Error: %s\n", mysql_error(conn));
    mysql_close(conn);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <id>\n", argv[0]);
        return 1;
    }

    MYSQL *conn = mysql_init(NULL);
    if (!mysql_real_connect(conn, "localhost", "admin", "admin", "matarelrato-db", 0, NULL, 0)) {
        report_error(conn);
    }

    char query[512];
    char *select = "SELECT m.id, m.status, m.start_time, m.end_time, u.username ";
    char *from   = "FROM matches m LEFT JOIN users u ON m.winner_id = u.id ";
    char *where  = "WHERE m.id = ";

    /*
    Straight up connect them through the ID and Username in the SQL side, so it's cleaner C code.
    */

    sprintf(query, "%s %s %s %s", select, from, where, argv[1]);

    if (mysql_query(conn, query)) {
        report_error(conn);
    }

    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW row = mysql_fetch_row(res);

    if (row) {
        printf("Match ID:  %s\n", row[0]);
        printf("Status:    %s\n", row[1] ? row[1] : "N/A");
        printf("Start:     %s\n", row[2] ? row[2] : "N/A");
        printf("End:       %s\n", row[3] ? row[3] : "N/A");
        printf("Winner:    %s\n", row[4] ? row[4] : "Non-determined");
    } else {
        printf("No match found.\n");
    }

    mysql_free_result(res);
    mysql_close(conn);
    return 0;
}