// Day 1: RAII — 资源获取即初始化
// 打开这个文件，按照 TODO 提示完成实现
// 编译: g++ -std=c++20 -Wall -Wextra -o raii raii_scaffold.cpp
// 运行: ./raii

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

// ============================================================
// 练习 1: RAII 文件包装器
// 包装 C 语言的 FILE*，让析构函数自动关闭文件
// ============================================================
class FileGuard {
public:
  // TODO 1.1: 构造函数 — 打开文件
  // 提示: 用 fopen(path, mode)，如果失败要处理（抛异常或设置错误状态）
  FileGuard(const char *path, const char *mode) {
    _file = fopen(path, mode);
    if (_file == nullptr) {
      perror("fopen fail !!!");
      throw std::runtime_error("fopen fail");
    }
  }

  // TODO 1.2: 析构函数 — 自动关闭文件
  // 提示: 只有当 file_ 非空时才 fclose
  ~FileGuard() {
    if (_file != nullptr) {
      fclose(_file);
    }
  }

  // TODO 1.3: 禁止拷贝（文件句柄不能有两个所有者）
  // 提示: 用 = delete
  FileGuard(const FileGuard &) = delete;
  FileGuard &operator=(const FileGuard &) = delete;

  FileGuard(FileGuard &&other) noexcept : _file(other._file) {
    other._file = nullptr;
  }
  FileGuard &operator=(FileGuard &&other) noexcept {
    if (this != &other) {
      if (_file)
        fclose(_file);
      _file = other._file;
      other._file = nullptr;
    }
    return *this;
  }

  // 写入一行
  void writeln(const char *text) {
    if (_file) {
      fprintf(_file, "%s\n", text);
    }
  }
  bool readln(char *buf, int size) {
    if (!_file)
      return false;
    return fgets(buf, size, _file) != nullptr;
  }

  bool is_open() const { return _file != nullptr; }

private:
  FILE *_file = nullptr;
};

// ============================================================
// 练习 2: RAII 堆内存管理
// 管理一个动态分配的 int 数组，使用移动语义避免多余拷贝
// ============================================================
class HeapArray {
public:
  // TODO 2.1: 构造函数 — 分配 size 个 int，全部初始化为 0
  explicit HeapArray(size_t size) : _size(size) {
    _data = new int[_size]{}; // 在这里写
  }

  // TODO 2.2: 析构函数 — 释放内存
  ~HeapArray() {
    delete[] _data; // 在这里写
  }

  // TODO 2.3: 禁止拷贝
  HeapArray(const HeapArray &) = delete;
  HeapArray &operator=(const HeapArray &) = delete;
  // HeapArray(const HeapArray&) = delete;
  // HeapArray& operator=(const HeapArray&) = delete;

  // TODO 2.4: 移动构造
  // HeapArray(HeapArray&& other) noexcept { ... }
  HeapArray(HeapArray &&other) noexcept
      : _data(other._data), _size(other._size) {
    other._data = nullptr;
    other._size = 0;
  }

  // TODO 2.5: 移动赋值
  // HeapArray& operator=(HeapArray&& other) noexcept { ... }
  HeapArray &operator=(HeapArray &&other) noexcept {
    if (this != &other) {
      delete[] _data;
      _data = other._data;
      _size = other._size;
      other._data = nullptr;
      other._size = 0;
    }
    return *this;
  }

  size_t size() const { return _size; }

  int &operator[](size_t i) { return _data[i]; }
  const int &operator[](size_t i) const { return _data[i]; }

private:
  int *_data = nullptr;
  size_t _size = 0;
};

// ============================================================
// 练习 3: 用异常演示 RAII 的好处
// ============================================================
void demo_raii_vs_manual() {
  std::cout << "=== RAII vs 手动管理 演示 ===\n\n";

  // 手动管理 — 如果中间抛异常，内存泄漏
  std::cout << "手动管理: ";
  try {
    int *raw = new int[100];
    std::cout << "分配成功，然后抛异常...\n";
    throw std::runtime_error("出错了！");
    delete[] raw; // 这行永远不会执行 → 内存泄漏
  } catch (const std::exception &e) {
    std::cout << "捕获异常: " << e.what()
              << " — 注意：上面那行 delete 没执行！\n";
  }

  std::cout << "\n";

  // RAII 管理 — 析构函数保证释放
  std::cout << "RAII (HeapArray): ";
  try {
    HeapArray arr(100);
    std::cout << "分配成功，然后抛异常...\n";
    throw std::runtime_error("出错了！");
    // arr 的析构函数会在栈展开时自动调用 → 内存安全释放
  } catch (const std::exception &e) {
    std::cout << "捕获异常: " << e.what();
    std::cout << " — 但 arr 的析构函数已经自动执行了，内存安全！\n";
  }
}

// ============================================================
// 练习 4: 一个简单的 RAII 计时器
// 构造时记录开始时间，析构时输出耗时
// ============================================================

class Timer {
public:
  Timer(const char *name)
      : name_(name), start_(std::chrono::steady_clock::now()) {}

  // TODO 4.1: 析构时计算耗时并输出 "Timer [name]: X ms"
  ~Timer() {
    auto end = std::chrono::steady_clock::now();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start_)
            .count();
    std::cout << "Timer [" << name_ << "]: " << ms << "ms" << std::endl;
  }

private:
  const char *name_;
  std::chrono::steady_clock::time_point start_;
};

// ============================================================
// main — 运行所有练习的测试
// ============================================================
int main() {
  // --- 练习 1: FileGuard ---
  {
    FileGuard f("/tmp/test_raii.txt", "w");
    f.writeln("hello from RAII");
    f.writeln("no need to call fclose!");
  } // f 在这里自动关闭文件

  // 验证文件内容
  std::cout << "验证 FileGuard 写入的内容:\n";
  {
    FileGuard f("/tmp/test_raii.txt", "r");
    char buf[256];
    while (f.readln(buf, sizeof(buf))) {
      std::cout << "  " << buf << "\n";
    }
  }
  std::cout << "\n";

  // --- 练习 2: HeapArray ---
  {
    HeapArray a(5);
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;

    std::cout << "HeapArray a:";
    for (size_t i = 0; i < a.size(); i++)
      std::cout << " " << a[i];
    std::cout << "\n";

    // 验证移动语义 — 把 a 移动到 b，然后检查 a.size() 是否为 0
    HeapArray b(std::move(a));
    std::cout << "移动后 a.size() = " << a.size()
              << " (应为0), b.size() = " << b.size() << "\n";
  }
  std::cout << "\n";

  // --- 练习 3: 异常安全演示 ---
  demo_raii_vs_manual();
  std::cout << "\n";

  // --- 练习 4: Timer ---
  {
    Timer t("sleep_test");
    // 模拟一些耗时操作
    volatile int sum = 0;
    for (int i = 0; i < 1000000; i++)
      sum += i;
    std::cout << "sum = " << sum << "\n";
  } // t 在这里输出耗时

  std::cout << "\n全部练习完成！\n";
  return 0;
}
