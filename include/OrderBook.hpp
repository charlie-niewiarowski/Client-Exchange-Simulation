//
// Created by charl on 1/17/2026.
//

#ifndef UNTITLED_ORDERBOOK_HPP
#define UNTITLED_ORDERBOOK_HPP

#include "Engine.hpp"
#include "OrderGateway.hpp"

class OrderBook {
    boost::asio::io_context io_;
    OrderGateway gateway_;
    Engine engine_;
public:
    OrderBook(Port port);
    void run();
private:
    using Core = uint8_t;
    inline void pin_thread(Core core);
};


#endif //UNTITLED_ORDERBOOK_HPP