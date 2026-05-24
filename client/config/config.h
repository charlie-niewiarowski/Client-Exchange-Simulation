//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

// Exchange endpoint
#define EXCHANGE_HOST "127.0.0.1"
#define EXCHANGE_PORT  4000

// Per-client circular buffer of live order IDs (for CANCEL/MODIFY targeting)
#define MAX_ACTIVE_ORDERS 32u

// epoll_wait batch size
#define CLIENT_EPOLL_BATCH 512

// Reconnect delay after a dropped connection, in milliseconds
#define RECONNECT_DELAY_MS 1000

// Set to 0 to disable per-request logging (useful for pure throughput runs)
#define LOG_ENABLED 1

#endif // CLIENT_CONFIG_H
