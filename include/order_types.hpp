//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_TYPES_H
#define UNTITLED_TYPES_H

#include <cstdint>

enum class Side : uint8_t { BID, ASK };
enum class OrderType : uint8_t { MARKET, LIMIT };
using OrderId = uint32_t;
using Price = uint32_t;
using Quantity = uint32_t;

class OrderRequest {
    Side side;
    OrderType type;
    Price price;
    Quantity quantity;
public:
    OrderRequest(Side side, OrderType type, Price price, Quantity quantity)
        : side(side), type(type), price(price), quantity(quantity) {}

    Side get_side() const { return side; }
    OrderType get_type() const { return type; }
    Price get_price() const { return price; }
    Quantity get_quantity() const { return quantity; }
};

#endif //UNTITLED_TYPES_H