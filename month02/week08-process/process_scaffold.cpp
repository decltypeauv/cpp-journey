// Week 08: 进程 — fork / exec / wait / pipe
// 编译: cmake -B build && cmake --build build
// 运行: ./build/process
//
// 本周进入 Linux 进程管理的核心：
// 每个运行中的程序都是一个「进程」。
// 对进程的操作是理解 OS 和所有后续主题（信号、IPC、网络）的基础。

#include <sys/wait.h>  // wait, waitpid, WIFEXITED, WEXITSTATUS
#include <unistd.h>    // fork, exec*, pipe, dup2, getpid, getppid

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using std::cout;
using std::string;
using std::vector;

// ============================================================
// 练习 1: fork() — 一个调用，两个返回
// ============================================================
//
// fork() 是 Linux 创建新进程的唯一方式（底层是 clone()）。
// 调用一次，返回两次：
//   - 在父进程中返回子进程的 PID (> 0)
//   - 在子进程中返回 0
//   - 出错返回 -1
//
// 子进程获得父进程地址空间的「副本」（copy-on-write）。

void exercise1_fork_basics() {
  cout << "=== 练习 1: fork() 基础 ===\n";

  // TODO 1.1: 最基本的 fork — 观察两个返回值
  pid_t pid = fork();

  if (pid < 0) {
    cout << "  ❌ fork 失败: " << std::strerror(errno) << "\n";
    return;
  }

  if (pid == 0) {
    // 子进程
    cout << "  👶 子进程: 我的 PID = " << getpid()
         << ", 父进程 PID = " << getppid() << "\n";
    cout << "  👶 fork() 在子进程中返回了: " << pid << "\n";
    cout << std::flush; // _exit() 不刷新缓冲区，必须手动 flush！
    _exit(0); // ← 子进程演示完毕，立即退出！否则会继续执行后面的代码
  } else {
    // 父进程: 等待子进程结束再继续
    int status;
    waitpid(pid, &status, 0);
    cout << "  👨 父进程: 我的 PID = " << getpid()
         << ", fork() 返回子进程 PID = " << pid << "\n";
  }

  // TODO 1.2: 观察 fork 后的变量独立
  int shared_value = 100;
  pid_t pid2 = fork();

  if (pid2 < 0) {
    cout << "  ❌ 第二次 fork 失败\n";
    return;
  }

  if (pid2 == 0) {
    // 子进程修改 "自己的" 变量
    shared_value = 200;
    cout << "  👶 子进程: shared_value = " << shared_value
         << " (地址: " << &shared_value << ")\n";
    cout << std::flush; // _exit() 不刷新 C++ 缓冲区
    _exit(0); // ← 子进程必须退出！否则会继续执行练习 2, 3, 4...
  } else {
    // 给子进程一点时间先执行（不保证顺序，仅为了输出好看）
    usleep(10000); // 10ms
    int status;
    waitpid(pid2, &status, 0); // 收割子进程
    cout << "  👨 父进程: shared_value = " << shared_value
         << " (地址: " << &shared_value << ")\n";
    cout << "  💡 虚拟地址相同但物理内存不同 — 进程隔离的核心\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 2: exec() 家族 — 替换进程映像
// ============================================================
//
// exec 不是创建新进程，而是用新程序替换当前进程的代码和数据。
// 调用成功后不会返回（因为代码被替换了），失败返回 -1。
//
// 家族成员：
//   execl  — list（变参列表）
//   execv  — vector（数组）
//   execle — list + 自定义环境变量
//   execve — vector + 自定义环境变量（唯一真正的系统调用）
//   execlp — list + PATH 搜索
//   execvp — vector + PATH 搜索
//
// 记忆技巧: l=list, v=vector, e=environment, p=PATH

void exercise2_exec_family() {
  cout << "=== 练习 2: exec() 家族 ===\n";

  // TODO 2.1: fork + exec 组合 — 最经典的模式
  {
    pid_t pid = fork();
    if (pid < 0) {
      cout << "  ❌ fork 失败\n";
      return;
    }

    if (pid == 0) {
      // 子进程：用 execvp 执行 /bin/ls
      // execvp: 在 PATH 中搜索 "ls"，用 vector 传参
      const char *args[] = {"ls", "-la", "/tmp", nullptr};
      execvp("ls", const_cast<char *const *>(args));

      // 如果 exec 成功，下面这行永远不会执行
      // 如果执行到了这里，说明 exec 失败了
      cout << "  ❌ execvp 失败: " << std::strerror(errno) << "\n";
      cout << std::flush;
      _exit(1); // 注意：用 _exit 而不是 exit，避免刷新父进程的缓冲区
    } else {
      // 父进程等待子进程结束
      int status;
      waitpid(pid, &status, 0);
      if (WIFEXITED(status)) {
        cout << "  ✅ ls 命令执行完毕，退出码: " << WEXITSTATUS(status) << "\n";
      }
    }
  }

  // TODO 2.2: 对比 execl vs execv
  // execl: 参数一个一个列出来，以 nullptr 结尾
  // execv: 参数放在数组中
  {
    pid_t pid = fork();
    if (pid == 0) {
      // execl 版本 — 适合参数固定的场景
      // execl("/bin/echo", "echo", "Hello from execl!", nullptr);
      // execv 版本 — 适合参数动态构造的场景
      const char *args[] = {"echo", "Hello from execv!", nullptr};
      execvp("echo", const_cast<char *const *>(args));
      _exit(1);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
  }

  // TODO 2.3: exec 失败的处理
  {
    cout << "  尝试执行一个不存在的程序:\n";
    pid_t pid = fork();
    if (pid == 0) {
      execl("/nonexistent/binary", "binary", nullptr);
      // exec 失败 → 子进程需要立即退出
      cout << "  ❌ 这行不应该出现（除非 exec 失败）\n";
      cout << std::flush;
      _exit(127); // 127 是 shell 对 "command not found" 的约定
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
      cout << "  子进程退出码: " << WEXITSTATUS(status) << " (127 = command not found)\n";
    }
  }

  cout << "\n";
}

// ============================================================
// 练习 3: wait / waitpid — 收割子进程
// ============================================================
//
// 子进程结束后，内核保留其 exit code 和少量信息 → 成为「僵尸进程」
// 父进程必须调用 wait/waitpid 来「收割」— 释放 PCB 资源
// 如果父进程不收割就结束了 → 子进程变成「孤儿」→ 被 init(PID=1) 收养

void exercise3_wait_and_reap() {
  cout << "=== 练习 3: wait / waitpid ===\n";

  // TODO 3.1: waitpid 基本用法 — 等待特定子进程
  {
    pid_t pid = fork();
    if (pid == 0) {
      cout << "  👶 子进程 " << getpid() << " 开始工作...\n";
      usleep(50000); // 模拟工作 50ms
      cout << "  👶 子进程 " << getpid() << " 完成，退出码 42\n";
      cout << std::flush;
      _exit(42);
    } else if (pid > 0) {
      cout << "  👨 父进程等待子进程 " << pid << " 结束...\n";
      int status;
      pid_t waited = waitpid(pid, &status, 0); // 阻塞等待
      cout << "  👨 waitpid 返回: " << waited << "\n";

      // 检查退出方式
      if (WIFEXITED(status)) {
        cout << "  ✅ 正常退出，退出码: " << WEXITSTATUS(status) << "\n";
      } else if (WIFSIGNALED(status)) {
        cout << "  💀 被信号杀死，信号: " << WTERMSIG(status) << "\n";
      }
    }
  }

  // TODO 3.2: 多个子进程的收割顺序
  {
    cout << "\n  创建 3 个子进程...\n";
    vector<pid_t> children;

    for (int i = 0; i < 3; ++i) {
      pid_t pid = fork();
      if (pid == 0) {
        // 子进程: 不同的睡眠时间模拟不同工作时长
        int sleep_ms = (3 - i) * 30000; // 90ms, 60ms, 30ms
        usleep(sleep_ms);
        cout << "  👶 子进程 " << i << " (PID=" << getpid() << ") 退出\n";
        cout << std::flush;
        _exit(i);
      } else if (pid > 0) {
        children.push_back(pid);
      }
    }

    // 父进程按创建顺序收割
    for (size_t i = 0; i < children.size(); ++i) {
      int status;
      pid_t waited = waitpid(children[i], &status, 0);
      cout << "  👨 收割了子进程 " << waited
           << " (退出码 " << WEXITSTATUS(status) << ")\n";
    }
  }

  // TODO 3.3: WNOHANG — 非阻塞等待
  {
    cout << "\n  非阻塞等待演示:\n";
    pid_t pid = fork();
    if (pid == 0) {
      usleep(100000); // 100ms — 父进程会先检查到没结束
      _exit(0);
    } else if (pid > 0) {
      int status;
      pid_t result = waitpid(pid, &status, WNOHANG);
      if (result == 0) {
        cout << "  ⏳ 子进程尚未结束（WNOHANG 返回 0）\n";
        cout << "  现在阻塞等待...\n";
        waitpid(pid, &status, 0);
        cout << "  ✅ 子进程已结束\n";
      }
    }
  }

  cout << "\n";
}

// ============================================================
// 练习 4: 僵尸进程与孤儿进程
// ============================================================
//
// 僵尸 (Zombie):  子进程已结束但父进程未 wait → 占用 PCB 条目
// 孤儿 (Orphan):  父进程先于子进程结束 → 子进程被 init (PID=1) 收养
//
// 关键区别:
//   僵尸 = 子进程死了，没人收尸 → 内存泄漏（PCB 级别）
//   孤儿 = 父进程死了，子进程被收养 → 无害，init 会自动收割

void exercise4_zombie_and_orphan() {
  cout << "=== 练习 4: 僵尸进程与孤儿进程 ===\n";

  // TODO 4.1: 制造一个短暂的僵尸进程并观察
  // 用 WNOHANG 让父进程不立即收割，在另一个终端用 ps aux | grep Z 观察
  {
    pid_t pid = fork();
    if (pid == 0) {
      cout << "  👶 子进程 " << getpid() << " 立即退出（变成僵尸）\n";
      cout << std::flush;
      _exit(0);
    } else if (pid > 0) {
      cout << "  👨 父进程故意等 1 秒再收割...\n";
      cout << "  💀 在这 1 秒内，子进程是僵尸状态 (ps aux 可见 Z+)\n";
      sleep(1);

      int status;
      waitpid(pid, &status, 0);
      cout << "  ✅ 父进程已收割，僵尸被清理\n";
    }
  }

  // TODO 4.2: 孤儿进程 — fork 两次的技巧（daemon 常用）
  {
    cout << "\n  演示孤儿进程:\n";
    pid_t pid = fork();
    if (pid == 0) {
      // 第一代子进程
      pid_t pid2 = fork();
      if (pid2 == 0) {
        // 第二代子进程（孙进程）
        usleep(100000); // 等第一代父进程退出
        pid_t ppid = getppid();
        cout << "  🧒 孙进程 " << getpid() << " 的父进程是 " << ppid << "\n";
        cout << "  💡 如果 ppid=1，说明被 init 收养了\n";
        cout << std::flush;
        _exit(0);
      } else if (pid2 > 0) {
        // 第一代子进程立即退出 → 孙进程变孤儿
        _exit(0);
      }
    } else if (pid > 0) {
      // 原始父进程收割第一代子进程
      int status;
      waitpid(pid, &status, 0);
      usleep(150000); // 给孙进程时间输出
      cout << "  👨 原始父进程已收割第一代子进程\n";
      cout << "  💡 孙进程由 init(PID=1) 自动收割，不会变成僵尸\n";
    }
  }

  cout << "\n";
}

// ============================================================
// 练习 5: pipe() — 父子进程间的单向数据通道
// ============================================================
//
// pipe() 创建一个单向数据通道，返回两个 fd：
//   pipefd[0] — 读端
//   pipefd[1] — 写端
//
// fork 后子进程继承 pipe 的两个 fd，父子可以通过 pipe 通信。
// 这是最简单的 IPC（进程间通信）形式。

void exercise5_pipe_basics() {
  cout << "=== 练习 5: pipe() 基础 ===\n";

  // TODO 5.1: 最基本的 pipe — 父写子读
  {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
      cout << "  ❌ pipe 创建失败: " << std::strerror(errno) << "\n";
      return;
    }

    pid_t pid = fork();
    if (pid < 0) {
      cout << "  ❌ fork 失败\n";
      return;
    }

    if (pid == 0) {
      // 子进程 — 读端
      close(pipefd[1]); // 关闭不需要的写端

      char buffer[128];
      ssize_t n = read(pipefd[0], buffer, sizeof(buffer) - 1);
      if (n > 0) {
        buffer[n] = '\0';
        cout << "  👶 子进程读到: \"" << buffer << "\"\n";
      }
      close(pipefd[0]);
      cout << std::flush;
      _exit(0);
    } else {
      // 父进程 — 写端
      close(pipefd[0]); // 关闭不需要的读端

      const char *msg = "Hello from parent via pipe!";
      cout << "  👨 父进程发送: \"" << msg << "\"\n";
      write(pipefd[1], msg, std::strlen(msg));
      close(pipefd[1]); // 关闭写端 → 子进程 read 返回 0 (EOF)

      int status;
      waitpid(pid, &status, 0);
    }
  }

  // TODO 5.2: 双向通信 — 需要两个 pipe
  {
    cout << "\n  双向通信（两个 pipe）:\n";

    int parent_to_child[2]; // 父 → 子
    int child_to_parent[2]; // 子 → 父

    if (pipe(parent_to_child) == -1 || pipe(child_to_parent) == -1) {
      cout << "  ❌ pipe 创建失败\n";
      return;
    }

    pid_t pid = fork();
    if (pid == 0) {
      // 子进程
      close(parent_to_child[1]); // 关父→子的写端（子只读）
      close(child_to_parent[0]); // 关子→父的读端（子只写）

      // 从父进程读请求
      char request[64];
      ssize_t n = read(parent_to_child[0], request, sizeof(request) - 1);
      if (n > 0) request[n] = '\0';
      cout << "  👶 子进程收到: \"" << request << "\"\n";

      // 向父进程发响应
      const char *response = "pong";
      write(child_to_parent[1], response, std::strlen(response));

      close(parent_to_child[0]);
      close(child_to_parent[1]);
      cout << std::flush;
      _exit(0);
    } else {
      // 父进程
      close(parent_to_child[0]); // 关父→子的读端（父只写）
      close(child_to_parent[1]); // 关子→父的写端（父只读）

      // 向子进程发请求
      const char *request = "ping";
      cout << "  👨 父进程发送: \"" << request << "\"\n";
      write(parent_to_child[1], request, std::strlen(request));

      // 从子进程读响应
      char response[64];
      ssize_t n = read(child_to_parent[0], response, sizeof(response) - 1);
      response[n] = '\0';
      cout << "  👨 父进程收到: \"" << response << "\"\n";

      close(parent_to_child[1]);
      close(child_to_parent[0]);

      int status;
      waitpid(pid, &status, 0);
    }
  }

  // TODO 5.3: pipe + dup2 — 重定向标准输出 (实现简易 popen)
  {
    cout << "\n  pipe + dup2 重定向:\n";

    int pipefd[2];
    if (pipe(pipefd) == -1) {
      cout << "  ❌ pipe 失败\n";
      return;
    }

    pid_t pid = fork();
    if (pid == 0) {
      // 子进程: 把 stdout 重定向到 pipe 的写端
      close(pipefd[0]);           // 关闭读端
      dup2(pipefd[1], STDOUT_FILENO); // stdout → pipe 写端
      close(pipefd[1]);           // 关闭原始 fd

      // 现在子进程的任何 printf/cout 都会进入 pipe
      execl("/bin/date", "date", "+%Y-%m-%d %H:%M:%S", nullptr);
      _exit(1);
    } else {
      close(pipefd[1]); // 关闭写端

      // 父进程从 pipe 读取子进程的输出
      char buffer[256];
      ssize_t n = read(pipefd[0], buffer, sizeof(buffer) - 1);
      if (n > 0) {
        buffer[n] = '\0';
        // 去掉末尾换行
        if (buffer[n - 1] == '\n') buffer[n - 1] = '\0';
        cout << "  👨 父进程捕获到命令输出: \"" << buffer << "\"\n";
      }
      close(pipefd[0]);

      int status;
      waitpid(pid, &status, 0);
      cout << "  💡 这就是 shell 管道 `|` 的底层实现原理\n";
    }
  }

  cout << "\n";
}

// ============================================================
// 练习 6: 综合 — 实现简易的 popen
// ============================================================
//
// popen() 是标准库函数，封装了 fork + exec + pipe + dup2。
// 我们来手动实现一个简化版，理解其内部原理。

class SimplePopen {
  int _pipefd[2]{-1, -1};
  pid_t _child_pid = -1;

public:
  // 启动一个子进程执行 cmd，返回可读的文件描述符
  bool start(const char *cmd) {
    if (pipe(_pipefd) == -1) return false;

    _child_pid = fork();
    if (_child_pid < 0) {
      ::close(_pipefd[0]);
      ::close(_pipefd[1]);
      return false;
    }

    if (_child_pid == 0) {
      // 子进程: stdout → pipe
      ::close(_pipefd[0]);
      dup2(_pipefd[1], STDOUT_FILENO);
      dup2(_pipefd[1], STDERR_FILENO); // 也捕获 stderr
      ::close(_pipefd[1]);

      execl("/bin/sh", "sh", "-c", cmd, nullptr);
      _exit(127);
    }

    // 父进程
    ::close(_pipefd[1]); // 关闭写端
    return true;
  }

  // 读取一行
  string readLine() {
    string line;
    char ch;
    while (::read(_pipefd[0], &ch, 1) == 1) {
      if (ch == '\n') break;
      line += ch;
    }
    return line;
  }

  // 等待子进程结束，返回退出码
  int close() {
    if (_pipefd[0] >= 0) {
      ::close(_pipefd[0]);
      _pipefd[0] = -1;
    }
    int status;
    if (_child_pid > 0) {
      waitpid(_child_pid, &status, 0);
      _child_pid = -1;
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return -1;
  }

  ~SimplePopen() { close(); }
};

void exercise6_simple_popen() {
  cout << "=== 练习 6: 简易 popen 实现 ===\n";

  // TODO 6.1: 使用 SimplePopen 执行命令并读取输出
  {
    SimplePopen cmd;
    if (cmd.start("echo 'Hello from subprocess'; echo 'Line two'")) {
      string line;
      while (!(line = cmd.readLine()).empty()) {
        cout << "  📄 输出: " << line << "\n";
      }
      int exit_code = cmd.close();
      cout << "  ✅ 子进程退出码: " << exit_code << "\n";
    } else {
      cout << "  ❌ 无法启动子进程\n";
    }
  }

  // TODO 6.2: 执行会失败的命令
  {
    cout << "\n  执行不存在的命令:\n";
    SimplePopen cmd;
    if (cmd.start("nonexistent_command_xyz 2>&1")) {
      string line;
      while (!(line = cmd.readLine()).empty()) {
        cout << "  📄 stderr: " << line << "\n";
      }
      int exit_code = cmd.close();
      cout << "  ⚠️ 退出码: " << exit_code << " (127 = command not found)\n";
    }
  }

  cout << "\n";
}

// ============================================================
// 练习 7: 进程生命周期与状态
// ============================================================
//
// Linux 进程状态 (ps aux 的 STAT 列):
//   R — Running/Runnable（运行中或就绪队列中）
//   S — Sleeping (interruptible)（可中断睡眠，等 I/O 等事件）
//   D — Disk sleep (uninterruptible)（不可中断睡眠，等磁盘 I/O）
//   T — Stopped（被 SIGSTOP / ptrace 暂停）
//   Z — Zombie（已退出，等待父进程收割）
//
// 状态转换:
//   创建 (fork) → 就绪 (R) ⇄ 运行 (R) → 睡眠 (S/D) → 就绪 (R) → 退出 → 僵尸 (Z) → 消失

void exercise7_process_lifecycle() {
  cout << "=== 练习 7: 进程生命周期 ===\n";

  // TODO 7.1: 观察进程树 — 每个进程都有父进程（除了 init）
  {
    cout << "  当前进程信息:\n";
    cout << "    我的 PID:  " << getpid() << "\n";
    cout << "    父进程 PID: " << getppid() << "\n";
    cout << "  💡 在终端运行: pstree -p " << getpid() << " 查看进程树\n";
    cout << "  💡 或者: cat /proc/" << getpid() << "/status | grep -E 'Pid|PPid|State'\n";
  }

  // TODO 7.2: 递归 fork — 注意控制（不小心会 fork 炸弹！）
  {
    cout << "\n  安全的层级 fork:\n";
    cout << "  💡 fork 炸弹的危险: 如果不限制，进程数指数增长 → 系统崩溃\n";
    cout << "  💡 安全做法: 总是检查 pid，确保只有一条路径继续 fork\n";

    // 安全的示例: 创建一个进程链（不是树）
    pid_t p = fork();
    if (p == 0) {
      // 只有子进程继续 fork，父进程直接 wait
      pid_t p2 = fork();
      if (p2 == 0) {
        cout << "  👶 孙进程 " << getpid() << " (父=" << getppid() << ")\n";
        cout << std::flush;
        _exit(0);
      } else if (p2 > 0) {
        int status;
        waitpid(p2, &status, 0);
        cout << "  👶 子进程 " << getpid() << " 收割了孙进程\n";
        cout << std::flush;
        _exit(0);
      }
    } else if (p > 0) {
      int status;
      waitpid(p, &status, 0);
      cout << "  👨 根进程 " << getpid() << " 收割了子进程\n";
    }
  }

  // TODO 7.3: 进程的实际内存开销 — copy-on-write
  {
    cout << "\n  Copy-on-Write 观察:\n";
    cout << "  💡 fork 后父子共享物理页，直到某一方写入时才复制\n";
    cout << "  💡 这意味着 fork 很快，而且初始内存开销很小\n";

    // 分配一块大内存，但不修改 → fork 后不会实际复制
    constexpr size_t BIG_SIZE = 10 * 1024 * 1024; // 10MB
    auto *big_array = new char[BIG_SIZE];
    std::memset(big_array, 0, BIG_SIZE); // 先写入触发物理分配

    cout << "  已分配并触达 10MB 内存\n";

    pid_t pid = fork();
    if (pid == 0) {
      // 子进程不修改 big_array → 共享父进程的物理页
      cout << "  👶 子进程: 查看但不修改大数组的某个字节: "
           << static_cast<int>(big_array[0]) << "\n";
      cout << std::flush;
      _exit(0);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
      cout << "  👨 父进程: fork 时没有立即复制 10MB (COW 的威力)\n";
    }

    delete[] big_array;
  }

  cout << "\n";
}

// ============================================================
// 练习 8: 常见陷阱与最佳实践
// ============================================================

void exercise8_pitfalls() {
  cout << "=== 练习 8: 常见陷阱与最佳实践 ===\n";

  // TODO 8.1: fork 后子进程应该用 _exit() 而不是 exit()
  {
    cout << "  💡 fork 后用 _exit vs exit:\n";
    cout << "    exit()  → 调用 atexit 注册的函数 + 刷新所有缓冲区\n";
    cout << "    _exit() → 直接退出，不做任何清理\n";
    cout << "    子进程用 exit() 可能导致:\n";
    cout << "      - 父进程的缓冲区被子进程重复刷新\n";
    cout << "      - atexit 注册的函数被执行两次\n";
    cout << "    规则: 子进程用 _exit()，父进程用 exit()\n";
  }

  // TODO 8.2: fork 后记得关闭不需要的 fd
  {
    cout << "\n  💡 fork 后关闭不用的 pipe fd:\n";
    cout << "    如果写端不关闭，读端永远不会收到 EOF (read 会一直阻塞)\n";
    cout << "    如果读端不关闭，写端在 pipe 满后会阻塞\n";
    cout << "    规则: fork 后立即关闭子进程中不需要的 fd\n";
  }

  // TODO 8.3: 避免 fork 炸弹
  {
    cout << "\n  💡 防止 fork 炸弹:\n";
    cout << "    for (...) { fork(); }  ← 每次迭代都在父子进程中继续！\n";
    cout << "    真正执行的 fork 次数 = 2^n - 1，3 次循环 = 7 个 fork\n";
    cout << "    安全做法: 在子进程分支中 break 或 _exit\n";
  }

  // TODO 8.4: SIGCHLD 与异步收割
  {
    cout << "\n  💡 生产环境中推荐 SIGCHLD 处理:\n";
    cout << "    循环 waitpid(-1, &status, WNOHANG) 直到返回 0\n";
    cout << "    避免 while(wait(NULL) > 0)；如果有子进程长时间运行，会永远阻塞\n";
    cout << "    C++ 最佳实践: 使用 signalfd 或 self-pipe trick 配合 event loop\n";
  }

  cout << "\n";
}

// ============================================================
// main
// ============================================================

int main() {
  // ⚠️ fork + 缓冲 I/O 的重要教训：
  // fork 会复制父进程的地址空间，包括 cout 的缓冲区。
  // 如果 fork 之前有未刷新的数据在缓冲区中，子进程也会有一份 →
  // 子进程退出 (或 flush) 时，父进程的缓冲数据会被重复输出！
  //
  // 解决方案：
  //   1. fork 之前总是 flush (cout << flush / fflush(stdout))
  //   2. 或者设 stdout 为无缓冲模式 (setvbuf / unitbuf)
  //   3. 子进程使用 _exit() 而非 exit() — 不刷新继承的缓冲区
  //
  // 下面我们用方案 2 来避免练习输出的混乱。
  std::cout << std::unitbuf; // 每次输出后自动 flush

  cout << "Week 08: 进程 — fork / exec / wait / pipe\n";
  cout << "==========================================\n\n";

  exercise1_fork_basics();
  exercise2_exec_family();
  exercise3_wait_and_reap();
  exercise4_zombie_and_orphan();
  exercise5_pipe_basics();
  exercise6_simple_popen();
  exercise7_process_lifecycle();
  exercise8_pitfalls();

  cout << "✅ Week 08 全部练习完成！\n";
  cout << "\n📝 总结要点:\n";
  cout << "  1. fork() 创建新进程，调用一次返回两次（子进程返回 0，父进程返回子 PID）\n";
  cout << "  2. exec() 替换进程映像，不创建新进程（成功不返回，失败返回 -1）\n";
  cout << "  3. fork + exec 是 Linux 创建新程序的唯一方式\n";
  cout << "  4. wait/waitpid 收割子进程，防止僵尸\n";
  cout << "  5. pipe + dup2 是 shell 管道 | 的底层实现\n";
  cout << "  6. Copy-on-Write 让 fork 既快又省内存\n";
  cout << "  7. 子进程用 _exit()，不要用 exit()\n";
  cout << "  8. fork 后记得关闭不需要的 fd\n";

  return 0;
}
