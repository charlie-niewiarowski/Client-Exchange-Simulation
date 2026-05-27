cmake_minimum_required(VERSION 3.25)
project(Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(client
    app/main.cpp
    src/load_generator.cpp
)

target_compile_options(client PRIVATE -O3 -march=native)

# Pull in the exchange type headers without rebuilding the exchange binary.
target_include_directories(client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/include          # communication_types.h, order_types.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/server/include   # protocol.h (INBOUND_BSIZE, OUTBOUND_BSIZE)
    ${CMAKE_CURRENT_SOURCE_DIR}/include                      # order_factory.h, client_state.h
    ${CMAKE_CURRENT_SOURCE_DIR}/config                       # config.h
)
