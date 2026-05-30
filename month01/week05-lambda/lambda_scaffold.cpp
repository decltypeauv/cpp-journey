// Day 6: Lambda 深入 — 从「能用」到「精通」
// 编译: cmake -B build && cmake --build build
// 运行: ./build/lambda

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using std::cout;
using std::function;
using std::make_unique;
using std::string;
using std::vector;

// ============================================================
// 练习 1: Lambda 基础解剖 — 每个部分叫什么、做什么
// ============================================================
void exercise1_basics() {
  std::cout << "=== 练习 1: Lambda 基础解剖 ===\n";

  //   [捕获] (参数) mutable noexcept -> 返回类型 { 函数体 }
  //   ─┬──  ──┬──   ─┬──   ──┬───   ──┬─────   ──┬───
  //    捕获列表  参数  可变    不抛异常   尾置返回    函数体

  // TODO 1.1: 最简单的 lambda — 无捕获、无参数
  auto hello = []() { return "Hello"; };
  std::cout << hello() << "\n";

  // TODO 1.2: 带参数的 lambda
  auto add = [](int a, int b) { return a + b; };
  std::cout << "add(3, 4) = " << add(3, 4) << "\n";

  // TODO 1.3: 显式指定返回类型（尾置返回）
  // 什么时候需要？当有多个 return 且类型不同时，编译器可能推导失败
  auto safe_div = [](double a, double b) -> double {
    if (b == 0.0) return 0.0;  // 两个 return 都是 double → 自动推导也行
    return a / b;
  };
  std::cout << "safe_div(10, 3) = " << safe_div(10, 3) << "\n";
  std::cout << "safe_div(10, 0) = " << safe_div(10, 0) << "\n";

  // TODO 1.4: lambda 的本质 — 编译器生成的匿名函数对象
  // 下面的 lambda:
  //   auto inc = [](int x) { return x + 1; };
  // 等价于编译器生成:
  //   struct __anonymous {
  //     auto operator()(int x) const { return x + 1; }
  //   };
  //   auto inc = __anonymous{};
  auto inc = [](int x) { return x + 1; };
  std::cout << "inc(41) = " << inc(41) << "\n";

  // 关键理解: lambda 不是「函数指针」，它是一个「对象」！
  // 每个 lambda 有自己独一无二的类型（即使两个 lambda 签名完全一样）
}

// ============================================================
// 练习 2: 捕获列表 — 这是 lambda 最精妙也最易错的部分
// ============================================================
void exercise2_captures() {
  std::cout << "\n=== 练习 2: 捕获列表 ===\n";

  int x = 10;
  string s = "hello";

  // TODO 2.1: 按值捕获 [=] — 拷贝一份，lambda 内外互不影响
  auto by_val = [=]() { return x + s.size(); };
  x = 999;  // 修改外部的 x
  s = "changed";
  std::cout << "by_val() = " << by_val()
            << "  (用的还是旧值 x=10, s=\"hello\")\n";
  // 关键: 捕获发生在 lambda 定义时，不是调用时！

  // TODO 2.2: 按引用捕获 [&] — lambda 内修改会影响外部
  x = 10;  // 恢复
  auto by_ref = [&]() {
    x += 5;  // 修改外部 x
    return x;
  };
  std::cout << "by_ref() = " << by_ref() << ", 外部 x = " << x << "\n";

  // TODO 2.3: 混合捕获 — 指定哪些按值、哪些按引用
  int a = 1, b = 2;
  auto mixed = [a, &b]() {           // a 按值，b 按引用
    // a += 1; // ❌ 编译错误！按值捕获默认 const
    b += 10;   // ✅ 按引用可以修改
    return a + b;
  };
  std::cout << "mixed() = " << mixed() << ", b = " << b << "\n";

  // TODO 2.4: mutable — 让按值捕获的变量可以修改（只影响副本）
  int counter = 0;
  auto mut = [counter]() mutable {   // mutable 去掉 operator() 的 const
    counter += 1;
    return counter;
  };
  std::cout << "mut() = " << mut() << " (副本: 1)\n";
  std::cout << "mut() = " << mut() << " (副本: 2)\n";
  std::cout << "外部 counter = " << counter << " (仍是 0)\n";
  // mutable 让按值捕获的副本可以被修改，但不影响外部原变量
  // 这本质上是 lambda 内部的一个「局部静态变量」

  // TODO 2.5: 初始化捕获 [var = expr] (C++14) — 最强大的捕获方式
  // 可以移动 unique_ptr 进 lambda！
  auto ptr = make_unique<int>(42);
  auto owning = [p = std::move(ptr)]() {  // 所有权移入 lambda
    return *p;
  };
  // ptr 现在是 nullptr
  std::cout << "owning() = " << owning() << "\n";
  std::cout << "ptr == nullptr: " << (ptr == nullptr) << "\n";

  // 初始化捕获也可以做表达式计算:
  auto computed = [half = x / 2.0]() { return half; };
  std::cout << "computed() = " << computed() << " (x/2 = 5)\n";
}

// ============================================================
// 练习 3: 泛型 Lambda — 让 lambda 也变成模板
// ============================================================

// TODO 3.1: C++14 泛型 lambda — 用 auto 做参数类型
// 这等价于写了一个模板的 operator()
void exercise3_generic_lambdas() {
  std::cout << "\n=== 练习 3: 泛型 Lambda ===\n";

  // auto 参数 — 每个 auto 对应一个模板参数
  auto generic_add = [](auto a, auto b) { return a + b; };
  std::cout << "generic_add(1, 2)     = " << generic_add(1, 2) << "\n";
  std::cout << "generic_add(1.5, 2.3) = " << generic_add(1.5, 2.3) << "\n";
  std::cout << "generic_add(string(\"A\"), string(\"B\")) = "
            << generic_add(string("A"), string("B")) << "\n";

  // TODO 3.2: 泛型 lambda + decltype — 推导返回类型
  auto safe_multiply = [](auto a, auto b) -> decltype(a * b) {
    return a * b;
  };
  // decltype(a * b) 在编译期计算 a*b 会产生的类型
  auto result = safe_multiply(3, 4.5);
  std::cout << "safe_multiply(3, 4.5) = " << result
            << "  (type: double)\n";

  // TODO 3.3: 变参泛型 lambda
  auto print_all = [](auto &&...args) {
    ((std::cout << args << " "), ...);  // 折叠表达式配合泛型变参
    std::cout << "\n";
  };
  print_all(1, 2.5, "hello", string("world"));

  // TODO 3.4: C++20 模板 lambda — 比 auto 更精确
  // 需要显式指定模板参数或多个参数必须同类型时使用
  auto typed_add = []<typename T>(T a, T b) {  // 约束 a, b 同类型
    return a + b;
  };
  std::cout << "typed_add(1, 2) = " << typed_add(1, 2) << "\n";
  // typed_add(1, 2.5);  // ❌ 编译错误: T 推导冲突 (int vs double)

  // TODO 3.5: 泛型 lambda 的本质
  // auto add = [](auto a, auto b) { return a + b; };
  // 等价于:
  // struct __anonymous {
  //   template <typename T, typename U>
  //   auto operator()(T a, U b) const { return a + b; }
  // };
  // 这就是为什么泛型 lambda 只能用 C++14 以上
}

// ============================================================
// 练习 4: std::function — 类型擦除的「函数容器」
// ============================================================
void exercise4_std_function() {
  std::cout << "\n=== 练习 4: std::function ===\n";

  // std::function 可以存储任何「可调用对象」: lambda、函数指针、函数对象
  // 代价: 有虚函数调用的开销（类型擦除）+ 可能堆分配

  // TODO 4.1: 用 std::function 存储不同类型的可调用物
  function<int(int, int)> op;  // 接受两个 int，返回 int

  op = [](int a, int b) { return a + b; };
  std::cout << "lambda: 3 + 4 = " << op(3, 4) << "\n";

  op = [](int a, int b) { return a * b; };
  std::cout << "lambda: 3 * 4 = " << op(3, 4) << "\n";

  // 也能存函数指针
  // 但不能存泛型 lambda（因为 std::function 的类型是固定的）
  // function<int(int, int)> op2 = [](auto a, auto b) { return a + b; }; // ❌

  // TODO 4.2: std::function 的开销 — 什么时候用，什么时候不用
  // ✅ 用 std::function 的场景:
  //   - 需要把可调用对象存为成员变量
  //   - 回调注册（不同调用者传入不同的 lambda）
  // ❌ 不用 std::function 的场景:
  //   - 直接传给算法（用 auto&& 或模板参数即可）
  //   - 性能敏感的热路径

  struct CallbackStore {
    function<void(const string &)> _on_event;
    void trigger(const string &msg) {
      if (_on_event) _on_event(msg);
    }
  };

  CallbackStore cb;
  cb._on_event = [](const string &msg) { cout << "收到: " << msg << "\n"; };
  cb.trigger("按钮点击");
  cb._on_event = [](const string &msg) { cout << "日志: " << msg << "\n"; };
  cb.trigger("页面加载");

  // TODO 4.3: std::bind — 老式的参数绑定（了解即可，新代码用 lambda）
  // C++11 之前没有 lambda，用 bind 绑定参数
  auto minus = [](int a, int b) { return a - b; };
  // auto minus_10 = std::bind(minus, std::placeholders::_1, 10); // 老式写法
  auto minus_10 = [&](int x) { return minus(x, 10); };  // lambda 写法，更清晰
  std::cout << "minus_10(20) = " << minus_10(20) << "\n";
}

// ============================================================
// 练习 5: 高阶模式 — 返回 lambda 的 lambda、组合、柯里化
// ============================================================
void exercise5_higher_order() {
  std::cout << "\n=== 练习 5: 高阶 Lambda 模式 ===\n";

  // TODO 5.1: Lambda 工厂 — 返回 lambda 的函数
  // 场景: 根据配置生成不同行为的函数
  auto make_multiplier = [](int factor) {
    return [factor](int x) { return x * factor; };
  };

  auto times_2 = make_multiplier(2);
  auto times_10 = make_multiplier(10);
  std::cout << "times_2(5)  = " << times_2(5) << "\n";
  std::cout << "times_10(5) = " << times_10(5) << "\n";

  // TODO 5.2: 柯里化 (Currying) — 把多参数函数转成单参数链
  // f(a, b, c) → f(a)(b)(c)
  auto curry_add = [](int a) {
    return [a](int b) { return [a, b](int c) { return a + b + c; }; };
  };
  std::cout << "curry_add(1)(2)(3) = " << curry_add(1)(2)(3) << "\n";

  // TODO 5.3: Lambda 组合 — f(g(x))
  auto compose = [](auto f, auto g) {
    return [f, g](auto x) { return f(g(x)); };
  };

  auto inc = [](int x) { return x + 1; };
  auto square = [](int x) { return x * x; };

  auto inc_then_square = compose(square, inc);   // square(inc(x))
  auto square_then_inc = compose(inc, square);   // inc(square(x))

  std::cout << "inc_then_square(3)  = " << inc_then_square(3)
            << "  (3+1=4, 4²=16)\n";
  std::cout << "square_then_inc(3)  = " << square_then_inc(3)
            << "  (3²=9, 9+1=10)\n";

  // TODO 5.4: RAII 回调 — 用 lambda 做作用域守卫
  // 场景: 进入作用域做某事，离开时自动清理
  auto make_scope_guard = [](auto on_exit) {
    // 创造一个对象，析构时调用 on_exit
    struct Guard {
      decltype(on_exit) _fn;
      ~Guard() { _fn(); }
    };
    return Guard{std::move(on_exit)};
  };

  {
    cout << "进入作用域\n";
    auto guard = make_scope_guard([]() { cout << "  离开作用域 (自动清理!)\n"; });
    cout << "  做了一些工作...\n";
  }  // guard 析构 → 自动调用 on_exit
  cout << "已离开作用域\n";
}

// ============================================================
// 练习 6: 实战模式 — Lambda 在 STL 算法中的经典用法
// ============================================================
void exercise6_real_world() {
  std::cout << "\n=== 练习 6: Lambda 实战模式 ===\n";

  // TODO 6.1: find_if — 查找第一个满足条件的元素
  vector<int> nums{1, 4, 7, 10, 13, 16, 19};
  auto first_even = std::find_if(nums.begin(), nums.end(),
                                  [](int n) { return n % 2 == 0; });
  if (first_even != nums.end()) {
    std::cout << "第一个偶数: " << *first_even << "\n";
  }

  // TODO 6.2: sort 自定义排序 — 多字段排序
  struct Student {
    string _name;
    int _score;
    int _age;
  };
  vector<Student> students{
      {"Alice", 85, 20},
      {"Bob", 92, 19},
      {"Charlie", 85, 18},
      {"Diana", 92, 21},
      {"Eve", 78, 20},
  };

  // 按分数降序，同分按年龄升序
  std::sort(students.begin(), students.end(), [](const auto &a, const auto &b) {
    if (a._score != b._score) return a._score > b._score;  // 降序
    return a._age < b._age;                                 // 升序
  });

  std::cout << "排序结果 (分数↓, 年龄↑):\n";
  for (const auto &s : students) {
    std::cout << "  " << s._name << "  score=" << s._score << "  age=" << s._age
              << "\n";
  }

  // TODO 6.3: transform — 把容器映射成另一种容器
  vector<int> input{1, 2, 3, 4, 5};
  vector<int> squared(input.size());
  std::transform(input.begin(), input.end(), squared.begin(),
                 [](int n) { return n * n; });

  std::cout << "平方: ";
  for (int n : squared) std::cout << n << " ";
  std::cout << "\n";

  // TODO 6.4: remove_if — 条件删除
  vector<int> values{5, 12, 8, 3, 15, 7, 20, 1};
  auto new_end = std::remove_if(values.begin(), values.end(),
                                 [](int n) { return n > 10; });
  values.erase(new_end, values.end());  // 真正删除

  std::cout << "删除 >10 的元素后: ";
  for (int n : values) std::cout << n << " ";
  std::cout << "\n";

  // TODO 6.5: 闭包做计数器/状态机
  auto make_counter = []() {
    return [count = 0]() mutable { return ++count; };
  };
  auto counter = make_counter();
  std::cout << "counter: " << counter() << ", " << counter() << ", " << counter()
            << "\n";

  // TODO 6.6: 用 lambda 替代手写比较函数
  // 旧式: 需要定义 struct 或写全局函数
  // Lambda: 一行搞定，定义在使用点旁边
  string s1 = "Apple", s2 = "apple";
  auto case_insensitive = [](char a, char b) {
    return std::tolower(a) < std::tolower(b);
  };
  bool less = std::lexicographical_compare(
      s1.begin(), s1.end(), s2.begin(), s2.end(), case_insensitive);
  std::cout << "\"Apple\" < \"apple\" (忽略大小写): " << (less ? "true" : "false")
            << "\n";
}

// ============================================================
// 总结思考:
// 1. 为什么 lambda 的捕获发生在「定义时」而不是「调用时」？
// 2. 什么时候必须用 mutable？
// 3. 初始化捕获 [p = std::move(ptr)] 能做什么按值/按引用做不到的事？
// 4. std::function 和直接 auto 接 lambda 的区别是什么？
// 5. 泛型 lambda 比普通 lambda 多了什么能力？
// 6. 在 sort 之类的算法中用 lambda 比写函数指针好在哪？
// ============================================================

int main() {
  exercise1_basics();
  exercise2_captures();
  exercise3_generic_lambdas();
  exercise4_std_function();
  exercise5_higher_order();
  exercise6_real_world();

  std::cout << "\n全部练习完成！\n";
  return 0;
}
