cmake_minimum_required(VERSION 3.25)
project(Exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_library(common_lib INTERFACE
        include/order_types.h
        engine/config/macros.h
        server/config/config.h)
target_include_directories(common_lib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(RunExchange app/main.cpp
        app/exchange.cpp

        engine/src/engine.cpp

        server/src/buffer.cpp
        server/src/connection_info.cpp
        server/src/server.cpp
)

add_executable(SingleClient tests/tests.cpp)

target_link_libraries(RunExchange PUBLIC common_lib)
