//
// Created by cniew on 5/24/26.
//
// Header-only factory: builds randomised outbound wire frames.
//
// Probability table (uniform roll over [0, 99]):
//   roll > 40  (59 / 100)  → NEW    order
//   roll > 15  (25 / 100)  → CANCEL order
//   roll >  5  (10 / 100)  → MODIFY order
//   roll <= 5  ( 6 / 100)  → INVALID / MALFORMED
//
// Every frame is exactly INBOUND_BSIZE bytes:
//   "EXCHANGE\n"   (9 bytes)
//   InboundMessage (20 bytes)
//   '\n'           ( 1 byte)
//  ─────────────────────────  30 bytes total
//

#ifndef ORDER_FACTORY_H
#define ORDER_FACTORY_H

#include "communication_types.h"
#include "protocol.h"

#include <random>
#include <cstring>

namespace OrderFactory {

// ─────────────────────────────────────────────────────────────────────────────
// Tag reported back to the caller so it can log what was built.
// GARBAGE  – random bytes in the header zone        → MALFORMED_REQUEST
// INVALID  – correct header, body fails validation  → INVALID_ORDER
// ─────────────────────────────────────────────────────────────────────────────
enum class FrameKind : uint8_t { NEW, CANCEL, MODIFY, GARBAGE, INVALID };

// ─────────────────────────────────────────────────────────────────────────────
// Per-message-type builders (return an InboundMessage, no framing)
// ─────────────────────────────────────────────────────────────────────────────

inline InboundMessage make_new(const ClientId cid, std::mt19937& rng) {
    static std::uniform_int_distribution<Price>    price_dist{1, 10'000};
    static std::uniform_int_distribution<Quantity> qty_dist  {1, 1'000};
    static std::uniform_int_distribution<int>      side_dist {0, 1};
    static std::uniform_int_distribution<int>      type_dist {0, 1};

    const auto order_type = static_cast<OrderType>(type_dist(rng));
    return InboundMessage{
        .client_id    = cid,
        .order_id     = 0,
        .price        = (order_type == OrderType::LIMIT) ? price_dist(rng) : Price{0},
        .quantity     = qty_dist(rng),
        .message_type = MessageType::NEW,
        .side         = static_cast<Side>(side_dist(rng)),
        .order_type   = order_type,
        .padding      = 0,
    };
}

inline InboundMessage make_cancel(const ClientId cid, const OrderId target_oid) {
    // Server validation only checks message_type == CANCEL; all other fields
    // are irrelevant.  If target_oid is unknown the engine returns FAILURE
    // (still an OK-framed response) — exercising that error path is useful.
    return InboundMessage{
        .client_id    = cid,
        .order_id     = target_oid,
        .price        = 0,
        .quantity     = 0,
        .message_type = MessageType::CANCEL,
        .side         = Side::BID,
        .order_type   = OrderType::LIMIT,
        .padding      = 0,
    };
}

inline InboundMessage make_modify(const ClientId cid, const OrderId target_oid,
                                  std::mt19937& rng) {
    static std::uniform_int_distribution<Price>    price_dist{1, 10'000};
    static std::uniform_int_distribution<Quantity> qty_dist  {1, 1'000};
    static std::uniform_int_distribution<int>      side_dist {0, 1};

    return InboundMessage{
        .client_id    = cid,
        .order_id     = target_oid,
        .price        = price_dist(rng),
        .quantity     = qty_dist(rng),
        .message_type = MessageType::MODIFY,
        .side         = static_cast<Side>(side_dist(rng)),
        .order_type   = OrderType::LIMIT,
        .padding      = 0,
    };
}

inline InboundMessage make_invalid_body(const ClientId cid, std::mt19937& rng) {
    // Correct "EXCHANGE\n" header will be prepended; body itself fails
    // validate_message() in one of two ways (coin flip):
    //   a) LIMIT NEW with price == 0
    //   b) NEW with quantity > 1 000 000
    static std::uniform_int_distribution<int>      coin    {0, 1};
    static std::uniform_int_distribution<Quantity> big_qty {1'000'001u, 2'000'000u};

    if (coin(rng) == 0) {
        return InboundMessage{
            .client_id    = cid,
            .order_id     = 0,
            .price        = 0,         // LIMIT + price==0 → invalid
            .quantity     = 100,
            .message_type = MessageType::NEW,
            .side         = Side::BID,
            .order_type   = OrderType::LIMIT,
            .padding      = 0,
        };
    }
    return InboundMessage{
        .client_id    = cid,
        .order_id     = 0,
        .price        = 100,
        .quantity     = big_qty(rng), // > 1 000 000 → invalid
        .message_type = MessageType::NEW,
        .side         = Side::BID,
        .order_type   = OrderType::LIMIT,
        .padding      = 0,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// frame_message
//   Writes "EXCHANGE\n" + msg + '\n' into dst[0..INBOUND_BSIZE).
// ─────────────────────────────────────────────────────────────────────────────
inline void frame_message(const InboundMessage& msg, char* dst) {
    constexpr size_t hlen = sizeof("EXCHANGE\n") - 1; // 9
    std::memcpy(dst, "EXCHANGE\n", hlen);
    std::memcpy(dst + hlen, &msg, sizeof(msg));
    dst[hlen + sizeof(msg)] = '\n';
}

// ─────────────────────────────────────────────────────────────────────────────
// build_frame
//   Top-level entry: rolls RNG, picks a frame type, fills dst[INBOUND_BSIZE],
//   and returns the FrameKind for logging.
//
//   active_order  – a known live order ID for CANCEL / MODIFY targeting.
//                   Pass 0 when no live orders are tracked; the engine will
//                   return Status::FAILURE (still an OK-framed response),
//                   which exercises that code path.
//   msg_out       – populated with the InboundMessage for NEW/CANCEL/MODIFY so
//                   the caller can log price/qty/etc.; zeroed for GARBAGE.
// ─────────────────────────────────────────────────────────────────────────────
inline FrameKind build_frame(const ClientId  cid,
                             const OrderId   active_order,
                             char*           dst,
                             InboundMessage& msg_out,
                             std::mt19937&   rng) {
    static std::uniform_int_distribution<int>     roll     {0, 99};
    static std::uniform_int_distribution<uint8_t> byte_dist{0, 255};
    static std::uniform_int_distribution<int>     coin     {0, 1};

    const int r = roll(rng);
    msg_out = {};

    if (r > 40) {
        msg_out = make_new(cid, rng);
        frame_message(msg_out, dst);
        return FrameKind::NEW;
    }

    if (r > 15) {
        msg_out = make_cancel(cid, active_order);
        frame_message(msg_out, dst);
        return FrameKind::CANCEL;
    }

    if (r > 5) {
        msg_out = make_modify(cid, active_order, rng);
        frame_message(msg_out, dst);
        return FrameKind::MODIFY;
    }

    // INVALID / MALFORMED (r <= 5) — 50 / 50 between garbage bytes and
    // a well-framed but validate_message()-failing body.
    if (coin(rng) == 0) {
        for (size_t i = 0; i < INBOUND_BSIZE; ++i)
            dst[i] = static_cast<char>(byte_dist(rng));
        return FrameKind::GARBAGE;
    }

    msg_out = make_invalid_body(cid, rng);
    frame_message(msg_out, dst);
    return FrameKind::INVALID;
}

} // namespace OrderFactory

#endif // ORDER_FACTORY_H
