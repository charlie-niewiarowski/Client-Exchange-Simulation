//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include <array>
#include <atomic>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>
#include <sys/epoll.h>

#include "connection_info.h"
#include "communication_types.h"
#include "config.h"
#include "ring_buffer.hpp"


// Pushed by handleRequests when a protocol/IO error is detected on a client fd.
// Consumed by serveResponses so the error reply is serialized and sent without
// the inbound thread ever touching outbound-owned state.
struct ErrorRecord {
    int         fd;
    ServerError err;
};

// ─────────────────────────────────────────────────────────────────────────────

class Server {
public:
    Server(InboundRing& in, OutboundRing& out, std::atomic<bool>& stop);
    Server() = delete;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    ~Server();

    void run();

private:
    //===== threads =====
    std::jthread inbound_thread_;
    std::jthread outbound_thread_;

    //===== connection state =====

    // INBOUND-THREAD-OWNED only handleRequests and its callees touch this.
    std::array<std::optional<ConnectionInfo>, MAX_CLIENTS + 10> info_map_{};

    // ClientId -> ConnectionInfo* routing table.
    std::array<std::atomic<ConnectionInfo*>, MAX_CLIENTS + 10> client_map_{};

    //===== cross thread queues =====

    // inbound -> outbound: error responses the inbound thread cannot push itself
    // (push_outbound / writable_fds_ are outbound-thread-owned).
    RingBuffer<ErrorRecord> error_ring_{128};

    // outbound -> inbound: file descriptors that the outbound thread wants the
    // inbound thread to close (because a send failed with a hard error).
    // Outbound condemns and abandons the connection; inbound does the actual
    // close and memory cleanup.
    RingBuffer<int> close_ring_{128};

    // inbound -> outbound: new fd / ConnectionInfo* pairs produced by
    // registerConnection so outbound can populate fd_to_info_.
    RingBuffer<std::pair<int, ConnectionInfo*>> register_ring_{128};

    //===== intermodule rings =====
    InboundRing&  in_ring_;
    OutboundRing& out_ring_;

    //===== stop flag =====
    std::atomic<bool>& stop_;

    //===== inbound epoll =====
    int epoll_in_ = -1;
    epoll_event events_in_[MAX_CLIENTS]{};

    //===== outbound epoll =====
    // epoll_out_ is used only for partial-send completions (EPOLLOUT).
    int epoll_out_ = -1;
    epoll_event events_out_[MAX_CLIENTS]{};

    //===== outbound state =====
    // Set of fds that have messages queued or a partial send in progress.
    std::unordered_set<int> writable_fds_;

    // Outbound-thread's own fd → ConnectionInfo* table.
    // Populated from register_ring_; cleared by condemnConnection.
    // Never shared with the inbound thread.
    std::array<ConnectionInfo*, MAX_CLIENTS + 10> fd_to_info_{};

    // Set to true by condemnConnection.  Checked before every outbound access
    // so stale epoll_out_ events for a condemned fd are safely skipped without
    // ever touching the (possibly already-freed) ConnectionInfo.
    std::array<bool, MAX_CLIENTS + 10> condemned_fds_{};

    // Tracks which fds are currently registered with epoll_out_ for EPOLLOUT.
    // Prevents double EPOLL_CTL_ADD when a partial send is already in progress.
    std::array<bool, MAX_CLIENTS + 10> epollout_armed_{};

    //===== setup =====
    bool setupEpoll();
    [[nodiscard]] std::optional<EpollSocket> setupListenSocket() const;

    //===== handleRequests =====
    void handleRequests();
    std::optional<int> registerConnection(int listen_fd);
    void readAndProcessBytes(int client_fd);
    void readRequest(ConnectionInfo& info);
    void processRequest(ConnectionInfo& info);

    //===== serveResponses =====
    void serveResponses();
    void routeOutboundMessages();
    void beginSends();
    // Pops one message from info.outbound_, serialises it as a fixed-size
    // OUTBOUND_BSIZE frame, and appends it to the write buffer.
    // Also saves a PendingLatency entry so finishBatch can record t6 later.
    static void appendResponse(ConnectionInfo& info);
    void sendResponse(ConnectionInfo& info);
    // Called when the write buffer is fully drained.  Records latency for every
    // message in the batch, clears latency_batch_, and resets the write buffer.
    void finishBatch(ConnectionInfo& info) const;

    //===== other helpers =====
    // Condemns a connection from the outbound thread: marks it, removes it from
    // writable_fds_ / fd_to_info_, and posts its fd to close_ring_ for the
    // inbound thread to call closeConnection.
    void condemnConnection(int fd);

    // Called only from the inbound thread.
    void closeConnection(int client_fd);
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP