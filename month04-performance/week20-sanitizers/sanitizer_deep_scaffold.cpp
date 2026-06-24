// ============================================================================
// Month 4: 极致性能 — Beyond the Code
// Week 20: Sanitizers & UB Detection — 让 bug 无所遁形
//
// 核心哲学:
//   「Undefined Behavior 是 C++ 最大的敌人 — sanitizer 是你的守护者」
//   「CI 不集成 sanitizer = 裸奔上高速」
//
//   Sanitizer 是编译器内置的运行时检测工具 (Google 开发):
//   - ASan:  每次内存访问都在影子内存中检查合法性
//   - UBSan: 在可疑操作前后插入检查代码
//   - TSan:  记录每次内存访问的 happens-before 关系
//   - MSan:  追踪每个 bit 的初始化状态
//   - LSan:  扫描可达内存找出不可达的分配
//
// 本周目标:
//   - 理解每种 sanitizer 的检测范围和原理
//   - 学会用 sanitizer 编译和解读报告
//   - 将 sanitizer 集成到 CMake/CI 工作流
//
// 编译方式:
//   $ g++ -fsanitize=address -g -O1 scaffold.cpp -o sani_deep
//   $ ./sani_deep <ex_num>    # 运行特定练习
//
//   # 或用 CMake:
//   $ cmake -B build-asan -DSANITIZE=asan && cmake --build build-asan
//   $ ./build-asan/sanitizer_deep <ex_num>
//
// 10 个练习, 每个演示一种 sanitizer 检测的 bug 类型
// ============================================================================

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

// ============================================================================
// 编译指南 (在注释中, 供用户手动编译和测试)
//
// $ g++ -fsanitize=address   -g -O1 sanitizer_deep_scaffold.cpp -o /tmp/test_asan
// $ g++ -fsanitize=undefined -g -O1 sanitizer_deep_scaffold.cpp -o /tmp/test_ubsan
// $ g++ -fsanitize=thread    -g -O1 sanitizer_deep_scaffold.cpp -o /tmp/test_tsan -lpthread
// $ g++ -fsanitize=leak      -g -O1 sanitizer_deep_scaffold.cpp -o /tmp/test_lsan
//    (MSan requires Clang: clang++ -fsanitize=memory ...)
//
// 环境变量:
//   ASAN_OPTIONS=halt_on_error=0:continue_after_error=1  # 不停止, 收集所有错误
//   UBSAN_OPTIONS=print_stacktrace=1                      # 带调用栈
//   TSAN_OPTIONS=history_size=7                            # 更大的竞态历史
// ============================================================================

// ============================================================================
// 工具: 让 sanitizer 友好的 helper
// ============================================================================

static volatile int64_t g_sink = 0;

// 标记内存为不可访问 (ASan 会检测对该区域的访问)
#ifdef __has_feature
  #if __has_feature(address_sanitizer)
    // ASan poison API (仅在编译了 ASan 时声明)
    extern "C" void __asan_poison_memory_region(void const volatile*, size_t);
    extern "C" void __asan_unpoison_memory_region(void const volatile*, size_t);
  #endif
#endif

// ============================================================================
// Ex1: ASan — Heap Buffer Overflow
//
// 概念:
//   ASan 在每次 malloc/new 时分配额外的 "redzone" (不可访问区域),
//   任何溢出到 redzone 的访问立即被检测。
//
//   原理: 影子内存 (Shadow Memory)
//   每 8 字节应用内存 → 1 字节影子内存, 描述 8 字节的可访问性
//   - 0 = 全部 8 字节可访问
//   - 1-7 = 前 N 字节可访问
//   - 负数 = 全部不可访问 (不同值表示不同原因)
//
// 检测类型:
//   - heap-buffer-overflow (读写越界)
//   - stack-buffer-overflow
//   - global-buffer-overflow
//   - heap-use-after-free
//   - stack-use-after-return / stack-use-after-scope
//   - double-free, invalid-free
//   - memory leaks (集成了 LSan)
//
// 任务: 运行以下 buggy 代码, 观察 ASan 报告
// ============================================================================

namespace ex1_heap_overflow {
  void heap_write_overflow() {
    std::cout << "\n--- heap_write_overflow ---\n";
    int* arr = new int[10];
    arr[10] = 42;  // BUG: 越界写入! 索引应该是 [0, 9]
    std::cout << "arr[10] = " << arr[10] << " (WRITE overflow)\n";
    delete[] arr;
  }

  void heap_read_overflow() {
    std::cout << "\n--- heap_read_overflow ---\n";
    int* arr = new int[10];
    for (int i = 0; i <= 10; ++i) {  // BUG: i <= 10 越界
      g_sink += arr[i];  // BUG: 读越界
    }
    std::cout << "Read past end of array\n";
    delete[] arr;
  }

  void heap_underflow() {
    std::cout << "\n--- heap_underflow ---\n";
    int* arr = new int[10];
    int* p = arr - 1;  // BUG: 指向 redzone (分配块之前)
    *p = 99;  // BUG: 写入 redzone → heap-buffer-overflow (underflow)
    std::cout << "Before-array write\n";
    delete[] arr;
  }

  void malloc_overflow() {
    std::cout << "\n--- malloc_overflow ---\n";
    char* buf = (char*)std::malloc(8);
    std::strcpy(buf, "TOO_LONG_STRING");  // BUG: 写入 16 字节到 8 字节分配
    std::cout << "Buffer: " << buf << "\n";
    std::free(buf);
  }

  void run() {
    std::cout << "\n===== Ex1: ASan — Heap Buffer Overflow =====\n";
    std::cout << "Expected: ASan reports 'heap-buffer-overflow' with exact location\n";
    std::cout << "Compile: g++ -fsanitize=address -g -O1 scaffold.cpp -o test\n";
    std::cout << "ASan report includes:\n";
    std::cout << "  - Bug type (heap-buffer-overflow)\n";
    std::cout << "  - Address and which redzone it hit\n";
    std::cout << "  - Where the memory was allocated\n";
    std::cout << "  - Stack trace of both allocation and violation\n\n";

    heap_write_overflow();
    // heap_read_overflow();   // uncomment to test read overflow
    // heap_underflow();       // uncomment to test underflow
    // malloc_overflow();      // uncomment to test malloc overflow
  }
}

// ============================================================================
// Ex2: ASan — Stack & Global Buffer Overflow
//
// 概念:
//   ASan 同样保护栈和全局变量, 在变量两侧插入 "canary" 区域。
//   栈上变量之间也会有 redzone 间隔。
//
// 任务: 触发 stack overflow 和 global overflow
// ============================================================================

namespace ex2_stack_global_overflow {
  // Bug 1: 栈缓冲区溢出
  void stack_overflow(int index) {
    int numbers[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    // BUG: 如果 index >= 8, 溢出到相邻栈变量或返回地址
    std::cout << "numbers[" << index << "] = " << numbers[index] << "\n";
  }

  // Bug 2: 字符串栈溢出
  void string_stack_overflow() {
    char name[16];
    const char* input = "Alexander_the_Great";  // 19 chars + null = 20
    std::strcpy(name, input);  // BUG: 20 bytes into 16 byte buffer!
    std::cout << "Name: " << name << "\n";
  }

  // Bug 3: 全局变量溢出
  int global_array[5] = {10, 20, 30, 40, 50};
  const char* global_string = "short";

  void global_overflow() {
    std::cout << "\n--- global_overflow ---\n";
    // BUG: 写入全局数组越界
    for (int i = 0; i <= 8; ++i) {
      global_array[i] = i * 100;  // i >= 5 → 覆盖相邻全局变量!
    }
    std::cout << "Global array modified (also corrupted global_string?)\n";
    // 检查 global_string 是否被破坏
    std::cout << "global_string = " << (global_string ? global_string : "NULL")
              << " (may be corrupted!)\n";
  }

  void run() {
    std::cout << "\n===== Ex2: ASan — Stack & Global Overflow =====\n";
    std::cout << "ASan adds redzones around stack and global variables.\n\n";

    stack_overflow(8);  // 边界: 刚好越界
    stack_overflow(12); // 深入 redzone

    // string_stack_overflow();  // uncomment: will trigger stack-buffer-overflow
    // global_overflow();        // uncomment: will trigger global-buffer-overflow

    std::cout << "\nASan report format for stack overflow:\n";
    std::cout << "  SUMMARY: AddressSanitizer: stack-buffer-overflow\n";
    std::cout << "  Shadow bytes around the buggy address show redzone pattern\n";
  }
}

// ============================================================================
// Ex3: ASan — Use-After-Free / Use-After-Return / Double-Free
//
// 概念:
//   ASan 不放内存回 allocator, 而是放入 "quarantine zone",
//   标记为不可访问。后续访问立即检测为 use-after-free。
//
//   Use-after-return: ASan 在栈帧返回后将栈内存移入 fake stack
//   (需 ASAN_OPTIONS=detect_stack_use_after_return=1)
//
// 任务: 触发各种 use-after 模式
// ============================================================================

namespace ex3_use_after {
  // Bug 1: use-after-free (堆)
  void use_after_free() {
    std::cout << "\n--- use_after_free ---\n";
    int* p = new int(42);
    delete p;
    // p 现在指向 quarantine zone
    *p = 100;  // BUG: heap-use-after-free (WRITE)
    std::cout << "After free: " << *p << "\n";  // BUG: heap-use-after-free (READ)
  }

  // Bug 2: use-after-free with vector (更隐蔽)
  void use_after_free_vector() {
    std::vector<int> vec = {1, 2, 3};
    int* ptr = &vec[0];
    // push_back 可能导致 reallocation → ptr 悬空!
    for (int i = 0; i < 100; ++i) {
      vec.push_back(i);
    }
    // ptr now dangling
    std::cout << "Dangling ptr: " << *ptr << " (may be garbage)\n";  // BUG
  }

  // Bug 3: double-free
  void double_free() {
    int* p = new int(99);
    delete p;
    // delete p;  // BUG: double-free (uncomment to trigger)
    // ASan: ERROR: AddressSanitizer: attempting double-free
  }

  // Bug 4: use-after-return (需 ASAN_OPTIONS=detect_stack_use_after_return=1)
  int* return_stack_ptr() {
    int local = 42;
    return &local;  // BUG: never do this
  }

  void use_after_return() {
    int* p = return_stack_ptr();
    // 栈帧已销毁, p 悬空
    std::cout << "Stack value after return: " << *p << "\n";  // BUG
  }

  void run() {
    std::cout << "\n===== Ex3: ASan — Use-After-Free / Double-Free =====\n";
    std::cout << "ASan keeps freed memory in quarantine to catch reuse.\n\n";

    use_after_free();

    std::cout << "\nVector reallocation dangling pointer:\n";
    use_after_free_vector();

    double_free();  // double free is commented out — uncomment to test

    std::cout << "\nUse-after-return (needs env var):\n";
    std::cout << "ASAN_OPTIONS=detect_stack_use_after_return=1 ./test\n";
    use_after_return();  // may or may not be caught (need ASAN_OPTIONS)

    std::cout << "\nASan use-after-free report includes:\n";
    std::cout << "  - Where the memory was freed (the delete call)\n";
    std::cout << "  - Where the memory was allocated (the new call)\n";
    std::cout << "  - Where the use occurred\n";
    std::cout << "  → Complete lifecycle of the dangling pointer!\n";
  }
}

// ============================================================================
// Ex4: UBSan — Integer Overflow & Division by Zero
//
// 概念:
//   UBSan 检测 Undefined Behavior (UB) — C++ 标准说「未定义」
//   但编译器恰好做了某事的情况。
//
//   最危险的 UB: 测试通过, 生产崩溃, 因为编译器换了/优化级别变了。
//
//   UBSan 检测:
//   - 有符号整数溢出 (signed-integer-overflow)
//   - 除零 (division-by-zero)
//   - 移位超出范围 (shift)
//   - 空指针解引用 (null)
//   - 非法类型转换 (invalid-cast)
//   - 对齐违规 (misaligned-access)
//   - 越界 (bounds)
//   - 不可达代码 (unreachable)
//   - 等等...
//
// 编译: -fsanitize=undefined (或 -fsanitize=integer,alignment,bounds,...)
//
// 任务: 触发各类整数 UB
// ============================================================================

namespace ex4_ubsan_integer {
  void signed_overflow() {
    std::cout << "\n--- signed_overflow ---\n";
    int big = 2147483640;  // near INT_MAX (2,147,483,647)
    big += 100;  // BUG: 有符号整数溢出 → UB!
    std::cout << "INT_MAX + 100 = " << big
              << " (UB! could wrap to negative)\n";
  }

  void multiplication_overflow() {
    std::cout << "\n--- multiplication_overflow ---\n";
    int a = 100000;
    int b = 100000;
    int c = a * b;  // BUG: a*b = 10,000,000,000 > INT_MAX → overflow
    std::cout << "100000 * 100000 = " << c << " (UB! Should be 10B)\n";

    // 正确做法: use int64_t or check before multiply
    int64_t safe = (int64_t)a * b;
    std::cout << "Safe (int64_t): " << safe << "\n";
  }

  void division_by_zero() {
    std::cout << "\n--- division_by_zero ---\n";
    int zero = 0;
    // int crash = 100 / zero;  // BUG: division by zero → SIGFPE
    std::cout << "100 / 0 = (未执行, 会触发 SIGFPE)\n";
    // UBSan 会报告: runtime error: division by zero
  }

  void shift_overflow() {
    std::cout << "\n--- shift_overflow ---\n";
    int x = 1;
    int y = x << 31;  // BUG: 1 << 31 在有符号 int 中溢出 (INT_MIN?)
    // 正确: 1u << 31 (unsigned)
    std::cout << "1 << 31 (signed) = " << y << " (UB!)\n";

    unsigned int safe = 1u << 31;
    std::cout << "1u << 31 (unsigned) = " << safe << " (OK)\n";
  }

  void negation_overflow() {
    std::cout << "\n--- negation_overflow ---\n";
    int min_int = -2147483647 - 1;  // INT_MIN
    // int neg = -min_int;  // BUG: -INT_MIN = INT_MAX+1 → overflow
    // UBSan: negation of -2147483648 cannot be represented in type 'int'
    std::cout << "-INT_MIN = (cannot represent, UB)\n";
  }

  void run() {
    std::cout << "\n===== Ex4: UBSan — Integer UB =====\n";
    std::cout << "Compile: g++ -fsanitize=undefined -g scaffold.cpp -o test\n";
    std::cout << "UBSan reports: 'runtime error: signed integer overflow'\n\n";

    signed_overflow();
    multiplication_overflow();
    division_by_zero();
    shift_overflow();
    negation_overflow();

    std::cout << "\nUBSan options:\n";
    std::cout << "  -fsanitize=integer  # only integer checks\n";
    std::cout << "  -fno-sanitize-recover=undefined  # abort on first error\n";
    std::cout << "  UBSAN_OPTIONS=print_stacktrace=1  # with backtrace\n";
  }
}

// ============================================================================
// Ex5: UBSan — Null Pointer, Alignment, Invalid Cast
//
// 概念:
//   UBSan 还检测:
//   - null: 空指针解引用
//   - alignment: 对齐违规 (如 int* 指向奇数地址)
//   - object-size: 越界访问 (结合 -fsanitize=bounds)
//   - vptr: 虚函数表损坏
//   - function: 函数指针类型不匹配调用
//   - float-divide-by-zero: 浮点除零
//   - float-cast-overflow: 浮点转换溢出
//
// 任务: 触发这些 UB 类型
// ============================================================================

namespace ex5_ubsan_pointer_cast {
  void null_pointer_deref() {
    std::cout << "\n--- null_pointer_deref ---\n";
    int* p = nullptr;
    // *p = 42;  // BUG: null pointer dereference → SIGSEGV
    // UBSan + ASan together: both will report
    std::cout << "Null deref would crash (or UBSan reports 'null pointer')\n";
    // 安全版本: if (p) *p = 42;
  }

  void misaligned_access() {
    std::cout << "\n--- misaligned_access ---\n";
    char buffer[16];
    // 故意从奇数地址读取 int (需要 4 字节对齐)
    int* misaligned = reinterpret_cast<int*>(buffer + 1);
    // *misaligned = 42;  // BUG: misaligned access (x86 允许但慢, ARM 崩溃)
    std::cout << "Misaligned int* = " << (void*)misaligned
              << " (align=" << (uintptr_t(misaligned) % 4) << ", expect 0)\n";
    std::cout << "UBSan: 'runtime error: store to misaligned address'\n";
  }

  // invalid cast: base-to-derived without virtual inheritance checking
  struct Base {
    int x = 1;
    virtual ~Base() = default;
  };
  struct Derived : Base {
    int y = 2;
  };

  void invalid_downcast() {
    std::cout << "\n--- invalid_downcast ---\n";
    Base* b = new Base();       // 不是 Derived!
    // Derived* d = static_cast<Derived*>(b);  // 编译通过但是 UB!
    // 正确: 用 dynamic_cast (需要 RTTI)
    Derived* d = dynamic_cast<Derived*>(b);
    std::cout << "dynamic_cast result: " << (d ? "Derived" : "nullptr (correct)")
              << "\n";
    delete b;
  }

  void bool_cast_overflow() {
    std::cout << "\n--- bool_cast_overflow ---\n";
    int val = 256;
    bool b = static_cast<bool>(val);  // OK: 256 → true
    // But:
    // bool b = *reinterpret_cast<bool*>(&val);  // BUG: val 的 byte 是 0 → false
    std::cout << "256 → bool = " << b << "\n";
    std::cout << "UBSan catches: 'load of value X which is not a valid bool'\n";
  }

  void run() {
    std::cout << "\n===== Ex5: UBSan — Pointer / Alignment / Cast =====\n";

    null_pointer_deref();
    misaligned_access();
    invalid_downcast();
    bool_cast_overflow();

    std::cout << "\nFull UBSan list: g++ -fsanitize=undefined -fno-sanitize-recover=undefined\n";
    std::cout << "Or select specific: -fsanitize=alignment,null,bounds,enum\n";
  }
}

// ============================================================================
// Ex6: UBSan — Enum, Bounds, Vptr, Unreachable
//
// 任务: 触发更多 UBSan 检测类型
// ============================================================================

namespace ex6_ubsan_more {
  enum Color { RED = 0, GREEN = 1, BLUE = 2 };

  void enum_range_violation() {
    std::cout << "\n--- enum_range_violation ---\n";
    // 创建「非法」的 Color 值
    int raw = 5;
    Color c = static_cast<Color>(raw);  // BUG: 5 不在 [0,2] 范围内
    switch (c) {
      case RED:   std::cout << "RED\n"; break;
      case GREEN: std::cout << "GREEN\n"; break;
      case BLUE:  std::cout << "BLUE\n"; break;
      default:    std::cout << "UNKNOWN (5!)\n"; break;
    }
    // UBSan: 'runtime error: load of value 5 which is not a valid value for Color'
  }

  void array_bounds_violation() {
    std::cout << "\n--- array_bounds_violation ---\n";
    int arr[3] = {10, 20, 30};
    // -fsanitize=bounds 会捕获:
    int idx = 3;
    std::cout << "arr[3] = " << arr[idx] << " (UB!)\n";  // BUG: 越界
    // UBSan: 'runtime error: index 3 out of bounds for type 'int [3]''
  }

  void unreachable_code() {
    std::cout << "\n--- unreachable_code ---\n";
    int x = 5;
    if (x == 5) {
      std::cout << "Normal path\n";
      return;
    }
    // __builtin_unreachable();  // 告诉编译器这不会执行
    // 如果实际执行到这里 → UBSan: 'reached unreachable code'
  }

  void vptr_corruption() {
    std::cout << "\n--- vptr_corruption ---\n";
    struct WithVtable {
      int data = 42;
      virtual void foo() { std::cout << "foo: " << data << "\n"; }
    };

    WithVtable* obj = new WithVtable();
    // 模拟 vtable 损坏 (memset 整个对象, 覆盖 vptr)
    std::memset(obj, 0, sizeof(WithVtable));
    // obj->foo();  // BUG: vptr is 0 → crash
    std::cout << "Vptr corrupted to 0 (calling foo would crash)\n";
    delete obj;  // 也可能崩溃 (virtual destructor 查找 vtable)
    std::cout << "UBSan (-fsanitize=vptr): 'vptr is invalid'\n";
  }

  void run() {
    std::cout << "\n===== Ex6: UBSan — Enum / Bounds / Vptr =====\n";

    enum_range_violation();
    array_bounds_violation();
    unreachable_code();
    vptr_corruption();
  }
}

// ============================================================================
// Ex7: TSan — ThreadSanitizer 数据竞争
//
// 概念:
//   TSan 检测数据竞争 (data race): 两个线程同时访问同一内存,
//   至少一个是写操作, 且没有 happens-before 关系 (锁/原子).
//
//   TSan 原理: 记录每次内存访问, 构建 happens-before 图,
//   在检测到无 happens-before 关系的冲突访问时报告。
//
//   开销: 5-15x 慢, 5-10x 内存 — 仅在测试时使用
//
// 编译: -fsanitize=thread (TSan 需要所有代码都编译了 TSan,
//        包括依赖库, 否则可能误报)
//
// 任务: 触发典型的 data race 模式
// ============================================================================

namespace ex7_tsan_data_race {
  // Race 1: plain write without lock
  int g_counter = 0;  // BUG: not atomic, no mutex

  void race_plain_write() {
    std::cout << "\n--- race_plain_write ---\n";
    g_counter = 0;
    std::thread t1([] {
      for (int i = 0; i < 100000; ++i) g_counter++;
    });
    std::thread t2([] {
      for (int i = 0; i < 100000; ++i) g_counter++;
    });
    t1.join();
    t2.join();
    std::cout << "Final counter: " << g_counter
              << " (expected 200000, race may lose updates)\n";
    // TSan: 'WARNING: ThreadSanitizer: data race on g_counter'
  }

  // Race 2: unprotected struct field
  struct Account {
    int64_t balance = 0;
    // BUG: no mutex, no atomic
  };

  void race_struct_field() {
    std::cout << "\n--- race_struct_field ---\n";
    Account acc;
    // Thread 1: reads balance
    std::thread reader([&acc] {
      for (int i = 0; i < 100000; ++i) {
        g_sink = acc.balance;  // BUG: read without lock
      }
    });
    // Thread 2: writes balance
    std::thread writer([&acc] {
      for (int i = 0; i < 100000; ++i) {
        acc.balance += 100;  // BUG: write without lock
      }
    });
    reader.join();
    writer.join();
    std::cout << "Final balance: " << acc.balance << "\n";
  }

  // Race 3: vector push_back from multiple threads
  void race_vector() {
    std::cout << "\n--- race_vector ---\n";
    std::vector<int> vec;
    // BUG: push_back without mutex
    std::thread t1([&] {
      for (int i = 0; i < 5000; ++i) vec.push_back(i);
    });
    std::thread t2([&] {
      for (int i = 0; i < 5000; ++i) vec.push_back(i);
    });
    t1.join();
    t2.join();
    std::cout << "Vector size: " << vec.size()
              << " (expected 10000, may be less or crash)\n";
  }

  // Race 4: 不正确的 double-checked locking
  class DoubleCheckBug {
    int* _data = nullptr;
    std::mutex _m;
  public:
    int* get_data() {
      if (!_data) {  // BUG: 读 _data 没有锁!
        std::lock_guard lk(_m);
        if (!_data) {
          _data = new int(42);
        }
      }
      return _data;
    }
    // 正确: _data 应该是 std::atomic<int*>
  };

  void run() {
    std::cout << "\n===== Ex7: TSan — Data Race Detection =====\n";
    std::cout << "Compile: g++ -fsanitize=thread -g scaffold.cpp -o test -lpthread\n";
    std::cout << "TSan reports: type of race, threads involved, stack traces\n\n";

    race_plain_write();
    // race_struct_field();  // uncomment to test
    // race_vector();        // uncomment to test

    std::cout << "\nTSan report format:\n";
    std::cout << "  Previous write of size 4 at 0x... by thread T1:\n";
    std::cout << "    #0 increment() buggy.cpp:42\n";
    std::cout << "  Write of size 4 at 0x... by thread T2:\n";
    std::cout << "    #0 increment() buggy.cpp:42\n";
    std::cout << "  → 两个 write 之间没有 happens-before 关系!\n";
    std::cout << "\nFix: std::atomic<int> or std::mutex\n";
  }
}

// ============================================================================
// Ex8: MSan — MemorySanitizer 未初始化内存
//
// 概念:
//   MSan 追踪每个 bit 是否被初始化过。读取未初始化内存 → 报告。
//   (仅 Clang 支持完整 MSan; GCC 支持有限)
//
//   原理: 每个 bit 的应用内存 → 1 bit 的影子内存 (初始化为 0 = 未初始化)
//   - 0 = 未初始化
//   - 1 = 已初始化
//
// 任务: 触发未初始化内存读取
// ============================================================================

namespace ex8_msan_uninit {
  void uninitialized_stack() {
    std::cout << "\n--- uninitialized_stack ---\n";
    int x;  // 未初始化
    if (x == 0) {  // BUG: 读取未初始化变量 (UB!)
      std::cout << "x happened to be 0 (or stack pattern)\n";
    } else {
      std::cout << "x = " << x << " (garbage from stack)\n";
    }
    // MSan: 'Use of uninitialized value x'
  }

  void uninitialized_heap() {
    std::cout << "\n--- uninitialized_heap ---\n";
    int* p = new int;  // 未初始化 (用 new int() 会初始化为 0)
    g_sink = *p;  // BUG: 读未初始化堆内存
    std::cout << "Heap value: " << *p << " (undefined!)\n";
    delete p;
  }

  void partially_initialized_struct() {
    std::cout << "\n--- partially_initialized_struct ---\n";
    struct Point {
      int x, y, z;
    };
    Point p;
    p.x = 10;
    p.y = 20;
    // p.z 未初始化!
    if (p.z > 0) {  // BUG: 读未初始化成员
      std::cout << "z is positive (but it's garbage!)\n";
    }
    std::cout << "Point: (" << p.x << ", " << p.y << ", " << p.z << ")\n";
  }

  void uninitialized_array() {
    std::cout << "\n--- uninitialized_array ---\n";
    int arr[100];
    // 部分初始化
    for (int i = 0; i < 50; ++i) arr[i] = i;
    // arr[50..99] 未初始化
    int sum = 0;
    for (int i = 0; i < 100; ++i) sum += arr[i];  // BUG: 读未初始化
    std::cout << "Sum (half initialized): " << sum << "\n";
  }

  void run() {
    std::cout << "\n===== Ex8: MSan — Uninitialized Memory =====\n";
    std::cout << "Compile: clang++ -fsanitize=memory -g scaffold.cpp -o test\n";
    std::cout << "MSan needs ALL linked libraries built with MSan too.\n\n";

    uninitialized_stack();
    uninitialized_heap();
    partially_initialized_struct();
    uninitialized_array();

    std::cout << "\nMSan report format:\n";
    std::cout << "  'Use of uninitialized value of size 4'\n";
    std::cout << "  'Uninitialized value was created by an allocation'\n";
    std::cout << "  With -fsanitize-memory-track-origins: shows WHERE uninit happened\n";
    std::cout << "\nWithout MSan (GCC), use Valgrind --track-origins=yes\n";
  }
}

// ============================================================================
// Ex9: LSan — LeakSanitizer 内存泄漏
//
// 概念:
//   LSan 在程序退出时扫描所有可达内存, 报告不可达的分配。
//   集成在 ASan 中 (ASan 默认在退出时运行 LSan)。
//   也可以单独使用 (-fsanitize=leak)。
//
//   检测类型:
//   - 直接泄漏: 直接分配的内存没有指针指向它
//   - 间接泄漏: 指针链中某个中间节点泄漏, 导致子节点不可达
//
// 任务: 触发不同类型的内存泄漏
// ============================================================================

namespace ex9_lsan_leak {
  // 泄漏 1: 直接泄漏
  void direct_leak() {
    std::cout << "\n--- direct_leak ---\n";
    int* leaked = new int(100);  // BUG: never deleted
    g_sink += *leaked;
    std::cout << "Allocated " << *leaked << " but forgot to delete\n";
    // LSan: 'Direct leak of 4 byte(s) in 1 object(s) allocated from...'
  }

  // 泄漏 2: 循环引用 (shared_ptr)
  struct Node {
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;  // 如果不用 weak_ptr → 循环引用 → 泄漏
    ~Node() { std::cout << "Node destroyed\n"; }
  };

  void cycle_leak() {
    std::cout << "\n--- cycle_leak ---\n";
    // 正确: 用 weak_ptr 打破循环
    auto n1 = std::make_shared<Node>();
    auto n2 = std::make_shared<Node>();
    n1->next = n2;
    n2->prev = n1;  // weak_ptr → 不会阻止析构
    std::cout << "Nodes created with correct weak_ptr (no leak)\n";
    // 如果 next 和 prev 都用 shared_ptr → 循环引用 → 泄漏
  }

  // 泄漏 3: 异常路径泄漏
  void exception_leak() {
    std::cout << "\n--- exception_leak ---\n";
    try {
      int* data = new int[1000];  // 分配资源
      // 模拟: 下面抛异常
      throw std::runtime_error("Something went wrong!");
      // delete[] data;  // BUG: 永远不会执行到这里!
    } catch (const std::exception& e) {
      std::cout << "Caught: " << e.what()
                << " — but leaked 1000 ints!\n";
      // 正确: 用 std::unique_ptr<int[]>
    }
  }

  // 泄漏 4: realloc 泄漏
  void realloc_leak() {
    std::cout << "\n--- realloc_leak ---\n";
    char* buf = (char*)std::malloc(1024);
    // 如果不 free 原始的 buf...
    buf = (char*)std::malloc(2048);  // BUG: 原 1024 泄漏!
    std::strcpy(buf, "new allocation overwrote old pointer");
    std::free(buf);  // 只释放了 2048, 1024 泄漏了
    std::cout << "Only freed the second allocation, first one leaked\n";
  }

  void run() {
    std::cout << "\n===== Ex9: LSan — Memory Leaks =====\n";
    std::cout << "Compile: g++ -fsanitize=address -g scaffold.cpp -o test\n";
    std::cout << "(LSan runs automatically at exit when using ASan)\n\n";

    direct_leak();
    cycle_leak();
    exception_leak();
    realloc_leak();

    std::cout << "\nAt exit, LSan will report:\n";
    std::cout << "  Direct leak of ... byte(s) in N object(s) allocated from:\n";
    std::cout << "  #0 operator new (malloc) ...\n";
    std::cout << "  #1 direct_leak() scaffold.cpp:NNN\n";
    std::cout << "\nSuppress leaks: export LSAN_OPTIONS=suppressions=leaks.txt\n";
  }
}

// ============================================================================
// Ex10: 综合实战 — 用所有 Sanitizer 查找 bug
//
// 场景:
//   一个「简易 JSON Parser」包含多种 bug:
//     - Buffer overflow (解析超长字符串)
//     - Use-after-free (返回临时对象的引用)
//     - 整数溢出 (数组索引计算)
//     - 数据竞争 (缓存未加锁)
//     - 内存泄漏 (异常路径)
//
// 任务:
//   1. 读代码, 尝试用肉眼找到 bug
//   2. 分别用 ASan / UBSan / TSan 编译和运行
//   3. 对比三种 sanitizer 的报告, 看看各有何发现
//   4. 修复所有 bug
// ============================================================================

namespace ex10_json_parser_bugs {
  class SimpleJsonParser {
    // 简单的 JSON 字符串解析器 (有很多 bug)

    struct JsonValue {
      enum Type { STRING, NUMBER, OBJECT } type;
      std::string str_val;
      double num_val = 0;
      // BUG 1: 没有管理 children 的生命周期
    };

    // BUG 2: 缓存没有线程安全 (TSan 会报告)
    mutable std::unordered_map<std::string, double> _parse_cache;
    // 注: mutable 允许在 const 方法中修改, 但未加锁

    // BUG 3: 固定大小缓冲区
    char _buffer[128];
    int _buf_pos = 0;

    // BUG 4: 越界读取
    char peek(const char* input, int& pos) const {
      return input[pos];  // BUG: 不检查 pos 是否超出 input 长度
    }

    // BUG 5: pos 可能溢出
    void skip_whitespace(const char* input, int& pos) const {
      while (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\n')
        ++pos;  // BUG: 如果 input 全是空白, pos 一直增加
    }

  public:
    // BUG 6: 返回局部变量引用 (use-after-return)
    const std::string& parse_string_bug(const char* input, int& pos) {
      std::string result;  // 局部变量!
      if (input[pos] == '"') {
        ++pos;
        while (input[pos] != '"' && input[pos] != '\0') {
          result += input[pos];
          ++pos;
        }
        ++pos;  // skip closing quote
      }
      return result;  // BUG: 返回局部引用!
    }

    // 修复版本
    std::string parse_string_fixed(const char* input, int& pos) {
      std::string result;
      if (input[pos] == '"') {
        ++pos;
        while (input[pos] != '"' && input[pos] != '\0') {
          result += input[pos];
          ++pos;
        }
        ++pos;
      }
      return result;  // OK: 返回值拷贝
    }

    // BUG 7: 缓冲区溢出
    void append_to_buffer(char ch) {
      _buffer[_buf_pos++] = ch;  // BUG: 不检查 _buf_pos >= 128
    }

    // BUG 8: 竞态条件 — parse_number 修改 _parse_cache
    double parse_number(const char* input, int& pos) const {
      // 先查缓存 (read without lock)
      std::string key(input + pos);
      auto it = _parse_cache.find(key);
      if (it != _parse_cache.end()) {
        return it->second;  // BUG: 读缓存无锁
      }

      // 解析数字
      double value = 0.0;
      while (input[pos] >= '0' && input[pos] <= '9') {
        value = value * 10 + (input[pos] - '0');
        ++pos;
      }
      if (input[pos] == '.') {
        ++pos;
        double frac = 0.1;
        while (input[pos] >= '0' && input[pos] <= '9') {
          value += (input[pos] - '0') * frac;
          frac *= 0.1;
          ++pos;
        }
      }

      // 写缓存 (write without lock)
      _parse_cache[key] = value;  // BUG: 写缓存无锁 (TSan!)
      return value;
    }

    // BUG 9: 未初始化成员 — JsonValue 的 num_val 在 STRING 类型时未初始化
    JsonValue parse_value_buggy(const char* input, int pos) {
      JsonValue val;
      if (input[pos] == '"') {
        val.type = JsonValue::STRING;
        val.str_val = parse_string_fixed(input, pos);
        // BUG: num_val 未初始化, 如果是 MSan 会报
      } else {
        val.type = JsonValue::NUMBER;
        val.num_val = parse_number(input, pos);
      }
      return val;
    }

    void run_tests() {
      std::cout << "\n--- Parsing tests ---\n";

      const char* test1 = "\"hello\"";
      int pos = 0;
      // BUG: 调用有 use-after-return 的版本
      // const std::string& ref = parse_string_bug(test1, pos);
      // std::cout << "Parsed: " << ref << "\n";  // UB: dangling reference

      // 正确版本
      std::string val = parse_string_fixed(test1, pos);
      std::cout << "Parsed string: " << val << "\n";

      // 测试数字解析 (带缓存竞态)
      const char* test2 = "123.456";
      pos = 0;
      double d1 = parse_number(test2, pos);
      std::cout << "Parsed number: " << d1 << "\n";

      // 多线程下调用 parse_number → TSan 检测竞态
      std::thread t1([this] {
        for (int i = 0; i < 100; ++i) {
          int p = 0;
          parse_number("789.012", p);
        }
      });
      std::thread t2([this] {
        for (int i = 0; i < 100; ++i) {
          int p = 0;
          parse_number("345.678", p);
        }
      });
      t1.join();
      t2.join();
      std::cout << "Multi-threaded parsing done (cache race may be detected by TSan)\n";

      // BUG: 缓冲区溢出
      for (int i = 0; i < 200; ++i) {
        append_to_buffer('X');  // BUG: 超过 128 字节
      }
      std::cout << "Buffer filled (overflowed? ASan will tell)\n";
    }
  };

  void run() {
    std::cout << "\n===== Ex10: JSON Parser — Find All Bugs! =====\n";
    std::cout << "This parser has ~9 bugs of different types.\n";
    std::cout << "Try compiling with each sanitizer:\n";
    std::cout << "  g++ -fsanitize=address   scaffold.cpp → ASan  finds overflow + UAF\n";
    std::cout << "  g++ -fsanitize=undefined scaffold.cpp → UBSan finds uninitialized\n";
    std::cout << "  g++ -fsanitize=thread    scaffold.cpp → TSan  finds race on cache\n";
    std::cout << "  g++ -fsanitize=leak      scaffold.cpp → LSan  finds leaks\n\n";

    SimpleJsonParser parser;
    parser.run_tests();

    std::cout << "\n─── Bug Report Card ───\n";
    std::cout << "Bug 1: Uninitialized num_val in JsonValue        → MSan / UBSan\n";
    std::cout << "Bug 2: _parse_cache race (mutable without mutex)  → TSan\n";
    std::cout << "Bug 3: _buffer overflow (append_to_buffer)        → ASan\n";
    std::cout << "Bug 4: peek() without bounds check                → ASan\n";
    std::cout << "Bug 5: skip_whitespace() infinite loop            → runtime\n";
    std::cout << "Bug 6: Return local reference (parse_string_bug)  → ASan stack-use-after-return\n";
    std::cout << "Bug 7: Buffer overflow in append_to_buffer (200>128) → ASan\n";
    std::cout << "Bug 8: parse_number() race on _parse_cache       → TSan\n";
    std::cout << "Bug 9: Memory leak (JsonValue children)            → LSan\n";
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  int ex_num = 99;
  if (argc > 1) {
    ex_num = std::atoi(argv[1]);
    if (ex_num == 0) ex_num = 99;
  }

  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Month 4 / Week 20: Sanitizers & UB Detection\n";
  std::cout << "══════════════════════════════════════════════\n";

  // 检查编译时是否启用了 sanitizer
  #if defined(__SANITIZE_ADDRESS__)
    std::cout << "  [ASan ENABLED]\n";
  #endif
  #if defined(__SANITIZE_THREAD__)
    std::cout << "  [TSan ENABLED]\n";
  #endif
  #if defined(__SANITIZE_UNDEFINED__)
    std::cout << "  [UBSan not detectable via macro]\n";
  #endif
  #if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    std::cout << "  [No sanitizer detected — bugs will still run but won't be reported]\n";
    std::cout << "  Compile with: -fsanitize=address|undefined|thread|leak\n";
  #endif

  std::cout << "Running exercise " << (ex_num == 99 ? "LAST (10)" : std::to_string(ex_num)) << "\n\n";

  switch (ex_num) {
    case 1:  ex1_heap_overflow::run(); break;
    case 2:  ex2_stack_global_overflow::run(); break;
    case 3:  ex3_use_after::run(); break;
    case 4:  ex4_ubsan_integer::run(); break;
    case 5:  ex5_ubsan_pointer_cast::run(); break;
    case 6:  ex6_ubsan_more::run(); break;
    case 7:  ex7_tsan_data_race::run(); break;
    case 8:  ex8_msan_uninit::run(); break;
    case 9:  ex9_lsan_leak::run(); break;
    case 10: ex10_json_parser_bugs::run(); break;
    default:
      ex1_heap_overflow::run();
      ex2_stack_global_overflow::run();
      ex3_use_after::run();
      ex4_ubsan_integer::run();
      ex5_ubsan_pointer_cast::run();
      ex6_ubsan_more::run();
      ex7_tsan_data_race::run();
      ex8_msan_uninit::run();
      ex9_lsan_leak::run();
      ex10_json_parser_bugs::run();
      break;
  }

  std::cout << "\n Week 20 Done! 🎉\n";
  std::cout << "Next: Try compiling with each sanitizer:\n";
  std::cout << "  g++ -fsanitize=address   -g scaffold.cpp && ./a.out 1\n";
  std::cout << "  g++ -fsanitize=undefined -g scaffold.cpp && ./a.out 4\n";
  std::cout << "  g++ -fsanitize=thread    -g scaffold.cpp && ./a.out 7\n";
  return 0;
}
