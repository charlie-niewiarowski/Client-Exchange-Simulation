//
// Created by cniew on 5/16/26.
//

#include "../include/connection_info.h"

#include <cstring>

OutboundMessage ConnectionInfo::pop_outbound() {
    OutboundMessage msg{};
    outbound_.pop(msg);
    return msg;
}
