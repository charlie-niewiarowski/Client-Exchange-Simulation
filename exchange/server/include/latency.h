//
// Created by cniew on 5/24/26.
//

#ifndef LATENCY_H
#define LATENCY_H

#include <chrono>
#include <immintrin.h>
#include <iostream>
#include <thread>
#include <vector>
#include <hdr/hdr_histogram.h>

#include "config.h"

struct LatencySample {
    Timestamp t1, // read bytes
    t2, // pushed inbound
    t3, // popped inbound
    t4, // pushed outbound
    t5, // popped outbound
    t6; // sent bytes
};

class LatencyHandler {
public:
    LatencyHandler() {
        samples_.resize(LATENCY_SAMPLE_COUNT); // resize (not reserve) so operator[] into valid slots

        const Timestamp tsc1 = __rdtsc();
        const auto wall1 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Timestamp tsc2 = __rdtsc();
        const auto wall2 = std::chrono::steady_clock::now();

        ticks_per_ns_ = static_cast<double>(tsc2 - tsc1) /
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall2 - wall1).count());
    }

    ~LatencyHandler() {
        if (count_ == 0) {
            std::cout << "LatencyHandler: no samples collected\n";
            return;
        }

        hdr_histogram *read_push_inbound, *push_pop_inbound, *pop_push_outbound, *push_pop_outbound, *pop_send_outbound,
                      *end_to_end;
        hdr_init(1, 1'000'000'000, 5, &read_push_inbound);
        hdr_init(1, 1'000'000'000, 5, &push_pop_inbound);
        hdr_init(1, 1'000'000'000, 5, &pop_push_outbound);
        hdr_init(1, 1'000'000'000, 5, &push_pop_outbound);
        hdr_init(1, 1'000'000'000, 5, &pop_send_outbound);
        hdr_init(1, 1'000'000'000, 5, &end_to_end);



        for (size_t i{LATENCY_SAMPLE_DROP}; i < count_; ++i) {
            const auto& s = samples_[i];

            hdr_record_value(read_push_inbound, calc_latency(s.t1, s.t2));  // gateway read  → ring push
            hdr_record_value(push_pop_inbound,  calc_latency(s.t2, s.t3));  // ring push     → engine pop
            hdr_record_value(pop_push_outbound, calc_latency(s.t3, s.t4));  // engine pop    → outbound push
            hdr_record_value(push_pop_outbound, calc_latency(s.t4, s.t5));  // outbound push → gateway pop
            hdr_record_value(pop_send_outbound, calc_latency(s.t5, s.t6));  // gateway pop   → bytes sent
            hdr_record_value(end_to_end, calc_latency(s.t1, s.t6));  // gateway pop   → bytes sent

        }

        hdr_percentiles_print(read_push_inbound, stdout, 5, 1.0, CLASSIC);
        hdr_percentiles_print(push_pop_inbound, stdout, 5, 1.0, CLASSIC);
        hdr_percentiles_print(pop_push_outbound, stdout, 5, 1.0, CLASSIC);
        hdr_percentiles_print(push_pop_outbound, stdout, 5, 1.0, CLASSIC);
        hdr_percentiles_print(pop_send_outbound, stdout, 5, 1.0, CLASSIC);
        hdr_percentiles_print(end_to_end, stdout, 5, 1.0, CLASSIC);
    }

    void push_sample(const LatencySample& sample) {
        if (count_ < samples_.size()) { // guard against overflow past LATENCY_SAMPLE_COUNT
            samples_[count_++] = sample;
        }
    }

private:
    std::vector<LatencySample> samples_;
    size_t count_{};
    double ticks_per_ns_{};

    [[nodiscard]] int64_t calc_latency(const Timestamp t1, const Timestamp t2) const {
        const int64_t diff{static_cast<int64_t>(t2 - t1)};
        return diff / static_cast<int64_t>(ticks_per_ns_);
    }
};

#endif //LATENCY_H