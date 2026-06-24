// ============================================================================
// Month 4: 极致性能 — Beyond the Code
// Week 19: gdb 深度调试 — 程序的可观测性
//
// 核心哲学:
//   「调试是比写代码更重要的技能 — 你花在读代码上的时间远多于写代码」
//   「不要用 printf 调试 — gdb 能让你在时间中穿梭」
//
//   gdb (GNU Debugger) 是 Linux 下最强大的调试器:
//   - 断点 (breakpoint): 让程序停在任意位置
//   - 观察点 (watchpoint): 监视变量何时被修改
//   - 反向调试 (reverse debugging): 回到过去
//   - Python 脚本: 自定义命令扩展 gdb
//
// 本周目标:
//   - 掌握 gdb 的进阶断点技术 (条件/临时/命令列表)
//   - 学会 watchpoint / catchpoint / core dump 分析
//   - 理解多线程调试的关键技巧
//   - 体验 rr (Mozilla 的时间旅行调试器)
//   - 建立「不用 printf 也能高效调试」的信心
//
// 10 个练习, 每个都包含可操作的 gdb 会话示例
//
// 编译: cmake -B build && cmake --build build
// 注意: 必须用 -g -O0 编译 (已在 CMakeLists.txt 设置)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

// ============================================================================
// 工具
// ============================================================================

static volatile int64_t g_sink = 0;

// 通过命令行参数选择要运行的练习
static int g_selected_ex = 99;  // 99 = run last exercise only (for gdb sessions)

// ============================================================================
// Ex1: 进阶断点 — 条件断点、临时断点、命令列表
//
// 概念:
//   break (b): 设置断点
//   tbreak:    临时断点 (只停一次)
//   condition: 给断点加条件 (条件为真才停)
//   commands:  断点触发后自动执行命令列表
//   ignore:    跳过断点 N 次
//
// 常见场景:
//   「循环到第 500 次才出 bug」→ tbreak file:line if i == 500
//   「想看某个函数被谁调用」→ commands 1 → bt → continue → end
//
// 任务: 用 gdb 调试以下代码, 完成指定的调试任务
// ============================================================================

namespace ex1_advanced_breakpoints {
  // 模拟: 数据库连接池, 偶尔在第 347 次 borrow 时返回 nullptr
  class ConnectionPool {
    std::vector<int> _connections;
    int _borrow_count = 0;
  public:
    ConnectionPool() {
      for (int i = 0; i < 100; ++i) _connections.push_back(i + 1000);
    }

    int* borrow() {
      _borrow_count++;
      // BUG: 第 347 次 borrow 返回 nullptr (模拟)
      if (_borrow_count == 347) return nullptr;
      if (_connections.empty()) return nullptr;
      int* conn = &_connections.back();
      return conn;
    }

    void crash_if_null(int* conn) {
      if (!conn) {
        // BUG 触发点: 解引用空指针
        std::cerr << "About to crash...\n";
        *conn = 42;  // ← SEGFAULT here
      }
    }
  };

  void fibonacci_bug() {
    // BUG: 斐波那契计算, n=45 时 int 溢出导致负数
    std::vector<int> fib(100);
    fib[0] = 0; fib[1] = 1;
    for (int i = 2; i < 100; ++i) {
      fib[i] = fib[i-1] + fib[i-2];
      // BUG: i=46 时 fib[46] 变成负数 (int 溢出)
    }
    // 找到第一个负数出现的位置
    for (size_t i = 0; i < fib.size(); ++i) {
      if (fib[i] < 0) {
        std::cout << "First negative at fib[" << i << "] = " << fib[i] << "\n";
        break;
      }
    }
  }

  void run() {
    std::cout << "\n===== Ex1: Advanced Breakpoints =====\n";
    std::cout << "This exercise is meant to be debugged with gdb.\n\n";

    std::cout << "─── gdb 命令速查 ───\n";
    std::cout << "# 条件断点: 只在特定条件下停止\n";
    std::cout << "$ gdb ./gdb_deep\n";
    std::cout << "(gdb) b ConnectionPool::borrow if _borrow_count >= 347\n";
    std::cout << "(gdb) run 1\n\n";
    std::cout << "# 断点命令列表: 自动执行\n";
    std::cout << "(gdb) b borrow\n";
    std::cout << "(gdb) commands\n";
    std::cout << "> silent         # 不打印断点信息\n";
    std::cout << "> p _borrow_count\n";
    std::cout << "> continue\n";
    std::cout << "> end\n\n";
    std::cout << "# 临时断点 (只停一次):\n";
    std::cout << "(gdb) tbreak crash_if_null\n";
    std::cout << "(gdb) run 1\n\n";
    std::cout << "# 跳过断点 N 次:\n";
    std::cout << "(gdb) ignore 1 346  # 忽略断点 1 的前 346 次触发\n";
    std::cout << "(gdb) run 1\n\n";
    std::cout << "# 在 fibonacci_bug 中设置条件断点:\n";
    std::cout << "(gdb) b fibonacci_bug if fib[i] < 0\n";
    std::cout << "(gdb) run 1\n";

    // 实际运行触发 BUG
    ConnectionPool pool;
    for (int i = 0; i < 400; ++i) {
      int* conn = pool.borrow();
      if (!conn) {
        std::cout << "Null at borrow #" << (i + 1) << " — but not crashing here\n";
        // 在 gdb 中运行才会触发 crash_if_null 中的 SEGFAULT
        break;
      }
    }

    fibonacci_bug();
    std::cout << "\nTip: Run with './gdb_deep 1' under gdb to practice breakpoints\n";
  }
}

// ============================================================================
// Ex2: Watchpoints — 监视变量何时被修改
//
// 概念:
//   watch:  硬件监视点 (通过 CPU 调试寄存器), 变量被写时触发
//   rwatch: 读监视点 (变量被读时触发)
//   awatch: 读写监视点 (读写任一触发)
//
//   watch 的魔力: 「我不知道是谁改了这个变量, 但我可以抓住它!」
//
//   限制: 硬件 watchpoint 只有 4 个 (x86_64 的 DR0-DR3 寄存器)
//   软件 watchpoint 每步都检查 → 极慢但无数量限制
//
// 任务: 用 watchpoint 找到以下代码中的神秘写入者
// ============================================================================

namespace ex2_watchpoints {
  struct Config {
    int max_connections = 100;
    int timeout_ms = 5000;
    int retry_count = 3;
    char padding[32];  // 防止相邻变量干扰
  };

  Config g_config;

  // 函数 A: 正常修改
  void init_config() {
    g_config.max_connections = 200;
    g_config.timeout_ms = 10000;
  }

  // 函数 B: 看似无害
  void log_config() {
    std::cout << "Config: max_conn=" << g_config.max_connections
              << " timeout=" << g_config.timeout_ms << "\n";
  }

  // 函数 C: BUG! buf 溢出, 第 60 字节覆盖到 Config::timeout_ms
  void parse_user_input(const char* input) {
    char buf[48];
    // BUG: 故意用危险的 strcpy, 长输入覆盖 g_config.timeout_ms
    // 输入长度 > 48 就会溢出到全局变量区域
    std::strcpy(buf, input);
    std::cout << "Parsed: " << buf << "\n";
  }

  // 函数 D: 多线程竞态
  std::atomic<bool> g_running{true};
  int g_shared_counter = 0;  // BUG: 不是 atomic, 被多线程写

  void racy_writer(int id) {
    while (g_running) {
      g_shared_counter++;  // ← 竞态条件
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

  void run() {
    std::cout << "\n===== Ex2: Watchpoints =====\n";

    // Part 1: 用 watchpoint 找到谁改了 timeout_ms
    init_config();
    log_config();

    std::cout << "\n─── gdb watchpoint 练习 ───\n";
    std::cout << "# 监视 timeout_ms 何时被修改:\n";
    std::cout << "(gdb) watch g_config.timeout_ms\n";
    std::cout << "(gdb) run 2\n";
    std::cout << "# gdb 会在 timeout_ms 被修改时停止, 显示是谁改的\n\n";

    // 触发 BUG: 超长输入覆盖 timeout_ms
    char malicious_input[128];
    std::memset(malicious_input, 'A', 120);
    malicious_input[119] = '\0';

    std::cout << "Before parse: timeout_ms=" << g_config.timeout_ms << "\n";
    parse_user_input(malicious_input);
    std::cout << "After parse:  timeout_ms=" << g_config.timeout_ms
              << " (CHANGED! Who did it?)\n\n";

    std::cout << "# 更多 watchpoint 技巧:\n";
    std::cout << "(gdb) info watchpoints          # 列出所有 watchpoint\n";
    std::cout << "(gdb) rwatch g_config.max_connections  # 读监视点\n";
    std::cout << "(gdb) awatch g_shared_counter   # 读写监视点\n";
    std::cout << "(gdb) watch -l g_config.timeout_ms  # -l = 监视地址而非变量\n\n";

    // Part 2: 竞态条件 watchpoint
    std::cout << "─── Racy writer demo (Ctrl-C to stop) ───\n";
    std::thread t1(racy_writer, 1);
    std::thread t2(racy_writer, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g_running = false;
    t1.join();
    t2.join();
    std::cout << "g_shared_counter = " << g_shared_counter
              << " (may differ from expected due to race)\n";
    std::cout << "Try: (gdb) watch g_shared_counter thread 2\n";
  }
}

// ============================================================================
// Ex3: Catchpoints — 捕获异常/系统调用/fork
//
// 概念:
//   catch throw:  在 C++ 异常被抛出时停止
//   catch catch:  在 C++ 异常被捕获时停止
//   catch syscall: 在系统调用时停止 (可过滤特定 syscall)
//   catch fork:   在 fork() 时停止
//   catch exec:   在 exec() 时停止
//
// 任务: 用 catchpoints 追踪异常抛出和系统调用
// ============================================================================

namespace ex3_catchpoints {
  // 深层异常抛出: 调用栈很深, 异常被顶层 catch
  void level3() {
    throw std::runtime_error("Something went wrong in level3");
  }

  void level2() {
    level3();
  }

  void level1() {
    try {
      level2();
    } catch (const std::exception& e) {
      std::cout << "Caught: " << e.what() << "\n";
    }
  }

  // 自定义异常
  class DatabaseError : public std::exception {
    std::string _msg;
  public:
    DatabaseError(const std::string& msg) : _msg(msg) {}
    const char* what() const noexcept override { return _msg.c_str(); }
  };

  void query_database() {
    throw DatabaseError("Connection refused: timeout after 30s");
  }

  void run() {
    std::cout << "\n===== Ex3: Catchpoints =====\n";

    std::cout << "─── gdb catchpoint 命令 ───\n";
    std::cout << "# 捕获所有 C++ 异常抛出:\n";
    std::cout << "(gdb) catch throw\n";
    std::cout << "(gdb) run 3\n";
    std::cout << "# gdb 在 level3() 的 throw 处停止, 此时调用栈完整!\n\n";

    std::cout << "# 捕获特定类型:\n";
    std::cout << "(gdb) catch throw if strcmp(\n";
    std::cout << "  (char*)((std::runtime_error*)\$rdi)->what(),\n";
    std::cout << "  \"refused\") != 0\n\n";

    std::cout << "# 捕获系统调用:\n";
    std::cout << "(gdb) catch syscall write\n";
    std::cout << "(gdb) catch syscall open\n";
    std::cout << "(gdb) info breakpoints  # 查看所有断点/catchpoints\n\n";

    std::cout << "# 其他有用的 catchpoints:\n";
    std::cout << "(gdb) catch fork     # fork 时停止\n";
    std::cout << "(gdb) catch exec     # exec 时停止\n";
    std::cout << "(gdb) catch signal   # 收到信号时停止\n";
    std::cout << "(gdb) catch signal SIGSEGV  # SEGFAULT 时停止\n\n";

    // 运行演示
    std::cout << "Running exception demo:\n";
    level1();

    std::cout << "\nRunning custom exception demo:\n";
    try {
      query_database();
    } catch (const DatabaseError& e) {
      std::cout << "Caught DatabaseError: " << e.what() << "\n";
    }

    std::cout << "\nTip: (gdb) catch throw → (gdb) bt → see full throw-site stack!\n";
  }
}

// ============================================================================
// Ex4: Core Dump 分析 — 从崩溃现场重建真相
//
// 概念:
//   Core dump 是进程崩溃时的内存快照, 包含:
//   - 所有线程的寄存器状态
//   - 调用栈
//   - 堆/栈内容
//   - 打开的文件描述符
//
//   即使程序已经退出, core dump 也能让你「回到过去」。
//
// 启用 coredump:
//   $ ulimit -c unlimited
//   $ echo '/tmp/core.%e.%p' | sudo tee /proc/sys/kernel/core_pattern
//
// 任务:
//   1. 故意触发 SEGFAULT, 保存 core dump
//   2. 用 gdb <binary> <core> 加载 core 文件
//   3. bt / info registers / info threads / frame N 分析
// ============================================================================

namespace ex4_core_dump {
  // 场景: 链表操作 bug — 释放后使用 (use-after-free)
  struct Node {
    int value;
    Node* next;
  };

  Node* create_list(int n) {
    Node* head = new Node{0, nullptr};
    Node* cur = head;
    for (int i = 1; i < n; ++i) {
      cur->next = new Node{i, nullptr};
      cur = cur->next;
    }
    return head;
  }

  void delete_list(Node* head) {
    while (head) {
      Node* tmp = head;
      head = head->next;
      delete tmp;
    }
  }

  void print_and_crash(Node* head) {
    // 打印链表
    Node* p = head;
    while (p) {
      std::cout << p->value << " ";
      p = p->next;
    }
    std::cout << "\n";

    // BUG: 在已经 deleted 的链表上继续访问
    // 期望: head 仍指向原来的节点, 但内存已被释放
    // 行为: undefined behavior — 可能 SIGSEGV, 可能读到垃圾
    Node* second = head->next;  // ← use-after-free!
    std::cout << "Second node value: " << second->value << "\n";
  }

  // 场景 2: stack buffer overflow (覆盖返回地址)
  void overflow_buffer() {
    char buf[16];
    // 故意写超长数据
    std::strcpy(buf, "THIS_STRING_IS_WAY_TOO_LONG_FOR_16_BYTES_BUF");
    std::cout << "If you see this, stack wasn't corrupted... yet\n";
  }

  void run() {
    std::cout << "\n===== Ex4: Core Dump Analysis =====\n";

    // 检查 core dump 设置
    std::cout << "─── Enable core dumps ───\n";
    std::cout << "$ ulimit -c unlimited\n";
    std::cout << "$ cat /proc/sys/kernel/core_pattern\n";
    std::cout << "  → should show a path like 'core' or '/tmp/core.%e.%p'\n\n";

    std::cout << "─── gdb core dump 分析流程 ───\n";
    std::cout << "# 1. 触发崩溃并生成 core:\n";
    std::cout << "$ ./gdb_deep 4\n\n";
    std::cout << "# 2. 用 gdb 加载 core:\n";
    std::cout << "$ gdb ./gdb_deep core\n\n";
    std::cout << "# 3. 核心命令:\n";
    std::cout << "(gdb) bt                  # 调用栈 — 从哪里崩溃\n";
    std::cout << "(gdb) bt full             # 调用栈 + 每个 frame 的局部变量\n";
    std::cout << "(gdb) info registers      # 寄存器状态\n";
    std::cout << "(gdb) frame 0             # 跳到崩溃点\n";
    std::cout << "(gdb) list                # 显示崩溃点源码\n";
    std::cout << "(gdb) p *head             # 检查指针内容\n";
    std::cout << "(gdb) x/16xb \$rsp         # 检查栈内存\n";
    std::cout << "(gdb) info threads        # 所有线程\n";
    std::cout << "(gdb) thread apply all bt # 所有线程的调用栈\n\n";

    // 实际触发 bug
    std::cout << "─── Triggering use-after-free ───\n";
    Node* list = create_list(10);
    std::cout << "List before delete: ";
    delete_list(list);

    std::cout << "\nAccessing deleted list...\n";
    // 注意: 在 core 分析模式下才触发, 这里用 try-catch 保护
    // 如果想生成 core dump, 注释掉 try-catch
    try {
      print_and_crash(list);
    } catch (...) {
      std::cout << "Caught crash (no core dump generated in this run)\n";
    }

    std::cout << "\n─── Stack overflow demo ───\n";
    std::cout << "Skipping buffer overflow (uncomment to test):\n";
    // overflow_buffer();  // uncomment to trigger stack smash
    std::cout << "To test buffer overflow, uncomment overflow_buffer() call\n";
    std::cout << "Then: $ gdb ./gdb_deep\n";
    std::cout << "(gdb) run 4\n";
    std::cout << "(gdb) bt  # ← corrupted stack may show ?? instead of function names\n";
  }
}

// ============================================================================
// Ex5: 多线程调试 — 别再 printf 每个线程了
//
// 概念:
//   gdb 支持完整的多线程调试:
//   - info threads: 列出所有线程
//   - thread N: 切换到线程 N
//   - thread apply all bt: 所有线程的调用栈
//   - set scheduler-locking on: 只运行当前线程 (其他冻结)
//   - set scheduler-locking step: 单步时只运行当前线程
//
// 任务:
//   1. 调试死锁 (deadlock)
//   2. 调试活锁 (livelock)
//   3. 使用 scheduler-locking 隔离线程
// ============================================================================

namespace ex5_multithreaded_debugging {
  // --- Deadlock demo ---
  class DeadlockDemo {
    std::mutex _m1, _m2;
    int _counter = 0;
  public:
    void thread_a() {
      std::lock_guard l1(_m1);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::lock_guard l2(_m2);  // ← waits for _m2, held by thread_b
      _counter++;
    }

    void thread_b() {
      std::lock_guard l2(_m2);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::lock_guard l1(_m1);  // ← waits for _m1, held by thread_a
      _counter++;
    }

    void run_deadlock() {
      std::thread t1(&DeadlockDemo::thread_a, this);
      std::thread t2(&DeadlockDemo::thread_b, this);
      // DEADLOCK: t1 holds _m1, waits for _m2; t2 holds _m2, waits for _m1
      std::cout << "Threads started — deadlock imminent...\n";
      std::cout << "Run under gdb: (gdb) run 5\n";
      std::cout << "After deadlock, Ctrl-C and:\n";
      std::cout << "(gdb) info threads\n";
      std::cout << "(gdb) thread apply all bt\n";
      std::cout << "(gdb) thread 1\n";
      std::cout << "(gdb) frame 2         # find where it's blocked\n";
      std::cout << "(gdb) p _m1._M_owner  # check who owns the mutex\n";
      t1.join();
      t2.join();
    }
  };

  // --- 活锁 demo (条件变量丢失通知) ---
  class LivelockDemo {
    std::mutex _m;
    std::condition_variable _cv;
    bool _ready = false;
    int _data = 0;
  public:
    void producer() {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      {
        std::lock_guard lk(_m);
        _data = 42;
        _ready = true;
      }
      _cv.notify_one();
    }

    void consumer() {
      std::unique_lock lk(_m);
      // BUG?: 如果用 _cv.wait(lk) 而非 wait(lk, pred), 可能有 spurious wakeup
      // 正确: _cv.wait(lk, [this]{ return _ready; });
      while (!_ready) {  // 手动循环也是对的
        _cv.wait(lk);
      }
      std::cout << "Consumer got: " << _data << "\n";
    }
  };

  void run() {
    std::cout << "\n===== Ex5: Multi-threaded Debugging =====\n";

    std::cout << "─── gdb 多线程命令速查 ───\n";
    std::cout << "(gdb) info threads              # 列出所有线程\n";
    std::cout << "(gdb) thread 2                  # 切换到线程 2\n";
    std::cout << "(gdb) thread apply all bt       # 所有线程的 backtrace\n";
    std::cout << "(gdb) thread apply 2-4 bt       # 线程 2-4 的 backtrace\n";
    std::cout << "(gdb) set scheduler-locking on  # 冻结其他线程\n";
    std::cout << "(gdb) set scheduler-locking step # 单步时只运行当前线程\n";
    std::cout << "(gdb) set scheduler-locking off  # 恢复正常\n";
    std::cout << "(gdb) break thread_a thread 1   # 只在线程 1 中触发断点\n\n";

    std::cout << "─── Deadlock demo ───\n";
    std::cout << "Run 'gdb ./gdb_deep' and type:\n";
    std::cout << "(gdb) b DeadlockDemo::thread_a\n";
    std::cout << "(gdb) b DeadlockDemo::thread_b\n";
    std::cout << "(gdb) run 5\n";
    std::cout << "When deadlocked, Ctrl-C then → info threads → thread apply all bt\n\n";

    // 实际运行死锁 (如果你愿意等...)
    std::cout << "Running deadlock demo (will deadlock, press Ctrl-C to stop):\n";
    DeadlockDemo dd;
    // dd.run_deadlock();  // uncomment to experience deadlock

    std::cout << "\nDeadlock detection tip:\n";
    std::cout << "$ gdb -batch -ex 'run 5' -ex 'thread apply all bt' ./gdb_deep\n";
    std::cout << "$ # 或运行时 attach: gdb -p $(pgrep gdb_deep)\n";
  }
}

// ============================================================================
// Ex6: Reverse Debugging (rr) — 时间旅行调试
//
// 概念:
//   rr (Record & Replay) 是 Mozilla 开发的时间旅行调试器,
//   基于 gdb 协议, 支持:
//   - reverse-continue (rc): 反向运行到上一个断点
//   - reverse-step (rs):     反向单步执行
//   - reverse-next (rn):     反向逐过程
//   - reverse-finish:        反向运行到当前函数被调用之前
//
//   原理: rr 记录所有非确定性输入 (系统调用, 信号, 共享内存),
//   然后可以无限次重放, 每次重放完全一致。
//
// 安装:
//   $ sudo apt install rr    # Ubuntu/Debian
//   $ sudo dnf install rr    # Fedora
//
// 任务: 用 rr 找到以下代码中的 heisenbug
//   (heisenbug = 调试时不出现, 正常运行出现)
// ============================================================================

namespace ex6_reverse_debugging {
  // Heisenbug: 有时崩溃, 有时不崩溃 — 取决于未初始化的内存
  // (在调试模式下, 内存通常被初始化为 0 → bug 消失了!)
  int heisenbug_function() {
    int uninitialized;  // BUG: 未初始化
    // 在 Debug 模式下, uninitialized 可能是 0
    // 在 Release 模式下, uninitialized 可能是任何值
    if (uninitialized == 0) {
      return 42;  // 正常路径
    } else {
      // 异常路径: 可能触发越界访问
      return *(int*)(nullptr);  // CRASH!
    }
  }

  // 更真实的 heisenbug: 竞争条件
  class HeisenCounter {
    int _count = 0;
    std::mutex _m;
  public:
    void safe_increment() {
      std::lock_guard lk(_m);
      _count++;
    }
    void unsafe_increment() {
      _count++;  // BUG: no lock
    }

    int get() const { return _count; }
  };

  void run() {
    std::cout << "\n===== Ex6: Reverse Debugging (rr) =====\n";

    std::cout << "─── rr 工作流 ───\n";
    std::cout << "# 1. 录制 (record):\n";
    std::cout << "$ rr record ./gdb_deep 6\n\n";
    std::cout << "# 2. 重放 (replay):\n";
    std::cout << "$ rr replay\n";
    std::cout << "# 这会启动 gdb, 你可以像普通 gdb 一样操作\n\n";
    std::cout << "# 3. 反向调试命令:\n";
    std::cout << "(gdb) break crash_function\n";
    std::cout << "(gdb) continue         # 到达崩溃点\n";
    std::cout << "(gdb) reverse-continue  # 后退到上一个断点!\n";
    std::cout << "(gdb) reverse-step      # 后退一条指令\n";
    std::cout << "(gdb) reverse-next      # 后退一行\n";
    std::cout << "(gdb) reverse-finish    # 退回到调用者\n";
    std::cout << "(gdb) set exec-direction reverse  # 切换默认方向\n\n";

    std::cout << "─── rr 使用场景 ───\n";
    std::cout << "1. 在崩溃点设置 watch -l addr\n";
    std::cout << "   → reverse-continue → 自动回到 addr 被修改的时刻\n";
    std::cout << "2. heisenbug: 先录制 100 次运行, 找到失败的来重放\n";
    std::cout << "   $ for i in {1..100}; do rr record ./buggy_prog; done\n";
    std::cout << "   $ rr replay -a  # 列出所有录制 → 选失败的\n\n";

    std::cout << "─── Chaos mode ───\n";
    std::cout << "$ rr record --chaos ./gdb_deep  # 随机化线程调度\n";
    std::cout << "$ rr replay -a  # 多个录制中选一个出 bug 的\n\n";

    std::cout << "─── Running heisenbug demo ───\n";
    // heisenbug 太危险, 只讲解
    std::cout << "Heisenbug: uninitialized variable behavior depends on stack state\n";
    std::cout << "Without rr: if bug doesn't reproduce under gdb, you're stuck\n";
    std::cout << "With rr:    record once without gdb → replay with full gdb control\n";
  }
}

// ============================================================================
// Ex7: gdb Python 脚本 — 自定义命令
//
// 概念:
//   gdb 内嵌 Python 解释器, 可以:
//   - 写自定义命令 (像 warden 命令)
//   - 美化打印 (pretty printer) 自定义类型
//   - 自动化调试 (脚本化断点/检查)
//
// 常用 Python API:
//   gdb.execute("bt")            # 执行 gdb 命令
//   gdb.parse_and_eval("var")    # 获取变量值
//   gdb.selected_frame()         # 当前栈帧
//   gdb.Breakpoint("file:line")  # 创建断点
//
// 任务: 编写一个 gdb Python 脚本文件 (单独文件)
//   1. 自定义命令 `leak-check` 检查容器是否泄漏
//   2. Pretty printer for 自定义类型
// ============================================================================

namespace ex7_gdb_python {
  // 自定义数据结构 (需要 pretty printer 才能友好显示)
  struct Vec3 {
    double x, y, z;
  };

  class BoundingBox {
    Vec3 _min, _max;
  public:
    BoundingBox(double x1, double y1, double z1,
                double x2, double y2, double z2)
      : _min{x1, y1, z1}, _max{x2, y2, z2} {}
    const Vec3& min() const { return _min; }
    const Vec3& max() const { return _max; }
    double volume() const {
      return (_max.x - _min.x) * (_max.y - _min.y) * (_max.z - _min.z);
    }
  };

  // 查找: 哪个函数在调用 find_buggy_value 时传入了负数参数
  int find_buggy_value(int input) {
    if (input < 0) {
      // BUG: 输入不应该为负数, 需要找到调用者
      std::cerr << "BUG: negative input " << input << "\n";
      return -1;
    }
    return input * 2;
  }

  void caller_a() { find_buggy_value(10); }
  void caller_b() { find_buggy_value(-5); }  // ← 这里传入了负数
  void caller_c() { find_buggy_value(20); }

  void run() {
    std::cout << "\n===== Ex7: gdb Python Scripting =====\n";

    BoundingBox box(0, 0, 0, 10, 20, 30);
    std::cout << "Box volume: " << box.volume() << "\n";

    std::cout << "\n─── gdb Python Script 示例 ───\n";
    std::cout << "# 保存以下内容到 ~/.gdbinit 或 gdb_scripts.py:\n\n";

    std::cout << "```python\n";
    std::cout << "import gdb\n\n";
    std::cout << "# 自定义命令: warden\n";
    std::cout << "class Warden(gdb.Command):\n";
    std::cout << "    \"\"\"Warden 内存检查命令\"\"\"\n";
    std::cout << "    def __init__(self):\n";
    std::cout << "        super().__init__('warden', gdb.COMMAND_USER)\n";
    std::cout << "    def invoke(self, arg, from_tty):\n";
    std::cout << "        # 打印当前线程和调用栈\n";
    std::cout << "        gdb.execute('info threads')\n";
    std::cout << "        gdb.execute('bt')\n";
    std::cout << "        print('[warden] checkpoint')\n";
    std::cout << "Warden()\n\n";
    std::cout << "# Pretty printer for Vec3\n";
    std::cout << "class Vec3Printer:\n";
    std::cout << "    def __init__(self, val):\n";
    std::cout << "        self.val = val\n";
    std::cout << "    def to_string(self):\n";
    std::cout << "        x = self.val['x']\n";
    std::cout << "        y = self.val['y']\n";
    std::cout << "        z = self.val['z']\n";
    std::cout << "        return f'Vec3({x}, {y}, {z})'\n";
    std::cout << "```\n\n";

    std::cout << "# 使用:\n";
    std::cout << "(gdb) source gdb_scripts.py    # 加载脚本\n";
    std::cout << "(gdb) warden                   # 执行自定义命令\n";
    std::cout << "(gdb) p box                    # 使用 pretty printer\n\n";

    // 运行调用链 — 用 gdb 找到谁传了负数
    caller_a();
    caller_b();  // ← BUG 来源
    caller_c();

    std::cout << "\n─── Python 断点技巧 ───\n";
    std::cout << "(gdb) py\n";
    std::cout << "> bp = gdb.Breakpoint('find_buggy_value')\n";
    std::cout << "> bp.condition = 'input < 0'\n";
    std::cout << "> bp.commands = 'bt\\np input\\ncontinue'\n";
    std::cout << "> end\n";
    std::cout << "# 这会在 input < 0 时自动打印 backtrace!\n";
  }
}

// ============================================================================
// Ex8: 动态断点注入 — 不用重新编译就加日志
//
// 概念:
//   有时候不能重新编译 (生产环境 / 调试客户环境),
//   但可以用 gdb 动态注入代码:
//
//   1. dprintf: 动态 printf — 在断点处执行 printf 并自动 continue
//   2. compile code: 现场编译并注入 C 代码 (需要 gcc 可用)
//   3. 修改寄存器/内存: set var = value
//
// 任务: 用 dprintf 和 compile code 对运行中的程序加日志
// ============================================================================

namespace ex8_dynamic_instrumentation {
  static int g_request_count = 0;
  static int g_error_count = 0;

  void handle_request(int client_id, const std::string& path) {
    g_request_count++;
    // 模拟: 某些请求路径太长 → 处理超时
    if (path.size() > 50) {
      g_error_count++;
    }
    // 处理逻辑...
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  void process_batch() {
    std::vector<std::string> paths = {
      "/api/users", "/api/orders", "/api/products",
      "/api/very/long/path/that/might/cause/problems/with/our/server/xxxxxxxxxxxxxxxxx",
      "/api/health", "/api/metrics"
    };
    for (int i = 0; i < 100; ++i) {
      for (const auto& path : paths) {
        handle_request(i, path);
      }
    }
    std::cout << "Processed " << g_request_count << " requests, "
              << g_error_count << " errors\n";
  }

  void run() {
    std::cout << "\n===== Ex8: Dynamic Instrumentation =====\n";

    // 先运行一次 (无注入)
    process_batch();

    std::cout << "\n─── gdb 动态注入技巧 ───\n";
    std::cout << "# 1. dprintf — 动态 printf:\n";
    std::cout << "(gdb) dprintf handle_request, \"[req] client=%d path=%s\\n\", client_id, path.c_str()\n";
    std::cout << "(gdb) run 8\n";
    std::cout << "# dprintf 默认会继续执行 (不停止), 像加了 printf 一样!\n\n";

    std::cout << "# 2. 条件 dprintf (只记录长路径):\n";
    std::cout << "(gdb) dprintf handle_request, \"LONG PATH: %s\\n\", path.c_str()\n";
    std::cout << "(gdb) cond 1 path.size() > 50\n\n";

    std::cout << "# 3. 修改变量值 (现场打补丁):\n";
    std::cout << "(gdb) b handle_request\n";
    std::cout << "(gdb) commands\n";
    std::cout << "> silent\n";
    std::cout << "> set var path = \"/replaced\"\n";  // 太危险, 只是概念
    std::cout << "> continue\n";
    std::cout << "> end\n\n";

    std::cout << "# 4. compile code — 现场编译注入 (需要 gcc):\n";
    std::cout << "(gdb) compile code printf(\"g_error_count = %d\\n\", g_error_count)\n";
    std::cout << "(gdb) compile code ++g_error_count  # 甚至可以修改!\n\n";

    std::cout << "# 5. 不用断点, 直接调用函数:\n";
    std::cout << "(gdb) call (void)printf(\"Debug: counter=%d\\n\", g_request_count)\n\n";

    std::cout << "Note: dprintf 是「不打断程序运行就能加日志」的最佳方式\n";
    std::cout << "      相比 printf 调试: 不需要改代码/重新编译/部署\n";
  }
}

// ============================================================================
// Ex9: 内存调试 — 堆损坏/栈溢出/内存泄漏
//
// 概念:
//   gdb 本身不是内存调试器, 但配合以下工具可以:
//   - AddressSanitizer (ASan): 检测 heap/stack/global buffer overflow + use-after-free
//   - Valgrind: 检测内存泄漏 + 未初始化读取
//   - gdb + malloc hooks: 追踪分配
//
//   gdb 自身可以:
//   - x/Nx addr:    检查任意内存地址
//   - info proc mappings: 查看进程内存映射
//   - find /b start, end, pattern: 在内存中搜索
//
// 任务:
//   1. 实现一个带内存 bug 的程序
//   2. 用 gdb 配合 ASan 找到 bug
// ============================================================================

namespace ex9_memory_debugging {
  // BUG 1: off-by-one (堆溢出)
  void heap_overflow() {
    int* arr = new int[10];
    for (int i = 0; i <= 10; ++i) {  // BUG: i <= 10, 应该是 i < 10
      arr[i] = i * 2;
    }
    std::cout << "Heap overflow: arr[10] = " << arr[10] << "\n";
    delete[] arr;
  }

  // BUG 2: use-after-return (栈指针返回)
  int* stack_pointer_bug() {
    int local = 42;
    return &local;  // BUG: 返回局部变量地址
  }

  // BUG 3: 内存泄漏
  void memory_leak() {
    for (int i = 0; i < 1000; ++i) {
      int* leaked = new int(i);
      // BUG: 忘记 delete
      g_sink += *leaked;
    }
  }

  // BUG 4: double-free
  void double_free() {
    int* p = new int(100);
    delete p;
    // ... 100 行代码后 ...
    // delete p;  // BUG: double free (注释掉以免实际崩溃)
    std::cout << "Double-free skipped (would abort)\n";
  }

  void run() {
    std::cout << "\n===== Ex9: Memory Debugging =====\n";

    std::cout << "─── 使用 AddressSanitizer 编译 ───\n";
    std::cout << "$ g++ -fsanitize=address -g -O0 buggy.cpp -o buggy\n";
    std::cout << "$ ./buggy\n";
    std::cout << "# ASan 会精确报告:\n";
    std::cout << "# - 哪一行触发了 heap-buffer-overflow\n";
    std::cout << "# - 内存在哪里分配的 (用于 use-after-free)\n";
    std::cout << "# - 内存泄漏的分配栈\n\n";

    std::cout << "─── gdb + ASan 组合 ───\n";
    std::cout << "$ gdb ./buggy\n";
    std::cout << "(gdb) run 9\n";
    std::cout << "# ASan 检测到 bug → SIGABRT → gdb 停在崩溃点\n";
    std::cout << "(gdb) bt  # 完整的调用栈 + ASan 错误报告\n\n";

    std::cout << "─── gdb 内存命令 ───\n";
    std::cout << "(gdb) info proc mappings    # 查看内存映射\n";
    std::cout << "(gdb) x/64x \$rsp            # 检查栈内容\n";
    std::cout << "(gdb) x/s 0x7f...          # 查看地址处的字符串\n";
    std::cout << "(gdb) find /b 0x400000, 0x600000, 0x2a  # 搜索 42\n";
    std::cout << "(gdb) p sizeof(MyStruct)    # 检查类型大小\n";
    std::cout << "(gdb) p &var                # 获取变量地址\n";
    std::cout << "(gdb) watch -l *(int*)0x... # 监视特定地址的写入\n\n";

    // 运行 buggy 代码 (受保护的)
    std::cout << "─── Running heap_overflow (may corrupt heap) ───\n";
    heap_overflow();
    std::cout << "Survived! But heap may be corrupted...\n";

    std::cout << "\n─── stack_pointer_bug ───\n";
    int* dangling = stack_pointer_bug();
    std::cout << "Dangling stack pointer: " << *dangling
              << " (may or may not still be 42)\n";

    std::cout << "\n─── memory_leak ───\n";
    memory_leak();
    std::cout << "Leaked 1000 ints. Use Valgrind to detect:\n";
    std::cout << "$ valgrind --leak-check=full ./gdb_deep 9\n";
  }
}

// ============================================================================
// Ex10: 综合实战 — Bug 狩猎
//
// 场景:
//   一个「简易银行账户系统」, 包含 5 个故意的 bug:
//     1. 死锁 (transfer 顺序不一致)
//     2. 整数溢出 (取款不检查下溢)
//     3. 未初始化 (新账户余额未设)
//     4. use-after-free (关闭账户后仍可访问)
//     5. 竞态条件 (并发存款不原子)
//
// 任务:
//   1. 在不看答案的情况下, 用 gdb 找到所有 5 个 bug
//   2. 每个 bug: 描述现象 → gdb 命令 → 找到根因 → 修复
//   3. 记录你的调试过程
// ============================================================================

namespace ex10_bug_hunt {
  // 简易银行系统 (有 5 个 bug)
  class Bank {
    struct Account {
      int64_t id;
      std::string owner;
      int64_t balance;  // BUG 2: 未初始化 (某些构造函数未设置)
      bool active = true;

      Account(int64_t i, std::string o) : id(i), owner(std::move(o)) {
        // BUG 2 (continued): balance 未初始化! 应该是 balance(0)
      }
    };

    std::mutex _accounts_mutex;
    std::unordered_map<int64_t, std::shared_ptr<Account>> _accounts;
    int64_t _next_id = 1000;

  public:
    int64_t create_account(const std::string& owner) {
      std::lock_guard lk(_accounts_mutex);
      int64_t id = _next_id++;
      auto acc = std::make_shared<Account>(id, owner);
      _accounts[id] = acc;
      return id;
    }

    bool close_account(int64_t id) {
      std::lock_guard lk(_accounts_mutex);
      auto it = _accounts.find(id);
      if (it == _accounts.end()) return false;
      it->second->active = false;
      _accounts.erase(it);  // ← shared_ptr refcount decrement
      return true;
    }

    // BUG 3: 转账时两个 mutex 可能死锁
    //   thread A: transfer(1001, 1002, 100)
    //   thread B: transfer(1002, 1001, 50)
    //   如果 A 先锁了 1001, B 先锁了 1002 → 死锁
    bool transfer(int64_t from_id, int64_t to_id, int64_t amount) {
      // BUG 3: 锁定顺序不是全局一致的!
      //   应该按 id 排序后再锁
      std::lock_guard lk(_accounts_mutex);  // 用全局锁回避了问题
      // 但如果有人不小心拆成两个锁, 就会死锁

      auto it_from = _accounts.find(from_id);
      auto it_to = _accounts.find(to_id);

      if (it_from == _accounts.end() || it_to == _accounts.end())
        return false;

      auto& from = it_from->second;
      auto& to = it_to->second;

      if (!from->active || !to->active)
        return false;

      // BUG 4: 不检查余额是否足够, 可能整数下溢
      //   from->balance -= amount;  // 如果 balance < amount, 变负数!
      if (from->balance < amount) return false;  // 这个检查是正确的
      from->balance -= amount;
      to->balance += amount;

      // BUG 5: to->balance += amount 可能溢出 (int64 最大值 ~9e18)
      //   如果 to->balance 接近 INT64_MAX, 加法溢出 → 变成负数
      //   正确做法: 检查 to->balance + amount > INT64_MAX

      return true;
    }

    // BUG 6: 竞态条件 — 存款不是原子操作
    void deposit_racy(int64_t id, int64_t amount) {
      // 正确: 应该 lock
      // 但故意不 lock → 竞态条件
      auto it = _accounts.find(id);
      if (it != _accounts.end() && it->second->active) {
        int64_t current = it->second->balance;  // ← 读
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        it->second->balance = current + amount;  // ← 写 (TOCTOU!)
      }
    }

    void safe_deposit(int64_t id, int64_t amount) {
      std::lock_guard lk(_accounts_mutex);
      auto it = _accounts.find(id);
      if (it != _accounts.end() && it->second->active) {
        it->second->balance += amount;
      }
    }

    int64_t balance(int64_t id) {
      std::lock_guard lk(_accounts_mutex);
      auto it = _accounts.find(id);
      if (it != _accounts.end() && it->second->active)
        return it->second->balance;
      return -1;
    }

    size_t account_count() const { return _accounts.size(); }
  };

  void run() {
    std::cout << "\n===== Ex10: Bug Hunt — Bank System =====\n";
    std::cout << "There are 5 bugs in this code. Can you find them all?\n\n";

    Bank bank;

    // 创建账户
    int64_t alice = bank.create_account("Alice");
    int64_t bob   = bank.create_account("Bob");
    int64_t eve   = bank.create_account("Eve");

    std::cout << "Created accounts: Alice=" << alice
              << " Bob=" << bob << " Eve=" << eve << "\n";

    // BUG 1 揭示: 未初始化余额
    std::cout << "Alice initial balance: " << bank.balance(alice)
              << " (is this 0? Or garbage?)\n";
    std::cout << "Bob initial balance:   " << bank.balance(bob)
              << " (is this 0? Or garbage?)\n";

    // 存款
    bank.safe_deposit(alice, 10000);
    bank.safe_deposit(bob, 5000);

    std::cout << "\nAfter deposit:\n";
    std::cout << "Alice: " << bank.balance(alice) << "\n";
    std::cout << "Bob:   " << bank.balance(bob) << "\n";

    // 转账
    std::cout << "\nTransfer 2000 from Alice to Bob...\n";
    bank.transfer(alice, bob, 2000);
    std::cout << "Alice: " << bank.balance(alice) << "\n";
    std::cout << "Bob:   " << bank.balance(bob) << "\n";

    // BUG 2: 竞态条件
    std::cout << "\nRacy concurrent deposits to Eve (bug!):\n";
    std::thread t1([&] {
      for (int i = 0; i < 1000; ++i)
        bank.deposit_racy(eve, 1);
    });
    std::thread t2([&] {
      for (int i = 0; i < 1000; ++i)
        bank.deposit_racy(eve, 1);
    });
    t1.join();
    t2.join();
    int64_t eve_balance = bank.balance(eve);
    std::cout << "Eve balance: " << eve_balance
              << " (expected 2000, actual may be less due to race)\n";

    // BUG 3: use-after-close
    std::cout << "\nClosing Alice's account...\n";
    bank.close_account(alice);
    std::cout << "Account count: " << bank.account_count() << "\n";
    // 在实际代码中, 如果有其他地方持有 shared_ptr, 账户仍可访问
    std::cout << "Alice's balance after close: " << bank.balance(alice)
              << " (should be -1 if properly handled)\n";

    std::cout << "\n─── Bug Hunting Guide ───\n";
    std::cout << "Bug 1: Uninitialized balance → (gdb) b Bank::create_account → check Account ctor\n";
    std::cout << "Bug 2: Racy deposit → (gdb) info threads → thread apply all bt\n";
    std::cout << "Bug 3: Deadlock potential → (gdb) b transfer → check lock ordering\n";
    std::cout << "Bug 4: Integer underflow in transfer (balance < 0 not checked)\n";
    std::cout << "Bug 5: Integer overflow in transfer balance += amount\n";
    std::cout << "\nTry: $ gdb ./gdb_deep\n";
    std::cout << "(gdb) b Bank::create_account\n";
    std::cout << "(gdb) b Bank::transfer\n";
    std::cout << "(gdb) b Bank::deposit_racy thread 1\n";
    std::cout << "(gdb) run 10\n";
    std::cout << "(gdb) watch -l it->second->balance  # Watch balance address\n";
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  // 选择练习 (默认运行 last = ex10)
  int ex_num = 99;
  if (argc > 1) {
    ex_num = std::atoi(argv[1]);
    if (ex_num == 0) ex_num = 99;  // 0 = last
  }

  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Month 4 / Week 19: gdb Depth Debugging\n";
  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Running exercise " << (ex_num == 99 ? "LAST (10)" : std::to_string(ex_num)) << "\n";
  std::cout << "Compiled with: -g -O0 (debug symbols, no optimization)\n";
  std::cout << "Try: gdb ./gdb_deep    (then 'run N' for exercise N)\n";

  switch (ex_num) {
    case 1:  ex1_advanced_breakpoints::run(); break;
    case 2:  ex2_watchpoints::run(); break;
    case 3:  ex3_catchpoints::run(); break;
    case 4:  ex4_core_dump::run(); break;
    case 5:  ex5_multithreaded_debugging::run(); break;
    case 6:  ex6_reverse_debugging::run(); break;
    case 7:  ex7_gdb_python::run(); break;
    case 8:  ex8_dynamic_instrumentation::run(); break;
    case 9:  ex9_memory_debugging::run(); break;
    case 10: ex10_bug_hunt::run(); break;
    default:  // run all
      ex1_advanced_breakpoints::run();
      ex2_watchpoints::run();
      ex3_catchpoints::run();
      ex4_core_dump::run();
      ex5_multithreaded_debugging::run();
      ex6_reverse_debugging::run();
      ex7_gdb_python::run();
      ex8_dynamic_instrumentation::run();
      ex9_memory_debugging::run();
      ex10_bug_hunt::run();
      break;
  }

  std::cout << "\n Week 19 Done! 🎉\n";
  std::cout << "Next: Run 'gdb ./gdb_deep' and practice each exercise\n";
  return 0;
}
