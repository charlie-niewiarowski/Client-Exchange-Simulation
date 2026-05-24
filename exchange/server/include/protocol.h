//
// Created by cniew on 5/16/26.
//

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "../../include/communication_types.h"

#include <cstring>

inline constexpr size_t INBOUND_BSIZE  = strlen("EXCHANGE\n") + sizeof(InboundMessage) + 1; // +1 for \n
inline constexpr size_t OUTBOUND_BSIZE = std::max<size_t>(64, 13 + sizeof(OutboundMessage));
inline constexpr size_t BUFFER_SIZE    = std::max(INBOUND_BSIZE, OUTBOUND_BSIZE);

// status
#define OK_STATUS "EXCHANGE\nOK\n"
#define ERROR_STATUS "EXCHANGE\nERROR\n"

// Framing overhead for an error response: "EXCHANGE\nERROR\n" + trailing "\n"
inline constexpr size_t ERROR_FRAME_OVERHEAD = sizeof("EXCHANGE\nERROR\n") - 1 + 1;
inline constexpr size_t MAX_ERROR_MSG_LEN    = sizeof(OutboundMessage) - 1;

inline constexpr size_t RINGBUF_SIZE = 16;

// READING must be 0 so a default-constructed ConnectionInfo starts here.
enum class ConnectionState : uint8_t {
    READING = 0, PROCESSING, SERIALIZING, SENDING
};

constexpr std::string_view server_error_string(const ServerError error) {
    switch (error) {
        case ServerError::MALFORMED_REQUEST:
            return "malformed request";
        case ServerError::INVALID_ORDER:
            return "invalid order";
        case ServerError::SYSTEM_ERROR:
            return "system error";
        case ServerError::EXECUTION_ERROR:
            return "execution error";
        default:
            return "null";
    }
}

#endif //PROTOCOL_H
