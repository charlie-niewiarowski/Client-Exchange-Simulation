//
// Created by charl on 1/13/2026.
//

#include "../include/server.h"
#include "../include/validation.h"

#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <iostream>
#include <fcntl.h>
#include <filesystem>

#define ever (;;)

//=============================================================================
// Forward-Declared Helpers
//=============================================================================

static int epoll_ctrl(int epoll_fd, int fd, int op, int target);

//=============================================================================
// Special Member Functions
//=============================================================================

Server::Server(InboundRing& in, OutboundRing& out)
    : in_ring_(in), out_ring_(out) {}

Server::~Server() {
    if (epoll_fd_ != -1) close(epoll_fd_);
}

//=============================================================================
// run()
//=============================================================================

void Server::run() {
    if (!setupEpoll()) return;

    auto socket_setup{setupListenSocket()};
    if (!socket_setup.has_value()) return;
    const EpollSocket listen_sock{std::move(socket_setup.value())};

    for ever {
        #if TESTING
        if (!running_.load(std::memory_order_relaxed)) return;
        #endif

        const int nfds = epoll_wait(epoll_fd_, events_, MAX_CLIENTS, 0);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            const int fd = events_[i].data.fd;

            if (fd == listen_sock.get()) {
                registerConnection(listen_sock.get());
                continue;
            }

            const ConnectionInfo& info{info_map_.at(fd)};
            if (info.state() == ConnectionState::READING && events_[i].events & EPOLLIN) {
                readAndProcessBytes(fd);
            }
            else if (info.state() == ConnectionState::SENDING && events_[i].events & EPOLLOUT) {
                finishSend(fd);
            }
        }

        // route outbound messages to respective connections
        routeOutboundMessages();

        // serialize ready fds and try to perform a full write
        beginSends();
    }
}

//=============================================================================
// Setup Functions
//=============================================================================

std::optional<EpollSocket> Server::setupListenSocket() const {
    struct addrinfo hints = {}, *result;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (const int s = getaddrinfo(nullptr, PORT, &hints, &result); s != 0) {
        std::cerr << "getaddrinfo error: " << gai_strerror(s) << std::endl;
        return std::nullopt;
    }

    const int raw_fd = socket(result->ai_family, result->ai_socktype, 0);
    if (raw_fd == -1) {
        std::cerr << "socket() error: " << strerror(errno) << std::endl;
        freeaddrinfo(result);
        return std::nullopt;
    }

    EpollSocket listen_sock{raw_fd, epoll_fd_};

    constexpr int opt = 1;
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

//=============================================================================
// Loop Body
//=============================================================================

std::optional<int> Server::registerConnection(const int listen_fd) {
    const int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd == -1) {
        std::cerr << "accept error: " << strerror(errno) << std::endl;
        return std::nullopt;
    }

    EpollSocket client_sock{client_fd, epoll_fd_};

    const int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags == -1) return std::nullopt;
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    if (epoll_ctrl(epoll_fd_, client_fd, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        return std::nullopt;
    }

    info_map_.emplace(client_fd, std::move(client_sock));

    return client_fd;
}

void Server::readAndProcessBytes(const int client_fd) {
    ConnectionInfo& info{info_map_.at(client_fd)};

    readRequest(info);
    if (info.state() != ConnectionState::PROCESSING) return;

    processRequest(info);
}

// the reason that this is a wrapper is to help understanding of what the function
// actually does; this doesn't just send responses, it specifically tries to finish
// writes that either did not happen or partially happened.
void Server::finishSend(const int client_fd) {
    sendResponse(info_map_.at(client_fd));
}

void Server::routeOutboundMessages() {
    OutboundMessage msg{};

    while (out_ring_.pop(msg)) {
        if (!client_map_.contains(msg.client_id)) continue;

        ConnectionInfo *info{client_map_.at(msg.client_id)};
        info->push_outbound(msg);

        writable_fds_.insert(info->fd());
    }
}

void Server::beginSends() {
    for (auto it{writable_fds_.begin()}; it != writable_fds_.end(); ) {
        const int fd{*it};

        ConnectionInfo& info{info_map_.at(fd)};
        serializeResponse(info);
        sendResponse(info);

        if (!info_map_.contains(fd) || !info.has_pending_outbound()) {
            it = writable_fds_.erase(it);
        }
        else {
            ++it;
        }
    }
}

//=============================================================================
// Read/Parse/Execute Pipeline
//=============================================================================

void Server::readRequest(ConnectionInfo& info) {
    Buffer& buf{info.read_buffer()};

    const int client_fd{info.fd()};
    const ssize_t n{buf.read_from(client_fd)};

    if (n > 0) {
        if (buf.length() >= INBOUND_BSIZE) {
            info.set_state(ConnectionState::PROCESSING);
        }
    }
    else if (n == 0) {
        closeConnection(client_fd);
    }
    else if (errno != EAGAIN && errno != EWOULDBLOCK) { // also means n < 0
        info.push_error(ServerError::SYSTEM_ERROR);
        info.set_state(ConnectionState::SERIALIZING);
        writable_fds_.insert(client_fd);

        buf.clear();
    }
}

void Server::processRequest(ConnectionInfo &info) {
    Buffer& buf{info.read_buffer()};
    const char* data{buf.view().data()};

    constexpr size_t header_len = sizeof("EXCHANGE\n") - 1;

    if (memcmp(data, "EXCHANGE\n", header_len) != 0) {
        info.push_error(ServerError::MALFORMED_REQUEST);
        info.set_state(ConnectionState::SERIALIZING);
        writable_fds_.insert(info.fd());

        buf.clear();
        return;
    }

    InboundMessage msg{};
    memcpy(&msg, data + header_len, sizeof(InboundMessage));
    buf.clear();

    if (!validate_message(msg)) {
        info.push_error(ServerError::INVALID_ORDER);
        info.set_state(ConnectionState::SERIALIZING);
        writable_fds_.insert(info.fd());

        return;
    }

    info.set_inbound(msg);
    client_map_[msg.client_id] = &info_map_.at(info.fd());

    while (!in_ring_.push(info.inbound())) {}
    info.set_state(ConnectionState::SERIALIZING);
}

//=============================================================================
// Serialization/Write Pipeline
//=============================================================================

void Server::serializeResponse(ConnectionInfo &info) {
    const OutboundMessage msg{info.pop_outbound()};
    const ServerError err{msg.server_error};

    char buf_contents[OUTBOUND_BSIZE];
    size_t response_len;
    if (err == ServerError::NONE) {
        memcpy(buf_contents, OK_STATUS, strlen(OK_STATUS));
        memcpy(buf_contents + strlen(OK_STATUS), &msg.client_id, sizeof(ClientId));
        memcpy(buf_contents + strlen(OK_STATUS) + sizeof(ClientId), &msg.order_id, sizeof(OrderId));
        response_len = strlen(OK_STATUS) + sizeof(ClientId) + sizeof(OrderId);
    }
    else {
        memcpy(buf_contents, ERROR_STATUS, strlen(ERROR_STATUS));

        const std::string_view err_string{server_error_string(err)};
        memcpy(buf_contents + strlen(ERROR_STATUS), err_string.data(), err_string.length());
        buf_contents[strlen(ERROR_STATUS) + err_string.length()] = '\n';
        response_len = strlen(ERROR_STATUS) + err_string.length() + 1;
    }

    Buffer& buf = info.write_buffer();
    buf.fill(buf_contents, response_len);

    info.set_state(ConnectionState::SENDING);
}


void Server::sendResponse(ConnectionInfo& info) {
    Buffer& buf = info.write_buffer();

    const int client_fd = info.fd();
    const ssize_t n = buf.write_to(client_fd);

    if (buf.bytes_written() == buf.length()) {
        finishResponseCycle(info);
    }
    else if (n > 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        epoll_ctrl(epoll_fd_, client_fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT);
    }
    else {
        closeConnection(client_fd);
    }
}

void Server::finishResponseCycle(ConnectionInfo& info) const {
    info.write_buffer().clear();
    info.set_state(ConnectionState::READING);
    epoll_ctrl(epoll_fd_, info.fd(), EPOLL_CTL_MOD, EPOLLIN);
}

//=============================================================================
// Helpers
//=============================================================================

void Server::closeConnection(const int client_fd) {
    const auto it{info_map_.find(client_fd)};
    if (it == info_map_.end()) return;

    client_map_.erase(it->second.inbound().client_id);

    // erasing destructs ConnectionInfo -> destructs EpollSocket -> calls EPOLL_CTL_DEL then close(fd)
    info_map_.erase(it);
}

static int epoll_ctrl(const int epoll_fd, const int fd, const int op, const int target) {
    epoll_event ev{};
    ev.events = target;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, op, fd, &ev) == -1) {
        std::cerr << "epoll_ctl error: " << strerror(errno) << std::endl;
        return -1;
    }

    return 0;
}