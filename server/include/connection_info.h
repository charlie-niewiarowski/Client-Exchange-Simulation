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
#include <optional>

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
    Buffer& buffer() { return buf_; }

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
    [[nodiscard]] bool has_error() const { return error_; }
    [[nodiscard]] std::string_view error() const { return err_msg_; }
    void set_error(std::string_view msg);
    void clear_error() { error_ = false; err_msg_.clear(); }

private:
    // data
    const EpollSocket sock_;
    Buffer buf_;
    OutboundQueue outbound_{RINGBUF_SIZE};
    InboundMessage inbound_{};

    // state
    ConnectionState state_ = ConnectionState::READING;

    // error
    bool error_{false};
    std::string err_msg_;
};

static_assert(MAX_ERROR_MSG_LEN > 32, "error message capacity unreasonably small");


#endif //TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP