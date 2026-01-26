//
// Created by charl on 11/25/2025.
//

#include "../include/Engine.hpp"
#include "../config/macros.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <experimental/scope>

//=============================================================================
// Special Member Functions + Their Helpers
//=============================================================================
Engine::Order::Order(OrderId id, OrderRequest order_request) {
    id_ = id;
    side_ = order_request.get_side();
    order_type_ = order_request.get_type();
    price_ = order_request.get_price();
    initial_quantity_ = order_request.get_quantity();
    remaining_quantity_ =  order_request.get_quantity();
}

Engine::Engine() {
    orders_.reserve(PREALLOCATION_COUNT);

    auto memory_err = setup_shared_memory();
    if (!memory_err) {
        std::cout << "Engine: Could not setup shared memory";
    }
}

Engine::~Engine() {
    unmap_memory();
}

//===== 1. Memory Helpers =====
std::expected<void, SetupError> Engine::setup_shared_memory() {

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

std::expected<void, SetupError> Engine::allocate() {
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

std::expected<void, SetupError> Engine::initialize_buffers() {
    constexpr size_t INBOUND_BYTES = sizeof(RingBuffer<InboundMessage>);
    inbound_buff_ = static_cast<RingBuffer<InboundMessage>*>(base_);

    auto* outbound_base = static_cast<std::byte*>(base_) + INBOUND_BYTES;
    outbound_client_msgs_ = reinterpret_cast<ClientMessageMap*>(outbound_base);
}

//===== 2. Destruction Helpers =====
void Engine::unmap_memory() {
    if (base_) {
        UnmapViewOfFile(base_);
    }
    if (hMapFile_) {
        CloseHandle(hMapFile_);
    }
}


//=============================================================================
// run() Function + the Suite of Helpers
//=============================================================================
void Engine::run() {
    std::jthread engine_thread([&]() {
       engine_loop();
    });

    std::jthread io_thread([&]() {
       expose_trades();
    });
}

//===== 1. Main Engine Execution Loop =====
void Engine::engine_loop() {
    for (;;) {
        InboundMessage msg{};
        inbound_buff_->pop(msg);
        execute_request(msg);
        match_orders();
    }
}

//===== 1.1. Request Execution For Inbound Messages ======
void Engine::execute_request(InboundMessage msg) {
    OutboundMessage outbound_msg{msg.client_id, msg.order_id, msg.message_type, Status::SUCCESS};
    switch (msg.message_type) {
        case MessageType::NEW:
            add_order(OrderRequest{msg.side, msg.order_type, msg.price, msg.quantity});
            break;
        case MessageType::CANCEL:
            if (!orders_.contains(msg.order_id)) {
                outbound_msg.status = Status::FAILURE;
                break;
            }
            cancel_order(msg.order_id);
            break;
        case MessageType::MODIFY:
            if (!orders_.contains(msg.order_id)) {
                outbound_msg.status = Status::FAILURE;
                break;
            }
            modify_order(msg.order_id, OrderRequest{msg.side, msg.order_type, msg.price, msg.quantity});
    }

    (*outbound_client_msgs_)[msg.client_id].push_back(outbound_msg);
}

//===== 1.1.1. Add Order To the Orderbook =====
void Engine::add_order(OrderRequest order_request) {
    auto id = next_id_++;
    Order order{id, order_request};

    auto side = order_request.get_side();
    auto price = order_request.get_price();

    if (orders_.contains(id) || !can_match(side, price)) return;

    if (side == Side::BID) {
        if (!bids_.contains(price)) {
            bids_.insert(std::make_pair(price, PriceLevelQueue()));
        }

        bids_[price].push_back(id);
    } else {
        if (!asks_.contains(price)) {
            asks_.insert(std::make_pair(price, PriceLevelQueue()));
        }

        asks_[price].push_back(id);
    }

    orders_.insert(std::make_pair(id, order));
}

//===== 1.1.2. Cancel a Given Order =====
void Engine::cancel_order(OrderId id) {
    if (!orders_.contains(id)) return;

    auto &order = orders_.at(id);
    auto price = order.price_;

    if (order.side_ == Side::BID) {
        auto& queue = bids_[price];

        auto itr = std::find(queue.begin(), queue.end(), id);
        if (itr != queue.end()) queue.erase(itr);

        if (queue.empty()) bids_.erase(price);
    } else {
        auto& queue = asks_[price];

        auto itr = std::find(queue.begin(), queue.end(), id);
        if (itr != queue.end()) queue.erase(itr);

        if (queue.empty()) asks_.erase(price);
    }

    orders_.erase(id);
}

//===== 1.1.3. Modify a Given Order =====
void Engine::modify_order(OrderId id, OrderRequest order_request) {
    auto& order = orders_.at(id);
    order = Order{id, order_request};

    return match_orders();
}

//===== 1.2. Order Matching Mechanism =====
void Engine::match_orders() {
    while (!bids_.empty() || !asks_.empty()) {
        auto& [bid_price, highest_bids] = *bids_.begin();
        auto& [ask_price, lowest_asks] = *asks_.begin();
        if (bid_price < ask_price) break;

        while (!highest_bids.empty() && !lowest_asks.empty()) {
            // get the orders
            auto bid_id = highest_bids.front();
            auto& bid = orders_.at(bid_id);
            auto ask_id = lowest_asks.front();
            auto& ask = orders_.at(ask_id);

            // fill the orders
            auto fill_quantity = std::min(bid.remaining_quantity_, ask.remaining_quantity_);
            bid.fill(fill_quantity);
            ask.fill(fill_quantity);

            // remove orders if filled
            if (bid.is_filled()) {
                highest_bids.pop_front();
                orders_.erase(bid_id);
            }
            if (ask.is_filled()) {
                lowest_asks.pop_front();
                orders_.erase(ask_id);
            }

            // remove PriceLevelQueue if empty
            if (highest_bids.empty()) {
                bids_.erase(bid_price);
            }
            if (lowest_asks.empty()) {
                asks_.erase(ask_price);
            }

            {
                std::unique_lock<std::shared_mutex> lock(trades_mux_);
                trades_buffer_.push(Trade{
                    TradeInfo(bid.id_, bid.price_, fill_quantity),
                    TradeInfo(ask.id_, ask.price_, fill_quantity)}
                );
            }

        }
    }
}

//===== 1.2.1. Helper For Checking If Orders Can Be Matched =====
bool Engine::can_match(Side side, Price price) const {
    if (side == Side::BID) {
        if (asks_.empty()) return false;

        const auto& lowest_ask = asks_.cbegin()->first;
        return price >= lowest_ask;
    }

    if (bids_.empty()) return false;
    const auto& highest_bid = bids_.cbegin()->first;
    return price <= highest_bid;
}

//===== 2. Function For Writing Trade Results To the Console =====
void Engine::expose_trades() {
    for (;;) {
        Trade trade{};

        {
            std::unique_lock<std::shared_mutex> lock(trades_mux_);
            if (!trades_buffer_.empty()) {
                trades_buffer_.pop(trade);
            }
        }

        write_to_console(trade);
    }
}

//===== 2.1 Helper To Perform the Formatting / Writes =====
void Engine::write_to_console(const Trade &trade) {
    const TradeInfo& bid{ trade.get_bid_info() };
    const TradeInfo& ask{ trade.get_ask_info() };

    std::cout << "bid -> " << "client_id: " << bid.client_id_
                           << " order_id: " << bid.order_id
                           << " price: " << bid.price
                           << "quantity: " << bid.quantity << "\n";
    std::cout << "ask -> " << "client_id: " << ask.client_id_
                           << " order_id: " << ask.order_id
                           << " price: " << ask.price
                           << "quantity: " << ask.quantity << "\n";
}