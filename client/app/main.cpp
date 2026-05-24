//
// Created by cniew on 5/24/26.
//
// Exchange load-generator entry point.
//
// Usage:
//   client [num_clients] [rng_seed]
//
//   num_clients  – concurrent TCP connections to open (default: 10)
//   rng_seed     – optional RNG seed for reproducibility (default: random)
//
// Runs until SIGINT / SIGTERM, then prints aggregate stats.
//

#include "../include/orchestrator.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>

static ClientOrchestrator* g_orch = nullptr;

static void on_signal(int) {
    if (g_orch) g_orch->stop();
}

int main(const int argc, char* argv[]) {
    int      num_clients = 10;
    uint32_t rng_seed    = std::random_device{}();

    if (argc >= 2) {
        num_clients = std::atoi(argv[1]);
        if (num_clients <= 0) {
            std::fprintf(stderr, "Usage: %s [num_clients] [rng_seed]\n", argv[0]);
            return 1;
        }
    }
    if (argc >= 3) {
        rng_seed = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    }

    std::fprintf(stderr,
        "=== Exchange load generator ===\n"
        "  clients : %d\n"
        "  seed    : %u\n"
        "  target  : %s:%d\n"
        "  logging : %s\n"
        "Ctrl-C to stop.\n\n",
        num_clients, rng_seed,
        EXCHANGE_HOST, EXCHANGE_PORT,
        LOG_ENABLED ? "ON" : "OFF");

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    ClientOrchestrator orch{num_clients, rng_seed};
    g_orch = &orch;

    orch.run();

    const auto s = orch.stats();
    std::fprintf(stderr,
        "\n=== Stats ===\n"
        "  requests sent   : %llu\n"
        "  ACK responses   : %llu\n"
        "  MATCH responses : %llu\n"
        "  ERR responses   : %llu\n"
        "  orders in flight: %lld\n",
        static_cast<unsigned long long>(s.requests_sent),
        static_cast<unsigned long long>(s.responses_ack),
        static_cast<unsigned long long>(s.responses_match),
        static_cast<unsigned long long>(s.responses_err),
        -1 * static_cast<long long>(s.orders_in_flight));

    return 0;
}
