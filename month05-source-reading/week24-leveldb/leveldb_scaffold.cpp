// ============================================================================
// Month 5 Week 24: leveldb 源码阅读
// 日期: 2026-06-24
//
// 阅读目标: Google leveldb — LSM Tree 存储引擎的经典实现
// 源码位置: ../leveldb/
// 源码规模: ~22K 行 C++ (db/ + table/ + util/ + include/)
//
// leveldb 是 Jeff Dean 和 Sanjay Ghemawat 写的键值存储引擎，
// 是 C++ 源码阅读的"必读教材"——代码简洁、设计精妙、注释出色。
//
// 核心架构 (LSM Tree):
//   写入路径: Put → WAL(日志) → MemTable(SkipList) → 满了 → SSTable(磁盘)
//   读取路径: Get → MemTable → Immutable MemTable → SSTable(L0→L1→...→L6)
//   合并 (Compaction): L层文件数超限 → 合并到 L+1 层
//
// 10 个练习, 按自底向上顺序: 基础组件 → 内存结构 → 磁盘格式 → 整合
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;

// ============================================================================
// 辅助工具
// ============================================================================

template <typename... Args>
void println(Args&&... args) {
  if constexpr (sizeof...(args) > 0) {
    ((std::cout << std::forward<Args>(args)), ...);
  }
  std::cout << '\n';
}

template <typename... Args>
void print(Args&&... args) {
  ((std::cout << std::forward<Args>(args)), ...);
}

void HR(std::string_view title = "") {
  println("\n", std::string(72, '='), "\n  ", title, "\n", std::string(72, '='));
}

// ── Slice: leveldb 的核心抽象 ──────────────────────────────────────────
// include/leveldb/slice.h
// Slice = {const char* data_, size_t size_} — 不拥有内存的字符串视图
// leveldb 中几乎所有接口都用 Slice 传参, 避免 std::string 拷贝
struct Slice {
  const char* _data = nullptr;
  size_t _size = 0;

  Slice() = default;
  Slice(const char* d, size_t n) : _data(d), _size(n) {}
  Slice(const std::string& s) : _data(s.data()), _size(s.size()) {}
  Slice(const char* s) : _data(s), _size(std::strlen(s)) {}

  const char* data() const { return _data; }
  size_t size() const { return _size; }
  bool empty() const { return _size == 0; }

  char operator[](size_t i) const { return _data[i]; }

  void remove_prefix(size_t n) {
    assert(n <= _size);
    _data += n;
    _size -= n;
  }

  std::string to_string() const { return {_data, _size}; }

  bool operator==(const Slice& o) const {
    return _size == o._size && std::memcmp(_data, o._data, _size) == 0;
  }

  bool starts_with(const Slice& prefix) const {
    return _size >= prefix._size &&
           std::memcmp(_data, prefix._data, prefix._size) == 0;
  }
};

// ── 编码层 (全局可用, leveldb 的 util/coding.h) ──────────────────────
inline void EncodeFixed32(char* dst, uint32_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(value);
  buf[1] = static_cast<uint8_t>(value >> 8);
  buf[2] = static_cast<uint8_t>(value >> 16);
  buf[3] = static_cast<uint8_t>(value >> 24);
}
inline uint32_t DecodeFixed32(const char* p) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(p);
  return (static_cast<uint32_t>(buf[0])) |
         (static_cast<uint32_t>(buf[1]) << 8) |
         (static_cast<uint32_t>(buf[2]) << 16) |
         (static_cast<uint32_t>(buf[3]) << 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(value);
  buf[1] = static_cast<uint8_t>(value >> 8);
  buf[2] = static_cast<uint8_t>(value >> 16);
  buf[3] = static_cast<uint8_t>(value >> 24);
  buf[4] = static_cast<uint8_t>(value >> 32);
  buf[5] = static_cast<uint8_t>(value >> 40);
  buf[6] = static_cast<uint8_t>(value >> 48);
  buf[7] = static_cast<uint8_t>(value >> 56);
}

// Varint32
inline char* EncodeVarint32(char* dst, uint32_t v) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  static const int B = 128;
  if (v < (1 << 7)) {
    *(ptr++) = v;
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B;     *(ptr++) = v >> 7;
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;     *(ptr++) = (v >> 7) | B;   *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;     *(ptr++) = (v >> 7) | B;   *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;     *(ptr++) = (v >> 7) | B;   *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;   *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}

// Varint64
inline char* EncodeVarint64(char* dst, uint64_t v) {
  static const int B = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= B) {
    *(ptr++) = v | B;
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

// Varint32 decode
inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const uint8_t*>(p));
    if ((result & 128) == 0) { *value = result; return p + 1; }
  }
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) { result |= ((byte & 127) << shift); }
    else { result |= (byte << shift); *value = result; return p; }
  }
  return nullptr;
}

inline bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, limit - q);
  return true;
}

// Varint64 decode
inline const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) { result |= ((byte & 127) << shift); }
    else { result |= (byte << shift); *value = result; return p; }
  }
  return nullptr;
}

inline bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, limit - q);
  return true;
}

inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) { v >>= 7; len++; }
  return len;
}

// ── Status: 错误处理 ───────────────────────────────────────────────────
// include/leveldb/status.h
// leveldb 不用异常, 用 Status 返回错误 (类似 Rust 的 Result)
struct Status {
  enum Code { kOk = 0, kNotFound = 1, kCorruption = 2, kNotSupported = 3,
              kInvalidArgument = 4, kIOError = 5 };
  Code _code = kOk;
  std::string _msg;

  static Status OK() { return {}; }
  static Status NotFound(const std::string& m) { return {kNotFound, m}; }
  static Status Corruption(const std::string& m) { return {kCorruption, m}; }
  static Status IOError(const std::string& m) { return {kIOError, m}; }
  static Status InvalidArgument(const std::string& m) { return {kInvalidArgument, m}; }

  bool ok() const { return _code == kOk; }
  bool IsNotFound() const { return _code == kNotFound; }
  std::string ToString() const {
    if (ok()) return "OK";
    const char* names[] = {"OK","NotFound","Corruption","NotSupported",
                           "InvalidArgument","IOError"};
    return std::string(names[_code]) + ": " + _msg;
  }
};

// ============================================================================
// Exercise 1: leveldb 概览与编译
// ============================================================================
//
// 【阅读清单】
//   include/leveldb/db.h      — 公开 API: DB::Open/Put/Get/Delete
//   include/leveldb/options.h — Options: create_if_missing, comparator, filter
//   include/leveldb/slice.h   — Slice 定义
//   include/leveldb/status.h  — Status 定义
//   doc/index.md              — 架构文档
//
// 【关键概念: LSM Tree】
//   传统 B-Tree (MySQL InnoDB): 随机写 → 多次磁盘 seek → 慢
//   LSM Tree (leveldb/RocksDB): 顺序写 WAL + 内存排序 + 批量刷盘 → 快
//
//   写入路径:
//     1. Write-Ahead Log (WAL) — 先写日志, 崩溃恢复
//     2. MemTable (SkipList)  — 内存有序结构, O(log n)
//     3. 当 MemTable 满(4MB) → 冻结为 Immutable MemTable
//     4. 后台线程 flush → SSTable (Sorted String Table, 磁盘)
//
//   读取路径 (可能需要多次磁盘查找):
//     1. 查 MemTable (最新数据)
//     2. 查 Immutable MemTable
//     3. 查 SSTable: L0 → L1 → ... → L6 (层级递进)
//
//   合并 (Compaction):
//     L层 SSTable 数 > 阈值 → 选一个文件 + L+1 层有重叠的文件合并
//     Minor Compaction: MemTable → L0 SSTable
//     Major Compaction: L0 → L1, L1 → L2 ...
//
// 【动手: 编译 leveldb 并运行基准测试】

namespace ex1 {

void run() {
  HR("Ex1: leveldb 概览");

  println("leveldb 源码结构:");
  println("  include/leveldb/   — 公开头文件 (db.h, options.h, slice.h ...)");
  println("  db/                — 核心实现 (db_impl, memtable, skiplist, log)");
  println("  table/             — SSTable 格式 (table, block, filter_block, format)");
  println("  util/              — 工具库 (coding, arena, cache, bloom, crc32c)");
  println("  port/              — 平台抽象 (线程, 原子操作)");
  println();

  println("LSM Tree 架构:");
  println("  写入: Put → WAL → MemTable(SkipList) → 满了 → SSTable(磁盘)");
  println("  读取: Get → MemTable → Immutable MemTable → SSTable L0→L6");
  println("  后台: Compaction 合并 SSTable, 删除过期版本");
  println();

  // 编译 leveldb (需要 cmake)
  fs::path leveldb_dir = "../leveldb";
  if (fs::exists(leveldb_dir)) {
    println("✅ leveldb 源码已就绪: ", fs::canonical(leveldb_dir).string());
  }

  println("\n📖 阅读指导:");
  println("  1. 先读 include/leveldb/db.h — 理解公开接口 (< 100 行)");
  println("  2. 再读 doc/index.md — 架构概览");
  println("  3. 然后按自底向上读: util/ → db/skiplist.h → db/memtable → table/ → db/db_impl");
  println("  4. 每个文件都短小精悍 (平均 200-400 行), 逐文件读即可");
}

} // namespace ex1

// ============================================================================
// Exercise 2: Coding — Varint 编码与字节序
// ============================================================================
//
// 【阅读清单】
//   util/coding.h    — EncodeFixed32/64, EncodeVarint32/64, GetVarint32Ptr
//   util/coding.cc   — 实现细节
//
// 【关键设计】
//   leveldb 的所有持久化都用小端字节序 (least-significant byte first)
//   注释写明: "Recent clang and gcc optimize this to a single mov instruction"
//
//   Varint (变长整数):
//     - 小数字用 1 字节, 大数字最多 5 字节 (32-bit) 或 10 字节 (64-bit)
//     - 每字节低 7 位存数据, 最高位(128=0x80)表示"还有后续字节"
//     - leveldb 对 32-bit varint 用了手写展开 (性能优化)
//     - 对 64-bit varint 用了 while 循环 (展开代码太长)
//
//   Length-Prefixed Slice:
//     - [Varint32 length][data bytes]
//     - leveldb 中最常见的编码模式

namespace ex2 {

// ── Length-Prefixed Slice (使用全局编码函数) ────────────────────────
void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  char buf[5];
  char* end = EncodeVarint32(buf, value.size());
  dst->append(buf, end - buf);
  dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, &len);
  if (q == nullptr) return false;
  if (static_cast<size_t>(limit - q) < len) return false;
  *result = Slice(q, len);
  *input = Slice(q + len, limit - q - len);
  return true;
}

void run() {
  HR("Ex2: Coding — Varint 编码");

  // ── Fixed32 编码演示 ──
  {
    char buf[4];
    EncodeFixed32(buf, 0x12345678);
    uint32_t decoded = DecodeFixed32(buf);
    println("Fixed32: 0x12345678 → bytes: ",
            std::hex, (int)(uint8_t)buf[0], ' ', (int)(uint8_t)buf[1], ' ',
            (int)(uint8_t)buf[2], ' ', (int)(uint8_t)buf[3],
            " → decode: 0x", decoded, std::dec);
    println("  (小端: 低字节在前 — 78 56 34 12)");
  }
  println();

  // ── Varint32 编码演示 ──
  {
    println("Varint32 编码示例:");
    std::vector<uint32_t> values = {0, 1, 127, 128, 16383, 16384, 1000000, 0xFFFFFFFF};
    for (auto v : values) {
      char buf[5];
      char* end = EncodeVarint32(buf, v);
      int bytes = end - buf;
      print("  v=", v, " → ", bytes, " bytes: ");
      for (int i = 0; i < bytes; i++)
        print(" ", std::hex, (int)(uint8_t)buf[i], std::dec);
      println();
    }
    println("  (小值省空间: 0→1B, 128→2B, 1M→3B, MAX→5B)");
  }
  println();

  // ── Length-Prefixed 编码 ──
  {
    println("Length-Prefixed Slice 编码:");
    std::string buf;
    PutLengthPrefixedSlice(&buf, Slice("hello"));
    PutLengthPrefixedSlice(&buf, Slice("world!"));
    println("  编码 'hello' + 'world!' → ", buf.size(), " bytes");
    println("  格式: [1B len=5][hello][1B len=6][world!]");

    // 解码
    Slice input(buf);
    Slice result;
    while (GetLengthPrefixedSlice(&input, &result)) {
      println("  decoded: '", result.to_string(), "'");
    }
  }
}

} // namespace ex2

// ============================================================================
// Exercise 3: Arena — Bump Pointer 分配器
// ============================================================================
//
// 【阅读清单】
//   util/arena.h   — Arena 接口
//   util/arena.cc  — 实现 (仅 66 行!)
//
// 【关键设计】
//   - 预分配 4KB blocks, 线性分配 (bump pointer)
//   - O(1) 分配, 不能单独 free, 只能整体销毁
//   - 大对象 (>1KB) 单独分配, 避免浪费
//   - AllocateAligned: 保证 8 字节对齐 (或 sizeof(void*))
//   - memory_usage_ 用 atomic 跟踪 (允许无锁读取)
//
// 【为什么 SkipList 用 Arena?】
//   MemTable 中所有 SkipList 节点生命周期相同 — 完美适合 Arena
//   避免百万次 malloc/free, 内存局部性好

namespace ex3 {

static const int kBlockSize = 4096;

struct MiniArena {
  char* _alloc_ptr = nullptr;
  size_t _alloc_remaining = 0;
  std::vector<char*> _blocks;
  std::atomic<size_t> _memory_usage{0};

  ~MiniArena() {
    for (auto* b : _blocks) delete[] b;
  }

  char* Allocate(size_t bytes) {
    assert(bytes > 0);
    if (bytes <= _alloc_remaining) {
      char* result = _alloc_ptr;
      _alloc_ptr += bytes;
      _alloc_remaining -= bytes;
      return result;
    }
    return AllocateFallback(bytes);
  }

  char* AllocateAligned(size_t bytes) {
    const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
    size_t current_mod = reinterpret_cast<uintptr_t>(_alloc_ptr) & (align - 1);
    size_t slop = (current_mod == 0 ? 0 : align - current_mod);
    size_t needed = bytes + slop;
    if (needed <= _alloc_remaining) {
      char* result = _alloc_ptr + slop;
      _alloc_ptr += needed;
      _alloc_remaining -= needed;
      assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
      return result;
    }
    return AllocateFallback(bytes); // fallback always aligned
  }

  size_t MemoryUsage() const { return _memory_usage.load(std::memory_order_relaxed); }

private:
  char* AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) {
      // 大对象: 单独分配
      char* result = new char[bytes];
      _blocks.push_back(result);
      _memory_usage.fetch_add(bytes + sizeof(char*), std::memory_order_relaxed);
      return result;
    }
    // 分配新 block, 浪费旧 block 的剩余空间
    _alloc_ptr = new char[kBlockSize];
    _alloc_remaining = kBlockSize;
    _blocks.push_back(_alloc_ptr);
    _memory_usage.fetch_add(kBlockSize + sizeof(char*), std::memory_order_relaxed);

    char* result = _alloc_ptr;
    _alloc_ptr += bytes;
    _alloc_remaining -= bytes;
    return result;
  }
};

void run() {
  HR("Ex3: Arena — Bump Pointer 分配器");

  MiniArena arena;

  // 分配几个小对象
  auto* a = reinterpret_cast<uint32_t*>(arena.Allocate(sizeof(uint32_t)));
  *a = 42;
  auto* b = reinterpret_cast<double*>(arena.Allocate(sizeof(double)));
  *b = 3.14159;
  (void)arena.Allocate(100); // c

  println("分配了 3 个小对象: *a=", *a, " *b=", *b);
  println("Arena 总内存: ", arena.MemoryUsage(), " bytes");
  println("  (所有分配在同一 4KB block 内 — 连续内存, cache-friendly!)");

  // 对齐分配
  auto* d = arena.AllocateAligned(8);
  uintptr_t addr = reinterpret_cast<uintptr_t>(d);
  println("Aligned 分配: address=", d, " (对齐=", addr % 8 == 0 ? "✅" : "❌", ")");

  // 大对象
  (void)arena.Allocate(2000); // big: > 1KB, 单独 block
  println("大对象(2000B) 单独分配");

  // 性能对比
  constexpr int N = 100000;
  {
    // std::allocator (new/delete)
    std::vector<int*> ptrs;
    ptrs.reserve(N);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) ptrs.push_back(new int(i));
    for (auto* p : ptrs) delete p;
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    println("\nnew/delete ", N, " 次: ", us, "us");
  }
  {
    MiniArena a2;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) {
      auto* p = reinterpret_cast<int*>(a2.Allocate(sizeof(int)));
      *p = i;
    }
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    println("Arena      ", N, " 次: ", us, "us (不能单独 free, 整体释放)");
  }
  println("\n📖 阅读 util/arena.cc (66 行) — leveldb 最精炼的设计之一");
}

} // namespace ex3

// ============================================================================
// Exercise 4: SkipList — 跳表
// ============================================================================
//
// 【阅读清单】
//   db/skiplist.h — **仅一个头文件!** 模板实现, 380 行
//   db/skiplist_test.cc — 测试用例
//
// 【关键设计】
//   - 概率平衡 (非强制平衡): 1/4 概率增加一层, 期望高度 = 1/(1-1/4) ≈ 1.33
//   - kMaxHeight = 12 → 最多支持 4^12 ≈ 16M 个节点 (概率性)
//   - 写需外部锁, 读完全无锁 (利用 acquire/release 内存序)
//   - Node 用 flexible array: next_[1] + AllocateAligned 分配额外空间
//   - 从不删除节点 (直到整个 SkipList 销毁) — 简化并发控制
//
// 【核心算法】
//   FindGreaterOrEqual(key, prev):
//     从最高层开始, 如果 next->key < key → 前进; 否则降层
//     记录每层的 prev 节点 (供 Insert 用)
//
//   Insert(key):
//     1. FindGreaterOrEqual 找到位置 + 记录 prev[]
//     2. RandomHeight 生成随机高度
//     3. 每层插入: x->next = prev[i]->next; prev[i]->next = x
//     4. 用 release store 发布新节点 (读者看到即完整)

namespace ex4 {

// 简化版 SkipList (为了可读性, 去掉了 leveldb 的模板和并发安全)
struct SimpleSkipList {
  static constexpr int kMaxHeight = 12;
  static constexpr int kBranching = 4;

  struct Node {
    int key;
    std::vector<Node*> next; // next[level] — leveldb 用 flexible array
    explicit Node(int k, int height) : key(k), next(height, nullptr) {}
  };

  Node* _head;
  int _max_height = 1;
  std::mt19937 _rng{42};

  SimpleSkipList() {
    _head = new Node(-1, kMaxHeight); // sentinel
  }

  ~SimpleSkipList() {
    // leveldb 从不删除节点 (用 Arena), 这里手动清理
    Node* node = _head;
    while (node) {
      Node* next = node->next[0];
      delete node;
      node = next;
    }
  }

  int RandomHeight() {
    int h = 1;
    while (h < kMaxHeight && (_rng() % kBranching == 0)) h++;
    return h;
  }

  Node* FindGreaterOrEqual(int key, Node** prev = nullptr) {
    Node* x = _head;
    int level = _max_height - 1;
    while (true) {
      Node* next = x->next[level];
      if (next && next->key < key) {
        x = next; // 本层继续前进
      } else {
        if (prev) prev[level] = x;
        if (level == 0) return next;
        level--; // 降层
      }
    }
  }

  void Insert(int key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || x->key != key); // 不重复

    int height = RandomHeight();
    if (height > _max_height) {
      for (int i = _max_height; i < height; i++) prev[i] = _head;
      _max_height = height;
    }

    Node* n = new Node(key, height);
    for (int i = 0; i < height; i++) {
      n->next[i] = prev[i]->next[i];
      prev[i]->next[i] = n;
    }
  }

  bool Contains(int key) {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && x->key == key;
  }

  void Print() {
    println("SkipList (height=", _max_height, "):");
    for (int level = _max_height - 1; level >= 0; level--) {
      print("  L", level, ": HEAD");
      Node* x = _head->next[level];
      while (x) { print(" -> ", x->key); x = x->next[level]; }
      println();
    }
  }
};

void run() {
  HR("Ex4: SkipList — 跳表");

  SimpleSkipList list;

  // 插入
  std::vector<int> keys = {3, 6, 7, 9, 12, 19, 17, 26, 21, 25};
  for (auto k : keys) list.Insert(k);

  list.Print();
  println();

  // 查找
  for (auto k : {6, 12, 15, 25, 100}) {
    println("  Contains(", k, "): ", list.Contains(k) ? "✅" : "❌");
  }
  println();

  // 复杂度
  println("📖 时间复杂度:");
  println("  查找: O(log n) 期望 (每层跳过 ~4 个节点)");
  println("  插入: O(log n) 期望");
  println("  空间: O(n) 期望 (每节点 ~1.33 层)");
  println();
  println("📖 leveldb 的 SkipList 额外亮点:");
  println("  1. 并发读无锁 — acquire/release 内存序保证可见性");
  println("  2. Node 用 Arena 分配 — 生命周期统一管理");
  println("  3. 模板化 Key + Comparator — 泛型设计");
  println("  4. next_[1] flexible array — 省内存");
  println();
  println("📖 精读 db/skiplist.h (380 行) — 并发数据结构的教科书级实现");
}

} // namespace ex4

// ============================================================================
// Exercise 5: Bloom Filter — 布隆过滤器
// ============================================================================
//
// 【阅读清单】
//   util/bloom.cc         — BloomFilterPolicy 实现 (91 行)
//   include/leveldb/filter_policy.h — FilterPolicy 接口
//   util/hash.h           — Hash 函数 (MurmurHash-like)
//
// 【关键设计】
//   - 作用: 快速判断 key 是否可能在某 SSTable 中
//   - 空间换时间: 用 10 bits/key → ~1% false positive rate
//   - 创建参数:
//       bits_per_key: 位/键
//       k = bits_per_key * ln(2) ≈ bits_per_key * 0.69  (哈希函数数)
//       例: 10 bits/key → k=7 → 误报率 ≈ 0.0081
//   - Double Hashing 技巧 (Kirsch & Mitzenmacher 2006):
//       h_i(x) = h1(x) + i * h2(x), 其中 h2(x) = (h1 >> 17) | (h1 << 15)
//       用一个 hash 值生成 k 个"独立"hash
//   - 过滤器末尾存 k 值 → 不同参数的过滤器可互操作

namespace ex5 {

struct MiniBloomFilter {
  size_t _bits_per_key;
  size_t _k; // hash 函数数

  explicit MiniBloomFilter(int bits_per_key) : _bits_per_key(bits_per_key) {
    _k = static_cast<size_t>(bits_per_key * 0.69); // ln(2) ≈ 0.69
    if (_k < 1) _k = 1;
    if (_k > 30) _k = 30;
  }

  // 简单 hash (leveldb 用 MurmurHash-like, util/hash.cc)
  static uint32_t Hash(const Slice& key) {
    uint32_t h = 0xbc9f1d34;
    for (size_t i = 0; i < key.size(); i++) {
      h ^= static_cast<uint8_t>(key[i]);
      h *= 0x01000193; // FNV-like
    }
    return h;
  }

  // 创建 bloom filter (keys 数组, n 个键)
  std::string CreateFilter(const std::vector<Slice>& keys) {
    size_t bits = keys.size() * _bits_per_key;
    if (bits < 64) bits = 64; // 最小 64 bits

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8; // round up to full byte

    std::string filter(bytes, '\0');
    filter.push_back(static_cast<char>(_k)); // last byte = # of probes

    for (const auto& key : keys) {
      uint32_t h = Hash(key);
      const uint32_t delta = (h >> 17) | (h << 15); // rotate right 17
      for (size_t j = 0; j < _k; j++) {
        const uint32_t bitpos = h % bits;
        filter[bitpos / 8] |= (1 << (bitpos % 8));
        h += delta;
      }
    }
    return filter;
  }

  // 检查 key 是否可能存在
  bool KeyMayMatch(const Slice& key, const Slice& filter) const {
    if (filter.size() < 2) return false;

    size_t bits = (filter.size() - 1) * 8;
    size_t k = static_cast<uint8_t>(filter[filter.size() - 1]);
    if (k > 30) return true; // 兼容不同参数

    uint32_t h = Hash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((filter[bitpos / 8] & (1 << (bitpos % 8))) == 0)
        return false; // 一定不存在
      h += delta;
    }
    return true; // 可能存在 (含误报)
  }
};

void run() {
  HR("Ex5: Bloom Filter — 布隆过滤器");

  MiniBloomFilter bf(10); // 10 bits/key

  // 创建过滤器: 插入 1000 个 key
  // ⚠️ 先 reserve 再 push_back — vector 扩容会使 Slice 指针失效!
  std::vector<std::string> key_strings;
  key_strings.reserve(1000);
  std::vector<Slice> keys;
  keys.reserve(1000);
  for (int i = 0; i < 1000; i++) {
    key_strings.push_back("key_" + std::to_string(i));
    keys.push_back(Slice(key_strings.back()));
  }
  auto filter = bf.CreateFilter(keys);

  println("插入 1000 keys @ 10 bits/key:");
  println("  过滤器大小: ", filter.size(), " bytes (", filter.size() * 8, " bits)");
  println("  理论误报率: ", std::pow(0.6185, 10 * 0.69), " (k=", bf._k, ")");
  println("  实际每 key 空间: ", (double)filter.size() / 1000, " bytes");
  println();

  // 验证: 存在的 key (同样要保证 string 存活)
  int false_negatives = 0;
  for (int i = 0; i < 1000; i++) {
    std::string s = "key_" + std::to_string(i);
    if (!bf.KeyMayMatch(Slice(s), filter))
      false_negatives++;
  }
  println("  假阴性 (存在的 key 被判定为不存在): ", false_negatives,
          false_negatives == 0 ? " ✅" : " ❌");

  // 验证: 不存在的 key — 测误报率
  int false_positives = 0;
  constexpr int TEST_N = 10000;
  for (int i = 2000; i < 2000 + TEST_N; i++) {
    std::string s = "key_" + std::to_string(i);
    if (bf.KeyMayMatch(Slice(s), filter))
      false_positives++;
  }
  double fp_rate = (double)false_positives / TEST_N;
  println("  假阳性 (不存在的 key 被判定为存在): ", false_positives,
          "/", TEST_N, " = ", fp_rate * 100, "%");
  println();

  println("📖 为什么 leveldb 需要 Bloom Filter?");
  println("  Get(key) 可能要查很多 SSTable 文件");
  println("  每次磁盘 read 都很贵 (~10ms)");
  println("  先查 Bloom Filter: 不存在 → 跳过这个 SSTable → 省一次磁盘 IO");
  println("  10 bits/key -> ~1% 误报 -> 99% 的「不存在」查询被快速拦截");
}

} // namespace ex5

// ============================================================================
// Exercise 6: Cache — LRU 缓存 (ShardedLRU)
// ============================================================================
//
// 【阅读清单】
//   util/cache.cc         — LRUCache + ShardedLRUCache + HandleTable (500+ 行)
//   include/leveldb/cache.h — Cache 接口
//
// 【关键设计】
//   leveldb 的 Cache 有三层:
//   1. HandleTable — 开放寻址哈希表 (open addressing), 存 {key → LRUHandle*}
//   2. LRUCache — 双向链表实现 LRU 淘汰 (in_use + lru 两个链表)
//   3. ShardedLRUCache — 16 个分片, 减少锁竞争 (key hash & 15)
//
//   LRUHandle:
//     - 同时存在于哈希表和 LRU 链表中
//     - refs 引用计数: 0 = 只被 cache 持有 (可淘汰), ≥1 = 在 use 中
//     - in_cache: 是否还在哈希表中 (淘汰时从哈希表移除, 但等 refs=0 才 delete)
//
//   Insert/Lookup 流程:
//     Insert: 建 LRUHandle → 插入哈希表 → 插入 lru 链表 → 可能淘汰
//     Lookup: 哈希查找 → refs++ → 从 lru 移到 in_use → 返回
//     Release: refs-- → 若 refs==0 且 !in_cache → delete
//              若 refs==0 且 in_cache → 从 in_use 移回 lru

namespace ex6 {

// 简化版 LRU Cache (核心概念演示)
template <typename K, typename V>
struct SimpleLRUCache {
  struct Entry {
    K key;
    V value;
    Entry(K k, V v) : key(std::move(k)), value(std::move(v)) {}
  };

  using ListIter = typename std::list<Entry>::iterator;
  std::list<Entry> _lru; // 头部=最新, 尾部=最旧
  std::unordered_map<K, ListIter> _map;
  size_t _capacity;

  explicit SimpleLRUCache(size_t cap) : _capacity(cap) {}

  std::optional<V> Get(const K& key) {
    auto it = _map.find(key);
    if (it == _map.end()) return std::nullopt;
    // 移到链表头部 (最近使用)
    _lru.splice(_lru.begin(), _lru, it->second);
    return it->second->value;
  }

  void Put(const K& key, const V& value) {
    auto it = _map.find(key);
    if (it != _map.end()) {
      // 更新并移到头部
      it->second->value = value;
      _lru.splice(_lru.begin(), _lru, it->second);
      return;
    }
    // 淘汰最旧
    if (_lru.size() >= _capacity) {
      auto last = _lru.back();
      _map.erase(last.key);
      _lru.pop_back();
    }
    // 插入头部
    _lru.emplace_front(key, value);
    _map[key] = _lru.begin();
  }

  size_t Size() const { return _lru.size(); }
};

void run() {
  HR("Ex6: Cache — LRU 缓存");

  SimpleLRUCache<int, std::string> cache(3);

  cache.Put(1, "one");
  cache.Put(2, "two");
  cache.Put(3, "three");
  println("插入 1,2,3 → size=", cache.Size());

  // 访问 1 (移到头部)
  auto v = cache.Get(1);
  println("Get(1) → ", v.value_or("null"));

  cache.Put(4, "four"); // 淘汰 2 (最旧)
  println("Put(4) → 淘汰 key=2");

  println("Get(1)=", cache.Get(1).value_or("null"), " (✅ 还在)");
  println("Get(2)=", cache.Get(2).value_or("null"), " (❌ 被淘汰)");
  println("Get(3)=", cache.Get(3).value_or("null"), " (✅ 还在)");
  println("Get(4)=", cache.Get(4).value_or("null"), " (✅ 新插入)");
  println();

  println("📖 leveldb 的 Cache 额外亮点:");
  println("  1. ShardedLRUCache — 16 shards, hash(key) & 15 选分片, 锁粒度 1/16");
  println("  2. HandleTable — 开放寻址哈希表, 32 bucket 起步, 2x 扩容");
  println("  3. 双链表设计: in_use 链表(正在被使用) + lru 链表(可淘汰)");
  println("  4. refs 引用计数保证安全性: 正在用的 handle 不会被 delete");
  println();
  println("📖 精读 util/cache.cc — leveldb 最复杂的单文件 (~500 行)");
}

} // namespace ex6

// ============================================================================
// Exercise 7: Write-Ahead Log (WAL)
// ============================================================================
//
// 【阅读清单】
//   db/log_format.h   — 日志格式定义
//   db/log_writer.h   — Writer 接口
//   db/log_writer.cc  — Writer 实现
//   db/log_reader.h   — Reader 接口
//   db/log_reader.cc  — Reader 实现
//
// 【关键设计】
//   日志以 32KB block 为单位组织 (类似磁盘扇区)
//   每个 block 内可存多个 record, record 可跨 block (fragmented)
//
//   Record 格式:
//     Header (7 bytes): [CRC32(4B) | Length(2B) | Type(1B)]
//     Data: [变长数据]
//
//   Record Type:
//     kFullType   — 完整记录, 不跨 block
//     kFirstType  — 分片第一条
//     kMiddleType — 分片中间
//     kLastType   — 分片最后一条
//
//   CRC32C: 硬件加速的 CRC (SSE4.2 _mm_crc32_u64)
//     - leveldb 用 cpuid 检测硬件支持
//     - 支持则用硬件指令, 否则 fallback 到查表法

namespace ex7 {

// ── 日志格式常量 (来自 db/log_format.h) ────────────────────────────
static const int kLogBlockSize = 32768;
static const int kLogHeaderSize = 4 + 2 + 1; // CRC(4) + len(2) + type(1)

enum RecordType : uint8_t {
  kZeroType   = 0,
  kFullType   = 1,
  kFirstType  = 2,
  kMiddleType = 3,
  kLastType   = 4,
  kEof        = 5,  // 内部用
  kBadRecord  = 6,
};

// ── 简化的 Log Writer ──────────────────────────────────────────────
struct SimpleLogWriter {
  std::string _dest;
  int _block_offset = 0;

  // 添加一条记录
  void AddRecord(const Slice& data) {
    const char* ptr = data.data();
    size_t left = data.size();
    bool begin = true;

    do {
      int leftover = kLogBlockSize - _block_offset;
      assert(leftover >= 0);
      if (leftover < kLogHeaderSize) {
        // 当前 block 放不下 header → 填充零
        _dest.append(leftover, '\0');
        _block_offset = 0;
        continue;
      }

      int avail = leftover - kLogHeaderSize;
      size_t fragment_len = std::min(left, (size_t)avail);

      RecordType type;
      if (begin && left == fragment_len) {
        type = kFullType;
      } else if (begin) {
        type = kFirstType;
      } else if (left == fragment_len) {
        type = kLastType;
      } else {
        type = kMiddleType;
      }

      EmitPhysicalRecord(type, ptr, fragment_len);
      ptr += fragment_len;
      left -= fragment_len;
      begin = false;
    } while (left > 0);
  }

private:
  // CRC32 (简化版, 仅用于演示概念)
  static uint32_t CRC32(const char* data, size_t n) {
    // leveldb 用 SSE4.2 CRC32C 硬件指令, 这里用简单累加模拟
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
      crc = (crc >> 8) ^ (uint32_t)(uint8_t)data[i] * 0xEDB88320;
    }
    return crc;
  }

  void EmitPhysicalRecord(RecordType type, const char* ptr, size_t len) {
    assert(len <= 0xFFFF);
    assert(_block_offset + kLogHeaderSize + len <= kLogBlockSize);

    // 格式化 header (小端)
    char header[kLogHeaderSize];
    // CRC32 of (type, data) — 注意 leveldb 先算 type CRC 再算 data CRC
    uint32_t crc = CRC32(&reinterpret_cast<const char&>(type), 1);
    crc = CRC32(ptr, len); // 简化, leveldb 正确拼接
    EncodeFixed32(header, crc);
    EncodeFixed32(header + 4, len); // 其实只需要 2 字节, leveldb 额外编码 type
    // 简化: header+4 = len 低 16 位
    header[4] = static_cast<char>(len & 0xFF);
    header[5] = static_cast<char>((len >> 8) & 0xFF);
    header[6] = static_cast<char>(type);

    _dest.append(header, kLogHeaderSize);
    _dest.append(ptr, len);
    _block_offset += kLogHeaderSize + len;
  }
};

// ── 简化的 Log Reader ──────────────────────────────────────────────
struct SimpleLogReader {
  Slice _data;

  explicit SimpleLogReader(const std::string& s) : _data(Slice(s)) {}

  bool ReadRecord(Slice* record) {
    // 跳过碎片记录, 只演示 kFullType
    while (_data.size() >= kLogHeaderSize) {
      // 读 header
      /*uint32_t crc =*/ (void)DecodeFixed32(_data.data());
      uint16_t length = (uint8_t)_data[4] | ((uint8_t)_data[5] << 8);
      uint8_t  type   = _data[6];

      if (static_cast<size_t>(kLogHeaderSize) + length > _data.size()) return false;

      if (type == kFullType) {
        *record = Slice(_data.data() + kLogHeaderSize, length);
        _data = Slice(_data.data() + kLogHeaderSize + length,
                      _data.size() - kLogHeaderSize - length);
        return true;
      }
      // 跳过非 kFullType (简化实现)
      _data = Slice(_data.data() + kLogHeaderSize + length,
                    _data.size() - kLogHeaderSize - length);
    }
    return false;
  }
};

void run() {
  HR("Ex7: Write-Ahead Log (WAL)");

  SimpleLogWriter writer;

  // 写入几条记录
  writer.AddRecord(Slice("SET key1 value1"));
  writer.AddRecord(Slice("SET key2 value2"));
  writer.AddRecord(Slice("DELETE key1"));
  writer.AddRecord(Slice(std::string(300, 'X'))); // 小记录, 不跨 block

  println("写入 4 条记录 → 日志大小: ", writer._dest.size(), " bytes");

  // 读取
  SimpleLogReader reader(writer._dest);
  Slice record;
  int count = 0;
  while (reader.ReadRecord(&record)) {
    count++;
    println("  record[", count, "]: '", record.to_string(), "' (", record.size(), "B)");
  }
  println();

  println("📖 日志格式设计要点:");
  println("  1. 32KB block 对齐 — 方便磁盘预读和 mmap");
  println("  2. CRC32C 校验 — 检测磁盘损坏 (bit rot)");
  println("  3. 记录可分片 — 大 Value (>32KB) 也能安全写入");
  println("  4. 预计算 type_crc_ — AddRecord 热路径避免重复计算 CRC");
  println();
  println("📖 精读 db/log_writer.cc (110 行) + db/log_reader.cc (310 行)");
}

} // namespace ex7

// ============================================================================
// Exercise 8: MemTable — 内存表
// ============================================================================
//
// 【阅读清单】
//   db/memtable.h    — MemTable 接口
//   db/memtable.cc   — 实现 (SkipList + Arena + InternalKey)
//   db/dbformat.h    — InternalKey 编码, ValueType, SequenceNumber
//   db/dbformat.cc
//
// 【关键设计】
//   MemTable = SkipList + Arena + KeyComparator
//   - InternalKey = user_key + SequenceNumber(7B) + ValueType(1B)
//   - 编码后的 InternalKey 按 (user_key DESC, seq DESC) 排序
//   - ValueType: kTypeValue=1 (有效值), kTypeDeletion=0 (删除标记)
//   - Get 时找到 key → 检查 seq+type → 返回 value 或 NotFound
//   - 引用计数: MemTable 可能被多个 iterator 同时引用

namespace ex8 {

// ── InternalKey 编码 (简化自 db/dbformat.h) ─────────────────────────
// 格式: user_key + PackedSeqAndType (8 bytes)
// PackedSeqAndType: SequenceNumber(56 bits) | ValueType(8 bits)
// 注意 leveldb 用大端存 seq: 从后往前写, 这样按 memcmp 排序时 seq 大的在前

enum ValueType : uint8_t { kTypeDeletion = 0, kTypeValue = 1 };
using SequenceNumber = uint64_t;

// 简化: 直接拼接 seq+type 到 key 后面 (不模拟 leveldb 的复杂比较器)
static void AppendInternalKey(std::string* dst, const Slice& key,
                               SequenceNumber seq, ValueType type) {
  dst->append(key.data(), key.size());
  // Pack seq and type (大端序, 这样比较时自然排序)
  // leveldb 技巧: 把 seq 存成大端且取反, 让更新 seq 排前面
  uint64_t packed = (seq << 8) | type;
  for (int i = 7; i >= 0; i--) {
    dst->push_back(static_cast<char>((packed >> (i * 8)) & 0xFF));
  }
}

static Slice ExtractUserKey(const Slice& internal_key) {
  return Slice(internal_key.data(), internal_key.size() - 8);
}

static std::pair<SequenceNumber, ValueType> ExtractSeqAndType(const Slice& ik) {
  const char* p = ik.data() + ik.size() - 8;
  uint64_t packed = 0;
  for (int i = 0; i < 8; i++) {
    packed = (packed << 8) | static_cast<uint8_t>(p[i]);
  }
  return {packed >> 8, static_cast<ValueType>(packed & 0xFF)};
}

// ── 简化的 MemTable ─────────────────────────────────────────────────
struct SimpleMemTable {
  // 用 std::map 替代 SkipList (概念相同, 实现简单)
  std::map<std::string, std::string, std::less<>> _data;

  void Add(SequenceNumber seq, ValueType type,
           const Slice& key, const Slice& value) {
    std::string internal_key;
    AppendInternalKey(&internal_key, key, seq, type);
    _data[internal_key] = value.to_string();
  }

  // 查询: 找最新的 seq
  // leveldb: seq 大端取反, 同一 key 最新排最前 → lower_bound 直击
  // 简化实现: std::map 是升序, seq 递增排在后面 → 遍历取最后匹配
  Status Get(const Slice& user_key, std::string* value) {
    bool found = false;
    ValueType result_type = kTypeDeletion;
    std::string result_value;
    for (const auto& [k, v] : _data) {
      Slice ik(k);
      if (ExtractUserKey(ik) == user_key) {
        found = true;
        auto st = ExtractSeqAndType(ik);
        result_type = st.second;
        result_value = v;
      }
    }
    if (!found) return Status::NotFound("key not in memtable");
    if (result_type == kTypeDeletion) return Status::NotFound("deleted");
    *value = result_value;
    return Status::OK();
  }

  size_t Size() const { return _data.size(); }
};

void run() {
  HR("Ex8: MemTable — 内存表");

  SimpleMemTable mt;

  // 模拟: 同一个 key 被多次写入 (seq 递增)
  mt.Add(1, kTypeValue, Slice("name"), Slice("Alice"));
  mt.Add(2, kTypeValue, Slice("name"), Slice("Bob"));   // 更新
  mt.Add(3, kTypeValue, Slice("age"), Slice("25"));
  mt.Add(4, kTypeDeletion, Slice("name"), Slice(""));    // 删除

  println("MemTable 内容 (" , mt.Size(), " 条 InternalKey):");
  for (const auto& [k, v] : mt._data) {
    auto [seq, type] = ExtractSeqAndType(Slice(k));
    println("  user_key='", ExtractUserKey(Slice(k)).to_string(),
            "' seq=", seq, " type=", (type == kTypeValue ? "VALUE" : "DEL"),
            " value='", v, "'");
  }
  println();

  // 查询
  {
    std::string val;
    auto s = mt.Get(Slice("name"), &val);
    println("Get('name'): ", s.ToString()); // 被删除
  }
  {
    std::string val;
    auto s = mt.Get(Slice("age"), &val);
    println("Get('age'): ", s.ToString(), " value='", val, "'");
  }
  {
    std::string val;
    auto s = mt.Get(Slice("unknown"), &val);
    println("Get('unknown'): ", s.ToString());
  }
  println();

  println("📖 InternalKey 编码技巧:");
  println("  1. user_key 在前 → 先按 user_key 排序");
  println("  2. seq 大端存 + 取反 → 同一 key 内, 新版本排在旧版本前");
  println("  3. ValueType 在最后 → 区分有效值和删除标记");
  println("  4. 这 8 bytes 让 memcmp 直接能正确排序 SkipList!");
  println();
  println("📖 精读 db/memtable.cc (170 行) + db/dbformat.h (200 行)");
}

} // namespace ex8

// ============================================================================
// Exercise 9: SSTable — Sorted String Table (磁盘格式)
// ============================================================================
//
// 【阅读清单】
//   table/format.h         — BlockHandle, Footer, ReadBlock
//   table/format.cc        — 编码/解码
//   table/block.h          — Block (数据块)
//   table/block.cc         — Block 实现 + Block::Iter
//   table/block_builder.h  — BlockBuilder
//   table/block_builder.cc — 构建 Block (重启点机制)
//   table/table.h          — Table (SSTable 读取器)
//   table/table.cc         — Table::Open, InternalGet, 两级迭代
//   table/table_builder.cc — TableBuilder (SSTable 构建器)
//   table/filter_block.h   — FilterBlockBuilder/Reader
//   table/filter_block.cc  — 过滤器块实现
//
// 【SSTable 磁盘布局】
//   ┌──────────────────────┐
//   │  Data Block 0        │  ← key-value 数据, 按序排列
//   │  Data Block 1        │  ← 每 block ~4KB, 末尾有 restart 数组
//   │  ...                 │
//   │  Data Block N        │
//   ├──────────────────────┤
//   │  Filter Block        │  ← Bloom Filter (每 2KB 数据一个 filter entry)
//   ├──────────────────────┤
//   │  Meta Index Block    │  ← { "filter.xxx": BlockHandle → Filter Block }
//   ├──────────────────────┤
//   │  Index Block         │  ← { last_key_of_DB_i: BlockHandle → DB_i }
//   ├──────────────────────┤
//   │  Footer (48 bytes)   │  ← { meta_index_handle, index_handle, magic }
//   └──────────────────────┘
//
// 【Block 内部布局】
//   ┌────────────────────┐
//   │  Entry: shared_bytes(变长) + unshared_bytes(变长) + value_len(变长) │
//   │          + unshared_key_data + value_data                          │
//   │  Entry: ...                                                       │
//   │  ... (多个 entry)                                                 │
//   ├────────────────────┤
//   │  Restart offsets    │  ← 每 16 个 entry 设一个重启点 (完整 key)
//   │  (uint32_t array)   │
//   ├────────────────────┤
//   │  Num restarts (4B)  │
//   ├────────────────────┤
//   │  Type (1B)          │  ← 0=uncompressed, 1=snappy
//   │  CRC32 (4B)         │
//   └────────────────────┘

namespace ex9 {

// ── BlockHandle ─────────────────────────────────────────────────────
struct BlockHandle {
  uint64_t _offset = ~0ULL;
  uint64_t _size = ~0ULL;

  static constexpr int kMaxEncodedLength = 10 + 10;

  void EncodeTo(std::string* dst) const {
    char buf[10];
    char* end = EncodeVarint64(buf, _offset);
    dst->append(buf, end - buf);
    end = EncodeVarint64(buf, _size);
    dst->append(buf, end - buf);
  }

  static BlockHandle DecodeFrom(Slice* input) {
    BlockHandle h;
    uint64_t val;
    if (GetVarint64(input, &val)) h._offset = val;
    if (GetVarint64(input, &val)) h._size = val;
    return h;
  }
};

// ── Footer ──────────────────────────────────────────────────────────
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ULL;

struct Footer {
  BlockHandle _meta_index;
  BlockHandle _index;
  static constexpr int kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  std::string Encode() const {
    std::string dst;
    _meta_index.EncodeTo(&dst);
    _index.EncodeTo(&dst);
    // 填充到固定长度
    dst.resize(kEncodedLength - 8, '\0');
    // 追加 magic (小端, 固定 8 bytes)
    char magic[8];
    for (int i = 0; i < 8; i++) {
      magic[i] = static_cast<char>((kTableMagicNumber >> (i * 8)) & 0xFF);
    }
    dst.append(magic, 8);
    return dst;
  }
};

// ── 简化的 Table Builder ────────────────────────────────────────────
struct SimpleTableBuilder {
  std::string _data_blocks;     // 所有 data block 连续存放
  std::vector<BlockHandle> _data_handles;
  std::string _last_key;
  std::string _current_block;
  int _entry_count = 0;
  std::vector<uint32_t> _restarts;
  static constexpr int kRestartInterval = 16;

  void Add(const Slice& key, const Slice& value) {
    // 计算共享前缀
    size_t shared = 0;
    size_t min_len = std::min(key.size(), _last_key.size());
    while (shared < min_len && key[shared] == _last_key[shared]) shared++;

    // Encode entry
    char buf[15];
    char* p = EncodeVarint32(buf, shared);
    p = EncodeVarint32(p, key.size() - shared);
    p = EncodeVarint32(p, value.size());
    _current_block.append(buf, p - buf);
    _current_block.append(key.data() + shared, key.size() - shared);
    _current_block.append(value.data(), value.size());

    _last_key = key.to_string();
    _entry_count++;

    if (_entry_count % kRestartInterval == 0) {
      _restarts.push_back(_current_block.size());
      _last_key.clear(); // 重置前缀压缩
    }
  }

  void FinishBlock() {
    if (_current_block.empty()) return;
    // 写 restart 数组
    for (auto offset : _restarts)
      for (int i = 0; i < 4; i++)
        _current_block.push_back(static_cast<char>((offset >> (i * 8)) & 0xFF));
    // 写 num_restarts
    uint32_t n = _restarts.size();
    for (int i = 0; i < 4; i++)
      _current_block.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));

    BlockHandle handle;
    handle._offset = _data_blocks.size();
    handle._size = _current_block.size();
    _data_handles.push_back(handle);
    _data_blocks += _current_block;

    _current_block.clear();
    _restarts.clear();
    _entry_count = 0;
    _last_key.clear();
  }

  std::string Finish() {
    FinishBlock();
    // 构建 Index Block: (last_key, handle) pairs
    std::string index_block;
    for (size_t i = 0; i < _data_handles.size(); i++) {
      // 简化: 用 index 作为 key
      std::string idx_key = "~index~" + std::to_string(i);
      char buf[10];
      (void)EncodeVarint32(buf, idx_key.size());
      // ... 实际 leveldb 的 index block 也用相同的 block 格式
      // 这里简化演示
    }
    return _data_blocks; // 简化返回
  }
};

void run() {
  HR("Ex9: SSTable — 磁盘格式");

  SimpleTableBuilder builder;

  println("构建 SSTable (插入 100 个有序 key):");
  for (int i = 0; i < 100; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "value_" + std::to_string(i * 10);
    builder.Add(Slice(key), Slice(val));
  }
  builder.FinishBlock();
  auto data = builder.Finish();

  println("  Data blocks: ", builder._data_handles.size(), " blocks");
  println("  总数据大小: ", data.size(), " bytes");
  println();

  println("📖 SSTable 格式关键设计:");
  println("  Data Block:");
  println("    - 前缀压缩 (shared prefix encoding) — 连续 key 共享前缀");
  println("    - Restart 点 (每 16 个 entry) — 二分查找, O(log restart)");
  println("    - 末尾 5 bytes: 1B type (压缩标记) + 4B CRC32");
  println();
  println("  Index Block (一级索引):");
  println("    - (last_key_of_data_block, BlockHandle) 对");
  println("    - 二分查找定位 key 所属的 Data Block");
  println();
  println("  Two-Level Iterator (二级索引):");
  println("    - Level 1: Index Block iterator → 选 Data Block");
  println("    - Level 2: Data Block iterator → 遍历 entries");
  println();
  println("  Filter Block:");
  println("    - 每 2KB 数据生成一个 Bloom Filter");
  println("    - Filter offset 数组做二级索引 → 快速定位 filter");
  println();
  println("  Footer:");
  println("    - 固定 48 bytes → 解析时直接 seek 到文件末尾读");
  println("    - 包含 meta_index_handle + index_handle + magic number");
  println();
  println("📖 精读顺序: table/format.h → table/block_builder.cc → table/block.cc");
  println("            → table/filter_block.cc → table/table.cc → table/table_builder.cc");
}

} // namespace ex9

// ============================================================================
// Exercise 10: DBImpl — 整合全貌
// ============================================================================
//
// 【阅读清单】
//   db/db_impl.h           — DBImpl 类声明
//   db/db_impl.cc          — Open/Get/Put/Delete/Write/Compact (1500+ 行)
//   db/version_set.h       — Version, VersionSet, Compaction
//   db/version_set.cc      — 版本管理, 合并逻辑
//   db/version_edit.h      — VersionEdit (文件增删记录)
//   db/write_batch.cc      — WriteBatch 批量写入
//
// 【关键流程】
//
//   DB::Open():
//     1. 读 MANIFEST → 恢复 VersionSet (所有 SSTable 文件列表)
//     2. 读 WAL → 恢复 MemTable (崩溃恢复)
//     3. 启动后台 Compaction 线程
//
//   Put(key, value):
//     1. WriteBatch 封装
//     2. 获取写锁 (同一时刻只有一个 writer)
//     3. 写 WAL (log_writer.AddRecord)
//     4. 插入 MemTable (mem_->Add)
//     5. 若 MemTable 满 → 触发 Compaction
//
//   Get(key):
//     1. 获取当前 Version (Snapshot)
//     2. 查 MemTable → 找到? 返回
//     3. 查 Immutable MemTable → 找到? 返回
//     4. 按 level 顺序查 SSTable:
//        L0: 文件 key range 可能重叠, 按 seq 从新到旧查每个文件
//        L1-L6: 文件不重叠, 二分定位一个文件, 用 TableCache 查
//        每个 SSTable: Index Block → Data Block → Bloom Filter 预检
//     5. 返回 value 或 NotFound
//
//   Compaction:
//     1. 选文件 (size 或 seek 触发)
//     2. 构建 MergingIterator: 覆盖 L层 compact_file + L+1层所有重叠文件
//     3. 归并排序写出新 SSTable
//     4. VersionEdit 记录变更
//     5. 安装新 Version (MVCC: 旧 Version 被 snapshot 持有不删除)

namespace ex10 {

void run() {
  HR("Ex10: DBImpl — 整合全貌");

  println("╔══════════════════════════════════════════════════════╗");
  println("║        leveldb 完整数据流                              ║");
  println("╠══════════════════════════════════════════════════════╣");
  println("║                                                      ║");
  println("║  PUT ──► WriteBatch                                  ║");
  println("║           │                                          ║");
  println("║           ├──► WAL (log_writer.cc) ──► 磁盘           ║");
  println("║           │     CRC32C 校验                           ║");
  println("║           │                                          ║");
  println("║           └──► MemTable (memtable.cc)                ║");
  println("║                  │                                   ║");
  println("║                  ├── SkipList (skiplist.h)           ║");
  println("║                  │     O(log n) 写, 并发读无锁         ║");
  println("║                  │                                   ║");
  println("║                  ├── Arena (arena.cc)                ║");
  println("║                  │     4KB blocks, bump pointer       ║");
  println("║                  │                                   ║");
  println("║                  └── InternalKey (dbformat.h)        ║");
  println("║                        (user_key,seq,type)            ║");
  println("║                                                      ║");
  println("║  MemTable 满 (4MB) → Immutable MemTable               ║");
  println("║                     → 后台 flush                      ║");
  println("║                     → SSTable (table_builder.cc)      ║");
  println("║                                                      ║");
  println("║  GET ──► MemTable → Immut MemTable                    ║");
  println("║         → SSTable L0 (查 TableCache)                  ║");
  println("║            ├── Index Block 二分定位                   ║");
  println("║            ├── Bloom Filter 预检                      ║");
  println("║            └── Data Block 前缀压缩扫描                 ║");
  println("║         → SSTable L1-L6 (二分定位一个文件)            ║");
  println("║                                                      ║");
  println("║  后台 Compaction:                                     ║");
  println("║    L0 (4 files) → merge → L1                          ║");
  println("║    L1 (10MB) → merge → L2 (100MB)                     ║");
  println("║    L2 (100MB) → merge → L3 (1000MB)                   ║");
  println("║    每层大小 × 10                                       ║");
  println("║                                                      ║");
  println("╚══════════════════════════════════════════════════════╝");
  println();

  println("📖 核心源文件阅读顺序 (自底向上):");
  println();
  println("  基础层 (util/):");
  println("    1. util/slice.h       (40行)  — 零拷贝字符串视图");
  println("    2. util/status.h      (60行)  — 错误处理");
  println("    3. util/coding.cc     (156行) — 编码层, 一切持久化的基础");
  println("    4. util/arena.cc      (66行)  — 最简单的内存分配器");
  println("    5. util/crc32c.cc     (300行) — 硬件加速 CRC");
  println("    6. util/bloom.cc      (91行)  — Bloom Filter");
  println("    7. util/cache.cc      (500行) — LRU Cache, 最复杂的 util");
  println();
  println("  内存层 (db/):");
  println("    8. db/skiplist.h      (380行) — 并发数据结构教科书");
  println("    9. db/dbformat.h      (200行) — InternalKey 编码精髓");
  println("   10. db/memtable.cc     (170行) — SkipList+Arena+Comparator 组合");
  println("   11. db/log_writer.cc   (110行) — WAL 写入");
  println("   12. db/log_reader.cc   (310行) — WAL 读取 (最复杂的 log 代码)");
  println();
  println("  磁盘层 (table/):");
  println("   13. table/format.h     (99行)  — BlockHandle, Footer, 常量");
  println("   14. table/block_builder.cc (160行) — Data Block 构建");
  println("   15. table/block.cc     (290行) — Block 读取 + Iter");
  println("   16. table/filter_block.cc (190行) — Filter Block 构建/读取");
  println("   17. table/table_builder.cc (270行) — SSTable 构建");
  println("   18. table/table.cc     (320行) — SSTable 读取");
  println("   19. table/two_level_iterator.cc (180行) — 二级迭代器");
  println();
  println("  整合层 (db/):");
  println("   20. db/version_set.h   (260行) — Version/Compaction");
  println("   21. db/version_set.cc  (900行) — Version 管理, 最长文件!");
  println("   22. db/version_edit.cc (120行) — VersionEdit 增量变更");
  println("   23. db/db_impl.cc      (1500行) — DB 实现, 最核心!");
  println("   24. db/write_batch.cc  (120行) — 批量写入");
  println();

  // 统计
  println("📊 代码量统计 (核心文件):");
  println("  util/    ~1,500 行 (7 files)");
  println("  db/      ~4,500 行 (12 files)");
  println("  table/   ~1,800 行 (8 files)");
  println("  总计     ~7,800 行 (27 files)");
  println("  加上测试 + 头文件 + 辅助 ≈ 22,000 行");
  println();

  println("🔑 leveldb 最值得学习的 10 个设计决策:");
  println("  1. Slice — 零拷贝字符串, 影响了一代 C++ 项目");
  println("  2. Varint — 空间换时间, 小数字 1 字节, 省磁盘");
  println("  3. Arena — 最简单的内存管理, 完美的生命周期匹配");
  println("  4. SkipList — 概率平衡, 并发读无锁, 比 B-Tree 简单");
  println("  5. InternalKey — 8 字节让 memcmp = key 比较");
  println("  6. Block 前缀压缩 — 重启点机制, 快速定位 + 高压缩率");
  println("  7. Two-Level Iterator — 两层嵌套迭代器, 优雅的组合模式");
  println("  8. MVCC (Sequence Number) — 无锁快照, 读写不互斥");
  println("  9. Leveled Compaction — 空间放大 vs 写放大的经典权衡");
  println("  10. MANIFEST + CURRENT — 原子故障恢复, 无 fsync 目录");
}

} // namespace ex10

// ============================================================================
// Main
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║     Month 5 Week 24: leveldb 源码阅读                          ║
║     "看透实现 — LSM Tree 存储引擎的经典"                        ║
╚══════════════════════════════════════════════════════════════╝)");

  ex1::run();
  ex2::run();
  ex3::run();
  ex4::run();
  ex5::run();
  ex6::run();
  ex7::run();
  ex8::run();
  ex9::run();
  ex10::run();

  HR("Week 24 完成!");
  println("✅ 阅读了 leveldb 核心架构 (LSM Tree)");
  println("✅ 理解了 Varint / Arena / SkipList / BloomFilter / LRU Cache");
  println("✅ 理解了 WAL / MemTable / SSTable 三层存储");
  println("✅ 理解了 Compaction / MVCC / Snapshot");
  println("✅ 下一步: Week 25 — fmtlib 源码阅读");
  println();
  println("📖 推荐继续阅读:");
  println("  1. RocksDB (leveldb 的 Facebook 增强版 — 优化了 10 年!)");
  println("  2. doc/impl.md (leveldb 官方实现文档)");
  println("  3. Bigtable Paper (Google 2006 — LSM Tree 的起源)");
  return 0;
}
