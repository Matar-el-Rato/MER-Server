#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <pthread.h>

#define MAX_USERNAME     12
#define MAX_PASSWORD     12
#define MAX_CLIENTS      64
#define MAX_CHAT_MESSAGE 100

// Request Types
typedef enum {
    REQ_REGISTER     = 1,
    REQ_LOGIN        = 2,
    REQ_CHANGE_SKIN  = 3, // Update user skin ID
    REQ_LIST_ROOMS   = 4, // Future: Get list of active rooms
    REQ_JOIN_ROOM    = 5, // Future: Join a specific room
    REQ_SEND_CHAT    = 6, // Future: Send message (Lobby or Room)
    REQ_GAME_ACTION  = 7, // Future: Perform game move
    REQ_LEAVE_ROOM   = 8, // Future: Exit a room back to Lobby
    REQ_CONNECT_LIVE = 9, // Open a persistent connection for server-push notifications
    REQ_LOGOUT       = 13 // Graceful logout: server removes client and closes connection
} request_type_t;

// Push Message Types (server -> client, unsolicited)
typedef enum {
    MSG_USER_LIST  = 10, // Broadcast: full list of currently connected live clients
    MSG_CHAT       = 11, // Broadcast: chat message from a player (scoped to sender's room)
    MSG_ROOM_STATE = 12  // Broadcast: current occupancy of one room (sent to all live clients)
} push_msg_type_t;

#define NUM_ROOMS        3
#define MAX_ROOM_PLAYERS 4

// Response Codes
typedef enum {
    RES_SUCCESS                = 0,
    RES_ERR_USER_EXISTS        = 1,
    RES_ERR_INVALID_CREDENTIALS = 2,
    RES_ERR_DATABASE           = 3,
    RES_ERR_INVALID_INPUT      = 4,
    RES_ERR_UNKNOWN            = 99
} response_code_t;

// Packet structures (using fixed sizes for simplicity in this version)

typedef struct {
    uint8_t type;
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
} register_req_t;

typedef struct {
    uint8_t type;
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
} login_req_t;

// packed: no padding after uint8_t, so sizeof = 9 and matches the C# client's 9-byte packet layout
typedef struct __attribute__((packed)) {
    uint8_t type;
    int user_id;
    int skin_id;
} change_skin_req_t;

typedef struct {
    uint8_t code;
    char message[128];
} generic_res_t;

// Sent by the client after a successful login to open a persistent connection.
// packed: prevents 3-byte padding that gcc would insert between username[12] and the
// uint32_t field, so sizeof = 17, matching the C# client's layout (1 + 12 + 4).
// user_id is transmitted in network byte order (big-endian); use ntohl() on receipt.
typedef struct __attribute__((packed)) {
    uint8_t  type;                   // REQ_CONNECT_LIVE
    char     username[MAX_USERNAME]; // zero-padded, not necessarily null-terminated
    uint32_t user_id;                // network byte order - call ntohl() before use
} connect_live_req_t;

// Pushed by the server to ALL live clients whenever the connected-user list changes.
// packed: prevents any padding between count and users[].
// Maximum wire size: 1 + 1 + 64*12 = 770 bytes.
typedef struct __attribute__((packed)) {
    uint8_t type;                            // MSG_USER_LIST
    uint8_t count;                           // number of entries in users[]
    char    users[MAX_CLIENTS][MAX_USERNAME]; // each entry zero-padded to MAX_USERNAME bytes
} user_list_msg_t;

// Sent by the client on the live connection to send a lobby chat message.
// packed: no padding after type byte. Wire size: 1 + 100 = 101 bytes.
typedef struct __attribute__((packed)) {
    uint8_t type;                       // REQ_SEND_CHAT
    char    message[MAX_CHAT_MESSAGE];  // null-terminated, max 100 chars
} chat_msg_req_t;

// Pushed by the server to ALL live clients when a player sends a message.
// packed: no padding. Wire size: 1 + 12 + 100 = 113 bytes.
typedef struct __attribute__((packed)) {
    uint8_t type;                       // MSG_CHAT
    char    username[MAX_USERNAME];     // sender username, zero-padded
    char    message[MAX_CHAT_MESSAGE];  // message text, zero-padded
} chat_broadcast_t;

// Pushed by the server to ALL live clients when a room's membership changes.
// packed: no padding. Wire size: 1 + 1 + 1 + 4*12 = 51 bytes.
typedef struct __attribute__((packed)) {
    uint8_t type;                                    // MSG_ROOM_STATE
    uint8_t room_id;                                 // 1-3
    uint8_t count;                                   // number of occupied slots
    char    players[MAX_ROOM_PLAYERS][MAX_USERNAME]; // zero-padded names
} room_state_msg_t;

// Sent by the client on the live connection to join a room.
// packed. Wire size: 2 bytes.
typedef struct __attribute__((packed)) {
    uint8_t type;    // REQ_JOIN_ROOM
    uint8_t room_id; // 1-3
} join_room_req_t;

// Entry in the server's live connected-client table (internal use, not transmitted).
typedef struct {
    int  socket_fd;
    char username[MAX_USERNAME];
    int  user_id;
    int  room_id; // 0 = lobby, 1-3 = room
} connected_client_t;

// List of currently connected live clients, protected by a mutex.
typedef struct {
    connected_client_t entries[MAX_CLIENTS];
    int                count;
    pthread_mutex_t    mutex;
} client_list_t;

#define CLIENT_LIST_INIT { .count = 0, .mutex = PTHREAD_MUTEX_INITIALIZER }

#endif // PROTOCOL_H
