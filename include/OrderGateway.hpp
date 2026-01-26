//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include <optional>
#include <shared_mutex>
#include <winsock2.h>
#include <windows.h>
#include <expected>

#include "communication_types.hpp"
#include "../lib/RingBuffer.hpp"

class OrderGateway {
    HANDLE hMapFile_{nullptr};
    void* base_{nullptr};

    RingBuffer<InboundMessage>* inbound_buff_{nullptr};
    ClientMessageMap* outbound_client_msgs_{nullptr};
        std::shared_mutex client_mux_;

    SOCKET socket_{INVALID_SOCKET};

public:
    OrderGateway();
    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;
    OrderGateway(OrderGateway&&) = delete;
    OrderGateway& operator=(OrderGateway&&) = delete;
    ~OrderGateway();

    void run();
private:
    // constructor setup helpers
    std::expected<void, SetupError> setup_socket();
        std::expected<void, SetupError> initialize_winsock();
        std::expected<void, SetupError> create_socket();
        std::expected<void, SetupError> bind_socket();
        std::expected<void, SetupError> listen_for_requests();

    std::expected<void, SetupError> setup_shared_memory();
        std::expected<void, SetupError> allocate();
        std::expected<void, SetupError> initialize_buffers();

    // destructor helpers
    void unmap_memory();

    // run() helpers
    void handle_client();
        SOCKET accept_connection();
        std::optional<InboundMessage> parse_message(const SOCKET& accept_socket);
        int send_fail(SOCKET client_socket, InboundMessage msg);
        int send_response(SOCKET client_socket, OutboundMessage msg);

    // validation engine
    bool validate_message(const InboundMessage& msg);
        bool validate_new(const InboundMessage& msg);
        bool validate_cancel(const InboundMessage& msg);
        bool validate_modify(const InboundMessage& msg);
};


#endif //TOYEXCHANGE_ORDERGATEWAY_HPP