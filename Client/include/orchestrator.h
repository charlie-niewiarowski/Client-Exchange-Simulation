//
// Created by cniew on 5/24/26.
//
// ClientOrchestrator
// ──────────────────
// Manages N simulated client connections using a single epoll event loop.
// Every client is one TCP socket + a ClientState; the loop is single-threaded
// (no shared mutable state between clients).
//
// Request / response cycle
// ────────────────────────
//  1. Non-blocking connect() per client at startup.
//  2. On connect completion  (EPOLLOUT in CONNECTING): send first request.
//  3. On recv() returning data (EPOLLIN  in AWAITING):
//       a. Drain ALL complete responses from the read buffer via memmove.
//          (Multiple responses can arrive in one recv(), e.g. a MATCH
//          notification coalescing with an ACK.)
//       b. After the drain loop, send exactly ONE new request.
//  4. On partial send        (EPOLLOUT in SENDING):   flush remaining bytes.
//  5. On error / EOF:        close fd, schedule reconnect with linear backoff.
//
// Logging (stderr, controlled by LOG_ENABLED in config.h):
//
//   Client   7: [SEND] NEW    LIMIT BID  price=  150  qty=   20
//   Client   7: [OK]   NEW    order_id=17
//   Client   7: [SEND] CANCEL  order_id=17
//   Client   7: [OK]   CANCEL  order_id=17
//   Client   7: [SEND] GARBAGE
//   Client   7: [ERR]  GARB  : malformed request
//   Client   7: [SEND] INVALID  (price=0 qty=100)
//   Client   7: [ERR]  INVAL : invalid order
//

#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "client_state.h"

#include <atomic>
#include <vector>
#include <sys/epoll.h>

class ClientOrchestrator {
public:
    explicit ClientOrchestrator(int num_clients,
                                uint32_t rng_seed = std::random_device{}());

    ClientOrchestrator(const ClientOrchestrator&)            = delete;
    ClientOrchestrator& operator=(const ClientOrchestrator&) = delete;
    ClientOrchestrator(ClientOrchestrator&&)                 = delete;
    ClientOrchestrator& operator=(ClientOrchestrator&&)      = delete;

    ~ClientOrchestrator();

    // Blocks until stop() is called.
    void run();
    void stop() { running_.store(false, std::memory_order_relaxed); }

    struct Stats {
        uint64_t requests_sent;
        uint64_t responses_ok;
        uint64_t responses_err;
    };
    [[nodiscard]] Stats stats() const;

private:
    int                      num_clients_;
    int                      epoll_fd_ = -1;
    std::vector<ClientState> clients_;    // stable after ctor (pre-reserved)
    std::mt19937             rng_;
    std::atomic<bool>        running_{true};
    epoll_event              events_[CLIENT_EPOLL_BATCH]{};

    // ── Setup ─────────────────────────────────────────────────────────────────
    bool setupEpoll();
    bool connectClient(ClientState& cs);

    // ── epoll event handlers ──────────────────────────────────────────────────
    void handleConnect (ClientState& cs);  // EPOLLOUT while CONNECTING
    void handleWritable(ClientState& cs);  // EPOLLOUT while SENDING
    void handleReadable(ClientState& cs);  // EPOLLIN  while AWAITING
    void handleError   (ClientState& cs);  // EPOLLERR | EPOLLHUP

    // ── Core request / response logic ─────────────────────────────────────────
    void buildAndSend   (ClientState& cs);  // pick random frame + start send
    void flushWrite     (ClientState& cs);  // (re)try sending write_buf
    void consumeResponse(ClientState& cs);  // parse one complete response from
                                            // read_buf, advance read_len

    // ── Reconnection ──────────────────────────────────────────────────────────
    void scheduleReconnect(ClientState& cs);
    void processPendingReconnects();

    // ── epoll wrappers ────────────────────────────────────────────────────────
    void epollAdd(ClientState& cs, uint32_t events);
    void epollMod(ClientState& cs, uint32_t events);
    void closeClient (ClientState& cs);

    // ── Logging ───────────────────────────────────────────────────────────────
    static void logConnect   (const ClientState& cs);
    static void logDisconnect(const ClientState& cs);
    static void logSend      (const ClientState& cs);
    static void logOk        (const ClientState& cs, OrderId oid);
    static void logErr       (const ClientState& cs, const char* msg, size_t len);
};

#endif // ORCHESTRATOR_H
