//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_ORDERBOOK_H
#define UNTITLED_ORDERBOOK_H
#pragma once

#include <map>
#include <atomic>
#include <unordered_map>
#include <windows.h>
#include <expected>
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

            remaining_quantity_ = quantity;
        }

        friend class Engine;
    };

    using OrderMap = std::unordered_map<OrderId, Order>;

    BidLevels bids_;
    AskLevels asks_;
    OrderMap orders_;
    std::atomic<OrderId> next_id_{0};

    HANDLE hMapFile_{nullptr};
    void* base_{nullptr};

    RingBuffer<InboundMessage>* inbound_buff_{nullptr};
    ClientMessageMap* outbound_client_msgs_{nullptr};
        std::shared_mutex client_mux_;

    RingBuffer<Trade> trades_buffer_{RINGBUFFER_COUNT};
        std::shared_mutex trades_mux_;
public:
    Engine();
    Engine(const Engine& other) = delete;
    Engine& operator=(const Engine& rhs) = delete;
    Engine(const Engine&& other) = delete;
    Engine& operator=(const Engine&& rhs) = delete;
    ~Engine();

    void run();
private:
    // setup and cleanup helpers
    std::expected<void, SetupError> setup_shared_memory();
        std::expected<void, SetupError> allocate();
        std::expected<void, SetupError> initialize_buffers();
    void unmap_memory();

    // helpers for run() function
    void engine_loop(); // main thread for message execution / relaying and order matching
        void execute_request(InboundMessage msg);
            void add_order(OrderRequest order_request);
            void cancel_order(OrderId id);
            void modify_order(OrderId id, OrderRequest request);
        void publish_outbound(InboundMessage msg);
        void match_orders();
            bool can_match(Side side, Price price) const;

    void expose_trades(); // I/O bound thread that outputs trades to the console
        void write_to_console(const Trade &trade);


};


#endif //UNTITLED_ORDERBOOK_H