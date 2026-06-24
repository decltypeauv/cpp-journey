// ============================================================================
// Month 4: 极致性能 — Beyond the Code
// Week 17: CPU Cache & Memory Hierarchy
//
// 核心问题意识:
//   「CPU 大部分时间不是在计算，而是在等待数据」
//   1990年后 CPU 速度增长远超内存速度 (Memory Wall)
//   L1 cache ~0.5ns, L2 ~7ns, L3 ~30ns, RAM ~100ns
//   一次主存访问的时间 ≈ 执行 200-400 条指令
//
// 本周目标:
//   - 理解 CPU cache 层次结构与工作原理
//   - 学会编写 cache-friendly 代码
//   - 识别和修复 false sharing / cache miss 问题
//   - 掌握数据局部性 (spatial + temporal locality)
//
// 10 个练习, 由浅入深
// ============================================================================

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

// ============================================================================
// Timer 工具
// ============================================================================

class Timer {
  high_resolution_clock::time_point _start;
public:
  Timer() : _start(high_resolution_clock::now()) {}
  int64_t elapsed_ns() const {
    return duration_cast<nanoseconds>(
      high_resolution_clock::now() - _start
    ).count();
  }
  double elapsed_us() const { return elapsed_ns() / 1000.0; }
  double elapsed_ms() const { return elapsed_ns() / 1'000'000.0; }
};

// ============================================================================
// Ex1: 测量 CPU Cache 层次的速度差异
//
// 概念:
//   CPU 访问不同大小的内存块, 速度差异巨大:
//   - L1 (32KB per core)  → 4 cycles  ≈ 1ns
//   - L2 (256KB-1MB)      → 12 cycles ≈ 4ns
//   - L3 (8-32MB shared)  → 40 cycles ≈ 13ns
//   - RAM                  → 300 cycles ≈ 100ns
//
// 实验: 遍历不同大小的数组, 观察随着数组增大
//        从 L1 → L2 → L3 → RAM 的性能跳变。
//        每个元素只读一次, 排除重用效应。
//
// 任务: 补全代码, 然后用不同 SIZE 运行, 找出你 CPU 的 cache 边界。
//       提示: 用 `getconf -a | grep CACHE` 查看 CPU cache 大小
// ============================================================================

namespace ex1_cache_hierarchy {
  // 步长: 每 N 个元素读一个, 保证访问跨越足够距离
  constexpr int STRIDE = 16;  // 16 × 8 bytes = 128 bytes between accesses

  void measure(const char* label, size_t bytes) {
    // 分配 bytes 大小的 int64_t 数组
    size_t count = bytes / sizeof(int64_t);
    std::vector<int64_t> data(count, 0);

    // 测量: 顺序读取但跳步 STRIDE
    volatile int64_t sink = 0;
    Timer t;
    for (size_t i = 0; i < count; i += STRIDE) {
      sink += data[i];  // volatile 防止优化掉
    }
    int64_t elapsed = t.elapsed_ns();
    size_t accesses = count / STRIDE;
    std::cout << std::setw(20) << label
              << " | size=" << std::setw(8) << (bytes / 1024) << "KB"
              << " | accesses=" << accesses
              << " | total=" << elapsed / 1000 << "us"
              << " | per-access=" << elapsed / (int64_t)accesses << "ns"
              << "\n";
  }

  void run() {
    std::cout << "\n===== Ex1: Cache Hierarchy Speed Test =====\n";
    std::cout << "Stride = " << STRIDE << " × 8 bytes = "
              << STRIDE * 8 << " bytes between accesses\n\n";

    // 从 8KB → 64MB, 跨越 L1/L2/L3/RAM
    for (size_t kb : {8, 16, 32, 64, 128, 256, 512,
                       1024, 2048, 4096, 8192, 16384, 32768, 65536}) {
      size_t bytes = kb * 1024;
      char label[32];
      snprintf(label, sizeof(label), "%zuKB", kb);
      measure(label, bytes);
    }
  }
}

// ============================================================================
// Ex2: Cache Line 与 False Sharing
//
// 概念:
//   Cache line 是 CPU 缓存的最小单位 (通常 64 字节)。
//   当一个 core 写入它的 cache line 时, 整条 cache line 被标记为 dirty,
//   其他 core 的同一 cache line 副本全部失效 (invalidate)。
//
//   False Sharing: 两个线程修改「不同」变量, 但这两个变量恰好
//   在同一个 cache line 内 → 竞争的不是数据而是 cache line,
//   导致性能灾难性退化。
//
// 实验:
//   - bad:  两个 int64_t 紧挨着 → 在同一 cache line → 互相 invalidate
//   - good: 每个 int64_t 后有 padding → 各自独占 cache line → 无干扰
//
// 任务: 对比 bad 和 good 两个版本, 理解 padding 的作用。
//       计算理论加速比, 然后实际测量。
// ============================================================================

namespace ex2_false_sharing {
  // --- Bad: 两个计数器在同一 cache line ---
  struct BadCounters {
    int64_t counter1 = 0;  // 共享同一 cache line
    int64_t counter2 = 0;  // ← counter1 和 counter2 紧挨着!
  };

  // --- Good: padding 隔离到各自的 cache line ---
  struct GoodCounters {
    alignas(64) int64_t counter1 = 0;
    alignas(64) int64_t counter2 = 0;  // ← 每个独占自己的 cache line
  };

  template<typename Counters>
  int64_t test(const char* label) {
    Counters c;
    constexpr int ITERATIONS = 100'000'000;  // 1e8

    // 两个线程同时疯狂自增各自的计数器
    std::barrier ready(2);
    std::jthread t1([&] {
      ready.arrive_and_wait();
      for (int i = 0; i < ITERATIONS; ++i) {
        c.counter1++;
      }
    });
    ready.arrive_and_wait();
    for (int i = 0; i < ITERATIONS; ++i) {
      c.counter2++;
    }
    t1.join();

    Timer t;
    // 再跑一轮计时 (已预热)
    std::jthread t1b([&] {
      ready.arrive_and_wait();
      for (int i = 0; i < ITERATIONS; ++i) {
        c.counter1++;
      }
    });
    ready.arrive_and_wait();
    for (int i = 0; i < ITERATIONS; ++i) {
      c.counter2++;
    }
    t1b.join();
    int64_t elapsed = t.elapsed_ms();

    std::cout << std::setw(20) << label
              << " | " << elapsed << " ms"
              << " | sum=" << (c.counter1 + c.counter2)
              << "\n";
    return elapsed;
  }

  void run() {
    std::cout << "\n===== Ex2: False Sharing =====\n";
    std::cout << "Cache line size: " << std::hardware_destructive_interference_size
              << " bytes\n\n";

    int64_t bad_time  = test<BadCounters>("  BAD (same line)");
    int64_t good_time = test<GoodCounters>("GOOD (padded)");

    if (bad_time > 0 && good_time > 0) {
      std::cout << "\nSpeedup: " << (double)bad_time / good_time
                << "x faster with padding\n";
      std::cout << "Why: Two variables on same cache line → each write\n"
                << "     invalidates the other core's cache line → constant\n"
                << "     cache-coherency traffic. Padding isolates them.\n";
    }
  }
}

// ============================================================================
// Ex3: AoS vs SoA (Array of Structs vs Struct of Arrays)
//
// 概念:
//   AoS: struct Vertex { float x,y,z; }; Vertex verts[N];
//   SoA: struct Vertices { float x[N], y[N], z[N]; };
//
//   AoS 适合「访问一个对象的多个字段」的场景 (单 Vertex 的 xyz 在一起)
//   SoA 适合「对所有对象的同一字段做 SIMD/向量化」的场景
//
//   当遍历只访问一个字段时 (如只读 x), AoS 浪费 2/3 的 cache line
//   (y 和 z 也被拖进缓存但不使用)
//
// 任务: 完成两种数据布局, 测量:
//       1. 读取所有 x 坐标 (SoA 应该更快)
//       2. 读取单个顶点的 xyz (AoS 应该更快)
// ============================================================================

namespace ex3_aos_vs_soa {
  constexpr int N = 1'000'000;

  // --- AoS 布局 ---
  struct Vertex {
    float x, y, z;
  };

  // --- SoA 布局 ---
  struct VertexList {
    std::vector<float> x;
    std::vector<float> y;
    std::vector<float> z;
    VertexList(size_t n) : x(n), y(n), z(n) {}
  };

  // TODO: task
  //   1. 创建 AoS 数组: std::vector<Vertex> aos(N);
  //   2. 创建 SoA 结构: VertexList soa(N);
  //   3. 测量"只访问 x"的遍历速度 (读所有 x 求和)
  //   4. 测量"访问 xyz"的遍历速度 (读每个顶点的 xyz 求范数)
  //   5. 解释结果

  void run() {
    std::cout << "\n===== Ex3: AoS vs SoA =====\n";
    std::cout << "Array size: " << N << " elements\n";
    std::cout << "Memory: AoS = " << (N * sizeof(Vertex) / 1024.0 / 1024.0)
              << " MB, SoA = " << (3 * N * sizeof(float) / 1024.0 / 1024.0) << " MB\n\n";

    // 初始化数据
    std::vector<Vertex> aos(N);
    VertexList soa(N);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    for (int i = 0; i < N; ++i) {
      float x = dist(rng), y = dist(rng), z = dist(rng);
      aos[i] = {x, y, z};
      soa.x[i] = x; soa.y[i] = y; soa.z[i] = z;
    }

    constexpr int ITER = 1000;
    volatile float sink = 0;
    Timer t;

    // --- 测试 1: 只访问 x (预期 SoA 胜) ---
    // AoS: 遍历 struct 数组只读 x → y,z 也被拖入 cache (浪费 2/3 带宽)
    sink = 0;
    t = Timer();
    for (int iter = 0; iter < ITER; ++iter) {
      for (int i = 0; i < N; ++i) {
        sink += aos[i].x;
      }
    }
    std::cout << "X-only AoS: " << t.elapsed_us() << " us\n";

    // SoA: 遍历 x 数组 → 全部数据是有效负载
    sink = 0;
    t = Timer();
    for (int iter = 0; iter < ITER; ++iter) {
      for (int i = 0; i < N; ++i) {
        sink += soa.x[i];
      }
    }
    std::cout << "X-only SoA: " << t.elapsed_us() << " us\n";

    // --- 测试 2: 访问完整 xyz (预期 AoS 胜) ---
    sink = 0;
    t = Timer();
    for (int iter = 0; iter < ITER; ++iter) {
      for (int i = 0; i < N; ++i) {
        sink += aos[i].x + aos[i].y + aos[i].z;
      }
    }
    std::cout << "XYZ AoS:    " << t.elapsed_us() << " us\n";

    sink = 0;
    t = Timer();
    for (int iter = 0; iter < ITER; ++iter) {
      for (int i = 0; i < N; ++i) {
        sink += soa.x[i] + soa.y[i] + soa.z[i];
      }
    }
    std::cout << "XYZ SoA:    " << t.elapsed_us() << " us\n";

    std::cout << "\nInsight: AoS wins when accessing all fields together\n"
              << "         SoA wins when accessing only a subset of fields\n";
  }
}

// ============================================================================
// Ex4: Loop Interchange — Row-Major vs Column-Major
//
// 概念:
//   C++ 多维数组是 Row-Major 存储: matrix[row][col],
//   同一行的元素在内存中连续。
//
//   坏: for (col) for (row) → 每次访问跳过一整行, cache miss 率极高
//   好: for (row) for (col) → 顺序访问, 完美利用 cache line
//
// 任务: 测量两种遍历顺序在 4096×4096 矩阵上的性能差异
//       预期: column-major 遍历慢 10-50 倍
// ============================================================================

namespace ex4_loop_interchange {
  constexpr int SIZE = 4096;

  void run() {
    std::cout << "\n===== Ex4: Loop Interchange =====\n";

    // 分配 SIZE × SIZE 的矩阵 (用 1D 模拟 2D)
    std::vector<int> matrix(SIZE * SIZE, 1);

    // 任务 1: Row-Major 遍历 (先 row 再 col)
    //    for (int r = 0; r < SIZE; r++)
    //      for (int c = 0; c < SIZE; c++)
    //        sum += matrix[r * SIZE + c];
    //    → 顺序访问, cache-friendly

    // 任务 2: Column-Major 遍历 (先 col 再 row)
    //    for (int c = 0; c < SIZE; c++)
    //      for (int r = 0; r < SIZE; r++)
    //        sum += matrix[r * SIZE + c];
    //    → 每次跳过 SIZE 个元素, cache-unfriendly

    // 任务 3: 在更小尺寸上验证 (256, 512, 1024, 2048, 8192)
    //   看什么时候 Row-Major 优势开始显著下降 (L3 装不下)

    volatile int64_t sink = 0;

    {
      Timer t;
      for (int r = 0; r < SIZE; r++)
        for (int c = 0; c < SIZE; c++)
          sink += matrix[r * SIZE + c];
      std::cout << "Row-Major:  " << t.elapsed_ms() << " ms\n";
    }

    {
      Timer t;
      for (int c = 0; c < SIZE; c++)
        for (int r = 0; r < SIZE; r++)
          sink += matrix[r * SIZE + c];
      std::cout << "Col-Major:  " << t.elapsed_ms() << " ms\n";
    }
  }
}

// ============================================================================
// Ex5: Branch Prediction
//
// 概念:
//   CPU 用分支预测器 (Branch Predictor) 猜测 if/for 的走向,
//   预测对了流水线顺畅, 预测错了 flush 流水线 → 惩罚 ~20 cycles。
//
//   关键: 现代 CPU 的分支预测器极其聪明, 但有弱点:
//   - 对「规律性分支」预测准确率接近 100%
//   - 对「随机分支」预测准确率 ≈ 50%
//
// 实验: 对比「已排序数据」→ 规律分支 vs「随机数据」→ 随机分支
//       在相同计算量下的性能差异。
//
// 任务: 补全代码, 对比 sorted vs unsorted 的 branchy 版本
//       额外: 尝试 branchless 版本 (用 bit 运算消除分支)
// ============================================================================

namespace ex5_branch_prediction {
  constexpr int SIZE = 100'000;  // 足够大让分支预测器"学习"

  void run() {
    std::cout << "\n===== Ex5: Branch Prediction =====\n";

    // 生成随机数组
    std::vector<int> data(SIZE);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    for (int& v : data) v = dist(rng);

    int64_t sum;
    Timer t;

    // --- 版本 1: 未排序 + 分支 ---
    //   遍历 data, 对 data[i] >= 128 的元素求和
    //   每次 data[i] >= 128 是随机的 → 分支预测 miss 率高
    sum = 0;
    t = Timer();
    for (int i = 0; i < SIZE * 1000; ++i) {
      if (data[i % SIZE] >= 128) {
        sum += data[i % SIZE];
      }
    }
    std::cout << "Unsorted + Branch:  " << t.elapsed_ms() << " ms (sum=" << sum << ")\n";

    // --- 版本 2: 排序后 + 分支 ---
    //   先排序 data, 再跑同样逻辑
    //   data[i] >= 128 变成: 前半全是 false, 后半全是 true → 预测 100%
    std::sort(data.begin(), data.end());
    sum = 0;
    t = Timer();
    for (int i = 0; i < SIZE * 1000; ++i) {
      if (data[i % SIZE] >= 128) {
        sum += data[i % SIZE];
      }
    }
    std::cout << "Sorted + Branch:    " << t.elapsed_ms() << " ms (sum=" << sum << ")\n";

    // --- 版本 3: Branchless ---
    //   原理: 用 bit 运算创建 mask, 消除显式分支
    //   mask = -(data[i] >= 128)  ← 编译器可能用 cmov
    //   但更可靠的是: sum += data[i] & mask where mask = 0 或 ~0
    //
    //   安全做法: 用三元运算符, 编译时加 -march=native 可能转 cmov
    std::mt19937 rng2(42);
    for (int& v : data) v = rng2();  // 重新生成未排序数据
    sum = 0;
    t = Timer();
    for (int i = 0; i < SIZE * 1000; ++i) {
      int val = data[i % SIZE];
      // branchless: 用位运算构造 mask
      // (val - 128) >> 31: 如果 val>=128, 非负→0, 如果 val<128, 负→-1
      // 但这在 val 接近 INT_MIN 时有 UB 风险, 用更安全的方式:
      int mask = (val >= 128) ? -1 : 0;  // -1 = 0xFFFFFFFF
      sum += val & mask;
    }
    std::cout << "Unsorted Branchless: " << t.elapsed_ms() << " ms (sum=" << sum << ")\n";

    // 验证 correctness
    int64_t expected = 0;
    for (int i = 0; i < SIZE * 1000; ++i) {
      int val = data[i % SIZE];
      if (val >= 128) expected += val;
    }
    std::cout << "Verification: branchless sum=" << sum
              << " expected=" << expected << " "
              << (sum == expected ? "OK" : "MISMATCH!") << "\n";
  }
}

// ============================================================================
// Ex6: Cache Prefetch — __builtin_prefetch
//
// 概念:
//   __builtin_prefetch(addr, rw, locality) 告诉 CPU 预取数据到 cache,
//   让后面的访存操作变成 L1 hit 而非 RAM 访问。
//
//   预取距离 (prefetch distance) 是关键:
//   - 太近: 数据到时还没用到 → 可能被 evict
//   - 太远: 数据到了但等待被用 → 也可能被 evict (prefetch window)
//   - 合适的距离: 刚好遮住访存延迟
//
// 任务:
//   1. 遍历链表, 对比有无 prefetch 的速度
//   2. 测试不同的 prefetch 距离, 找到最优值
//   3. 用链表遍历 (每个节点随机散布在内存中) 来体现 prefetch 价值
// ============================================================================

namespace ex6_prefetch {
  // 一个简单的链表节点
  struct Node {
    int64_t data[8];  // 64 bytes → 一个 cache line
    Node* next;
  };

  // 创建随机链表: N 个节点, 随机链接 (模拟最坏情况的遍历)
  std::vector<Node> create_random_list(int n) {
    std::vector<Node> nodes(n);
    std::vector<int> indices(n);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);

    for (int i = 0; i < n - 1; ++i) {
      nodes[indices[i]].next = &nodes[indices[i + 1]];
    }
    nodes[indices[n - 1]].next = nullptr;
    return nodes;
  }

  void run() {
    std::cout << "\n===== Ex6: Cache Prefetch =====\n";
    constexpr int N = 1'000'000;  // 不能全放 cache

    auto nodes = create_random_list(N);

    // 任务 1: 无 prefetch 的链表遍历
    volatile int64_t sink = 0;
    {
      Node* p = &nodes[0];
      Timer t;
      while (p) {
        sink += p->data[0];
        p = p->next;
      }
      std::cout << "No prefetch:   " << t.elapsed_ms() << " ms\n";
    }

    // 任务 2: 带 prefetch 的链表遍历
    // __builtin_prefetch(p->next, 0, 3);
    //   0 = read (1 = write)
    //   3 = high temporal locality
    {
      Node* p = &nodes[0];
      Timer t;
      while (p) {
        // 在访问当前节点前预取下一个
        if (p->next) {
          __builtin_prefetch(p->next, 0, 3);
        }
        sink += p->data[0];
        p = p->next;
      }
      std::cout << "With prefetch: " << t.elapsed_ms() << " ms\n";
    }

    // 任务 3: 测试不同 prefetch distance
    std::cout << "\nPrefetch distance test:\n";
    for (int dist : {1, 2, 4, 8, 16}) {
      Node* p = &nodes[0];
      Timer t;
      while (p) {
        // 提前 dist 步预取
        Node* future = p;
        for (int d = 0; d < dist && future; ++d)
          future = future->next;
        if (future) {
          __builtin_prefetch(future, 0, 3);
        }
        sink += p->data[0];
        p = p->next;
      }
      std::cout << "  dist=" << dist << ": " << t.elapsed_ms() << " ms\n";
    }
    std::cout << "Note: Optimal distance = memory_latency / node_process_time\n"
              << "      Too close → arrives after use; too far → evicted before use\n";
  }
}

// ============================================================================
// Ex7: Memory Alignment — alignas & std::hardware_destructive_interference_size
//
// 概念:
//   alignas(N): 强制变量/类型对齐到 N 字节边界
//   std::hardware_destructive_interference_size: 两个变量的最小安全间距
//     (避免 false sharing 的推荐值, 通常是 cache line 大小)
//
//   SIMD 要求: 16/32/64 字节对齐 (SSE/AVX/AVX-512)
//   Cache line: 64 字节对齐 (避免跨 cache line 读取)
//
// 任务:
//   1. 验证不同 alignas 的对齐效果 (alignof / reinterpret_cast 地址)
//   2. 测量未对齐 vs 对齐的读写速度
//   3. 用 std::hardware_destructive_interference_size 正确隔离变量
// ============================================================================

namespace ex7_memory_alignment {
  // 任务 1: 对比不同对齐
  struct Unaligned {
    char c;
    int64_t x;
  };
  // TODO: 用 alignas(64) 创建一个对齐版本

  struct Aligned64 {
    char c;
    alignas(64) int64_t x;  // x 在 cache line 边界开始
  };

  void run() {
    std::cout << "\n===== Ex7: Memory Alignment =====\n";

    std::cout << "sizeof(Unaligned) = " << sizeof(Unaligned)
              << " (padding=" << sizeof(Unaligned) - 1 - 8 << ")\n";
    std::cout << "sizeof(Aligned64) = " << sizeof(Aligned64) << "\n";

    // 验证对齐
    Aligned64 a;
    std::cout << "Address of c: " << (void*)&a.c << "\n";
    std::cout << "Address of x: " << (void*)&a.x << "\n";
    std::cout << "x offset % 64 = "
              << (reinterpret_cast<uintptr_t>(&a.x) % 64) << " (should be 0)\n";

    // 硬件破坏性干扰大小
    std::cout << "hardware_destructive_interference_size = "
              << std::hardware_destructive_interference_size << "\n";
    std::cout << "hardware_constructive_interference_size = "
              << std::hardware_constructive_interference_size << "\n";

    // 任务 2: 测量 aligned vs unaligned 访问速度
    {
      constexpr int COUNT = 1'000'000;
      // 对齐分配
      alignas(64) int64_t aligned_arr[COUNT] alignas(64);
      // 未对齐: 故意偏移 1 字节
      char* raw = new char[COUNT * sizeof(int64_t) + 64];
      int64_t* unaligned_arr = reinterpret_cast<int64_t*>(raw + 1);
      // 确保跨 cache line
      std::cout << "\nAligned addr % 64 = "
                << (reinterpret_cast<uintptr_t>(aligned_arr) % 64) << "\n";
      std::cout << "Unaligned addr % 64 = "
                << (reinterpret_cast<uintptr_t>(unaligned_arr) % 64) << "\n";

      // 初始化
      for (int i = 0; i < COUNT; ++i) {
        aligned_arr[i] = i;
        unaligned_arr[i] = i;
      }

      volatile int64_t s = 0;
      Timer ta;
      for (int iter = 0; iter < 100; ++iter)
        for (int i = 0; i < COUNT; ++i)
          s += aligned_arr[i];
      std::cout << "Aligned sum:   " << ta.elapsed_us() << " us\n";

      Timer tu;
      for (int iter = 0; iter < 100; ++iter)
        for (int i = 0; i < COUNT; ++i)
          s += unaligned_arr[i];
      std::cout << "Unaligned sum: " << tu.elapsed_us() << " us\n";
      delete[] raw;
    }

    // 任务 3: 线程安全对齐容器
    //   用 std::hardware_destructive_interference_size 隔离各线程数据
    {
      constexpr size_t PAD = std::hardware_destructive_interference_size;
      std::cout << "\nPer-thread counter with padding = " << PAD << " bytes:\n";
      struct alignas(PAD) PaddedCounter {
        int64_t value = 0;
      };
      // 用 padding 保证两个计数器不在同一 cache line
      PaddedCounter c1, c2;
      std::cout << "c1 addr: " << &c1 << ", c2 addr: " << &c2
                << ", distance: " << (char*)&c2 - (char*)&c1 << " bytes\n";
      std::cout << "Same cache line? "
                << ((reinterpret_cast<uintptr_t>(&c1) / 64) ==
                    (reinterpret_cast<uintptr_t>(&c2) / 64) ? "YES" : "NO")
                << "\n";
    }
  }
}

// ============================================================================
// Ex8: 数据导向设计 (Data-Oriented Design) — ECS 基础
//
// 概念:
//   OOP: GameEntity {update(); render(); pos; vel; health; sprite; ...}
//   DOD: Position[count], Velocity[count], Health[count] — 拆分存储
//
//   ECS (Entity Component System):
//   - Entity: 只是一个 ID
//   - Component: 纯数据 (如 Position, Velocity)
//   - System: 纯逻辑, 遍历特定 Component
//
//   优势: 遍历 N 个实体的 Position+Velocity →
//         连续内存 + cache 友好 + SIMD 友好
//
// 任务:
//   1. 实现一个微型的 ECS 内核 (Entity/SparseSet/ComponentPool)
//   2. 对比 OOP vs ECS 在 10万 实体上的移动系统性能
//   3. 实现两个 System: MovementSystem 和 AgingSystem
// ============================================================================

namespace ex8_dod_ecs {
  using Entity = uint32_t;
  constexpr Entity INVALID_ENTITY = ~0u;

  // -----------------------------------------------------------------------
  // Component Pool: 用稠密数组存储 Component, 用 sparse 映射 Entity→index
  // -----------------------------------------------------------------------
  template<typename T>
  class ComponentPool {
    std::vector<T> _dense;          // 实际数据, 紧密排列 (SoA)
    std::vector<Entity> _sparse;    // sparse[entity] → dense index
    std::vector<Entity> _entities;  // entities[dense index] → entity (反向映射)

  public:
    // TODO: 实现 add(entity, component), remove(entity), get(entity)
    //       size(), has(entity), entities() 迭代器

    void add(Entity e, const T& component) {
      // 如果 _sparse 不够大, 扩容
      if (e >= _sparse.size()) {
        _sparse.resize(e + 1, INVALID_ENTITY);
      }
      if (_sparse[e] != INVALID_ENTITY) {
        // already exists → update
        _dense[_sparse[e]] = component;
        return;
      }
      _sparse[e] = _dense.size();
      _dense.push_back(component);
      _entities.push_back(e);
    }

    bool has(Entity e) const {
      return e < _sparse.size() && _sparse[e] != INVALID_ENTITY;
    }

    T* get(Entity e) {
      if (!has(e)) return nullptr;
      return &_dense[_sparse[e]];
    }

    const T* get(Entity e) const {
      if (!has(e)) return nullptr;
      return &_dense[_sparse[e]];
    }

    size_t size() const { return _dense.size(); }

    // 反向查找: dense index → Entity
    Entity entity_at(size_t index) const { return _entities[index]; }

    const std::vector<Entity>& entities() const { return _entities; }

    // 遍历所有 component: for (auto& comp : pool.components()) { ... }
    std::vector<T>& components() { return _dense; }
    const std::vector<T>& components() const { return _dense; }
  };

  // --- Components (纯数据) ---
  struct Position { float x, y; };
  struct Velocity { float dx, dy; };
  struct Health  { int hp, max_hp; };

  // --- World: 管理 Entity + Component ---
  class World {
    std::vector<Entity> _entities;  // 所有活跃 entity id
    Entity _next_id = 0;

  public:
    ComponentPool<Position> positions;
    ComponentPool<Velocity> velocities;
    ComponentPool<Health>   healths;

    Entity spawn() {
      Entity e = _next_id++;
      _entities.push_back(e);
      return e;
    }

    // --- System 示例 ---
    void movement_system(float dt) {
      // TODO: 遍历有 Position + Velocity 的实体
      //   只遍历较小的 pool (通常是 velocities)
      for (size_t i = 0; i < velocities.size(); ++i) {
        Entity e = velocities.entity_at(i);  // 需要暴露
        Position* pos = positions.get(e);
        if (pos) {
          // 注意: get() 每次 O(1) 随机访问 → cache miss 风险
          // 更好的做法: 同时拥有两个 pool 的 dense 数组 + join 遍历
          (void)pos; (void)dt; // TODO: actually update
        }
      }
    }

    // 更好的遍历方式: 直接访问两个 dense 数组
    void movement_system_opt(float dt) {
      // 前提: 有 Position 保证有 Velocity, 且 index 对齐
      auto& pos = positions.components();
      auto& vel = velocities.components();
      size_t n = std::min(pos.size(), vel.size());
      for (size_t i = 0; i < n; ++i) {
        pos[i].x += vel[i].dx * dt;
        pos[i].y += vel[i].dy * dt;
      }
      // 全部是顺序访问 → cache friendly!
    }
  };

  void run() {
    std::cout << "\n===== Ex8: Data-Oriented Design (ECS) =====\n";

    // 任务 1: 补全 ComponentPool 的实现
    //         (框架已创建, 需要 add/remove/get/has/遍历)

    // 任务 2: 创建 100,000 个 Entity, 随机分配 Position + Velocity
    World world;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    constexpr int ENTITIES = 100'000;
    for (int i = 0; i < ENTITIES; ++i) {
      Entity e = world.spawn();
      world.positions.add(e, {dist(rng), dist(rng)});
      world.velocities.add(e, {dist(rng) * 0.1f, dist(rng) * 0.1f});
    }

    // 任务 3: 测量 movement_system_opt 的性能
    Timer t;
    for (int frame = 0; frame < 100; ++frame) {
      world.movement_system_opt(1.0f / 60.0f);
    }
    std::cout << "ECS Movement (100 frames, "
              << ENTITIES << " entities): "
              << t.elapsed_us() / 100.0 << " us/frame\n";

    // 任务 4: 创建等效的 OOP 版本, 对比性能
    std::cout << "\n--- OOP comparison ---\n";
    struct GameObject {
      float x, y, dx, dy;
    };
    std::vector<GameObject*> oop_objects;
    oop_objects.reserve(ENTITIES);
    for (int i = 0; i < ENTITIES; ++i) {
      auto* obj = new GameObject{dist(rng), dist(rng),
                                 dist(rng) * 0.1f, dist(rng) * 0.1f};
      oop_objects.push_back(obj);
    }
    // 随机打乱指针顺序 (模拟真实 OOP 分配场景)
    std::shuffle(oop_objects.begin(), oop_objects.end(), rng);

    Timer toop;
    float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < 100; ++frame) {
      for (auto* obj : oop_objects) {
        obj->x += obj->dx * dt;
        obj->y += obj->dy * dt;
      }
    }
    std::cout << "OOP Movement (100 frames, "
              << ENTITIES << " objects): "
              << toop.elapsed_us() / 100.0 << " us/frame\n";
    std::cout << "OOP objects are scattered in heap → pointer chasing → cache misses\n";

    for (auto* obj : oop_objects) delete obj;
  }
}

// ============================================================================
// Ex9: Cache-Oblivious 算法 — 矩阵分块乘法
//
// 概念:
//   Cache-oblivious: 算法本身不知道 cache 大小, 但通过递归分治
//   自动适配所有 cache 层次。
//
//   经典案例: 矩阵乘法
//   - Naive:       O(n³), 差劲的 cache 利用率
//   - Tiled:       O(n³), 手动分块, 需要 tune tile size
//   - Recursive:   O(n³), 自动分治到 L1 能装下的子矩阵
//
// 任务:
//   1. 实现 Naive 矩阵乘法
//   2. 实现 Tiled 矩阵乘法 (手动 block)
//   3. 实现递归分治矩阵乘法 (cache-oblivious)
//   4. 在 1024×1024 上对比三者性能
// ============================================================================

namespace ex9_matrix_multiply {
  using Matrix = std::vector<double>;

  int idx(int r, int c, int N) { return r * N + c; }

  // Naive: 标准三重循环
  Matrix naive_multiply(const Matrix& A, const Matrix& B, int N) {
    Matrix C(N * N, 0.0);
    for (int i = 0; i < N; ++i)
      for (int j = 0; j < N; ++j)
        for (int k = 0; k < N; ++k)
          C[idx(i, j, N)] += A[idx(i, k, N)] * B[idx(k, j, N)];
    return C;
  }

  // Tiled 矩阵乘法: 手动分块利用 cache
  Matrix tiled_multiply(const Matrix& A, const Matrix& B, int N, int TILE = 64) {
    Matrix C(N * N, 0.0);
    for (int i0 = 0; i0 < N; i0 += TILE) {
      for (int j0 = 0; j0 < N; j0 += TILE) {
        for (int k0 = 0; k0 < N; k0 += TILE) {
          // 内层: 三重循环限制在 TILE 范围内
          int i_end = std::min(i0 + TILE, N);
          int j_end = std::min(j0 + TILE, N);
          int k_end = std::min(k0 + TILE, N);
          for (int i = i0; i < i_end; ++i) {
            for (int k = k0; k < k_end; ++k) {
              double aik = A[idx(i, k, N)];
              for (int j = j0; j < j_end; ++j) {
                C[idx(i, j, N)] += aik * B[idx(k, j, N)];
              }
            }
          }
        }
      }
    }
    return C;
  }

  // 递归分治矩阵乘法 (cache-oblivious)
  //   自动适配所有 cache 层次, 不需要 tune tile size
  void rec_multiply(const double* A, const double* B, double* C,
                    int N, int stride,
                    int r0, int c0, int k0, int size) {
    constexpr int BASE = 64;  // 到 L1 大小停止分治
    if (size <= BASE) {
      for (int i = r0; i < r0 + size; ++i) {
        for (int k = k0; k < k0 + size; ++k) {
          double aik = A[i * stride + k];
          for (int j = c0; j < c0 + size; ++j) {
            C[i * stride + j] += aik * B[k * stride + j];
          }
        }
      }
      return;
    }
    int half = size / 2;
    // C11 += A11*B11
    rec_multiply(A, B, C, N, stride, r0, c0, k0, half);
    // C12 += A11*B12
    rec_multiply(A, B, C, N, stride, r0, c0 + half, k0, half);
    // C11 += A12*B21
    rec_multiply(A, B, C, N, stride, r0, c0, k0 + half, half);
    // C12 += A12*B22
    rec_multiply(A, B, C, N, stride, r0, c0 + half, k0 + half, half);
    // C21 += A21*B11
    rec_multiply(A, B, C, N, stride, r0 + half, c0, k0, half);
    // C22 += A21*B12
    rec_multiply(A, B, C, N, stride, r0 + half, c0 + half, k0, half);
    // C21 += A22*B21
    rec_multiply(A, B, C, N, stride, r0 + half, c0, k0 + half, half);
    // C22 += A22*B22
    rec_multiply(A, B, C, N, stride, r0 + half, c0 + half, k0 + half, half);
  }

  Matrix recursive_multiply(const Matrix& A, const Matrix& B, int N) {
    Matrix C(N * N, 0.0);
    rec_multiply(A.data(), B.data(), C.data(), N, N, 0, 0, 0, N);
    return C;
  }

  void run() {
    std::cout << "\n===== Ex9: Matrix Multiply =====\n";
    constexpr int N = 1024;

    // 生成随机矩阵
    Matrix A(N * N), B(N * N);
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (int i = 0; i < N * N; ++i) {
      A[i] = dist(rng);
      B[i] = dist(rng);
    }

    double ref_c0;
    // 跳过 naive (太慢 ~50s), 或用更小的 N 测试
    {
      Timer t;
      auto C = naive_multiply(A, B, N);
      ref_c0 = C[0];
      std::cout << "Naive:        " << t.elapsed_ms() << " ms (C[0]=" << C[0] << ")\n";
    }

    {
      Timer t;
      auto C = tiled_multiply(A, B, N, 64);
      std::cout << "Tiled (64):   " << t.elapsed_ms() << " ms (C[0]=" << C[0]
                << " " << (std::abs(C[0] - ref_c0) < 0.01 ? "OK" : "MISMATCH!") << ")\n";
    }

    {
      Timer t;
      auto C = recursive_multiply(A, B, N);
      std::cout << "Recursive:    " << t.elapsed_ms() << " ms (C[0]=" << C[0]
                << " " << (std::abs(C[0] - ref_c0) < 0.01 ? "OK" : "MISMATCH!") << ")\n";
    }

    // 更小块测试 tile size 敏感性
    std::cout << "\nTile size sensitivity (N=" << N << "):\n";
    for (int ts : {16, 32, 64, 128, 256}) {
      Timer t;
      auto C = tiled_multiply(A, B, N, ts);
      std::cout << "  TILE=" << ts << ": " << t.elapsed_ms() << " ms\n";
    }
  }
}

// ============================================================================
// Ex10: 综合实战 — 优化一个真实的数据处理 Pipeline
//
// 场景:
//   「股票行情处理引擎」
//   输入: 100万条行情记录 (symbol_id, price, volume)
//   处理:
//     1. 按 symbol_id 分组
//     2. 计算每个 symbol 的 VWAP (成交量加权均价)
//     3. 找出 VWAP 偏离均值超过 2σ 的异常 symbol
//   输出: 异常 symbol 列表和偏离度
//
// 任务:
//   1. 先用"最常见的方式"实现 (关注正确性)
//   2. 应用本周学到的技术优化 (至少 5 个优化点):
//      - Cache line 对齐 (alignas)
//      - AoS → SoA (拆分 price/volume)
//      - 数据预取 (prefetch)
//      - Branch prediction 友好 (分组前排序)
//      - 内存池 (预分配, 减少 alloc)
//   3. 用 perf stat 测量优化前后的 cache-misses, IPC, branch-misses
//   4. 写一段总结, 说明每个优化的效果和理由
// ============================================================================

namespace ex10_market_data_pipeline {
  struct TradeRecord {
    int64_t symbol_id;
    double price;
    int64_t volume;
    int64_t timestamp;
  };

  struct SymbolStats {
    double total_price_volume = 0.0;  // Σ(price * volume)
    int64_t total_volume = 0;
    double vwap() const {
      return total_volume > 0 ? total_price_volume / total_volume : 0.0;
    }
  };

  void run() {
    std::cout << "\n===== Ex10: Real-World Optimization =====\n";

    // 1. 生成 100 万条模拟行情数据
    constexpr int RECORDS = 1'000'000;
    constexpr int SYMBOLS = 5000;

    std::vector<TradeRecord> trades(RECORDS);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int64_t> sym_dist(0, SYMBOLS - 1);
    std::uniform_real_distribution<double> p_dist(1.0, 1000.0);
    std::uniform_int_distribution<int64_t> v_dist(1, 10000);

    for (auto& t : trades) {
      t.symbol_id = sym_dist(rng);
      t.price     = p_dist(rng);
      t.volume    = v_dist(rng);
      t.timestamp = 0;  // simplified
    }

    // 2. V1: "最常见的方式" — unordered_map<symbol_id, SymbolStats>
    std::cout << "--- V1: Naive (unordered_map) ---\n";
    {
      Timer t1;
      std::unordered_map<int64_t, SymbolStats> stats;
      for (const auto& tr : trades) {
        auto& s = stats[tr.symbol_id];
        s.total_price_volume += tr.price * tr.volume;
        s.total_volume += tr.volume;
      }
      // 计算均值和标准差
      double sum_vwap = 0.0;
      std::vector<double> vwaps;
      for (const auto& [id, s] : stats) {
        double v = s.vwap();
        sum_vwap += v;
        vwaps.push_back(v);
      }
      double mean = sum_vwap / vwaps.size();
      double sq_sum = 0.0;
      for (double v : vwaps) sq_sum += (v - mean) * (v - mean);
      double stdev = std::sqrt(sq_sum / vwaps.size());
      // 找出异常
      std::vector<std::pair<int64_t, double>> anomalies;
      for (const auto& [id, s] : stats) {
        double dev = s.vwap() - mean;
        if (std::abs(dev) > 2.0 * stdev) {
          anomalies.emplace_back(id, dev);
        }
      }
      std::cout << "V1 (naive):  " << t1.elapsed_ms() << " ms"
                << " | symbols=" << stats.size()
                << " | anomalies=" << anomalies.size()
                << " | mean_vwap=" << mean << "\n";
    }

    // 3. V2: 优化版本
    //   - 先按 symbol_id 排序 (branch prediction 友好 + 顺序分组)
    //   - SoA: 拆分为 price 和 volume 数组
    //   - alignas(64) 对齐 SymbolStats
    //   - 预取下一批 trades
    std::cout << "--- V2: Optimized ---\n";
    {
      Timer t2;

      // Step 1: 按 symbol_id 排序 (让相同 symbol 连续 → 顺序处理)
      std::vector<TradeRecord> sorted = trades;
      std::sort(sorted.begin(), sorted.end(),
                [](const TradeRecord& a, const TradeRecord& b) {
                  return a.symbol_id < b.symbol_id;
                });

      // Step 2: SoA + Stack 分配 SymbolStats (避免 heap alloc)
      alignas(64) SymbolStats stats_arr[SYMBOLS];  // 固定大小, 连续内存
      // 用 visited 跟踪哪些 symbol 有数据
      bool visited[SYMBOLS] = {false};

      // Step 3: 顺序遍历已排序数据 (同 symbol 连续 → cache friendly)
      size_t i = 0;
      while (i < sorted.size()) {
        int64_t sym = sorted[i].symbol_id;
        // 预取下一组数据
        if (i + 16 < sorted.size()) {
          __builtin_prefetch(&sorted[i + 16], 0, 3);
        }
        auto& s = stats_arr[sym];
        visited[sym] = true;
        // 批量处理同一 symbol 的所有记录
        while (i < sorted.size() && sorted[i].symbol_id == sym) {
          s.total_price_volume += sorted[i].price * sorted[i].volume;
          s.total_volume += sorted[i].volume;
          ++i;
        }
      }

      // Step 4: 计算 VWAP 统计
      double sum_vwap = 0.0;
      int sym_count = 0;
      for (int id = 0; id < SYMBOLS; ++id) {
        if (visited[id]) {
          sum_vwap += stats_arr[id].vwap();
          ++sym_count;
        }
      }
      double mean = sum_vwap / sym_count;

      double sq_sum = 0.0;
      for (int id = 0; id < SYMBOLS; ++id) {
        if (visited[id]) {
          double dev = stats_arr[id].vwap() - mean;
          sq_sum += dev * dev;
        }
      }
      double stdev = std::sqrt(sq_sum / sym_count);

      // 找出异常
      std::vector<std::pair<int64_t, double>> anomalies;
      for (int id = 0; id < SYMBOLS; ++id) {
        if (visited[id]) {
          double dev = stats_arr[id].vwap() - mean;
          if (std::abs(dev) > 2.0 * stdev) {
            anomalies.emplace_back(id, dev);
          }
        }
      }

      std::cout << "V2 (optimized): " << t2.elapsed_ms() << " ms"
                << " | symbols=" << sym_count
                << " | anomalies=" << anomalies.size()
                << " | mean_vwap=" << mean << "\n";
    }

    // 4. 优化报告
    std::cout << "\n════ Optimization Report ════\n";
    std::cout << "1. Sort by symbol_id:\n"
              << "   - 相同 symbol 的记录连续存储 → spatial locality 大增\n"
              << "   - 分支预测: while 循环的 sym 比较变成 predictable\n"
              << "   - unordered_map hash lookup 变成顺序扫描\n\n";
    std::cout << "2. Stack-allocated alignas(64) array:\n"
              << "   - 消除 unordered_map 的链表追逐 (cache miss)\n"
              << "   - 连续内存 → 遍历时 cache line 利用 100%\n"
              << "   - alignas(64) 避免 false sharing (虽然本例单线程)\n\n";
    std::cout << "3. Data prefetch:\n"
              << "   - __builtin_prefetch 提前拉取下一批数据\n"
              << "   - 掩盖内存访问延迟\n\n";
    std::cout << "4. Single-pass processing:\n"
              << "   - V1: 遍历 trades + 遍历 hashmap + 遍历 vwaps (3 pass)\n"
              << "   - V2: sort + scan + scan stats (still ~3 pass, but contiguous)\n\n";
    std::cout << "5. Trade-off:\n"
              << "   - Sort 本身 O(n log n) → 小数据量未必赢\n"
              << "   - 大数据量: sort overhead < hashmap cache miss overhead\n"
              << "   - 用 perf stat 测 cache-misses 可量化差异\n";
  }
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Month 4 / Week 17: CPU Cache & Memory Hierarchy\n";
  std::cout << "══════════════════════════════════════════════\n";

  ex1_cache_hierarchy::run();
  ex2_false_sharing::run();
  ex3_aos_vs_soa::run();
  ex4_loop_interchange::run();
  ex5_branch_prediction::run();
  ex6_prefetch::run();
  ex7_memory_alignment::run();
  ex8_dod_ecs::run();
  ex9_matrix_multiply::run();
  ex10_market_data_pipeline::run();

  std::cout << "\n Week 17 Done! 🎉\n";
  return 0;
}
