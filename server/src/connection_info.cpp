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

void ConnectionInfo::set_error(const std::string_view msg) {
    assert(msg.length() <= MAX_ERROR_MSG_LEN);

    error_ = true;
    err_msg_ = msg;
}
