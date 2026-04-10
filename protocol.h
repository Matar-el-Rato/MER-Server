#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_USERNAME 12
#define MAX_PASSWORD 12
#define MAX_CLIENTS  64

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
    REQ_CONNECT_LIVE = 9  // Open a persistent connection for server-push notifications
} request_type_t;

// Push Message Types (server -> client, unsolicited)
typedef enum {
    MSG_USER_LIST = 10  // Broadcast: full list of currently connected live clients
} push_msg_type_t;

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

// Entry in the server's live connected-client table (internal use, not transmitted).
typedef struct {
    int  socket_fd;
    char username[MAX_USERNAME];
    int  user_id;
} connected_client_t;

#endif // PROTOCOL_H
