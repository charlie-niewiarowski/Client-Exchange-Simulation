cmake_minimum_required(VERSION 3.25.1)
project(ExchangeServer)

set(CMAKE_CXX_STANDARD 23)

add_executable(ExchangeServer
      tests/server_entry.cpp
      src/buffer.cpp
      src/connection_info.cpp
      src/server.cpp
)

add_executable(ExchangeClient
      tests/client_entry.cpp
      tests/client.cpp
)

target_include_directories(ExchangeServer PRIVATE include)
target_include_directories(ExchangeClient PRIVATE include tests)

target_compile_options(ExchangeServer PRIVATE -fsanitize=address,undefined)
target_link_options(ExchangeServer PRIVATE -fsanitize=address,undefined)