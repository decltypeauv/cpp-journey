// Week 10: 多线程 — std::thread, mutex, condition_variable, atomic, future
// 编译: cmake -B build && cmake --build build
// 运行: ./build/multithreading
//
// 线程是操作系统调度的最小单位。
// C++11 起提供了跨平台的标准线程库，让并发编程
// 不再依赖 pthread (POSIX) 或 Win32 Thread API。
//
// 线程之间的数据共享比进程更轻量（共享地址空间），
// 但也带来了数据竞争（data race）的风险。

#include <atomic>
#include <barrier>      // C++20: std::barrier
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <latch>        // C++20: std::latch
#include <mutex>
#include <queue>
#include <semaphore>    // C++20: std::counting_semaphore
#include <shared_mutex>
#include <string>
#include <syncstream>   // C++20: std::osyncstream
#include <thread>
#include <vector>

using std::cout;
using std::string;
using std::vector;

// 辅助：让输出更好看
constexpr auto SLEEP_SHORT = std::chrono::milliseconds(100);
constexpr auto SLEEP_LONG  = std::chrono::milliseconds(500);

// ============================================================
// 练习 1: std::thread 基础 — 创建、join、detach
// ============================================================
//
// std::thread 代表一个可执行的线程。
// 构造时传入一个可调用对象（函数、lambda、函数对象），
// 线程立即开始执行。
//
// join()  — 等待线程结束
// detach() — 将线程与 std::thread 对象分离，让它独立运行

void thread_worker(int id, const string &msg) {
  cout << "  [线程 " << id << "] 启动: " << msg << "\n";
  std::this_thread::sleep_for(SLEEP_SHORT);
  cout << "  [线程 " << id << "] 完成\n";
}

void exercise1_thread_basics() {
  cout << "=== 练习 1: std::thread 基础 ===\n";

  // TODO 1.1: 最基本的线程创建和 join
  {
    cout << "  主线程开始...\n";

    std::thread t1(thread_worker, 1, "你好，我从线程而来");
    std::thread t2([]() {
      cout << "  [线程 2] lambda 线程启动\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      cout << "  [线程 2] lambda 线程完成\n";
    });

    // join — 等待线程结束
    t1.join();
    t2.join();

    cout << "  两个线程都结束了\n";
  }

  // TODO 1.2: detach — 分离线程
  {
    cout << "\n  detach 演示:\n";
    std::thread t3([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      cout << "  [detached 线程] 我独立运行，不需要 join\n";
    });
    t3.detach();
    // 注意：detach 后 std::thread 对象不再代表任何线程
    // joinable() 返回 false
    cout << "  t3.joinable() = " << std::boolalpha << t3.joinable() << "\n";

    // 给 detach 线程一点时间跑完
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  // TODO 1.3: std::thread 是 move-only 的
  {
    cout << "\n  std::thread 是 move-only 的:\n";
    std::thread t4([]{ cout << "    t4 在工作\n"; });
    // std::thread t5 = t4;   // ❌ 拷贝 — 编译错误
    std::thread t5 = std::move(t4);  // ✅ 移动 — t4 不再代表任何线程
    t5.join();
    cout << "    t4.joinable() = " << t4.joinable() << " (移动后变空)\n";
  }

  // TODO 1.4: 获取硬件并发数
  {
    cout << "\n  硬件并发线程数: " << std::thread::hardware_concurrency() << "\n";
    cout << "  💡 这是 CPU 逻辑核数，不是物理核数（含超线程）\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 2: std::mutex — 互斥锁保护临界区
// ============================================================
//
// 数据竞争 (data race): 多个线程同时访问同一内存位置，
// 且至少有一个是写操作 → 未定义行为。
//
// mutex: 保证同一时刻只有一个线程进入临界区。

class UnsafeCounter {
  int _value = 0;

public:
  void increment() {
    // ❌ 不是原子的: 读 → +1 → 写（3 步）
    ++_value;
  }
  int value() const { return _value; }
};

class SafeCounter {
  int _value = 0;
  mutable std::mutex _mtx; // mutable: 在 const 方法中也能加锁

public:
  void increment() {
    std::lock_guard<std::mutex> lock(_mtx); // RAII 加锁
    ++_value;
    // lock 析构 → 自动解锁（即使 ++_value 抛异常）
  }

  int value() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _value;
  }
};

void exercise2_mutex() {
  cout << "=== 练习 2: std::mutex ===\n";

  // TODO 2.1: 演示数据竞争
  {
    cout << "  演示数据竞争:\n";

    UnsafeCounter unsafe_ctr;
    constexpr int N_THREADS = 10;
    constexpr int N_INCREMENTS = 10000;

    {
      vector<std::thread> threads;
      for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&unsafe_ctr]() {
          for (int j = 0; j < N_INCREMENTS; ++j) {
            unsafe_ctr.increment();
          }
        });
      }
      for (auto &t : threads) t.join();
    }

    cout << "    不安全计数器: " << unsafe_ctr.value()
         << " (期望 " << N_THREADS * N_INCREMENTS << ")\n";
    cout << "    ⚠️ 丢失了 "
         << N_THREADS * N_INCREMENTS - unsafe_ctr.value() << " 次计数!\n";
  }

  // TODO 2.2: 用 mutex 修复
  {
    cout << "\n  用 mutex 修复:\n";

    SafeCounter safe_ctr;
    constexpr int N_THREADS = 10;
    constexpr int N_INCREMENTS = 10000;

    {
      vector<std::thread> threads;
      for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&safe_ctr]() {
          for (int j = 0; j < N_INCREMENTS; ++j) {
            safe_ctr.increment();
          }
        });
      }
      for (auto &t : threads) t.join();
    }

    cout << "    安全计数器: " << safe_ctr.value()
         << " (期望 " << N_THREADS * N_INCREMENTS << ") ✅\n";
  }

  // TODO 2.3: std::lock_guard vs std::unique_lock
  {
    cout << "\n  std::lock_guard vs std::unique_lock:\n";
    cout << "  ┌──────────────────┬──────────────────────┐\n";
    cout << "  │ lock_guard       │ 最简单的 RAII 锁     │\n";
    cout << "  │                  │ 构造=加锁, 析构=解锁 │\n";
    cout << "  │                  │ 不能手动 unlock      │\n";
    cout << "  ├──────────────────┼──────────────────────┤\n";
    cout << "  │ unique_lock      │ 可以手动 lock/unlock │\n";
    cout << "  │                  │ 可以延迟加锁 (defer) │\n";
    cout << "  │                  │ 配合 condition_var   │\n";
    cout << "  │                  │ 可以转移所有权(move) │\n";
    cout << "  └──────────────────┴──────────────────────┘\n";

    std::mutex mtx;
    std::unique_lock<std::mutex> lock(mtx, std::defer_lock); // 不立即加锁
    cout << "    owns_lock() = " << lock.owns_lock() << " (defer_lock)\n";
    lock.lock();
    cout << "    owns_lock() = " << lock.owns_lock() << " (lock 后)\n";
    lock.unlock();
    cout << "    owns_lock() = " << lock.owns_lock() << " (unlock 后)\n";
    // unique_lock 析构时如果还持有锁会自动释放
  }

  // TODO 2.4: std::recursive_mutex — 允许同一线程多次加锁
  {
    cout << "\n  std::recursive_mutex:\n";
    cout << "    同一线程可以对 recursive_mutex 多次 lock\n";
    cout << "    ⚠️ 通常建议重构代码以避免递归加锁\n";
    cout << "    但有时在递归函数或多层调用中不可避免\n";
  }

  // TODO 2.5: std::shared_mutex (C++17) — 读写锁
  {
    cout << "\n  std::shared_mutex — 读写锁:\n";
    cout << "    lock_shared()      — 共享锁（多个读者可同时持有）\n";
    cout << "    lock()             — 独占锁（写者独享）\n";
    cout << "    适合读多写少的场景\n";
  }

  // TODO 2.6: std::scoped_lock (C++17) — 同时锁多个 mutex
  {
    cout << "\n  std::scoped_lock — 同时锁多个 mutex:\n";
    cout << "    std::scoped_lock lock(mtx1, mtx2, mtx3);\n";
    cout << "    自动防止死锁（内部用 std::lock 算法）\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 3: std::condition_variable — 线程间等待/通知
// ============================================================
//
// condition_variable 让线程可以等待某个条件变为真，
// 而不是 busy-waiting（轮询空转）。
//
// 核心模式:
//   std::unique_lock<std::mutex> lock(mtx);
//   cv.wait(lock, []{ return condition; });  // 有谓词的版本

template <typename T>
class ThreadSafeQueue {
  std::queue<T> _q;
  mutable std::mutex _mtx;
  std::condition_variable _cv;
  bool _done{false};

public:
  void push(T val) {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _q.push(std::move(val));
    }
    _cv.notify_one(); // 通知一个等待的消费者
  }

  bool try_pop(T &val) {
    std::unique_lock<std::mutex> lock(_mtx);
    // wait 的谓词版本: 等价于 while (!condition) { cv.wait(lock); }
    _cv.wait(lock, [this] { return !_q.empty() || _done; });

    if (_q.empty()) return false; // done 信号

    val = std::move(_q.front());
    _q.pop();
    return true;
  }

  void set_done() {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _done = true;
    }
    _cv.notify_all(); // 唤醒所有等待者
  }
};

void exercise3_condition_variable() {
  cout << "=== 练习 3: std::condition_variable ===\n";

  // TODO 3.1: 生产者-消费者
  {
    cout << "  生产者-消费者模型:\n";
    ThreadSafeQueue<int> queue;

    // 消费者线程
    std::thread consumer([&queue]() {
      int val;
      cout << "  [消费者] 等待数据...\n";
      while (queue.try_pop(val)) {
        cout << "  [消费者] 取到: " << val << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      cout << "  [消费者] 队列关闭，退出\n";
    });

    // 生产者线程
    std::thread producer([&queue]() {
      for (int i = 1; i <= 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cout << "  [生产者] 放入: " << i << "\n";
        queue.push(i);
      }
      queue.set_done();
      cout << "  [生产者] 完成，发出 done 信号\n";
    });

    producer.join();
    consumer.join();
  }

  // TODO 3.2: notify_one vs notify_all
  {
    cout << "\n  notify_one vs notify_all:\n";
    cout << "    notify_one()  — 唤醒一个等待的线程（不确定是哪个）\n";
    cout << "    notify_all()  — 唤醒所有等待的线程\n";
    cout << "    默认用 notify_one（减少惊群效应）\n";
    cout << "    当状态变化对所有等待线程都有意义时用 notify_all\n";
  }

  // TODO 3.3: wait 的虚假唤醒 (spurious wakeup)
  {
    cout << "\n  💡 虚假唤醒 (spurious wakeup):\n";
    cout << "    wait 可能在没有通知的情况下返回（OS 的原因）\n";
    cout << "    这就是为什么 wait 必须配合「条件检查」:\n";
    cout << "    ┌──────────────────────────────────────┐\n";
    cout << "    │ // ✅ 正确: 带谓词的 wait           │\n";
    cout << "    │ cv.wait(lock, []{ return ready; });  │\n";
    cout << "    │ // 等价于:                           │\n";
    cout << "    │ while (!ready) { cv.wait(lock); }    │\n";
    cout << "    └──────────────────────────────────────┘\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 4: std::atomic — 无锁原子操作
// ============================================================
//
// atomic 提供不可分割的操作 — CPU 保证在一个指令周期内完成。
// 不需要 mutex，性能更好，但适用范围有限。
//
// 原子操作的 memory order 是一块高级话题，
// 入门阶段先用默认的 memory_order_seq_cst 即可。

void exercise4_atomic() {
  cout << "=== 练习 4: std::atomic ===\n";

  // TODO 4.1: 用 atomic 替代 mutex 做计数器
  {
    std::atomic<int> counter{0};
    constexpr int N_THREADS = 10;
    constexpr int N_INCREMENTS = 10000;

    {
      vector<std::thread> threads;
      for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&counter]() {
          for (int j = 0; j < N_INCREMENTS; ++j) {
            // ++ 是原子操作，不需要加锁
            counter.fetch_add(1, std::memory_order_relaxed);
            // 或者简写: ++counter; (默认 memory_order_seq_cst)
          }
        });
      }
      for (auto &t : threads) t.join();
    }

    cout << "  atomic 计数器: " << counter.load()
         << " (期望 " << N_THREADS * N_INCREMENTS << ") ✅\n";
  }

  // TODO 4.2: atomic 的基本操作
  {
    cout << "\n  atomic 基本操作:\n";

    std::atomic<int> a{0};

    cout << "    a = " << a.load() << "\n";
    a.store(42);
    cout << "    a.store(42) → a = " << a.load() << "\n";

    int expected = 42;
    bool success = a.compare_exchange_strong(expected, 100);
    cout << "    CAS(42, 100): " << std::boolalpha << success
         << ", a = " << a.load() << "\n";

    expected = 0;
    success = a.compare_exchange_strong(expected, 200);
    cout << "    CAS(0, 200): " << std::boolalpha << success
         << " (失败，a 现在是 " << a.load() << ")\n";

    cout << "    a.fetch_add(1) → " << a.fetch_add(1) << " (返回旧值)\n";
    cout << "    现在 a = " << a.load() << "\n";
  }

  // TODO 4.3: atomic<bool> 作为轻量级标志
  {
    cout << "\n  atomic<bool> — 轻量级开关:\n";
    std::atomic<bool> running{true};

    std::thread worker([&running]() {
      int ticks = 0;
      while (running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ++ticks;
      }
      cout << "    [worker] 检测到 running=false, 退出 (跑了 "
           << ticks << " 个 tick)\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cout << "    主线程设置 running = false...\n";
    running.store(false, std::memory_order_release);

    worker.join();
  }

  // TODO 4.4: is_lock_free — 检查是否真的是无锁
  {
    cout << "\n  is_lock_free 检查:\n";
    cout << "    atomic<int>:    " << std::boolalpha
         << std::atomic<int>{}.is_lock_free() << "\n";
    cout << "    atomic<bool>:   " << std::atomic<bool>{}.is_lock_free() << "\n";
    cout << "    atomic<double>: " << std::atomic<double>{}.is_lock_free()
         << " (浮点原子不常见)\n";
    cout << "    💡 现代 x86-64 CPU 上基本类型几乎都是 lock-free\n";
  }

  // TODO 4.5: 什么是 memory order？（简要）
  {
    cout << "\n  Memory Order 简介:\n";
    cout << "  ┌─────────────────────────┬───────────────────────┐\n";
    cout << "  │ memory_order_seq_cst    │ 顺序一致（默认，最安全│\n";
    cout << "  │ memory_order_acquire    │ 读操作（后面的不能重排前│\n";
    cout << "  │ memory_order_release    │ 写操作（前面的不能重排后│\n";
    cout << "  │ memory_order_acq_rel    │ 读-改-写，兼具两者    │\n";
    cout << "  │ memory_order_relaxed    │ 只有原子性，无顺序保障│\n";
    cout << "  └─────────────────────────┴───────────────────────┘\n";
    cout << "    入门: 默认 seq_cst 就够了\n";
    cout << "    进阶: acquire-release 在生产者-消费者中足够且更快\n";
    cout << "    高手: relaxed 只在纯计数器等无依赖场景用\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 5: std::future / std::promise / std::async
// ============================================================
//
// future/promise 提供了一种「结果在未来某时刻就绪」的机制。
// promise 端 produce 值，future 端 consume 值。
// 不需要直接的锁或条件变量。

int compute_expensive(int x) {
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return x * x + 2 * x + 1; // (x+1)^2
}

void exercise5_future_promise_async() {
  cout << "=== 练习 5: std::future / std::promise / std::async ===\n";

  // TODO 5.1: std::async — 最简单的异步方式
  {
    cout << "  std::async 启动异步任务:\n";

    // std::launch::async  — 保证在新线程执行
    // std::launch::deferred — 延迟到 get() 时才在当前线程执行
    auto fut = std::async(std::launch::async, compute_expensive, 10);

    cout << "  主线程继续做别的事...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // get() 阻塞直到结果就绪
    cout << "  结果: " << fut.get() << " (期望 121 = 11²)\n";
  }

  // TODO 5.2: std::promise — 手动设置结果
  {
    cout << "\n  std::promise + std::future:\n";

    std::promise<string> promise;
    std::future<string> future = promise.get_future();

    std::thread worker([promise = std::move(promise)]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      promise.set_value("任务完成！");
      // promise.set_exception(...) 也可以传异常
    });

    cout << "  等待 promise...\n";
    cout << "  future.get() → \"" << future.get() << "\"\n";
    worker.join();
  }

  // TODO 5.3: std::shared_future — 多个消费者
  {
    cout << "\n  std::shared_future — 多个消费者:\n";
    cout << "    std::future 的结果只能 get() 一次\n";
    cout << "    std::shared_future 可以被多次 get()\n";
    cout << "    允许多个线程等待同一个结果\n";

    std::promise<int> promise;
    std::shared_future<int> sf = promise.get_future().share();

    vector<std::thread> consumers;
    for (int i = 0; i < 3; ++i) {
      consumers.emplace_back([sf, i]() {
        cout << "    [消费者 " << i << "] 等待...\n";
        int result = sf.get();
        cout << "    [消费者 " << i << "] 拿到: " << result << "\n";
      });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    promise.set_value(42);

    for (auto &t : consumers) t.join();
  }

  // TODO 5.4: wait_for / wait_until — 超时等待
  {
    cout << "\n  带超时的 future 等待:\n";

    auto fut = std::async(std::launch::async, []() {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      return 42;
    });

    using namespace std::chrono_literals;
    auto status = fut.wait_for(100ms);
    if (status == std::future_status::timeout) {
      cout << "    100ms 后: 还没有结果 (timeout)\n";
    }

    status = fut.wait_for(500ms);
    if (status == std::future_status::ready) {
      cout << "    500ms 后: 结果就绪 → " << fut.get() << "\n";
    }
  }

  // TODO 5.5: std::packaged_task — 包装可调用对象
  {
    cout << "\n  std::packaged_task:\n";
    cout << "    将一个可调用对象包装成 future-able 的 task\n";
    cout << "    适合线程池（创建一次 task，移动给线程执行）\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 6: 死锁 — 产生原因与避免方法
// ============================================================
//
// 死锁的四个必要条件（全部满足才发生）:
//   1. 互斥: 资源不能被共享
//   2. 持有并等待: 持有一个锁，等待另一个锁
//   3. 不可抢占: 锁只能由持有者释放
//   4. 循环等待: A 等 B，B 等 A
//
// 打破任意一个即可防止死锁。

void exercise6_deadlock() {
  cout << "=== 练习 6: 死锁与避免 ===\n";

  // TODO 6.1: 死锁演示
  {
    cout << "  死锁演示:\n";
    cout << "  ┌──────────────────────────────────────┐\n";
    cout << "  │ 线程 1: lock(A) → sleep → lock(B)    │\n";
    cout << "  │ 线程 2: lock(B) → sleep → lock(A)    │\n";
    cout << "  │ → 线程 1 等 B（被线程 2 持有）       │\n";
    cout << "  │ → 线程 2 等 A（被线程 1 持有）       │\n";
    cout << "  │ → 永远互相等待 = 死锁                 │\n";
    cout << "  └──────────────────────────────────────┘\n";

    std::mutex mtx_a, mtx_b;

    // 注意：这段代码会死锁，我们用超时来演示避免
    // 实际运行死锁则跳过...
    cout << "\n  （跳过实际死锁演示 — 会永久卡住）\n";
  }

  // TODO 6.2: 避免方法 1 — 总是按相同顺序加锁
  {
    cout << "\n  解决方案 1: 总是按相同顺序加锁:\n";
    cout << "    所有线程都先锁 A 再锁 B → 不会循环等待\n";
    cout << "    这是最简单也最实用的方法\n";
  }

  // TODO 6.3: 避免方法 2 — std::lock / std::scoped_lock
  {
    cout << "\n  解决方案 2: std::lock / std::scoped_lock:\n";
    cout << "    std::lock(mtx1, mtx2, mtx3);  // 原子地锁多个\n";
    cout << "    std::scoped_lock(mtx1, mtx2); // C++17 RAII 版本\n";
    cout << "    内部用死锁避免算法（try_lock + 回退）\n";

    std::mutex a, b;

    auto worker = [&](int id) {
      std::scoped_lock lock(a, b); // 安全地同时持有两个锁
      cout << "    [线程 " << id << "] 同时持有 a 和 b\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    t1.join();
    t2.join();
  }

  // TODO 6.4: 避免方法 3 — try_lock 和回退
  {
    cout << "\n  解决方案 3: try_lock + 回退:\n";
    cout << "    获取不到锁时释放已持有的锁，等一会再重试\n";
    cout << "    std::unique_lock 的 try_lock 返回 false 表示锁被占用\n";
  }

  // TODO 6.5: 避免方法 4 — 锁的层级（减少锁的数量）
  {
    cout << "\n  解决方案 4: 减小锁的粒度:\n";
    cout << "    能不共享就不共享（per-thread data）\n";
    cout << "    能不加锁就不加锁（atomic，lock-free）\n";
    cout << "    锁越小，死锁可能性越低\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 7: thread_local — 线程局部存储
// ============================================================
//
// thread_local 变量在每个线程中都有自己独立的一份实例。
// 不需要加锁，天生线程安全。
//
// 生命周期: 线程开始时初始化，线程结束时析构。

// 全局 thread_local 变量
thread_local int g_tls_id = -1;
thread_local char g_tls_buffer[64] = {};

int next_thread_id() {
  static std::atomic<int> next_id{1};
  return next_id.fetch_add(1);
}

void exercise7_thread_local() {
  cout << "=== 练习 7: thread_local ===\n";

  // TODO 7.1: thread_local 的基本使用
  {
    cout << "  thread_local 变量 — 每个线程独立实例:\n";

    auto worker = [](const string &name) {
      g_tls_id = next_thread_id();

      // 写入 thread_local 缓冲区（不需要加锁）
      snprintf(g_tls_buffer, sizeof(g_tls_buffer), "线程 %s (id=%d)", name.c_str(),
               g_tls_id);

      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // 每个线程读自己版本的数据
      cout << "    [" << g_tls_buffer << "] 在运行\n";
    };

    vector<std::thread> threads;
    threads.emplace_back(worker, "A");
    threads.emplace_back(worker, "B");
    threads.emplace_back(worker, "C");
    for (auto &t : threads) t.join();

    // 主线程也有自己的一份
    cout << "    主线程的 g_tls_id = " << g_tls_id << " (默认值 -1)\n";
  }

  // TODO 7.2: thread_local vs static
  {
    cout << "\n  thread_local vs static:\n";
    cout << "  ┌──────────────┬──────────────────────────┐\n";
    cout << "  │ static       │ 全局唯一，所有线程共享   │\n";
    cout << "  │ thread_local │ 每线程一份               │\n";
    cout << "  │ static       │ 需要同步访问 (mutex)     │\n";
    cout << "  │ thread_local │ 不需要同步（天然安全）   │\n";
    cout << "  └──────────────┴──────────────────────────┘\n";
  }

  // TODO 7.3: thread_local 在函数内
  {
    cout << "\n  thread_local 在函数内:\n";
    cout << "    void foo() {\n";
    cout << "      thread_local int call_count = 0;  // 每个线程独立计数\n";
    cout << "      ++call_count;\n";
    cout << "    }\n";
    cout << "    可以用作每线程的调用计数器或缓存\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 8: std::jthread (C++20) — 自动 join 的线程
// ============================================================
//
// std::jthread 是对 std::thread 的改进：
//   1. 析构时自动 join（不用手动调 join）
//   2. 支持 stop_token — 可以请求线程停止

void exercise8_jthread() {
  cout << "=== 练习 8: std::jthread (C++20) ===\n";

  // TODO 8.1: jthread 自动 join
  {
    cout << "  jthread 自动 join 演示:\n";
    {
      // 不用手动 join！析构时自动 join
      std::jthread jt1([]() {
        cout << "    [jthread 1] 工作中...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cout << "    [jthread 1] 完成\n";
      });

      std::jthread jt2([]() {
        cout << "    [jthread 2] 工作中...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cout << "    [jthread 2] 完成\n";
      });

      cout << "    离开作用域，jthread 自动 join...\n";
    } // jt1 和 jt2 自动 join
    cout << "    作用域结束，两个 jthread 都已 join\n";
  }

  // TODO 8.2: stop_token — 优雅停止
  {
    cout << "\n  stop_token — 请求线程停止:\n";

    std::jthread worker([](std::stop_token stoken) {
      int count = 0;
      while (!stoken.stop_requested()) { // 轮询停止请求
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ++count;
      }
      cout << "    [worker] 收到停止请求，已运行 " << count
           << " 次循环，开始清理...\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cout << "    主线程请求 worker 停止...\n";
    worker.request_stop();

    // 也可以直接等: worker.join();
    // 也可以 cooperation cancel:
    // auto source = worker.get_stop_source();
    // source.request_stop();

    worker.join();
  }

  // TODO 8.3: std::jthread vs std::thread
  {
    cout << "\n  std::jthread vs std::thread:\n";
    cout << "  ┌──────────────┬──────────────────┬───────────────────┐\n";
    cout << "  │              │ std::thread      │ std::jthread      │\n";
    cout << "  ├──────────────┼──────────────────┼───────────────────┤\n";
    cout << "  │ 析构行为     │ 未 join → crash  │ 自动 join         │\n";
    cout << "  │ stop_token   │ 不支持           │ ✅ 支持           │\n";
    cout << "  │ 推荐程度     │ 旧代码兼容       │ ✅ 新代码默认用  │\n";
    cout << "  └──────────────┴──────────────────┴───────────────────┘\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 9: C++20 同步原语 — latch, barrier, semaphore
// ============================================================
//
// C++20 新增了三个重要的同步原语：
//   latch:   一次性倒计数器，等待所有线程到达某点
//   barrier: 可复用的阶段同步点
//   counting_semaphore: 限制并发访问数量

void exercise9_cpp20_sync() {
  cout << "=== 练习 9: C++20 同步原语 ===\n";

  // TODO 9.1: std::latch — 一次性同步点
  {
    cout << "  std::latch — 等待所有线程就绪:\n";

    constexpr int N_WORKERS = 5;
    std::latch start_latch{1};    // 倒计时从 1 开始 (用于"起跑")
    std::latch done_latch{N_WORKERS}; // 倒计时从 N 开始 (等待所有人完成)

    vector<std::thread> workers;
    for (int i = 0; i < N_WORKERS; ++i) {
      workers.emplace_back([i, &start_latch, &done_latch]() {
        start_latch.wait(); // 等起跑信号
        cout << "    [worker " << i << "] 工作中...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * (i + 1)));
        done_latch.count_down(); // 我完成了!
      });
    }

    cout << "    准备... ";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cout << "出发!\n";
    start_latch.count_down(); // → 0 → 唤醒所有等待的线程

    done_latch.wait(); // 等所有人都完成
    cout << "    所有 worker 都完成了!\n";

    for (auto &t : workers) t.join();
  }

  // TODO 9.2: std::barrier — 可复用的阶段同步
  {
    cout << "\n  std::barrier — 多阶段同步:\n";
    cout << "    barrier 可以让所有线程在某个点汇合\n";
    cout << "    所有线程到达后 → 继续下一阶段\n";
    cout << "    适合分阶段并行算法（如并行排序的每个 pass）\n";

    constexpr int N_WORKERS = 3;

    auto on_completion = []() noexcept {
      cout << "      [阶段完成!]\n";
    };
    std::barrier sync_point{N_WORKERS, on_completion};

    vector<std::thread> workers;
    for (int i = 0; i < N_WORKERS; ++i) {
      workers.emplace_back([i, &sync_point]() {
        for (int phase = 1; phase <= 2; ++phase) {
          cout << "    [worker " << i << "] 阶段 " << phase
               << " 工作...\n";
          std::this_thread::sleep_for(
              std::chrono::milliseconds(30 * (i + 1)));
          sync_point.arrive_and_wait(); // 到达并等待其他人
        }
      });
    }
    for (auto &t : workers) t.join();
  }

  // TODO 9.3: std::counting_semaphore — 控制并发数量
  {
    cout << "\n  std::counting_semaphore — 限制并发数:\n";

    // 最多允许 2 个线程同时访问"受限制的资源"
    constexpr int MAX_CONCURRENT = 2;
    std::counting_semaphore<MAX_CONCURRENT> sem{MAX_CONCURRENT};

    vector<std::thread> workers;
    for (int i = 0; i < 5; ++i) {
      workers.emplace_back([i, &sem]() {
        sem.acquire(); // 获取一个槽位
        cout << "    [请求 " << i << "] 获取槽位，开始处理...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cout << "    [请求 " << i << "] 完成，释放槽位\n";
        sem.release(); // 释放槽位
      });
    }
    for (auto &t : workers) t.join();
  }

  // TODO 9.4: std::binary_semaphore
  {
    cout << "\n  std::binary_semaphore:\n";
    cout << "    using binary_semaphore = counting_semaphore<1>;\n";
    cout << "    等价于一个轻量级的 mutex（但没有所有权概念）\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 10: 实战 — 并行计算与线程池基础
// ============================================================
//
// 综合运用：并行 map-reduce + 简单的线程池概念

// 并行求和 (map-reduce 的 reduce 阶段)
template <typename Iterator>
auto parallel_sum(Iterator begin, Iterator end) {
  using ValueType = typename std::iterator_traits<Iterator>::value_type;

  auto distance = std::distance(begin, end);
  if (distance == 0) return ValueType{};

  // 确定线程数
  unsigned int n_threads = std::min(
      static_cast<unsigned int>(std::thread::hardware_concurrency()),
      static_cast<unsigned int>(distance));
  if (n_threads < 2) n_threads = 2;

  auto chunk_size = distance / n_threads;

  std::vector<std::future<ValueType>> futures;

  for (unsigned int i = 0; i < n_threads; ++i) {
    auto chunk_begin = begin + i * chunk_size;
    auto chunk_end =
        (i == n_threads - 1) ? end : chunk_begin + chunk_size;

    // 每个 chunk 异步求和
    futures.push_back(std::async(std::launch::async, [=]() {
      ValueType sum{};
      for (auto it = chunk_begin; it != chunk_end; ++it) {
        sum += *it;
      }
      return sum;
    }));
  }

  // 汇总结果
  ValueType total{};
  for (auto &f : futures) {
    total += f.get();
  }
  return total;
}

// 简单线程池概念演示
class SimpleThreadPool {
  std::vector<std::thread> _workers;
  std::mutex _mtx;
  std::condition_variable _cv;
  std::queue<std::function<void()>> _tasks;
  std::atomic<bool> _stop{false};

public:
  explicit SimpleThreadPool(size_t n_threads) {
    for (size_t i = 0; i < n_threads; ++i) {
      _workers.emplace_back([this]() {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(_mtx);
            _cv.wait(lock, [this] { return _stop.load() || !_tasks.empty(); });

            if (_stop.load() && _tasks.empty()) return;

            task = std::move(_tasks.front());
            _tasks.pop();
          }
          task();
        }
      });
    }
  }

  void enqueue(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _tasks.push(std::move(task));
    }
    _cv.notify_one();
  }

  ~SimpleThreadPool() {
    _stop.store(true);
    _cv.notify_all();
    for (auto &w : _workers) {
      if (w.joinable()) w.join();
    }
  }
};

void exercise10_real_world() {
  cout << "=== 练习 10: 实战 — 并行计算与线程池 ===\n";

  // TODO 10.1: 并行求和
  {
    cout << "  并行 sum:\n";

    // 创建一个大数据集
    vector<long long> data(10'000'000);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = i + 1;
    }

    // 单线程版本（为了对比）
    auto start = std::chrono::high_resolution_clock::now();
    auto single_result = parallel_sum(data.begin(), data.end());
    auto single_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);

    cout << "    结果: " << single_result << " (期望 "
         << (data.size() * (data.size() + 1) / 2) << ")\n";
    cout << "    耗时: " << single_time.count() << "ms (用 async "
         << std::thread::hardware_concurrency() << " 个线程)\n";
  }

  // TODO 10.2: 简易线程池
  {
    cout << "\n  简易线程池:\n";

    SimpleThreadPool pool(4); // 4 个工作线程

    std::atomic<int> completed{0};
    constexpr int N_TASKS = 8;

    for (int i = 1; i <= N_TASKS; ++i) {
      pool.enqueue([i, &completed]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + i * 10));
        cout << "    [任务 " << i << "] 完成\n";
        completed.fetch_add(1);
      });
    }

    // 等待所有任务完成（简单轮询）
    while (completed.load() < N_TASKS) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cout << "    所有 " << N_TASKS << " 个任务完成!\n";
  } // pool 析构 → 清理线程

  // TODO 10.3: 多线程编程的核心建议
  {
    cout << "\n  🎯 多线程编程核心建议:\n";
    cout << "  1. 默认不要共享数据 — 用 message passing 或 per-thread 数据\n";
    cout << "  2. 必须共享时 — 优先用 atomic，其次用 mutex\n";
    cout << "  3. 锁的粒度越小越好 — 锁内只做必要的事\n";
    cout << "  4. 避免嵌套锁 — 如果必须，用 std::scoped_lock\n";
    cout << "  5. 用 condition_variable 替代 busy-waiting\n";
    cout << "  6. 用 std::async / future 简化异步操作\n";
    cout << "  7. 新代码用 std::jthread 替代 std::thread\n";
    cout << "  8. 线程安全的第一原则: 不要让多个线程同时写同一块数据\n";
  }

  cout << "\n";
}

// ============================================================
// main
// ============================================================

int main() {
  // 对于多线程输出，使用单元缓冲减少交错
  std::cout << std::unitbuf;

  cout << "Week 10: 多线程 — std::thread, mutex, condition_variable, atomic, future\n";
  cout << "==============================================================\n\n";

  exercise1_thread_basics();
  exercise2_mutex();
  exercise3_condition_variable();
  exercise4_atomic();
  exercise5_future_promise_async();
  exercise6_deadlock();
  exercise7_thread_local();
  exercise8_jthread();
  exercise9_cpp20_sync();
  exercise10_real_world();

  cout << "✅ Week 10 全部练习完成！\n";
  cout << "\n📝 总结要点:\n";
  cout << "  1. std::thread 创建线程，join 等待，jthread(C++20) 自动 join\n";
  cout << "  2. 数据竞争 = 未定义行为 → 用 mutex 或 atomic 保护共享数据\n";
  cout << "  3. lock_guard/unique_lock/scoped_lock 是 RAII 锁，防止忘记解锁\n";
  cout << "  4. condition_variable 用于等待条件而非轮询（生产者-消费者）\n";
  cout << "  5. atomic 提供无锁原子操作，比 mutex 更快但适用范围窄\n";
  cout << "  6. future/promise/async 提供「结果在未来」的异步模型\n";
  cout << "  7. 死锁四条件：打破任意一个即可防止死锁\n";
  cout << "  8. thread_local 变量每线程独立副本，天然线程安全\n";
  cout << "  9. C++20 新增: jthread(自动join), latch, barrier, semaphore\n";
  cout << "  10. 多线程第一原则: 能不共享就不共享，必须共享优先用 atomic\n";

  return 0;
}
