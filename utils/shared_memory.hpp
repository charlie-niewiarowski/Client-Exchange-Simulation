//
// Created by cniew on 2/3/26.
//

#ifndef SHARED_MEMORY_HPP
#define SHARED_MEMORY_HPP

void* create_shm(const char *name, const int& size);
void destroy_shm(const char *name);

#endif //SHARED_MEMORY_HPP
