//
// Created by cniew on 5/16/26.
//

#include "../include/buffer.h"

#include <cstring>
#include <cassert>
#include "sys/socket.h"

ssize_t Buffer::read_from(int fd) {
    ssize_t n = recv(fd, data_ + len_, remaining_capacity(), 0);
    if (n > 0) len_ += static_cast<size_t>(n);
    return n;
}

ssize_t Buffer::write_to(int fd) {
    ssize_t n = send(fd, data_ + bytes_written_, len_ - bytes_written_, MSG_NOSIGNAL);
    if (n > 0) bytes_written_ += static_cast<size_t>(n);
    return n;
}

void Buffer::overwrite(size_t start, const char *src) {
    assert(bytes_written_ == 0); // make sure we're not overwriting data that needs to be written

    memcpy(data_ + start, src, strlen(src));
    len_ = std::max(len_, start + strlen(src));
}