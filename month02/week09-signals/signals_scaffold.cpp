// Week 09: 信号 — Signal 处理、发送、阻塞与安全模式
// 编译: cmake -B build && cmake --build build
// 运行: ./build/signals
//
// 信号是 Linux 进程间异步通知的机制。
// 它是 OS 打断进程正常执行流的"软件中断"。
// 理解信号是写出健壮的 Linux 服务程序的基础。

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>  // signalfd (Linux-specific)
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using std::cout;
using std::string;

// ============================================================
// 练习 1: signal() — 最基础（但不推荐）的信号处理
// ============================================================
//
// signal(signum, handler) 为指定信号注册一个处理函数。
// 当进程收到信号时，handler 被调用。
//
// ⚠️ signal() 的行为在不同 Unix 系统上不一致：
//   - System V: 信号处理一次后自动重置为 SIG_DFL
//   - BSD/Linux: 保持注册，不需要重新 signal
//   生产代码应使用 sigaction() 而非 signal()

// 全局变量 — 信号处理器中唯一"安全"的非局部状态
// （实际上 volatile sig_atomic_t 是标准保证的）
volatile std::sig_atomic_t g_signal_count = 0;
volatile std::sig_atomic_t g_last_signal = 0;

void simple_handler(int signum) {
  g_signal_count = g_signal_count + 1;
  g_last_signal = signum;

  // ⚠️ 信号处理器中安全能做的事情非常有限：
  //   ✅ 可以安全使用:
  //      - 读写 volatile sig_atomic_t 变量
  //      - 调用 _exit()
  //      - 调用 write() 写入文件描述符
  //   ❌ 绝对不能:
  //      - malloc / free / new / delete
  //      - cout / printf (不是 async-signal-safe)
  //      - 获取锁 (mutex/semaphore)
  //      - 调用任何非 async-signal-safe 的函数
  //
  // 下面这行 write 是安全的：
  const char msg[] = "  ⚡ 信号处理器被调用!\n";
  write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

void exercise1_signal_basics() {
  cout << "=== 练习 1: signal() 基础 ===\n";

  // TODO 1.1: 注册 SIGINT 的处理器（Ctrl+C）
  {
    cout << "  注册 SIGINT 处理器...\n";
    auto old_handler = std::signal(SIGINT, simple_handler);
    if (old_handler == SIG_ERR) {
      cout << "  ❌ 无法注册信号处理器\n";
      return;
    }

    cout << "  已注册。试着按 Ctrl+C 发送 SIGINT...\n";
    cout << "  (我们将用 raise() 模拟，避免真的需要你按键)\n";

    // raise() — 向自己发送信号
    raise(SIGINT);
    cout << "  收到信号次数: " << g_signal_count << "\n";
    cout << "  最后一个信号: " << g_last_signal << " (SIGINT=" << SIGINT << ")\n";

    // 恢复默认处理器
    std::signal(SIGINT, old_handler);
  }

  // TODO 1.2: 信号的特殊值
  {
    cout << "\n  信号的特殊处理值:\n";
    cout << "    SIG_DFL (" << reinterpret_cast<void *>(SIG_DFL)
         << ") — 默认处理（通常终止进程）\n";
    cout << "    SIG_IGN (" << reinterpret_cast<void *>(SIG_IGN)
         << ") — 忽略信号\n";
    cout << "    SIG_ERR (" << reinterpret_cast<void *>(SIG_ERR)
         << ") — signal() 返回值，表示出错\n";

    // 演示忽略信号 — 用 SIGUSR1 避免干扰后续练习
    cout << "    暂时忽略 SIGUSR1...\n";
    std::signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1); // 这个 SIGUSR1 被忽略，什么也不发生
    cout << "    SIGUSR1 被忽略，计数器没变: " << g_signal_count << "\n";

    std::signal(SIGUSR1, SIG_DFL); // 恢复默认
    cout << "    已恢复 SIGUSR1 默认处理\n";
  }

  // TODO 1.3: SIGKILL 和 SIGSTOP 不能被捕获或忽略
  {
    cout << "\n  💡 SIGKILL 和 SIGSTOP 不能被捕获:\n";
    cout << "    SIGKILL (9)  — 必杀，内核直接终止进程\n";
    cout << "    SIGSTOP (19) — 必停，内核直接暂停进程\n";
    cout << "    这意味着: signal(SIGKILL, handler) 无效\n";
    cout << "    这是保证 sysadmin 总能杀死失控进程的最后手段\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 2: sigaction() — 正确注册信号处理器的姿势
// ============================================================
//
// sigaction 比 signal 更强大和可控：
//   - 行为跨平台一致（不会自动重置）
//   - 可以指定在信号处理器执行期间阻塞哪些信号
//   - 可以获取信号发送时的更多上下文（siginfo_t）
//   - 支持 SA_RESTART 等标志控制被中断的系统调用

volatile std::sig_atomic_t g_sigaction_count = 0;

// 带扩展信息的信号处理器（需要 SA_SIGINFO 标志）
void sigaction_handler(int signum, siginfo_t *info, void * /*ctx*/) {
  g_sigaction_count = g_sigaction_count + 1;

  // siginfo_t 包含：
  //   si_signo — 信号编号
  //   si_pid   — 发送者的 PID（对于 kill 发送的信号）
  //   si_uid   — 发送者的 UID
  //   si_code  — 信号来源（SI_USER, SI_KERNEL, SI_QUEUE 等）

  char buf[128];
  int len = snprintf(buf, sizeof(buf),
                     "  ⚡ sigaction: 信号=%d, 发送者PID=%d, 次数=%d\n", signum,
                     info->si_pid, static_cast<int>(g_sigaction_count));
  write(STDOUT_FILENO, buf, len);
  // 注意：用了 snprintf — 严格来说不保证 async-signal-safe
  // 生产代码中更安全的做法是用 write + 手动格式化
}

void exercise2_sigaction() {
  cout << "=== 练习 2: sigaction() ===\n";

  // TODO 2.1: 基本的 sigaction 用法
  {
    struct sigaction sa {};
    sa.sa_sigaction = sigaction_handler; // 使用三参数版本
    sa.sa_flags = SA_SIGINFO;            // 启用扩展信息
    // sa_mask: 在处理器运行期间额外阻塞的信号集合
    sigemptyset(&sa.sa_mask); // 不额外阻塞任何信号

    if (sigaction(SIGUSR1, &sa, nullptr) == -1) {
      cout << "  ❌ sigaction 失败: " << strerror(errno) << "\n";
      return;
    }

    cout << "  已为 SIGUSR1 注册处理器 (sigaction + SA_SIGINFO)\n";
    cout << "  发送 SIGUSR1 给自己...\n";
    raise(SIGUSR1);
    cout << "  处理器被调用了 " << g_sigaction_count << " 次\n";

    // 恢复默认
    struct sigaction old_sa {};
    old_sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &old_sa, nullptr);
  }

  // TODO 2.2: SA_RESTART — 自动重启被中断的系统调用
  {
    cout << "\n  SA_RESTART 标志的作用:\n";

    // 没有 SA_RESTART: 信号处理器返回后，被中断的 read/accept 返回 EINTR
    // 有 SA_RESTART:   信号处理器返回后，被中断的 read/accept 自动重启

    struct sigaction sa {};
    sa.sa_sigaction = sigaction_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART; // ← 关键标志

    if (sigaction(SIGUSR2, &sa, nullptr) == -1) {
      cout << "  ❌ sigaction 失败\n";
      return;
    }

    cout << "    ✅ SIGUSR2 处理器已注册 (SA_RESTART)\n";
    cout << "    收到信号后，阻塞中的 read/accept 将自动重启\n";
    cout << "    ❌ 如果不设 SA_RESTART，需要手动处理 EINTR 错误\n";

    struct sigaction old_sa {};
    old_sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR2, &old_sa, nullptr);
  }

  // TODO 2.3: 在处理器执行期间阻塞其他信号
  {
    cout << "\n  sa_mask: 处理器运行期间阻塞的信号:\n";

    struct sigaction sa {};
    sa.sa_sigaction = sigaction_handler;
    sa.sa_flags = SA_SIGINFO;

    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);  // 处理 SIGUSR1 时，暂时阻塞 SIGINT
    sigaddset(&sa.sa_mask, SIGTERM); // 处理 SIGUSR1 时，暂时阻塞 SIGTERM

    cout << "    处理 SIGUSR1 时，SIGINT 和 SIGTERM 会被暂存\n";
    cout << "    处理器返回后，被阻塞的信号自动解除并递送\n";

    // 清理
    struct sigaction old_sa {};
    old_sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &old_sa, nullptr);
  }

  cout << "\n";
}

// ============================================================
// 练习 3: 信号屏蔽 — 阻塞与解除阻塞
// ============================================================
//
// 每个进程有一个「信号掩码」(signal mask)，指定哪些信号被阻塞。
// 被阻塞的信号不会被递送，而是处于 pending 状态。
// 解除阻塞后，pending 信号立即被递送。

void exercise3_signal_masks() {
  cout << "=== 练习 3: 信号屏蔽 ===\n";

  // TODO 3.1: sigprocmask — 阻塞 / 解除阻塞信号
  // 使用 SIGUSR1 做演示，避免 SIGINT 默认行为杀死进程
  {
    sigset_t block_set, old_set;

    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);  // 阻塞 SIGUSR1

    // 先确认 SIGUSR1 有处理器（复用练习 1 的 handler）
    std::signal(SIGUSR1, simple_handler);

    cout << "  阻塞 SIGUSR1 2 秒...\n";
    if (sigprocmask(SIG_BLOCK, &block_set, &old_set) == -1) {
      cout << "  ❌ sigprocmask 失败\n";
      return;
    }

    // 在这期间，即使收到 SIGUSR1 也不会被递送
    // 它处于 pending 状态
    cout << "  现在发一个 SIGUSR1（被阻塞）...\n";
    raise(SIGUSR1);

    cout << "  g_signal_count 没有变化: " << g_signal_count
         << " (信号在 pending)\n";

    // 解除阻塞 → pending 信号立即递送
    cout << "  解除阻塞 → pending 信号将被立即递送\n";
    sigprocmask(SIG_SETMASK, &old_set, nullptr);
    cout << "  信号已递送，g_signal_count = " << g_signal_count << "\n";

    std::signal(SIGUSR1, SIG_DFL); // 恢复默认
  }

  // TODO 3.2: 检查 pending 信号
  {
    cout << "\n  检查 pending 信号:\n";

    sigset_t pending;
    if (sigpending(&pending) == -1) {
      cout << "  ❌ sigpending 失败\n";
      return;
    }

    cout << "  Pending 信号: ";
    for (int sig = 1; sig < NSIG; ++sig) {
      if (sigismember(&pending, sig)) {
        cout << sig << " (" << strsignal(sig) << ") ";
      }
    }
    cout << "\n";
  }

  // TODO 3.3: 信号集操作 API
  {
    cout << "\n  信号集操作:\n";
    cout << "    sigemptyset(&set) — 清空集合\n";
    cout << "    sigfillset(&set)  — 全部置 1（包含所有信号）\n";
    cout << "    sigaddset(&set, sig) — 添加一个信号\n";
    cout << "    sigdelset(&set, sig) — 删除一个信号\n";
    cout << "    sigismember(&set, sig) — 查询信号是否在集合中\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 4: 发送信号 — kill, raise, killpg
// ============================================================
//
// kill 不只是"杀死进程"，而是"向进程发送信号"。
// 名字是历史原因（早期 Unix 信号主要是终止进程）。

void exercise4_sending_signals() {
  cout << "=== 练习 4: 发送信号 ===\n";

  // TODO 4.1: kill(pid, sig) — 发送信号给指定进程
  {
    cout << "  kill() 不是'杀进程'，而是'发信号':\n";
    cout << "    kill(pid, 0)     — 不发送信号，只检查进程是否存在\n";
    cout << "    kill(pid, SIGTERM) — 礼貌地请进程退出\n";
    cout << "    kill(pid, SIGKILL) — 强制终止\n";
    cout << "    kill(pid, SIGSTOP) — 暂停进程\n";
    cout << "    kill(pid, SIGUSR1) — 自定义信号 1\n";

    // 演示 kill(pid, 0) 检查进程存在
    pid_t my_pid = getpid();
    if (kill(my_pid, 0) == 0) {
      cout << "    ✅ 进程 " << my_pid << " 存在且有权发信号\n";
    }
  }

  // TODO 4.2: pid 参数的特殊值
  {
    cout << "\n  kill() 的 pid 参数:\n";
    cout << "    pid > 0   — 发给指定 PID\n";
    cout << "    pid == 0  — 发给同一进程组的所有进程\n";
    cout << "    pid == -1 — 发给所有有权发送的进程（谨慎！）\n";
    cout << "    pid < -1  — 发给进程组 |pid|\n";
  }

  // TODO 4.3: raise(sig) — 等同于 kill(getpid(), sig)
  {
    cout << "\n  raise(SIGINT) 等价于 kill(getpid(), SIGINT)\n";
    cout << "  raise 是 C 标准库的一部分，kill 是 POSIX\n";
  }

  // TODO 4.4: alarm() — 定时向自己发 SIGALRM
  {
    cout << "\n  alarm(seconds) — 定时器:\n";
    cout << "    调用 alarm(5) → 5 秒后内核向进程发 SIGALRM\n";
    cout << "    alarm(0) → 取消之前的闹钟\n";
    cout << "    默认 SIGALRM 会终止进程，需要注册处理器才有用\n";
    cout << "    更现代的选择: timerfd_create (Week 12 或之后的主题)\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 5: SIGCHLD — 子进程状态变化的异步通知
// ============================================================
//
// 每当子进程停止或终止，父进程都会收到 SIGCHLD。
// 默认 SIGCHLD 被忽略，但我们可以用它来异步收割子进程。
//
// ⚠️ 标准信号（如 SIGCHLD）不排队：
//   如果多个子进程同时退出，可能只触发一次 SIGCHLD。
//   所以在 SIGCHLD 处理器中必须用循环 waitpid 收割所有子进程。

volatile std::sig_atomic_t g_children_reaped = 0;

void sigchld_handler(int /*signum*/) {
  // ⚠️ 关键：用循环收割！因为多个子进程退出可能合并成一个信号
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    g_children_reaped = g_children_reaped + 1;
    // 在生产代码中，你可能想把 pid 和 status 写入一个
    // async-signal-safe 的数据结构（如 self-pipe）
  }
}

void exercise5_sigchld() {
  cout << "=== 练习 5: SIGCHLD ===\n";

  // TODO 5.1: 用 SIGCHLD 异步收割子进程
  {
    // 注册 SIGCHLD 处理器
    struct sigaction sa {};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; // SA_NOCLDSTOP: 只在子进程终止时通知
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, nullptr);

    cout << "  创建 3 个子进程，用 SIGCHLD 异步收割...\n";

    for (int i = 0; i < 3; ++i) {
      pid_t pid = fork();
      if (pid == 0) {
        // 子进程：很快退出
        usleep((3 - i) * 20000); // 不同时间退出
        _exit(i);
      }
    }

    // 父进程继续做事，不阻塞等子进程
    cout << "  父进程继续工作（不等子进程）...\n";
    usleep(150000); // 等所有子进程结束 + SIGCHLD 被处理

    cout << "  通过 SIGCHLD 收割的子进程数: " << g_children_reaped << "\n";
    cout << "  💡 这就是 event-driven 的进程收割方式\n";

    // 恢复 SIGCHLD 默认
    struct sigaction old_sa {};
    old_sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &old_sa, nullptr);
  }

  // TODO 5.2: SIG_IGN 的特殊效果
  {
    cout << "\n  💡 SIGCHLD 的特殊处理:\n";
    cout << "    如果显式设置 signal(SIGCHLD, SIG_IGN):\n";
    cout << "    子进程退出后自动被内核回收，不产生僵尸\n";
    cout << "    这是 POSIX.1-2001 规定的行为\n";
    cout << "    但这样就不知道子进程的退出状态了\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 6: Self-Pipe Trick — 信号处理器与主循环的安全桥梁
// ============================================================
//
// 问题：信号处理器中不能安全地做复杂操作（printf, malloc...）。
// 解决：信号处理器只写入一个 pipe，主循环读 pipe 并处理。
//
// 这是实现 event-loop 中安全处理信号的最经典模式。

class SelfPipe {
  int _pipefd[2]{-1, -1};

public:
  bool init() {
    if (pipe(_pipefd) == -1) return false;
    // 设为非阻塞，防止 pipe 满了导致信号处理器阻塞
    fcntl(_pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(_pipefd[1], F_SETFL, O_NONBLOCK);
    return true;
  }

  int readFd() const { return _pipefd[0]; }

  // 在信号处理器中调用 — 只做 write
  void notify(int signum) {
    // write 是 async-signal-safe
    ssize_t n __attribute__((unused));
    n = write(_pipefd[1], &signum, sizeof(signum));
  }

  // 在主循环中调用 — 可以做任何事
  int consume() {
    int sig;
    if (read(_pipefd[0], &sig, sizeof(sig)) == sizeof(sig)) {
      return sig;
    }
    return -1;
  }

  ~SelfPipe() {
    ::close(_pipefd[0]);
    ::close(_pipefd[1]);
  }
};

// 全局 self-pipe 实例
SelfPipe g_self_pipe;

void self_pipe_handler(int signum) {
  g_self_pipe.notify(signum);
  // 就这一行！所有复杂逻辑延迟到主循环中处理
}

void exercise6_self_pipe() {
  cout << "=== 练习 6: Self-Pipe Trick ===\n";

  if (!g_self_pipe.init()) {
    cout << "  ❌ self-pipe 初始化失败\n";
    return;
  }

  // 注册信号处理器（只写 pipe）
  struct sigaction sa {};
  sa.sa_handler = self_pipe_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, nullptr);
  sigaction(SIGUSR2, &sa, nullptr);

  cout << "  已注册 SIGUSR1/SIGUSR2 → self-pipe 处理器\n";

  // 模拟：主循环 & 几个异步事件
  cout << "  发送 SIGUSR1 和 SIGUSR2...\n";
  raise(SIGUSR1);
  raise(SIGUSR2);

  // 主循环处理
  cout << "  主循环读取 self-pipe:\n";
  for (int i = 0; i < 2; ++i) {
    int sig = g_self_pipe.consume();
    if (sig >= 0) {
      cout << "    📥 收到信号: " << sig << " (" << strsignal(sig)
           << ") — 可以安全地 printf/malloc/加锁了！\n";
    }
  }

  // 恢复
  struct sigaction old_sa {};
  old_sa.sa_handler = SIG_DFL;
  sigaction(SIGUSR1, &old_sa, nullptr);
  sigaction(SIGUSR2, &old_sa, nullptr);
  cout << "\n";
}

// ============================================================
// 练习 7: signalfd — Linux 的现代信号处理方式
// ============================================================
//
// signalfd 把信号变成文件描述符。
// 这样信号就可以和 epoll/select/poll 统一处理了。
// 这是 Linux 特有的，但非常强大。

void exercise7_signalfd() {
  cout << "=== 练习 7: signalfd ===\n";

  // TODO 7.1: 用 signalfd 读取信号
  {
    // 先阻塞要处理的信号（signalfd 从 mask 中"偷走"信号）
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
      cout << "  ❌ sigprocmask 失败\n";
      return;
    }

    // 创建 signalfd
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd == -1) {
      cout << "  ❌ signalfd 失败: " << strerror(errno) << "\n";
      sigprocmask(SIG_UNBLOCK, &mask, nullptr);
      return;
    }

    cout << "  信号现在可以通过 fd " << sfd << " 读取\n";

    // 发送信号
    cout << "  发送 SIGUSR1...\n";
    raise(SIGUSR1);

    // 用 read 读取信号信息
    struct signalfd_siginfo fdsi;
    ssize_t n = read(sfd, &fdsi, sizeof(fdsi));
    if (n == sizeof(fdsi)) {
      cout << "  📥 signalfd 读到: 信号=" << fdsi.ssi_signo
           << " (" << strsignal(fdsi.ssi_signo) << ")"
           << ", 发送者PID=" << fdsi.ssi_pid
           << ", 发送者UID=" << fdsi.ssi_uid << "\n";
    }

    ::close(sfd);
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  }

  // TODO 7.2: signalfd vs self-pipe 对比
  {
    cout << "\n  signalfd vs self-pipe 对比:\n";
    cout << "  ┌──────────┬─────────────────────┬──────────────────────┐\n";
    cout << "  │          │ Self-Pipe           │ signalfd             │\n";
    cout << "  ├──────────┼─────────────────────┼──────────────────────┤\n";
    cout << "  │ 可移植性 │ ✅ 所有 Unix        │ ❌ Linux only        │\n";
    cout << "  │ 复杂度   │ 需要信号处理器+pipe │ 只需 read(fd)       │\n";
    cout << "  │ 信号信息 │ 只有信号编号        │ siginfo_t 完整信息  │\n";
    cout << "  │ 丢失风险 │ pipe 满时可能丢      │ 内核保证不丢        │\n";
    cout << "  │ epoll    │ ✅ (fd 可 epoll)    │ ✅ (fd 可 epoll)    │\n";
    cout << "  └──────────┴─────────────────────┴──────────────────────┘\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 8: 实战 — 实现一个能优雅退出的后台服务框架
// ============================================================
//
// 综合运用：sigaction + SIGTERM/SIGINT + SIGCHLD + self-pipe/atomic

class GracefulService {
  // 注意：信号处理器中只能安全使用 volatile sig_atomic_t
  // std::atomic<bool> 在 Linux 上对 lock-free bool 通常安全但不严格符合标准
  // 我们在此用普通 volatile + signal handler 只操作这个变量
  volatile std::sig_atomic_t _running{1};

  // 辅助：在处理器中写 flag
  static void handle_shutdown(int signum) {
    (void)signum;
    // 信号处理器中只做最小的事情：
    // 但这里无法访问 _running，因为它不是静态的
    // 实际项目中常用全局变量
  }

public:
  void run() {
    cout << "  🏃 服务启动 (PID=" << getpid() << ")\n";

    // 注册优雅退出信号（复用练习 1 的 handler，它只累加计数器）
    struct sigaction sa {};
    sa.sa_handler = simple_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr); // 常用于 reload 配置

    // 自动收割子进程
    struct sigaction sa_chld {};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, nullptr);

    // 启动一个假的工作子进程
    pid_t worker = fork();
    if (worker == 0) {
      sleep(1);
      _exit(0);
    }

    int ticks = 0;
    int prev_count = g_signal_count;
    while (ticks < 3) {
      cout << "  ⏲️  主循环 tick " << ++ticks << "\n";

      // 检测是否收到过信号（通过全局计数器）
      if (g_signal_count > prev_count) {
        cout << "  🔔 检测到信号！（计数器从 " << prev_count
             << " 变为 " << g_signal_count << "）\n";
        break;
      }

      // 模拟 I/O 等待
      sleep(1);
    }

    // 停止
    cout << "  🛑 开始清理...\n";

    // 收割剩余子进程
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      cout << "    收割子进程 " << pid << "\n";
    }

    cout << "  👋 服务优雅退出完成\n";

    // 恢复默认
    struct sigaction old_sa {};
    old_sa.sa_handler = SIG_DFL;
    sigaction(SIGTERM, &old_sa, nullptr);
    sigaction(SIGINT, &old_sa, nullptr);
    sigaction(SIGHUP, &old_sa, nullptr);
    sigaction(SIGCHLD, &old_sa, nullptr);
  }
};

void exercise8_graceful_service() {
  cout << "=== 练习 8: 优雅退出的服务框架 ===\n";

  GracefulService svc;

  // 在子进程中模拟发送 SIGTERM
  pid_t pid = fork();
  if (pid == 0) {
    usleep(500000); // 0.5 秒后发信号给父进程
    kill(getppid(), SIGTERM);
    _exit(0);
  }

  svc.run();

  // 收割发信号的子进程
  int status;
  waitpid(pid, &status, 0);

  cout << "\n";
}

// ============================================================
// 练习 9: EINTR — 系统调用被信号中断
// ============================================================
//
// 当进程在阻塞系统调用（read, write, accept, sleep...）中收到信号，
// 且信号处理器返回后，系统调用可能返回 -1 并设 errno = EINTR。
//
// SA_RESTART 可以自动重启大多数系统调用，但不是全部。

void exercise9_eintr() {
  cout << "=== 练习 9: EINTR — 被中断的系统调用 ===\n";

  // TODO 9.1: 哪些系统调用不会被 SA_RESTART 自动重启？
  {
    cout << "  SA_RESTART 不能自动重启的系统调用:\n";
    cout << "    ❌ poll(), select()\n";
    cout << "    ❌ epoll_wait()\n";
    cout << "    ❌ sleep() — 返回剩余秒数\n";
    cout << "    ❌ nanosleep()\n";
    cout << "    ❌ recv(), send() 等带超时的 socket 调用\n";
    cout << "  → 这些调用必须手动处理 EINTR\n";
  }

  // TODO 9.2: 正确处理 EINTR 的模式
  {
    cout << "\n  处理 EINTR 的正确模式:\n";
    cout << "  ┌────────────────────────────────────────┐\n";
    cout << "  │ while ((n = read(fd, buf, size)) < 0) {│\n";
    cout << "  │   if (errno == EINTR) continue;         │\n";
    cout << "  │   break;  // 真正的错误                 │\n";
    cout << "  │ }                                       │\n";
    cout << "  └────────────────────────────────────────┘\n";
  }

  // TODO 9.3: 阻塞等待任意信号 — sigsuspend
  {
    cout << "\n  sigsuspend — 原子地替换信号掩码并暂停等待信号:\n";
    cout << "    可以用于实现信号驱动的同步点\n";
    cout << "    更简单的替代: pause() — 暂停直到收到任意信号\n";
  }

  cout << "\n";
}

// ============================================================
// 练习 10: 常见信号速查表
// ============================================================

void exercise10_signal_reference() {
  cout << "=== 练习 10: 常用信号速查 ===\n";

  cout << "  ┌──────────────┬──────┬────────────────────────────────┐\n";
  cout << "  │ 信号         │ 编号 │ 默认行为 / 用途                │\n";
  cout << "  ├──────────────┼──────┼────────────────────────────────┤\n";
  cout << "  │ SIGHUP       │    1 │ 终端断开 (常用于 reload 配置) │\n";
  cout << "  │ SIGINT       │    2 │ Ctrl+C，中断前台进程           │\n";
  cout << "  │ SIGQUIT      │    3 │ Ctrl+\\，core dump              │\n";
  cout << "  │ SIGILL       │    4 │ 非法指令                       │\n";
  cout << "  │ SIGABRT      │    6 │ abort() 调用                   │\n";
  cout << "  │ SIGFPE       │    8 │ 算术错误（除零等）             │\n";
  cout << "  │ SIGKILL      │    9 │ 必杀信号，不可捕获             │\n";
  cout << "  │ SIGSEGV      │   11 │ 段错误 (segfault)              │\n";
  cout << "  │ SIGPIPE      │   13 │ 向关闭的 pipe/socket 写        │\n";
  cout << "  │ SIGALRM      │   14 │ alarm() 定时器                 │\n";
  cout << "  │ SIGTERM      │   15 │ 默认的终止信号 (kill 默认)    │\n";
  cout << "  │ SIGCHLD      │   17 │ 子进程状态变化                 │\n";
  cout << "  │ SIGCONT      │   18 │ 继续被暂停的进程               │\n";
  cout << "  │ SIGSTOP      │   19 │ 暂停进程，不可捕获             │\n";
  cout << "  │ SIGTSTP      │   20 │ Ctrl+Z，暂停前台进程           │\n";
  cout << "  │ SIGUSR1      │   10 │ 用户自定义 1                   │\n";
  cout << "  │ SIGUSR2      │   12 │ 用户自定义 2                   │\n";
  cout << "  └──────────────┴──────┴────────────────────────────────┘\n";

  cout << "\n  🎯 日常开发最常用的 5 个信号:\n";
  cout << "    1. SIGINT  — Ctrl+C，用于停止交互式程序\n";
  cout << "    2. SIGTERM — systemd/docker stop 发出的优雅退出信号\n";
  cout << "    3. SIGHUP  — reload 配置文件 (守护进程约定)\n";
  cout << "    4. SIGCHLD — 收割子进程 (event loop 必备)\n";
  cout << "    5. SIGPIPE — 向关闭的连接/管道写 (网络编程必处理)\n";

  cout << "\n";
}

// ============================================================
// main
// ============================================================

int main() {
  // 确保每个输出都立即 flush（避免 fork 导致的缓冲区重复问题）
  std::cout << std::unitbuf;

  cout << "Week 09: 信号 — Signal 处理、发送、阻塞与安全模式\n";
  cout << "==================================================\n\n";

  exercise1_signal_basics();
  exercise2_sigaction();
  exercise3_signal_masks();
  exercise4_sending_signals();
  exercise5_sigchld();
  exercise6_self_pipe();
  exercise7_signalfd();
  exercise8_graceful_service();
  exercise9_eintr();
  exercise10_signal_reference();

  cout << "✅ Week 09 全部练习完成！\n";
  cout << "\n📝 总结要点:\n";
  cout << "  1. 信号是 OS 对进程的异步通知，是软件中断\n";
  cout << "  2. 用 sigaction() 而不用 signal() — 行为更可控\n";
  cout << "  3. 信号处理器中只能调用 async-signal-safe 函数\n";
  cout << "  4. 信号处理器和主循环通信: self-pipe trick 或 signalfd\n";
  cout << "  5. 用 SA_RESTART 自动重启被中断的系统调用\n";
  cout << "  6. 多个子进程退出可能合并为一个 SIGCHLD → 必须循环 waitpid\n";
  cout << "  7. SIGKILL 和 SIGSTOP 不可捕获/忽略 → 管理员最后的武器\n";
  cout << "  8. SIGTERM → 优雅退出, SIGHUP → reload 配置\n";
  cout << "  9. signalfd 可以把信号变成 fd → 统一到 epoll event loop\n";
  cout << "  10. self-pipe 可移植，signalfd 功能更强 (Linux only)\n";

  return 0;
}
