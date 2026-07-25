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
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.hpp, client_state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infra     # buffer, communication_types, order_types
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol  # protocol.hpp, ouch.hpp
)

target_include_directories(client-debug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.hpp, client_state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../infra     # buffer, communication_types, order_types
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol  # protocol.hpp, ouch.hpp
)

#=============================================================================
# Tests (GoogleTest)
#=============================================================================
if(BUILD_TESTING)
    add_executable(client_tests
            tests/order_factory_test.cpp)
    target_include_directories(client_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/config
            ${CMAKE_CURRENT_SOURCE_DIR}/../infra
            ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    target_link_libraries(client_tests PRIVATE GTest::gtest_main)
    add_test(NAME client_tests COMMAND client_tests)
endif()
