// ============================================================================
// tinyweb/common.hpp — 共享类型, 工具函数, 平台头文件
// ============================================================================
#pragma once

// ── C 标准库 ─────────────────────────────────────────────────────────
#include <cassert>    // assert() 调试断言
#include <cerrno>     // errno 错误码
#include <csignal>    // sigaction, SIGINT, SIGTERM
#include <cstdint>    // uint8_t, uint32_t, uint64_t 定宽整数
#include <cstring>    // std::memcpy, std::memcmp, std::strlen

// ── C++ 标准库 ───────────────────────────────────────────────────────
#include <atomic>     // std::atomic<T> 无锁引用计数
#include <chrono>     // std::chrono::steady_clock 高精度计时
#include <condition_variable> // std::condition_variable 线程同步
#include <filesystem> // std::filesystem::path 文件路径操作
#include <fstream>    // std::ifstream 文件读取
#include <functional> // std::function<Ret(Args...)> 类型擦除回调
#include <iomanip>    // std::setprecision 格式化输出
#include <iostream>   // std::cout, std::cerr 控制台 I/O
#include <map>        // std::map<K,V> 有序字典 (MemTable)
#include <memory>     // std::unique_ptr, std::shared_ptr, std::make_unique
#include <mutex>      // std::mutex, std::lock_guard 线程安全
#include <optional>   // std::optional<T> 可空值
#include <queue>      // std::queue<T> 任务队列 (ThreadPool)
#include <string>     // std::string 动态字符串
#include <string_view>// std::string_view 零拷贝字符串视图
#include <thread>     // std::thread 线程
#include <unordered_map> // std::unordered_map<K,V> 哈希表 (EventLoop callbacks)
#include <vector>     // std::vector<T> 动态数组

// ── Linux 系统调用 ───────────────────────────────────────────────────
#include <sys/epoll.h>   // epoll_create1, epoll_ctl, epoll_wait
#include <sys/socket.h>  // socket, bind, listen, accept4, send, recv
#include <netinet/in.h>  // sockaddr_in, htons, INADDR_ANY
#include <netinet/tcp.h> // TCP_NODELAY, TCP_CORK
#include <arpa/inet.h>   // inet_ntop, inet_pton
#include <fcntl.h>       // fcntl (设置非阻塞)
#include <unistd.h>      // close, read, write

// ── 命名空间别名 ─────────────────────────────────────────────────────
namespace fs = std::filesystem;      // 文件系统操作
namespace sc = std::chrono;          // 时间操作

// ── 控制台输出工具 ───────────────────────────────────────────────────
// println: 打印任意数量参数, 末尾自动换行
template <typename... Args>
inline void println(Args&&... args) {
  if constexpr (sizeof...(args) > 0) {    // 有参数时才展开 (避免空参数包语法错误)
    ((std::cout << std::forward<Args>(args)), ...); // C++17 折叠表达式: 逐个输出
  }
  std::cout << '\n';                     // 末尾换行
}

// print: 打印任意数量参数, 不换行
template <typename... Args>
inline void print(Args&&... args) {
  ((std::cout << std::forward<Args>(args)), ...); // C++17 折叠表达式
}

// HR: 打印水平分隔线 + 标题 (用于控制台输出结构)
inline void HR(std::string_view title = "") {
  println("\n", std::string(72, '='), "\n  ", title, "\n", std::string(72, '='));
}

// ============================================================================
// Slice — 零拷贝字符串视图 (from leveldb include/leveldb/slice.h)
// ============================================================================
// Slice = { const char* data, size_t size }
// 不拥有内存, 可指向 std::string / char[] / mmap 等任意来源
// 比 C++17 std::string_view 早了好几年, 是 leveldb 的核心抽象之一
struct Slice {
  const char* _d = nullptr; // 指向数据的指针
  size_t _n = 0;            // 数据字节数

  // ── 构造函数 ────────────────────────────────────────────────────
  Slice() = default;                           // 空 Slice
  Slice(const char* d, size_t n) : _d(d), _n(n) {} // 从指针+长度构造
  Slice(const std::string& s) : _d(s.data()), _n(s.size()) {} // 从 std::string 构造 (不拷贝!)
  Slice(const char* s) : _d(s), _n(std::strlen(s)) {} // 从 C 字符串构造

  // ── 访问器 ──────────────────────────────────────────────────────
  const char* data() const { return _d; }      // 返回数据指针
  size_t size() const { return _n; }           // 返回数据长度
  bool empty() const { return _n == 0; }       // 是否为空
  char operator[](size_t i) const { return _d[i]; } // 下标访问
  std::string str() const { return {_d, _n}; } // 拷贝到 std::string
  bool operator==(const Slice& o) const {      // 比较 (逐字节 memcmp)
    return _n == o._n && std::memcmp(_d, o._d, _n) == 0;
  }
};

// ============================================================================
// Varint 编码 — 变长整数 (from leveldb util/coding.h)
// ============================================================================
// 原理: 每字节低 7 位存数据, 最高位 (0x80) = "还有后续字节"
// 小数字省空间: 0-127 → 1B, 128-16383 → 2B, ...

// EncodeVarint32: 将 uint32_t 编码为 varint, 返回写入后的指针位置
inline char* EncodeVarint32(char* dst, uint32_t v) {
  auto* p = reinterpret_cast<uint8_t*>(dst);   // 按字节操作
  static const int B = 128;                     // continuation bit = 0x80
  if (v < (1 << 7)) {                           // 0-127: 1 字节
    *(p++) = v;
  } else if (v < (1 << 14)) {                   // 128-16383: 2 字节
    *(p++) = v | B;                             // 第一字节: 低 7 位 + B
    *(p++) = v >> 7;                            // 第二字节: 高 7 位
  } else if (v < (1 << 21)) {                   // 16384-: 3 字节
    *(p++) = v | B; *(p++) = (v >> 7) | B; *(p++) = v >> 14;
  } else if (v < (1 << 28)) {                   // 4 字节
    *(p++) = v | B; *(p++) = (v >> 7) | B; *(p++) = (v >> 14) | B;
    *(p++) = v >> 21;
  } else {                                      // 5 字节 (最大)
    *(p++) = v | B; *(p++) = (v >> 7) | B; *(p++) = (v >> 14) | B;
    *(p++) = (v >> 21) | B; *(p++) = v >> 28;
  }
  return reinterpret_cast<char*>(p);            // 返回结束位置
}

// GetVarint32: 从 buffer 解码一个 varint32, 返回读取后的指针位置 (nullptr=失败)
inline const char* GetVarint32(const char* p, const char* limit, uint32_t* v) {
  if (p < limit) {                              // 至少 1 字节
    uint32_t r = *(const uint8_t*)p;            // 读第一字节
    if ((r & 128) == 0) { *v = r; return p + 1; } // 单字节 fast path
  }
  uint32_t r = 0;                               // 累积结果
  for (uint32_t s = 0; s <= 28 && p < limit; s += 7) { // 每次 7 位
    uint32_t b = *(const uint8_t*)p; p++;      // 读一字节
    if (b & 128) r |= ((b & 127) << s);         // continuation: 低 7 位 + 继续
    else { r |= (b << s); *v = r; return p; }   // 最后一字节: 停止
  }
  return nullptr;                                // 数据不足
}
