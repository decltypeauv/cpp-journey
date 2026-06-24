// ============================================================================
// tinyweb/connection.hpp — 连接管理 (RAII 生命周期)
// ============================================================================
// 每个 TCP 连接对应一个 Connection 对象
// 职责: I/O 事件处理, HTTP 解析, WebSocket 升级, 响应发送
#pragma once
#include "common.hpp"            // std::shared_ptr, std::unique_ptr, std::enable_shared_from_this, errno
#include "event_loop.hpp"        // EventLoop
#include "buffer.hpp"            // Buffer (输入/输出缓冲)
#include "http_request.hpp"      // HttpParser
#include "http_response.hpp"     // HttpResponse
#include "router.hpp"            // Router
#include "file_server.hpp"       // FileServer
#include "websocket.hpp"         // WsConnection, WebSocketFrame
#include "socket.hpp"            // sock::set_nonblocking, sock::set_nodelay

class Connection : public std::enable_shared_from_this<Connection> {
  int _fd;                             // socket 文件描述符
  EventLoop& _loop;                    // 事件循环引用 (不拥有)
  Buffer _in_buf;                      // 输入缓冲区 (从 socket 读入)
  Buffer _out_buf;                     // 输出缓冲区 (写到 socket)
  HttpParser _parser;                  // HTTP 请求解析器
  Router* _router = nullptr;           // 路由器 (不拥有)
  FileServer* _file_server = nullptr;  // 文件服务器 (不拥有)
  bool _closed = false;                // 是否已关闭 (防止重复关闭)
  bool _is_websocket = false;          // 是否已升级为 WebSocket
  std::unique_ptr<WsConnection> _ws;   // WebSocket 连接对象 (升级后创建)

public:
  // 构造函数: 绑定 fd 到事件循环
  Connection(int fd, EventLoop& loop) : _fd(fd), _loop(loop) {
    sock::set_nonblocking(fd);         // 设置非阻塞 (epoll 必需)
    sock::set_nodelay(fd);             // 禁用 Nagle (低延迟)
  }

  void set_router(Router* r) { _router = r; }        // 设置路由器
  void set_file_server(FileServer* fs) { _file_server = fs; } // 设置文件服务器
  bool is_websocket() const { return _is_websocket; } // 是否 WebSocket 模式

  // start: 注册到事件循环, 开始接收数据
  void start() {
    auto self = shared_from_this();    // 获取 shared_ptr (防止提前析构)
    _loop.add(_fd, EPOLLIN,           // 注册: 监听可读事件
      [self](uint32_t events) { self->on_event(events); });
  }

private:
  // on_event: epoll 事件回调
  void on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { // 错误 或 挂断
      close_conn(); return;              // → 直接关闭
    }

    if (events & EPOLLOUT) {             // socket 可写
      if (_is_websocket && _ws) {        // WebSocket 模式?
        ssize_t n = _ws->_out_buf.write_to(_fd); // 写 WS 输出缓冲
        if (n < 0 && errno != EAGAIN) { close_conn(); return; } // 写错误
        if (_ws->_out_buf.size() == 0)   // 数据全部写出?
          _loop.mod(_fd, EPOLLIN);       // → 切回只读模式
      } else {                           // HTTP 模式
        ssize_t n = _out_buf.write_to(_fd); // 写 HTTP 输出缓冲
        if (n < 0 && errno != EAGAIN) { close_conn(); return; }
        if (_out_buf.size() == 0) _loop.mod(_fd, EPOLLIN);
      }
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) { // socket 可读 (或对端半关闭)
      ssize_t n = _in_buf.read_from(_fd); // 读入数据
      if (n == 0 || (n < 0 && errno != EAGAIN)) { close_conn(); return; } // 对端关闭 或 错误
      if (n > 0) {                       // 有新数据
        if (_is_websocket) process_ws(); // WebSocket 模式 → 解析帧
        else process_input();            // HTTP 模式 → 解析请求
      }
    }
  }

  // process_ws: 处理 WebSocket 帧
  void process_ws() {
    auto view = _in_buf.view();          // 获取可读数据视图
    while (true) {
      size_t consumed = 0;               // 被消耗的字节数
      auto frame = WebSocketFrame::parse( // 尝试解析帧
        view.data(), view.size(), consumed);
      if (!frame) break;                 // 数据不够 → 退出循环, 等待更多数据
      if (consumed == 0) break;
      _in_buf.clear();                   // 清空缓冲 (简化: 实际应 drain consumed 字节)
      view = _in_buf.view();
      if (_ws) {
        _ws->_ws.feed_frame(*frame);     // 喂帧给 WebSocket 状态机
        if (frame->opcode == WsOpcode::PING) // PING → 自动回复 PONG
          _ws->send_pong();
      }
    }
  }

  // process_input: 处理 HTTP 请求
  void process_input() {
    auto view = _in_buf.view();          // 获取可读数据
    _parser.parse(view.data(), view.size()); // 增量解析

    if (_parser.error()) {               // 解析错误?
      send_response(HttpResponse{}.set_status(400)
        .set_json(R"({"error":"Bad Request"})"));
      return;
    }

    if (_parser.done()) {                // 解析完成?
      auto& req = _parser.request();     // 获取请求
      HttpResponse resp;
      if (_router) resp = _router->dispatch(req); // 路由分发
      else resp.set_status(404).set_json(R"({"error":"No Router"})");

      // 检测 WebSocket 升级 (内部标记 X-WS-Upgrade)
      if (resp.status == 101 && resp.headers.count("X-WS-Upgrade")) {
        resp.headers.erase("X-WS-Upgrade"); // 移除内部头
        _is_websocket = true;            // 标记为 WebSocket 模式
        _ws = std::make_unique<WsConnection>(_fd); // 创建 WS 连接
        _ws->_on_destroy = [this] { close_conn(); }; // 销毁时关闭连接
      }

      send_response(std::move(resp));    // 发送响应
      _in_buf.clear();                   // 清空输入缓冲
      _parser.reset();                   // 重置解析器 (连接复用)
    }
  }

  // send_response: 发送 HTTP/WS 响应
  void send_response(HttpResponse resp) {
    resp.write_to(_out_buf);             // 序列化到输出缓冲
    ssize_t n = _out_buf.write_to(_fd); // 尝试立即写出
    if (n < 0 && errno != EAGAIN) { close_conn(); return; }
    if (_out_buf.size() > 0)            // 还有数据未写出?
      _loop.mod(_fd, EPOLLIN | EPOLLOUT); // → 同时监听读写 (等待可写时继续)
  }

  // close_conn: 关闭连接 (idempotent)
  void close_conn() {
    if (_closed) return;                 // 已关闭 → 不重复操作
    _closed = true;
    _loop.del(_fd);                      // 从事件循环移除
    close(_fd);                          // 关闭 socket
  }
};
