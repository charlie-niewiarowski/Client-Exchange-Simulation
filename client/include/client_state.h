//
// Created by cniew on 5/24/26.
//
// Per-connection state for one simulated client.
//
// Lifecycle
// ─────────
//  CONNECTING  non-blocking connect() issued; waiting for EPOLLOUT to confirm
//  SENDING     batch frame partially written; waiting for EPOLLOUT to drain
//  AWAITING    pipeline has in-flight requests; waiting for EPOLLIN responses
//              or, when send_pending==true: waiting for the closed-loop rate
//              limiter to allow the next batch
//  RECONNECT   connection lost; reconnect_at_ns holds the next retry deadline
//
// Pipelining
// ──────────
// Each connection keeps up to PIPELINE_DEPTH requests in-flight simultaneously.
// buildBatch() packs PIPELINE_DEPTH frames into write_buf and sends them with
// a single send() call.  As responses arrive, consumeResponse() pops from the
// front of pipe_ring (FIFO — TCP preserves order) and immediately calls
// buildBatch() to refill the window.
//
//  acks_pending == 0            →  pipeline empty; buildBatch() starts fresh
//  0 < acks_pending < DEPTH     →  partial pipeline; buildBatch() tops it up
//  acks_pending == PIPELINE_DEPTH  →  pipeline full; buildBatch() is a no-op
//

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "communication_types.h"
#include "protocol.h"
#include "order_factory.h"
#include "../config/config.h"

#include <array>
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

    // ── Write buffer (outbound batch) ─────────────────────────────────────────
    // Holds up to PIPELINE_DEPTH frames packed consecutively; sent in one
    // send() call to reduce syscall overhead.
    char   write_buf[PIPELINE_DEPTH * INBOUND_BSIZE]{};
    size_t write_len = 0;
    size_t write_off = 0;

    // ── Read buffer (inbound responses) ───────────────────────────────────────
    // Sized to hold all responses for one pipelined batch.  Multiple responses
    // can land in a single recv() call; the consumer must advance past each one.
    char   read_buf[PIPELINE_DEPTH * OUTBOUND_BSIZE]{};
    size_t read_len = 0;

    // ── Active order ID ring (CANCEL / MODIFY targeting) ──────────────────────
    std::array<OrderId, MAX_ACTIVE_ORDERS> active_orders{};
    uint32_t active_head = 0;   // next write slot (wraps)
    uint32_t active_size = 0;   // valid entries, capped at MAX_ACTIVE_ORDERS

    // ── Pipeline ring (per-request metadata, FIFO in TCP order) ──────────────
    struct PipeSlot {
        OrderFactory::FrameKind kind = OrderFactory::FrameKind::NEW;
        InboundMessage          msg{};
    };
    std::array<PipeSlot, PIPELINE_DEPTH> pipe_ring{};
    uint32_t ring_head    = 0;  // cumulative frames built   (producer index)
    uint32_t ring_tail    = 0;  // cumulative responses consumed (consumer index)
    uint32_t acks_pending = 0;  // ring_head - ring_tail  (current in-flight count)

    // Logging cache — updated by buildOne (just before logSend) and by
    // consumeResponse (just before logAck / logMatch / logErr).
    OrderFactory::FrameKind last_kind = OrderFactory::FrameKind::NEW;
    InboundMessage          last_msg{};

    // ── Closed-loop rate limiting ─────────────────────────────────────────────
    uint64_t next_send_ns = 0;
    bool     send_pending = false;

    // ── Reconnection ──────────────────────────────────────────────────────────
    uint64_t reconnect_at_ns = 0;
    uint32_t reconnect_count = 0;

    // ── Statistics ────────────────────────────────────────────────────────────
    uint64_t requests_sent    = 0;
    uint64_t responses_ack    = 0;  // direct ACK per sent request
    uint64_t responses_match  = 0;  // unsolicited fill notifications
    uint64_t responses_err    = 0;
    int64_t  orders_in_flight = 0;  // requests sent but not yet responded to

    // ── Per-client RNG ────────────────────────────────────────────────────────
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
