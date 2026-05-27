//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

// Exchange endpoint
#define EXCHANGE_HOST "127.0.0.1"
#define EXCHANGE_PORT  4000

// Per-client circular buffer of live order IDs available for CANCEL / MODIFY
#define MAX_ACTIVE_ORDERS 32u

// Number of requests kept in-flight per connection.
// One send() per PIPELINE_DEPTH frames; one recv() may return multiple responses.
// Throughput ceiling ≈ num_clients × PIPELINE_DEPTH / RTT.
#define PIPELINE_DEPTH 32

// epoll_wait batch size
#define CLIENT_EPOLL_BATCH 512

// Reconnect backoff: linear up to this ceiling (milliseconds)
#define RECONNECT_DELAY_MS 1000

// CPU core to pin the client event-loop thread to
#define CLIENT_CORE 4

// Set to 1 to print thread ID on startup (mirrors DIAGNOSTICS in exchange)
#define DIAGNOSTICS 0

// Set to 0 to disable per-request logging (useful for pure throughput runs)
#define LOG_ENABLED 1

// open-loop rate cap: aggregate orders / second across ALL clients.
// The orchestrator divides this evenly so each client's inter-arrival time is
//   inter_arrival_ns = num_clients * 1e9 / EXPECTED_THROUGHPUT
// A client will not issue its next request until both (a) the previous
// response has been received AND (b) its inter-arrival slot has elapsed.
// Set to 0 to disable rate limiting (open-loop / go as fast as possible).
#define EXPECTED_THROUGHPUT 0

#endif // CLIENT_CONFIG_H
