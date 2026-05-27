//
// Created by charl on 1/13/2026.
//

#include "../include/server.h"
#include "../include/latency.h"
#include "../include/validation.h"
#include "lib.h"

#include <cstring>
#include <immintrin.h>
#include <sys/socket.h>
#include <netdb.h>
#include <iostream>
#include <fcntl.h>

// Destructs at program exit, printing latency percentiles to stdout.
static LatencyHandler latency_handler;

#define ever (;;)

//=============================================================================
// Forward-Declared Helpers
//=============================================================================

static int epoll_ctrl(int epoll_fd, int fd, int op, int target);

//=============================================================================
// Special Member Functions
//=============================================================================

Server::Server(InboundRing& in, OutboundRing& out, std::atomic<bool>& stop)
    : in_ring_(in), out_ring_(out), stop_(stop) {}

Server::~Server() {
    if (epoll_in_  != -1) close(epoll_in_);
    if (epoll_out_ != -1) close(epoll_out_);
}

//=============================================================================
// run()
//=============================================================================

void Server::run() {
    if (!setupEpoll()) return;

    inbound_thread_ = std::jthread([&]() {
        pin_to_core(INBOUND_CORE);
        handleRequests();
    });

    outbound_thread_ = std::jthread([&]() {
        pin_to_core(OUTBOUND_CORE);
        serveResponses();
    });
}

//=============================================================================
// Setup
//=============================================================================

bool Server::setupEpoll() {
    epoll_in_  = epoll_create(MAX_CLIENTS);
    epoll_out_ = epoll_create(MAX_CLIENTS);

    if (epoll_in_ == -1 || epoll_out_ == -1) {
        std::cerr << "epoll_create error: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

std::optional<EpollSocket> Server::setupListenSocket() const {
    struct addrinfo hints = {}, *result;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

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

    EpollSocket listen_sock{raw_fd, epoll_in_};

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

    if (epoll_ctrl(epoll_in_, raw_fd, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        return std::nullopt;
    }

    return listen_sock;
}

//=============================================================================
// handleRequests  (inbound thread)
//=============================================================================

void Server::handleRequests() {
    auto socket_setup{setupListenSocket()};
    if (!socket_setup.has_value()) {
        stop_.store(true, std::memory_order_relaxed);
        return;
    }
    const EpollSocket listen_sock{std::move(socket_setup.value())};

    while (!stop_.load(std::memory_order_relaxed)) {
        // Drain close_ring_ first so we never attempt to read a condemned fd.
        {
            int dead_fd{};
            while (close_ring_.pop(dead_fd)) {
                closeConnection(dead_fd);
            }
        }

        const int nfds = epoll_wait(epoll_in_, events_in_, MAX_CLIENTS, 0);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            const int fd = events_in_[i].data.fd;

            if (fd == listen_sock.get()) {
                registerConnection(listen_sock.get());
                continue;
            }

            readAndProcessBytes(fd);
        }
    }
}

std::optional<int> Server::registerConnection(const int listen_fd) {
    const int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd == -1) {
        std::cerr << "accept error: " << strerror(errno) << std::endl;
        return std::nullopt;
    }

    EpollSocket client_sock{client_fd, epoll_in_};

    const int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags == -1) return std::nullopt;
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    if (epoll_ctrl(epoll_in_, client_fd, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        return std::nullopt;
    }

    info_map_[client_fd].emplace(std::move(client_sock));

    // Notify the outbound thread so it can set fd_to_info_[fd] before the
    // first response needs to be sent.
    register_ring_.push({client_fd, &(*info_map_[client_fd])});

    return client_fd;
}

void Server::readAndProcessBytes(const int client_fd) {
    if (!info_map_[client_fd].has_value()) return;

    ConnectionInfo& info{*info_map_[client_fd]};
    readRequest(info);

    // readRequest may have called closeConnection (EOF), resetting the optional.
    if (!info_map_[client_fd].has_value()) return;

    processRequest(info);
}

void Server::readRequest(ConnectionInfo& info) {
    const int     client_fd{info.fd()};
    ReadBuffer&   buf{info.read_buffer()};
    const ssize_t n{buf.read_from(client_fd)};

    if (n == 0) {
        closeConnection(client_fd);
    }
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Hard read error -> route an error reply to the outbound thread.
        error_ring_.push({client_fd, ServerError::SYSTEM_ERROR});
        buf.clear();
    }
    // n > 0 or EAGAIN -> processRequest drains complete frames when ready.
}

void Server::processRequest(ConnectionInfo& info) {
    ReadBuffer& buf{info.read_buffer()};

    while (buf.length() >= INBOUND_BSIZE) {
        const Timestamp recv_tsc = __rdtsc();

        const char* data{buf.view().data()};
        constexpr size_t header_len = sizeof("EXCHANGE\n") - 1;
        if (memcmp(data, "EXCHANGE\n", header_len) != 0) {
            error_ring_.push({info.fd(), ServerError::MALFORMED_REQUEST});
            buf.advance(INBOUND_BSIZE);
            continue;
        }

        InboundMessage msg{};
        memcpy(&msg, data + header_len, sizeof(InboundMessage));
        msg.recv_tsc = recv_tsc;
        buf.advance(INBOUND_BSIZE);

        if (!validate_message(msg)) {
            error_ring_.push({info.fd(), ServerError::INVALID_ORDER});
            continue;
        }

        // t2 -> server is handing the message to the engine ring.
        msg.server_push = __rdtsc();
        info.set_inbound(msg);

        // Register the ClientId -> ConnectionInfo* mapping on first sighting.
        // store-release pairs with the outbound thread's load-acquire.
        const ClientId cid = msg.client_id;
        if (cid < client_map_.size() &&
            client_map_[cid].load(std::memory_order_relaxed) == nullptr) {
            client_map_[cid].store(&(*info_map_[info.fd()]),
                                   std::memory_order_release);
        }

        while (!in_ring_.push(info.inbound())) {}
    }
}

//=============================================================================
// serveResponses  (outbound thread)
//=============================================================================

void Server::serveResponses() {
    while (!stop_.load(std::memory_order_relaxed)) {
        routeOutboundMessages();
        beginSends();

        // EPOLLOUT events -> a previously blocked send can now proceed.
        const int nfds = epoll_wait(epoll_out_, events_out_, MAX_CLIENTS, 0);

        if (nfds == -1 && errno != EINTR) {
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            const int fd = events_out_[i].data.fd;

            if (fd < 0 || fd >= static_cast<int>(condemned_fds_.size())) continue;
            if (condemned_fds_[fd] || !fd_to_info_[fd]) continue;

            ConnectionInfo& info = *fd_to_info_[fd];
            sendResponse(info);

            // If the partial send drained the buffer, deregister EPOLLOUT.
            if (!condemned_fds_[fd] && info.write_buffer().remaining_to_send() == 0) {
                epoll_ctrl(epoll_out_, fd, EPOLL_CTL_DEL, EPOLLIN | EPOLLOUT);
                epollout_armed_[fd] = false;
            }
        }
    }
}

void Server::routeOutboundMessages() {
    // ===== 1. New connections (inbound -> outbound) =====
    // Process these first so fd_to_info_ is populated before we need it below.
    {
        std::pair<int, ConnectionInfo*> reg{};
        while (register_ring_.pop(reg)) {
            fd_to_info_[reg.first]    = reg.second;
            condemned_fds_[reg.first] = false;
            epollout_armed_[reg.first] = false;
        }
    }

    // ===== 2. Engine responses (out_ring_) =====
    {
        OutboundMessage msg{};
        while (out_ring_.pop(msg)) {
            const ClientId cid = msg.client_id;
            if (cid >= client_map_.size()) continue;

            ConnectionInfo* info = client_map_[cid].load(std::memory_order_acquire);
            if (!info) continue;

            info->push_outbound(msg);
            writable_fds_.insert(info->fd());
        }
    }

    // ===== 3. Error replies from the inbound thread (error_ring_) =====
    {
        ErrorRecord rec{};
        while (error_ring_.pop(rec)) {
            const int fd = rec.fd;
            if (fd < 0 || fd >= static_cast<int>(fd_to_info_.size())) continue;
            if (condemned_fds_[fd] || !fd_to_info_[fd]) continue;

            fd_to_info_[fd]->push_outbound(OutboundMessage{.server_error = rec.err});
            writable_fds_.insert(fd);
        }
    }
}

void Server::beginSends() {
    for (auto it = writable_fds_.begin(); it != writable_fds_.end(); ) {
        const int fd = *it;

        if (condemned_fds_[fd] || !fd_to_info_[fd]) {
            it = writable_fds_.erase(it);
            continue;
        }

        ConnectionInfo& info    = *fd_to_info_[fd];
        WriteBuffer&    wbuf    = info.write_buffer();

        // Fill the write buffer with as many queued messages as fit.
        // This packs multiple responses into one send() syscall.
        while (info.has_pending_outbound() && wbuf.has_room(OUTBOUND_BSIZE)) {
            appendResponse(info);
        }

        // Send whatever is now in the buffer.
        if (wbuf.remaining_to_send() > 0) {
            sendResponse(info);
        }

        // Stay in writable_fds_ only if there is still work to do.
        if (condemned_fds_[fd]) {
            it = writable_fds_.erase(it);
        } else if (wbuf.remaining_to_send() == 0 && !info.has_pending_outbound()) {
            it = writable_fds_.erase(it);
        } else {
            ++it;
        }
    }
}

// Pops one message from the outbound queue, serialises it as a fixed-size
// OUTBOUND_BSIZE frame zero-padded to that size, and appends it to the write
// buffer.  Saves a PendingLatency entry so finishBatch can record t6 later.
/*static*/ void Server::appendResponse(ConnectionInfo& info) {
    const OutboundMessage msg  = info.pop_outbound();
    const Timestamp       t5   = __rdtsc();   // t5 -> dequeued and serialised

    // Build the fixed-size frame.  The array is zero-initialised so any bytes
    // beyond the actual response are already padded with 0x00.
    char frame[OUTBOUND_BSIZE]{};

    if (msg.server_error == ServerError::NONE) {
        memcpy(frame, OK_STATUS, strlen(OK_STATUS));
        memcpy(frame + strlen(OK_STATUS),                  &msg.client_id, sizeof(ClientId));
        memcpy(frame + strlen(OK_STATUS) + sizeof(ClientId), &msg.order_id, sizeof(OrderId));
    }
    else {
        memcpy(frame, ERROR_STATUS, strlen(ERROR_STATUS));
        const std::string_view err_str{server_error_string(msg.server_error)};
        memcpy(frame + strlen(ERROR_STATUS), err_str.data(), err_str.size());
        frame[strlen(ERROR_STATUS) + err_str.size()] = '\n';
    }

    info.write_buffer().append(frame, OUTBOUND_BSIZE);

    // Only ACK responses (NEW / CANCEL / MODIFY) are tracked: their t1->t6
    // spans measure deterministic service time.  MATCH messages are excluded
    // because t1 belongs to the original order and includes queuing delay
    // waiting for a counterparty.
    const bool record = (msg.server_error == ServerError::NONE &&
                         msg.message_type != MessageType::MATCH);
    info.push_latency({
        msg.recv_tsc,        msg.server_push_tsc,
        msg.engine_pop_tsc,  msg.engine_push_tsc,
        t5,                  record
    });
}

void Server::sendResponse(ConnectionInfo& info) {
    WriteBuffer& wbuf    = info.write_buffer();
    const int    fd      = info.fd();
    const ssize_t n      = wbuf.write_to(fd);

    if (wbuf.remaining_to_send() == 0) {
        // All bytes are in the kernel send buffer -> record latency and reset.
        finishBatch(info);
    }
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Hard send error -> condemn; inbound thread will close the fd.
        condemnConnection(fd);
    }
    else if (!epollout_armed_[fd]) {
        // Partial send or EAGAIN -> arm EPOLLOUT to resume without busy-waiting.
        epoll_ctrl(epoll_out_, fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLOUT);
        epollout_armed_[fd] = true;
    }
}

void Server::finishBatch(ConnectionInfo& info) const {
    const Timestamp t6 = __rdtsc();   // t6 -> bytes are in the kernel send buffer

    for (int i = 0; i < info.latency_count(); ++i) {
        const PendingLatency& e = info.latency_entry(i);
        if (e.record) {
            latency_handler.push_sample({
                e.recv_tsc,       e.server_push_tsc,
                e.engine_pop_tsc, e.engine_push_tsc,
                e.server_pop,     t6
            });
        }
    }

    info.clear_latency_batch();
    info.write_buffer().try_reset();
}

//=============================================================================
// condemnConnection  (outbound thread only)
//=============================================================================

void Server::condemnConnection(const int fd) {
    condemned_fds_[fd]   = true;
    fd_to_info_[fd]      = nullptr;
    epollout_armed_[fd]  = false;
    writable_fds_.erase(fd);
    // Notify the inbound thread to close the fd and free the ConnectionInfo.
    while (!close_ring_.push(fd)) {}
}

//=============================================================================
// closeConnection  (inbound thread only)
//=============================================================================

void Server::closeConnection(const int client_fd) {
    if (!info_map_[client_fd].has_value()) return;

    const ClientId cid = info_map_[client_fd]->inbound().client_id;
    // Clear the routing table so routeOutboundMessages skips this client_id.
    // store-release pairs with the outbound thread's load-acquire.
    if (cid > 0 && cid < client_map_.size()) {
        client_map_[cid].store(nullptr, std::memory_order_release);
    }

    // Resetting the optional destructs ConnectionInfo -> destructs EpollSocket
    // -> EPOLL_CTL_DEL on epoll_in_ then close(fd).
    // close(fd) also implicitly removes the fd from epoll_out_.
    info_map_[client_fd].reset();
}

//=============================================================================
// Helpers
//=============================================================================

static int epoll_ctrl(const int epoll_fd, const int fd, const int op, const int target) {
    epoll_event ev{};
    ev.events  = target | EPOLLET;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, op, fd, &ev) == -1) {
        std::cerr << "epoll_ctl error: " << strerror(errno) << std::endl;
        return -1;
    }

    return 0;
}
