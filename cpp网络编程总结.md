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

## Week 12: TCP 深入 (2026-06-23) ✅

### Nagle 算法
- Nagle: 有未确认小包在途时，后续小包等待 → 减少小包数，但增加延迟
- `TCP_NODELAY`: 禁用 Nagle → 每次 send 立即发出（低延迟场景必备）
- `TCP_CORK`: Linux 特有，攒包直到取消 CORK → 比 Nagle 更可控
- Nagle + Delayed ACK = 40ms "死锁" → 解决方案: TCP_NODELAY 或 writev

### TCP KeepAlive
- `TCP_KEEPIDLE`: 空闲多久后首次探测 (默认 7200s!)
- `TCP_KEEPINTVL`: 探测间隔 (默认 75s)
- `TCP_KEEPCNT`: 最大探测次数 (默认 9)
- 应用层心跳 > 内核 KeepAlive: 更灵活, 可检测应用层健康状态, 不受中间设备干扰

### 粘包/拆包 — TCP 字节流的消息边界
- **粘包**: send("A") + send("B") → recv 收到 "AB" (合并)
- **拆包**: send("Hello") → recv 收到 "Hel", 再 recv 收到 "lo" (拆分)
- 根本原因: TCP 是字节流, 不保留应用层消息边界
- 三种解决方案:

| 方案 | 优点 | 缺点 | 适用 |
|------|------|------|------|
| 定长消息 | 简单 O(1) | 浪费带宽, 不灵活 | 固定格式数据 |
| 分隔符 \\n | 人类可读 | 需转义, 逐字节低效 | HTTP, Redis |
| 长度前缀 [4B len][data] | 二进制安全, 变长, O(1) | 需处理字节序 | gRPC, MySQL |

### shutdown() vs close()
- `close(fd)`: 减少引用计数, 计数到 0 才关闭双向; 受 dup/fork 影响
- `shutdown(fd, SHUT_WR)`: 发 FIN 但仍可收数据 (半关闭)
- `shutdown(fd, SHUT_RD)`: 丢弃接收缓冲, 再 recv 返回 0
- 优雅关闭: shutdown(SHUT_WR) → 读到 FIN → close()

### SO_LINGER
- 默认(l_onoff=0): close 立即返回, 内核后台发剩余数据
- 优雅等待(l_onoff=1, l_linger>0): close 阻塞最多 N 秒
- 立即 RST(l_onoff=1, l_linger=0): 发 RST 跳过 TIME_WAIT → 危险, 不推荐

### TCP 状态机
- 11 种状态: CLOSED → LISTEN → SYN_RCVD → ESTABLISHED → ...
- 三次握手: SYN → SYN+ACK → ACK
- 四次挥手: FIN → ACK → FIN → ACK
- **CLOSE_WAIT 堆积** = 服务端 bug (收到 FIN 未 close)
- **TIME_WAIT** 正常 (2MSL≈60s), 不应强制消除

---
---
## Week 13: epoll / IO 多路复用 (待学习)

## Week 14: HTTP 协议 + 简易 HTTP Server (待学习)

## Week 15: 网络服务实战 (待学习)
