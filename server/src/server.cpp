//
// Created by charl on 1/13/2026.
//

#include "../include/server.h"

#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <iostream>
#include <fcntl.h>
#include <filesystem>

#include "../tests/client.h"

#define ever (;;)

//=============================================================================
// Forward-declared free forms
//=============================================================================

static bool validate_message(const InboundMessage& msg);
static bool validate_new(const InboundMessage& msg);
static bool validate_cancel(const InboundMessage& msg);
static bool validate_modify(const InboundMessage& msg);

static int epoll_ctrl(int epoll_fd, int fd, int op, int target);

//=============================================================================
// Special Member Functions
//=============================================================================

Server::Server(InboundRing& in, OutboundRing& out)
    : in_ring_(in), out_ring_(out) {}

Server::~Server() {
    if (epoll_fd_ != -1) ::close(epoll_fd_);
}

//=============================================================================
// run()
//=============================================================================

void Server::run() {
    if (!setupEpoll()) return;

    auto socket_setup = setupListenSocket();
    if (!socket_setup.has_value()) return;
    const EpollSocket listen_sock = std::move(socket_setup.value());

    for ever {
        int nfds = epoll_wait(epoll_fd_, events_, MAX_CLIENTS, 0);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            const int fd = events_[i].data.fd;

            if (fd == listen_sock.get()) {
                registerConnection(listen_sock.get());
            }
            else if (events_[i].events & EPOLLIN) {
                handleRequests(fd);
            }
            else if (events_[i].events & EPOLLOUT) {
                finishBlockedWrites(fd);
            }
        }

        // route outbound messages to respective fds
        drainOutboundRing();

        // serialize ready fds and try to perform a full write
        handleResponses();
    }
}

//=============================================================================
// run() Helper Suite
//=============================================================================

std::optional<EpollSocket> Server::setupListenSocket() {
    struct addrinfo hints = {}, *result;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (int s = getaddrinfo(nullptr, PORT, &hints, &result); s != 0) {
        std::cerr << "getaddrinfo error: " << gai_strerror(s) << std::endl;
        return std::nullopt;
    }

    int raw_fd = socket(result->ai_family, result->ai_socktype, 0);
    if (raw_fd == -1) {
        std::cerr << "socket() error: " << strerror(errno) << std::endl;
        freeaddrinfo(result);
        return std::nullopt;
    }

    EpollSocket listen_sock{raw_fd, epoll_fd_};

    int opt = 1;
    setsockopt(raw_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(raw_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (bind(raw_fd, result->ai_addr, result->ai_addrlen) == -1) {
        std::cerr << "bind() error: " << std::strerror(errno) << std::endl;
        freeaddrinfo(result);
        return std::nullopt;
    }

    freeaddrinfo(result);

    if (listen(raw_fd, 32) != 0) {
        std::cerr << "listen() error: " << strerror(errno) << std::endl;
        return std::nullopt;
    }

    if (epoll_ctrl(epoll_fd_, raw_fd, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        return std::nullopt;
    }

    return listen_sock;
}

bool Server::setupEpoll() {
    epoll_fd_ = epoll_create(32);

    if (epoll_fd_ == -1) {
        std::cerr << "epoll_create error: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

std::optional<int> Server::registerConnection(int listen_fd) {
    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd == -1) {
        std::cerr << "accept error: " << strerror(errno) << std::endl;
        return std::nullopt;
    }

    EpollSocket client_sock{client_fd, epoll_fd_};

    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags == -1) return std::nullopt;
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    if (epoll_ctrl(epoll_fd_, client_fd, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        return std::nullopt;
    }

    info_map_.emplace(client_fd, std::move(client_sock));

    return client_fd;
}

void Server::handleRequests(const int client_fd) {
    ConnectionInfo& info{info_map_.at(client_fd)};

    switch (info.state()) {
        case ConnectionState::READING:
            readRequest(info);
            if (!info_map_.contains(client_fd)) return;
            if (info.state() != ConnectionState::PARSING) break;
            [[fallthrough]];
        case ConnectionState::PARSING:
            parseRequest(info);
            if (info.state() != ConnectionState::EXECUTING) break;
            [[fallthrough]];
        case ConnectionState::EXECUTING:
            executeRequest(info);
            if (info.state() != ConnectionState::SERIALIZING) break;
            [[fallthrough]];
        default:
            break;
    }
}

// the reason that this is a wrapper is to helper understanding of what the function
// actually does; this doesn't just send responses, it specifically tries to finish
// writes that either did not happen or partially happened.
void Server::finishBlockedWrites(const int client_fd) {
    sendResponse(info_map_.at(client_fd));
}

void Server::drainOutboundRing() {
    OutboundMessage msg{};

    while (out_ring_.pop(msg)) {
        if (!client_map_.contains(msg.client_id)) continue;

        ConnectionInfo *info{client_map_.at(msg.client_id)};
        info->push_outbound(msg);

        writable_fds_.insert(info->fd());
    }
}

void Server::handleResponses() {
    for (const int fd : writable_fds_) {
        ConnectionInfo& info{info_map_.at(fd)};

        switch (info.state()) {
            case ConnectionState::SERIALIZING:
                serializeResponse(info);
                [[fallthrough]];
            case ConnectionState::SENDING:
                sendResponse(info);
                break;
            default:
                break;
        }
    }

    writable_fds_.clear();
}

//=============================================================================
// handleClient() Helper Suite
//=============================================================================

void Server::readRequest(ConnectionInfo& info) {
    Buffer& buf = info.buffer();
    const int client_fd = info.fd();
    const ssize_t n = buf.read_from(client_fd);

    if (n > 0) {
        if (buf.length() >= INBOUND_BSIZE) {
            info.set_state(ConnectionState::PARSING);
        }
    }
    else if (n == 0) {
        closeConnection(client_fd);
    }
    else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            info.set_error(err_system_error);
            info.set_state(ConnectionState::SERIALIZING);
        }
    }
}

void Server::parseRequest(ConnectionInfo &info) {
    Buffer& buf{info.buffer()};
    std::string_view buf_contents{buf.view()};

    constexpr size_t header_len = sizeof("EXCHANGE\n") - 1;

    if (buf_contents.substr(0, header_len) != "EXCHANGE\n") {
        info.set_error(err_malformed_request);
        info.set_state(ConnectionState::SERIALIZING);
        return;
    }

    InboundMessage msg{};
    memcpy(&msg, buf_contents.data() + header_len, sizeof(InboundMessage));
    info.set_inbound(msg);
    info.set_state(ConnectionState::EXECUTING);

    client_map_[msg.client_id] = &info_map_.at(info.fd());
}

void Server::executeRequest(ConnectionInfo &info) {
    if (validate_message(info.inbound()) == false) {
        info.set_error(err_invalid_order);
        info.set_state(ConnectionState::SERIALIZING);
        return;
    }

    if (in_ring_.push(info.inbound()) == true) {
        info.set_state(ConnectionState::SERIALIZING);
    }
}

void Server::serializeResponse(ConnectionInfo &info) {
    OutboundMessage msg{};

    if (!info.has_error()) {
        msg = info.pop_outbound();

        if (msg.status == Status::FAILURE) {
            info.set_error(err_execution_error);
        }
    }

    const char *status = info.has_error() ? "EXCHANGE\nERROR\n" : "EXCHANGE\nOK\n";
    Buffer& buf = info.buffer();
    buf.overwrite(0, status);

    if (info.has_error()) {
        buf.overwrite(strlen(status), info.error().data());
    } else {
        char cid_buf[sizeof(ClientId) + sizeof(OrderId)];
        memcpy(cid_buf, &msg.client_id, sizeof(ClientId));
        memcpy(cid_buf + sizeof(ClientId), &msg.order_id, sizeof(OrderId));

        buf.overwrite(strlen(status), cid_buf);
    }

    info.set_state(ConnectionState::SENDING);
}


void Server::sendResponse(ConnectionInfo& info) {
    Buffer& buf = info.buffer();

    if (buf.length() == buf.bytes_written() && buf.view().empty()) {
        finishResponseCycle(info);
        return;
    }

    const int client_fd = info.fd();
    const ssize_t n = buf.write_to(client_fd);

    if (n == buf.length()) {
        finishResponseCycle(info);

    }
    else if (n == 0) {
        closeConnection(client_fd);
    }
    else if (n > 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        epoll_ctrl(epoll_fd_, client_fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT);
    }
}

//=============================================================================
// closeConnection() / finishResponseCycle()
//=============================================================================

void Server::closeConnection(int client_fd) {
    auto it = info_map_.find(client_fd);
    if (it == info_map_.end()) return;

    client_map_.erase(it->second.inbound().client_id);

    // Erasing destructs ConnectionInfo → destructs EpollSocket → calls EPOLL_CTL_DEL then close(fd).
    info_map_.erase(it);
}

void Server::finishResponseCycle(ConnectionInfo& info) {
    info.buffer().clear();
    info.clear_error();
    info.set_state(ConnectionState::READING);

    epoll_ctrl(epoll_fd_, info.fd(), EPOLL_CTL_MOD, EPOLLIN);
}

//=============================================================================
// Forward-declared helper implementations
//=============================================================================

static bool validate_message(const InboundMessage &msg) {
    switch (msg.message_type) {
        case MessageType::NEW: return validate_new(msg);
        case MessageType::CANCEL: return validate_cancel(msg);
        case MessageType::MODIFY: return validate_modify(msg);
        default: return false;
    }
}

static bool validate_new(const InboundMessage &msg) {
    auto raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 0) return false;

    auto raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    auto raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.side == Side::BID && msg.price <= 0) return false;
    if (msg.side == Side::ASK && msg.price < 0) return false;

    if (msg.quantity < 0 || msg.quantity > 1e6) return false;

    return true;
}

static bool validate_cancel(const InboundMessage &msg) {
    auto raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 1) return false;

    return true;
}

static bool validate_modify(const InboundMessage &msg) {
    auto raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 2) return false;

    auto raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    auto raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.side == Side::BID && msg.price <= 0) return false;
    if (msg.side == Side::ASK && msg.price < 0) return false;

    if (msg.quantity < 0 || msg.quantity > 1e6) return false;

    return true;
}

static int epoll_ctrl(int epoll_fd, int fd, int op, int target)  {
    epoll_event ev{};
    ev.events = target;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, op, fd, &ev) == -1) {
        std::cerr << "epoll_ctl error: " << strerror(errno) << std::endl;
        return -1;
    }

    return 0;
}