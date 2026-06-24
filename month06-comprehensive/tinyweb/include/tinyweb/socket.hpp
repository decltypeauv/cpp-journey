// ============================================================================
// tinyweb/socket.hpp — Socket 工具函数 (非阻塞, TCP_NODELAY, 监听)
// ============================================================================
#pragma once
#include "common.hpp"    // 系统头文件 + Slice + println

namespace sock {

// set_nonblocking: 设置 fd 为非阻塞模式 (O_NONBLOCK)
// 非阻塞 socket 是 epoll 高效工作的基础 — 没有它, send/recv 会阻塞整个线程
inline int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);            // 获取当前文件状态标志
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK); // 追加 O_NONBLOCK 并写回
}

// set_nodelay: 禁用 Nagle 算法 (TCP_NODELAY)
// Nagle 会攒包等待更多数据, 对 HTTP 服务器有害 (增加延迟)
inline int set_nodelay(int fd) {
  int opt = 1;                                   // 选项值: 1 = 启用 TCP_NODELAY (即禁用 Nagle)
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

// set_reuseaddr: 允许端口复用 (SO_REUSEADDR)
// 服务器重启时立即绑定端口, 不等 TIME_WAIT 状态消失
inline int set_reuseaddr(int fd) {
  int opt = 1;                                   // 选项值: 1 = 启用
  return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

// create_listen_socket: 创建 TCP 监听 socket 的完整流程
//   socket() → bind() → listen() → 返回非阻塞 fd
inline int create_listen_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);     // 创建 TCP socket
  if (fd < 0) return -1;                        // 失败返回 -1

  set_reuseaddr(fd);                            // 端口复用 (防止重启时 bind 失败)
  set_nonblocking(fd);                          // 非阻塞模式 (epoll 必需)

  sockaddr_in addr{};                           // IPv4 地址结构 (零初始化)
  addr.sin_family = AF_INET;                    // 地址族: IPv4
  addr.sin_addr.s_addr = INADDR_ANY;            // 绑定所有网络接口 (0.0.0.0)
  addr.sin_port = htons(port);                  // 端口号: 主机字节序 → 网络字节序 (大端)

  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; } // 绑定失败则关闭并返回
  if (listen(fd, SOMAXCONN) < 0) { close(fd); return -1; }           // 开始监听 (backlog = 系统最大值)

  return fd;                                    // 返回监听 fd
}

// get_peer_addr: 获取对端地址 (用于日志)
inline std::string get_peer_addr(int fd) {
  sockaddr_in addr{};                           // 地址结构
  socklen_t len = sizeof(addr);                 // 地址长度 (值-结果参数)
  if (getpeername(fd, (sockaddr*)&addr, &len) < 0) return "unknown"; // 获取对端地址
  char buf[INET_ADDRSTRLEN];                    // IPv4 地址字符串缓冲 (至少 16 字节)
  inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)); // 网络字节序 → 可读 IP 字符串
  return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port)); // "IP:port"
}

} // namespace sock
