//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_ORDERBOOK_H
#define UNTITLED_ORDERBOOK_H
#pragma once

#include <map>
#include <atomic>
#include <unordered_map>
#include <shared_mutex>
#include <stdexcept>

#include "engine_types.hpp"
#include "communication_types.hpp"
#include "Trade.hpp"
#include "../lib/RingBuffer.hpp"
#include "../config/macros.h"

class Engine {
    class Order { // main data variable
        ClientId client_id_;
        OrderId id_;
        Side side_;
        OrderType order_type_;
        Price price_;
        Quantity initial_quantity_;
        Quantity remaining_quantity_;

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
    };

    using OrderMap = std::unordered_map<OrderId, Order>;

    BidLevels bids_;
    AskLevels asks_;
    OrderMap orders_;
    std::atomic<OrderId> next_id_{0};

    RingBuffer<InboundMessage>* rx_ring{nullptr};
    RingBuffer<OutboundMessage>* tx_ring{nullptr};

    std::atomic_bool rx_dispatch_pending{false};
    std::function<void()> tx_notify_;

    // Owned ring buffers used in tests instead of shm-backed ones
    std::unique_ptr<RingBuffer<InboundMessage>>  local_rx_ring_;
    std::unique_ptr<RingBuffer<OutboundMessage>> local_tx_ring_;

    std::shared_mutex trades_mux_;
    RingBuffer<Trade> trades_buffer_{RINGBUFFER_COUNT};
public:
    Engine();
    Engine(const Engine& other) = delete;
    Engine& operator=(const Engine& rhs) = delete;
    Engine(const Engine&& other) = delete;
    Engine& operator=(const Engine&& rhs) = delete;
    ~Engine();

    void run();
    void set_tx_notify(const std::function<void()>& func) { tx_notify_ = func; }
    void rx_push_trigger();

    // Test accessors — seed state and inspect internals without background threads
    void setup_local_rings();  // replace shm rings with heap-allocated ones for tests
    OrderId direct_add(const OrderRequest& req);
    void push_inbound(const InboundMessage& msg);
    bool pop_outbound(OutboundMessage& msg);
    bool pop_trade(Trade& trade);
    void trigger_matching();

    size_t order_count()     const { return orders_.size(); }
    bool   has_order(OrderId id) const { return orders_.contains(id); }
    size_t bid_level_count() const { return bids_.size(); }
    size_t ask_level_count() const { return asks_.size(); }
    size_t bids_at(Price price) const;
    size_t asks_at(Price price) const;
private:
    // helpers for run() function
    void drain_rx_ring();
        void execute_request(InboundMessage msg);
            void add_order(OrderId id, OrderRequest order_request);
            void cancel_order(OrderId id);
            void modify_order(OrderId id, OrderRequest request);
        void match_orders();
            bool can_match(Side side, Price price) const;

    void expose_trades(); // I/O bound thread that outputs trades to the console
        void write_to_console(const Trade &trade);
};


#endif //UNTITLED_ORDERBOOK_H