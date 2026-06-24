// ============================================================================
// Month 5 Week 28: 收官 — 源码阅读总结 + Capstone
// 日期: 2026-06-24
//
// "看透实现" — Month 5 的终极目标
//
// Month 5 阅读了 4 个经典 C/C++ 项目 + 手写了 Mini STL:
//   W23: STL 源码深潜 (vector/string/function/arena/sort/map)
//   W24: leveldb (LSM Tree 存储引擎)
//   W25: fmtlib   (现代格式化库)
//   W26: libevent (Reactor 事件驱动)
//   W27: Mini STL (手写 10 个核心组件)
//
// Week 28 结构:
//   Part A (Ex1-5): 全景回顾 — 知识检查 + 跨项目模式对比
//   Part B (Ex6-10): Capstone — MiniDB (集成所有源码模式)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace std::literals;

// ============================================================================
// 辅助
// ============================================================================
template <typename... Args> void println(Args&&... args) {
  if constexpr (sizeof...(args) > 0) ((std::cout << std::forward<Args>(args)), ...);
  std::cout << '\n';
}
template <typename... Args> void print(Args&&... args) {
  ((std::cout << std::forward<Args>(args)), ...);
}
void HR(std::string_view t) { println("\n", std::string(72, '='), "\n  ", t, "\n", std::string(72, '=')); }

template <typename F>
auto benchmark(F&& f, int n = 100000) {
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; i++) { volatile auto _ = f(i); (void)_; }
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
}

// ============================================================================
// Part A: 全景回顾 — 跨项目模式对比
// ============================================================================

// ============================================================================
// Exercise 1: 知识检查 — Month 5 全景填空
// ============================================================================
// 覆盖 W23-W27 所有关键概念

namespace ex1 {

void run() {
  HR("Ex1: Month 5 全景知识检查");

  println("📝 10 个核心概念自查 (尝试在心里回答, 再对比答案):\n");

  struct Q {
    const char* q;
    const char* a;
  };
  std::vector<Q> quiz = {
    {"leveldb: LSM Tree 的写入路径是什么?",
     "Put → WAL(日志) → MemTable(SkipList) → 满4MB → flush → SSTable(L0)"},
    {"leveldb: SkipList 为什么能并发读无锁?",
     "acquire/release 内存序: Insert 用 release store 发布节点, 读用 acquire load 看到完整节点"},
    {"fmtlib: format_decimal 的 2-digit 查表法原理?",
     "value%100→查 digits[0..99] 表→一次写 2 个字符, 减少除法次数"},
    {"fmtlib: type erasure 如何实现 (basic_format_arg)?",
     "tagged union: enum type + union {int,uint,double,string_view,pointer}"},
    {"libevent: Reactor 模式的核心循环是什么?",
     "timeout=min_heap_top→evsel->dispatch(epoll_wait)→active 就绪事件→执行回调→loop"},
    {"libevent: evbuffer 为什么用链式?",
     "避免 realloc 拷贝; 支持 readv/writev scatter-gather I/O; 引用计数共享"},
    {"STL: vector 的 push_back 为什么用 placement new?",
     "分离内存分配(operator new)和对象构造(placement new)→capacity > size"},
    {"STL: unique_ptr 如何做到 sizeof==sizeof(T*)?",
     "[[no_unique_address]] + Empty Base Optimization→无状态 Deleter 零开销"},
    {"STL: std::function 的 type erasure 如何实现?",
     "Concept(虚基类) + Model<T>(模板子类) → 编译期多态+运行期多态的桥接"},
    {"综合: 这 4 个项目共同的 C++ 设计哲学?",
     "零开销抽象 + RAII + 迭代器模式 + 模板元编程 + 类型安全"}
  };

  for (size_t i = 0; i < quiz.size(); i++) {
    println("  Q", i + 1, ": ", quiz[i].q);
    println("     A: ", quiz[i].a);
    println();
  }
}

} // namespace ex1

// ============================================================================
// Exercise 2: 跨项目设计模式对比
// ============================================================================
// 识别 leveldb / fmtlib / libevent / STL 中的共同模式

namespace ex2 {

void run() {
  HR("Ex2: 跨项目设计模式对比");

  println("🔍 4 个项目中的共同设计模式:\n");

  println("  模式 1: Type Erasure (类型擦除)");
  println("    leveldb : Slice = {const char*, size_t} → 零拷贝字符串视图");
  println("    fmtlib  : basic_format_arg = tagged union → 异构参数统一存储");
  println("    libevent: event_callback_fn = void(*)(evutil_socket_t, short, void*) → 通用回调");
  println("    STL     : std::function = Concept/Model → 任意可调用对象");
  println("    📌 共同点: 隐藏具体类型, 提供统一接口");
  println();

  println("  模式 2: Arena / Pool Allocation (内存池)");
  println("    leveldb : Arena (4KB blocks, bump pointer) — SkipList 节点");
  println("    fmtlib  : format_arg_store (栈上数组) — 参数存储");
  println("    libevent: evbuffer_chain pool — 缓冲区节点复用");
  println("    STL     : std::pmr (polymorphic memory resource)");
  println("    📌 共同点: 批量分配 + 统一释放 → 减少 malloc/free, 改善局部性");
  println();

  println("  模式 3: Iterator Pattern (迭代器)");
  println("    leveldb : Iterator → SkipList::Iterator → Block::Iter → TwoLevelIterator");
  println("    fmtlib  : format_to(OutputIt, ...) → 泛型输出迭代器");
  println("    libevent: TAILQ_FOREACH → 侵入式链表遍历");
  println("    STL     : 5 种 iterator categories + tag dispatch");
  println("    📌 共同点: 算法与数据结构解耦, 统一遍历接口");
  println();

  println("  模式 4: RAII / Scope Guard (资源管理)");
  println("    leveldb : ScopedFd (auto-close), MutexLock (auto-unlock)");
  println("    fmtlib  : 隐式: format 返回 string → 自动管理内存");
  println("    libevent: bufferevent_free → 自动释放关联的 events+buffers");
  println("    STL     : unique_ptr, shared_ptr, lock_guard, jthread");
  println("    📌 共同点: 资源获取即初始化, 析构自动释放 → 无泄漏");
  println();

  println("  模式 5: Callback / Observer (回调)");
  println("    leveldb : Compaction 完成回调, WriteBatch 回调");
  println("    fmtlib  : formatter<T>::format = 用户提供的格式化回调");
  println("    libevent: event_callback_fn → 事件就绪时执行");
  println("    STL     : std::function, 自定义 comparator, 自定义 allocator");
  println("    📌 共同点: 控制反转 (IoC), 框架调用用户代码");
  println();
}

} // namespace ex2

// ============================================================================
// Exercise 3: 技术选型决策树
// ============================================================================
// "如果要实现 X, 应该选哪个项目的设计?"

namespace ex3 {

void run() {
  HR("Ex3: 技术选型决策树");

  println("🤔 10 个场景: 选哪个项目的设计?\n");

  struct Scenario {
    const char* need;
    const char* source;
    const char* reason;
  };
  std::vector<Scenario> decisions = {
    {"需要零拷贝的字符串参数传递",
     "leveldb: Slice ({const char*, size_t})",
     "不拥有内存, 可指向 string/char[]/mmap 等任意来源"},
    {"需要高性能的 int → string 转换",
     "fmtlib: format_decimal (2-digit 查表法)",
     "value%100 一次处理 2 字符, 比 snprintf 快 4x"},
    {"需要单线程处理 10000+ 并发连接",
     "libevent: Reactor (event_base + event)",
     "IO 多路复用 + 回调, 1 线程远超 10000 线程"},
    {"需要 O(log n) 的有序插入和查找",
     "STL: RB Tree (std::map/set) 或 leveldb: SkipList",
     "内存用 SkipList (更简单), 磁盘用 B-Tree (更少 IO)"},
    {"需要编译期检查格式化字符串的正确性",
     "fmtlib: consteval format_string (C++20)",
     "类型不匹配 → 编译错误, 不是运行时异常"},
    {"需要快速判断 key 是否可能存在 (容忍误报)",
     "leveldb: Bloom Filter (10 bits/key → ~1% FPR)",
     "O(k) 查找, 99% 的不存在查询被过滤, 省磁盘 IO"},
    {"需要类型安全的 union (存多种类型之一)",
     "STL: std::variant (tagged union + visit)",
     "编译期穷尽检查, 不会漏掉类型分支"},
    {"需要异步日志 (不阻塞主线程)",
     "libevent: evbuffer + callback + 后台 flush",
     "日志写入内存缓冲区 → 后台线程异步刷盘"},
    {"需要可扩展的格式化 (自定义类型)",
     "fmtlib: formatter<T>::parse/format 两阶段协议",
     "用户只需特化自己的类型, 框架自动处理填充/对齐/精度"},
    {"需要崩溃恢复 (数据不丢失)",
     "leveldb: WAL (Write-Ahead Log) + MANIFEST",
     "先写日志再写数据; 崩溃后重放日志恢复"}
  };

  for (size_t i = 0; i < decisions.size(); i++) {
    println("  Q", i + 1, ": ", decisions[i].need);
    println("     → ", decisions[i].source);
    println("     → ", decisions[i].reason);
    println();
  }
}

} // namespace ex3

// ============================================================================
// Exercise 4: 架构对比 — leveldb vs libevent vs fmtlib
// ============================================================================

namespace ex4 {

void run() {
  HR("Ex4: 架构对比 — 三个项目的设计哲学");

  println("╔═══════════════╤═══════════════╤═══════════════╤═══════════════╗");
  println("║    维度        │   leveldb     │   fmtlib      │   libevent    ║");
  println("╠═══════════════╪═══════════════╪═══════════════╪═══════════════╣");
  println("║ 语言          │ C++ (少量C)   │ C++11/14/17   │ C (纯C)       ║");
  println("║ 核心问题      │ 持久化 KV 存储│ 类型安全格式化 │ 事件驱动 I/O  ║");
  println("║ 核心模式      │ LSM Tree      │ Pipeline      │ Reactor       ║");
  println("║ 并发策略      │ 细粒度锁+MVCC │ 无锁(header)  │ 单线程+回调   ║");
  println("║ 内存管理      │ Arena (pool)  │ 栈上+SSO      │ malloc/free   ║");
  println("║ 错误处理      │ Status (code) │ 异常+constexpr │ 返回值+回调   ║");
  println("║ 可扩展性      │ Comparator    │ formatter<T>  │ eventop vtable║");
  println("║ 测试覆盖      │ 广泛 (>50%?)  │ 广泛 (>90%!)  │ 广泛 (regress)║");
  println("║ 代码风格      │ 注释详尽      │ 模板重度       │ 宏+TAILQ      ║");
  println("║ 学习价值      │ 系统设计      │ 模板元编程     │ 事件驱动      ║");
  println("╚═══════════════╧═══════════════╧═══════════════╧═══════════════╝");
  println();

  println("📖 阅读难度排名 (最容易→最难):");
  println("  1. libevent (C, 简单直接, 数据结构清晰)");
  println("  2. leveldb  (C++, 少量模板, 系统设计为主)");
  println("  3. fmtlib   (C++, 大量模板, SFINAE, consteval)");
  println();

  println("📖 学习收获排名 (最大收获→较小):");
  println("  1. leveldb  — 系统设计 + 算法 + 工程实践");
  println("  2. fmtlib   — 模板元编程 + API 设计 + 性能优化");
  println("  3. libevent — 事件驱动 + C 语言抽象 + 跨平台");
}

} // namespace ex4

// ============================================================================
// Exercise 5: 源码阅读方法论总结
// ============================================================================

namespace ex5 {

void run() {
  HR("Ex5: 源码阅读方法论总结");

  println("📖 Month 5 建立的源码阅读方法:\n");

  println("  第一步: 看公开 API");
  println("    → 读 include/ 头文件");
  println("    → 理解 '这个库解决什么问题'");
  println("    → 写一个 hello-world 级别的使用示例");
  println();

  println("  第二步: 找到主循环/主流程");
  println("    → leveldb: DB::Open → Get/Put → Compaction");
  println("    → fmtlib: format → vformat_to → visit_format_arg");
  println("    → libevent: event_base_new → event_add → event_base_dispatch");
  println("    → STL: 每个组件的构造函数 + 关键方法");
  println();

  println("  第三步: 自底向上读基础组件");
  println("    → leveldb: util/coding → util/arena → db/skiplist → db/memtable");
  println("    → fmtlib: coding-like → format_specs → format_arg → formatter");
  println("    → libevent: minheap → evmap → event → event_base → bufferevent");
  println("    → STL: unique_ptr → optional → variant → vector → map");
  println();

  println("  第四步: 追踪一个典型调用的完整路径");
  println("    → leveldb: Put(\"key\", \"value\") 从 API 到磁盘的全路径");
  println("    → fmtlib: format(\"x={}\", 42) 从格式串到 string 的全路径");
  println("    → libevent: 从 socket 可读到用户回调被调用的全路径");
  println();

  println("  第五步: 手写简化版");
  println("    → 实现核心数据结构和算法 (Week 27)");
  println("    → 删除边缘情况, 只保留核心逻辑");
  println("    → 边写边对比源码, 理解为什么源码那么写");
  println();

  println("  第六步: 对比不同项目的相似模式");
  println("    → Type Erasure: leveldb::Slice ≈ fmtlib::format_arg ≈ STL::function");
  println("    → Iterator: leveldb::Iterator ≈ STL::iterator ≈ libevent TAILQ");
  println("    → Arena: leveldb::Arena ≈ 项目中的各种 pool allocator");
  println();
}

} // namespace ex5

// ============================================================================
// Part B: Capstone — MiniDB (集成所有 Month 5 模式)
// ============================================================================
//
// MiniDB: 一个极简的 Key-Value 存储引擎
//
// 集成的模式:
//   leveldb  → Slice (零拷贝字符串), Varint (编码), Bloom Filter
//              Arena (内存池), WAL (预写日志), SSTable (排序表)
//   fmtlib   → format_to (迭代器输出), parse/format 两阶段协议
//   libevent → event loop (事件驱动 Compaction)
//   Mini STL → MiniOptional (错误处理), MiniSpan (视图)
//
// 架构 (简化版 LSM Tree):
//   ┌─────────────────────────────────────┐
//   │  Put(key, value)                    │
//   │    ↓                                │
//   │  WAL Append (varint 编码)           │
//   │    ↓                                │
//   │  MemTable (std::map, 内存排序)      │
//   │    ↓ (满了 或 手动)                 │
//   │  flush → SSTable (文件)              │
//   │                                     │
//   │  Get(key) → MemTable → SSTable      │
//   │    Bloom Filter 预检 → 快速跳过     │
//   └─────────────────────────────────────┘

// ── Slice (from leveldb) ─────────────────────────────────────────────
struct Slice {
  const char* _d; size_t _n;
  Slice() : _d(nullptr), _n(0) {}
  Slice(const char* d, size_t n) : _d(d), _n(n) {}
  Slice(const std::string& s) : _d(s.data()), _n(s.size()) {}
  Slice(const char* s) : _d(s), _n(std::strlen(s)) {}
  const char* data() const { return _d; }
  size_t size() const { return _n; }
  char operator[](size_t i) const { return _d[i]; }
  std::string str() const { return {_d, _n}; }
  bool operator==(const Slice& o) const { return _n == o._n && std::memcmp(_d, o._d, _n) == 0; }
};

// ── Varint 编码 (from leveldb) ───────────────────────────────────────
inline char* EncodeVarint32(char* dst, uint32_t v) {
  uint8_t* p = reinterpret_cast<uint8_t*>(dst);
  static const int B = 128;
  if (v < (1<<7)) { *(p++) = v; }
  else if (v < (1<<14)) { *(p++) = v|B; *(p++) = v>>7; }
  else if (v < (1<<21)) { *(p++) = v|B; *(p++) = (v>>7)|B; *(p++) = v>>14; }
  else if (v < (1<<28)) { *(p++) = v|B; *(p++) = (v>>7)|B; *(p++) = (v>>14)|B; *(p++) = v>>21; }
  else { *(p++) = v|B; *(p++) = (v>>7)|B; *(p++) = (v>>14)|B; *(p++) = (v>>21)|B; *(p++) = v>>28; }
  return reinterpret_cast<char*>(p);
}
inline const char* GetVarint32(const char* p, const char* limit, uint32_t* v) {
  if (p < limit) { uint32_t r = *(uint8_t*)p; if ((r&128)==0) { *v=r; return p+1; } }
  uint32_t r = 0;
  for (uint32_t s = 0; s <= 28 && p < limit; s += 7) {
    uint32_t b = *(uint8_t*)p; p++;
    if (b & 128) r |= ((b&127)<<s); else { r |= (b<<s); *v=r; return p; }
  }
  return nullptr;
}

// ── Bloom Filter (from leveldb) ──────────────────────────────────────
struct MiniBloom {
  size_t _k;
  explicit MiniBloom(int bits_per_key) {
    _k = (size_t)(bits_per_key * 0.69);
    if (_k < 1) _k = 1; if (_k > 30) _k = 30;
  }
  static uint32_t Hash(const Slice& key) {
    uint32_t h = 0xbc9f1d34;
    for (size_t i = 0; i < key.size(); i++) { h ^= (uint8_t)key.data()[i]; h *= 0x01000193; }
    return h;
  }
  std::string CreateFilter(const std::vector<Slice>& keys, int bits_per_key) {
    size_t bits = keys.size() * bits_per_key;
    if (bits < 64) bits = 64;
    size_t bytes = (bits + 7) / 8; bits = bytes * 8;
    std::string f(bytes, '\0'); f.push_back((char)_k);
    for (auto& key : keys) {
      uint32_t h = Hash(key);
      const uint32_t delta = (h >> 17) | (h << 15);
      for (size_t j = 0; j < _k; j++) { f[h % bits / 8] |= (1 << (h % bits % 8)); h += delta; }
    }
    return f;
  }
  bool MayMatch(const Slice& key, const Slice& filter) const {
    if (filter.size() < 2) return false;
    size_t bits = (filter.size() - 1) * 8;
    size_t k = (uint8_t)filter[filter.size() - 1];
    if (k > 30) return true;
    uint32_t h = Hash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; j++) {
      if ((filter[h % bits / 8] & (1 << (h % bits % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }
};

// ── Arena (from leveldb) ─────────────────────────────────────────────
struct MiniArena {
  static constexpr int kBlock = 4096;
  char* _ptr = nullptr; size_t _rem = 0;
  std::vector<char*> _blocks;
  ~MiniArena() { for (auto* b : _blocks) delete[] b; }
  char* Alloc(size_t n) {
    if (n <= _rem) { char* r = _ptr; _ptr += n; _rem -= n; return r; }
    if (n > kBlock/4) { char* r = new char[n]; _blocks.push_back(r); return r; }
    _ptr = new char[kBlock]; _rem = kBlock; _blocks.push_back(_ptr);
    char* r = _ptr; _ptr += n; _rem -= n; return r;
  }
};

// ── MiniOptional (from Mini STL) ─────────────────────────────────────
struct NulloptT {};
constexpr NulloptT kNullopt{};
template <typename T>
struct MiniOptional {
  alignas(T) unsigned char _buf[sizeof(T)]; bool _has = false;
  MiniOptional() = default;
  MiniOptional(NulloptT) {}
  MiniOptional(const T& v) : _has(true) { new(_buf) T(v); }
  ~MiniOptional() { if (_has) ((T*)_buf)->~T(); }
  bool ok() const { return _has; }
  T& operator*() { return *(T*)_buf; }
  const T& operator*() const { return *(const T*)_buf; }
};

// ── Status (from leveldb) ────────────────────────────────────────────
struct Status {
  enum Code { kOk, kNotFound, kCorruption, kIOError } _c = kOk;
  std::string _msg;
  static Status OK() { return {}; }
  static Status NotFound(const std::string& m) { return {kNotFound, m}; }
  static Status Corruption(const std::string& m) { return {kCorruption, m}; }
  bool ok() const { return _c == kOk; }
  std::string str() const {
    const char* names[] = {"OK","NotFound","Corruption","IOError"};
    return std::string(names[_c]) + ": " + _msg;
  }
};

// ============================================================================
// Exercise 6: MiniDB — Write-Ahead Log (WAL)
// ============================================================================

namespace ex6 {

// ── 简化的 WAL ──────────────────────────────────────────────────────
struct SimpleWAL {
  std::string _buf;

  // 追加一条记录: [varint key_len][key][varint val_len][value]
  void Append(const Slice& key, const Slice& value) {
    char header[10];
    char* p = EncodeVarint32(header, key.size());
    _buf.append(header, p - header);
    _buf.append(key.data(), key.size());
    p = EncodeVarint32(header, value.size());
    _buf.append(header, p - header);
    _buf.append(value.data(), value.size());
  }

  // 遍历所有记录
  void Replay(std::function<void(const Slice&, const Slice&)> cb) {
    const char* p = _buf.data();
    const char* end = p + _buf.size();
    while (p < end) {
      uint32_t klen, vlen;
      p = GetVarint32(p, end, &klen);
      if (!p || p + klen > end) break;
      Slice key(p, klen); p += klen;
      p = GetVarint32(p, end, &vlen);
      if (!p || p + vlen > end) break;
      Slice value(p, vlen); p += vlen;
      cb(key, value);
    }
  }

  size_t size() const { return _buf.size(); }
};

void run() {
  HR("Ex6: MiniDB — WAL (Write-Ahead Log)");

  SimpleWAL wal;
  wal.Append(Slice("name"), Slice("Alice"));
  wal.Append(Slice("age"), Slice("30"));
  wal.Append(Slice("city"), Slice("Shanghai"));

  println("WAL 大小: ", wal.size(), " bytes (3 records)");
  println("Replay:");
  wal.Replay([](const Slice& k, const Slice& v) {
    println("  ", k.str(), " = ", v.str());
  });
  println();

  println("📖 WAL 使用 Varint 编码 + Length-Prefixed Slice");
  println("   格式: [varint key_len][key bytes][varint val_len][val bytes]");
  println("   这是 leveldb 的 WAL 简化版 (去掉了 CRC + block 格式)");
}

} // namespace ex6

// ============================================================================
// Exercise 7: MiniDB — MemTable + Bloom Filter
// ============================================================================

namespace ex7 {

// ── MemTable (内存表) ────────────────────────────────────────────────
struct MemTable {
  std::map<std::string, std::string, std::less<>> _data;

  void Put(const Slice& key, const Slice& value) {
    _data[std::string(key.data(), key.size())] = std::string(value.data(), value.size());
  }

  MiniOptional<std::string> Get(const Slice& key) {
    auto it = _data.find(std::string_view(key.data(), key.size()));
    if (it != _data.end()) return it->second;
    return kNullopt;
  }

  size_t size() const { return _data.size(); }
};

// ── SSTable Metadata ─────────────────────────────────────────────────
struct SSTableMeta {
  std::string _filename;
  std::string _bloom_filter;  // Bloom filter for keys
  std::string _min_key, _max_key; // key range

  SSTableMeta(const std::string& fn, const std::vector<Slice>& keys, int bits_per_key = 10)
    : _filename(fn) {
    MiniBloom bf(bits_per_key);
    _bloom_filter = bf.CreateFilter(keys, bits_per_key);
    if (!keys.empty()) {
      _min_key = keys[0].str();
      _max_key = keys.back().str();
    }
  }

  // 快速检查: key 是否可能在这个 SSTable 中
  bool MayContain(const Slice& key) const {
    // 1. range check
    if (!_min_key.empty() && key.str() < _min_key) return false;
    if (!_max_key.empty() && key.str() > _max_key) return false;
    // 2. bloom filter check
    MiniBloom bf(10);
    return bf.MayMatch(key, _bloom_filter);
  }
};

void run() {
  HR("Ex7: MiniDB — MemTable + Bloom Filter");

  // MemTable
  MemTable mt;
  mt.Put(Slice("key_0001"), Slice("value_0001"));
  mt.Put(Slice("key_0500"), Slice("value_0500"));
  mt.Put(Slice("key_1000"), Slice("value_1000"));

  println("MemTable: ", mt.size(), " entries");
  auto v = mt.Get(Slice("key_0500"));
  println("  Get(key_0500) = ", v.ok() ? *v : "not found");
  auto v2 = mt.Get(Slice("key_9999"));
  println("  Get(key_9999) = ", v2.ok() ? *v2 : "not found");
  println();

  // Bloom Filter for SSTable
  std::vector<std::string> key_strs;
  key_strs.reserve(100);
  std::vector<Slice> keys;
  keys.reserve(100);
  for (int i = 0; i < 100; i++) { key_strs.push_back("user_" + std::to_string(i)); keys.push_back(Slice(key_strs.back())); }

  SSTableMeta meta("sst_001.sst", keys, 10);

  // 查询测试
  println("SSTable Bloom Filter test:");
  int fp = 0;
  for (int i = 0; i < 100; i++) {
    if (!meta.MayContain(Slice(key_strs[i]))) println("  ❌ false negative! (should not happen)");
  }
  for (int i = 200; i < 1200; i++) {
    if (meta.MayContain(Slice("user_" + std::to_string(i)))) fp++;
  }
  println("  Inserted keys: all passed ✅");
  println("  Non-inserted keys: ", fp, " false positives / 1000 = ", fp / 10.0, "%");

  println("\n📖 MemTable + Bloom Filter 是 LSM Tree 的两层防护:");
  println("  1. 先查 MemTable (最新数据)");
  println("  2. Bloom Filter 预检 → 跳过不可能包含 key 的 SSTable");
  println("  3. 只在 Bloom Filter 说 '可能' 时才实际读 SSTable");
}

} // namespace ex7

// ============================================================================
// Exercise 8: MiniDB — SSTable Builder
// ============================================================================

namespace ex8 {

// ── 简化的 SSTable Builder (from leveldb table_builder.cc) ──────────
struct SSTableBuilder {
  std::string _data;

  // 添加一个 key-value (必须按 key 排序!)
  void Add(const Slice& key, const Slice& value) {
    char buf[5];
    char* p = EncodeVarint32(buf, key.size());
    _data.append(buf, p - buf);
    _data.append(key.data(), key.size());
    p = EncodeVarint32(buf, value.size());
    _data.append(buf, p - buf);
    _data.append(value.data(), value.size());
  }

  std::string Finish() { return std::move(_data); }

  size_t size() const { return _data.size(); }
};

// ── 简化的 SSTable Reader ───────────────────────────────────────────
struct SSTableReader {
  Slice _data;

  explicit SSTableReader(const std::string& s) : _data(Slice(s)) {}

  MiniOptional<std::string> Get(const Slice& target_key) {
    const char* p = _data.data();
    const char* end = p + _data.size();
    while (p < end) {
      uint32_t klen, vlen;
      p = GetVarint32(p, end, &klen);
      if (!p || p + klen > end) return kNullopt;
      Slice key(p, klen); p += klen;
      p = GetVarint32(p, end, &vlen);
      if (!p || p + vlen > end) return kNullopt;
      Slice value(p, vlen); p += vlen;
      if (key == target_key) return value.str();
    }
    return kNullopt;
  }

  // 遍历所有 entries (用于 compaction)
  void ForEach(std::function<void(const Slice&, const Slice&)> cb) {
    const char* p = _data.data();
    const char* end = p + _data.size();
    while (p < end) {
      uint32_t klen, vlen;
      p = GetVarint32(p, end, &klen);
      if (!p || p + klen > end) break;
      Slice key(p, klen); p += klen;
      p = GetVarint32(p, end, &vlen);
      if (!p || p + vlen > end) break;
      Slice value(p, vlen); p += vlen;
      cb(key, value);
    }
  }
};

void run() {
  HR("Ex8: MiniDB — SSTable Builder & Reader");

  // 构建 SSTable
  SSTableBuilder builder;
  for (int i = 0; i < 10; i++) {
    std::string k = "key_" + std::to_string(i);
    std::string v = "value_" + std::to_string(i * 10);
    builder.Add(Slice(k), Slice(v));
  }
  auto sst_data = builder.Finish();

  println("SSTable built: ", builder.size(), " bytes, 10 entries");

  // 读取
  SSTableReader reader(sst_data);
  println("Read:");
  for (auto k : {"key_0", "key_5", "key_9", "key_99"}) {
    auto v = reader.Get(Slice(k));
    println("  Get(", k, ") = ", v.ok() ? *v : "(not found)");
  }
  println();

  println("📖 SSTable = Varint 编码的有序 key-value 序列");
  println("   实际 leveldb 的 SSTable 还包括:");
  println("   - Data Block (前缀压缩 + restart 点 + CRC)");
  println("   - Index Block (二分定位)");
  println("   - Filter Block (Bloom Filter)");
  println("   - Footer (48 bytes, meta+index handle + magic)");
}

} // namespace ex8

// ============================================================================
// Exercise 9: MiniDB — 整合引擎 (Put + Get + Flush)
// ============================================================================

namespace ex9 {

using ex6::SimpleWAL;
using ex7::MemTable;
using ex7::SSTableMeta;
using ex8::SSTableBuilder;
using ex8::SSTableReader;

// ── MiniDB 引擎 ──────────────────────────────────────────────────────
struct MiniDB {
  std::string _db_path;
  MemTable _mem;
  SimpleWAL _wal;

  // SSTable 层 (简化: 只有一个 SSTable)
  std::string _sst_data;
  std::unique_ptr<SSTableMeta> _sst_meta;
  bool _has_sst = false;

  explicit MiniDB(const std::string& path) : _db_path(path) {}

  // PUT — 写入路径
  void Put(const Slice& key, const Slice& value) {
    // 1. 写 WAL (崩溃恢复)
    _wal.Append(key, value);
    // 2. 写 MemTable
    _mem.Put(key, value);
  }

  // GET — 读取路径
  MiniOptional<std::string> Get(const Slice& key) {
    // 1. 查 MemTable (最新数据)
    auto v = _mem.Get(key);
    if (v.ok()) return v;
    // 2. 查 SSTable
    if (_has_sst) {
      if (_sst_meta && !_sst_meta->MayContain(key)) return kNullopt;
      SSTableReader reader(_sst_data);
      return reader.Get(key);
    }
    return kNullopt;
  }

  // FLUSH — 将 MemTable 刷到 SSTable
  void Flush() {
    if (_mem.size() == 0) return;

    // 构建 SSTable (key 已经有序, 因为 std::map)
    SSTableBuilder builder;
    // 遍历 MemTable 写入 SSTable
    for (auto& [k, v] : _mem._data) {
      builder.Add(Slice(k), Slice(v));
    }
    _sst_data = builder.Finish();

    // 构建 Bloom Filter metadata
    std::vector<Slice> keys;
    for (auto& [k, v] : _mem._data) keys.push_back(Slice(k));
    _sst_meta = std::make_unique<SSTableMeta>("sst_001.sst", keys, 10);

    _has_sst = true;
    println("  Flushed ", _mem.size(), " entries to SSTable");

    // 清空 MemTable (实际 leveldb: swap mem + 新建 mem)
    _mem = MemTable{};
    _wal = SimpleWAL{};
  }

  size_t mem_size() const { return _mem.size(); }
};

void run() {
  HR("Ex9: MiniDB — 整合引擎");

  println("MiniDB 引擎架构 (代码见 namespace ex9):");
  println();
  println("  ┌──────────────────────────────────────┐");
  println("  │  MiniDB::Put(key, value)             │");
  println("  │    1. WAL.Append(key, val) — 持久化  │");
  println("  │    2. MemTable.Put(key, val) — 内存  │");
  println("  └──────────────────────────────────────┘");
  println();
  println("  ┌──────────────────────────────────────┐");
  println("  │  MiniDB::Get(key) → Optional<string> │");
  println("  │    1. MemTable.Get(key) → 如果有 ✓   │");
  println("  │    2. Bloom Filter 预检 → 没有则 ✗   │");
  println("  │    3. SSTable.Get(key) → 扫描查找    │");
  println("  └──────────────────────────────────────┘");
  println();
  println("  ┌──────────────────────────────────────┐");
  println("  │  MiniDB::Flush()                     │");
  println("  │    1. 遍历 MemTable → SSTable Builder│");
  println("  │    2. Build Bloom Filter for keys    │");
  println("  │    3. 清空 MemTable + WAL            │");
  println("  └──────────────────────────────────────┘");
  println();

  println("📖 MiniDB 集成了 Month 5 的核心模式:");
  println("  leveldb   → Slice, Varint, WAL, MemTable, Bloom Filter, SSTable");
  println("  STL       → std::map (替代 SkipList), std::unique_ptr, std::string");
  println("  Mini STL  → MiniOptional (替代 Status/NotFound)");
  println("  fmtlib    → format_to 模式 (Varint 编码写出)");
  println("  libevent  → 回调模式 (SSTable.ForEach)");
}

} // namespace ex9

// ============================================================================
// Exercise 10: Month 5 综合反思 + Month 6 路线图
// ============================================================================

namespace ex10 {

void run() {
  HR("Ex10: Month 5 反思 + Month 6 路线图");

  println("╔══════════════════════════════════════════════════════╗");
  println("║        Month 5: 源码阅读 — 看透实现                    ║");
  println("╠══════════════════════════════════════════════════════╣");
  println("║                                                      ║");
  println("║  W23: STL 源码深潜     (~1300行 scaffold)             ║");
  println("║       vector/string/function/arena/sort/map          ║");
  println("║                                                      ║");
  println("║  W24: leveldb 源码阅读  (~1650行 scaffold)            ║");
  println("║       LSM Tree + WAL + SkipList + Bloom + SSTable     ║");
  println("║                                                      ║");
  println("║  W25: fmtlib 源码阅读   (~1360行 scaffold)            ║");
  println("║       format parse + type erasure + int/float format  ║");
  println("║                                                      ║");
  println("║  W26: libevent 源码阅读 (~1290行 scaffold)            ║");
  println("║       Reactor + evbuffer + bufferevent + minheap      ║");
  println("║                                                      ║");
  println("║  W27: 小型 STL 实现     (~1490行 scaffold)            ║");
  println("║       10 components: ptr/opt/var/span/list/deque/...  ║");
  println("║                                                      ║");
  println("║  W28: Month 5 收官     (this file)                   ║");
  println("║       回顾 + 跨项目对比 + MiniDB Capstone              ║");
  println("║                                                      ║");
  println("╠══════════════════════════════════════════════════════╣");
  println("║  📊 总计: ~7500 行 scaffold 代码                      ║");
  println("║  📊 阅读: ~100,000 行源码 (leveldb+fmt+libevent+STL)  ║");
  println("║  📊 实现: 20+ 个 Mini STL 组件                        ║");
  println("║  📊 手写: 10 个 MiniDB 核心模块                       ║");
  println("╚══════════════════════════════════════════════════════╝");
  println();

  println("🔑 Month 5 的 10 大收获:");
  println("  1. 「源码是最好的老师」— 读 10 本书不如读 1 个经典项目");
  println("  2. 「设计决策都有原因」— CoW 被废, SSO 胜出, 2x vs 1.5x");
  println("  3. 「零开销抽象是可能的」— Slice, unique_ptr, span 都是零开销");
  println("  4. 「Type Erasure 是 C++ 的核心模式」— 从 Slice 到 std::function");
  println("  5. 「Arena/Pool 无处不在」— leveldb/STL/fmtlib 都有自定义分配器");
  println("  6. 「Iterator 是算法与结构的桥梁」— 5 种 category + tag dispatch");
  println("  7. 「Reactor 模式统治 I/O」— libevent, nginx, Node.js, Redis");
  println("  8. 「LSM Tree 统治存储」— leveldb, RocksDB, Cassandra, HBase");
  println("  9. 「编译期计算是未来」— consteval, constexpr, template meta");
  println("  10.「C 和 C++ 可以都很优雅」— leveldb(C++) + libevent(C) 都是杰作");
  println();

  println("📖 Month 6 路线图 — 综合项目:");
  println();
  println("  方案 A: 网络服务框架");
  println("    - 集成 libevent 的 Reactor + leveldb 存储 + fmtlib 日志");
  println("    - 实现 HTTP Server → REST API → 数据持久化");
  println("    - 性能测试: 对比 raw epoll vs libevent vs asio");
  println();
  println("  方案 B: 分布式 KV 存储");
  println("    - 基于 leveldb 设计, 加上 Raft 共识算法");
  println("    - 网络层用 libevent reactor");
  println("    - 日志和监控用 fmtlib");
  println();
  println("  方案 C: C++ 编译时工具集");
  println("    - constexpr JSON parser / config loader");
  println("    - compile-time regex / format checker");
  println("    - 深入学习 C++20/23 的 consteval/constexpr 能力");
  println();
  println("  方案 D: 性能分析工具");
  println("    - perf + eBPF +火焰图集成");
  println("    - 自动检测: false sharing, cache miss, branch mispredict");
  println("    - 生成优化建议报告");
  println();

  println("🎯 Month 1-5 全景:");
  println("  M1: 现代 C++ (RAII/Move/STL/Templates/Lambda/Exception) ✅");
  println("  M2: OS 边界 (File/Process/Signal/Thread) ✅");
  println("  M3: 网络编程 (Socket/TCP/epoll/HTTP/Chat/Redis) ✅");
  println("  M4: 极致性能 (Cache/perf/gdb/Sanitizer/io_uring) ✅");
  println("  M5: 源码阅读 (STL/leveldb/fmtlib/libevent/Mini STL) ✅");
  println("  M6: 综合项目 ← 即将开始");
}

} // namespace ex10

// ============================================================================
// Main
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║  Month 5 Week 28: 收官 — 源码阅读总结 + MiniDB Capstone       ║
║  "看透实现 → 写出自己的"                                      ║
╚══════════════════════════════════════════════════════════════╝)");

  // Part A: 回顾
  ex1::run();
  ex2::run();
  ex3::run();
  ex4::run();
  ex5::run();

  // Part B: Capstone
  HR("Part B: Capstone — MiniDB");
  println("  集成 Month 5 所有源码模式:\n");
  ex6::run();
  ex7::run();
  ex8::run();
  ex9::run();
  ex10::run();

  HR("🎉 Month 5 完成!");
  println();
  println("  ✅ Week 23: STL 源码深潜 — 看懂 vector/string/function 内部实现");
  println("  ✅ Week 24: leveldb — 理解 LSM Tree 存储引擎的每一层");
  println("  ✅ Week 25: fmtlib  — 理解现代 C++ 格式化库的极致优化");
  println("  ✅ Week 26: libevent — 理解 Reactor 模式的事件驱动设计");
  println("  ✅ Week 27: Mini STL — 亲手实现 10 个标准库核心组件");
  println("  ✅ Week 28: Capstone — 集成所有模式构建 MiniDB");
  println();
  println("  🌟 你现在拥有的能力:");
  println("    - 阅读并理解 10 万行级别的 C/C++ 项目");
  println("    - 识别并应用 Type Erasure, Arena, Iterator, Reactor 等核心模式");
  println("    - 从零实现 STL 核心组件 (unique_ptr→RB Tree→Variant)");
  println("    - 构建 LSM Tree 存储引擎 (WAL→MemTable→SSTable)");
  println("    - 理解编译期计算 (consteval)、模板元编程、性能优化");
  println();
  println("  🚀 下一步: Month 6 综合项目");
  return 0;
}
