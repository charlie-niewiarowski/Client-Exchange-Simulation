//
// Created by charl on 11/25/2025.
//

#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include "../include/engine.h"
#include "lib.h"

#define ever (;;)

//=============================================================================
// Forward-declared free forms
//=============================================================================

#if LOGGING
void write_to_console(const Trade &trade);
#endif

//=============================================================================
// Special Member Functions + Their Helpers
//=============================================================================
Engine::Order::Order(const OrderId id, const OrderRequest& order_request) {
    id_ = id;
    client_id_ = order_request.get_clientId();
    side_ = order_request.get_side();
    order_type_ = order_request.get_type();
    price_ = order_request.get_price();
    initial_quantity_ = order_request.get_quantity();
    remaining_quantity_ = order_request.get_quantity();
    recv_tsc = order_request.get_recv_tsc();
    server_push_tsc = order_request.get_server_push_tsc();
    engine_pop_tsc = order_request.get_engine_in_tsc();
}

Engine::Engine(InboundRing& in_ring, OutboundRing& out_ring, std::atomic<bool>& stop)
    : in_ring_(in_ring), out_ring_(out_ring), stop_(stop) {
    orders_.reserve(PREALLOCATION_COUNT);
}

//=============================================================================
// run()
//=============================================================================
void Engine::run() {
    matching_thread_ = std::jthread([&]() {
        #if DIAGNOSTICS
        std::cerr << "matching tid: " << gettid() << std::endl;
        #endif

        pin_to_core(MATCHING_CORE);
        handleMatching();
    });

    #if LOGGING
         expose_thread_ = std::jthread([&]() {
           exposeTrades();
        });
    #endif
}

//=============================================================================
// step()  — test interface, processes all pending inbound messages once
//=============================================================================
#if TESTING
void Engine::step() {
    InboundMessage msg{};
    while (in_ring_.pop(msg)) {
        executeRequest(msg);
    }
}
#endif

//=============================================================================
// run() Helper Suite
//=============================================================================
void Engine::handleMatching() {
    InboundMessage msg{};

    while (!stop_.load(std::memory_order_relaxed)) {
        if (in_ring_.pop(msg)) {
            executeRequest(msg);
        }
    }
}

#if LOGGING
void Engine::exposeTrades() {
    Trade trade{};

    while (!stop_.load(std::memory_order_relaxed)) {
        while (trades_ring_.pop(trade)) {
            write_to_console(trade);
        }
    }
}
#endif

//=============================================================================
// matching helpers
//=============================================================================
void Engine::executeRequest(const InboundMessage &msg) {
    const Timestamp engine_pop_tsc = __rdtsc(); // t2: engine has received the inbound message

    // For NEW orders assign the id now so it can be returned in the response.
    const OrderId assigned_id = (msg.message_type == MessageType::NEW) ? next_id_++ : msg.order_id;

    // Designated initializer: timestamps must be set explicitly now that they lead the struct.
    OutboundMessage outbound_msg{
        .recv_tsc        = msg.recv_tsc,
        .server_push_tsc = msg.server_push,
        .engine_pop_tsc  = engine_pop_tsc,
        .client_id       = msg.client_id,
        .order_id        = assigned_id,
        .message_type    = msg.message_type,
        .status          = Status::SUCCESS
    };
    bool suppress_ack = false;

    switch (msg.message_type) {
        case MessageType::NEW:
            suppress_ack = addOrder(assigned_id, OrderRequest{
                msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity,
                msg.recv_tsc, msg.server_push, engine_pop_tsc
            });
            break;
        case MessageType::CANCEL:
            if (!orders_.contains(msg.order_id) || orders_.at(msg.order_id).client_id_ != msg.client_id) {
                outbound_msg.status = Status::FAILURE;
                break;
            }
            cancelOrder(msg.order_id);
            break;
        case MessageType::MODIFY:
            if (!orders_.contains(msg.order_id)
                    || orders_.at(msg.order_id).client_id_ != msg.client_id
                    || orders_.at(msg.order_id).side_ != msg.side) {
                outbound_msg.status = Status::FAILURE;
                break;
            }
            suppress_ack = modifyOrder(msg.order_id, OrderRequest{
                msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity,
                msg.recv_tsc, msg.server_push, engine_pop_tsc
            });
            break;
        default:
            #if LOGGING
            std::cerr << "engine: Inbound message attempted unknown request\n";
            #endif
            outbound_msg.status = Status::FAILURE;
    }

    if (!suppress_ack) {
        outbound_msg.engine_push_tsc = __rdtsc(); // t3: engine is pushing the outbound message
        out_ring_.push(outbound_msg);
    }
}

bool Engine::addOrder(OrderId id, const OrderRequest& order_request) {
    if (orders_.contains(id)) return false;

    Order order{id, order_request};
    const auto side = order_request.get_side();
    const auto price = order_request.get_price();

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

    return matchOrders();
}

void Engine::cancelOrder(const OrderId id) {
    const auto& order = orders_.at(id);
    const auto price = order.price_;

    if (order.side_ == Side::BID) {
        auto& queue = bids_[price];
        if (auto itr = std::ranges::find(queue, id); itr != queue.end()) {
            queue.erase(itr);
        }

        if (queue.empty()) bids_.erase(price);
    } else {
        auto& queue = asks_[price];
        if (auto itr = std::ranges::find(queue, id); itr != queue.end()) {
            queue.erase(itr);
        }

        if (queue.empty()) asks_.erase(price);
    }

    orders_.erase(id);
}

bool Engine::modifyOrder(const OrderId id, const OrderRequest& order_request) {
    auto& order = orders_.at(id);

    const auto new_price = order_request.get_price();
    const auto new_quantity = order_request.get_quantity();
    if (new_price != order.price_ || new_quantity != order.initial_quantity_) {
        cancelOrder(id);
        return addOrder(id, order_request);
    }

    order = Order{id, order_request};
    return matchOrders();
}

bool Engine::matchOrders() {
    bool any_filled = false;

    while (!bids_.empty() && !asks_.empty()) {
        auto& [bid_price, highest_bids] = *bids_.begin();
        auto& [ask_price, lowest_asks] = *asks_.begin();

        bool level_matched = false;

        while (!highest_bids.empty() && !lowest_asks.empty()) {
            // get the orders
            auto bid_id = highest_bids.front();
            auto& bid = orders_.at(bid_id);
            auto ask_id = lowest_asks.front();
            auto& ask = orders_.at(ask_id);

            // LIMIT orders only fill when prices cross; MARKET orders fill at any price
            bool either_market = bid.order_type_ == OrderType::MARKET || ask.order_type_ == OrderType::MARKET;
            bool prices_cross  = bid.price_ >= ask.price_;
            if (!either_market && !prices_cross) break;

            level_matched = true;
            any_filled = true;

            // fill the orders
            auto fill_quantity = std::min(bid.remaining_quantity_, ask.remaining_quantity_);

            #if LOGGING
            Trade trade{
                TradeInfo(bid.client_id_, bid.id_, bid.price_, fill_quantity),
                TradeInfo(ask.client_id_, ask.id_, ask.price_, fill_quantity)
            };
            #endif

            OutboundMessage buy_msg{
                .recv_tsc      = bid.recv_tsc,
                .engine_pop_tsc = bid.engine_pop_tsc,
                .client_id     = bid.client_id_,
                .order_id      = bid.id_,
                .message_type  = MessageType::MATCH,
                .status        = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc      = ask.recv_tsc,
                .engine_pop_tsc = ask.engine_pop_tsc,
                .client_id     = ask.client_id_,
                .order_id      = ask.id_,
                .message_type  = MessageType::MATCH,
                .status        = Status::SUCCESS
            };

            bid.fill(fill_quantity);
            ask.fill(fill_quantity);

            out_ring_.push(buy_msg);
            out_ring_.push(ask_msg);

            // remove orders if filled
            if (bid.is_filled()) {
                highest_bids.pop_front();
                orders_.erase(bid_id);
            }
            if (ask.is_filled()) {
                lowest_asks.pop_front();
                orders_.erase(ask_id);
            }

            #if LOGGING
            trades_ring_.push(trade);
            #endif

            // erase empty price levels and break to avoid dangling references
            const bool bid_level_done = highest_bids.empty();
            const bool ask_level_done = lowest_asks.empty();
            if (bid_level_done) bids_.erase(bid_price);
            if (ask_level_done) asks_.erase(ask_price);
            if (bid_level_done || ask_level_done) break;
        }

        if (!level_matched) break;
    }

    return any_filled;
}

bool Engine::canMatch(const Side side, const Price price) const {
    if (side == Side::BID) {
        if (asks_.empty()) return false;

        const auto& lowest_ask = asks_.cbegin()->first;
        return price >= lowest_ask;
    }

    if (bids_.empty()) return false;
    const auto& highest_bid = bids_.cbegin()->first;
    return price <= highest_bid;
}

//=============================================================================
// Forward-declared free form implementations
//=============================================================================

#if LOGGING
void write_to_console(const Trade &trade) {
    const TradeInfo& bid{ trade.get_bid_info() };
    const TradeInfo& ask{ trade.get_ask_info() };

    std::cout << "bid -> " << "client_id: " << bid.client_id_
                           << " order_id: " << bid.order_id
                           << " price: " << bid.price
                           << " quantity: " << bid.quantity << "\n";
    std::cout << "ask -> " << "client_id: " << ask.client_id_
                           << " order_id: " << ask.order_id
                           << " price: " << ask.price
                           << " quantity: " << ask.quantity << "\n\n";
}
#endif