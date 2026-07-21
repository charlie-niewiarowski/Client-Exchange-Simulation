cmake_minimum_required(VERSION 3.25)
project(Exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

include(FetchContent)

FetchContent_Declare(
        hdr_histogram
        GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
        GIT_TAG 0.11.9
)
FetchContent_MakeAvailable(hdr_histogram)

add_library(common_lib INTERFACE
        include/lib.hpp)
target_include_directories(common_lib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

add_library(config INTERFACE)
target_include_directories(config INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/config)

set(EXCHANGE_SOURCES
        app/main.cpp
        app/exchange.cpp
        engine/src/engine.cpp
        engine/src/orderbook.cpp
        server/src/connection_info.cpp
        server/src/server.cpp
)

# Release
add_executable(exchange-release ${EXCHANGE_SOURCES})
target_link_libraries(exchange-release PRIVATE common_lib config hdr_histogram_static)
target_compile_options(exchange-release PRIVATE
        -O3 -g -fno-omit-frame-pointer -march=native)

# Debug
add_executable(exchange-debug ${EXCHANGE_SOURCES})
target_link_libraries(exchange-debug PRIVATE common_lib config hdr_histogram_static)
target_compile_options(exchange-debug PRIVATE
        -O3 -g3 -fno-omit-frame-pointer
        -fsanitize=address,undefined -march=native)
target_link_options(exchange-debug PRIVATE
        -fsanitize=address,undefined)