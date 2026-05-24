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
    #if DIAGNOSTICS
        std::cerr << "exchange tid: " << gettid() << std::endl;
    #endif

    std::jthread server_thread([&]() {
        #if DIAGNOSTICS
            std::cerr << "server tid: " << gettid() << std::endl;
        #endif
       server_.run();
    });

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(SERVER_CORE, &cpuset);
    pthread_setaffinity_np(server_thread.native_handle(), sizeof(cpu_set_t), &cpuset);

    engine_.run();
}
