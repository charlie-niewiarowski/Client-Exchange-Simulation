//
// Created by cniew on 5/16/26.
//

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "communication_types.h"

#include <algorithm>
#include <cstring>
#include <string_view>

//=============================================================================
// Wire frame sizes
//=============================================================================

// Inbound frame: "EXCHANGE\n"(9) + InboundMessage(48) + '\n'(1) = 58 bytes + padding for cache alignment.
inline constexpr size_t INBOUND_BSIZE = 64;

// Outbound frame: fixed 64 bytes (cache-line aligned).
//   OK    -> "EXCHANGE\nOK\n"(12) + ClientId(4) + OrderId(8)          = 24 bytes
//   ERROR -> "EXCHANGE\nERROR\n"(15) + longest_error_str(17) + '\n'(1) = 33 bytes
// Both fit comfortably; the remaining bytes are zero-padded by the sender.
inline constexpr size_t OUTBOUND_BSIZE = 32;

//=============================================================================
// Status prefixes (server -> client)
//=============================================================================

#define OK_STATUS    "EXCHANGE\nOK\n"
#define ERROR_STATUS "EXCHANGE\nERROR\n"

//=============================================================================
// Per-connection outbound ring capacity (must be a power of two)
//=============================================================================

inline constexpr size_t RINGBUF_SIZE = 64;

//=============================================================================
// Error string helpers
//=============================================================================

constexpr std::string_view server_error_string(const ServerError error) {
    switch (error) {
        case ServerError::MALFORMED_REQUEST: return "malformed req";
        case ServerError::INVALID_ORDER:     return "invalid order";
        case ServerError::SYSTEM_ERROR:      return "system error";
        case ServerError::EXECUTION_ERROR:   return "execution error";
        default:                             return "null";
    }
}

#endif //PROTOCOL_H
