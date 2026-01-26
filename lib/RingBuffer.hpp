//
// Created by charl on 1/15/2026.
//

#ifndef RINGBUFFER_HPP
#define RINGBUFFER_HPP
#include <atomic>

template <typename T>
class RingBuffer {
    alignas(64) std::atomic<size_t> write_idx_{0};
    alignas(64) std::atomic<size_t> read_idx_{0};
    alignas(64) T buffer[]; // must be a power of 2 to allow bit-masking
public:
    explicit RingBuffer(uint32_t N) { buffer.reserve(N); }

    bool push(const T& item);
    bool pop(T& item);
    size_t size() const;
    bool empty() const;
};

#endif //RINGBUFFER_HPP