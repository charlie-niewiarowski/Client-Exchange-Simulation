//
// Created by cniew on 5/16/26.
//
// Header-only fixed-capacity byte buffer.  The template parameter N is the
// capacity in bytes; both the read and write buffers in ConnectionInfo use
// this template with different N values.
//
// Read path (inbound):
//   read_from() -> view() / advance()
//
// Write path (outbound):
//   append()  - copies a serialised frame into the tail of the buffer
//   write_to() - drains bytes starting at bytes_written_
//   try_reset() - resets indices once all bytes have been sent
//
// has_room(n) lets the caller check before calling append() so the buffer
// is never over-filled.
//

#ifndef BUFFER_H
#define BUFFER_H

#include <cassert>
#include <cstring>
#include <string_view>
#include <sys/socket.h>

template <size_t N>
class Buffer {
public:
    Buffer() = default;

    //===== read path =====

    // Returns the number of bytes currently in the buffer.
    [[nodiscard]] size_t length()             const { return len_; }
    [[nodiscard]] size_t remaining_capacity() const { return N - len_; }
    [[nodiscard]] std::string_view view()     const { return {data_, len_}; }

    // Appends up to remaining_capacity() bytes from fd.
    // Returns the recv() return value directly (> 0, 0, or -1 with errno set).
    ssize_t read_from(const int fd) {
        ssize_t n = recv(fd, data_ + len_, N - len_, 0);
        if (n > 0) len_ += static_cast<size_t>(n);
        return n;
    }

    // Discards the first n bytes, shifting the remainder to the front.
    void advance(const size_t n) {
        if (n >= len_) { len_ = 0; return; }
        std::memmove(data_, data_ + n, len_ - n);
        len_ -= n;
    }

    //===== write path =====
    [[nodiscard]] size_t bytes_written()      const { return bytes_written_; }
    [[nodiscard]] size_t remaining_to_send()  const { return len_ - bytes_written_; }
    [[nodiscard]] bool   has_room(size_t n)   const { return len_ + n <= N; }

    // Appends n bytes from src to the tail of the buffer.
    // Returns false (no-op) if the buffer does not have room for n bytes.
    bool append(const char* src, const size_t n) {
        if (!has_room(n)) return false;
        std::memcpy(data_ + len_, src, n);
        len_ += n;
        return true;
    }

    // Sends bytes [bytes_written_, len_) to fd.
    // Returns the send() return value directly.
    ssize_t write_to(const int fd) {
        ssize_t n = send(fd, data_ + bytes_written_, len_ - bytes_written_, MSG_NOSIGNAL);
        if (n > 0) bytes_written_ += static_cast<size_t>(n);
        return n;
    }

    // Resets both indices to 0 once all buffered bytes have been sent.
    // No-op if remaining_to_send() > 0.
    void try_reset() {
        if (bytes_written_ == len_) {
            len_ = 0;
            bytes_written_ = 0;
        }
    }

    // Unconditional reset — discards any data and resets both indices.
    void clear() { len_ = 0; bytes_written_ = 0; }

private:
    char   data_[N]{};
    size_t len_          = 0;
    size_t bytes_written_ = 0;
};

#endif //BUFFER_H
