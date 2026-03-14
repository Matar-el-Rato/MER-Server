#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_USERNAME 12
#define MAX_PASSWORD 12

// Request Types
typedef enum {
    REQ_REGISTER = 1,
    REQ_LOGIN    = 2,
    // Future game actions can be added here
    // REQ_JOIN_ROOM = 3,
    // REQ_MOVE      = 4,
} request_type_t;

// Response Codes
typedef enum {
    RES_SUCCESS = 0,
    RES_ERR_USER_EXISTS = 1,
    RES_ERR_INVALID_CREDENTIALS = 2,
    RES_ERR_DATABASE = 3,
    RES_ERR_UNKNOWN = 99
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

typedef struct {
    uint8_t code;
    char message[128];
} generic_res_t;

#endif // PROTOCOL_H
