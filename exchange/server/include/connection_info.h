//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP
#define TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP

#include "protocol.h"
#include "socket.h"
#include "buffer.h"
#include "ring_buffer.hpp"

#include <algorithm>

using OutboundQueue = RingBuffer<OutboundMessage>;

class ConnectionInfo {
public:
    // special member functions
    explicit ConnectionInfo(EpollSocket sock) // constructed when a new client connects. takes ownership of the socket.
        : sock_(std::move(sock)) {}

    ConnectionInfo(const ConnectionInfo&)            = delete;
    ConnectionInfo& operator=(const ConnectionInfo&) = delete;
    ConnectionInfo(ConnectionInfo&&)                 = delete;
    ConnectionInfo& operator=(ConnectionInfo&&)      = delete;

    ~ConnectionInfo() = default;

    // socket functions
    [[nodiscard]] int fd() const { return sock_.get(); }

    // buffer functions
    Buffer& read_buffer() { return read_buf_; }
    Buffer& write_buffer() { return write_buf_; }

    // outbound queue functions
    void push_outbound(const OutboundMessage& msg) { outbound_.push(msg); }
    [[nodiscard]] OutboundMessage pop_outbound();
    [[nodiscard]] bool has_pending_outbound() const { return !outbound_.empty(); }

    // inbound msg functions
    [[nodiscard]] const InboundMessage& inbound() const { return inbound_; }
    void set_inbound(const InboundMessage &m) { inbound_ = m; }

    // state functions
    [[nodiscard]] ConnectionState state() const { return state_; }
    void set_state(ConnectionState s) { state_ = s; }

    // error functions
    void push_error(ServerError err);

    // latency: serializeResponse stores t1/t2/t3 from the outbound message here;
    // sendResponse completes the sample with t4 before writing to the socket.
    void set_pending_timestamps(Timestamp t1, Timestamp t2, Timestamp t3) {
        pending_t1_ = t1; pending_t2_ = t2; pending_t3_ = t3;
        has_pending_latency_ = true;
    }
    [[nodiscard]] bool      has_pending_latency() const { return has_pending_latency_; }
    [[nodiscard]] Timestamp pending_t1()          const { return pending_t1_; }
    [[nodiscard]] Timestamp pending_t2()          const { return pending_t2_; }
    [[nodiscard]] Timestamp pending_t3()          const { return pending_t3_; }
    void clear_pending_latency() { has_pending_latency_ = false; }

private:
    // data
    const EpollSocket sock_;
    Buffer read_buf_;
    Buffer write_buf_;
    OutboundQueue outbound_{RINGBUF_SIZE};
    InboundMessage inbound_{};

    // state
    ConnectionState state_ = ConnectionState::READING;

    // pending latency timestamps (t1/t2/t3 from the outbound message; t4 added in sendResponse)
    Timestamp pending_t1_{};
    Timestamp pending_t2_{};
    Timestamp pending_t3_{};
    bool has_pending_latency_{false};
};


#endif //TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP