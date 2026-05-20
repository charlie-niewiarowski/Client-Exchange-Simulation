//
// Created by cniew on 5/17/26.
//

#include "client.h"
#include "../include/protocol.h"

#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <iostream>

void Client::run() {
    addrinfo hints{}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("localhost", "4000", &hints, &result) != 0) {
        std::cerr << "getaddrinfo failed: " << strerror(errno) << '\n';
        return;
    }

    fd = socket(result->ai_family, result->ai_socktype, 0);
    if (fd == -1) {
        std::cerr << "socket() failed: " << strerror(errno) << '\n';
        freeaddrinfo(result);
        return;
    }

    if (connect(fd, result->ai_addr, result->ai_addrlen) == -1) {
        std::cerr << "connect() failed: " << strerror(errno) << '\n';
        freeaddrinfo(result);
        close(fd);
        fd = -1;
        return;
    }
    freeaddrinfo(result);

    std::cout << "connected" << std::endl;

    sendMessage();
    readResponse();

    close(fd);
    fd = -1;
}

void Client::sendMessage() {
    constexpr InboundMessage msg{32, 12, 200, 10, MessageType::NEW, Side::BID, OrderType::LIMIT};
    char buf[INBOUND_BSIZE];

    size_t len = snprintf(buf, sizeof(buf), "EXCHANGE\n");
    memcpy(buf + len, &msg, sizeof(msg));
    buf[len + sizeof(InboundMessage)] = '\n';

    len += sizeof(msg) + 1;

    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) return;
        sent += n;
    }
}

void Client::readResponse() {
    char buf[OUTBOUND_BSIZE];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';

        ClientId cid;
        OrderId oid;
        size_t len = strlen("EXCHANGE\nOK\n");

        memcpy(&cid, &buf[len], sizeof(ClientId));
        memcpy(&oid, &buf[len + sizeof(ClientId)], sizeof(OrderId));

        std::cout << buf << cid <<  " " << oid << std::endl;
    }
}