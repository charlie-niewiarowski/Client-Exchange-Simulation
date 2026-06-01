//
// Created by charl on 1/13/2026.
//

#ifndef ORDER_GATEWAY_MESSAGE_TYPES_HPP
#define ORDER_GATEWAY_MESSAGE_TYPES_HPP

#include <unordered_map>
#include <vector>
#include <memory>

#include "order_types.h"

enum class MessageType : uint8_t { NEW = 0, CANCEL = 1, MODIFY = 2, MATCH = 4};

struct InboundMessage { // gateway -> engine
    Timestamp recv_tsc;       // 8 bytes  @ 0
    Timestamp server_push;    // 8 bytes  @ 8

    OrderId order_id;         // 8 bytes  @ 16  |  junk value for NEW
    ClientId client_id;       // 4 bytes  @ 24
    Price price;              // 4 bytes  @ 28
    Quantity quantity;        // 4 bytes  @ 32
    MessageType message_type; // 1 byte   @ 33
    Side side;                // 1 byte   @ 34
    OrderType order_type;     // 1 byte   @ 35
    uint8_t padding[4];       // 4 bytes  @ 36
}; // 40 bytes (sizeof verified by compiler)

enum class Status : uint8_t { FAILURE = 0, SUCCESS = 1};
enum class ServerError : uint8_t { NONE = 0, MALFORMED_REQUEST, INVALID_ORDER, SYSTEM_ERROR, EXECUTION_ERROR };

struct OutboundMessage { // engine -> gateway
    Timestamp recv_tsc;        // 8 bytes  @ 0
    Timestamp server_push_tsc; // 8 bytes  @ 8
    Timestamp engine_pop_tsc;  // 8 bytes  @ 16
    Timestamp engine_push_tsc; // 8 bytes  @ 24

    OrderId order_id;          // 8 bytes  @ 32
    ClientId client_id;        // 4 bytes  @ 40
    MessageType message_type;  // 1 byte   @ 44
    Status status;             // 1 byte   @ 45
    ServerError server_error;  // 1 byte   @ 46
    uint8_t padding[1];        // 1 byte   @ 47
}; // 48 bytes (sizeof verified by compiler)

using ClientMessageMap = std::unordered_map<ClientId, std::vector<OutboundMessage>>;

#endif //ORDER_GATEWAY_MESSAGE_TYPES_HPP