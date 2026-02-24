//
// Created by charl on 1/13/2026.
//

#include "../include/OrderGateway.hpp"
#include "../lib/RingBuffer.hpp"
#include "../config/macros.h"
#include "../utils/shared_memory.hpp"

#include <cstring>
#include <iostream>
#include <span>

using boost::asio::ip::tcp;

//=============================================================================
// TCP Session Subclass
//=============================================================================
void OrderGateway::Session::read(const ClientId& client_id) {
    auto self = shared_from_this();

    socket_.async_read_some(
        boost::asio::buffer(buffer_),
        [this, self, client_id](boost::system::error_code ec, std::size_t bytes) {
            if (!ec) {
                auto msg = std::bit_cast<InboundMessage>(buffer_);
                msg.client_id = client_id;

                if (!gateway_.validate_message(msg)) {
                    gateway_.communicate_failure(msg);
                    return;
                }

                gateway_.rx_ring_->push(msg);
                gateway_.rx_notify_();
            }
        });
}

void OrderGateway::Session::write(std::array<char, sizeof(OutboundMessage)> msg) {
    auto self = shared_from_this();

    boost::asio::async_write(
        socket_,
        boost::asio::buffer(msg),
        [this, self, msg](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                ClientId client_id;
                std::memcpy(&client_id, msg.data(), sizeof(client_id));
                read(client_id); // continue conversation
            }
        });
}
//=============================================================================
// Special Member Functions
//=============================================================================
OrderGateway::OrderGateway(boost::asio::io_context& io, Port port)
    : io_(io), acceptor_(io, tcp::endpoint(tcp::v4(), port)) {
    void* rx_ptr{create_shm(INBOUND_SHM_NAME, RINGBUFFER_COUNT)};
    rx_ring_ = static_cast<RingBuffer<InboundMessage>*>(rx_ptr);
    void* tx_ptr{create_shm(OUTBOUND_SHM_NAME, RINGBUFFER_COUNT)};
    tx_ring_ = static_cast<RingBuffer<OutboundMessage>*>(tx_ptr);
}

//=============================================================================
// Inbound and Outbound Communications
//=============================================================================
//===== 1. Outbound->Inbound Pipeline =====
void OrderGateway::start_accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                ++next_client_id_;
                handle_client(std::move(socket));
            }
            start_accept(); // accept next client
        });
}

void OrderGateway::handle_client(tcp::socket socket) {
    auto session = std::make_shared<Session>(*this, std::move(socket));
    sessions_.insert(std::make_pair(next_client_id_.load(), session));
    session->read(next_client_id_);
}

//===== 2. Inbound->Outbound Pipeline =====
void OrderGateway::tx_push_trigger() {
    if (!tx_dispatch_pending.exchange(true, std::memory_order_acq_rel)) {
        boost::asio::post(io_, [this] {
            tx_dispatch_pending.store(false, std::memory_order_release);
            drain_tx_ring();
        });
    }
}

void OrderGateway::drain_tx_ring() {
    OutboundMessage msg{0};
    while (tx_ring_->pop(msg)) {
        route_tx_message(msg);
    }
}

void OrderGateway::route_tx_message(const OutboundMessage& msg) {
    auto buffer = std::bit_cast<std::array<char, sizeof(OutboundMessage)>>(msg);

    auto session = sessions_.at(msg.client_id);
    session->write(buffer);
}

//=============================================================================
// Validation Component
//=============================================================================
//===== 1 Validating Orders =====
bool OrderGateway::validate_message(const InboundMessage& msg) {
    if (!sessions_.contains(msg.client_id)) return false;
    return validate_new(msg) && validate_cancel(msg) && validate_modify(msg);
}

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

bool OrderGateway::validate_cancel(const InboundMessage &msg) {
    uint8_t raw_message_type = static_cast<uint8_t>(msg.message_type);
    if (raw_message_type != 1) return false;

    return true;
}

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

//===== 2. Responding to Invalid Orders =====
void OrderGateway::communicate_failure(const InboundMessage& msg) {
    OutboundMessage outbound{0};
    outbound.client_id = msg.client_id;
    outbound.order_id = msg.order_id;
    outbound.message_type = msg.message_type;
    outbound.status = Status::FAILURE;

    auto buffer = std::bit_cast<std::array<char, sizeof(OutboundMessage)>>(outbound);
    auto session = sessions_.at(msg.client_id);
    session->write(buffer);
}