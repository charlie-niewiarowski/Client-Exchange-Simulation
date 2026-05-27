//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "communication_types.h"
#include "protocol.h"
#include "../config/config.h"

#include <random>

struct ClientState {
    // ── Identity ──────────────────────────────────────────────────────────────
    uint32_t client_id  = 0;
    int      fd         = -1;
    bool     connecting = false;  // true while non-blocking connect() is pending

    // ── Write buffer ──────────────────────────────────────────────────────────
    // One batch = PIPELINE_DEPTH frames packed consecutively.
    // write_off tracks bytes already handed to the kernel.
    char   write_buf[PIPELINE_DEPTH * INBOUND_BSIZE]{};
    size_t write_len = 0;
    size_t write_off = 0;

    // ── Read buffer ───────────────────────────────────────────────────────────
    // Sized for a full pipeline batch plus a few unsolicited MATCH notifications.
    // Data always lives at [0, read_len); advance_read() memmoves the remainder
    // left — cheap for the small sizes involved.
    char   read_buf[PIPELINE_DEPTH * 2 * OUTBOUND_BSIZE]{};
    size_t read_len = 0;

    // ── Pipeline ──────────────────────────────────────────────────────────────
    uint32_t acks_pending = 0;  // frames sent but not yet responded to

    // ── Stats ─────────────────────────────────────────────────────────────────
    uint64_t requests_sent    = 0;
    uint64_t responses_ack    = 0;
    uint64_t responses_match  = 0;
    uint64_t responses_err    = 0;
    int64_t  orders_in_flight = 0;

    // ── Per-client RNG ────────────────────────────────────────────────────────
    std::mt19937 rng;
};

#endif // CLIENT_STATE_H
