cmake_minimum_required(VERSION 3.25)
project(ClientExchangeSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# Test harness: GoogleTest via FetchContent. include(CTest) defines BUILD_TESTING
# (default ON) and calls enable_testing(); configure with -DBUILD_TESTING=OFF to skip.
include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Exchange server (matching engine + TCP gateway)
add_subdirectory(exchange)

# Load-generator client
add_subdirectory(client)
