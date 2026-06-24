# ============================================================================
# TinyWeb — 高性能 HTTP 服务器框架
# C++20, epoll, header-only 组件
# ============================================================================
cmake_minimum_required(VERSION 3.16)          # 最低 CMake 版本
project(tinyweb LANGUAGES CXX)                # 项目名 + 语言

# ── C++20 标准 ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)                    # 使用 C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)           # 强制要求 (不降级)
set(CMAKE_CXX_EXTENSIONS OFF)                 # 禁用编译器扩展 (严格标准)

# ── 编译选项 ──────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") # GCC 或 Clang
  add_compile_options(
    -Wall -Wextra -Wpedantic                  # 严格警告
    -g -O2                                    # 调试符号 + 优化
  )
endif()

# ── 头文件路径 ────────────────────────────────────────────────────────
include_directories(include)                  # 让 #include "tinyweb/xxx.hpp" 生效

# ── 可执行文件 ────────────────────────────────────────────────────────
add_executable(tinyweb src/main.cpp)          # 主程序

# ── 链接 ──────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)                # 查找 pthread
target_link_libraries(tinyweb PRIVATE Threads::Threads) # 链接 pthread

# ── 可选: TLS 支持 (需 OpenSSL) ──────────────────────────────────────
# find_package(OpenSSL)
# if(OpenSSL_FOUND)
#   target_compile_definitions(tinyweb PRIVATE TINYWEB_TLS)
#   target_link_libraries(tinyweb PRIVATE OpenSSL::SSL OpenSSL::Crypto)
# endif()
