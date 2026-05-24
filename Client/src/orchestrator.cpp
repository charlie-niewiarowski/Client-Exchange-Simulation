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

//=============================================================================
// File-local helpers
//=============================================================================

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

// Consume `n` bytes from the front of cs.read_buf, shifting the remainder left.
static void advance_read_buf(ClientState& cs, size_t n) {
    if (n >= cs.read_len) {
        cs.read_len = 0;
    } else {
        std::memmove(cs.read_buf, cs.read_buf + n, cs.read_len - n);
        cs.read_len -= n;
    }
}

// Returns true when read_buf[0..len) contains a complete exchange response.
//
// OK  response: exactly 20 bytes  ("EXCHANGE\nOK\n"    + ClientId + OrderId)
// ERR response: variable length   ("EXCHANGE\nERROR\n" + message  + '\n')
//
// Any other content: treat as complete once 64 bytes have accumulated so that
// a corrupted / unexpected frame does not stall the connection forever.
static bool response_complete(const char* buf, size_t len) {
    constexpr size_t OK_HDR  = sizeof("EXCHANGE\nOK\n")    - 1; // 12
    constexpr size_t OK_TOT  = OK_HDR + sizeof(ClientId) + sizeof(OrderId); // 20
    constexpr size_t ERR_HDR = sizeof("EXCHANGE\nERROR\n") - 1; // 15

    if (len < OK_HDR) return false;

    if (std::memcmp(buf, "EXCHANGE\nOK\n", OK_HDR) == 0)
        return len >= OK_TOT;

    if (len >= ERR_HDR && std::memcmp(buf, "EXCHANGE\nERROR\n", ERR_HDR) == 0) {
        for (size_t i = ERR_HDR; i < len; ++i)
            if (buf[i] == '\n') return true;
        return false;
    }

    return len >= static_cast<size_t>(OUTBOUND_BSIZE);
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

//=============================================================================
// Constructor / Destructor
//=============================================================================

ClientOrchestrator::ClientOrchestrator(const int num_clients,
                                       const uint32_t rng_seed)
    : num_clients_(num_clients), rng_(rng_seed)
{
    assert(num_clients_ > 0);

    // Pre-reserve so pointers into clients_ remain stable after emplace_back
    // (epoll stores ClientState* in ev.data.ptr).
    clients_.reserve(static_cast<size_t>(num_clients_));

    for (int i = 0; i < num_clients_; ++i) {
        ClientState& cs = clients_.emplace_back();
        cs.client_id    = static_cast<uint32_t>(i + 1);
        cs.rng          = std::mt19937{rng_()};
    }
}

ClientOrchestrator::~ClientOrchestrator() {
    for (auto& cs : clients_) {
        if (cs.fd != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
            ::close(cs.fd);
        }
    }
    if (epoll_fd_ != -1) ::close(epoll_fd_);
}

//=============================================================================
// run()
//=============================================================================

void ClientOrchestrator::run() {
    if (!setupEpoll()) return;

    for (auto& cs : clients_) connectClient(cs);

    while (running_.load(std::memory_order_relaxed)) {
        const int nfds = epoll_wait(epoll_fd_, events_, CLIENT_EPOLL_BATCH, 1);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "epoll_wait: %s\n", std::strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            auto*          cs = static_cast<ClientState*>(events_[i].data.ptr);
            const uint32_t ev = events_[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) { handleError(*cs);    continue; }

            switch (cs->phase) {
                case ClientPhase::CONNECTING: if (ev & EPOLLOUT) handleConnect (*cs); break;
                case ClientPhase::SENDING:    if (ev & EPOLLOUT) handleWritable(*cs); break;
                case ClientPhase::AWAITING:   if (ev & EPOLLIN)  handleReadable(*cs); break;
                case ClientPhase::RECONNECT:  break;
            }
        }

        processPendingReconnects();
    }
}

//=============================================================================
// Setup
//=============================================================================

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

    // Disable Nagle so every small request frame is sent immediately.
    constexpr int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(EXCHANGE_PORT);
    if (inet_pton(AF_INET, EXCHANGE_HOST, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "inet_pton('%s'): invalid address\n", EXCHANGE_HOST);
        ::close(fd);
        scheduleReconnect(cs);
        return false;
    }

    cs.fd        = fd;
    cs.read_len  = 0;
    cs.write_len = 0;
    cs.write_off = 0;

    const int rc = connect(fd,
                           reinterpret_cast<const sockaddr*>(&addr),
                           sizeof(addr));

    if (rc == 0) {
        // Immediate connect success (common on loopback).
        cs.phase = ClientPhase::AWAITING;
        logConnect(cs);
        epollAdd(cs, EPOLLIN);
        buildAndSend(cs);
    } else if (errno == EINPROGRESS) {
        cs.phase = ClientPhase::CONNECTING;
        epollAdd(cs, EPOLLOUT);
    } else {
        std::fprintf(stderr, "Client %3u: connect(): %s\n",
                     cs.client_id, std::strerror(errno));
        ::close(fd);
        cs.fd = -1;
        scheduleReconnect(cs);
        return false;
    }

    return true;
}

//=============================================================================
// epoll event handlers
//=============================================================================

void ClientOrchestrator::handleConnect(ClientState& cs) {
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);

    if (err != 0) {
        std::fprintf(stderr, "Client %3u: async connect failed: %s\n",
                     cs.client_id, std::strerror(err));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    logConnect(cs);
    cs.phase = ClientPhase::AWAITING;
    epollMod(cs, EPOLLIN);
    buildAndSend(cs);
}

void ClientOrchestrator::handleWritable(ClientState& cs) {
    flushWrite(cs);
}

void ClientOrchestrator::handleReadable(ClientState& cs) {
    const ssize_t n = recv(cs.fd,
                           cs.read_buf + cs.read_len,
                           sizeof(cs.read_buf) - cs.read_len,
                           0);

    if (n == 0) {
        logDisconnect(cs);
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        std::fprintf(stderr, "Client %3u: recv(): %s\n",
                     cs.client_id, std::strerror(errno));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    cs.read_len += static_cast<size_t>(n);

    // ── Drain all complete responses from the buffer ──────────────────────────
    //
    // Multiple responses can coalesce into one recv() call:
    //   • The direct ACK for the last sent request, plus
    //   • MATCH notifications for existing orders filled by other clients.
    //
    // consumeResponse() parses exactly one response and *advances* read_buf
    // (via memmove) past the consumed bytes so the remaining bytes are visible
    // on the next iteration.  We then fire ONE new request after the loop.
    //
    bool had_response = false;
    while (response_complete(cs.read_buf, cs.read_len)) {
        consumeResponse(cs);
        if (cs.phase == ClientPhase::RECONNECT) return;
        had_response = true;
    }

    // Send exactly one new request after draining all available responses.
    // This keeps write_buf stable (no overwrite while partially sent) and
    // keeps the request count from growing unboundedly if many MATCH events
    // arrive in a burst.
    if (had_response) buildAndSend(cs);
}

void ClientOrchestrator::handleError(ClientState& cs) {
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    std::fprintf(stderr, "Client %3u: socket error: %s\n",
                 cs.client_id,
                 err ? std::strerror(err) : "(unknown)");
    closeClient(cs);
    scheduleReconnect(cs);
}

//=============================================================================
// Core request / response logic
//=============================================================================

void ClientOrchestrator::buildAndSend(ClientState& cs) {
    cs.last_kind = OrderFactory::build_frame(
        cs.client_id,
        cs.pick_active_order(),
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
            cs.phase = ClientPhase::SENDING;
            epollMod(cs, EPOLLOUT);
            return;
        }
        std::fprintf(stderr, "Client %3u: send(): %s\n",
                     cs.client_id, std::strerror(errno));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    // Full frame sent — register for the response.
    cs.phase = ClientPhase::AWAITING;
    epollMod(cs, EPOLLIN);
}

// Parse one complete response from read_buf, log it, and advance read_len.
// Does NOT call buildAndSend — that is the caller's responsibility.
void ClientOrchestrator::consumeResponse(ClientState& cs) {
    const char* buf = cs.read_buf;
    const size_t len = cs.read_len;

    constexpr size_t OK_HDR  = sizeof("EXCHANGE\nOK\n")    - 1; // 12
    constexpr size_t OK_TOT  = OK_HDR + sizeof(ClientId) + sizeof(OrderId); // 20
    constexpr size_t ERR_HDR = sizeof("EXCHANGE\nERROR\n") - 1; // 15

    if (len >= OK_TOT && std::memcmp(buf, "EXCHANGE\nOK\n", OK_HDR) == 0) {
        // ── OK response ───────────────────────────────────────────────────────
        OrderId oid{};
        std::memcpy(&oid, buf + OK_HDR + sizeof(ClientId), sizeof(oid));

        logOk(cs, oid);
        ++cs.responses_ok;

        // Track order IDs assigned to NEW requests for future CANCEL / MODIFY.
        // We only do this when the last request was a NEW to avoid adding
        // MATCH / CANCEL echo IDs to the active set prematurely.
        if (cs.last_kind == OrderFactory::FrameKind::NEW && oid != 0)
            cs.record_active_order(oid);

        // KEY FIX: advance past exactly OK_TOT bytes, leaving any remaining
        // bytes (e.g. a coalesced MATCH notification) intact for the next
        // iteration of the drain loop.
        advance_read_buf(cs, OK_TOT);

    } else if (len >= ERR_HDR &&
               std::memcmp(buf, "EXCHANGE\nERROR\n", ERR_HDR) == 0) {
        // ── ERROR response ────────────────────────────────────────────────────
        size_t msg_end = ERR_HDR;
        while (msg_end < len && buf[msg_end] != '\n') ++msg_end;

        logErr(cs, buf + ERR_HDR, msg_end - ERR_HDR);
        ++cs.responses_err;

        advance_read_buf(cs, msg_end + 1); // +1 for the trailing '\n'

    } else {
        // ── Unrecognised frame ────────────────────────────────────────────────
        // Should not happen in normal operation.  Clear up to OUTBOUND_BSIZE
        // bytes so we don't stall.
        std::fprintf(stderr,
            "Client %3u: [WARN] unrecognised response frame (%zu bytes)\n",
            cs.client_id, len);
        advance_read_buf(cs, std::min(len, static_cast<size_t>(OUTBOUND_BSIZE)));
    }
}

//=============================================================================
// Reconnection
//=============================================================================

void ClientOrchestrator::scheduleReconnect(ClientState& cs) {
    cs.phase = ClientPhase::RECONNECT;
    ++cs.reconnect_count;

    const uint64_t attempt_ms = static_cast<uint64_t>(cs.reconnect_count) * 100ULL;
    const uint64_t cap_ms     = static_cast<uint64_t>(RECONNECT_DELAY_MS);
    const uint64_t delay_ns   = (attempt_ms < cap_ms ? attempt_ms : cap_ms)
                                * 1'000'000ULL;

    cs.reconnect_at_ns = monotonic_ns() + delay_ns;
}

void ClientOrchestrator::processPendingReconnects() {
    const uint64_t now = monotonic_ns();
    for (auto& cs : clients_) {
        if (cs.phase == ClientPhase::RECONNECT && now >= cs.reconnect_at_ns)
            connectClient(cs);
    }
}

//=============================================================================
// epoll helpers
//=============================================================================

void ClientOrchestrator::epollAdd(ClientState& cs, const uint32_t events) {
    epoll_ctrl(epoll_fd_, cs.fd, EPOLL_CTL_ADD, events, &cs);
}

void ClientOrchestrator::epollMod(ClientState& cs, const uint32_t events) {
    epoll_ctrl(epoll_fd_, cs.fd, EPOLL_CTL_MOD, events, &cs);
}

void ClientOrchestrator::closeClient(ClientState& cs) {
    if (cs.fd == -1) return;
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
    ::close(cs.fd);
    cs.fd = -1;
}

//=============================================================================
// Stats
//=============================================================================

ClientOrchestrator::Stats ClientOrchestrator::stats() const {
    Stats s{};
    for (const auto& cs : clients_) {
        s.requests_sent += cs.requests_sent;
        s.responses_ok  += cs.responses_ok;
        s.responses_err += cs.responses_err;
    }
    return s;
}

//=============================================================================
// Logging
//=============================================================================

void ClientOrchestrator::logConnect(const ClientState& cs) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [CONN] connected to %s:%d\n",
                 cs.client_id, EXCHANGE_HOST, EXCHANGE_PORT);
#else
    (void)cs;
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
                "Client %3u: [SEND] GARBAGE\n", cs.client_id);
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
        "Client %3u: [OK]   %s  order_id=%u\n",
        cs.client_id, kind_str(cs.last_kind), oid);
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
        cs.client_id, kind_str(cs.last_kind),
        static_cast<int>(msg_len), msg);
#else
    (void)cs; (void)msg; (void)msg_len;
#endif
}
