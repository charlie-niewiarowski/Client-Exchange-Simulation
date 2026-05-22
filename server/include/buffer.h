//
// Created by cniew on 5/16/26.
//clear

#ifndef BUFFER_H
#define BUFFER_H

#include "protocol.h"

#include <string>

class Buffer {
public:
    // special member functions
    Buffer() = default;

    // data access
    [[nodiscard]] std::string_view view() const { return {data_, len_}; }
    [[nodiscard]] size_t length() const { return len_; }
    [[nodiscard]] size_t bytes_written() const { return bytes_written_; }
    [[nodiscard]] size_t remaining_capacity() const { return BUFFER_SIZE - len_; }

    // read/write wrappers
    ssize_t read_from(int fd);
    ssize_t write_to(int fd);

    // utility state change
    void overwrite(size_t start, const char *src);
    void overwrite(size_t start, const char *src, size_t len);
    void clear() { len_ = 0; bytes_written_ = 0; }

private:
    char data_[BUFFER_SIZE];
    size_t len_ = 0;
    size_t bytes_written_ = 0;
};

#endif //BUFFER_H
