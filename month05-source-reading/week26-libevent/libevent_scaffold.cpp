// ============================================================================
// Month 5 Week 26: libevent 源码阅读
// 日期: 2026-06-24
//
// 阅读目标: libevent — 跨平台事件通知库 (Reactor 模式的经典实现)
// 源码位置: ../libevent/
// 源码规模: ~50,000 行 C (核心 event.c ~4000 行)
//
// libevent 是 Niels Provos 创建的事件驱动 I/O 库, 被广泛用于:
//   - memcached, tor, Chromium, ntpd, tmux 等知名项目
//   - 提供统一的事件循环抽象 (跨 epoll/kqueue/select/poll)
//   - Reactor 模式 + Proactor 风格的 bufferevent
//
// 核心架构:
//   event_base (事件循环引擎)
//     ├── event (fd 事件 / timer / signal)
//     │     通过 event_add → evmap → backend (epoll/kqueue/...)
//     │     回调: event_callback_fn
//     ├── min_heap (定时器堆) — O(log n) 插入/删除
//     ├── evbuffer (链式缓冲区) — scatter/gather I/O
//     ├── bufferevent (缓冲 I/O 抽象) — 输入/输出双缓冲区
//     └── evconnlistener (监听器) — accept 抽象
//
// 10 个练习, 自底向上: 事件循环→I/O后端→定时器→缓冲区→高层抽象→整合
// ============================================================================

#include <algorithm>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <list>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::literals;

// ============================================================================
// 辅助工具
// ============================================================================

template <typename... Args>
void println(Args&&... args) {
  if constexpr (sizeof...(args) > 0)
    ((std::cout << std::forward<Args>(args)), ...);
  std::cout << '\n';
}
template <typename... Args>
void print(Args&&... args) {
  ((std::cout << std::forward<Args>(args)), ...);
}
void HR(std::string_view title = "") {
  println("\n", std::string(72, '='), "\n  ", title, "\n", std::string(72, '='));
}

// ============================================================================
// Exercise 1: libevent 概览 — Reactor 模式
// ============================================================================
//
// 【阅读清单】
//   include/event2/event.h  — 公开 API (事件、事件循环)
//   README.md               — 项目概述
//   what_is_libevent.txt    — 设计理念
//
// 【关键概念: Reactor 模式】
//   同步阻塞模型 (每连接一线程):
//     while(1) { fd = accept(); thread([fd]{ read/write(fd); }); }
//     → 10000 连接 = 10000 线程, 上下文切换灾难
//
//   Reactor 模式 (事件驱动, 单线程):
//     base = event_base_new();
//     event_new(fd, EV_READ, callback);  // 注册兴趣
//     event_base_dispatch(base);         // 循环: wait → dispatch → wait...
//     → 1 线程处理 10000 连接, IO 多路复用
//
//   libevent 的抽象层次:
//     Layer 1: event_base (事件循环) + event (事件)
//     Layer 2: bufferevent (缓冲 I/O)
//     Layer 3: evconnlistener (监听器)
//     Layer 4: evhttp (HTTP 服务器)

namespace ex1 {

void run() {
  HR("Ex1: libevent 概览 — Reactor 模式");

  println("libevent 核心文件:");
  println("  event.c          (~4000行) — 核心事件循环, event_base");
  println("  epoll.c          (~400行)  — epoll 后端");
  println("  buffer.c         (~3000行) — evbuffer 链式缓冲区");
  println("  bufferevent.c    (~3000行) — 缓冲 I/O 抽象");
  println("  signal.c         (~500行)  — 信号处理");
  println("  listener.c       (~400行)  — 监听器");
  println("  minheap-internal.h (~150行) — 定时器最小堆");
  println();

  println("Reactor vs Proactor:");
  println("  Reactor (libevent): 通知 fd 可读, 用户自己 read()");
  println("  Proactor (io_uring/IOCP): 内核完成 read(), 通知数据已就绪");
  println();

  println("📖 阅读顺序 (自底向上):");
  println("  1. minheap-internal.h  — 定时器堆 (最简单, ~150行)");
  println("  2. epoll.c             — I/O 多路复用后端");
  println("  3. event.c (前半)      — event_base + event 数据结构");
  println("  4. event.c (后半)      — event_base_loop 主循环");
  println("  5. buffer.c            — evbuffer 链式缓冲区");
  println("  6. bufferevent.c       — 缓冲 I/O 抽象");
  println("  7. listener.c          — accept 抽象");
  println("  8. signal.c            — 信号集成");
}

} // namespace ex1

// ============================================================================
// Exercise 2: event_base — 事件循环核心
// ============================================================================
//
// 【阅读清单】
//   event-internal.h       — struct event_base 内部结构
//   event.c: event_base_new, event_base_loop, event_base_dispatch
//   include/event2/event.h — event_base 公开 API
//
// 【关键数据结构】
//   struct event_base {
//     const struct eventop *evsel;    // backend vtable (epoll/kqueue/...)
//     void *evbase;                   // backend-specific data
//     struct event_io_map io;         // fd → events mapping (evmap)
//     struct event_signal_map sigmap; // signal → events mapping
//     struct min_heap timeheap;       // timer heap
//     struct event_list activequeues; // active events (按优先级)
//     int event_count;                // 总事件数
//     ...
//   };
//
// 【event_base_loop 主循环】
//   1. 检查是否有事件
//   2. 从 timeheap 取最近超时时间 → timeout
//   3. 调用 backend->dispatch(base, tv=timeout)
//      → epoll_wait / kevent / poll / select
//   4. 将就绪 fd 事件插入 activequeues
//   5. 检查超时事件 → 从 timeheap pop 已到期的
//   6. 逐一执行 activequeues 中的回调
//   7. goto 1

namespace ex2 {

// ── 简化的事件循环核心 ──────────────────────────────────────────────
enum class EventType : uint8_t { READ = 0x02, WRITE = 0x04, TIMEOUT = 0x10, SIGNAL = 0x20, PERSIST = 0x80 };
inline EventType operator|(EventType a, EventType b) { return EventType(uint8_t(a) | uint8_t(b)); }
inline bool operator&(EventType a, EventType b) { return (uint8_t(a) & uint8_t(b)) != 0; }

using EventCallback = std::function<void(int fd, EventType what)>;

struct SimpleEvent {
  int fd;
  EventType events;       // 我们关心的事件类型
  EventCallback callback;
  bool pending = false;   // 是否已加入事件循环

  // 定时器字段
  std::chrono::steady_clock::time_point timeout;
  size_t heap_index = size_t(-1); // min-heap index (类似 libevent 的 ev_timeout_pos)
};

// ── 简化的 event_base ───────────────────────────────────────────────
struct SimpleEventBase {
  // fd → events mapping (模拟 evmap)
  std::unordered_map<int, std::vector<SimpleEvent*>> _fd_map;
  // 定时器堆 (std::priority_queue 简化)
  struct TimerEntry {
    std::chrono::steady_clock::time_point when;
    SimpleEvent* ev;
    bool operator>(const TimerEntry& o) const { return when > o.when; }
  };
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> _timer_heap;

  // active queue (就绪事件)
  std::vector<SimpleEvent*> _active;

  bool _running = true;

  // 注册 fd 事件
  void event_add(SimpleEvent* ev) {
    if (ev->events & EventType::TIMEOUT) {
      _timer_heap.push({ev->timeout, ev});
    } else {
      _fd_map[ev->fd].push_back(ev);
    }
    ev->pending = true;
  }

  void event_del(SimpleEvent* ev) {
    ev->pending = false;
    // 实际 libevent 会从 evmap 和 timeheap 中移除
  }

  // 模拟一次循环
  void loop_once() {
    _active.clear();
    auto now = std::chrono::steady_clock::now();

    // 1. 处理定时器
    while (!_timer_heap.empty() && _timer_heap.top().when <= now) {
      auto* ev = _timer_heap.top().ev;
      _timer_heap.pop();
      if (ev->pending) _active.push_back(ev);
    }

    // 2. 模拟 I/O 就绪 (实际 libevent 这里调 epoll_wait)
    // 这里简化: 所有注册的 fd 事件都标记为就绪
    for (auto& [fd, evs] : _fd_map) {
      for (auto* ev : evs) {
        if (ev->pending) _active.push_back(ev);
      }
    }

    // 3. 执行回调
    for (auto* ev : _active) {
      if (ev->pending) ev->callback(ev->fd, ev->events);
    }
  }
};

void run() {
  HR("Ex2: event_base — 事件循环核心");

  SimpleEventBase base;

  int call_count = 0;
  SimpleEvent ev;
  ev.fd = 42;
  ev.events = EventType::READ;
  ev.callback = [&](int fd, EventType what) {
    call_count++;
    println("  回调: fd=", fd, " event=READ  count=", call_count);
  };
  ev.timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  ev.events = ev.events | EventType::TIMEOUT;

  base.event_add(&ev);
  println("添加事件: fd=42 READ+TIMEOUT(100ms)");

  // 运行几轮
  for (int i = 0; i < 3; i++) {
    print("  loop ", i + 1, "... ");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    base.loop_once();
    println("done, callbacks fired: ", call_count);
  }
  println();

  println("📖 event_base_loop 伪代码:");
  println("  while (!done) {");
  println("    timeout = min_heap_top(timeheap);  // 最近超时");
  println("    nfds = evsel->dispatch(base, tv);  // epoll_wait / kevent");
  println("    for (i in 0..nfds) 将就绪事件加入 activequeues;");
  println("    while (now >= timeheap.top) pop+active 超时事件;");
  println("    for each event in activequeues: 执行 callback;");
  println("  }");
  println();
  println("📖 精读 event.c: event_base_new + event_base_loop (~500行核心)");
}

} // namespace ex2

// ============================================================================
// Exercise 3: event — 事件抽象
// ============================================================================
//
// 【阅读清单】
//   include/event2/event.h  — struct event, event_new/event_add/event_del
//   event.c                 — event_add_impl, event_del_impl
//   event-internal.h        — event_callback, event_queue_insert
//
// 【关键概念】
//   event 的生命周期:
//     new → pending (add) → active (callback 执行中) → pending (persist)
//                                                      → non-pending (non-persist)
//
//   事件类型:
//     EV_READ    — fd 可读 (包括 EOF/对端关闭)
//     EV_WRITE   — fd 可写
//     EV_TIMEOUT — 超时
//     EV_SIGNAL  — 信号
//     EV_PERSIST — 触发后不自动 del (需手动 del)
//     EV_ET      — 边沿触发 (epoll EPOLLET)
//
//   event_add 内部流程:
//     1. 若 fd 事件 → evmap_io_add(base, ev->fd, ev)
//        → 若这是该 fd 的第一个事件 → backend->add(base, fd, events)
//        → 否则 update backend 的事件掩码
//     2. 若定时器 → min_heap_push(timeheap, ev)
//     3. 若信号 → evmap_signal_add(base, sig, ev)

namespace ex3 {

using ex2::EventType;

// ── event 状态机 ─────────────────────────────────────────────────────
enum class EventState { NEW, PENDING, ACTIVE, DELETED };

struct TrackedEvent {
  int fd;
  EventType events;
  EventState state = EventState::NEW;
  int trigger_count = 0;

  void add() {
    state = EventState::PENDING;
    println("    event_add(fd=", fd, "): NEW → PENDING");
  }
  void activate() {
    state = EventState::ACTIVE;
    trigger_count++;
    println("    event_active(fd=", fd, "): PENDING → ACTIVE (触发 #", trigger_count, ")");
  }
  void done() {
    if (events & EventType::PERSIST) {
      state = EventState::PENDING;
      println("    callback done: → PENDING (PERSIST)");
    } else {
      state = EventState::NEW;
      println("    callback done: → NEW (非 PERSIST, 需重新 add)");
    }
  }
  void del() {
    state = EventState::DELETED;
    println("    event_del(fd=", fd, "): → DELETED");
  }
};

void run() {
  HR("Ex3: event — 事件抽象");

  println("Event 生命周期演示:");
  println();

  TrackedEvent ev_oneshot{1, EventType::READ};
  TrackedEvent ev_persist{2, EventType::READ | EventType::PERSIST};

  // 一轮: PERSIST vs non-PERSIST
  for (auto* ev : {&ev_oneshot, &ev_persist}) {
    println("  fd=", ev->fd, ev->events & EventType::PERSIST ? " (PERSIST)" : " (oneshot)");
    ev->add();
    ev->activate();
    ev->done();
    ev->activate(); // non-PERSIST 不会触发
    println();
  }

  println("📖 event_add 内部 (event.c: event_add_impl):");
  println("  1. 检查事件是否已在 pending → 若已 pending 则 update");
  println("  2. 若 EV_READ/EV_WRITE → evmap_io_add()");
  println("  3. 若 EV_TIMEOUT → min_heap_push(timeheap)");
  println("  4. 若 EV_SIGNAL → evmap_signal_add()");
  println("  5. 插入事件队列 (event_queue_insert)");
  println();
  println("📖 精读 include/event2/event.h + event.c: event_add_impl");
}

} // namespace ex3

// ============================================================================
// Exercise 4: I/O Multiplexing Backends — epoll 后端
// ============================================================================
//
// 【阅读清单】
//   epoll.c   — epoll 后端实现
//   kqueue.c  — kqueue 后端 (macOS/FreeBSD)
//   select.c  — select 后端 (fallback)
//   poll.c    — poll 后端
//
// 【关键设计: 后端抽象 (vtable 模式)】
//   struct eventop {
//     const char *name;
//     void *(*init)(struct event_base *);
//     int (*add)(struct event_base *, evutil_socket_t fd, short old, short events, void *fdinfo);
//     int (*del)(struct event_base *, evutil_socket_t fd, short old, short events, void *fdinfo);
//     int (*dispatch)(struct event_base *, struct timeval *);
//     void (*dealloc)(struct event_base *);
//   };
//
//   libevent 按优先级选择后端:
//     1. epoll (Linux)
//     2. kqueue (macOS/BSD)
//     3. /dev/poll (Solaris)
//     4. poll
//     5. select (最后的 fallback)
//
// 【epoll 后端的 epoll_dispatch()】
//   struct epollop { int epfd; struct epoll_event *events; };
//   1. epoll_wait(epollop->epfd, events, NEVENTS, timeout)
//   2. 遍历返回的事件:
//      - EPOLLIN/EPOLLHUP/EPOLLERR → 标记 EV_READ
//      - EPOLLOUT → 标记 EV_WRITE
//      - EPOLLRDHUP → 对端半关闭
//   3. 对每个就绪 fd: event_active(ev, what) → 加入 activequeues

namespace ex4 {

// ── 后端的 vtable 抽象 ──────────────────────────────────────────────
struct BackendOps {
  std::string name;
  int priority; // 越小越优先

  BackendOps(std::string n, int p) : name(std::move(n)), priority(p) {}
  virtual int add(int fd, uint32_t events) = 0;
  virtual int del(int fd) = 0;
  virtual int wait(int timeout_ms) = 0; // 返回就绪 fd 数
  virtual ~BackendOps() = default;
};

// ── 模拟 epoll 后端 ─────────────────────────────────────────────────
struct MockEpollBackend : BackendOps {
  struct WatchedFd {
    int fd;
    uint32_t events;
    bool ready = false; // 模拟就绪
  };
  std::unordered_map<int, WatchedFd> _fds;
  int _next_fd = 100;

  MockEpollBackend() : BackendOps("epoll", 1) {
    // 实际 epoll: _epfd = epoll_create1(EPOLL_CLOEXEC)
  }

  int add(int fd, uint32_t events) override {
    _fds[fd] = {fd, events, false};
    println("    epoll_ctl(ADD, fd=", fd, ", events=", events, ")");
    return 0;
  }
  int del(int fd) override {
    _fds.erase(fd);
    println("    epoll_ctl(DEL, fd=", fd, ")");
    return 0;
  }
  int wait(int timeout_ms) override {
    // 模拟: 随机让一个 fd 就绪
    if (_fds.empty()) return 0;
    int ready = 0;
    for (auto& [fd, w] : _fds) {
      if (!w.ready && (rand() % 4 == 0)) {
        w.ready = true;
        println("    epoll_wait → fd=", fd, " ready (EV_READ)");
        ready++;
      }
    }
    return ready;
  }
};

// ── 模拟 poll 后端 ──────────────────────────────────────────────────
struct MockPollBackend : BackendOps {
  MockPollBackend() : BackendOps("poll", 3) {}
  int add(int, uint32_t) override { return 0; }
  int del(int) override { return 0; }
  int wait(int) override { return 0; }
};

void run() {
  HR("Ex4: I/O Multiplexing Backends");

  println("libevent 后端选择优先级:");
  println("  1. epoll  (Linux)     — O(1) 就绪队列, 最好的扩展性");
  println("  2. kqueue (macOS/BSD) — 类似 epoll, 支持文件变更监控");
  println("  3. poll               — O(n) 扫描, 无 fd 数限制");
  println("  4. select             — O(n) 扫描, 最多 1024 fd");
  println();

  println("模拟 epoll 后端:");
  MockEpollBackend epoll;

  // 注册几个 fd
  epoll.add(3, 0x02);  // EV_READ
  epoll.add(5, 0x02 | 0x04); // EV_READ | EV_WRITE
  epoll.add(7, 0x02);

  // 等待事件
  println("  调用 epoll_wait...");
  epoll.wait(100);
  epoll.wait(100);

  // 删除
  epoll.del(3);
  println();

  println("📖 epoll_dispatch 核心 (epoll.c):");
  println("  1. epoll_wait(epollop->epfd, events, NEVENTS, timeout)");
  println("  2. for each ready event:");
  println("       if (events[i].events & (EPOLLIN|EPOLLHUP|EPOLLERR)) → EV_READ");
  println("       if (events[i].events & EPOLLOUT) → EV_WRITE");
  println("       if (events[i].events & EPOLLRDHUP) → EV_CLOSED");
  println("       event_active(ev, what) → 插 activequeues");
  println();
  println("📖 精读 epoll.c (~400行) — 简洁精炼的后端实现");
}

} // namespace ex4

// ============================================================================
// Exercise 5: Timer Management — 最小堆
// ============================================================================
//
// 【阅读清单】
//   minheap-internal.h — 最小堆实现 (~150行, 全内联)
//
// 【关键设计】
//   - libevent 用二叉最小堆管理定时器
//   - 堆顶 = 最近超时事件
//   - event 结构体中嵌入 ev_timeout_pos (堆索引) → O(1) 删除
//   - push: O(log n), pop: O(log n), erase: O(log n)
//
//   min_heap 结构:
//     struct min_heap {
//       struct event** p;  // 动态数组 (reserve 扩容)
//       size_t n;          // 当前元素数
//       size_t a;          // 容量
//     };
//
//   堆比较: min_heap_elem_greater(a, b) → a->ev_timeout > b->ev_timeout

namespace ex5 {

// ── 简化的最小堆 (类似 libevent 的 minheap-internal.h) ─────────────
template <typename T>
struct MinHeap {
  std::vector<T> _heap;

  bool empty() const { return _heap.empty(); }
  size_t size() const { return _heap.size(); }
  const T& top() const { return _heap[0]; }

  void push(const T& val) {
    _heap.push_back(val);
    shift_up(_heap.size() - 1);
  }

  void pop() {
    if (_heap.empty()) return;
    _heap[0] = _heap.back();
    _heap.pop_back();
    if (!_heap.empty()) shift_down(0);
  }

  // 删除指定位置 (libevent 用 heap_index 做到 O(log n) erase)
  void erase_at(size_t idx) {
    if (idx >= _heap.size()) return;
    _heap[idx] = _heap.back();
    _heap.pop_back();
    if (idx < _heap.size()) {
      shift_down(idx);
    }
  }

private:
  void shift_up(size_t idx) {
    while (idx > 0) {
      size_t parent = (idx - 1) / 2;
      if (!(_heap[idx] < _heap[parent])) break;
      std::swap(_heap[idx], _heap[parent]);
      idx = parent;
    }
  }

  void shift_down(size_t idx) {
    size_t n = _heap.size();
    while (true) {
      size_t smallest = idx;
      size_t left = 2 * idx + 1;
      size_t right = 2 * idx + 2;
      if (left < n && _heap[left] < _heap[smallest]) smallest = left;
      if (right < n && _heap[right] < _heap[smallest]) smallest = right;
      if (smallest == idx) break;
      std::swap(_heap[idx], _heap[smallest]);
      idx = smallest;
    }
  }
};

// ── 定时器条目 ──────────────────────────────────────────────────────
struct Timer {
  int id;
  int deadline_ms; // 截止时间 (毫秒)
  bool operator<(const Timer& o) const { return deadline_ms < o.deadline_ms; }
};

void run() {
  HR("Ex5: Timer Management — 最小堆");

  MinHeap<Timer> heap;

  // 插入几个定时器
  heap.push({1, 500});  // 500ms 后到期
  heap.push({2, 100});  // 100ms 后到期 ← 最近的!
  heap.push({3, 300});  // 300ms 后到期
  heap.push({4, 1000}); // 1000ms 后到期
  heap.push({5, 200});  // 200ms 后到期

  println("定时器堆 (size=", heap.size(), "):");
  println("  堆顶: id=", heap.top().id, " deadline=", heap.top().deadline_ms, "ms (最近超时)");
  println();

  // 按顺序弹出
  println("按超时顺序弹出:");
  while (!heap.empty()) {
    auto t = heap.top();
    println("  id=", t.id, " deadline=", t.deadline_ms, "ms");
    heap.pop();
  }
  println();

  println("📖 event_base_loop 中定时器的使用:");
  println("  1. timeout = heap.top().deadline - now → 计算 epoll_wait 超时");
  println("  2. epoll_wait(epfd, events, N, timeout) → 最多等 timeout ms");
  println("  3. pop 所有已到期的定时器 → event_active → 执行回调");
  println("  4. 如果 PERSIST timer → 推算新 deadline → push 回 heap");
  println();
  println("📖 精读 minheap-internal.h (~150行) — 全内联, 零外部依赖");
}

} // namespace ex5

// ============================================================================
// Exercise 6: evbuffer — 链式缓冲区
// ============================================================================
//
// 【阅读清单】
//   buffer.c              — evbuffer 实现 (~3000行)
//   include/event2/buffer.h — evbuffer 公开 API
//   evbuffer-internal.h   — evbuffer_chain 内部结构
//
// 【关键设计: 链式缓冲区 (Chain of Buffers)】
//   evbuffer = linked list of evbuffer_chain
//   每个 chain { buffer, buf_len, misalign(可写偏移), off(已用数据偏移) }
//
//   为什么用链式?
//   1. 避免 realloc 拷贝 — 追加新 chain, 不移动旧数据
//   2. scatter/gather I/O — readv/writev 直接操作 chain 链表
//   3. 引用计数 — 多个 evbuffer 可共享 chain (evbuffer_add_buffer_reference)
//
//   结构:
//     evbuffer → chain1 → chain2 → chain3 → ... → chainN
//                [misalign][data][free]  [data][free] ... [misalign][data]
//     写入: 在最后一个 chain 的 free 部分追加; 不够就 new chain
//     读取: 从第一个 chain 的 data 部分 drain; 读完就 free chain

namespace ex6 {

// ── 简化的 evbuffer_chain ───────────────────────────────────────────
struct SimpleChain {
  std::vector<char> _buf;
  size_t _off = 0;    // 已用数据起始 (level 0 bias)
  size_t _misalign = 0; // 预留空间 (用于 prepend/header)

  size_t data_len() const { return _buf.size() - _off; }
  const char* data() const { return _buf.data() + _off; }
  size_t space_left() const { return 0; } // 简化: chain 不可扩展
};

// ── 简化的 evbuffer ─────────────────────────────────────────────────
struct SimpleEvbuffer {
  std::list<SimpleChain> _chains;
  size_t _total_len = 0;

  // 添加数据
  void add(const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    SimpleChain chain;
    chain._buf.assign(p, p + len);
    chain._off = 0;
    _chains.push_back(std::move(chain));
    _total_len += len;
  }

  // 移除数据 (drain)
  void drain(size_t len) {
    while (len > 0 && !_chains.empty()) {
      auto& front = _chains.front();
      size_t available = front.data_len();
      if (len >= available) {
        len -= available;
        _total_len -= available;
        _chains.pop_front();
      } else {
        front._off += len;
        _total_len -= len;
        len = 0;
      }
    }
  }

  // 读取数据 (不 drain)
  std::string copyout(size_t max_len) const {
    std::string result;
    size_t remaining = std::min(max_len, _total_len);
    for (auto& chain : _chains) {
      if (remaining == 0) break;
      size_t take = std::min(remaining, chain.data_len());
      result.append(chain.data(), take);
      remaining -= take;
    }
    return result;
  }

  // readln — 读取一行 (以 \n 为分隔)
  std::optional<std::string> readln() {
    size_t offset = 0;
    for (auto& chain : _chains) {
      for (size_t i = 0; i < chain.data_len(); i++) {
        if (chain.data()[i] == '\n') {
          size_t len = offset + i + 1;
          std::string line = copyout(len);
          drain(len);
          // 去掉尾部 \r\n
          while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
          return line;
        }
      }
      offset += chain.data_len();
    }
    return std::nullopt;
  }

  size_t length() const { return _total_len; }
  size_t chain_count() const { return _chains.size(); }
};

void run() {
  HR("Ex6: evbuffer — 链式缓冲区");

  SimpleEvbuffer buf;

  // writev 模拟: 多次写入 (类似 HTTP 响应头+体)
  buf.add("HTTP/1.1 200 OK\r\n", 17);
  buf.add("Content-Type: text/plain\r\n", 26);
  buf.add("Content-Length: 13\r\n", 20);
  buf.add("\r\n", 2);
  buf.add("Hello, World!\n", 14);
  buf.add("Second line\n", 12);

  println("缓冲区状态: ", buf.length(), " bytes in ", buf.chain_count(), " chains");
  println();

  // 逐行读取 (readln)
  println("逐行读取 (evbuffer_readln):");
  while (auto line = buf.readln()) {
    println("  → '", *line, "'");
  }
  println("  剩余: ", buf.length(), " bytes");

  // 添加更多数据
  buf.add("data chunk\n", 11);
  buf.add("final chunk\n", 12);
  println("  追加后: ", buf.length(), " bytes");

  println();
  println("📖 evbuffer 链式设计优势:");
  println("  1. 无 realloc 拷贝 — 追加新 chain, 旧数据不动");
  println("  2. readv/writev — 一个系统调用操作多个 chain");
  println("  3. 引用计数 — 零拷贝共享 (evbuffer_add_buffer_reference)");
  println("  4. MMAP 支持 — 文件直接映射成 chain (sendfile)");
  println();
  println("📖 精读 buffer.c (~3000行) — evbuffer 是 libevent 最精巧的数据结构");
}

} // namespace ex6

// ============================================================================
// Exercise 7: bufferevent — 缓冲 I/O 抽象
// ============================================================================
//
// 【阅读清单】
//   bufferevent.c              — bufferevent 实现
//   include/event2/bufferevent.h — bufferevent API
//   bufferevent-internal.h     — bufferevent_private 结构
//
// 【关键设计】
//   bufferevent = event(READ) + event(WRITE) + evbuffer(input) + evbuffer(output)
//
//   它把"等待 fd 可读 → read → 放到缓冲区 → 通知用户"
//   整条流水线封装好, 用户只需:
//     bufev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
//     bufferevent_setcb(bufev, read_cb, write_cb, event_cb, NULL);
//     bufferevent_enable(bufev, EV_READ|EV_WRITE);
//
//   Watermark (水位线):
//     - low watermark: 输入至少 N 字节才触发 read_cb
//     - high watermark: 输出超过 N 字节时暂停读取 (背压)
//
//   数据传输流程:
//     数据到达 → fd 可读 → read() → input evbuffer → read_cb 被调用
//     write_cb → output evbuffer → fd 可写 → write()

namespace ex7 {

// ── 简化的 bufferevent ──────────────────────────────────────────────
struct SimpleBufferevent {
  int _fd;

  // 双缓冲区
  std::string _input;   // 读缓冲
  std::string _output;  // 写缓冲

  // 水位线
  size_t _wm_low = 0;   // 至少这么多数据才通知

  // 回调
  std::function<void(SimpleBufferevent*)> _read_cb;
  std::function<void(SimpleBufferevent*)> _write_cb;
  std::function<void(SimpleBufferevent*, short)> _event_cb;

  bool _read_enabled = false;
  bool _write_enabled = false;

  explicit SimpleBufferevent(int fd) : _fd(fd) {}

  void setcb(decltype(_read_cb) r, decltype(_write_cb) w, decltype(_event_cb) e) {
    _read_cb = r; _write_cb = w; _event_cb = e;
  }
  void enable_read()  { _read_enabled = true; println("    bufferevent_enable(READ)"); }
  void enable_write() { _write_enabled = true; println("    bufferevent_enable(WRITE)"); }

  void set_watermark(size_t low) { _wm_low = low; }

  // 模拟数据到达
  void simulate_read(const std::string& data) {
    _input += data;
    println("    fd=", _fd, " recv ", data.size(), " bytes → input buffer=", _input.size(), "B");
    if (_input.size() >= _wm_low && _read_cb) {
      _read_cb(this);
    }
  }

  // 读取
  std::string read(size_t max_len) {
    size_t n = std::min(max_len, _input.size());
    auto result = _input.substr(0, n);
    _input.erase(0, n);
    return result;
  }

  // 写入 (到输出缓冲区)
  void write(const std::string& data) {
    _output += data;
    println("    bufferevent_write: ", data.size(), "B → output buffer=", _output.size(), "B");
  }

  // 模拟 flush 到网络
  void simulate_flush() {
    if (!_output.empty()) {
      println("    fd=", _fd, " send ", _output.size(), " bytes → network");
      _output.clear();
      if (_write_cb) _write_cb(this);
    }
  }

  size_t input_len() const { return _input.size(); }
  size_t output_len() const { return _output.size(); }
};

void run() {
  HR("Ex7: bufferevent — 缓冲 I/O 抽象");

  SimpleBufferevent bufev(5);

  // 设置回调
  int msg_count = 0;
  bufev.setcb(
    [&](SimpleBufferevent* bev) {
      auto data = bev->read(1024);
      println("    read_cb: got '", data, "' (", data.size(), "B)");
      // echo 回去
      bev->write("echo: " + data + "\n");
      msg_count++;
    },
    [](SimpleBufferevent*) { println("    write_cb: output flushed"); },
    [](SimpleBufferevent*, short what) { println("    event_cb: what=", what); }
  );

  bufev.enable_read();
  bufev.enable_write();

  println("  输入水位线: 4 bytes (至少收 4B 才通知)");
  bufev.set_watermark(4);
  println();

  // 模拟数据到达
  println("  模拟数据流:");
  bufev.simulate_read("He");   // 2B — 未达到水位线
  bufev.simulate_read("llo");  // +3B = "Hello" — 触发 read_cb!
  bufev.simulate_read("World"); // 5B — 触发 read_cb!
  bufev.simulate_flush();
  println();

  println("  总消息数: ", msg_count);
  println();

  println("📖 bufferevent 的架构:");
  println("  输入路径: fd readable → event(READ) → read() → input evbuffer");
  println("             → watermark check → read_cb");
  println("  输出路径: write_cb → output evbuffer → watermark check");
  println("             → event(WRITE) → write() → fd writable");
  println();
  println("📖 精读 bufferevent.c (~3000行) + include/event2/bufferevent.h");
}

} // namespace ex7

// ============================================================================
// Exercise 8: evconnlistener — 监听器抽象
// ============================================================================
//
// 【阅读清单】
//   listener.c              — evconnlistener 实现
//   include/event2/listener.h — evconnlistener API
//
// 【关键设计】
//   evconnlistener_new_bind / evconnlistener_new:
//     - 创建 socket → bind → listen
//     - 内部用一个 bufferevent 或普通 event 监控 listen fd
//     - accept 成功后调用用户 callback
//     - 支持 LEV_OPT_CLOSE_ON_FREE / LEV_OPT_REUSEABLE 等选项
//
//   两种模式:
//     1. LEV_OPT_LEGACY: 普通 event(READ) + accept() (手动 accept)
//     2. 默认: 内部用 bufferevent + accept (更高效)
//
//   流程:
//     listener_cb → accept() → new fd → 调用用户 cb(新fd, addr)

namespace ex8 {

struct SimpleListener {
  int _listen_fd;
  std::function<void(int new_fd, const std::string& addr)> _accept_cb;
  int _accept_count = 0;

  SimpleListener(int fd, decltype(_accept_cb) cb) : _listen_fd(fd), _accept_cb(cb) {}

  void simulate_accept() {
    int new_fd = 100 + _accept_count;
    _accept_count++;
    println("    accept() → new fd=", new_fd);
    if (_accept_cb) _accept_cb(new_fd, "127.0.0.1:" + std::to_string(8000 + new_fd));
  }
};

void run() {
  HR("Ex8: evconnlistener — 监听器抽象");

  println("创建监听器 (模拟):");
  SimpleListener listener(3, [](int fd, const std::string& addr) {
    println("    新连接: fd=", fd, " addr=", addr);
    println("    → 创建 bufferevent, 开始处理...");
  });
  println();

  // 模拟几个新连接
  println("模拟 incoming connections:");
  listener.simulate_accept();
  listener.simulate_accept();
  listener.simulate_accept();
  println("  总连接数: ", listener._accept_count);
  println();

  println("📖 evconnlistener 内部流程 (listener.c):");
  println("  1. evconnlistener_new → socket+bind+listen → event(READ)");
  println("  2. listen_fd readable → listener_read_cb");
  println("  3. accept() in a loop (accept4 with SOCK_NONBLOCK)");
  println("  4. 每个新 fd → 调用用户的 accept callback");
  println("  5. 用户 callback 中: 创建 bufferevent, 注册事件...");
  println();
  println("📖 精读 listener.c (~400行) — 简洁清晰的 accept 抽象");
}

} // namespace ex8

// ============================================================================
// Exercise 9: Signal Handling — 信号集成到事件循环
// ============================================================================
//
// 【阅读清单】
//   signal.c               — 信号处理实现
//   evsignal-internal.h    — signal 内部结构
//
// 【关键设计】
//   传统信号处理的问题:
//     - 信号处理函数中能做的事极其有限 (async-signal-safe only)
//     - 不能调用 malloc, printf, 大部分库函数
//
//   libevent 的方案:
//     1. 注册 signal handler: sigaction(sig, handler)
//     2. handler 中只做一件事: 写 1 byte 到 socketpair/pipe 的一端
//     3. 另一端注册为 event(READ) → 主循环检测到可读 → 执行真正的回调
//     4. 回调在事件循环的正常上下文中执行 → 无限制!
//
//   这就是 self-pipe trick (我们在 Week 9 信号中学过!)

namespace ex9 {

// ── 模拟 self-pipe trick ─────────────────────────────────────────────
struct SignalHandler {
  int _signal_num;
  std::function<void(int)> _callback;

  // 模拟: pipe fds
  int _pipe_read = 10;   // 读端 (事件循环监控)
  int _pipe_write = 11;  // 写端 (信号处理器写入)

  // 模拟信号到达
  void simulate_signal(int sig) {
    println("    ★ 信号 ", sig, " 到达!");
    // 实际: write(_pipe_write, &sig, sizeof(sig))
    // → 主循环检测 _pipe_read 可读
    // → event_active → 执行下面的 callback
    if (_callback) _callback(sig);
  }
};

void run() {
  HR("Ex9: Signal Handling — 信号集成");

  println("self-pipe trick 流程:");
  println("  1. socketpair() 创建一对 fd");
  println("  2. sigaction(SIGINT, handler) — handler 只 write 1 byte");
  println("  3. 事件循环监控 pipe[0] 的 READ 事件");
  println("  4. 信号到达 → handler write byte → pipe[0] readable");
  println("  5. 事件循环执行真正的回调 (在正常上下文!)");
  println();

  SignalHandler sh;
  sh._signal_num = SIGINT;

  println("  模拟 SIGINT...");
  sh.simulate_signal(SIGINT);
  sh.simulate_signal(SIGTERM);
  println();

  println("📖 其他信号集成方式 (libevent 支持):");
  println("  1. self-pipe trick (传统方式)");
  println("  2. signalfd (Linux 2.6.22+) — 信号变成 fd, 直接读");
  println("  3. kqueue EVFILT_SIGNAL (macOS/BSD)");
  println();
  println("📖 精读 signal.c (~500行) + evsignal-internal.h");
}

} // namespace ex9

// ============================================================================
// Exercise 10: 完整 Echo Server + 架构全景
// ============================================================================
//
// 【阅读清单】
//   event.c            — 总览
//   sample/hello-world.c — libevent 自带示例
//
// 【整合架构】
//   main:
//     base = event_base_new()
//     listener = evconnlistener_new_bind(base, accept_cb, ...)
//     event_base_dispatch(base)
//
//   accept_cb(new_fd, addr):
//     bufev = bufferevent_socket_new(base, new_fd, BEV_OPT_CLOSE_ON_FREE)
//     bufferevent_setcb(bufev, read_cb, NULL, event_cb, NULL)
//     bufferevent_enable(bufev, EV_READ|EV_WRITE)
//
//   read_cb(bufev):
//     data = bufferevent_read(bufev)
//     bufferevent_write(bufev, data)  // echo
//
//   event_cb(bufev, what):
//     if BEV_EVENT_EOF → 对端关闭
//     if BEV_EVENT_ERROR → 错误
//     bufferevent_free(bufev)

namespace ex10 {

// ── 简化的 Echo Server (单线程) ─────────────────────────────────────
struct EchoServer {
  struct Connection {
    int fd;
    std::string name;
    std::string input_buf;
  };

  std::unordered_map<int, Connection> _conns;
  int _next_id = 1;

  void accept_connection(int fd) {
    int conn_fd = 100 + _next_id++;
    std::string name = "client_" + std::to_string(conn_fd);
    _conns[conn_fd] = {conn_fd, name, ""};
    println("  ✅ 新连接: ", name, " (fd=", conn_fd, ")");
    println("     注册 READ 事件 + 创建 input buffer");
  }

  void on_data(int fd, const std::string& data) {
    auto it = _conns.find(fd);
    if (it == _conns.end()) return;
    auto& conn = it->second;

    // 追加到输入缓冲
    conn.input_buf += data;

    // 查找完整行
    size_t pos;
    while ((pos = conn.input_buf.find('\n')) != std::string::npos) {
      std::string line = conn.input_buf.substr(0, pos);
      conn.input_buf.erase(0, pos + 1);
      // 去掉 \r
      if (!line.empty() && line.back() == '\r') line.pop_back();

      println("  📨 ", conn.name, " > '", line, "'");
      // echo
      println("  📤 echo > '", line, "' → ", conn.name);
      // 实际: bufferevent_write(bufev, data, len)
    }
  }

  void close_connection(int fd) {
    auto it = _conns.find(fd);
    if (it == _conns.end()) return;
    println("  ❌ 连接关闭: ", it->second.name);
    _conns.erase(it);
  }

  size_t conn_count() const { return _conns.size(); }
};

void run() {
  HR("Ex10: 完整 Echo Server + 架构全景");

  EchoServer server;

  println("启动 Echo Server...");
  println();

  // 模拟连接
  server.accept_connection(1);
  server.accept_connection(1);
  server.accept_connection(1);

  // 模拟数据
  server.on_data(100, "Hello, World!\n");
  server.on_data(101, "foo\nbar\nbaz\n");
  server.on_data(102, "partial line...");
  server.on_data(102, " now complete!\n");
  server.on_data(100, "exit\n");

  // 模拟关闭
  server.close_connection(100);

  println();
  println("活跃连接: ", server.conn_count());
  println();

  println("╔══════════════════════════════════════════════════════╗");
  println("║        libevent 完整架构全景                           ║");
  println("╠══════════════════════════════════════════════════════╣");
  println("║                                                      ║");
  println("║  main()                                              ║");
  println("║    │                                                 ║");
  println("║    ├─► event_base_new()                              ║");
  println("║    │     - 选择后端 (epoll > kqueue > poll > select)  ║");
  println("║    │     - 初始化 evmap / signal_map / timeheap       ║");
  println("║    │                                                 ║");
  println("║    ├─► evconnlistener_new_bind(base, cb, ...)        ║");
  println("║    │     - socket() + bind() + listen()               ║");
  println("║    │     - event(READ, listen_fd, listener_cb)        ║");
  println("║    │                                                 ║");
  println("║    └─► event_base_dispatch(base)                     ║");
  println("║          │                                           ║");
  println("║          └─► event_base_loop (无限循环):               ║");
  println("║                │                                     ║");
  println("║                ├─► timeout = min_heap_top()          ║");
  println("║                │                                     ║");
  println("║                ├─► evsel->dispatch()                 ║");
  println("║                │    (epoll_wait / kevent / poll)      ║");
  println("║                │                                     ║");
  println("║                ├─► 处理就绪 I/O:                      ║");
  println("║                │    event_active(ev, EV_READ/WRITE)   ║");
  println("║                │                                     ║");
  println("║                ├─► 处理超时:                          ║");
  println("║                │    min_heap_pop all expired          ║");
  println("║                │    event_active(ev, EV_TIMEOUT)      ║");
  println("║                │                                     ║");
  println("║                └─► 执行回调 (按优先级):                 ║");
  println("║                     for event in activequeues:        ║");
  println("║                       event->callback(fd, what, arg)   ║");
  println("║                                                      ║");
  println("║  accept_cb(fd, addr):                                ║");
  println("║    bufev = bufferevent_socket_new(base, fd, ...)     ║");
  println("║    bufferevent_setcb(bufev, read_cb, write_cb, ecb)  ║");
  println("║    bufferevent_enable(bufev, EV_READ|EV_WRITE)       ║");
  println("║                                                      ║");
  println("║  read_cb(bufev):                                     ║");
  println("║    data = bufferevent_get_input(bufev)               ║");
  println("║    → evbuffer (链式缓冲区)                            ║");
  println("║    bufferevent_write_buffer(bufev, data)             ║");
  println("║                                                      ║");
  println("╚══════════════════════════════════════════════════════╝");
  println();

  println("📊 核心文件概览:");
  println("  event.c            ~4000行 — event_base + event + event loop");
  println("  buffer.c           ~3000行 — evbuffer 链式缓冲区");
  println("  bufferevent.c      ~3000行 — bufferevent 缓冲 I/O");
  println("  epoll.c            ~400行  — epoll 后端");
  println("  listener.c         ~400行  — evconnlistener 监听器");
  println("  signal.c           ~500行  — 信号集成");
  println("  minheap-internal.h ~150行  — 定时器最小堆");
  println("  evmap.c            ~500行  — fd→event 映射");
  println("  evthread.c         ~800行  — 线程安全支持");
  println("  http.c             ~4000行 — 内置 HTTP 服务器!");
  println();

  println("🔑 libevent 最值得学习的 10 个设计:");
  println("  1. Reactor 模式 — 单线程事件驱动, 打败多线程 I/O");
  println("  2. Backend 抽象 (vtable) — 统一 epoll/kqueue/poll/select");
  println("  3. 最小堆定时器 — O(log n) 插入/删除, heap_index O(1) 定位");
  println("  4. 链式 evbuffer — 无 realloc, scatter/gather I/O, 引用计数");
  println("  5. bufferevent 双缓冲 — 输入/输出自动管理, watermark 背压");
  println("  6. self-pipe trick — 信号安全地集成到事件循环");
  println("  7. TAILQ (FreeBSD queue.h) — 侵入式链表, 零额外分配");
  println("  8. evmap — fd→events 高效映射, 支持多事件 per fd");
  println("  9. 优先级队列 — 事件按优先级执行, 低延迟事件优先");
  println("  10. 可嵌入 HTTP/DNS/RPC — 内置应用层协议, events 之上");
}

} // namespace ex10

// ============================================================================
// Main
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║     Month 5 Week 26: libevent 源码阅读                         ║
║     "看透实现 — Reactor 模式的 C 语言典范"                      ║
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

  HR("Week 26 完成!");
  println("✅ 理解了 Reactor 模式 (event_base + event + callback)");
  println("✅ 理解了 I/O 后端抽象 (epoll/kqueue/poll/select vtable)");
  println("✅ 理解了最小堆定时器 (O(log n) push/pop)");
  println("✅ 理解了 evbuffer 链式缓冲区 (无 realloc, scatter/gather)");
  println("✅ 理解了 bufferevent 双缓冲 + watermark 背压");
  println("✅ 理解了 self-pipe trick 信号集成");
  println("✅ 理解了 evconnlistener accept 抽象");
  println("✅ 理解了完整 Echo Server 架构");
  println("✅ 下一步: Week 27 — 小型 STL 实现");
  println();
  println("📖 推荐继续阅读:");
  println("  1. libuv (Node.js 的底层, libevent 的精神继承者)");
  println("  2. memcached (使用 libevent 的经典项目)");
  println("  3. C10K problem (http://www.kegel.com/c10k.html) — Reactor 模式的历史背景");
  return 0;
}
