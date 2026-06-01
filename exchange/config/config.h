//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_MACROS_H
#define UNTITLED_MACROS_H

// Exchange global
#define COMMUNICATION_RING_COUNT 65536 // subject to change

#define MATCHING_CORE 1
#define INBOUND_CORE 2
#define OUTBOUND_CORE 3

#define LOGGING 0
#define DIAGNOSTICS 0
#define TESTING 0

#define MIN_PRICE 0
#define MAX_PRICE (10'000 * 100) // the 100 is because we are using uint32_t, so $10.25 = uint32_t(1025).

// Engine subdir
#define PREALLOCATION_COUNT 1'000'000
#define TRADE_RING_COUNT COMMUNICATION_RING_COUNT

// Server subdir
#define PORT "4000"
#define MAX_CLIENTS 64

// Number of inbound messages the per-connection read buffer can hold before
// processRequest drains it.  Larger values let a single recv() feed more
// work into the engine without a second syscall.
#define READ_BATCH_SIZE 32

// Number of outbound messages that can be packed into the per-connection write
// buffer and sent in one send() syscall.  Larger values improve throughput at
// the cost of slightly higher per-message latency (batch fill delay).
#define WRITE_BATCH_SIZE 32

#define LATENCY_SAMPLE_COUNT 10'000'000

#endif //UNTITLED_MACROS_H