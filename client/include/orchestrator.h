//
// Created by cniew on 5/24/26.
//
// ClientOrchestrator
// ──────────────────
// Manages N simulated client connections to the exchange using a single
// epoll-based event loop (one thread, no shared mutable state between clients).
//
// Each client is represented by a ClientState and an fd registered with epoll.
// The orchestrator drives the request-response cycle:
//
//   1. Non-blocking connect() for every client.
//   2. On connect completion (EPOLLOUT): build + send first random request.
//   3. On response arrival (EPOLLIN): log the response, immediately build and
//      send the next request.
//   4. On partial send (EPOLLOUT): drain the remaining bytes.
//   5. On error / server close: close fd, schedule reconnect with backoff.
//
// Logging (stderr, one line per event, controlled by LOG_ENABLED in config.h):
//
//   Client   7: [SEND] NEW   LIMIT BID  price=  150 qty=   20
//   Client   7: [OK]   NEW   order_id=17
//   Client   7: [SEND] CANCEL order_id=17
//   Client   7: [OK]   CANCEL order_id=17
//   Client   7: [SEND] GARBAGE
//   Client   7: [ERR]  malformed request
//   Client   7: [SEND] INVALID
//   Client   7: [ERR]  invalid order
//

#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "client_state.h"

#include <atomic>
#include <vector>
#include <sys/epoll.h>

class ClientOrchestrator {
public:
    // num_clients  – number of concurrent TCP connections to open
    // rng_seed     – seed for the top-level RNG (used to seed per-client RNGs)
    explicit ClientOrchestrator(int num_clients, uint32_t rng_seed = std::random_device{}());

    ClientOrchestrator(const ClientOrchestrator&)            = delete;
    ClientOrchestrator& operator=(const ClientOrchestrator&) = delete;
    ClientOrchestrator(ClientOrchestrator&&)                 = delete;
    ClientOrchestrator& operator=(ClientOrchestrator&&)      = delete;

    ~ClientOrchestrator();

    // Blocks until stop() is called (typically from a signal handler).
    void run();
    void stop() { running_.store(false, std::memory_order_relaxed); }

    // Snapshot of aggregate statistics across all clients.
    struct Stats {
        uint64_t requests_sent;
        uint64_t responses_ok;
        uint64_t responses_err;
    };
    [[nodiscard]] Stats stats() const;

private:
    int                      num_clients_;
    int                      epoll_fd_ = -1;
    std::vector<ClientState> clients_;      // stable after constructor (reserved)
    std::mt19937             rng_;
    std::atomic<bool>        running_{true};

    epoll_event events_[CLIENT_EPOLL_BATCH]{};

    // ── Setup ─────────────────────────────────────────────────────────────────
    bool setupEpoll();
    bool connectClient(ClientState& cs);

    // ── epoll event dispatch ──────────────────────────────────────────────────
    void handleConnect (ClientState& cs);              // EPOLLOUT while CONNECTING
    void handleWritable(ClientState& cs);              // EPOLLOUT while SENDING
    void handleReadable(ClientState& cs);              // EPOLLIN  while AWAITING
    void handleError   (ClientState& cs);              // EPOLLERR | EPOLLHUP

    // ── Core request / response logic ─────────────────────────────────────────
    void buildAndSend  (ClientState& cs);              // build next random frame + start send
    void flushWrite    (ClientState& cs);              // (re)try sending write_buf
    void parseResponse (ClientState& cs);              // parse read_buf → log → next request

    // ── Reconnection ──────────────────────────────────────────────────────────
    void scheduleReconnect(ClientState& cs);
    void processPendingReconnects();

    // ── epoll helpers ─────────────────────────────────────────────────────────
    void epollAdd(ClientState& cs, uint32_t events);
    void epollMod(ClientState& cs, uint32_t events);
    void closeClient(ClientState& cs);               // remove from epoll + close fd

    // ── Logging ───────────────────────────────────────────────────────────────
    static void logSend    (const ClientState& cs);
    static void logOk      (const ClientState& cs, OrderId oid);
    static void logErr     (const ClientState& cs, const char* msg, size_t msg_len);
    static void logConnect (const ClientState& cs, bool success);
    static void logDisconnect(const ClientState& cs);
};

#endif // ORCHESTRATOR_H
