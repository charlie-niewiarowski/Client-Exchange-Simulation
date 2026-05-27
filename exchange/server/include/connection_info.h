//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP
#define TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP

#include "protocol.h"
#include "socket.h"
#include "buffer.h"
#include "ring_buffer.hpp"
#include "config.h"

#include <array>
#include <algorithm>

//=============================================================================
// Buffer aliases
//=============================================================================

// Per-connection read buffer -> holds READ_BATCH_SIZE complete inbound frames.
using ReadBuffer  = Buffer<READ_BATCH_SIZE  * INBOUND_BSIZE>;

// Per-connection write buffer -> holds WRITE_BATCH_SIZE complete outbound frames.
// Multiple serialised responses are appended here and sent in a single syscall.
using WriteBuffer = Buffer<WRITE_BATCH_SIZE * OUTBOUND_BSIZE>;

using OutboundQueue = RingBuffer<OutboundMessage>;

//=============================================================================
// PendingLatency
//=============================================================================

// Holds the data needed to record one LatencySample once the write batch is
// fully sent (t6).  Filled by appendResponse; drained by finishBatch.
// record == false -> MATCH or error response; skip it.
struct PendingLatency {
    Timestamp recv_tsc;
    Timestamp server_push_tsc;
    Timestamp engine_pop_tsc;
    Timestamp engine_push_tsc;
    Timestamp server_pop;   // t5 -> message dequeued and serialised
    bool      record;
};

//=============================================================================
// ConnectionInfo
//=============================================================================
//
// Owns all per-connection state.  The two field groups below must be accessed
// only from the thread that owns them.  Mixing threads is a data race.
//
//   Inbound thread  (handleRequests) -> reads socket, parses frames, pushes to engine ring
//   Outbound thread (serveResponses) -> pops engine responses, fills write batches, sends
//
// The socket fd is read-only after construction; both threads may call fd().

class ConnectionInfo {
public:
    explicit ConnectionInfo(EpollSocket sock)
        : sock_(std::move(sock)) {}

    ConnectionInfo(const ConnectionInfo&)            = delete;
    ConnectionInfo& operator=(const ConnectionInfo&) = delete;
    ConnectionInfo(ConnectionInfo&&)                 = delete;
    ConnectionInfo& operator=(ConnectionInfo&&)      = delete;

    ~ConnectionInfo() = default;

    //===== shared (read-only after construction) =====

    [[nodiscard]] int fd() const { return sock_.get(); }

    //===== inbound thread =====
    // read_buf_ and inbound_ are touched only by handleRequests.

    ReadBuffer& read_buffer() { return read_buf_; }

    [[nodiscard]] const InboundMessage& inbound() const { return inbound_; }
    void set_inbound(const InboundMessage& m)           { inbound_ = m; }

    //===== outbound thread =====
    // Everything below is touched only by serveResponses.

    WriteBuffer& write_buffer() { return write_buf_; }

    // Per-connection queue of engine responses waiting to be serialised.
    // Pushed by routeOutboundMessages; popped by appendResponse.
    void            push_outbound(const OutboundMessage& msg) { outbound_.push(msg); }
    OutboundMessage pop_outbound();
    [[nodiscard]] bool has_pending_outbound() const { return !outbound_.empty(); }

    //===== latency batch (outbound thread) =====
    // appendResponse fills one slot per serialised message.
    // finishBatch records all filled slots (setting t6) then clears the array.

    void push_latency(const PendingLatency& e) {
        if (latency_count_ < static_cast<int>(latency_batch_.size()))
            latency_batch_[latency_count_++] = e;
    }
    [[nodiscard]] int                   latency_count()            const { return latency_count_; }
    [[nodiscard]] const PendingLatency& latency_entry(const int i) const { return latency_batch_[i]; }
    void clear_latency_batch() { latency_count_ = 0; }

private:
    //===== shared (read-only after construction) =====
    const EpollSocket sock_;

    //===== inbound thread =====
    ReadBuffer     read_buf_;
    InboundMessage inbound_{};

    //===== outbound thread =====
    WriteBuffer   write_buf_;
    OutboundQueue outbound_{RINGBUF_SIZE};

    // One slot per message appended to the current write batch.
    std::array<PendingLatency, WRITE_BATCH_SIZE> latency_batch_{};
    int latency_count_{0};
};

#endif //TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP
