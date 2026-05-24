cmake_minimum_required(VERSION 3.25)
project(ClientExchangeSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# Exchange server (matching engine + TCP gateway)
add_subdirectory(exchange)

# Load-generator client
add_subdirectory(client)
