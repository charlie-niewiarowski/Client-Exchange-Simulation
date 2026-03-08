//
// Created by charl on 11/25/2025.
//

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

#include "../include/Engine.hpp"
#include "../config/macros.h"
#include "../utils/shared_memory.hpp"

//=============================================================================
// Special Member Functions + Their Helpers
//=============================================================================
Engine::Order::Order(const OrderId id, const OrderRequest order_request) {
    client_id_ = order_request.get_clientId();
    id_ = id;
    side_ = order_request.get_side();
    order_type_ = order_request.get_type();
    price_ = order_request.get_price();
    initial_quantity_ = order_request.get_quantity();
    remaining_quantity_ =  order_request.get_quantity();
}

Engine::Engine() {
    orders_.reserve(PREALLOCATION_COUNT);

    void* rx_ptr{create_shm(INBOUND_SHM_NAME, RINGBUFFER_COUNT)};
    rx_ring = static_cast<RingBuffer<InboundMessage>*>(rx_ptr);
    void* tx_ptr{create_shm(OUTBOUND_SHM_NAME, RINGBUFFER_COUNT)};
    tx_ring = static_cast<RingBuffer<OutboundMessage>*>(tx_ptr);
}

Engine::~Engine() {
    destroy_shm(INBOUND_SHM_NAME);
    destroy_shm(OUTBOUND_SHM_NAME);
}


//=============================================================================
// run() Function + the Suite of Helpers
//=============================================================================
void Engine::run() {
    std::jthread matching_thread([&]() {
       match_orders();
    });

    std::jthread io_thread([&]() {
       expose_trades();
    });
}

void Engine::rx_push_trigger() {
    if (!rx_dispatch_pending.exchange(true, std::memory_order_acq_rel)) {
        rx_dispatch_pending.store(false, std::memory_order_release);
        drain_rx_ring();
    }
}

void Engine::drain_rx_ring() {
    InboundMessage msg = {0};
    while (rx_ring->pop(std::ref(msg))) {
        execute_request(msg);
    }
}

//===== 1.1. Request Execution For Inbound Messages ======
void Engine::execute_request(InboundMessage msg) {
    OutboundMessage outbound_msg{msg.client_id, msg.order_id, msg.message_type, Status::SUCCESS};
    switch (msg.message_type) {
        case MessageType::NEW:
            add_order(-1, OrderRequest{msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity});
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
            modify_order(msg.order_id, OrderRequest{msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity});
            break;
        default:
            std::cerr << "Engine: Inbound message attempted MATCH request";
    }

    tx_ring->push(outbound_msg);
    tx_notify_();
}

//===== 1.1.1. Add Order To the Orderbook =====
void Engine::add_order(OrderId id, OrderRequest order_request) {
    if (id < 0) id = next_id_++;
    Order order{id, order_request};

    auto side = order_request.get_side();
    auto price = order_request.get_price();

    if (orders_.contains(id)) return;

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
    if (!orders_.contains(id)) return;
    auto& order = orders_.at(id);

    auto new_price = order_request.get_price();
    auto new_quantity = order_request.get_quantity();
    if (new_price != order.price_ || new_quantity != order.initial_quantity_) {
        cancel_order(id);
        add_order(id, order_request);
    }
    else {
        order = Order{id, order_request};
    }
}

//===== 1.2. Order Matching Mechanism =====
void Engine::match_orders() {
    while (!bids_.empty() && !asks_.empty()) {
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

            // capture trade info before fills mutate the orders
            Trade trade{
                TradeInfo(bid.client_id_, bid.id_, bid.price_, fill_quantity),
                TradeInfo(ask.client_id_, ask.id_, ask.price_, fill_quantity)
            };
            OutboundMessage buy_msg(bid.client_id_, bid.id_, MessageType::MATCH, Status::SUCCESS);
            OutboundMessage ask_msg(ask.client_id_, ask.id_, MessageType::MATCH, Status::SUCCESS);

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

            {
                std::unique_lock<std::shared_mutex> lock(trades_mux_);
                trades_buffer_.push(trade);
                tx_ring->push(buy_msg);
                tx_ring->push(ask_msg);
                tx_notify_();
            }

            // erase empty price levels and break to avoid dangling references
            bool bid_level_done = highest_bids.empty();
            bool ask_level_done = lowest_asks.empty();
            if (bid_level_done) bids_.erase(bid_price);
            if (ask_level_done) asks_.erase(ask_price);
            if (bid_level_done || ask_level_done) break;
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

//=============================================================================
// Test Accessors
//=============================================================================
void Engine::setup_local_rings() {
    local_rx_ring_ = std::make_unique<RingBuffer<InboundMessage>>(RINGBUFFER_COUNT);
    local_tx_ring_ = std::make_unique<RingBuffer<OutboundMessage>>(RINGBUFFER_COUNT);
    rx_ring = local_rx_ring_.get();
    tx_ring = local_tx_ring_.get();
}

OrderId Engine::direct_add(const OrderRequest& req) {
    auto id = next_id_++;
    Order order{id, req};
    orders_.insert({id, order});
    if (req.get_side() == Side::BID) {
        bids_[req.get_price()].push_back(id);
    } else {
        asks_[req.get_price()].push_back(id);
    }
    return id;
}

void Engine::push_inbound(const InboundMessage& msg) {
    rx_ring->push(msg);
    rx_push_trigger();
}

bool Engine::pop_outbound(OutboundMessage& msg) {
    return tx_ring->pop(msg);
}

bool Engine::pop_trade(Trade& trade) {
    std::unique_lock<std::shared_mutex> lock(trades_mux_);
    return trades_buffer_.pop(trade);
}

void Engine::trigger_matching() {
    match_orders();
}

size_t Engine::bids_at(Price price) const {
    auto it = bids_.find(price);
    return it == bids_.end() ? 0 : it->second.size();
}

size_t Engine::asks_at(Price price) const {
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.size();
}