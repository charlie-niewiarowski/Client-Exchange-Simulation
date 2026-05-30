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
// Throughput ceiling approx = num_clients × PIPELINE_DEPTH / RTT.
#define PIPELINE_DEPTH 32

// epoll_wait batch size
#define CLIENT_EPOLL_BATCH 512

// Reconnect backoff, linear up to this ceiling
#define RECONNECT_DELAY_MS 1000

// CPU core to pin the client event-loop thread to
#define CLIENT_CORE 4

// Set to 1 to print thread ID on startup
#define DIAGNOSTICS 0

// Set to 1 to print every server response as it arrives.
// Output format:
//   [OK]    cid:<id>  oid:<oid>          — ACK for a sent request
//   [MATCH] cid:<id>  oid:<oid>          — fill notification (unsolicited OK)
//   [ERR]   cid:<id>  :: <error string>  — server-side error
#define LOGGING 1

// open-loop rate cap: aggregate orders / second across ALL clients.
// The orchestrator divides this evenly so each client's inter-arrival time is
//   inter_arrival_ns = num_clients * 1e9 / EXPECTED_THROUGHPUT
// A client will not issue its next request until both (a) the previous
// response has been received AND (b) its inter-arrival slot has elapsed.
// Set to 0 to disable rate limiting (open-loop / go as fast as possible).
#define EXPECTED_THROUGHPUT 0

// GBM volatility per step: drives mid-price drift AND limit-price spread.
// Expected spread ≈ MID_PRICE_VOL * mid_price * sqrt(2/pi).
// At vol=0.002 and mid=10 000 that is ~16 ticks.
#define MID_PRICE_VOL 0.002

// Starting mid-price in integer ticks
#define MID_PRICE_INITIAL 10000.0

// GBM step is applied every N build_frame calls
// Must be a power of two for bitmask
#define MID_PRICE_UPDATE_N 16

// Lognormal quantity distribution
#define MEAN_QTY 100.0
#define QTY_VOL 0.8

#endif // CLIENT_CONFIG_H
