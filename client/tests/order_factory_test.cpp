//
// GoogleTest coverage for OrderFactory::build_frame (client-side order gen).
// Verifies the wire framing and the invariants of each generated message kind.
// OrderFactory is a process-wide singleton whose seed is fixed on the first
// instance() call; these tests assert seed-independent invariants.
//

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace {

constexpr size_t HLEN = sizeof("EXCHANGE\n") - 1;    // 9
constexpr size_t BODY = sizeof(InboundMessage);      // 56
constexpr Quantity MAX_QTY = 1'000'000u;

} // namespace

TEST(OrderFactory, FrameHasHeaderBodyTrailer) {
    OrderFactory& f = OrderFactory::instance(12345);
    char dst[INBOUND_BSIZE]{};
    InboundMessage msg{};
    f.build_frame(/*cid=*/7, /*active_order=*/0, dst, msg);

    EXPECT_EQ(0, std::memcmp(dst, "EXCHANGE\n", HLEN));   // header
    EXPECT_EQ(0, std::memcmp(dst + HLEN, &msg, BODY));    // body mirrors msg_out
    EXPECT_EQ('\n', dst[HLEN + BODY]);                    // trailer
}

TEST(OrderFactory, PropagatesClientId) {
    OrderFactory& f = OrderFactory::instance();
    char dst[INBOUND_BSIZE]{};
    InboundMessage msg{};
    f.build_frame(/*cid=*/42, /*active_order=*/0, dst, msg);
    EXPECT_EQ(42u, msg.client_id);
}

// Over many rolls: the returned FrameKind always matches the emitted
// message_type, the body always round-trips into the frame, and each kind
// satisfies its field invariants.
TEST(OrderFactory, KindMatchesBodyAndInvariants) {
    OrderFactory& f = OrderFactory::instance();
    constexpr OrderId ACTIVE = 100;

    for (int i = 0; i < 20000; ++i) {
        char dst[INBOUND_BSIZE]{};
        InboundMessage msg{};
        const OrderFactory::FrameKind kind = f.build_frame(/*cid=*/1, ACTIVE, dst, msg);

        ASSERT_EQ(0, std::memcmp(dst + HLEN, &msg, BODY)) << "frame body must mirror msg_out";
        ASSERT_EQ(1u, msg.client_id);

        switch (kind) {
        case OrderFactory::FrameKind::NEW:
            EXPECT_EQ(MessageType::NEW, msg.message_type);
            EXPECT_GE(msg.quantity, 1u);
            EXPECT_LE(msg.quantity, MAX_QTY);
            if (msg.order_type == OrderType::LIMIT)
                EXPECT_GE(msg.price, 1u);       // limit_price clamps to >= 1
            else
                EXPECT_EQ(0u, msg.price);        // market order carries price 0
            break;

        case OrderFactory::FrameKind::CANCEL:
            EXPECT_EQ(MessageType::CANCEL, msg.message_type);
            EXPECT_EQ(ACTIVE, msg.order_id);
            break;

        case OrderFactory::FrameKind::MODIFY:
            EXPECT_EQ(MessageType::MODIFY, msg.message_type);
            EXPECT_EQ(ACTIVE, msg.order_id);
            EXPECT_EQ(OrderType::LIMIT, msg.order_type);
            EXPECT_GE(msg.price, 1u);
            EXPECT_GE(msg.quantity, 1u);
            EXPECT_LE(msg.quantity, MAX_QTY);
            break;

        default:
            FAIL() << "garbage/invalid kinds are disabled; got unexpected kind";
        }
    }
}
