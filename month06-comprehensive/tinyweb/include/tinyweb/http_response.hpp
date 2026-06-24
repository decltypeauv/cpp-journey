// ============================================================================
// tinyweb/http_response.hpp — HTTP 响应构建器 (Builder 模式)
// ============================================================================
// 设计灵感: fmtlib 的 output iterator 模式
// 链式 API: HttpResponse{}.set_status(200).set_json("...")
#pragma once
#include "common.hpp"    // std::string, std::map
#include "buffer.hpp"    // Buffer 完整定义 (write_to 需要)

struct HttpResponse {
  int status = 200;                                   // HTTP 状态码 (默认 200)
  std::string status_msg = "OK";                      // 状态描述
  std::unordered_map<std::string, std::string> headers; // 响应头
  std::string body;                                   // 响应体

  // set_status: 设置状态码 (自动查找标准状态消息)
  HttpResponse& set_status(int s, std::string_view msg = "") {
    status = s;                                       // 设置状态码
    if (!msg.empty()) { status_msg = msg; }           // 自定义消息优先
    else {
      static const std::map<int, const char*> msgs = {// 标准 HTTP 状态码 → 消息 映射
        {200,"OK"},{201,"Created"},{204,"No Content"},
        {301,"Moved Permanently"},{302,"Found"},{304,"Not Modified"},
        {400,"Bad Request"},{401,"Unauthorized"},{403,"Forbidden"},{404,"Not Found"},
        {405,"Method Not Allowed"}, {429,"Rate Limit Exceeded"},
        {500,"Internal Server Error"},{503,"Service Unavailable"}
      };
      auto it = msgs.find(s);                         // 查找标准消息
      if (it != msgs.end()) status_msg = it->second;
    }
    return *this;                                     // 链式调用
  }

  // set_header: 设置单个响应头
  HttpResponse& set_header(const std::string& k, const std::string& v) { headers[k] = v; return *this; }

  // set_body: 设置响应体
  HttpResponse& set_body(std::string b) { body = std::move(b); return *this; }

  // 便捷方法: 设置 JSON 响应 (自动设置 Content-Type)
  HttpResponse& set_json(const std::string& json) {
    set_body(json);                                   // 设置 body
    set_header("Content-Type", "application/json");   // 设置 JSON MIME
    return *this;
  }

  // 便捷方法: 设置 HTML 响应
  HttpResponse& set_html(const std::string& html) {
    set_body(html);
    set_header("Content-Type", "text/html; charset=utf-8");
    return *this;
  }

  // set_content_type: 设置 Content-Type 头
  HttpResponse& set_content_type(const std::string& ct) { return set_header("Content-Type", ct); }

  // write_to: 将完整的 HTTP 响应序列化到 Buffer
  void write_to(class Buffer& buf) const {
    // ── 状态行: "HTTP/1.1 200 OK\r\n" ──────────────────────────────
    buf.append("HTTP/1.1 ");
    buf.append(std::to_string(status));
    buf.append(" ");
    buf.append(status_msg);
    buf.append("\r\n");

    // ── 响应头 ────────────────────────────────────────────────────
    auto hdrs = headers;                              // 可修改副本 (用于追加默认头)
    if (!body.empty() && !hdrs.count("Content-Length"))// 有 body 且未指定 Content-Length?
      hdrs["Content-Length"] = std::to_string(body.size()); // → 自动计算
    hdrs["Connection"] = "keep-alive";                // 默认保持连接 (HTTP/1.1)
    hdrs["Server"] = "TinyWeb/2.0";                   // 服务器标识
    for (auto& [k, v] : hdrs) {                       // 遍历所有响应头
      buf.append(k); buf.append(": "); buf.append(v); buf.append("\r\n");
    }
    buf.append("\r\n");                               // 空行 (header 与 body 分隔)

    // ── 响应体 ────────────────────────────────────────────────────
    if (!body.empty()) buf.append(body);              // 追加 body
  }
};
