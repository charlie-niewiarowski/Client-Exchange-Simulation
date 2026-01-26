//
// Created by charl on 1/13/2026.
//

#include "OrderGateway.hpp"
#include "../lib/RingBuffer.hpp"
#include "../config/macros.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <experimental/scope>

//=============================================================================
// Special Member Functions + Their Helpers
//=============================================================================
OrderGateway::OrderGateway() {
    auto socket_err =  setup_socket();
    if (!socket_err) {
        std::cout << "Gateway: Could not setup socket";
    }

    auto memory_err = setup_shared_memory();
    if (!memory_err) {
        std::cout << "Gateway: Could not setup shared memory";
    }
}

OrderGateway::~OrderGateway() {
    unmap_memory();
}

//===== 1. Socket Helpers =====
std::expected<void, SetupError> OrderGateway::setup_socket() {
    if (auto err = initialize_winsock(); !err) {
        std::cout << "Gateway: WSAStartup failed" << '\n';
        return err;
    }
    if (auto err = create_socket(); !err) {
        std::cout << "Gateway: socket creation failed" << '\n';
        return err;
    }

    std::experimental::scope_exit guard([&]() {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }); // will run always even if we return an error upon socket creation

    if (auto err = bind_socket(); !err) {
        std::cout << "Gateway: bind socket failed" << '\n';
        return err;
    }
    if (auto err = listen_for_requests(); !err) {
        std::cout << "Gateway: listen failed" << '\n';
        return err;
    }

    return {};
}

std::expected<void, SetupError> OrderGateway::initialize_winsock() {
    WSAData wsa_data;
    int wsa_err;
    WORD w_version_requested = MAKEWORD(2, 2);
    wsa_err = WSAStartup(w_version_requested, &wsa_data);

    if (wsa_err != 0) {
        return SetupError::WSA_FAIL;
    }
    return {};
}

std::expected<void, SetupError> OrderGateway::create_socket() {
    socket_ = socket(AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP);

    if (socket_ == INVALID_SOCKET) {
        WSACleanup();
        return SetupError::SOCKET_FAIL;
    }
    return {};
}

std::expected<void, SetupError> OrderGateway::bind_socket() {
    sockaddr_in address;
    address.sin_family = AF_UNSPEC;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((u_short)5555);

    if (bind(socket_, reinterpret_cast<SOCKADDR*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_);
        WSACleanup();
        return SetupError::BIND_FAIL;
    }
    return {};
}

std::expected<void, SetupError> OrderGateway::listen_for_requests() {
    if (listen(socket_, 1) == SOCKET_ERROR) {
        WSACleanup();
        return SetupError::LISTEN_FAIL;
    }
    return {};
}

//==== 2. Memory Helpers =====
std::expected<void, SetupError> OrderGateway::setup_shared_memory() {

    if (auto err = allocate(); !err) {
        std::cout << "Gateway: file setup or mapping failed" << '\n';
        return err;
    }

    std::experimental::scope_exit guard([&]() {
        unmap_memory();
    });

    if (auto err = initialize_buffers(); !err) {
        std::cout << "Gateway: initialize_buffers failed" << '\n';
        return err;
    }

    return {};
}

std::expected<void, SetupError> OrderGateway::allocate() {
    constexpr size_t INBOUND_BYTES = sizeof(RingBuffer<InboundMessage>);
    constexpr size_t OUTBOUND_BYTES = sizeof(RingBuffer<OutboundMessage>);
    constexpr size_t MEM_SIZE = INBOUND_BYTES + OUTBOUND_BYTES;

    hMapFile_ = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        MEM_SIZE,
        MAPPING_NAME);

    if (!hMapFile_) {
        return SetupError::ALLOC_FAIL;
    }

    base_ = MapViewOfFile(
        hMapFile_,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        MEM_SIZE);

    if (!base_) {
        CloseHandle(hMapFile_);
        return SetupError::MAP_FAIL;
    }

    return {};
}

std::expected<void, SetupError> OrderGateway::initialize_buffers() {
    constexpr size_t INBOUND_BYTES = sizeof(RingBuffer<InboundMessage>);
    inbound_buff_ = static_cast<RingBuffer<InboundMessage>*>(base_);

    auto* outbound_base = static_cast<std::byte*>(base_) + INBOUND_BYTES;
    outbound_client_msgs_ = reinterpret_cast<ClientMessageMap*>(outbound_base);
}

//===== 3. Destruction Helpers =====
void OrderGateway::unmap_memory() {
    if (base_) {
        UnmapViewOfFile(base_);
    }
    if (hMapFile_) {
        CloseHandle(hMapFile_);
    }
}

//=============================================================================
// run() function and the suite of helpers
//=============================================================================
void OrderGateway::run() {
    for (;;) {
        handle_client();
    }
}

//===== 1. Inbound->Outbound Pipeline =====
void OrderGateway::handle_client() {
    SOCKET accept_socket = accept_connection();
    if (accept_socket == INVALID_SOCKET) {
        std::cout << "Gateway: accept failed: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return;
    }

    std::optional<InboundMessage> inbound_opt = parse_message(accept_socket);
    if (inbound_opt == std::nullopt) {
        std::cout << "Gateway: recv failed" << std::endl;
        return;
    }
    InboundMessage inbound_msg = inbound_opt.value();

    if (!validate_message(inbound_msg)) {
        if (send_fail(accept_socket, inbound_msg) == -1) {
            std::cout << "Gateway: could not send failure message: " << WSAGetLastError() << '\n';
        }
        return;
    }

    inbound_buff_->push(inbound_msg);

    {
        std::shared_lock<std::shared_mutex> lock(client_mux_);
        const OutboundMessage msg{outbound_client_msgs_->at(inbound_msg.client_id).front()};
        int send_err = send_response(accept_socket, msg);
        if (send_err == - 1) {
            std::cout << "Gateway: sending response failed: " << WSAGetLastError() << '\n';

            if (send_fail(accept_socket, inbound_msg) == -1) {
                std::cout << "Gateway: could not send failure message: " << WSAGetLastError() << '\n';
            }
        }
    }

    closesocket(accept_socket);
}

//===== 1.1 Accepting Client Connection =====
SOCKET OrderGateway::accept_connection() {
    SOCKET accept_socket{INVALID_SOCKET};
    accept_socket = accept(socket_, nullptr, nullptr);
    return accept_socket;
}

//===== 1.2 Parsing a Client's Request =====
std::optional<InboundMessage> OrderGateway::parse_message(const SOCKET& accept_socket) {
    constexpr auto buffer_size = sizeof(InboundMessage);
    char buffer[buffer_size];
    int byte_count = recv(accept_socket, buffer, buffer_size, 0);

    if (byte_count != buffer_size) {
        return std::nullopt;
    }

    InboundMessage msg{};
    std::memcpy(&msg, buffer, buffer_size);
    msg.client_id = static_cast<ClientId>(accept_socket);

    return msg;
}

//===== 1.3 Sending a Generic Response to a Client =====
int OrderGateway::send_response(SOCKET client_socket, const OutboundMessage msg) {
    int bytes_sent = send(client_socket, reinterpret_cast<const char *>(&msg), sizeof(msg), 0);
    if (bytes_sent != sizeof(OutboundMessage)) {
        return -1;
    }

    return 0;
}

//===== 1.4 Generic Failure Helper Function =====
int OrderGateway::send_fail(SOCKET client_socket, InboundMessage msg) {
    OutboundMessage fail_msg{msg.client_id, msg.order_id, msg.message_type, Status::FAILURE};
    if (send_response(client_socket, fail_msg) == -1) {
        return -1;
    }
    return 0;
}

//===== 2. Validation Pipeline =====
bool OrderGateway::validate_message(const InboundMessage& msg) {
    return validate_new(msg) && validate_cancel(msg) && validate_modify(msg);
}

//===== 2.1 Validating NEW Orders =====
bool OrderGateway::validate_new(const InboundMessage& msg) {
    uint8_t raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 0) return false;

    uint8_t raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    uint8_t raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.side == Side::BID && msg.price <= 0) return false;
    if (msg.side == Side::ASK && msg.price < 0) return false;

    if (msg.quantity < 0 || msg.quantity > 1e6) return false;

    return true;
}

//===== 2.2 Validating CANCEL Orders =====
bool OrderGateway::validate_cancel(const InboundMessage &msg) {
    uint8_t raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 1) return false;

    return true;
}

//===== 2.3 Validating Modify Orders =====
bool OrderGateway::validate_modify(const InboundMessage &msg) {
    uint8_t raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 2) return false;

    uint8_t raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    uint8_t raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.side == Side::BID && msg.price <= 0) return false;
    if (msg.side == Side::ASK && msg.price < 0) return false;

    if (msg.quantity < 0 || msg.quantity > 1e6) return false;

    return true;
}