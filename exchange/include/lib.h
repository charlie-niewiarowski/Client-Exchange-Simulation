//
// Created by cniew on 5/27/26.
//

#ifndef LIB_H
#define LIB_H

#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <bits/cpu-set.h>

inline void pin_to_core(const int core) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core, &cpu_set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) == -1) {
        std::cerr << "pthread_setaffinity_np failed: " << strerror(errno) << std::endl;
    }

    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == -1) {
        std::cerr << "pthread_setschedparam failed: " << strerror(errno) << std::endl;
    }
}

#endif //LIB_H
