// ============================================================================
// tinyweb/event_loop.hpp — epoll 事件循环 (libevent Reactor 模式)
// ============================================================================
// 设计灵感: libevent 的 event_base + epoll backend
// 核心: epoll_wait → 就绪 fd → 查找回调 → 执行
#pragma once
#include "common.hpp"    // 系统头文件

class EventLoop {
public:
  using Callback = std::function<void(uint32_t events)>; // 事件回调签名: events 是 epoll 事件位掩码

  // 构造函数: 创建 epoll 实例
  explicit EventLoop(int max_events = 1024) : _max_events(max_events) {
    _epfd = epoll_create1(EPOLL_CLOEXEC);       // 创建 epoll fd (CLOEXEC: exec 时自动关闭)
    if (_epfd < 0)
      throw std::runtime_error("epoll_create1 failed: " + std::to_string(errno));
    _events.resize(_max_events);                // 预分配事件数组
  }

  // 析构: 关闭 epoll fd
  ~EventLoop() { if (_epfd >= 0) close(_epfd); }

  // 禁止拷贝 (epoll fd 独占)
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // add: 注册 fd 的事件监听
  void add(int fd, uint32_t events, Callback cb) {
    _callbacks[fd] = std::move(cb);             // 存储回调 (move 避免拷贝)
    epoll_ctl_op(fd, EPOLL_CTL_ADD, events);    // 向内核注册
  }

  // mod: 修改 fd 的事件监听 (如: 写完数据后切回只读)
  void mod(int fd, uint32_t events) {
    epoll_ctl_op(fd, EPOLL_CTL_MOD, events);    // 修改已有注册
  }

  // del: 删除 fd 的事件监听
  void del(int fd) {
    epoll_ctl_op(fd, EPOLL_CTL_DEL, 0);         // 从内核删除
    _callbacks.erase(fd);                       // 移除回调
  }

  // run: 主事件循环 (阻塞直到 stop() 或错误)
  void run() {
    _running = true;                            // 标记运行中
    while (_running) {                          // 循环直到 stop()
      int nfds = epoll_wait(_epfd, _events.data(), _max_events, -1); // 阻塞等待事件 (-1 = 无限超时)
      if (nfds < 0) {
        if (errno == EINTR) continue;           // 被信号中断 → 继续等待
        break;                                  // 致命错误 → 退出
      }
      for (int i = 0; i < nfds; i++) {          // 遍历所有就绪事件
        int fd = _events[i].data.fd;            // 获取就绪的 fd
        auto it = _callbacks.find(fd);          // 查找对应回调
        if (it != _callbacks.end())
          it->second(_events[i].events);        // 调用回调, 传入实际就绪的事件类型
      }
    }
  }

  // stop: 停止事件循环 (可从其他线程调用)
  void stop() { _running = false; }

private:
  int _epfd;                                    // epoll 文件描述符
  int _max_events;                              // 最大就绪事件数
  std::vector<epoll_event> _events;             // epoll_wait 返回的事件数组
  std::unordered_map<int, Callback> _callbacks; // fd → 回调函数 映射
  bool _running = false;                        // 循环控制标志

  // epoll_ctl_op: 包装 epoll_ctl, 统一错误处理
  void epoll_ctl_op(int fd, int op, uint32_t events) {
    epoll_event ev{};                           // epoll 事件结构 (零初始化)
    ev.events = events | EPOLLRDHUP;            // 总是监听对端半关闭 (优雅检测断开)
    ev.data.fd = fd;                            // 关联 fd (用于回调时识别)
    if (::epoll_ctl(_epfd, op, fd, &ev) < 0) {   // 调用系统 epoll_ctl
      if (op == EPOLL_CTL_DEL) return;          // DEL 失败通常是因为 fd 已关闭 → 忽略
      throw std::runtime_error("epoll_ctl failed: errno=" + std::to_string(errno));
    }
  }
};
