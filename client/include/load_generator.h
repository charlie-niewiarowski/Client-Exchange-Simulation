//
// Created by cniew on 5/24/26.
//
// LoadGenerator
//
// Manages N simulated client connections using a single epoll event loop.
// Every client is one TCP socket + a ClientState; the loop is single-threaded
// (no shared mutable state between clients).
//
// Pipelining
//
// Each connection keeps PIPELINE_DEPTH (config.h) requests in-flight at once.
// buildBatch() packs up to PIPELINE_DEPTH frames into write_buf and issues a
// single send() syscall.  Responses are consumed in FIFO order (TCP guarantees
// ordering) via pipe_ring.  As each response arrives, buildBatch() immediately
// tops the window back up to PIPELINE_DEPTH, so the pipeline stays full.
//
// Throughput: ≈ num_clients × PIPELINE_DEPTH / RTT
// Syscall reduction: 1 send() + 1 recv() per PIPELINE_DEPTH requests (vs
//   one send + one recv per request in the unpipelined baseline).
//
// Closed-loop rate cap (EXPECTED_THROUGHPUT > 0)
//
// When enabled, buildBatch() checks the monotonic clock before issuing a batch.
// If the inter-arrival slot has not elapsed, it sets send_pending and defers
// to processPendingReconnects().
//
// Request / response cycle
//
//  1. Non-blocking connect() per client at startup.
//  2. On connect completion  (EPOLLOUT in CONNECTING): send first full batch.
//  3. On recv() returning data (EPOLLIN  in AWAITING):
//       a. Drain ALL complete responses from read_buf (loop via consumeResponse).
//       b. Call buildBatch() to refill the pipeline.
//  4. On partial send        (EPOLLOUT in SENDING):   flush remaining bytes.
//  5. On error / EOF:        close fd, schedule reconnect with linear backoff.
//

#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "client_state.h"

#include <atomic>
#include <vector>
#include <sys/epoll.h>

class LoadGenerator {
public:
    explicit LoadGenerator(int num_clients,
                                uint32_t rng_seed = std::random_device{}());

    LoadGenerator(const LoadGenerator&)            = delete;
    LoadGenerator& operator=(const LoadGenerator&) = delete;
    LoadGenerator(LoadGenerator&&)                 = delete;
    LoadGenerator& operator=(LoadGenerator&&)      = delete;

    ~LoadGenerator();

    // Blocks until stop() is called.
    void run();
    void stop() { running_.store(false, std::memory_order_relaxed); }

    struct Stats {
        uint64_t requests_sent;
        uint64_t responses_ack;
        uint64_t responses_match;
        uint64_t responses_err;
        int64_t  orders_in_flight; // snapshot at time of call
    };
    [[nodiscard]] Stats stats() const;

private:
    int                      num_clients_;
    int                      epoll_fd_ = -1;
    std::vector<ClientState> clients_;    // stable after ctor (pre-reserved)
    std::mt19937             rng_;
    std::atomic<bool>        running_{true};
    epoll_event              events_[CLIENT_EPOLL_BATCH]{};
    // Per-client inter-arrival time derived from EXPECTED_THROUGHPUT.
    // 0 means unlimited (open-loop).
    uint64_t                 inter_arrival_ns_ = 0;

    // ── Setup ─────────────────────────────────────────────────────────────────
    bool setupEpoll();
    bool connectClient(ClientState& cs);

    // ── epoll event handlers ──────────────────────────────────────────────────
    void handleConnect (ClientState& cs);  // EPOLLOUT while CONNECTING
    void handleReadable(ClientState& cs);  // EPOLLIN  while AWAITING
    void handleError   (ClientState& cs);  // EPOLLERR | EPOLLHUP

    // ── Core request / response logic ─────────────────────────────────────────
    // buildOne  — append one frame to write_buf, record kind in pipe_ring
    // buildBatch — fill pipeline to PIPELINE_DEPTH and flush; no-op if full or
    //              write already in progress
    void buildOne       (ClientState& cs);
    void buildBatch     (ClientState& cs);
    void flushWrite     (ClientState& cs);  // (re)try draining write_buf
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
    static void logAck       (const ClientState& cs, OrderId oid);
    static void logMatch     (const ClientState& cs, OrderId oid);
    static void logErr       (const ClientState& cs, const char* msg, size_t len);
};

#endif // ORCHESTRATOR_H
