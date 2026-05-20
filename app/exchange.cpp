//
// Created by charl on 1/17/2026.
//

#include <thread>
#include <pthread.h>
#include <sched.h>

#include "exchange.h"

Exchange::Exchange() : engine_(in_ring_, out_ring_), server_{in_ring_, out_ring_} {}

void Exchange::run() {
    std::jthread engine_thread([&]() {
       engine_.run();
    });

    std::jthread server_thread([&]() {
       server_.run();
    });

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(SERVER_CORE, &cpuset);
    pthread_setaffinity_np(server_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
}
