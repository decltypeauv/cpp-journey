cmake_minimum_required(VERSION 3.16)
project(cpp-journey-gdb-debugging LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -g -O0)
endif()

add_executable(gdb_deep gdb_deep_scaffold.cpp)

find_package(Threads REQUIRED)
target_link_libraries(gdb_deep PRIVATE Threads::Threads)
