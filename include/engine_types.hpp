//
// Created by charl on 1/17/2026.
//

#ifndef UNTITLED_ORDERBOOK_TYPES_HPP
#define UNTITLED_ORDERBOOK_TYPES_HPP

#include <deque>
#include <map>
#include <unordered_map>
#include <functional>

#include "order_types.hpp"

using PriceLevelQueue = std::deque<OrderId>;
using BidLevels = std::map<Price, PriceLevelQueue, std::greater<>>;
using AskLevels = std::map<Price, PriceLevelQueue, std::less<>>;

#endif //UNTITLED_ORDERBOOK_TYPES_HPP