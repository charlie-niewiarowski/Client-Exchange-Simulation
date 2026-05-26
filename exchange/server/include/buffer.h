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
    [[nodiscard]] size_t length() const { return len_; }
    [[nodiscard]] size_t bytes_written() const { return bytes_written_; }
    [[nodiscard]] size_t remaining_capacity() const { return BUFFER_SIZE - len_; }

    // reading and writing
    ssize_t read_from(int fd); // used to read from a sock
    [[nodiscard]] std::string_view view() const { return {data_, len_}; } // used for parsing
	void fill(const char *src, size_t n); // used for serialization
    ssize_t write_to(int fd); // used for writing

	// util
    void clear()               { len_ = 0; bytes_written_ = 0; }
    void advance(size_t n);    // discard n bytes from the front, shift remainder left

private:
    char data_[BUFFER_SIZE]{};
    size_t len_ = 0;
    size_t bytes_written_ = 0;
};

#endif //BUFFER_H
