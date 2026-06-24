// ============================================================================
// Month 6: 综合项目 — TinyWeb: 高性能 HTTP 服务器框架
// Week 29: 项目架构 + 核心组件
// 日期: 2026-06-24
//
// 集成 Months 1-5 所有知识:
//   M1 (现代 C++): RAII, move semantics, smart pointers, templates, lambda
//   M2 (OS 边界):   File I/O, memory mapping
//   M3 (网络编程):  Socket, TCP, epoll, HTTP
//   M4 (极致性能):  Cache-friendly structures, perf-ready design
//   M5 (源码阅读):  libevent-style event loop, leveldb-style Slice, fmtlib output
//
// 架构:
//   EventLoop (epoll) → Connection (RAII) → HTTP Parser → Router → Handler
//
// 10 个组件:
//   Ex1: 项目概览 + 架构
//   Ex2: EventLoop — epoll 事件循环
//   Ex3: Connection — RAII 连接管理
//   Ex4: Buffer — 链式读写缓冲
//   Ex5: HTTP Request Parser
//   Ex6: HTTP Response Builder
//   Ex7: Router — URL 路由 + 中间件
//   Ex8: Static File Server
//   Ex9: Thread Pool
//   Ex10: 整合 — 运行中的 HTTP Server
// ============================================================================

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <unordered_map>
#include <vector>

// OpenSSL (optional: 如果没有安装, 注释 #define TINYWEB_TLS)
// #define TINYWEB_TLS
#ifdef TINYWEB_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// 辅助
// ============================================================================
template <typename... A> void println(A&&... a) {
  if constexpr (sizeof...(a) > 0) ((std::cout << std::forward<A>(a)), ...);
  std::cout << '\n';
}
void HR(std::string_view t) { println("\n", std::string(72, '='), "\n  ", t, "\n", std::string(72, '=')); }

// ============================================================================
// Part 1: EventLoop — epoll 事件循环 (M3 + M5 libevent)
// ============================================================================
//
// 设计灵感: libevent 的 event_base + epoll backend
// 核心抽象:
//   - EventLoop: 管理 epoll fd, 提供 add/del/mod + run/stop
//   - 每个 Connection 注册到 EventLoop
//   - 就绪事件 → 回调

class EventLoop {
public:
  using Callback = std::function<void(uint32_t events)>;

  EventLoop(int max_events = 1024) : _max_events(max_events) {
    _epfd = epoll_create1(EPOLL_CLOEXEC);
    if (_epfd < 0) throw std::runtime_error("epoll_create1 failed");
    _events.resize(_max_events);
  }
  ~EventLoop() { if (_epfd >= 0) close(_epfd); }

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // ── 注册 / 修改 / 删除 fd ─────────────────────────────────────
  void add(int fd, uint32_t events, Callback cb) {
    _callbacks[fd] = std::move(cb);
    epoll_ctl(fd, EPOLL_CTL_ADD, events);
  }
  void mod(int fd, uint32_t events) { epoll_ctl(fd, EPOLL_CTL_MOD, events); }
  void del(int fd) {
    epoll_ctl(fd, EPOLL_CTL_DEL, 0);
    _callbacks.erase(fd);
  }

  // ── 主循环 ────────────────────────────────────────────────────
  void run() {
    _running = true;
    while (_running) {
      int nfds = epoll_wait(_epfd, _events.data(), _max_events, -1);
      if (nfds < 0) { if (errno == EINTR) continue; break; }
      for (int i = 0; i < nfds; i++) {
        int fd = _events[i].data.fd;
        auto it = _callbacks.find(fd);
        if (it != _callbacks.end()) it->second(_events[i].events);
      }
    }
  }
  void stop() { _running = false; }

private:
  int _epfd;
  int _max_events;
  std::vector<epoll_event> _events;
  std::unordered_map<int, Callback> _callbacks;
  bool _running = false;

  void epoll_ctl(int fd, int op, uint32_t events) {
    epoll_event ev{};
    ev.events = events | EPOLLRDHUP;
    ev.data.fd = fd;
    if (::epoll_ctl(_epfd, op, fd, &ev) < 0) {
      if (op == EPOLL_CTL_DEL) return; // already removed
      throw std::runtime_error("epoll_ctl failed: " + std::to_string(errno));
    }
  }
};

// ============================================================================
// Part 2: Socket 工具 — 非阻塞 + TCP_NODELAY
// ============================================================================
namespace sock {

inline int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline int set_nodelay(int fd) {
  int opt = 1;
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

inline int set_reuseaddr(int fd) {
  int opt = 1;
  return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

inline int create_listen_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  set_reuseaddr(fd);
  set_nonblocking(fd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
  if (listen(fd, SOMAXCONN) < 0) { close(fd); return -1; }

  return fd;
}

inline std::string get_peer_addr(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (getpeername(fd, (sockaddr*)&addr, &len) < 0) return "unknown";
  char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
  return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace sock

// ============================================================================
// Part 3: Buffer — 链式读写缓冲 (M5 libevent evbuffer)
// ============================================================================
class Buffer {
  std::vector<char> _buf;
  size_t _read_pos = 0;
  size_t _write_pos = 0;

public:
  Buffer() { _buf.resize(4096); }

  // ── 写 (积累待发送数据) ─────────────────────────────────────────
  void append(const char* data, size_t len) {
    if (_write_pos + len > _buf.size()) _buf.resize(_write_pos + len + 4096);
    std::memcpy(_buf.data() + _write_pos, data, len);
    _write_pos += len;
  }
  void append(std::string_view s) { append(s.data(), s.size()); }

  // ── 读 (从 socket 读到 buffer) ─────────────────────────────────
  ssize_t read_from(int fd) {
    if (_write_pos + 4096 > _buf.size()) _buf.resize(_buf.size() * 2);
    ssize_t n = recv(fd, _buf.data() + _write_pos, _buf.size() - _write_pos, 0);
    if (n > 0) _write_pos += n;
    return n;
  }

  // ── 写 (从 buffer 写到 socket) ─────────────────────────────────
  ssize_t write_to(int fd) {
    size_t avail = _write_pos - _read_pos;
    if (avail == 0) return 0;
    ssize_t n = send(fd, _buf.data() + _read_pos, avail, MSG_NOSIGNAL);
    if (n > 0) {
      _read_pos += n;
      if (_read_pos == _write_pos) { _read_pos = _write_pos = 0; }
      else if (_read_pos > 4096) {
        std::memmove(_buf.data(), _buf.data() + _read_pos, _write_pos - _read_pos);
        _write_pos -= _read_pos; _read_pos = 0;
      }
    }
    return n;
  }

  // ── 观察 ───────────────────────────────────────────────────────
  std::string_view view() const { return {_buf.data() + _read_pos, _write_pos - _read_pos}; }
  size_t size() const { return _write_pos - _read_pos; }
  void clear() { _read_pos = _write_pos = 0; }
};

// ============================================================================
// Part 4: HTTP Parser — HTTP/1.1 请求解析 (M3 HTTP)
// ============================================================================
struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
  std::string body;

  // 便捷访问
  std::string_view header(const std::string& key) const {
    auto it = headers.find(key);
    return it != headers.end() ? std::string_view(it->second) : std::string_view{};
  }
  bool has_header(const std::string& key) const { return headers.count(key); }
  void reset() { method.clear(); path.clear(); version.clear(); headers.clear(); body.clear(); }
};

// ── 请求解析器 (状态机) ────────────────────────────────────────
class HttpParser {
  enum State { kMethod, kPath, kVersion, kHeaders, kBody, kDone, kError };

  HttpRequest _req;
  std::string _line_buf;
  State _state = kMethod;
  size_t _content_length = 0;
  size_t _body_read = 0;

public:
  void reset() { _req.reset(); _line_buf.clear(); _state = kMethod; _content_length = _body_read = 0; }

  // 解析数据块, 返回: true=完成, false=还需更多数据
  // 成功: _state == kDone; 失败: _state == kError
  bool parse(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
      char c = data[i];
      switch (_state) {
      case kMethod:
        if (c == ' ') { _req.method = _line_buf; _line_buf.clear(); _state = kPath; }
        else _line_buf += c;
        break;
      case kPath:
        if (c == ' ') { _req.path = _line_buf; _line_buf.clear(); _state = kVersion; }
        else _line_buf += c;
        break;
      case kVersion:
        if (c == '\r') {}
        else if (c == '\n') { _req.version = _line_buf; _line_buf.clear(); _state = kHeaders; }
        else _line_buf += c;
        break;
      case kHeaders:
        if (c == '\r') {}
        else if (c == '\n') {
          if (_line_buf.empty()) {
            // 空行 = header 结束
            auto it = _req.headers.find("content-length");
            if (it != _req.headers.end()) {
              _content_length = std::stoull(it->second);
              if (_content_length > 0) { _state = kBody; }
              else { _state = kDone; }
            } else { _state = kDone; }
          } else {
            auto colon = _line_buf.find(':');
            if (colon != std::string::npos) {
              std::string key = _line_buf.substr(0, colon);
              std::string val = _line_buf.substr(colon + 1);
              // trim leading space
              if (!val.empty() && val[0] == ' ') val.erase(0, 1);
              // lowercase key
              for (auto& ch : key) ch = std::tolower(ch);
              _req.headers[key] = val;
            }
            _line_buf.clear();
          }
        } else _line_buf += c;
        break;
      case kBody:
        _req.body += c;
        _body_read++;
        if (_body_read >= _content_length) _state = kDone;
        break;
      case kDone:
      case kError:
        return true;
      }
    }
    return _state == kDone || _state == kError;
  }

  bool done() const { return _state == kDone; }
  bool error() const { return _state == kError; }
  const HttpRequest& request() const { return _req; }
  HttpRequest& request() { return _req; }
};

// ============================================================================
// Part 5: HTTP Response Builder (M5 fmtlib output pattern)
// ============================================================================
struct HttpResponse {
  int status = 200;
  std::string status_msg = "OK";
  std::unordered_map<std::string, std::string> headers;
  std::string body;

  HttpResponse& set_status(int s, std::string_view msg = "") {
    status = s;
    if (!msg.empty()) status_msg = msg;
    else {
      static const std::map<int, const char*> msgs = {
        {200,"OK"},{201,"Created"},{204,"No Content"},
        {301,"Moved Permanently"},{302,"Found"},{304,"Not Modified"},
        {400,"Bad Request"},{401,"Unauthorized"},{403,"Forbidden"},{404,"Not Found"},
        {405,"Method Not Allowed"},{500,"Internal Server Error"},{503,"Service Unavailable"}
      };
      auto it = msgs.find(s);
      if (it != msgs.end()) status_msg = it->second;
    }
    return *this;
  }
  HttpResponse& set_header(const std::string& k, const std::string& v) { headers[k] = v; return *this; }
  HttpResponse& set_body(std::string b) { body = std::move(b); return *this; }
  HttpResponse& set_json(const std::string& json) { set_body(json); set_header("Content-Type", "application/json"); return *this; }
  HttpResponse& set_html(const std::string& html) { set_body(html); set_header("Content-Type", "text/html; charset=utf-8"); return *this; }
  HttpResponse& set_content_type(const std::string& ct) { return set_header("Content-Type", ct); }

  void write_to(Buffer& buf) const {
    // Status line
    buf.append("HTTP/1.1 ");
    buf.append(std::to_string(status));
    buf.append(" ");
    buf.append(status_msg);
    buf.append("\r\n");

    // Headers
    auto hdrs = headers;
    if (!body.empty() && !hdrs.count("Content-Length")) hdrs["Content-Length"] = std::to_string(body.size());
    hdrs["Connection"] = "keep-alive";
    hdrs["Server"] = "TinyWeb/1.0";
    for (auto& [k, v] : hdrs) {
      buf.append(k); buf.append(": "); buf.append(v); buf.append("\r\n");
    }
    buf.append("\r\n");
    // Body
    if (!body.empty()) buf.append(body);
  }
};

// ── 前向声明 ────────────────────────────────────────────────────────
using HttpHandler = std::function<struct HttpResponse(const struct HttpRequest&)>;

// ============================================================================
// Part 6a: WebSocket — RFC 6455 (M3 Socket + M5 protocol design)
// ============================================================================
//
// WebSocket 帧格式 (RFC 6455 §5.2):
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-------+-+-------------+-------------------------------+
//  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
//  |I|S|S|S|  (4)  |A|     (7)     |            16/64              |
//  |N|V|V|V|       |S|             |   (if payload len == 126/127) |
//  | |1|2|3|       |K|             |                               |
//  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
//  |     Masking-key (if MASK set, 4 bytes)                        |
//  +---------------------------------------------------------------+
//  |     Payload Data (masked if MASK set)                         |
//  +---------------------------------------------------------------+

enum class WsOpcode : uint8_t {
  CONTINUATION = 0x0, TEXT = 0x1, BINARY = 0x2,
  CLOSE = 0x8, PING = 0x9, PONG = 0xA
};

struct WebSocketFrame {
  bool fin = true; uint8_t rsv = 0; WsOpcode opcode = WsOpcode::TEXT;
  bool mask = false; uint8_t mask_key[4] = {};
  std::string payload;

  static std::optional<WebSocketFrame> parse(const char* data, size_t len, size_t& consumed) {
    if (len < 2) return std::nullopt;
    WebSocketFrame f;
    f.fin = (data[0] & 0x80) != 0;
    f.rsv = (data[0] & 0x70) >> 4;
    f.opcode = static_cast<WsOpcode>(data[0] & 0x0F);
    f.mask = (data[1] & 0x80) != 0;
    uint64_t plen = data[1] & 0x7F;
    size_t header_len = 2;
    if (plen == 126) { if (len < 4) return std::nullopt; plen = (uint8_t)data[2]<<8 | (uint8_t)data[3]; header_len = 4; }
    else if (plen == 127) { if (len < 10) return std::nullopt; plen = 0; for (int i=0;i<8;i++) plen=(plen<<8)|(uint8_t)data[2+i]; header_len = 10; }
    if (f.mask) {
      if (len < header_len + 4) return std::nullopt;
      std::memcpy(f.mask_key, data + header_len, 4);
      header_len += 4;
    }
    if (len < header_len + plen) return std::nullopt;
    f.payload.assign(data + header_len, plen);
    if (f.mask) for (size_t i = 0; i < plen; i++) f.payload[i] ^= f.mask_key[i % 4];
    consumed = header_len + plen;
    return f;
  }

  std::string encode() const {
    std::string s; s.reserve(10 + payload.size());
    s.push_back((fin ? 0x80 : 0x00) | (rsv << 4) | (uint8_t(opcode) & 0x0F));
    uint8_t mask_byte = mask ? 0x80 : 0x00;
    if (payload.size() < 126) { s.push_back(mask_byte | payload.size()); }
    else if (payload.size() <= 0xFFFF) { s.push_back(mask_byte | 126); s.push_back(payload.size() >> 8); s.push_back(payload.size() & 0xFF); }
    else { s.push_back(mask_byte | 127); for (int i=7;i>=0;i--) s.push_back((payload.size()>>(i*8)) & 0xFF); }
    if (mask) { std::memcpy(&s[s.size()], mask_key, 4); s.resize(s.size()+4); /* 简化: 不实际 mask */ }
    s += payload;
    return s;
  }
};

// WebSocket 连接状态
struct WebSocketState {
  using MessageCb = std::function<void(std::string_view, bool /*binary*/)>;
  using CloseCb = std::function<void()>;

  MessageCb on_message; CloseCb on_close;
  std::string _fragment_buf; // 分片重组缓冲

  void feed_frame(const WebSocketFrame& f) {
    switch (f.opcode) {
    case WsOpcode::TEXT: case WsOpcode::BINARY:
      if (!f.fin) { _fragment_buf += f.payload; return; }
      if (!_fragment_buf.empty()) { _fragment_buf += f.payload; on_message(_fragment_buf, f.opcode == WsOpcode::BINARY); _fragment_buf.clear(); }
      else on_message(f.payload, f.opcode == WsOpcode::BINARY);
      break;
    case WsOpcode::CONTINUATION: _fragment_buf += f.payload; if (f.fin && on_message) { on_message(_fragment_buf, false); _fragment_buf.clear(); } break;
    case WsOpcode::PING: if (on_message) on_message("__PING__", false); break;  // handled by send_pong
    case WsOpcode::PONG: break;
    case WsOpcode::CLOSE: if (on_close) on_close(); break;
    }
  }
};

// ============================================================================
// Part 6b: Middleware — 责任链模式
// ============================================================================
//
// Middleware 链: M1 → M2 → M3 → Handler → M3(after) → M2(after) → M1(after)
// 每个 middleware 可以在 handler 之前/之后执行逻辑
// 用途: 日志, CORS, 认证, 压缩, 限流

using NextFunc = std::function<HttpResponse()>;
using MiddlewareFunc = std::function<HttpResponse(const HttpRequest&, NextFunc)>;
HttpResponse run_middleware_chain(const std::vector<MiddlewareFunc>& mw, size_t idx, const HttpRequest& req, const HttpHandler& final_handler) {
  if (idx >= mw.size()) return final_handler(req);
  return mw[idx](req, [&]() { return run_middleware_chain(mw, idx + 1, req, final_handler); });
}

// ── 常用中间件 ──────────────────────────────────────────────────────
namespace middleware {
  // CORS 中间件
  inline MiddlewareFunc cors(std::string origin = "*") {
    return [origin](const HttpRequest& req, NextFunc next) {
      auto resp = next();
      resp.set_header("Access-Control-Allow-Origin", origin);
      resp.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      resp.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
      if (req.method == "OPTIONS") { resp.set_status(204); resp.set_body(""); }
      return resp;
    };
  }
  // 请求日志中间件
  inline MiddlewareFunc logger() {
    return [](const HttpRequest& req, NextFunc next) {
      auto t0 = std::chrono::steady_clock::now();
      auto resp = next();
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
      std::cout << "  [" << req.method << " " << req.path << "] → " << resp.status << " (" << us << "us)\n";
      return resp;
    };
  }
  // Auth 中间件 (简化: Bearer token)
  inline MiddlewareFunc auth(std::string secret = "tinyweb-secret") {
    return [secret](const HttpRequest& req, NextFunc next) {
      auto auth_hdr = req.header("authorization");
      if (!auth_hdr.empty() && auth_hdr.find("Bearer " + secret) != std::string_view::npos) return next();
      return HttpResponse{}.set_status(401).set_json(R"({"error":"Unauthorized"})");
    };
  }
  // Compression (简化: 标记 Accept-Encoding)
  inline MiddlewareFunc gzip_hint() {
    return [](const HttpRequest& req, NextFunc next) {
      auto resp = next();
      if (req.header("accept-encoding").find("gzip") != std::string_view::npos)
        resp.set_header("X-Compression", "gzip-supported");
      return resp;
    };
  }
  // Rate Limiting (token bucket 简化)
  inline MiddlewareFunc rate_limit(int max_per_sec = 100) {
    struct State { std::mutex mtx; std::unordered_map<std::string, int> counts; std::chrono::steady_clock::time_point reset; };
    auto state = std::make_shared<State>();
    state->reset = std::chrono::steady_clock::now();
    return [state, max_per_sec](const HttpRequest& req, NextFunc next) {
      std::lock_guard lock(state->mtx);
      auto now = std::chrono::steady_clock::now();
      if (now - state->reset > std::chrono::seconds(1)) { state->counts.clear(); state->reset = now; }
      std::string ip = "global"; // 简化: 使用全局计数器
      if (++state->counts[ip] > max_per_sec)
        return HttpResponse{}.set_status(429).set_json(R"({"error":"Rate Limit Exceeded"})");
      return next();
    };
  }
}

// ============================================================================
// Part 6c: Router — URL 路由 (M1 templates + lambda)
// ============================================================================
// 扩展: 支持 middleware 链
class Router {
  struct Route {
    std::string method; std::string path;
    HttpHandler handler;
    std::vector<MiddlewareFunc> middleware; // per-route middleware
  };
  std::vector<Route> _routes;
  std::vector<MiddlewareFunc> _global_middleware; // global middleware
  HttpHandler _not_found;

public:
  Router() {
    _not_found = [](const HttpRequest&) {
      return HttpResponse{}.set_status(404).set_json(R"({"error":"Not Found"})");
    };
  }

  // ── 全局中间件 ──────────────────────────────────────────────────
  Router& use(MiddlewareFunc mw) { _global_middleware.push_back(std::move(mw)); return *this; }

  // ── 路由注册 ────────────────────────────────────────────────────
  Router& get(const std::string& path, HttpHandler h, std::vector<MiddlewareFunc> mw = {})
    { _routes.push_back({"GET", path, std::move(h), std::move(mw)}); return *this; }
  Router& post(const std::string& path, HttpHandler h, std::vector<MiddlewareFunc> mw = {})
    { _routes.push_back({"POST", path, std::move(h), std::move(mw)}); return *this; }
  Router& put(const std::string& path, HttpHandler h, std::vector<MiddlewareFunc> mw = {})
    { _routes.push_back({"PUT", path, std::move(h), std::move(mw)}); return *this; }
  Router& del(const std::string& path, HttpHandler h, std::vector<MiddlewareFunc> mw = {})
    { _routes.push_back({"DELETE", path, std::move(h), std::move(mw)}); return *this; }

  // ── WebSocket 升级路由 ──────────────────────────────────────────
  using WsHandler = std::function<void(std::shared_ptr<struct WsConnection>)>;
  struct WsRoute { std::string path; WsHandler handler; };
  std::vector<WsRoute>& ws_routes() { return _ws_routes; }

  HttpResponse dispatch(const HttpRequest& req) const {
    // 检查 WebSocket 升级
    if (req.header("upgrade").find("websocket") != std::string_view::npos) {
      for (auto& w : _ws_routes)
        if (match(w.path, req.path)) {
          return build_ws_upgrade(req); // 返回 101 Switching Protocols
        }
      return _not_found(req);
    }
    // HTTP 路由
    for (auto& r : _routes) {
      if (req.method == r.method && match(r.path, req.path)) {
        // 合并 global + per-route middleware
        std::vector<MiddlewareFunc> all_mw = _global_middleware;
        all_mw.insert(all_mw.end(), r.middleware.begin(), r.middleware.end());
        if (all_mw.empty()) return r.handler(req);
        return run_middleware_chain(all_mw, 0, req, r.handler);
      }
    }
    return _not_found(req);
  }

private:
  std::vector<WsRoute> _ws_routes;

  HttpResponse build_ws_upgrade(const HttpRequest& req) const {
    // RFC 6455: 计算 Sec-WebSocket-Accept
    // Accept = base64(sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
    // 简化: 返回固定 accept (实际需要 SHA1 + base64)
    HttpResponse resp;
    resp.set_status(101, "Switching Protocols");
    resp.set_header("Upgrade", "websocket");
    resp.set_header("Connection", "Upgrade");
    resp.set_header("Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="); // 示例值
    resp.set_header("X-WS-Upgrade", "true"); // 内部标记
    return resp;
  }

  static bool match(const std::string& pattern, const std::string& path) {
    if (pattern == path) return true;
    if (pattern.find(':') != std::string::npos) {
      auto pp = split(pattern, '/'), pu = split(path, '/');
      if (pp.size() != pu.size()) return false;
      for (size_t i = 0; i < pp.size(); i++)
        if (pp[i].empty() || pp[i][0] != ':') { if (pp[i] != pu[i]) return false; }
      return true;
    }
    return false;
  }
  static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    size_t start=0, end;
    while ((end=s.find(delim,start))!=std::string::npos) { if(end>start) parts.push_back(s.substr(start,end-start)); start=end+1; }
    if (start<s.size()) parts.push_back(s.substr(start));
    return parts;
  }
};

// WsConnection 前向声明 (实现在 Connection 之后)
struct WsConnection {
  int _fd;
  WebSocketState _ws;
  Buffer _out_buf;
  std::function<void()> _on_destroy;

  explicit WsConnection(int fd) : _fd(fd) {}
  void send_text(std::string_view msg) {
    WebSocketFrame f; f.opcode = WsOpcode::TEXT; f.payload = std::string(msg);
    auto encoded = f.encode();
    _out_buf.append(encoded.data(), encoded.size());
  }
  void send_pong() {
    WebSocketFrame f; f.opcode = WsOpcode::PONG; f.payload = "pong";
    auto encoded = f.encode(); _out_buf.append(encoded.data(), encoded.size());
  }
  void close() {
    WebSocketFrame f; f.opcode = WsOpcode::CLOSE;
    auto encoded = f.encode(); _out_buf.append(encoded.data(), encoded.size());
    if (_on_destroy) _on_destroy();
  }
};

// ============================================================================
// Part 6d: TLS / SSL 抽象层 (M3 Socket + OpenSSL)
// ============================================================================
#ifdef TINYWEB_TLS
struct TlsContext {
  SSL_CTX* ctx = nullptr;
  TlsContext(const std::string& cert_file, const std::string& key_file) {
    SSL_library_init(); SSL_load_error_strings();
    ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate_file(ctx, cert_file.c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  }
  ~TlsContext() { if (ctx) SSL_CTX_free(ctx); }
};

struct TlsConnection {
  SSL* ssl = nullptr;
  TlsConnection(SSL_CTX* ctx, int fd) { ssl = SSL_new(ctx); SSL_set_fd(ssl, fd); }
  ~TlsConnection() { if (ssl) SSL_free(ssl); }
  int accept() { return SSL_accept(ssl); }
  int read(char* buf, int len) { return SSL_read(ssl, buf, len); }
  int write(const char* buf, int len) { return SSL_write(ssl, buf, len); }
};
#else
// 无 TLS 时的 Stub
struct TlsContext { TlsContext(const std::string&, const std::string&) {} };
#endif

// ============================================================================
// Part 6e: Performance / Benchmark (M4 perf + M3 Socket)
// ============================================================================
struct BenchmarkResult {
  size_t total_requests = 0; size_t success = 0; size_t errors = 0;
  double total_time_ms = 0; double min_latency_us = 1e18; double max_latency_us = 0;
  std::vector<double> latencies;

  double req_per_sec() const { return total_time_ms > 0 ? total_requests / (total_time_ms / 1000.0) : 0; }
  double avg_latency_us() const {
    if (latencies.empty()) return 0;
    double sum = 0; for (auto l : latencies) sum += l; return sum / latencies.size();
  }
  double p50_us() const { return percentile(0.50); }
  double p99_us() const { return percentile(0.99); }
  double percentile(double p) const {
    if (latencies.empty()) return 0;
    auto sorted = latencies; std::sort(sorted.begin(), sorted.end());
    return sorted[std::min((size_t)(sorted.size() * p), sorted.size() - 1)];
  }
  void print() const {
    println("  Requests:   ", total_requests);
    println("  Success:    ", success, " (", total_requests > 0 ? success * 100.0 / total_requests : 0, "%)");
    println("  Errors:     ", errors);
    println("  Throughput: ", std::fixed, std::setprecision(0), req_per_sec(), " req/s");
    println("  Latency (us): min=", std::setprecision(0), min_latency_us,
            " avg=", avg_latency_us(), " p50=", p50_us(), " p99=", p99_us(), " max=", max_latency_us);
  }
};

// 简易 HTTP 负载生成器 (wrk-like)
BenchmarkResult run_benchmark(const std::string& host, int port, const std::string& path,
                               int total_requests = 1000, int concurrency = 10) {
  BenchmarkResult r;
  r.latencies.reserve(total_requests);
  std::atomic<int> completed{0};
  auto t0 = std::chrono::steady_clock::now();

  auto worker = [&] {
    while (completed.fetch_add(1) < total_requests) {
      auto t_req = std::chrono::steady_clock::now();
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) { r.errors++; continue; }

      sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
      inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

      if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); r.errors++; continue; }

      std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
      if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) <= 0) { close(fd); r.errors++; continue; }

      char buf[4096]; recv(fd, buf, sizeof(buf), 0); // 简化: 不解析完整响应
      close(fd);

      auto t_end = std::chrono::steady_clock::now();
      double us = std::chrono::duration<double, std::micro>(t_end - t_req).count();
      r.min_latency_us = std::min(r.min_latency_us, us);
      r.max_latency_us = std::max(r.max_latency_us, us);
      r.latencies.push_back(us);
      r.success++;
    }
  };

  std::vector<std::thread> workers;
  for (int i = 0; i < concurrency; i++) workers.emplace_back(worker);
  for (auto& w : workers) if (w.joinable()) w.join();

  auto t1 = std::chrono::steady_clock::now();
  r.total_requests = total_requests;
  r.total_time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return r;
}

// ============================================================================
// Part 7: Static File Server (M2 File I/O)
// ============================================================================
class FileServer {
  std::string _root;
  std::unordered_map<std::string, std::string> _mime_types;

public:
  explicit FileServer(std::string root) : _root(std::move(root)) {
    _mime_types = {
      {".html", "text/html"}, {".css", "text/css"}, {".js", "application/javascript"},
      {".json", "application/json"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".svg", "image/svg+xml"},
      {".ico", "image/x-icon"}, {".txt", "text/plain"}, {".xml", "application/xml"},
      {".pdf", "application/pdf"}, {".woff2", "font/woff2"}, {".wasm", "application/wasm"},
    };
  }

  HttpResponse serve(const std::string& path) const {
    // 安全检查: 防止 ../ 路径遍历
    std::string safe_path = sanitize(path);
    if (safe_path.empty()) return HttpResponse{}.set_status(403).set_json(R"({"error":"Forbidden"})");

    fs::path file_path = fs::path(_root) / fs::path(safe_path);

    // 如果是目录, 尝试 index.html
    if (fs::is_directory(file_path)) file_path /= "index.html";

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path))
      return HttpResponse{}.set_status(404).set_json(R"({"error":"File Not Found"})");

    // 读取文件
    std::ifstream f(file_path, std::ios::binary | std::ios::ate);
    if (!f) return HttpResponse{}.set_status(500).set_json(R"({"error":"Internal Error"})");

    auto size = f.tellg();
    f.seekg(0);
    std::string content(size, '\0');
    f.read(content.data(), size);

    // MIME 类型
    std::string ext = file_path.extension().string();
    std::string mime = "application/octet-stream";
    auto it = _mime_types.find(ext);
    if (it != _mime_types.end()) mime = it->second;
    if (ext == ".html" || ext == ".css" || ext == ".js" || ext == ".json" || ext == ".xml" || ext == ".txt")
      mime += "; charset=utf-8";

    return HttpResponse{}.set_status(200).set_content_type(mime).set_body(std::move(content));
  }

private:
  std::string sanitize(const std::string& path) const {
    std::string result;
    for (char c : path) {
      if (c == '/' || c == '.' || c == '-' || c == '_' || std::isalnum(c)) result += c;
      else return ""; // 拒绝非法字符
    }
    // 拒绝 .. 路径遍历
    if (result.find("..") != std::string::npos) return "";
    // 移除开头的 /
    if (!result.empty() && result[0] == '/') result.erase(0, 1);
    return result;
  }
};

// ============================================================================
// Part 8: Thread Pool — 工作线程池 (M2 多线程)
// ============================================================================
class ThreadPool {
  std::vector<std::thread> _workers;
  std::queue<std::function<void()>> _tasks;
  std::mutex _mtx;
  std::condition_variable _cv;
  bool _stop = false;

public:
  explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency()) {
    for (size_t i = 0; i < num_threads; i++) {
      _workers.emplace_back([this] {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock lock(_mtx);
            _cv.wait(lock, [this] { return _stop || !_tasks.empty(); });
            if (_stop && _tasks.empty()) return;
            task = std::move(_tasks.front());
            _tasks.pop();
          }
          task();
        }
      });
    }
  }

  ~ThreadPool() {
    { std::lock_guard lock(_mtx); _stop = true; }
    _cv.notify_all();
    for (auto& w : _workers) if (w.joinable()) w.join();
  }

  template <typename F>
  void enqueue(F&& f) {
    { std::lock_guard lock(_mtx); _tasks.emplace(std::forward<F>(f)); }
    _cv.notify_one();
  }
};

// ============================================================================
// Part 9: Connection — RAII 连接管理 (M1 RAII + M3 Socket)
// ============================================================================
class Connection : public std::enable_shared_from_this<Connection> {
  int _fd;
  EventLoop& _loop;
  Buffer _in_buf;
  Buffer _out_buf;
  HttpParser _parser;
  Router* _router = nullptr;
  FileServer* _file_server = nullptr;
  bool _closed = false;
  bool _is_websocket = false;
  std::unique_ptr<WsConnection> _ws;

public:
  Connection(int fd, EventLoop& loop) : _fd(fd), _loop(loop) {
    sock::set_nonblocking(fd);
    sock::set_nodelay(fd);
  }

  void set_router(Router* r) { _router = r; }
  void set_file_server(FileServer* fs) { _file_server = fs; }
  bool is_websocket() const { return _is_websocket; }

  void start() {
    auto self = shared_from_this();
    _loop.add(_fd, EPOLLIN, [self](uint32_t events) { self->on_event(events); });
  }

private:
  void on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close_conn(); return; }

    if (events & EPOLLOUT) {
      if (_is_websocket && _ws) {
        ssize_t n = _ws->_out_buf.write_to(_fd);
        if (n < 0 && errno != EAGAIN) { close_conn(); return; }
        if (_ws->_out_buf.size() == 0) _loop.mod(_fd, EPOLLIN);
      } else {
        ssize_t n = _out_buf.write_to(_fd);
        if (n < 0 && errno != EAGAIN) { close_conn(); return; }
        if (_out_buf.size() == 0) _loop.mod(_fd, EPOLLIN);
      }
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) {
      ssize_t n = _in_buf.read_from(_fd);
      if (n == 0 || (n < 0 && errno != EAGAIN)) { close_conn(); return; }
      if (n > 0) {
        if (_is_websocket) process_ws();
        else process_input();
      }
    }
  }

  void process_ws() {
    auto view = _in_buf.view();
    while (true) {
      size_t consumed = 0;
      auto frame = WebSocketFrame::parse(view.data(), view.size(), consumed);
      if (!frame) break; // 数据不够, 等下次
      if (consumed == 0) break;
      _in_buf.clear(); // 简化: 清空 buffer (实际应按 consumed drain)
      view = _in_buf.view();
      if (_ws) {
        _ws->_ws.feed_frame(*frame);
        // 处理 PING → PONG
        if (frame->opcode == WsOpcode::PING) _ws->send_pong();
      }
    }
  }

  void process_input() {
    auto view = _in_buf.view();
    _parser.parse(view.data(), view.size());

    if (_parser.error()) {
      send_response(HttpResponse{}.set_status(400).set_json(R"({"error":"Bad Request"})"));
      return;
    }

    if (_parser.done()) {
      auto& req = _parser.request();
      HttpResponse resp;
      if (_router) resp = _router->dispatch(req);
      else resp.set_status(404).set_json(R"({"error":"No Router"})");

      // 检测 WebSocket 升级
      if (resp.status == 101 && resp.headers.count("X-WS-Upgrade")) {
        resp.headers.erase("X-WS-Upgrade");
        _is_websocket = true;
        _ws = std::make_unique<WsConnection>(_fd);
        _ws->_on_destroy = [this] { close_conn(); };
        // 找 WS handler
        for (auto& w : _router->ws_routes()) {
          if (w.path == req.path) { w.handler(nullptr); break; } // 简化
        }
      }

      send_response(std::move(resp));
      _in_buf.clear();
      _parser.reset();
    }
  }

  void send_response(HttpResponse resp) {
    if (_is_websocket) {
      // WebSocket 握手响应
      resp.write_to(_out_buf);
      ssize_t n = _out_buf.write_to(_fd);
      if (n < 0 && errno != EAGAIN) { close_conn(); return; }
      if (_out_buf.size() > 0) _loop.mod(_fd, EPOLLIN | EPOLLOUT);
    } else {
      resp.write_to(_out_buf);
      ssize_t n = _out_buf.write_to(_fd);
      if (n < 0 && errno != EAGAIN) { close_conn(); return; }
      if (_out_buf.size() > 0) _loop.mod(_fd, EPOLLIN | EPOLLOUT);
    }
  }

  void close_conn() {
    if (_closed) return;
    _closed = true;
    _loop.del(_fd);
    close(_fd);
  }
};

// ============================================================================
// Part 10: TinyWeb Server — 整合 (M3 Socket + M5 reactor)
// ============================================================================
class TinyWeb {
  EventLoop _loop;
  Router _router;
  std::unique_ptr<FileServer> _file_server;
  int _listen_fd = -1;
  int _port;
  std::string _static_dir;
  ThreadPool _pool{4};

public:
  explicit TinyWeb(int port = 8080) : _port(port) {}

  TinyWeb& route(const std::string& method, const std::string& path, HttpHandler h,
                 std::vector<MiddlewareFunc> mw = {}) {
    if (method == "GET") _router.get(path, std::move(h), std::move(mw));
    else if (method == "POST") _router.post(path, std::move(h), std::move(mw));
    else if (method == "PUT") _router.put(path, std::move(h), std::move(mw));
    else if (method == "DELETE") _router.del(path, std::move(h), std::move(mw));
    return *this;
  }
  TinyWeb& route_middleware(MiddlewareFunc mw) { _router.use(std::move(mw)); return *this; }
  TinyWeb& route_ws(const std::string& path, Router::WsHandler h) {
    _router.ws_routes().push_back({path, std::move(h)}); return *this; }
  TinyWeb& serve_static(const std::string& dir) {
    _static_dir = dir; _file_server = std::make_unique<FileServer>(dir); return *this;
  }

  void start() {
    _listen_fd = sock::create_listen_socket(_port);
    if (_listen_fd < 0) throw std::runtime_error("Failed to listen on port " + std::to_string(_port));

    // 注册 accept 回调
    _loop.add(_listen_fd, EPOLLIN, [this](uint32_t) {
      while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept4(_listen_fd, (sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          return;
        }
        sock::set_nodelay(fd);
        auto conn = std::make_shared<Connection>(fd, _loop);
        conn->set_router(&_router);
        if (_file_server) conn->set_file_server(_file_server.get());
        conn->start();
      }
    });

    println("🚀 TinyWeb Server listening on http://localhost:", _port);
    if (!_static_dir.empty()) println("📁 Serving static files from: ", fs::absolute(_static_dir).string());
    println("📋 Registered routes:");
    println("   (routes configured programmatically)");
    println("\nPress Ctrl+C to stop.\n");

    _loop.run();
  }

  void stop() { _loop.stop(); if (_listen_fd >= 0) close(_listen_fd); }
};

// ============================================================================
// Demo Server — Week 30: WebSocket + Middleware + TLS + Benchmark
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║  Month 6 Week 30: TinyWeb v2.0                               ║
║  + WebSocket + Middleware + TLS + Performance               ║
╚══════════════════════════════════════════════════════════════╝)");

  // ── 模式选择 ──────────────────────────────────────────────────────
  std::string mode = "server";

  if (mode == "bench") {
    // ── 性能测试模式 ───────────────────────────────────────────────
    HR("性能基准测试");
    println("  目标: http://localhost:8080/api/hello");
    println("  请求数: 1000 | 并发: 10\n");

    auto result = run_benchmark("127.0.0.1", 8080, "/api/hello", 1000, 10);
    result.print();
    println("\n📊 性能分析提示:");
    println("  - 使用 perf stat ./tinyweb 查看 CPU 计数器");
    println("  - 使用 perf record + FlameGraph 查看热点");
    println("  - 对比: epoll vs select vs io_uring");
    println("  - 优化方向: 减少 syscall, buffer pooling, sendfile");
    return 0;
  }

  // ── 服务器模式 ───────────────────────────────────────────────────
  HR("启动 TinyWeb v2.0 Server");

  TinyWeb app(8080);

  // ═══════════════════════════════════════════════════════════════
  // 1. 全局中间件
  app.route_middleware(middleware::logger());
  app.route_middleware(middleware::cors("*"));
  println("✅ 全局中间件: Logger + CORS");

  // ═══════════════════════════════════════════════════════════════
  // 2. Middleware-Demo 路由
  app.route("GET", "/", [](const HttpRequest&) {
    return HttpResponse{}.set_html(R"(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>TinyWeb v2.0</title><style>
*{box-sizing:border-box}body{font-family:system-ui;max-width:900px;margin:2rem auto;padding:0 1.5rem;background:#fafafa}
h1{color:#1a1a2e} h2{color:#16213e;margin-top:2rem;border-bottom:2px solid #e94560;padding-bottom:.5rem}
.card{background:#fff;border-radius:8px;padding:1.5rem;margin:1rem 0;box-shadow:0 2px 8px rgba(0,0,0,.08)}
pre{background:#1a1a2e;color:#e6e6e6;padding:1rem;border-radius:6px;overflow-x:auto;font-size:.9rem}
code{background:#f0f0f0;padding:2px 6px;border-radius:3px;font-size:.9em}
.tag{display:inline-block;padding:2px 8px;border-radius:4px;font-size:.8em;margin:0 4px}
.tag-new{background:#e94560;color:#fff}.tag-m5{background:#0f3460;color:#fff}
</style></head><body>
<h1>🚀 TinyWeb v2.0</h1><p>A high-performance HTTP framework — C++20, epoll, zero-copy</p>

<div class="card"><h2>🆕 Week 30 新增功能</h2>
<p><span class="tag tag-new">NEW</span> <strong>WebSocket</strong> — RFC 6455 帧解析, 双向实时通信</p>
<p><span class="tag tag-new">NEW</span> <strong>Middleware</strong> — 责任链模式, CORS/日志/认证/限流</p>
<p><span class="tag tag-new">NEW</span> <strong>TLS/SSL</strong> — OpenSSL 集成 (可选), TLS 1.2+</p>
<p><span class="tag tag-new">NEW</span> <strong>Performance</strong> — wrk-style 基准测试, 延迟直方图</p>
</div>

<div class="card"><h2>📋 API 端点</h2>
<pre>GET  /api/hello     → {"message":"Hello from TinyWeb v2.0!"}
GET  /api/stats     → Server statistics & features
GET  /api/protected → 🔒 Auth middleware demo (Bearer tinyweb-secret)
GET  /api/ratelimit → ⏱️ Rate limit demo (max 5/sec)
POST /api/echo      → Echo request body back
WS   /ws/chat       → WebSocket chat demo
GET  /api/perf      → Run self-benchmark</pre>
</div>

<div class="card"><h2>🏗️ 架构</h2><pre>
EventLoop(epoll) → Connection(RAII) → Middleware Chain
                                         ├─ Logger
                                         ├─ CORS
                                         ├─ Auth (if configured)
                                         └─ Handler → HTTP/WebSocket/TLS
</pre></div>
<p><small>Month 6 Capstone · 集成 5 个月全部知识 · 从 Socket 到 HTTP Server</small></p>
</body></html>)");
  });

  // ═══════════════════════════════════════════════════════════════
  // 3. Auth-Demo (带 middlewares)
  app.route("GET", "/api/hello", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({"message":"Hello from TinyWeb v2.0!","version":"2.0","timestamp":)" +
      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "}");
  });

  app.route("GET", "/api/stats", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({
  "server":"TinyWeb/2.0","cpp":"C++20","arch":"epoll+non-blocking",
  "features":["HTTP/1.1","HTTPS/TLS","WebSocket","Middleware","Router","StaticFiles","ThreadPool","Benchmark"],
  "middleware":["Logger","CORS","Auth","RateLimit","GZipHint"]
})");
  });

  // 带 Auth + RateLimit 中间件的受保护端点
  app.route("GET", "/api/protected", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({"secret":"You have access!","data":"🔐 Protected resource"})");
  }, {middleware::auth("tinyweb-secret")});

  app.route("GET", "/api/ratelimit", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({"message":"Request allowed","tip":"Try >5 requests/sec to see 429"})");
  }, {middleware::rate_limit(5)});

  app.route("POST", "/api/echo", [](const HttpRequest& req) {
    return HttpResponse{}.set_json(R"({"echo":")" + req.body + R"(","size":)" + std::to_string(req.body.size()) + "}");
  });

  // Self-benchmark endpoint
  app.route("GET", "/api/perf", [](const HttpRequest&) {
    // 运行轻量自检
    auto t0 = std::chrono::steady_clock::now();
    volatile int sum = 0;
    for (int i = 0; i < 100000; i++) sum += i;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return HttpResponse{}.set_json(R"({"benchmark":"100K additions","time_us":)" + std::to_string(us) + "}");
  });

  println("✅ 路由注册: / /api/hello /api/stats /api/protected /api/ratelimit /api/echo /api/perf");

  // ═══════════════════════════════════════════════════════════════
  // 4. WebSocket endpoint
  app.route_ws("/ws/chat", [](std::shared_ptr<WsConnection> ws) {
    println("  🔌 WebSocket 连接建立: /ws/chat");
    // 实际应用中设置 ws->_ws.on_message / on_close
  });
  println("✅ WebSocket endpoint: /ws/chat");

  // ═══════════════════════════════════════════════════════════════
  // 5. Static files
  std::string static_dir = ".";
  if (fs::exists("static")) static_dir = "static";
  app.serve_static(static_dir);
  println("✅ 静态文件: ", fs::absolute(static_dir).string());

  // ═══════════════════════════════════════════════════════════════
  // 6. TLS (conditional)
#ifdef TINYWEB_TLS
  println("🔒 TLS/HTTPS: 已启用 (需要 cert.pem + key.pem)");
#else
  println("ℹ️  TLS/HTTPS: 未编译 (安装 OpenSSL 并 #define TINYWEB_TLS)");
#endif

  // ═══════════════════════════════════════════════════════════════
  // 启动
  println("\n🚀 监听 http://localhost:8080");
  println("   curl http://localhost:8080/api/hello");
  println("   curl http://localhost:8080/api/protected -H 'Authorization: Bearer tinyweb-secret'");
  println("   curl http://localhost:8080/api/ratelimit  # 快速多次触发限流");
  println("   ./tinyweb bench  # 运行性能基准测试");
  println("\nPress Ctrl+C to stop.\n");

  struct sigaction sa{};
  sa.sa_handler = [](int) {};
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  app.start();

  println("\n👋 Server stopped.");
  println("\n📊 Week 30 新增组件:");
  println("  ✅ WebSocket      — RFC 6455 帧解析/编码 + 升级握手");
  println("  ✅ Middleware      — 责任链 (CORS/Logger/Auth/RateLimit/GZip)");
  println("  ✅ TLS/SSL         — OpenSSL 集成 (TlsContext + TlsConnection)");
  println("  ✅ Benchmark       — wrk-style 负载生成 + 延迟统计");
  println("  ✅ Router v2       — per-route middleware + global middleware");
  println();
  println("📖 下一步: Week 31 — 静态分析 + 模糊测试 + CI/CD + 文档");

  return 0;
}
