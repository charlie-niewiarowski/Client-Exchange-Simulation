//
// Created by cniew on 5/24/26.
//
// Header-only factory for building randomised outbound frames.
//
// Probability table (uniform roll over [0, 99]):
//   roll > 40  (59 / 100) → NEW   order
//   roll > 15  (25 / 100) → CANCEL order
//   roll >  5  (10 / 100) → MODIFY order
//   roll <= 5  ( 6 / 100) → INVALID / MALFORMED
//
// All frames are exactly INBOUND_BSIZE bytes:
//   "EXCHANGE\n"  (9 bytes)
//   InboundMessage (20 bytes)
//   '\n'          ( 1 byte)
// ─────────────────────────── 30 bytes
//

#ifndef ORDER_FACTORY_H
#define ORDER_FACTORY_H

#include "communication_types.h"
#include "protocol.h"

#include <random>
#include <cstring>

namespace OrderFactory {

// ─────────────────────────────────────────────────────────────────────────────
// Tag that tells the caller what kind of frame was built.
// GARBAGE  → random bytes (wrong header)  → server returns MALFORMED_REQUEST
// INVALID  → valid header, bad body       → server returns INVALID_ORDER
// ─────────────────────────────────────────────────────────────────────────────
enum class FrameKind : uint8_t { NEW, CANCEL, MODIFY, GARBAGE, INVALID };

// ─────────────────────────────────────────────────────────────────────────────
// Individual message builders (do NOT include framing)
// ─────────────────────────────────────────────────────────────────────────────

inline InboundMessage make_new(const ClientId cid, std::mt19937& rng) {
    static std::uniform_int_distribution<Price>    price_dist{1, 10'000};
    static std::uniform_int_distribution<Quantity> qty_dist  {1, 1'000};
    static std::uniform_int_distribution<int>      side_dist {0, 1};
    static std::uniform_int_distribution<int>      type_dist {0, 1};

    const auto order_type = static_cast<OrderType>(type_dist(rng));

    return InboundMessage{
        .client_id    = cid,
        .order_id     = 0,                                      // junk – server ignores for NEW
        .price        = (order_type == OrderType::LIMIT)
                            ? price_dist(rng) : Price{0},
        .quantity     = qty_dist(rng),
        .message_type = MessageType::NEW,
        .side         = static_cast<Side>(side_dist(rng)),
        .order_type   = order_type,
        .padding      = 0,
    };
}

inline InboundMessage make_cancel(const ClientId cid, const OrderId target_oid) {
    // Validation only checks message_type == CANCEL; other fields are ignored by
    // the server. The engine will return Status::FAILURE if the order is unknown
    // or owned by a different client (still an OK-framed response).
    return InboundMessage{
        .client_id    = cid,
        .order_id     = target_oid,
        .price        = 0,
        .quantity     = 0,
        .message_type = MessageType::CANCEL,
        .side         = Side::BID,         // unused by server/engine for CANCEL
        .order_type   = OrderType::LIMIT,  // unused
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

// ─────────────────────────────────────────────────────────────────────────────
// make_invalid_body
//   Valid "EXCHANGE\n" header + an InboundMessage that fails validate_message().
//   Two sub-variants (coin flip):
//     a) LIMIT NEW with price == 0  → INVALID_ORDER
//     b) ANY  NEW with qty > 1 000 000  → INVALID_ORDER
// ─────────────────────────────────────────────────────────────────────────────
inline InboundMessage make_invalid_body(const ClientId cid, std::mt19937& rng) {
    static std::uniform_int_distribution<int>      coin    {0, 1};
    static std::uniform_int_distribution<Quantity> big_qty {1'000'001u, 2'000'000u};

    if (coin(rng) == 0) {
        // LIMIT BID with price == 0 → fails validate_new()
        return InboundMessage{
            .client_id    = cid,
            .order_id     = 0,
            .price        = 0,
            .quantity     = 100,
            .message_type = MessageType::NEW,
            .side         = Side::BID,
            .order_type   = OrderType::LIMIT,
            .padding      = 0,
        };
    }

    // NEW with quantity > 1 000 000 → fails validate_new()
    return InboundMessage{
        .client_id    = cid,
        .order_id     = 0,
        .price        = 100,
        .quantity     = big_qty(rng),
        .message_type = MessageType::NEW,
        .side         = Side::BID,
        .order_type   = OrderType::LIMIT,
        .padding      = 0,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// frame_message
//   Wraps an InboundMessage in the exchange wire format and writes it into
//   dst[0..INBOUND_BSIZE-1].
// ─────────────────────────────────────────────────────────────────────────────
inline void frame_message(const InboundMessage& msg, char* dst) {
    constexpr size_t header_len = sizeof("EXCHANGE\n") - 1; // 9
    memcpy(dst, "EXCHANGE\n", header_len);
    memcpy(dst + header_len, &msg, sizeof(msg));
    dst[header_len + sizeof(msg)] = '\n';
}

// ─────────────────────────────────────────────────────────────────────────────
// build_frame
//   Top-level entry point: rolls the RNG, selects a request type, fills
//   dst[0..INBOUND_BSIZE-1], and returns the FrameKind for logging.
//
//   active_order  – a known live order ID to use for CANCEL/MODIFY;
//                   pass 0 if none are available (still sends valid wire format,
//                   engine will return Status::FAILURE for unknown orders).
//   msg_out       – populated with the InboundMessage for NEW/CANCEL/MODIFY
//                   so the caller can log fields; zeroed for GARBAGE/INVALID.
// ─────────────────────────────────────────────────────────────────────────────
inline FrameKind build_frame(const ClientId          cid,
                             const OrderId           active_order,
                             char*                   dst,
                             InboundMessage&         msg_out,
                             std::mt19937&           rng) {
    static std::uniform_int_distribution<int> roll{0, 99};
    static std::uniform_int_distribution<uint8_t> byte_dist{0, 255};

    const int r = roll(rng);
    msg_out = {};

    if (r > 40) {
        // ── NEW ──────────────────────────────────────────────────────────────
        msg_out = make_new(cid, rng);
        frame_message(msg_out, dst);
        return FrameKind::NEW;
    }

    if (r > 15) {
        // ── CANCEL ───────────────────────────────────────────────────────────
        // active_order == 0 means no tracked order; send junk ID anyway –
        // the engine returns Status::FAILURE (OK-framed) which exercises that path.
        msg_out = make_cancel(cid, active_order);
        frame_message(msg_out, dst);
        return FrameKind::CANCEL;
    }

    if (r > 5) {
        // ── MODIFY ───────────────────────────────────────────────────────────
        msg_out = make_modify(cid, active_order, rng);
        frame_message(msg_out, dst);
        return FrameKind::MODIFY;
    }

    // ── INVALID / MALFORMED (r <= 5) ─────────────────────────────────────────
    static std::uniform_int_distribution<int> coin{0, 1};
    if (coin(rng) == 0) {
        // Pure garbage: random bytes → MALFORMED_REQUEST
        for (size_t i = 0; i < INBOUND_BSIZE; ++i) {
            dst[i] = static_cast<char>(byte_dist(rng));
        }
        return FrameKind::GARBAGE;
    }

    // Valid header, invalid body → INVALID_ORDER
    msg_out = make_invalid_body(cid, rng);
    frame_message(msg_out, dst);
    return FrameKind::INVALID;
}

} // namespace OrderFactory

#endif // ORDER_FACTORY_H
