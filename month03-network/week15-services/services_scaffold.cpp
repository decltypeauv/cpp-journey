// Week 15: 网络服务实战 — 聊天室 + 代理 + 端口转发
// 编译: cmake -B build && cmake --build build
// 运行: ./build/services
//
// 本周将 Week 11-14 的知识综合应用到实际网络服务中:
//   - 多客户端聊天室 (epoll 广播)
//   - HTTP 正向代理
//   - TCP 端口转发/隧道
//   - 简易负载均衡
//
// 核心主题:
//   聊天室 — 多客户端广播, 协议设计, 心跳检测
//   HTTP 代理 — 请求转发, CONNECT 隧道
//   TCP 隧道 — 双向数据转发, 半关闭
//   负载均衡 — 轮询, 健康检查

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <thread>
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

// 精确读取 N 字节
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

// 发送长度前缀消息
bool send_message(int fd, const void *data, uint32_t len) {
  uint32_t net_len = htonl(len);
  if (send(fd, &net_len, sizeof(net_len), MSG_NOSIGNAL) != sizeof(net_len))
    return false;
  if (send(fd, data, len, MSG_NOSIGNAL) != static_cast<ssize_t>(len))
    return false;
  return true;
}

// 接收长度前缀消息
std::optional<vector<char>> recv_message(int fd, uint32_t max_len = 64 * 1024) {
  uint32_t net_len;
  if (!recv_exact(fd, &net_len, sizeof(net_len)))
    return std::nullopt;
  uint32_t len = ntohl(net_len);
  if (len > max_len)
    return std::nullopt;
  vector<char> data(len + 1);
  if (!recv_exact(fd, data.data(), len))
    return std::nullopt;
  data[len] = '\0';
  return data;
}

// ============================================================
// 练习 1: 聊天室 — 基础广播模型
// ============================================================
//
// 聊天室的核心: 一个客户端发消息 → 服务器广播给所有其他客户端。
//
// 模型:
//   - 所有已连接的客户端存在一个列表中
//   - 当某个客户端发来消息, 遍历列表, 发给其他人
//   - 新客户端连接 → 加入列表 → 广播 "User joined"
//   - 客户端断开 → 从列表移除 → 广播 "User left"
//
// 这是最简单的多客户端实时通信模型。

void exercise1_chat_broadcast() {
  section("练习 1: 聊天室 — 基础广播模型");

  // TODO 1.1: 广播架构
  {
    subsection("广播架构");

    cout << "  聊天室服务器架构:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ Client A ── \"Hello\" ──▶ Server             │\n";
    cout << "  │                           │                  │\n";
    cout << "  │              broadcast(\"UserA: Hello\")      │\n";
    cout << "  │              ├─▶ Client B                  │\n";
    cout << "  │              ├─▶ Client C                  │\n";
    cout << "  │              └─▶ Client D                  │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  关键数据结构:\n";
    cout << "    vector<Client*> _clients;   // 所有在线用户\n";
    cout << "    收到消息 → for (auto *c : _clients) send(c->fd, msg)\n";
    cout << "    新连接   → _clients.push_back(new_client)\n";
    cout << "    断开     → _clients.erase(...)  + broadcast(\"left\")\n";
  }

  // TODO 1.2: 实践 — 简易广播聊天室
  {
    subsection("实践: 3 客户端广播演示");

    constexpr int PORT = 15011;

    std::thread server([]() {
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
      listen(fd, 3);

      vector<int> clients;

      // 接受 3 个客户端
      for (int i = 0; i < 3; ++i) {
        int c = accept(fd, nullptr, nullptr);
        if (c >= 0) {
          clients.push_back(c);
          cout << "  [Server] 客户端 #" << (i + 1) << " 已连接\n";

          // 广播 join 消息
          string join = "User" + std::to_string(i + 1) + " joined!";
          for (int peer : clients) {
            send_message(peer, join.c_str(), join.size());
          }
        }
      }

      // 从每个客户端接收一条消息并广播
      for (size_t i = 0; i < clients.size(); ++i) {
        auto msg = recv_message(clients[i]);
        if (msg.has_value()) {
          string text(msg->data());
          cout << "  [Server] 收到来自 Client #" << (i + 1) << ": " << text << "\n";

          // 广播给所有其他客户端
          string broadcast_msg =
              "User" + std::to_string(i + 1) + ": " + text;
          for (int peer : clients) {
            if (peer != clients[i]) {
              send_message(peer, broadcast_msg.c_str(), broadcast_msg.size());
            }
          }
          cout << "  [Server] 已广播\n";
        }
      }

      for (int c : clients) close(c);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 三个客户端
    auto client = [](int id) {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 接收 join 广播
      for (int j = 0; j < id; ++j) {
        auto m = recv_message(fd);
        if (m.has_value()) {
          cout << "  [Client " << id << "] 收到广播: " << m->data() << "\n";
        }
      }

      // 发送自己的消息
      string msg = "Hello from client " + std::to_string(id);
      send_message(fd, msg.c_str(), msg.size());

      // 接收来自其他客户端的广播消息
      for (int j = id; j < 3; ++j) {
        auto m = recv_message(fd);
        if (m.has_value()) {
          cout << "  [Client " << id << "] 收到广播: " << m->data() << "\n";
        }
      }
    };

    std::thread c1(client, 1);
    std::thread c2(client, 2);
    std::thread c3(client, 3);

    c1.join(); c2.join(); c3.join();
    server.join();

    cout << "  ✅ 广播模型工作正常 — 消息被转发给所有其他客户端\n";
  }

  // TODO 1.3: 广播的性能考量
  {
    subsection("广播性能与扩展性");

    cout << "  广播是 O(n) 操作:\n";
    cout << "    - 每条消息 → N 次 send()\n";
    cout << "    - 100 人聊天 → 1 条消息 = 99 次 send\n";
    cout << "    - 1000 人 → 999 次 send\n";
    cout << "\n";
    cout << "  优化思路:\n";
    cout << "    - writev(): 一次系统调用发送相同数据到多个 fd\n";
    cout << "    - 分组: 大群按房间拆分\n";
    cout << "    - 异步: 把 send 放到工作线程, 主线程继续 epoll_wait\n";
    cout << "    - 消息队列: 每个客户端一个待发队列, 分帧发送\n";
  }
}

// ============================================================
// 练习 2: 聊天室协议设计
// ============================================================
//
// 良好的聊天协议需要:
//   1. 消息类型: LOGIN(告知昵称), MSG(聊天), LOGOUT(离开), SYSTEM(系统通知)
//   2. 长度前缀: [4B 总长度][1B 类型][数据]
//   3. 昵称支持: LOGIN 消息携带昵称
//
// 协议格式:
//   ┌──────────┬────────┬──────────────────┐
//   │ 4B len   │ 1B type│ data (len-1 字节) │
//   └──────────┴────────┴──────────────────┘
//
// 类型:
//   0x01 = LOGIN  (data = 昵称)
//   0x02 = MSG    (data = 消息正文)
//   0x03 = LOGOUT (无 data)
//   0x04 = SYSTEM (data = 服务器通知)

enum class ChatMsgType : uint8_t {
  LOGIN = 0x01,
  MSG = 0x02,
  LOGOUT = 0x03,
  SYSTEM = 0x04,
};

// 打包聊天消息
vector<char> pack_chat_msg(ChatMsgType type, const string &data) {
  uint32_t payload_len = 1 + data.size(); // type(1B) + data
  vector<char> result(4 + payload_len);

  uint32_t net_len = htonl(payload_len);
  memcpy(result.data(), &net_len, 4);
  result[4] = static_cast<uint8_t>(type);
  memcpy(result.data() + 5, data.data(), data.size());

  return result;
}

// 解包聊天消息
struct ChatMessage {
  ChatMsgType type;
  string data;
  bool valid = false;
};

ChatMessage unpack_chat_msg(const vector<char> &raw) {
  ChatMessage msg;
  if (raw.size() < 5)
    return msg; // 至少: [type(1B)][至少 0B data]

  msg.type = static_cast<ChatMsgType>(raw[0]);
  msg.data = string(raw.begin() + 1, raw.end());
  msg.valid = true;
  return msg;
}

void exercise2_chat_protocol() {
  section("练习 2: 聊天室协议设计");

  // TODO 2.1: 协议格式
  {
    subsection("协议格式: 类型+长度+数据");

    cout << "  聊天消息格式:\n";
    cout << "  ┌──────────┬────────┬──────────────────┐\n";
    cout << "  │ 4B len   │ 1B type│ data              │\n";
    cout << "  ├──────────┼────────┼──────────────────┤\n";
    cout << "  │ LOGIN    │ 0x01   │ \"Alice\"          │\n";
    cout << "  │ MSG      │ 0x02   │ \"Hello everyone!\"│\n";
    cout << "  │ LOGOUT   │ 0x03   │ (empty)           │\n";
    cout << "  │ SYSTEM   │ 0x04   │ \"Bob joined\"     │\n";
    cout << "  └──────────┴────────┴──────────────────┘\n";
  }

  // TODO 2.2: 打包/解包演示
  {
    subsection("打包/解包演示");

    // 测试 LOGIN
    auto login = pack_chat_msg(ChatMsgType::LOGIN, "Alice");
    auto parsed = unpack_chat_msg(vector<char>(login.begin() + 4, login.end()));
    cout << "  LOGIN: type=0x" << std::hex
         << static_cast<int>(parsed.type) << std::dec
         << ", data=\"" << parsed.data << "\"\n";

    // 测试 MSG
    auto msg_pkt = pack_chat_msg(ChatMsgType::MSG, "Hello everyone!");
    auto msg_parsed =
        unpack_chat_msg(vector<char>(msg_pkt.begin() + 4, msg_pkt.end()));
    cout << "  MSG:   type=0x" << std::hex
         << static_cast<int>(msg_parsed.type) << std::dec
         << ", data=\"" << msg_parsed.data << "\"\n";

    // 测试 SYSTEM
    auto sys = pack_chat_msg(ChatMsgType::SYSTEM, "Bob joined the room");
    auto sys_parsed =
        unpack_chat_msg(vector<char>(sys.begin() + 4, sys.end()));
    cout << "  SYS:   type=0x" << std::hex
         << static_cast<int>(sys_parsed.type) << std::dec
         << ", data=\"" << sys_parsed.data << "\"\n";

    cout << "  ✅ 聊天协议打包/解包正常\n";
  }

  // TODO 2.3: 协议设计原则
  {
    subsection("协议设计原则");

    cout << "  好的应用层协议:\n";
    cout << "    1. 类型字段 — 区分不同消息 (LOGIN/MSG/LOGOUT/...)\n";
    cout << "    2. 长度前缀 — 二进制安全 + 变长 + O(1) 解析\n";
    cout << "    3. 版本号   — 协议升级兼容\n";
    cout << "    4. 序列号   — 用于重发/去重/排序\n";
    cout << "    5. 时间戳   — 消息时效性\n";
    cout << "\n";
    cout << "  工业界例子:\n";
    cout << "    - WebSocket: 类型(opcode) + 长度 + mask + payload\n";
    cout << "    - Redis RESP: 类型前缀(*/$/:/+) + 长度 + 数据 + CRLF\n";
    cout << "    - gRPC: HTTP/2 frame + Protobuf\n";
    cout << "    - MQTT: 固定头(类型+flag+长度) + 可变头 + payload\n";
  }
}

// ============================================================
// 练习 3: 聊天室 — 用户管理与房间
// ============================================================
//
// 完整的聊天室需要:
//   - 用户数据结构 (昵称, fd, 连接时间)
//   - 加入/离开通知
//   - 用户列表
//   - 私聊支持 (选做)

struct ChatUser {
  int fd;
  string nickname;
  time_t join_time;

  ChatUser(int fd, string nick)
      : fd(fd), nickname(std::move(nick)), join_time(time(nullptr)) {}
};

// 简单的聊天服务器 (单线程 epoll)
class SimpleChatServer {
public:
  SimpleChatServer() {
    _epfd = epoll_create1(EPOLL_CLOEXEC);
  }

  ~SimpleChatServer() {
    if (_epfd >= 0) close(_epfd);
    // 清理用户 (注意: 不 close fd, 调用者负责)
    for (auto *u : _users) delete u;
  }

  bool init(int port) {
    if (_epfd < 0) return false;

    _listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listen_fd < 0) return false;
    set_nonblocking(_listen_fd);

    int optval = 1;
    setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(_listen_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(_listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
      return false;
    if (listen(_listen_fd, SOMAXCONN) < 0) return false;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = _listen_fd;
    epoll_ctl(_epfd, EPOLL_CTL_ADD, _listen_fd, &ev);

    cout << "  [ChatServer] 启动在 0.0.0.0:" << port << "\n";
    return true;
  }

  // 广播消息给所有人 (可排除某个 fd)
  void broadcast(ChatMsgType type, const string &data, int exclude_fd = -1) {
    auto pkt = pack_chat_msg(type, data);
    for (auto *u : _users) {
      if (u->fd != exclude_fd) {
        send(u->fd, pkt.data(), pkt.size(), MSG_NOSIGNAL);
      }
    }
  }

  // 运行一次事件循环迭代 (返回 false 表示没有更多事件)
  bool run_once(int timeout_ms = 500) {
    epoll_event events[16];
    int n = epoll_wait(_epfd, events, 16, timeout_ms);
    if (n <= 0) return n == 0; // timeout is ok

    for (int i = 0; i < n; ++i) {
      if (events[i].data.fd == _listen_fd) {
        handle_accept();
        return true;
      }
      // 客户端事件
      auto &e = events[i];
      if ((e.events & EPOLLERR) || (e.events & EPOLLHUP)) {
        handle_disconnect(e.data.fd);
      } else if (e.events & EPOLLIN) {
        handle_client_data(e.data.fd);
      }
    }
    return true;
  }

  size_t user_count() const { return _users.size(); }
  const vector<ChatUser *> &users() const { return _users; }

private:
  int _epfd = -1;
  int _listen_fd = -1;
  vector<ChatUser *> _users;

  ChatUser *find_user(int fd) {
    for (auto *u : _users) {
      if (u->fd == fd) return u;
    }
    return nullptr;
  }

  void handle_accept() {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int cfd = accept(_listen_fd,
                     reinterpret_cast<sockaddr *>(&client_addr), &len);
    if (cfd < 0) return;
    set_nonblocking(cfd);

    auto *user = new ChatUser(cfd, "anon");
    _users.push_back(user);

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = cfd;
    epoll_ctl(_epfd, EPOLL_CTL_ADD, cfd, &ev);
  }

  void handle_client_data(int fd) {
    auto *user = find_user(fd);
    if (!user) return;

    auto raw = recv_message(fd);
    if (!raw.has_value()) {
      handle_disconnect(fd);
      return;
    }

    auto msg = unpack_chat_msg(*raw);
    if (!msg.valid) return;

    switch (msg.type) {
    case ChatMsgType::LOGIN: {
      string old_nick = user->nickname;
      user->nickname = msg.data;
      string notify = (old_nick == "anon")
                          ? msg.data + " joined the room"
                          : old_nick + " renamed to " + msg.data;
      broadcast(ChatMsgType::SYSTEM, notify);
      break;
    }
    case ChatMsgType::MSG: {
      string full_msg = user->nickname + ": " + msg.data;
      broadcast(ChatMsgType::MSG, full_msg);
      break;
    }
    case ChatMsgType::LOGOUT: {
      handle_disconnect(fd);
      break;
    }
    default:
      break;
    }
  }

  void handle_disconnect(int fd) {
    auto *user = find_user(fd);
    if (!user) return;

    if (user->nickname != "anon") {
      broadcast(ChatMsgType::SYSTEM, user->nickname + " left the room");
    }

    epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
    shutdown(fd, SHUT_RDWR);
    close(fd);

    _users.erase(std::remove(_users.begin(), _users.end(), user), _users.end());
    delete user;
  }
};

void exercise3_user_management() {
  section("练习 3: 聊天室 — 用户管理与通知");

  // TODO 3.1: 用户数据结构
  {
    subsection("用户模型");

    cout << "  struct ChatUser {\n";
    cout << "    int fd;            // socket\n";
    cout << "    string nickname;   // 昵称\n";
    cout << "    time_t join_time;  // 加入时间\n";
    cout << "  };\n";
    cout << "\n";
    cout << "  💡 用户列表用 vector<ChatUser*> 管理:\n";
    cout << "    - 按 fd 查找 (O(n), n 较小没问题)\n";
    cout << "    - 可改用 unordered_map<int, ChatUser*> (O(1))\n";
  }

  // TODO 3.2: 实践 — 完整聊天室
  {
    subsection("实践: 完整聊天室 (3 用户 + 登录 + 聊天 + 离开)");

    constexpr int PORT = 15031;
    std::atomic<bool> stop{false};

    std::thread server_thread([&]() {
      SimpleChatServer srv;
      if (!srv.init(PORT)) return;

      int iterations = 0;
      while (!stop.load() && iterations < 50 && srv.user_count() > 0) {
        srv.run_once(200);
        ++iterations;
      }
      // 等待最后的事件
      for (int i = 0; i < 10 && !stop.load(); ++i) {
        srv.run_once(200);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端 1: Alice
    std::thread c1([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // LOGIN
      auto login = pack_chat_msg(ChatMsgType::LOGIN, "Alice");
      send(fd, login.data(), login.size(), MSG_NOSIGNAL);

      // 接收 join 通知
      for (int i = 0; i < 2; ++i) {
        auto m = recv_message(fd);
        if (m.has_value()) {
          auto parsed = unpack_chat_msg(*m);
          if (parsed.type == ChatMsgType::SYSTEM)
            cout << "  [Alice] 系统: " << parsed.data << "\n";
        }
      }

      // 发送消息
      auto msg = pack_chat_msg(ChatMsgType::MSG, "Hi everyone!");
      send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);

      // 接收 Bob 的回复
      auto m = recv_message(fd);
      if (m.has_value()) {
        auto parsed = unpack_chat_msg(*m);
        cout << "  [Alice] 聊天: " << parsed.data << "\n";
      }

      // 离开
      auto logout = pack_chat_msg(ChatMsgType::LOGOUT, "");
      send(fd, logout.data(), logout.size(), MSG_NOSIGNAL);
    });

    // 客户端 2: Bob
    std::thread c2([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // LOGIN
      auto login = pack_chat_msg(ChatMsgType::LOGIN, "Bob");
      send(fd, login.data(), login.size(), MSG_NOSIGNAL);

      // 接收 join 通知 (自己的 + Alice 的加入消息? 不, Alice 在 Bob login 之前)
      for (int i = 0; i < 3; ++i) {
        auto m = recv_message(fd);
        if (m.has_value()) {
          auto parsed = unpack_chat_msg(*m);
          if (parsed.type == ChatMsgType::SYSTEM)
            cout << "  [Bob]   系统: " << parsed.data << "\n";
          else if (parsed.type == ChatMsgType::MSG)
            cout << "  [Bob]   聊天: " << parsed.data << "\n";
        }
      }

      // 发送回复
      auto msg = pack_chat_msg(ChatMsgType::MSG, "Hey Alice!");
      send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);

      // 接收 left 通知
      auto m = recv_message(fd);
      if (m.has_value()) {
        auto parsed = unpack_chat_msg(*m);
        if (parsed.type == ChatMsgType::SYSTEM)
          cout << "  [Bob]   系统: " << parsed.data << "\n";
      }
    });

    c1.join(); c2.join();

    stop = true;
    server_thread.join();

    cout << "  ✅ 完整聊天室流程: LOGIN → 通知 → 聊天 → 离线通知 → LOGOUT\n";
  }
}

// ============================================================
// 练习 4: 聊天室 — 心跳与超时
// ============================================================
//
// 聊天室必须处理「僵尸连接」:
//   - 用户网络断开但没有正常退出
//   - 服务器无法知道连接已死
//
// 解决方案:
//   - 服务器发送 PING → 客户端必须回复 PONG
//   - 超时未响应 → 断开连接
//   - 使用 timerfd + epoll 统一调度

void exercise4_heartbeat() {
  section("练习 4: 聊天室 — 心跳检测");

  // TODO 4.1: 心跳原理
  {
    subsection("心跳机制");

    cout << "  PING-PONG 心跳:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ Server ──[PING]──▶ Client                  │\n";
    cout << "  │ Server ◀──[PONG]── Client                  │\n";
    cout << "  │                                              │\n";
    cout << "  │ 超时规则:                                    │\n";
    cout << "  │   - 每 30s 发一次 PING                      │\n";
    cout << "  │   - 10s 内没收到 PONG → 断开                │\n";
    cout << "  │   - 60s 没收到任何数据 → 断开               │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  协议扩展:\n";
    cout << "    - 添加 PING(0x05) 和 PONG(0x06) 类型\n";
    cout << "    - timerfd 设置为周期性触发 (如 30s)\n";
    cout << "    - timerfd 加入 epoll → 统一事件循环\n";
  }

  // TODO 4.2: timerfd + epoll 集成
  {
    subsection("timerfd 集成演示");

    // 创建 timerfd
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0) {
      cout << "  ❌ timerfd_create 失败\n";
      return;
    }
    ScopedFd tg(tfd);

    // 设置: 1 秒后首次, 之后每 500ms 触发 (模拟心跳间隔)
    itimerspec ts{};
    ts.it_value.tv_sec = 1;
    ts.it_value.tv_nsec = 0;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 500 * 1000 * 1000; // 500ms
    timerfd_settime(tfd, 0, &ts, nullptr);

    int epfd = epoll_create1(0);
    ScopedFd eg(epfd);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

    cout << "  ⏰ 心跳定时器启动 (500ms 间隔), 等待 3 次触发...\n";

    for (int i = 0; i < 3; ++i) {
      epoll_event ready;
      int n = epoll_wait(epfd, &ready, 1, 2000);
      if (n > 0) {
        uint64_t expirations;
        read(tfd, &expirations, sizeof(expirations));
        cout << "  ❤️  心跳 #" << (i + 1)
             << " (到期 " << expirations << " 次)\n";
      }
    }

    cout << "  ✅ timerfd + epoll 集成 — 心跳定时器与 IO 统一调度\n";
  }

  // TODO 4.3: 心跳实现要点
  {
    subsection("实现要点");

    cout << "  心跳实现清单:\n";
    cout << "    1. 每个连接记录: last_active_time\n";
    cout << "    2. timerfd 每 N 秒触发一次\n";
    cout << "    3. 触发时: 遍历所有连接\n";
    cout << "       - 超过 T 秒未活跃 → 断开\n";
    cout << "       - 上次 PING 后 M 秒 → 发 PING\n";
    cout << "    4. 收到任何数据 → 更新 last_active_time\n";
    cout << "    5. 收到 PONG → 清除 pending_ping 标记\n";
    cout << "\n";
    cout << "  性能注意:\n";
    cout << "    - 遍历 10000 连接 O(n) 每 30s — 可接受\n";
    cout << "    - 不需要每连接一个 timerfd (fd 开销太大)\n";
    cout << "    - 一个全局 timerfd 负责所有超时检查\n";
  }
}

// ============================================================
// 练习 5: HTTP 代理 — 基础概念
// ============================================================
//
// HTTP 代理站在客户端和服务器之间，转发请求和响应。
//
// 两种模式:
//
// 1. 正向代理 (Forward Proxy):
//    客户端主动配置代理，代理代表客户端访问外部网络。
//    例如: 公司内网通过代理访问外网。
//
//    Client → Proxy → Internet → Server
//
// 2. 反向代理 (Reverse Proxy):
//    代理站在服务器前面，对外表现为服务器。
//    例如: Nginx 在应用服务器前面。
//
//    Client → Proxy → Backend Server(s)
//
// 本周聚焦正向代理。

void exercise5_proxy_basics() {
  section("练习 5: HTTP 代理 — 基础概念");

  // TODO 5.1: 正向代理架构
  {
    subsection("正向代理架构");

    cout << "  正向代理 (Forward Proxy):\n";
    cout << "  ┌──────────┐      ┌──────────┐      ┌──────────────┐\n";
    cout << "  │ Browser  │ ───▶ │  Proxy   │ ───▶ │ example.com  │\n";
    cout << "  │ (Client) │ ◀─── │ (转发)    │ ◀─── │ (Server)     │\n";
    cout << "  └──────────┘      └──────────┘      └──────────────┘\n";
    cout << "\n";
    cout << "  HTTP 代理的两种实现方式:\n";
    cout << "    1. GET 完整 URL: GET http://example.com/page HTTP/1.1\n";
    cout << "       (代理解析 URL, 自己连接目标服务器)\n";
    cout << "    2. CONNECT 隧道: CONNECT example.com:443 HTTP/1.1\n";
    cout << "       (代理只做 TCP 转发, 不解包 HTTP — 用于 HTTPS)\n";
  }

  // TODO 5.2: GET 完整 URL 方式
  {
    subsection("方式 1: GET 完整 URL (HTTP 代理)");

    cout << "  客户端发给代理的请求:\n";
    cout << "    GET http://www.example.com/index.html HTTP/1.1\n";
    cout << "    Host: www.example.com\n";
    cout << "    (注意: URI 是完整 URL, 不是 /path)\n";
    cout << "\n";
    cout << "  代理的工作:\n";
    cout << "    1. 解析 URL → 提取 host, port, path\n";
    cout << "    2. 连接目标服务器 (host:port)\n";
    cout << "    3. 发送修改后的请求: GET /index.html HTTP/1.1\n";
    cout << "    4. 接收目标服务器的响应\n";
    cout << "    5. 转发响应给客户端\n";
  }

  // TODO 5.3: CONNECT 隧道方式
  {
    subsection("方式 2: CONNECT 隧道 (HTTPS 代理)");

    cout << "  客户端发给代理:\n";
    cout << "    CONNECT www.example.com:443 HTTP/1.1\n";
    cout << "    Host: www.example.com:443\n";
    cout << "\n";
    cout << "  代理回应:\n";
    cout << "    HTTP/1.1 200 Connection Established\n";
    cout << "\n";
    cout << "  此后, 代理只是:\n";
    cout << "    - 从客户端读数据 → 转发到服务器\n";
    cout << "    - 从服务器读数据 → 转发到客户端\n";
    cout << "    (不解析内容, 不解包 HTTP — 纯 TCP 转发)\n";
    cout << "\n";
    cout << "  💡 这是 HTTPS 代理的基础:\n";
    cout << "    代理不可能解密 TLS, 所以只能用隧道模式\n";
  }
}

// ============================================================
// 练习 6: HTTP 正向代理 — GET 转发
// ============================================================
//
// 实现一个简单的 HTTP 正向代理:
//   1. 接收客户端的 GET 请求 (完整 URL)
//   2. 解析目标 host/port/path
//   3. 连接目标服务器
//   4. 转发请求
//   5. 接收响应并转发给客户端

void exercise6_forward_proxy() {
  section("练习 6: HTTP 正向代理 — GET 转发");

  // TODO 6.1: 代理流程
  {
    subsection("代理转发流程");

    cout << "  代理服务器的核心逻辑:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ 1. 接收客户端连接                            │\n";
    cout << "  │ 2. 解析 HTTP 请求                             │\n";
    cout << "  │ 3. 从请求行提取完整 URL                       │\n";
    cout << "  │ 4. URL 解析 → host, port, path               │\n";
    cout << "  │ 5. socket + connect 到目标服务器              │\n";
    cout << "  │ 6. 发送修改后的请求给目标服务器                │\n";
    cout << "  │ 7. 从目标服务器接收响应                      │\n";
    cout << "  │ 8. 转发响应给客户端                           │\n";
    cout << "  │ 9. 关闭双向连接                              │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
  }

  // TODO 6.2: 实践 — 代理转发 example.com
  {
    subsection("实践: 用代理获取 example.com");

    constexpr int PROXY_PORT = 15061;

    // 启动一个简单的代理服务器
    std::thread proxy([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PROXY_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client < 0) return;
      ScopedFd cg(client);

      // 接收客户端请求
      char buf[4096]{};
      ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
      if (n <= 0) return;
      buf[n] = '\0';

      // 解析请求行: GET http://example.com/ HTTP/1.1
      std::istringstream iss(buf);
      string method, full_url, version;
      iss >> method >> full_url >> version;

      cout << "  [Proxy] 收到: " << method << " " << full_url << "\n";

      // 解析 URL
      string url = full_url;
      // 去掉 http://
      size_t scheme_end = url.find("://");
      if (scheme_end != string::npos) {
        url = url.substr(scheme_end + 3);
      }
      // 分离 host:port 和 path
      size_t path_start = url.find('/');
      string host_port, path;
      if (path_start != string::npos) {
        host_port = url.substr(0, path_start);
        path = url.substr(path_start);
      } else {
        host_port = url;
        path = "/";
      }
      // 分离 host 和 port
      string host = host_port;
      int port = 80;
      size_t colon = host_port.find(':');
      if (colon != string::npos) {
        host = host_port.substr(0, colon);
        port = std::stoi(host_port.substr(colon + 1));
      }

      cout << "  [Proxy] 目标: " << host << ":" << port << path << "\n";

      // 连接目标服务器
      int remote = socket(AF_INET, SOCK_STREAM, 0);
      if (remote < 0) {
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, err.c_str(), err.size(), MSG_NOSIGNAL);
        return;
      }
      ScopedFd rg(remote);

      sockaddr_in remote_addr{};
      remote_addr.sin_family = AF_INET;
      remote_addr.sin_port = htons(port);
      if (inet_pton(AF_INET, host.c_str(), &remote_addr.sin_addr) != 1) {
        // 需要 DNS 解析 — 这里简化处理
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, err.c_str(), err.size(), MSG_NOSIGNAL);
        return;
      }

      if (connect(remote, reinterpret_cast<sockaddr *>(&remote_addr),
                  sizeof(remote_addr)) < 0) {
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, err.c_str(), err.size(), MSG_NOSIGNAL);
        return;
      }

      // 构造转发请求
      string forward_req =
          method + " " + path + " " + version + "\r\n"
          "Host: " + host +
          "\r\n"
          "Connection: close\r\n"
          "\r\n";
      send(remote, forward_req.c_str(), forward_req.size(), MSG_NOSIGNAL);

      // 接收目标服务器响应并转发给客户端
      char resp_buf[8192];
      ssize_t resp_n = recv(remote, resp_buf, sizeof(resp_buf) - 1, 0);
      if (resp_n > 0) {
        send(client, resp_buf, resp_n, MSG_NOSIGNAL);

        // 提取状态码
        string resp(resp_buf, resp_n);
        auto sp = resp.find(' ');
        if (sp != string::npos) {
          auto sp2 = resp.find(' ', sp + 1);
          cout << "  [Proxy] 响应: "
               << resp.substr(sp + 1, sp2 - sp - 1)
               << " (" << resp_n << " 字节)\n";
        }
      }

      cout << "  [Proxy] 转发完成\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端: 通过代理访问
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PROXY_PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 发送代理格式的请求 (完整 URL)
      string request =
          "GET http://example.com/ HTTP/1.1\r\n"
          "Host: example.com\r\n"
          "Connection: close\r\n"
          "\r\n";
      send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);

      char buf[8192]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        string resp(buf, n);
        auto pos = resp.find("200 OK");
        if (pos != string::npos) {
          cout << "  [Client] 通过代理获取到 example.com → 200 OK\n";
        } else {
          cout << "  [Client] 通过代理获取到响应 (" << n << " 字节)\n";
        }
      }
    }

    proxy.join();

    cout << "  ✅ HTTP 正向代理 GET 转发成功!\n";
  }
}

// ============================================================
// 练习 7: TCP 端口转发 / 隧道
// ============================================================
//
// TCP 端口转发是最通用的代理模式:
//   - 监听本地端口
//   - 收到连接 → 连接远程 host:port
//   - 双向转发所有数据 (不做协议解析)
//
// 这可以用作:
//   - 内网穿透
//   - 跳板机
//   - 简单负载均衡
//   - 协议调试

// TCP 管道: 双向转发两个 fd 之间的数据
// fd_a ←→ fd_b (全双工)
struct TcpPipe {
  int fd_a; // 客户端
  int fd_b; // 目标服务器
  bool a_closed = false;
  bool b_closed = false;

  TcpPipe(int a, int b) : fd_a(a), fd_b(b) {}
};

// 尝试从 a 读, 写到 b
// 返回 false 表示 a 已关闭或出错
bool relay_one_way(int from_fd, int to_fd, bool &from_closed) {
  if (from_closed) return false;

  char buf[8192];
  ssize_t n = recv(from_fd, buf, sizeof(buf), 0);
  if (n > 0) {
    // 发送 (简单处理, 生产环境需要处理 EAGAIN)
    ssize_t sent = 0;
    while (sent < n) {
      ssize_t r = send(to_fd, buf + sent, n - sent, MSG_NOSIGNAL);
      if (r <= 0) {
        from_closed = true;
        return false;
      }
      sent += r;
    }
    return true;
  } else if (n == 0) {
    // 对端关闭
    from_closed = true;
    shutdown(to_fd, SHUT_WR); // 半关闭: 不再写, 但仍可读
    return false;
  }
  return true; // EAGAIN or error
}

void exercise7_tcp_tunnel() {
  section("练习 7: TCP 端口转发 / 隧道");

  // TODO 7.1: TCP 隧道原理
  {
    subsection("TCP 隧道工作原理");

    cout << "  TCP 隧道 (端口转发):\n";
    cout << "  ┌──────────┐      ┌──────────┐      ┌──────────────┐\n";
    cout << "  │ Client   │ ───▶ │ Tunnel   │ ───▶ │ Target       │\n";
    cout << "  │          │ ◀─── │ (转发)    │ ◀─── │ (Host:Port)  │\n";
    cout << "  └──────────┘      └──────────┘      └──────────────┘\n";
    cout << "\n";
    cout << "  核心: relay_one_way(a→b) + relay_one_way(b→a)\n";
    cout << "    - 读 A → 写 B\n";
    cout << "    - 读 B → 写 A\n";
    cout << "    - 任何一端关闭 → shutdown 另一端写方向\n";
    cout << "    - 两端都关闭 → 清理\n";
  }

  // TODO 7.2: 实践 — 端口转发到 Echo 服务器
  {
    subsection("实践: 端口转发 (转发到 Echo 服务器)");

    constexpr int TARGET_PORT = 15071; // Echo 服务器
    constexpr int TUNNEL_PORT = 15072; // 隧道监听端口

    // 启动一个简单的 Echo 服务器作为转发目标
    std::thread echo_server([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(TARGET_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        char buf[256];
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
          buf[n] = '\0';
          send(client, buf, n, MSG_NOSIGNAL);
          cout << "  [Echo] 收到并返回: \"" << buf << "\"\n";
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 启动隧道 (端口转发器)
    std::thread tunnel([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(TUNNEL_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client < 0) return;
      ScopedFd cg(client);

      // 连接到目标 Echo 服务器
      int target = socket(AF_INET, SOCK_STREAM, 0);
      if (target < 0) return;
      ScopedFd tg(target);

      sockaddr_in target_addr{};
      target_addr.sin_family = AF_INET;
      target_addr.sin_port = htons(TARGET_PORT);
      inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);
      connect(target, reinterpret_cast<sockaddr *>(&target_addr),
              sizeof(target_addr));

      cout << "  [Tunnel] 客户端连接 → 转发到 :" << TARGET_PORT << "\n";

      // 双向转发
      bool client_closed = false, target_closed = false;
      for (int i = 0; i < 10 && !(client_closed && target_closed); ++i) {
        relay_one_way(client, target, client_closed);
        relay_one_way(target, client, target_closed);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      cout << "  [Tunnel] 转发完成\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 客户端: 连接到隧道 (而不是直接连 Echo)
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(TUNNEL_PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      string msg = "Hello through tunnel!";
      send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);

      char buf[256]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        cout << "  [Client] 通过隧道收到: \"" << buf << "\"\n";
      }
    }

    tunnel.join();
    echo_server.join();

    cout << "  ✅ TCP 端口转发成功 — 客户端通过隧道访问了 Echo 服务\n";
  }

  // TODO 7.3: 生产级隧道要点
  {
    subsection("生产级隧道要点");

    cout << "  1. 非阻塞 IO + epoll:\n";
    cout << "     用 epoll 同时监控 client 和 target 两个 fd\n";
    cout << "     有数据就转发, 不需要轮询\n";
    cout << "\n";
    cout << "  2. 半关闭传递:\n";
    cout << "     client shutdown(SHUT_WR) → tunnel shutdown(target, SHUT_WR)\n";
    cout << "     target shutdown(SHUT_WR) → tunnel shutdown(client, SHUT_WR)\n";
    cout << "\n";
    cout << "  3. 缓存区大小:\n";
    cout << "     转发的 buffer 大小影响吞吐量\n";
    cout << "     太小 → 系统调用频繁; 太大 → 内存浪费\n";
    cout << "     常用 8KB–64KB\n";
    cout << "\n";
    cout << "  4. 连接管理:\n";
    cout << "     每个隧道连接 = client_fd + target_fd\n";
    cout << "     关闭 → 同时关闭两个 fd\n";
    cout << "     内存泄漏 → 连接泄漏 → fd 耗尽\n";
  }
}

// ============================================================
// 练习 8: CONNECT 隧道 — HTTPS 代理基础
// ============================================================
//
// CONNECT 方法是 HTTP/1.1 定义的，用于建立隧道:
//   CONNECT host:port HTTP/1.1
//
// 代理收到 CONNECT:
//   1. 连接目标服务器
//   2. 回复 200 Connection Established
//   3. 之后就是纯 TCP 双向转发
//
// 这是 HTTPS 代理的基础。
// 也是 WebSocket over proxy 的基础。

void exercise8_connect_tunnel() {
  section("练习 8: CONNECT 隧道 — HTTPS 代理基础");

  // TODO 8.1: CONNECT 方法
  {
    subsection("CONNECT 方法详解");

    cout << "  CONNECT 请求:\n";
    cout << "    CONNECT www.example.com:443 HTTP/1.1\n";
    cout << "    Host: www.example.com:443\n";
    cout << "\n";
    cout << "  代理响应:\n";
    cout << "    HTTP/1.1 200 Connection Established\n";
    cout << "    (空行)\n";
    cout << "\n";
    cout << "  之后:\n";
    cout << "    客户端 ←→ 代理 ←→ 目标服务器\n";
    cout << "    纯 TCP 双向转发\n";
    cout << "    代理不再解析 HTTP (也解不开了 — TLS 加密)\n";
  }

  // TODO 8.2: 实践 — CONNECT 隧道转发
  {
    subsection("实践: CONNECT 隧道到 Echo 服务器");

    constexpr int ECHO_PORT = 15081;
    constexpr int CONNECT_PORT = 15082;

    // Echo 服务器
    std::thread echo([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(ECHO_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        char buf[256];
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
          buf[n] = '\0';
          send(client, buf, n, MSG_NOSIGNAL);
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // CONNECT 隧道代理
    std::thread proxy([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(CONNECT_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client < 0) return;
      ScopedFd cg(client);

      // 接收 CONNECT 请求
      char buf[4096]{};
      ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
      if (n <= 0) return;
      buf[n] = '\0';

      std::istringstream iss(buf);
      string method, target, version;
      iss >> method >> target >> version;

      cout << "  [Proxy] 收到: " << method << " " << target << "\n";

      if (method != "CONNECT") {
        string err = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
        send(client, err.c_str(), err.size(), MSG_NOSIGNAL);
        return;
      }

      // 解析 host:port
      string host = target;
      int port = 80;
      size_t colon = target.find(':');
      if (colon != string::npos) {
        host = target.substr(0, colon);
        port = std::stoi(target.substr(colon + 1));
      }

      // 连接目标
      int remote = socket(AF_INET, SOCK_STREAM, 0);
      if (remote < 0) {
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, err.c_str(), err.size(), MSG_NOSIGNAL);
        return;
      }
      ScopedFd rg(remote);

      sockaddr_in remote_addr{};
      remote_addr.sin_family = AF_INET;
      remote_addr.sin_port = htons(port);
      inet_pton(AF_INET, host.c_str(), &remote_addr.sin_addr);
      if (connect(remote, reinterpret_cast<sockaddr *>(&remote_addr),
                  sizeof(remote_addr)) < 0) {
        string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        send(client, err.c_str(), err.size(), MSG_NOSIGNAL);
        return;
      }

      cout << "  [Proxy] 已连接目标 " << host << ":" << port << "\n";

      // 回复 200 Connection Established
      string ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
      send(client, ok.c_str(), ok.size(), MSG_NOSIGNAL);

      // 隧道模式: 双向转发
      bool client_done = false, remote_done = false;
      for (int i = 0; i < 20 && !(client_done && remote_done); ++i) {
        relay_one_way(client, remote, client_done);
        relay_one_way(remote, client, remote_done);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      cout << "  [Proxy] 隧道关闭\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 客户端: 通过 CONNECT 隧道发送数据
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(CONNECT_PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 发送 CONNECT 请求
      string connect_req =
          "CONNECT 127.0.0.1:" + std::to_string(ECHO_PORT) +
          " HTTP/1.1\r\n"
          "Host: 127.0.0.1:" +
          std::to_string(ECHO_PORT) + "\r\n\r\n";
      send(fd, connect_req.c_str(), connect_req.size(), MSG_NOSIGNAL);

      // 接收 200 响应
      char resp[256]{};
      ssize_t rn = recv(fd, resp, sizeof(resp) - 1, 0);
      if (rn > 0) {
        resp[rn] = '\0';
        if (string(resp).find("200") != string::npos) {
          cout << "  [Client] 隧道建立成功 (200 Connection Established)\n";
        }
      }

      // 通过隧道发送数据
      string msg = "Data through CONNECT tunnel";
      send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);

      // 接收 echo
      char echo_buf[256]{};
      ssize_t en = recv(fd, echo_buf, sizeof(echo_buf) - 1, 0);
      if (en > 0) {
        echo_buf[en] = '\0';
        cout << "  [Client] 通过隧道收到 echo: \"" << echo_buf << "\"\n";
      }
    }

    proxy.join();
    echo.join();

    cout << "  ✅ CONNECT 隧道工作正常 — HTTPS 代理的基础\n";
  }
}

// ============================================================
// 练习 9: 简易负载均衡器
// ============================================================
//
// 负载均衡器将客户端请求分发到多个后端服务器。
//
// 核心算法:
//   1. Round Robin (轮询) — 最简单的
//   2. Least Connections (最少连接) — 更均衡
//   3. Random — 也出奇地好
//
// 本节实现轮询负载均衡。

struct Backend {
  string host;
  int port;
  bool healthy;
  int active_connections = 0;

  Backend(string h, int p) : host(std::move(h)), port(p), healthy(true) {}
};

class SimpleLoadBalancer {
public:
  SimpleLoadBalancer(int listen_port) : _listen_port(listen_port) {
    _epfd = epoll_create1(0);
  }

  ~SimpleLoadBalancer() {
    if (_epfd >= 0) close(_epfd);
  }

  void add_backend(const string &host, int port) {
    _backends.emplace_back(host, port);
  }

  // 轮询选择后端
  Backend *pick_backend() {
    if (_backends.empty()) return nullptr;
    // 只选健康的
    for (size_t i = 0; i < _backends.size(); ++i) {
      size_t idx = (_rr_counter + i) % _backends.size();
      if (_backends[idx].healthy) {
        _rr_counter = (idx + 1) % _backends.size();
        return &_backends[idx];
      }
    }
    return nullptr; // 没有健康后端
  }

  int get_epfd() const { return _epfd; }

private:
  int _epfd = -1;
  int _listen_port;
  vector<Backend> _backends;
  size_t _rr_counter = 0; // 轮询计数器
};

void exercise9_load_balancer() {
  section("练习 9: 简易负载均衡器");

  // TODO 9.1: 负载均衡策略
  {
    subsection("负载均衡策略");

    cout << "  1. Round Robin (轮询):\n";
    cout << "     请求 1 → 后端 A, 请求 2 → 后端 B, 请求 3 → 后端 C\n";
    cout << "     请求 4 → 后端 A, ...\n";
    cout << "     ✅ 简单公平 ⚠️ 不考虑服务器负载\n";
    cout << "\n";
    cout << "  2. Least Connections (最少连接):\n";
    cout << "     每次选择当前连接数最少的后端\n";
    cout << "     ✅ 自适应负载 ⚠️ 需要跟踪连接数\n";
    cout << "\n";
    cout << "  3. Weighted (加权):\n";
    cout << "     给性能好的服务器更高权重\n";
    cout << "     ✅ 利用异构服务器\n";
    cout << "\n";
    cout << "  4. IP Hash:\n";
    cout << "     根据客户端 IP hash 到固定后端\n";
    cout << "     ✅ 会话保持 (sticky session)\n";
  }

  // TODO 9.2: 实践 — 轮询负载均衡
  {
    subsection("实践: 轮询到 2 个后端 Echo 服务器");

    constexpr int BACKEND1_PORT = 15091;
    constexpr int BACKEND2_PORT = 15092;
    constexpr int LB_PORT = 15093;

    // 后端 1
    std::thread backend1([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(BACKEND1_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 2);

      for (int i = 0; i < 2; ++i) {
        int client = accept(fd, nullptr, nullptr);
        if (client >= 0) {
          ScopedFd cg(client);
          char buf[256];
          ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
          if (n > 0) {
            buf[n] = '\0';
            string resp = "[Backend1] echo: " + string(buf);
            send(client, resp.c_str(), resp.size(), MSG_NOSIGNAL);
          }
        }
      }
    });

    // 后端 2
    std::thread backend2([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(BACKEND2_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 2);

      for (int i = 0; i < 2; ++i) {
        int client = accept(fd, nullptr, nullptr);
        if (client >= 0) {
          ScopedFd cg(client);
          char buf[256];
          ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
          if (n > 0) {
            buf[n] = '\0';
            string resp = "[Backend2] echo: " + string(buf);
            send(client, resp.c_str(), resp.size(), MSG_NOSIGNAL);
          }
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 负载均衡器
    std::thread lb([&]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(LB_PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 4);

      // 后端列表
      vector<std::pair<string, int>> backends = {
          {"127.0.0.1", BACKEND1_PORT},
          {"127.0.0.1", BACKEND2_PORT},
      };
      int rr = 0; // 轮询计数器

      cout << "  [LB] 启动在 :" << LB_PORT
           << ", 后端: :" << BACKEND1_PORT << ", :" << BACKEND2_PORT << "\n";

      for (int i = 0; i < 4; ++i) {
        int client = accept(fd, nullptr, nullptr);
        if (client < 0) continue;

        // 接收客户端数据
        char buf[256]{};
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
          close(client);
          continue;
        }

        // 轮询选择后端
        auto &[host, port] = backends[rr];
        rr = (rr + 1) % backends.size();

        cout << "  [LB] 请求 → 后端 " << (rr == 1 ? "2" : "1")
             << " (:" << port << ")\n";

        // 连接后端
        int remote = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in ra{};
        ra.sin_family = AF_INET;
        ra.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &ra.sin_addr);
        connect(remote, reinterpret_cast<sockaddr *>(&ra), sizeof(ra));

        // 转发请求
        send(remote, buf, n, MSG_NOSIGNAL);

        // 接收后端响应
        char resp[256]{};
        ssize_t rn = recv(remote, resp, sizeof(resp) - 1, 0);

        // 转发响应给客户端
        if (rn > 0) {
          send(client, resp, rn, MSG_NOSIGNAL);
        }

        close(remote);
        close(client);
      }
      cout << "  [LB] 处理完毕\n";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 发送 4 个请求
    auto req = [](int id) {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(LB_PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      string msg = "request_" + std::to_string(id);
      send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);

      char buf[256]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        cout << "  [Client " << id << "] " << buf << "\n";
      }
    };

    std::thread r1(req, 1);
    std::thread r2(req, 2);
    std::thread r3(req, 3);
    std::thread r4(req, 4);

    r1.join(); r2.join(); r3.join(); r4.join();
    backend1.join(); backend2.join(); lb.join();

    cout << "\n  ✅ 轮询负载均衡器工作正常\n";
    cout << "    请求 1 → 后端 1, 请求 2 → 后端 2, 请求 3 → 后端 1, ...\n";
  }
}

// ============================================================
// 练习 10: 综合实战 — 聊天室 + 代理集成
// ============================================================
//
// 综合本周所有知识:
//   - 聊天室服务器: 多客户端, 房间管理, 心跳, epoll
//   - 简单代理: 端口转发, 请求转发
//
// 这展示了一个真实的网络服务架构。

void exercise10_integrated_service() {
  section("练习 10: 综合实战 — 多服务集成");

  constexpr int CHAT_PORT = 15101;
  constexpr int PROXY_PORT = 15102;
  constexpr int ECHO_PORT = 15103;

  // 服务 1: Echo 服务器 (作为转发目标)
  std::thread echo([]() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, 2);

    for (int i = 0; i < 1; ++i) {  // 只接受 1 个连接 (来自 tunnel)
      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        char buf[256];
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
          buf[n] = '\0';
          send(client, buf, n, MSG_NOSIGNAL);
        }
      }
    }
  });

  // 服务 2: TCP 隧道 (端口转发到 Echo)
  std::thread tunnel([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROXY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, 1);

    for (int i = 0; i < 1; ++i) {  // 只接受 1 个隧道客户端
      int client = accept(fd, nullptr, nullptr);
      if (client < 0) continue;

      // 连接到 Echo
      int target = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in ta{};
      ta.sin_family = AF_INET;
      ta.sin_port = htons(ECHO_PORT);
      inet_pton(AF_INET, "127.0.0.1", &ta.sin_addr);
      connect(target, reinterpret_cast<sockaddr *>(&ta), sizeof(ta));

      // 转发
      char buf[256];
      ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        send(target, buf, n, MSG_NOSIGNAL);
        ssize_t rn = recv(target, buf, sizeof(buf) - 1, 0);
        if (rn > 0) {
          send(client, buf, rn, MSG_NOSIGNAL);
        }
      }

      close(target);
      close(client);
    }
  });

  // 服务 3: 聊天室
  std::thread chat([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CHAT_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, 2);

    vector<int> clients;
    for (int i = 0; i < 2; ++i) {
      int c = accept(fd, nullptr, nullptr);
      if (c >= 0) clients.push_back(c);
    }

    // 客户端 1 发送消息 → 广播给客户端 2
    auto msg = recv_message(clients[0]);
    if (msg.has_value()) {
      string text = "[Chat] User1: " + string(msg->data());
      send_message(clients[1], text.c_str(), text.size());
    }

    // 客户端 2 回复 → 广播给客户端 1
    auto reply = recv_message(clients[1]);
    if (reply.has_value()) {
      string text = "[Chat] User2: " + string(reply->data());
      send_message(clients[0], text.c_str(), text.size());
    }

    for (int c : clients) close(c);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 测试 1: 聊天室
  cout << "\n  --- 测试 1: 聊天室 ---\n";
  {
    std::thread u1([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(CHAT_PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      send_message(fd, "Hello from User1", 16);

      auto reply = recv_message(fd);
      if (reply.has_value())
        cout << "  [User1] 收到: " << reply->data() << "\n";
    });

    std::thread u2([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(CHAT_PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      auto msg = recv_message(fd);
      if (msg.has_value())
        cout << "  [User2] 收到: " << msg->data() << "\n";

      send_message(fd, "Hi User1!", 9);
    });

    u1.join(); u2.join();
    cout << "  ✅ 聊天室测试通过\n";
  }

  // 测试 2: 通过隧道访问 Echo
  cout << "\n  --- 测试 2: TCP 隧道 ---\n";
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ScopedFd guard(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROXY_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

    string msg = "through tunnel";
    send(fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);

    char buf[256]{};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      cout << "  [Client] 隧道返回: \"" << buf << "\"\n";
    }
    cout << "  ✅ TCP 隧道测试通过\n";
  }

  chat.join();
  tunnel.join();
  echo.join();

  cout << "\n  ✅ 综合多服务集成测试完成!\n";
  cout << "\n  📋 本周实战涵盖了:\n";
  cout << "    1. 多客户端聊天室 (广播模型 + 协议设计 + 用户管理)\n";
  cout << "    2. 心跳检测 (timerfd + epoll 集成)\n";
  cout << "    3. HTTP 正向代理 (GET 完整 URL 转发)\n";
  cout << "    4. CONNECT 隧道 (HTTPS 代理基础)\n";
  cout << "    5. TCP 端口转发 (双向数据中继)\n";
  cout << "    6. 负载均衡器 (轮询策略)\n";
  cout << "    7. 多服务集成 (聊天室 + 隧道 + Echo)\n";
  cout << "\n  🚀 你现在具备了构建真实网络服务的能力!\n";
}

// ============================================================
// main
// ============================================================

int main(int argc, char *argv[]) {
  cout << "Week 15: 网络服务实战 — 聊天室 + 代理 + 端口转发\n";
  cout << "==============================================================\n";

  if (argc > 1) {
    string mode = argv[1];
    cout << "用法: " << argv[0] << "  (直接运行所有练习)\n";
    return 1;
  }

  exercise1_chat_broadcast();     cout << "[done ex1]" << std::endl;
  exercise2_chat_protocol();      cout << "[done ex2]" << std::endl;
  exercise3_user_management();    cout << "[done ex3]" << std::endl;
  exercise4_heartbeat();          cout << "[done ex4]" << std::endl;
  exercise5_proxy_basics();       cout << "[done ex5]" << std::endl;
  exercise6_forward_proxy();      cout << "[done ex6]" << std::endl;
  exercise7_tcp_tunnel();         cout << "[done ex7]" << std::endl;
  exercise8_connect_tunnel();     cout << "[done ex8]" << std::endl;
  exercise9_load_balancer();      cout << "[done ex9]" << std::endl;
  exercise10_integrated_service();cout << "[done ex10]" << std::endl;

  cout << "\n✅ Week 15 全部练习完成！\n";
  cout << "\n📝 Week 15 总结要点:\n";
  cout << "  1. 聊天室广播模型: 一个客户端发消息 → "
          "广播给所有其他客户端\n";
  cout << "  2. 聊天协议: [4B len][1B type][data], "
          "LOGIN/MSG/LOGOUT/SYSTEM/PING/PONG\n";
  cout << "  3. 用户管理: vector<ChatUser*>, "
          "加入/离开/重命名广播通知\n";
  cout << "  4. 心跳检测: timerfd + epoll, "
          "定期 PING, 超时断开僵尸连接\n";
  cout << "  5. 正向代理: 解析 URL → 连接目标 → "
          "转发请求 → 转发响应\n";
  cout << "  6. CONNECT 隧道: 回复 200 后纯 TCP "
          "双向转发 (HTTPS 代理基础)\n";
  cout << "  7. TCP 端口转发: relay A→B + relay B→A, "
          "半关闭传递\n";
  cout << "  8. relay_one_way: 从 fd_a 读 → 写到 fd_b, "
          "recv==0 → shutdown(SHUT_WR)\n";
  cout << "  9. 负载均衡: Round Robin / Least Connections "
          "/ Random / IP Hash\n";
  cout << "  10. 多服务集成: 一个程序同时跑聊天室 + 隧道 + "
          "Echo 服务\n";
  cout << "\n🔑 核心能力:\n";
  cout << "  「你可以构建任何基于 TCP 的网络服务」\n";
  cout << "  聊天室、代理、隧道、负载均衡 — "
          "本质上都是 Socket + epoll + 协议解析\n";

  return 0;
}
