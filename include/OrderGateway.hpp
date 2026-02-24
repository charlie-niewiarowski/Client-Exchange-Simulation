//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include <memory>
#include <array>
#include <utility>
#include <boost/asio.hpp>
#include <unordered_map>

#include "communication_types.hpp"
#include "order_gateway_types.hpp"
#include "../lib/RingBuffer.hpp"

class OrderGateway {
    // Session class used to represent TCP session
    class Session : public std::enable_shared_from_this<Session> {
        OrderGateway& gateway_;
        boost::asio::ip::tcp::socket socket_;
        std::array<char, sizeof(InboundMessage)> buffer_{};

    public:
        Session(OrderGateway& g, boost::asio::ip::tcp::socket socket) :
            gateway_(g), socket_{std::move(socket)} {}

        void read(const ClientId& client_id);
        void write(std::array<char, sizeof(OutboundMessage)> msg);
    };

    // Underlying structures to help the gateway manage sessions
    using SessionPtr = std::shared_ptr<Session>;
    using Sessions = std::unordered_map<ClientId, SessionPtr>;
    std::atomic<ClientId> next_client_id_{0};
    Sessions sessions_;

    /**
     * Ring buffer pointers that point to shared memory
     * tx_dispatch_pending is an atomic flag that tells us if we're already draining the tx buffer or not
     * rx_notify_ represents the function that should be called when the gateway wants to notify
    **/
    RingBuffer<InboundMessage>* rx_ring_{nullptr};
    RingBuffer<OutboundMessage>* tx_ring_{nullptr};
    std::atomic_bool tx_dispatch_pending{false};
    std::function<void()> rx_notify_;

    // boost::asio types used to boot up listening socket
    boost::asio::io_context& io_;
    boost::asio::ip::tcp::acceptor acceptor_;
public:
    // Special member functions, the only one that matters is the defined constructor
    OrderGateway(boost::asio::io_context& io, Port port);
    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;
    OrderGateway(OrderGateway&&) = delete;
    OrderGateway& operator=(OrderGateway&&) = delete;
    ~OrderGateway() = default; // we don't allocate any memory using new so the compiler can generate this for us

    // start accepting connections
    void start_accept();

    // triggered everytime the engine pushes an outbound message
    void tx_push_trigger();
    // allows the engine to set the gateway's notifier function
    void set_rx_notify(const std::function<void()>& func) { rx_notify_ = func; }
private:
    // entry point for client/server interaction (sessions)
    void handle_client(boost::asio::ip::tcp::socket socket);
    // main mechanisms for the tx trigger
    void drain_tx_ring();
        void route_tx_message(const OutboundMessage& msg);

    // validation engine
    bool validate_message(const InboundMessage& msg);
        bool validate_new(const InboundMessage& msg);
        bool validate_cancel(const InboundMessage& msg);
        bool validate_modify(const InboundMessage& msg);
    void communicate_failure(const InboundMessage& msg);
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP