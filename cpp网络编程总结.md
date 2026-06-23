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
## Week 13: epoll / IO 多路复用 (2026-06-23) ✅

### 演进路线
- `select()`: fd_set, FD_SETSIZE=1024, O(n) 扫描, 每次拷贝
- `poll()`: pollfd 数组, 无 fd 限制, events≠revents(可重用), 仍 O(n)
- `epoll()`: Linux 专属, 红黑树+就绪链表, epoll_wait O(1) 只返回就绪 fd

### epoll 核心 API
- `epoll_create1(EPOLL_CLOEXEC)` — 创建 epoll 实例
- `epoll_ctl(epfd, ADD/MOD/DEL, fd, &ev)` — 管理监控列表
- `epoll_wait(epfd, events, maxevents, timeout)` — 等待事件 (LT/ET)
- `epoll_data_t` 联合体: ptr(推荐), fd, u32, u64

### LT vs ET (最核心的概念!)
| | LT (默认) | ET (EPOLLET) |
|---|---|---|
| 通知 | 有数据就通知 | 状态变化时通知一次 |
| 复杂度 | 低, 类似 poll | 高, 容易丢事件 |
| 要求 | 不强制非阻塞 | **必须**非阻塞 + 读到 EAGAIN |

### EPOLLONESHOT
- 触发后自动暂停监控 → 同一 fd 不会被多个线程同时处理
- 处理完后必须 `EPOLL_CTL_MOD` 重新注册

### EPOLLRDHUP — ⚠️ 关键发现
- **必须显式注册 `EPOLLRDHUP` 才能可靠检测对端关闭!**
- 单靠 `EPOLLIN` + `recv=0` 在非阻塞 socket 上不可靠

### timerfd + epoll
- `timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK)`
- `timerfd_settime(fd, 0, &itimerspec, nullptr)`
- 定时器和 IO 事件统一处理 (一个事件循环)

### 非阻塞 connect
- `connect` → `EINPROGRESS` → `epoll(EPOLLOUT)` → `getsockopt(SO_ERROR)`
- 用于并行连接、超时控制

### 推荐架构
```
epoll(LT/ET) + 非阻塞 IO + EPOLLRDHUP + Connection* ptr + timerfd 心跳
```

## Week 14: HTTP 协议 + 简易 HTTP Server (2026-06-23)

### HTTP 请求格式
```
METHOD SP URI SP VERSION CRLF
Header-Name: value CRLF
...
CRLF       ← 空行 (header 结束)
[body]     ← 可选, 长度由 Content-Length 决定
```

### HTTP 响应格式
```
VERSION SP STATUS-CODE SP REASON-PHRASE CRLF
Header-Name: value CRLF
...
CRLF
[body]
```

### 关键规则
- **CRLF (`\r\n`)** 是 HTTP 的行分隔符, 空行标记 header 结束
- **HTTP/1.1 默认 Keep-Alive**: 一个 TCP 连接处理多个请求
- **Connection: close**: 客户端/服务器声明要关闭连接
- **Content-Length**: 告知 body 的精确长度 (字节) — 静态文件首选
- **Transfer-Encoding: chunked**: 分块传输 — 动态内容/流式传输
- **Host header**: HTTP/1.1 必须, 支持虚拟主机

### URL 结构
```
scheme://user:pass@host:port/path?query#fragment
```
- 请求行中的 URI 只包含 path + query (不含 scheme/host)
- Host header 提供 host:port

### MIME 类型
- 根据文件扩展名确定 Content-Type
- text/* 类型加 `charset=utf-8`
- 未知类型 → `application/octet-stream`

### 请求解析流程
1. 读请求行 → METHOD, URI, VERSION
2. 逐行读头部 → 直到空行 (`\r\n\r\n`)
3. 检查 Content-Length → 读 N 字节 body
4. 头部名转小写 (大小写不敏感)

### Content-Length vs chunked
| | Content-Length | chunked |
|---|---|---|
| 提前知道大小 | ✅ 需要 | ❌ 不需要 |
| 适合场景 | 静态文件 | 动态生成 |
| 实现复杂度 | 简单 | 中等 |

### 安全防御清单
1. 路径检查: 拒绝包含 `..` 的路径 (目录遍历攻击)
2. 大小限制: 请求行/header/body 都设上限
3. 超时控制: 请求超时 30s, Keep-Alive 空闲超时 5s
4. URL 解码: 先解码再检查路径 (防止 `%2e%2e%2f` 绕过)
5. realpath(): 验证最终路径在文档根目录内
6. Slowloris 防御: 最小数据速率检测

### Keep-Alive 机制
- HTTP/1.1 默认启用
- 减少 TCP 握手开销和 TIME_WAIT
- 需要: Content-Length (确定 body 边界) + 空闲超时
- 响应头: `Connection: keep-alive` 或 `Connection: close`

### 架构模式
```
静态文件 HTTP Server =
  epoll(accept/read) + HTTP 请求解析 + 路径安全检查 +
  read_file + MIME 检测 + 响应构建 + TCP 发送
```

### 核心洞察
> 「HTTP Server 本质上就是: 解析文本请求 → 构建文本响应 → 通过 TCP 发送」
> 没有魔法, 全是字符串操作。约 500 行 C++20 代码就能实现可浏览器访问的 HTTP Server。

## Week 15: 网络服务实战 (2026-06-23)

### 聊天室广播模型
```
Client A ── "Hello" ──▶ Server ──broadcast──▶ Client B, C, D...
```
- `for (auto *c : _clients) send(c->fd, msg)` — O(n) 广播
- 优化: writev() 一次系统调用发多份

### 聊天协议: [4B len][1B type][data]
| Type | 值 | 含义 |
|------|------|------|
| LOGIN | 0x01 | 登录 (data=昵称) |
| MSG | 0x02 | 聊天消息 |
| LOGOUT | 0x03 | 退出 |
| SYSTEM | 0x04 | 服务器通知 |
| PING | 0x05 | 心跳请求 |
| PONG | 0x06 | 心跳回复 |

### 用户管理
- `vector<ChatUser*>` — 简单场景; `unordered_map<int, ChatUser*>` — O(1) 查找
- 加入/离开/改名 → SYSTEM 广播通知所有用户

### 心跳检测 (timerfd + epoll)
- 一个全局 timerfd 管所有连接的超时
- 每 N 秒遍历连接列表, 超时 T 秒未活跃 → 断开
- 发 PING 后 M 秒未收到 PONG → 断开

### HTTP 正向代理
```
Client → Proxy → parse URL → connect target → forward req → forward resp → Client
```
- 请求 URI 是完整 URL: `GET http://example.com/path HTTP/1.1`
- 代理提取 host:port, 转发 `GET /path HTTP/1.1` 到目标

### CONNECT 隧道 (HTTPS 代理基础)
```
Client → CONNECT host:443 → Proxy → 200 Established → 纯 TCP 双向转发
```
- 代理不解包内容 (TLS 加密解不开)
- relay_one_way(client→target) + relay_one_way(target→client)

### TCP 端口转发
```
relay_one_way(from_fd, to_fd):
  recv(from) → send(to)
  recv==0 → shutdown(to, SHUT_WR)  // 半关闭传递
```
- 生产级: epoll 同时监控两个 fd
- 缓冲区大小: 8KB–64KB 典型值

### 负载均衡策略
| 策略 | 优势 | 劣势 |
|------|------|------|
| Round Robin | 简单公平 | 不考虑负载 |
| Least Connections | 自适应 | 需跟踪连接数 |
| Weighted | 利用异构 | 需要配置权重 |
| IP Hash | 会话保持 | 分布不均 |

### 架构模式
```
网络服务 =
  Socket API (TCP/UDP) + epoll (多路复用) +
  协议解析 (长度前缀/HTTP/自定义) + 业务逻辑 +
  安全防御 (超时/大小限制/路径检查)
```

### 核心能力
> 「你可以构建任何基于 TCP 的网络服务」
> 聊天室、代理、隧道、负载均衡 — 本质上都是 Socket + epoll + 协议解析

## Week 16: Month 3 收官 (2026-06-23)

### Part A: W11-15 回顾
W11 Socket 基础 → W12 TCP 深入 → W13 epoll → W14 HTTP → W15 服务

### Part B: 性能对比
- select: O(n), FD_SETSIZE=1024
- poll: O(n), 无 fd 限制
- epoll: O(1) 就绪链表, 红黑树管理, 大规模显著领先

### Part C: Mini-Redis (RESP 协议 KV 存储)
**RESP 类型**: +Simple String, -Error, :Integer, $Bulk String, *Array
**KV 引擎**: unordered_map + lazy expire + cleanup_expired
**AOF**: 写命令追加到文件 → 重启时重放恢复

### Month 3 总结
> 「我可以用 C++ 构建任何基于 TCP 的网络服务」
> Socket → TCP 深入 → epoll → 协议解析 → 服务 → 生产级

- 6 周 (W11-16), 60 个练习, 全部完成 ✅
- 项目: HTTP Server, 聊天室, 代理, 隧道, 负载均衡器, Mini-Redis
- 万能公式: **Socket + epoll + 协议解析 + 业务逻辑 + 安全防御**

## Month 4: 极致性能 (下一步)
