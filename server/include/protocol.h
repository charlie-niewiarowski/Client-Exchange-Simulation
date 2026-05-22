//
// Created by cniew on 5/16/26.
//

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "../../include/communication_types.h"

inline constexpr size_t INBOUND_BSIZE  = 9  + sizeof(InboundMessage);
inline constexpr size_t OUTBOUND_BSIZE = std::max<size_t>(64, 13 + sizeof(OutboundMessage));
inline constexpr size_t BUFFER_SIZE    = std::max(INBOUND_BSIZE, OUTBOUND_BSIZE);

// Framing overhead for an error response: "EXCHANGE\nERROR\n" + trailing "\n"
inline constexpr size_t ERROR_FRAME_OVERHEAD = sizeof("EXCHANGE\nERROR\n") - 1 + 1;
inline constexpr size_t MAX_ERROR_MSG_LEN    = BUFFER_SIZE - ERROR_FRAME_OVERHEAD;

inline constexpr size_t RINGBUF_SIZE = 16;

// READING must be 0 so a default-constructed ConnectionInfo starts here.
enum class ConnectionState : uint8_t {
    READING = 0, PARSING, EXECUTING, SERIALIZING, SENDING
};

inline constexpr char err_malformed_request[] = "malformed request\n";
inline constexpr char err_invalid_order[]     = "invalid order\n";
inline constexpr char err_order_failed[]      = "order failed\n";
inline constexpr char err_system_error[]      = "system error\n";
inline constexpr char err_execution_error[]   = "execution error\n";

#endif //PROTOCOL_H
