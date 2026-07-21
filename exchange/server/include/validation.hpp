//
// Validation logic extracted from server.cpp so it can be unit-tested directly.
//

#ifndef VALIDATION_H
#define VALIDATION_H

#include "communication_types.hpp"

inline bool validate_new(const InboundMessage &msg) {
    auto raw_type = static_cast<uint8_t>(msg.message_type);
    if (raw_type != 0) return false;

    auto raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    auto raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.order_type == OrderType::LIMIT && msg.price == 0) return false;

    if (msg.quantity > 1'000'000u) return false;

    return true;
}

inline bool validate_cancel(const InboundMessage &msg) {
    auto raw_type = static_cast<uint8_t>(msg.message_type);
    if (raw_type != 1) return false;
    return true;
}

inline bool validate_modify(const InboundMessage &msg) {
    auto raw_type = static_cast<uint8_t>(msg.message_type);
    if (raw_type != 2) return false;

    auto raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    auto raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.order_type == OrderType::LIMIT && msg.price == 0) return false;

    if (msg.quantity > 1'000'000u) return false;

    return true;
}

inline bool validate_message(const InboundMessage &msg) {
    switch (msg.message_type) {
        case MessageType::NEW:    return validate_new(msg);
        case MessageType::CANCEL: return validate_cancel(msg);
        case MessageType::MODIFY: return validate_modify(msg);
        default:                  return false;
    }
}

#endif // VALIDATION_H