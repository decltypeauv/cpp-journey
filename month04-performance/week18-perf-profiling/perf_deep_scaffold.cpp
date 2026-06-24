// ============================================================================
// Month 4: 极致性能 — Beyond the Code
// Week 18: perf & Profiling — 测量驱动优化
//
// 核心哲学:
//   「没有测量就没有优化」(You can't optimize what you don't measure)
//   「不要猜测性能瓶颈 — 让 perf 告诉你」
//
//   perf 是 Linux 内核的性能分析工具, 利用 CPU 硬件性能计数器 (PMC)
//   提供精确到指令级别的性能数据。它是每个 C++ 性能工程师的必备工具。
//
// 本周目标:
//   - 掌握 perf stat / record / report / top / trace 核心命令
//   - 学会编写稳定的微基准测试
//   - 理解火焰图并从中识别热点
//   - 建立 「profile → 找热点 → 优化 → 验证」 的工作流
//
// 10 个练习, 由浅入深
// ============================================================================

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

// ============================================================================
// 工具: 高精度 Timer (RAII)
// ============================================================================

class Timer {
  high_resolution_clock::time_point _start;
  const char* _label;
public:
  explicit Timer(const char* label = nullptr)
    : _start(high_resolution_clock::now()), _label(label) {}
  ~Timer() {
    if (_label) {
      std::cout << _label << ": " << elapsed_ms() << " ms\n";
    }
  }
  int64_t elapsed_ns() const {
    return duration_cast<nanoseconds>(
      high_resolution_clock::now() - _start
    ).count();
  }
  double elapsed_us() const { return elapsed_ns() / 1000.0; }
  double elapsed_ms() const { return elapsed_ns() / 1'000'000.0; }
};

// ============================================================================
// 工具: 微基准测试框架 (Microbenchmark Harness)
// 提供: 预热 (warmup), 多轮迭代, 统计 (min/avg/median/stddev)
// ============================================================================

class MicroBench {
  std::function<void()> _fn;
  int _warmup_iters = 3;
  int _measure_iters = 10;
  std::vector<double> _samples;

public:
  explicit MicroBench(std::function<void()> fn) : _fn(std::move(fn)) {}

  MicroBench& warmup(int n) { _warmup_iters = n; return *this; }
  MicroBench& iterations(int n) { _measure_iters = n; return *this; }

  struct Result {
    double min_ms;
    double avg_ms;
    double median_ms;
    double stddev_ms;
    double max_ms;
    double total_ms;
    int samples;
  };

  Result run(const char* label) {
    _samples.clear();
    _samples.reserve(_measure_iters);

    // Warmup
    for (int i = 0; i < _warmup_iters; ++i) {
      _fn();
    }

    // Measurement
    for (int i = 0; i < _measure_iters; ++i) {
      Timer t;
      _fn();
      _samples.push_back(t.elapsed_ms());
    }

    // Statistics
    std::sort(_samples.begin(), _samples.end());
    double sum = std::accumulate(_samples.begin(), _samples.end(), 0.0);
    double avg = sum / _samples.size();
    double median = _samples[_samples.size() / 2];

    double sq_sum = 0.0;
    for (double s : _samples) sq_sum += (s - avg) * (s - avg);
    double stddev = std::sqrt(sq_sum / _samples.size());

    Result r{_samples[0], avg, median, stddev,
             _samples.back(), sum, (int)_samples.size()};

    std::cout << std::setw(28) << label
              << " | min=" << std::setw(8) << std::fixed << std::setprecision(3) << r.min_ms
              << " | avg=" << std::setw(8) << r.avg_ms
              << " | med=" << std::setw(8) << r.median_ms
              << " | σ=" << std::setw(6) << r.stddev_ms
              << " ms\n";
    return r;
  }
};

// ============================================================================
// 工具: 模拟 CPU-bound 工作量
// ============================================================================

// 可被优化掉的工作负载
volatile int64_t g_sink = 0;

// 不可优化: 编译器屏障
#ifdef __GNUC__
#define DO_NOT_OPTIMIZE(x) asm volatile("" : "+r"(x))
#else
#define DO_NOT_OPTIMIZE(x)
#endif

// ============================================================================
// Ex1: 理解 perf stat — 从硬件计数器读懂程序行为
//
// 概念:
//   perf stat 提供一批关键硬件计数器:
//   - cycles:      CPU 周期总数 (与 CPU 频率相关)
//   - instructions: 执行的指令数
//   - IPC:         instructions per cycle (理想值 > 2.0, < 0.5 很糟糕)
//   - cache-misses:   L3 cache miss 次数 (访问主存)
//   - cache-references: L3 cache 访问次数
//   - branch-misses:   分支预测失败次数
//   - task-clock:      程序占用的 CPU 时间
//   - context-switches: 上下文切换
//
// 实验: 编写三个不同 cache 行为的函数, 用 perf stat 对比
//
// 命令行:
//   $ perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses ./perf_deep
//   $ perf stat -d ./perf_deep  # 详细输出
//   $ perf stat -r 5 ./perf_deep  # 重复 5 次取平均
//
// 任务: 观察下面三个函数的 perf 差异, 填写预期计数器值
// ============================================================================

namespace ex1_perf_stat_basics {
  constexpr int SIZE = 16 * 1024 * 1024;  // 16M ints = 64 MB (不能全放 cache)

  // 函数 1: 顺序遍历 (cache-friendly)
  void sequential_sum(int64_t* data, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) {
      sum += data[i];
    }
    g_sink = sum;
  }

  // 函数 2: 随机跳跃访问 (cache-unfriendly)
  void random_jump(int64_t* data, const int* indices, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) {
      sum += data[indices[i]];
    }
    g_sink = sum;
  }

  // 函数 3: 密集计算 (计算重, 访存轻)
  void compute_heavy(int n) {
    double result = 0.0;
    for (int i = 0; i < n; ++i) {
      result += std::sin(i * 0.001) * std::cos(i * 0.0001);
    }
    g_sink = (int64_t)result;
  }

  void run() {
    std::cout << "\n===== Ex1: perf stat Basics =====\n";

    // 分配 64MB 数组
    std::vector<int64_t> data(SIZE);
    std::iota(data.begin(), data.end(), 0);

    // 随机索引
    std::vector<int> indices(SIZE / 100);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);

    std::cout << "Data size: " << (SIZE * 8 / 1024 / 1024) << " MB\n";
    std::cout << "Run: perf stat -e cycles,instructions,cache-misses ./perf_deep\n\n";

    // 1. 顺序访问
    {
      Timer t("  Sequential sum");
      sequential_sum(data.data(), SIZE);
    }
    std::cout << "  Expected: HIGH IPC, LOW cache-misses, LOW branch-misses\n\n";

    // 2. 随机访问
    {
      Timer t("  Random jump");
      random_jump(data.data(), indices.data(), indices.size());
    }
    std::cout << "  Expected: LOW IPC, HIGH cache-misses, LOW branch-misses\n\n";

    // 3. 密集计算
    {
      Timer t("  Compute heavy");
      compute_heavy(SIZE / 10);
    }
    std::cout << "  Expected: HIGH IPC, LOW cache-misses, LOW branch-misses\n";
    std::cout << "            (计算密集 = 无分支 + 无访存 = IPC 最高)\n";

    std::cout << "\n─── Self-Check: perf stat 预期表 ───\n";
    std::cout << "Function        | cycles | instructions | IPC  | cache-misses\n";
    std::cout << "sequential_sum  | medium | medium       | ~1.5 | VERY LOW\n";
    std::cout << "random_jump     | HIGH   | medium       | <0.3 | VERY HIGH\n";
    std::cout << "compute_heavy   | HIGH   | VERY HIGH    | >2.0 | ~0\n";
  }
}

// ============================================================================
// Ex2: perf record + perf report — 采样找到热点
//
// 概念:
//   perf record 以固定频率 (默认 4000 Hz) 采样 CPU 正在执行的指令地址,
//   生成 perf.data 文件。perf report 分析这些采样, 按函数分组显示
//   每个函数消耗的 CPU 时间百分比。
//
// 命令行:
//   $ perf record ./perf_deep
//   $ perf report            # TUI 交互式浏览
//   $ perf report --stdio    # 纯文本输出
//   $ perf report -g         # 显示调用图 (call graph)
//   $ perf report --sort=dso # 按动态库排序
//
// 实验: 编写一个多级调用链的程序, 制造清晰的性能层级,
//        然后运行 perf record + report 查看结果
//
// 任务: 补全三个叶子函数, 使它们产生显著不同的时间占比,
//        然后用 perf 验证实际占比是否符合预期
// ============================================================================

namespace ex2_perf_record_report {
  // 三个叶子函数, 不同的消耗时间
  void fast_op() {
    volatile int x = 0;
    for (int i = 0; i < 1000; ++i) x += i;
  }

  void medium_op() {
    volatile int x = 0;
    for (int i = 0; i < 1'000'000; ++i) x += i;
  }

  void slow_op() {
    volatile int x = 0;
    for (int i = 0; i < 100'000'000; ++i) x += i;  // ~100M iterations
  }

  // 这些 wrapper 故意弄深调用栈, 测试 perf report -g 的效果
  void wrapper_a() { for (int i = 0; i < 10; ++i) fast_op(); }
  void wrapper_b() { for (int i = 0; i < 10; ++i) medium_op(); }
  void wrapper_c() { for (int i = 0; i < 5; ++i) slow_op(); }

  // 顶层入口, 预期耗时分布: slow_op ≈ 80%, medium_op ≈ 15%, fast_op ≈ 5%
  void workload() {
    for (int i = 0; i < 20; ++i) {
      wrapper_a();  // ~20 × 10 × 1000 operations
      wrapper_b();  // ~20 × 10 × 1M operations
      wrapper_c();  // ~20 × 5 × 100M operations
    }
  }

  void run() {
    std::cout << "\n===== Ex2: perf record + perf report =====\n";
    std::cout << "Running multi-level workload...\n";
    std::cout << "Expected time distribution:\n";
    std::cout << "  slow_op   ~90%  (100M × 5 iter × 20 wrapper)\n";
    std::cout << "  medium_op ~9%   (1M × 10 iter × 20 wrapper)\n";
    std::cout << "  fast_op   ~1%\n\n";

    Timer t("  workload");
    workload();

    std::cout << "\n─── perf record 使用指南 ───\n";
    std::cout << "# 1. 采样 (需要 sudo 权限):\n";
    std::cout << "$ perf record -g -F 99 ./perf_deep\n";
    std::cout << "  -g: 记录调用栈\n";
    std::cout << "  -F 99: 采样频率 99Hz (避免与定时器锁步)\n\n";
    std::cout << "# 2. 查看报告:\n";
    std::cout << "$ perf report -g --stdio | head -40\n\n";
    std::cout << "# 3. 只查看用户空间:\n";
    std::cout << "$ perf report --dsos=perf_deep\n\n";
    std::cout << "# 4. 生成可读的火焰图脚本输入:\n";
    std::cout << "$ perf script > out.perf\n";
  }
}

// ============================================================================
// Ex3: perf top / perf trace — 实时监控
//
// 概念:
//   perf top:    类似 top 命令, 但显示的是 CPU 采样热点函数 (实时刷新)
//   perf trace:  类似 strace, 记录系统调用及其耗时
//
// 实验:
//   1. 编写一个持续运行的程序, 用 perf top 观察
//   2. 用 perf trace 跟踪程序的系统调用
//
// 常用命令:
//   $ perf top -p <pid>       # 监控特定进程
//   $ perf top -s comm,dso    # 按进程/库分组
//   $ perf trace -p <pid>     # 跟踪系统调用
//   $ perf trace -e 'syscalls:sys_enter_*' ./prog  # 跟踪所有 syscall
// ============================================================================

namespace ex3_perf_top_trace {
  // 一个有规律的系统调用模式
  void syscall_worker(int thread_id, int iterations) {
    char buf[64];
    for (int i = 0; i < iterations; ++i) {
      // 故意做一些 IO (会触发 write syscall)
      snprintf(buf, sizeof(buf),
               "thread=%d iter=%d some_padding_to_make_this_longer\n",
               thread_id, i);
      // 把 buf 写到 /dev/null 模拟轻量 IO
      FILE* f = fopen("/dev/null", "w");
      if (f) {
        fputs(buf, f);
        fclose(f);
      }
      // CPU 密集部分
      volatile double x = 0;
      for (int j = 0; j < 10000; ++j) x += std::sqrt(j * 1.0);
    }
  }

  void run() {
    std::cout << "\n===== Ex3: perf top / perf trace =====\n";
    constexpr int ITER = 100;

    std::cout << "Running mixed IO/CPU workload (" << ITER << " iters)...\n";
    Timer t("  syscall_worker");
    syscall_worker(0, ITER);

    std::cout << "\n─── 实时监控命令 ───\n";
    std::cout << "# 方法 1: perf top (实时热点)\n";
    std::cout << "$ ./perf_deep &\n";
    std::cout << "$ perf top -p $!\n\n";
    std::cout << "# 方法 2: perf trace (系统调用)\n";
    std::cout << "$ perf trace -p $! 2>&1 | head -30\n";
    std::cout << "$ perf trace -s -p $!   # 统计模式\n\n";
    std::cout << "# 方法 3: perf stat 实时模式\n";
    std::cout << "$ perf stat -I 1000 -p $!  # 每秒输出一次\n\n";
    std::cout << "# 提示: perf top 主要用于查找 TOP 热点函数\n";
    std::cout << "#       perf trace 用于发现意外的 syscall 开销\n";
  }
}

// ============================================================================
// Ex4: Flame Graphs — 火焰图
//
// 概念:
//   火焰图 (Flame Graph) 是 Brendan Gregg 发明的可视化工具,
//   将 perf 采样数据转换成可交互的 SVG 图形。
//
//   每个框 = 一个函数调用, 宽度 = 采样次数 (CPU 时间占比)
//   Y 轴 = 调用栈深度, X 轴 = 按字母排序
//
//   优势: 一眼看出 "哪个函数的哪个调用路径" 消耗最多 CPU
//
// 生成流程:
//   1. perf record -g -F 99 ./prog
//   2. perf script > out.perf
//   3. stackcollapse-perf.pl out.perf > out.folded   (来自 FlameGraph 项目)
//   4. flamegraph.pl out.folded > flamegraph.svg     (来自 FlameGraph 项目)
//
//   更简单的方式 (新内核):
//   1. perf record -g --call-graph dwarf ./prog
//   2. perf script report flamegraph  # 直接输出火焰图数据
//
// 任务: 编写一个有明确调用层级关系的程序,
//        预期每个调用链的火焰图占比, 然后实际生成验证
// ============================================================================

namespace ex4_flamegraph {
  // 不同深度的调用树, 模拟真实程序
  // main → process → {hot_path, warm_path, cold_path}

  void heavy_math() {
    volatile double x = 0;
    for (int i = 0; i < 50'000'000; ++i) x += std::sin(i * 0.0001);
  }

  void moderate_math() {
    volatile double x = 0;
    for (int i = 0; i < 5'000'000; ++i) x += std::cos(i * 0.0001);
  }

  void light_math() {
    volatile double x = 0;
    for (int i = 0; i < 500'000; ++i) x += std::tan(i * 0.0001);
  }

  // --- 调用链路 ---
  void hot_path()  { heavy_math(); }       // ~90% time
  void warm_path() { moderate_math(); }    // ~9% time
  void cold_path() { light_math(); }       // ~1% time

  void process_workload() {
    for (int i = 0; i < 10; ++i) {
      hot_path();
      if (i % 10 == 0) warm_path();  // 每 10 次 run 1 次
      if (i % 100 == 0) cold_path(); // 每 100 次 run 1 次
    }
  }

  void run() {
    std::cout << "\n===== Ex4: Flame Graphs =====\n";
    Timer t("  flamegraph workload");
    process_workload();

    std::cout << "\n─── Flame Graph 生成命令 ───\n";
    std::cout << "# 1. 用 DWARF 记录调用栈 (比帧指针更完整):\n";
    std::cout << "$ perf record -g --call-graph dwarf -F 99 ./perf_deep\n\n";
    std::cout << "# 2. 安装 FlameGraph 工具:\n";
    std::cout << "$ git clone https://github.com/brendangregg/FlameGraph.git\n\n";
    std::cout << "# 3. 生成火焰图:\n";
    std::cout << "$ perf script | FlameGraph/stackcollapse-perf.pl > out.folded\n";
    std::cout << "$ FlameGraph/flamegraph.pl out.folded > flamegraph.svg\n\n";
    std::cout << "# 4. 在浏览器中打开 flamegraph.svg, 点击框放大查看\n\n";
    std::cout << "# 新内核方式 (Linux 6.x):\n";
    std::cout << "$ perf report --flamegraph  # 直接在 TUI 中显示!\n\n";

    std::cout << "预期火焰图:\n";
    std::cout << "  ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁ hot_path\n";
    std::cout << "  ▁▁▁▁▁▁▁▁ warm_path\n";
    std::cout << "  ▏ cold_path\n";
  }
}

// ============================================================================
// Ex5: 微基准测试实践 — 编写稳定的 Benchmark
//
// 概念:
//   测量微小函数 (几 ns ~ 几 us) 的真实性能需要非常小心:
//   → 预热 I-cache / D-cache / branch predictor
//   → 避免死代码消除 (编译器会优化掉不用的结果)
//   → 足够的迭代次数 (让噪声平均化)
//   → 排除系统干扰 (固定 CPU 频率, 绑定核心)
//
// 任务:
//   1. 完成 MicroBench 框架
//   2. Benchmark: std::string vs std::string_view 查找
//   3. Benchmark: std::unordered_map vs std::map lookup
//   4. Benchmark: for 循环 vs 迭代器 vs range-for
//   5. 理解为什么 benchmark 结果是"反直觉"的
// ============================================================================

namespace ex5_micro_benchmark {
  constexpr int N = 100'000;

  void run() {
    std::cout << "\n===== Ex5: Micro-Benchmarking =====\n";
    std::random_device rd;
    std::mt19937 rng(42);

    // --- Benchmark 1: string substr vs string_view ---
    {
      std::cout << "\n1. Substring extraction:\n";
      std::string long_str(10000, 'x');
      long_str[5000] = 'T';  // 在中间插入一个目标字符

      // string::find + substr (allocates new string)
      MicroBench([&] {
        auto pos = long_str.find('T');
        std::string sub = long_str.substr(pos, 100);
        g_sink += sub.size();
      })
      .warmup(5)
      .iterations(20)
      .run("  std::string::substr");

      // string::find + string_view (no allocation!)
      MicroBench([&] {
        auto pos = long_str.find('T');
        std::string_view sv(long_str.data() + pos, 100);
        g_sink += sv.size();
      })
      .warmup(5)
      .iterations(20)
      .run("  std::string_view");
    }

    // --- Benchmark 2: map 查找 ---
    {
      std::cout << "\n2. Map lookup (" << N << " elements):\n";

      std::map<int, int> ordered_map;
      std::unordered_map<int, int> hash_map;
      for (int i = 0; i < N; ++i) {
        ordered_map[i] = i * 2;
        hash_map[i] = i * 2;
      }

      // 顺序查找 (对 std::map 有利)
      MicroBench([&] {
        int64_t sum = 0;
        for (int i = 0; i < N; ++i)
          sum += ordered_map[i];
        g_sink = sum;
      })
      .warmup(3).iterations(10)
      .run("  std::map seq lookup");

      MicroBench([&] {
        int64_t sum = 0;
        for (int i = 0; i < N; ++i)
          sum += hash_map[i];
        g_sink = sum;
      })
      .warmup(3).iterations(10)
      .run("  unordered_map seq lookup");

      // 随机查找
      std::vector<int> random_keys(N);
      std::iota(random_keys.begin(), random_keys.end(), 0);
      std::shuffle(random_keys.begin(), random_keys.end(), rng);

      MicroBench([&] {
        int64_t sum = 0;
        for (int k : random_keys)
          sum += ordered_map[k];
        g_sink = sum;
      })
      .warmup(3).iterations(10)
      .run("  std::map rand lookup");

      MicroBench([&] {
        int64_t sum = 0;
        for (int k : random_keys)
          sum += hash_map[k];
        g_sink = sum;
      })
      .warmup(3).iterations(10)
      .run("  unordered_map rand lookup");
    }

    // --- Benchmark 3: 循环方式对比 ---
    {
      std::cout << "\n3. Loop style (sum 1M ints):\n";
      std::vector<int> vec(1'000'000);
      std::iota(vec.begin(), vec.end(), 0);

      MicroBench([&] {
        int64_t sum = 0;
        for (size_t i = 0; i < vec.size(); ++i)
          sum += vec[i];
        g_sink = sum;
      })
      .warmup(5).iterations(20)
      .run("  Indexed for");

      MicroBench([&] {
        int64_t sum = 0;
        for (auto it = vec.begin(); it != vec.end(); ++it)
          sum += *it;
        g_sink = sum;
      })
      .warmup(5).iterations(20)
      .run("  Iterator");

      MicroBench([&] {
        int64_t sum = 0;
        for (int x : vec)
          sum += x;
        g_sink = sum;
      })
      .warmup(5).iterations(20)
      .run("  Range-for");
    }
  }
}

// ============================================================================
// Ex6: 构建自己的 Profiler — 函数级计时器
//
// 概念:
//   除了 perf, 自己也可以用 RAII 实现轻量级 profiler,
//   记录每个函数被调用的次数和总耗时。
//
//   这在以下场景非常有用:
//   - perf 无法使用 (容器/特殊环境)
//   - 需要与业务逻辑关联的 profile (如 "处理哪个用户最慢")
//   - 需要程序内部上报到监控系统
//
// 任务:
//   1. 完成 Profiler 单例类
//   2. 使用 PROFILE_SCOPE 宏标记需要测量的函数
//   3. 运行多线程程序, 输出 profiling 报告
// ============================================================================

namespace ex6_custom_profiler {
  struct ProfileEntry {
    std::string name;
    int64_t total_ns = 0;
    int64_t call_count = 0;
    int64_t max_ns = 0;
  };

  class Profiler {
    std::mutex _mutex;
    std::unordered_map<std::string, ProfileEntry> _entries;
  public:
    static Profiler& instance() {
      static Profiler p;
      return p;
    }

    void record(const std::string& name, int64_t ns) {
      std::lock_guard lock(_mutex);
      auto& entry = _entries[name];
      entry.name = name;
      entry.total_ns += ns;
      entry.call_count++;
      if (ns > entry.max_ns) entry.max_ns = ns;
    }

    void report() const {
      // 排序: 按总耗时降序
      std::vector<const ProfileEntry*> sorted;
      for (auto& [name, entry] : _entries)
        sorted.push_back(&entry);
      std::sort(sorted.begin(), sorted.end(),
                [](auto* a, auto* b) { return a->total_ns > b->total_ns; });

      std::cout << "\n═══ Profiler Report ═══\n";
      std::cout << std::setw(24) << "Function"
                << " |" << std::setw(10) << "total_ms"
                << " |" << std::setw(8) << "calls"
                << " |" << std::setw(10) << "avg_us"
                << " |" << std::setw(10) << "max_us"
                << "\n";
      std::cout << std::string(75, '-') << "\n";

      int64_t grand_total = 0;
      for (auto* e : sorted) {
        std::cout << std::setw(24) << e->name
                  << " |" << std::setw(10) << std::fixed << std::setprecision(3)
                  << e->total_ns / 1e6
                  << " |" << std::setw(8) << e->call_count
                  << " |" << std::setw(10) << e->total_ns / (e->call_count * 1000.0)
                  << " |" << std::setw(10) << e->max_ns / 1000.0
                  << "\n";
        grand_total += e->total_ns;
      }
      std::cout << std::string(75, '-') << "\n";
      std::cout << "  Grand total: " << grand_total / 1e6 << " ms\n";
    }
  };

  // RAII scope profiler
  class ScopeProfiler {
    const char* _name;
    high_resolution_clock::time_point _start;
  public:
    explicit ScopeProfiler(const char* name)
      : _name(name), _start(high_resolution_clock::now()) {}
    ~ScopeProfiler() {
      auto ns = duration_cast<nanoseconds>(
        high_resolution_clock::now() - _start
      ).count();
      Profiler::instance().record(_name, ns);
    }
  };

#define PROFILE_SCOPE(name) ScopeProfiler _prof_##__LINE__(name)

  // --- 被测函数 ---
  void fast_function() {
    PROFILE_SCOPE("fast_function");
    volatile int x = 0;
    for (int i = 0; i < 100000; ++i) x += i;
  }

  void medium_function() {
    PROFILE_SCOPE("medium_function");
    fast_function();  // 嵌套调用
    fast_function();
    volatile int x = 0;
    for (int i = 0; i < 1'000'000; ++i) x += i;
  }

  void slow_function() {
    PROFILE_SCOPE("slow_function");
    medium_function();
    volatile int x = 0;
    for (int i = 0; i < 10'000'000; ++i) x += i;
  }

  void run() {
    std::cout << "\n===== Ex6: Custom Profiler =====\n";

    std::cout << "Running instrumented functions...\n";
    for (int i = 0; i < 5; ++i) {
      slow_function();
      medium_function();
      fast_function();
    }

    Profiler::instance().report();
    std::cout << "\nNote: This profiler adds ~100ns overhead per call.\n"
              << "      Use perf for zero-overhead sampling.\n";
  }
}

// ============================================================================
// Ex7: 性能对比 — 用 Benchmark 验证算法的理论复杂度
//
// 概念:
//   理论上 vector::push_back 是 amortized O(1),
//   但实际中因为 reallocation, 可能出现锯齿状延迟。
//   用 benchmark 可以验证:
//   - reserve vs no-reserve 性能差异
//   - vector vs list 在插入/遍历场景的差异
//   - std::sort vs std::stable_sort 性能差异
//
// 任务:
//   对每组算法在多个 size 下 benchmark, 验证大 O 复杂度
// ============================================================================

namespace ex7_complexity_bench {
  // 对多个 size 运行同一个 benchmark
  void bench_across_sizes(
    const char* label,
    std::function<void(int)> fn,
    std::vector<int> sizes
  ) {
    std::cout << label << ":\n";
    for (int n : sizes) {
      Timer t;
      fn(n);
      std::cout << "  n=" << std::setw(8) << n << "  | "
                << t.elapsed_us() << " us\n";
    }
  }

  void run() {
    std::cout << "\n===== Ex7: Complexity Benchmarking =====\n";

    std::vector<int> sizes = {1000, 10000, 100000, 500000, 1000000};

    // --- vector push_back: reserve vs no-reserve ---
    std::cout << "\n1. vector<int>::push_back:\n";
    bench_across_sizes("  With reserve", [](int n) {
      std::vector<int> v;
      v.reserve(n);
      for (int i = 0; i < n; ++i) v.push_back(i);
      g_sink = v.size();
    }, sizes);

    bench_across_sizes("  No reserve", [](int n) {
      std::vector<int> v;
      for (int i = 0; i < n; ++i) v.push_back(i);
      g_sink = v.size();
    }, sizes);

    // --- Sort: std::sort vs std::stable_sort ---
    std::vector<int> sort_sizes = {1000, 10000, 100000, 500000};
    std::cout << "\n2. Sort comparison:\n";

    bench_across_sizes("  std::sort", [](int n) {
      std::vector<int> v(n);
      std::mt19937 rng(42);
      for (int i = 0; i < n; ++i) v[i] = rng();
      std::sort(v.begin(), v.end());
      g_sink = v[0];
    }, sort_sizes);

    bench_across_sizes("  std::stable_sort", [](int n) {
      std::vector<int> v(n);
      std::mt19937 rng(42);
      for (int i = 0; i < n; ++i) v[i] = rng();
      std::stable_sort(v.begin(), v.end());
      g_sink = v[0];
    }, sort_sizes);

    // --- vector vs forward_list 遍历 ---
    std::cout << "\n3. Container traversal (sum):\n";
    bench_across_sizes("  vector sum", [](int n) {
      std::vector<int> v(n);
      std::iota(v.begin(), v.end(), 0);
      int64_t sum = 0;
      for (int x : v) sum += x;
      g_sink = sum;
    }, sizes);

    bench_across_sizes("  list sum", [](int n) {
      std::list<int> lst;
      for (int i = 0; i < n; ++i) lst.push_back(i);
      int64_t sum = 0;
      for (int x : lst) sum += x;
      g_sink = sum;
    }, sizes);
  }
}

// ============================================================================
// Ex8: Cache Miss 归因 — 用 perf 精确定位
//
// 概念:
//   通过 perf record -e cache-misses -g, 可以采样 cache miss
//   发生在哪条指令、哪个数据结构上。
//
//   perf mem record 更进一步: 记录每次内存访问的延迟,
//   标注是 L1/L2/L3 hit 还是 RAM, 精确到数据地址!
//
// 实验:
//   编写一个同时有 cache-friendly 和 cache-unfriendly 访问的程序,
//   用 perf 定位 cache-unfriendly 部分。
//
// 命令:
//   $ perf record -e cache-misses -g ./perf_deep
//   $ perf report -g --stdio
//
//   $ perf mem record ./perf_deep     # 记录内存访问延迟
//   $ perf mem report                 # 按延迟/地址/symbol 分析
// ============================================================================

namespace ex8_cache_miss_attribution {
  static constexpr int M = 4096;
  static constexpr int BLOCK = 64;

  // 好的矩阵乘法 (tiled, cache-friendly)
  void good_matmul(double* C, const double* A, const double* B, int n) {
    std::memset(C, 0, n * n * sizeof(double));
    for (int i0 = 0; i0 < n; i0 += BLOCK) {
      for (int j0 = 0; j0 < n; j0 += BLOCK) {
        for (int k0 = 0; k0 < n; k0 += BLOCK) {
          for (int i = i0; i < std::min(i0 + BLOCK, n); ++i) {
            for (int k = k0; k < std::min(k0 + BLOCK, n); ++k) {
              double aik = A[i * n + k];
              for (int j = j0; j < std::min(j0 + BLOCK, n); ++j) {
                C[i * n + j] += aik * B[k * n + j];
              }
            }
          }
        }
      }
    }
  }

  // 坏的矩阵乘法 (naive, cache-hostile)
  void bad_matmul(double* C, const double* A, const double* B, int n) {
    std::memset(C, 0, n * n * sizeof(double));
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < n; ++j) {
        for (int k = 0; k < n; ++k) {
          C[i * n + j] += A[i * n + k] * B[k * n + j];  // B 列访问!
        }
      }
    }
  }

  void run() {
    std::cout << "\n===== Ex8: Cache Miss Attribution =====\n";
    constexpr int N = 512;  // 512×512 doubles = 2MB each

    std::vector<double> A(N * N), B(N * N), C(N * N);
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (int i = 0; i < N * N; ++i) {
      A[i] = dist(rng);
      B[i] = dist(rng);
    }

    {
      Timer t("  Good matmul (tiled)");
      good_matmul(C.data(), A.data(), B.data(), N);
    }
    std::cout << "  Expected: ~5-10% cache miss rate, high IPC\n\n";

    {
      Timer t("  Bad matmul (naive)");
      bad_matmul(C.data(), A.data(), B.data(), N);
    }
    std::cout << "  Expected: ~40-60% cache miss rate, low IPC\n\n";

    std::cout << "─── perf mem record 使用指南 ───\n";
    std::cout << "# 记录内存访问延迟 (需要 root):\n";
    std::cout << "$ perf mem record ./perf_deep\n\n";
    std::cout << "# 分析报告:\n";
    std::cout << "$ perf mem report --sort=mem,sym\n";
    std::cout << "  这会显示每个函数的内存访问延迟分布:\n";
    std::cout << "  - L1 hit     (~0.5ns)\n";
    std::cout << "  - L2 hit     (~7ns)\n";
    std::cout << "  - L3 hit     (~30ns)\n";
    std::cout << "  - Local RAM  (~100ns)\n";
    std::cout << "  - Remote RAM (~200ns, NUMA)\n\n";
    std::cout << "# 也可以用 perf record -e 方式:\n";
    std::cout << "$ perf record -e cache-misses,cache-references -g ./perf_deep\n";
    std::cout << "$ perf report --sort=dso,sym --stdio\n";
  }
}

// ============================================================================
// Ex9: 实战 — 用 perf 剖析 ffind 项目
//
// 概念:
//   ffind (/home/limsuig/ffind) 是多线程文件搜索工具。
//   用 perf 找出它的性能瓶颈, 提出优化建议。
//
// 实验步骤:
//   1. 用较大的目录树运行 ffind (如搜索 /usr/include)
//   2. perf record -g ./ffind <pattern> <dir>
//   3. perf report 找出热点函数
//   4. 分析 CPU 时间是花在 IO wait 还是计算上
//   5. 提出至少 3 条优化建议
//
// 任务 (如果 ffind 可用):
//   1. 运行 perf stat ffind "TODO" /usr/include
//   2. 记录 IPC, cache-misses, context-switches
//   3. 如果 IPC < 1.0, 说明是 memory-bound (可能是字符串匹配)
//   4. 如果 context-switches 多, 说明线程数过多
// ============================================================================

namespace ex9_profile_ffind {
  void run() {
    std::cout << "\n===== Ex9: Profile ffind =====\n";

    // 检查 ffind 是否存在
    std::string ffind_path = "/home/limsuig/ffind/build/ffind";
    FILE* f = fopen(ffind_path.c_str(), "r");
    bool ffind_exists = (f != nullptr);
    if (f) fclose(f);

    if (ffind_exists) {
      std::cout << "ffind found at: " << ffind_path << "\n";
      std::cout << "\n─── profiing 命令序列 ───\n\n";
      std::cout << "# Step 1: 整体统计\n";
      std::cout << "$ perf stat -d " << ffind_path << " 'include' /usr/include 2>&1\n\n";
      std::cout << "# Step 2: 热点采样\n";
      std::cout << "$ perf record -g -F 99 " << ffind_path << " 'include' /usr/include\n";
      std::cout << "$ perf report -g --stdio | head -50\n\n";
      std::cout << "# Step 3: cache miss 归因\n";
      std::cout << "$ perf record -e cache-misses -g " << ffind_path << " 'include' /usr/include\n";
      std::cout << "$ perf report --sort=sym --stdio | head -20\n\n";
      std::cout << "# Step 4: 线程分析\n";
      std::cout << "$ perf record -e sched:sched_switch -g " << ffind_path << " ...\n";
      std::cout << "$ perf script | grep -c 'sched_switch'\n\n";
      std::cout << "常见瓶颈和优化方向:\n";
      std::cout << "  A. IPC < 1.0 + cache-misses 高\n";
      std::cout << "     → memory-bound (字符串匹配/读取)\n";
      std::cout << "     → 方案: Boyer-Moore 算法, mmap 映射文件\n";
      std::cout << "  B. context-switches 高\n";
      std::cout << "     → 线程数 > CPU 核心数\n";
      std::cout << "     → 方案: 减少线程, 用 thread pool\n";
      std::cout << "  C. branch-misses 高\n";
      std::cout << "     → 正则引擎回溯过多\n";
      std::cout << "     → 方案: 用 RE2/hyperscan (无回溯正则引擎)\n";
      std::cout << "  D. 大量 time()/stat() syscall\n";
      std::cout << "     → 文件系统元数据操作过多\n";
      std::cout << "     → 方案: 批量 stat, 用 getdents 代替 readdir\n";
    } else {
      std::cout << "ffind not found at " << ffind_path << "\n";
      std::cout << "Build it first:\n";
      std::cout << "$ cd /home/limsuig/ffind && cmake -B build && cmake --build build\n";
    }

    std::cout << "\n─── 通用优化检查表 ───\n";
    std::cout << "□ 1. IPC > 2.0? → compute-bound, 考虑 SIMD/并行\n";
    std::cout << "□ 2. IPC < 0.5? → memory-bound, 检查 cache/数据结构\n";
    std::cout << "□ 3. cache-miss rate > 10%? → 数据布局优化\n";
    std::cout << "□ 4. branch-miss rate > 5%? → 排序/branchless\n";
    std::cout << "□ 5. context-switches > 1000/s? → 减少线程\n";
    std::cout << "□ 6. front-end stalled? → I-cache miss, 函数太大\n";
    std::cout << "□ 7. back-end stalled? → 数据依赖链太长\n";
  }
}

// ============================================================================
// Ex10: 综合实战 — 优化慢程序全流程
//
// 场景:
//   「文本词频统计器」
//   给一个 100MB 的文本文件, 统计每个单词出现的次数,
//   输出 Top-K 高频词。
//
// 流程:
//   Step 1: 实现 v1 (正确但慢) → profile → 找瓶颈
//   Step 2: 应用优化 (至少 3 个):
//     - mmap 代替 ifstream (减少拷贝)
//     - 完美哈希/更好的容器
//     - 多线程分段统计
//     - 避免 std::string 拷贝 (用 string_view)
//   Step 3: 每次优化后用 MicroBench 验证性能
//   Step 4: 写优化报告: 做了什么, 为什么, 提升多少
//
// 任务: 完成 V1 实现, 然后逐步优化
// ============================================================================

namespace ex10_word_count_optimize {
  // ---- V1: 最直接的方式 ----
  struct V1Result {
    std::unordered_map<std::string, int> freq;
    int64_t elapsed_ms;
    int64_t bytes_processed;
  };

  V1Result word_count_v1(const std::string& filepath) {
    V1Result result;
    Timer t;
    std::ifstream file(filepath);
    if (!file) {
      std::cerr << "Cannot open: " << filepath << "\n";
      result.elapsed_ms = t.elapsed_ms();
      return result;
    }

    std::string word;
    // 逐字读取: 字母数字字符组成单词, 其他字符分隔
    char ch;
    while (file.get(ch)) {
      if (std::isalnum(static_cast<unsigned char>(ch))) {
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      } else if (!word.empty()) {
        result.freq[word]++;
        word.clear();
      }
    }
    if (!word.empty()) {
      result.freq[word]++;
    }

    result.elapsed_ms = t.elapsed_ms();
    result.bytes_processed = file.tellg();
    return result;
  }

  // ---- V2: 优化版本 (string_view + reserve) ----
  struct V2Result {
    // 用更高效的哈希: 预分配桶, 避免 rehash
    std::unordered_map<std::string, int> freq;
    int64_t elapsed_ms;

    V2Result() {
      freq.reserve(500000);  // 预分配: 预计 50 万 unique words
    }
  };

  V2Result word_count_v2(const std::string& filepath) {
    V2Result result;
    Timer t;

    // mmap the file (or use large buffer)
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
      result.elapsed_ms = t.elapsed_ms();
      return result;
    }

    std::streamsize size = file.tellg();
    file.seekg(0);
    std::string buffer(size, '\0');
    file.read(buffer.data(), size);

    // 单次扫描, 用 string_view 避免拷贝
    const char* p = buffer.data();
    const char* end = p + size;
    while (p < end) {
      // 跳过非字母数字
      while (p < end && !std::isalnum(static_cast<unsigned char>(*p)))
        ++p;
      const char* start = p;
      // 收集单词
      while (p < end && std::isalnum(static_cast<unsigned char>(*p)))
        ++p;
      if (p > start) {
        // 原地转小写 (buffer 是可写的)
        for (const char* q = start; q < p; ++q)
          const_cast<char&>(*q) = std::tolower(static_cast<unsigned char>(*q));
        std::string word(start, p - start);
        result.freq[std::move(word)]++;
      }
    }

    result.elapsed_ms = t.elapsed_ms();
    return result;
  }

  // 生成测试数据
  std::string generate_test_file(const std::string& path, int64_t target_bytes) {
    std::ofstream out(path);
    // 100 个常见英文单词
    static const char* words[] = {
      "the", "be", "to", "of", "and", "a", "in", "that", "have", "it",
      "for", "not", "on", "with", "he", "as", "you", "do", "at", "this",
      "but", "his", "by", "from", "they", "we", "say", "her", "she", "or",
      "an", "will", "my", "one", "all", "would", "there", "their", "what", "so",
      "up", "out", "if", "about", "who", "get", "which", "go", "me", "when",
      "make", "can", "like", "time", "no", "just", "him", "know", "take", "people",
      "into", "year", "your", "good", "some", "could", "them", "see", "other", "than",
      "then", "now", "look", "only", "come", "its", "over", "think", "also", "back",
      "after", "use", "two", "how", "our", "work", "first", "well", "way", "even",
      "new", "want", "because", "any", "these", "give", "day", "most", "us", "great"
    };

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> word_idx(0, 99);
    std::uniform_int_distribution<int> space_len(1, 5);

    int64_t written = 0;
    while (written < target_bytes) {
      out << words[word_idx(rng)];
      written += std::strlen(words[word_idx(rng)]);
      int spaces = space_len(rng);
      for (int i = 0; i < spaces; ++i) out << ' ';
      written += spaces;
    }
    out.close();
    return path;
  }

  void run() {
    std::cout << "\n===== Ex10: Word Count Optimization =====\n";

    // 生成测试文件 (10MB)
    std::string test_file = "/tmp/wc_test_10mb.txt";
    std::cout << "Generating test file (~10 MB)...\n";
    generate_test_file(test_file, 10'000'000);

    // 检查文件大小
    std::ifstream check(test_file, std::ios::binary | std::ios::ate);
    auto file_size = check.tellg();
    check.close();
    std::cout << "File size: " << (file_size / 1024.0 / 1024.0) << " MB\n\n";

    // V1 Benchmark
    int64_t v1_time = 0;
    {
      auto result = word_count_v1(test_file);
      v1_time = result.elapsed_ms;
      std::cout << "V1: " << v1_time << " ms"
                << " | unique words=" << result.freq.size()
                << " | top word='" << std::max_element(
                  result.freq.begin(), result.freq.end(),
                  [](auto& a, auto& b) { return a.second < b.second; }
                )->first << "'"
                << "\n";
    }

    // V2 Benchmark
    int64_t v2_time = 0;
    {
      auto result = word_count_v2(test_file);
      v2_time = result.elapsed_ms;
      std::cout << "V2: " << v2_time << " ms"
                << " | unique words=" << result.freq.size()
                << " | top word='" << std::max_element(
                  result.freq.begin(), result.freq.end(),
                  [](auto& a, auto& b) { return a.second < b.second; }
                )->first << "'"
                << "\n";
    }

    // 对比
    if (v1_time > 0 && v2_time > 0) {
      std::cout << "\nSpeedup: " << (double)v1_time / v2_time << "x\n";
    }

    // 优化报告
    std::cout << "\n════ Optimization Report ════\n";
    std::cout << "V1 approach:\n";
    std::cout << "  - std::ifstream::get() 逐字节读取 (1 syscall per char?)\n";
    std::cout << "  - std::string::operator+= 逐字符追加 (多次 realloc)\n";
    std::cout << "  - unordered_map 未 reserve (多次 rehash)\n\n";
    std::cout << "V2 optimizations:\n";
    std::cout << "  1. 全文读入内存 (单次 read syscall)\n";
    std::cout << "  2. 指针扫描代替 ifstream::get (no buffer overhead)\n";
    std::cout << "  3. unordered_map::reserve 预分配桶 (避免 rehash)\n";
    std::cout << "  4. 原地转小写 (避免 per-char tolower 调用)\n";
    std::cout << "  5. std::move(word) 避免 string 拷贝\n\n";
    std::cout << "Further optimizations (V3+):\n";
    std::cout << "  - mmap() 零拷贝映射文件\n";
    std::cout << "  - 多线程: 分块→各线程独立统计→合并\n";
    std::cout << "  - 完美哈希: 如果词典固定, 用 gperf\n";
    std::cout << "  - SSE/AVX: 用 SIMD 检测字母数字字符边界\n";
    std::cout << "  - flat_hash_map: absl::flat_hash_map 或 folly::F14\n";

    // 清理
    std::remove(test_file.c_str());
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Month 4 / Week 18: perf & Profiling\n";
  std::cout << "══════════════════════════════════════════════\n";

  // 运行哪个练习?
  // 默认全部, 可用参数选择: ./perf_deep 1 2 5
  auto should_run = [&](int n) -> bool {
    if (argc <= 1) return true;  // 无参数 = 全部运行
    for (int i = 1; i < argc; ++i)
      if (std::atoi(argv[i]) == n) return true;
    return false;
  };

  if (should_run(1))  ex1_perf_stat_basics::run();
  if (should_run(2))  ex2_perf_record_report::run();
  if (should_run(3))  ex3_perf_top_trace::run();
  if (should_run(4))  ex4_flamegraph::run();
  if (should_run(5))  ex5_micro_benchmark::run();
  if (should_run(6))  ex6_custom_profiler::run();
  if (should_run(7))  ex7_complexity_bench::run();
  if (should_run(8))  ex8_cache_miss_attribution::run();
  if (should_run(9))  ex9_profile_ffind::run();
  if (should_run(10)) ex10_word_count_optimize::run();

  std::cout << "\n Week 18 Done! 🎉\n";
  std::cout << "Next: Try these perf commands on the binary:\n";
  std::cout << "  perf stat -d ./perf_deep <ex_num>\n";
  std::cout << "  perf record -g -F 99 ./perf_deep <ex_num>\n";
  std::cout << "  perf report -g --stdio\n";
  return 0;
}
