//
// Created by cniew on 5/24/26.
//

#include "../include/load_generator.h"
#include "../include/order_factory.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

//=============================================================================
// Response framing constants
//=============================================================================

// The server pads every outbound response to exactly OUTBOUND_BSIZE bytes
// (zero-padded), so the client always advances by exactly OUTBOUND_BSIZE per
// frame regardless of whether it is an OK or an ERROR.
static constexpr size_t FRAME_SIZE = OUTBOUND_BSIZE;   // 64

//=============================================================================
// File-local helper
//=============================================================================

// Discard n bytes from the front of read_buf.  Buffer is small (≤4 KB) so
// memmove is a single SIMD op; reset instead when empty to skip it entirely.
static inline void advanceRead(ClientState& cs, const size_t n) {
    cs.read_len -= n;
    if (cs.read_len == 0) return;               // common case: fully drained
    std::memmove(cs.read_buf, cs.read_buf + n, cs.read_len);
}

//=============================================================================
// Special member functions
//=============================================================================

LoadGenerator::LoadGenerator(const int num_clients, const uint64_t rng_seed)
    : num_clients_(num_clients)
{
    OrderFactory::instance(rng_seed);  // seed the singleton before first build_frame
    clients_.reserve(static_cast<size_t>(num_clients_));
    for (int i = 0; i < num_clients_; ++i) {
        ClientState& cs = clients_.emplace_back();
        cs.client_id = static_cast<uint32_t>(i + 1);
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
//=============================================================================

void LoadGenerator::run() {
    if (!setupEpoll()) return;
    for (auto& cs : clients_) connectClient(cs);

    while (running_.load(std::memory_order_relaxed)) {
        const int nfds = epoll_wait(epoll_fd_, events_, CLIENT_EPOLL_BATCH, 0);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "epoll_wait: %s\n", std::strerror(errno));
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            auto* cs = static_cast<ClientState*>(events_[i].data.ptr);
            const uint32_t ev = events_[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                reconnect(*cs);
                continue;
            }
            if (ev & EPOLLOUT) {
                if (cs->connecting) handleConnect(*cs);
                else                flushWrite(*cs);
                if (cs->fd == -1) continue;  // reconnect happened inside
            }
            if (ev & EPOLLIN) handleReadable(*cs);
        }
    }
}

//=============================================================================
// Setup
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
        std::fprintf(stderr, "socket: %s\n", std::strerror(errno));
        return false;
    }

    constexpr int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(EXCHANGE_PORT);
    inet_pton(AF_INET, EXCHANGE_HOST, &addr.sin_addr);

    cs.fd          = fd;
    cs.connecting  = true;
    cs.write_len   = 0;
    cs.write_off   = 0;
    cs.read_len    = 0;
    cs.acks_pending = 0;

    const int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    if (rc == 0 || errno == EINPROGRESS) {
        // Arm EPOLLOUT: fires immediately (rc==0, loopback) or on handshake
        // completion (EINPROGRESS).  handleConnect() switches to EPOLLIN.
        epollAdd(cs, EPOLLOUT);
        return true;
    }

    std::fprintf(stderr, "connect: %s\n", std::strerror(errno));
    ::close(fd); cs.fd = -1;
    return false;
}

//=============================================================================
// Event handlers
//=============================================================================

// EPOLLOUT while connecting — check for async error, then start the pipeline.
void LoadGenerator::handleConnect(ClientState& cs) {
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) { reconnect(cs); return; }

    cs.connecting = false;
    epollMod(cs, EPOLLIN);   // switch to EPOLLIN only; writes go direct via send()
    buildBatch(cs);
}

// EPOLLIN — recv() and parse every complete response, then top up the pipeline.
//
// The server pads every response to exactly FRAME_SIZE (OUTBOUND_BSIZE) bytes,
// so the client always advances by FRAME_SIZE per message.  This gives byte-
// level alignment: no matter where TCP splits a stream, we never mistake the
// tail bytes of one frame for the header of the next.
void LoadGenerator::handleReadable(ClientState& cs) {
    const ssize_t n = recv(cs.fd,
                           cs.read_buf + cs.read_len,
                           sizeof(cs.read_buf) - cs.read_len,
                           0);
    if (n == 0) { reconnect(cs); return; }
    if (n <  0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        reconnect(cs); return;
    }
    cs.read_len += static_cast<size_t>(n);

    // Drain all complete FRAME_SIZE-aligned responses.
    while (cs.read_len >= FRAME_SIZE) {
        const char* frame = cs.read_buf;

        // Validate the "EXCHANGE\n" prefix before inspecting the type byte.
        // A mismatched prefix means the server sent protocol-violating bytes.
        if (memcmp(frame, "EXCHANGE\n", 9) != 0) {
            #if LOGGING
            std::fprintf(stderr, "[WARN]  cid:%-6u :: bad response header\n",
                         static_cast<unsigned>(cs.client_id));
            #endif
            reconnect(cs);
            return;
        }

        const char type = frame[9];   // 'O' = OK, 'E' = ERROR
        if (type == 'O') {
            if (cs.acks_pending > 0) {
                --cs.acks_pending;
                --cs.orders_in_flight;
                ++cs.responses_ack;
                #if LOGGING
                {
                    ClientId log_cid; std::memcpy(&log_cid, frame + 12, sizeof(log_cid));
                    OrderId  log_oid; std::memcpy(&log_oid, frame + 16, sizeof(log_oid));
                    std::fprintf(stderr, "[OK]    cid:%-6u oid:%llu\n",
                                 static_cast<unsigned>(log_cid),
                                 static_cast<unsigned long long>(log_oid));
                }
                #endif
            } else {
                ++cs.responses_match;  // unsolicited fill notification
                #if LOGGING
                {
                    ClientId log_cid; std::memcpy(&log_cid, frame + 12, sizeof(log_cid));
                    OrderId  log_oid; std::memcpy(&log_oid, frame + 16, sizeof(log_oid));
                    std::fprintf(stderr, "[MATCH] cid:%-6u oid:%llu\n",
                                 static_cast<unsigned>(log_cid),
                                 static_cast<unsigned long long>(log_oid));
                }
                #endif
            }
        } else if (type == 'E') {
            if (cs.acks_pending > 0) { --cs.acks_pending; --cs.orders_in_flight; }
            ++cs.responses_err;
            #if LOGGING
            {
                // Error string starts after "EXCHANGE\nERROR\n" (15 bytes), ends at '\n'.
                const char* err_start = frame + 15;
                const char* nl = static_cast<const char*>(
                    std::memchr(err_start, '\n', FRAME_SIZE - 15));
                const int err_len = nl ? static_cast<int>(nl - err_start)
                                       : static_cast<int>(FRAME_SIZE - 15);
                std::fprintf(stderr, "[ERR]   cid:%-6u :: %.*s\n",
                             static_cast<unsigned>(cs.client_id), err_len, err_start);
            }
            #endif
        } else {
            // Unknown type byte — server sent something that violates the protocol.
            #if LOGGING
            std::fprintf(stderr, "[WARN]  cid:%-6u :: unknown response type '\\x%02x'\n",
                         static_cast<unsigned>(cs.client_id),
                         static_cast<unsigned char>(type));
            #endif
            reconnect(cs);
            return;
        }

        advanceRead(cs, FRAME_SIZE);
    }
    // read_len < FRAME_SIZE -> incomplete frame; wait for the next recv().

    buildBatch(cs);
}

//=============================================================================
// Pipeline
//=============================================================================

// Pack PIPELINE_DEPTH frames into write_buf and send in one syscall.
// No-op if the previous batch is still draining or the pipeline is already full.
void LoadGenerator::buildBatch(ClientState& cs) {
    if (cs.write_len > 0)                  return;  // batch still in flight
    if (cs.acks_pending == PIPELINE_DEPTH) return;  // pipeline saturated

    auto& factory = OrderFactory::instance();
    cs.write_off = 0;
    cs.write_len = 0;
    while (cs.acks_pending < PIPELINE_DEPTH) {
        InboundMessage msg{};
        factory.build_frame(cs.client_id, 0, cs.write_buf + cs.write_len, msg);
        cs.write_len    += INBOUND_BSIZE;
        ++cs.acks_pending;
        ++cs.requests_sent;
        ++cs.orders_in_flight;
    }

    flushWrite(cs);
}

// Drain write_buf to the kernel.  On success the socket stays armed EPOLLIN
// only — no epoll_ctl needed.  On EAGAIN we arm EPOLLOUT and let the event
// loop retry via the EPOLLOUT path (rare on loopback).
void LoadGenerator::flushWrite(ClientState& cs) {
    while (cs.write_off < cs.write_len) {
        const ssize_t n = send(cs.fd,
                               cs.write_buf + cs.write_off,
                               cs.write_len  - cs.write_off,
                               MSG_NOSIGNAL);
        if (n > 0) { cs.write_off += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            epollMod(cs, EPOLLIN | EPOLLOUT);  // will retry via flushWrite()
            return;
        }
        reconnect(cs);
        return;
    }
    // Fully sent.  Reset buffer; leave socket armed EPOLLIN (no epoll_ctl).
    cs.write_len = 0;
    cs.write_off = 0;
}

//=============================================================================
// Reconnect
//=============================================================================

void LoadGenerator::reconnect(ClientState& cs) {
    if (cs.fd == -1) return;
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
    ::close(cs.fd);
    cs.fd = -1;
    // Pending acks will never arrive; correct the in-flight counter.
    cs.orders_in_flight -= static_cast<int64_t>(cs.acks_pending);
    cs.acks_pending = 0;
    connectClient(cs);
}

//=============================================================================
// epoll helpers
//=============================================================================

void LoadGenerator::epollAdd(ClientState& cs, const uint32_t events) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = &cs;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cs.fd, &ev);
}

void LoadGenerator::epollMod(ClientState& cs, const uint32_t events) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = &cs;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, cs.fd, &ev);
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
