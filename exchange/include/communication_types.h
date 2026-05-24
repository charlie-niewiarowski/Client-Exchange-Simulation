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
    ClientId client_id;       // 4 bytes
    OrderId order_id;         // 4 bytes   |   junk value for NEW
    Price price;              // 4 bytes
    Quantity quantity;        // 4 bytes
    MessageType message_type; // 1 byte
    Side side;                // 1 byte
    OrderType order_type;     // 1 byte
    uint8_t padding;          // 1 byte
}; // 20 bytes

enum class Status : uint8_t { FAILURE = 0, SUCCESS = 1};
enum class ServerError : uint8_t { NONE = 0, MALFORMED_REQUEST, INVALID_ORDER, SYSTEM_ERROR, EXECUTION_ERROR };

struct OutboundMessage { // engine -> gateway
    ClientId client_id;       // 4 bytes
    OrderId order_id;         // 4 bytes
    MessageType message_type; // 1 byte
    Status status;            // 1 byte
    ServerError server_error; // 1 byte
    uint8_t padding;          // 1 byte
}; // 12 bytes

using ClientMessageMap = std::unordered_map<ClientId, std::vector<OutboundMessage>>;

#endif //ORDER_GATEWAY_MESSAGE_TYPES_HPP