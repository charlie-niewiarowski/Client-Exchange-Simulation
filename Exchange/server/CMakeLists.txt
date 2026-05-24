cmake_minimum_required(VERSION 3.25.1)
project(ExchangeServer)

set(CMAKE_CXX_STANDARD 23)

# Interface library that carries the top-level include path.
# Any target that links against common_headers inherits it automatically,
# so relative paths never appear more than once.
add_library(common_headers INTERFACE)
target_include_directories(common_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # ring_buffer.hpp, communication_types.h, order_types.h
)

# Common server include directory (server-local headers).
set(SERVER_INCLUDE include)

# Common server sources shared across server + test executables.
set(SERVER_SOURCES
    src/buffer.cpp
    src/connection_info.cpp
    src/server.cpp
)

# Test executables
#
# Run sequentially (all three use port 4000) with:
#   cmake --build . && ctest --output-on-failure

enable_testing()

# 1. Validation unit tests: p1ure logic, no networking.
add_executable(TestValidation
    tests/test_validation.cpp
)
target_include_directories(TestValidation PRIVATE ${SERVER_INCLUDE})
target_link_libraries(TestValidation PRIVATE common_headers)
add_test(NAME TestValidation COMMAND TestValidation)

# 2. Pipeline isolation tests: one fixture per test, server on port 4000.
add_executable(TestServerPipeline
    tests/test_server_pipeline.cpp
    ${SERVER_SOURCES}
)
target_include_directories(TestServerPipeline PRIVATE ${SERVER_INCLUDE} tests)
target_link_libraries(TestServerPipeline PRIVATE common_headers)
add_test(NAME TestServerPipeline COMMAND TestServerPipeline)

# 3. Comprehensive integration test: multiple clients, port 4000.
add_executable(TestIntegration
    tests/test_integration.cpp
    ${SERVER_SOURCES}
)
target_include_directories(TestIntegration PRIVATE ${SERVER_INCLUDE} tests)
target_link_libraries(TestIntegration PRIVATE common_headers)
add_test(NAME TestIntegration COMMAND TestIntegration)

# Run the two network tests sequentially so they don't race on port 4000.
set_tests_properties(TestServerPipeline TestIntegration PROPERTIES RESOURCE_LOCK tcp4000)