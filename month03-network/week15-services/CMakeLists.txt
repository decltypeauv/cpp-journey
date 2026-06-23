cmake_minimum_required(VERSION 3.16)
project(cpp-journey-services LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(services services_scaffold.cpp)

find_package(Threads REQUIRED)
target_link_libraries(services PRIVATE Threads::Threads)
