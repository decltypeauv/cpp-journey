// ============================================================================
// Month 4: 极致性能 — Beyond the Code
// Week 22: Month 4 收官 — 从测量到优化, 从调试到交付
//
// 六周旅程回顾:
//   W17: CPU Cache — 内存层次, false sharing, AoS/SoA, ECS, prefetch
//   W18: perf    — 硬件计数器, 火焰图, micro-benchmark, 优化流程
//   W19: gdb     — 条件断点, watchpoint, rr, Python 脚本, core dump
//   W20: sanitizer — ASan/UBSan/TSan/MSan/LSan, CI 集成
//   W21: io_uring — 异步 I/O, batch submit, 零拷贝, SQ/CQ ring
//   W22: capstone — 综合实战: 异步日志服务器
//
// Capstone 项目: 高性能异步日志服务器 (Async Log Server)
// ============================================================================
// 架构:
//   ┌──────────┐     ┌──────────────┐     ┌──────────┐
//   │  Client  │ ──► │  io_uring    │ ──► │  Ring    │
//   │ (netcat) │     │  accept/read │     │  Buffer  │
//   └──────────┘     └──────────────┘     └──────────┘
//                         │                      │
//                    非阻塞提交             cache-friendly
//                         │                  无锁 SPSC
//                    批量收割                  │
//                                              ▼
//                                        ┌──────────┐
//                                        │  Disk    │
//                                        │  Writer  │
//                                        │ (async)  │
//                                        └──────────┘
//
// 技术栈:
//   - io_uring: 异步网络 accept + read
//   - SPSC ring buffer: lock-free, cache-line padded
//   - Memory-mapped file: 零拷贝写入
//   - Timer/alarm: 定期 fsync
//
// 10 个练习: 前5个回顾, 后5个构建 capstone
// ============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/io_uring.h>
#include <mutex>
#include <netinet/in.h>
#include <numeric>
#include <random>
#include <set>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono;
static volatile int64_t g_sink = 0;

// ============================================================================
// Timer
// ============================================================================

class Timer {
  high_resolution_clock::time_point _start;
public:
  Timer() : _start(high_resolution_clock::now()) {}
  int64_t elapsed_ns() const {
    return duration_cast<nanoseconds>(high_resolution_clock::now() - _start).count();
  }
  double elapsed_us() const { return elapsed_ns() / 1000.0; }
  double elapsed_ms() const { return elapsed_ns() / 1'000'000.0; }
};

// ============================================================================
// Part A: 知识回顾与自检 (Ex1-5)
// ============================================================================

// ============================================================================
// Ex1: 六周全景回顾 — 核心概念映射
//
// 任务: 完成下面的填空/连线题 (在注释中回答)
// ============================================================================

namespace ex1_review {
  void run() {
    std::cout << "\n===== Ex1: Month 4 核心概念回顾 =====\n\n";

    std::cout << "─── 概念 ↔ 工具 映射 ───\n";
    std::cout << "1. 程序大部分时间在等 ___(内存)___ 而不是计算\n";
    std::cout << "2. Cache line 大小 = ___(64)___ 字节 (x86_64)\n";
    std::cout << "3. False sharing 用 ___(alignas(64) / padding)___ 解决\n";
    std::cout << "4. SoA 适合 ___(单字段遍历)___, AoS 适合 ___(全字段访问)___\n";
    std::cout << "5. Branch predictor 对 ___(规律)___ 分支准确率 ≈ 100%\n";
    std::cout << "6. __builtin_prefetch 用于 ___(提前拉数据到 cache)___\n";
    std::cout << "7. perf stat 看 ___(cycles/IPC/cache-misses)___, perf record 看 ___(热点函数)___\n";
    std::cout << "8. IPC < 1.0 → ___(memory)___ bound, IPC > 2.0 → ___(compute)___ bound\n";
    std::cout << "9. 火焰图的 X 轴=___(按字母排序的函数)___, Y 轴=___(调用栈深度)___, 宽度=___(CPU时间)___\n";
    std::cout << "10. gdb catch throw 在 ___(异常抛出)___ 时停止, 调用栈完整\n";
    std::cout << "11. watchpoint 用 CPU ___(调试寄存器 DR0-DR3)___ 实现, 最多___(4)___个\n";
    std::cout << "12. rr 的原理是 ___(录制非确定性输入, 无限重放)___\n";
    std::cout << "13. ASan 检测: 影子内存每___(8)___字节 → ___(1)___字节\n";
    std::cout << "14. UBSan 检测 ___(有符号整数溢出/null/对齐/类型转换)___\n";
    std::cout << "15. TSan 检测 ___(data race)___, 开销 ___(5-15)___x\n";
    std::cout << "16. io_uring 两个环形队列: SQ = ___(Submission Queue)___, CQ = ___(Completion Queue)___\n";
    std::cout << "17. io_uring 的核心优势: ___(批量提交/真正异步文件IO/零拷贝)___\n";
    std::cout << "18. Linked operations 用 flag ___(IOSQE_IO_LINK)___ 实现\n\n";

    std::cout << "─── Self-Check Score ───\n";
    std::cout << "能回答 15+/18 → Month 4 掌握扎实 ✅\n";
    std::cout << "能回答 10-14  → 不错, 回顾薄弱环节\n";
    std::cout << "能回答 <10   → 建议重新过一遍对应 Week\n";
  }
}

// ============================================================================
// Ex2: 优化检查表 (Checklist) — 遇到性能问题时的系统化流程
//
// 任务: 理解并记住这个检查表, 下次遇到性能问题逐项排查
// ============================================================================

namespace ex2_checklist {
  void run() {
    std::cout << "\n===== Ex2: 性能优化系统化检查表 =====\n\n";

    std::cout << "═══ Tier 1: 算法与数据结构 (影响最大) ═══\n";
    std::cout << "□ O(n²) → O(n log n)?  (hash map vs linear search)\n";
    std::cout << "□ 不必要的数据拷贝?     (传值 vs const&, string_view)\n";
    std::cout << "□ 内存分配在热路径?     (reserve, object pool, arena)\n\n";

    std::cout << "═══ Tier 2: 测量 (找到真正的瓶颈) ═══\n";
    std::cout << "□ perf stat -d ./prog          → 整体指标\n";
    std::cout << "□ IPC < 1.0?      → memory-bound, 检查 cache\n";
    std::cout << "□ cache-miss > 10%? → 检查数据布局\n";
    std::cout << "□ branch-miss > 5%? → 检查分支预测\n";
    std::cout << "□ perf record -g + report      → 热点函数\n";
    std::cout << "□ 火焰图                        → 可视化调用栈\n\n";

    std::cout << "═══ Tier 3: 数据布局 (Cache 友好) ═══\n";
    std::cout << "□ AoS → SoA?       (只访问部分字段时)\n";
    std::cout << "□ False sharing?    (alignas(64) 隔离热数据)\n";
    std::cout << "□ Loop interchange? (row-major 遍历 column-major 矩阵)\n";
    std::cout << "□ 对齐热数据?       (alignas, destructive_interference_size)\n";
    std::cout << "□ Prefetch?         (随机访问场景更有效)\n\n";

    std::cout << "═══ Tier 4: 分支优化 ═══\n";
    std::cout << "□ 数据可否先排序?   (让分支可预测)\n";
    std::cout << "□ 可否用 branchless? (cmov, bit 运算)\n";
    std::cout << "□ __builtin_expect? (提示编译器常见路径)\n\n";

    std::cout << "═══ Tier 5: 并发与 I/O ═══\n";
    std::cout << "□ 线程数 = CPU 核数? (不过载)\n";
    std::cout << "□ 锁粒度是否过粗?    (细粒度锁 / lock-free)\n";
    std::cout << "□ 文件 I/O → io_uring? (真正异步, 不用线程池)\n";
    std::cout << "□ 大批量 syscall → batch submit? (io_uring)\n\n";

    std::cout << "═══ Tier 6: 工具验证 ═══\n";
    std::cout << "□ ASan clean?     (无内存错误)\n";
    std::cout << "□ UBSan clean?    (无未定义行为)\n";
    std::cout << "□ TSan clean?     (无数据竞争)\n";
    std::cout << "□ Benchmark 稳定? (stddev < 5% mean)\n";
    std::cout << "□ gdb 可调试?     (-g 编译, 符号完整)\n";
  }
}

// ============================================================================
// Ex3: 技术选型决策树
//
// 任务: 针对不同场景选择正确的工具/技术
// ============================================================================

namespace ex3_decision_tree {
  void run() {
    std::cout << "\n===== Ex3: 技术选型决策树 =====\n\n";

    struct Scenario {
      const char* description;
      const char* answer;
    };

    Scenario scenarios[] = {
      {"程序跑得慢, 不知道瓶颈在哪",
       "→ perf stat + perf record → 火焰图 → 定位热点函数"},
      {"IPC < 0.5, cache-misses 很高",
       "→ memory-bound → 检查数据布局 (AoS→SoA, loop interchange)"},
      {"两个线程的简单计数器, 速度比预期慢 3x",
       "→ False sharing → 用 alignas(64) padding 隔离"},
      {"遍历 1000 万元素的 std::list, 极其慢",
       "→ 指针追逐 cache miss → 换成 std::vector (连续内存)"},
      {"程序偶尔崩溃, 但 gdb 下不崩溃",
       "→ heisenbug → 用 rr record/replay 捕捉"},
      {"多线程程序, 结果有时对有时错",
       "→ data race → 用 TSan (-fsanitize=thread) 检测"},
      {"生产环境, 不能加日志, 需要排查问题",
       "→ gdb dprintf / gdb -p attach / perf trace"},
      {"想确定一个 bug 是哪个 commit 引入的",
       "→ git bisect + ASan/UBSan 编译"},
      {"文件 I/O 密集, 线程池 CPU 占用高",
       "→ io_uring 异步 I/O, 消除线程池开销"},
      {"需要同时处理 10K 网络连接 + 文件读写",
       "→ io_uring (网络+文件统一事件循环) / epoll + io_uring 混合"},
    };

    for (auto& s : scenarios) {
      std::cout << "Q: " << s.description << "\n";
      std::cout << "A: " << s.answer << "\n\n";
    }
  }
}

// ============================================================================
// Ex4: 用 Month 4 技术优化 ffind
//
// 概念:
//   ffind 是我们 Month 2 的多线程文件搜索工具。
//   现在用 Month 4 的技术系统性检查和优化它。
//
// 任务:
//   1. 用 perf 测量 ffind 的性能特征
//   2. 提出至少 3 个基于 Month 4 技术的优化方案
//   3. 用 sanitizer 检查 ffind 的代码质量
// ============================================================================

namespace ex4_optimize_ffind {
  void run() {
    std::cout << "\n===== Ex4: 用 Month 4 技术优化 ffind =====\n\n";

    std::string ffind_path = "/home/limsuig/ffind/build/ffind";

    std::cout << "─── Step 1: 性能测量 ───\n";
    std::cout << "# 整体指标:\n";
    std::cout << "$ perf stat -d " << ffind_path << " 'include' /usr/include\n\n";
    std::cout << "# 热点采样:\n";
    std::cout << "$ perf record -g -F 99 " << ffind_path << " 'include' /usr/include\n";
    std::cout << "$ perf report -g --stdio | head -30\n\n";
    std::cout << "# Cache miss 归因:\n";
    std::cout << "$ perf record -e cache-misses -g " << ffind_path << " 'include' /usr/include\n";
    std::cout << "$ perf report --sort=sym --stdio | head -20\n\n";

    std::cout << "─── Step 2: 基于指标的优化方案 ───\n\n";

    std::cout << "A. IPC 低 + cache-misses 高 → memory-bound\n";
    std::cout << "   诊断: 文件内容读取/正则匹配可能有随机访问\n";
    std::cout << "   优化: \n";
    std::cout << "     - mmap() 文件, 让内核管理 page cache (减少 read() 拷贝)\n";
    std::cout << "     - 用 Boyer-Moore 代替 std::regex (BM 是顺序扫描, cache-friendly)\n";
    std::cout << "     - 批量读入 4KB 块, 在缓存中搜索\n\n";

    std::cout << "B. branch-misses 高\n";
    std::cout << "   诊断: 正则引擎回溯/字符分类分支多\n";
    std::cout << "   优化:\n";
    std::cout << "     - 用 RE2/Hyperscan 代替 std::regex (无回溯, 预测性好)\n";
    std::cout << "     - 字符分类用 lookup table 代替 if-else 链\n\n";

    std::cout << "C. context-switches 多\n";
    std::cout << "   诊断: 线程数 > CPU 核数\n";
    std::cout << "   优化:\n";
    std::cout << "     - 限制线程数为 std::thread::hardware_concurrency() - 1\n";
    std::cout << "     - 用 thread pool 代替 per-directory thread\n";
    std::cout << "     - 或: 用 io_uring 代替线程遍历 (单线程异步扫描)\n\n";

    std::cout << "D. 大量 stat/open/read syscalls\n";
    std::cout << "   诊断: 每个文件都单独 open+read+close\n";
    std::cout << "   优化:\n";
    std::cout << "     - batch stat: 用 getdents64 一次性读目录+stat\n";
    std::cout << "     - 用 io_uring 批量提交 open+read+close, 消除 syscall 开销\n\n";

    std::cout << "─── Step 3: Sanitizer 检查 ───\n";
    std::cout << "$ cd /home/limsuig/ffind\n";
    std::cout << "$ cmake -B build-asan -DCMAKE_CXX_FLAGS='-fsanitize=address -g -O1'\n";
    std::cout << "$ cmake --build build-asan\n";
    std::cout << "$ ./build-asan/ffind 'test' /tmp\n";
    std::cout << "# 同样用 -fsanitize=undefined 和 -fsanitize=thread 检查\n";
  }
}

// ============================================================================
// Ex5: 实战优化 Benchmark — 对比「优化前 vs 优化后」
//
// 概念:
//   写一个可测量的基准测试, 对比三种排序策略:
//     A. 不排序, 直接处理
//     B. 排序后处理 (branch prediction 友好)
//     C. 不排序 + branchless 处理
//
// 任务: 完成 benchmark, 验证 Ex5 (Branch Prediction) 的结论
//        在更大的数据集上是否仍然成立
// ============================================================================

namespace ex5_benchmark_comparison {
  // 模拟 ffind 的核心操作: 判断字符是否是「单词边界」
  bool is_word_boundary_branchy(char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '.' || c == ',' || c == ';' || c == ':' ||
        c == '(' || c == ')' || c == '{' || c == '}' ||
        c == '[' || c == ']') return true;
    return false;
  }

  // branchless 版本: lookup table
  // C++20 constexpr 编译期构建 LUT
  static constexpr auto make_lut = []() {
    std::array<bool, 256> t{};
    for (unsigned char ch : {' ', '\t', '\n', '\r', '.', ',',
                              ';', ':', '(', ')', '{', '}', '[', ']'}) {
      t[ch] = true;
    }
    return t;
  };
  static constexpr auto lut = make_lut();

  bool is_word_boundary_lut(char c) {
    return lut[static_cast<unsigned char>(c)];
  }

  void run() {
    std::cout << "\n===== Ex5: Benchmark 实战 =====\n";

    // 生成测试数据
    constexpr int N = 10'000'000;
    std::vector<char> data(N);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(32, 126);  // printable ASCII
    for (int i = 0; i < N; ++i) data[i] = (char)dist(rng);

    std::cout << "Testing on " << N / 1'000'000 << "M characters\n\n";

    // Test 1: Unsorted (random characters, unpredictable branches)
    {
      Timer t;
      int count = 0;
      for (int i = 0; i < N; ++i)
        if (is_word_boundary_branchy(data[i])) count++;
      g_sink = count;
      std::cout << "Unsorted + branchy:  " << t.elapsed_ms() << " ms\n";
    }

    // Test 2: Sorted (all boundary chars grouped → predictable)
    {
      std::sort(data.begin(), data.end());
      Timer t;
      int count = 0;
      for (int i = 0; i < N; ++i)
        if (is_word_boundary_branchy(data[i])) count++;
      g_sink = count;
      std::cout << "Sorted + branchy:    " << t.elapsed_ms() << " ms\n";
    }

    // Test 3: LUT (branchless)
    {
      // reshuffle data
      std::shuffle(data.begin(), data.end(), rng);
      Timer t;
      int count = 0;
      for (int i = 0; i < N; ++i)
        if (is_word_boundary_lut(data[i])) count++;
      g_sink = count;
      std::cout << "Shuffled + LUT:      " << t.elapsed_ms() << " ms\n";
    }

    std::cout << "\n结论: 排序让分支可预测, LUT 完全消除分支\n";
    std::cout << "      ffind 的字符分类适合用 LUT 优化\n";
  }
}

// ============================================================================
// Part B: Capstone — 高性能异步日志服务器 (Ex6-10)
// ============================================================================
//
// 设计目标:
//   - 接受 TCP 连接, 接收日志行 (以 \n 结尾)
//   - 写入 ring buffer (SPSC, lock-free, cache-line padded)
//   - 后台线程从 ring buffer 消费, 批量写入磁盘文件
//   - 使用 io_uring 做异步磁盘写入
//
// ============================================================================

// ============================================================================
// Ex6: SPSC Ring Buffer — cache-friendly 无锁队列
//
// 概念:
//   单生产者单消费者 (SPSC) ring buffer 是最快的无锁队列:
//   - 不需要 mutex (每个方向只有一个线程)
//   - 用 atomic + memory order 保证正确性
//   - cache-line padding 消除 false sharing
//
// 任务: 完成 SPSC ring buffer 的实现
// ============================================================================

namespace ex6_spsc_ringbuffer {
  static constexpr size_t CACHE_LINE = 64;

  template<typename T, size_t Capacity>
  class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    // 用 cache line padding 隔离 producer 和 consumer 的索引
    alignas(CACHE_LINE) std::atomic<size_t> _write_idx{0};
    alignas(CACHE_LINE) std::atomic<size_t> _read_idx{0};
    // 数据区
    T _buffer[Capacity];

  public:
    // Producer: 尝试 push (不阻塞)
    bool try_push(const T& item) {
      size_t write = _write_idx.load(std::memory_order_relaxed);
      size_t next = write + 1;
      size_t read = _read_idx.load(std::memory_order_acquire);
      if (next - read > Capacity) return false;  // full
      _buffer[write & (Capacity - 1)] = item;
      _write_idx.store(next, std::memory_order_release);
      return true;
    }

    // Consumer: 尝试 pop (不阻塞)
    bool try_pop(T& item) {
      size_t read = _read_idx.load(std::memory_order_relaxed);
      size_t write = _write_idx.load(std::memory_order_acquire);
      if (read == write) return false;  // empty
      item = _buffer[read & (Capacity - 1)];
      _read_idx.store(read + 1, std::memory_order_release);
      return true;
    }

    size_t size() const {
      size_t w = _write_idx.load(std::memory_order_acquire);
      size_t r = _read_idx.load(std::memory_order_acquire);
      return w - r;
    }

    bool empty() const { return size() == 0; }
  };

  void run() {
    std::cout << "\n===== Ex6: SPSC Ring Buffer =====\n";

    SPSCRingBuffer<int, 1024> rb;
    std::cout << "Ring buffer capacity: 1024, cache-line padded ("
              << CACHE_LINE << " bytes)\n\n";

    // Producer 线程
    std::atomic<bool> done{false};
    std::thread producer([&] {
      for (int i = 0; i < 1'000'000; ++i) {
        while (!rb.try_push(i)) {
          std::this_thread::yield();  // backpressure
        }
      }
      done = true;
    });

    // Consumer 线程
    std::thread consumer([&] {
      int item;
      int64_t sum = 0;
      int count = 0;
      while (!done || !rb.empty()) {
        if (rb.try_pop(item)) {
          sum += item;
          count++;
        }
      }
      std::cout << "Consumer: " << count << " items, sum=" << sum << "\n";
    });

    Timer t;
    producer.join();
    consumer.join();
    std::cout << "1M push/pop in " << t.elapsed_ms() << " ms\n";
    std::cout << "≈ " << (1'000'000.0 / t.elapsed_us()) << " ops/us\n";
    std::cout << "≈ " << (1'000'000.0 / t.elapsed_us() * 1'000'000) / 1'000'000
              << "M ops/sec\n";
    std::cout << "\nKey: No mutex, cache-line padded → minimal contention\n";
  }
}

// ============================================================================
// Ex7: Log Entry 设计与格式化
//
// 概念:
//   日志条目的内存布局影响 cache 效率。
//   定长条目 = 更快, 变长条目 = 更灵活。
//
// 任务: 设计 LogEntry 结构, 实现高效的格式化
// ============================================================================

namespace ex7_log_entry {
  // 定长日志条目 (64 字节 — 正好一个 cache line)
  struct alignas(64) LogEntry {
    int64_t timestamp_us;          // 8 bytes
    int32_t level;                 // 4 bytes (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
    int32_t pid;                   // 4 bytes
    char message[48];              // 48 bytes (null-terminated)

    LogEntry() {
      std::memset(this, 0, sizeof(*this));
    }

    void format(int32_t lvl, const char* msg) {
      timestamp_us = duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()
      ).count();
      level = lvl;
      pid = getpid();
      std::strncpy(message, msg, sizeof(message) - 1);
      message[sizeof(message) - 1] = '\0';
    }

    // 序列化为文本行 (用于写入文件)
    int to_string(char* buf, size_t bufsize) const {
      static const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
      const char* lname = (level >= 0 && level < 4) ? level_names[level] : "UNKNOWN";
      return snprintf(buf, bufsize, "[%lld] %s %d %s\n",
                      (long long)timestamp_us, lname, pid, message);
    }
  };

  void run() {
    std::cout << "\n===== Ex7: Log Entry Design =====\n";
    std::cout << "sizeof(LogEntry) = " << sizeof(LogEntry)
              << " bytes (1 cache line)\n";

    LogEntry entry;
    entry.format(2, "Connection timeout after 30 seconds");

    char buf[128];
    entry.to_string(buf, sizeof(buf));
    std::cout << "Formatted: " << buf;

    std::cout << "\nDesign choices:\n";
    std::cout << "  - Fixed 64B = 1 cache line → aligned memory access\n";
    std::cout << "  - message[48] is small → fast copy, ok for log lines\n";
    std::cout << "  - For larger messages: use external buffer pool\n";
    std::cout << "  - Compile-time check: static_assert(sizeof(LogEntry) == 64)\n";
  }
}

// ============================================================================
// Ex8: Disk Writer — 异步批量落盘 (概念实现)
//
// 概念:
//   Disk writer 从 ring buffer 消费 LogEntry,
//   批量写入文件 (批量 = 减少 write syscall 次数)。
//   使用 io_uring 做异步写入 (用普通 write 做概念演示)。
//
// 任务: 实现 disk writer 的核心逻辑
// ============================================================================

namespace ex8_disk_writer {
  class DiskWriter {
    int _fd = -1;
    std::string _filepath;
    int64_t _total_written = 0;
    int64_t _total_syncs = 0;

  public:
    bool open(const std::string& path) {
      _filepath = path;
      _fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      return _fd >= 0;
    }

    // 批量写入 (积累 BATCH_SIZE 条后一次性 write)
    int batch_write(const std::string& data) {
      if (_fd < 0) return -1;
      ssize_t n = ::write(_fd, data.data(), data.size());
      if (n > 0) _total_written += n;
      return n;
    }

    void sync() {
      if (_fd >= 0) {
        ::fsync(_fd);
        _total_syncs++;
      }
    }

    void close() {
      if (_fd >= 0) {
        sync();
        ::close(_fd);
        _fd = -1;
      }
    }

    int64_t total_written() const { return _total_written; }

    // io_uring 异步写入 (概念)
    // prep_write(sqe, _fd, data, len, offset, user_data);
    // 然后由事件循环收割 CQE
  };

  void run() {
    std::cout << "\n===== Ex8: Disk Writer =====\n";

    DiskWriter writer;
    writer.open("/tmp/capstone_test.log");

    // 模拟批量写入
    constexpr int LINES = 100'000;
    constexpr int BATCH = 1000;

    std::string batch_buffer;
    batch_buffer.reserve(BATCH * 128);

    Timer t;
    for (int i = 0; i < LINES; ++i) {
      char buf[128];
      int len = snprintf(buf, sizeof(buf),
                         "[%d] INFO %d Log message number %d\n",
                         i * 1000, getpid(), i);
      batch_buffer.append(buf, len);

      if (batch_buffer.size() >= BATCH * 100 || i == LINES - 1) {
        writer.batch_write(batch_buffer);
        batch_buffer.clear();
      }
    }
    writer.close();

    std::cout << LINES / 1000 << "K lines written in " << t.elapsed_ms() << " ms\n";
    std::cout << "Total: " << writer.total_written() / 1024.0 << " KB\n";
    std::cout << "Batch size: " << BATCH << " lines → ~"
              << LINES / BATCH << " write() calls (vs " << LINES
              << " without batching)\n";
    std::cout << "\nWith io_uring: async writes → 零 CPU 等待磁盘\n";

    unlink("/tmp/capstone_test.log");
  }
}

// ============================================================================
// Ex9: 集成 — 完整异步日志服务器
//
// 概念:
//   组装所有组件:
//   1. TCP listener (accept with io_uring)
//   2. Log entry parser (read from socket, 以 \n 分隔)
//   3. SPSC ring buffer (push parsed LogEntry)
//   4. Disk writer (pop from ring, batch write to file)
//   5. Periodic fsync (每 3 秒)
//
// 任务: 理解集成架构, 测试端到端吞吐
// ============================================================================

namespace ex9_integration {
  static constexpr int PORT = 22000;

  void run() {
    std::cout << "\n===== Ex9: Full Integration — Async Log Server =====\n\n";

    std::cout << "═══ Architecture ═══\n";
    std::cout << "                                     \n";
    std::cout << "  [Client 1] ──┐                     \n";
    std::cout << "  [Client 2] ──┤                     \n";
    std::cout << "  [Client N] ──┘                     \n";
    std::cout << "       │ TCP                         \n";
    std::cout << "       ▼                             \n";
    std::cout << "  ┌──────────────┐                   \n";
    std::cout << "  │ io_uring     │ ← 1 event loop   \n";
    std::cout << "  │ accept/read  │   (no threads)    \n";
    std::cout << "  └──────┬───────┘                   \n";
    std::cout << "         │ parsed LogEntry           \n";
    std::cout << "         ▼                           \n";
    std::cout << "  ┌──────────────┐                   \n";
    std::cout << "  │ SPSC Ring    │ ← lock-free       \n";
    std::cout << "  │ Buffer (1M)  │   cache padded    \n";
    std::cout << "  └──────┬───────┘                   \n";
    std::cout << "         │ batch                     \n";
    std::cout << "         ▼                           \n";
    std::cout << "  ┌──────────────┐                   \n";
    std::cout << "  │ io_uring     │ ← async disk IO  \n";
    std::cout << "  │ write/fsync  │   batch writes    \n";
    std::cout << "  └──────┬───────┘                   \n";
    std::cout << "         ▼                           \n";
    std::cout << "  [  Disk  ]                         \n\n";

    std::cout << "═══ Performance Characteristics ═══\n";
    std::cout << "Single-thread, event-driven (no thread pool)\n";
    std::cout << "Expected throughput: ~500K log lines/sec\n";
    std::cout << "Bottleneck: disk write bandwidth (mitigated by batching)\n";
    std::cout << "Memory: ring buffer (64MB for 1M entries @ 64B each)\n\n";

    std::cout << "═══ Running demo server (3 seconds) ═══\n";
    std::cout << "Server would start on port " << PORT << "\n";
    std::cout << "Test: for i in {1..10000}; do echo \"log $i\" | nc localhost " << PORT << "; done\n";
    std::cout << "Or:   echo 'ERROR: disk full' | nc localhost " << PORT << "\n\n";

    // 简化版: 不依赖 io_uring, 用标准 socket + thread 演示概念
    std::cout << "Simplified demo (no io_uring, pure concept):\n";

    // 模拟整个流程的吞吐测试
    int total_lines = 0;
    std::atomic<bool> running{true};

    // "网络"线程: 生成日志
    std::thread network([&] {
      for (int i = 0; i < 100'000 && running; ++i) {
        // 模拟 receive from socket → parse → push to ring
        total_lines++;
      }
      running = false;
    });

    // "磁盘"线程: 消费
    int written = 0;
    std::thread disk([&] {
      while (running || total_lines > written) {
        written = total_lines;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });

    Timer t;
    network.join();
    disk.join();
    std::cout << total_lines / 1000.0 << "K lines in " << t.elapsed_ms() << " ms\n";
    std::cout << "≈ " << int(total_lines / (t.elapsed_us() / 1'000'000.0) / 1000)
              << "K lines/sec (conceptual, single thread)\n";
  }
}

// ============================================================================
// Ex10: 最终反思 — Month 4 学习成果 & 下一步
//
// 任务: 完成反思, 规划 Month 5
// ============================================================================

namespace ex10_reflection {
  void run() {
    std::cout << "\n===== Ex10: Month 4 最终反思 =====\n\n";

    std::cout << "═══ 你学到了什么 ═══\n\n";
    std::cout << "Week 17 — CPU Cache:\n";
    std::cout << "  ✅ 理解内存层次 (L1/L2/L3/RAM)\n";
    std::cout << "  ✅ 写出 cache-friendly 代码 (AoS/SoA, padding, loop interchange)\n";
    std::cout << "  ✅ 识别并修复 false sharing (2x speedup)\n";
    std::cout << "  ✅ 使用 branchless 技术消除分支预测失败\n";
    std::cout << "  ✅ 理解 ECS/DOD vs OOP 的性能差异 (2x)\n\n";

    std::cout << "Week 18 — perf:\n";
    std::cout << "  ✅ 读懂 perf stat 的硬件计数器\n";
    std::cout << "  ✅ IPC < 1.0 = memory-bound, > 2.0 = compute-bound\n";
    std::cout << "  ✅ 用 perf record + report + 火焰图定位热点\n";
    std::cout << "  ✅ 建立 profile → hotspot → optimize → verify 流程\n";
    std::cout << "  ✅ 编写稳定的 micro-benchmark (预留+多轮+统计)\n\n";

    std::cout << "Week 19 — gdb:\n";
    std::cout << "  ✅ 条件断点 (b func if x < 0) — 只在bug触发时停\n";
    std::cout << "  ✅ watchpoint 捕获变量被谁修改\n";
    std::cout << "  ✅ catch throw 在异常抛出时拿到完整调用栈\n";
    std::cout << "  ✅ core dump 分析崩溃现场\n";
    std::cout << "  ✅ rr 录制+重放+反向调试 — heisenbug 杀手\n";
    std::cout << "  ✅ gdb Python 自定义命令 (warden, deadlock-check)\n\n";

    std::cout << "Week 20 — Sanitizers:\n";
    std::cout << "  ✅ ASan 检测 heap/stack/global overflow + use-after-free\n";
    std::cout << "  ✅ UBSan 检测整数溢出/null/对齐/类型转换\n";
    std::cout << "  ✅ TSan 检测数据竞争\n";
    std::cout << "  ✅ MSan/LSan 检测未初始化内存和泄漏\n";
    std::cout << "  ✅ CI 集成 sanitizer 是最佳实践\n\n";

    std::cout << "Week 21 — io_uring:\n";
    std::cout << "  ✅ 理解双环形队列 (SQ+CQ) 架构\n";
    std::cout << "  ✅ 用 raw syscall 实现 io_uring wrapper (~170行)\n";
    std::cout << "  ✅ 批量提交 (N 个 IO → 1 个 syscall)\n";
    std::cout << "  ✅ 统一文件+网络事件循环\n";
    std::cout << "  ✅ 理解 fixed files/buffers, linked ops, multi-shot\n\n";

    std::cout << "Week 22 — Capstone:\n";
    std::cout << "  ✅ SPSC ring buffer (lock-free, cache padded)\n";
    std::cout << "  ✅ Log entry 设计 (64B cache line aligned)\n";
    std::cout << "  ✅ 批量写入 (batch write)\n";
    std::cout << "  ✅ 整合所有技术到完整系统\n\n";

    std::cout << "═══ 能力宣言 ═══\n";
    std::cout << "Month 1: 「我不再写 C with Class — 我用 RAII/SmartPtr/Templates/Lambda」\n";
    std::cout << "Month 2: 「我理解程序与 OS 的边界 — File/Process/Signal/Thread」\n";
    std::cout << "Month 3: 「我可以用 C++ 构建任何基于 TCP 的网络服务」\n";
    std::cout << "Month 4: 「我理解硬件 — 让 C++ 代码在 CPU/内存/磁盘上跑得最快」\n";
    std::cout << "         「我拥有完整的工具箱: 测量→分析→优化→调试→交付」\n\n";

    std::cout << "═══ 下一步: Month 5 — 源码阅读 ═══\n";
    std::cout << "Week 23: STL 实现深潜 (std::vector/string/function 内部)\n";
    std::cout << "Week 24: leveldb 源码阅读 (LSM-tree, Skiplist, Bloom filter)\n";
    std::cout << "Week 25: fmtlib 源码阅读 (编译期格式字符串, 极致性能)\n";
    std::cout << "Week 26: libevent 源码阅读 (事件驱动库的内部)\n";
    std::cout << "Week 27: 小型 STL 实现 (自己写 vector/string/smart_ptr)\n";
    std::cout << "Week 28: Month 5 收官 — 源码阅读心得 & 小型项目\n\n";

    std::cout << "═══ Month 4 统计数据 ═══\n";
    std::cout << "6 周, 60 个练习\n";
    std::cout << "~7000 行 scaffold 代码\n";
    std::cout << "5 个实战项目: Cache Bench, Micro-Bench, Bug Hunt, Echo Server, Log Server\n";
    std::cout << "掌握工具: perf, gdb, rr, ASan, UBSan, TSan, MSan, LSan, io_uring\n";
    std::cout << "核心能力: 测量驱动的性能优化 + 系统化调试\n";
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  int ex_num = 99;
  if (argc > 1) ex_num = std::atoi(argv[1]);

  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Month 4 / Week 22: Capstone — 收官\n";
  std::cout << "══════════════════════════════════════════════\n";

  switch (ex_num) {
    case 1:  ex1_review::run(); break;
    case 2:  ex2_checklist::run(); break;
    case 3:  ex3_decision_tree::run(); break;
    case 4:  ex4_optimize_ffind::run(); break;
    case 5:  ex5_benchmark_comparison::run(); break;
    case 6:  ex6_spsc_ringbuffer::run(); break;
    case 7:  ex7_log_entry::run(); break;
    case 8:  ex8_disk_writer::run(); break;
    case 9:  ex9_integration::run(); break;
    case 10: ex10_reflection::run(); break;
    default:
      ex1_review::run();
      ex2_checklist::run();
      ex3_decision_tree::run();
      ex4_optimize_ffind::run();
      ex5_benchmark_comparison::run();
      ex6_spsc_ringbuffer::run();
      ex7_log_entry::run();
      ex8_disk_writer::run();
      ex9_integration::run();
      ex10_reflection::run();
  }

  std::cout << "\n══════════════════════════════════════════════\n";
  std::cout << "Month 4 完成! 🎉🎉🎉\n";
  std::cout << "══════════════════════════════════════════════\n";
  return 0;
}
