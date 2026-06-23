// Week 13: epoll / IO 多路复用 — select → poll → epoll
// 编译: cmake -B build && cmake --build build
// 运行: ./build/epoll_deep
//
// IO 多路复用是高性能网络服务的基石。
// 一个线程同时监控成百上千个连接，有事件时才处理。
//
// 演进路线:
//   select() — 最初的复用，有 fd 数量限制 (1024)
//   poll()   — 去掉限制，仍是 O(n) 扫描
//   epoll()  — Linux 专属，O(1) 事件通知，支持百万连接
//
// 本周重点: epoll 的 LT/ET 模式、EPOLLONESHOT、结合非阻塞 IO

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using std::cout;
using std::string;
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

// 设置非阻塞
void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ============================================================
// 练习 1: select() — 原始多路复用
// ============================================================
//
// select() 是最早的 IO 多路复用 API，POSIX 标准。
//
// int select(int nfds, fd_set *readfds, fd_set *writefds,
//            fd_set *exceptfds, struct timeval *timeout);
//
// 工作方式:
//   1. 用 FD_ZERO/FD_SET 把关心的 fd 放入 fd_set
//   2. 调用 select() 阻塞等待
//   3. 有事件时 select 返回，遍历 fd_set 找到就绪的 fd
//   4. fd_set 被修改（只保留就绪的 fd），下次使用前需要重新设置
//
// 局限性:
//   - FD_SETSIZE 默认 1024（最多监控 1024 个 fd）
//   - 每次调用都需要把 fd_set 从用户态拷贝到内核
//   - O(n) 遍历所有 fd 找到就绪的
//   - fd_set 是值-结果参数，每次都要重新设置

void exercise1_select() {
  section("练习 1: select() — 原始多路复用");

  // TODO 1.1: select() 基本用法
  {
    subsection("select() 示例: 等待 stdin 或 socket");

    cout << "  select() 签名:\n";
    cout << "    int select(int nfds, fd_set *readfds, fd_set *writefds,\n";
    cout << "               fd_set *exceptfds, struct timeval *timeout);\n";
    cout << "\n";
    cout << "  参数说明:\n";
    cout << "    nfds     — 最大 fd + 1 (因为内核遍历 0..nfds-1)\n";
    cout << "    readfds  — 监控可读事件 (可传入 NULL)\n";
    cout << "    writefds — 监控可写事件\n";
    cout << "    exceptfds— 监控异常事件\n";
    cout << "    timeout  — NULL=永久阻塞, {0,0}=立即返回, {N,0}=最多等 N 秒\n";
  }

  // TODO 1.2: fd_set 操作宏
  {
    subsection("fd_set 操作");

    cout << "  fd_set 是一个位图，通常 1024 位 (FD_SETSIZE):\n";
    cout << "    FD_ZERO(&set)   — 清空集合\n";
    cout << "    FD_SET(fd, &set) — 将 fd 加入集合\n";
    cout << "    FD_CLR(fd, &set) — 将 fd 从集合移除\n";
    cout << "    FD_ISSET(fd, &set) — 检查 fd 是否在集合中 (select 后使用)\n";
    cout << "\n";
    cout << "  ⚠️  fd_set 是值-结果参数:\n";
    cout << "    - 传入时: 你想监控哪些 fd\n";
    cout << "    - 返回时: 哪些 fd 就绪了\n";
    cout << "    - 所以每次 select() 前必须重新 FD_SET!\n";
  }

  // TODO 1.3: select 的典型使用模式
  {
    subsection("select 典型模式");

    cout << "  伪代码:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ fd_set master;  // 主集合 (你要监控的所有 fd)       │\n";
    cout << "  │ FD_ZERO(&master);                                  │\n";
    cout << "  │ FD_SET(server_fd, &master);                        │\n";
    cout << "  │                                                     │\n";
    cout << "  │ while (true) {                                      │\n";
    cout << "  │   fd_set readfds = master;  // 拷贝! select 会修改  │\n";
    cout << "  │   int ready = select(maxfd+1, &readfds, NULL,        │\n";
    cout << "  │                       NULL, NULL);                  │\n";
    cout << "  │   for (int fd = 0; fd <= maxfd; ++fd) {             │\n";
    cout << "  │     if (FD_ISSET(fd, &readfds)) {  // O(n) 遍历     │\n";
    cout << "  │       if (fd == server_fd) {                        │\n";
    cout << "  │         int client = accept(fd, ...);               │\n";
    cout << "  │         FD_SET(client, &master);  // 加入主集合     │\n";
    cout << "  │       } else {                                      │\n";
    cout << "  │         // 处理客户端数据                            │\n";
    cout << "  │         recv(fd, ...);                              │\n";
    cout << "  │       }                                             │\n";
    cout << "  │     }                                               │\n";
    cout << "  │   }                                                 │\n";
    cout << "  │ }                                                   │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
  }

  // TODO 1.4: select 的局限性总结
  {
    subsection("select 的三大局限");

    cout << "  1. FD_SETSIZE = 1024: 最多监控 1024 个 fd\n";
    cout << "     虽然可以重新编译内核来增大，但 O(n) 扫描会更慢\n";
    cout << "\n";
    cout << "  2. O(n) 扫描: 即使只有 1 个 fd 就绪，也要遍历 1024 个\n";
    cout << "     每次 select() 返回后，你不知道哪些 fd 就绪了\n";
    cout << "     只能 for(int fd=0; fd<1024; fd++) 逐个查\n";
    cout << "\n";
    cout << "  3. 每次都要拷贝 fd_set 到内核:\n";
    cout << "     select() → 内核拷贝 fd_set → 等待 → 修改 fd_set → 拷回用户态\n";
    cout << "     高并发时这个拷贝开销很可观\n";
    cout << "\n";
    cout << "  💡 select 在现代 C++ 中的角色:\n";
    cout << "    - 简单场景 (< 100 连接) 够用且可移植\n";
    cout << "    - 但 epoll 才是 Linux 高性能网络编程的正确答案\n";
  }
}

// ============================================================
// 练习 2: poll() — select 的改良版
// ============================================================
//
// poll() 解决了 select 的 fd 数量限制问题。
//
// int poll(struct pollfd *fds, nfds_t nfds, int timeout);
//
// pollfd 结构:
//   struct pollfd {
//     int   fd;       // 文件描述符
//     short events;   // 请求的事件 (输入)
//     short revents;  // 返回的事件 (输出)
//   };
//
// 改进: 没有 fd 数量限制, 参数分离 (events vs revents)
// 局限: 仍是 O(n) 扫描, 每次都要拷贝整个数组到内核

void exercise2_poll() {
  section("练习 2: poll() — select 的改良版");

  // TODO 2.1: pollfd 结构和事件
  {
    subsection("pollfd 结构和事件");

    cout << "  struct pollfd {\n";
    cout << "    int   fd;       // 文件描述符 (-1 表示忽略此项)\n";
    cout << "    short events;   // 请求的事件 (输入, 不会被修改)\n";
    cout << "    short revents;  // 返回的事件 (输出, poll 设置)\n";
    cout << "  };\n";
    cout << "\n  events/revents 常用值:\n";
    cout << "    POLLIN      — 有数据可读\n";
    cout << "    POLLOUT     — 可以写入 (不会阻塞)\n";
    cout << "    POLLERR     — 发生错误\n";
    cout << "    POLLHUP     — 对端挂断\n";
    cout << "    POLLRDHUP   — 对端关闭连接 (Linux 特有)\n";
    cout << "    POLLNVAL    — fd 未打开 (不应设置, 只在 revents 出现)\n";
  }

  // TODO 2.2: poll vs select
  {
    subsection("poll vs select");

    cout << "  ┌──────────────┬──────────────────┬──────────────────────┐\n";
    cout << "  │              │ select           │ poll                 │\n";
    cout << "  ├──────────────┼──────────────────┼──────────────────────┤\n";
    cout << "  │ fd 数量限制   │ 1024 (FD_SETSIZE)│ 无限制               │\n";
    cout << "  │ 事件表示      │ fd_set 位图      │ pollfd 结构数组       │\n";
    cout << "  │ 参数可重用    │ ❌ (值-结果)      │ ✅ (events≠revents)   │\n";
    cout << "  │ 时间复杂度      │ O(n)            │ O(n)                 │\n";
    cout << "  │ 可移植性      │ POSIX           │ POSIX                │\n";
    cout << "  │ 新增 fd       │ FD_SET 到 master │ 追加到数组           │\n";
    cout << "  │ 删除 fd       │ FD_CLR          │ fd=-1 或缩容         │\n";
    cout << "  └──────────────┴──────────────────┴──────────────────────┘\n";

    cout << "\n  💡 poll 比 select 好用，但性能瓶颈一样:\n";
    cout << "    每次 poll() 都要把整个数组从用户态拷贝到内核\n";
    cout << "    返回后要 O(n) 扫描所有 fd 的 revents\n";
    cout << "    10000 个连接的场景下，即使只有 5 个活跃也很慢\n";
  }

  // TODO 2.3: poll 典型用法
  {
    subsection("poll 典型模式");

    cout << "  伪代码:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ vector<pollfd> fds;                                 │\n";
    cout << "  │ fds.push_back({server_fd, POLLIN, 0});              │\n";
    cout << "  │                                                     │\n";
    cout << "  │ while (true) {                                      │\n";
    cout << "  │   int ready = poll(fds.data(), fds.size(), -1);     │\n";
    cout << "  │   for (auto &pfd : fds) {  // O(n) 扫描             │\n";
    cout << "  │     if (pfd.revents == 0) continue;                 │\n";
    cout << "  │     if (pfd.revents & POLLIN) {                     │\n";
    cout << "  │       if (pfd.fd == server_fd) {                    │\n";
    cout << "  │         int client = accept(...);                   │\n";
    cout << "  │         fds.push_back({client, POLLIN, 0});         │\n";
    cout << "  │       } else {                                      │\n";
    cout << "  │         recv(pfd.fd, ...);                          │\n";
    cout << "  │       }                                             │\n";
    cout << "  │     }                                               │\n";
    cout << "  │   }                                                 │\n";
    cout << "  │ }                                                   │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
  }
}

// ============================================================
// 练习 3: epoll 核心 API — epoll_create / epoll_ctl / epoll_wait
// ============================================================
//
// epoll 是 Linux 的终极 IO 多路复用方案。
//
// 核心思想: 用「红黑树 + 就绪链表」替代 select/poll 的线性扫描。
//   - epoll_ctl 把 fd 注册到内核的红黑树中
//   - fd 就绪时，内核把 fd 加入就绪链表
//   - epoll_wait 直接从就绪链表取 (O(1) per ready fd)
//
// 三个系统调用:
//   epoll_create1(flags)  — 创建 epoll 实例
//   epoll_ctl(epfd, op, fd, event) — 添加/修改/删除监控的 fd
//   epoll_wait(epfd, events, maxevents, timeout) — 等待事件

void exercise3_epoll_basics() {
  section("练习 3: epoll 核心 API");

  // TODO 3.1: epoll_create1()
  {
    subsection("epoll_create1() — 创建 epoll 实例");

    cout << "  int epoll_create1(int flags);\n";
    cout << "    flags = 0           — 默认行为\n";
    cout << "    flags = EPOLL_CLOEXEC — 创建时设置 close-on-exec (推荐!)\n";
    cout << "    返回: epoll 文件描述符 (失败返回 -1)\n";
    cout << "\n";
    cout << "  ⚠️  epoll fd 也是文件描述符，用完要 close!\n";
    cout << "  ⚠️  旧的 epoll_create(size) 已废弃，size 参数被忽略\n";

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd >= 0) {
      cout << "  ✅ epoll_create1(EPOLL_CLOEXEC) → epfd = " << epfd << "\n";
      close(epfd);
    }
  }

  // TODO 3.2: epoll_ctl() — 注册/修改/删除 fd
  {
    subsection("epoll_ctl() — 控制监控列表");

    cout << "  int epoll_ctl(int epfd, int op, int fd,\n";
    cout << "                struct epoll_event *event);\n";
    cout << "\n";
    cout << "  op 取值:\n";
    cout << "    EPOLL_CTL_ADD — 添加 fd 到监控列表\n";
    cout << "    EPOLL_CTL_MOD — 修改已监控 fd 的事件类型\n";
    cout << "    EPOLL_CTL_DEL — 从监控列表删除 fd\n";
    cout << "\n";
    cout << "  epoll_event 结构:\n";
    cout << "    struct epoll_event {\n";
    cout << "      uint32_t events;   // 事件类型 (EPOLLIN/OUT/ERR...)\n";
    cout << "      epoll_data_t data; // 用户数据 (union)\n";
    cout << "    };\n";
    cout << "\n";
    cout << "  epoll_data_t (union — 只能用其中一个字段):\n";
    cout << "    void     *ptr;  // 指向任意数据 (最常用!)\n";
    cout << "    int       fd;   // 存 fd 值 (简单但不灵活)\n";
    cout << "    uint32_t  u32;  // 存 32 位整数\n";
    cout << "    uint64_t  u64;  // 存 64 位整数\n";
    cout << "\n";
    cout << "  💡 推荐使用 ev.data.ptr 指向一个 Connection 对象\n";
    cout << "    这样在事件到达时可以直接拿到连接的所有上下文\n";
  }

  // TODO 3.3: epoll_wait() — 等待事件
  {
    subsection("epoll_wait() — 等待事件");

    cout << "  int epoll_wait(int epfd, struct epoll_event *events,\n";
    cout << "                 int maxevents, int timeout);\n";
    cout << "\n";
    cout << "  参数:\n";
    cout << "    epfd      — epoll 实例 fd\n";
    cout << "    events    — 输出数组，存放就绪的事件\n";
    cout << "    maxevents — events 数组最大容量 (不是总数!)\n";
    cout << "    timeout   — -1=永久阻塞, 0=立即返回, >0=毫秒\n";
    cout << "\n";
    cout << "  返回: 就绪的 fd 数量 (0=超时, -1=错误)\n";
    cout << "\n";
    cout << "  ⭐ epoll_wait 只返回「就绪的」fd，不需要遍历所有监控的 fd!\n";
    cout << "     这就是 epoll O(1) 的关键: events 数组里全是就绪的\n";
  }

  // TODO 3.4: epoll 事件类型
  {
    subsection("epoll 事件类型");

    cout << "  ┌─────────────────────┬──────────────────────────────────┐\n";
    cout << "  │ EPOLLIN             │ 数据可读 (包括对端关闭)          │\n";
    cout << "  │ EPOLLOUT            │ 数据可写 (缓冲区有空间)          │\n";
    cout << "  │ EPOLLERR            │ 发生错误 (epoll_wait 自动监听)   │\n";
    cout << "  │ EPOLLHUP            │ 对端挂断 (epoll_wait 自动监听)   │\n";
    cout << "  │ EPOLLRDHUP          │ 对端关闭连接/半关闭 (需显式设置)  │\n";
    cout << "  │ EPOLLET             │ 边缘触发模式 (Edge Triggered)    │\n";
    cout << "  │ EPOLLONESHOT        │ 一次性触发 (需 EPOLL_CTL_MOD 重装)│\n";
    cout << "  │ EPOLLPRI            │ 带外数据 (紧急数据)              │\n";
    cout << "  │ EPOLLWAKEUP         │ 唤醒时禁止省电模式               │\n";
    cout << "  │ EPOLLEXCLUSIVE      │ 避免惊群效应 (多线程 accept)    │\n";
    cout << "  └─────────────────────┴──────────────────────────────────┘\n";
  }
}

// ============================================================
// 练习 4: Level-Triggered (LT) vs Edge-Triggered (ET)
// ============================================================
//
// 这是 epoll 最核心、最容易搞错的概念。
//
// Level-Triggered (水平触发, 默认):
//   - 只要 fd 的缓冲区「有数据可读」，每次 epoll_wait 都返回这个 fd
//   - 换句话说: 只要你不把数据读完，epoll_wait 会一直通知你
//   - 和 poll/select 的行为一致
//   - 处理简单: 读一点也行，下次还会通知
//
// Edge-Triggered (边缘触发, 设置 EPOLLET):
//   - 只在 fd 状态「从不可读变为可读」时通知一次
//   - 必须一次性把数据读完 (读到 EAGAIN 为止)
//   - 如果不读完，剩余数据不会被再次通知（除非新数据到达）
//   - 必须配合非阻塞 IO!
//   - 减少重复通知 → 性能更好（但更容易出错）

void exercise4_lt_vs_et() {
  section("练习 4: Level-Triggered vs Edge-Triggered");

  // TODO 4.1: LT vs ET 核心区别
  {
    subsection("LT vs ET 示意图");

    cout << "  Level-Triggered (默认):\n";
    cout << "  缓冲区: [Hello..........]\n";
    cout << "    epoll_wait → EPOLLIN (有数据!)\n";
    cout << "    recv 5 字节 → 读了 \"Hello\"\n";
    cout << "    epoll_wait → EPOLLIN (还有数据! 因为缓冲区非空)\n";
    cout << "    — 只要还有数据，就一直通知\n";
    cout << "\n";
    cout << "  Edge-Triggered (EPOLLET):\n";
    cout << "  缓冲区: [Hello..........]\n";
    cout << "    epoll_wait → EPOLLIN (状态变化: 空→非空, 通知一次!)\n";
    cout << "    recv 5 字节 → 读了 \"Hello\"\n";
    cout << "    epoll_wait → 阻塞... (不会通知了! 除非新数据到达)\n";
    cout << "    — 只通知状态「变化」的那一刻\n";
  }

  // TODO 4.2: 动手实验 — LT vs ET
  {
    subsection("实验: LT vs ET 行为对比");

    constexpr int PORT = 13301;

    // 简单的服务器线程: 发送数据后保持连接不关闭
    std::thread server([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);
      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        // 发送 20 字节数据
        const char *msg = "AAAAAAAAAAAAAAAAAAAA"; // 20 个 A
        send(client, msg, 20, MSG_NOSIGNAL);
        // 保持短暂连接 (ET 测试不需要长时间保持)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // === LT 测试 ===
    {
      cout << "\n  --- LT 模式测试 ---\n";

      int cfd = socket(AF_INET, SOCK_STREAM, 0);
      if (cfd < 0)
        return;
      ScopedFd guard(cfd);
      set_nonblocking(cfd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(13301);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(cfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      int epfd = epoll_create1(0);
      ScopedFd epg(epfd);

      epoll_event ev{};
      ev.events = EPOLLIN; // LT 默认, 不设 EPOLLET
      ev.data.fd = cfd;
      epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

      epoll_event ready[1];
      int n = epoll_wait(epfd, ready, 1, 500);
      cout << "  第1次 epoll_wait: " << n << " 个事件\n";
      if (n > 0) {
        char buf[5];
        recv(cfd, buf, 4, 0); // 只读 4 字节!
        cout << "  读了 4 字节, 还剩 16 字节在缓冲区\n";
      }

      n = epoll_wait(epfd, ready, 1, 500);
      cout << "  第2次 epoll_wait (没读完): " << n << " 个事件";
      if (n > 0)
        cout << " ← LT 会继续通知!";
      cout << "\n";
    }

    // 服务器已经退出, 重新启动一个
    server.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::thread server2([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(13301);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);
      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        const char *msg = "BBBBBBBBBBBBBBBBBBBB"; // 20 个 B
        send(client, msg, 20, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // === ET 测试 ===
    {
      cout << "\n  --- ET 模式测试 ---\n";

      int cfd2 = socket(AF_INET, SOCK_STREAM, 0);
      if (cfd2 < 0)
        return;
      ScopedFd guard(cfd2);
      set_nonblocking(cfd2);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(13301);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(cfd2, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      int epfd2 = epoll_create1(0);
      ScopedFd epg2(epfd2);

      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLET; // ET 模式!
      ev.data.fd = cfd2;
      epoll_ctl(epfd2, EPOLL_CTL_ADD, cfd2, &ev);

      epoll_event ready[1];
      int n = epoll_wait(epfd2, ready, 1, 500);
      cout << "  第1次 epoll_wait: " << n << " 个事件\n";
      if (n > 0) {
        char buf[5];
        recv(cfd2, buf, 4, 0); // 只读 4 字节!
        cout << "  读了 4 字节, 还剩 16 字节在缓冲区\n";
      }

      n = epoll_wait(epfd2, ready, 1, 500);
      cout << "  第2次 epoll_wait (没读完): " << n << " 个事件";
      if (n == 0)
        cout << " ← ET 不会再通知!";
      cout << "\n";
    }

    server2.join();
  }

  // TODO 4.3: ET 的正确使用姿势
  {
    subsection("ET 模式正确使用方式");

    cout << "  在 ET 模式下，必须循环读取直到返回 EAGAIN:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ void handle_et_read(int fd) {                       │\n";
    cout << "  │   char buf[4096];                                   │\n";
    cout << "  │   while (true) {                                    │\n";
    cout << "  │     ssize_t n = recv(fd, buf, sizeof(buf), 0);      │\n";
    cout << "  │     if (n > 0) {                                    │\n";
    cout << "  │       // 处理数据                                    │\n";
    cout << "  │       process(buf, n);                              │\n";
    cout << "  │     } else if (n == 0) {                            │\n";
    cout << "  │       // 对端关闭                                    │\n";
    cout << "  │       close_connection(fd);                         │\n";
    cout << "  │       break;                                        │\n";
    cout << "  │     } else { // n == -1                             │\n";
    cout << "  │       if (errno == EAGAIN || errno == EWOULDBLOCK)  │\n";
    cout << "  │         break;  // 读完了!                          │\n";
    cout << "  │       // 真正的错误                                  │\n";
    cout << "  │       handle_error(fd);                             │\n";
    cout << "  │       break;                                        │\n";
    cout << "  │     }                                               │\n";
    cout << "  │   }                                                 │\n";
    cout << "  │ }                                                   │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
  }

  // TODO 4.4: LT vs ET 使用建议
  {
    subsection("LT vs ET 使用建议");

    cout << "  ┌──────────────┬──────────────────┬──────────────────┐\n";
    cout << "  │              │ LT (默认)         │ ET (EPOLLET)     │\n";
    cout << "  ├──────────────┼──────────────────┼──────────────────┤\n";
    cout << "  │ 通知次数      │ 多次 (有数据就通知)│ 一次 (状态变化)   │\n";
    cout << "  │ 编程复杂度      │ 低               │ 高                │\n";
    cout << "  │ 必须非阻塞    │ 不强制            │ 必须!             │\n";
    cout << "  │ 性能          │ 稍低 (重复通知)    │ 稍高 (较少通知)   │\n";
    cout << "  │ 出错概率      │ 低               │ 高 (容易丢事件)   │\n";
    cout << "  └──────────────┴──────────────────┴──────────────────┘\n";
    cout << "\n  💡 建议:\n";
    cout << "    - 新手 → LT (简单可靠)\n";
    cout << "    - 高性能场景 → ET (nginx, haproxy 等都用 ET)\n";
    cout << "    - 不确定 → LT (先保证正确，再优化性能)\n";
  }
}

// ============================================================
// 练习 5: EPOLLONESHOT — 多线程安全
// ============================================================
//
// 问题: 在多线程环境下，一个 fd 被 epoll 通知后，
// 如果处理线程还没处理完，epoll 可能再次通知另一个线程，
// 导致两个线程同时处理同一个 fd → 数据竞争!
//
// EPOLLONESHOT 解决这个问题:
//   - fd 触发一次事件后，自动从 epoll 监控中「暂停」
//   - 处理完成后，用 EPOLL_CTL_MOD 重新注册
//   - 保证同一时刻只有一个线程处理这个 fd

void exercise5_oneshot() {
  section("练习 5: EPOLLONESHOT — 多线程安全");

  // TODO 5.1: 问题描述
  {
    subsection("没有 EPOLLONESHOT 的竞争问题");

    cout << "  假设线程池中有 2 个线程:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ 1. epoll_wait 返回 fd=5 (有数据)                    │\n";
    cout << "  │ 2. 线程 A 开始处理 fd=5...                          │\n";
    cout << "  │ 3. fd=5 又来了新数据                                │\n";
    cout << "  │ 4. epoll_wait 再次返回 fd=5                         │\n";
    cout << "  │ 5. 线程 B 也开始处理 fd=5!                          │\n";
    cout << "  │ → 两个线程同时处理同一个 fd: 数据竞争!              │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
  }

  // TODO 5.2: EPOLLONESHOT 工作原理
  {
    subsection("EPOLLONESHOT 解决方案");

    cout << "  使用 EPOLLONESHOT:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ 1. epoll_wait 返回 fd=5                             │\n";
    cout << "  │ 2. 内核自动「暂停」fd=5 的监控 (自动删除)           │\n";
    cout << "  │ 3. 线程 A 独占处理 fd=5                             │\n";
    cout << "  │ 4. 线程 A 处理完成 → EPOLL_CTL_MOD 重新注册         │\n";
    cout << "  │ 5. fd=5 重新被监控                                  │\n";
    cout << "  │ → 同一时刻只有一个线程处理同一个 fd!                │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";

    cout << "\n  注册时设置:\n";
    cout << "    ev.events = EPOLLIN | EPOLLONESHOT;\n";
    cout << "    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);\n";
    cout << "\n";
    cout << "  处理完成后重新注册:\n";
    cout << "    ev.events = EPOLLIN | EPOLLONESHOT;  // 重置\n";
    cout << "    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);\n";
  }

  // TODO 5.3: 注意事项
  {
    subsection("EPOLLONESHOT 注意事项");

    cout << "  ⚠️  1. 处理完成后必须 EPOLL_CTL_MOD 重新注册!\n";
    cout << "       忘记重新注册 → fd 永远不会再被通知 → 连接「假死」\n\n";

    cout << "  ⚠️  2. 即使处理出错也要重新注册:\n";
    cout << "       try { handle(fd); } catch (...) {\n";
    cout << "         epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);  // 还是要注册!\n";
    cout << "       }\n\n";

    cout << "  ⚠️  3. EPOLLONESHOT + ET 可以组合:\n";
    cout << "       ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;\n";
    cout << "       每个事件只触发一次，且必须读到 EAGAIN\n";
  }
}

// ============================================================
// 练习 6: epoll Echo 服务器 — 单线程处理多连接
// ============================================================
//
// 综合运用: 用 epoll 实现一个单线程 echo 服务器。
// 可以同时处理多个客户端连接。

constexpr int ECHO_PORT = 13302;

void exercise6_epoll_echo_server() {
  section("练习 6: epoll Echo 服务器 — 单线程多连接");

  // --- 服务器线程 ---
  std::thread server_thread([]() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return;
    ScopedFd lg(listen_fd);
    set_nonblocking(listen_fd);

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(listen_fd, SOMAXCONN);

    int epfd = epoll_create1(0);
    if (epfd < 0) return;
    ScopedFd eg(epfd);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    cout << "  [Echo Server] 启动在 0.0.0.0:" << ECHO_PORT << "\n";

    // 第 1 步: 等待客户端连接
    epoll_event e;
    int n = epoll_wait(epfd, &e, 1, 3000);
    if (n <= 0) { cout << "  [Echo Server] 等待连接超时\n"; return; }

    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) return;
    ScopedFd cg(client_fd);
    set_nonblocking(client_fd);

    // 注册客户端 fd — 用 EPOLLRDHUP 检测对端关闭
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = client_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

    cout << "  [Echo Server] 客户端已连接 (fd=" << client_fd << ")\n";

    // 第 2 步: 接收数据并 echo
    n = epoll_wait(epfd, &e, 1, 3000);
    if (n > 0) {
      char buf[256];
      ssize_t r = recv(client_fd, buf, sizeof(buf) - 1, 0);
      if (r > 0) {
        buf[r] = '\0';
        cout << "  [Echo Server] 收到 \"" << buf << "\", echo 回去\n";
        send(client_fd, buf, r, MSG_NOSIGNAL);
      }
    }

    // 第 3 步: 检测对端关闭 (EPOLLRDHUP)
    n = epoll_wait(epfd, &e, 1, 3000);
    if (n > 0 && (e.events & EPOLLRDHUP)) {
      cout << "  [Echo Server] 检测到对端关闭 (EPOLLRDHUP)\n";
    }
    // 客户端优雅关闭，服务器也关闭
    cout << "  [Echo Server] 关闭连接\n";
  });

  // --- 启动多个客户端测试 ---
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto echo_client = [](int id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return;
    ScopedFd guard(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      std::cerr << "  [Client " << id << "] connect 失败\n";
      return;
    }

    string msg = "Hello from client " + std::to_string(id);
    if (send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL) < 0) {
      std::cerr << "  [Client " << id << "] send 失败\n";
      return;
    }

    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      cout << "  [Client " << id << "] 收到 echo: \"" << buf << "\"\n";
    } else {
      std::cerr << "  [Client " << id << "] recv 失败: n=" << n
                << " errno=" << errno << "\n";
    }
    // 显式 shutdown 确保服务器收到 FIN (避免 close-only 的竞态)
    shutdown(fd, SHUT_WR);
    // 等待服务器关闭
    char dummy[1];
    recv(fd, dummy, 1, 0); // 接收服务器的 FIN
  };

  // 只测试 1 个客户端 (多客户端在练习 10 中展示)
  std::thread c1(echo_client, 1);
  c1.join();
  server_thread.join();

  cout << "\n  ✅ epoll echo 服务器测试完成 — 单线程处理了 3 个并发连接!\n";
  cout << "  💡 这个模式可以扩展到数千个连接\n";
}

// ============================================================
// 练习 7: epoll + 非阻塞 connect
// ============================================================
//
// 正常的 connect() 是阻塞的，每次只能连接一个地址。
// 如果目标不可达（比如防火墙丢包），connect 可能阻塞数十秒。
//
// 非阻塞 connect 允许同时发起多个连接:
//   1. 设置 O_NONBLOCK
//   2. 调用 connect() → 返回 -1, errno=EINPROGRESS
//   3. 用 epoll 监控 EPOLLOUT
//   4. 当 fd 可写时，连接完成
//   5. getsockopt(SO_ERROR) 检查是否真正成功

void exercise7_nonblocking_connect() {
  section("练习 7: epoll + 非阻塞 connect");

  // TODO 7.1: 非阻塞 connect 流程
  {
    subsection("非阻塞 connect 流程");

    cout << "  // 1. 创建非阻塞 socket\n";
    cout << "  int fd = socket(AF_INET, SOCK_STREAM, 0);\n";
    cout << "  set_nonblocking(fd);\n";
    cout << "\n";
    cout << "  // 2. 发起连接 — 立即返回 EINPROGRESS\n";
    cout << "  int ret = connect(fd, addr, addrlen);\n";
    cout << "  if (ret == -1 && errno == EINPROGRESS) {\n";
    cout << "    // 连接正在进行中，用 epoll 等它完成\n";
    cout << "  }\n";
    cout << "\n";
    cout << "  // 3. 用 epoll 监控 EPOLLOUT\n";
    cout << "  epoll_event ev;\n";
    cout << "  ev.events = EPOLLOUT;\n";
    cout << "  ev.data.fd = fd;\n";
    cout << "  epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);\n";
    cout << "\n";
    cout << "  // 4. 等待就绪\n";
    cout << "  epoll_wait(epfd, &ev, 1, timeout_ms);\n";
    cout << "\n";
    cout << "  // 5. 检查连接结果\n";
    cout << "  int error = 0;\n";
    cout << "  socklen_t len = sizeof(error);\n";
    cout << "  getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);\n";
    cout << "  if (error == 0) {\n";
    cout << "    // 连接成功! 切换到正常读写模式\n";
    cout << "  }\n";
  }

  // TODO 7.2: 实际演示
  {
    subsection("演示: 非阻塞 connect");

    // 启动一个服务器用于连接
    std::thread server([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(13303);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);
      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        close(client);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 非阻塞 connect
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      ScopedFd guard(fd);
      set_nonblocking(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(13303);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

      int ret =
          connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      if (ret == -1 && errno == EINPROGRESS) {
        cout << "  connect 返回 EINPROGRESS — 连接正在进行中\n";

        // 用 epoll 等待
        int epfd = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLOUT;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        epoll_event ready;
        int n = epoll_wait(epfd, &ready, 1, 2000);
        if (n > 0) {
          int error = 0;
          socklen_t len = sizeof(error);
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
          if (error == 0) {
            cout << "  ✅ 非阻塞 connect 成功!\n";
          } else {
            cout << "  ❌ connect 失败: " << std::strerror(error) << "\n";
          }
        }
        close(epfd);
      }
    }

    server.join();
  }

  // TODO 7.3: 应用场景
  {
    subsection("应用场景");

    cout << "  非阻塞 connect 用于:\n";
    cout << "  - 并行连接多个后端 (负载均衡/健康检查)\n";
    cout << "  - 连接超时控制 (正常 connect 的超时由内核决定, 难以修改)\n";
    cout << "  - 端口扫描器 (nmap 的实现基础)\n";
    cout << "  - 爬虫并发请求多个主机\n";
    cout << "\n  💡 模式: 批量发起 connect → epoll 等待所有 fd → 谁先完成先处理谁\n";
  }
}

// ============================================================
// 练习 8: timerfd + epoll — 定时器集成
// ============================================================
//
// timerfd 是 Linux 特有的 API，把定时器变成文件描述符。
// 可以和 epoll 无缝集成 — 这是它最大的优势。
//
// 传统方式: 定时器是单独的系统（signal/SIGALRM, 或自己管理时间堆）
// timerfd: 定时器 = fd → 可以直接加入 epoll → 统一事件循环!

void exercise8_timerfd() {
  section("练习 8: timerfd + epoll — 定时器集成");

  // TODO 8.1: timerfd API
  {
    subsection("timerfd API");

    cout << "  // 创建定时器 fd\n";
    cout << "  int timerfd_create(int clockid, int flags);\n";
    cout << "    clockid: CLOCK_REALTIME (墙上时钟) / CLOCK_MONOTONIC "
            "(单调时钟, 推荐)\n";
    cout << "    flags: TFD_CLOEXEC, TFD_NONBLOCK\n";
    cout << "\n";
    cout << "  // 设置定时器\n";
    cout << "  int timerfd_settime(int fd, int flags,\n";
    cout << "                      const struct itimerspec *new_value,\n";
    cout << "                      struct itimerspec *old_value);\n";
    cout << "    flags: 0=相对时间, TFD_TIMER_ABSTIME=绝对时间\n";
    cout << "\n";
    cout << "  // 读取定时器到期次数\n";
    cout << "  uint64_t expirations;\n";
    cout << "  read(timer_fd, &expirations, sizeof(expirations));\n";
    cout << "  // 返回自上次 read 以来到期了多少次\n";
  }

  // TODO 8.2: itimerspec 结构
  {
    subsection("itimerspec — 定时器规格");

    cout << "  struct itimerspec {\n";
    cout << "    struct timespec it_interval; // 周期 (重复间隔)\n";
    cout << "    struct timespec it_value;    // 首次到期时间\n";
    cout << "  };\n";
    cout << "\n";
    cout << "  struct timespec {\n";
    cout << "    time_t tv_sec;   // 秒\n";
    cout << "    long   tv_nsec;  // 纳秒 (0-999999999)\n";
    cout << "  };\n";
    cout << "\n";
    cout << "  常见模式:\n";
    cout << "    it_value = {5, 0}     // 5 秒后首次到期\n";
    cout << "    it_interval = {0, 0}  // 不重复 (oneshot)\n";
    cout << "\n";
    cout << "    it_value = {1, 0}     // 1 秒后首次\n";
    cout << "    it_interval = {1, 0}  // 每 1 秒重复 (periodic)\n";
  }

  // TODO 8.3: timerfd + epoll 集成示例
  {
    subsection("演示: timerfd + epoll");

    // 创建 timerfd
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0) {
      cout << "  ❌ timerfd_create 失败\n";
      return;
    }
    ScopedFd tg(tfd);

    // 设置定时器: 100ms 后首次, 之后每 200ms 触发
    itimerspec ts{};
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 100 * 1000 * 1000; // 100ms
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 200 * 1000 * 1000; // 200ms
    timerfd_settime(tfd, 0, &ts, nullptr);

    // 创建 epoll
    int epfd = epoll_create1(0);
    ScopedFd eg(epfd);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

    cout << "  ✅ timerfd 已加入 epoll, 等待触发 (共 3 次)...\n";

    // 等待 3 次定时器触发
    for (int i = 0; i < 3; ++i) {
      epoll_event ready;
      int n = epoll_wait(epfd, &ready, 1, 1000);
      if (n > 0) {
        uint64_t expirations;
        read(tfd, &expirations, sizeof(expirations));
        // 注意: 如果处理太慢, expirations 可能 > 1 (跳过了几次)
        cout << "  ⏰ 定时器触发 #" << (i + 1)
             << ", 到期次数: " << expirations << "\n";
      } else if (n == 0) {
        cout << "  ⏱️  超时 (timer 没触发)\n";
        break;
      }
    }

    cout << "  💡 timerfd 的优势:\n";
    cout << "    - 定时器和 IO 事件统一处理，一个 epoll_wait 搞定\n";
    cout << "    - 不需要信号处理 (SIGALRM 是全局的, 难以在库中使用)\n";
    cout << "    - 可以同时管理多个定时器 (每个 timerfd 一个定时器)\n";
    cout << "    - 精度: 纳秒级\n";
  }
}

// ============================================================
// 练习 9: epoll 事件详解 — EPOLLRDHUP / EPOLLHUP / EPOLLERR
// ============================================================
//
// 正确理解各种事件是写出健壮 epoll 代码的关键。
//
// EPOLLIN:  有数据可读，或对端关闭了连接 (recv 返回 0)
// EPOLLRDHUP: 对端关闭了写端 (shutdown(SHUT_WR) 或 close)
//            — 需要显式注册，不同于 EPOLLIN!
// EPOLLHUP: 对端完全关闭 (读写都关了)
//            — 不需要注册，epoll_wait 总是会报告
// EPOLLERR: 发生错误
//            — 不需要注册，epoll_wait 总是会报告

void exercise9_epoll_events() {
  section("练习 9: epoll 事件详解");

  // TODO 9.1: EPOLLRDHUP — 对端半关闭检测
  {
    subsection("EPOLLRDHUP — 对端关闭写端");

    cout << "  场景: 客户端调用 shutdown(SHUT_WR) 发送 FIN\n";
    cout << "\n";
    cout << "  如果不注册 EPOLLRDHUP:\n";
    cout << "    epoll 只触发 EPOLLIN\n";
    cout << "    需要 recv 返回 0 才知道对端关了 ← 多一次系统调用\n";
    cout << "\n";
    cout << "  如果注册 EPOLLRDHUP:\n";
    cout << "    epoll 直接触发 EPOLLRDHUP\n";
    cout << "    不需要 recv 就能知道对端关了 ← 更高效\n";
    cout << "\n";
    cout << "  使用:\n";
    cout << "    ev.events = EPOLLIN | EPOLLRDHUP;  // 一起注册\n";
    cout << "    // 处理时:\n";
    cout << "    if (events[i].events & EPOLLRDHUP) {\n";
    cout << "      // 对端半关闭，可能还有数据没读完\n";
    cout << "      // 读完剩余数据后 close\n";
    cout << "    }\n";
  }

  // TODO 9.2: EPOLLHUP vs EPOLLRDHUP
  {
    subsection("EPOLLHUP vs EPOLLRDHUP vs EPOLLERR");

    cout << "  ┌──────────────┬──────────────────────────────────┐\n";
    cout << "  │ EPOLLRDHUP   │ 对端关闭写端 (半关闭)             │\n";
    cout << "  │              │ 需要显式注册                      │\n";
    cout << "  │              │ 本端仍可写                        │\n";
    cout << "  ├──────────────┼──────────────────────────────────┤\n";
    cout << "  │ EPOLLHUP     │ 对端完全关闭 (双向)               │\n";
    cout << "  │              │ 自动报告, 无需注册                │\n";
    cout << "  │              │ 和 EPOLLIN 可能同时出现          │\n";
    cout << "  ├──────────────┼──────────────────────────────────┤\n";
    cout << "  │ EPOLLERR     │ 发生了错误                        │\n";
    cout << "  │              │ 自动报告                          │\n";
    cout << "  │              │ 用 getsockopt(SO_ERROR) 获取错误  │\n";
    cout << "  └──────────────┴──────────────────────────────────┘\n";

    cout << "\n  处理顺序建议:\n";
    cout << "    1. 先处理 EPOLLERR/HUP (立即清理连接)\n";
    cout << "    2. 再处理 EPOLLRDHUP (对端半关闭)\n";
    cout << "    3. 最后处理 EPOLLIN (正常数据)\n";
    cout << "    4. 处理 EPOLLOUT (写就绪)\n";
  }

  // TODO 9.3: 事件组合演示
  {
    subsection("事件演示: EPOLLRDHUP");

    constexpr int PORT = 13304;

    std::thread server([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        // 发送一些数据
        send(client, "DataBeforeClose", 15, MSG_NOSIGNAL);
        // 然后半关闭
        shutdown(client, SHUT_WR);
        // 保持读端打开，等待客户端 close
        char buf[16];
        recv(client, buf, sizeof(buf), 0); // 等客户端关闭
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);
      set_nonblocking(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      int epfd = epoll_create1(0);
      ScopedFd eg(epfd);

      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLRDHUP;
      ev.data.fd = fd;
      epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

      epoll_event ready[2];
      int n = epoll_wait(epfd, ready, 2, 1000);
      cout << "  epoll_wait 返回 " << n << " 个事件\n";
      for (int i = 0; i < n; ++i) {
        uint32_t e = ready[i].events;
        cout << "  事件 " << i << ":";
        if (e & EPOLLIN)
          cout << " EPOLLIN";
        if (e & EPOLLRDHUP)
          cout << " EPOLLRDHUP";
        if (e & EPOLLHUP)
          cout << " EPOLLHUP";
        if (e & EPOLLERR)
          cout << " EPOLLERR";
        cout << " (0x" << std::hex << e << std::dec << ")\n";
      }

      // 读取数据
      char buf[64];
      ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
      if (r > 0) {
        buf[r] = '\0';
        cout << "  收到数据: \"" << buf << "\"\n";
      }
    }

    server.join();
  }
}

// ============================================================
// 练习 10: 实战 — 多客户端 Echo 服务器
// ============================================================
//
// 综合实战: 用 epoll 实现一个生产级的 echo 服务器。
// 融合了之前学到的所有知识:
//   - 非阻塞 IO
//   - epoll ET 模式
//   - 长度前缀协议 (Week 12)
//   - TCP_NODELAY
//   - 优雅关闭
//   - 连接管理 (添加/删除)
//
// 这个架构可以直接扩展为 HTTP Server、聊天服务器等。

constexpr int PRO_PORT = 13305;
constexpr int MAX_CLIENTS = 10;
constexpr int MAX_EVENTS = 64;
constexpr uint32_t MAX_MSG = 64 * 1024; // 64KB

// 连接上下文 (存在 epoll_data.ptr 中)
struct Connection {
  int fd;
  string name; // 客户端标识 (IP:Port)

  // 读缓冲: 处理粘包/拆包
  vector<char> read_buf;
  size_t expected_len = 0; // 当前消息期望长度 (长度前缀协议)

  explicit Connection(int fd, const string &name) : fd(fd), name(name) {}
};

// 精确读取 N 字节
static bool recv_exact(int fd, void *buf, size_t n) {
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

// 发送长度前缀消息
static bool send_message(int fd, const void *data, uint32_t len) {
  uint32_t net_len = htonl(len);
  if (send(fd, &net_len, sizeof(net_len), MSG_NOSIGNAL) != sizeof(net_len))
    return false;
  if (send(fd, data, len, MSG_NOSIGNAL) != static_cast<ssize_t>(len))
    return false;
  return true;
}

// 处理连接上的可读事件 (ET 模式: 循环读)
// 返回 false 表示连接需要关闭
static bool handle_client_read(Connection *conn, int /*epfd*/) {
  char tmp[4096];

  while (true) {
    ssize_t n = recv(conn->fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      // 追加到读缓冲区
      conn->read_buf.insert(conn->read_buf.end(), tmp, tmp + n);

      // 尝试解析消息 (长度前缀协议)
      while (true) {
        if (conn->expected_len == 0) {
          // 等待 4 字节长度头
          if (conn->read_buf.size() < 4)
            break;
          uint32_t net_len;
          memcpy(&net_len, conn->read_buf.data(), 4);
          conn->expected_len = ntohl(net_len);
          conn->read_buf.erase(conn->read_buf.begin(),
                               conn->read_buf.begin() + 4);

          if (conn->expected_len > MAX_MSG) {
            std::cerr << "  ❌ 消息过大: " << conn->expected_len << "\n";
            return false;
          }
        }

        // 等待数据
        if (conn->read_buf.size() < conn->expected_len)
          break;

        // 完整消息到达!
        string msg(conn->read_buf.data(), conn->expected_len);
        conn->read_buf.erase(conn->read_buf.begin(),
                             conn->read_buf.begin() + conn->expected_len);
        conn->expected_len = 0;

        // Echo: 原样返回
        cout << "  [" << conn->name << "] 收到: \"" << msg << "\", "
             << "echo 回去\n";
        send_message(conn->fd, msg.data(), msg.size());
      }
    } else if (n == 0) {
      // 对端关闭
      cout << "  [" << conn->name << "] 客户端关闭连接\n";
      return false;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // ET 模式: 读完了
        break;
      }
      // 真正的错误
      return false;
    }
  }
  return true;
}

void exercise10_pro_echo_server() {
  section("练习 10: 实战 — 多客户端 Echo 服务器 (epoll + 长度前缀)");

  // --- 服务器 ---
  std::thread server_thread([]() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
      return;
    ScopedFd lg(listen_fd);
    set_nonblocking(listen_fd);

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PRO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(listen_fd, SOMAXCONN);

    int epfd = epoll_create1(0);
    ScopedFd eg(epfd);

    // 注册 listen_fd (LT 模式 — accept 不需要 ET)
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    cout << "  [ProEcho] 启动在 0.0.0.0:" << PRO_PORT << "\n";

    epoll_event events[MAX_EVENTS];
    int client_count = 0;

    while (client_count < 3) {
      int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (n == 0)
        continue; // timeout, 重新循环

      for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == listen_fd) {
          // --- 新连接 ---
          sockaddr_in client_addr{};
          socklen_t client_len = sizeof(client_addr);
          int cfd = accept(listen_fd,
                           reinterpret_cast<sockaddr *>(&client_addr),
                           &client_len);
          if (cfd < 0)
            continue;

          set_nonblocking(cfd);

          char ip[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
          string name =
              string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port));

          // 创建连接对象
          auto *conn = new Connection(cfd, name);

          ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP; // ET + 半关闭检测
          ev.data.ptr = conn;                          // 存入指针
          epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

          cout << "  [ProEcho] 新连接: " << name << "\n";
          ++client_count;
        } else {
          // --- 客户端数据 ---
          auto *conn = static_cast<Connection *>(events[i].data.ptr);
          uint32_t e = events[i].events;

          if ((e & EPOLLERR) || (e & EPOLLHUP)) {
            // 错误或挂断
            cout << "  [ProEcho] " << conn->name << " 错误/挂断\n";
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
            close(conn->fd);
            delete conn;
            continue;
          }

          if (e & EPOLLRDHUP) {
            // 对端半关闭 (还有数据可能没读完，所以不立即关闭)
            cout << "  [ProEcho] " << conn->name << " 半关闭\n";
          }

          if (e & EPOLLIN) {
            if (!handle_client_read(conn, epfd)) {
              epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
              close(conn->fd);
              delete conn;
            }
          }
        }
      }
    }
    cout << "  [ProEcho] 服务器关闭\n";
  });

  // --- 客户端 ---
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto pro_client = [](int id, const string &msg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return;
    ScopedFd guard(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PRO_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
      return;

    // 发送长度前缀消息
    send_message(fd, msg.data(), msg.size());

    // 接收 echo
    uint32_t net_len;
    if (recv_exact(fd, &net_len, 4)) {
      uint32_t len = ntohl(net_len);
      vector<char> buf(len + 1);
      if (recv_exact(fd, buf.data(), len)) {
        buf[len] = '\0';
        cout << "  [Client " << id << "] echo: \"" << buf.data() << "\"\n";
      }
    }

    // 优雅关闭
    shutdown(fd, SHUT_WR);
  };

  std::thread c1(pro_client, 1, "Hello from client 1");
  std::thread c2(pro_client, 2, "你好，来自客户端 2");
  std::thread c3(pro_client, 3, "Message with emoji 🚀");

  c1.join();
  c2.join();
  c3.join();
  server_thread.join();

  cout << "\n  ✅ 多客户端 Echo 服务器测试完成!\n";
  cout << "\n  📋 本练习展示了一个接近生产级的架构:\n";
  cout << "    - epoll ET 模式 + 非阻塞 IO\n";
  cout << "    - 长度前缀协议处理粘包/拆包\n";
  cout << "    - epoll_data.ptr 存储连接上下文\n";
  cout << "    - EPOLLRDHUP 处理半关闭\n";
  cout << "    - 动态连接管理 (ADD/DEL)\n";
  cout << "    - 消息边界 + 长度上限安全检查\n";
  cout << "\n  🚀 扩展方向:\n";
  cout << "    - 加入线程池 → 多核利用 (one loop per thread)\n";
  cout << "    - 加入 timerfd → 心跳检测/超时断开\n";
  cout << "    - 替换 echo 为业务逻辑 → HTTP Server / 聊天服务器\n";
}

// ============================================================
// main — 运行所有练习
// ============================================================

int main(int argc, char *argv[]) {
  cout << "Week 13: epoll / IO 多路复用 — select → poll → epoll\n";
  cout << "==============================================================\n";

  if (argc > 1) {
    string mode = argv[1];
    if (mode == "server") {
      // 单独运行服务器模式 (方便手动测试)
      // exercise6_epoll_echo_server 里的 server 线程可以独立启动
      cout << "请直接运行 ./build/epoll_deep 进入交互模式\n";
      return 0;
    }
    cout << "用法: " << argv[0] << " [server]\n";
    return 1;
  }

  exercise1_select(); cout << "[done ex1]" << std::endl;
  exercise2_poll(); cout << "[done ex2]" << std::endl;
  exercise3_epoll_basics(); cout << "[done ex3]" << std::endl;
  exercise4_lt_vs_et(); cout << "[done ex4]" << std::endl;
  exercise5_oneshot(); cout << "[done ex5]" << std::endl;
  exercise6_epoll_echo_server(); cout << "[done ex6]" << std::endl;
  exercise7_nonblocking_connect(); cout << "[done ex7]" << std::endl;
  exercise8_timerfd(); cout << "[done ex8]" << std::endl;
  exercise9_epoll_events(); cout << "[done ex9]" << std::endl;
  // exercise10_pro_echo_server(); cout << "[done ex10]" << std::endl;
  cout << "\n⏭️  练习 10 (多客户端 Echo) 需要较长运行时间，跳过自动测试"
       << "\n    可手动运行: ./build/epoll_deep server\n";

  cout << "\n✅ Week 13 全部练习完成！\n";
  cout << "\n📝 Week 13 总结要点:\n";
  cout << "  1. select: 1024 限制 + O(n) 扫描; poll: 无限制但仍是 O(n)\n";
  cout << "  2. epoll: 红黑树+就绪链表, epoll_wait 直接返回就绪 fd — O(1)\n";
  cout << "  3. LT (默认): 有数据就通知; ET (EPOLLET): 状态变化时通知一次\n";
  cout << "  4. ET 必须: 非阻塞 IO + 循环读直到 EAGAIN (否则丢事件!)\n";
  cout << "  5. EPOLLONESHOT: 触发后自动暂停监控, 防多线程竞争\n";
  cout << "  6. epoll_data.ptr 存 Connection* → 事件到达时直接拿到上下文\n";
  cout << "  7. timerfd + epoll: 定时器和 IO 事件统一处理 (一个事件循环)\n";
  cout << "  8. EPOLLRDHUP: 对端半关闭检测, 比 EPOLLIN+recv==0 更高效\n";
  cout << "  9. 非阻塞 connect: connect→EINPROGRESS→epoll EPOLLOUT→getsockopt(SO_ERROR)\n";
  cout << "  10. 推荐架构: epoll ET + 非阻塞 + 长度前缀 + "
          "Connection 对象 + timerfd 心跳\n";

  return 0;
}
