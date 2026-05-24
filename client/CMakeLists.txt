cmake_minimum_required(VERSION 3.25)
project(Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(client
    app/main.cpp
    src/orchestrator.cpp
)

# Pull in the exchange types and protocol constants without rebuilding the
# exchange.  We only need the headers.
target_include_directories(client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/include          # communication_types.h, order_types.h, ring_buffer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/server/include   # protocol.h, buffer.h, socket.h …
)
