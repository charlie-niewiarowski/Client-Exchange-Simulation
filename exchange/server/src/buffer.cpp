//
// Created by cniew on 5/16/26.
//

#include "../include/buffer.h"

#include <cstring>
#include <cassert>
#include "sys/socket.h"

ssize_t Buffer::read_from(const int fd) {
    ssize_t n = recv(fd, data_ + len_, remaining_capacity(), 0);
    if (n > 0) len_ += static_cast<size_t>(n);
    return n;
}

void Buffer::fill(const char *src, const size_t n) {
	assert(n <= BUFFER_SIZE);
	memcpy(data_, src, n);
    len_ = n;
}

void Buffer::advance(const size_t n) {
    if (n >= len_) { len_ = 0; return; }
    std::memmove(data_, data_ + n, len_ - n);
    len_ -= n;
}

ssize_t Buffer::write_to(const int fd) {
    ssize_t n = send(fd, data_ + bytes_written_, len_ - bytes_written_, MSG_NOSIGNAL);
    if (n > 0) bytes_written_ += static_cast<size_t>(n);
    return n;
}