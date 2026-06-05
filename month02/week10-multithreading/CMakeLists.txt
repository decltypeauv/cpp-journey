cmake_minimum_required(VERSION 3.16)
project(cpp-journey-multithreading LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 需要链接 pthread 库 (Linux)
find_package(Threads REQUIRED)

add_executable(multithreading multithreading_scaffold.cpp)
target_link_libraries(multithreading PRIVATE Threads::Threads)
