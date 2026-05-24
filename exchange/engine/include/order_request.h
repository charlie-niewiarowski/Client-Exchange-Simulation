//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_TYPES_H
#define UNTITLED_TYPES_H

#include "order_types.h"

class OrderRequest {
    ClientId client_id;
    Side side;
    OrderType type;
    Price price;
    Quantity quantity;
public:
    OrderRequest(ClientId client_id, Side side, OrderType type, Price price, Quantity quantity)
        : client_id(client_id), side(side), type(type), price(price), quantity(quantity) {}

    ClientId get_clientId() const { return client_id; }
    Side get_side() const { return side; }
    OrderType get_type() const { return type; }
    Price get_price() const { return price; }
    Quantity get_quantity() const { return quantity; }
};

#endif //UNTITLED_TYPES_H