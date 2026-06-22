// Week 11: Socket 编程基础 — socket/bind/listen/accept/connect
// 编译: cmake -B build && cmake --build build
// 运行: ./build/socket_basics
//
// Socket 是网络编程的基石。它封装了操作系统提供的
// 网络通信端点，让进程可以通过网络发送和接收数据。
//
// 核心系统调用流程:
//   服务端: socket() → bind() → listen() → accept() → recv()/send() → close()
//   客户端: socket() → connect() → send()/recv() → close()
//
// 本文件包含可运行的示例，每个练习都是自包含的。
// Socket API 函数在出错时返回 -1 并设置 errno。

#include <arpa/inet.h>   // inet_pton, inet_ntop, htons, htonl
#include <cerrno>        // errno
#include <cstring>       // memset, strerror, memcpy
#include <fcntl.h>       // fcntl
#include <iostream>
#include <netdb.h>       // getaddrinfo, freeaddrinfo, gai_strerror
#include <netinet/in.h>  // sockaddr_in, sockaddr_in6, IPPROTO_TCP
#include <netinet/tcp.h> // TCP_NODELAY
#include <string>
#include <sys/socket.h>  // socket, bind, listen, accept, connect, send, recv
#include <sys/types.h>
#include <thread>
#include <unistd.h>      // close, read, write, sleep

using std::cout;
using std::string;

// ============================================================
// 辅助工具
// ============================================================

// 辅助: 打印分隔线
void section(const string &title) {
  cout << "\n=== " << title << " ===\n";
}

// 辅助: 检查系统调用结果，出错时打印并退出
// 实际项目中应抛异常或返回 std::expected (C++23)
#define CHECK(expr, msg)                                                       \
  do {                                                                         \
    if ((expr) == -1) {                                                        \
      std::cerr << "❌ " << msg << ": " << std::strerror(errno) << "\n";       \
      return;                                                                  \
    }                                                                          \
  } while (0)

// RAII 封装: 自动关闭 socket (文件描述符)
// 这是 C++ 的核心思想: 用析构管理资源
class ScopedFd {
  int _fd;

public:
  explicit ScopedFd(int fd = -1) : _fd(fd) {}
  ~ScopedFd() {
    if (_fd >= 0) close(_fd);
  }
  // 禁止拷贝（文件描述符是独占资源）
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  // 允许移动
  ScopedFd(ScopedFd &&other) noexcept : _fd(other._fd) { other._fd = -1; }
  ScopedFd &operator=(ScopedFd &&other) noexcept {
    if (this != &other) {
      if (_fd >= 0) close(_fd);
      _fd       = other._fd;
      other._fd = -1;
    }
    return *this;
  }
  int  get() const { return _fd; }
  bool valid() const { return _fd >= 0; }
  // 允许直接传给 C API（需要时）
  int  release() {
    int fd = _fd;
    _fd    = -1;
    return fd;
  }
};

// ============================================================
// 练习 1: 创建 Socket — socket() 系统调用
// ============================================================
//
// socket(domain, type, protocol) 创建一个通信端点。
//
// domain (地址族):
//   AF_INET  — IPv4 (最常用)
//   AF_INET6 — IPv6
//   AF_UNIX  — 本地 Unix domain socket
//
// type (套接字类型):
//   SOCK_STREAM  — TCP (可靠、面向连接、字节流)
//   SOCK_DGRAM   — UDP (不可靠、无连接、数据报)
//   SOCK_RAW     — 原始套接字 (需要 root)
//
// protocol: 通常设为 0，系统根据 type 自动选择。

void exercise1_create_socket() {
  section("练习 1: socket() — 创建套接字");

  // TODO 1.1: 创建一个 TCP socket (IPv4)
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
      cout << "  ❌ 创建 socket 失败: " << std::strerror(errno) << "\n";
      return;
    }
    cout << "  ✅ 创建了 TCP socket, fd = " << fd << "\n";

    // 用完一定要关闭！文件描述符是有限资源。
    close(fd);
    cout << "  ✅ 已关闭\n";
  }

  // TODO 1.2: 创建一个 UDP socket
  {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd, "创建 UDP socket");
    cout << "  ✅ 创建了 UDP socket, fd = " << fd << "\n";
    close(fd);
  }

  // TODO 1.3: socket() 返回的 fd 是什么？
  {
    cout << "\n  💡 socket fd 是什么？\n";
    cout << "    - socket() 返回一个文件描述符 (file descriptor)\n";
    cout << "    - 和 open() 返回的 fd 是同一类东西\n";
    cout << "    - 你可以对 socket fd 使用 read()/write()（虽然不推荐）\n";
    cout << "    - 也可以使用 fcntl() 设置非阻塞模式\n";
    cout << "    - \"一切皆文件\" — Unix 哲学\n";
  }

  // TODO 1.4: 地址族 vs 协议族
  {
    cout << "\n  常用地址族:\n";
    cout << "  ┌──────────┬──────────────────────────────────┐\n";
    cout << "  │ AF_INET  │ IPv4, 地址格式: 192.168.1.1     │\n";
    cout << "  │ AF_INET6 │ IPv6, 地址格式: ::1              │\n";
    cout << "  │ AF_UNIX  │ 本地文件系统路径 (高性能 IPC)    │\n";
    cout << "  │ AF_PACKET│ 链路层 (原始帧, 需要 root)       │\n";
    cout << "  └──────────┴──────────────────────────────────┘\n";
  }
}

// ============================================================
// 练习 2: 地址结构 — sockaddr_in, inet_pton, 字节序
// ============================================================
//
// IPv4 地址用 sockaddr_in 表示。
// 关键字段:
//   sin_family — AF_INET
//   sin_port   — 端口号 (网络字节序! 用 htons)
//   sin_addr   — IP 地址 (网络字节序, in_addr 结构)
//
// 字节序 (Endianness):
//   网络字节序 = 大端 (Big Endian)
//   x86/x64     = 小端 (Little Endian)
//   → 必须用 htons/htonl/ntohs/ntohl 转换！

void exercise2_address_structures() {
  section("练习 2: 地址结构与字节序");

  // TODO 2.1: 构建 sockaddr_in
  {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(8080); // 主机字节序 → 网络字节序

    // inet_pton: "presentation" (字符串) → "network" (二进制)
    // 比旧的 inet_addr() 更安全，支持 IPv6
    int ret = inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (ret == 1) {
      cout << "  ✅ inet_pton 成功: 127.0.0.1 → 网络字节序\n";
    } else if (ret == 0) {
      cout << "  ❌ 无效的地址字符串\n";
      return;
    } else {
      cout << "  ❌ inet_pton 失败: " << std::strerror(errno) << "\n";
      return;
    }
  }

  // TODO 2.2: inet_ntop — 二进制转字符串
  {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(8080);
    inet_pton(AF_INET, "192.168.1.100", &addr.sin_addr);

    char buf[INET_ADDRSTRLEN]; // IPv4 地址最长 15 字符 + null
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    cout << "  ✅ inet_ntop: 二进制 → \"" << buf << "\"\n";
  }

  // TODO 2.3: 字节序转换
  {
    uint16_t host_port = 8080;
    uint16_t net_port  = htons(host_port);
    uint16_t back      = ntohs(net_port);

    cout << "\n  字节序演示:\n";
    cout << "    主机字节序: " << host_port << "\n";
    cout << "    网络字节序: " << net_port << " (大端, 在 x86 上不同)\n";
    cout << "    转回来:     " << back << "\n";

    cout << "\n  字节序函数速记:\n";
    cout << "    htons() — Host TO Network Short (16-bit, 用于端口)\n";
    cout << "    htonl() — Host TO Network Long  (32-bit, 用于 IPv4 地址)\n";
    cout << "    ntohs() — Network TO Host Short\n";
    cout << "    ntohl() — Network TO Host Long\n";
  }

  // TODO 2.4: INADDR_ANY — 监听所有接口
  {
    cout << "\n  特殊地址:\n";
    cout << "    INADDR_ANY     (0.0.0.0) — 监听本机所有网络接口\n";
    cout << "    INADDR_LOOPBACK (127.0.0.1) — 只监听本地回环\n";
    cout << "    IN6ADDR_ANY_INIT            — IPv6 版本\n";
    cout << "    服务器通常用 INADDR_ANY，客户端用具体 IP 或域名\n";
  }
}

// ============================================================
// 练习 3: TCP 服务器 — bind / listen / accept
// ============================================================
//
// 服务器端流程:
//   1. socket()  — 创建套接字
//   2. bind()    — 绑定到地址+端口
//   3. listen()  — 开始监听（转为被动套接字）
//   4. accept()  — 接受连接（阻塞直到有客户端连接）
//   5. recv()/send() — 收发数据
//   6. close()   — 关闭连接
//
// bind() 把 socket 和具体的 IP:port 关联起来。
// listen() 标记该 socket 为被动模式，设置连接队列大小。
// accept() 从队列中取出一个已完成的连接，返回新的 socket fd。

void exercise3_tcp_server() {
  section("练习 3: TCP 服务器 — bind / listen / accept");

  // TODO 3.1: 完整的 TCP 服务器
  {
    // 1. 创建 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd, "socket");
    ScopedFd guard(listen_fd); // RAII: 保证函数退出时关闭

    // 2. 设置 SO_REUSEADDR — 允许重启后立即绑定同一端口
    //    没有这个选项的话，服务器关闭后端口会有一段 TIME_WAIT 期
    int optval = 1;
    CHECK(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)),
          "setsockopt");

    // 3. bind — 绑定到 0.0.0.0:12345
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(12345);
    addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0 — 监听所有接口
    CHECK(bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
          "bind");

    cout << "  ✅ 已绑定到 0.0.0.0:12345\n";

    // 4. listen — 转为被动套接字
    //    backlog = 5: 内核中等待 accept 的连接队列最大长度
    CHECK(listen(listen_fd, 5), "listen");
    cout << "  ✅ 开始监听 (backlog=5)\n";

    // 5. accept — 等待客户端连接
    //    accept 是阻塞的: 没有客户端连接时会一直等待
    //    返回的 client_fd 是新的 socket，用于和这个客户端通信
    cout << "  ⏳ 等待连接... (在另一个终端运行 ./build/socket_basics client 来连接)\n";

    sockaddr_in client_addr{};
    socklen_t   client_addr_len = sizeof(client_addr);

    int client_fd =
        accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_addr_len);
    CHECK(client_fd, "accept");
    ScopedFd client_guard(client_fd);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    cout << "  ✅ 接受连接! 客户端: " << client_ip << ":"
         << ntohs(client_addr.sin_port) << "\n";

    // 6. 接收数据
    char buf[1024];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      cout << "  📩 收到 " << n << " 字节: \"" << buf << "\"\n";

      // 7. 发送响应
      const char *response = "你好，来自服务器的问候！\n";
      send(client_fd, response, strlen(response), 0);
      cout << "  📤 已发送响应\n";
    }

    // 连接关闭 (ScopedFd 自动处理)
  }

  // TODO 3.2: accept 返回的 client_fd vs listen_fd
  {
    cout << "\n  💡 listen_fd vs client_fd:\n";
    cout << "  ┌────────────┬──────────────────────────────────┐\n";
    cout << "  │ listen_fd  │ 只用于 accept()，不传数据        │\n";
    cout << "  │ client_fd  │ accept() 返回，用于和客户端通信   │\n";
    cout << "  │            │ 每个客户端有一个独立的 client_fd  │\n";
    cout << "  └────────────┴──────────────────────────────────┘\n";
    cout << "  listen_fd 在整个服务器生命周期保持不变\n";
    cout << "  每个新连接都会产生一个新的 client_fd\n";
  }

  // TODO 3.3: backlog 的含义
  {
    cout << "\n  💡 listen() 的 backlog:\n";
    cout << "    listen(fd, backlog) 设置「已完成连接」队列大小\n";
    cout << "    内核维护两个队列:\n";
    cout << "      1. 未完成队列 (SYN_RCVD 状态)\n";
    cout << "      2. 已完成队列 (ESTABLISHED 状态, 等 accept 取走)\n";
    cout << "    backlog 影响的是队列 2 的大小\n";
    cout << "    当队列满时，新的连接请求会被丢弃\n";
  }
}

// ============================================================
// 练习 4: TCP 客户端 — connect
// ============================================================
//
// 客户端流程:
//   1. socket()  — 创建套接字
//   2. connect() — 连接到服务器 (三次握手)
//   3. send()/recv() — 收发数据
//   4. close()  — 关闭连接
//
// connect() 会触发 TCP 三次握手:
//   Client → Server: SYN
//   Server → Client: SYN+ACK
//   Client → Server: ACK

void exercise4_tcp_client() {
  section("练习 4: TCP 客户端 — connect");

  // 首先启动一个简单的服务器线程，这样客户端有地方可连
  std::thread server_thread([]() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(12346);
    addr.sin_addr.s_addr = INADDR_ANY;
    (void)bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    (void)listen(fd, 1);

    sockaddr_in client_addr{};
    socklen_t   client_len = sizeof(client_addr);
    int         client_fd  = accept(fd, reinterpret_cast<sockaddr *>(&client_addr),
                                    &client_len);
    if (client_fd >= 0) {
      // 接收并回显
      char buf[256];
      ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        send(client_fd, buf, n, 0); // echo 回去
      }
      close(client_fd);
    }
    close(fd);
  });

  // 给服务器一点时间启动
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // TODO 4.1: 客户端连接流程
  {
    // 1. 创建 socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd, "socket");
    ScopedFd guard(fd);

    // 2. 构建目标地址
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(12346);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr),
          "inet_pton");

    // 3. connect — 发起连接
    int ret = connect(fd, reinterpret_cast<sockaddr *>(&server_addr),
                      sizeof(server_addr));
    CHECK(ret, "connect");
    cout << "  ✅ 已连接到 127.0.0.1:12346\n";

    // 4. 发送数据
    const char *msg = "Hello from client!";
    ssize_t     sent = send(fd, msg, strlen(msg), 0);
    cout << "  📤 发送了 " << sent << " 字节: \"" << msg << "\"\n";

    // 5. 接收响应
    char    buf[256];
    ssize_t received = recv(fd, buf, sizeof(buf) - 1, 0);
    if (received > 0) {
      buf[received] = '\0';
      cout << "  📩 收到 " << received << " 字节: \"" << buf << "\"\n";
    }
  }

  server_thread.join();

  // TODO 4.2: connect 失败的常见原因
  {
    cout << "\n  connect 常见错误:\n";
    cout << "  ┌─────────────────────┬──────────────────────────────┐\n";
    cout << "  │ ECONNREFUSED        │ 端口上没有服务在监听         │\n";
    cout << "  │ ETIMEDOUT           │ 网络不可达（防火墙/路由）    │\n";
    cout << "  │ ENETUNREACH         │ 网络不可达（没有路由）       │\n";
    cout << "  │ EADDRNOTAVAIL       │ 本地地址不可用               │\n";
    cout << "  └─────────────────────┴──────────────────────────────┘\n";
  }

  // TODO 4.3: send() vs write() — 哪个更好？
  {
    cout << "\n  send() vs write() vs sendmsg():\n";
    cout << "    write(fd, buf, len) — 最简单的写入\n";
    cout << "    send(fd, buf, len, flags) — 多了 flags 参数\n";
    cout << "      flags=0: 同 write\n";
    cout << "      flags=MSG_NOSIGNAL: 对端关闭时不发 SIGPIPE (推荐!)\n";
    cout << "      flags=MSG_DONTWAIT: 非阻塞发送\n";
    cout << "    sendmsg() — 支持 scatter/gather I/O\n";
    cout << "    默认用 send() + MSG_NOSIGNAL，更安全\n";
  }
}

// ============================================================
// 练习 5: setsockopt — 套接字选项
// ============================================================
//
// setsockopt() 允许精细控制套接字行为。
// level 参数指定选项的层级 (SOL_SOCKET 是通用层, IPPROTO_TCP 是 TCP 层)。
//
// 常用选项:
//   SO_REUSEADDR — 允许重用本地地址（服务器重启必备）
//   SO_KEEPALIVE — TCP keep-alive 探测
//   SO_RCVBUF / SO_SNDBUF — 调整收发缓冲区大小
//   TCP_NODELAY  — 禁用 Nagle 算法（低延迟场景）

void exercise5_setsockopt() {
  section("练习 5: setsockopt — 套接字选项");

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd, "socket");
  ScopedFd guard(fd);

  // TODO 5.1: SO_REUSEADDR — 重用地址
  {
    int optval = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)),
          "setsockopt SO_REUSEADDR");
    cout << "  ✅ SO_REUSEADDR = 1\n";
    cout << "    作用: 服务器重启后立即可绑定同一端口（避免\"Address already in use\"）\n";
  }

  // TODO 5.2: SO_KEEPALIVE — TCP 保活
  {
    int optval = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)),
          "setsockopt SO_KEEPALIVE");
    cout << "  ✅ SO_KEEPALIVE = 1\n";
    cout << "    作用: 长时间空闲后发送探测包，检测对端是否还活着\n";
    cout << "    默认探测间隔很长（2 小时），需要配合 TCP_KEEPIDLE 等调整\n";
  }

  // TODO 5.3: TCP_NODELAY — 禁用 Nagle 算法
  {
    int optval = 1;
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)),
          "setsockopt TCP_NODELAY");
    cout << "  ✅ TCP_NODELAY = 1\n";
    cout << "    Nagle 算法: 合并小包以减少网络开销\n";
    cout << "    禁用后: 每次 send 立即发出（适合游戏、SSH 等低延迟场景）\n";
    cout << "    延迟敏感 → 禁用, 吞吐量优先 → 启用（默认）\n";
  }

  // TODO 5.4: SO_RCVTIMEO / SO_SNDTIMEO — 超时设置
  {
    cout << "\n  收发超时:\n";
    cout << "    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};\n";
    cout << "    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));\n";
    cout << "    设置后 recv/accept 等阻塞调用在超时后返回 -1 (errno=EAGAIN)\n";
    cout << "    ⚠️ 这是简单方案，生产环境更推荐 epoll + non-blocking\n";
  }

  // TODO 5.5: getsockopt — 读取选项
  {
    int optval;
    socklen_t optlen = sizeof(optval);
    getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, &optlen);
    cout << "\n  getsockopt 读取 SO_REUSEADDR = " << optval << "\n";
  }
}

// ============================================================
// 练习 6: getaddrinfo — 现代地址解析
// ============================================================
//
// getaddrinfo() 替代了旧的 gethostbyname()。
// 它支持 IPv4/IPv6 双栈，线程安全，返回链表。
//
// 参数:
//   node    — 主机名或 IP 字符串 (如 "google.com" 或 "127.0.0.1")
//   service — 端口号或服务名 (如 "80" 或 "http")
//   hints   — 过滤条件 (地址族、套接字类型等)
//   res     — 输出: 地址信息链表

void exercise6_getaddrinfo() {
  section("练习 6: getaddrinfo — 地址解析");

  // TODO 6.1: 解析本地地址
  {
    addrinfo hints{};
    hints.ai_family   = AF_INET;      // 只要 IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP
    hints.ai_flags    = AI_PASSIVE;   // 用于 bind (适合服务器)

    addrinfo *res = nullptr;
    int       ret = getaddrinfo(nullptr, "8080", &hints, &res);
    if (ret != 0) {
      cout << "  ❌ getaddrinfo 失败: " << gai_strerror(ret) << "\n";
    } else {
      // 遍历结果链表
      for (addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
        char ip_str[INET_ADDRSTRLEN];
        auto *addr_in = reinterpret_cast<sockaddr_in *>(rp->ai_addr);
        inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
        cout << "  ✅ 地址: " << ip_str << ", 端口: "
             << ntohs(addr_in->sin_port) << "\n";
      }
      freeaddrinfo(res); // 必须释放！
      cout << "  ✅ freeaddrinfo 已调用\n";
    }
  }

  // TODO 6.2: 解析域名
  {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    // 注意: 这个调用需要网络连接（DNS 查询）
    int ret = getaddrinfo("localhost", "http", &hints, &res);
    if (ret != 0) {
      cout << "  localhost 解析: " << gai_strerror(ret) << "\n";
    } else {
      auto *addr_in = reinterpret_cast<sockaddr_in *>(res->ai_addr);
      char  ip_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
      cout << "  ✅ localhost → " << ip_str << ":" << ntohs(addr_in->sin_port)
           << "\n";
      freeaddrinfo(res);
    }
  }

  // TODO 6.3: getaddrinfo 客户端模式
  {
    cout << "\n  客户端模式 (不设置 AI_PASSIVE):\n";

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    int       ret = getaddrinfo("127.0.0.1", "80", &hints, &res);
    if (ret == 0) {
      cout << "  ✅ 客户端地址解析成功\n";
      cout << "    可以用 res->ai_addr 直接传给 connect()\n";
      freeaddrinfo(res);
    }
  }

  // TODO 6.4: hints.ai_flags 常用值
  {
    cout << "\n  ai_flags 常用值:\n";
    cout << "  ┌──────────────────┬─────────────────────────────┐\n";
    cout << "  │ AI_PASSIVE       │ 用于 bind(), 地址为 INADDR_ANY│\n";
    cout << "  │ AI_CANONNAME     │ 返回规范主机名               │\n";
    cout << "  │ AI_NUMERICHOST   │ 禁止 DNS 查询, node 必须是 IP │\n";
    cout << "  │ AI_NUMERICSERV   │ 禁止服务名解析, service 是数字 │\n";
    cout << "  │ AI_V4MAPPED      │ 没有 IPv6 地址时返回映射的 IPv4│\n";
    cout << "  │ AI_ADDRCONFIG    │ 只返回系统配置了的地址族      │\n";
    cout << "  └──────────────────┴─────────────────────────────┘\n";
  }
}

// ============================================================
// 练习 7: 简易 TCP Echo 服务器 + 客户端
// ============================================================
//
// 综合练习: 一个完整的 echo 服务器。
// 服务器接收客户端发来的消息，原样返回。
// 支持多轮对话（一行一 echo）。

constexpr int ECHO_PORT = 12347;

// 单线程 echo 服务器（一次只能服务一个客户端）
void run_echo_server() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  ScopedFd guard(fd);

  int optval = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(ECHO_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) return;
  if (listen(fd, 1) < 0) return;

  cout << "  [Echo Server] 监听 0.0.0.0:" << ECHO_PORT << "\n";

  sockaddr_in client_addr{};
  socklen_t   client_len = sizeof(client_addr);
  int         client_fd  = accept(fd, reinterpret_cast<sockaddr *>(&client_addr),
                                  &client_len);
  if (client_fd < 0) return;
  ScopedFd client_guard(client_fd);

  char client_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
  cout << "  [Echo Server] 客户端已连接: " << client_ip << ":"
       << ntohs(client_addr.sin_port) << "\n";

  // 循环收发: 收到什么就发回什么
  char buf[1024];
  for (int i = 0; i < 5; ++i) { // 最多收发 5 轮
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      if (n == 0) cout << "  [Echo Server] 客户端关闭了连接\n";
      else cout << "  [Echo Server] recv 错误: " << std::strerror(errno) << "\n";
      break;
    }
    buf[n] = '\0';
    // 去掉末尾的换行（美观）
    string msg(buf);
    if (!msg.empty() && msg.back() == '\n') msg.pop_back();
    cout << "  [Echo Server] 收到: \"" << msg << "\", 回显\n";

    send(client_fd, buf, n, MSG_NOSIGNAL);
  }
}

// 客户端: 发送几条消息
void run_echo_client() {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  ScopedFd guard(fd);

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port   = htons(ECHO_PORT);
  inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

  if (connect(fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
    cout << "  [Echo Client] 连接失败: " << std::strerror(errno) << "\n";
    return;
  }
  cout << "  [Echo Client] 已连接\n";

  // 发送几条消息
  const char *messages[] = {"你好", "Hello", "Bonjour", "Ciao", "再见"};
  for (const auto *msg : messages) {
    cout << "  [Echo Client] 发送: \"" << msg << "\"\n";
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
    // 加个换行方便服务器解析（非必须）
    send(fd, "\n", 1, MSG_NOSIGNAL);

    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      string response(buf);
      if (!response.empty() && response.back() == '\n') response.pop_back();
      cout << "  [Echo Client] 收到: \"" << response << "\"\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void exercise7_echo_server() {
  section("练习 7: TCP Echo 服务器 + 客户端");

  std::thread server(run_echo_server);
  std::thread client(run_echo_client);

  client.join();
  server.join();

  cout << "\n  ✅ Echo 测试完成\n";
  cout << "  💡 这只是一个单线程 echo 服务器\n";
  cout << "     Week 13 我们将用 epoll 实现能同时处理数千连接的高性能服务器\n";
}

// ============================================================
// 练习 8: UDP 基础 — sendto / recvfrom
// ============================================================
//
// UDP 是无连接协议:
//   - 不需要 connect (但也可以 connect 以固定对端)
//   - 不保证送达（可能丢包）
//   - 不保证顺序
//   - 保留消息边界（不像 TCP 是字节流）
//
// 适用场景: DNS 查询、视频直播、在线游戏（实时性 > 可靠性）

constexpr int UDP_PORT = 12348;

void run_udp_echo_server() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  ScopedFd guard(fd);

  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(UDP_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;
  (void)bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

  cout << "  [UDP Server] 监听 0.0.0.0:" << UDP_PORT << "\n";

  // UDP: 用 recvfrom 接收（同时获取发送方地址）
  char        buf[256];
  sockaddr_in client_addr{};
  socklen_t   client_len = sizeof(client_addr);

  for (int i = 0; i < 3; ++i) {
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (n > 0) {
      buf[n] = '\0';
      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
      cout << "  [UDP Server] 收到来自 " << ip << ":"
           << ntohs(client_addr.sin_port) << " 的消息: \"" << buf << "\"\n";

      // echo 回去
      sendto(fd, buf, n, 0, reinterpret_cast<sockaddr *>(&client_addr), client_len);
    }
  }
}

void run_udp_client() {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  ScopedFd guard(fd);

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port   = htons(UDP_PORT);
  inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

  const char *messages[] = {"Ping", "Pong", "Pang"};
  for (const auto *msg : messages) {
    cout << "  [UDP Client] 发送: \"" << msg << "\"\n";
    sendto(fd, msg, strlen(msg), 0,
           reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr));

    // 接收回显
    char        buf[256];
    sockaddr_in from{};
    socklen_t   from_len = sizeof(from);
    ssize_t     n        = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                    reinterpret_cast<sockaddr *>(&from), &from_len);
    if (n > 0) {
      buf[n] = '\0';
      cout << "  [UDP Client] 收到: \"" << buf << "\"\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void exercise8_udp() {
  section("练习 8: UDP — sendto / recvfrom");

  std::thread server(run_udp_echo_server);
  std::thread client(run_udp_client);

  client.join();
  server.join();

  // TODO 8.1: UDP vs TCP
  {
    cout << "\n  UDP vs TCP 对比:\n";
    cout << "  ┌─────────────┬──────────────────┬────────────────────┐\n";
    cout << "  │             │ TCP              │ UDP                │\n";
    cout << "  ├─────────────┼──────────────────┼────────────────────┤\n";
    cout << "  │ 连接       │ 面向连接 (三次握手)│ 无连接             │\n";
    cout << "  │ 可靠性     │ 保证送达+顺序    │ 不保证             │\n";
    cout << "  │ 边界       │ 字节流(无边界)   │ 保留消息边界       │\n";
    cout << "  │ 速度       │ 较慢(拥塞控制)   │ 较快               │\n";
    cout << "  │ 适用       │ HTTP, 文件传输   │ DNS, 直播, 游戏    │\n";
    cout << "  │ send/recv  │ send()/recv()    │ sendto()/recvfrom()│\n";
    cout << "  └─────────────┴──────────────────┴────────────────────┘\n";
  }

  // TODO 8.2: UDP 也可以 connect
  {
    cout << "\n  💡 UDP connect:\n";
    cout << "    UDP socket 也可以调用 connect()\n";
    cout << "    不是建立连接，而是「固定」对端地址\n";
    cout << "    connect 后可以用 send()/recv() 代替 sendto()/recvfrom()\n";
    cout << "    也能收到 ICMP 错误（如端口不可达）\n";
  }

  // TODO 8.3: SOCK_DGRAM 的最大包大小
  {
    cout << "\n  UDP 包大小限制:\n";
    cout << "    理论最大: 65507 字节 (IPv4)\n";
    cout << "    实际安全: ~1400 字节 (避免分片, 适合以太网 MTU)\n";
    cout << "    超过 MTU 会 IP 分片 → 增加丢包率\n";
  }
}

// ============================================================
// 练习 9: 非阻塞 I/O 基础 — fcntl O_NONBLOCK
// ============================================================
//
// 默认情况下 socket 操作是阻塞的:
//   - accept() 没有连接 → 阻塞
//   - recv() 没有数据 → 阻塞
//   - connect() 等待握手完成 → 阻塞
//
// 非阻塞模式: 操作立即返回，如果没有数据则返回 -1 (errno=EAGAIN/EWOULDBLOCK)
// 非阻塞 I/O 是多路复用 (epoll) 的基础。

void exercise9_nonblocking() {
  section("练习 9: 非阻塞 I/O — fcntl O_NONBLOCK");

  // TODO 9.1: 设置非阻塞
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd, "socket");
    ScopedFd guard(fd);

    // 获取当前 flags
    int flags = fcntl(fd, F_GETFL, 0);
    CHECK(flags, "fcntl F_GETFL");

    // 设置 O_NONBLOCK
    CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK), "fcntl F_SETFL");

    cout << "  ✅ 已设置 O_NONBLOCK\n";

    // 在非阻塞 socket 上调用 accept → 立即返回
    // （因为没有 bind/listen，这个 accept 会立即失败）
    int ret = accept(fd, nullptr, nullptr);
    if (ret == -1) {
      cout << "  accept 返回 -1, errno = " << errno << " ("
           << std::strerror(errno) << ")\n";
      cout << "  💡 非阻塞模式下, 操作不可立即完成时返回 EAGAIN\n";
    }
  }

  // TODO 9.2: 非阻塞 connect
  {
    cout << "\n  非阻塞 connect:\n";
    cout << "    1. 设置 O_NONBLOCK\n";
    cout << "    2. 调用 connect() → 通常返回 -1, errno=EINPROGRESS\n";
    cout << "    3. 用 select/poll/epoll 等待可写 → 连接完成\n";
    cout << "    4. getsockopt(SO_ERROR) 检查是否真正成功\n";
    cout << "    这样可以同时发起多个连接，不用串行等待\n";
  }

  // TODO 9.3: 阻塞 vs 非阻塞 vs 异步
  {
    cout << "\n  I/O 模型对比:\n";
    cout << "  ┌─────────────────┬──────────────────────────────┐\n";
    cout << "  │ 阻塞 I/O        │ 线程睡眠，数据就绪后唤醒     │\n";
    cout << "  │ 非阻塞 I/O      │ 立即返回，没数据时返回 EAGAIN │\n";
    cout << "  │ I/O 多路复用    │ select/poll/epoll 同时监控    │\n";
    cout << "  │                 │ 多个 fd，有事件时通知         │\n";
    cout << "  │ 异步 I/O        │ 内核完成操作后回调 (io_uring) │\n";
    cout << "  └─────────────────┴──────────────────────────────┘\n";
    cout << "    Week 13 将深入 epoll (多路复用)\n";
  }
}

// ============================================================
// 练习 10: 实战 — 简易 HTTP 请求
// ============================================================
//
// 综合运用: 用原始 socket 发送一个 HTTP GET 请求。
// 这展示了 HTTP 协议本质上就是"在 TCP 上发送文本"。

void exercise10_simple_http() {
  section("练习 10: 实战 — 用 socket 发送 HTTP 请求");

  cout << "  演示: 连接到 example.com 并发送 HTTP GET 请求\n";

  // 1. 解析域名
  addrinfo hints{};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *res = nullptr;
  int       ret = getaddrinfo("example.com", "80", &hints, &res);
  if (ret != 0) {
    cout << "  ⚠️ DNS 解析失败: " << gai_strerror(ret)
         << " (可能需要网络连接)\n";
    // 降级: 手动构造
    cout << "  降级: 使用本地地址演示...\n";

    // 启动一个简单的响应服务器
    std::thread fake_server([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family      = AF_INET;
      addr.sin_port        = htons(12349);
      addr.sin_addr.s_addr = INADDR_ANY;
      (void)bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      (void)listen(fd, 1);
      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        // 读取请求
        char req[1024];
        recv(client, req, sizeof(req) - 1, 0);
        // 发送最小的 HTTP 响应
        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 20\r\n"
            "\r\n"
            "Hello from C++ HTTP!\n";
        send(client, resp, strlen(resp), 0);
        close(client);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int fd2 = socket(AF_INET, SOCK_STREAM, 0);
    if (fd2 >= 0) {
      ScopedFd guard2(fd2);
      sockaddr_in local{};
      local.sin_family = AF_INET;
      local.sin_port   = htons(12349);
      inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
      (void)connect(fd2, reinterpret_cast<sockaddr *>(&local), sizeof(local));

      // 发送 HTTP 请求
      const char *request =
          "GET / HTTP/1.1\r\n"
          "Host: localhost:12349\r\n"
          "Connection: close\r\n"
          "\r\n";
      send(fd2, request, strlen(request), 0);

      // 接收响应
      char resp[4096];
      ssize_t n = recv(fd2, resp, sizeof(resp) - 1, 0);
      if (n > 0) {
        resp[n] = '\0';
        cout << "  📩 HTTP 响应:\n" << resp << "\n";
      }
    }
    fake_server.join();
    return;
  }

  // 2. 创建 socket 并连接
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd, "socket");
  ScopedFd guard(fd);

  ret = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  CHECK(ret, "connect");

  cout << "  ✅ 已连接到 example.com:80\n";

  // 3. 发送 HTTP 请求
  const char *request =
      "GET / HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  send(fd, request, strlen(request), 0);
  cout << "  📤 已发送 HTTP GET 请求\n";

  // 4. 接收响应
  char    response[8192];
  ssize_t total = 0;
  ssize_t n;
  while ((n = recv(fd, response + total, sizeof(response) - total - 1, 0)) > 0) {
    total += n;
    if (total >= static_cast<ssize_t>(sizeof(response) - 1)) break;
  }
  response[total] = '\0';

  // 只打印前 500 字符
  string preview(response, std::min(total, static_cast<ssize_t>(500)));
  cout << "  📩 收到 " << total << " 字节 (显示前 500):\n"
       << preview << "\n";
  if (total > 500) cout << "  ... (截断)\n";

  cout << "\n  💡 HTTP 协议本质:\n";
  cout << "    - HTTP 是在 TCP 连接上发送文本（或二进制）\n";
  cout << "    - 请求和响应都是「头部 + 空行 + 可选 body」\n";
  cout << "    - 我们将在 Week 14 深入 HTTP 协议\n";
}

// ============================================================
// main — 运行所有练习
// ============================================================

int main(int argc, char *argv[]) {
  cout << "Week 11: Socket 编程基础 — socket/bind/listen/accept/connect\n";
  cout << "================================================================\n";

  // 如果传递了命令行参数则只运行特定模式
  if (argc > 1) {
    string mode = argv[1];
    if (mode == "server") {
      run_echo_server();
      return 0;
    }
    if (mode == "client") {
      run_echo_client();
      return 0;
    }
    if (mode == "udp-server") {
      run_udp_echo_server();
      return 0;
    }
    if (mode == "udp-client") {
      run_udp_client();
      return 0;
    }
    cout << "用法: " << argv[0]
         << " [server|client|udp-server|udp-client]\n";
    return 1;
  }

  // 默认运行所有练习
  exercise1_create_socket();
  exercise2_address_structures();
  // exercise3 是阻塞演示，需要手动运行客户端
  // 可以: ./build/socket_basics 先跳过, 在另一个终端运行 client
  cout << "\n⏭️  跳过练习 3 (TCP 服务器 — 需要手动客户端)"
       << "\n    在另一个终端运行: ./build/socket_basics client\n";
  exercise4_tcp_client();
  exercise5_setsockopt();
  exercise6_getaddrinfo();
  exercise7_echo_server();
  exercise8_udp();
  exercise9_nonblocking();
  exercise10_simple_http();

  cout << "\n✅ Week 11 全部练习完成！\n";
  cout << "\n📝 总结要点:\n";
  cout << "  1. socket() 创建通信端点, AF_INET=IPv4, SOCK_STREAM=TCP, SOCK_DGRAM=UDP\n";
  cout << "  2. 服务器: socket → bind → listen → accept → recv/send → close\n";
  cout << "  3. 客户端: socket → connect → send/recv → close\n";
  cout << "  4. 网络字节序是大端, 必须用 htons/htonl/ntohs/ntohl 转换\n";
  cout << "  5. setsockopt 控制套接字行为: SO_REUSEADDR(重启必备), TCP_NODELAY(低延迟)\n";
  cout << "  6. getaddrinfo 是线程安全的地址解析函数, 替代 gethostbyname\n";
  cout << "  7. TCP 面向连接/可靠/字节流; UDP 无连接/不可靠/保留边界\n";
  cout << "  8. 非阻塞 I/O (O_NONBLOCK) 是多路复用(epoll)的基础\n";
  cout << "  9. send() + MSG_NOSIGNAL 防止对端关闭时触发 SIGPIPE\n";
  cout << "  10. 用 RAII (ScopedFd) 管理 socket 文件描述符, 避免泄漏\n";

  return 0;
}
