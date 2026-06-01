//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_MACROS_H
#define UNTITLED_MACROS_H

//=============================================================================
// user-level macros (feel free to tinker around)
//=============================================================================

#define LOGGING 0
#define DIAGNOSTICS 1
#define TESTING 0

#define MIN_PRICE 0
#define MAX_PRICE (10'000 * 100) // the 100 is because we are using uint32_t, so $10.25 = uint32_t(1025).

#define MATCHING_CORE 1
#define INBOUND_CORE 2
#define OUTBOUND_CORE 3

//=============================================================================
// development macros (DO NOT TOUCH UNLESS FOR A REASON)
//=============================================================================

//===== global =====
#define COMMUNICATION_RING_COUNT 262144

//===== engine =====
#define PREALLOCATION_COUNT 1'000'000
#define TRADE_RING_COUNT COMMUNICATION_RING_COUNT // only relevant for logging == 1

//===== server =====
#define PORT "4000"
#define MAX_CLIENTS 64

// Number of inbound/outbound messages the per-connection buffers can hold before drained
#define PIPELINE_DEPTH 32

#define LATENCY_SAMPLE_DROP 100'000 // gets rid of cold start's influence
#define LATENCY_SAMPLE_COUNT (100'000'000 + LATENCY_SAMPLE_DROP) // despite skipping the first X samples we still
                                                              // get the desired number of samples
#endif //UNTITLED_MACROS_H