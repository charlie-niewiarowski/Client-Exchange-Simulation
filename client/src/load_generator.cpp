//
// Created by cniew on 5/24/26.
//

#include "../include/load_generator.h"
#include "../config/config.h"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

//=============================================================================
// File-local forward declarations
//=============================================================================

static uint64_t monotonic_ns();
static int      epoll_ctrl(int epoll_fd, int fd, int op, uint32_t events, void* ptr);
static void     advance_read_buf(ClientState& cs, size_t n);
static bool     response_complete(const char* buf, size_t len);

#if LOG_ENABLED
static const char* side_str(Side s);
static const char* type_str(OrderType t);
static const char* kind_str(OrderFactory::FrameKind k);
#endif

//=============================================================================
// Special member functions
//=============================================================================

LoadGenerator::LoadGenerator(const int num_clients, const uint32_t rng_seed)
    : num_clients_(num_clients), rng_(rng_seed)
{
    assert(num_clients_ > 0);

#if EXPECTED_THROUGHPUT > 0
    // Closed-loop rate cap: spread evenly across clients.
    //   inter_arrival_ns = num_clients * 1e9 / EXPECTED_THROUGHPUT
    inter_arrival_ns_ = static_cast<uint64_t>(num_clients_) * 1'000'000'000ULL
                        / static_cast<uint64_t>(EXPECTED_THROUGHPUT);
#endif

    // Reserve before emplace_back so epoll's ClientState* pointers stay stable.
    clients_.reserve(static_cast<size_t>(num_clients_));
    for (int i = 0; i < num_clients_; ++i) {
        ClientState& cs = clients_.emplace_back();
        cs.client_id    = static_cast<uint32_t>(i + 1);
        cs.rng          = std::mt19937{rng_()};
    }
}

LoadGenerator::~LoadGenerator() {
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
//
// Throughput ceiling (unpipelined): clients / RTT.
//
// Hypotheses for why the client bottlenecks before the exchange:
//
//  1. Syscall overhead: each unpipelined round-trip crosses the kernel 2–3×
//     (send, recv, and occasionally epoll_ctl).  At ~100–200 ns each that is a
//     300–600 ns floor per request.  PIPELINE_DEPTH amortises this: one send()
//     covers PIPELINE_DEPTH frames, and recv() may return multiple responses.
//
//  2. epoll_wait wakeup latency: even when data is ready, epoll_wait has a
//     context-switch / cache-refill cost on every return.  Pipelining amortises
//     this over PIPELINE_DEPTH responses per wakeup.
//
//  3. epoll_ctl on the fast path: avoided by checking cs.phase == SENDING
//     before calling epollMod(EPOLLIN) in flushWrite.
//
//  4. RNG + frame construction: lightweight per frame; negligible once
//     amortised across a batch.
//=============================================================================

void LoadGenerator::run() {
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

            if (ev & (EPOLLERR | EPOLLHUP)) { handleError(*cs); continue; }

            switch (cs->phase) {
                case ClientPhase::CONNECTING: if (ev & EPOLLOUT) handleConnect (*cs); break;
                case ClientPhase::SENDING:    if (ev & EPOLLOUT) flushWrite    (*cs); break;
                case ClientPhase::AWAITING:   if (ev & EPOLLIN)  handleReadable(*cs); break;
                case ClientPhase::RECONNECT:  break;
            }
        }

        processPendingReconnects();
    }
}

//=============================================================================
// Run helpers
//=============================================================================

bool LoadGenerator::setupEpoll() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        std::fprintf(stderr, "epoll_create1: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

bool LoadGenerator::connectClient(ClientState& cs) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        std::fprintf(stderr, "socket(): %s\n", std::strerror(errno));
        scheduleReconnect(cs);
        return false;
    }

    // Disable Nagle — every small request frame goes out immediately.
    constexpr int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(EXCHANGE_PORT);
    if (inet_pton(AF_INET, EXCHANGE_HOST, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "inet_pton('%s'): invalid address\n", EXCHANGE_HOST);
        ::close(fd);
        scheduleReconnect(cs);
        return false;
    }

    // Reset pipeline state — any in-flight requests from the previous session
    // will never receive responses; adjust the in-flight counter accordingly.
    cs.orders_in_flight -= static_cast<int64_t>(cs.acks_pending);
    cs.acks_pending = 0;
    cs.ring_head    = 0;
    cs.ring_tail    = 0;
    cs.fd           = fd;
    cs.read_len     = 0;
    cs.write_len    = 0;
    cs.write_off    = 0;

    const int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    if (rc == 0) {
        // Immediate connect (common on loopback).
        cs.phase = ClientPhase::AWAITING;
        logConnect(cs);
        epollAdd(cs, EPOLLIN);
        buildBatch(cs);
    } else if (errno == EINPROGRESS) {
        cs.phase = ClientPhase::CONNECTING;
        epollAdd(cs, EPOLLOUT);
    } else {
        std::fprintf(stderr, "Client %3u: connect(): %s\n", cs.client_id, std::strerror(errno));
        ::close(fd); cs.fd = -1;
        scheduleReconnect(cs);
        return false;
    }

    return true;
}

void LoadGenerator::handleConnect(ClientState& cs) {
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
    buildBatch(cs);
}

// Hot path — called on every inbound response batch.
// Drains all coalesced responses from one recv(), then refills the pipeline.
void LoadGenerator::handleReadable(ClientState& cs) {
    const ssize_t n = recv(cs.fd,
                           cs.read_buf + cs.read_len,
                           sizeof(cs.read_buf) - cs.read_len,
                           0);
    if (n == 0) { logDisconnect(cs); closeClient(cs); scheduleReconnect(cs); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        std::fprintf(stderr, "Client %3u: recv(): %s\n", cs.client_id, std::strerror(errno));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    cs.read_len += static_cast<size_t>(n);

    while (response_complete(cs.read_buf, cs.read_len)) {
        consumeResponse(cs);
        if (cs.phase == ClientPhase::RECONNECT) return;
    }

    // Refill the pipeline: build and send however many new frames fit.
    buildBatch(cs);
}

void LoadGenerator::handleError(ClientState& cs) {
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    std::fprintf(stderr, "Client %3u: socket error: %s\n",
                 cs.client_id, err ? std::strerror(err) : "(unknown)");
    closeClient(cs);
    scheduleReconnect(cs);
}

void LoadGenerator::processPendingReconnects() {
    const uint64_t now = monotonic_ns();
    for (auto& cs : clients_) {
        if (cs.phase == ClientPhase::RECONNECT && now >= cs.reconnect_at_ns) {
            connectClient(cs);
        }
#if EXPECTED_THROUGHPUT > 0
        else if (cs.send_pending && now >= cs.next_send_ns) {
            cs.send_pending = false;
            buildBatch(cs);
        }
#endif
    }
}

//=============================================================================
// handleReadable helpers — hot path
//=============================================================================

void LoadGenerator::consumeResponse(ClientState& cs) {
    const char*  buf = cs.read_buf;
    const size_t len = cs.read_len;

    constexpr size_t OK_HDR  = sizeof("EXCHANGE\nOK\n")    - 1; // 12
    constexpr size_t OK_TOT  = OK_HDR + sizeof(ClientId) + sizeof(OrderId); // 20
    constexpr size_t ERR_HDR = sizeof("EXCHANGE\nERROR\n") - 1; // 15

    if (len >= OK_TOT && std::memcmp(buf, "EXCHANGE\nOK\n", OK_HDR) == 0) {
        OrderId oid{};
        std::memcpy(&oid, buf + OK_HDR + sizeof(ClientId), sizeof(oid));

        if (cs.acks_pending > 0) {
            // This OK is the direct ACK for the oldest in-flight request.
            cs.last_kind = cs.pipe_ring[cs.ring_tail % PIPELINE_DEPTH].kind;
            ++cs.ring_tail;
            --cs.acks_pending;
            --cs.orders_in_flight;
            logAck(cs, oid);
            ++cs.responses_ack;
            if (cs.last_kind == OrderFactory::FrameKind::NEW && oid != 0)
                cs.record_active_order(oid);
        } else {
            // No request in flight — unsolicited MATCH notification.
            logMatch(cs, oid);
            ++cs.responses_match;
        }
        advance_read_buf(cs, OK_TOT);

    } else if (len >= ERR_HDR && std::memcmp(buf, "EXCHANGE\nERROR\n", ERR_HDR) == 0) {
        size_t msg_end = ERR_HDR;
        while (msg_end < len && buf[msg_end] != '\n') ++msg_end;

        cs.last_kind = cs.pipe_ring[cs.ring_tail % PIPELINE_DEPTH].kind;
        ++cs.ring_tail;
        --cs.acks_pending;
        logErr(cs, buf + ERR_HDR, msg_end - ERR_HDR);
        ++cs.responses_err;
        --cs.orders_in_flight;
        advance_read_buf(cs, msg_end + 1);

    } else {
        std::fprintf(stderr, "Client %3u: [WARN] unrecognised response (%zu bytes)\n",
                     cs.client_id, len);
        if (cs.acks_pending > 0) {
            ++cs.ring_tail;
            --cs.acks_pending;
        }
        --cs.orders_in_flight;
        advance_read_buf(cs, std::min(len, static_cast<size_t>(OUTBOUND_BSIZE)));
    }
}

// Append one frame to write_buf and record it in the pipeline ring.
// Caller must ensure acks_pending < PIPELINE_DEPTH and write_len has room.
void LoadGenerator::buildOne(ClientState& cs) {
    auto& slot = cs.pipe_ring[cs.ring_head % PIPELINE_DEPTH];
    slot.kind  = OrderFactory::build_frame(
        cs.client_id, cs.pick_active_order(),
        cs.write_buf + cs.write_len, slot.msg, cs.rng);
    cs.write_len += INBOUND_BSIZE;
    ++cs.ring_head;
    ++cs.acks_pending;
    ++cs.requests_sent;
    ++cs.orders_in_flight;

    // Update logging cache before logSend so it reflects this frame.
    cs.last_kind = slot.kind;
    cs.last_msg  = slot.msg;
    logSend(cs);
}

// Fill the pipeline up to PIPELINE_DEPTH frames and flush to the socket.
// Guards:
//   write_len > 0  — a previous batch is still being sent; don't overwrite.
//   acks_pending == PIPELINE_DEPTH  — pipeline already full; nothing to add.
void LoadGenerator::buildBatch(ClientState& cs) {
    if (cs.write_len > 0)                    return;
    if (cs.acks_pending == PIPELINE_DEPTH)   return;

#if EXPECTED_THROUGHPUT > 0
    if (monotonic_ns() < cs.next_send_ns) {
        cs.send_pending = true;
        return;
    }
    cs.next_send_ns = monotonic_ns() + inter_arrival_ns_;
    cs.send_pending = false;
#endif

    while (cs.acks_pending < PIPELINE_DEPTH)
        buildOne(cs);

    flushWrite(cs);
}

// Drains write_buf to the kernel.
// On EAGAIN: arms EPOLLOUT and returns; run() re-enters via SENDING.
// On success: resets the write buffer (so buildBatch can refill it) and
//   switches back to EPOLLIN only when leaving SENDING, saving one epoll_ctl
//   syscall per round-trip on the fast path (send rarely blocks on loopback).
void LoadGenerator::flushWrite(ClientState& cs) {
    while (cs.write_off < cs.write_len) {
        const ssize_t n = send(cs.fd,
                               cs.write_buf + cs.write_off,
                               cs.write_len  - cs.write_off,
                               MSG_NOSIGNAL);
        if (n > 0) { cs.write_off += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            cs.phase = ClientPhase::SENDING;
            epollMod(cs, EPOLLOUT);
            return;
        }
        std::fprintf(stderr, "Client %3u: send(): %s\n", cs.client_id, std::strerror(errno));
        closeClient(cs);
        scheduleReconnect(cs);
        return;
    }

    // Batch fully sent — reset buffer so buildBatch can pack the next one.
    cs.write_len = 0;
    cs.write_off = 0;

    // Only call epollMod when switching back from SENDING; saves one epoll_ctl
    // per round-trip on the fast path.
    if (cs.phase == ClientPhase::SENDING)
        epollMod(cs, EPOLLIN);
    cs.phase = ClientPhase::AWAITING;
}

//=============================================================================
// Connection helpers
//=============================================================================

void LoadGenerator::scheduleReconnect(ClientState& cs) {
    cs.phase = ClientPhase::RECONNECT;
    ++cs.reconnect_count;

    const uint64_t    attempt_ms = static_cast<uint64_t>(cs.reconnect_count) * 100ULL;
    constexpr uint64_t cap_ms    = static_cast<uint64_t>(RECONNECT_DELAY_MS);
    cs.reconnect_at_ns = monotonic_ns()
                       + (attempt_ms < cap_ms ? attempt_ms : cap_ms) * 1'000'000ULL;
}

void LoadGenerator::closeClient(ClientState& cs) {
    if (cs.fd == -1) return;
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
    ::close(cs.fd);
    cs.fd = -1;
}

void LoadGenerator::epollAdd(ClientState& cs, const uint32_t events) {
    epoll_ctrl(epoll_fd_, cs.fd, EPOLL_CTL_ADD, events, &cs);
}

void LoadGenerator::epollMod(ClientState& cs, const uint32_t events) {
    epoll_ctrl(epoll_fd_, cs.fd, EPOLL_CTL_MOD, events, &cs);
}

//=============================================================================
// Stats
//=============================================================================

LoadGenerator::Stats LoadGenerator::stats() const {
    Stats s{};
    for (const auto& cs : clients_) {
        s.requests_sent    += cs.requests_sent;
        s.responses_ack    += cs.responses_ack;
        s.responses_match  += cs.responses_match;
        s.responses_err    += cs.responses_err;
        s.orders_in_flight += cs.orders_in_flight;
    }
    return s;
}

//=============================================================================
// Logging  (entire bodies stripped by preprocessor when LOG_ENABLED == 0)
//=============================================================================

void LoadGenerator::logConnect([[maybe_unused]] const ClientState& cs) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [CONN] connected to %s:%d\n",
                 cs.client_id, EXCHANGE_HOST, EXCHANGE_PORT);
#endif
}

void LoadGenerator::logDisconnect([[maybe_unused]] const ClientState& cs) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [DISC] server closed connection\n", cs.client_id);
#endif
}

void LoadGenerator::logSend([[maybe_unused]] const ClientState& cs) {
#if LOG_ENABLED
    using K = OrderFactory::FrameKind;
    switch (cs.last_kind) {
        case K::NEW:
            std::fprintf(stderr, "Client %3u: [SEND] NEW    %s %s  price=%6u  qty=%6u\n",
                cs.client_id, type_str(cs.last_msg.order_type), side_str(cs.last_msg.side),
                cs.last_msg.price, cs.last_msg.quantity);
            break;
        case K::CANCEL:
            std::fprintf(stderr, "Client %3u: [SEND] CANCEL  order_id=%u\n",
                cs.client_id, cs.last_msg.order_id);
            break;
        case K::MODIFY:
            std::fprintf(stderr, "Client %3u: [SEND] MODIFY  order_id=%u  price=%6u  qty=%6u\n",
                cs.client_id, cs.last_msg.order_id, cs.last_msg.price, cs.last_msg.quantity);
            break;
        case K::GARBAGE:
            std::fprintf(stderr, "Client %3u: [SEND] GARBAGE\n", cs.client_id);
            break;
        case K::INVALID:
            std::fprintf(stderr, "Client %3u: [SEND] INVALID  (price=%u qty=%u)\n",
                cs.client_id, cs.last_msg.price, cs.last_msg.quantity);
            break;
    }
#endif
}

void LoadGenerator::logAck([[maybe_unused]] const ClientState& cs,
                           [[maybe_unused]] const OrderId      oid) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [ACK]  %s  order_id=%llu\n",
                 cs.client_id, kind_str(cs.last_kind),
                 static_cast<unsigned long long>(oid));
#endif
}

void LoadGenerator::logMatch([[maybe_unused]] const ClientState& cs,
                             [[maybe_unused]] const OrderId      oid) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [MATCH] order_id=%llu\n",
                 cs.client_id, static_cast<unsigned long long>(oid));
#endif
}

void LoadGenerator::logErr([[maybe_unused]] const ClientState& cs,
                           [[maybe_unused]] const char*        msg,
                           [[maybe_unused]] const size_t       msg_len) {
#if LOG_ENABLED
    std::fprintf(stderr, "Client %3u: [ERR]  %s: %.*s\n",
                 cs.client_id, kind_str(cs.last_kind),
                 static_cast<int>(msg_len), msg);
#endif
}

//=============================================================================
// File-local helper impls
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

// Consume n bytes from the front of cs.read_buf, shifting the remainder left.
static void advance_read_buf(ClientState& cs, size_t n) {
    if (n >= cs.read_len) { cs.read_len = 0; return; }
    std::memmove(cs.read_buf, cs.read_buf + n, cs.read_len - n);
    cs.read_len -= n;
}

// True when read_buf[0..len) holds a complete exchange response.
//   OK  — 20 bytes ("EXCHANGE\nOK\n" + ClientId + OrderId)
//   ERR — variable ("EXCHANGE\nERROR\n" + message + '\n')
//   Other — complete once OUTBOUND_BSIZE bytes accumulate (stall guard).
static bool response_complete(const char* buf, size_t len) {
    constexpr size_t OK_HDR  = sizeof("EXCHANGE\nOK\n")    - 1; // 12
    constexpr size_t OK_TOT  = OK_HDR + sizeof(ClientId) + sizeof(OrderId); // 20
    constexpr size_t ERR_HDR = sizeof("EXCHANGE\nERROR\n") - 1; // 15

    if (len < OK_HDR) return false;
    if (std::memcmp(buf, "EXCHANGE\nOK\n",    OK_HDR) == 0) return len >= OK_TOT;
    if (len >= ERR_HDR &&
        std::memcmp(buf, "EXCHANGE\nERROR\n", ERR_HDR) == 0) {
        for (size_t i = ERR_HDR; i < len; ++i)
            if (buf[i] == '\n') return true;
        return false;
    }
    return len >= static_cast<size_t>(OUTBOUND_BSIZE);
}

#if LOG_ENABLED
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
#endif
