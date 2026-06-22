# C++ 网络编程总结

> Month 3: 从 Socket API 到高性能网络服务

---

## Week 11: Socket 编程基础 (2026-06-22)

### 核心系统调用

```
服务器: socket() → bind() → listen() → accept() → recv()/send() → close()
客户端: socket() → connect() → send()/recv() → close()
```

### 关键概念

1. **socket()** — 创建通信端点。`AF_INET`=IPv4, `SOCK_STREAM`=TCP, `SOCK_DGRAM`=UDP
2. **字节序** — 网络字节序=大端。端口用 `htons()`/`ntohs()`, IP用 `htonl()`/`ntohl()`
3. **inet_pton/ntop** — 安全的 IP 地址转换 (替代 inet_addr/ntoa)
4. **getaddrinfo** — 线程安全的地址解析 (替代 gethostbyname)
5. **SO_REUSEADDR** — 允许服务器重启后立即绑定同一端口

### TCP vs UDP

| | TCP | UDP |
|---|-----|-----|
| 连接 | 面向连接 | 无连接 |
| 可靠性 | 保证送达+顺序 | 不保证 |
| 边界 | 字节流 | 保留消息边界 |
| 速度 | 较慢 | 较快 |
| API | send/recv | sendto/recvfrom |

### 非阻塞 I/O

- `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` 设置非阻塞
- 非阻塞操作不可立即完成时返回 -1, errno=EAGAIN
- 是多路复用(epoll)的基础

### C++ 最佳实践

- 用 RAII (ScopedFd) 管理 socket 文件描述符
- `send()` + `MSG_NOSIGNAL` 防止 SIGPIPE
- 新代码用 `getaddrinfo`，不用 `gethostbyname`

---

## Week 12: TCP 深入 (待学习)

## Week 13: epoll / IO 多路复用 (待学习)

## Week 14: HTTP 协议 + 简易 HTTP Server (待学习)

## Week 15: 网络服务实战 (待学习)
