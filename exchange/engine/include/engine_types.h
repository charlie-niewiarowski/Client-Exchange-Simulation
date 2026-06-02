//
// Created by charl on 1/17/2026.
//

#ifndef UNTITLED_ORDERBOOK_TYPES_HPP
#define UNTITLED_ORDERBOOK_TYPES_HPP

#include "order_request.h"
#include "order_types.h"
#include "config.h"

#include <unordered_map>

//=============================================================================
// Order class
//=============================================================================

class Order { // main data variable
public:
    Order(const OrderId id, const OrderRequest &order_request) {
        id_ = id;
        client_id_ = order_request.get_clientId();
        side_ = order_request.get_side();
        order_type_ = order_request.get_type();
        price_ = order_request.get_price();
        initial_quantity_ = order_request.get_quantity();
        remaining_quantity_ = order_request.get_quantity();
        recv_tsc = order_request.get_recv_tsc();
        #if DIAGNOSTICS
        server_push_tsc = order_request.get_server_push_tsc();
        engine_pop_tsc = order_request.get_engine_in_tsc();
        #endif
    }

    bool is_filled() const { return remaining_quantity_ == 0; }
    void fill(const Quantity quantity) {
        if (quantity > remaining_quantity_) {
            throw std::logic_error("Filling quantity larger than remaining quantity");
        }

        remaining_quantity_ -= quantity;
    }

    void set_next(Order *next) { next_ = next; }
    [[nodiscard]] Order *get_next() const { return next_; }

    void set_prev(Order *prev) {prev_ = prev; }
    [[nodiscard]] Order *get_prev() const { return prev_; }

    friend class Engine;
private:
    Timestamp recv_tsc;           // 8 bytes  @ 0
    #if DIAGNOSTICS
    Timestamp server_push_tsc;    // 8 bytes  @ 8
    Timestamp engine_pop_tsc;     // 8 bytes  @ 16
    #endif
    OrderId id_;                  // 8 bytes  @ 8/24
    Order *prev_{nullptr};        // 8 bytes  @ 16/32
    Order *next_{nullptr};        // 8 bytes  @ 24/40
    ClientId client_id_;          // 4 bytes  @ 28/44
    Price price_;                 // 4 bytes  @ 32/48
    Quantity initial_quantity_;   // 4 bytes  @ 32/48
    Quantity remaining_quantity_; // 4 bytes  @ 36/52
    Side side_;                   // 1 byte   @ 40/56
    OrderType order_type_;        // 1 byte   @ 41/57
    uint8_t padding[7];           // 6 bytes  @ 42/58
}; // 48/64 bytes (sizeof verified by compiler)

//=============================================================================
// PriceLevelQueue class
//=============================================================================

class PriceLevelQueue {
public:
    PriceLevelQueue() = default;

    PriceLevelQueue(const PriceLevelQueue&) = delete;
    PriceLevelQueue& operator=(const PriceLevelQueue&) = delete;
    PriceLevelQueue(PriceLevelQueue&&) = delete;
    PriceLevelQueue& operator=(PriceLevelQueue&&) = delete;

    ~PriceLevelQueue() = default;

    void push(Order *new_order) {
        if (!head) {
            head = new_order;
        }
        if (tail) {
            tail->set_next(new_order);
            new_order->set_prev(tail);
        }
        tail = new_order;
        ++size_;
    }

    Order* pop() {
        if (!head) return nullptr;
        Order *res{head};
        head = head->get_next();
        if (head) {
            head->set_prev(nullptr);
        } else {
            tail = nullptr;
        }
        --size_;
        return res;
    }

    Order *front() { return head; }

    void remove(const Order *to_remove) {
        Order *prev{to_remove->get_prev()}, *next{to_remove->get_next()};

        if (prev) prev->set_next(next);
        else      head = next;

        if (next) next->set_prev(prev);
        else      tail = prev;

        --size_;
    }

    bool empty() const { return head == nullptr; }
    size_t size() const { return size_; }
private:
    Order *head{nullptr}, *tail{nullptr};
    size_t size_{0};
};

//=============================================================================
// types
//=============================================================================

using OrderMap = std::unordered_map<OrderId, Order>;
using BidLevels = std::map<Price, PriceLevelQueue, std::greater<>>;
using AskLevels = std::map<Price, PriceLevelQueue, std::less<>>;

#endif //UNTITLED_ORDERBOOK_TYPES_HPP