//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP
#define TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP

#include "protocol.h"
#include "socket.h"
#include "buffer.h"
#include "../../include/ring_buffer.hpp"

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

private:
    // data
    const EpollSocket sock_;
    Buffer read_buf_;
    Buffer write_buf_;
    OutboundQueue outbound_{RINGBUF_SIZE};
    InboundMessage inbound_{};

    // state
    ConnectionState state_ = ConnectionState::READING;
};


#endif //TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP