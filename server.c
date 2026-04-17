#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>   // ntohl() - convert network byte order to host order
#include <pthread.h>     // pthreads for live-connection threads
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <crypt.h>
#include <time.h>
#include "protocol.h"
#include "db.h"

#define SALT "$6$matar_el_rato$" // SHA-512 Salt

#define DEFAULT_PORT 8888

/* Writes "[HH:MM:SS] " followed by msg to stdout.
 * Used for all server log lines so every entry carries an absolute timestamp. */
static void tlog(const char *msg) {
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    char       ts[16];
    strftime(ts, sizeof(ts), "[%H:%M:%S] ", tm);
    write(1, ts,  strlen(ts));
    write(1, msg, strlen(msg));
}

static int sock_listen_fd = -1;
static db_t *db_ptr = NULL;

/* =============================================================
 * SHARED STATE: connected live clients
 * =============================================================
 * g_live is accessed by multiple pthreads simultaneously:
 *   - The main accept() thread calls add_client() for every new
 *     REQ_CONNECT_LIVE connection.
 *   - Each handle_live_connection() thread calls remove_client()
 *     when its client disconnects.
 *   - Both paths call broadcast_user_list(), which iterates the
 *     entire list and writes to every client's socket.
 *
 * WHY MUTUAL EXCLUSION IS NEEDED:
 * Without the mutex, two threads doing concurrent swap-removals
 * can both read the same count, write to the same slot,
 * and decrement twice — leaving the list corrupted and the count
 * wrong. Similarly, a broadcast iterating entries[0..count-1]
 * while another thread is removing an entry mid-loop could read a
 * stale socket_fd that has already been closed (and potentially
 * reused by the OS for a different connection).
 *
 * ALL reads and writes to g_live MUST hold g_live.mutex.
 * No exceptions.
 * ============================================================= */
static client_list_t g_live = CLIENT_LIST_INIT;

static void handle_shutdown(int sig) {
    (void)sig;
    tlog("\nShutting down...\n");
    if (sock_listen_fd != -1) close(sock_listen_fd);
    if (db_ptr != NULL) db_close(db_ptr);
    exit(0);
}

void error(const char *msg) {
    perror(msg);
    exit(1);
}

/* =============================================================
 * broadcast_user_list
 *
 * PRECONDITION: g_live.mutex MUST already be held by the caller.
 * This function does NOT lock/unlock by itself.
 *
 * WHY THE LOCK MUST BE HELD FOR THE ENTIRE SEND LOOP:
 * If we released the mutex after building the message and then
 * looped over socket fds, remove_client() could run on another
 * thread between two send() calls. That thread would call
 * close(fd) on the departing client's socket. If the OS reuses
 * that fd number for a brand-new unrelated connection before our
 * send() reaches it, we would write a stale user-list packet into
 * an innocent socket. Holding the lock for the full loop prevents
 * close(fd) — and therefore OS fd reuse — from happening while
 * we are mid-broadcast.
 *
 * SEND FAILURE HANDLING:
 * If send() fails for one client (socket already broken), we log
 * and continue. A failed send must not abort the broadcast for
 * other healthy clients. The dead connection will call
 * remove_client() itself when its recv() loop wakes up.
 * We MUST NOT call remove_client() from here — we already hold
 * the mutex and would deadlock.
 * ============================================================= */
static void broadcast_user_list(void) {
    user_list_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type  = MSG_USER_LIST;
    msg.count = (uint8_t)g_live.count;

    for (int i = 0; i < g_live.count; i++) {
        strncpy(msg.users[i], g_live.entries[i].username, MAX_USERNAME - 1);
    }

    size_t send_size = sizeof(uint8_t) * 2 + (size_t)g_live.count * MAX_USERNAME;

    char list_log[160];
    snprintf(list_log, sizeof(list_log),
        "[broadcast] sending user list (%d players) to %d client(s)\n",
        g_live.count, g_live.count);
    tlog(list_log);

    for (int i = 0; i < g_live.count; i++) {
        ssize_t sent = send(g_live.entries[i].socket_fd, &msg, send_size, MSG_NOSIGNAL);
        if (sent < 0) {
            char log[160];
            snprintf(log, sizeof(log),
                "[broadcast] send() failed for '%s' (fd=%d): %s"
                " - will be cleaned up on disconnect\n",
                g_live.entries[i].username, g_live.entries[i].socket_fd, strerror(errno));
            tlog(log);
        }
    }
}

/* =============================================================
 * broadcast_chat
 *
 * Pushes a MSG_CHAT packet to all currently connected live clients.
 * Acquires g_live.mutex internally — caller must NOT already hold it.
 *
 * WHY THIS ACQUIRES ITS OWN LOCK (unlike broadcast_user_list):
 * broadcast_user_list is called by add_client/remove_client which
 * already hold the mutex, so it must not re-acquire it. broadcast_chat
 * is called from handle_live_connection (the per-client recv loop),
 * which holds no lock — so this function acquires it itself.
 * ============================================================= */
/* =============================================================
 * broadcast_room_state
 *
 * PRECONDITION: g_live.mutex MUST already be held by the caller.
 * Sends MSG_ROOM_STATE for the given room_id to ALL live clients.
 * ============================================================= */
static void broadcast_room_state(int room_id) {
    room_state_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type    = MSG_ROOM_STATE;
    msg.room_id = (uint8_t)room_id;
    msg.count   = 0;

    for (int i = 0; i < g_live.count; i++) {
        if (g_live.entries[i].room_id == room_id && msg.count < MAX_ROOM_PLAYERS) {
            strncpy(msg.players[msg.count], g_live.entries[i].username, MAX_USERNAME - 1);
            msg.count++;
        }
    }

    char log[128];
    snprintf(log, sizeof(log), "[room] broadcasting room %d state (%d player(s))\n",
        room_id, (int)msg.count);
    tlog(log);

    for (int i = 0; i < g_live.count; i++) {
        ssize_t sent = send(g_live.entries[i].socket_fd, &msg, sizeof(msg), MSG_NOSIGNAL);
        if (sent < 0) {
            char elog[128];
            snprintf(elog, sizeof(elog), "[room] send() failed for '%s': %s\n",
                g_live.entries[i].username, strerror(errno));
            tlog(elog);
        }
    }
}

/* =============================================================
 * broadcast_chat_from_fd
 *
 * Routes a MSG_CHAT packet only to clients in the same room as
 * the sender (0 = lobby). Acquires g_live.mutex internally.
 * ============================================================= */
static void broadcast_chat(const char *username, const char *message) {
    /* caller passes this only for the old lobby-only path; replaced below */
    (void)username; (void)message;
}

static void broadcast_chat_from_fd(int sender_fd, const char *username, const char *message) {
    chat_broadcast_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CHAT;
    strncpy(msg.username, username, MAX_USERNAME - 1);
    strncpy(msg.message,  message,  MAX_CHAT_MESSAGE - 1);

    pthread_mutex_lock(&g_live.mutex);

    /* Find sender's room (0 = lobby). */
    int sender_room = 0;
    for (int i = 0; i < g_live.count; i++) {
        if (g_live.entries[i].socket_fd == sender_fd) {
            sender_room = g_live.entries[i].room_id;
            break;
        }
    }

    char chat_log[256];
    snprintf(chat_log, sizeof(chat_log),
        "[chat] room=%d '%s' from '%s'\n", sender_room, message, username);
    tlog(chat_log);

    /* Route: send only to clients in the same room (lobby→lobby, room→room). */
    for (int i = 0; i < g_live.count; i++) {
        if (g_live.entries[i].room_id != sender_room) continue;
        ssize_t sent = send(g_live.entries[i].socket_fd, &msg, sizeof(msg), MSG_NOSIGNAL);
        if (sent < 0) {
            char log[160];
            snprintf(log, sizeof(log),
                "[chat] send() failed for '%s' (fd=%d): %s\n",
                g_live.entries[i].username, g_live.entries[i].socket_fd, strerror(errno));
            tlog(log);
        }
    }
    pthread_mutex_unlock(&g_live.mutex);
}

/* =============================================================
 * add_client
 *
 * Adds a new live client to g_live and triggers a broadcast so
 * all clients receive the updated list immediately.
 *
 * MUTUAL EXCLUSION: acquires g_live.mutex.
 * The insert and the broadcast are performed under the same lock
 * so no other thread can observe a state where the new client
 * is missing from a broadcast that should include it.
 *
 * RISK - server at capacity:
 * If MAX_CLIENTS is reached, the new client is rejected cleanly.
 * Its socket is closed by the caller (handle_live_connection).
 * ============================================================= */
static int add_client(int fd, const char *username, int user_id) {
    pthread_mutex_lock(&g_live.mutex);

    if (g_live.count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&g_live.mutex);
        char log[128];
        snprintf(log, sizeof(log),
            "[add_client] MAX_CLIENTS (%d) reached, rejecting fd=%d user='%s'\n",
            MAX_CLIENTS, fd, username);
        tlog(log);
        return -1;
    }

    g_live.entries[g_live.count].socket_fd = fd;
    strncpy(g_live.entries[g_live.count].username, username, MAX_USERNAME - 1);
    g_live.entries[g_live.count].username[MAX_USERNAME - 1] = '\0';
    g_live.entries[g_live.count].user_id  = user_id;
    g_live.entries[g_live.count].room_id  = 0;
    g_live.count++;

    char log[128];
    snprintf(log, sizeof(log),
        "[connect] '%s' (id=%d) joined. Active clients: %d\n",
        username, user_id, g_live.count);
    tlog(log);

    broadcast_user_list(); /* called with mutex already held */

    pthread_mutex_unlock(&g_live.mutex);
    return 0;
}

/* =============================================================
 * remove_client
 *
 * Removes a client by socket fd using O(1) swap-removal, then
 * broadcasts the updated list to all remaining clients.
 *
 * MUTUAL EXCLUSION: acquires g_live.mutex.
 * Same reasoning as add_client: the removal and the broadcast
 * must be atomic from other threads' perspective, so no thread
 * can receive a list that still contains a client we just removed.
 * ============================================================= */
static void remove_client(int fd) {
    pthread_mutex_lock(&g_live.mutex);

    for (int i = 0; i < g_live.count; i++) {
        if (g_live.entries[i].socket_fd == fd) {
            int was_in_room = g_live.entries[i].room_id;

            char log[128];
            snprintf(log, sizeof(log),
                "[disconnect] '%s' left. Active clients: %d\n",
                g_live.entries[i].username, g_live.count - 1);
            tlog(log);

            /* O(1) swap-removal */
            g_live.entries[i] = g_live.entries[g_live.count - 1];
            g_live.count--;

            broadcast_user_list(); /* called with mutex already held */
            if (was_in_room != 0)
                broadcast_room_state(was_in_room); /* notify all that room changed */
            break;
        }
    }

    pthread_mutex_unlock(&g_live.mutex);
}

/* =============================================================
 * handle_live_connection  (pthread entry point)
 *
 * Reads the REQ_CONNECT_LIVE handshake (the type byte was already
 * consumed by the dispatch loop in main before this thread was
 * spawned), registers the client, then blocks in a recv() loop
 * until the client disconnects. On exit it unregisters and closes.
 *
 * PARTIAL READ RISK:
 * TCP is a stream protocol. A single send() on the client may
 * arrive as multiple recv() calls on the server. We therefore
 * read the handshake payload in a loop until all expected bytes
 * have arrived, rather than assuming one recv() = one packet.
 *
 * ENDIANNESS:
 * user_id is transmitted in network byte order (big-endian) by
 * the C# client (IPAddress.HostToNetworkOrder). We call ntohl()
 * to convert to host byte order before storing.
 *
 * HEARTBEAT LIMITATION (known, accepted for this version):
 * If the remote host disappears without sending a TCP FIN (e.g.
 * power loss, abrupt cable pull), recv() will block indefinitely
 * until the OS TCP keep-alive timeout fires, which is disabled by
 * default and can be ~2 hours. For production, enable SO_KEEPALIVE
 * on the socket or implement an application-level ping/pong.
 * ============================================================= */
static void *handle_live_connection(void *arg) {
    int fd = (int)(intptr_t)arg;

    /* Read the rest of connect_live_req_t in a loop.
     * The type byte (1 byte) was already consumed in main(). */
    connect_live_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = REQ_CONNECT_LIVE; /* already consumed by caller */

    size_t  to_read  = sizeof(connect_live_req_t) - sizeof(uint8_t);
    size_t  received = 0;
    char   *buf      = (char *)&req + sizeof(uint8_t);

    while (received < to_read) {
        ssize_t n = recv(fd, buf + received, to_read - received, 0);
        if (n <= 0) {
            /* Client disconnected or errored before handshake completed */
            close(fd);
            return NULL;
        }
        received += (size_t)n;
    }

    req.username[MAX_USERNAME - 1] = '\0'; /* guarantee null termination */

    /* Convert user_id from network byte order to host byte order */
    int user_id = (int)ntohl(req.user_id);

    if (add_client(fd, req.username, user_id) != 0) {
        /* Server was at capacity — reject and close */
        close(fd);
        return NULL;
    }

    /* Message loop: read and dispatch incoming packets from this client.
     * Each iteration reads one type byte then the fixed-size payload for
     * that packet type. Unknown types are logged and skipped (zero payload
     * assumed — add an explicit drain if new variable-length types are added). */
    uint8_t msg_type;
    while (1) {
        ssize_t n = recv(fd, &msg_type, 1, 0);
        if (n <= 0) break;

        if (msg_type == REQ_SEND_CHAT) {
            char message[MAX_CHAT_MESSAGE];
            memset(message, 0, sizeof(message));

            size_t received = 0;
            int    ok       = 1;
            while (received < MAX_CHAT_MESSAGE) {
                ssize_t r = recv(fd, message + received, MAX_CHAT_MESSAGE - received, 0);
                if (r <= 0) { ok = 0; break; }
                received += (size_t)r;
            }
            if (!ok) break;

            message[MAX_CHAT_MESSAGE - 1] = '\0';
            for (int i = 0; message[i] != '\0' && i < MAX_CHAT_MESSAGE - 1; i++) {
                if (!isprint((unsigned char)message[i]))
                    message[i] = ' ';
            }

            /* Route chat only to clients in the same room (or lobby). */
            broadcast_chat_from_fd(fd, req.username, message);

        } else if (msg_type == REQ_JOIN_ROOM) {
            uint8_t room_id = 0;
            ssize_t r = recv(fd, &room_id, 1, 0);
            if (r <= 0) break;

            if (room_id < 1 || room_id > NUM_ROOMS) continue;

            pthread_mutex_lock(&g_live.mutex);

            int client_idx = -1;
            int room_count = 0;
            for (int i = 0; i < g_live.count; i++) {
                if (g_live.entries[i].socket_fd == fd)         client_idx = i;
                if (g_live.entries[i].room_id   == (int)room_id) room_count++;
            }

            if (client_idx == -1) {
                pthread_mutex_unlock(&g_live.mutex);
                break;
            }

            if (room_count < MAX_ROOM_PLAYERS) {
                int old_room = g_live.entries[client_idx].room_id;
                g_live.entries[client_idx].room_id = (int)room_id;

                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg),
                    "[room] '%s' joined room %d\n", req.username, (int)room_id);
                tlog(log_msg);

                if (old_room != 0 && old_room != (int)room_id)
                    broadcast_room_state(old_room);
                broadcast_room_state((int)room_id);
            }
            pthread_mutex_unlock(&g_live.mutex);

        } else if (msg_type == REQ_LEAVE_ROOM) {
            pthread_mutex_lock(&g_live.mutex);

            for (int i = 0; i < g_live.count; i++) {
                if (g_live.entries[i].socket_fd == fd && g_live.entries[i].room_id != 0) {
                    int old_room = g_live.entries[i].room_id;
                    g_live.entries[i].room_id = 0;

                    char log_msg[128];
                    snprintf(log_msg, sizeof(log_msg),
                        "[room] '%s' left room %d\n", req.username, old_room);
                    tlog(log_msg);

                    broadcast_room_state(old_room);
                    break;
                }
            }
            pthread_mutex_unlock(&g_live.mutex);

        } else {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg),
                "[live] unknown msg_type=%d from '%s', ignoring\n",
                (int)msg_type, req.username);
            tlog(log_msg);
        }
    }

    remove_client(fd);
    close(fd);
    return NULL;
}

// Function to handle a single client (stateless request/response)
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

        // Ensure null-termination regardless of what the client sent
        req.username[MAX_USERNAME - 1] = '\0';
        req.password[MAX_PASSWORD - 1] = '\0';

        generic_res_t res;
        memset(&res, 0, sizeof(res));

        // Validate username: non-empty and only printable characters
        int ulen = strlen(req.username);
        int valid = (ulen > 0 && strlen(req.password) > 0);
        for (int i = 0; i < ulen && valid; i++) {
            if (!isprint((unsigned char)req.username[i]))
                valid = 0;
        }

        if (!valid) {
            res.code = RES_ERR_INVALID_INPUT;
            strcpy(res.message, "Invalid username or password");
            write(sock_conn, &res, sizeof(res));
            close(sock_conn);
            return;
        }

        char log_msg[256];
        sprintf(log_msg, "Register request for user: %s\n", req.username);
        tlog(log_msg);

        // Hash the password before storing
        char *pass_hash = crypt(req.password, SALT);

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
        tlog(log_msg);

        // Hash the incoming password with the same salt to compare
        char *pass_hash = crypt(req.password, SALT);

        int user_id, skin_id;
        generic_res_t res;
        memset(&res, 0, sizeof(res));
        if (db_authenticate_user(db, req.username, pass_hash, &user_id, &skin_id) == 0) {
            res.code = RES_SUCCESS;
            sprintf(res.message, "Login successful. Welcome ID %d SKIN %d", user_id, skin_id);
            char skin_log[128];
            snprintf(skin_log, sizeof(skin_log), "[login] user_id=%d skin_id=%d\n", user_id, skin_id);
            tlog(skin_log);
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
        tlog(log_msg);

        generic_res_t res;
        memset(&res, 0, sizeof(res));
        if (db_update_skin(db, req.user_id, req.skin_id) == 0) {
            unsigned long long affected = mysql_affected_rows(db->conn);
            char ok_log[128];
            snprintf(ok_log, sizeof(ok_log),
                "[skin] updated user_id=%d skin_id=%d (rows affected: %llu)\n",
                req.user_id, req.skin_id, affected);
            tlog(ok_log);
            res.code = RES_SUCCESS;
            strcpy(res.message, "Skin updated successfully");
        } else {
            char err_log[128];
            snprintf(err_log, sizeof(err_log),
                "[skin] db_update_skin FAILED for user_id=%d\n", req.user_id);
            tlog(err_log);
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
        /* Lobby chat is handled on the persistent live connection (handle_live_connection),
         * not through the stateless request path.
         *
         * TODO: Per-room chat — when rooms exist, consider whether room messages
         * should also travel over the live connection or use this stateless path. */

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

    signal(SIGINT,  handle_shutdown);
    signal(SIGTERM, handle_shutdown);

    /* Ignore SIGPIPE globally.
     * When a live client drops its connection, any subsequent write/send
     * to its socket would raise SIGPIPE, which terminates the process by
     * default. We use MSG_NOSIGNAL in broadcast_user_list() as a per-call
     * guard, but this global ignore is belt-and-suspenders for any other
     * write paths that may be added in the future. */
    signal(SIGPIPE, SIG_IGN);

    // Initialize Database
    if (!db_init(&db, "localhost", "admin", "admin", "matarelrato-db")) {
        fprintf(stderr, "Failed to connect to database\n");
        exit(1);
    }
    tlog("Connected to database successfully.\n");
    db_ptr = &db;

    // 1. Create Socket (System Call)
    if ((sock_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        error("Error creant socket");
    sock_listen_fd = sock_listen;

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
    tlog(start_msg);

    for (;;) {
        // 4. Accept (System Call)
        sock_conn = accept(sock_listen, NULL, NULL);
        if (sock_conn < 0) {
            if (errno == EINTR) continue;
            perror("Error in accept");
            continue;
        }

        /* Peek at the first byte to determine the request type without
         * consuming it. This lets handle_client() read the type byte
         * itself as before (no change to stateless path). Only the live
         * connection branch explicitly consumes the byte before handing
         * the socket off to the new thread. */
        uint8_t req_type;
        ssize_t peeked = recv(sock_conn, &req_type, sizeof(req_type), MSG_PEEK);
        if (peeked <= 0) {
            close(sock_conn);
            continue;
        }

        if (req_type == REQ_CONNECT_LIVE) {
            /* Consume the type byte — handle_live_connection expects it gone */
            recv(sock_conn, &req_type, sizeof(req_type), 0);

            pthread_t tid;
            int rc = pthread_create(&tid, NULL, handle_live_connection,
                                    (void *)(intptr_t)sock_conn);
            if (rc != 0) {
                /* RISK: pthread_create failure.
                 * We must close the socket here to avoid a file descriptor
                 * leak. The client will receive a TCP RST and can retry. */
                char log[128];
                snprintf(log, sizeof(log),
                    "[error] pthread_create failed for fd=%d: %s\n",
                    sock_conn, strerror(rc));
                tlog(log);
                close(sock_conn);
                continue;
            }

            /* Detach the thread so it frees its own resources on exit.
             * We never pthread_join() live-connection threads. */
            pthread_detach(tid);

        } else {
            /* Stateless request: handle synchronously on this thread.
             * handle_client() will read the type byte itself via read().
             * The socket is closed inside handle_client() after the response. */
            handle_client(sock_conn, &db);
        }
    }

    // Clean up
    db_close(&db);
    close(sock_listen);

    return 0;
}
