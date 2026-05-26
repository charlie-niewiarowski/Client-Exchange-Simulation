//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_MACROS_H
#define UNTITLED_MACROS_H

// Exchange global
#define COMMUNICATION_RING_COUNT 65536 // subject to change

#define SERVER_CORE 2

#define LOGGING 0
#define DIAGNOSTICS 0
#define TESTING 0

// Engine subdir
#define PREALLOCATION_COUNT 1'000'000
#define TRADE_RING_COUNT COMMUNICATION_RING_COUNT

#define MATCHING_CORE 1

// Server subdir
#define PORT "4000"

#define MAX_CLIENTS 64

#define LATENCY_SAMPLE_COUNT 10'000'000

#endif //UNTITLED_MACROS_H