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
        if (db_authenticate_user(db, req.username, pass_hash, &user_id) == 0) {
            res.code = RES_SUCCESS;
            sprintf(res.message, "Login successful. Welcome ID %d", user_id);
        } else {
            res.code = RES_ERR_INVALID_CREDENTIALS;
            strcpy(res.message, "Invalid credentials");
        }
        write(sock_conn, &res, sizeof(res));
    }
    
    /* 
       FUTURE EXPANSION - GAME FUNCTIONS:
       
       - REQ_JOIN_ROOM:
         - Check room capacity in `rooms` table.
         - If space available, add user to `match_participants`.
         - Update `current_players` in `rooms`.
       
       - REQ_MATCH_EVENT (e.g., player move):
         - Validate move logic.
         - Insert event into `match_events` table.
         - Broadcast move to other participants (requires keeping connections open).
       
       - REQ_SEND_CHAT:
         - Insert message into `chat_messages` table.
         - Broadcast message to room/match members.
       
       - REQ_GET_MATCH_HISTORY:
         - Query `matches` and `match_participants` for user ID.
         - Return results as a list of packets.

       - MULTI-CLIENT HANDLING:
         - Currently, we close after one transaction.
         - For a real-time game, we would enter a 'game loop' here or use 
           IO multiplexing (select/poll/epoll) in main.
    */

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
