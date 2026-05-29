//
// Created by cniew on 5/16/26.
//

#include "../include/connection_info.h"

OutboundMessage OutboundState::pop_outbound() {
    OutboundMessage msg{};
    staging_.pop(msg);
    return msg;
}