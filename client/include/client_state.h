//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "communication_types.h"
#include "protocol.h"
#include "../config/config.h"

struct ClientState {
    //===== identity =====
    uint32_t client_id = 0;
    int fd = -1;
    bool connecting = false;  // true while non-blocking connect() is pending

    //===== write buf =====
    // One batch = PIPELINE_DEPTH frames packed consecutively.
    // write_off tracks bytes already handed to the kernel.
    char write_buf[PIPELINE_DEPTH * INBOUND_BSIZE]{};
    size_t write_len = 0;
    size_t write_off = 0;

    //===== read buf =====
    // Sized for a full pipeline batch plus a few unsolicited MATCH notifications.
    // Data always lives at [0, read_len); advance_read() memmoves the remainder
    // left — cheap for the small sizes involved.
    char read_buf[PIPELINE_DEPTH * 2 * OUTBOUND_BSIZE]{};
    size_t read_len = 0;

    //===== pipelining =====
    uint32_t acks_pending = 0;   // frames sent but not yet responded to
    uint64_t next_send_ns = 0;   // earliest time (CLOCK_MONOTONIC ns) to send next request

    // Pending request types: FIFO ring parallel to the in-flight pipeline.
    // pending_tail advances on every send; pending_head on every ACK consumed.
    MessageType pending_types[PIPELINE_DEPTH]{};
    uint32_t    pending_head = 0;
    uint32_t    pending_tail = 0;

    // Active order ring: order IDs from NEW ACKs, used as CANCEL/MODIFY targets.
    // active_write/active_count track the write side; active_read cycles reads.
    OrderId  active_orders[MAX_ACTIVE_ORDERS]{};
    uint32_t active_write = 0;
    uint32_t active_count = 0;
    uint32_t active_read  = 0;

    //===== stats =====
    uint64_t requests_sent = 0;
    uint64_t responses_ack = 0;
    uint64_t responses_match = 0;
    uint64_t responses_err = 0;
    int64_t orders_in_flight = 0;

};

#endif // CLIENT_STATE_H
