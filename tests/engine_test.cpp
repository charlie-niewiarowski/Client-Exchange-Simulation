//
// Created by charlie on 11/26/2025.
//

#include "../include/Engine.hpp"
#include <gtest/gtest.h>

#include <string>
#include <iostream>

class OrderBookTest : public ::testing::Test {
protected:
    Engine book;

    OrderRequest make_limit(Side side, double px, Quantity qty) {
        return OrderRequest(side, OrderType::LIMIT, px, qty);
    }

    OrderRequest make_market(Side side, Quantity qty) {
        return OrderRequest(side, OrderType::MARKET, 0.0, qty);
    }
};

TEST_F(OrderBookTest, NewBookIsEmpty) {
}

int main() {
    testing::InitGoogleTest();
    return RUN_ALL_TESTS();
}