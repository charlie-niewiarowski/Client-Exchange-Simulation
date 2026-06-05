//
// Created by charl on 11/25/2025.
//

#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <iostream>
#include <thread>

#include "../include/engine.h"
#include "../include/engine_types.h"
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
// order execution
//=============================================================================
void Engine::executeRequest(const InboundMessage &msg) {
#if DIAGNOSTICS
    const Timestamp engine_pop_tsc = __rdtsc(); // t3
#endif

    // For NEW orders assign the id now so it can be returned in the response.
    const OrderId assigned_id = (msg.message_type == MessageType::NEW) ? next_id_++ : msg.order_id;

    OutboundMessage outbound_msg{
        .read_begin_tsc  = msg.read_begin_tsc,
#if DIAGNOSTICS
        .recv_tsc        = msg.recv_tsc,
        .server_push_tsc = msg.server_push,
        .engine_pop_tsc  = engine_pop_tsc,
#endif
        .order_id        = assigned_id,
        .client_id       = msg.client_id,
        .message_type    = msg.message_type,
        .status          = Status::SUCCESS
    };
    bool suppress_ack = false;

    switch (msg.message_type) {
        case MessageType::NEW:
            suppress_ack = addOrder(assigned_id, OrderRequest{
                msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity
#if DIAGNOSTICS
                , msg.recv_tsc, msg.server_push, engine_pop_tsc
#endif
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
                msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity
#if DIAGNOSTICS
                , msg.recv_tsc, msg.server_push, engine_pop_tsc
#endif
            });
            break;
        default:
            #if LOGGING
            std::cerr << "engine: Inbound message attempted unknown request\n";
            #endif
            outbound_msg.status = Status::FAILURE;
    }

    if (!suppress_ack) {
#if DIAGNOSTICS
        outbound_msg.engine_push_tsc = __rdtsc(); // t4
#endif
        out_ring_.push(outbound_msg);
    }
}

bool Engine::addOrder(OrderId id, const OrderRequest& order_request) {
    if (order_request.get_type() == OrderType::MARKET) {
        return matchMarket(id, order_request);
    }

    if (orders_.contains(id)) return false;

    Order& order = orders_.emplace(id, Order{id, order_request}).first->second;

    if (order.side_ == Side::BID) {
        if (bids_[order.price_ - MIN_PRICE].empty()) ++non_empty_bid_levels_;
        bids_[order.price_ - MIN_PRICE].push(&order);
        if (order.price_ > best_bid_price_) best_bid_price_ = order.price_;
    } else {
        if (asks_[order.price_ - MIN_PRICE].empty()) ++non_empty_ask_levels_;
        asks_[order.price_ - MIN_PRICE].push(&order);
        if (order.price_ < best_ask_price_) best_ask_price_ = order.price_;
    }

    return matchOrders();
}

void Engine::cancelOrder(const OrderId id) {
    const auto& order = orders_.at(id);
    const auto price = order.price_;

    if (order.side_ == Side::BID) {
        bids_[price - MIN_PRICE].remove(&order);
        if (bids_[price - MIN_PRICE].empty()) {
            --non_empty_bid_levels_;
            if (non_empty_bid_levels_ == 0)    best_bid_price_ = MIN_PRICE;
            else if (price == best_bid_price_) update_best_bid();
        }
    } else {
        asks_[price - MIN_PRICE].remove(&order);
        if (asks_[price - MIN_PRICE].empty()) {
            --non_empty_ask_levels_;
            if (non_empty_ask_levels_ == 0)    best_ask_price_ = MAX_PRICE;
            else if (price == best_ask_price_) update_best_ask();
        }
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
    if (order_request.get_type() == OrderType::MARKET) {
        return matchMarket(id, order_request);
    }

    return matchOrders();
}

//=============================================================================
// matching
//=============================================================================

bool Engine::matchOrders() {
    bool any_filled = false;

    while (non_empty_bid_levels_ > 0 && non_empty_ask_levels_ > 0) {
        auto& highest_bids = bids_[best_bid_price_ - MIN_PRICE];
        auto& lowest_asks = asks_[best_ask_price_ - MIN_PRICE];

        bool level_matched = false;

        while (!highest_bids.empty() && !lowest_asks.empty()) {
            // get the orders
            auto bid = highest_bids.front();
            auto ask = lowest_asks.front();

            // LIMIT orders only fill when prices cross; MARKET orders fill at any price
            if (best_bid_price_ < best_ask_price_) break;

            level_matched = true;
            any_filled = true;

            // fill the orders
            auto fill_quantity = std::min(bid->remaining_quantity_, ask->remaining_quantity_);

            #if LOGGING
            Trade trade{
                TradeInfo(bid.client_id_, bid.id_, bid.price_, fill_quantity),
                TradeInfo(ask.client_id_, ask.id_, ask.price_, fill_quantity)
            };
            #endif

            OutboundMessage buy_msg{
                .recv_tsc       = bid->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = bid->engine_pop_tsc,
                #endif
                .order_id       = bid->id_,
                .client_id      = bid->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc       = ask->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = ask->engine_pop_tsc,
                #endif
                .order_id       = ask->id_,
                .client_id      = ask->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };

            bid->fill(fill_quantity);
            ask->fill(fill_quantity);

            out_ring_.push(buy_msg);
            out_ring_.push(ask_msg);

            // remove orders if filled
            if (bid->is_filled()) {
                highest_bids.pop();
                orders_.erase(bid->id_);
            }
            if (ask->is_filled()) {
                lowest_asks.pop();
                orders_.erase(ask->id_);
            }

            #if LOGGING
            trades_ring_.push(trade);
            #endif

            // erase empty price levels and break to avoid dangling references
            const bool bid_level_done = highest_bids.empty();
            const bool ask_level_done = lowest_asks.empty();
            if (bid_level_done) {
                --non_empty_bid_levels_;
                if (non_empty_bid_levels_ == 0) best_bid_price_ = MIN_PRICE;
                else update_best_bid();
            }
            if (ask_level_done) {
                --non_empty_ask_levels_;
                if (non_empty_ask_levels_ == 0) best_ask_price_ = MAX_PRICE;
                else update_best_ask();
            }
            if (bid_level_done || ask_level_done) break;
        }

        if (!level_matched) break;
    }

    return any_filled;
}

bool Engine::matchMarket(OrderId id, const OrderRequest& order_request) {
    auto remaining{order_request.get_quantity()};

    if (order_request.get_side() == Side::BID) {
        while (non_empty_ask_levels_ > 0 && remaining > 0) {
            auto& lowest_asks{asks_[best_ask_price_ - MIN_PRICE]};

            if (lowest_asks.empty()) return false;

            auto ask = lowest_asks.front();

            auto fill_quantity = std::min(remaining, ask->remaining_quantity_);

            OutboundMessage market_msg{
                .recv_tsc       = order_request.get_recv_tsc(),
                .engine_pop_tsc = order_request.get_engine_in_tsc(),
                .order_id       = id,
                .client_id      = order_request.get_clientId(),
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc       = ask->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = ask->engine_pop_tsc,
                #endif
                .order_id       = ask->id_,
                .client_id      = ask->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };

            remaining -= fill_quantity;
            ask->fill(fill_quantity);

            out_ring_.push(market_msg);
            out_ring_.push(ask_msg);

            // remove orders if filled
            if (ask->is_filled()) {
                lowest_asks.pop();
                orders_.erase(ask->id_);
            }

            #if LOGGING
            trades_ring_.push(trade);
            #endif

            if (lowest_asks.empty()) {
                --non_empty_ask_levels_;
                if (non_empty_ask_levels_ == 0) best_ask_price_ = MAX_PRICE;
                else update_best_ask();
            }
        }
    }
    else if (order_request.get_side() == Side::ASK) {
        while (non_empty_bid_levels_ > 0 && remaining > 0) {
            auto& lowest_bids = bids_[best_bid_price_ - MIN_PRICE];

            if (lowest_bids.empty()) return false;


            auto bid = lowest_bids.front();

            auto fill_quantity = std::min(remaining, bid->remaining_quantity_);

            OutboundMessage market_msg{
                .recv_tsc       = order_request.get_recv_tsc(),
                .engine_pop_tsc = order_request.get_engine_in_tsc(),
                .order_id       = id,
                .client_id      = order_request.get_clientId(),
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc       = bid->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = bid->engine_pop_tsc,
                #endif
                .order_id       = bid->id_,
                .client_id      = bid->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };

            remaining -= fill_quantity;
            bid->fill(fill_quantity);

            out_ring_.push(market_msg);
            out_ring_.push(ask_msg);

            // remove orders if filled
            if (bid->is_filled()) {
                lowest_bids.pop();
                orders_.erase(bid->id_);
            }

            #if LOGGING
            trades_ring_.push(trade);
            #endif

            if (lowest_bids.empty()) {
                --non_empty_bid_levels_;
                if (non_empty_bid_levels_ == 0) best_bid_price_ = MIN_PRICE;
                else update_best_bid();
            }
        }
    }

        return remaining == 0;
}

bool Engine::canMatch(const Side side, const Price price) const {
    if (side == Side::BID) {
        if (non_empty_ask_levels_ == 0) return false;
        return price >= best_ask_price_;
    }

    if (non_empty_bid_levels_ == 0) return false;
    return price <= best_bid_price_;
}

void Engine::update_best_bid() {
    while (best_bid_price_ > MIN_PRICE && bids_[best_bid_price_ - MIN_PRICE].empty()) {
        --best_bid_price_;
    }
}

void Engine::update_best_ask() {
    while (best_ask_price_ < MAX_PRICE && asks_[best_ask_price_ - MIN_PRICE].empty()) {
        ++best_ask_price_;
    }
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