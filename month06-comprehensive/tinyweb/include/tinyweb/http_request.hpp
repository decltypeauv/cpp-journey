// ============================================================================
// tinyweb/http_request.hpp — HTTP 请求结构 + 解析器状态机
// ============================================================================
#pragma once
#include "common.hpp"    // std::string, std::unordered_map, std::string_view

// ── HttpRequest: HTTP 请求的解析结果 ────────────────────────────────
struct HttpRequest {
  std::string method;                             // 请求方法: GET, POST, PUT, DELETE, ...
  std::string path;                               // 请求路径: /api/hello (含 query string)
  std::string version;                            // HTTP 版本: "HTTP/1.1"
  std::unordered_map<std::string, std::string> headers; // 请求头: { "content-type": "application/json", ... }
  std::string body;                               // 请求体 (POST/PUT 数据)

  // header: 查找请求头, 不存在则返回空 string_view (安全, 无异常)
  std::string_view header(const std::string& key) const {
    auto it = headers.find(key);                  // 查找 (key 已小写化)
    return it != headers.end()                    // 找到?
      ? std::string_view(it->second)              // → 返回值的视图
      : std::string_view{};                       // → 返回空视图
  }

  void reset() {                                  // 重置所有字段 (连接复用)
    method.clear(); path.clear(); version.clear(); headers.clear(); body.clear();
  }
};

// ── HttpParser: HTTP/1.1 请求解析器 (状态机) ────────────────────────
// 设计: 无堆分配增量解析, 每次喂入数据块, 返回是否完成
class HttpParser {
  // 解析状态枚举 (状态机各阶段)
  enum State { kMethod, kPath, kVersion, kHeaders, kBody, kDone, kError };

  HttpRequest _req;          // 解析结果
  std::string _line_buf;     // 当前行缓冲区
  State _state = kMethod;    // 当前状态 (从请求方法开始)
  size_t _content_length = 0;// Content-Length 值 (body 字节数)
  size_t _body_read = 0;     // 已读取的 body 字节数

public:
  void reset() {              // 重置解析器 (连接复用)
    _req.reset(); _line_buf.clear(); _state = kMethod;
    _content_length = _body_read = 0;
  }

  // parse: 增量解析 n 字节数据
  // 返回: true=解析完成 (done 或 error), false=还需要更多数据
  bool parse(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {            // 逐字节处理
      char c = data[i];                           // 当前字符
      switch (_state) {                           // 根据当前状态分发

      case kMethod:                               // 解析请求方法
        if (c == ' ') {                           // 空格 = 方法结束
          _req.method = _line_buf;                // 提取方法
          _line_buf.clear();                      // 清空行缓冲
          _state = kPath;                         // 转下一状态
        } else _line_buf += c;                    // 继续累积方法名
        break;

      case kPath:                                 // 解析请求路径
        if (c == ' ') {                           // 空格 = 路径结束
          _req.path = _line_buf;                  // 提取路径
          _line_buf.clear();
          _state = kVersion;                      // 转下一状态
        } else _line_buf += c;
        break;

      case kVersion:                              // 解析 HTTP 版本
        if (c == '\r') {}                         // 忽略 CR
        else if (c == '\n') {                     // LF = 请求行结束
          _req.version = _line_buf;               // 提取版本
          _line_buf.clear();
          _state = kHeaders;                      // 转 header 解析
        } else _line_buf += c;
        break;

      case kHeaders:                              // 解析请求头
        if (c == '\r') {}                         // 忽略 CR
        else if (c == '\n') {                     // LF = 行结束
          if (_line_buf.empty()) {                // 空行 = header 部分结束
            auto it = _req.headers.find("content-length"); // 查找 Content-Length
            if (it != _req.headers.end()) {       // 有 body?
              _content_length = std::stoull(it->second); // 解析长度
              _state = _content_length > 0 ? kBody : kDone; // 有 body→kBody, 无→完成
            } else _state = kDone;                // 无 Content-Length → 完成
          } else {
            auto colon = _line_buf.find(':');     // 找冒号分隔符
            if (colon != std::string::npos) {
              std::string key = _line_buf.substr(0, colon); // 冒号前 = key
              std::string val = _line_buf.substr(colon + 1);// 冒号后 = value
              if (!val.empty() && val[0] == ' ')  // 去掉 value 开头的空格
                val.erase(0, 1);
              for (auto& ch : key) ch = std::tolower(ch); // key 小写化 (HTTP header 大小写不敏感)
              _req.headers[key] = val;            // 存储 header
            }
            _line_buf.clear();                    // 清空行缓冲
          }
        } else _line_buf += c;                    // 普通字符 → 累积
        break;

      case kBody:                                 // 读取请求体
        _req.body += c;                           // 逐字节累积 body
        _body_read++;                             // 已读计数+1
        if (_body_read >= _content_length)        // 读够 Content-Length 字节
          _state = kDone;                         // → 完成
        break;

      case kDone: case kError:                    // 已完成或出错 → 不再处理
        return true;
      }
    }
    return _state == kDone || _state == kError;   // 返回是否结束
  }

  // 状态查询
  bool done() const { return _state == kDone; }   // 是否成功完成解析
  bool error() const { return _state == kError; } // 是否解析出错
  const HttpRequest& request() const { return _req; } // 获取解析结果 (只读)
  HttpRequest& request() { return _req; }         // 获取解析结果 (可写)
};
