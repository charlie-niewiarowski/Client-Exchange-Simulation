//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include "connection_info.h"
#include "communication_types.h"

#include <sys/epoll.h>
#include <unordered_set>

#define MAX_CLIENTS 100
#define PORT "4000"

class Server {
public:
    // Special member functions, the only one that matters is the defined constructor
    Server(InboundRing& in, OutboundRing& out);
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
private:
    //===== data structures =====
    std::unordered_map<int, ConnectionInfo> info_map_; // map of file descriptors to important info about connection
                                                       // this data structure owns the ConnectionInfo
    std::unordered_map<ClientId, ConnectionInfo*> client_map_; // map of ClientIds to ConnectionInfo* for routing
                                                              // uses pointers because lack of ownership
    // should reference actual objects in memory that were provided by the parent class
    InboundRing& in_ring_;
    OutboundRing& out_ring_;

    // epoll data
    int epoll_fd_ = -1;
    epoll_event events_[MAX_CLIENTS];

    std::unordered_set<int> writable_fds_;

    //===== run() helpers =====
    std::optional<EpollSocket> setupListenSocket();
    bool setupEpoll();

    std::optional<int> registerConnection(int listen_fd);
    void handleRequests(int client_fd);
    void finishBlockedWrites(int client_fd);

    void drainOutboundRing();
    void handleResponses();

    //===== read pipeline helpers =====
    void readRequest(ConnectionInfo &info);
    void parseRequest(ConnectionInfo& info);
    void executeRequest(ConnectionInfo& info);

    //===== write pipeline helpers =====
    void serializeResponse(ConnectionInfo &info);
    void sendResponse(ConnectionInfo& info);
    void closeConnection(int client_fd);
    void finishResponseCycle(ConnectionInfo& info);
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP