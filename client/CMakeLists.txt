cmake_minimum_required(VERSION 3.25)
project(Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(client-release
    app/main.cpp
    src/load_generator.cpp
)
add_executable(client-debug
        app/main.cpp
        src/load_generator.cpp
)

target_compile_options(client-release PRIVATE -O3 -march=native)
target_compile_options(client-debug PRIVATE -g -march=native)

# Pull in the exchange type headers without rebuilding the exchange binary.
target_include_directories(client-release PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/include          # communication_types.h, order_types.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/server/include   # protocol.h (INBOUND_BSIZE, OUTBOUND_BSIZE)
    ${CMAKE_CURRENT_SOURCE_DIR}/include                      # order_factory.h, client_state.h
    ${CMAKE_CURRENT_SOURCE_DIR}/config                       # config.h
)

target_include_directories(client-debug PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/include          # communication_types.h, order_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/server/include   # protocol.h (INBOUND_BSIZE, OUTBOUND_BSIZE)
        ${CMAKE_CURRENT_SOURCE_DIR}/include                      # order_factory.h, client_state.h
        ${CMAKE_CURRENT_SOURCE_DIR}/config                       # config.h
)
