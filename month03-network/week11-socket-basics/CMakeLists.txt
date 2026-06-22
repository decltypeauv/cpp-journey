cmake_minimum_required(VERSION 3.16)
project(cpp-journey-socket-basics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(socket_basics socket_basics_scaffold.cpp)

# Socket API 需要链接 pthread (某些平台)
find_package(Threads REQUIRED)
target_link_libraries(socket_basics PRIVATE Threads::Threads)
