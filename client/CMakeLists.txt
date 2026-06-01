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

target_include_directories(client-release PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.h, client_state.h
    ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared # protocol headers
)

target_include_directories(client-debug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.h, client_state.h
    ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared # protocol headers
)
