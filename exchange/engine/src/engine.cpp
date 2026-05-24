//
// Created by charl on 11/25/2025.
//

#include <algorithm>
#include <cmath>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include "../include/engine.h"

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
Engine::Order::Order(const OrderId id, const OrderRequest order_request) {
    id_ = id;
    client_id_ = order_request.get_clientId();
    side_ = order_request.get_side();
    order_type_ = order_request.get_type();
    price_ = order_request.get_price();
    initial_quantity_ = order_request.get_quantity();
    remaining_quantity_ =  order_request.get_quantity();
}

Engine::Engine(InboundRing& in_ring, OutboundRing& out_ring)
    : in_ring_(in_ring), out_ring_(out_ring) {
    orders_.reserve(PREALLOCATION_COUNT);
}

//=============================================================================
// run()
//=============================================================================
void Engine::run() {
    std::jthread matching_thread([&]() {
        #if LOGGING
        std::cerr << "matching thread: " << gettid() << std::endl;
        #endif
        handleMatching();
    });

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(MATCHING_CORE, &cpuset);
    pthread_setaffinity_np(matching_thread.native_handle(), sizeof(cpu_set_t), &cpuset);

    #if LOGGING
        std::jthread io_thread([&]() {
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

    for ever {
        while (in_ring_.pop(msg)) {
            executeRequest(msg);
        }
    }
}

#if LOGGING
void Engine::exposeTrades() {
    Trade trade{};

    for ever {
        while (trades_ring_.pop(trade)) {
            write_to_console(trade);
        }
    }
}
#endif

//=============================================================================
// matching helpers
//=============================================================================
void Engine::executeRequest(InboundMessage msg) {
    // For NEW orders assign the id now so it can be returned in the response.
    OrderId assigned_id = (msg.message_type == MessageType::NEW) ? next_id_++ : msg.order_id;
    OutboundMessage outbound_msg{msg.client_id, assigned_id, msg.message_type, Status::SUCCESS};
    bool suppress_ack = false;

    switch (msg.message_type) {
        case MessageType::NEW:
            suppress_ack = addOrder(assigned_id, OrderRequest{msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity});
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
            suppress_ack = modifyOrder(msg.order_id, OrderRequest{msg.client_id, msg.side, msg.order_type, msg.price, msg.quantity});
            break;
        default:
            #if LOGGING
            std::cerr << "engine: Inbound message attempted unknown request\n";
            #endif
            outbound_msg.status = Status::FAILURE;
    }

    if (!suppress_ack) {
        out_ring_.push(outbound_msg);
    }
}

bool Engine::addOrder(OrderId id, OrderRequest order_request) {
    Order order{id, order_request};

    auto side = order_request.get_side();
    auto price = order_request.get_price();

    if (orders_.contains(id)) return false;

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

void Engine::cancelOrder(OrderId id) {
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

bool Engine::modifyOrder(OrderId id, OrderRequest order_request) {
    auto& order = orders_.at(id);

    auto new_price = order_request.get_price();
    auto new_quantity = order_request.get_quantity();
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

            out_ring_.push(buy_msg);
            out_ring_.push(ask_msg);
            trades_ring_.push(trade);

            // erase empty price levels and break to avoid dangling references
            bool bid_level_done = highest_bids.empty();
            bool ask_level_done = lowest_asks.empty();
            if (bid_level_done) bids_.erase(bid_price);
            if (ask_level_done) asks_.erase(ask_price);
            if (bid_level_done || ask_level_done) break;
        }

        if (!level_matched) break;
    }

    return any_filled;
}

bool Engine::canMatch(Side side, Price price) const {
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