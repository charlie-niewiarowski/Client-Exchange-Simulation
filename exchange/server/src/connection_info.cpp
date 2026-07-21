//
// Created by cniew on 5/16/26.
//

#include "../include/connection_info.hpp"

OutboundMessage OutboundState::pop_outbound() {
    OutboundMessage msg{};
    staging_.pop(msg);
    return msg;
}