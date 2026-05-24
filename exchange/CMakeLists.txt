cmake_minimum_required(VERSION 3.25)
project(Exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_library(common_lib INTERFACE)
add_library(config     INTERFACE)

target_include_directories(common_lib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(config INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/config)

set(EXCHANGE_SOURCES
        app/main.cpp
        app/exchange.cpp

        engine/src/engine.cpp

        server/src/buffer.cpp
        server/src/connection_info.cpp
        server/src/server.cpp
)

# Release build
# Optimised; debug symbols and frame pointers kept for perf / profiling
add_executable(server-release ${EXCHANGE_SOURCES})
target_link_libraries(server-release PUBLIC common_lib config)
target_compile_options(server-release PRIVATE
        -O2
        -g
        -fno-omit-frame-pointer
)

# Debug build
# No optimisation; full debug info, assertions enabled
add_executable(server-debug ${EXCHANGE_SOURCES})
target_link_libraries(server-debug PUBLIC common_lib config)
target_compile_options(server-debug PRIVATE
        -O0
        -g3
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
)
target_link_options(server-debug PRIVATE
        -fsanitize=address,undefined
)
