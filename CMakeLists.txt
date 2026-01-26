cmake_minimum_required(VERSION 3.31)
project(untitled)

set(CMAKE_CXX_STANDARD 23)

include(FetchContent)
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG origin/main
)

FetchContent_MakeAvailable(googletest)

add_executable(test
        tests/engine_test.cpp
        src/Engine.cpp
        src/OrderGateway.cpp
        include/Trade.hpp
        lib/RingBuffer.cpp
        lib/RingBuffer.hpp
        include/engine_types.hpp
        src/OrderBook.cpp
        include/OrderBook.hpp
)

target_include_directories(test PRIVATE include)

target_link_libraries(test
        gtest
        gtest_main
)

include(GoogleTest)
gtest_discover_tests(test)
