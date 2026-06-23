cmake_minimum_required(VERSION 3.16)
project(cpp-journey-epoll LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(epoll_deep epoll_deep_scaffold.cpp)

# epoll, socket, timerfd 需要链接 pthread (某些平台)
find_package(Threads REQUIRED)
target_link_libraries(epoll_deep PRIVATE Threads::Threads)
