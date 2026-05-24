//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_ORDERBOOK_H
#define UNTITLED_ORDERBOOK_H

#include <map>
#include <atomic>
#include <unordered_map>
#include <stdexcept>

#include "engine_types.h"
#include "trade.h"
#include "order_request.h"
#include "ring_buffer.hpp"
#include "communication_types.h"
#include "macros.h"

class Engine {

public:
    Engine(InboundRing& in_ring, OutboundRing& out_ring);

    Engine(const Engine& other) = delete;
    Engine& operator=(const Engine& rhs) = delete;
    Engine(const Engine&& other) = delete;
    Engine& operator=(const Engine&& rhs) = delete;

    ~Engine() = default;

    void run();

    // Testing — compiled in only when TESTING is defined in macros.h
    #if TESTING
    void step();
    size_t order_count()     const { return orders_.size(); }
    size_t bid_level_count() const { return bids_.size(); }
    size_t ask_level_count() const { return asks_.size(); }
    size_t bids_at(Price p) const {
        auto it = bids_.find(p);
        return it != bids_.end() ? it->second.size() : 0u;
    }
    size_t asks_at(Price p) const {
        auto it = asks_.find(p);
        return it != asks_.end() ? it->second.size() : 0u;
    }
    bool has_order(OrderId id) const { return orders_.contains(id); }
    bool pop_outbound(OutboundMessage& msg) { return out_ring_.pop(msg); }
    bool pop_trade(Trade& t) { return trades_ring_.pop(t); }
    OrderId next_order_id() const { return next_id_.load(std::memory_order_relaxed); }
    #endif // TESTING

private:
    class Order { // main data variable
    public:
        Order(OrderId id, OrderRequest order_request);

        bool is_filled() const { return remaining_quantity_ == 0; }
        void fill(Quantity quantity) {
            if (quantity > remaining_quantity_) {
                throw std::logic_error("Filling quantity larger than remaining quantity");
            }

            remaining_quantity_ -= quantity;
        }

        friend class Engine;
    private:
        ClientId client_id_;
        OrderId id_;
        Side side_;
        OrderType order_type_;
        Price price_;
        Quantity initial_quantity_;
        Quantity remaining_quantity_;
    };
    using OrderMap = std::unordered_map<OrderId, Order>;

    BidLevels bids_;
    AskLevels asks_;
    OrderMap orders_;
    std::atomic<OrderId> next_id_{0};

    InboundRing& in_ring_;
    OutboundRing& out_ring_;

    RingBuffer<Trade> trades_ring_{TRADE_RING_COUNT};

    // run() helper suite
    void handleMatching();

    #if LOGGING
    void exposeTrades();
    #endif

    // matching helpers
    void executeRequest(InboundMessage msg);
    bool addOrder(OrderId id, OrderRequest order_request);
    void cancelOrder(OrderId id);
    bool modifyOrder(OrderId id, OrderRequest request);
    bool matchOrders();
    bool canMatch(Side side, Price price) const;
};


#endif //UNTITLED_ORDERBOOK_H