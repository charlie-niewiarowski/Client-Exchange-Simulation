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

add_executable(exchange app/main.cpp
        app/exchange.cpp

        engine/src/engine.cpp

        server/src/buffer.cpp
        server/src/connection_info.cpp
        server/src/server.cpp
)

target_link_libraries(exchange PUBLIC common_lib)
target_link_libraries(exchange PUBLIC config)