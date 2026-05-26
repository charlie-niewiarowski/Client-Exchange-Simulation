//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include <array>

#include "connection_info.h"
#include "communication_types.h"
#include "config.h"

#include <sys/epoll.h>
#include <unordered_set>
#include <optional>
#include <unordered_map>


class Server {
public:
    // Special member functions, the only one that matters is the defined constructor
    Server(InboundRing& in, OutboundRing& out, std::atomic<bool>& stop);
    Server() = delete;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    ~Server();

    /*
     * Essentially a main function, other modules don't need to access internal functions.
     *
     * Sets up listen socket and epoll, loops over file descriptors, sets up new connections
     * or handles client connections
    */
    void run();

    // Testing — compiled in only when TESTING is defined in macros.h
    #if TESTING
    void stop() { running_.store(false, std::memory_order_relaxed); }
    #endif
private:
    //===== data structures =====
    std::array<std::optional<ConnectionInfo>, MAX_CLIENTS + 10> info_map_{}; // map of file descriptors to important info about connection
                                                       // this data structure owns the ConnectionInfo
    std::unordered_map<ClientId, ConnectionInfo*> client_map_; // map of ClientIds to ConnectionInfo* for routing
                                                              // uses pointers because lack of ownership
    // should reference actual objects in memory that were provided by the parent class
    InboundRing& in_ring_;
    OutboundRing& out_ring_;

    // Testing — only needed when stop() exists
    #if TESTING
    std::atomic<bool> running_{true};
    #endif

    // epoll data
    int epoll_fd_ = -1;
    epoll_event events_[MAX_CLIENTS]{};

    std::unordered_set<int> writable_fds_;

    // stop
    std::atomic<bool>& stop_;

    //===== setup =====
    std::optional<EpollSocket> setupListenSocket() const;
    bool setupEpoll();

    //===== loop body =====
    std::optional<int> registerConnection(int listen_fd);
    void readAndProcessBytes(int client_fd);
    void finishSend(int client_fd);

    void routeOutboundMessages();
    void beginSends();

    //===== read pipeline helpers =====
    void readRequest(ConnectionInfo &info);
    void processRequest(ConnectionInfo& info);

    //===== write pipeline helpers =====
    static void serializeResponse(ConnectionInfo &info);
    void sendResponse(ConnectionInfo& info);
    void finishResponseCycle(ConnectionInfo& info) const;

    //===== ubiquitous helpers =====
    void closeConnection(int client_fd);
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP