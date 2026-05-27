//
// Created by cniew on 5/27/26.
//

#ifndef LIB_H
#define LIB_H

#include <pthread.h>
#include <sched.h>
#include <bits/cpu-set.h>

inline void pin_to_core(const int core) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core, &cpu_set);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
}

#endif //LIB_H
