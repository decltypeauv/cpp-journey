// Day 7: 异常安全 — RAII 存在的理由
// 编译: cmake -B build && cmake --build build
// 运行: ./build/exceptions

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using std::cout;
using std::make_unique;
using std::string;
using std::unique_ptr;

// ============================================================
// 练习 1: 异常基础 — try/catch/throw 是怎么工作的
// ============================================================
void exercise1_basics() {
  std::cout << "=== 练习 1: 异常基础 ===\n";

  // TODO 1.1: 最基础的 try-catch
  // throw 抛出异常 → 栈展开 (stack unwinding) → 找最近的匹配 catch
  try {
    throw std::runtime_error("something went wrong");
  } catch (const std::runtime_error &e) {
    std::cout << "  caught runtime_error: " << e.what() << "\n";
  } catch (const std::exception &e) {
    std::cout << "  caught exception: " << e.what() << "\n";
  } catch (...) {
    std::cout << "  caught unknown exception\n";
  }
  std::cout << "  异常处理完毕，程序继续运行\n";

  // TODO 1.2: 异常继承体系 — 为什么用引用捕获
  // 标准异常体系:
  //   std::exception
  //   ├── std::logic_error       (程序逻辑错误)
  //   │   ├── std::invalid_argument
  //   │   └── std::out_of_range
  //   └── std::runtime_error     (运行时错误)
  //       ├── std::system_error
  //       └── std::overflow_error

  // 关键规则: 用 const & 捕获！
  // ✅ catch (const std::exception& e)  — 多态，不拷贝
  // ❌ catch (std::exception e)        — 切片！派生类信息丢失
  // ❌ catch (const char* msg)         — 只匹配 throw "string"，不匹配 exception

  try {
    throw std::invalid_argument("bad argument");
  } catch (const std::logic_error &e) {
    std::cout << "  caught by base class: " << e.what() << "\n";
    // 引用捕获保留多态 — what() 调用的是 invalid_argument 的版本
  }

  // TODO 1.3: 栈展开 — 从 throw 点到 catch 点，中间所有局部对象被析构
  // 这就是 RAII 能保证资源释放的原因！
  struct Tracer {
    string _name;
    Tracer(string name) : _name(std::move(name)) {
      std::cout << "    构造: " << _name << "\n";
    }
    ~Tracer() { std::cout << "    析构: " << _name << "\n"; }
  };

  try {
    Tracer a("A");
    {
      Tracer b("B");
      Tracer c("C");
      std::cout << "  即将 throw...\n";
      throw std::runtime_error("boom!");
    }  // b, c 会在这里被析构吗？
  } catch (const std::exception &e) {
    std::cout << "  caught: " << e.what() << "\n";
  }
  // 输出顺序: 构造A → 构造B → 构造C → throw
  //           → 析构C → 析构B → 析构A → caught
  // 栈展开保证所有局部对象被正确析构！
}

// ============================================================
// 练习 2: RAII = 异常安全的基础 — 没有 RAII 会怎样？
// ============================================================

// TODO 2.1: 写一个「不安全」的资源管理函数，用原始 new/delete
// 关键问题: 如果 do_work 抛出异常，资源会泄漏吗？
struct Resource {
  int _id;
  Resource(int id) : _id(id) {
    std::cout << "  Resource(" << _id << ") acquired\n";
  }
  ~Resource() { std::cout << "  Resource(" << _id << ") released\n"; }
  void use() {
    // 模拟: 使用时可能失败
  }
};

void unsafe_work(bool fail) {
  // ❌ 反模式: 手动 new/delete，异常不安全
  auto *r1 = new Resource(1);
  auto *r2 = new Resource(2);

  if (fail) {
    // 忘记 delete r1 和 r2！资源泄漏！
    // 在真实代码里，更危险的是: delete 写了，但 throw 跳过了它
    delete r1;
    delete r2;
    throw std::runtime_error("failure in unsafe_work");
  }

  delete r1;
  delete r2;
}

void safe_work(bool fail) {
  // ✅ 正确做法: 用 unique_ptr 管理（RAII）
  auto r1 = make_unique<Resource>(1);
  auto r2 = make_unique<Resource>(2);

  if (fail) {
    // 即使 throw，r1 和 r2 的析构函数也会被调用 → delete 自动发生
    throw std::runtime_error("failure in safe_work");
  }
  // 正常路径也不需要手动 delete
}

void exercise2_raii_is_safety() {
  std::cout << "\n=== 练习 2: RAII = 异常安全的基础 ===\n";

  std::cout << "--- 测试 safe_work (fail=true) ---\n";
  try {
    safe_work(true);
  } catch (const std::exception &e) {
    std::cout << "  caught: " << e.what() << "\n";
  }
  // 观察输出: Resource(1) acquired → Resource(2) acquired
  //           → Resource(2) released → Resource(1) released
  // 自动释放！顺序是构造的逆序。

  std::cout << "--- 测试 safe_work (fail=false) ---\n";
  try {
    safe_work(false);
  } catch (...) {
  }
  // 正常退出也会释放，RAII 不依赖异常 — 它依赖的是「离开作用域」
}

// ============================================================
// 练习 3: 三种异常安全保证 — Basic / Strong / No-throw
// ============================================================

// 场景: 一个简陋的 StringList 类，我们逐步改进它的异常安全性

class StringList {
  size_t _size = 0;
  size_t _cap = 0;
  string *_data = nullptr;

 public:
  ~StringList() { delete[] _data; }

  void push_back(const string &s) {
    // 如果空间不够，分配更多
    if (_size == _cap) {
      size_t new_cap = _cap == 0 ? 4 : _cap * 2;

      // ❌ 方式 A: 无异常安全保证
      // auto *new_data = new string[new_cap];
      // for (size_t i = 0; i < _size; ++i)
      //   new_data[i] = _data[i];   // 如果复制时抛异常怎么办？
      // delete[] _data;             // ⚠️ 这行可能执行不到！
      // _data = new_data;
      // _cap = new_cap;

      // ✅ 方式 B: Strong guarantee — 失败时原数据不变
      auto *new_data = new string[new_cap];  // 分配内存（可能抛 bad_alloc）
      try {
        for (size_t i = 0; i < _size; ++i)
          new_data[i] = _data[i];  // 逐元素复制（string 的拷贝可能抛异常）
      } catch (...) {
        delete[] new_data;  // 清理已分配的新空间
        throw;              // 重新抛出 — _data 和 _size 未变！
      }
      delete[] _data;  // 只有复制全部成功才释放旧数据
      _data = new_data;
      _cap = new_cap;
    }
    _data[_size] = s;
    ++_size;
  }

  // TODO 3.1: 分析上面的代码 — 为什么 delete[] _data 放在 try 后面？
  // 答: 只有当所有 string 拷贝成功后才替换旧数据，
  //     保证「要么全部成功，要么原数据毫发无伤」= Strong Guarantee

  void print() const {
    std::cout << "[";
    for (size_t i = 0; i < _size; ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << _data[i];
    }
    std::cout << "]\n";
  }
};

void exercise3_guarantees() {
  std::cout << "\n=== 练习 3: 三种异常安全保证 ===\n";

  // 三种保证（按从弱到强）:

  // 1. No-throw guarantee (最强)  — 函数绝不抛异常，标记 noexcept
  //    例如: 析构函数、swap、基本操作、移动构造
  //    → 调用者可以完全信任，不需要任何保护

  // 2. Strong guarantee (强保证)  — 如果抛异常，对象状态不变（原子性）
  //    例如: push_back 要么成功添加，要么对象恢复原状
  //    → 调用者可以安全地重试

  // 3. Basic guarantee (基本保证) — 如果抛异常，对象仍可安全析构，不泄漏
  //    例如: 允许部分修改，但保证资源不泄漏
  //    → 最小要求，低于这个就是 Bug

  StringList list;
  list.push_back("hello");
  list.push_back("world");
  list.push_back("exception");
  std::cout << "list: ";
  list.print();

  // TODO 3.2 思考: unique_ptr 和 shared_ptr 分别提供什么保证？
  // unique_ptr<T> 构造:    no-throw (只是指针赋值)
  // unique_ptr<T> 析构:    no-throw (delete 不抛异常)
  // make_unique<T>(args):  如果 T 构造抛异常 → 堆内存自动 delete → basic
  // make_shared<T>(args):  同上，控制块也会自动清理
}

// ============================================================
// 练习 4: noexcept — 编译器的优化提示 + 安全承诺
// ============================================================

// TODO 4.1: noexcept 函数 — 承诺「我绝不抛异常」
// 编译器会据此做优化，而且调用者可以做静态检查
void no_throw_func() noexcept {
  // 只能调用其他 noexcept 操作，或不抛异常的操作
  std::cout << "  no_throw_func: guaranteed no exception\n";
  // throw std::runtime_error("oops"); // ❌ 编译警告，运行期会调用 terminate()
}

void might_throw_func() {
  // 没有 noexcept — 可能抛，也可能不抛
  std::cout << "  might_throw_func: could throw\n";
}

void exercise4_noexcept() {
  std::cout << "\n=== 练习 4: noexcept ===\n";

  // noexcept 运算符 — 编译期检查表达式是否 noexcept
  std::cout << std::boolalpha;
  std::cout << "  no_throw_func() noexcept?  " << noexcept(no_throw_func())
            << "\n";
  std::cout << "  might_throw_func() noexcept? "
            << noexcept(might_throw_func()) << "\n";

  // TODO 4.2: noexcept 最关键的用途 — 移动构造/赋值
  // vector 扩容时:
  //   如果 T 的移动构造是 noexcept → 用移动（安全且快）
  //   如果 T 的移动构造不是 noexcept → 用拷贝（避免移动失败后无法恢复）

  struct MoveNoexcept {
    int *_data;
    MoveNoexcept(int val) : _data(new int(val)) {}
    MoveNoexcept(MoveNoexcept &&other) noexcept : _data(other._data) {
      other._data = nullptr;
    }
    //                      ^^^^^^^^ 这个 noexcept 太重要了！
    ~MoveNoexcept() { delete _data; }
  };

  struct MoveMayThrow {
    int *_data;
    MoveMayThrow(int val) : _data(new int(val)) {}
    MoveMayThrow(MoveMayThrow &&other) : _data(other._data) {
      other._data = nullptr;
      // 没有 noexcept — vector 扩容时不会用这个移动构造！
    }
    ~MoveMayThrow() { delete _data; }
  };

  // 验证 vector 是否会使用移动
  std::cout << "  vector<MoveNoexcept> — 扩容时用移动: "
            << std::is_nothrow_move_constructible_v<MoveNoexcept> << "\n";
  std::cout << "  vector<MoveMayThrow>  — 扩容时用拷贝: "
            << std::is_nothrow_move_constructible_v<MoveMayThrow> << "\n";

  MoveNoexcept m1(10);
  MoveNoexcept m2 = std::move(m1);
  std::cout << "  MoveNoexcept: m1._data = " << m1._data
            << " (应为 nullptr), m2._data = " << m2._data << "\n";

  // 规则: 移动构造/赋值应该总是 noexcept
  // 析构函数隐式 noexcept（不需要显式写）
  // swap 应该 noexcept
}

// ============================================================
// 练习 5: Copy-and-Swap 惯用法 — 实现 Strong Guarantee 的标准方式
// ============================================================

// 一个管理动态数组的类，展示 copy-and-swap 如何实现异常安全的 operator=
class SafeBuffer {
  size_t _size;
  int *_data;

 public:
  explicit SafeBuffer(size_t n) : _size(n), _data(new int[n]{}) {}

  ~SafeBuffer() { delete[] _data; }

  // 拷贝构造 — 可能抛异常（new 失败），但源对象不受影响
  SafeBuffer(const SafeBuffer &other)
      : _size(other._size), _data(new int[other._size]) {
    for (size_t i = 0; i < _size; ++i) {
      _data[i] = other._data[i];  // int 拷贝不抛异常，但通用类型可能抛
    }
  }

  // TODO 5.1: Copy-and-swap operator=
  // 步骤:
  //   1. 先拷贝 rhs 创建一个临时对象（利用拷贝构造）
  //      → 如果拷贝失败，*this 不受影响！
  //   2. 用 noexcept swap 交换临时对象和 *this 的内容
  //      → 交换绝对安全
  //   3. 临时对象析构，带走旧数据
  //      → 析构 noexcept
  SafeBuffer &operator=(const SafeBuffer &rhs) {
    // 拷贝 rhs → 失败时抛异常，*this 毫发无伤
    SafeBuffer temp(rhs);
    // 交换 — noexcept
    swap(temp);
    // temp 析构，带走旧数据
    return *this;
  }

  // 移动赋值 — 同样用 swap
  SafeBuffer &operator=(SafeBuffer &&rhs) noexcept {
    SafeBuffer temp(std::move(rhs));
    swap(temp);
    return *this;
  }

  // 移动构造
  SafeBuffer(SafeBuffer &&other) noexcept : _size(other._size), _data(other._data) {
    other._size = 0;
    other._data = nullptr;
  }

  // swap — 永远 noexcept
  void swap(SafeBuffer &other) noexcept {
    std::swap(_size, other._size);
    std::swap(_data, other._data);
  }

  int &operator[](size_t i) { return _data[i]; }
  const int &operator[](size_t i) const { return _data[i]; }
  size_t size() const { return _size; }

  void print() const {
    std::cout << "[";
    for (size_t i = 0; i < _size; ++i) {
      if (i) std::cout << ", ";
      std::cout << _data[i];
    }
    std::cout << "]\n";
  }
};

// ADL swap
void swap(SafeBuffer &a, SafeBuffer &b) noexcept { a.swap(b); }

void exercise5_copy_and_swap() {
  std::cout << "\n=== 练习 5: Copy-and-Swap 惯用法 ===\n";

  SafeBuffer buf1(5);
  for (size_t i = 0; i < buf1.size(); ++i)
    buf1[i] = static_cast<int>((i + 1) * 10);

  SafeBuffer buf2(3);
  buf2[0] = 100;
  buf2[1] = 200;
  buf2[2] = 300;

  std::cout << "交换前:\n  buf1: ";
  buf1.print();
  std::cout << "  buf2: ";
  buf2.print();

  buf1 = buf2;  // copy-and-swap operator=

  std::cout << "交换后:\n  buf1: ";
  buf1.print();
  std::cout << "  buf2: ";
  buf2.print();

  // copy-and-swap 的精髓:
  // ┌─────────────────────────────────────────────────┐
  // │ Strong guarantee = 先做可能失败的事（拷贝），    │
  // │                    再做绝对不失败的事（swap）     │
  // └─────────────────────────────────────────────────┘
}

// ============================================================
// 练习 6: 实战模式 — 用异常安全思维设计你的代码
// ============================================================
void exercise6_patterns() {
  std::cout << "\n=== 练习 6: 异常安全实战模式 ===\n";

  // TODO 6.1: Scope Guard 模式 — 离开作用域时执行清理
  // 前面 Lambda 练习学过，这里强调它的异常安全性
  {
    std::cout << "--- Scope Guard ---\n";
    bool committed = false;
    // 模拟: 做一系列操作，如果中途失败，回滚
    auto rollback = [&]() {
      if (!committed) {
        std::cout << "  ⚠ rollback! (模拟回滚)\n";
      }
    };

    try {
      std::cout << "  执行步骤1...\n";
      std::cout << "  执行步骤2...\n";
      // 如果这里 throw，rollback 会在 catch 前执行
      committed = true;
      std::cout << "  提交成功\n";
    } catch (...) {
      // rollback 在调用者那边已经触发了
    }
    if (!committed) rollback();
  }

  // TODO 6.2: 用 unique_ptr 做「事务」管理
  // 场景: 构建一个对象，在完成前不希望对外可见
  {
    std::cout << "--- 构造事务 ---\n";
    struct ComplexObject {
      unique_ptr<int> _a;
      unique_ptr<string> _b;
      ComplexObject(unique_ptr<int> a, unique_ptr<string> b)
          : _a(std::move(a)), _b(std::move(b)) {}
      void describe() const {
        std::cout << "    ComplexObject(a=" << *_a << ", b=" << *_b << ")\n";
      }
    };

    // 先创建所有部分，确认成功后再组合
    auto a = make_unique<int>(42);  // 失败则直接 throw，不浪费任何资源
    auto b = make_unique<string>("hello");
    // 两部分都成功 → 构造最终对象
    ComplexObject obj(std::move(a), std::move(b));
    obj.describe();
  }

  // TODO 6.3: 异常安全的资源获取模式
  // 原则: 「获取资源 → 立即交给 RAII 对象管理」
  {
    std::cout << "--- 资源获取模式 ---\n";

    // ❌ 危险: new 和 管理对象之间有窗口
    // Manager m(new Resource);  // 如果 Manager 构造抛异常，Resource 泄漏

    // ✅ 安全: 先把资源交给 RAII，再传递
    // auto res = make_unique<Resource>();
    // Manager m(std::move(res));

    auto res = make_unique<Resource>(99);
    std::cout << "  Resource 已安全包装在 unique_ptr 中\n";
  }

  // TODO 6.4: 总结：写异常安全代码的 4 条黄金法则
  std::cout << "\n--- 异常安全 4 条黄金法则 ---\n";
  std::cout << "  1. 用 RAII 管理所有资源（内存、文件、锁、socket）\n";
  std::cout << "  2. 析构函数永远不抛异常（默认就是 noexcept）\n";
  std::cout << "  3. 先做可能失败的事，再做不可逆的事（copy-and-swap）\n";
  std::cout << "  4. 移动构造/赋值标记 noexcept\n";
}

// ============================================================
// 总结思考:
// 1. 为什么说「RAII 是为异常安全而生的」？
// 2. delete 可能抛异常吗？为什么析构函数默认 noexcept 是安全的？
// 3. 为什么 vector 扩容时关心移动构造是否 noexcept？
// 4. copy-and-swap 如何用「空间换安全」实现 strong guarantee？
// 5. 什么函数应该标记 noexcept？（析构、移动、swap、getter）
// ============================================================

int main() {
  exercise1_basics();
  exercise2_raii_is_safety();
  exercise3_guarantees();
  exercise4_noexcept();
  exercise5_copy_and_swap();
  exercise6_patterns();

  std::cout << "\n全部练习完成！\n";
  return 0;
}
