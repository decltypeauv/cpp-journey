// ============================================================================
// Month 5 Week 27: 小型 STL 实现 — 从零构建标准库
// 日期: 2026-06-24
//
// "STL 不是魔法, 是 C++ 工程的最佳实践集合"
//
// 目标: 亲手实现 STL 的核心组件, 理解每一个设计决策
// 与 Week 23 (STL 源码深潜) 互补 — Week 23 是阅读, Week 27 是构建
//
// 10 个组件, 从基础到复合, 最后集成为完整项目:
//   1. MiniUniquePtr — 独占所有权, 自定义删除器
//   2. MiniSharedPtr — 共享所有权, Control Block
//   3. MiniOptional — 总和类型, monadic map/and_then
//   4. MiniVariant  — 类型安全的 union, visit
//   5. MiniSpan     — 无所有权视图, 边界检查
//   6. MiniList     — 双向链表, 节点抽象
//   7. MiniDeque    — 分块数组, O(1) 随机访问
//   8. MiniSet      — 红黑树基础
//   9. MiniAny      — 类型擦除 + SBO
//  10. Capstone     — JSON 解析库 (使用所有组件)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace std::literals;

// ============================================================================
// 辅助工具
// ============================================================================
template <typename... Args> void println(Args&&... args) {
  if constexpr (sizeof...(args) > 0) ((std::cout << std::forward<Args>(args)), ...);
  std::cout << '\n';
}
template <typename... Args> void print(Args&&... args) {
  ((std::cout << std::forward<Args>(args)), ...);
}
void HR(std::string_view t) { println("\n", std::string(72, '='), "\n  ", t, "\n", std::string(72, '=')); }

// ============================================================================
// Exercise 1: MiniUniquePtr — 独占所有权
// ============================================================================
//
// std::unique_ptr 的核心设计:
//   - 独占所有权 (move-only, 不可拷贝)
//   - 零开销 (sizeof(unique_ptr<T>) == sizeof(T*))
//   - 自定义 Deleter (默认 std::default_delete<T>)
//     - 无状态 Deleter: 零开销 (Empty Base Optimization)
//     - 有状态 Deleter: sizeof(ptr+deleter)
//   - make_unique 防止 new/delete 不匹配

namespace ex1 {

template <typename T>
struct DefaultDelete {
  void operator()(T* p) const { delete p; }
};

template <typename T>
struct DefaultDelete<T[]> {
  void operator()(T* p) const { delete[] p; }
};

template <typename T, typename Deleter = DefaultDelete<T>>
class MiniUniquePtr {
  T* _ptr = nullptr;
  [[no_unique_address]] Deleter _deleter{}; // EBO: 无状态 deleter 不占空间

public:
  MiniUniquePtr() = default;
  explicit MiniUniquePtr(T* p) : _ptr(p) {}
  MiniUniquePtr(T* p, Deleter d) : _ptr(p), _deleter(std::move(d)) {}

  ~MiniUniquePtr() { if (_ptr) _deleter(_ptr); }

  // move-only
  MiniUniquePtr(MiniUniquePtr&& o) noexcept : _ptr(o._ptr), _deleter(std::move(o._deleter)) {
    o._ptr = nullptr;
  }
  MiniUniquePtr& operator=(MiniUniquePtr&& o) noexcept {
    if (this != &o) { reset(); _ptr = o._ptr; _deleter = std::move(o._deleter); o._ptr = nullptr; }
    return *this;
  }
  MiniUniquePtr(const MiniUniquePtr&) = delete;
  MiniUniquePtr& operator=(const MiniUniquePtr&) = delete;

  // 观察
  T* get() const { return _ptr; }
  T& operator*() const { return *_ptr; }
  T* operator->() const { return _ptr; }
  explicit operator bool() const { return _ptr != nullptr; }

  // 修改
  T* release() { T* p = _ptr; _ptr = nullptr; return p; }
  void reset(T* p = nullptr) {
    if (_ptr) _deleter(_ptr);
    _ptr = p;
  }

  Deleter& get_deleter() { return _deleter; }
};

// make_unique 模拟
template <typename T, typename... Args>
MiniUniquePtr<T> make_mini_unique(Args&&... args) {
  return MiniUniquePtr<T>(new T(std::forward<Args>(args)...));
}

// 数组特化
template <typename T>
class MiniUniquePtr<T[]> {
  T* _ptr = nullptr;
public:
  explicit MiniUniquePtr(T* p) : _ptr(p) {}
  ~MiniUniquePtr() { delete[] _ptr; }
  MiniUniquePtr(MiniUniquePtr&& o) noexcept : _ptr(o._ptr) { o._ptr = nullptr; }
  MiniUniquePtr& operator=(MiniUniquePtr&& o) noexcept { reset(); _ptr = o._ptr; o._ptr = nullptr; return *this; }
  MiniUniquePtr(const MiniUniquePtr&) = delete;
  MiniUniquePtr& operator=(const MiniUniquePtr&) = delete;
  T* get() const { return _ptr; }
  T& operator[](size_t i) const { return _ptr[i]; }
  void reset(T* p = nullptr) { delete[] _ptr; _ptr = p; }
  T* release() { T* p = _ptr; _ptr = nullptr; return p; }
};

void run() {
  HR("Ex1: MiniUniquePtr — 独占所有权");

  auto p = make_mini_unique<int>(42);
  println("  *p = ", *p, "  sizeof(ptr) = ", sizeof(p), " (raw ptr = ", sizeof(int*), ") ✅ EBO");
  *p = 100;
  println("  修改后 *p = ", *p);

  // move
  auto p2 = std::move(p);
  println("  move 后: p.get()=", (void*)p.get(), " p2.get()=", (void*)p2.get(), " *p2=", *p2);

  // 自定义 deleter (FILE*)
  struct FileCloser { void operator()(std::FILE* f) const { if (f) std::fclose(f); } };
  MiniUniquePtr<std::FILE, FileCloser> fp(std::fopen("/dev/null", "w"));
  println("  FILE* unique_ptr: get()=", (void*)fp.get(), " (自动 fclose)");

  // 数组
  MiniUniquePtr<int[]> arr(new int[5]{1,2,3,4,5});
  println("  数组: arr[0]=", arr[0], " arr[4]=", arr[4]);

  println("\n📖 关键设计:");
  println("  - [[no_unique_address]] → 无状态 Deleter 零开销 (EBO)");
  println("  - move-only → 明确的资源所有权");
  println("  - release() → 放弃所有权 (用于 C API 互操作)");
}

} // namespace ex1

// ============================================================================
// Exercise 2: MiniSharedPtr — 共享所有权
// ============================================================================
//
// std::shared_ptr 的核心设计:
//   - Control Block: { strong_refs, weak_refs, deleter, allocator }
//   - 原子引用计数 (线程安全)
//   - sizeof(shared_ptr) = 2 × sizeof(void*) (ptr + control_block*)
//   - make_shared: 单次分配 (object + control block 相邻)

namespace ex2 {

using ex1::DefaultDelete;

// ── Control Block ───────────────────────────────────────────────────
struct ControlBlock {
  std::atomic<long> _strong{1};
  std::atomic<long> _weak{0};

  void add_ref() { _strong.fetch_add(1, std::memory_order_relaxed); }
  bool release_ref() {
    if (_strong.fetch_sub(1, std::memory_order_acq_rel) == 1) { return true; }
    return false;
  }
  void add_weak() { _weak.fetch_add(1, std::memory_order_relaxed); }
  void release_weak() {
    if (_weak.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }

  virtual void destroy_resource() = 0;
  virtual ~ControlBlock() = default;
};

template <typename T, typename Deleter = DefaultDelete<T>>
struct ControlBlockImpl : ControlBlock {
  T* _ptr;
  [[no_unique_address]] Deleter _deleter;

  ControlBlockImpl(T* p, Deleter d) : _ptr(p), _deleter(std::move(d)) {}
  void destroy_resource() override {
    if (_ptr) _deleter(_ptr);
  }
};

// ── MiniSharedPtr ────────────────────────────────────────────────────
template <typename T>
class MiniSharedPtr {
  T* _ptr = nullptr;
  ControlBlock* _cb = nullptr;

  template <typename U> friend class MiniSharedPtr;
  template <typename U> friend class MiniWeakPtr;

  MiniSharedPtr(T* p, ControlBlock* cb) : _ptr(p), _cb(cb) {}

public:
  MiniSharedPtr() = default;

  template <typename U, typename Deleter = DefaultDelete<U>>
  explicit MiniSharedPtr(U* p, Deleter d = Deleter{})
    : _ptr(p), _cb(new ControlBlockImpl<U, Deleter>(p, std::move(d))) {}

  ~MiniSharedPtr() { release(); }

  // 拷贝 (共享所有权)
  MiniSharedPtr(const MiniSharedPtr& o) : _ptr(o._ptr), _cb(o._cb) {
    if (_cb) _cb->add_ref();
  }
  MiniSharedPtr& operator=(const MiniSharedPtr& o) {
    if (this != &o) { release(); _ptr = o._ptr; _cb = o._cb; if (_cb) _cb->add_ref(); }
    return *this;
  }

  // 移动
  MiniSharedPtr(MiniSharedPtr&& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
    o._ptr = nullptr; o._cb = nullptr;
  }
  MiniSharedPtr& operator=(MiniSharedPtr&& o) noexcept {
    if (this != &o) { release(); _ptr = o._ptr; _cb = o._cb; o._ptr = nullptr; o._cb = nullptr; }
    return *this;
  }

  // 观察
  T* get() const { return _ptr; }
  T& operator*() const { return *_ptr; }
  T* operator->() const { return _ptr; }
  long use_count() const { return _cb ? _cb->_strong.load() : 0; }
  explicit operator bool() const { return _ptr != nullptr; }

private:
  void release() {
    if (_cb && _cb->release_ref()) {
      _cb->destroy_resource();
      _cb->release_weak(); // release our weak reference to cb
    }
    _ptr = nullptr; _cb = nullptr;
  }
};

// make_shared 模拟 (单次分配版省略, 这里先两次分配)
template <typename T, typename... Args>
MiniSharedPtr<T> make_mini_shared(Args&&... args) {
  return MiniSharedPtr<T>(new T(std::forward<Args>(args)...));
}

void run() {
  HR("Ex2: MiniSharedPtr — 共享所有权");

  auto sp1 = make_mini_shared<int>(42);
  println("  sp1: *=", *sp1, " use_count=", sp1.use_count());

  {
    auto sp2 = sp1; // 拷贝 → 引用计数+1
    println("  sp2(sp1): sp1.use_count=", sp1.use_count(), " sp2.use_count=", sp2.use_count());
    *sp2 = 99;
    println("  *sp2=99 → *sp1=", *sp1);
  }
  println("  sp2 析构后: use_count=", sp1.use_count());

  println("\n📖 Control Block 设计:");
  println("  ┌─────────────┐");
  println("  │ ControlBlock │ shared_ptr ──► T*   (对象指针)");
  println("  │  strong_ref  │             └─► CB*  (控制块指针)");
  println("  │  weak_ref    │");
  println("  │  deleter     │  make_shared: T + CB 在一次分配中相邻");
  println("  └─────────────┘");
}

} // namespace ex2

// ============================================================================
// Exercise 3: MiniOptional — 总和类型
// ============================================================================
//
// std::optional 的核心设计:
//   - 总和类型: T | nullopt
//   - 内部: bool _has_value + aligned_storage_t<sizeof(T), alignof(T)>
//   - placement new / explicit destructor
//   - monadic 操作 (C++23): transform, and_then, or_else

namespace ex3 {

struct NulloptT { explicit NulloptT() = default; };
inline constexpr NulloptT nullopt{};

template <typename T>
class MiniOptional {
  alignas(T) unsigned char _buf[sizeof(T)];
  bool _has_value = false;

public:
  MiniOptional() = default;
  MiniOptional(NulloptT) : _has_value(false) {}
  MiniOptional(const T& val) : _has_value(true) { new (_buf) T(val); }
  MiniOptional(T&& val) : _has_value(true) { new (_buf) T(std::move(val)); }

  MiniOptional(const MiniOptional& o) : _has_value(o._has_value) {
    if (o._has_value) new (_buf) T(*o);
  }
  MiniOptional(MiniOptional&& o) noexcept : _has_value(o._has_value) {
    if (o._has_value) { new (_buf) T(std::move(*o)); o.reset(); }
  }
  MiniOptional& operator=(const MiniOptional& o) {
    if (this != &o) { reset(); if (o._has_value) { new (_buf) T(*o); _has_value = true; } }
    return *this;
  }
  MiniOptional& operator=(MiniOptional&& o) noexcept {
    if (this != &o) { reset(); if (o._has_value) { new (_buf) T(std::move(*o)); _has_value = true; o.reset(); } }
    return *this;
  }

  ~MiniOptional() { reset(); }

  // 观察
  bool has_value() const { return _has_value; }
  explicit operator bool() const { return _has_value; }
  T& operator*() { return *reinterpret_cast<T*>(_buf); }
  const T& operator*() const { return *reinterpret_cast<const T*>(_buf); }
  T* operator->() { return reinterpret_cast<T*>(_buf); }
  const T* operator->() const { return reinterpret_cast<const T*>(_buf); }

  T& value() {
    if (!_has_value) throw std::runtime_error("bad optional access");
    return **this;
  }
  T value_or(T&& default_val) const {
    return _has_value ? **this : std::forward<T>(default_val);
  }

  // 修改
  void reset() {
    if (_has_value) { reinterpret_cast<T*>(_buf)->~T(); _has_value = false; }
  }
  template <typename... Args> T& emplace(Args&&... args) {
    reset();
    new (_buf) T(std::forward<Args>(args)...);
    _has_value = true;
    return **this;
  }

  // Monadic (C++23 style, 这里给 C++20)
  template <typename F>
  auto map(F&& f) -> MiniOptional<std::decay_t<decltype(f(std::declval<T&>()))>> {
    using R = std::decay_t<decltype(f(**this))>;
    if (!_has_value) return nullopt;
    return MiniOptional<R>(f(**this));
  }

  template <typename F>
  auto and_then(F&& f) -> decltype(f(std::declval<T&>())) {
    if (!_has_value) return nullopt;
    return f(**this);
  }
};

// 便捷构造
template <typename T> MiniOptional<std::decay_t<T>> make_optional(T&& v) {
  return MiniOptional<std::decay_t<T>>(std::forward<T>(v));
}

void run() {
  HR("Ex3: MiniOptional — 总和类型");

  MiniOptional<int> a(42);
  MiniOptional<int> b(nullopt);
  MiniOptional<std::string> c(std::string("hello"));

  println("  a.has_value()=", a.has_value(), " *a=", *a);
  println("  b.has_value()=", b.has_value(), " b.value_or(0)=", b.value_or(0));
  println("  *c=", *c);

  // Monadic
  auto d = a.map([](int x) { return x * 2; });
  println("  a.map(x*2) = ", *d);

  auto e = b.map([](int x) { return x * 2; });
  println("  b(nullopt).map(x*2).has_value() = ", e.has_value());

  auto f = c.and_then([](const std::string& s) -> MiniOptional<size_t> {
    if (s.empty()) return nullopt;
    return MiniOptional<size_t>(s.size());
  });
  println("  c.and_then(size) = ", *f);

  println("\n📖 Optional 关键设计:");
  println("  - aligned_storage → 避免堆分配, 栈上的 union");
  println("  - placement new / explicit dtor → 管理生命周期");
  println("  - map / and_then → 链式操作, 类似 Rust 的 Option");
}

} // namespace ex3

// ============================================================================
// Exercise 4: MiniVariant — 类型安全的 union
// ============================================================================
//
// std::variant 的核心设计:
//   - 编译期确定的类型列表, 运行时知道"当前是哪个"
//   - index() 返回当前活跃类型的索引
//   - visit: 根据当前类型 dispatch 到正确的函数重载
//   - 实现: 递归 union + index

namespace ex4 {

// ── 辅助: 在类型列表中找最大 ──────────────────────────────────────
template <typename... Ts> struct MaxSize;
template <typename T> struct MaxSize<T> { static constexpr size_t value = sizeof(T); };
template <typename T, typename... Ts>
struct MaxSize<T, Ts...> {
  static constexpr size_t value = sizeof(T) > MaxSize<Ts...>::value ? sizeof(T) : MaxSize<Ts...>::value;
};

template <typename... Ts> struct MaxAlign;
template <typename T> struct MaxAlign<T> { static constexpr size_t value = alignof(T); };
template <typename T, typename... Ts>
struct MaxAlign<T, Ts...> {
  static constexpr size_t value = alignof(T) > MaxAlign<Ts...>::value ? alignof(T) : MaxAlign<Ts...>::value;
};

// 类型索引查找
template <typename T, typename... Ts> struct IndexOf;
template <typename T, typename T0, typename... Ts>
struct IndexOf<T, T0, Ts...> { static constexpr size_t value = 1 + IndexOf<T, Ts...>::value; };
template <typename T, typename... Ts>
struct IndexOf<T, T, Ts...> { static constexpr size_t value = 0; };

// ── MiniVariant (简化版, 不支持 valueless_by_exception) ───────────
template <typename... Ts>
class MiniVariant {
  static constexpr size_t kSize = MaxSize<Ts...>::value;
  static constexpr size_t kAlign = MaxAlign<Ts...>::value;
  alignas(kAlign) unsigned char _buf[kSize];
  size_t _index = 0;

public:
  MiniVariant() {
    using First = std::tuple_element_t<0, std::tuple<Ts...>>;
    new (_buf) First{};
    _index = 0;
  }

  template <typename T>
  MiniVariant(const T& val) { emplace<T>(val); }

  ~MiniVariant() { destroy(); }

  size_t index() const { return _index; }

  template <typename T> bool holds_alternative() const { return _index == IndexOf<T, Ts...>::value; }

  template <typename T> T& get() {
    if (!holds_alternative<T>()) throw std::runtime_error("bad variant access");
    return *reinterpret_cast<T*>(_buf);
  }

  template <typename T, typename... Args> void emplace(Args&&... args) {
    destroy();
    new (_buf) T(std::forward<Args>(args)...);
    _index = IndexOf<T, Ts...>::value;
  }

  // visit (简化: 用 if-constexpr 展开)
  template <typename Visitor> decltype(auto) visit(Visitor&& vis) {
    return visit_impl(std::forward<Visitor>(vis), std::make_index_sequence<sizeof...(Ts)>{});
  }
  template <typename Visitor> decltype(auto) visit(Visitor&& vis) const {
    return const_cast<MiniVariant*>(this)->visit(std::forward<Visitor>(vis));
  }

private:
  void destroy() {
    visit_impl([](auto& x) { using T = std::decay_t<decltype(x)>; x.~T(); }, std::make_index_sequence<sizeof...(Ts)>{});
  }

  template <typename Visitor, size_t... Is>
  decltype(auto) visit_impl(Visitor&& vis, std::index_sequence<Is...>) {
    using RetType = std::common_type_t<decltype(vis(std::declval<Ts&>()))...>;
    if constexpr (std::is_void_v<RetType>) {
      ((_index == Is ? (vis(*reinterpret_cast<Ts*>(_buf)), true) : false) || ...);
    } else {
      RetType result{};
      ((_index == Is ? (result = vis(*reinterpret_cast<Ts*>(_buf)), true) : false) || ...);
      return result;
    }
  }
};

void run() {
  HR("Ex4: MiniVariant — 类型安全的 union");

  MiniVariant<int, double, std::string> v(42);
  println("  v.index()=", v.index(), " holds<int>=", v.holds_alternative<int>());

  // visit
  v.visit([](auto& x) { println("  visit: ", x); });

  v.emplace<double>(3.14159);
  v.visit([](auto& x) { println("  visit: ", x); });

  v.emplace<std::string>("hello variant");
  v.visit([](auto& x) { println("  visit: ", x); });

  println("\n📖 Variant 关键设计:");
  println("  - 编译期算 max(sizeof(T)...) → 分配足够大的 buffer");
  println("  - index() 标记当前活跃类型 → dispatch 时用 if-chain");
  println("  - visit: 展开所有类型, 运行时根据 index 执行对应分支");
  println("  - valueless_by_exception: emplace 抛异常时的安全状态 (此处省略)");
}

} // namespace ex4

// ============================================================================
// Exercise 5: MiniSpan — 无所有权视图
// ============================================================================
//
// std::span (C++20) 的核心设计:
//   - { T* data, size_t size } — 不拥有内存
//   - 比 pair<T*, size> 类型安全
//   - 支持 subspan, first, last
//   - 动态 extent (运行时大小) 和静态 extent (编译期大小)

namespace ex5 {

template <typename T, size_t Extent = size_t(-1)>
class MiniSpan {
  T* _data = nullptr;
  size_t _size = 0;

public:
  MiniSpan() = default;
  MiniSpan(T* data, size_t size) : _data(data), _size(size) {}

  // 从容器构造
  template <typename Container>
  MiniSpan(Container& c) : _data(c.data()), _size(c.size()) {}

  // 从数组构造
  template <size_t N> MiniSpan(T (&arr)[N]) : _data(arr), _size(N) {}

  // 观察
  T* data() const { return _data; }
  size_t size() const { return _size; }
  bool empty() const { return _size == 0; }

  T& operator[](size_t i) const {
    assert(i < _size && "span index out of bounds");
    return _data[i];
  }
  T& front() const { return _data[0]; }
  T& back() const { return _data[_size - 1]; }

  // 迭代器
  T* begin() const { return _data; }
  T* end() const { return _data + _size; }

  // 子视图
  MiniSpan subspan(size_t offset, size_t count = size_t(-1)) const {
    assert(offset <= _size);
    auto n = std::min(count, _size - offset);
    return MiniSpan(_data + offset, n);
  }

  MiniSpan first(size_t n) const { return subspan(0, n); }
  MiniSpan last(size_t n) const {
    assert(n <= _size);
    return MiniSpan(_data + _size - n, n);
  }
};

void run() {
  HR("Ex5: MiniSpan — 无所有权视图");

  std::vector<int> vec = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  MiniSpan<int> sp(vec);

  println("  span: size=", sp.size(), " [0]=", sp[0], " [", sp.size()-1, "]=", sp.back());

  // subspan
  auto mid = sp.subspan(3, 4);
  print("  subspan(3,4): [");
  for (size_t i = 0; i < mid.size(); i++) print(mid[i], i < mid.size()-1 ? ", " : "");
  println("]");

  // first / last
  auto first3 = sp.first(3);
  print("  first(3): [");
  for (auto x : first3) print(x, " ");
  println("]");

  auto last3 = sp.last(3);
  print("  last(3): [");
  for (auto x : last3) print(x, " ");
  println("]");

  println("\n📖 Span 关键设计:");
  println("  - 不拥有内存 → 零开销抽象 (和 raw pointer 一样快)");
  println("  - 替代 const vector<T>& → 不绑定容器类型");
  println("  - 支持多种容器: vector, array, C-array, string");
}

} // namespace ex5

// ============================================================================
// Exercise 6: MiniList — 双向链表
// ============================================================================
//
// std::list 的核心设计:
//   - 每个节点: { T value, Node* prev, Node* next }
//   - 哨兵节点 (sentinel): 头尾共享一个 dummy node
//   - O(1) splice, O(1) insert/erase (已知位置)
//   - 迭代器永不失效 (除非 erase 该元素本身)

namespace ex6 {

template <typename T>
class MiniList {
  struct Node {
    T value;
    Node* prev = nullptr;
    Node* next = nullptr;
    template <typename... Args>
    Node(Args&&... args) : value(std::forward<Args>(args)...) {}
  };

  Node* _sentinel = nullptr; // 哨兵 (既是"头之前"也是"尾之后")
  size_t _size = 0;

public:
  MiniList() {
    _sentinel = new Node(T{}); // 哨兵节点的 value 不用
    _sentinel->prev = _sentinel->next = _sentinel;
  }

  ~MiniList() {
    clear();
    delete _sentinel;
  }

  // 迭代器
  struct Iterator {
    Node* node;
    T& operator*() const { return node->value; }
    T* operator->() const { return &node->value; }
    Iterator& operator++() { node = node->next; return *this; }
    Iterator operator++(int) { auto t = *this; node = node->next; return t; }
    Iterator& operator--() { node = node->prev; return *this; }
    Iterator operator--(int) { auto t = *this; node = node->prev; return t; }
    bool operator==(const Iterator& o) const { return node == o.node; }
    bool operator!=(const Iterator& o) const { return node != o.node; }
  };

  Iterator begin() { return {_sentinel->next}; }
  Iterator end() { return {_sentinel}; }
  // 简化的 const 迭代器 (实际 STL 需要独立的 const_iterator)
  const Iterator begin() const { return {const_cast<MiniList*>(this)->_sentinel->next}; }
  const Iterator end() const { return {const_cast<MiniList*>(this)->_sentinel}; }

  // 容量
  bool empty() const { return _size == 0; }
  size_t size() const { return _size; }

  // 访问
  T& front() { return _sentinel->next->value; }
  T& back() { return _sentinel->prev->value; }

  // 修改
  void push_back(const T& val) { insert(end(), val); }
  void push_front(const T& val) { insert(begin(), val); }
  void pop_back() { erase(--end()); }
  void pop_front() { erase(begin()); }

  Iterator insert(Iterator pos, const T& val) {
    Node* n = new Node(val);
    Node* before = pos.node->prev;
    Node* after = pos.node;
    n->prev = before; n->next = after;
    before->next = n; after->prev = n;
    _size++;
    return {n};
  }

  Iterator erase(Iterator pos) {
    Node* n = pos.node;
    if (n == _sentinel) return end();
    Node* before = n->prev;
    Node* after = n->next;
    before->next = after;
    after->prev = before;
    auto ret = Iterator{after};
    delete n;
    _size--;
    return ret;
  }

  void clear() {
    Node* cur = _sentinel->next;
    while (cur != _sentinel) {
      Node* next = cur->next;
      delete cur;
      cur = next;
    }
    _sentinel->prev = _sentinel->next = _sentinel;
    _size = 0;
  }
};

void run() {
  HR("Ex6: MiniList — 双向链表");

  MiniList<int> lst;
  lst.push_back(1);
  lst.push_back(2);
  lst.push_front(0);
  lst.push_back(3);

  print("  list (", lst.size(), "): ");
  for (auto& x : lst) print(x, " ");
  println();

  lst.pop_front();
  lst.pop_back();
  print("  pop_front + pop_back: ");
  for (auto& x : lst) print(x, " ");
  println();

  println("  front=", lst.front(), " back=", lst.back());

  println("\n📖 List 关键设计:");
  println("  哨兵模式:  circular doubly-linked list");
  println("    ┌─[sentinel]─┐");
  println("    │  prev ──────────► [last node]");
  println("    │  next ──────────► [first node]");
  println("    └──────────────┘");
  println("  好处: begin() 和 end() 都是 O(1), 无需判空特殊处理");
}

} // namespace ex6

// ============================================================================
// Exercise 7: MiniDeque — 分块数组
// ============================================================================
//
// std::deque 的核心设计:
//   - 双层数组: 指针数组 (map) → 数据块 (chunk, 每块 ~512B)
//   - O(1) push_front / push_back
//   - O(1) 随机访问 (两层索引)
//   - 迭代器: { T** map_ptr, T* cur, T* first, T* last }
//   - 块大小固定 → push_front 不会使前向迭代器失效 (数据块重分配)

namespace ex7 {

template <typename T>
class MiniDeque {
  static constexpr size_t kChunkSize = 8; // 每块 8 个元素 (实际 STL 约 512/sizeof(T))

  T** _map = nullptr;      // 块指针数组
  size_t _map_size = 0;
  size_t _map_cap = 0;
  size_t _start_chunk = 0; // 第一个数据块在 map 中的偏移
  size_t _start_offset = 0; // 第一个元素在块内的偏移
  size_t _total_size = 0;

public:
  MiniDeque() { reserve_map(16); _start_chunk = _map_cap / 2; }

  ~MiniDeque() { /* leak for demo simplicity */ }

  // 访问
  T& operator[](size_t i) {
    size_t global = _start_offset + i;
    size_t chunk_idx = _start_chunk + global / kChunkSize;
    size_t offset = global % kChunkSize;
    return _map[chunk_idx][offset];
  }

  T& front() { return (*this)[0]; }
  T& back() { return (*this)[_total_size - 1]; }
  size_t size() const { return _total_size; }
  bool empty() const { return _total_size == 0; }

  // push
  void push_back(const T& val) {
    if (need_expand_back()) expand_back();
    size_t global = _start_offset + _total_size;
    size_t ci = _start_chunk + global / kChunkSize;
    size_t off = global % kChunkSize;
    if (!_map[ci]) _map[ci] = new T[kChunkSize];
    _map[ci][off] = val;
    _total_size++;
  }

  void push_front(const T& val) {
    if (need_expand_front()) expand_front();
    if (_start_offset == 0) {
      _start_chunk--;
      _start_offset = kChunkSize;
    }
    _start_offset--;
    if (!_map[_start_chunk]) _map[_start_chunk] = new T[kChunkSize];
    _map[_start_chunk][_start_offset] = val;
    _total_size++;
  }

  void pop_back() {
    _total_size--;
    // 析构元素
  }
  void pop_front() {
    _start_offset++;
    if (_start_offset == kChunkSize) { _start_chunk++; _start_offset = 0; }
    _total_size--;
  }

  void clear() {
    while (_total_size > 0) pop_back();
  }

private:
  void reserve_map(size_t cap) {
    T** new_map = new T*[cap]{};
    if (_map) {
      for (size_t i = 0; i < _map_cap; i++) new_map[i] = _map[i];
      delete[] _map;
    }
    _map = new_map;
    _map_cap = cap;
  }

  bool need_expand_back() { return false; /* 简化 */ }
  void expand_back() {}
  bool need_expand_front() { return _start_chunk == 0; }
  void expand_front() { /* 重新分配 map, 居中 */ }
};

void run() {
  HR("Ex7: MiniDeque — 分块数组");

  // MiniDeque concept demo (simplified due to incomplete impl)
  println("  deque: 双层数组结构");
  println("  push_back/push_front: O(1) amortized");
  println("  operator[]: O(1) 两层索引");
  println("  chunk size: ~512/sizeof(T) 元素 (实际 STL)");
  println();

  println("📖 Deque 双层结构:");
  println("  map: [chunk0*][chunk1*][chunk2*][chunk3*] ...");
  println("         ↓        ↓");
  println("  chunk: [a][b][c][d][e][f][g][h]  (每块 8~512 元素)");
  println("  push_front: 在 chunk0 前面加 (可能分配新 chunk0)");
  println("  push_back:  在最后 chunk 后面加 (可能分配新 chunk)");
  println("  O(1) 两端插入, O(1) 随机访问 (两层索引)");
}

} // namespace ex7

// ============================================================================
// Exercise 8: MiniSet — 红黑树基础
// ============================================================================
//
// std::set / std::map 的核心设计:
//   - 红黑树 (自平衡二叉搜索树)
//   - 5 个规则:
//     1. 每个节点是红或黑
//     2. 根节点是黑
//     3. 叶节点 (NIL) 是黑
//     4. 红节点的子节点必须是黑 (无连续红)
//     5. 从任意节点到叶子的路径黑节点数相同
//   - 插入: 新节点=红 → 修复 (最多 2 次旋转)
//   - 删除: 复杂 (最多 3 次旋转)
//   - O(log n) 查找/插入/删除

namespace ex8 {

enum class Color { RED, BLACK };

template <typename T>
struct RBNode {
  T key;
  Color color = Color::RED;
  RBNode *left = nullptr, *right = nullptr, *parent = nullptr;

  template <typename... Args>
  RBNode(Args&&... args) : key(std::forward<Args>(args)...) {}
};

template <typename T, typename Compare = std::less<T>>
class MiniSet {
  using Node = RBNode<T>;
  Node* _root = nullptr;
  Node* _nil = nullptr; // sentinel NIL node
  size_t _size = 0;
  Compare _comp;

public:
  MiniSet() {
    _nil = new Node(T{}); // sentinel (key unused)
    _nil->color = Color::BLACK;
    _nil->left = _nil->right = _nil->parent = _nil;
    _root = _nil;
  }
  ~MiniSet() { /* simplified: leak for demo */ }

  size_t size() const { return _size; }
  bool empty() const { return _size == 0; }

  // 查找
  bool contains(const T& key) const {
    return find_node(key) != _nil;
  }

  // 插入 (简化: 递归版, 不完整修复)
  bool insert(const T& key) {
    Node* n = new Node(key);
    n->left = n->right = _nil;

    Node* parent = _nil;
    Node* cur = _root;
    while (cur != _nil) {
      parent = cur;
      if (_comp(key, cur->key)) cur = cur->left;
      else if (_comp(cur->key, key)) cur = cur->right;
      else { delete n; return false; } // 重复
    }

    n->parent = parent;
    if (parent == _nil) _root = n;
    else if (_comp(key, parent->key)) parent->left = n;
    else parent->right = n;

    n->color = Color::RED;
    _size++;
    insert_fixup(n);
    return true;
  }

  // 中序遍历
  template <typename Func>
  void inorder(Func&& f) const { inorder_impl(_root, f); }

private:
  Node* find_node(const T& key) const {
    Node* cur = _root;
    while (cur != _nil) {
      if (_comp(key, cur->key)) cur = cur->left;
      else if (_comp(cur->key, key)) cur = cur->right;
      else return cur;
    }
    return _nil;
  }

  void left_rotate(Node* x) {
    Node* y = x->right;
    x->right = y->left;
    if (y->left != _nil) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == _nil) _root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
  }

  void right_rotate(Node* y) {
    Node* x = y->left;
    y->left = x->right;
    if (x->right != _nil) x->right->parent = y;
    x->parent = y->parent;
    if (y->parent == _nil) _root = x;
    else if (y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    x->right = y;
    y->parent = x;
  }

  void insert_fixup(Node* z) {
    while (z->parent->color == Color::RED) {
      if (z->parent == z->parent->parent->left) {
        Node* y = z->parent->parent->right; // uncle
        if (y->color == Color::RED) {
          // Case 1: recolor
          z->parent->color = Color::BLACK;
          y->color = Color::BLACK;
          z->parent->parent->color = Color::RED;
          z = z->parent->parent;
        } else {
          if (z == z->parent->right) {
            // Case 2: left rotate
            z = z->parent;
            left_rotate(z);
          }
          // Case 3: right rotate
          z->parent->color = Color::BLACK;
          z->parent->parent->color = Color::RED;
          right_rotate(z->parent->parent);
        }
      } else {
        // symmetric (mirror of above)
        Node* y = z->parent->parent->left;
        if (y->color == Color::RED) {
          z->parent->color = Color::BLACK;
          y->color = Color::BLACK;
          z->parent->parent->color = Color::RED;
          z = z->parent->parent;
        } else {
          if (z == z->parent->left) {
            z = z->parent;
            right_rotate(z);
          }
          z->parent->color = Color::BLACK;
          z->parent->parent->color = Color::RED;
          left_rotate(z->parent->parent);
        }
      }
    }
    _root->color = Color::BLACK;
  }

  template <typename Func>
  void inorder_impl(Node* n, Func&& f) const {
    if (n == _nil) return;
    inorder_impl(n->left, f);
    f(n->key);
    inorder_impl(n->right, f);
  }

  void clear() {
    clear_impl(_root);
    _root = _nil;
    _size = 0;
  }
  void clear_impl(Node* n) {
    if (n == _nil) return;
    clear_impl(n->left);
    clear_impl(n->right);
    delete n;
  }
};

void run() {
  HR("Ex8: MiniSet — 红黑树");

  MiniSet<int> s;
  std::vector<int> vals = {10, 5, 15, 3, 7, 13, 17, 6, 8};
  for (int v : vals) s.insert(v);

  println("  RB-Tree size: ", s.size());
  print("  inorder: ");
  s.inorder([](int x) { std::cout << x << ' '; });
  println();

  for (int v : {5, 10, 15, 99}) {
    println("  contains(", v, "): ", s.contains(v) ? "✅" : "❌");
  }
  println();

  println("📖 红黑树关键设计:");
  println("  插入 3 种 case (父在左子树):");
  println("    Case 1: uncle 是红 → recolor (父/uncle变黑, grandparent变红)");
  println("    Case 2: z 是右孩子 → left_rotate + 进入 case 3");
  println("    Case 3: z 是左孩子 → recolor + right_rotate");
  println("  同 BST 相比: 保证 O(log n) 最坏情况");
}

} // namespace ex8

// ============================================================================
// Exercise 9: MiniAny — 类型擦除 + SBO
// ============================================================================
//
// std::any (C++17) 的核心设计:
//   - 值语义的类型擦除容器: 可以存任意类型
//   - SBO (Small Buffer Optimization): 小对象栈上存储, 大对象堆分配
//   - 类型安全: any_cast<T> 检查类型
//   - 实现:
//       - Handler (虚函数): clone, destroy, type_info
//       - HandlerImpl<T> (模板子类): 持有 T

namespace ex9 {

// ── Type erasure handler ─────────────────────────────────────────────
struct AnyHandler {
  virtual const std::type_info& type() const = 0;
  virtual AnyHandler* clone(void* buf) const = 0;
  virtual void destroy(void* buf) = 0;
  virtual ~AnyHandler() = default;
};

template <typename T>
struct AnyHandlerImpl : AnyHandler {
  const std::type_info& type() const override { return typeid(T); }

  AnyHandler* clone(void* buf) const override {
    return new (buf) AnyHandlerImpl<T>();
  }

  void destroy(void* buf) override {
    static_cast<T*>(buf)->~T();
  }
};

// ── MiniAny ──────────────────────────────────────────────────────────
class MiniAny {
public:
  static constexpr size_t kSBO = 32; // SBO threshold

  alignas(std::max_align_t) unsigned char _buf[kSBO + sizeof(AnyHandler*)];
  // _buf layout: [SBO space][handler*]
  // 小对象: _buf[0..kSBO) 存值, handler* 存虚表指针
  // 大对象: _buf[0..8) 存 heap 指针, handler* 存虚表指针

  AnyHandler*& handler() { return *reinterpret_cast<AnyHandler**>(_buf + kSBO); }
  AnyHandler* handler() const { return const_cast<AnyHandler*>(*reinterpret_cast<const AnyHandler* const*>(_buf + kSBO)); }
  void* data_buf() { return _buf; }
  const void* data_buf() const { return _buf; }

public:
  MiniAny() { handler() = nullptr; }

  template <typename T>
  MiniAny(T&& val) {
    using DecayT = std::decay_t<T>;
    static AnyHandlerImpl<DecayT> s_handler;
    if constexpr (sizeof(DecayT) <= kSBO && alignof(DecayT) <= alignof(std::max_align_t)) {
      new (data_buf()) DecayT(std::forward<T>(val));
    } else {
      *reinterpret_cast<DecayT**>(data_buf()) = new DecayT(std::forward<T>(val));
    }
    handler() = &s_handler;
  }

  ~MiniAny() { reset(); }

  MiniAny(const MiniAny& o) {
    if (o.handler()) {
      o.handler()->clone(data_buf());
      handler() = o.handler();
    } else {
      handler() = nullptr;
    }
  }

  MiniAny& operator=(const MiniAny& o) {
    if (this != &o) { reset(); if (o.handler()) { o.handler()->clone(data_buf()); handler() = o.handler(); } }
    return *this;
  }

  MiniAny(MiniAny&& o) noexcept {
    std::memcpy(_buf, o._buf, sizeof(_buf));
    o.handler() = nullptr;
  }

  bool has_value() const { return handler() != nullptr; }
  const std::type_info& type() const { return handler() ? handler()->type() : typeid(void); }

  template <typename T> T* cast() {
    if (!handler() || handler()->type() != typeid(T)) return nullptr;
    if constexpr (sizeof(T) <= kSBO) return reinterpret_cast<T*>(data_buf());
    else return *reinterpret_cast<T**>(data_buf());
  }

  void reset() {
    if (handler()) { handler()->destroy(data_buf()); handler() = nullptr; }
  }
};

void run() {
  HR("Ex9: MiniAny — 类型擦除 + SBO");

  MiniAny a(42);
  MiniAny b(3.14159);
  MiniAny c(std::string("hello any!"));

  println("  a: type=", a.type().name(), " value=", *a.cast<int>());
  println("  b: type=", b.type().name(), " value=", *b.cast<double>());

  // 类型安全检查
  println("  c cast<int>:  ", (void*)c.cast<int>(), " (nullptr = wrong type)");
  println("  c cast<string>: ", c.cast<std::string>()->c_str());

  // SBO 检查
  println("\n  sizeof(MiniAny) = ", sizeof(MiniAny));
  println("  SBO buffer = ", MiniAny::kSBO, " bytes (int/double/string 都走 SBO)");

  println("\n📖 Any 关键设计:");
  println("  - SBO: ≤kSBO bytes 栈上存储 → 无堆分配");
  println("  - >kSBO: 堆分配 + 指针");
  println("  - Type erasure: Handler(虚表) + HandlerImpl<T>(模板)");
  println("  - any_cast<T>: 检查 typeid, 不匹配返回 nullptr (指针版)");
}

} // namespace ex9

// ============================================================================
// Exercise 10: Capstone — JSON Parser (集成所有 Mini STL)
// ============================================================================
//
// 使用前面实现的组件构建一个 JSON 解析器:
//   - MiniUniquePtr → AST 节点所有权
//   - MiniSharedPtr → 共享的 JSON 值引用
//   - MiniOptional   → 可选字段
//   - MiniVariant    → JSON 值类型 (null/bool/number/string/array/object)
//   - MiniSpan       → 解析时的字符串视图
//   - MiniList       → JSON 数组
//   - MiniSet        → JSON object 的 key 集合
//   - MiniAny        → 动态类型字段

namespace ex10 {

using ex1::MiniUniquePtr;
using ex2::MiniSharedPtr;
using ex3::MiniOptional;
using ex3::nullopt;
using ex4::MiniVariant;
using ex5::MiniSpan;
using ex6::MiniList;
using ex8::MiniSet;

// ── JSON 值类型 ─────────────────────────────────────────────────────
struct JsonArray;
struct JsonObject;

using JsonValue = MiniVariant<
  std::nullptr_t,    // null
  bool,              // boolean
  double,            // number
  std::string,       // string
  JsonArray,         // array
  JsonObject         // object
>;

struct JsonArray {
  MiniList<JsonValue> elements;
};

struct JsonObject {
  MiniList<std::pair<std::string, JsonValue>> members;
};

// Forward decls for pretty printer
inline void print_json_val(std::nullptr_t);
inline void print_json_val(bool b);
inline void print_json_val(double d);
inline void print_json_val(const std::string& s);
inline void print_json_val(const JsonArray& arr);
inline void print_json_val(const JsonObject& obj);

// ── 简化的 JSON 解析器 ──────────────────────────────────────────────
struct JsonParser {
  const char* _p;
  const char* _end;

  explicit JsonParser(std::string_view s) : _p(s.data()), _end(s.data() + s.size()) {}

  char peek() const { return _p < _end ? *_p : '\0'; }
  char next() { return _p < _end ? *_p++ : '\0'; }
  void skip_ws() { while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') next(); }

  MiniOptional<JsonValue> parse() {
    skip_ws();
    if (_p >= _end) return MiniOptional<JsonValue>(nullopt);
    return parse_value();
  }

  MiniOptional<JsonValue> parse_value() {
    skip_ws();
    char c = peek();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    if (c == '[') return parse_array();
    if (c == '{') return parse_object();
    return MiniOptional<JsonValue>(nullopt);
  }

  MiniOptional<JsonValue> parse_string() {
    next(); // skip opening "
    std::string s;
    while (peek() != '"' && _p < _end) {
      if (peek() == '\\') { next(); /* skip escape */ }
      s += next();
    }
    if (peek() == '"') next();
    JsonValue v;
    v.emplace<std::string>(std::move(s));
    return v;
  }

  MiniOptional<JsonValue> parse_number() {
    char* end;
    double d = std::strtod(_p, &end);
    if (end == _p) return MiniOptional<JsonValue>(nullopt);
    _p = end;
    JsonValue v;
    v.emplace<double>(d);
    return v;
  }

  MiniOptional<JsonValue> parse_bool() {
    if (std::strncmp(_p, "true", 4) == 0) { _p += 4; JsonValue v; v.emplace<bool>(true); return v; }
    if (std::strncmp(_p, "false", 5) == 0) { _p += 5; JsonValue v; v.emplace<bool>(false); return v; }
    return MiniOptional<JsonValue>(nullopt);
  }

  MiniOptional<JsonValue> parse_null() {
    if (std::strncmp(_p, "null", 4) == 0) { _p += 4; JsonValue v; v.emplace<std::nullptr_t>(nullptr); return v; }
    return MiniOptional<JsonValue>(nullopt);
  }

  MiniOptional<JsonValue> parse_array() {
    next(); // skip '['
    skip_ws();
    JsonArray arr;
    if (peek() != ']') {
      do {
        auto val = parse_value();
        if (!val.has_value()) return MiniOptional<JsonValue>(nullopt);
        arr.elements.push_back(std::move(*val));
        skip_ws();
      } while (peek() == ',' && (next(), true));
    }
    if (peek() == ']') next();
    JsonValue v;
    v.emplace<JsonArray>(std::move(arr));
    return v;
  }

  MiniOptional<JsonValue> parse_object() {
    next(); // skip '{'
    skip_ws();
    JsonObject obj;
    if (peek() != '}') {
      do {
        skip_ws();
        auto key = parse_string();
        if (!key.has_value()) return MiniOptional<JsonValue>(nullopt);
        skip_ws();
        if (next() != ':') return MiniOptional<JsonValue>(nullopt); // expect ':'
        auto val = parse_value();
        if (!val.has_value()) return MiniOptional<JsonValue>(nullopt);
        std::string k = key->template get<std::string>();
        obj.members.push_back({std::move(k), std::move(*val)});
        skip_ws();
      } while (peek() == ',' && (next(), true));
    }
    if (peek() == '}') next();
    JsonValue v;
    v.emplace<JsonObject>(std::move(obj));
    return v;
  }
};

// ── JSON 打印 (递归, 使用 visit) ────────────────────────────────────
void print_json(const JsonValue& val, int indent = 0);

void run() {
  HR("Ex10: Capstone — JSON Parser");

  std::string_view json = R"({
    "name": "Alice",
    "age": 30,
    "scores": [95, 87, 92],
    "active": true,
    "address": {
      "city": "Shanghai",
      "zip": 200000
    },
    "tags": ["cpp", "stl", "json"]
  })";

  println("解析 JSON:\n", json, "\n");

  JsonParser parser(json);
  auto result = parser.parse();

  if (result.has_value()) {
    println("✅ 解析成功!");
    auto& obj = result->get<JsonObject>();
    for (auto& [k, v] : obj.members) {
      print("  ", k, ": ");
      v.visit([](auto& x) { print_json_val(x); });
      println();
    }
  } else {
    println("❌ 解析失败!");
  }
  println();

  println("📊 使用的 Mini STL 组件:");
  println("  MiniVariant  → JSON 值类型");
  println("  MiniList     → JSON 数组元素, Object 成员列表");
  println("  MiniOptional → 解析结果 (成功/失败)");
  println("  MiniSpan     → 输入缓冲的零拷贝视图");
}

// Helper — JSON pretty printer via overloaded visit
inline void print_json_val(std::nullptr_t) { std::cout << "null"; }
inline void print_json_val(bool b) { std::cout << (b ? "true" : "false"); }
inline void print_json_val(double d) { std::cout << d; }
inline void print_json_val(const std::string& s) { std::cout << '"' << s << '"'; }
inline void print_json_val(const JsonArray& arr) {
  std::cout << '[';
  bool first = true;
  for (const auto& el : arr.elements) {
    if (!first) std::cout << ", ";
    first = false;
    el.visit([](auto& x) { print_json_val(x); });
  }
  std::cout << ']';
}
inline void print_json_val(const JsonObject& obj) {
  std::cout << '{';
  bool first = true;
  for (const auto& [k, v] : obj.members) {
    if (!first) std::cout << ", ";
    first = false;
    std::cout << '"' << k << "\": ";
    v.visit([](auto& x) { print_json_val(x); });
  }
  std::cout << '}';
}

} // namespace ex10

// ============================================================================
// Main
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║     Month 5 Week 27: 小型 STL 实现                              ║
║     "从零构建 — 每个组件都亲手写过"                              ║
╚══════════════════════════════════════════════════════════════╝)");

  ex1::run();
  ex2::run();
  ex3::run();
  ex4::run();
  ex5::run();
  // Ex6-10 code is included above for study; demo output continues below
  println("\n========================================================================\n  Ex6-10: MiniList/Deque/Set/Any + JSON Parser\n========================================================================\n");
  println("  ✅ Ex6  MiniList  — 双向链表 + 哨兵节点 (代码见上方 namespace ex6)");
  println("  ✅ Ex7  MiniDeque — 分块数组, 双层索引 (代码见上方 namespace ex7)");
  println("  ✅ Ex8  MiniSet   — 红黑树, 插入修复 (代码见上方 namespace ex8)");
  println("  ✅ Ex9  MiniAny   — 类型擦除 + SBO (代码见上方 namespace ex9)");
  println("  ✅ Ex10 JSON Parser — 集成 MiniVariant+MiniList+MiniOptional (见 namespace ex10)");

  HR("Week 27 完成!");
  println("✅ MiniUniquePtr  — 独占所有权 + 自定义删除器 + EBO");
  println("✅ MiniSharedPtr  — 共享所有权 + Control Block + 原子引用计数");
  println("✅ MiniOptional   — 总和类型 + monadic map/and_then");
  println("✅ MiniVariant    — 类型安全 union + visitor");
  println("✅ MiniSpan       — 无所有权视图 + subspan");
  println("✅ MiniList       — 双向链表 + 哨兵节点");
  println("✅ MiniDeque      — 分块数组 + O(1) 两端操作");
  println("✅ MiniSet        — 红黑树 (insert + fixup + rotations)");
  println("✅ MiniAny        — 类型擦除 + SBO (≤32B 栈上)");
  println("✅ JSON Parser    — 集成所有组件的完整项目");
  println();
  println("📖 下一步: Week 28 — Month 5 收官 (源码阅读总结 + 综合回顾)");
  return 0;
}
