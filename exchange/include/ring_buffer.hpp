//
// Created by cniew on 5/16/26.
//

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <memory>
#include <atomic>
#include <cassert>

#include "communication_types.h"

template <typename T>
class RingBuffer {
    alignas(64) std::atomic<size_t> write_idx_{0};
    alignas(64) std::atomic<size_t> read_idx_{0};
    alignas(64) std::unique_ptr<T[]> buffer;
    alignas(64) uint32_t mask;
public:
    explicit RingBuffer(uint32_t N) : // N must be a power of two because of mask
        buffer(std::make_unique<T[]>(N)),
        mask(N - 1)
    {
        assert((N & (N - 1)) == 0);
    }

    bool push(const T& item);
    bool pop(T& item);
    size_t size() const;
    bool empty() const;
};

template<typename T>
bool RingBuffer<T>::push(const T& item) {
    size_t write_idx = write_idx_.load(std::memory_order_relaxed);
    size_t next_write = (write_idx + 1) & mask;

    if (next_write == read_idx_.load(std::memory_order_acquire)) return false;

    buffer[write_idx] = item;
    write_idx_.store(next_write, std::memory_order_release);
    return true;
}

template<typename T>
bool RingBuffer<T>::pop(T &item) {
    size_t read_idx = read_idx_.load(std::memory_order_relaxed);

    if (read_idx == write_idx_.load(std::memory_order_acquire)) return false;

    item = buffer[read_idx];

    size_t next_read = (read_idx + 1) & mask;
    read_idx_.store(next_read, std::memory_order_release);
    return true;
}

template<typename T>
size_t RingBuffer<T>::size() const {
    size_t write_idx = write_idx_.load(std::memory_order_acquire);
    size_t read_idx = read_idx_.load(std::memory_order_acquire);

    if (write_idx >= read_idx) return write_idx - read_idx;
    return (mask + 1) + (write_idx - read_idx);
}

template<typename T>
bool RingBuffer<T>::empty() const {
    size_t write_idx = write_idx_.load(std::memory_order_acquire);
    size_t read_idx = read_idx_.load(std::memory_order_acquire);

    if (write_idx == read_idx) return true;
    return false;
}

using InboundRing = RingBuffer<InboundMessage>;
using OutboundRing = RingBuffer<OutboundMessage>;

#endif //RINGBUFFER_H
