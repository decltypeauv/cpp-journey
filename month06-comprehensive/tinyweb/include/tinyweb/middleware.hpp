// ============================================================================
// tinyweb/middleware.hpp — 中间件责任链
// ============================================================================
// 中间件链: M1(before) → M2(before) → Handler → M2(after) → M1(after)
// 用途: 日志, CORS, 认证, 限流, 压缩 — 横切关注点
#pragma once
#include "common.hpp"             // std::function, std::string, std::chrono, std::mutex
#include "http_request.hpp"       // HttpRequest
#include "http_response.hpp"      // HttpResponse

// ── 类型别名 ─────────────────────────────────────────────────────────
// HttpHandler: HTTP 请求处理器 (Router 的最终目标)
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// NextFunc: 中间件调用链中 "下一个处理器" 的工厂函数
// 调用 next() 返回链中后续处理的结果, 中间件可以修改这个结果
using NextFunc = std::function<HttpResponse()>;

// MiddlewareFunc: 中间件函数签名
// 参数: req=当前请求, next=调用链中剩余部分
// 返回: HttpResponse (可以是自己生成的, 也可以是 next() 的修改版本)
using MiddlewareFunc = std::function<HttpResponse(const HttpRequest&, NextFunc)>;

// run_middleware_chain: 递归执行中间件链
// mw: 中间件列表, idx: 当前索引, req: 请求, final_handler: 最终业务处理器
inline HttpResponse run_middleware_chain(
    const std::vector<MiddlewareFunc>& mw,   // 中间件列表 (不可变)
    size_t idx,                              // 当前执行到的位置
    const HttpRequest& req,                  // HTTP 请求
    const HttpHandler& final_handler)        // 最终处理器 (所有中间件通过后执行)
{
  if (idx >= mw.size())                      // 递归终点: 所有中间件都已执行
    return final_handler(req);               // → 直接调用业务处理器
  return mw[idx](req, [&]() {                // 调用当前中间件, 传入 next lambda
    return run_middleware_chain(              // next() 触发递归
      mw, idx + 1, req, final_handler);      // 执行下一个中间件
  });
}

// ============================================================================
// 内置中间件
// ============================================================================
namespace middleware {

// cors: 跨域资源共享中间件
// 自动添加 Access-Control-Allow-* 头, 处理 OPTIONS 预检请求
inline MiddlewareFunc cors(std::string origin = "*") {
  return [origin](const HttpRequest& req, NextFunc next) {
    auto resp = next();                       // 先执行业务逻辑
    resp.set_header("Access-Control-Allow-Origin", origin); // 允许指定源
    resp.set_header("Access-Control-Allow-Methods",         // 允许的 HTTP 方法
                    "GET, POST, PUT, DELETE, OPTIONS");
    resp.set_header("Access-Control-Allow-Headers",         // 允许的请求头
                    "Content-Type, Authorization");
    if (req.method == "OPTIONS") {            // OPTIONS 预检请求
      resp.set_status(204);                   // No Content
      resp.set_body("");                      // 空 body
    }
    return resp;                              // 返回修改后的响应
  };
}

// logger: 请求日志中间件
// 打印 [METHOD /path] → STATUS (TIMEus) 格式日志
inline MiddlewareFunc logger() {
  return [](const HttpRequest& req, NextFunc next) {
    auto t0 = std::chrono::steady_clock::now(); // 记录开始时间
    auto resp = next();                         // 执行业务逻辑
    auto us = std::chrono::duration_cast<std::chrono::microseconds>( // 计算耗时
      std::chrono::steady_clock::now() - t0).count();
    std::cout << "  [" << req.method << " " << req.path << "] → "
              << resp.status << " (" << us << "us)\n"; // 输出日志
    return resp;                                // 透传响应
  };
}

// auth: Bearer Token 认证中间件
// 检查 Authorization: Bearer <secret> 头
inline MiddlewareFunc auth(std::string secret = "tinyweb-secret") {
  return [secret](const HttpRequest& req, NextFunc next) {
    auto auth_hdr = req.header("authorization"); // 获取 Auth 头
    if (!auth_hdr.empty() &&                    // 头非空
        auth_hdr.find("Bearer " + secret) != std::string_view::npos) // 匹配 token?
      return next();                            // 认证通过 → 继续
    return HttpResponse{}.set_status(401)       // 认证失败 → 返回 401
           .set_json(R"({"error":"Unauthorized"})");
  };
}

// rate_limit: 令牌桶限流中间件 (简化: 全局计数器)
// 超过 max_per_sec 请求/秒 → 返回 429
inline MiddlewareFunc rate_limit(int max_per_sec = 100) {
  // 状态在所有请求间共享 (shared_ptr 确保生命周期)
  struct State { std::mutex mtx; int count = 0;
                 std::chrono::steady_clock::time_point reset; };
  auto state = std::make_shared<State>();       // 共享状态
  state->reset = std::chrono::steady_clock::now(); // 初始化计时窗口
  return [state, max_per_sec](const HttpRequest& req, NextFunc next) {
    std::lock_guard lock(state->mtx);           // 线程安全
    auto now = std::chrono::steady_clock::now();
    if (now - state->reset > std::chrono::seconds(1)) { // 超过 1 秒 → 重置窗口
      state->count = 0;                         // 重置计数器
      state->reset = now;                       // 更新窗口起始时间
    }
    if (++state->count > max_per_sec)           // 超过限制?
      return HttpResponse{}.set_status(429)     // → 返回 429 Too Many Requests
             .set_json(R"({"error":"Rate Limit Exceeded"})");
    return next();                              // 未超限 → 继续
  };
}

} // namespace middleware
