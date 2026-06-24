// ============================================================================
// tinyweb/thread_pool.hpp — 工作线程池
// ============================================================================
// 用于将 CPU 密集型任务从事件循环线程中分离 (如: 图片处理, 数据库查询)
#pragma once
#include "common.hpp"    // std::thread, std::mutex, std::condition_variable, std::queue, std::function

class ThreadPool {
  std::vector<std::thread> _workers;          // 工作线程列表
  std::queue<std::function<void()>> _tasks;   // 任务队列 (FIFO)
  std::mutex _mtx;                            // 保护任务队列的互斥锁
  std::condition_variable _cv;                // 条件变量: 通知工作线程有新任务
  bool _stop = false;                         // 停止标志

public:
  // 构造函数: 启动 num_threads 个工作线程
  // 默认 = 硬件并发数 (CPU 核心数)
  explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency()) {
    for (size_t i = 0; i < num_threads; i++) {
      _workers.emplace_back([this] {           // 每个工作线程运行此 lambda
        while (true) {
          std::function<void()> task;          // 当前任务
          {
            std::unique_lock lock(_mtx);       // 获取锁
            _cv.wait(lock, [this] {            // 等待: 有任务 或 停止
              return _stop || !_tasks.empty();
            });
            if (_stop && _tasks.empty()) return; // 停止 且 无任务 → 退出线程
            task = std::move(_tasks.front());  // 取队首任务
            _tasks.pop();                      // 移除
          }                                    // 释放锁 (任务执行不持锁)
          task();                              // 执行任务
        }
      });
    }
  }

  // 析构: 通知所有线程停止并 join
  ~ThreadPool() {
    {
      std::lock_guard lock(_mtx);              // 获取锁
      _stop = true;                            // 设置停止标志
    }
    _cv.notify_all();                          // 唤醒所有工作线程
    for (auto& w : _workers)                   // join 所有线程
      if (w.joinable()) w.join();
  }

  // enqueue: 提交任务到线程池
  template <typename F>
  void enqueue(F&& f) {
    {
      std::lock_guard lock(_mtx);              // 获取锁
      _tasks.emplace(std::forward<F>(f));      // 入队 (完美转发)
    }
    _cv.notify_one();                          // 通知一个工作线程
  }
};
