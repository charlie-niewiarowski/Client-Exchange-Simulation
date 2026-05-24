cmake_minimum_required(VERSION 3.25)
project(ExchangeEngine)

set(CMAKE_CXX_STANDARD 23)

add_library(common_headers INTERFACE)
target_include_directories(common_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

enable_testing()

add_executable(TestEngine
    tests/engine_test.cpp
    src/engine.cpp
)
target_include_directories(TestEngine PRIVATE include config tests)
target_link_libraries(TestEngine PRIVATE common_headers pthread)
add_test(NAME TestEngine COMMAND TestEngine)