cmake_minimum_required(VERSION 3.16)
project(cpp-journey-month5-capstone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -g -O1)
endif()

add_executable(month5_capstone month5_capstone.cpp)
