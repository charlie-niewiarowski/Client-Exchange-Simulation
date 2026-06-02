//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_ORDERBOOK_H
#define UNTITLED_ORDERBOOK_H

#include <map>
#include <atomic>
#include <thread>

#include "engine_types.h"
#include "order_request.h"
#include "ring_buffer.hpp"
#include "communication_types.h"
#include "config.h"

class Engine {

public:
    Engine(InboundRing& in_ring, OutboundRing& out_ring, std::atomic<bool>& stop);

    Engine(const Engine& other) = delete;
    Engine& operator=(const Engine& rhs) = delete;
    Engine(const Engine&& other) = delete;
    Engine& operator=(const Engine&& rhs) = delete;

    ~Engine() = default;

    void run();

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
    //===== threads ======
    std::jthread matching_thread_;

    //===== data containers ======
    BidLevels bids_;
    AskLevels asks_;
    OrderMap orders_;
    std::atomic<OrderId> next_id_{0};

    //===== communication with server ======
    InboundRing& in_ring_;
    OutboundRing& out_ring_;

    //===== stop ======
    std::atomic<bool>& stop_;

    //===== thread entry =====
    void handleMatching();

    //===== state modifications =====
    void executeRequest(const InboundMessage &msg);
    bool addOrder(OrderId id, const OrderRequest &order_request);
    void cancelOrder(OrderId id);
    bool modifyOrder(OrderId id, const OrderRequest &order_request);

    //===== matching ======
    bool matchOrders();
    bool matchMarket(OrderId id, const OrderRequest& order_request);
    bool canMatch(Side side, Price price) const;

    #if LOGGING
    std::jthread expose_thread_;
    RingBuffer<Trade> trades_ring_{TRADE_RING_COUNT};
    void exposeTrades();
    #endif
};


#endif //UNTITLED_ORDERBOOK_H