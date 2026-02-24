//
// Created by cniew on 2/3/26.
//

#include "shared_memory.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/mman.h>

void* create_shm(const char *name, const int& size) {
    const int shm_fd = shm_open(name, O_CREAT | O_RDWR, 066);
    ftruncate(shm_fd, size);
    return mmap(nullptr, size, PROT_WRITE, MAP_SHARED, shm_fd, 0);
}

void destroy_shm(const char *name) { shm_unlink(name); }