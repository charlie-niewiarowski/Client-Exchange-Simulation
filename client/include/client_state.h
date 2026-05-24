//
// Created by cniew on 5/24/26.
//
// Per-connection state for one simulated client.
//
// Lifecycle
// ─────────
//  CONNECTING  non-blocking connect() issued; waiting for EPOLLOUT to confirm
//  SENDING     request frame partially written; waiting for EPOLLOUT to drain
//  AWAITING    full frame sent; waiting for EPOLLIN response from the exchange
//  RECONNECT   connection lost; reconnect_at_ns holds the next retry deadline
//

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "communication_types.h"
#include "protocol.h"
#include "order_factory.h"
#include "../config/config.h"

#include <array>
#include <cstdint>
#include <random>

enum class ClientPhase : uint8_t {
    CONNECTING,
    SENDING,
    AWAITING,
    RECONNECT,
};

struct ClientState {
    // ── Identity ──────────────────────────────────────────────────────────────
    uint32_t    client_id = 0;
    int         fd        = -1;
    ClientPhase phase     = ClientPhase::RECONNECT;

    // ── Write buffer (outbound request) ───────────────────────────────────────
    char   write_buf[INBOUND_BSIZE]{};
    size_t write_len = 0;
    size_t write_off = 0;

    // ── Read buffer (inbound responses) ───────────────────────────────────────
    // Sized to OUTBOUND_BSIZE (64 bytes).  Multiple responses can land in a
    // single recv() call (e.g., a MATCH notification arriving alongside the ACK
    // for the current request); the consumer must advance past each one rather
    // than clearing the whole buffer.
    char   read_buf[OUTBOUND_BSIZE]{};
    size_t read_len = 0;

    // ── Active order ID ring (CANCEL / MODIFY targeting) ──────────────────────
    std::array<OrderId, MAX_ACTIVE_ORDERS> active_orders{};
    uint32_t active_head = 0;   // next write slot (wraps)
    uint32_t active_size = 0;   // valid entries, capped at MAX_ACTIVE_ORDERS

    // ── Last-sent request metadata (for response log attribution) ─────────────
    OrderFactory::FrameKind last_kind  = OrderFactory::FrameKind::NEW;
    InboundMessage          last_msg{};
    // True from the moment a request is sent until its ACK is consumed.
    // Any OK frame received while this is false is an unsolicited MATCH.
    bool                    expect_ack = false;

    // ── Reconnection ──────────────────────────────────────────────────────────
    uint64_t reconnect_at_ns = 0;
    uint32_t reconnect_count = 0;

    // ── Statistics ────────────────────────────────────────────────────────────
    uint64_t requests_sent   = 0;
    uint64_t responses_ack   = 0;  // first OK after each sent request
    uint64_t responses_match = 0;  // unsolicited fill notifications
    uint64_t responses_err   = 0;
    int64_t  orders_in_flight = 0; // requests sent but not yet responded to

    // ── Per-client RNG ────────────────────────────────────────────────────────
    // Seeded independently from the orchestrator's RNG so every client
    // generates a different order stream with no shared mutable state.
    std::mt19937 rng;

    // ─────────────────────────────────────────────────────────────────────────
    // Active-order helpers
    // ─────────────────────────────────────────────────────────────────────────

    void record_active_order(const OrderId oid) {
        active_orders[active_head % MAX_ACTIVE_ORDERS] = oid;
        ++active_head;
        if (active_size < MAX_ACTIVE_ORDERS) ++active_size;
    }

    // Returns 0 if no live orders are tracked (CANCEL / MODIFY will use a
    // junk ID → engine returns Status::FAILURE, still useful for load testing).
    [[nodiscard]] OrderId pick_active_order() {
        if (active_size == 0) return 0;
        std::uniform_int_distribution<uint32_t> pick{0, active_size - 1};
        const uint32_t slot =
            (active_head - active_size + pick(rng)) % MAX_ACTIVE_ORDERS;
        return active_orders[slot];
    }
};

#endif // CLIENT_STATE_H
