// Day 5: Templates — 告别重复代码，拥抱泛型
// 编译: cmake -B build && cmake --build build
// 运行: ./build/templates

#include <concepts>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
using std::cout;
using std::endl;
using std::string;
using std::vector;

// ============================================================
// 练习 1: 函数模板 — 告别重载，一套代码适用多种类型
// ============================================================

// TODO 1.1: 实现函数模板 my_max(a, b)，返回较大值
// 要求: 支持 int, double, string 等可比较类型
// 提示: template <typename T> T my_max(T a, T b) { ... }
// YOUR CODE HERE
template <typename T> T my_max(const T &a, const T &b) {
  return (a > b) ? a : b;
}
// TODO 1.2: 实现函数模板 print_pair(key, value)，接受两个不同类型
// 打印格式: "key => value"
// 提示: template <typename K, typename V>
// YOUR CODE HERE
template <typename K, typename V> void print_pair(K key, V value) {
  cout << key << " => " << value << endl;
}

void exercise1_function_templates() {
  std::cout << "=== 练习 1: 函数模板 ===\n";

  // 测试 my_max
  std::cout << "my_max(3, 7) = " << my_max(3, 7) << "\n";
  std::cout << "my_max(3.14, 2.71) = " << my_max(3.14, 2.71) << "\n";
  std::cout << "my_max(string(\"apple\"), string(\"zoo\")) = "
            << my_max(string("apple"), string("zoo")) << "\n";

  // 测试 print_pair
  print_pair(string("name"), string("Alice"));
  print_pair(string("age"), 25);
  print_pair(42, 3.14159);

  // TODO 1.3: 思考 — my_max 对 int 和 double 各实例化了几次？
  // 编译器为每种类型组合生成一份独立代码，这个过程叫什么？
  // YOUR ANSWER HERE (写注释即可)
  // 回答: 对 int 实例化 1 次，对 double 实例化 1 次，对 string 实例化 1 次
  // 这个过程叫「模板实例化」(template instantiation)
  // 编译器在编译期将模板参数替换为具体类型，生成对应的函数定义
}

// ============================================================
// 练习 2: 类模板 — 泛型容器
// ============================================================

// TODO 2.1: 实现类模板 Stack<T> — 一个简单的栈
// 要求:
//   - push(T value) 压入
//   - pop() 弹出并返回栈顶（如果空则抛出异常）
//   - top() 查看栈顶（const）
//   - empty() 是否为空
//   - 内部用 std::vector<T> 存储
// 提示: 所有方法在类内实现（header-only 风格）
// YOUR CODE HERE
template <typename T> class Stack {
  std::vector<T> _data;

public:
  void push(T value) { _data.push_back(std::move(value)); }

  T pop() {
    if (_data.empty())
      throw std::runtime_error("Stack::pop: empty stack");
    T top = std::move(_data.back());
    _data.pop_back();
    return top;
  }

  const T &top() const {
    if (_data.empty())
      throw std::runtime_error("Stack::top: empty stack");
    return _data.back();
  }

  bool empty() const { return _data.empty(); }
};

// TODO 2.2: 实现类模板 Pair<K, V> — 键值对
// 有 _key 和 _value 两个成员，构造函数接受 K 和 V
// YOUR CODE HERE
template <typename K, typename V> struct Pair {
  K _key;
  V _value;

  Pair(K k, V v) : _key(std::move(k)), _value(std::move(v)) {}

  void print() const { cout << "Pair(" << _key << ", " << _value << ")\n"; }
};

void exercise2_class_templates() {
  std::cout << "\n=== 练习 2: 类模板 ===\n";

  // 测试 Stack<int>
  Stack<int> si;
  si.push(1);
  si.push(2);
  si.push(3);
  std::cout << "Stack<int>: ";
  while (!si.empty()) {
    std::cout << si.pop() << " ";
  }
  std::cout << "\n";

  // 测试 Stack<string>
  Stack<string> ss;
  ss.push("hello");
  ss.push("world");
  std::cout << "Stack<string> top: " << ss.top() << "\n";

  // 测试 Pair
  Pair<string, int> p1("age", 30);
  p1.print();
  Pair<double, char> p2(3.14, 'x');
  p2.print();

  // C++17 CTAD (Class Template Argument Deduction)
  // Pair p3("name", string("Bob"));  // C++17: 自动推导为 Pair<const char*,
  // string>
}

// ============================================================
// 练习 3: 模板特化 — 对特定类型做不同处理
// ============================================================

// 3.1 主模板: TypeName<T> — 返回类型的字符串描述
template <typename T> struct TypeName {
  static string get() { return "unknown"; }
};

// TODO 3.2: 为 int 做全特化，返回 "int"
// 提示: template<> struct TypeName<int> { ... };
// YOUR CODE HERE
template <> struct TypeName<int> {
  static string get() { return "int"; }
};

// TODO 3.3: 为 double 做全特化，返回 "double"
// YOUR CODE HERE
template <> struct TypeName<double> {
  static string get() { return "double"; }
};

// TODO 3.4: 为 string 做全特化，返回 "std::string"
// YOUR CODE HERE
template <> struct TypeName<string> {
  static string get() { return "std::string"; }
};

// TODO 3.5: 部分特化 — 为指针类型 T* 做处理，返回 TypeName<T>::get() + "*"
// 提示: template <typename T> struct TypeName<T*> { ... };
// YOUR CODE HERE
template <typename T> struct TypeName<T *> {
  static string get() { return TypeName<T>::get() + "*"; }
};

void exercise3_specialization() {
  std::cout << "\n=== 练习 3: 模板特化 ===\n";

  std::cout << "TypeName<int>::get()    = " << TypeName<int>::get() << "\n";
  std::cout << "TypeName<double>::get() = " << TypeName<double>::get() << "\n";
  std::cout << "TypeName<string>::get() = " << TypeName<string>::get() << "\n";
  std::cout << "TypeName<int*>::get()   = " << TypeName<int *>::get() << "\n";
  std::cout << "TypeName<double*>::get()= " << TypeName<double *>::get()
            << "\n";
  std::cout << "TypeName<float>::get()  = " << TypeName<float>::get()
            << "  ← 走主模板\n";

  // 思考: 全特化 vs 部分特化 的区别？
  // 全特化: template<> — 所有模板参数被具体类型替换
  // 部分特化: template<typename T> struct X<T*> — 保留部分参数，只特化一部分
}

// ============================================================
// 练习 4: 非类型模板参数 — 编译期常量
// ============================================================

// TODO 4.1: 实现 FixedArray<T, N> — 编译期固定大小的数组
// 要求:
//   - 大小 N 是编译期常量 (size_t)
//   - 内部用 T _data[N] 栈上分配（不需要 new/delete）
//   - size() 返回 N
//   - operator[](i) 读写元素（不做边界检查以保持性能）
//   - at(i) 带边界检查（越界抛异常）
//   - begin() / end() 返回指针，支持 range-for
//   - front() / back() 返回首尾元素引用
// YOUR CODE HERE
template <typename T, size_t N> class FixedArray {
  T _data[N];

public:
  constexpr size_t size() const { return N; }

  T &operator[](size_t i) { return _data[i]; }
  const T &operator[](size_t i) const { return _data[i]; }

  T &at(size_t i) {
    if (i >= N)
      throw std::out_of_range("FixedArray::at: index out of range");
    return _data[i];
  }

  T *begin() { return _data; }
  T *end() { return _data + N; }
  const T *begin() const { return _data; }
  const T *end() const { return _data + N; }

  T &front() { return _data[0]; }
  T &back() { return _data[N - 1]; }
};

// TODO 4.2: 实现函数模板 array_sum，对任意大小的 FixedArray<T,N> 求和
// 提示: 用 range-for 遍历
// YOUR CODE HERE
template <typename T, size_t N> T array_sum(const FixedArray<T, N> &arr) {
  T total{};
  for (const auto &v : arr) {
    total += v;
  }
  return total;
}

void exercise4_nontype_params() {
  std::cout << "\n=== 练习 4: 非类型模板参数 ===\n";

  FixedArray<int, 5> arr;
  for (size_t i = 0; i < arr.size(); ++i) {
    arr[i] = static_cast<int>((i + 1) * 10);
  }

  std::cout << "arr.size() = " << arr.size() << "\n";
  std::cout << "arr: ";
  for (auto v : arr)
    std::cout << v << " ";
  std::cout << "\n";

  std::cout << "arr.front() = " << arr.front() << "\n";
  std::cout << "arr.back()  = " << arr.back() << "\n";
  std::cout << "sum = " << array_sum(arr) << "\n";

  // 思考: sizeof(FixedArray<int, 5>) 大概多大？和 std::vector<int> 的区别？
  // FixedArray: 5 * sizeof(int) = 20 字节（栈上，无堆分配开销）
  // std::vector: 24 字节（3 个指针）+ 堆上分配的动态数组
}

// ============================================================
// 练习 5: 变参模板 + 折叠表达式 (C++17)
// ============================================================

// TODO 5.1: 实现 print_all(Args... args) — 打印任意数量的参数
// 提示: 用折叠表达式 (cout << ... << args) 或递归
// C++17 折叠: (std::cout << ... << args)
// YOUR CODE HERE
template <typename... Args> void print_all(Args... args) {
  (std::cout << ... << args);
  std::cout << "\n";
}

// TODO 5.2: 实现 sum(Args... args) — 返回所有参数的和
// 提示: 用折叠表达式 (... + args)
// YOUR CODE HERE
template <typename... Args> auto sum(Args... args) { return (... + args); }

// TODO 5.3: 实现 print_with_sep(sep, args...) — 用分隔符打印
// 例如: print_with_sep(" | ", 1, 2, 3) 输出 "1 | 2 | 3"
// 提示: 先处理第一个参数避免多余分隔符，或使用更复杂的折叠技巧
// 折叠技巧: ((cout << sep << args), ...) — 注意第一个参数前也会有分隔符
// 更优雅的方式: 用数组 + 初始化列表展开
// YOUR CODE HERE
template <typename Sep, typename... Args>
void print_with_sep(Sep sep, Args... args) {
  bool first = true;
  auto print_one = [&](auto &&arg) {
    if (first) {
      first = false;
    } else {
      cout << sep;
    }
    cout << arg;
  };
  (print_one(args), ...);
  cout << "\n";
}

void exercise5_variadic() {
  std::cout << "\n=== 练习 5: 变参模板 + 折叠表达式 ===\n";

  print_all(1, " + ", 2, " + ", 3, " = ", 6);
  print_all("Hello", ", ", "World", "!");

  std::cout << "sum(1, 2, 3, 4, 5) = " << sum(1, 2, 3, 4, 5) << "\n";
  std::cout << "sum(1.5, 2.5, 3.0) = " << sum(1.5, 2.5, 3.0) << "\n";

  print_with_sep(" -> ", 1, 2, 3, 4, 5);
  print_with_sep<string>(" | ", "apple", "banana", "cherry");

  // 思考:
  // 1. sizeof...(Args) 可以在编译期拿到参数个数
  // 2. 折叠表达式有 4 种: 一元左折叠(... op args)、一元右折叠(args op ...)
  //    二元左折叠(init op ... op args)、二元右折叠(args op ... op init)
}

// ============================================================
// 练习 6: C++20 Concepts — 约束模板参数
// ============================================================

// TODO 6.1: 定义一个 concept — Numeric，约束类型为整数或浮点数
// 提示: template <typename T> concept Numeric = std::is_arithmetic_v<T>;
// 或者用 requires 表达式
// YOUR CODE HERE
template <typename T>
concept Numeric = std::is_arithmetic_v<T>;

// TODO 6.2: 用 concept 约束 multiply(a, b) — 只接受数值类型
// 注意: 两个参数类型可以不同，但都必须满足 Numeric
// 提示: template <Numeric T, Numeric U> auto multiply(T a, U b) { ... }
// 或者用缩写: auto multiply(Numeric auto a, Numeric auto b) { ... }
// YOUR CODE HERE
auto multiply(Numeric auto a, Numeric auto b) {
  // 使用更精确的类型做返回值
  return a * b;
}

// TODO 6.3: 定义一个 Printable concept — 约束类型支持 cout <<
// 然后用它写一个 print_value 函数
// 提示: template<typename T> concept Printable = requires(T t) { std::cout <<
// t; }; YOUR CODE HERE
template <typename T>
concept Printable = requires(T t) { std::cout << t; };

void print_value(Printable auto const &v) { cout << v << "\n"; }

// TODO 6.4: 用 requires 子句约束一个函数模板 addable_or_string<T>
// 要求: 要么支持 + 运算，要么可转换为 string
// 提示: 用 requires(T a, T b) { { a + b }; }
// YOUR CODE HERE
template <typename T>
  requires requires(T a, T b) {
    { a + b };
  }
T add_or_concat(T a, T b) {
  return a + b;
}

void exercise6_concepts() {
  std::cout << "\n=== 练习 6: C++20 Concepts ===\n";

  // multiply 只接受数值
  std::cout << "multiply(3, 4)     = " << multiply(3, 4) << "\n";
  std::cout << "multiply(3.5, 2)   = " << multiply(3.5, 2) << "\n";
  std::cout << "multiply(2, 3.14)  = " << multiply(2, 3.14) << "\n";
  // 下面这行会编译失败 — 这就是 concept 的价值：清晰的错误信息
  // multiply(string("hello"), string("world"));  // 编译错误: 不满足 Numeric

  // print_value 接受任何可打印类型
  print_value(42);
  print_value(3.14159);
  print_value(string("Hello Concepts!"));

  // add_or_concat 接受支持 + 的类型
  std::cout << "add_or_concat(10, 20)          = " << add_or_concat(10, 20)
            << "\n";
  std::cout << "add_or_concat(string(\"A\"), string(\"B\")) = "
            << add_or_concat(string("A"), string("B")) << "\n";

  // 思考: concept vs SFINAE vs static_assert 的区别？
  // concept: 约束在「接口」上，编译错误在调用点给出，信息清晰
  // SFINAE: 替换失败不是错误，用于重载决议，语法晦涩
  // static_assert: 约束在「函数体内部」，错误信息在实现内部，不够直观
  // C++20 concepts 是现代 C++ 推荐做法
}

// ============================================================
// 总结思考:
// 1. 模板的核心价值是什么？（提示: 编译期多态 vs 运行期多态）
// 2. 模板代码为什么通常是 header-only？
// 3. 什么场景用全特化，什么场景用部分特化？
// 4. 非类型模板参数只能是整数吗？（提示: C++20 允许浮点和类类型）
// 5. 变参模板在哪些 STL 设施中使用？（std::tuple, std::variant,
// make_shared...）
// 6. concept 比传统的 enable_if/SFINAE 好在哪里？
// ============================================================

int main() {
  exercise1_function_templates();
  exercise2_class_templates();
  exercise3_specialization();
  exercise4_nontype_params();
  exercise5_variadic();
  exercise6_concepts();

  std::cout << "\n全部练习完成！\n";
  return 0;
}
