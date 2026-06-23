// Week 14: HTTP 协议 + 简易 HTTP Server
// 编译: cmake -B build && cmake --build build
// 运行: ./build/http_server
//
// HTTP 是互联网最广泛的应用层协议。
// 在 Week 11-13 掌握了 TCP/Socket/epoll 之后，
// 本周学习如何在这之上构建 HTTP 协议。
//
// 核心主题:
//   HTTP 请求/响应格式 — method, URI, headers, body
//   URL 解析 — scheme, host, port, path, query
//   请求解析 — 健壮地解析浏览器发来的请求
//   响应构建 — 状态码, Content-Type, Content-Length
//   传输编码 — Content-Length vs chunked
//   MIME 类型 — 文件扩展名 → Content-Type 映射
//   静态文件服务 — 实现一个能访问的 HTTP 服务器!
//   Keep-Alive — HTTP/1.1 持久连接

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
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
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

// ============================================================
// 练习 1: HTTP 协议基础 — 请求/响应格式
// ============================================================
//
// HTTP 是文本协议，人类可读。核心是「请求-响应」模型。
//
// HTTP 请求格式:
//   METHOD URI HTTP/VERSION\r\n          ← 请求行
//   Header-Name: value\r\n               ← 头部 (0-n 个)
//   \r\n                                 ← 空行 (头结束)
//   [body]                               ← 可选的消息体
//
// HTTP 响应格式:
//   HTTP/VERSION STATUS-CODE REASON\r\n  ← 状态行
//   Header-Name: value\r\n               ← 响应头
//   \r\n                                 ← 空行
//   [body]                               ← 可选的消息体
//
// 每个行以 CRLF (\r\n) 结尾 — 这是 HTTP 协议的「律法」。

void exercise1_http_basics() {
  section("练习 1: HTTP 协议基础");

  // TODO 1.1: 解剖一个 HTTP 请求
  {
    subsection("HTTP 请求解剖");

    cout << "  一个典型的 HTTP/1.1 GET 请求:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ GET /index.html HTTP/1.1                   │ ← 请求行\n";
    cout << "  │ Host: www.example.com                      │ ← 头部\n";
    cout << "  │ User-Agent: Mozilla/5.0                    │\n";
    cout << "  │ Accept: text/html                          │\n";
    cout << "  │ Connection: keep-alive                     │\n";
    cout << "  │                                            │ ← 空行 (\\r\\n)\n";
    cout << "  │ [GET 请求没有 body]                         │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  请求行 = METHOD SP URI SP VERSION CRLF\n";
    cout << "    METHOD:  GET | POST | PUT | DELETE | HEAD | OPTIONS | ...\n";
    cout << "    URI:     /path/to/resource?key=value\n";
    cout << "    VERSION: HTTP/1.0 | HTTP/1.1 | HTTP/2\n";
  }

  // TODO 1.2: 解剖一个 HTTP 响应
  {
    subsection("HTTP 响应解剖");

    cout << "  一个典型的 HTTP/1.1 200 OK 响应:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ HTTP/1.1 200 OK                            │ ← 状态行\n";
    cout << "  │ Content-Type: text/html; charset=utf-8      │ ← 头部\n";
    cout << "  │ Content-Length: 127                        │\n";
    cout << "  │ Connection: keep-alive                     │\n";
    cout << "  │                                            │ ← 空行 (\\r\\n)\n";
    cout << "  │ <html>                                     │ ← body\n";
    cout << "  │   <body><h1>Hello World!</h1></body>        │\n";
    cout << "  │ </html>                                    │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  状态行 = VERSION SP STATUS-CODE SP REASON-PHRASE CRLF\n";
    cout << "    STATUS-CODE: 200(OK) 301(Moved) 404(Not Found) 500(Internal Error)\n";
  }

  // TODO 1.3: HTTP 协议版本演进
  {
    subsection("HTTP 版本演进");

    cout << "  ┌─────────────┬──────┬──────────────────────────────────────┐\n";
    cout << "  │ HTTP/0.9    │ 1991 │ 只有 GET, 无 header, 响应就是 HTML   │\n";
    cout << "  │ HTTP/1.0    │ 1996 │ header/status code, 每请求一个连接     │\n";
    cout << "  │ HTTP/1.1    │ 1997 │ Keep-Alive, pipeline, chunked, Host  │\n";
    cout << "  │ HTTP/2      │ 2015 │ 二进制分帧, 多路复用, header 压缩     │\n";
    cout << "  │ HTTP/3      │ 2022 │ QUIC(UDP), 0-RTT, 更好的移动网络    │\n";
    cout << "  └─────────────┴──────┴──────────────────────────────────────┘\n";
    cout << "\n  💡 本周聚焦 HTTP/1.1 — 它仍是应用最广的版本\n";
  }

  // TODO 1.4: 常见状态码
  {
    subsection("常见 HTTP 状态码");

    cout << "  ┌───────┬──────────────────────┬────────────────────────────────┐\n";
    cout << "  │ 200   │ OK                   │ 请求成功                        │\n";
    cout << "  │ 201   │ Created              │ 资源已创建 (POST 结果)          │\n";
    cout << "  │ 204   │ No Content           │ 成功但没有 body (DELETE 后)     │\n";
    cout << "  │ 301   │ Moved Permanently    │ 永久重定向                      │\n";
    cout << "  │ 302   │ Found                │ 临时重定向                      │\n";
    cout << "  │ 304   │ Not Modified         │ 缓存命中                        │\n";
    cout << "  │ 400   │ Bad Request          │ 请求格式错误                    │\n";
    cout << "  │ 404   │ Not Found            │ 资源不存在                      │\n";
    cout << "  │ 405   │ Method Not Allowed   │ 不支持的 method                  │\n";
    cout << "  │ 413   │ Payload Too Large    │ 请求体太大                      │\n";
    cout << "  │ 500   │ Internal Server Error│ 服务器内部错误                  │\n";
    cout << "  │ 505   │ HTTP Version Not Sup.│ 不支持的 HTTP 版本               │\n";
    cout << "  └───────┴──────────────────────┴────────────────────────────────┘\n";
    cout << "\n  💡 记住: 2xx成功, 3xx重定向, 4xx客户端错误, 5xx服务端错误\n";
  }

  // TODO 1.5: CRLF — HTTP 的「句号」
  {
    subsection("CRLF — HTTP 协议的生命线");

    cout << "  HTTP 的每一个行都以 \\r\\n (CRLF, 0x0D 0x0A) 结尾:\n";
    cout << "    - 请求行: GET / HTTP/1.1\\r\\n\n";
    cout << "    - 头部行: Host: localhost\\r\\n\n";
    cout << "    - 空行:   \\r\\n  (标记 header 结束, body 开始)\n";
    cout << "\n";
    cout << "  ⚠️  错误的换行符是 HTTP 解析 bug 的最常见来源:\n";
    cout << "    - 只有 \\n (LF) — 不符标准, 但有些服务器兼容\n";
    cout << "    - 多了空格 — 可能被当作头部名称的一部分\n";
    cout << "    - 多余的空行 — 可能导致解析器误判 body 开始\n";
    cout << "\n";
    cout << "  💡 健壮的解析器: 同时接受 \\r\\n 和 \\n 作为行尾\n";
  }
}

// ============================================================
// 练习 2: URL 解析
// ============================================================
//
// URL (Uniform Resource Locator) 的结构:
//   scheme://username:password@host:port/path?query#fragment
//
// HTTP 请求的 URI 不包含 scheme/host/port (那些在 Host header 中):
//   GET /path/to/file?key=value HTTP/1.1
//   Host: www.example.com:8080
//
// 本节实现一个简单的 URL 解析器，理解各个组件的含义。

// 简单 URL 解析结果
struct ParsedUrl {
  string scheme;   // http, https
  string host;     // www.example.com
  int port = 80;   // 默认 80
  string path;     // /index.html
  string query;    // key=value&foo=bar
  string fragment; // # 之后的内容 (通常不发给服务器)

  bool valid = false;
};

// 简单的 URL 解析器
ParsedUrl parse_url(const string &url_str) {
  ParsedUrl result;
  string_view url(url_str);
  size_t pos = 0;

  // 1. 解析 scheme (如果存在)
  size_t scheme_end = url.find("://");
  if (scheme_end != string::npos) {
    result.scheme = url.substr(0, scheme_end);
    if (result.scheme == "https")
      result.port = 443;
    pos = scheme_end + 3; // 跳过 ://
  }

  // 2. 解析 host (和可选的 port)
  size_t path_start = url.find('/', pos);
  size_t query_start = url.find('?', pos);
  size_t fragment_start = url.find('#', pos);

  // host 的结束位置: 最先出现的 / 或 ? 或 #
  size_t host_end = string::npos;
  for (size_t end : {path_start, query_start, fragment_start}) {
    if (end != string::npos && (host_end == string::npos || end < host_end))
      host_end = end;
  }

  string_view host_part;
  if (host_end == string::npos) {
    host_part = url.substr(pos);
  } else {
    host_part = url.substr(pos, host_end - pos);
  }

  // 检查 host 中有没有 :port
  size_t port_sep = host_part.find(':');
  if (port_sep != string::npos) {
    result.host = string(host_part.substr(0, port_sep));
    result.port = std::stoi(string(host_part.substr(port_sep + 1)));
  } else {
    result.host = string(host_part);
  }

  // 3. 解析 path
  if (path_start != string::npos) {
    size_t path_end = url.size();
    if (query_start != string::npos && query_start < path_end)
      path_end = query_start;
    if (fragment_start != string::npos && fragment_start < path_end)
      path_end = fragment_start;
    result.path = string(url.substr(path_start, path_end - path_start));
  } else {
    result.path = "/";
  }

  // 4. 解析 query string
  if (query_start != string::npos) {
    size_t query_end = url.size();
    if (fragment_start != string::npos && fragment_start < query_end)
      query_end = fragment_start;
    result.query = string(
        url.substr(query_start + 1, query_end - query_start - 1));
  }

  // 5. 解析 fragment
  if (fragment_start != string::npos) {
    result.fragment = string(url.substr(fragment_start + 1));
  }

  result.valid = !result.host.empty();
  return result;
}

void exercise2_url_parsing() {
  section("练习 2: URL 解析");

  // TODO 2.1: URL 结构
  {
    subsection("URL 完整结构");

    cout << "  https://user:pass@www.example.com:8080/path/file.html?q=1#section\n";
    cout << "  └───┘   └──┬──┘  └──────┬──────┘ └──┘ └────┬──────┘ └─┬─┘ └──┬──┘\n";
    cout << "  scheme  user:pass   host         port    path       query  fragment\n";
    cout << "\n";
    cout << "  HTTP 请求中:\n";
    cout << "    - 请求行只含 path 和 query: GET /path/file.html?q=1 HTTP/1.1\n";
    cout << "    - Host 头部提供 host 和 port: Host: www.example.com:8080\n";
    cout << "    - fragment (#section) 不发给服务器, 浏览器自己处理\n";
  }

  // TODO 2.2: URL 解析实践
  {
    subsection("URL 解析器测试");

    auto test = [](const string &url) {
      auto p = parse_url(url);
      if (p.valid) {
        cout << "  URL:  " << url << "\n";
        cout << "    scheme:   \"" << p.scheme << "\"\n";
        cout << "    host:     \"" << p.host << "\"\n";
        cout << "    port:     " << p.port << "\n";
        cout << "    path:     \"" << p.path << "\"\n";
        cout << "    query:    \"" << p.query << "\"\n";
        cout << "    fragment: \"" << p.fragment << "\"\n";
      } else {
        cout << "  ❌ 解析失败: " << url << "\n";
      }
    };

    test("http://www.example.com/index.html");
    test("https://github.com/decltypeauv/cpp-journey");
    test("http://localhost:8080/api/users?id=42&name=alice");
    test("http://192.168.1.1:3000/path/to/page#section2");
    test("/just/a/path?key=value");
  }

  // TODO 2.3: query string 解析
  {
    subsection("Query String 解析");

    cout << "  Query string 格式: key1=value1&key2=value2\n";
    cout << "  特殊字符需要 URL 编码:\n";
    cout << "    空格 → %20\n";
    cout << "    &    → %26   (否则被当作分隔符)\n";
    cout << "    =    → %3D   (否则被当作 key=value 分隔)\n";
    cout << "    中文 → %E4%BD%A0 (UTF-8 编码后转 %XX)\n";
    cout << "\n";
    cout << "  URL 解码的核心逻辑 (C++):\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ string url_decode(string_view str) {        │\n";
    cout << "  │   string result;                            │\n";
    cout << "  │   for (size_t i = 0; i < str.size(); ++i) { │\n";
    cout << "  │     if (str[i] == '%' && i+2 < str.size()) {│\n";
    cout << "  │       int val;                              │\n";
    cout << "  │       sscanf(str.substr(i+1,2), \"%x\", &val);│\n";
    cout << "  │       result += static_cast<char>(val);     │\n";
    cout << "  │       i += 2;                               │\n";
    cout << "  │     } else if (str[i] == '+') {             │\n";
    cout << "  │       result += ' ';                        │\n";
    cout << "  │     } else {                                │\n";
    cout << "  │       result += str[i];                     │\n";
    cout << "  │     }                                       │\n";
    cout << "  │   }                                         │\n";
    cout << "  │   return result;                            │\n";
    cout << "  │ }                                           │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
  }

  // TODO 2.4: URL 路径安全检查
  {
    subsection("⚠️ URL 路径安全 — 目录遍历攻击");

    cout << "  恶意 URL 示例:\n";
    cout << "    GET /../../../etc/passwd HTTP/1.1\n";
    cout << "    GET /..%2F..%2F..%2Fetc%2Fpasswd HTTP/1.1 (URL 编码绕过)\n";
    cout << "    GET /files/../../../etc/shadow HTTP/1.1 (相对路径)\n";
    cout << "\n";
    cout << "  防御措施:\n";
    cout << "    1. 先 URL 解码, 再规范化路径\n";
    cout << "    2. 检查规范化后的路径是否在文档根目录内\n";
    cout << "    3. 拒绝包含 \"..\" 的路径\n";
    cout << "    4. 用 realpath() 解析符号链接后检查\n";
    cout << "\n";
  }
}

// ============================================================
// 练习 3: HTTP 请求解析 — 一个健壮的解析器
// ============================================================
//
// HTTP 请求是文本格式，但实际解析有很多细节:
//   - 请求行: "METHOD SP URI SP VERSION CRLF"
//   - 头部:   "Name: Value CRLF" (可多行, 值可能跨行)
//   - 空行:   "CRLF" (标记头部结束)
//   - Body:   根据 Content-Length 或 Transfer-Encoding 读取
//
// 本节构建一个简单但健壮的 HTTP 请求解析器。

// HTTP 请求结构
struct HttpRequest {
  string method;   // GET, POST, ...
  string uri;      // /index.html?page=1
  string version;  // HTTP/1.1

  // 解析后的头部存储在 map 中 (key 转为小写方便查找)
  std::map<string, string> headers;

  // 消息体
  string body;

  // 原始请求 (调试用)
  string raw;

  bool valid = false;
};

// 从 socket 读取一行 (直到 \r\n 或 \n)
// 返回 nullopt 表示连接关闭或错误
std::optional<string> recv_line(int fd) {
  string line;
  char c;
  while (true) {
    ssize_t n = recv(fd, &c, 1, 0);
    if (n <= 0)
      return std::nullopt;
    if (c == '\r') {
      // 偷看下一个字符是不是 \n
      // 用非阻塞 peek: 实际上用 MSG_PEEK 偷看
      char next;
      ssize_t peek_n = recv(fd, &next, 1, MSG_PEEK | MSG_DONTWAIT);
      if (peek_n > 0 && next == '\n') {
        recv(fd, &next, 1, 0); // 消费 \n
      }
      return line;
    }
    if (c == '\n') {
      return line;
    }
    line += c;
    // 防止一行太长的攻击
    if (line.size() > 8192) {
      std::cerr << "  ❌ 行太长, 可能是攻击\n";
      return std::nullopt;
    }
  }
}

// 解析整个 HTTP 请求
HttpRequest parse_http_request(int fd) {
  HttpRequest req;

  // 1. 读取请求行: METHOD SP URI SP VERSION
  auto request_line = recv_line(fd);
  if (!request_line.has_value() || request_line->empty())
    return req;

  req.raw = *request_line + "\n";

  // 用 stringstream 拆分请求行
  std::istringstream iss(*request_line);
  iss >> req.method >> req.uri >> req.version;
  if (req.method.empty() || req.uri.empty() || req.version.empty())
    return req;

  // 2. 逐行读取头部
  while (true) {
    auto header_line = recv_line(fd);
    if (!header_line.has_value())
      return req;
    if (header_line->empty())
      break; // 空行 = 头部结束

    req.raw += *header_line + "\n";

    // 解析 "Name: Value"
    size_t colon = header_line->find(':');
    if (colon != string::npos) {
      string name = header_line->substr(0, colon);
      string value = header_line->substr(colon + 1);

      // 去除 value 前后的空格
      size_t start = value.find_first_not_of(" \t");
      if (start != string::npos) {
        value = value.substr(start);
      }
      // 转小写
      for (auto &ch : name)
        ch = std::tolower(static_cast<unsigned char>(ch));
      req.headers[name] = value;
    }
  }

  // 3. 如果有 body, 读取 body
  auto it = req.headers.find("content-length");
  if (it != req.headers.end()) {
    size_t body_len = std::stoul(it->second);
    if (body_len > 10 * 1024 * 1024) { // 限制 10MB
      std::cerr << "  ❌ body 太大: " << body_len << "\n";
      return req;
    }
    req.body.resize(body_len);
    size_t received = 0;
    while (received < body_len) {
      ssize_t n = recv(fd, &req.body[received], body_len - received, 0);
      if (n <= 0) {
        req.body.resize(received);
        break;
      }
      received += n;
    }
  }

  req.valid = true;
  return req;
}

void exercise3_request_parsing() {
  section("练习 3: HTTP 请求解析");

  // TODO 3.1: 请求解析流程
  {
    subsection("解析流程");

    cout << "  第1步: 读请求行 → 提取 METHOD, URI, VERSION\n";
    cout << "  第2步: 逐行读头部 → 直到遇到空行 (\\r\\n)\n";
    cout << "  第3步: 检查 Content-Length → 读取 N 字节 body\n";
    cout << "\n";
    cout << "  💡 头部 key 转小写: HTTP 头部名是大小写不敏感的\n";
    cout << "    Content-Length = content-length = CONTENT-LENGTH\n";
  }

  // TODO 3.2: 实践 — 发送并解析请求
  {
    subsection("实践: 发送 HTTP 请求并查看解析结果");

    constexpr int PORT = 14031;

    // 服务器: 接收并解析 HTTP 请求
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
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        auto req = parse_http_request(client);
        if (req.valid) {
          cout << "  [Server] 解析到的请求:\n";
          cout << "    Method:  \"" << req.method << "\"\n";
          cout << "    URI:     \"" << req.uri << "\"\n";
          cout << "    Version: \"" << req.version << "\"\n";
          cout << "    Headers:\n";
          for (auto &[k, v] : req.headers) {
            cout << "      " << k << ": " << v << "\n";
          }
          if (!req.body.empty()) {
            cout << "    Body (" << req.body.size()
                 << " 字节): \"" << req.body << "\"\n";
          }
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端: 发送格式化的 HTTP 请求
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 构造 POST 请求 (有 body)
      string body = "{\"username\":\"alice\",\"password\":\"secret123\"}";
      string request =
          "POST /api/login HTTP/1.1\r\n"
          "Host: localhost:" + std::to_string(PORT) + "\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: " + std::to_string(body.size()) + "\r\n"
          "Connection: close\r\n"
          "\r\n" +
          body;

      send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);
      cout << "  [Client] 已发送 HTTP POST 请求\n";

      // 给服务器时间解析
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.join();
    cout << "  ✅ 学会了如何构造和解析原始 HTTP 请求\n";
  }

  // TODO 3.3: 常见请求头一览
  {
    subsection("常见请求头");

    cout << "  ┌─────────────────────┬──────────────────────────────────────┐\n";
    cout << "  │ Host                │ 目标主机 (HTTP/1.1 必须)              │\n";
    cout << "  │ User-Agent          │ 客户端标识                            │\n";
    cout << "  │ Accept              │ 期望的响应内容类型                    │\n";
    cout << "  │ Content-Type        │ 请求体的媒体类型                      │\n";
    cout << "  │ Content-Length      │ 请求体的长度 (字节)                   │\n";
    cout << "  │ Connection          │ keep-alive or close                   │\n";
    cout << "  │ Transfer-Encoding   │ chunked (分块传输)                    │\n";
    cout << "  │ Cookie              │ 之前服务器设置的 cookie               │\n";
    cout << "  │ Authorization       │ 认证凭证 (Basic / Bearer token)      │\n";
    cout << "  └─────────────────────┴──────────────────────────────────────┘\n";
  }
}

// ============================================================
// 练习 4: HTTP 响应构建
// ============================================================
//
// HTTP 响应 = 状态行 + 响应头 + 空行 + body
//
// 常用的 Content-Type:
//   text/html; charset=utf-8  — HTML
//   text/plain                — 纯文本
//   application/json          — JSON
//   image/png, image/jpeg     — 图片
//   application/octet-stream  — 通用二进制流
//
// 一个良好的 HTTP 响应应该包含:
//   - Content-Type: 告诉浏览器如何解析
//   - Content-Length: 告诉浏览器 body 有多长
//   - Connection: keep-alive 或 close
//   - Server: 服务器标识
//   - Date: 响应时间

void exercise4_response_building() {
  section("练习 4: HTTP 响应构建");

  // TODO 4.1: 构建标准响应
  {
    subsection("响应模板");

    cout << "  一个标准的 HTTP/1.1 响应:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ HTTP/1.1 200 OK                            │\n";
    cout << "  │ Content-Type: text/plain                    │\n";
    cout << "  │ Content-Length: 12                          │\n";
    cout << "  │ Connection: keep-alive                      │\n";
    cout << "  │ Server: cpp-journey/1.0                     │\n";
    cout << "  │                                             │\n";
    cout << "  │ Hello World!                                │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  构建代码:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ string build_response(                        │\n";
    cout << "  │     int code, const string& type,             │\n";
    cout << "  │     string_view body) {                       │\n";
    cout << "  │   string resp;                                │\n";
    cout << "  │   resp += \"HTTP/1.1 \" + to_string(code) + ...;│\n";
    cout << "  │   resp += \"Content-Type: \" + type + \"\\r\\n\";   │\n";
    cout << "  │   resp += \"Content-Length: \" + ... + \"\\r\\n\";  │\n";
    cout << "  │   resp += \"Connection: keep-alive\\r\\n\";       │\n";
    cout << "  │   resp += \"\\r\\n\";                              │\n";
    cout << "  │   resp += body;                                │\n";
    cout << "  │   return resp;                                 │\n";
    cout << "  │ }                                              │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
  }

  // TODO 4.2: 实践 — 发送响应给客户端
  {
    subsection("实践: 接收请求并返回响应");

    constexpr int PORT = 14041;

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
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);

        // 读请求 (简化为读到空行)
        string line;
        char c;
        while (true) {
          if (recv(client, &c, 1, 0) <= 0) break;
          line += c;
          if (line.size() >= 4 && line.substr(line.size() - 4) == "\r\n\r\n")
            break;
        }
        cout << "  [Server] 收到请求 ("
             << line.size() << " 字节)\n";

        // 构建并发送响应
        string body = R"(<!DOCTYPE html>
<html>
<head><title>C++ HTTP Server</title></head>
<body>
  <h1>🚀 Hello from C++ HTTP Server!</h1>
  <p>这条路真的可以走通 — 用 C++ 写 Web 服务器</p>
</body>
</html>)";

        string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " +
            std::to_string(body.size()) +
            "\r\n"
            "Connection: close\r\n"
            "Server: cpp-journey/14.0\r\n"
            "\r\n" +
            body;

        send(client, response.c_str(), response.size(), MSG_NOSIGNAL);
        cout << "  [Server] 已发送 HTTP 响应 (" << body.size()
             << " 字节 body)\n";
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 发送 GET 请求
      string request =
          "GET / HTTP/1.1\r\n"
          "Host: localhost\r\n"
          "Connection: close\r\n"
          "\r\n";
      send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);

      // 接收响应
      char buf[4096]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        cout << "  [Client] 收到响应:\n";
        // 只显示前几行
        string response(buf, n);
        size_t body_start = response.find("\r\n\r\n");
        if (body_start != string::npos) {
          cout << "    Status+Headers: " << body_start + 4 << " 字节\n";
          cout << "    Body: " << (response.size() - body_start - 4)
               << " 字节 HTML\n";
        }
        cout << "  ✅ HTTP 请求-响应循环完成!\n";
      }
    }

    server.join();
  }

  // TODO 4.3: 常见响应头
  {
    subsection("常见响应头");

    cout << "  ┌──────────────────────┬─────────────────────────────────────┐\n";
    cout << "  │ Content-Type         │ 响应体的 MIME 类型                  │\n";
    cout << "  │ Content-Length       │ 响应体的字节数                      │\n";
    cout << "  │ Connection           │ keep-alive 或 close                 │\n";
    cout << "  │ Server               │ 服务器软件标识                      │\n";
    cout << "  │ Date                 │ 响应生成时间 (RFC 1123 格式)        │\n";
    cout << "  │ Last-Modified        │ 资源最后修改时间 (供缓存判断)       │\n";
    cout << "  │ Cache-Control        │ 缓存策略: no-cache, max-age=N       │\n";
    cout << "  │ Location             │ 重定向目标 URL (3xx 响应)           │\n";
    cout << "  │ Set-Cookie           │ 要求浏览器存储 cookie               │\n";
    cout << "  │ Transfer-Encoding    │ chunked (分块传输)                  │\n";
    cout << "  └──────────────────────┴─────────────────────────────────────┘\n";
  }
}

// ============================================================
// 练习 5: Content-Length vs Transfer-Encoding: chunked
// ============================================================
//
// HTTP 有两种方式告诉客户端 body 的长度:
//
// 1. Content-Length: 提前知道大小
//    - 最简单的方案
//    - 必须一次性知道 body 的完整长度
//    - 适合: 静态文件, JSON 响应
//
// 2. Transfer-Encoding: chunked: 分块传输
//    - 不需要提前知道总长度
//    - 边生成边发送 (流式)
//    - 适合: 动态内容, 大文件, 实时数据
//    - 格式:
//        chunk-size(hex)\r\n
//        chunk-data\r\n
//        chunk-size(hex)\r\n
//        chunk-data\r\n
//        0\r\n          ← 最后一个 chunk 大小为 0
//        trailer\r\n    ← 可选尾头部
//        \r\n           ← 结束

void exercise5_content_length_vs_chunked() {
  section("练习 5: Content-Length vs Transfer-Encoding: chunked");

  // TODO 5.1: Content-Length — 传统方式
  {
    subsection("Content-Length — 提前知道大小");

    cout << "  使用场景: 文件大小已知的静态文件服务\n";
    cout << "  流程:\n";
    cout << "    1. stat() 获取文件大小 → Content-Length: 102400\n";
    cout << "    2. 发送头部\n";
    cout << "    3. 发送整个文件内容\n";
    cout << "    4. 客户端读完 Content-Length 字节后知道结束了\n";
  }

  // TODO 5.2: chunked — 分块传输
  {
    subsection("Transfer-Encoding: chunked — 动态内容");

    cout << "  格式:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ HTTP/1.1 200 OK                            │\n";
    cout << "  │ Transfer-Encoding: chunked                 │\n";
    cout << "  │                                             │\n";
    cout << "  │ 1A\\r\\n                     ← 16 进制长度     │\n";
    cout << "  │ Now serving 26 bytes...\\r\\n  ← chunk 数据    │\n";
    cout << "  │ 0\\r\\n                      ← 最后一个 chunk  │\n";
    cout << "  │ \\r\\n                       ← 结束标记        │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  chunk 格式细节:\n";
    cout << "    - 长度用十六进制表示 (1A = 26 字节)\n";
    cout << "    - 长度和数据之间没有空格!\n";
    cout << "    - 每个 chunk 后跟 \\r\\n\n";
    cout << "    - 最后一个 chunk 长度 = 0\\r\\n\n";
    cout << "    - 可选的 trailer headers (类似 HTTP headers)\n";
    cout << "    - 最后以 \\r\\n 结束\n";
    cout << "\n";
    cout << "  💡 chunked 的优势:\n";
    cout << "    - 服务器可以在不知道总长度的情况下开始响应\n";
    cout << "    - 适合实时流、大文件传输、SSE (Server-Sent Events)\n";
  }

  // TODO 5.3: 实践 — chunked 响应
  {
    subsection("实践: 发送 chunked 响应");

    constexpr int PORT = 14051;

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
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);

        // 读完请求头
        string buf;
        char c;
        for (int i = 0; i < 1024; ++i) {
          if (recv(client, &c, 1, 0) <= 0) break;
          buf += c;
          if (buf.size() >= 4 && buf.substr(buf.size() - 4) == "\r\n\r\n") break;
        }

        // 发送 chunked 响应头
        string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(client, header.c_str(), header.size(), MSG_NOSIGNAL);

        // 发送多个 chunk
        auto send_chunk = [&](const string &data) {
          char size_hex[32];
          snprintf(size_hex, sizeof(size_hex), "%zx\r\n", data.size());
          send(client, size_hex, strlen(size_hex), MSG_NOSIGNAL);
          send(client, data.c_str(), data.size(), MSG_NOSIGNAL);
          send(client, "\r\n", 2, MSG_NOSIGNAL);
        };

        send_chunk("Hello! ");
        cout << "  [Server] chunk 1: \"Hello! \"\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        send_chunk("这是分块传输。");
        cout << "  [Server] chunk 2: \"这是分块传输。\"\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        send_chunk("Goodbye!");
        cout << "  [Server] chunk 3: \"Goodbye!\"\n";

        // 结束 chunk
        send(client, "0\r\n\r\n", 5, MSG_NOSIGNAL);
        cout << "  [Server] chunked 传输完成\n";
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      // 发送请求
      string request = "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
      send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);

      // 接收并解析 chunked 响应
      char buf[4096]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        cout << "  [Client] 原始响应:\n" << string(buf, n);
        cout << "  ✅ chunked 响应接收完成\n";
      }
    }

    server.join();
  }

  // TODO 5.4: 何时用哪个?
  {
    subsection("Content-Length vs chunked 选择");

    cout << "  ┌──────────────────┬──────────────────┬──────────────────┐\n";
    cout << "  │                  │ Content-Length   │ chunked          │\n";
    cout << "  ├──────────────────┼──────────────────┼──────────────────┤\n";
    cout << "  │ 需要提前知道大小  │ ✅ 是            │ ❌ 不需要        │\n";
    cout << "  │ 静态文件         │ ✅ 完美           │ ⚠️ 可以但不是最优 │\n";
    cout << "  │ 动态生成内容      │ ⚠️ 需要缓冲全部   │ ✅ 边生成边发送  │\n";
    cout << "  │ 大文件下载        │ ✅ (可断点续传)   │ ⚠️ 可以          │\n";
    cout << "  │ 流式推送          │ ❌ 不适用         │ ✅ 唯一选择      │\n";
    cout << "  │ 客户端兼容性      │ ✅ 所有版本        │ ⚠️ HTTP/1.1+    │\n";
    cout << "  └──────────────────┴──────────────────┴──────────────────┘\n";
    cout << "\n  💡 本周 HTTP Server: 我们用 Content-Length (简单可靠)\n";
  }
}

// ============================================================
// 练习 6: MIME 类型
// ============================================================
//
// MIME (Multipurpose Internet Mail Extensions) 类型告诉浏览器
// 如何解析接收到的内容。
//
// Content-Type: text/html; charset=utf-8
// └────主类型────┘ └子类型┘ └──────参数──────┘
//
// 常见 MIME:
//   .html → text/html
//   .css  → text/css
//   .js   → application/javascript
//   .json → application/json
//   .png  → image/png
//   .jpg  → image/jpeg
//   .svg  → image/svg+xml
//   .pdf  → application/pdf
//   .zip  → application/zip
//   无扩展 → application/octet-stream (二进制流)

// MIME 类型映射表
const std::map<string, string> MIME_TYPES = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "application/javascript; charset=utf-8"},
    {".mjs", "application/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".xml", "application/xml"},
    {".txt", "text/plain; charset=utf-8"},
    {".csv", "text/csv; charset=utf-8"},
    {".md", "text/markdown; charset=utf-8"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".webp", "image/webp"},
    {".mp4", "video/mp4"},
    {".webm", "video/webm"},
    {".mp3", "audio/mpeg"},
    {".wav", "audio/wav"},
    {".ogg", "audio/ogg"},
    {".pdf", "application/pdf"},
    {".zip", "application/zip"},
    {".gz", "application/gzip"},
    {".tar", "application/x-tar"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {"", "application/octet-stream"}, // 默认
};

// 根据文件扩展名获取 MIME 类型
string get_mime_type(const string &path) {
  size_t dot = path.rfind('.');
  if (dot == string::npos)
    return MIME_TYPES.at("");

  string ext = path.substr(dot);
  // 转小写
  for (auto &c : ext)
    c = std::tolower(static_cast<unsigned char>(c));

  auto it = MIME_TYPES.find(ext);
  if (it != MIME_TYPES.end())
    return it->second;
  return MIME_TYPES.at("");
}

void exercise6_mime_types() {
  section("练习 6: MIME 类型");

  // TODO 6.1: MIME 类型分类
  {
    subsection("MIME 类型分类");

    cout << "  ┌───────────────┬─────────────────────────────────────────────┐\n";
    cout << "  │ text/*        │ HTML, CSS, JS, plain text, CSV              │\n";
    cout << "  │ image/*       │ PNG, JPEG, GIF, SVG, WebP                  │\n";
    cout << "  │ audio/*       │ MP3, WAV, OGG                              │\n";
    cout << "  │ video/*       │ MP4, WebM, OGG video                       │\n";
    cout << "  │ application/* │ JSON, PDF, ZIP, octet-stream               │\n";
    cout << "  │ font/*        │ WOFF, WOFF2, TTF                           │\n";
    cout << "  │ multipart/*   │ form-data (文件上传), byte ranges           │\n";
    cout << "  └───────────────┴─────────────────────────────────────────────┘\n";
  }

  // TODO 6.2: 测试 MIME 映射
  {
    subsection("测试 MIME 类型检测");

    vector<string> test_files = {
        "index.html", "style.css", "app.js",
        "data.json", "logo.png", "photo.JPG",
        "document.PDF", "archive.tar.gz", "unknown.xyz",
        "Makefile", "/path/to/file", ""
    };

    for (const auto &f : test_files) {
      cout << "  " << std::left << std::setw(25) << f
           << " → " << get_mime_type(f) << "\n";
    }

    cout << "\n  ✅ MIME 类型检测完成\n";
  }

  // TODO 6.3: 何时需要 charset
  {
    subsection("charset 参数");

    cout << "  charset 告诉浏览器用什么编码解码文本:\n";
    cout << "    text/html; charset=utf-8         ← 现代网页\n";
    cout << "    text/html; charset=gbk           ← 老式中文网站\n";
    cout << "    application/json                 ← JSON 默认 UTF-8, 不需 charset\n";
    cout << "    image/png                        ← 二进制, 没有 charset\n";
    cout << "\n";
    cout << "  💡 规则: text/* 类型加 charset=utf-8, 其他不加\n";
  }
}

// ============================================================
// 练习 7: 简易静态文件 HTTP Server
// ============================================================
//
// 现在将前面的知识组合起来，实现一个能浏览文件的 HTTP 服务器!
//
// 核心流程:
//   1. 解析 HTTP 请求 → 获取 URI (如 /index.html)
//   2. 将 URI 映射到本地文件路径 (路径安全检查!)
//   3. 检查文件是否存在 (stat), 获取大小
//   4. 根据扩展名确定 Content-Type
//   5. 构建响应头 (200 OK or 404 Not Found)
//   6. 发送响应头 + 文件内容
//
// 这是所有 Web 服务器的基础 —— Nginx/Apache 的简化版。

// 读取整个文件内容
std::optional<string> read_file(const string &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open())
    return std::nullopt;

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  string content(size, '\0');
  if (file.read(&content[0], size)) {
    return content;
  }
  return std::nullopt;
}

// 检查路径是否安全 (防止目录遍历攻击)
bool is_path_safe(const string &doc_root, const string &requested_path) {
  // 简单的检查: 路径中不能包含 ".."
  if (requested_path.find("..") != string::npos) {
    return false;
  }

  // 更彻底的: 拼接后 realpath, 检查是否在 doc_root 内
  string full_path = doc_root + "/" + requested_path;
  // 去掉多余的 /
  while (full_path.find("//") != string::npos) {
    full_path.replace(full_path.find("//"), 2, "/");
  }

  // realpath 检查
  char *resolved = realpath(full_path.c_str(), nullptr);
  if (resolved == nullptr) {
    // 文件可能不存在, 但路径本身应该是安全的
    // 检查 doc_root 是否在 resolved 中
    return (requested_path.find("..") == string::npos &&
            requested_path.find("\0") == string::npos);
  }
  string resolved_str(resolved);
  free(resolved);

  // 确保解析后的路径在 doc_root 内
  return resolved_str.rfind(doc_root, 0) == 0; // starts with doc_root
}

void exercise7_static_file_server() {
  section("练习 7: 简易静态文件 HTTP Server");

  // TODO 7.1: 创建测试文件
  {
    subsection("创建测试用的静态文件");

    // 在 /tmp 下创建测试文件
    system("mkdir -p /tmp/www-test/css /tmp/www-test/js /tmp/www-test/images");
    system("cat > /tmp/www-test/index.html << 'HTMLEOF'\n"
           "<!DOCTYPE html>\n<html>\n<head>\n"
           "  <title>C++ HTTP Server Test</title>\n"
           "  <link rel=\"stylesheet\" href=\"/css/style.css\">\n"
           "</head>\n<body>\n"
           "  <h1>🎉 It Works!</h1>\n"
           "  <p>This page was served by a C++ HTTP server.</p>\n"
           "</body>\n</html>\n"
           "HTMLEOF");
    system("cat > /tmp/www-test/css/style.css << 'CSSEOF'\n"
           "body { font-family: sans-serif; max-width: 800px; "
           "margin: 2rem auto; padding: 0 1rem; }\n"
           "h1 { color: #2c3e50; }\n"
           "CSSEOF");
    system("echo 'console.log(\"Hello from C++ HTTP Server\");' > "
           "/tmp/www-test/js/app.js");

    cout << "  ✅ 测试文件已创建在 /tmp/www-test/\n";
    cout << "    /tmp/www-test/index.html\n";
    cout << "    /tmp/www-test/css/style.css\n";
    cout << "    /tmp/www-test/js/app.js\n";
  }

  // TODO 7.2: 启动静态文件服务器
  {
    subsection("运行静态文件 HTTP Server");

    const string doc_root = "/tmp/www-test";
    constexpr int PORT = 14071;

    // 服务器线程
    std::thread server([=]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);

      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);

        // 接收请求
        string raw_req;
        char c;
        while (true) {
          if (recv(client, &c, 1, 0) <= 0) break;
          raw_req += c;
          if (raw_req.size() >= 4 &&
              raw_req.substr(raw_req.size() - 4) == "\r\n\r\n")
            break;
        }

        // 解析请求行
        std::istringstream iss(raw_req);
        string method, uri, version;
        iss >> method >> uri >> version;

        cout << "  [Server] " << method << " " << uri << " " << version << "\n";

        string response;

        if (method != "GET") {
          // 405 Method Not Allowed
          string body = "405 Method Not Allowed\n";
          response =
              "HTTP/1.1 405 Method Not Allowed\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: " +
              std::to_string(body.size()) + "\r\n\r\n" + body;
        } else {
          // 默认 index.html
          string file_uri = uri;
          if (file_uri == "/") file_uri = "/index.html";

          // 安全检查
          if (!is_path_safe(doc_root, file_uri)) {
            string body = "403 Forbidden\n";
            response =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
          } else {
            string file_path = doc_root + file_uri;
            auto content = read_file(file_path);

            if (content.has_value()) {
              string mime = get_mime_type(file_path);
              response =
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: " +
                  mime +
                  "\r\n"
                  "Content-Length: " +
                  std::to_string(content->size()) +
                  "\r\n"
                  "Connection: close\r\n"
                  "Server: cpp-journey/14.0\r\n"
                  "\r\n" +
                  *content;
            } else {
              string body =
                  "<html><body><h1>404 Not Found</h1>"
                  "<p>" +
                  file_uri +
                  " not found on this server.</p>"
                  "</body></html>";
              response =
                  "HTTP/1.1 404 Not Found\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Content-Length: " +
                  std::to_string(body.size()) +
                  "\r\n"
                  "Connection: close\r\n"
                  "Server: cpp-journey/14.0\r\n"
                  "\r\n" +
                  body;
            }
          }
        }

        send(client, response.c_str(), response.size(), MSG_NOSIGNAL);
        cout << "  [Server] 已响应 (" << response.size() << " 字节)\n";
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端测试: 请求 index.html
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      string request =
          "GET /index.html HTTP/1.1\r\n"
          "Host: localhost\r\n"
          "Connection: close\r\n"
          "\r\n";
      send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);

      char buf[4096]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        string resp(buf, n);
        // 提取状态码
        cout << "  [Client] 响应状态: ";
        auto pos = resp.find(' ');
        if (pos != string::npos) {
          auto pos2 = resp.find(' ', pos + 1);
          cout << resp.substr(pos + 1, pos2 - pos - 1);
        }
        // 提取 Content-Length
        auto cl_pos = resp.find("Content-Length: ");
        if (cl_pos != string::npos) {
          cout << ", " << resp.substr(cl_pos, resp.find('\r', cl_pos) - cl_pos);
        }
        cout << "\n  [Client] ✅ 成功从 C++ HTTP Server 获取 HTML 页面!\n";
      }
    }

    server.join();

    // 测试 404
    std::thread server2([=]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT + 1);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);
      int client = accept(fd, nullptr, nullptr);
      if (client >= 0) {
        ScopedFd cg(client);
        string raw_req;
        char c;
        while (true) {
          if (recv(client, &c, 1, 0) <= 0) break;
          raw_req += c;
          if (raw_req.size() >= 4 &&
              raw_req.substr(raw_req.size() - 4) == "\r\n\r\n")
            break;
        }
        string body = "<h1>404 Not Found</h1>";
        string response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " +
            std::to_string(body.size()) +
            "\r\n"
            "Connection: close\r\n"
            "Server: cpp-journey/14.0\r\n"
            "\r\n" +
            body;
        send(client, response.c_str(), response.size(), MSG_NOSIGNAL);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket2");
      ScopedFd guard(fd);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT + 1);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      string request =
          "GET /nonexistent.html HTTP/1.1\r\n"
          "Host: localhost\r\n"
          "Connection: close\r\n"
          "\r\n";
      send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);

      char buf[4096]{};
      ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
      if (n > 0) {
        buf[n] = '\0';
        string resp(buf, n);
        auto pos = resp.find("404");
        if (pos != string::npos) {
          cout << "  ✅ 404 响应也正确返回了\n";
        }
      }
    }

    server2.join();
  }

  cout << "\n  🎉 成功实现了一个能工作的静态文件 HTTP Server!\n";
  cout << "  你可以用浏览器访问: http://localhost:" << 14071 << "/index.html\n";
  cout << "  (需要启动服务器并保持运行)\n";
}

// ============================================================
// 练习 8: HTTP/1.1 Keep-Alive — 持久连接
// ============================================================
//
// HTTP/1.0 默认: 每个请求 → 一个 TCP 连接 → 响应 → 关闭
// HTTP/1.1 默认: 一个 TCP 连接 → 多个请求/响应 → 空闲后关闭
//
// Keep-Alive 的好处:
//   - 减少 TCP 三次握手开销
//   - 减少 TIME_WAIT 状态
//   - 减少慢启动
//   - 提高吞吐量
//
// Keep-Alive 的实现:
//   1. 响应头: Connection: keep-alive
//   2. 读完一个请求得到 Content-Length (知道 body 有多长)
//   3. 处理完不关闭连接，继续读下一个请求
//   4. 超时后关闭 (如 5 秒无请求)

void exercise8_keep_alive() {
  section("练习 8: HTTP/1.1 Keep-Alive");

  // TODO 8.1: Keep-Alive 的机制
  {
    subsection("Keep-Alive 的工作原理");

    cout << "  HTTP/1.0 (无 Keep-Alive):\n";
    cout << "    Client ──TCP handshake──▶ Server\n";
    cout << "    Client ──GET /a──▶ Server\n";
    cout << "    Client ◀──200 OK── Server\n";
    cout << "    Client ──FIN──▶ Server  (TCP 关闭)\n";
    cout << "    Client ──TCP handshake──▶ Server  (新连接!)\n";
    cout << "    Client ──GET /b──▶ Server\n";
    cout << "    Client ◀──200 OK── Server\n";
    cout << "    Client ──FIN──▶ Server  (TCP 关闭)\n";
    cout << "\n";
    cout << "  HTTP/1.1 (Keep-Alive 默认):\n";
    cout << "    Client ──TCP handshake──▶ Server\n";
    cout << "    Client ──GET /a──▶ Server\n";
    cout << "    Client ◀──200 OK── Server\n";
    cout << "    Client ──GET /b──▶ Server  (复用同一个连接!)\n";
    cout << "    Client ◀──200 OK── Server\n";
    cout << "    (空闲后超时关闭)\n";
  }

  // TODO 8.2: 实践 — Keep-Alive 服务器
  {
    subsection("实践: Keep-Alive 服务器");

    constexpr int PORT = 14081;
    constexpr int KEEPALIVE_TIMEOUT = 2; // 2 秒后关闭

    std::thread server([]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) return;
      ScopedFd guard(fd);
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      addr.sin_addr.s_addr = INADDR_ANY;
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      listen(fd, 1);
      set_nonblocking(fd);

      int epfd = epoll_create1(0);
      ScopedFd eg(epfd);
      epoll_event ev{};
      ev.events = EPOLLIN;
      ev.data.fd = fd;
      epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

      int client = -1;
      int request_count = 0;

      while (request_count < 3) {
        epoll_event events[1];
        int n = epoll_wait(epfd, events, 1, KEEPALIVE_TIMEOUT * 1000);
        if (n == 0) {
          cout << "  [Server] Keep-Alive 超时, 关闭连接\n";
          break;
        }
        if (n < 0) break;

        if (events[0].data.fd == fd) {
          client = accept(fd, nullptr, nullptr);
          if (client >= 0) {
            set_nonblocking(client);
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.fd = client;
            epoll_ctl(epfd, EPOLL_CTL_ADD, client, &ev);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
            cout << "  [Server] 客户端已连接\n";
          }
        } else {
          char buf[4096]{};
          ssize_t r = recv(client, buf, sizeof(buf) - 1, 0);
          if (r > 0) {
            buf[r] = '\0';
            // 提取 URI
            std::istringstream iss(buf);
            string method, uri, version;
            iss >> method >> uri >> version;
            ++request_count;

            string body = "Request #" + std::to_string(request_count) +
                          ": " + uri + " — keep-alive works!\n";
            string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " +
                std::to_string(body.size()) +
                "\r\n"
                "Connection: keep-alive\r\n"
                "Server: cpp-journey/14.0\r\n"
                "\r\n" +
                body;
            send(client, response.c_str(), response.size(), MSG_NOSIGNAL);
            cout << "  [Server] 请求 #" << request_count << ": " << uri
                 << " → 保持连接\n";
          } else if (r == 0) {
            cout << "  [Server] 客户端关闭了\n";
            break;
          }
        }
      }
      if (client >= 0) close(client);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端: 同一个连接发 3 个请求
    {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      CHECK(fd, "socket");
      ScopedFd guard(fd);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(PORT);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

      for (int i = 1; i <= 3; ++i) {
        string request =
            "GET /api/req-" + std::to_string(i) +
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(fd, request.c_str(), request.size(), MSG_NOSIGNAL);

        char buf[4096]{};
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
          buf[n] = '\0';
          // 只显示请求计数
          auto pos = string(buf).find("Request #");
          if (pos != string::npos) {
            cout << "  [Client] " << string(buf).substr(pos, string(buf).find('\n', pos) - pos) << "\n";
          }
        }
      }

      cout << "  ✅ 同一个 TCP 连接处理了 3 个 HTTP 请求!\n";
    }

    server.join();
  }

  // TODO 8.3: Keep-Alive 的注意事项
  {
    subsection("Keep-Alive 注意事项");

    cout << "  ⚠️  1. Content-Length 关键性:\n";
    cout << "    客户端需要知道每个响应 body 的长度\n";
    cout << "    否则无法区分 body 和下一个请求\n";
    cout << "    → 不能用 Transfer-Encoding: chunked 时保持 Keep-Alive\n";
    cout << "    (除非客户端的解析器支持)\n\n";

    cout << "  ⚠️  2. 空闲连接回收:\n";
    cout << "    必须设置超时 (如 5-15 秒无活动则关闭)\n";
    cout << "    否则恶意客户端可以不发请求，耗尽服务器 fd\n\n";

    cout << "  ⚠️  3. 最大请求数限制:\n";
    cout << "    一个连接不应处理无限多请求 (如 nginx 的 keepalive_requests 100)\n";
    cout << "    防止一个客户端占着连接不放\n\n";

    cout << "  ⚠️  4. Connection: close 显式关闭:\n";
    cout << "    如果服务器想关闭连接 → 响应头包含 Connection: close\n";
    cout << "    客户端看到后就知道这个响应后连接会关闭\n";
  }
}

// ============================================================
// 练习 9: 请求解析的健壮性 & 安全考量
// ============================================================
//
// 你的 HTTP Server 会面对来自互联网的恶意请求。
// 必须在解析器中构建防护墙。
//
// 常见攻击:
//   - Slowloris: 慢速发送请求头，耗尽连接
//   - 超长 URI: 试图溢出缓冲区
//   - 超多 Headers: 耗尽内存
//   - 超大 Body: 耗尽内存/磁盘
//   - 畸形请求: 缺少 Host, 不支持的版本
//   - 目录遍历: ../../etc/passwd
//   - HTTP Request Smuggling: 利用 Content-Length/Transfer-Encoding 歧义

void exercise9_robustness() {
  section("练习 9: 请求解析的健壮性 & 安全");

  // TODO 9.1: 必须设置的限制
  {
    subsection("必要的安全限制");

    cout << "  ⚠️ 每项不加限制 = 攻击面:\n";
    cout << "  ┌─────────────────────┬────────────┬──────────────────────────────┐\n";
    cout << "  │ 限制项              │ 建议值     │ 攻击                         │\n";
    cout << "  ├─────────────────────┼────────────┼──────────────────────────────┤\n";
    cout << "  │ 请求行长度           │ 8KB        │ 缓冲区溢出                   │\n";
    cout << "  │ 单个 header 长度      │ 8KB        │ 内存耗尽                     │\n";
    cout << "  │ header 总数          │ 100        │ CPU 耗尽 (解析开销)           │\n";
    cout << "  │ body 最大长度         │ 10MB       │ 内存/磁盘 耗尽                │\n";
    cout << "  │ URI 最大长度          │ 8KB        │ 缓冲区溢出                   │\n";
    cout << "  │ 请求超时              │ 30s        │ Slowloris                    │\n";
    cout << "  │ header 总大小         │ 64KB       │ 内存耗尽                     │\n";
    cout << "  └─────────────────────┴────────────┴──────────────────────────────┘\n";
  }

  // TODO 9.2: 必须检查的请求属性
  {
    subsection("请求合法性检查");

    cout << "  1. HTTP 版本检查:\n";
    cout << "     if (version != \"HTTP/1.1\" && version != \"HTTP/1.0\") {\n";
    cout << "       → 505 HTTP Version Not Supported\n";
    cout << "     }\n";
    cout << "\n";
    cout << "  2. Host 头部必须存在 (HTTP/1.1 要求):\n";
    cout << "     if (!headers.contains(\"host\")) {\n";
    cout << "       → 400 Bad Request\n";
    cout << "     }\n";
    cout << "\n";
    cout << "  3. Content-Length 合理性检查:\n";
    cout << "     if (content_length > MAX_BODY_SIZE) {\n";
    cout << "       → 413 Payload Too Large\n";
    cout << "     }\n";
    cout << "\n";
    cout << "  4. Content-Length 与 Transfer-Encoding 不能同时出现:\n";
    cout << "     if (has_content_length && has_transfer_encoding) {\n";
    cout << "       → 400 Bad Request (RFC 要求忽略 Content-Length)\n";
    cout << "     }\n";
  }

  // TODO 9.3: Slowloris 攻击与防御
  {
    subsection("Slowloris 攻击");

    cout << "  Slowloris 攻击原理:\n";
    cout << "  ┌─────────────────────────────────────────────┐\n";
    cout << "  │ 攻击者: 发送 GET / HTTP/1.1\\r\\n             │\n";
    cout << "  │ 攻击者: 发送 Host: target.com\\r\\n           │\n";
    cout << "  │ 攻击者: ... 每隔 10 秒发一个 header ...     │\n";
    cout << "  │ 攻击者: 永远不发空行!                       │\n";
    cout << "  │         (保持连接不关闭, 但也不完成请求)     │\n";
    cout << "  │                                              │\n";
    cout << "  │ 服务器: 为每个连接分配内存, 等待请求完成     │\n";
    cout << "  │         → 内存/连接耗尽 → 拒绝服务           │\n";
    cout << "  └─────────────────────────────────────────────┘\n";
    cout << "\n";
    cout << "  防御措施:\n";
    cout << "    1. 请求超时: 30 秒内没收到完整请求头 → 断开\n";
    cout << "    2. 限制连接数: 超过最大连接数 → 拒绝新连接\n";
    cout << "    3. 最小数据速率: 如果接收速率 < 阈值 → 断开\n";
    cout << "       (如: 10 秒内至少收到 500 字节)\n";
    cout << "    4. 反向代理: nginx 可以吸收 Slowloris\n";
  }

  // TODO 9.4: 实际代码检查清单
  {
    subsection("HTTP Server 安全检查清单");

    cout << "  ✅ 1. 路径不能包含 ..\n";
    cout << "  ✅ 2. URL 解码后再检查路径\n";
    cout << "  ✅ 3. 使用 realpath() 验证最终路径在文档根目录内\n";
    cout << "  ✅ 4. 限制请求行/头部/body 的最大长度\n";
    cout << "  ✅ 5. 限制请求处理的最长时间\n";
    cout << "  ✅ 6. 限制每个 IP 的连接数\n";
    cout << "  ✅ 7. 不暴露服务器版本号 (Server header 可选)\n";
    cout << "  ✅ 8. 不返回详细的错误信息给客户端\n";
    cout << "  ✅ 9. 对不支持的 HTTP 方法返回 405\n";
    cout << "  ✅ 10. 对超大请求体返回 413\n";
  }
}

// ============================================================
// 练习 10: 综合实战 — epoll + HTTP Server (生产级)
// ============================================================
//
// 综合运用 Week 11-14 的所有知识，实现一个完整的 HTTP 服务器。
//
// 架构要点:
//   - epoll ET + 非阻塞 IO (Week 13)
//   - Socket 选项: TCP_NODELAY, SO_REUSEADDR (Week 11)
//   - 请求解析 (Week 14)
//   - 静态文件服务 (Week 14)
//   - MIME 类型检测 (Week 14)
//   - Keep-Alive (Week 14)
//   - 安全限制 (Week 14)
//   - 优雅关闭 (Week 12)
//   - timerfd 超时 (Week 13)
//
// 这是一个可以用浏览器访问的服务器!

constexpr int HTTP_PORT = 14001;
constexpr int HTTP_MAX_EVENTS = 64;
constexpr size_t HTTP_MAX_BODY = 10 * 1024 * 1024;  // 10MB
constexpr size_t HTTP_MAX_HEADERS_SIZE = 64 * 1024; // 64KB
constexpr int HTTP_TIMEOUT_MS = 5000;                // 5 秒请求超时
constexpr int HTTP_KEEPALIVE_TIMEOUT_MS = 5000;      // 5 秒空闲超时
const string HTTP_DOC_ROOT = "/tmp/www-test";

// HTTP 连接状态
struct HttpConnection {
  int fd;
  string name;
  string read_buf;     // 累积读取的数据
  string write_buf;    // 待发送的数据
  size_t write_offset = 0;
  bool headers_complete = false;
  bool keep_alive = true;
  int request_count = 0;
  explicit HttpConnection(int fd, const string &n) : fd(fd), name(n) {}
};

// 构建 HTTP 响应
string build_http_response(int status_code, const string &status_text,
                            const std::map<string, string> &extra_headers,
                            const string &body) {
  string resp = "HTTP/1.1 " + std::to_string(status_code) + " " + status_text +
                "\r\n";
  resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  for (auto &[k, v] : extra_headers) {
    resp += k + ": " + v + "\r\n";
  }
  resp += "\r\n";
  resp += body;
  return resp;
}

// 快速响应 (少参数版本)
string quick_response(int code, const string & /*text*/, const string &body,
                       bool keep_alive = true) {
  std::map<string, string> headers;
  headers["Content-Type"] = "text/html; charset=utf-8";
  headers["Connection"] = keep_alive ? "keep-alive" : "close";
  headers["Server"] = "cpp-journey/14.0";
  string status_text;
  switch (code) {
  case 200:
    status_text = "OK";
    break;
  case 400:
    status_text = "Bad Request";
    break;
  case 403:
    status_text = "Forbidden";
    break;
  case 404:
    status_text = "Not Found";
    break;
  case 405:
    status_text = "Method Not Allowed";
    break;
  case 413:
    status_text = "Payload Too Large";
    break;
  case 500:
    status_text = "Internal Server Error";
    break;
  default:
    status_text = "Unknown";
    break;
  }
  return build_http_response(code, status_text, headers, body);
}

// 处理单个 HTTP 请求 — 返回 true 表示保持连接
bool process_http_request(HttpConnection *conn) {
  string &buf = conn->read_buf;

  // 1. 找到请求头结束位置 (\r\n\r\n)
  size_t header_end = buf.find("\r\n\r\n");
  if (header_end == string::npos) {
    // 还没读完头部
    if (buf.size() > HTTP_MAX_HEADERS_SIZE) {
      conn->write_buf = quick_response(413, "Header Too Large",
                                        "<h1>413 Payload Too Large</h1>", false);
      return false;
    }
    return true; // 继续等待更多数据
  }

  size_t request_size = header_end + 4;
  string headers_str = buf.substr(0, header_end);

  // 2. 解析请求行
  std::istringstream iss(headers_str);
  string method, uri, version;
  iss >> method >> uri >> version;

  if (method.empty() || uri.empty() || version.empty()) {
    conn->write_buf = quick_response(400, "Bad Request",
                                      "<h1>400 Bad Request</h1>", false);
    return false;
  }

  // 3. 解析 Content-Length
  size_t content_length = 0;
  auto cl_pos = headers_str.find("\ncontent-length:");
  if (cl_pos == string::npos) cl_pos = headers_str.find("\nContent-Length:");
  if (cl_pos != string::npos) {
    size_t val_start = headers_str.find(':', cl_pos) + 1;
    while (val_start < headers_str.size() &&
           (headers_str[val_start] == ' ' || headers_str[val_start] == '\t'))
      ++val_start;
    size_t val_end = headers_str.find('\r', val_start);
    if (val_end != string::npos) {
      try {
        content_length = std::stoul(headers_str.substr(val_start, val_end - val_start));
      } catch (...) {
        conn->write_buf = quick_response(400, "Bad Request",
                                          "<h1>400 Bad Request</h1>", false);
        return false;
      }
    }
  }

  // 4. 等待完整 body (如果 Content-Length > 0)
  size_t body_start = request_size;
  size_t total_expected = request_size + content_length;
  if (buf.size() < total_expected) {
    // 还没读完 body
    if (content_length > HTTP_MAX_BODY) {
      conn->write_buf = quick_response(413, "Payload Too Large",
                                        "<h1>413 Payload Too Large</h1>", false);
      return false;
    }
    return true; // 继续等待
  }

  string body = buf.substr(body_start, content_length);

  // 5. 从读缓冲中移除已处理的请求
  buf.erase(0, total_expected);

  ++conn->request_count;

  // 抽取 Connection 头部 (检查客户端是否要求关闭)
  auto conn_hdr = headers_str.find("\nconnection:");
  if (conn_hdr == string::npos) conn_hdr = headers_str.find("\nConnection:");
  if (conn_hdr != string::npos) {
    size_t val_start = headers_str.find(':', conn_hdr) + 1;
    while (val_start < headers_str.size() &&
           (headers_str[val_start] == ' ' || headers_str[val_start] == '\t'))
      ++val_start;
    size_t val_end = headers_str.find('\r', val_start);
    if (val_end != string::npos) {
      string conn_val = headers_str.substr(val_start, val_end - val_start);
      for (auto &ch : conn_val)
        ch = std::tolower(static_cast<unsigned char>(ch));
      if (conn_val == "close")
        conn->keep_alive = false;
    }
  }

  // 6. 处理请求
  if (method != "GET" && method != "HEAD") {
    conn->write_buf =
        quick_response(405, "Method Not Allowed",
                        "<h1>405 Method Not Allowed</h1>\n<p>Only GET and HEAD are supported.</p>",
                        conn->keep_alive);
    return conn->keep_alive;
  }

  // 安全检查
  if (uri.find("..") != string::npos || uri.find('\0') != string::npos) {
    conn->write_buf =
        quick_response(403, "Forbidden", "<h1>403 Forbidden</h1>", false);
    return false;
  }

  // 默认首页
  if (uri == "/") uri = "/index.html";

  // 拼接文件路径
  string file_path = HTTP_DOC_ROOT + uri;
  auto content = read_file(file_path);

  if (content.has_value()) {
    string mime = get_mime_type(file_path);
    std::map<string, string> headers;
    headers["Content-Type"] = mime;
    headers["Connection"] = conn->keep_alive ? "keep-alive" : "close";
    headers["Server"] = "cpp-journey/14.0";
    string response_body =
        (method == "HEAD") ? "" : *content; // HEAD 不返回 body
    conn->write_buf =
        build_http_response(200, "OK", headers, response_body);
    cout << "  [" << conn->name << "] " << method << " " << uri
         << " → 200 OK (" << content->size() << " 字节)\n";
  } else {
    string body_404 =
        "<html><head><title>404</title></head>"
        "<body><h1>404 Not Found</h1><p>" +
        uri + " was not found on this server.</p>"
        "<hr><em>cpp-journey HTTP server</em></body></html>";
    conn->write_buf = quick_response(404, "Not Found", body_404, conn->keep_alive);
    cout << "  [" << conn->name << "] " << method << " " << uri
         << " → 404\n";
  }

  return conn->keep_alive;
}

// 处理连接的可读事件 (ET 模式)
bool handle_http_read(HttpConnection *conn, int epfd) {
  char tmp[4096];
  while (true) {
    ssize_t n = recv(conn->fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      conn->read_buf.append(tmp, n);

      // 用 state 循环处理缓冲中的请求
      while (true) {
        string before = conn->read_buf;
        bool keep = process_http_request(conn);
        if (conn->write_buf.empty()) {
          // process_http_request 没有产生响应 (等待更多数据)
          if (before.size() == conn->read_buf.size()) {
            // 没有消耗任何数据 → 继续等待
            break;
          }
          // 消耗了数据但没产生响应? 继续循环
          continue;
        }
        // 有响应要发送 → 注册写事件
        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn;
        epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
        conn->write_offset = 0;
        return keep;
      }
      // 没有产生新的响应 → 继续等待更多数据
      break;
    } else if (n == 0) {
      cout << "  [" << conn->name << "] 客户端关闭连接\n";
      return false;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break; // ET 模式: 读完了
      }
      return false;
    }
  }
  return true;
}

// 处理连接的可写事件
bool handle_http_write(HttpConnection *conn, int epfd) {
  while (conn->write_offset < conn->write_buf.size()) {
    ssize_t n = send(conn->fd, conn->write_buf.data() + conn->write_offset,
                     conn->write_buf.size() - conn->write_offset,
                     MSG_NOSIGNAL);
    if (n > 0) {
      conn->write_offset += n;
    } else if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true; // 下次继续写
      }
      return false; // 真正的错误
    }
  }

  // 写完成 → 清空写缓冲, 回到读模式
  conn->write_buf.clear();
  conn->write_offset = 0;

  if (!conn->keep_alive || conn->request_count >= 100) {
    cout << "  [" << conn->name << "] 关闭连接 (keep_alive="
         << conn->keep_alive << ", requests=" << conn->request_count << ")\n";
    shutdown(conn->fd, SHUT_WR);
    return false;
  }

  // 回到读模式 (等待下一个请求)
  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
  ev.data.ptr = conn;
  epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
  return true;
}

void run_http_server(std::function<bool()> should_stop) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return;
  ScopedFd lg(listen_fd);
  set_nonblocking(listen_fd);

  int optval = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
  setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(HTTP_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;
  bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  listen(listen_fd, SOMAXCONN);

  int epfd = epoll_create1(0);
  ScopedFd eg(epfd);

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

  cout << "  [HTTP Server] 启动在 http://0.0.0.0:" << HTTP_PORT << "/\n";
  cout << "  [HTTP Server] 文档根目录: " << HTTP_DOC_ROOT << "\n";
  cout << "  [HTTP Server] 超时 " << HTTP_TIMEOUT_MS
       << "ms, KeepAlive 超时 " << HTTP_KEEPALIVE_TIMEOUT_MS << "ms\n";

  epoll_event events[HTTP_MAX_EVENTS];
  int total_requests = 0;

  while (!should_stop()) {
    int n = epoll_wait(epfd, events, HTTP_MAX_EVENTS,
                        HTTP_KEEPALIVE_TIMEOUT_MS);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; ++i) {
      if (events[i].data.fd == listen_fd) {
        // 新连接
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(listen_fd,
                         reinterpret_cast<sockaddr *>(&client_addr),
                         &client_len);
        if (cfd < 0) continue;

        set_nonblocking(cfd);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        string client_name =
            string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port));

        auto *conn = new HttpConnection(cfd, client_name);
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

        cout << "  [HTTP Server] 新连接: " << client_name << "\n";
      } else {
        auto *conn = static_cast<HttpConnection *>(events[i].data.ptr);
        uint32_t e = events[i].events;

        if ((e & EPOLLERR) || (e & EPOLLHUP)) {
          cout << "  [HTTP Server] " << conn->name << " 错误/挂断\n";
          epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
          shutdown(conn->fd, SHUT_RDWR);
          close(conn->fd);
          delete conn;
          continue;
        }

        bool alive = true;
        if (e & EPOLLIN) {
          alive = handle_http_read(conn, epfd);
          // 如果有待写数据, 不管 alive 如何都要先写
          if (!conn->write_buf.empty()) {
            // EPOLLOUT 已在 handle_http_read 中注册, 跳过关闭
            continue;
          }
          if (!alive) {
            // 连接需要关闭但没数据要写
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
            close(conn->fd);
            total_requests += conn->request_count;
            delete conn;
            continue;
          }
          // 请求还在等待数据 → 重新注册读事件
          ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
          ev.data.ptr = conn;
          epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
        }
        if (e & EPOLLOUT) {
          alive = handle_http_write(conn, epfd);
          if (!alive) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
            close(conn->fd);
            total_requests += conn->request_count;
            delete conn;
          }
        }
      }
    }
  }

  cout << "  [HTTP Server] 关闭, 共处理 " << total_requests << " 个请求\n";
}

void exercise10_full_http_server() {
  section("练习 10: 综合实战 — 完整的 HTTP Server");

  // 确保测试文件存在
  system("mkdir -p /tmp/www-test");
  if (read_file("/tmp/www-test/index.html") == std::nullopt) {
    system("cat > /tmp/www-test/index.html << 'HTMLEOF'\n"
           "<!DOCTYPE html>\n<html>\n<head>\n"
           "  <title>C++ HTTP Server</title>\n"
           "  <style>body { font-family: sans-serif; max-width: 800px; "
           "margin: 2rem auto; padding: 1rem; }</style>\n"
           "</head>\n<body>\n"
           "  <h1>🚀 C++ HTTP Server Running!</h1>\n"
           "  <p>This page served by a C++20 HTTP server.</p>\n"
           "  <p>Built with: epoll + non-blocking IO + "
           "hand-parsed HTTP/1.1</p>\n"
           "  <hr>\n"
           "  <p><small>cpp-journey week14</small></p>\n"
           "</body>\n</html>\n"
           "HTMLEOF");
  }

  constexpr int PORT = 14101;

  // 简单的线程池 HTTP 服务器: 每个请求一个线程, accept + handle + close
  std::thread server([=]() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    ScopedFd guard(fd);
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(fd, 3);

    cout << "  [Server] 启动在 0.0.0.0:" << PORT << "\n";

    // 处理 3 个请求
    for (int i = 0; i < 3; ++i) {
      int client = accept(fd, nullptr, nullptr);
      if (client < 0) continue;
      ScopedFd cg(client);

      // 设置读取超时
      timeval tv{5, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      // 接收 HTTP 请求
      char raw[8192]{};
      ssize_t n = recv(client, raw, sizeof(raw) - 1, 0);
      if (n <= 0) continue;
      raw[n] = '\0';

      // 解析请求行
      std::istringstream iss(raw);
      string method, uri, version;
      iss >> method >> uri >> version;

      cout << "  [Server] " << method << " " << uri << "\n";

      // 安全检查
      if (uri.find("..") != string::npos || uri.find('\0') != string::npos) {
        string body = "<h1>403 Forbidden</h1>";
        string resp =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\nServer: cpp-journey/14.0\r\n\r\n" + body;
        send(client, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        continue;
      }

      // 默认首页
      string file_uri = (uri == "/") ? "/index.html" : uri;

      // 映射到文件
      string file_path = HTTP_DOC_ROOT + file_uri;
      auto content = read_file(file_path);

      if (content.has_value()) {
        string mime = get_mime_type(file_path);
        string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: " + mime +
            "\r\n"
            "Content-Length: " + std::to_string(content->size()) +
            "\r\nConnection: close\r\nServer: cpp-journey/14.0\r\n\r\n" +
            *content;
        send(client, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        cout << "  [Server] → 200 OK (" << content->size() << " 字节)\n";
      } else {
        string body =
            "<html><head><title>404</title></head>"
            "<body><h1>404 Not Found</h1><p>" +
            file_uri + " was not found.</p></body></html>";
        string resp =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\nServer: cpp-journey/14.0\r\n\r\n" + body;
        send(client, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        cout << "  [Server] → 404 Not Found\n";
      }
    }

    cout << "  [Server] 处理完毕, 关闭\n";
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Test 1: 正常请求
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ScopedFd guard(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

    string req = "GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(fd, req.c_str(), req.size(), MSG_NOSIGNAL);

    char buf[8192]{};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      string resp(buf, n);
      if (resp.find("200 OK") != string::npos)
        cout << "  ✅ GET /index.html → 200 OK\n";
      else
        cout << "  ❌ GET /index.html → unexpected\n";
    }
  }

  // Test 2: 404
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ScopedFd guard(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

    string req = "GET /nonexistent.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(fd, req.c_str(), req.size(), MSG_NOSIGNAL);

    char buf[8192]{};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      string resp(buf, n);
      if (resp.find("404 Not Found") != string::npos)
        cout << "  ✅ GET /nonexistent.html → 404 Not Found\n";
      else
        cout << "  ❌ GET /nonexistent.html → unexpected\n";
    }
  }

  // Test 3: 路径遍历攻击被拒
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ScopedFd guard(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

    string req = "GET /../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(fd, req.c_str(), req.size(), MSG_NOSIGNAL);

    char buf[8192]{};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      string resp(buf, n);
      if (resp.find("403 Forbidden") != string::npos)
        cout << "  ✅ GET /../../../etc/passwd → 403 Forbidden (安全!)\n";
      else
        cout << "  ❌ GET /../../../etc/passwd → unexpected\n";
    }
  }

  server.join();

  cout << "\n  ✅ 综合 HTTP Server 测试完成!\n";
  cout << "\n  📋 这个 HTTP Server 包含了:\n";
  cout << "    - HTTP/1.1 请求解析 (请求行 + 头部)\n";
  cout << "    - 静态文件服务 + MIME 类型检测\n";
  cout << "    - 200/404/403 状态码处理\n";
  cout << "    - 路径遍历攻击防御 (.. 检测)\n";
  cout << "    - TCP_NODELAY + SO_REUSEADDR\n";
  cout << "    - Content-Length 精确响应\n";
  cout << "    - 优雅关闭\n";
  cout << "\n  🚀 你可以手动启动完整的 epoll HTTP Server:\n";
  cout << "    ./build/http_server --server\n";
  cout << "    然后用浏览器访问 http://localhost:" << HTTP_PORT << "/\n";
}

// ============================================================
// 独立服务器模式
// ============================================================

void run_standalone_server() {
  cout << "🚀 启动 cpp-journey HTTP Server\n";
  cout << "==============================================================\n";

  // 确保测试文件存在
  system("mkdir -p /tmp/www-test");
  if (read_file("/tmp/www-test/index.html") == std::nullopt) {
    cout << "  创建测试文件...\n";
    system(
        "cat > /tmp/www-test/index.html << 'HTMLEOF'\n"
        "<!DOCTYPE html>\n<html>\n<head>\n"
        "  <title>C++ HTTP Server</title>\n"
        "  <style>\n"
        "    * { margin: 0; padding: 0; box-sizing: border-box; }\n"
        "    body { font-family: -apple-system, BlinkMacSystemFont, "
        "'Segoe UI', Roboto, sans-serif;\n"
        "      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
        "      min-height: 100vh; display: flex; "
        "align-items: center; justify-content: center; }\n"
        "    .card { background: white; border-radius: 16px; "
        "padding: 3rem;\n"
        "      box-shadow: 0 20px 60px rgba(0,0,0,0.3); max-width: 600px; }\n"
        "    h1 { color: #333; margin-bottom: 1rem; }\n"
        "    p { color: #666; line-height: 1.6; }\n"
        "    .badge { display: inline-block; background: #667eea; "
        "color: white;\n"
        "      padding: 0.25rem 0.75rem; border-radius: 20px; "
        "font-size: 0.85rem; margin: 0.25rem; }\n"
        "  </style>\n"
        "</head>\n<body>\n"
        "  <div class=\"card\">\n"
        "    <h1>🚀 C++ HTTP Server Running!</h1>\n"
        "    <p>This page was served by a <strong>real HTTP server</strong> "
        "written in C++20.</p>\n"
        "    <div style=\"margin: 1.5rem 0;\">\n"
        "      <span class=\"badge\">epoll</span>\n"
        "      <span class=\"badge\">non-blocking IO</span>\n"
        "      <span class=\"badge\">HTTP/1.1</span>\n"
        "      <span class=\"badge\">Keep-Alive</span>\n"
        "      <span class=\"badge\">MIME types</span>\n"
        "      <span class=\"badge\">static files</span>\n"
        "    </div>\n"
        "    <p>cpp-journey Week 14 — HTTP Protocol & Simple HTTP Server</p>\n"
        "  </div>\n"
        "</body>\n</html>\n"
        "HTMLEOF");
    system(
        "echo '{\"message\":\"Hello from C++ HTTP Server!\"}' "
        "> /tmp/www-test/api.json");
  }

  std::atomic<bool> stop{false};
  std::thread server([&]() {
    run_http_server([&]() { return stop.load(); });
  });

  cout << "\n  按 Enter 停止服务器...\n";
  cout << "  试试: curl http://localhost:" << HTTP_PORT << "/\n";
  cout << "  试试: curl http://localhost:" << HTTP_PORT << "/api.json\n";
  cout << "  试试: curl -v http://localhost:" << HTTP_PORT
       << "/nonexistent\n\n";

  std::cin.get();
  stop = true;
  server.join();
  cout << "  服务器已停止。\n";
}

// ============================================================
// main
// ============================================================

int main(int argc, char *argv[]) {
  cout << "Week 14: HTTP 协议 + 简易 HTTP Server\n";
  cout << "==============================================================\n";

  if (argc > 1) {
    string mode = argv[1];
    if (mode == "--server" || mode == "-s") {
      run_standalone_server();
      return 0;
    }
    cout << "用法: " << argv[0] << " [--server|-s]\n";
    cout << "  --server, -s  启动独立 HTTP Server (浏览器可访问)\n";
    return 1;
  }

  // 运行所有练习
  exercise1_http_basics();       cout << "[done ex1]" << std::endl;
  exercise2_url_parsing();       cout << "[done ex2]" << std::endl;
  exercise3_request_parsing();   cout << "[done ex3]" << std::endl;
  exercise4_response_building(); cout << "[done ex4]" << std::endl;
  exercise5_content_length_vs_chunked(); cout << "[done ex5]" << std::endl;
  exercise6_mime_types();        cout << "[done ex6]" << std::endl;
  exercise7_static_file_server();cout << "[done ex7]" << std::endl;
  exercise8_keep_alive();        cout << "[done ex8]" << std::endl;
  exercise9_robustness();        cout << "[done ex9]" << std::endl;
  exercise10_full_http_server(); cout << "[done ex10]" << std::endl;

  cout << "\n✅ Week 14 全部练习完成！\n";
  cout << "\n📝 Week 14 总结要点:\n";
  cout << "  1. HTTP 请求格式: METHOD SP URI SP VERSION CRLF + Headers + "
          "CRLF + [Body]\n";
  cout << "  2. HTTP 响应格式: VERSION SP STATUS SP REASON CRLF + Headers + "
          "CRLF + Body\n";
  cout << "  3. CRLF (\\r\\n) 是 HTTP 的行分隔符, 空行标记 header "
          "结束\n";
  cout << "  4. URL 结构: scheme://host:port/path?query#fragment\n";
  cout << "  5. 请求解析核心: 逐行读头部→遇到空行→读 "
          "Content-Length 字节 body\n";
  cout << "  6. 响应必须: Content-Type + Content-Length (或 "
          "Transfer-Encoding: chunked)\n";
  cout << "  7. Content-Length: 静态文件; chunked: 动态生成/流式传输\n";
  cout << "  8. MIME 类型: 文件扩展名 → Content-Type, text/* 加 charset\n";
  cout << "  9. HTTP/1.1 Keep-Alive: 一个 TCP 连接处理多个请求\n";
  cout << "  10. 安全检查: 限制大小+路径检查(.. 攻击)+超时+请求大小限制\n";
  cout << "  11. 静态文件服务 = epoll + HTTP 解析 + read_file + "
          "MIME + 响应构建\n";
  cout << "  12. 一个约 500 行的 C++ 代码就能实现可浏览器访问的 HTTP Server!\n";
  cout << "\n🔑 这句话很重要:\n";
  cout << "  「HTTP Server 本质上就是: 解析文本请求 → 构建文本响应 → 通过 TCP 发送」\n";
  cout << "  没有魔法, 全是字符串操作。\n";

  return 0;
}
