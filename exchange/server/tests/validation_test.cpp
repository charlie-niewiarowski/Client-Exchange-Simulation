//
// GoogleTest coverage for the four validation functions in validation.hpp.
// Pure logic, no networking. Assertions lock in the *current* behavior:
//   - message_type must match the function (NEW=0 / CANCEL=1 / MODIFY=2)
//   - Side and OrderType must be in range {0,1}
//   - LIMIT orders with price==0 are rejected (regardless of side)
//   - quantity > 1,000,000 is rejected
//

#include "validation.hpp"

#include <gtest/gtest.h>

namespace {

InboundMessage make_new(ClientId cid, Side side, Price price, Quantity qty,
                        OrderType type = OrderType::LIMIT) {
    InboundMessage m{};
    m.client_id    = cid;
    m.price        = price;
    m.quantity     = qty;
    m.message_type = MessageType::NEW;
    m.side         = side;
    m.order_type   = type;
    return m;
}

InboundMessage make_cancel(ClientId cid, OrderId oid) {
    InboundMessage m{};
    m.client_id    = cid;
    m.order_id     = oid;
    m.message_type = MessageType::CANCEL;
    m.side         = Side::BID;
    m.order_type   = OrderType::LIMIT;
    return m;
}

InboundMessage make_modify(ClientId cid, OrderId oid, Side side, Price price, Quantity qty) {
    InboundMessage m{};
    m.client_id    = cid;
    m.order_id     = oid;
    m.price        = price;
    m.quantity     = qty;
    m.message_type = MessageType::MODIFY;
    m.side         = side;
    m.order_type   = OrderType::LIMIT;
    return m;
}

constexpr Quantity MAX_QTY = 1'000'000u;

} // namespace

//=============================================================================
// validate_new
//=============================================================================

TEST(ValidateNew, BidLimitValid)  { EXPECT_TRUE(validate_new(make_new(1, Side::BID, 100, 10))); }
TEST(ValidateNew, AskLimitValid)  { EXPECT_TRUE(validate_new(make_new(1, Side::ASK, 100, 10))); }

TEST(ValidateNew, BidLimitPriceZeroFails) {
    EXPECT_FALSE(validate_new(make_new(1, Side::BID, 0, 10)));
}
TEST(ValidateNew, AskLimitPriceZeroFails) {
    // current logic rejects any LIMIT with price==0, independent of side
    EXPECT_FALSE(validate_new(make_new(1, Side::ASK, 0, 10)));
}

TEST(ValidateNew, MarketPriceZeroValid) {
    EXPECT_TRUE(validate_new(make_new(1, Side::ASK, 0, 5, OrderType::MARKET)));
    EXPECT_TRUE(validate_new(make_new(1, Side::BID, 0, 5, OrderType::MARKET)));
}

TEST(ValidateNew, ZeroQuantityPasses)     { EXPECT_TRUE(validate_new(make_new(1, Side::BID, 100, 0))); }
TEST(ValidateNew, MaxQuantityPasses)      { EXPECT_TRUE(validate_new(make_new(1, Side::BID, 100, MAX_QTY))); }
TEST(ValidateNew, ExcessiveQuantityFails) { EXPECT_FALSE(validate_new(make_new(1, Side::BID, 100, MAX_QTY + 1))); }

TEST(ValidateNew, WrongMessageTypeFails) {
    InboundMessage m = make_new(1, Side::BID, 100, 10);
    m.message_type = MessageType::CANCEL;
    EXPECT_FALSE(validate_new(m));
}
TEST(ValidateNew, InvalidSideFails) {
    InboundMessage m = make_new(1, Side::BID, 100, 10);
    m.side = static_cast<Side>(99);
    EXPECT_FALSE(validate_new(m));
}
TEST(ValidateNew, InvalidOrderTypeFails) {
    InboundMessage m = make_new(1, Side::BID, 100, 10);
    m.order_type = static_cast<OrderType>(99);
    EXPECT_FALSE(validate_new(m));
}

//=============================================================================
// validate_cancel  (only the message_type is checked)
//=============================================================================

TEST(ValidateCancel, Valid)              { EXPECT_TRUE(validate_cancel(make_cancel(1, 42))); }
TEST(ValidateCancel, AnyOrderIdValid) {
    EXPECT_TRUE(validate_cancel(make_cancel(1, 0)));
    EXPECT_TRUE(validate_cancel(make_cancel(1, 999999)));
}
TEST(ValidateCancel, NewTypeFails) {
    InboundMessage m = make_cancel(1, 42);
    m.message_type = MessageType::NEW;
    EXPECT_FALSE(validate_cancel(m));
}
TEST(ValidateCancel, ModifyTypeFails) {
    InboundMessage m = make_cancel(1, 42);
    m.message_type = MessageType::MODIFY;
    EXPECT_FALSE(validate_cancel(m));
}

//=============================================================================
// validate_modify
//=============================================================================

TEST(ValidateModify, Valid)               { EXPECT_TRUE(validate_modify(make_modify(1, 42, Side::BID, 100, 10))); }
TEST(ValidateModify, BidLimitPriceZeroFails) { EXPECT_FALSE(validate_modify(make_modify(1, 42, Side::BID, 0, 10))); }
TEST(ValidateModify, AskLimitPriceZeroFails) { EXPECT_FALSE(validate_modify(make_modify(1, 42, Side::ASK, 0, 10))); }
TEST(ValidateModify, ExcessiveQuantityFails) { EXPECT_FALSE(validate_modify(make_modify(1, 42, Side::BID, 100, MAX_QTY + 1))); }

TEST(ValidateModify, WrongMessageTypeFails) {
    InboundMessage m = make_modify(1, 42, Side::BID, 100, 10);
    m.message_type = MessageType::NEW;
    EXPECT_FALSE(validate_modify(m));
}
TEST(ValidateModify, InvalidSideFails) {
    InboundMessage m = make_modify(1, 42, Side::BID, 100, 10);
    m.side = static_cast<Side>(99);
    EXPECT_FALSE(validate_modify(m));
}
TEST(ValidateModify, InvalidOrderTypeFails) {
    InboundMessage m = make_modify(1, 42, Side::BID, 100, 10);
    m.order_type = static_cast<OrderType>(99);
    EXPECT_FALSE(validate_modify(m));
}

//=============================================================================
// validate_message dispatch
//=============================================================================

TEST(ValidateMessage, DispatchesNew) {
    EXPECT_TRUE(validate_message(make_new(1, Side::BID, 100, 10)));
    EXPECT_FALSE(validate_message(make_new(1, Side::BID, 0, 10))); // invalid NEW
}
TEST(ValidateMessage, DispatchesCancel) { EXPECT_TRUE(validate_message(make_cancel(1, 42))); }
TEST(ValidateMessage, DispatchesModify) { EXPECT_TRUE(validate_message(make_modify(1, 42, Side::BID, 100, 10))); }
TEST(ValidateMessage, UnknownTypeFails) {
    InboundMessage m{};
    m.message_type = MessageType::MATCH; // not a client-originated type
    EXPECT_FALSE(validate_message(m));
}
