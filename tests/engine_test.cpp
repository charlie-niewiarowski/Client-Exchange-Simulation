//
// Engine unit tests — GTest
// Run via: cmake --build build --target test && cd build && ctest --output-on-failure
//

#include <gtest/gtest.h>
#include "../include/Engine.hpp"

// ── Request builders ─────────────────────────────────────────────────────────

static OrderRequest bid(ClientId cid, Price price, Quantity qty) {
    return {cid, Side::BID, OrderType::LIMIT, price, qty};
}

static OrderRequest ask(ClientId cid, Price price, Quantity qty) {
    return {cid, Side::ASK, OrderType::LIMIT, price, qty};
}

static InboundMessage make_new(ClientId cid, Side side, Price price, Quantity qty) {
    return {cid, 0, price, qty, MessageType::NEW, side, OrderType::LIMIT, 0};
}

static InboundMessage make_cancel(ClientId cid, OrderId oid) {
    return {cid, oid, 0, 0, MessageType::CANCEL, Side::BID, OrderType::LIMIT, 0};
}

static InboundMessage make_modify(ClientId cid, OrderId oid, Side side, Price price, Quantity qty) {
    return {cid, oid, price, qty, MessageType::MODIFY, side, OrderType::LIMIT, 0};
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class EngineTest : public ::testing::Test {
protected:
    Engine engine;

    void SetUp() override {
        // tx_notify_ must be set; execute_request and match_orders call it unconditionally
        engine.set_tx_notify([]{});
        // The shm-backed ring buffers in the Engine constructor are only 32 bytes and
        // are never placement-new'd, so mask==0 and every push silently fails.
        // Replace them with properly-constructed heap-allocated rings for testing.
        engine.setup_local_rings();
    }

    std::vector<OutboundMessage> drain_outbound() {
        std::vector<OutboundMessage> out;
        OutboundMessage msg{};
        while (engine.pop_outbound(msg)) out.push_back(msg);
        return out;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Add-order (direct_add — bypasses ring buffer, used to seed state)
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(EngineTest, DirectAddBidRestsOnBook) {
    engine.direct_add(bid(1, 100, 10));

    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_EQ(engine.bid_level_count(), 1);
    EXPECT_EQ(engine.bids_at(100), 1);
}

TEST_F(EngineTest, DirectAddAskRestsOnBook) {
    engine.direct_add(ask(1, 100, 10));

    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_EQ(engine.ask_level_count(), 1);
    EXPECT_EQ(engine.asks_at(100), 1);
}

TEST_F(EngineTest, DirectAddMultipleBidsAtSamePriceLevel) {
    engine.direct_add(bid(1, 100, 5));
    engine.direct_add(bid(2, 100, 5));

    EXPECT_EQ(engine.order_count(), 2);
    EXPECT_EQ(engine.bid_level_count(), 1);  // same price → same level
    EXPECT_EQ(engine.bids_at(100), 2);
}

TEST_F(EngineTest, DirectAddBidsAtDifferentPriceLevels) {
    engine.direct_add(bid(1, 100, 5));
    engine.direct_add(bid(2, 110, 5));

    EXPECT_EQ(engine.bid_level_count(), 2);
    EXPECT_EQ(engine.bids_at(100), 1);
    EXPECT_EQ(engine.bids_at(110), 1);
}

TEST_F(EngineTest, DirectAddReturnsDistinctOrderIds) {
    OrderId id1 = engine.direct_add(bid(1, 100, 5));
    OrderId id2 = engine.direct_add(bid(1, 100, 5));

    EXPECT_NE(id1, id2);
    EXPECT_TRUE(engine.has_order(id1));
    EXPECT_TRUE(engine.has_order(id2));
}

// ═════════════════════════════════════════════════════════════════════════════
// NEW message path (push_inbound — exercises the full ring-buffer flow)
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(EngineTest, NewMessageAddsOrderToBook) {
    engine.push_inbound(make_new(1, Side::BID, 100, 10));

    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_EQ(engine.bid_level_count(), 1);
}

TEST_F(EngineTest, NewMessageSendsSuccessOutbound) {
    engine.push_inbound(make_new(1, Side::BID, 100, 10));

    auto out = drain_outbound();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].message_type, MessageType::NEW);
    EXPECT_EQ(out[0].status, Status::SUCCESS);
    EXPECT_EQ(out[0].client_id, 1u);
}

TEST_F(EngineTest, NewAskMessageAddsAskToBook) {
    engine.push_inbound(make_new(2, Side::ASK, 200, 5));

    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_EQ(engine.ask_level_count(), 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// CANCEL
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(EngineTest, CancelExistingBidRemovesFromBook) {
    OrderId id = engine.direct_add(bid(1, 100, 10));
    engine.push_inbound(make_cancel(1, id));

    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_EQ(engine.bid_level_count(), 0);
    EXPECT_FALSE(engine.has_order(id));
}

TEST_F(EngineTest, CancelExistingAskRemovesFromBook) {
    OrderId id = engine.direct_add(ask(1, 100, 10));
    engine.push_inbound(make_cancel(1, id));

    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_EQ(engine.ask_level_count(), 0);
}

TEST_F(EngineTest, CancelSendsSuccessOutbound) {
    OrderId id = engine.direct_add(bid(1, 100, 10));
    engine.push_inbound(make_cancel(1, id));

    auto out = drain_outbound();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].status, Status::SUCCESS);
    EXPECT_EQ(out[0].order_id, id);
}

TEST_F(EngineTest, CancelOnlyRemovesTargetedOrder) {
    OrderId id1 = engine.direct_add(bid(1, 100, 10));
    OrderId id2 = engine.direct_add(bid(2, 100, 5));
    engine.push_inbound(make_cancel(1, id1));

    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_TRUE(engine.has_order(id2));
    EXPECT_FALSE(engine.has_order(id1));
    EXPECT_EQ(engine.bids_at(100), 1);
}

TEST_F(EngineTest, CancelNonExistentOrderSendsFailure) {
    engine.push_inbound(make_cancel(1, 9999));

    auto out = drain_outbound();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].status, Status::FAILURE);
}

// ═════════════════════════════════════════════════════════════════════════════
// MODIFY
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(EngineTest, ModifyNonExistentOrderSendsFailure) {
    engine.push_inbound(make_modify(1, 9999, Side::BID, 100, 10));

    auto out = drain_outbound();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].status, Status::FAILURE);
}

TEST_F(EngineTest, ModifyExistingOrderSendsSuccess) {
    OrderId id = engine.direct_add(bid(1, 100, 10));
    engine.push_inbound(make_modify(1, id, Side::BID, 100, 20));

    auto out = drain_outbound();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].status, Status::SUCCESS);
    EXPECT_EQ(out[0].order_id, id);
}

TEST_F(EngineTest, ModifyKeepsOrderInBook) {
    OrderId id = engine.direct_add(bid(1, 100, 10));
    engine.push_inbound(make_modify(1, id, Side::BID, 100, 20));

    EXPECT_TRUE(engine.has_order(id));
    EXPECT_EQ(engine.order_count(), 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// Matching
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(EngineTest, FullMatchBothOrdersRemoved) {
    engine.direct_add(bid(1, 100, 10));
    engine.direct_add(ask(2, 100, 10));
    engine.trigger_matching();

    EXPECT_EQ(engine.order_count(), 0);
    EXPECT_EQ(engine.bid_level_count(), 0);
    EXPECT_EQ(engine.ask_level_count(), 0);
}

TEST_F(EngineTest, FullMatchTradeRecordedWithCorrectInfo) {
    engine.direct_add(bid(1, 100, 10));
    engine.direct_add(ask(2, 100, 10));
    engine.trigger_matching();

    Trade t{};
    ASSERT_TRUE(engine.pop_trade(t));
    EXPECT_EQ(t.get_bid_info().client_id_, 1u);
    EXPECT_EQ(t.get_ask_info().client_id_, 2u);
    EXPECT_EQ(t.get_bid_info().quantity, 10u);
    EXPECT_EQ(t.get_ask_info().quantity, 10u);
    EXPECT_EQ(t.get_bid_info().price, 100u);
}

TEST_F(EngineTest, FullMatchSendsMatchOutboundForBothSides) {
    engine.direct_add(bid(1, 100, 10));
    engine.direct_add(ask(2, 100, 10));
    engine.trigger_matching();

    auto out = drain_outbound();
    ASSERT_EQ(out.size(), 2u);

    bool bid_matched = false, ask_matched = false;
    for (auto& msg : out) {
        EXPECT_EQ(msg.message_type, MessageType::MATCH);
        EXPECT_EQ(msg.status, Status::SUCCESS);
        if (msg.client_id == 1) bid_matched = true;
        if (msg.client_id == 2) ask_matched = true;
    }
    EXPECT_TRUE(bid_matched);
    EXPECT_TRUE(ask_matched);
}

TEST_F(EngineTest, PartialFillBidLargerAskFullyFilled) {
    engine.direct_add(bid(1, 100, 15));
    engine.direct_add(ask(2, 100, 10));
    engine.trigger_matching();

    // ask fully filled, bid has 5 remaining
    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_EQ(engine.ask_level_count(), 0);
    EXPECT_EQ(engine.bid_level_count(), 1);
}

TEST_F(EngineTest, PartialFillAskLargerBidFullyFilled) {
    engine.direct_add(bid(1, 100, 10));
    engine.direct_add(ask(2, 100, 15));
    engine.trigger_matching();

    // bid fully filled, ask has 5 remaining
    EXPECT_EQ(engine.order_count(), 1);
    EXPECT_EQ(engine.bid_level_count(), 0);
    EXPECT_EQ(engine.ask_level_count(), 1);
}

TEST_F(EngineTest, PartialFillProducesOneTrade) {
    engine.direct_add(bid(1, 100, 15));
    engine.direct_add(ask(2, 100, 10));
    engine.trigger_matching();

    Trade t{};
    ASSERT_TRUE(engine.pop_trade(t));
    EXPECT_EQ(t.get_bid_info().quantity, 10u);  // fill quantity = min(15, 10)
    EXPECT_EQ(t.get_ask_info().quantity, 10u);
}

TEST_F(EngineTest, NoCrossNeitherOrderFilled) {
    engine.direct_add(bid(1, 90, 10));
    engine.direct_add(ask(2, 100, 10));
    engine.trigger_matching();

    EXPECT_EQ(engine.order_count(), 2);

    Trade t{};
    EXPECT_FALSE(engine.pop_trade(t));
}

TEST_F(EngineTest, FIFOFirstBidAtSamePriceMatchedFirst) {
    OrderId bid1 = engine.direct_add(bid(1, 100, 5));
    OrderId bid2 = engine.direct_add(bid(2, 100, 5));
    engine.direct_add(ask(3, 100, 5));  // only enough to fill one bid
    engine.trigger_matching();

    // bid1 was queued first → matched first
    EXPECT_FALSE(engine.has_order(bid1));
    EXPECT_TRUE(engine.has_order(bid2));
    EXPECT_EQ(engine.order_count(), 1);
}

TEST_F(EngineTest, BestBidPriceMatchedFirst) {
    // Higher bid price is best; BidLevels uses std::greater so *begin() == highest
    OrderId bid_high = engine.direct_add(bid(1, 110, 5));
    OrderId bid_low  = engine.direct_add(bid(2, 100, 5));
    engine.direct_add(ask(3, 100, 5));  // ask crosses both, but only fills one
    engine.trigger_matching();

    EXPECT_FALSE(engine.has_order(bid_high));  // 110 matched first
    EXPECT_TRUE(engine.has_order(bid_low));
}

TEST_F(EngineTest, BestAskPriceMatchedFirst) {
    // Lower ask price is best; AskLevels uses std::less so *begin() == lowest
    OrderId ask_low  = engine.direct_add(ask(1, 90, 5));
    OrderId ask_high = engine.direct_add(ask(2, 100, 5));
    engine.direct_add(bid(3, 100, 5));  // bid crosses both, but only fills one
    engine.trigger_matching();

    EXPECT_FALSE(engine.has_order(ask_low));   // 90 matched first
    EXPECT_TRUE(engine.has_order(ask_high));
}

TEST_F(EngineTest, MultipleSequentialMatchesInOneCall) {
    // Large ask consumes two bids queued at the same price level
    OrderId bid1 = engine.direct_add(bid(1, 100, 5));
    OrderId bid2 = engine.direct_add(bid(2, 100, 5));
    engine.direct_add(ask(3, 100, 10));
    engine.trigger_matching();

    EXPECT_FALSE(engine.has_order(bid1));
    EXPECT_FALSE(engine.has_order(bid2));
    EXPECT_EQ(engine.order_count(), 0);

    // Two separate trades should have been recorded
    Trade t{};
    EXPECT_TRUE(engine.pop_trade(t));
    EXPECT_TRUE(engine.pop_trade(t));
}

TEST_F(EngineTest, MatchAfterNewMessageThroughRing) {
    // End-to-end: both orders submitted via the message path, then matched
    engine.push_inbound(make_new(1, Side::BID, 100, 10));
    engine.push_inbound(make_new(2, Side::ASK, 100, 10));
    engine.trigger_matching();

    EXPECT_EQ(engine.order_count(), 0);

    Trade t{};
    EXPECT_TRUE(engine.pop_trade(t));
}