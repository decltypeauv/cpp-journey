// ============================================================================
// tinyweb/server.hpp — TinyWeb HTTP 服务器 (顶层接口)
// ============================================================================
// 职责: 整合所有组件, 提供声明式 API, 管理服务器生命周期
// 用法:
//   TinyWeb app(8080);
//   app.route("GET", "/api/hello", [](auto& req){ return ...; });
//   app.route_middleware(middleware::logger());
//   app.route_ws("/ws/chat", [](auto ws){ ... });
//   app.serve_static("./public");
//   app.start(); // 阻塞直到 stop()
#pragma once
#include "common.hpp"            // std::unique_ptr, std::thread, csignal
#include "event_loop.hpp"        // EventLoop
#include "router.hpp"            // Router
#include "file_server.hpp"       // FileServer
#include "thread_pool.hpp"       // ThreadPool
#include "connection.hpp"        // Connection
#include "socket.hpp"            // sock::create_listen_socket, sock::set_nodelay

#ifdef TINYWEB_TLS
#include "tls.hpp"               // TlsContext (可选)
#endif

class TinyWeb {
  EventLoop _loop;                              // epoll 事件循环 (核心)
  Router _router;                               // URL 路由器
  std::unique_ptr<FileServer> _file_server;     // 静态文件服务器 (可选)
  ThreadPool _pool{4};                          // 工作线程池 (4 线程)
  int _listen_fd = -1;                          // 监听 socket fd
  int _port;                                    // 监听端口
  std::string _static_dir;                      // 静态文件目录

public:
  // 构造函数: 指定监听端口
  explicit TinyWeb(int port = 8080) : _port(port) {}

  // ── 路由注册 API ──────────────────────────────────────────────────
  // route: 注册 HTTP 路由 (可带中间件)
  TinyWeb& route(const std::string& method,     // HTTP 方法
                 const std::string& path,        // URL 路径
                 HttpHandler h,                  // 业务处理器
                 std::vector<MiddlewareFunc> mw = {}) // 可选中间件
  {
    if (method == "GET") _router.get(path, std::move(h), std::move(mw));
    else if (method == "POST") _router.post(path, std::move(h), std::move(mw));
    else if (method == "PUT") _router.put(path, std::move(h), std::move(mw));
    else if (method == "DELETE") _router.del(path, std::move(h), std::move(mw));
    return *this;                                // 链式调用
  }

  // route_middleware: 注册全局中间件 (应用到所有路由)
  TinyWeb& route_middleware(MiddlewareFunc mw) {
    _router.use(std::move(mw));                  // 委托给 Router
    return *this;
  }

  // route_ws: 注册 WebSocket 路由
  TinyWeb& route_ws(const std::string& path,     // WebSocket 路径
                    Router::WsHandler h)         // WS 连接处理器
  {
    _router.ws_routes().push_back({path, std::move(h)}); // 追加到 WS 路由列表
    return *this;
  }

  // serve_static: 启用静态文件服务
  TinyWeb& serve_static(const std::string& dir) {
    _static_dir = dir;                           // 记录目录
    _file_server = std::make_unique<FileServer>(dir); // 创建文件服务器
    return *this;
  }

  // ── 启动服务器 (阻塞) ────────────────────────────────────────────
  void start() {
    _listen_fd = sock::create_listen_socket(_port); // socket + bind + listen
    if (_listen_fd < 0)
      throw std::runtime_error("Failed to listen on port " + std::to_string(_port));

    // accept_cb: 新连接到达时的回调
    _loop.add(_listen_fd, EPOLLIN, [this](uint32_t) {
      while (true) {                             // 循环 accept (直到 EAGAIN)
        sockaddr_in addr{};                      // 客户端地址
        socklen_t len = sizeof(addr);
        int fd = accept4(_listen_fd,             // 接受连接 (非阻塞 + close-on-exec)
                         (sockaddr*)&addr, &len,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {                            // 没有更多连接
          if (errno == EAGAIN || errno == EWOULDBLOCK) break; // → 退出循环
          return;                                // 致命错误
        }
        sock::set_nodelay(fd);                   // 禁用 Nagle (低延迟 HTTP)

        auto conn = std::make_shared<Connection>(fd, _loop); // 创建 Connection (shared_ptr 管理)
        conn->set_router(&_router);              // 注入路由器
        if (_file_server)                        // 如果有文件服务器?
          conn->set_file_server(_file_server.get()); // → 注入
        conn->start();                           // 注册到事件循环, 开始接收数据
      }
    });

    // ── 启动信息 ──────────────────────────────────────────────────
    println("🚀 TinyWeb Server listening on http://localhost:", _port);
    if (!_static_dir.empty())
      println("📁 Serving static files from: ", fs::absolute(_static_dir).string());
    println("\nPress Ctrl+C to stop.\n");

    _loop.run();                                 // 进入事件循环 (阻塞)
  }

  // ── 停止服务器 ────────────────────────────────────────────────────
  void stop() {
    _loop.stop();                                // 停止事件循环
    if (_listen_fd >= 0) close(_listen_fd);     // 关闭监听 socket
  }
};
