//
// Created by charl on 1/17/2026.
//

#include <thread>
#include "../include/OrderBook.hpp"

OrderBook::OrderBook(Port port) :
    io_{}, gateway_{io_, port}, engine_{} {

    // wire notifications
    engine_.set_tx_notify([&]{
        gateway_.tx_push_trigger(); // engine will call this whenever it wants to notify
    });                            // gateway that it has enqueued an outbound message

    gateway_.set_rx_notify([&]{
        engine_.rx_push_trigger(); // same idea as above but flipped
    });
}

void OrderBook::run() {
    gateway_.start_accept();

    std::thread io_thread([this]{
        io_.run();
    });

    std::thread engine_thread([this]{
        engine_.run();
    });

    engine_thread.join();
    io_thread.join();
}
