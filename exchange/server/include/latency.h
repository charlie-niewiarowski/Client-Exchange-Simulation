//
// Created by cniew on 5/24/26.
//

#ifndef LATENCY_H
#define LATENCY_H

#include <algorithm>
#include <chrono>
#include <immintrin.h>
#include <iostream>
#include <thread>
#include <vector>
#include <hdr/hdr_histogram.h>

#include "config.h"
struct LatencySample {
    uint64_t t1, t2, t3, t4;
};

class LatencyHandler {
public:
    LatencyHandler() {
        samples_.resize(LATENCY_SAMPLE_COUNT); // resize (not reserve) so operator[] into valid slots

        // Calibrate TSC frequency against the wall clock.
        // Note: do NOT shadow ticks_per_ns_ with a local declaration here.
        const uint64_t tsc1 = __rdtsc();
        const auto     wall1 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t tsc2 = __rdtsc();
        const auto     wall2 = std::chrono::steady_clock::now();

        ticks_per_ns_ = static_cast<double>(tsc2 - tsc1) /
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(wall2 - wall1).count());
    }

    ~LatencyHandler() {
        if (count_ == 0) {
            std::cout << "LatencyHandler: no samples collected\n";
            return;
        }

        struct hdr_histogram *hist;
        hdr_init(1, 1'000'000'000, 5, &hist);

        for (size_t i = 0; i < count_; ++i) {
            const auto& s = samples_[i];
            if (s.t1 == 0 || s.t3 == 0 || s.t3 < s.t1) continue;  // sanity
            uint64_t latency_ns = (s.t3 - s.t1) / ticks_per_ns_;
            hdr_record_value(hist, latency_ns);
        }

        hdr_percentiles_print(hist, stdout, 5, 1.0, CLASSIC);

        std::cout << "samples collected : " << count_  << "\n"
                  << "median latency    : " << hdr_value_at_percentile(hist, 50.0) << " ns\n"
                  << "p99    latency    : " << hdr_value_at_percentile(hist, 99.0) << " ns\n"
                  << "p999   latency    : " << hdr_value_at_percentile(hist, 99.9) << " ns\n";
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
};

#endif //LATENCY_H