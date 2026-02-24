cmake_minimum_required(VERSION 3.25)
project(SingleInstrumentExchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# GoogleTest
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

add_executable(test
        tests/engine_test.cpp
        src/Engine.cpp
        src/OrderGateway.cpp
        src/OrderBook.cpp
        utils/shared_memory.cpp
)

find_package(Boost REQUIRED)
target_include_directories(test PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(test
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(test)