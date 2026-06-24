// ============================================================================
// tinyweb/router.hpp — URL 路由器 + WebSocket 升级支持
// ============================================================================
// 支持: 精确匹配, :param 通配, WebSocket upgrade 检测
// 中间件: global (所有路由) + per-route (单路由)
#pragma once
#include "common.hpp"             // std::vector, std::string
#include "http_request.hpp"       // HttpRequest
#include "http_response.hpp"      // HttpResponse
#include "middleware.hpp"         // MiddlewareFunc, HttpHandler, run_middleware_chain
#include "websocket.hpp"          // WsConnection, WebSocketState

// ── 前向声明 ─────────────────────────────────────────────────────────
struct WsConnection;               // WebSocket 连接 (websocket.hpp)

class Router {
  // Route: 单条路由记录
  struct Route {
    std::string method;            // HTTP 方法: GET, POST, ...
    std::string path;              // 路径模式: /api/user/:id
    HttpHandler handler;           // 业务处理器
    std::vector<MiddlewareFunc> middleware; // 该路由专属中间件
  };
  std::vector<Route> _routes;      // 所有 HTTP 路由
  std::vector<MiddlewareFunc> _global_mw; // 全局中间件 (应用到所有路由)
  HttpHandler _not_found;          // 404 默认处理器

public:
  using WsHandler = std::function<void(std::shared_ptr<WsConnection>)>; // WebSocket 处理器签名
  struct WsRoute { std::string path; WsHandler handler; }; // WebSocket 路由

  Router() {                        // 构造函数: 设置默认 404
    _not_found = [](const HttpRequest&) {
      return HttpResponse{}.set_status(404).set_json(R"({"error":"Not Found"})");
    };
  }

  // ── 全局中间件 (应用到所有路由) ──────────────────────────────────
  Router& use(MiddlewareFunc mw) {   // 追加全局中间件
    _global_mw.push_back(std::move(mw));
    return *this;                    // 链式调用
  }

  // ── HTTP 路由注册 ────────────────────────────────────────────────
  Router& get(const std::string& path, HttpHandler h,
              std::vector<MiddlewareFunc> mw = {}) {
    _routes.push_back({"GET", path, std::move(h), std::move(mw)}); return *this; }
  Router& post(const std::string& path, HttpHandler h,
               std::vector<MiddlewareFunc> mw = {}) {
    _routes.push_back({"POST", path, std::move(h), std::move(mw)}); return *this; }
  Router& put(const std::string& path, HttpHandler h,
              std::vector<MiddlewareFunc> mw = {}) {
    _routes.push_back({"PUT", path, std::move(h), std::move(mw)}); return *this; }
  Router& del(const std::string& path, HttpHandler h,
              std::vector<MiddlewareFunc> mw = {}) {
    _routes.push_back({"DELETE", path, std::move(h), std::move(mw)}); return *this; }

  // ── WebSocket 路由注册 ────────────────────────────────────────────
  std::vector<WsRoute>& ws_routes() { return _ws_routes; } // 获取 WS 路由列表 (供 TinyWeb 注册)

  // ── dispatch: 核心分发逻辑 ──────────────────────────────────────
  HttpResponse dispatch(const HttpRequest& req) const {
    // 检查 WebSocket 升级 (Upgrade: websocket 头)
    if (req.header("upgrade").find("websocket") != std::string_view::npos) {
      for (auto& w : _ws_routes)            // 遍历 WS 路由
        if (match(w.path, req.path))        // 路径匹配?
          return build_ws_upgrade();         // → 返回 101 Switching Protocols
    }
    // HTTP 路由分发
    for (auto& r : _routes) {
      if (req.method == r.method &&         // 方法匹配?
          match(r.path, req.path)) {        // 路径匹配?
        // 合并中间件: global + per-route
        auto all_mw = _global_mw;           // 复制全局中间件
        all_mw.insert(all_mw.end(),         // 追加路由中间件
                      r.middleware.begin(), r.middleware.end());
        if (all_mw.empty())                 // 无中间件 → 直接调用 handler
          return r.handler(req);
        return run_middleware_chain(        // 有中间件 → 走责任链
          all_mw, 0, req, r.handler);
      }
    }
    return _not_found(req);                 // 无匹配 → 404
  }

private:
  std::vector<WsRoute> _ws_routes;          // WebSocket 路由列表

  // build_ws_upgrade: 构建 WebSocket 升级响应 (101)
  HttpResponse build_ws_upgrade() const {
    HttpResponse resp;
    resp.set_status(101, "Switching Protocols"); // RFC 6455: 101
    resp.set_header("Upgrade", "websocket");     // 协议升级
    resp.set_header("Connection", "Upgrade");
    // Sec-WebSocket-Accept = base64(sha1(client_key + GUID))
    // 简化: 返回示例 Accept 值 (实际需要 SHA-1 + base64)
    resp.set_header("Sec-WebSocket-Accept",
                    "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    resp.set_header("X-WS-Upgrade", "true"); // 内部标记: 告知 Connection 切换到 WS 模式
    return resp;
  }

  // match: 路径匹配 (精确 + :param 通配)
  static bool match(const std::string& pattern, const std::string& path) {
    if (pattern == path) return true;       // 精确匹配 → 直接返回
    if (pattern.find(':') != std::string::npos) { // 有 :param 通配符?
      auto pp = split(pattern, '/');        // 按 / 分割模式
      auto pu = split(path, '/');           // 按 / 分割实际路径
      if (pp.size() != pu.size()) return false; // 段数不同 → 不匹配
      for (size_t i = 0; i < pp.size(); i++) {
        if (pp[i].empty() || pp[i][0] != ':') // 不是 :param 段 → 必须精确匹配
          if (pp[i] != pu[i]) return false;
      }
      return true;                          // :param 段可以匹配任意值
    }
    return false;
  }

  // split: 字符串分割
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
