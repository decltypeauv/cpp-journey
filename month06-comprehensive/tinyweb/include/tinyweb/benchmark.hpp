// ============================================================================
// tinyweb/benchmark.hpp — 性能基准测试框架 (wrk-style)
// ============================================================================
#pragma once
#include "common.hpp"    // std::string, std::vector, std::thread, std::atomic, inet, socket, ...
#include "socket.hpp"    // sock 工具

// ── BenchmarkResult: 基准测试结果 ────────────────────────────────────
struct BenchmarkResult {
  size_t total_requests = 0;        // 总请求数
  size_t success = 0;               // 成功数
  size_t errors = 0;                // 失败数
  double total_time_ms = 0;         // 总耗时 (毫秒)
  double min_latency_us = 1e18;     // 最小延迟 (微秒)
  double max_latency_us = 0;        // 最大延迟 (微秒)
  std::vector<double> latencies;    // 所有延迟样本

  double req_per_sec() const {      // 吞吐量 (请求/秒)
    return total_time_ms > 0 ? total_requests / (total_time_ms / 1000.0) : 0;
  }
  double avg_latency_us() const {   // 平均延迟
    if (latencies.empty()) return 0;
    double sum = 0;
    for (auto l : latencies) sum += l;
    return sum / latencies.size();
  }
  double p50_us() const { return percentile(0.50); } // 中位数延迟
  double p99_us() const { return percentile(0.99); } // P99 延迟

  double percentile(double p) const { // 计算百分位延迟
    if (latencies.empty()) return 0;
    auto sorted = latencies;        // 复制排序
    std::sort(sorted.begin(), sorted.end());
    return sorted[std::min((size_t)(sorted.size() * p), sorted.size() - 1)];
  }

  void print() const {              // 打印格式化报告
    println("  Requests:   ", total_requests);
    println("  Success:    ", success, " (",
            total_requests > 0 ? success * 100.0 / total_requests : 0, "%)");
    println("  Errors:     ", errors);
    println("  Throughput: ", std::fixed, std::setprecision(0), req_per_sec(), " req/s");
    println("  Latency (us): min=", std::setprecision(0), min_latency_us,
            " avg=", avg_latency_us(), " p50=", p50_us(),
            " p99=", p99_us(), " max=", max_latency_us);
  }
};

// run_benchmark: 运行 HTTP 负载测试
// host/port: 目标服务器, path: 请求路径
// total_requests: 总请求数, concurrency: 并发连接数
inline BenchmarkResult run_benchmark(
    const std::string& host, int port, const std::string& path,
    int total_requests = 1000, int concurrency = 10)
{
  BenchmarkResult r;                          // 结果对象
  r.latencies.reserve(total_requests);       // 预分配延迟数组 (避免 realloc)
  std::atomic<int> completed{0};             // 原子计数器: 已完成请求数
  auto t0 = std::chrono::steady_clock::now();// 开始计时

  // worker: 每个并发线程的执行函数
  auto worker = [&] {
    while (completed.fetch_add(1) < total_requests) { // 原子递增并检查
      auto t_req = std::chrono::steady_clock::now(); // 单请求计时开始

      int fd = socket(AF_INET, SOCK_STREAM, 0);     // 创建 TCP socket
      if (fd < 0) { r.errors++; continue; }          // 创建失败 → 计数并重试

      sockaddr_in addr{};                            // 目标地址
      addr.sin_family = AF_INET;                     // IPv4
      addr.sin_port = htons(port);                   // 端口 (主机→网络字节序)
      inet_pton(AF_INET, host.c_str(), &addr.sin_addr); // 解析 IP

      if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { // TCP 连接
        close(fd); r.errors++; continue;             // 连接失败
      }

      // 发送 HTTP 请求
      std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host
                      + "\r\nConnection: close\r\n\r\n";
      if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) <= 0) {
        close(fd); r.errors++; continue;             // 发送失败
      }

      // 接收响应 (简化: 读一次, 不解析)
      char buf[4096];
      recv(fd, buf, sizeof(buf), 0);
      close(fd);                                     // 关闭连接

      auto t_end = std::chrono::steady_clock::now(); // 单请求计时结束
      double us = std::chrono::duration<double, std::micro>(t_end - t_req).count();
      r.min_latency_us = std::min(r.min_latency_us, us); // 更新最小延迟
      r.max_latency_us = std::max(r.max_latency_us, us); // 更新最大延迟
      r.latencies.push_back(us);                     // 记录延迟样本
      r.success++;                                   // 成功计数+1
    }
  };

  std::vector<std::thread> workers;                  // 并发线程列表
  for (int i = 0; i < concurrency; i++)              // 启动 concurrency 个线程
    workers.emplace_back(worker);
  for (auto& w : workers) if (w.joinable()) w.join();// 等待所有线程完成

  auto t1 = std::chrono::steady_clock::now();        // 结束计时
  r.total_requests = total_requests;
  r.total_time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return r;
}
