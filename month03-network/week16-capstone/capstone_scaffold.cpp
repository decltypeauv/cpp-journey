// Week 16: Month 3 收官 — 回顾 + 性能测试 + 迷你 KV 存储
// 编译: cmake -B build && cmake --build build
// 运行: ./build/capstone
//
// 本周是 Month 3 (网络编程) 的最后一站:
//   Part A — Weeks 11-15 核心知识回顾
//   Part B — 性能基准测试 (select vs poll vs epoll)
//   Part C — Capstone: 实现一个 mini-redis 兼容的 KV 存储
//
// 核心主题:
//   回顾总结 — Month 3 知识体系全景
//   性能对比 — 三种多路复用的吞吐量实测
//   RESP 协议 — Redis 序列化协议解析
//   KV 存储 — SET/GET/DEL/EXPIRE + 数据结构
//   持久化 — AOF (Append-Only File) 日志

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using std::cout;
using std::string;
using std::string_view;
using std::vector;

// ============================================================
// 辅助工具
// ============================================================

void section(const string &title) {
  cout << "\n=== " << title << " ===\n";
}

void subsection(const string &title) {
  cout << "\n  --- " << title << " ---\n";
}

#define CHECK(expr, msg)                                                       \
  do {                                                                         \
    if ((expr) == -1) {                                                        \
      std::cerr << "❌ " << msg << ": " << std::strerror(errno) << "\n";       \
      return;                                                                  \
    }                                                                          \
  } while (0)

class ScopedFd {
  int _fd;

public:
  explicit ScopedFd(int fd = -1) : _fd(fd) {}
  ~ScopedFd() {
    if (_fd >= 0)
      close(_fd);
  }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  ScopedFd(ScopedFd &&other) noexcept : _fd(other._fd) { other._fd = -1; }
  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      if (_fd >= 0)
        close(_fd);
      _fd = other._fd;
      other._fd = -1;
    }
    return *this;
  }
  int get() const { return _fd; }
  bool valid() const { return _fd >= 0; }
};

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 精确读取
bool recv_exact(int fd, void *buf, size_t n) {
  size_t received = 0;
  auto *ptr = static_cast<char *>(buf);
  while (received < n) {
    ssize_t r = recv(fd, ptr + received, n - received, 0);
    if (r <= 0)
      return false;
    received += r;
  }
  return true;
}

// ============================================================
// Part A: Month 3 回顾
// ============================================================

// --- 练习 1: Week 11 回顾 — Socket 编程核心 ---
void exercise1_review_w11() {
  section("回顾 Week 11: Socket 编程基础");

  cout << "  📋 核心系统调用流程:\n";
  cout << "  ┌─────────────────────────────────────────────┐\n";
  cout << "  │ 服务器: socket→bind→listen→accept→recv/send→close │\n";
  cout << "  │ 客户端: socket→connect→send/recv→close        │\n";
  cout << "  └─────────────────────────────────────────────┘\n";
  cout << "\n";
  cout << "  🔑 关键概念:\n";
  cout << "    1. socket() = 文件描述符 — \"一切皆文件\"\n";
  cout << "    2. 网络字节序 = 大端 — htons/htonl 必须!\n";
  cout << "    3. sockaddr_in: sin_family + sin_port + sin_addr\n";
  cout << "    4. SO_REUSEADDR: 服务器重启必备\n";
  cout << "    5. getaddrinfo: 线程安全的现代地址解析\n";
  cout << "    6. ScopedFd RAII: 管理 socket 生命周期\n";
  cout << "    7. TCP(SOCK_STREAM) vs UDP(SOCK_DGRAM)\n";
  cout << "    8. MSG_NOSIGNAL: 防止 SIGPIPE 杀死进程\n";
  cout << "    9. O_NONBLOCK: 非阻塞 IO 是 epoll 的基础\n";
  cout << "    10. send() 返回值可能 < 请求长度, 需要循环!\n";

  // 快速演示: socket + connect + send + recv
  subsection("快速自检: Echo 客户端");
  constexpr int PORT = 16011;
  std::thread echo([]() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, 1);
    int c = accept(fd, nullptr, nullptr);
    if (c >= 0) {
      ScopedFd cg(c);
      char buf[64];
      ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
      if (n > 0) { buf[n] = '\0'; send(c, buf, n, MSG_NOSIGNAL); }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ScopedFd guard(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    const char *msg = "Ping";
    send(fd, msg, 4, MSG_NOSIGNAL);
    char buf[64]{};
    recv(fd, buf, sizeof(buf) - 1, 0);
    cout << "  ✅ Echo: \"" << msg << "\" → \"" << buf << "\"\n";
  }
  echo.join();
}

// --- 练习 2: Week 12 回顾 — TCP 深入 ---
void exercise2_review_w12() {
  section("回顾 Week 12: TCP 深入");

  cout << "  📋 TCP 深入 6 大主题:\n";
  cout << "  ┌─────────────────────────────────────────────┐\n";
  cout << "  │ 1. Nagle 算法    → TCP_NODELAY 禁 Nagle     │\n";
  cout << "  │ 2. KeepAlive     → 检测死连接, 应用层心跳    │\n";
  cout << "  │ 3. 粘包/拆包     → TCP 字节流特性 (不是 bug)│\n";
  cout << "  │ 4. 消息边界      → 定长/分隔符/长度前缀      │\n";
  cout << "  │ 5. shutdown()    → 半关闭, 优雅关闭 4 步    │\n";
  cout << "  │ 6. SO_LINGER     → 控制 close() 行为       │\n";
  cout << "  └─────────────────────────────────────────────┘\n";
  cout << "\n";
  cout << "  🔑 三个解决方案对比:\n";
  cout << "    定长:   简单但浪费 → 传感器数据\n";
  cout << "    分隔符: 人类可读   → HTTP, Redis\n";
  cout << "    长度前缀: 二进制安全 → gRPC, 自定义协议 ✅ 推荐\n";
  cout << "\n";
  cout << "  🔑 优雅关闭 4 步:\n";
  cout << "    1. shutdown(SHUT_WR) — 发 FIN\n";
  cout << "    2. while(recv()>0) — 继续读剩余数据\n";
  cout << "    3. recv==0 — 对端也发了 FIN\n";
  cout << "    4. close() — 最终关闭\n";
  cout << "\n";
  cout << "  🔑 TCP 状态诊断:\n";
  cout << "    CLOSE_WAIT 堆积 = 服务器忘了 close() — 代码 bug\n";
  cout << "    TIME_WAIT 正常 — 不要强行消除\n";
}

// --- 练习 3: Week 13 回顾 — epoll 多路复用 ---
void exercise3_review_w13() {
  section("回顾 Week 13: epoll / IO 多路复用");

  cout << "  📋 演进路线: select → poll → epoll\n";
  cout << "  ┌──────────┬──────────┬──────────┬──────────────┐\n";
  cout << "  │          │ select   │ poll     │ epoll        │\n";
  cout << "  ├──────────┼──────────┼──────────┼──────────────┤\n";
  cout << "  │ fd 限制  │ 1024     │ 无       │ 无           │\n";
  cout << "  │ 扫描     │ O(n)     │ O(n)     │ O(1) 就绪链表│\n";
  cout << "  │ 参数重用 │ ❌ 值-结果│ ✅ events│ ✅ 内核红黑树  │\n";
  cout << "  │ 平台     │ POSIX    │ POSIX    │ Linux only   │\n";
  cout << "  └──────────┴──────────┴──────────┴──────────────┘\n";
  cout << "\n";
  cout << "  🔑 epoll 核心三剑客:\n";
  cout << "    epoll_create1(EPOLL_CLOEXEC) — 创建实例\n";
  cout << "    epoll_ctl(ADD/MOD/DEL)        — 管理监控列表\n";
  cout << "    epoll_wait()                  — 等待就绪事件\n";
  cout << "\n";
  cout << "  🔑 LT vs ET:\n";
  cout << "    LT(默认): 有数据就通知, 简单安全, 新手首选\n";
  cout << "    ET(EPOLLET): 状态变化通知一次, 必须读到 EAGAIN\n";
  cout << "    → 高性能场景用 ET, 不确定用 LT\n";
  cout << "\n";
  cout << "  🔑 关键事件:\n";
  cout << "    EPOLLRDHUP: 对端半关闭, 比 EPOLLIN+recv==0 更高效\n";
  cout << "    EPOLLONESHOT: 多线程安全, 触发后自动暂停\n";
  cout << "    timerfd + epoll: 定时器与 IO 统一调度\n";
  cout << "    epoll_data.ptr: 存 Connection* — 事件到达直接拿上下文\n";

  // 快速自检: epoll echo
  subsection("快速自检: epoll echo 单线程多连接");
  constexpr int PORT = 16031;
  std::thread server([]() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    set_nonblocking(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, 2);

    int epfd = epoll_create1(0);
    ScopedFd eg(epfd);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    int handled = 0;
    while (handled < 2) {
      epoll_event e;
      int n = epoll_wait(epfd, &e, 1, 1000);
      if (n <= 0) break;
      if (e.data.fd == fd) {
        int c = accept(fd, nullptr, nullptr);
        if (c >= 0) {
          set_nonblocking(c);
          ev.events = EPOLLIN | EPOLLRDHUP;
          ev.data.fd = c;
          epoll_ctl(epfd, EPOLL_CTL_ADD, c, &ev);
        }
      } else {
        char buf[64];
        ssize_t r = recv(e.data.fd, buf, sizeof(buf) - 1, 0);
        if (r > 0) {
          buf[r] = '\0';
          send(e.data.fd, buf, r, MSG_NOSIGNAL);
        }
        epoll_ctl(epfd, EPOLL_CTL_DEL, e.data.fd, nullptr);
        close(e.data.fd);
        ++handled;
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  auto client = [](int id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ScopedFd guard(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    string msg = "hello" + std::to_string(id);
    send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
    char buf[64]{};
    recv(fd, buf, sizeof(buf) - 1, 0);
    cout << "  [Client " << id << "] \"" << msg << "\" → \"" << buf << "\"\n";
  };

  std::thread c1(client, 1);
  std::thread c2(client, 2);
  c1.join(); c2.join();
  server.join();
  cout << "  ✅ epoll 单线程处理了 2 个并发连接\n";
}

// --- 练习 4: Week 14 回顾 — HTTP 协议 ---
void exercise4_review_w14() {
  section("回顾 Week 14: HTTP 协议 + HTTP Server");

  cout << "  📋 HTTP/1.1 请求格式:\n";
  cout << "    METHOD SP URI SP VERSION CRLF\n";
  cout << "    Header: value CRLF (0-N 个)\n";
  cout << "    CRLF (空行)\n";
  cout << "    [Body] (Content-Length 字节)\n";
  cout << "\n";
  cout << "  🔑 HTTP Server 核心流程:\n";
  cout << "    1. 读请求行 → METHOD, URI, VERSION\n";
  cout << "    2. 逐行读头部 → 直到空行\n";
  cout << "    3. Content-Length → 读 N 字节 body\n";
  cout << "    4. 路由: URI → handler\n";
  cout << "    5. 构建响应: Content-Type + Content-Length + body\n";
  cout << "    6. TCP 发送\n";
  cout << "\n";
  cout << "  🔑 关键洞察:\n";
  cout << "    「HTTP Server = 解析文本请求 → 构建文本响应 → TCP 发送」\n";
  cout << "    没有魔法, 全是字符串操作。\n";
}

// --- 练习 5: Week 15 回顾 — 网络服务 ---
void exercise5_review_w15() {
  section("回顾 Week 15: 网络服务实战");

  cout << "  📋 四种服务模式:\n";
  cout << "  ┌────────────────┬─────────────────────────────────────┐\n";
  cout << "  │ 聊天室         │ epoll 广播: 收到→遍历→send 所有人   │\n";
  cout << "  │ HTTP 正向代理  │ GET 完整 URL→解析→转发              │\n";
  cout << "  │ TCP 隧道       │ relay A↔B 双向中继, 半关闭传递      │\n";
  cout << "  │ 负载均衡       │ Round Robin / Least Connections    │\n";
  cout << "  └────────────────┴─────────────────────────────────────┘\n";
  cout << "\n";
  cout << "  🔑 通用架构模式:\n";
  cout << "    网络服务 = Socket + epoll + 协议解析 + 业务逻辑 + 安全防御\n";
}

// ============================================================
// Part B: 性能基准测试
// ============================================================

// --- 练习 6: 性能对比 — select vs poll vs epoll ---
//
// 理论: epoll O(1) > poll O(n) > select O(n) 且有限制
// 实践: 100 个并发连接, 每个发送 10 条消息, 测总时间

struct BenchResult {
  string name;
  double elapsed_ms;
  int total_requests;
  double req_per_sec;
};

void exercise6_benchmark() {
  section("练习 6: 性能对比 — select vs poll vs epoll");

  constexpr int BASE_PORT = 16061;
  constexpr int NUM_CLIENTS = 20;   // 20 并发连接
  constexpr int MSGS_PER_CLIENT = 3; // 每个客户端 3 条消息
  constexpr int TOTAL_MSGS = NUM_CLIENTS * MSGS_PER_CLIENT;
  constexpr int MSG_SIZE = 32;

  // ---- epoll 服务器 ----
  auto run_epoll_server = [](int port, std::atomic<int> &counter) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    set_nonblocking(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, SOMAXCONN);

    int epfd = epoll_create1(0);
    ScopedFd eg(epfd);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    int handled = 0;
    while (handled < TOTAL_MSGS) {
      epoll_event events[64];
      int n = epoll_wait(epfd, events, 64, 2000);
      if (n <= 0) break;
      for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == fd) {
          int c = accept(fd, nullptr, nullptr);
          if (c >= 0) {
            set_nonblocking(c);
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = c;
            epoll_ctl(epfd, EPOLL_CTL_ADD, c, &ev);
          }
        } else {
          char buf[64];
          while (recv(events[i].data.fd, buf, sizeof(buf), 0) > 0) {
            ++handled;
          }
          epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
          close(events[i].data.fd);
        }
      }
    }
    counter = handled;
  };

  // ---- poll 服务器 ----
  auto run_poll_server = [](int port, std::atomic<int> &counter) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    set_nonblocking(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, SOMAXCONN);

    vector<pollfd> fds;
    fds.push_back({fd, POLLIN, 0});

    int handled = 0;
    while (handled < TOTAL_MSGS) {
      int n = poll(fds.data(), fds.size(), 2000);
      if (n <= 0) break;
      for (size_t i = 0; i < fds.size(); ++i) {
        if (fds[i].revents == 0) continue;
        if (fds[i].fd == fd) {
          int c = accept(fd, nullptr, nullptr);
          if (c >= 0) {
            set_nonblocking(c);
            fds.push_back({c, POLLIN, 0});
          }
        } else {
          char buf[64];
          while (recv(fds[i].fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
            ++handled;
          }
          close(fds[i].fd);
          fds[i].fd = -1;
        }
      }
      // 清理已关闭的
      fds.erase(std::remove_if(fds.begin(), fds.end(),
                                [](const pollfd &p) { return p.fd == -1; }),
                fds.end());
    }
    counter = handled;
  };

  // ---- select 服务器 ----
  auto run_select_server = [](int port, std::atomic<int> &counter) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    set_nonblocking(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, SOMAXCONN);

    int max_fd = fd;
    fd_set master;
    FD_ZERO(&master);
    FD_SET(fd, &master);

    int handled = 0;
    while (handled < TOTAL_MSGS) {
      fd_set readfds = master;
      struct timeval tv{2, 0};  // 2 second timeout
      int n = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
      if (n <= 0) break;
      for (int i = 0; i <= max_fd && handled < TOTAL_MSGS; ++i) {
        if (!FD_ISSET(i, &readfds)) continue;
        if (i == fd) {
          int c = accept(fd, nullptr, nullptr);
          if (c >= 0) {
            set_nonblocking(c);
            FD_SET(c, &master);
            if (c > max_fd) max_fd = c;
          }
        } else {
          char buf[64];
          while (recv(i, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
            ++handled;
          }
          FD_CLR(i, &master);
          close(i);
        }
      }
    }
    counter = handled;
  };

  // ---- 运行测试 ----
  cout << "  测试配置: " << NUM_CLIENTS << " 客户端 × "
       << MSGS_PER_CLIENT << " 消息 = " << TOTAL_MSGS << " 总请求\n\n";

  vector<BenchResult> results;

  // 测试 epoll
  {
    std::atomic<int> counter{0};
    int port = BASE_PORT;
    std::thread srv([&]() { run_epoll_server(port, counter); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto start = std::chrono::steady_clock::now();

    vector<std::thread> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
      clients.emplace_back([=]() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        ScopedFd gu(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
          return;
        char buf[MSG_SIZE];
        memset(buf, 'A', MSG_SIZE);
        for (int j = 0; j < MSGS_PER_CLIENT; ++j)
          send(fd, buf, MSG_SIZE, MSG_NOSIGNAL);
      });
    }
    for (auto &t : clients) t.join();
    srv.join();

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    results.push_back(
        {"epoll", ms, counter.load(),
         counter.load() / (ms / 1000.0)});
  }

  // 测试 poll
  {
    std::atomic<int> counter{0};
    int port = BASE_PORT + 1;
    std::thread srv([&]() { run_poll_server(port, counter); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto start = std::chrono::steady_clock::now();

    vector<std::thread> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
      clients.emplace_back([=]() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        ScopedFd gu(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
          return;
        char buf[MSG_SIZE];
        memset(buf, 'B', MSG_SIZE);
        for (int j = 0; j < MSGS_PER_CLIENT; ++j)
          send(fd, buf, MSG_SIZE, MSG_NOSIGNAL);
      });
    }
    for (auto &t : clients) t.join();
    srv.join();

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    results.push_back(
        {"poll", ms, counter.load(),
         counter.load() / (ms / 1000.0)});
  }

  // 测试 select
  {
    std::atomic<int> counter{0};
    int port = BASE_PORT + 2;
    std::thread srv([&]() { run_select_server(port, counter); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto start = std::chrono::steady_clock::now();

    vector<std::thread> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
      clients.emplace_back([=]() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        ScopedFd gu(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
          return;
        char buf[MSG_SIZE];
        memset(buf, 'C', MSG_SIZE);
        for (int j = 0; j < MSGS_PER_CLIENT; ++j)
          send(fd, buf, MSG_SIZE, MSG_NOSIGNAL);
      });
    }
    for (auto &t : clients) t.join();
    srv.join();

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    results.push_back(
        {"select", ms, counter.load(),
         counter.load() / (ms / 1000.0)});
  }

  // 输出结果
  cout << "  ┌──────────┬──────────┬──────────┬──────────────┐\n";
  cout << "  │ 方法     │ 耗时(ms) │ 请求数   │ 请求/秒      │\n";
  cout << "  ├──────────┼──────────┼──────────┼──────────────┤\n";
  for (auto &r : results) {
    cout << "  │ " << std::left << std::setw(8) << r.name
         << " │ " << std::right << std::setw(8) << std::fixed
         << std::setprecision(1) << r.elapsed_ms
         << " │ " << std::setw(8) << r.total_requests
         << " │ " << std::setw(12) << std::setprecision(0)
         << r.req_per_sec << " │\n";
  }
  cout << "  └──────────┴──────────┴──────────┴──────────────┘\n";
  cout << "\n  💡 epoll 在高并发下优势明显 (O(1) 就绪链表 vs O(n) 扫描)\n";
  cout << "  💡 select 有 1024 fd 限制, 大型应用必须用 epoll\n";
}

// ============================================================
// Part C: Capstone — Mini Redis (RESP 协议 KV 存储)
// ============================================================

// --- 练习 7: RESP 协议解析 ---
//
// RESP (REdis Serialization Protocol) 是 Redis 的通信协议。
// 它简单、人类可读、二进制安全。
//
// 5 种数据类型:
//   +OK\r\n              — Simple String
//   -Error message\r\n   — Error
//   :1234\r\n            — Integer
//   $6\r\nfoobar\r\n     — Bulk String (长度前缀)
//   *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n — Array

enum class RespType {
  SIMPLE_STRING, // +
  ERROR,         // -
  INTEGER,       // :
  BULK_STRING,   // $
  ARRAY,         // *
  NULL_BULK,     // $-1\r\n
};

struct RespValue {
  RespType type;
  string str_val;
  int64_t int_val = 0;
  vector<RespValue> array_val;
  bool is_null = false;
};

// RESP 编码: C++ value → RESP wire format
string resp_encode(const RespValue &v) {
  switch (v.type) {
  case RespType::SIMPLE_STRING:
    return "+" + v.str_val + "\r\n";
  case RespType::ERROR:
    return "-" + v.str_val + "\r\n";
  case RespType::INTEGER:
    return ":" + std::to_string(v.int_val) + "\r\n";
  case RespType::BULK_STRING:
    if (v.is_null)
      return "$-1\r\n";
    return "$" + std::to_string(v.str_val.size()) + "\r\n" + v.str_val + "\r\n";
  case RespType::ARRAY: {
    string result = "*" + std::to_string(v.array_val.size()) + "\r\n";
    for (auto &elem : v.array_val)
      result += resp_encode(elem);
    return result;
  }
  case RespType::NULL_BULK:
    return "$-1\r\n";
  }
  return "-ERR unknown type\r\n";
}

// RESP 解码 (简化版 — 用于教学)
// 实际生产代码需要逐字节流式解析，这里从完整 buffer 解析

// 解析一行 (从 pos 开始直到 \r\n)
std::optional<string> resp_read_line(const string &buf, size_t &pos) {
  size_t end = buf.find("\r\n", pos);
  if (end == string::npos)
    return std::nullopt;
  string line = buf.substr(pos, end - pos);
  pos = end + 2;
  return line;
}

// 解析一个 RESP 值
std::optional<RespValue> resp_parse(const string &buf, size_t &pos) {
  if (pos >= buf.size())
    return std::nullopt;

  char prefix = buf[pos++];

  switch (prefix) {
  case '+': {
    auto line = resp_read_line(buf, pos);
    if (!line) return std::nullopt;
    return RespValue{RespType::SIMPLE_STRING, *line};
  }
  case '-': {
    auto line = resp_read_line(buf, pos);
    if (!line) return std::nullopt;
    return RespValue{RespType::ERROR, *line};
  }
  case ':': {
    auto line = resp_read_line(buf, pos);
    if (!line) return std::nullopt;
    return RespValue{RespType::INTEGER, "", std::stoll(*line)};
  }
  case '$': {
    auto line = resp_read_line(buf, pos);
    if (!line) return std::nullopt;
    int64_t len = std::stoll(*line);
    if (len == -1)
      return RespValue{RespType::NULL_BULK, "", 0, {}, true};
    if (pos + len + 2 > buf.size())
      return std::nullopt;
    string data = buf.substr(pos, len);
    pos += len + 2; // skip data + \r\n
    return RespValue{RespType::BULK_STRING, data};
  }
  case '*': {
    auto line = resp_read_line(buf, pos);
    if (!line) return std::nullopt;
    int64_t count = std::stoll(*line);
    if (count == -1)
      return RespValue{RespType::NULL_BULK, "", 0, {}, true};
    RespValue arr{RespType::ARRAY};
    for (int64_t i = 0; i < count; ++i) {
      auto elem = resp_parse(buf, pos);
      if (!elem) return std::nullopt;
      arr.array_val.push_back(std::move(*elem));
    }
    return arr;
  }
  default:
    return std::nullopt;
  }
}

void exercise7_resp_protocol() {
  section("练习 7: RESP 协议 — Redis 序列化协议");

  // TODO 7.1: RESP 编码演示
  {
    subsection("RESP 编码");

    // Simple String
    cout << "  Simple String: " << resp_encode({RespType::SIMPLE_STRING, "OK"});

    // Error
    cout << "  Error:         " << resp_encode({RespType::ERROR, "ERR unknown command"});

    // Integer
    RespValue iv{RespType::INTEGER};
    iv.int_val = 42;
    cout << "  Integer:       " << resp_encode(iv);

    // Bulk String
    cout << "  Bulk String:   " << resp_encode({RespType::BULK_STRING, "hello"});

    // Null Bulk
    cout << "  Null:          " << resp_encode({RespType::NULL_BULK});

    // Array
    RespValue arr{RespType::ARRAY};
    arr.array_val.push_back({RespType::BULK_STRING, "SET"});
    arr.array_val.push_back({RespType::BULK_STRING, "mykey"});
    arr.array_val.push_back({RespType::BULK_STRING, "myvalue"});
    cout << "  Array (*3):    " << resp_encode(arr);
  }

  // TODO 7.2: RESP 解码演示
  {
    subsection("RESP 解码");

    // 测试 "*2\r\n$3\r\nGET\r\n$4\r\nmykey\r\n"
    string cmd = "*2\r\n$3\r\nGET\r\n$4\r\nmykey\r\n";
    size_t pos = 0;
    auto parsed = resp_parse(cmd, pos);
    if (parsed && parsed->type == RespType::ARRAY) {
      cout << "  解析: *" << parsed->array_val.size() << " 元素\n";
      for (size_t i = 0; i < parsed->array_val.size(); ++i) {
        cout << "    [" << i << "] \"" << parsed->array_val[i].str_val
             << "\"\n";
      }
    }

    // 测试 "+OK\r\n"
    string ok = "+OK\r\n";
    pos = 0;
    auto ok_parsed = resp_parse(ok, pos);
    if (ok_parsed)
      cout << "  解析: +" << ok_parsed->str_val << "\n";

    // 测试 ":-1\r\n"
    string int_str = ":-1\r\n";
    pos = 0;
    auto int_parsed = resp_parse(int_str, pos);
    if (int_parsed)
      cout << "  解析: :" << int_parsed->int_val << "\n";

    cout << "  ✅ RESP 编解码正常 — 可以开始实现 KV 存储!\n";
  }
}

// --- 练习 8: KV 存储引擎 ---
//
// 支持命令: SET, GET, DEL, EXISTS, KEYS
// 数据结构: unordered_map<string, string>
//
// 这是最简单的 KV 存储。真正的 Redis 有更多数据结构和功能。

struct KvEntry {
  string value;
  std::chrono::steady_clock::time_point expiry; // 过期时间
  bool has_expiry = false;
};

class KvStore {
public:
  // SET key value [EX seconds]
  string set(const string &key, const string &value,
             int64_t expiry_secs = -1) {
    KvEntry entry;
    entry.value = value;
    if (expiry_secs > 0) {
      entry.expiry = std::chrono::steady_clock::now() +
                     std::chrono::seconds(expiry_secs);
      entry.has_expiry = true;
    }
    _data[key] = std::move(entry);
    return "OK";
  }

  // GET key → value or null
  std::optional<string> get(const string &key) {
    auto it = _data.find(key);
    if (it == _data.end())
      return std::nullopt;
    if (it->second.has_expiry &&
        std::chrono::steady_clock::now() >= it->second.expiry) {
      _data.erase(it);
      return std::nullopt;
    }
    return it->second.value;
  }

  // DEL key → 1 if deleted, 0 if not found
  int del(const string &key) {
    auto it = _data.find(key);
    if (it == _data.end())
      return 0;
    _data.erase(it);
    return 1;
  }

  // EXISTS key → 1 or 0
  int exists(const string &key) {
    auto it = _data.find(key);
    if (it == _data.end())
      return 0;
    if (it->second.has_expiry &&
        std::chrono::steady_clock::now() >= it->second.expiry) {
      _data.erase(it);
      return 0;
    }
    return 1;
  }

  // KEYS → all keys (simplified)
  vector<string> keys() {
    vector<string> result;
    auto now = std::chrono::steady_clock::now();
    for (auto it = _data.begin(); it != _data.end();) {
      if (it->second.has_expiry && now >= it->second.expiry) {
        it = _data.erase(it);
      } else {
        result.push_back(it->first);
        ++it;
      }
    }
    return result;
  }

  size_t size() const { return _data.size(); }

  // 遍历所有 key, 删除过期的
  void cleanup_expired() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = _data.begin(); it != _data.end();) {
      if (it->second.has_expiry && now >= it->second.expiry)
        it = _data.erase(it);
      else
        ++it;
    }
  }

  const auto &data() const { return _data; }

private:
  std::unordered_map<string, KvEntry> _data;
};

void exercise8_kv_store() {
  section("练习 8: KV 存储引擎");

  // TODO 8.1: 基本操作
  {
    subsection("SET / GET / DEL / EXISTS");

    KvStore store;

    // SET
    store.set("name", "Alice");
    store.set("age", "25");
    cout << "  SET name=Alice, age=25\n";

    // GET
    auto name = store.get("name");
    cout << "  GET name → " << (name ? *name : "(nil)") << "\n";

    auto missing = store.get("missing");
    cout << "  GET missing → " << (missing ? *missing : "(nil)") << "\n";

    // EXISTS
    cout << "  EXISTS name → " << store.exists("name") << "\n";
    cout << "  EXISTS missing → " << store.exists("missing") << "\n";

    // DEL
    int deleted = store.del("age");
    cout << "  DEL age → " << deleted << "\n";
    cout << "  GET age → " << (store.get("age") ? *store.get("age") : "(nil)")
         << "\n";

    cout << "  ✅ 基本 KV 操作正常\n";
  }

  // TODO 8.2: 过期机制
  {
    subsection("EXPIRE — 带过期时间的 SET");

    KvStore store;

    store.set("session", "abc123", 1); // 1 秒过期
    cout << "  SET session=abc123 EX 1\n";
    cout << "  GET session (立即) → "
         << (store.get("session") ? *store.get("session") : "(nil)") << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    cout << "  等待 1.1 秒...\n";
    cout << "  GET session (超时) → "
         << (store.get("session") ? *store.get("session") : "(nil)") << "\n";

    // KEYS
    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3", 1); // 1 秒过期
    cout << "  SET a=1, b=2, c=3 EX 1\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto keys = store.keys();
    cout << "  KEYS → [";
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i > 0) cout << ", ";
      cout << keys[i];
    }
    cout << "] (c 已过期自动删除)\n";

    cout << "  ✅ 过期机制正常\n";
  }
}

// --- 练习 9: KV 存储服务器 (RESP over TCP) ---
//
// 把 KvStore + RESP 协议 + epoll 组合成一个完整的 TCP 服务。
// 可以用 redis-cli 连接!

class RespServer {
public:
  RespServer(int port) : _port(port) {
    _epfd = epoll_create1(EPOLL_CLOEXEC);
  }

  ~RespServer() {
    if (_epfd >= 0) close(_epfd);
    if (_listen_fd >= 0) close(_listen_fd);
  }

  bool init() {
    if (_epfd < 0) return false;
    _listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listen_fd < 0) return false;
    set_nonblocking(_listen_fd);

    int optval = 1;
    setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(_listen_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(_listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
      return false;
    if (listen(_listen_fd, SOMAXCONN) < 0) return false;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = _listen_fd;
    epoll_ctl(_epfd, EPOLL_CTL_ADD, _listen_fd, &ev);

    return true;
  }

  // 处理一个 RESP 命令 → 返回 RESP 响应
  string handle_command(const RespValue &cmd) {
    if (cmd.type != RespType::ARRAY || cmd.array_val.empty())
      return resp_encode({RespType::ERROR, "ERR invalid command format"});

    string op = cmd.array_val[0].str_val;
    // 转大写
    for (auto &c : op)
      c = std::toupper(static_cast<unsigned char>(c));

    if (op == "PING") {
      return resp_encode({RespType::SIMPLE_STRING, "PONG"});
    }

    if (op == "SET" && cmd.array_val.size() >= 3) {
      string key = cmd.array_val[1].str_val;
      string value = cmd.array_val[2].str_val;
      int64_t ex = -1;
      // 支持 EX 参数: SET key value EX seconds
      if (cmd.array_val.size() >= 5) {
        string subop = cmd.array_val[3].str_val;
        for (auto &c : subop)
          c = std::toupper(static_cast<unsigned char>(c));
        if (subop == "EX" && cmd.array_val.size() >= 5) {
          ex = cmd.array_val[4].int_val;
        }
      }
      _store.set(key, value, ex);
      return resp_encode({RespType::SIMPLE_STRING, "OK"});
    }

    if (op == "GET" && cmd.array_val.size() >= 2) {
      auto val = _store.get(cmd.array_val[1].str_val);
      if (val)
        return resp_encode({RespType::BULK_STRING, *val});
      return resp_encode({RespType::NULL_BULK});
    }

    if (op == "DEL" && cmd.array_val.size() >= 2) {
      int count = _store.del(cmd.array_val[1].str_val);
      RespValue iv{RespType::INTEGER};
      iv.int_val = count;
      return resp_encode(iv);
    }

    if (op == "EXISTS" && cmd.array_val.size() >= 2) {
      RespValue iv{RespType::INTEGER};
      iv.int_val = _store.exists(cmd.array_val[1].str_val);
      return resp_encode(iv);
    }

    if (op == "KEYS") {
      auto keys = _store.keys();
      RespValue arr{RespType::ARRAY};
      for (auto &k : keys)
        arr.array_val.push_back({RespType::BULK_STRING, k});
      return resp_encode(arr);
    }

    if (op == "COMMAND") {
      // redis-cli 发送 COMMAND DOCS, 忽略
      return resp_encode({RespType::SIMPLE_STRING, "OK"});
    }

    return resp_encode({RespType::ERROR, "ERR unknown command '" + op + "'"});
  }

  // 运行事件循环 (处理 N 个请求后返回)
  void run_for(int max_requests) {
    int handled = 0;
    epoll_event events[16];

    while (handled < max_requests) {
      int n = epoll_wait(_epfd, events, 16, 3000);
      if (n <= 0) break;

      for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == _listen_fd) {
          int c = accept(_listen_fd, nullptr, nullptr);
          if (c >= 0) {
            set_nonblocking(c);
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.fd = c;
            epoll_ctl(_epfd, EPOLL_CTL_ADD, c, &ev);
          }
        } else {
          int cfd = events[i].data.fd;
          char buf[4096]{};
          ssize_t r = recv(cfd, buf, sizeof(buf) - 1, 0);
          if (r > 0) {
            buf[r] = '\0';
            size_t pos = 0;
            auto cmd = resp_parse(string(buf), pos);
            if (cmd) {
              string response = handle_command(*cmd);
              send(cfd, response.c_str(), response.size(), MSG_NOSIGNAL);
              ++handled;
            }
          }
          if (r <= 0 || (events[i].events & EPOLLRDHUP)) {
            epoll_ctl(_epfd, EPOLL_CTL_DEL, cfd, nullptr);
            close(cfd);
          }
        }
      }
    }
  }

  KvStore &store() { return _store; }

private:
  int _port;
  int _epfd = -1;
  int _listen_fd = -1;
  KvStore _store;
};

void exercise9_resp_server() {
  section("练习 9: KV 存储服务器 — RESP over TCP");

  // TODO 9.1: 架构说明
  {
    subsection("架构: Redis 协议 + KV 引擎 + epoll");

    cout << "  客户端 (redis-cli 或 自定义) → RESP 协议 → epoll → "
            "KV Store\n";
    cout << "  支持命令: PING, SET, GET, DEL, EXISTS, KEYS\n";
    cout << "  SET 支持 EX 参数: SET key value EX seconds\n";
  }

  // TODO 9.2: 启动服务器并测试
  {
    subsection("实践: 发送 RESP 命令");

    constexpr int PORT = 16091;

    std::thread server([]() {
      RespServer srv(PORT);
      if (!srv.init()) return;
      srv.run_for(10); // 处理 10 个命令
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto send_cmd = [](const string &cmd_str) {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      send(fd, cmd_str.c_str(), cmd_str.size(), MSG_NOSIGNAL);

      char buf[4096]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        string resp(buf, n);
        // 清理显示
        for (auto &c : resp)
          if (c == '\r' || c == '\n') c = ' ';
        cout << "    " << cmd_str.substr(0, cmd_str.find('\r'))
             << " → " << resp << "\n";
      }
    };

    // PING
    send_cmd("*1\r\n$4\r\nPING\r\n");

    // SET
    send_cmd("*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nAlice\r\n");

    // GET
    send_cmd("*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");

    // SET with EX (过期 1 秒)
    send_cmd("*5\r\n$3\r\nSET\r\n$4\r\ntemp\r\n$5\r\nhello\r\n"
             "$2\r\nEX\r\n$1\r\n1\r\n");

    // EXISTS
    send_cmd("*2\r\n$6\r\nEXISTS\r\n$4\r\nname\r\n");

    // KEYS
    send_cmd("*1\r\n$4\r\nKEYS\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // GET temp (应该已过期)
    send_cmd("*2\r\n$3\r\nGET\r\n$4\r\ntemp\r\n");

    // DEL
    send_cmd("*2\r\n$3\r\nDEL\r\n$4\r\nname\r\n");

    // GET name (应该 nil)
    send_cmd("*2\r\n$3\r\nGET\r\n$4\r\nname\r\n");

    server.join();

    cout << "\n  ✅ KV 存储服务器 (RESP 协议) 工作正常!\n";
    cout << "  💡 你可以用 redis-cli 连接这个服务器:\n";
    cout << "    redis-cli -p " << PORT << "\n";
  }
}

// --- 练习 10: 持久化 — AOF 日志 + 总结 ---
//
// AOF (Append-Only File) 是 Redis 的持久化方式之一。
// 原理: 把每个「写」命令追加到文件末尾。
// 重启时重放 AOF 文件恢复数据。
//
// 格式 (RESP): 每行一个命令
//   *3\r\n$3\r\nSET\r\n$4\r\nkey1\r\n$6\r\nvalue1\r\n
//   *3\r\n$3\r\nSET\r\n$4\r\nkey2\r\n$5\r\nvalue2\r\n

class AofLogger {
public:
  explicit AofLogger(const string &path) : _path(path) {}

  bool open() {
    _file.open(_path, std::ios::app | std::ios::binary);
    return _file.is_open();
  }

  // 记录一条 RESP 写命令
  bool append(const string &resp_cmd) {
    if (!_file.is_open())
      return false;
    _file.write(resp_cmd.data(), resp_cmd.size());
    _file.flush();
    return true;
  }

  // 重放 AOF 文件恢复数据到 KvStore
  static int replay(const string &path, KvStore &store) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
      return 0;

    string content((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
    file.close();

    int count = 0;
    size_t pos = 0;
    while (pos < content.size()) {
      auto cmd = resp_parse(content, pos);
      if (!cmd || cmd->type != RespType::ARRAY || cmd->array_val.empty())
        break;
      string op = cmd->array_val[0].str_val;
      for (auto &c : op)
        c = std::toupper(static_cast<unsigned char>(c));

      if (op == "SET" && cmd->array_val.size() >= 3) {
        store.set(cmd->array_val[1].str_val, cmd->array_val[2].str_val);
        ++count;
      } else if (op == "DEL" && cmd->array_val.size() >= 2) {
        store.del(cmd->array_val[1].str_val);
        ++count;
      }
    }
    return count;
  }

  string path() const { return _path; }

private:
  string _path;
  std::ofstream _file;
};

void exercise10_persistence_and_summary() {
  section("练习 10: AOF 持久化 + Month 3 总结");

  // TODO 10.1: AOF 持久化演示
  {
    subsection("AOF 持久化");

    const string aof_path = "/tmp/mini-redis.aof";
    // 清理旧文件
    std::remove(aof_path.c_str());

    // 写入 AOF
    {
      AofLogger aof(aof_path);
      if (aof.open()) {
        aof.append("*3\r\n$3\r\nSET\r\n$4\r\nkey1\r\n$6\r\nvalue1\r\n");
        aof.append("*3\r\n$3\r\nSET\r\n$4\r\nkey2\r\n$5\r\nhello\r\n");
        aof.append("*2\r\n$3\r\nDEL\r\n$4\r\nkey2\r\n");
        aof.append("*3\r\n$3\r\nSET\r\n$4\r\nkey3\r\n$5\r\nworld\r\n");
        cout << "  ✅ AOF 日志已写入: " << aof_path << "\n";
      }
    }

    // 重放 AOF 恢复
    {
      KvStore recovered;
      int count = AofLogger::replay(aof_path, recovered);
      cout << "  ✅ AOF 重放完成: " << count << " 条命令\n";
      cout << "  KEYS: [";
      auto keys = recovered.keys();
      for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) cout << ", ";
        cout << keys[i] << "=" << *recovered.get(keys[i]);
      }
      cout << "]\n";
    }

    // 清理
    std::remove(aof_path.c_str());
  }

  // TODO 10.2: Month 3 知识全景图
  {
    subsection("Month 3 知识全景");

    cout << "  ┌──────────────────────────────────────────────────┐\n";
    cout << "  │            Month 3: 网络编程                      │\n";
    cout << "  ├──────────────────────────────────────────────────┤\n";
    cout << "  │                                                   │\n";
    cout << "  │  W11: Socket 基础                                 │\n";
    cout << "  │    socket/bind/listen/accept/connect/send/recv    │\n";
    cout << "  │    ↓                                              │\n";
    cout << "  │  W12: TCP 深入                                    │\n";
    cout << "  │    Nagle/KeepAlive/粘包/消息边界/优雅关闭          │\n";
    cout << "  │    ↓                                              │\n";
    cout << "  │  W13: IO 多路复用                                  │\n";
    cout << "  │    select→poll→epoll, LT/ET, EPOLLONESHOT        │\n";
    cout << "  │    ↓                                              │\n";
    cout << "  │  W14: HTTP 协议                                    │\n";
    cout << "  │    请求/响应格式, URL解析, MIME, 静态文件服务      │\n";
    cout << "  │    ↓                                              │\n";
    cout << "  │  W15: 网络服务实战                                 │\n";
    cout << "  │    聊天室/代理/隧道/负载均衡                       │\n";
    cout << "  │    ↓                                              │\n";
    cout << "  │  W16: 收官 — RESP 协议 + KV 存储 + AOF           │\n";
    cout << "  │    性能测试, Mini-Redis, 持久化, 知识总结          │\n";
    cout << "  │                                                   │\n";
    cout << "  └──────────────────────────────────────────────────┘\n";
  }

  // TODO 10.3: 能力清单
  {
    subsection("你现在能做什么");

    cout << "  ✅ 用 socket() 创建 TCP/UDP 客户端和服务器\n";
    cout << "  ✅ 理解 TCP 字节流、粘包/拆包, 用长度前缀解决\n";
    cout << "  ✅ 实现 TCP 的优雅关闭 (shutdown + 4 步挥手)\n";
    cout << "  ✅ 用 epoll 处理成百上千并发连接 (LT/ET/ONESHOT)\n";
    cout << "  ✅ 解析 HTTP/1.1 请求, 构建标准响应\n";
    cout << "  ✅ 实现静态文件 HTTP Server (浏览器可访问)\n";
    cout << "  ✅ 构建多客户端聊天室 (广播 + 用户管理 + 心跳)\n";
    cout << "  ✅ 实现 HTTP 正向代理和 TCP 端口转发\n";
    cout << "  ✅ 设计二进制应用层协议 (类型+长度+数据)\n";
    cout << "  ✅ 实现 RESP 协议兼容的 KV 存储 (redis-cli 可连)\n";
    cout << "  ✅ 理解 AOF 持久化原理\n";
  }

  // TODO 10.4: 下一步
  {
    subsection("🚀 Month 4 预告: 极致性能");

    cout << "  Month 4 将深入程序与硬件的边界:\n";
    cout << "    - CPU 缓存优化 (cache line, false sharing)\n";
    cout << "    - perf 性能分析 (火焰图, 热点定位)\n";
    cout << "    - gdb 高级调试 (core dump, 多线程调试)\n";
    cout << "    - Sanitizers (ASan, TSan, UBSan)\n";
    cout << "    - 零拷贝 (sendfile, splice, mmap)\n";
    cout << "    - 内存池与对象池\n";
    cout << "    - 无锁数据结构 (lock-free queue)\n";
    cout << "    - io_uring (Linux 新一代异步 IO)\n";
    cout << "\n";
    cout << "  Month 5 预告: 源码阅读\n";
    cout << "    - STL 实现 (vector, string, unordered_map)\n";
    cout << "    - leveldb (LSM-Tree 存储引擎)\n";
    cout << "    - fmt (现代 C++ 格式化库)\n";
    cout << "    - libevent (事件驱动库)\n";
    cout << "\n";
    cout << "  Month 6 预告: 综合项目\n";
    cout << "    - 自选方向, 构建完整项目\n";
  }
}

// ============================================================
// main
// ============================================================

int main(int argc, char *argv[]) {
  cout << "Week 16: Month 3 收官 — 回顾 + 性能测试 + Mini KV 存储\n";
  cout << "================================================================\n";

  if (argc > 1) {
    string mode = argv[1];
    if (mode == "--server" || mode == "-s") {
      // 启动独立的 KV 存储服务器 (可用 redis-cli 连接)
      constexpr int PORT = 16379;
      cout << "🚀 Mini-Redis 启动在 0.0.0.0:" << PORT << "\n";
      cout << "   试试: redis-cli -p " << PORT << "\n";
      cout << "   试试: echo -e '*1\\r\\n\\$4\\r\\nPING\\r\\n' | nc localhost "
           << PORT << "\n";
      cout << "   按 Ctrl+C 停止\n";
      RespServer srv(PORT);
      if (srv.init()) {
        srv.run_for(999999);
      }
      return 0;
    }
    cout << "用法: " << argv[0] << " [--server|-s]\n";
    return 1;
  }

  // Part A: 回顾
  exercise1_review_w11();        cout << "[done ex1]" << std::endl;
  exercise2_review_w12();        cout << "[done ex2]" << std::endl;
  exercise3_review_w13();        cout << "[done ex3]" << std::endl;
  exercise4_review_w14();        cout << "[done ex4]" << std::endl;
  exercise5_review_w15();        cout << "[done ex5]" << std::endl;

  // Part B: 性能测试
  exercise6_benchmark();         cout << "[done ex6]" << std::endl;

  // Part C: Capstone — Mini Redis
  exercise7_resp_protocol();     cout << "[done ex7]" << std::endl;
  exercise8_kv_store();          cout << "[done ex8]" << std::endl;
  exercise9_resp_server();       cout << "[done ex9]" << std::endl;
  exercise10_persistence_and_summary(); cout << "[done ex10]" << std::endl;

  cout << "\n✅ Week 16 全部练习完成！\n";
  cout << "\n🎉 Month 3 (网络编程) 圆满完成!\n";
  cout << "\n📝 Month 3 知识体系:\n";
  cout << "  W11: Socket 编程基础 (TCP/UDP, 地址结构, 字节序)\n";
  cout << "  W12: TCP 深入 (Nagle/KeepAlive/粘包/消息边界/优雅关闭)\n";
  cout << "  W13: IO 多路复用 (select/poll/epoll, LT/ET, timerfd)\n";
  cout << "  W14: HTTP 协议 (请求/响应/URL/MIME/静态文件服务)\n";
  cout << "  W15: 网络服务实战 (聊天室/代理/隧道/负载均衡)\n";
  cout << "  W16: 收官 (RESP 协议/KV 存储/AOF 持久化/性能测试)\n";
  cout << "\n🔑 Month 3 核心能力:\n";
  cout << "  「我可以用 C++ 构建任何基于 TCP 的网络服务」\n";
  cout << "  Socket → TCP 深入 → epoll → 协议 → 服务 → 生产级\n";

  return 0;
}
