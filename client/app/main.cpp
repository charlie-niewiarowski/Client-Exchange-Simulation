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
// Runs until SIGINT / SIGTERM, then prints aggregate stats and throughput.
//

#include "../include/load_generator.h"
#include "../config/config.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static LoadGenerator* g_orch = nullptr;

static void on_signal(int) {
    if (g_orch) g_orch->stop();
}

// Returns monotonic time in nanoseconds.
static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

int main(const int argc, char* argv[]) {
    int num_clients = 10;
    uint32_t rng_seed = std::random_device{}();

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

    LoadGenerator orch{num_clients, rng_seed};
    g_orch = &orch;

    // Pin the event-loop thread to CLIENT_CORE.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(CLIENT_CORE, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        std::fprintf(stderr, "[WARN] pthread_setaffinity_np: %s\n", std::strerror(errno));

    #if DIAGNOSTICS
        std::fprintf(stderr, "client tid: %d\n", static_cast<int>(gettid()));
    #endif

    const uint64_t t0 = now_ns();
    orch.run();
    const uint64_t t1 = now_ns();

    const double elapsed_s = static_cast<double>(t1 - t0) / 1e9;

    const auto s = orch.stats();
    // MATCHs not included because i care about the number of orders responded to not the number of responses in total
    const uint64_t total_responses = s.responses_ack + s.responses_err;

    // Guard against divide-by-zero if the user stops immediately.
    const double inv = elapsed_s > 0.0 ? 1.0 / elapsed_s : 0.0;

    std::fprintf(stderr,
        "\n=== Stats ===\n"
        "  elapsed         : %.3f s\n"
        "  requests sent   : %llu\n"
        "  ACK responses   : %llu\n"
        "  MATCH responses : %llu\n"
        "  ERR responses   : %llu\n"
        "  orders in flight: %lld\n"
        "  --- throughput ---\n"
        "  requests/s      : %.0f\n"
        "  ACK/s           : %.0f\n"
        "  MATCH/s         : %.0f\n"
        "  ERR/s           : %.0f\n"
        "  total resp/s    : %.0f\n",
        elapsed_s,
        static_cast<unsigned long long>(s.requests_sent),
        static_cast<unsigned long long>(s.responses_ack),
        static_cast<unsigned long long>(s.responses_match),
        static_cast<unsigned long long>(s.responses_err),
        -1LL * static_cast<long long>(s.orders_in_flight),
        static_cast<double>(s.requests_sent)   * inv,
        static_cast<double>(s.responses_ack)   * inv,
        static_cast<double>(s.responses_match) * inv,
        static_cast<double>(s.responses_err)   * inv,
        static_cast<double>(total_responses)   * inv);

    return 0;
}
