//
// Created by charl on 1/13/2026.
//

#ifndef ORDER_GATEWAY_MESSAGE_TYPES_HPP
#define ORDER_GATEWAY_MESSAGE_TYPES_HPP

#include <unordered_map>
#include <vector>

#include "order_types.hpp"
#include "../lib/RingBuffer.hpp"

enum class MessageType : uint8_t { NEW = 0, CANCEL = 1, MODIFY = 2};
using OrderId = uint32_t;
using ClientId = uint32_t;

struct InboundMessage { // gateway -> engine
    ClientId client_id;       // 8 bytes
    OrderId order_id;         // 4 bytes   |   junk value for NEW
    Price price;              // 4 bytes
    Quantity quantity;        // 4 bytes
    MessageType message_type; // 1 byte
    Side side;                // 1 byte
    OrderType order_type;     // 1 byte
    uint8_t padding;          // 1 byte
}; // 24 bytes

enum class Status : uint8_t { FAILURE = 0, SUCCESS = 1};

struct OutboundMessage { // engine -> gateway
    ClientId client_id;       // 8 bytes
    OrderId order_id;         // 4 bytes
    MessageType message_type; // 1 byte
    Status status;            // 1 byte
    uint8_t padding[2];       // 2 bytes
}; // 16 bytes

struct SharedMemoryHeader {
    uint32_t num_inbound_entries;
    uint32_t max_outbound_entries;
    uint32_t outbound_entries;
    uint16_t max_num_clients;
    uint16_t num_clients;
};

using ClientMessageMap = std::unordered_map<ClientId, std::vector<OutboundMessage>>;

class SharedMemory {
    SharedMemoryHeader header_;
    char data[0];
};

enum class SetupError { WSA_FAIL, SOCKET_FAIL, BIND_FAIL, LISTEN_FAIL, ALLOC_FAIL, MAP_FAIL };

#endif //ORDER_GATEWAY_MESSAGE_TYPES_HPP