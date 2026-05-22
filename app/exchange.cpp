//
// Created by charl on 1/17/2026.
//

#include "exchange.h"

#include <thread>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <iostream>

Exchange::Exchange() : engine_(in_ring_, out_ring_), server_{in_ring_, out_ring_} {}

void Exchange::run() {
    std::cerr << "exchange/tid: " << gettid() << std::endl;

    std::jthread server_thread([&]() {
        pid_t tid = gettid();
        std::cerr << "server tid: " << tid << std::endl;
       server_.run();
    });

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(SERVER_CORE, &cpuset);
    pthread_setaffinity_np(server_thread.native_handle(), sizeof(cpu_set_t), &cpuset);

    engine_.run();
}
