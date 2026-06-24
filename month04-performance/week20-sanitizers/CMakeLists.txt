cmake_minimum_required(VERSION 3.16)
project(cpp-journey-sanitizers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -g -O1)
endif()

# Build variants (use cmake -DSANITIZE=asan|ubsan|tsan|msan|lsan):
#   cmake -B build-asan -DSANITIZE=asan
#   cmake -B build-ubsan -DSANITIZE=ubsan
#   cmake -B build-tsan -DSANITIZE=tsan
#   cmake --build build-<variant> && ./sanitizer_deep <ex_num>
#
# Or compile directly:
#   g++ -fsanitize=address -g -O1 sanitizer_deep_scaffold.cpp -o sani_deep

if(DEFINED SANITIZE)
  if(SANITIZE STREQUAL "asan")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    link_libraries(-fsanitize=address)
    message(STATUS "Building with AddressSanitizer")
  elseif(SANITIZE STREQUAL "ubsan")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    link_libraries(-fsanitize=undefined)
    message(STATUS "Building with UndefinedBehaviorSanitizer")
  elseif(SANITIZE STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    link_libraries(-fsanitize=thread)
    message(STATUS "Building with ThreadSanitizer")
  elseif(SANITIZE STREQUAL "msan")
    add_compile_options(-fsanitize=memory -fno-omit-frame-pointer -fsanitize-memory-track-origins)
    link_libraries(-fsanitize=memory)
    message(STATUS "Building with MemorySanitizer (Clang only)")
  elseif(SANITIZE STREQUAL "lsan")
    add_compile_options(-fsanitize=leak -fno-omit-frame-pointer)
    link_libraries(-fsanitize=leak)
    message(STATUS "Building with LeakSanitizer")
  endif()
endif()

add_executable(sanitizer_deep sanitizer_deep_scaffold.cpp)

find_package(Threads REQUIRED)
target_link_libraries(sanitizer_deep PRIVATE Threads::Threads)
