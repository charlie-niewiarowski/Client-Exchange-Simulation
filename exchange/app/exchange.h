//
// Created by charl on 1/17/2026.
//

#ifndef UNTITLED_ORDERBOOK_HPP
#define UNTITLED_ORDERBOOK_HPP

#include "../engine/include/engine.h"
#include "../server/include/server.h"
#include "macros.h"

class Exchange {
public:
    Exchange();
    void run();
private:
    InboundRing in_ring_{COMMUNICATION_RING_COUNT};
    OutboundRing out_ring_{COMMUNICATION_RING_COUNT};

    Engine engine_;
    Server server_;
};


#endif //UNTITLED_ORDERBOOK_HPP