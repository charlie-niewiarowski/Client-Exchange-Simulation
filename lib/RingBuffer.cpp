//
// Created by charl on 1/15/2026.
//

#include "RingBuffer.hpp"

template<typename T>
bool RingBuffer<T>::push(const T& item) {
    size_t write_idx = write_idx_.load(std::memory_order_relaxed);
    size_t next_write = (write_idx + 1) & (buffer.size() - 1);

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

    size_t next_read = (read_idx + 1) & (buffer.size() - 1);
    read_idx_.store(next_read, std::memory_order_release);
    return true;
}

template<typename T>
size_t RingBuffer<T>::size() const {
    size_t write_idx = write_idx_.load(std::memory_order_acquire);
    size_t read_idx = read_idx_.load(std::memory_order_acquire);

    if (write_idx >= read_idx) return write_idx - read_idx;
    return buffer.size() + (write_idx - read_idx);
}

template<typename T>
bool RingBuffer<T>::empty() const {
    size_t write_idx = write_idx_.load(std::memory_order_acquire);
    size_t read_idx = read_idx_.load(std::memory_order_acquire);

    if (write_idx == read_idx) return true;
    return false;
}


