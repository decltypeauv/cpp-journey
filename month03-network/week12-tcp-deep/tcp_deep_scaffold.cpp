// Week 12: TCP 深入 — Nagle / KeepAlive / 粘包 / 优雅关闭
// 编译: cmake -B build && cmake --build build
// 运行: ./build/tcp_deep
//
// 在 Week 11 的基础上，深入理解 TCP 的细节行为。
// 这些知识对编写可靠的网络服务至关重要。
//
// 核心主题:
//   Nagle 算法 — 吞吐量与延迟的权衡
//   KeepAlive  — 检测死连接
//   粘包/拆包 — TCP 字节流的消息边界问题
//   shutdown() — 半关闭与优雅关闭
//   SO_LINGER  — 控制 close() 行为
//   TCP 状态机 — 11 种状态和状态转换

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <optional>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
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

// RAII socket 封装
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

// ============================================================
// 练习 1: Nagle 算法深入
// ============================================================
//
// Nagle 算法是 TCP 层面的优化，默认启用。
// 它的规则很简单:
//   - 如果有一个未确认的小包在途，后续的小包必须等待
//   - 直到收到 ACK 或者累积到 MSS 大小才发送
//
// 目的: 减少网络上的小包数量，提高带宽利用率。
// 代价: 增加延迟（对延迟敏感的应用是灾难）。
//
// 何时禁用它 (TCP_NODELAY):
//   - 交互式应用 (SSH, telnet)
//   - 在线游戏 (延迟 > 吞吐量)
//   - HTTP 请求/响应 (已发送完，不要再等)
//   - 任何 request-response 模式

void exercise1_nagle() {
  section("练习 1: Nagle 算法深入");

  // TODO 1.1: Nagle 算法原理
  {
    cout << "  Nagle 算法规则:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ 1. 如果发送窗口中有未确认的数据 (unacked)          │\n";
    cout << "  │ 2. 且新数据 < MSS (Maximum Segment Size)            │\n";
    cout << "  │    → 延迟发送, 等待更多数据或 ACK                   │\n";
    cout << "  │ 3. 否则 → 立即发送                                  │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
    cout << "\n  这意味着: 在 ACK 回来之前，小包会被「攒」起来\n";
  }

  // TODO 1.2: 实验 — 观察 Nagle 对小包的影响
  {
    subsection("实验: Nagle 对小包延迟的影响");

    // 启动简单的 echo 服务器（接收两次连接：Nagle ON 和 OFF 各一次）
    std::thread server_thread([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);

      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(12351);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 2);

      // 接受两次连接 — 分别对应 Nagle ON 和 OFF 的测试
      for (int conn = 0; conn < 2; ++conn) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd =
            accept(fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
        if (client_fd >= 0) {
          ScopedFd cg(client_fd);
          // 只是接收数据然后丢弃 (不做 echo, 避免干扰测量)
          char buf[256];
          for (int i = 0; i < 10; ++i) {
            recv(client_fd, buf, sizeof(buf), 0);
          }
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- 客户端: 对比 Nagle ON vs OFF ---

    auto test_nagle = [](bool nagle_on) {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
        return;
      ScopedFd guard(fd);

      if (!nagle_on) {
        int optval = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
      }

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(12351);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
          -1) {
        cout << "    (connect 失败: " << std::strerror(errno)
             << " — 服务器可能已关闭)\n";
        return;
      }

      // 连续发送 10 个小包 (每个只有 1 字节)
      const char c = 'X';
      for (int i = 0; i < 10; ++i) {
        send(fd, &c, 1, MSG_NOSIGNAL);
      }
    };

    // 测试 Nagle ON (默认)
    {
      test_nagle(true);
      cout << "  ✅ Nagle ON:  10 个小包可能被合并为 1-2 个 TCP 段\n";
      cout << "     → 带宽利用率高, 但第一个小包可能延迟 ~40ms 等 ACK\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 测试 Nagle OFF (TCP_NODELAY)
    {
      test_nagle(false);
      cout << "  ✅ Nagle OFF: 每个 send() 立即生成一个 TCP 段\n";
      cout << "     → 10 个小包 = 10 个 TCP 段, 延迟最低\n";
    }

    server_thread.join();
  }

  // TODO 1.3: Nagle + Delayed ACK 的「40ms 问题」
  {
    subsection("Nagle + Delayed ACK = 灾难组合");

    cout << "  这是一个经典的交互问题:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ 1. 发送方: 小包在途, Nagle 阻止新的小包            │\n";
    cout << "  │ 2. 接收方: Delayed ACK (等 ~40ms, 看有没有数据可捎) │\n";
    cout << "  │ 3. 双方互相等待, 造成 ~40ms 的「死锁」延迟         │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
    cout << "\n  解决方案:\n";
    cout << "    - 发送方: TCP_NODELAY\n";
    cout << "    - 发送方: TCP_QUICKACK (Linux) — 接收方禁用 Delayed ACK\n";
    cout << "    - 应用层: 用 writev() 合并 write+write → 一次系统调用\n";
    cout << "    - 应用层: 用 TCP_CORK (Linux) 攒到一定大小再发送\n";
  }

  // TODO 1.4: TCP_CORK — Linux 特有的替代方案
  {
    subsection("TCP_CORK vs TCP_NODELAY");

    cout << "  TCP_NODELAY (禁用 Nagle):\n";
    cout << "    - 立即发送，即使是很小的包\n";
    cout << "    - 适合: 低延迟场景\n";
    cout << "\n";
    cout << "  TCP_CORK (塞住管道):\n";
    cout << "    - 将多个 write 合并成一个 TCP 段再发送\n";
    cout << "    - int opt = 1; setsockopt(fd, IPPROTO_TCP, TCP_CORK, &opt, sizeof(opt));\n";
    cout << "    - opt = 0 时立即发送所有累积的数据\n";
    cout << "    - 比 Nagle 更可控: 你决定何时 flush\n";
    cout << "    - 适合: HTTP 响应 (header+body 一起发)\n";
    cout << "\n";
    cout << "  💡 经验法则:\n";
    cout << "    延迟敏感 → TCP_NODELAY\n";
    cout << "    吞吐量优先 → 默认 Nagle\n";
    cout << "    精确控制 → TCP_CORK\n";
  }
}

// ============================================================
// 练习 2: TCP KeepAlive — 检测死连接
// ============================================================
//
// TCP 连接建立后，如果对端崩溃/网络断掉但没有发 FIN，
// 我们怎么知道连接已经死了？
//
// TCP KeepAlive 就是为此设计的:
//   - 如果连接空闲了 tcp_keepalive_time 秒
//   - 内核开始发送探测包 (keep-alive probes)
//   - 如果连续 tcp_keepalive_probes 次没有响应
//   - 连接被关闭 (recv 返回错误)
//
// 注意: KeepAlive 默认配置非常保守 (2 小时空闲 + 9 次探测)
//       大多数应用需要自己实现应用层心跳。

void exercise2_keepalive() {
  section("练习 2: TCP KeepAlive");

  // TODO 2.1: 配置 KeepAlive 参数
  {
    subsection("配置 KeepAlive");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd, "socket");
    ScopedFd guard(fd);

    // 1. 启用 KeepAlive
    int optval = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)),
          "SO_KEEPALIVE");

    // 2. TCP_KEEPIDLE — 空闲多久后开始探测 (秒)
    //    Linux 默认: 7200 (2小时!) → 太长了
    optval = 60; // 60 秒空闲后开始探测
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &optval, sizeof(optval)),
          "TCP_KEEPIDLE");

    // 3. TCP_KEEPINTVL — 探测间隔 (秒)
    //    Linux 默认: 75 秒
    optval = 10; // 每 10 秒发一次探测
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &optval, sizeof(optval)),
          "TCP_KEEPINTVL");

    // 4. TCP_KEEPCNT — 多少次探测失败后关闭连接
    //    Linux 默认: 9 次
    optval = 3; // 3 次探测失败 → 连接死
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &optval, sizeof(optval)),
          "TCP_KEEPCNT");

    cout << "  ✅ KeepAlive 已配置:\n";
    cout << "    - 空闲 60 秒后开始探测\n";
    cout << "    - 每 10 秒探测一次\n";
    cout << "    - 连续 3 次失败 → 连接关闭\n";
    cout << "    - 总超时: 60 + 3×10 = 90 秒\n";
  }

  // TODO 2.2: 读取当前 KeepAlive 配置
  {
    subsection("读取 KeepAlive 配置");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd, "socket");
    ScopedFd guard(fd);

    int idle, intvl, cnt;
    socklen_t len = sizeof(idle);

    getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, &len);
    getsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, &len);
    getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, &len);

    cout << "  系统默认 KeepAlive 参数:\n";
    cout << "    TCP_KEEPIDLE = " << idle << " 秒 (空闲后首次探测)\n";
    cout << "    TCP_KEEPINTVL = " << intvl << " 秒 (探测间隔)\n";
    cout << "    TCP_KEEPCNT = " << cnt << " 次 (最大重试)\n";

    if (idle >= 7200) {
      cout << "  ⚠️  默认空闲时间 2 小时! 对大多数应用来说太长了\n";
      cout << "    建议根据业务需求调整 (如 30-60 秒)\n";
    }
  }

  // TODO 2.3: KeepAlive vs 应用层心跳
  {
    subsection("KeepAlive vs 应用层心跳");

    cout << "  ┌──────────────┬──────────────────┬──────────────────┐\n";
    cout << "  │              │ TCP KeepAlive    │ 应用层心跳       │\n";
    cout << "  ├──────────────┼──────────────────┼──────────────────┤\n";
    cout << "  │ 实现层级      │ 内核 TCP 栈      │ 应用程序         │\n";
    cout << "  │ 检测能力      │ 连接是否存活      │ 连接+应用是否健康 │\n";
    cout << "  │ 配置灵活性    │ 有限(3个参数)     │ 完全自定义       │\n";
    cout << "  │ 跨平台        │ 参数名不同 😞    │ 完全一致         │\n";
    cout << "  │ 中间设备      │ 可能被路由器丢弃  │ 不会被干扰       │\n";
    cout << "  └──────────────┴──────────────────┴──────────────────┘\n";
    cout << "\n  💡 最佳实践: 两者结合使用\n";
    cout << "    - 启用 KeepAlive (作为最后的保底)\n";
    cout << "    - 应用层心跳 (真正可靠的存活检测)\n";
  }

  // TODO 2.4: 系统级 KeepAlive 配置
  {
    subsection("系统级默认值");

    cout << "  系统默认参数 (可通过 /proc/sys/net/ipv4/ 调整):\n";
    cout << "    /proc/sys/net/ipv4/tcp_keepalive_time    — 默认 7200\n";
    cout << "    /proc/sys/net/ipv4/tcp_keepalive_intvl   — 默认 75\n";
    cout << "    /proc/sys/net/ipv4/tcp_keepalive_probes  — 默认 9\n";
    cout << "\n  ⚠️  修改系统级参数会影响所有 socket\n";
    cout << "    推荐用 setsockopt 逐个 socket 配置\n";
  }
}

// ============================================================
// 练习 3: 粘包问题 — TCP 字节流的本质
// ============================================================
//
// TCP 是字节流协议，不保留消息边界。
// send("Hello") + send("World") 可能被对端 recv 为:
//   - "HelloWorld" (粘包)
//   - "Hel" + "loWorld" (拆包)
//   - "H" + "elloWorld" (再拆)
//
// 这是 TCP 的设计特性，不是 bug。
// 应用层必须自己处理消息边界！

void exercise3_sticky_packet() {
  section("练习 3: 粘包问题 — TCP 字节流演示");

  // TODO 3.1: 演示粘包/拆包
  {
    subsection("实验: 观察粘包/拆包");

    constexpr int PORT = 12352;

    // 服务器: 发送两条消息，中间不延时
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
        // 连续发送两条消息 — TCP 很可能会合并它们
        send(client, "Hello", 5, 0);
        send(client, "World", 5, 0);
        // 没有延时: Nagle 可能合并，即使不合并也会在同一个 TCP 段中
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 客户端: 接收数据
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 用大缓冲区一次性接收
      char buf[1024]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

      cout << "  服务器发送: send(\"Hello\") + send(\"World\")\n";
      cout << "  客户端收到: \"" << buf << "\" (" << n << " 字节)\n";

      if (string(buf, n) == "HelloWorld") {
        cout << "  🔴 粘包! 两条消息被合并了\n";
      } else if (string(buf, n).find("Hello") != string::npos &&
                 string(buf, n).find("World") != string::npos) {
        cout << "  🟡 部分粘包\n";
      }

      cout << "\n  💡 如果客户端只读 3 字节:\n";
      cout << "    recv(fd, buf, 3, 0) → \"Hel\"\n";
      cout << "    recv(fd, buf, 5, 0) → \"loWor\"\n";
      cout << "    剩下的 \"ld\" 还在缓冲区等着\n";
      cout << "    这就是「拆包」\n";
    }

    server.join();
  }

  // TODO 3.2: 粘包的根本原因
  {
    subsection("为什么会出现粘包/拆包?");

    cout << "  三个层面的原因:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ 1. 发送端: Nagle 算法合并小包                      │\n";
    cout << "  │ 2. 接收端: 应用读取速度 ≠ 网络到达速度             │\n";
    cout << "  │ 3. 网络层: IP 分片、MTU 限制、路由缓存             │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
    cout << "\n  TCP 只保证:\n";
    cout << "    ✅ 字节按序到达\n";
    cout << "    ✅ 不丢不重\n";
    cout << "    ❌ 不保证消息边界\n";
  }

  // TODO 3.3: 粘包对应用的影响
  {
    subsection("现实中的场景");

    cout << "  假设你的协议是 JSON 消息:\n";
    cout << "    send(\"{\\\"type\\\":\\\"login\\\"}\\n\");\n";
    cout << "    send(\"{\\\"type\\\":\\\"msg\\\"}\\n\");\n\n";
    cout << "  接收端可能收到:\n";
    cout << "    \"{\\\"type\\\":\\\"login\\\"}\\n{\\\"type\\\":\\\"msg\\\"}\\n\"\n";
    cout << "    — 两条 JSON 粘在一起了!\n\n";
    cout << "  解决办法 (下面三个练习):\n";
    cout << "    1. 定长消息\n";
    cout << "    2. 分隔符 (如 \\\\n, \\\\r\\\\n)\n";
    cout << "    3. 长度前缀 (最常用!)\n";
  }
}

// ============================================================
// 练习 4: 解决方案 1 — 定长消息
// ============================================================
//
// 最简单粗暴的解决方案: 所有消息都一样长。
// recv 时读固定字节数即可。
//
// 优点: 实现简单，解析零开销
// 缺点: 浪费带宽（短消息要填充），不灵活

void exercise4_fixed_length() {
  section("练习 4: 方案 1 — 定长消息");

  constexpr int MSG_SIZE = 64; // 所有消息都是 64 字节

  // TODO 4.1: 定长消息的发送和接收
  {
    subsection("定长消息示例");

    constexpr int PORT = 12353;

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

        // 发送 3 条定长消息
        char msg[MSG_SIZE]{};
        const char *texts[] = {"Hello", "TCP-Deep-Dive", "Bye"};
        for (const auto *t : texts) {
          memset(msg, 0, MSG_SIZE); // 清空
          strncpy(msg, t, MSG_SIZE - 1);
          send(client, msg, MSG_SIZE, MSG_NOSIGNAL);
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      char buf[MSG_SIZE]{};
      for (int i = 0; i < 3; ++i) {
        // 精确读取 MSG_SIZE 字节
        ssize_t received = 0;
        while (received < MSG_SIZE) {
          ssize_t n = recv(fd, buf + received, MSG_SIZE - received, 0);
          if (n <= 0)
            break;
          received += n;
        }
        cout << "  消息 " << (i + 1) << ": \"" << buf << "\" ("
             << received << " 字节)\n";
      }
    }

    server.join();

    cout << "\n  定长消息的优缺点:\n";
    cout << "    ✅ 简单: 读 N 字节 = 一条消息\n";
    cout << "    ✅ 高效: 不需要解析分隔符或长度\n";
    cout << "    ❌ 浪费: 短消息也占满 64 字节\n";
    cout << "    ❌ 不支持变长数据 (如文件传输)\n";
    cout << "    💡 适合: 固定格式的传感器数据、游戏状态同步\n";
  }
}

// ============================================================
// 练习 5: 解决方案 2 — 分隔符协议
// ============================================================
//
// 用特殊字符标记消息结束，如:
//   - \\n (换行) — 适合文本协议
//   - \\r\\n — HTTP, SMTP 等标准协议
//   - \\0 — Redis 协议中的字符串
//
// 接收端逐字节（或逐块）读取，直到遇到分隔符。
//
// 优点: 人类可读，调试方便
// 缺点: 需要对数据进行转义（如果数据中出现了分隔符）

void exercise5_delimiter() {
  section("练习 5: 方案 2 — 分隔符协议");

  constexpr int PORT = 12354;

  // TODO 5.1: 基于 \\n 分隔符的协议
  {
    subsection("\\\\n 分隔符协议");

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
        // 发送以 \\n 分隔的消息
        const char *msgs[] = {"LOGIN user=alice\n", "SEND hello\n",
                              "LOGOUT\n"};
        // 故意合并发送 — 接收端必须能拆分
        string combined;
        for (const auto *m : msgs)
          combined += m;
        send(client, combined.c_str(), combined.size(), MSG_NOSIGNAL);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 逐字节读取，直到遇到 \\n
      string line;
      char c;
      int msg_count = 0;

      while (msg_count < 3) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0)
          break;
        if (c == '\n') {
          cout << "  消息 " << (++msg_count) << ": \"" << line << "\"\n";
          line.clear();
        } else {
          line += c;
        }
      }
    }

    server.join();

    cout << "\n  分隔符协议的优缺点:\n";
    cout << "    ✅ 人类可读 (telnet 可直接调试)\n";
    cout << "    ✅ 变长消息\n";
    cout << "    ❌ 逐字节读取效率低 (可用缓冲区优化)\n";
    cout << "    ❌ 数据中不能包含分隔符 (需要转义)\n";
    cout << "    💡 适合: 文本协议 (HTTP, Redis, SMTP)\n";
  }

  // TODO 5.2: 高效读取 — 用缓冲区而非逐字节
  {
    subsection("优化: 批量读取 + 行拆分");

    cout << "  高效的读行实现 (伪代码):\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ class LineReader {                                  │\n";
    cout << "  │   char _buf[4096];  // 读缓冲区                     │\n";
    cout << "  │   string _pending;  // 未完成的行                    │\n";
    cout << "  │                                                     │\n";
    cout << "  │   optional<string> readline(int fd) {               │\n";
    cout << "  │     while (true) {                                  │\n";
    cout << "  │       auto pos = _pending.find('\\\\n');              │\n";
    cout << "  │       if (pos != npos) {                            │\n";
    cout << "  │         string line = _pending.substr(0, pos);      │\n";
    cout << "  │         _pending.erase(0, pos+1);                   │\n";
    cout << "  │         return line;                                │\n";
    cout << "  │       }                                             │\n";
    cout << "  │       ssize_t n = recv(fd, _buf, sizeof(_buf), 0);  │\n";
    cout << "  │       if (n <= 0) return nullopt;                   │\n";
    cout << "  │       _pending.append(_buf, n);                     │\n";
    cout << "  │     }                                               │\n";
    cout << "  │   }                                                 │\n";
    cout << "  │ };                                                  │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
    cout << "\n  这样一次系统调用就能处理多行，比逐字节高效得多\n";
  }
}

// ============================================================
// 练习 6: 解决方案 3 — 长度前缀协议 (最常用!)
// ============================================================
//
// 每条消息 = [长度(4字节)] + [数据(N字节)]
//
// 这就是「TLV」(Type-Length-Value) 编码的简化版。
// 几乎所有二进制协议都用这种方式。
//
// 优点:
//   - 支持任意二进制数据 (不需要转义)
//   - 接收端提前知道消息大小，可以高效分配缓冲区
//   - 解析简单快速
//
// 这是工业界最常用的方案！

void exercise6_length_prefix() {
  section("练习 6: 方案 3 — 长度前缀协议 (推荐)");

  constexpr int PORT = 12355;

  // TODO 6.1: 实现长度前缀协议
  {
    subsection("长度前缀协议: [4 字节长度][数据]");

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

        // 发送 3 条消息，每条 = [4字节长度] [数据]
        const char *messages[] = {"Hello, World!", "短", "A longer message with more content"};
        for (const auto *msg : messages) {
          uint32_t len = htonl(strlen(msg)); // 网络字节序!
          send(client, &len, sizeof(len), MSG_NOSIGNAL);
          send(client, msg, strlen(msg), MSG_NOSIGNAL);
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // --- 辅助函数: 精确读取 N 字节 ---
      auto recv_exact = [](int fd, void *buf, size_t n) -> bool {
        size_t received = 0;
        auto *ptr = static_cast<char *>(buf);
        while (received < n) {
          ssize_t r = recv(fd, ptr + received, n - received, 0);
          if (r <= 0)
            return false;
          received += r;
        }
        return true;
      };

      // 循环接收消息
      for (int i = 0; i < 3; ++i) {
        // 1. 读取 4 字节长度
        uint32_t net_len;
        if (!recv_exact(fd, &net_len, sizeof(net_len)))
          break;
        uint32_t len = ntohl(net_len);

        // 2. 读取 len 字节数据
        vector<char> data(len + 1);
        if (!recv_exact(fd, data.data(), len))
          break;
        data[len] = '\0';

        cout << "  消息 " << (i + 1) << ": \"" << data.data()
             << "\" (" << len << " 字节)\n";
      }
    }

    server.join();
  }

  // TODO 6.2: 为什么这是最优方案?
  {
    subsection("三种方案对比");

    cout << "  ┌──────────────┬──────────┬──────────┬──────────┐\n";
    cout << "  │              │ 定长      │ 分隔符   │ 长度前缀  │\n";
    cout << "  ├──────────────┼──────────┼──────────┼──────────┤\n";
    cout << "  │ 变长消息      │ ❌        │ ✅       │ ✅       │\n";
    cout << "  │ 二进制安全      │ ✅       │ ❌       │ ✅       │\n";
    cout << "  │ 访问开销      │ O(1)     │ O(n)     │ O(1)     │\n";
    cout << "  │ 带宽效率      │ 差        │ 好       │ 好       │\n";
    cout << "  │ 实现复杂度      │ 低        │ 中       │ 中       │\n";
    cout << "  └──────────────┴──────────┴──────────┴──────────┘\n";

    cout << "\n  💡 工业界实践:\n";
    cout << "    - gRPC/Protobuf: 长度前缀\n";
    cout << "    - WebSocket frame: 长度前缀 (带 mask)\n";
    cout << "    - Redis: 文本分隔符 (人类可读)\n";
    cout << "    - HTTP/1.1: 分隔符 + Content-Length (混合)\n";
    cout << "    - MySQL/PostgreSQL: 长度前缀\n";
  }

  // TODO 6.3: 常见陷阱
  {
    subsection("实现时要注意");

    cout << "  ⚠️  问题 1: 长度字段字节序\n";
    cout << "    必须用 htonl/ntohl 转换!\n";
    cout << "    否则在不同架构间无法互通\n\n";

    cout << "  ⚠️  问题 2: 长度字段本身可能被拆包\n";
    cout << "    你请求 4 字节长度，recv 可能只返回 1 字节\n";
    cout << "    → 必须循环读取直到凑够 4 字节 (recv_exact)\n\n";

    cout << "  ⚠️  问题 3: 恶意客户端发送超大长度\n";
    cout << "    len = 0xFFFFFFFF → 你试图分配 4GB 内存 💥\n";
    cout << "    → 设置最大消息长度限制 (如 64MB)\n\n";

    cout << "  ⚠️  问题 4: 长度字段大小\n";
    cout << "    uint16_t: 最大 64KB (适合小消息)\n";
    cout << "    uint32_t: 最大 4GB (适合大多数场景)\n";
    cout << "    uint64_t: 最大 ∞ (适合大文件)\n";
    cout << "    选 32-bit 最通用\n";
  }
}

// ============================================================
// 练习 7: shutdown() — 半关闭与优雅关闭
// ============================================================
//
// close() 的问题:
//   - close(fd) 立即关闭读写双向
//   - 如果有其他线程还在用这个 fd → 数据可能丢失
//   - close 后对端 recv 返回 0 (FIN)
//
// shutdown() 的优势:
//   - SHUT_RD   → 关闭读方向
//   - SHUT_WR   → 关闭写方向 (发送 FIN, 但仍可接收)
//   - SHUT_RDWR → 关闭双向 (同 close，但不会立即释放 fd)
//
// 半关闭 (half-close) 的含义:
//   shutdown(fd, SHUT_WR) → "我不再发送数据了，但我还能接收"
//   对端 recv 返回 0 → 对端知道你发完了
//   对端可以继续发送剩余数据

void exercise7_shutdown() {
  section("练习 7: shutdown() — 半关闭与优雅关闭");

  // TODO 7.1: 实验 — shutdown(SHUT_WR) 后的行为
  {
    subsection("实验: 半关闭 (SHUT_WR)");

    constexpr int PORT = 12356;

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

        // 1. 接收客户端数据
        char buf[256];
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
          buf[n] = '\0';
          cout << "  [Server] 收到: \"" << buf << "\"\n";
        }

        // 2. 发送响应
        send(client, "Response", 8, MSG_NOSIGNAL);

        // 3. shutdown(SHUT_WR) — 告诉客户端"我不再发了"
        shutdown(client, SHUT_WR);
        cout << "  [Server] shutdown(SHUT_WR) → 已发送 FIN\n";

        // 4. 但仍然可以接收!
        n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
          buf[n] = '\0';
          cout << "  [Server] shutdown 后仍可接收: \"" << buf << "\"\n";
        } else if (n == 0) {
          cout << "  [Server] 客户端也关闭了\n";
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 1. 发送数据
      send(fd, "Hello", 5, MSG_NOSIGNAL);

      // 2. 接收响应
      char buf[256];
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        cout << "  [Client] 收到: \"" << buf << "\"\n";
      }

      // 3. 检查服务器是否关闭了写端
      n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n == 0) {
        cout << "  [Client] recv 返回 0 — 服务器已 shutdown(SHUT_WR)\n";
      }

      // 4. 客户端还可以发送 (服务器端还在收)
      if (send(fd, "Final", 5, MSG_NOSIGNAL) > 0) {
        cout << "  [Client] shutdown(SHUT_WR) 后仍可发送 ✅\n";
      }
    }

    server.join();
  }

  // TODO 7.2: close() vs shutdown()
  {
    subsection("close() vs shutdown()");

    cout << "  ┌──────────────┬──────────────────────────────┐\n";
    cout << "  │ close(fd)    │ 关闭文件描述符               │\n";
    cout << "  │              │ - 减少引用计数               │\n";
    cout << "  │              │ - 计数到 0 才真正关闭 socket │\n";
    cout << "  │              │ - 关闭双向通信               │\n";
    cout << "  │              │ - dup/多线程共享 fd 会受影响 │\n";
    cout << "  ├──────────────┼──────────────────────────────┤\n";
    cout << "  │ shutdown()   │ 关闭 socket 的读/写方向      │\n";
    cout << "  │              │ - 不管引用计数               │\n";
    cout << "  │              │ - SHUT_WR: 发 FIN，仍可读    │\n";
    cout << "  │              │ - SHUT_RD: 丢弃接收缓冲数据  │\n";
    cout << "  │              │ - SHUT_RDWR: 同 close 语义   │\n";
    cout << "  └──────────────┴──────────────────────────────┘\n";
  }

  // TODO 7.3: 优雅关闭的正确姿势
  {
    subsection("优雅关闭的 4 步流程");

    cout << "  正确的 TCP 连接关闭流程:\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ // 客户端                                          │\n";
    cout << "  │ 1. shutdown(fd, SHUT_WR)   // 发 FIN, 不再写       │\n";
    cout << "  │ 2. while (recv() > 0)       // 继续读直到收到 FIN   │\n";
    cout << "  │    process(data);                                  │\n";
    cout << "  │ 3. // recv 返回 0 (服务器发了 FIN)                 │\n";
    cout << "  │ 4. close(fd);              // 最终关闭             │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
    cout << "\n  注: HTTP 通常用 Connection: close 请求优雅关闭\n";
  }
}

// ============================================================
// 练习 8: SO_LINGER — 控制 close() 行为
// ============================================================
//
// SO_LINGER 控制 close() 对还未发送的数据的处理方式。
//
// 默认行为 (l_onoff=0):
//   close() 立即返回，内核在后台尝试发送剩余数据。
//   如果对端已经不可达 → 数据静默丢失。
//
// SO_LINGER 启用 (l_onoff=1):
//   close() 会阻塞 l_linger 秒，尝试发送剩余数据。
//   l_linger=0: 发送 RST 而不是 FIN，丢弃所有数据。
//   l_linger>0: 最多等待 l_linger 秒，超时则发 RST。

void exercise8_so_linger() {
  section("练习 8: SO_LINGER — 控制 close() 行为");

  // TODO 8.1: SO_LINGER 的结构
  {
    subsection("linger 结构体");

    cout << "  struct linger {\n";
    cout << "    int l_onoff;  // 0=关闭, 1=开启\n";
    cout << "    int l_linger; // 延迟秒数\n";
    cout << "  };\n";
  }

  // TODO 8.2: 三种模式
  {
    subsection("三种模式对比");

    cout << "  模式 1: 默认 (l_onoff = 0)\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ close() 立即返回                                    │\n";
    cout << "  │ 内核在后台发送剩余数据 (优雅关闭)                    │\n";
    cout << "  │ 数据可能成功发送，也可能丢失 — 没有通知              │\n";
    cout << "  │ 适合: 大多数应用 (HTTP Server)                      │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n\n";

    cout << "  模式 2: 优雅等待 (l_onoff=1, l_linger=N>0)\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ close() 阻塞最多 N 秒                               │\n";
    cout << "  │ 在 N 秒内内核尝试发送剩余数据                        │\n";
    cout << "  │ 超时 → 返回 -1, errno=EWOULDBLOCK, 发 RST          │\n";
    cout << "  │ 适合: 必须确认数据已发送的场景                      │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n\n";

    cout << "  模式 3: 立即中止 (l_onoff=1, l_linger=0)\n";
    cout << "  ┌─────────────────────────────────────────────────────┐\n";
    cout << "  │ close() 立即发送 RST (不是 FIN!)                     │\n";
    cout << "  │ 丢弃所有未发送的数据和接收缓冲区的数据               │\n";
    cout << "  │ 对端 recv 返回 ECONNRESET                            │\n";
    cout << "  │ 对端知道这不是正常关闭                                │\n";
    cout << "  │ 适合: 异常断开 (已知有 bug 需要强制重置)             │\n";
    cout << "  └─────────────────────────────────────────────────────┘\n";
  }

  // TODO 8.3: TIME_WAIT 状态与 SO_LINGER
  {
    subsection("TIME_WAIT 与 SO_LINGER");

    cout << "  主动关闭方进入 TIME_WAIT 状态 (持续 2MSL ≈ 60-120 秒)\n";
    cout << "  目的: 确保最后的 ACK 到达，让旧连接的残留包在网络中消失\n\n";

    cout << "  SO_LINGER (l_onoff=1, l_linger=0) 发 RST:\n";
    cout << "    - 跳过 TIME_WAIT 状态\n";
    cout << "    - 可能让旧连接的残留包被新连接误收\n";
    cout << "    - ⚠️ 不推荐这样做！\n\n";

    cout << "  正确减少 TIME_WAIT 的方法:\n";
    cout << "    - SO_REUSEADDR (允许重用地址，不影响 TIME_WAIT)\n";
    cout << "    - 让客户端主动关闭 (TIME_WAIT 在客户端)\n";
    cout << "    - 调整系统参数: net.ipv4.tcp_tw_reuse\n";
  }

  // TODO 8.4: 设置 SO_LINGER
  {
    subsection("代码示例");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      ScopedFd guard(fd);

      struct linger ling;
      ling.l_onoff = 1;  // 启用
      ling.l_linger = 5; // 等待 5 秒

      setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
      cout << "  ✅ SO_LINGER 已设置: 5 秒优雅等待\n";
    }

    cout << "\n  💡 生产环境建议:\n";
    cout << "    - 大多数场景: 使用默认行为 (l_onoff=0)\n";
    cout << "    - 需要确认送达: 应用层 ACK，不要依赖 SO_LINGER\n";
    cout << "    - 需要快速重置: 用 shutdown() + close()，不要 RST\n";
  }
}

// ============================================================
// 练习 9: TCP 状态机
// ============================================================
//
// TCP 连接有 11 种状态 (netstat -tan 可以看到):
//
// 客户端视角: CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1
//              → FIN_WAIT_2 → TIME_WAIT → CLOSED
//
// 服务器视角: CLOSED → LISTEN → SYN_RCVD → ESTABLISHED
//              → CLOSE_WAIT → LAST_ACK → CLOSED
//
// 理解状态机对调试网络问题非常重要。

void exercise9_tcp_state_machine() {
  section("练习 9: TCP 状态机");

  // TODO 9.1: TCP 11 种状态
  {
    subsection("TCP 11 种状态");

    cout << "  ┌──────────────┬──────────────────────────────────────┐\n";
    cout << "  │ CLOSED       │ 初始/最终状态, 连接不存在            │\n";
    cout << "  │ LISTEN       │ 服务器等待连接 (被动打开)             │\n";
    cout << "  │ SYN_SENT     │ 客户端发了 SYN, 等待 SYN+ACK         │\n";
    cout << "  │ SYN_RCVD     │ 服务器收到 SYN, 发了 SYN+ACK         │\n";
    cout << "  │ ESTABLISHED  │ 连接已建立, 数据传输中               │\n";
    cout << "  │ FIN_WAIT_1   │ 主动关闭方: 已发 FIN, 等 ACK         │\n";
    cout << "  │ FIN_WAIT_2   │ 主动关闭方: 收到 ACK, 等对端 FIN     │\n";
    cout << "  │ CLOSE_WAIT   │ 被动关闭方: 收到 FIN, 发了 ACK       │\n";
    cout << "  │ CLOSING      │ 双方同时关闭 (罕见)                  │\n";
    cout << "  │ LAST_ACK     │ 被动关闭方: 发了 FIN, 等最后 ACK     │\n";
    cout << "  │ TIME_WAIT    │ 主动关闭方: 等 2MSL 确保旧包消失     │\n";
    cout << "  └──────────────┴──────────────────────────────────────┘\n";
  }

  // TODO 9.2: 三次握手状态转换
  {
    subsection("三次握手 — 状态转换");

    cout << "  客户端                    服务器\n";
    cout << "  CLOSED                    CLOSED\n";
    cout << "    │                          │\n";
    cout << "    │ socket()                 │ socket() + bind() + listen()\n";
    cout << "    ▼                          ▼\n";
    cout << "  (准备连接)                 LISTEN\n";
    cout << "    │                          │\n";
    cout << "    │ connect() ──SYN──▶       │\n";
    cout << "    ▼                          ▼\n";
    cout << "  SYN_SENT                  SYN_RCVD\n";
    cout << "    │         ◀──SYN+ACK──     │\n";
    cout << "    │                          │\n";
    cout << "    │         ──ACK──▶         │\n";
    cout << "    ▼                          ▼\n";
    cout << "  ESTABLISHED ◀══════▶ ESTABLISHED\n\n";

    cout << "  💡 connect() 阻塞直到 ESTABLISHED (或失败)\n";
    cout << "  💡 accept() 阻塞直到三次握手完成, 从 SYN queue 取出\n";
  }

  // TODO 9.3: 四次挥手状态转换
  {
    subsection("四次挥手 — 状态转换");

    cout << "  主动关闭方 (Client)         被动关闭方 (Server)\n";
    cout << "  ESTABLISHED                ESTABLISHED\n";
    cout << "    │                           │\n";
    cout << "    │ close() ──FIN──▶          │\n";
    cout << "    ▼                           ▼\n";
    cout << "  FIN_WAIT_1                 CLOSE_WAIT\n";
    cout << "    │         ◀──ACK──          │ (recv 返回 0)\n";
    cout << "    ▼                           │\n";
    cout << "  FIN_WAIT_2                    │ (应用处理剩余数据)\n";
    cout << "    │                           │ close()\n";
    cout << "    │         ◀──FIN──          ▼\n";
    cout << "    │                        LAST_ACK\n";
    cout << "    │         ──ACK──▶          │\n";
    cout << "    ▼                           ▼\n";
    cout << "  TIME_WAIT                  CLOSED\n";
    cout << "  (等 2MSL)\n";
    cout << "    ▼\n";
    cout << "  CLOSED\n\n";

    cout << "  ⚠️  CLOSE_WAIT 堆积是常见的服务器 bug:\n";
    cout << "    对端发了 FIN，服务器 recv 返回 0\n";
    cout << "    但服务器没有调用 close() → 永远卡在 CLOSE_WAIT\n";
    cout << "    查看: netstat -tan | grep CLOSE_WAIT\n";
  }

  // TODO 9.4: 常见问题诊断
  {
    subsection("用 netstat/ss 诊断 TCP 状态问题");

    cout << "  大量 SYN_RCVD:\n";
    cout << "    → 可能是 SYN flood 攻击\n";
    cout << "    → 也可能是 backlog 太小, 合法连接进不来\n\n";

    cout << "  大量 TIME_WAIT:\n";
    cout << "    → 短连接太多 (高并发 HTTP/1.0)\n";
    cout << "    → 解决方案: 长连接, 连接池, HTTP Keep-Alive\n";
    cout << "    → 调系统参数: tcp_tw_reuse, tcp_max_tw_buckets\n\n";

    cout << "  大量 CLOSE_WAIT:\n";
    cout << "    → 服务器 bug: 收到 FIN 后没有 close()\n";
    cout << "    → 检查代码: recv()==0 后是否正确关闭连接\n\n";

    cout << "  连接卡在 FIN_WAIT_2:\n";
    cout << "    → 对端没有发 FIN (可能忘了 close)\n";
    cout << "    → Linux 有 tcp_fin_timeout 参数 (默认 60s)\n";
  }

  // TODO 9.5: 查看本机 TCP 连接状态
  {
    subsection("查看本机 TCP 状态");

    cout << "  常用命令:\n";
    cout << "    ss -tan          # 查看所有 TCP 连接 (推荐, 比 netstat 快)\n";
    cout << "    ss -tan state time-wait | wc -l  # 统计 TIME_WAIT\n";
    cout << "    ss -tan state close-wait         # 找出 CLOSE_WAIT 泄漏\n";
    cout << "    watch -n1 'ss -tan | head -20'   # 实时监控\n";
  }
}

// ============================================================
// 练习 10: 实战 — 带消息边界的可靠 Echo 协议
// ============================================================
//
// 综合运用本周知识:
//   - 长度前缀协议 (练习 6)
//   - TCP_NODELAY (练习 1) — 低延迟
//   - KeepAlive (练习 2) — 检测死连接
//   - 优雅关闭 (练习 7) — shutdown
//
// 协议格式: [4 字节长度] [数据]
// 服务器 echo 回客户端发来的每条消息。

constexpr int ECHO_PORT = 12357;
constexpr uint32_t MAX_MSG_SIZE = 1024 * 1024; // 1MB 上限

// 精确读取 N 字节 (处理拆包)
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

// 发送一条长度前缀消息
bool send_message(int fd, const void *data, uint32_t len) {
  uint32_t net_len = htonl(len);
  // 发送长度
  if (send(fd, &net_len, sizeof(net_len), MSG_NOSIGNAL) != sizeof(net_len))
    return false;
  // 发送数据
  if (send(fd, data, len, MSG_NOSIGNAL) != static_cast<ssize_t>(len))
    return false;
  return true;
}

// 接收一条长度前缀消息
// 返回接收到的数据, 连接关闭返回 nullopt
std::optional<vector<char>> recv_message(int fd) {
  // 1. 读取 4 字节长度
  uint32_t net_len;
  if (!recv_exact(fd, &net_len, sizeof(net_len)))
    return std::nullopt;
  uint32_t len = ntohl(net_len);

  // 2. 安全校验
  if (len > MAX_MSG_SIZE) {
    std::cerr << "  ❌ 消息长度超出限制: " << len << " > " << MAX_MSG_SIZE
              << "\n";
    return std::nullopt;
  }

  // 3. 读取数据
  vector<char> data(len + 1);
  if (!recv_exact(fd, data.data(), len))
    return std::nullopt;
  data[len] = '\0';
  return data;
}

// 配置 socket 选项
void configure_socket(int fd) {
  // TCP_NODELAY: 立即发送，不等 Nagle
  int optval = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

  // KeepAlive: 60 秒空闲 + 10 秒间隔 × 3 次
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
  optval = 60;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &optval, sizeof(optval));
  optval = 10;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &optval, sizeof(optval));
  optval = 3;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &optval, sizeof(optval));
}

void run_pro_echo_server() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return;
  ScopedFd guard(fd);

  int optval = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ECHO_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;
  bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  listen(fd, 1);

  cout << "  [ProEcho Server] 监听 0.0.0.0:" << ECHO_PORT << "\n";

  int client = accept(fd, nullptr, nullptr);
  if (client < 0)
    return;
  ScopedFd cg(client);

  configure_socket(client);
  cout << "  [ProEcho Server] 客户端已连接, socket 已配置\n";

  // 循环接收消息并 echo
  int count = 0;
  while (count < 5) {
    auto msg = recv_message(client);
    if (!msg.has_value()) {
      cout << "  [ProEcho Server] 连接关闭或出错\n";
      break;
    }
    cout << "  [ProEcho Server] 收到: \"" << msg->data() << "\" ("
         << msg->size() - 1 << " 字节)\n";

    // Echo 回去
    send_message(client, msg->data(), msg->size() - 1);
    cout << "  [ProEcho Server] 已 echo\n";
    ++count;
  }

  // 优雅关闭
  shutdown(client, SHUT_WR);
  cout << "  [ProEcho Server] shutdown(SHUT_WR), 等待客户端关闭\n";
}

void run_pro_echo_client() {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return;
  ScopedFd guard(fd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ECHO_PORT);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

  configure_socket(fd);
  cout << "  [ProEcho Client] 已连接\n";

  // 发送几条消息
  const char *messages[] = {"Ping", "Hello TCP Deep Dive!",
                            "长度前缀协议测试", "短", "再见 👋"};
  for (const auto *msg : messages) {
    cout << "  [ProEcho Client] 发送: \"" << msg << "\"\n";
    send_message(fd, msg, strlen(msg));

    auto resp = recv_message(fd);
    if (resp.has_value()) {
      cout << "  [ProEcho Client] 收到: \"" << resp->data() << "\"\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  // 客户端主动发起优雅关闭
  shutdown(fd, SHUT_WR);
  cout << "  [ProEcho Client] shutdown(SHUT_WR)\n";
}

void exercise10_pro_echo() {
  section("练习 10: 实战 — 带消息边界的可靠 Echo 协议");

  std::thread server(run_pro_echo_server);
  std::thread client(run_pro_echo_client);

  client.join();
  server.join();

  cout << "\n  ✅ ProEcho 测试完成\n\n";
  cout << "  📋 本练习涵盖了 Week 12 的核心知识点:\n";
  cout << "    1. ✅ 长度前缀协议 — 解决粘包/拆包\n";
  cout << "    2. ✅ TCP_NODELAY — 低延迟发送\n";
  cout << "    3. ✅ KeepAlive — 死连接检测\n";
  cout << "    4. ✅ shutdown() — 优雅关闭\n";
  cout << "    5. ✅ recv_exact — 处理拆包的通用模式\n";
  cout << "    6. ✅ 最大长度限制 — 防止恶意报文\n";
}

// ============================================================
// main — 运行所有练习
// ============================================================

int main(int argc, char *argv[]) {
  cout << "Week 12: TCP 深入 — Nagle / KeepAlive / 粘包 / 优雅关闭\n";
  cout << "==============================================================\n";

  if (argc > 1) {
    string mode = argv[1];
    if (mode == "server") {
      run_pro_echo_server();
      return 0;
    }
    if (mode == "client") {
      run_pro_echo_client();
      return 0;
    }
    cout << "用法: " << argv[0] << " [server|client]\n";
    return 1;
  }

  // 默认运行所有练习
  exercise1_nagle();
  exercise2_keepalive();
  exercise3_sticky_packet();
  exercise4_fixed_length();
  exercise5_delimiter();
  exercise6_length_prefix();
  exercise7_shutdown();
  exercise8_so_linger();
  exercise9_tcp_state_machine();
  exercise10_pro_echo();

  cout << "\n✅ Week 12 全部练习完成！\n";
  cout << "\n📝 Week 12 总结要点:\n";
  cout << "  1. Nagle 算法合并小包 → 高吞吐; TCP_NODELAY 禁用它 → 低延迟\n";
  cout << "  2. KeepAlive 默认 2h 空闲, 应用层心跳才是真正可靠的检测手段\n";
  cout << "  3. TCP 是字节流, 不保留消息边界 → 必须应用层解决粘包/拆包\n";
  cout << "  4. 长度前缀协议: [4字节长度][数据] — 最通用的解决方案\n";
  cout << "  5. 接收端必须循环 recv() 直到读够指定字节数 (recv_exact 模式)\n";
  cout << "  6. shutdown(SHUT_WR) 发送 FIN 后仍可接收 → 实现半关闭\n";
  cout << "  7. SO_LINGER 控制 close 行为: 默认(优雅), 等待, 或立即 RST\n";
  cout << "  8. TCP 11 种状态: CLOSE_WAIT 泄漏是常见 bug, TIME_WAIT 是正常行为\n";
  cout << "  9. set TCP_NODELAY + KeepAlive 是每个网络应用的标配\n";
  cout << "  10. 恶意报文防御: 消息长度上限检查, 防止内存耗尽\n";

  return 0;
}
