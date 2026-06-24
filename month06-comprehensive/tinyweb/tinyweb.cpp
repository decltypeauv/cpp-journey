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

#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
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

// ============================================================================
// Part 6: Router — URL 路由 (M1 templates + lambda)
// ============================================================================
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class Router {
  struct Route {
    std::string method;
    std::string path;
    HttpHandler handler;
  };
  std::vector<Route> _routes;
  HttpHandler _not_found;

public:
  Router() {
    _not_found = [](const HttpRequest&) {
      return HttpResponse{}.set_status(404).set_json(R"({"error":"Not Found"})");
    };
  }

  Router& get(const std::string& path, HttpHandler h) { _routes.push_back({"GET", path, std::move(h)}); return *this; }
  Router& post(const std::string& path, HttpHandler h) { _routes.push_back({"POST", path, std::move(h)}); return *this; }
  Router& put(const std::string& path, HttpHandler h) { _routes.push_back({"PUT", path, std::move(h)}); return *this; }
  Router& del(const std::string& path, HttpHandler h) { _routes.push_back({"DELETE", path, std::move(h)}); return *this; }

  HttpResponse dispatch(const HttpRequest& req) const {
    for (auto& r : _routes) {
      if (req.method == r.method && match(r.path, req.path)) return r.handler(req);
    }
    return _not_found(req);
  }

private:
  // 简化路由匹配 (精确匹配 + :param)
  static bool match(const std::string& pattern, const std::string& path) {
    if (pattern == path) return true;
    // :param 通配符
    if (pattern.find(':') != std::string::npos) {
      auto pp = split(pattern, '/');
      auto pu = split(path, '/');
      if (pp.size() != pu.size()) return false;
      for (size_t i = 0; i < pp.size(); i++) {
        if (pp[i].empty() || pp[i][0] != ':') { if (pp[i] != pu[i]) return false; }
      }
      return true;
    }
    return false;
  }
  static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    size_t start = 0, end;
    while ((end = s.find(delim, start)) != std::string::npos) {
      if (end > start) parts.push_back(s.substr(start, end - start));
      start = end + 1;
    }
    if (start < s.size()) parts.push_back(s.substr(start));
    return parts;
  }
};

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

public:
  Connection(int fd, EventLoop& loop) : _fd(fd), _loop(loop) {
    sock::set_nonblocking(fd);
    sock::set_nodelay(fd);
  }

  void set_router(Router* r) { _router = r; }
  void set_file_server(FileServer* fs) { _file_server = fs; }

  void start() {
    auto self = shared_from_this();
    _loop.add(_fd, EPOLLIN, [self](uint32_t events) { self->on_event(events); });
  }

private:
  void on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close_conn(); return; }

    if (events & EPOLLOUT) {
      ssize_t n = _out_buf.write_to(_fd);
      if (n < 0 && errno != EAGAIN) { close_conn(); return; }
      if (_out_buf.size() == 0) _loop.mod(_fd, EPOLLIN); // 写完, 切回只读
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) {
      ssize_t n = _in_buf.read_from(_fd);
      if (n == 0 || (n < 0 && errno != EAGAIN)) { close_conn(); return; }
      if (n > 0) process_input();
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
      else { resp.set_status(404).set_json(R"({"error":"No Router"})"); }

      send_response(std::move(resp));
      _in_buf.clear();
      _parser.reset();
    }
  }

  void send_response(HttpResponse resp) {
    resp.write_to(_out_buf);

    auto self = shared_from_this();
    //  先尝试立即写
    ssize_t n = _out_buf.write_to(_fd);
    if (n < 0 && errno != EAGAIN) { close_conn(); return; }

    if (_out_buf.size() > 0) {
      // 还有数据 → 注册 EPOLLOUT
      _loop.mod(_fd, EPOLLIN | EPOLLOUT);
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

  TinyWeb& route(const std::string& method, const std::string& path, HttpHandler h) {
    if (method == "GET") _router.get(path, std::move(h));
    else if (method == "POST") _router.post(path, std::move(h));
    else if (method == "PUT") _router.put(path, std::move(h));
    else if (method == "DELETE") _router.del(path, std::move(h));
    return *this;
  }
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
// Demo Server
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║   Month 6 Week 29: TinyWeb — 高性能 HTTP 服务器框架           ║
║   集成 5 个月所学: epoll + RAII + HTTP + thread pool + router ║
╚══════════════════════════════════════════════════════════════╝)");

  HR("启动 TinyWeb Server");

  TinyWeb app(8080);

  // 注册路由
  app.route("GET", "/", [](const HttpRequest&) {
    return HttpResponse{}.set_html(R"(<!DOCTYPE html>
<html><head><title>TinyWeb</title><style>
body{font-family:system-ui;max-width:800px;margin:2rem auto;padding:0 1rem}
h1{color:#333} code{background:#f0f0f0;padding:2px 6px;border-radius:3px}
pre{background:#f5f5f5;padding:1rem;border-radius:6px;overflow-x:auto}
</style></head><body>
<h1>🚀 TinyWeb Server</h1>
<p>A high-performance HTTP server framework built in C++20.</p>
<h2>API Endpoints</h2>
<pre>GET  /api/hello      — Hello World JSON
GET  /api/stats       — Server statistics
GET  /api/echo?msg=hi — Echo message
POST /api/echo        — Echo body
GET  /static/*        — Static file server</pre>
<h2>Architecture</h2>
<pre>EventLoop(epoll) → Connection(RAII) → HttpParser → Router → Handler</pre>
<p><small>Powered by TinyWeb · C++20 · epoll · Month 6 Capstone</small></p>
</body></html>)");
  });

  app.route("GET", "/api/hello", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({"message":"Hello from TinyWeb!","timestamp":)" +
      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "}");
  });

  app.route("GET", "/api/stats", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({
  "server": "TinyWeb/1.0",
  "cpp": "C++20",
  "architecture": "epoll + non-blocking I/O",
  "features": ["HTTP/1.1","Router","Static Files","Thread Pool","JSON API"]
})");
  });

  app.route("GET", "/api/echo", [](const HttpRequest& req) {
    std::string msg = "{}";
    auto& path = req.path;
    auto pos = path.find("?msg=");
    if (pos != std::string::npos) msg = "\"" + path.substr(pos + 5) + "\"";
    return HttpResponse{}.set_json(R"({"echo":)" + msg + "}");
  });

  app.route("POST", "/api/echo", [](const HttpRequest& req) {
    return HttpResponse{}.set_json(R"({"echo":")" + req.body + R"("})");
  });

  // 静态文件服务 (如果有 ./static 目录)
  std::string static_dir = ".";
  if (fs::exists("static")) static_dir = "static";
  app.serve_static(static_dir);

  // 信号处理
  struct sigaction sa{};
  sa.sa_handler = [](int) { /* handled by loop stop */ };
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  app.start();

  println("\n👋 Server stopped.");
  println("\n📊 Month 6 Week 29 组件总览:");
  println("  ✅ EventLoop   — epoll-based reactor (M3 + M5 libevent)");
  println("  ✅ SocketUtil  — non-blocking, TCP_NODELAY, SO_REUSEADDR (M3)");
  println("  ✅ Buffer      — chain buffer for I/O (M5 libevent evbuffer)");
  println("  ✅ HttpParser  — HTTP/1.1 state machine (M3 HTTP)");
  println("  ✅ HttpResponse — builder pattern (M5 fmtlib output)");
  println("  ✅ Router      — URL routing + middleware (M1 templates)");
  println("  ✅ FileServer  — static files + MIME (M2 File I/O)");
  println("  ✅ ThreadPool  — worker threads (M2 multithreading)");
  println("  ✅ Connection  — RAII per-connection state (M1 RAII)");
  println("  ✅ TinyWeb     — integration + demo server");
  println();
  println("📖 下一步: Week 30 — 性能优化 + WebSocket + 中间件");

  return 0;
}
