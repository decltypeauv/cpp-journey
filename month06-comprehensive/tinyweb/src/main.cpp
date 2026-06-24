// ============================================================================
// tinyweb/src/main.cpp — TinyWeb 服务器入口 + 演示
// ============================================================================
// 启动方式:
//   ./tinyweb              → HTTP 服务器 (端口 8080)
//   ./tinyweb bench        → 自基准测试模式
// ============================================================================
#include "tinyweb/server.hpp"        // TinyWeb 服务器类
#include "tinyweb/middleware.hpp"    // 内置中间件 (logger, cors, auth, rate_limit)
#include "tinyweb/benchmark.hpp"     // 基准测试框架
#include <iostream>                  // std::cout
#include <cstring>                   // std::strcmp

int main(int argc, char** argv) {
  // ── 模式判断 ──────────────────────────────────────────────────────
  if (argc > 1 && std::strcmp(argv[1], "bench") == 0) {
    // ==================================================================
    // 基准测试模式: 向目标服务器发送负载
    // ==================================================================
    HR("TinyWeb Benchmark");
    println("  目标: http://localhost:8080/api/hello");
    println("  请求数: 1000 | 并发: 10\n");

    auto result = run_benchmark(            // 运行负载测试
      "127.0.0.1", 8080, "/api/hello",      // 目标地址 + 路径
      1000, 10);                             // 1000 请求, 10 并发
    result.print();                          // 打印统计报告

    println("\n📊 性能分析提示:");
    println("  - perf stat ./tinyweb  → CPU 计数器 (IPC, cache-miss, branch-miss)");
    println("  - perf record + FlameGraph → 火焰图 (找到热点函数)");
    println("  - 对比: epoll vs select vs io_uring");
    return 0;
  }

  // ==================================================================
  // 服务器模式: 启动 HTTP 服务器
  // ==================================================================
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║  TinyWeb v2.0 — 高性能 HTTP 服务器框架                       ║
║  C++20 · epoll · WebSocket · Middleware · TLS               ║
╚══════════════════════════════════════════════════════════════╝)");

  TinyWeb app(8080);                         // 创建服务器 (端口 8080)

  // ═══════════════════════════════════════════════════════════════
  // 1. 全局中间件 (应用到所有路由)
  // ═══════════════════════════════════════════════════════════════
  app.route_middleware(middleware::logger()); // 请求日志: [METHOD /path] → STATUS (TIME)
  app.route_middleware(middleware::cors("*"));// CORS: 允许跨域访问
  println("✅ 全局中间件: Logger + CORS");

  // ═══════════════════════════════════════════════════════════════
  // 2. 路由注册
  // ═══════════════════════════════════════════════════════════════

  // ── 首页 ──────────────────────────────────────────────────────
  app.route("GET", "/", [](const HttpRequest&) {
    return HttpResponse{}.set_html(R"(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>TinyWeb v2.0</title><style>
*{box-sizing:border-box}body{font-family:system-ui;max-width:900px;margin:2rem auto;padding:0 1.5rem;background:#fafafa}
h1{color:#1a1a2e}h2{color:#16213e;margin-top:2rem;border-bottom:2px solid #e94560;padding-bottom:.5rem}
.card{background:#fff;border-radius:8px;padding:1.5rem;margin:1rem 0;box-shadow:0 2px 8px rgba(0,0,0,.08)}
pre{background:#1a1a2e;color:#e6e6e6;padding:1rem;border-radius:6px;overflow-x:auto;font-size:.9rem}
.tag{display:inline-block;padding:2px 8px;border-radius:4px;font-size:.8em;margin:0 4px}
.tag-new{background:#e94560;color:#fff}
</style></head><body>
<h1>🚀 TinyWeb v2.0</h1><p>高性能 HTTP 框架 — C++20 · epoll · 零拷贝</p>
<div class="card"><h2>🆕 Week 30 新增</h2>
<p><span class="tag tag-new">NEW</span> <strong>WebSocket</strong> — RFC 6455 帧解析, 双向实时通信</p>
<p><span class="tag tag-new">NEW</span> <strong>Middleware</strong> — 责任链: CORS/日志/认证/限流</p>
<p><span class="tag tag-new">NEW</span> <strong>TLS/SSL</strong> — OpenSSL 集成 (可选)</p>
<p><span class="tag tag-new">NEW</span> <strong>Benchmark</strong> — wrk-style 负载测试</p>
</div>
<div class="card"><h2>📋 API 端点</h2><pre>
GET  /api/hello      → Hello World JSON
GET  /api/stats      → 服务器统计
GET  /api/protected  → 🔒 Auth 演示 (需 Bearer token)
GET  /api/ratelimit  → ⏱️ 限流演示 (5 req/s)
POST /api/echo       → Echo body
WS   /ws/chat        → WebSocket 演示</pre></div>
<p><small>Month 6 Capstone · 从 Socket 到 HTTP Server · 5 个月知识的结晶</small></p>
</body></html>)");
  });

  // ── API 端点 ──────────────────────────────────────────────────
  app.route("GET", "/api/hello", [](const HttpRequest&) {
    return HttpResponse{}.set_json(
      R"({"message":"Hello from TinyWeb v2.0!","version":"2.0","timestamp":)"
      + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "}");
  });

  app.route("GET", "/api/stats", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({
  "server":"TinyWeb/2.0","cpp":"C++20","arch":"epoll+non-blocking",
  "features":["HTTP/1.1","HTTPS/TLS","WebSocket","Middleware","Router","StaticFiles","ThreadPool","Benchmark"],
  "middleware":["Logger","CORS","Auth","RateLimit"]
})");
  });

  // ── Auth 演示: 需 Bearer tinyweb-secret ────────────────────────
  app.route("GET", "/api/protected", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({"secret":"You have access!","data":"🔐 Protected resource"})");
  }, {middleware::auth("tinyweb-secret")});

  // ── 限流演示: 5 req/s ─────────────────────────────────────────
  app.route("GET", "/api/ratelimit", [](const HttpRequest&) {
    return HttpResponse{}.set_json(R"({"message":"Request allowed","tip":"Try >5 requests/sec to see 429"})");
  }, {middleware::rate_limit(5)});

  // ── Echo ──────────────────────────────────────────────────────
  app.route("POST", "/api/echo", [](const HttpRequest& req) {
    return HttpResponse{}.set_json(
      R"({"echo":")" + req.body + R"(","size":)" + std::to_string(req.body.size()) + "}");
  });

  println("✅ HTTP 路由: / /api/hello /api/stats /api/protected /api/ratelimit /api/echo");

  // ═══════════════════════════════════════════════════════════════
  // 3. WebSocket 端点
  // ═══════════════════════════════════════════════════════════════
  app.route_ws("/ws/chat", [](std::shared_ptr<WsConnection> ws) {
    println("  🔌 WebSocket 连接建立: /ws/chat");
  });
  println("✅ WebSocket: /ws/chat");

  // ═══════════════════════════════════════════════════════════════
  // 4. 静态文件服务
  // ═══════════════════════════════════════════════════════════════
  std::string static_dir = ".";
  if (fs::exists("static")) static_dir = "static";  // 有 static 目录就用它
  app.serve_static(static_dir);
  println("✅ 静态文件: ", fs::absolute(static_dir).string());

  // ═══════════════════════════════════════════════════════════════
  // 5. 启动
  // ═══════════════════════════════════════════════════════════════
  println("\n🚀 监听 http://localhost:8080");
  println("   curl http://localhost:8080/api/hello");
  println("   curl http://localhost:8080/api/protected -H 'Authorization: Bearer tinyweb-secret'");
  println("   curl http://localhost:8080/api/ratelimit  # 快速多次触发 429");
  println("   ./tinyweb bench  # 性能基准测试");
  println("\nPress Ctrl+C to stop.\n");

  struct sigaction sa{};                     // 信号处理: Ctrl+C 优雅退出
  sa.sa_handler = [](int) {};
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  app.start();                               // 启动服务器 (阻塞)

  println("\n👋 Server stopped.");
  return 0;
}
