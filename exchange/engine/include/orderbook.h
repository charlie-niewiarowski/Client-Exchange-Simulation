//
// Orderbook — an autonomous price-time-priority order book.
//
// The Orderbook owns the full book state (the OrderPool, the per-price
// PriceLevelQueues, and the OrderId -> Order* lookup map) and all of the
// mutating logic: adding, cancelling, modifying, and matching orders.
//
// It is driven exclusively through process(Command): the Engine translates each
// inbound wire message into a Command and hands it over. Matching results
// (MATCH fills) are pushed directly onto the outbound ring the Orderbook was
// constructed with; process() returns a ProcessResult telling the caller how to
// acknowledge the command.
//

#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include "order.h"
#include "price_level_queue.h"
#include "order_pool.h"
#include "order_request.h"
#include "order_types.h"
#include "communication_types.h"
#include "ring_buffer.hpp"
#include "trade.h"
#include "config.h"

#include <array>
#include <unordered_map>

//=============================================================================
// Book storage types
//=============================================================================
//
// The map does NOT own the Order objects — it stores non-owning pointers into
// the OrderPool arena. Ownership is centralised in the pool (see order_pool.h).
// The bid/ask arrays are indexed by (price - MIN_PRICE): one PriceLevelQueue
// per tick.

using OrderMap  = std::unordered_map<OrderId, Order*>;
using BidLevels = std::array<PriceLevelQueue, MAX_PRICE - MIN_PRICE + 1>;
using AskLevels = std::array<PriceLevelQueue, MAX_PRICE - MIN_PRICE + 1>;

//=============================================================================
// Command — the single input to the Orderbook
//=============================================================================
//
// Carries the assigned OrderId (the Engine assigns ids for NEW orders) and all
// fields required to add/cancel/modify. Diagnostic timestamps are only present
// under DIAGNOSTICS, mirroring OrderRequest.

struct Command {
    MessageType type;
    OrderId     order_id;
    ClientId    client_id;
    Side        side;
    OrderType   order_type;
    Price       price;
    Quantity    quantity;
#if DIAGNOSTICS
    Timestamp   recv_tsc;
    Timestamp   server_push_tsc;
    Timestamp   engine_pop_tsc;
#endif
};

//=============================================================================
// ProcessResult — how the caller should acknowledge a processed Command
//=============================================================================
//
// suppress_ack is true when the command produced MATCH messages instead of a
// plain acknowledgement (the fills were already pushed onto the outbound ring).

struct ProcessResult {
    Status status;
    bool   suppress_ack;
};

//=============================================================================
// Orderbook
//=============================================================================

class Orderbook {
public:
    explicit Orderbook(OutboundRing& out_ring);

    Orderbook(const Orderbook&)            = delete;
    Orderbook& operator=(const Orderbook&) = delete;
    Orderbook(Orderbook&&)                 = delete;
    Orderbook& operator=(Orderbook&&)      = delete;

    ~Orderbook() = default;

    // Route point: apply a single command to the book.
    ProcessResult process(const Command& cmd);

#if LOGGING
    // Drain a trade produced by matching (consumed by the Engine's expose thread).
    bool pop_trade(Trade& t) { return trades_ring_.pop(t); }
#endif

#if TESTING
    size_t order_count()     const { return orders_.size(); }
    size_t bid_level_count() const { return non_empty_bid_levels_; }
    size_t ask_level_count() const { return non_empty_ask_levels_; }
    size_t bids_at(Price p)  const { return bids_[p - MIN_PRICE].size(); }
    size_t asks_at(Price p)  const { return asks_[p - MIN_PRICE].size(); }
    bool   has_order(OrderId id) const { return orders_.contains(id); }
#endif

private:
    //===== data containers + facilitating members ======
    OrderPool pool_;   // owns every Order; must outlive orders_/bids_/asks_ pointers
    BidLevels bids_;
    AskLevels asks_;
    OrderMap  orders_; // OrderId -> Order* into pool_ (non-owning)

    Price  best_bid_price_{MIN_PRICE};
    Price  best_ask_price_{MAX_PRICE};
    size_t non_empty_bid_levels_{0};
    size_t non_empty_ask_levels_{0};

    //===== output ======
    OutboundRing& out_ring_;   // MATCH fills are pushed here
#if LOGGING
    RingBuffer<Trade> trades_ring_{TRADE_RING_COUNT};
#endif

    //===== state modifications =====
    bool addOrder(OrderId id, const OrderRequest& order_request);
    void cancelOrder(OrderId id);
    bool modifyOrder(OrderId id, const OrderRequest& order_request);

    void update_best_bid();
    void update_best_ask();

    //===== matching ======
    bool matchOrders();
    bool matchMarket(OrderId id, const OrderRequest& order_request);
    bool canMatch(Side side, Price price) const;
};

#endif // ORDERBOOK_H
