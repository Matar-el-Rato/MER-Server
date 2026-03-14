#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <crypt.h>
#include "protocol.h"
#include "db.h"

#define SALT "$6$matar_el_rato$" // SHA-512 Salt

#define DEFAULT_PORT 8888

void error(const char *msg) {
    perror(msg);
    exit(1);
}

// Function to handle a single client
void handle_client(int sock_conn, db_t *db) {
    uint8_t req_type;
    int ret;

    // Read request type first
    ret = read(sock_conn, &req_type, sizeof(req_type));
    if (ret <= 0) {
        close(sock_conn);
        return;
    }

    if (req_type == REQ_REGISTER) {
        register_req_t req;
        read(sock_conn, (char*)&req + sizeof(uint8_t), sizeof(register_req_t) - sizeof(uint8_t));
        
        char log_msg[256];
        sprintf(log_msg, "Register request for user: %s\n", req.username);
        write(1, log_msg, strlen(log_msg));
        
        // Hash the password before storing
        char *pass_hash = crypt(req.password, SALT);
        
        generic_res_t res;
        memset(&res, 0, sizeof(res));
        if (db_register_user(db, req.username, pass_hash) == 0) {
            res.code = RES_SUCCESS;
            strcpy(res.message, "User registered successfully");
        } else {
            res.code = RES_ERR_USER_EXISTS;
            strcpy(res.message, "User already exists or DB error");
        }
        write(sock_conn, &res, sizeof(res));

    } else if (req_type == REQ_LOGIN) {
        login_req_t req;
        read(sock_conn, (char*)&req + sizeof(uint8_t), sizeof(login_req_t) - sizeof(uint8_t));

        char log_msg[256];
        sprintf(log_msg, "Login request for user: %s\n", req.username);
        write(1, log_msg, strlen(log_msg));

        // Hash the incoming password with the same salt to compare
        char *pass_hash = crypt(req.password, SALT);

        int user_id;
        generic_res_t res;
        memset(&res, 0, sizeof(res));
        if (db_authenticate_user(db, req.username, pass_hash, &user_id) == 0) {
            res.code = RES_SUCCESS;
            sprintf(res.message, "Login successful. Welcome ID %d", user_id);
        } else {
            res.code = RES_ERR_INVALID_CREDENTIALS;
            strcpy(res.message, "Invalid credentials");
        }
        write(sock_conn, &res, sizeof(res));
    } else if (req_type == REQ_CHANGE_SKIN) {
        change_skin_req_t req;
        read(sock_conn, (char*)&req + sizeof(uint8_t), sizeof(change_skin_req_t) - sizeof(uint8_t));

        char log_msg[256];
        sprintf(log_msg, "Change skin request: User ID %d -> Skin ID %d\n", req.user_id, req.skin_id);
        write(1, log_msg, strlen(log_msg));

        generic_res_t res;
        memset(&res, 0, sizeof(res));
        if (db_update_skin(db, req.user_id, req.skin_id) == 0) {
            res.code = RES_SUCCESS;
            strcpy(res.message, "Skin updated successfully");
        } else {
            res.code = RES_ERR_DATABASE;
            strcpy(res.message, "Failed to update skin");
        }
        write(sock_conn, &res, sizeof(res));
    } else if (req_type == REQ_LIST_ROOMS) {
        /*
           REQ_LIST_ROOMS
           1. Query the `rooms` table for all room IDs and player counts.
           2. Format the results into a packet containing tuples of (room_id, current, max).
           3. Send to client to populate the "Room Browser" UI.
        */

    } else if (req_type == REQ_JOIN_ROOM) {
        /*
           REQ_JOIN_ROOM
           1. Check if room exists and if `current_players < max_players`.
           2. Update `rooms.current_players` (increment).
           3. Insert record into `match_participants` (linking user to current room/match).
           4. Respond with SUCCESS and the match_id assigned.
        */

    } else if (req_type == REQ_SEND_CHAT) {
        /*
           REQ_SEND_CHAT
           1. Parse message and optional `room_id`.
           2. Insert into `chat_messages` table.
           3. ROUTING LOGIC:
              - If room_id is NULL/0: Broadcast to all connected clients WHO ARE NOT in a room.
              - If room_id is NOT 0: Broadcast ONLY to clients who are match_participants in that room.
        */

    } else if (req_type == REQ_GAME_ACTION) {
        /*
           REQ_GAME_ACTION
           1. Validate that the user is actually in the match.
           2. Parse action (move, dice roll, etc) and insert into `match_events`.
           3. Trigger "Game Logic Engine" to determine the next state.
           4. Notify other room participants of the change.
        */
    }


    close(sock_conn);
}

int main(int argc, char *argv[]) {
    int sock_listen, sock_conn;
    struct sockaddr_in serv_adr;
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    db_t db;

    // Initialize Database
    if (!db_init(&db, "localhost", "admin", "admin", "matarelrato-db")) {
        fprintf(stderr, "Failed to connect to database\n");
        exit(1);
    }
    printf("Connected to database successfully.\n");

    // 1. Create Socket (System Call)
    if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        error("Error creant socket");

    // 2. Bind (System Call)
    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_adr.sin_port = htons(port);

    if (bind(sock_listen, (struct sockaddr *) &serv_adr, sizeof(serv_adr)) < 0)
        error("Error al bind");

    // 3. Listen (System Call)
    if (listen(sock_listen, 10) < 0)
        error("Error in listen");

    char start_msg[128];
    sprintf(start_msg, "Server listening on port %d...\n", port);
    write(1, start_msg, strlen(start_msg));

    for (;;) {
        // 4. Accept (System Call)
        sock_conn = accept(sock_listen, NULL, NULL);
        if (sock_conn < 0) {
            if (errno == EINTR) continue;
            perror("Error in accept");
            continue;
        }

        /* 
           FUTURE MULTI-THREADING HOOK (STATIC MEMORY VERSION):
           Instead of calling handle_client directly, we would do:
           
           static int socket_pool[MAX_THREADS];
           // Find an empty slot in socket_pool, copy sock_conn there,
           // and pass the pointer to the thread.
           
           pthread_t tid;
           pthread_create(&tid, NULL, handle_client_thread, &socket_pool[i]);
        */

        // Current single-threaded handling
        handle_client(sock_conn, &db);
    }

    // Clean up
    db_close(&db);
    close(sock_listen);

    return 0;
}
