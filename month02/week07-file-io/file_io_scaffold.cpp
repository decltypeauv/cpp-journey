// Week 07: 文件 I/O — 程序与 OS 的数据边界
// 编译: cmake -B build && cmake --build build
// 运行: ./build/fileio
//
// Month 2 开始！从本周起，我们走出纯语言特性，进入程序与操作系统的交互领域。

#include <sys/stat.h>  // open
#include <fcntl.h>      // open flags
#include <unistd.h>     // read, write, close

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using std::cout;
using std::string;
using std::vector;

// ============================================================
// 练习 1: C++ 文件流基础 — ifstream / ofstream / fstream
// ============================================================
//
// C++ 的文件流是 RAII 封装的：构造时打开，析构时自动关闭。
// 这是 C 的 FILE* 和 POSIX fd 没有的便利。

void exercise1_fstream_basics() {
  cout << "=== 练习 1: C++ 文件流基础 ===\n";

  const char *filename = "/tmp/cpp-journey-week07-test.txt";

  // TODO 1.1: 写文件 — ofstream
  {
    std::ofstream out(filename);  // 默认 text mode，自动创建/覆盖
    if (!out.is_open()) {
      cout << "  ❌ 无法打开文件写入: " << filename << "\n";
      return;
    }
    out << "Hello, C++ File I/O!\n";
    out << "第二行: " << 42 << "\n";
    out << "第三行: " << 3.14159 << "\n";

    cout << "  写入完成，文件句柄即将自动关闭（RAII）\n";
  }  // out 析构 → 自动 close，即使中途有异常也会关

  // TODO 1.2: 读文件 — ifstream，逐行读取
  {
    std::ifstream in(filename);
    if (!in.is_open()) {
      cout << "  ❌ 无法打开文件读取: " << filename << "\n";
      return;
    }

    cout << "  读取内容:\n";
    string line;
    int line_num = 1;
    while (std::getline(in, line)) {
      cout << "    L" << line_num++ << ": " << line << "\n";
    }
  }

  // TODO 1.3: 追加模式 — ios::app
  {
    std::ofstream out(filename, std::ios::app);  // append
    out << "第四行: 追加的内容\n";
    cout << "  追加了一行\n";
  }

  // TODO 1.4: 打开模式组合
  // | 模式            | 含义                        |
  // |-----------------|-----------------------------|
  // | std::ios::in     | 读（ifstream 默认）          |
  // | std::ios::out    | 写（ofstream 默认），会清空  |
  // | std::ios::app    | 追加，每次写入都在文件末尾   |
  // | std::ios::ate    | 打开后定位到文件末尾         |
  // | std::ios::trunc  | 清空已有内容（out 的默认）   |
  // | std::ios::binary | 二进制模式，不做换行符转换   |
  //
  // 组合示例: std::ios::in | std::ios::out   → 读写模式
  //          std::ios::out | std::ios::app   → 追加（不清空）
  //          std::ios::in | std::ios::binary → 二进制读

  // 清理
  std::filesystem::remove(filename);
}

// ============================================================
// 练习 2: 流状态 — failbit / badbit / eofbit / goodbit
// ============================================================
//
// 每个流内部维护一个状态掩码 iostate:
//   goodbit = 0        (一切正常)
//   failbit = 1 << 0   (操作失败，但流仍可用)
//   eofbit  = 1 << 1   (到达文件末尾)
//   badbit  = 1 << 2   (不可恢复的错误，流不可再用)

void exercise2_stream_states() {
  cout << "\n=== 练习 2: 流状态 ===\n";

  // TODO 2.1: 检查流状态的几种方式
  const char *filename = "/tmp/cpp-journey-week07-int.txt";

  // 写一个整数文件
  {
    std::ofstream out(filename);
    out << "123\n456\nabc\n789\n";  // abc 不是整数！
  }

  {
    std::ifstream in(filename);
    int val;
    while (in >> val) {  // operator>> 返回流引用，隐式转 bool = !fail()
      cout << "  读到整数: " << val << "\n";
    }
    // 当读到 "abc" 时 >> 失败，failbit 被设置，循环结束

    if (in.eof()) {
      cout << "  原因: 到达 EOF\n";
    } else if (in.fail()) {
      cout << "  原因: failbit 被设置（格式错误）\n";
      // failbit 可以被清除！
      in.clear();   // 清除 failbit
      in.ignore(100, '\n');  // 跳过当前行
      if (in >> val) {
        cout << "  恢复后读到: " << val << "\n";
      }
    } else if (in.bad()) {
      cout << "  原因: badbit（不可恢复）\n";
    }
  }

  // TODO 2.2: 用 exceptions() 让流在出错时抛异常
  {
    std::ifstream in(filename);
    // 设置: failbit 时抛异常，badbit 时也抛
    in.exceptions(std::ios::failbit | std::ios::badbit);

    try {
      int val;
      while (true) {
        in >> val;
        cout << "  异常模式读到: " << val << "\n";
      }
    } catch (const std::ios_base::failure &e) {
      if (in.eof()) {
        cout << "  异常模式: 自然到达 EOF\n";
      } else {
        cout << "  异常模式: 格式错误 — " << e.what() << "\n";
      }
    }
  }

  // TODO 2.3: 流状态速查表
  cout << "\n  流状态操作速查:\n";
  cout << "    if (stream)         — 等价于 !stream.fail()\n";
  cout << "    stream.good()       — goodbit（无任何错误）\n";
  cout << "    stream.eof()        — 检查 eofbit\n";
  cout << "    stream.fail()       — failbit 或 badbit\n";
  cout << "    stream.bad()        — badbit（流已损坏）\n";
  cout << "    stream.clear()      — 清除所有错误标志\n";
  cout << "    stream.exceptions() — 设置哪些位触发异常\n";

  std::filesystem::remove(filename);
}

// ============================================================
// 练习 3: 二进制 I/O — read() / write()
// ============================================================
//
// operator<< 和 operator>> 做的是「格式化」I/O：
//   写 42 实际写入的是字符 '4' 和 '2'（2 字节）
//   读回来需要解析字符串 → 整数
//
// read() 和 write() 做的是「二进制」I/O：
//   写 42（int）实际写入的是 4 个字节的二进制表示
//   读回来直接就是二进制 int，无需解析

struct DataRecord {
  int32_t _id;
  double _value;
  char _name[32];  // 定长字符数组，适合二进制 I/O
};

void exercise3_binary_io() {
  cout << "\n=== 练习 3: 二进制 I/O ===\n";

  const char *filename = "/tmp/cpp-journey-week07-data.bin";

  // TODO 3.1: 写二进制文件
  {
    std::ofstream out(filename, std::ios::binary);
    //                                      ^^^^^^^^^^^^^ 关键！否则换行符会被转换

    vector<DataRecord> records = {
        {1, 3.14, "pi"},
        {2, 2.718, "euler"},
        {3, 1.618, "golden-ratio"},
    };

    for (const auto &rec : records) {
      // write 的参数: (const char* 数据地址, streamsize 字节数)
      out.write(reinterpret_cast<const char *>(&rec), sizeof(rec));
    }

    cout << "  写入了 " << records.size() << " 条记录 ("
         << (records.size() * sizeof(DataRecord)) << " 字节)\n";
  }

  // TODO 3.2: 读二进制文件
  {
    std::ifstream in(filename, std::ios::binary);

    cout << "  读取记录:\n";
    DataRecord rec;
    while (in.read(reinterpret_cast<char *>(&rec), sizeof(rec))) {
      cout << "    id=" << rec._id
           << "  value=" << rec._value
           << "  name=" << rec._name << "\n";
    }

    // gcount() 返回上次 read 实际读了多少字节
    // 如果文件大小不是 sizeof(DataRecord) 的整数倍，最后一条不完整
    if (in.gcount() > 0 && in.gcount() < static_cast<std::streamsize>(sizeof(rec))) {
      cout << "  ⚠ 最后一条记录不完整，只读了 " << in.gcount() << " 字节\n";
    }
  }

  // TODO 3.3: 二进制 vs 文本对比
  {
    // 同样存 4 个整数: 1, 20, 300, 4000
    int nums[] = {1, 20, 300, 4000};

    // 文本模式 — 存 "1\n20\n300\n4000\n" = 13 字节
    {
      std::ofstream out("/tmp/cpp-journey-week07-text.txt");
      for (int n : nums) out << n << '\n';
    }

    // 二进制模式 — 存 4 × sizeof(int) = 16 字节
    {
      std::ofstream out("/tmp/cpp-journey-week07-bin.bin", std::ios::binary);
      out.write(reinterpret_cast<const char *>(&nums), sizeof(nums));
    }

    auto text_sz = std::filesystem::file_size("/tmp/cpp-journey-week07-text.txt");
    auto bin_sz = std::filesystem::file_size("/tmp/cpp-journey-week07-bin.bin");

    cout << "\n  对比:\n";
    cout << "    文本: " << text_sz << " 字节 (人类可读)\n";
    cout << "    二进制: " << bin_sz << " 字节 (精确，无需解析)\n";

    // 文本优势: 人类可读，跨平台（换行符由流处理）
    // 二进制优势: 精确、高效（无需格式化/解析），适合 struct
  }

  std::filesystem::remove(filename);
  std::filesystem::remove("/tmp/cpp-journey-week07-text.txt");
  std::filesystem::remove("/tmp/cpp-journey-week07-bin.bin");
}

// ============================================================
// 练习 4: C++17 std::filesystem — 不再需要 stat / opendir
// ============================================================
//
// C++17 终于有了标准文件系统库。替代 POSIX 的 opendir/readdir/stat。
// 所有操作在 std::filesystem 命名空间下。

namespace fs = std::filesystem;

void exercise4_filesystem() {
  cout << "\n=== 练习 4: std::filesystem ===\n";

  const fs::path test_dir = "/tmp/cpp-journey-week07-fs";

  // TODO 4.1: 创建目录
  {
    // 清理上次的（如果存在）
    fs::remove_all(test_dir);

    // create_directory 创建单个目录，父目录必须存在
    // create_directories 递归创建（类似 mkdir -p）
    bool created = fs::create_directories(test_dir / "sub1" / "sub2");
    cout << "  目录创建: " << (created ? "是" : "否（已存在）") << "\n";

    // 创建几个文件
    for (int i = 1; i <= 3; ++i) {
      auto filepath = test_dir / ("file_" + std::to_string(i) + ".txt");
      std::ofstream out(filepath);
      out << "content " << i;
    }
    // 也在子目录创建文件
    {
      std::ofstream out(test_dir / "sub1" / "sub2" / "deep.txt");
      out << "deep file";
    }
  }

  // TODO 4.2: 遍历目录 — directory_iterator
  {
    cout << "\n  目录 " << test_dir << " 的内容:\n";

    // directory_iterator: 非递归，只遍历一层
    for (const auto &entry : fs::directory_iterator(test_dir)) {
      cout << "    " << entry.path().filename().string();
      if (entry.is_directory()) {
        cout << "/ (目录)";
      } else if (entry.is_regular_file()) {
        cout << " (" << entry.file_size() << " bytes)";
      }
      cout << "\n";
    }
  }

  // TODO 4.3: 递归遍历 — recursive_directory_iterator
  {
    cout << "\n  递归遍历:\n";
    // depth() 在 recursive_directory_iterator 上，不在 directory_entry 上
    auto it = fs::recursive_directory_iterator(test_dir);
    for (; it != fs::recursive_directory_iterator(); ++it) {
      auto indent = string(static_cast<size_t>(it.depth()) * 2, ' ');
      cout << "    " << indent;

      if (it->is_directory()) {
        cout << "📁 " << it->path().filename().string() << "/\n";
      } else {
        cout << "📄 " << it->path().filename().string()
             << " (" << it->file_size() << " bytes)\n";
      }
    }
  }

  // TODO 4.4: path 操作
  {
    fs::path p = test_dir / "sub1" / "sub2" / "deep.txt";

    cout << "\n  path 操作 (" << p << "):\n";
    cout << "    .filename()      = " << p.filename() << "\n";
    cout << "    .extension()     = " << p.extension() << "\n";
    cout << "    .stem()          = " << p.stem() << " (去掉扩展名)\n";
    cout << "    .parent_path()   = " << p.parent_path() << "\n";
    cout << "    .root_path()     = " << p.root_path() << "\n";
    cout << "    .is_absolute()   = " << p.is_absolute() << "\n";
    cout << "    .relative_path() = " << p.relative_path() << "\n";

    // 拼接: operator/ 自动处理分隔符
    fs::path p2 = test_dir / "sub1" / "sub2" / "another.txt";
    cout << "    拼接: " << p2 << "\n";
  }

  // TODO 4.5: 文件属性和检查
  {
    cout << "\n  文件属性:\n";
    cout << "    fs::exists(p)     — 是否存在\n";
    cout << "    fs::file_size(p)  — 文件大小\n";
    cout << "    fs::is_directory(p)\n";
    cout << "    fs::is_regular_file(p)\n";
    cout << "    fs::is_symlink(p)\n";
    cout << "    fs::last_write_time(p) — 最后修改时间\n";
    cout << "    fs::space(p)      — 磁盘空间信息（总/可用/剩余）\n";

    // C++20 可以用 last_write_time 获取修改时间，这里只演示存在性
    cout << "    file_1.txt 存在: " << fs::exists(test_dir / "file_1.txt") << "\n";
  }

  // TODO 4.6: 复制、重命名、删除
  {
    fs::copy_file(test_dir / "file_1.txt", test_dir / "file_1_copy.txt");
    cout << "\n  复制了 file_1.txt → file_1_copy.txt\n";

    fs::rename(test_dir / "file_1_copy.txt", test_dir / "file_1_renamed.txt");
    cout << "  重命名 file_1_copy.txt → file_1_renamed.txt\n";

    fs::remove(test_dir / "file_1_renamed.txt");
    cout << "  删除了 file_1_renamed.txt\n";
  }

  // 清理
  fs::remove_all(test_dir);
}

// ============================================================
// 练习 5: POSIX 文件描述符 — 用 RAII 包装 C 风格 fd
// ============================================================
//
// fstream 底层也是调用 open/read/write/close。
// 了解 POSIX 层可以：
//   1. 理解操作系统真正提供的接口
//   2. 使用 C++ 流不提供的功能（O_DIRECT, O_SYNC, mmap 等）
//   3. 在不能抛异常的场景中（嵌入式、内核模块）

// TODO 5.1: RAII 包装 POSIX 文件描述符
class FileDescriptor {
  int _fd;

 public:
  FileDescriptor() : _fd(-1) {}

  explicit FileDescriptor(int fd) : _fd(fd) {
    if (_fd < 0) {
      throw std::runtime_error("bad file descriptor");
    }
  }

  // 禁止拷贝
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  // 移动
  FileDescriptor(FileDescriptor &&other) noexcept : _fd(other._fd) {
    other._fd = -1;
  }
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (_fd >= 0) ::close(_fd);
      _fd = other._fd;
      other._fd = -1;
    }
    return *this;
  }

  ~FileDescriptor() {
    if (_fd >= 0) {
      ::close(_fd);
    }
  }

  [[nodiscard]] int get() const { return _fd; }
  [[nodiscard]] bool valid() const { return _fd >= 0; }
};

// 工厂函数 — 打开文件，返回 RAII 包装的 fd
FileDescriptor open_file(const char *path, int flags, mode_t mode = 0644) {
  int fd = ::open(path, flags, mode);
  if (fd < 0) {
    throw std::runtime_error(string("open failed: ") + std::strerror(errno));
  }
  return FileDescriptor(fd);
}

void exercise5_posix_fd() {
  cout << "\n=== 练习 5: POSIX 文件描述符 + RAII ===\n";

  const char *filename = "/tmp/cpp-journey-week07-posix.txt";

  // TODO 5.2: 用 POSIX 接口写文件
  {
    auto fd = open_file(filename, O_WRONLY | O_CREAT | O_TRUNC);
    //              flags: 只写 | 创建(不存在时) | 清空(已存在时)

    const char *data = "Hello from POSIX write()!\n第二行\n";
    ssize_t written = ::write(fd.get(), data, std::strlen(data));
    if (written < 0) {
      cout << "  ❌ write 失败: " << std::strerror(errno) << "\n";
    } else {
      cout << "  写入了 " << written << " 字节\n";
    }
  }  // fd 析构 → ::close(fd)，即使前面 throw 也会关

  // TODO 5.3: 用 POSIX 接口读文件
  {
    auto fd = open_file(filename, O_RDONLY);

    char buf[128];
    ssize_t n = ::read(fd.get(), buf, sizeof(buf) - 1);
    if (n < 0) {
      cout << "  ❌ read 失败: " << std::strerror(errno) << "\n";
    } else {
      buf[n] = '\0';  // 手动加 null terminator
      cout << "  读到了 " << n << " 字节:\n";
      cout << "  ---\n" << buf << "  ---\n";
      if (n == static_cast<ssize_t>(sizeof(buf) - 1)) {
        cout << "  (缓冲区满了，可能还有数据)\n";
      }
    }
  }

  // TODO 5.4: open flags 速查
  cout << "\n  POSIX open flags 速查:\n";
  cout << "    O_RDONLY   — 只读\n";
  cout << "    O_WRONLY   — 只写\n";
  cout << "    O_RDWR     — 读写\n";
  cout << "    O_CREAT    — 不存在则创建（需配合 mode）\n";
  cout << "    O_TRUNC    — 打开时清空内容\n";
  cout << "    O_APPEND   — 每次写前定位到末尾\n";
  cout << "    O_EXCL     — 与 O_CREAT 联用，文件存在则失败（原子创建）\n";
  cout << "    O_NONBLOCK — 非阻塞模式\n";
  cout << "    O_SYNC     — 每次写同步到磁盘\n";
  cout << "    O_DIRECT   — 绕过内核缓存，直接 I/O\n";

  // TODO 5.5: 对比 C++ fstream vs POSIX fd vs C FILE*
  cout << "\n  三层 I/O 体系对比:\n";
  cout << "    ┌─────────────────────────────────────────────────────┐\n";
  cout << "    │ C++ fstream  │ 类型安全，RAII，异常可选    │ 最高层 │\n";
  cout << "    │ C FILE*      │ 缓冲 I/O，fopen/fread/fprintf │ 中间层 │\n";
  cout << "    │ POSIX fd     │ 无缓冲，open/read/write      │ 最低层 │\n";
  cout << "    └─────────────────────────────────────────────────────┘\n";
  cout << "    fstream 底层调用 C FILE* 或直接调用 POSIX，\n";
  cout << "    取决于标准库实现（libstdc++ 用 FILE*，libc++ 直接调 POSIX）。\n";

  std::filesystem::remove(filename);
}

// ============================================================
// 练习 6: 实战 — 一个简单的日志写入器
// ============================================================
//
// 综合运用: RAII 文件管理 + 格式化写入 + 错误处理 + flush 策略

class LogWriter {
  std::ofstream _file;
  string _path;
  size_t _line_count = 0;

 public:
  explicit LogWriter(string path) : _path(std::move(path)) {
    // 追加模式打开，每行立即刷新（对于日志很重要！）
    _file.open(_path, std::ios::app);
    if (!_file.is_open()) {
      throw std::runtime_error("无法打开日志文件: " + _path);
    }
    // 日志文件: 写每行后自动 flush（防止 crash 时丢失）
    // 注意: 频繁 flush 影响性能，高吞吐场景用异步日志
  }

  void info(const string &msg) {
    _file << "[INFO]  " << msg << std::endl;  // endl 会 flush
    ++_line_count;
  }

  void warning(const string &msg) {
    _file << "[WARN]  " << msg << std::endl;
    ++_line_count;
  }

  void error(const string &msg) {
    _file << "[ERROR] " << msg << std::endl;
    ++_line_count;
  }

  [[nodiscard]] size_t line_count() const { return _line_count; }
  [[nodiscard]] const string &path() const { return _path; }
};

void exercise6_real_world() {
  cout << "\n=== 练习 6: 实战 — 日志写入器 ===\n";

  const char *logpath = "/tmp/cpp-journey-week07.log";

  {
    LogWriter logger(logpath);
    logger.info("服务启动");
    logger.info("正在处理数据...");
    logger.warning("发现可疑输入");
    logger.error("处理失败: 超时");
    logger.info("重试成功");

    cout << "  写入了 " << logger.line_count() << " 条日志到 " << logger.path() << "\n";
  }  // logger 析构 → 文件关闭

  // 验证: 读取日志文件
  {
    std::ifstream in(logpath);
    cout << "  日志内容:\n";
    string line;
    while (std::getline(in, line)) {
      cout << "    " << line << "\n";
    }
  }

  std::filesystem::remove(logpath);
}

// ============================================================
// 练习 7: 额外思考 — 字符串流（内存中的 I/O）
// ============================================================
//
// std::stringstream 像文件一样读写字符串。
// 用途: 格式化字符串、序列化/反序列化、单元测试 mock。

void exercise7_stringstream() {
  cout << "\n=== 练习 7: 字符串流（内存中的 I/O）===\n";

  // TODO 7.1: ostringstream — 把输出"写入"字符串
  {
    std::ostringstream oss;
    oss << "整数: " << 42 << ", 浮点: " << std::fixed << 3.14159;
    oss << ", hex: " << std::hex << 255;

    string result = oss.str();  // 拿到完整字符串
    cout << "  ostringstream: " << result << "\n";
  }

  // TODO 7.2: istringstream — 从字符串"读取"
  {
    string data = "42 3.14 hello";
    std::istringstream iss(data);
    int i;
    double d;
    string s;
    iss >> i >> d >> s;
    cout << "  istringstream: i=" << i << ", d=" << d << ", s=" << s << "\n";
  }

  // TODO 7.3: stringstream 的实用场景 — CSV 解析
  {
    string csv = "1,alice,100\n2,bob,200\n3,charlie,300\n";
    std::istringstream iss(csv);
    string line;
    while (std::getline(iss, line)) {
      std::istringstream line_ss(line);
      string id, name, score;
      std::getline(line_ss, id, ',');
      std::getline(line_ss, name, ',');
      std::getline(line_ss, score, ',');  // 第三个分隔符是 ',' 但后面是 '\n'
      cout << "    id=" << id << " name=" << name << " score=" << score << "\n";
    }
  }
}

// ============================================================
// 总结思考:
// 1. C++ iostream 的 RAII 如何保证文件描述符不泄漏？
// 2. failbit / badbit / eofbit 的区别？什么时候用 exceptions()？
// 3. 二进制 I/O 比格式化 I/O 快在哪里？什么时候用哪个？
// 4. std::filesystem 相比 POSIX stat/opendir 有什么优势？
// 5. 如果要在没有异常的环境中使用文件 I/O，怎么设计 RAII？
// 6. std::endl vs '\n' — 什么时候该用哪个？
// ============================================================

int main() {
  exercise1_fstream_basics();
  exercise2_stream_states();
  exercise3_binary_io();
  exercise4_filesystem();
  exercise5_posix_fd();
  exercise6_real_world();
  exercise7_stringstream();

  cout << "\n✅ Week 07 全部练习完成！\n";
  return 0;
}
