//
// Created by cniew on 5/24/26.
//

#include "../include/orchestrator.h"
#include "../config/config.h"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers (file-local)
// ─────────────────────────────────────────────────────────────────────────────

static uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

static int epoll_ctrl(int epoll_fd, int fd, int op, uint32_t events, void* ptr) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = ptr;
    if (epoll_ctl(epoll_fd, op, fd, &ev) == -1) {
        std::fprintf(stderr, "epoll_ctl(%d): %s\n", op, std::strerror(errno));
        return -1;
    }
    return 0;
}

static const char* side_str(Side s) {
    return s == Side::BID ? "BID" : "ASK";
}

static const char* type_str(OrderType t) {
    return t == OrderType::LIMIT ? "LIMIT" : "MKT  ";
}

static const char* kind_str(OrderFactory::FrameKind k) {
    switch (k) {
        case OrderFactory::FrameKind::NEW:     return "NEW   ";
        case OrderFactory::FrameKind::CANCEL:  return "CANCEL";
        case OrderFactory::FrameKind::MODIFY:  return "MODIFY";
        case OrderFactory::FrameKind::GARBAGE: return "GARB  ";
        case OrderFactory::FrameKind::INVALID: return "INVAL ";
    }
    return "?";
}

// Check whether read_buf[0..len) contains a complete exchange response.
// OK  response is exactly 20 bytes: "EXCHANGE\nOK\n" + ClientId + OrderId
// ERR response ends with '\n' somewhere after offset 15 ("EXCHANGE\nERROR\n")
static bool response_complete(const char* buf, size_t len) {
    constexpr size_t ok_hdr_len  = sizeof("EXCHANGE\nOK\n")    - 1; // 12
    constexpr size_t ok_total    = ok_hdr_len + sizeof(ClientId) + sizeof(OrderId); // 20
    constexpr size_t err_hdr_len = sizeof("EXCHANGE\nERROR\n") - 1; // 15

    if (len < 12) return false;

    if (std::memcmp(buf, "EXCHANGE\nOK\n", ok_hdr_len) == 0) {
        return len >= ok_total;
    }

    if (len >= err_hdr_len &&
        std::memcmp(buf, "EXCHANGE\nERROR\n", err_hdr_len) == 0) {
        // scan for trailing newline after the header
        for (size_t i = err_hdr_len; i < len; ++i) {
            if (buf[i] == '\n') return true;
        }
        return false;
    }

    // Unknown / corrupt frame — treat as complete so we don't block forever.
    // The response will be logged as an error.
    return len >= static_cast<size_t>(OUTBOUND_BSIZE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

ClientOrchestrator::ClientOrchestrator(const int num_clients, const uint32_t rng_seed)
    : num_clients_(num_clients), rng_(rng_seed)
{
    assert(num_clients_ > 0);

    // Reserve upfront so that pointers into clients_ remain stable after
    // emplace_back — epoll stores ClientState* in ev.data.ptr.
    clients_.reserve(static_cast<size_t>(num_clients_));

    for (int i = 0; i < num_clients_; ++i) {
        ClientState& cs = clients_.emplace_back();
        cs.client_id = static_cast<uint32_t>(i + 1);
        cs.rng       = std::mt19937{rng_()};   // seed each client independently
    }
}

ClientOrchestrator::~ClientOrchestrator() {
    for (auto& cs : clients_) {
        if (cs.fd != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
            close(cs.fd);
        }
    }
    if (epoll_fd_ != -1) close(epoll_fd_);
}

// ─────────────────────────────────────────────────────────────────────────────
// run()
// ─────────────────────────────────────────────────────────────────────────────

void ClientOrchestrator::run() {
    if (!setupEpoll()) return;

    // Initiate connections for all clients
    for (auto& cs : clients_) {
        connectClient(cs);
    }

    while (running_.load(std::memory_order_relaxed)) {
        // 1 ms timeout so we can check reconnect timers and the running_ flag
        const int nfds = epoll_wait(epoll_fd_, events_, CLIENT_EPOLL_BATCH, 1);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "epoll_wait: %s\n", std::strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            auto* cs = static_cast<ClientState*>(events_[i].data.ptr);
            const uint32_t ev = events_[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                handleError(*cs);
                continue;
            }

            switch (cs->phase) {
                case ClientPhase::CONNECTING:
                    if (ev & EPOLLOUT) handleConnect(*cs);
                    break;
                case ClientPhase::SENDING:
                    if (ev & EPOLLOUT) handleWritable(*cs);
                    break;
                case ClientPhase::AWAITING:
                    if (ev & EPOLLIN)  handleReadable(*cs);
                    break;
                case ClientPhase::RECONNECT:
                    // Should not appear in epoll, but ignore safely
                    break;
            }
        }

        processPendingReconnects();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────

bool ClientOrchestrator::setupEpoll() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        std::fprintf(stderr, "epoll_create1: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

bool ClientOrchestrator::connectClient(ClientState& cs) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        std::fprintf(stderr, "socket(): %s\n", std::strerror(errno));
        scheduleReconnect(cs);
        return false;
    }

    // Disable Nagle so every request is flushed immediately.
    constexpr int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(EXCHANGE_PORT);
    if (inet_pton(AF_INET, EXCHANGE_HOST, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "inet_pton('%s'): %s\n", EXCHANGE_HOST, std::strerror(errno));
        close(fd);
        scheduleReconnect(cs);
        return false;
    }

    cs.fd    = fd;
    cs.read_len  = 0;
    cs.write_len = 0;
    cs.write_off = 0;

    const int rc = connect(fd,
                           reinterpret_cast<const sockaddr*>(&addr),
                           sizeof(addr));

    if (rc == 0) {
        // Immediate connect (loopback can do this).
        cs.phase = ClientPhase::AWAITING; // will move to AWAITING after buildAndSend
        logConnect(cs, true);
        epollAdd(cs, EPOLLIN);
        buildAndSend(cs);
    } else if (errno == EINPROGRESS) {
        // Non-blocking connect in progress — wait for EPOLLOUT.
        cs.phase = ClientPhase::CONNECTING;
        epollAdd(cs, EPOLLOUT);
    } else {
        std::fprintf(stderr, "Client %3u: connect() failed: %s\n",
                     cs.client_id, std::strerror(errno));
        close(fd);
        cs.fd = -1;
        scheduleReconnect(cs);
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// epoll event handlers
// ─────────────────────────────────────────────────────────────────────────────

void ClientOrchestrator::handleConnect(ClientState& cs) {
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);

    if (err != 0) {
        std::fprintf(stderr, "Client %3u: async connect failed: %s\n",
                     cs.client_id, std::strerror(err));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    // Connected – switch to EPOLLIN and fire first request.
    logConnect(cs, true);
    epollMod(cs, EPOLLIN);
    buildAndSend(cs);
}

void ClientOrchestrator::handleWritable(ClientState& cs) {
    flushWrite(cs);
}

void ClientOrchestrator::handleReadable(ClientState& cs) {
    // Read into remaining space in read_buf.
    const ssize_t n = recv(cs.fd,
                           cs.read_buf + cs.read_len,
                           sizeof(cs.read_buf) - cs.read_len,
                           0);

    if (n == 0) {
        // Graceful close from exchange.
        logDisconnect(cs);
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // spurious wakeup
        std::fprintf(stderr, "Client %3u: recv() error: %s\n",
                     cs.client_id, std::strerror(errno));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    cs.read_len += static_cast<size_t>(n);

    // Drain all complete responses that arrived in this recv() call
    // (handles MATCH + ACK coalescing on the socket).
    while (response_complete(cs.read_buf, cs.read_len)) {
        parseResponse(cs);

        // If parseResponse triggered a reconnect, stop draining.
        if (cs.phase == ClientPhase::RECONNECT) return;
    }
}

void ClientOrchestrator::handleError(ClientState& cs) {
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);

    std::fprintf(stderr, "Client %3u: socket error: %s\n",
                 cs.client_id, err ? std::strerror(err) : "(unknown)");
    closeClient(cs);
    scheduleReconnect(cs);
}

// ─────────────────────────────────────────────────────────────────────────────
// Core request / response logic
// ─────────────────────────────────────────────────────────────────────────────

void ClientOrchestrator::buildAndSend(ClientState& cs) {
    const OrderId target = cs.pick_active_order();

    cs.last_kind = OrderFactory::build_frame(
        cs.client_id,
        target,
        cs.write_buf,
        cs.last_msg,
        cs.rng);

    cs.write_len = INBOUND_BSIZE;
    cs.write_off = 0;

    logSend(cs);
    ++cs.requests_sent;

    flushWrite(cs);
}

void ClientOrchestrator::flushWrite(ClientState& cs) {
    while (cs.write_off < cs.write_len) {
        const ssize_t n = send(cs.fd,
                               cs.write_buf + cs.write_off,
                               cs.write_len  - cs.write_off,
                               MSG_NOSIGNAL);

        if (n > 0) {
            cs.write_off += static_cast<size_t>(n);
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer temporarily full – wait for EPOLLOUT.
            cs.phase = ClientPhase::SENDING;
            epollMod(cs, EPOLLOUT);
            return;
        }

        // Hard send error.
        std::fprintf(stderr, "Client %3u: send() error: %s\n",
                     cs.client_id, std::strerror(errno));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    // Full frame sent – wait for the exchange response.
    cs.phase    = ClientPhase::AWAITING;
    cs.read_len = 0;
    epollMod(cs, EPOLLIN);
}

void ClientOrchestrator::parseResponse(ClientState& cs) {
    const char* buf = cs.read_buf;
    const size_t len = cs.read_len;

    constexpr size_t ok_hdr_len  = sizeof("EXCHANGE\nOK\n")    - 1; // 12
    constexpr size_t ok_total    = ok_hdr_len + sizeof(ClientId) + sizeof(OrderId); // 20
    constexpr size_t err_hdr_len = sizeof("EXCHANGE\nERROR\n") - 1; // 15

    if (len >= ok_total && std::memcmp(buf, "EXCHANGE\nOK\n", ok_hdr_len) == 0) {
        // ── OK response ───────────────────────────────────────────────────────
        OrderId  oid{};
        std::memcpy(&oid, buf + ok_hdr_len + sizeof(ClientId), sizeof(OrderId));

        logOk(cs, oid);
        ++cs.responses_ok;

        // Track the assigned order ID so future CANCEL/MODIFY rolls can use it.
        // We do this for all OK responses (NEW, CANCEL echo, MODIFY echo, MATCH);
        // stale IDs produce engine-level failures, which is fine for load testing.
        if (cs.last_kind == OrderFactory::FrameKind::NEW && oid != 0) {
            cs.record_active_order(oid);
        }

        // Consume this response from the buffer and immediately send the next request.
        cs.read_len = 0;
        buildAndSend(cs);

    } else if (len >= err_hdr_len &&
               std::memcmp(buf, "EXCHANGE\nERROR\n", err_hdr_len) == 0) {
        // ── ERROR response ────────────────────────────────────────────────────
        // Find trailing '\n' to determine message bounds.
        size_t msg_end = err_hdr_len;
        while (msg_end < len && buf[msg_end] != '\n') ++msg_end;

        const char* msg_start = buf + err_hdr_len;
        const size_t msg_len  = msg_end - err_hdr_len;

        logErr(cs, msg_start, msg_len);
        ++cs.responses_err;

        // Advance past this complete response (including the trailing '\n').
        const size_t consumed = msg_end + 1; // +1 for '\n'

        if (consumed < cs.read_len) {
            // Remaining bytes belong to the next response.
            std::memmove(cs.read_buf, buf + consumed, cs.read_len - consumed);
            cs.read_len -= consumed;
        } else {
            cs.read_len = 0;
        }

        buildAndSend(cs);

    } else {
        // Unrecognised frame – log a warning and clear the buffer.
        std::fprintf(stderr, "Client %3u: [WARN] unrecognised response frame (%zu bytes)\n",
                     cs.client_id, len);
        cs.read_len = 0;
        buildAndSend(cs);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconnection
// ─────────────────────────────────────────────────────────────────────────────

void ClientOrchestrator::scheduleReconnect(ClientState& cs) {
    cs.phase = ClientPhase::RECONNECT;
    ++cs.reconnect_count;

    // Simple linear backoff capped at RECONNECT_DELAY_MS per attempt.
    const uint64_t attempt  = static_cast<uint64_t>(cs.reconnect_count) * 100ULL;
    const uint64_t cap_ms   = static_cast<uint64_t>(RECONNECT_DELAY_MS);
    const uint64_t delay_ns = (attempt < cap_ms ? attempt : cap_ms) * 1'000'000ULL;

    cs.reconnect_at_ns = monotonic_ns() + delay_ns;
}

void ClientOrchestrator::processPendingReconnects() {
    const uint64_t now = monotonic_ns();
    for (auto& cs : clients_) {
        if (cs.phase != ClientPhase::RECONNECT) continue;
        if (now >= cs.reconnect_at_ns) {
            connectClient(cs);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// epoll helpers
// ─────────────────────────────────────────────────────────────────────────────

void ClientOrchestrator::epollAdd(ClientState& cs, const uint32_t events) {
    epoll_ctrl(epoll_fd_, cs.fd, EPOLL_CTL_ADD, events, &cs);
}

void ClientOrchestrator::epollMod(ClientState& cs, const uint32_t events) {
    epoll_ctrl(epoll_fd_, cs.fd, EPOLL_CTL_MOD, events, &cs);
}

void ClientOrchestrator::closeClient(ClientState& cs) {
    if (cs.fd == -1) return;
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
    close(cs.fd);
    cs.fd = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stats
// ─────────────────────────────────────────────────────────────────────────────

ClientOrchestrator::Stats ClientOrchestrator::stats() const {
    Stats s{};
    for (const auto& cs : clients_) {
        s.requests_sent   += cs.requests_sent;
        s.responses_ok    += cs.responses_ok;
        s.responses_err   += cs.responses_err;
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────────────────────

void ClientOrchestrator::logConnect(const ClientState& cs, bool success) {
#if LOG_ENABLED
    if (success) {
        std::fprintf(stderr, "Client %3u: [CONN] connected to %s:%d\n",
                     cs.client_id, EXCHANGE_HOST, EXCHANGE_PORT);
    }
#else
    (void)cs; (void)success;
#endif
}

void ClientOrchestrator::logDisconnect(const ClientState& cs) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [DISC] server closed connection\n",
                 cs.client_id);
#else
    (void)cs;
#endif
}

void ClientOrchestrator::logSend(const ClientState& cs) {
#if LOG_ENABLED
    using K = OrderFactory::FrameKind;

    switch (cs.last_kind) {
        case K::NEW:
            std::fprintf(stderr,
                "Client %3u: [SEND] NEW    %s %s  price=%6u  qty=%6u\n",
                cs.client_id,
                type_str(cs.last_msg.order_type),
                side_str(cs.last_msg.side),
                cs.last_msg.price,
                cs.last_msg.quantity);
            break;

        case K::CANCEL:
            std::fprintf(stderr,
                "Client %3u: [SEND] CANCEL  order_id=%u\n",
                cs.client_id, cs.last_msg.order_id);
            break;

        case K::MODIFY:
            std::fprintf(stderr,
                "Client %3u: [SEND] MODIFY  order_id=%u  price=%6u  qty=%6u\n",
                cs.client_id,
                cs.last_msg.order_id,
                cs.last_msg.price,
                cs.last_msg.quantity);
            break;

        case K::GARBAGE:
            std::fprintf(stderr,
                "Client %3u: [SEND] GARBAGE\n",
                cs.client_id);
            break;

        case K::INVALID:
            std::fprintf(stderr,
                "Client %3u: [SEND] INVALID  (price=%u qty=%u)\n",
                cs.client_id,
                cs.last_msg.price,
                cs.last_msg.quantity);
            break;
    }
#else
    (void)cs;
#endif
}

void ClientOrchestrator::logOk(const ClientState& cs, const OrderId oid) {
#if LOG_ENABLED
    std::fprintf(stderr,
        "Client %3u: [OK]   %s order_id=%u\n",
        cs.client_id,
        kind_str(cs.last_kind),
        oid);
#else
    (void)cs; (void)oid;
#endif
}

void ClientOrchestrator::logErr(const ClientState& cs,
                                const char*        msg,
                                const size_t       msg_len) {
#if LOG_ENABLED
    std::fprintf(stderr,
        "Client %3u: [ERR]  %s: %.*s\n",
        cs.client_id,
        kind_str(cs.last_kind),
        static_cast<int>(msg_len),
        msg);
#else
    (void)cs; (void)msg; (void)msg_len;
#endif
}
