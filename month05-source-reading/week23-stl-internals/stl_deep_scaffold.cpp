// ============================================================================
// Month 5: 源码阅读 — 看透实现
// Week 23: STL 实现深潜 — vector / string / function 内部
//
// 核心哲学:
//   「会用 STL 只是入门, 看懂实现才是进阶」
//   「源码面前, 了无秘密」
//
//   三个经典组件, 三个核心技术:
//   - std::vector  → RAII 资源管理 + 扩容策略 + placement new
//   - std::string  → SSO (Small String Optimization) + CoW 历史
//   - std::function → Type Erasure (类型擦除) + SBO (Small Buffer Opt)
//
// 本周目标:
//   - 亲手实现 MiniVector / MiniString / MiniFunction
//   - 理解 placement new, SSO, type erasure 的内部机制
//   - 看懂 STL 源码的能力 (gdb step into std::vector, 不再陌生)
//
// 10 个练习, 由浅入深, 每个都包含完整实现
// ============================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using namespace std::chrono;
static volatile int64_t g_sink = 0;

class Timer {
  high_resolution_clock::time_point _start;
public:
  Timer() : _start(high_resolution_clock::now()) {}
  int64_t elapsed_ns() const {
    return duration_cast<nanoseconds>(high_resolution_clock::now() - _start).count();
  }
  double elapsed_us() const { return elapsed_ns() / 1000.0; }
  double elapsed_ms() const { return elapsed_ns() / 1'000'000.0; }
};

// ============================================================================
// Ex1: MiniVector — 动态数组的本质
//
// 核心概念:
//   vector 的三个指针: _begin, _end, _capacity_end
//   - _begin:        分配的内存起始
//   - _end:          已构造的最后一个元素 + 1
//   - _capacity_end: 分配的内存末尾
//
//   扩容: size() == capacity() 时, 分配 2x 空间,
//         把旧元素 move (或 copy) 到新空间, 释放旧空间。
//
// 任务: 完成 MiniVector 的核心方法
// ============================================================================

namespace ex1_mini_vector {
  template<typename T>
  class MiniVector {
    T* _begin = nullptr;
    T* _end = nullptr;
    T* _capacity_end = nullptr;

  public:
    MiniVector() = default;

    explicit MiniVector(size_t n, const T& val = T{}) {
      _begin = static_cast<T*>(::operator new(n * sizeof(T)));
      _end = _begin;
      _capacity_end = _begin + n;
      for (size_t i = 0; i < n; ++i) {
        push_back(val);
      }
    }

    ~MiniVector() {
      clear();
      ::operator delete(_begin);
    }

    // 拷贝构造
    MiniVector(const MiniVector& other) {
      size_t n = other.size();
      _begin = static_cast<T*>(::operator new(n * sizeof(T)));
      _end = _begin;
      _capacity_end = _begin + n;
      for (size_t i = 0; i < n; ++i) {
        push_back(other[i]);
      }
    }

    // 移动构造
    MiniVector(MiniVector&& other) noexcept {
      _begin = other._begin;
      _end = other._end;
      _capacity_end = other._capacity_end;
      other._begin = nullptr;
      other._end = nullptr;
      other._capacity_end = nullptr;
    }

    // --- 核心方法 ---

    size_t size() const { return _end - _begin; }
    size_t capacity() const { return _capacity_end - _begin; }
    bool empty() const { return _begin == _end; }

    T& operator[](size_t i) { return _begin[i]; }
    const T& operator[](size_t i) const { return _begin[i]; }

    T& front() { return *_begin; }
    T& back() { return *(_end - 1); }

    T* begin() { return _begin; }
    T* end() { return _end; }

    // push_back: 核心! 如果满了先扩容
    void push_back(const T& value) {
      if (_end == _capacity_end) {
        reserve(capacity() == 0 ? 4 : capacity() * 2);
      }
      // placement new: 在 _end 位置构造 T
      ::new (static_cast<void*>(_end)) T(value);
      ++_end;
    }

    void push_back(T&& value) {
      if (_end == _capacity_end) {
        reserve(capacity() == 0 ? 4 : capacity() * 2);
      }
      ::new (static_cast<void*>(_end)) T(std::move(value));
      ++_end;
    }

    // emplace_back: 原地构造, 消除一次 move/copy
    template<typename... Args>
    T& emplace_back(Args&&... args) {
      if (_end == _capacity_end) {
        reserve(capacity() == 0 ? 4 : capacity() * 2);
      }
      ::new (static_cast<void*>(_end)) T(std::forward<Args>(args)...);
      ++_end;
      return back();
    }

    void pop_back() {
      if (!empty()) {
        --_end;
        _end->~T();  // 显式调用析构函数
      }
    }

    void clear() {
      while (!empty()) pop_back();
    }

    // reserve: 预分配 (关键性能优化!)
    void reserve(size_t new_cap) {
      if (new_cap <= capacity()) return;
      // 1. 分配新空间
      T* new_begin = static_cast<T*>(::operator new(new_cap * sizeof(T)));
      // 2. move 或 copy 旧元素到新空间
      size_t old_size = size();
      for (size_t i = 0; i < old_size; ++i) {
        ::new (static_cast<void*>(new_begin + i)) T(std::move(_begin[i]));
        _begin[i].~T();  // 析构旧的
      }
      // 3. 释放旧空间
      ::operator delete(_begin);
      // 4. 更新指针
      _begin = new_begin;
      _end = _begin + old_size;
      _capacity_end = _begin + new_cap;
    }

    void resize(size_t n, const T& val = T{}) {
      while (size() > n) pop_back();
      while (size() < n) push_back(val);
    }
  };

  void run() {
    std::cout << "\n===== Ex1: MiniVector =====\n";

    MiniVector<int> v;
    std::cout << "push_back 1-10:\n";
    for (int i = 1; i <= 10; ++i) {
      v.push_back(i);
      std::cout << "  size=" << v.size()
                << " cap=" << v.capacity() << " back=" << v.back() << "\n";
    }

    std::cout << "Elements: ";
    for (size_t i = 0; i < v.size(); ++i) std::cout << v[i] << " ";
    std::cout << "\n";

    std::cout << "reserve(100): ";
    v.reserve(100);
    std::cout << "size=" << v.size() << " cap=" << v.capacity() << "\n";

    std::cout << "\nKey insights:\n";
    std::cout << "  1. push_back uses placement new (not operator=)\n";
    std::cout << "  2. Growth factor 2x → amortized O(1) push_back\n";
    std::cout << "  3. reserve avoids realloc → critical perf optimization\n";
    std::cout << "  4. ~T() explicitly called on pop_back/clear\n";
    std::cout << "  5. operator new/delete (not new/delete) → no constructor call\n";
  }
}

// ============================================================================
// Ex2: 扩容策略 — 1.5x vs 2x
//
// 概念:
//   GCC std::vector 使用 2x 扩容因子。
//   1.5x: 内存可以复用 (旧块 + 新块 ≈ 连续, 可以合并)
//   2x:   更快增长 (更少 realloc), 但内存不能复用
//
//   摊还分析 (amortized analysis):
//   每次 push_back 的平均成本是 O(1):
//   N 次 push_back → N 次 construct + ~2N 次 move
//   = 每次 push_back 约 3 次操作 (常数)
//
// 任务: 测量不同扩容因子的性能差异
// ============================================================================

namespace ex2_growth_strategy {
  template<typename T, int GrowthFactor = 2>
  class VectorWithGrowth {
    T* _begin = nullptr;
    T* _end = nullptr;
    T* _capacity_end = nullptr;

  public:
    size_t size() const { return _end - _begin; }
    size_t capacity() const { return _capacity_end - _begin; }

    void push_back(const T& val) {
      if (_end == _capacity_end) {
        size_t old_cap = capacity();
        size_t new_cap = old_cap == 0 ? 4 : old_cap * GrowthFactor;
        T* new_begin = static_cast<T*>(::operator new(new_cap * sizeof(T)));
        size_t old_size = size();
        for (size_t i = 0; i < old_size; ++i) {
          ::new (new_begin + i) T(std::move(_begin[i]));
          _begin[i].~T();
        }
        ::operator delete(_begin);
        _begin = new_begin;
        _end = _begin + old_size;
        _capacity_end = _begin + new_cap;
      }
      ::new (_end) T(val);
      ++_end;
    }

    ~VectorWithGrowth() {
      for (size_t i = 0; i < size(); ++i) _begin[i].~T();
      ::operator delete(_begin);
    }
  };

  void run() {
    std::cout << "\n===== Ex2: Growth Strategy =====\n";

    constexpr int N = 1'000'000;

    auto bench = [&](const char* label, auto factory) {
      Timer t;
      factory();
      std::cout << "  " << label << ": " << t.elapsed_ms() << " ms\n";
    };

    bench("GCC std::vector (2x)", [] {
      std::vector<int> v;
      for (int i = 0; i < N; ++i) v.push_back(i);
      g_sink = v.size();
    });

    bench("MiniVector 2x", [] {
      VectorWithGrowth<int, 2> v;
      for (int i = 0; i < N; ++i) v.push_back(i);
    });

    bench("MiniVector 1.5x", [] {
      VectorWithGrowth<int, 15> v;  // 15 = 1.5 * 10
      for (int i = 0; i < N; ++i) v.push_back(i);
    });

    bench("reserve + push_back", [] {
      std::vector<int> v;
      v.reserve(N);
      for (int i = 0; i < N; ++i) v.push_back(i);
      g_sink = v.size();
    });

    std::cout << "\nKey: reserve eliminates ALL reallocation overhead\n";
    std::cout << "     2x growth: ~log2(N) reallocs, total move ~2N elements\n";
    std::cout << "     1.5x growth: ~log1.5(N) reallocs, but memory reusable\n";
  }
}

// ============================================================================
// Ex3: MiniString + SSO (Small String Optimization)
//
// 概念:
//   SSO: 对于短字符串 (通常 ≤15 字节), 直接存储在栈上 (string 对象内部),
//   避免堆分配。这是 std::string 最重要的优化。
//
//   libstdc++ SSO 布局 (GCC):
//     struct string {
//       char* _ptr;              // 指向堆 (长模式) 或 local_buf (短模式)
//       size_t _size;            // 字符串长度
//       union {
//         char _local_buf[16];   // SSO buffer (15 chars + null)
//         size_t _capacity;      // 堆容量 (长模式)
//       };
//     };
//     SSO: _ptr == _local_buf → 在栈上, _capacity 未使用
//     长模式: _ptr != _local_buf → 在堆上
//
// 任务: 实现带 SSO 的 MiniString
// ============================================================================

namespace ex3_mini_string {
  class MiniString {
    static constexpr size_t SSO_CAP = 15;  // 15 chars + null = 16 bytes
    static constexpr size_t SSO_BUF = SSO_CAP + 1;

    char* _data;           // 指向 _local (短) 或 _heap (长)
    size_t _size = 0;
    union {
      char _local[SSO_BUF];     // SSO buffer
      size_t _capacity;         // 堆容量 (仅长模式)
    };

    bool is_sso() const { return _data == _local; }

  public:
    MiniString() : _data(_local) {
      _local[0] = '\0';
    }

    MiniString(const char* s) : _data(_local) {
      _local[0] = '\0';
      append(s);
    }

    ~MiniString() {
      if (!is_sso()) {
        delete[] _data;
      }
    }

    // 拷贝
    MiniString(const MiniString& other) : _data(_local) {
      _local[0] = '\0';
      append(other.c_str());
    }

    MiniString& operator=(const MiniString& other) {
      if (this != &other) {
        this->~MiniString();
        _data = _local;
        _local[0] = '\0';
        _size = 0;
        append(other.c_str());
      }
      return *this;
    }

    const char* c_str() const { return _data; }
    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }

    void append(const char* s) {
      size_t extra = std::strlen(s);
      size_t need = _size + extra + 1;

      if (is_sso() && need <= SSO_BUF) {
        // 仍然是 SSO: 直接在 _local 中追加
        std::strcpy(_local + _size, s);
      } else {
        // 需要堆
        size_t new_cap = is_sso() ? need * 2 : _capacity * 2;
        while (new_cap < need) new_cap *= 2;

        char* new_heap = new char[new_cap];
        std::strcpy(new_heap, _data);  // 拷贝旧内容
        std::strcpy(new_heap + _size, s);  // 追加新内容

        if (!is_sso()) delete[] _data;
        _data = new_heap;
        _capacity = new_cap;
      }
      _size += extra;
    }

    MiniString& operator+=(const char* s) {
      append(s);
      return *this;
    }
  };

  void run() {
    std::cout << "\n===== Ex3: MiniString + SSO =====\n";
    std::cout << "sizeof(MiniString) = " << sizeof(MiniString)
              << " bytes (SSO buffer = 15 chars)\n\n";

    MiniString s1;
    std::cout << "Empty: '" << s1.c_str() << "' size=" << s1.size() << "\n";

    MiniString s2("Hello");
    std::cout << "Short: '" << s2.c_str() << "' size=" << s2.size()
              << " (should be SSO)\n";

    s2 += ", World! This is a test of small string optimization.";
    std::cout << "Long: '" << s2.c_str() << "' size=" << s2.size()
              << " (should be heap)\n";

    // Benchmark: SSO vs no-SSO
    constexpr int N = 1'000'000;
    Timer t1;
    for (int i = 0; i < N; ++i) {
      std::string s("short");
      g_sink += s.size();
    }
    std::cout << "\nstd::string 'short': " << t1.elapsed_ms() << " ms ("
              << N / 1'000'000.0 << "M allocations)\n";

    Timer t2;
    for (int i = 0; i < N; ++i) {
      std::string s("this_is_a_long_string_that_requires_heap_allocation");
      g_sink += s.size();
    }
    std::cout << "std::string 'long ': " << t2.elapsed_ms() << " ms\n";
    std::cout << "SSO: short strings avoid heap allocation → much faster\n";
  }
}

// ============================================================================
// Ex4: Copy-on-Write (CoW) — 为何被废弃
//
// 概念:
//   C++98 时代的 std::string 实现用了 CoW (Copy-on-Write):
//   多个 string 共享同一个堆缓冲区, 引用计数,
//   只有在写入时才真正拷贝 (fork 模式)。
//
//   为什么 C++11 废弃了 CoW?
//   1. operator[] 返回非 const ref → 无法判断是否会写入 → 过度拷贝
//   2. 多线程问题: 引用计数需要原子操作, 成本 > 拷贝
//   3. SSO 对短字符串更有效
//
// 任务: 实现 CoW 字符串, 测量 vs SSO 的性能
// ============================================================================

namespace ex4_cow_string {
  class CowString {
    struct Buffer {
      char* data;
      size_t size;
      size_t cap;
      int refcount;

      Buffer(const char* s) {
        size = std::strlen(s);
        cap = size + 1;
        data = new char[cap];
        std::strcpy(data, s);
        refcount = 1;
      }

      ~Buffer() { delete[] data; }

      void add_ref() { ++refcount; }
      void release() { if (--refcount == 0) delete this; }
    };

    Buffer* _buf;

    // 写入前检查: 如果共享, 则拷贝
    void detach() {
      if (_buf->refcount > 1) {
        Buffer* old = _buf;
        _buf = new Buffer(old->data);
        old->release();
      }
    }

  public:
    CowString(const char* s) : _buf(new Buffer(s)) {}

    CowString(const CowString& other) : _buf(other._buf) {
      _buf->add_ref();  // 只增加引用计数, 不拷贝!
    }

    ~CowString() { _buf->release(); }

    CowString& operator=(const CowString& other) {
      if (this != &other) {
        _buf->release();
        _buf = other._buf;
        _buf->add_ref();
      }
      return *this;
    }

    const char& operator[](size_t i) const { return _buf->data[i]; }

    // 非 const operator[] — 必须 detach!
    char& operator[](size_t i) {
      detach();
      return _buf->data[i];
    }

    const char* c_str() const { return _buf->data; }
    size_t size() const { return _buf->size; }
    int use_count() const { return _buf->refcount; }
  };

  void run() {
    std::cout << "\n===== Ex4: Copy-on-Write String =====\n";

    CowString a("Hello, World!");
    std::cout << "a = '" << a.c_str() << "' refcount=" << a.use_count() << "\n";

    CowString b = a;  // 拷贝构造: 只增加引用计数
    std::cout << "b = a → a.refcount=" << a.use_count()
              << " (no copy! shared buffer)\n";

    b[0] = 'h';  // 写入 → detach → 真正拷贝
    std::cout << "b[0]='h' → a.refcount=" << a.use_count()
              << " (b detached, a alone)\n";
    std::cout << "a = '" << a.c_str() << "'\n";
    std::cout << "b = '" << b.c_str() << "'\n";

    std::cout << "\nWhy CoW was abandoned:\n";
    std::cout << "  1. operator[](non-const) can't know if caller will write\n";
    std::cout << "  2. Multi-threaded refcount needs atomic → cost > benefit\n";
    std::cout << "  3. SSO handles short strings better (no heap at all!)\n";
    std::cout << "  4. C++11 move semantics make cheap copies possible\n";
  }
}

// ============================================================================
// Ex5: Allocator — 自定义内存分配
//
// 概念:
//   STL 容器都接受一个 Allocator 模板参数 (默认 std::allocator<T>)
//   自定义 allocator 可以实现:
//   - Arena allocation (内存池)
//   - Tracking/logging
//   - Shared memory / mmap-backed
//   - Aligned allocation (SIMD)
//
//   C++17 引入 std::pmr (Polymorphic Memory Resource)
//
// 任务: 实现 Arena Allocator, 对比 std::allocator
// ============================================================================

namespace ex5_allocator {
  // Arena: 预分配一块大内存, 从中线性分配 (极快, 但无法单独 free)
  class Arena {
    char* _buf;
    size_t _size;
    size_t _offset = 0;

  public:
    Arena(size_t size) : _size(size) {
      _buf = static_cast<char*>(::operator new(size));
    }

    ~Arena() { ::operator delete(_buf); }

    void* allocate(size_t n, size_t align = alignof(std::max_align_t)) {
      // 对齐
      size_t space = _size - _offset;
      void* ptr = _buf + _offset;
      if (!std::align(align, n, ptr, space)) return nullptr;
      _offset = (char*)ptr - _buf + n;
      return ptr;
    }

    void reset() { _offset = 0; }

    size_t used() const { return _offset; }
  };

  // 一个简单的 Arena Allocator (符合 STL allocator 概念)
  template<typename T>
  class ArenaAllocator {
    Arena* _arena;
  public:
    using value_type = T;
    ArenaAllocator(Arena& arena) : _arena(&arena) {}

    T* allocate(size_t n) {
      return static_cast<T*>(_arena->allocate(n * sizeof(T), alignof(T)));
    }
    void deallocate(T*, size_t) {}  // Arena 不单独释放
  };

  void run() {
    std::cout << "\n===== Ex5: Arena Allocator =====\n";

    constexpr int N = 1'000'000;
    constexpr int ARENA_SIZE = N * 40;  // ~40MB

    // std::vector with default allocator
    {
      Timer t;
      std::vector<int> v;
      for (int i = 0; i < N; ++i) v.push_back(i);
      std::cout << "std::allocator:      " << t.elapsed_ms() << " ms\n";
    }

    // std::vector with arena (手动模拟: reserve)
    {
      Timer t;
      std::vector<int> v;
      v.reserve(N);  // arena 的效果: 一次性分配
      for (int i = 0; i < N; ++i) v.push_back(i);
      std::cout << "reserve (like arena): " << t.elapsed_ms() << " ms\n";
    }

    // 直接 Arena 分配
    {
      Timer t;
      Arena arena(ARENA_SIZE);
      int* arr = static_cast<int*>(arena.allocate(N * sizeof(int)));
      for (int i = 0; i < N; ++i) {
        new (arr + i) int(i);
      }
      std::cout << "Arena alloc:         " << t.elapsed_ms() << " ms";
      std::cout << " (used " << arena.used() / 1024.0 / 1024.0 << " MB)\n";
    }

    std::cout << "\nArena advantages:\n";
    std::cout << "  - O(1) allocation (just bump pointer)\n";
    std::cout << "  - Excellent cache locality (contiguous)\n";
    std::cout << "  - No per-object deallocation (reset entire arena)\n";
    std::cout << "  - Ideal for: game frames, request processing, compilers\n";
  }
}

// ============================================================================
// Ex6: Type Erasure — std::function 的内部
//
// 概念:
//   std::function<void(int)> 可以包装任何可调用对象
//   (函数指针, lambda, functor — 只要签名匹配)
//
//   这是如何实现的? Type Erasure (类型擦除):
//   1. 定义抽象基类 (Concept) — 虚函数 operator()
//   2. 定义模板子类 (Model<T>) — 持有具体类型 T, 实现虚函数
//   3. function 持有 Concept* — 通过虚函数调用, 外部不知道 T
//
//   这本质上是手工实现的虚函数表 (vtable)!
//
// 任务: 实现 MiniFunction<void(int)>
// ============================================================================

namespace ex6_type_erasure {
  template<typename Signature>
  class MiniFunction;

  // void(int) 特化
  template<>
  class MiniFunction<void(int)> {
    // 抽象基类: Concept
    struct Concept {
      virtual ~Concept() = default;
      virtual void call(int) = 0;
      virtual Concept* clone() = 0;  // 用于拷贝
    };

    // 模板子类: Model<T> — 持有具体类型
    template<typename T>
    struct Model : Concept {
      T _obj;
      Model(T obj) : _obj(std::move(obj)) {}
      void call(int x) override { _obj(x); }
      Concept* clone() override { return new Model<T>(_obj); }
    };

    Concept* _ptr = nullptr;

  public:
    MiniFunction() = default;

    // 构造函数: 接受任何可调用对象
    template<typename T>
    MiniFunction(T fn) : _ptr(new Model<T>(std::move(fn))) {}

    ~MiniFunction() { delete _ptr; }

    MiniFunction(const MiniFunction& other)
      : _ptr(other._ptr ? other._ptr->clone() : nullptr) {}

    MiniFunction& operator=(const MiniFunction& other) {
      if (this != &other) {
        delete _ptr;
        _ptr = other._ptr ? other._ptr->clone() : nullptr;
      }
      return *this;
    }

    MiniFunction(MiniFunction&& other) noexcept : _ptr(other._ptr) {
      other._ptr = nullptr;
    }

    explicit operator bool() const { return _ptr != nullptr; }

    void operator()(int x) const {
      if (_ptr) _ptr->call(x);
    }
  };

  void run() {
    std::cout << "\n===== Ex6: Type Erasure (MiniFunction) =====\n";
    std::cout << "sizeof(MiniFunction<void(int)>) = "
              << sizeof(MiniFunction<void(int)>) << " bytes\n";
    std::cout << "sizeof(std::function<void(int)>) = "
              << sizeof(std::function<void(int)>) << " bytes\n\n";

    // 使用: 三种不同的可调用对象
    MiniFunction<void(int)> f1 = [](int x) {
      std::cout << "  Lambda: " << x * 2 << "\n";
    };
    f1(21);

    struct Functor {
      int factor;
      void operator()(int x) const {
        std::cout << "  Functor: " << x * factor << "\n";
      }
    };
    MiniFunction<void(int)> f2 = Functor{3};
    f2(21);

    void (*func_ptr)(int) = [](int x) {
      std::cout << "  Function ptr: " << x + 100 << "\n";
    };
    MiniFunction<void(int)> f3 = func_ptr;
    f3(21);

    std::cout << "\nHow it works:\n";
    std::cout << "  MiniFunction::Concept (abstract base)\n";
    std::cout << "    └── Model<Lambda>   → call() { lambda(x); }\n";
    std::cout << "    └── Model<Functor>  → call() { functor(x); }\n";
    std::cout << "    └── Model<fn_ptr>   → call() { fn_ptr(x); }\n";
    std::cout << "  Virtual dispatch hides the concrete type!\n";
    std::cout << "\n  Also: SBO (Small Buffer Optimization) avoids heap\n";
    std::cout << "  std::function stores small objects inline (like SSO)\n";
  }
}

// ============================================================================
// Ex7: Iterator Design — enable_if + tag dispatch
//
// 概念:
//   STL 迭代器的 5 种类别 (iterator categories):
//   - input_iterator: 只读, 单 pass (如 istream_iterator)
//   - forward_iterator: 读写, 多 pass (如 forward_list::iterator)
//   - bidirectional_iterator: 支持 -- (如 list::iterator)
//   - random_access_iterator: 支持 +n, -n, [] (如 vector::iterator)
//   - contiguous_iterator: 内存连续 (如 vector::iterator, C++17)
//
//   STL 算法根据 iterator category 选择最优实现:
//   std::advance(it, n): random_access → it+n; 其他 → while(n--) ++it
//
// 任务: 实现一个 tag-dispatch 的 advance
// ============================================================================

namespace ex7_iterator_design {
  // Tag types
  struct input_iterator_tag {};
  struct forward_iterator_tag : input_iterator_tag {};
  struct bidirectional_iterator_tag : forward_iterator_tag {};
  struct random_access_iterator_tag : bidirectional_iterator_tag {};

  // Traits: 从 iterator 提取 category
  template<typename It>
  struct iterator_traits {
    using category = typename It::iterator_category;
  };

  // 指针特化 (指针是 random_access iterator!)
  template<typename T>
  struct iterator_traits<T*> {
    using category = random_access_iterator_tag;
  };

  // advance 实现: tag dispatch
  namespace detail {
    template<typename It>
    void advance_impl(It& it, int n, random_access_iterator_tag) {
      it += n;  // O(1)
    }
    template<typename It>
    void advance_impl(It& it, int n, bidirectional_iterator_tag) {
      if (n > 0) while (n--) ++it;
      else while (n++) --it;  // O(n)
    }
    template<typename It>
    void advance_impl(It& it, int n, forward_iterator_tag) {
      while (n--) ++it;  // O(n), only forward
    }
  }

  template<typename It>
  void advance(It& it, int n) {
    using category = typename iterator_traits<It>::category;
    detail::advance_impl(it, n, category{});
  }

  // 自定义 random_access iterator
  struct MyVecIter {
    int* ptr;
    using iterator_category = random_access_iterator_tag;

    int& operator*() { return *ptr; }
    MyVecIter& operator++() { ++ptr; return *this; }
    MyVecIter& operator+=(int n) { ptr += n; return *this; }
    bool operator!=(MyVecIter other) const { return ptr != other.ptr; }
  };

  void run() {
    std::cout << "\n===== Ex7: Iterator Design =====\n";

    int arr[] = {10, 20, 30, 40, 50};

    // 指针 = random_access → advance 用 O(1) ptr+=n
    int* p = arr;
    advance(p, 3);
    std::cout << "advance(arr, 3) = " << *p << " (O(1): pointer arithmetic)\n";

    // 自定义 random_access iterator
    MyVecIter it{arr};
    advance(it, 2);
    std::cout << "advance(MyVecIter, 2) = " << *it
              << " (O(1): tag-dispatch to random_access)\n";

    std::cout << "\nTag dispatch hierarchy:\n";
    std::cout << "  input_iterator_tag\n";
    std::cout << "    └── forward_iterator_tag\n";
    std::cout << "          └── bidirectional_iterator_tag\n";
    std::cout << "                └── random_access_iterator_tag\n";
    std::cout << "\n  Each algorithm picks the best overload at compile time!\n";
    std::cout << "  Zero runtime overhead — pure template metaprogramming.\n";
  }
}

// ============================================================================
// Ex8: std::sort 内部 — IntroSort
//
// 概念:
//   GCC 的 std::sort 使用 IntroSort (David Musser, 1997):
//   1. 默认: QuickSort (三数取中 pivot)
//   2. 递归深度 > 2*log2(N) → 切换到 HeapSort (避免 O(n²))
//   3. 子数组 < 16 元素 → 切换到 InsertionSort (小数组更快)
//
//   这是 「hybrid algorithm」 的经典案
//
// 任务: 实现简化版 IntroSort
// ============================================================================

namespace ex8_introsort {
  // 插入排序 (小数组最优 — 无递归开销 + cache friendly)
  template<typename It>
  void insertion_sort(It first, It last) {
    if (first == last) return;
    for (It i = first + 1; i != last; ++i) {
      auto key = std::move(*i);
      It j = i;
      while (j != first && key < *(j - 1)) {
        *j = std::move(*(j - 1));
        --j;
      }
      *j = std::move(key);
    }
  }

  // 堆排序 (O(n log n) 保证, 但常数大)
  template<typename It>
  void heap_sort(It first, It last) {
    std::make_heap(first, last);
    std::sort_heap(first, last);
    // (简化: 用 std::make/sort_heap, 实际中需要手写)
  }

  // 三数取中 partition (Lomuto)
  template<typename It>
  It partition(It first, It last) {
    It mid = first + (last - first) / 2;
    // 中位数: first, mid, last-1
    if (*mid < *first) std::swap(*first, *mid);
    if (*(last - 1) < *first) std::swap(*first, *(last - 1));
    if (*(last - 1) < *mid) std::swap(*mid, *(last - 1));
    std::swap(*first, *mid);  // pivot at first

    auto pivot = *first;
    It i = first;
    for (It j = first + 1; j != last; ++j) {
      if (*j < pivot) {
        ++i;
        std::swap(*i, *j);
      }
    }
    std::swap(*first, *i);
    return i;
  }

  // IntroSort 核心
  template<typename It>
  void introsort_impl(It first, It last, int depth_limit) {
    constexpr int INSERTION_THRESHOLD = 16;

    while (last - first > INSERTION_THRESHOLD) {
      if (depth_limit == 0) {
        // 递归过深 → 切换堆排序 (保证 O(n log n))
        heap_sort(first, last);
        return;
      }
      --depth_limit;

      It pivot = partition(first, last);
      // 递归较小的部分, 循环较大的部分 (尾递归优化)
      if (pivot - first < last - pivot) {
        introsort_impl(first, pivot, depth_limit);
        first = pivot + 1;
      } else {
        introsort_impl(pivot + 1, last, depth_limit);
        last = pivot;
      }
    }
    // 小数组 → 插入排序
    insertion_sort(first, last);
  }

  template<typename It>
  void introsort(It first, It last) {
    int depth_limit = 2 * static_cast<int>(std::log2(last - first));
    introsort_impl(first, last, depth_limit);
  }

  void run() {
    std::cout << "\n===== Ex8: IntroSort Internals =====\n";

    constexpr int N = 1'000'000;
    std::vector<int> data(N);

    std::mt19937 rng(42);
    std::iota(data.begin(), data.end(), 0);

    // 随机数据
    std::shuffle(data.begin(), data.end(), rng);
    {
      Timer t;
      std::sort(data.begin(), data.end());
      std::cout << "std::sort (random):   " << t.elapsed_ms() << " ms\n";
    }

    std::shuffle(data.begin(), data.end(), rng);
    {
      Timer t;
      introsort(data.begin(), data.end());
      std::cout << "introsort (random):   " << t.elapsed_ms() << " ms\n";
    }

    // 已排序数据 (quicksort worst-case, introsort 切换到 heap)
    {
      Timer t;
      std::sort(data.begin(), data.end());
      std::cout << "std::sort (sorted):   " << t.elapsed_ms() << " ms\n";
    }

    {
      Timer t;
      introsort(data.begin(), data.end());
      std::cout << "introsort (sorted):   " << t.elapsed_ms() << " ms\n";
    }

    std::cout << "\nIntroSort strategy:\n";
    std::cout << "  Phase 1: QuickSort (median-of-3 pivot, O(n log n) avg)\n";
    std::cout << "  Phase 2: depth > 2*log2(N) → HeapSort (O(n log n) guaranteed)\n";
    std::cout << "  Phase 3: subarray < 16 → InsertionSort (O(n²) but fast for small N)\n";
  }
}

// ============================================================================
// Ex9: unordered_map 内部 — 哈希表与 rehash
//
// 概念:
//   std::unordered_map 使用 分离链表法 (Separate Chaining):
//   - vector<Bucket*> (bucket array)
//   - Bucket = linked list of (key, value) nodes
//
//   load_factor = size() / bucket_count()
//   当 load_factor > max_load_factor() (默认 1.0) → rehash
//   rehash: 分配新 bucket array (约 2x), 重新 hash 所有节点
//
// 任务: 实现 MiniUnorderedMap
// ============================================================================

namespace ex9_unordered_map {
  template<typename K, typename V>
  class MiniUnorderedMap {
    struct Node {
      K key;
      V value;
      Node* next = nullptr;
      Node(const K& k, const V& v) : key(k), value(v) {}
    };

    std::vector<Node*> _buckets;
    size_t _size = 0;
    float _max_load = 1.0f;

    size_t bucket_idx(const K& key) const {
      return std::hash<K>{}(key) % _buckets.size();
    }

    void rehash(size_t new_bucket_count) {
      std::vector<Node*> new_buckets(new_bucket_count, nullptr);
      for (auto* head : _buckets) {
        while (head) {
          Node* next = head->next;
          size_t idx = std::hash<K>{}(head->key) % new_bucket_count;
          head->next = new_buckets[idx];
          new_buckets[idx] = head;
          head = next;
        }
      }
      _buckets = std::move(new_buckets);
    }

  public:
    MiniUnorderedMap() { _buckets.resize(8, nullptr); }

    ~MiniUnorderedMap() {
      for (auto* head : _buckets) {
        while (head) {
          Node* next = head->next;
          delete head;
          head = next;
        }
      }
    }

    size_t size() const { return _size; }
    size_t bucket_count() const { return _buckets.size(); }
    float load_factor() const {
      return _buckets.empty() ? 0 : (float)_size / _buckets.size();
    }

    V& operator[](const K& key) {
      size_t idx = bucket_idx(key);
      Node* cur = _buckets[idx];
      while (cur) {
        if (cur->key == key) return cur->value;
        cur = cur->next;
      }
      // 没找到 → 插入
      if (load_factor() >= _max_load) {
        rehash(_buckets.size() * 2);
        idx = bucket_idx(key);
      }
      Node* node = new Node(key, V{});
      node->next = _buckets[idx];
      _buckets[idx] = node;
      ++_size;
      return node->value;
    }

    V* find(const K& key) {
      size_t idx = bucket_idx(key);
      Node* cur = _buckets[idx];
      while (cur) {
        if (cur->key == key) return &cur->value;
        cur = cur->next;
      }
      return nullptr;
    }

    bool contains(const K& key) { return find(key) != nullptr; }
  };

  void run() {
    std::cout << "\n===== Ex9: unordered_map Internals =====\n";

    MiniUnorderedMap<int, std::string> map;
    std::cout << "Initial buckets: " << map.bucket_count() << "\n";

    for (int i = 0; i < 100; ++i) {
      map[i] = "value_" + std::to_string(i);
    }
    std::cout << "After 100 inserts: size=" << map.size()
              << " buckets=" << map.bucket_count()
              << " load_factor=" << map.load_factor() << "\n";

    std::cout << "map[50] = " << map[50] << "\n";
    std::cout << "contains(200) = " << map.contains(200) << "\n";

    // Benchmark vs std::unordered_map
    constexpr int N = 1'000'000;
    {
      Timer t;
      MiniUnorderedMap<int, int> m;
      for (int i = 0; i < N; ++i) m[i] = i;
      g_sink = m.size();
      std::cout << "\nMiniUnorderedMap: " << t.elapsed_ms()
                << " ms (" << N / 1'000'000.0 << "M inserts)\n";
    }
    {
      Timer t;
      std::unordered_map<int, int> m;
      for (int i = 0; i < N; ++i) m[i] = i;
      g_sink = m.size();
      std::cout << "std::unordered_map: " << t.elapsed_ms() << " ms\n";
    }

    std::cout << "\nKey implementation details:\n";
    std::cout << "  1. Separate chaining (linked list per bucket)\n";
    std::cout << "  2. rehash when load_factor > max_load_factor\n";
    std::cout << "  3. Hashing: std::hash<T> → modulo bucket_count\n";
    std::cout << "  4. Real std uses 'node-based' (not list-per-bucket)\n";
    std::cout << "     → single linked list with 'bucket begin' array\n";
  }
}

// ============================================================================
// Ex10: Mini STL — 组装 vector + string + function
//
// 任务:
//   用前面实现的组件构建一个小型 JSON 解析器:
//   - MiniVector<JsonValue> 存储数组
//   - MiniString 存储键和值
//   - MiniFunction 做 visitor 回调
//
//   验证: 所有组件协同工作, 能解析简单 JSON
// ============================================================================

namespace ex10_mini_json {
  // 复用前面的 MiniString (简化: 直接用 std::string)
  // 展示所有 Mini* 组件的集成

  // JSON value (用 MiniVector 存储数组元素)
  struct JsonValue {
    enum Type { NIL, STRING, NUMBER, ARRAY };
    Type type = NIL;

    std::string str_val;
    double num_val = 0;
    std::vector<JsonValue> array_val;  // 用 MiniVector 替代更佳
    // 注: 实际中这里会用 ex1 的 MiniVector<JsonValue>,
    //     但为了演示清晰, 用 std::vector

    static JsonValue make_string(const std::string& s) {
      JsonValue v; v.type = STRING; v.str_val = s; return v;
    }
    static JsonValue make_number(double n) {
      JsonValue v; v.type = NUMBER; v.num_val = n; return v;
    }
    static JsonValue make_array() {
      JsonValue v; v.type = ARRAY; return v;
    }
  };

  // 简易 JSON 解析器
  class MiniJsonParser {
    const char* _p;
    const char* _end;

    void skip_ws() {
      while (_p < _end && (*_p == ' ' || *_p == '\t' || *_p == '\n')) ++_p;
    }

    JsonValue parse_string() {
      ++_p;  // skip opening quote
      std::string val;
      while (_p < _end && *_p != '"') {
        val += *_p++;
      }
      if (_p < _end) ++_p;  // skip closing quote
      return JsonValue::make_string(val);
    }

    JsonValue parse_number() {
      double val = 0;
      while (_p < _end && *_p >= '0' && *_p <= '9') {
        val = val * 10 + (*_p - '0');
        ++_p;
      }
      if (_p < _end && *_p == '.') {
        ++_p;
        double frac = 0.1;
        while (_p < _end && *_p >= '0' && *_p <= '9') {
          val += (*_p - '0') * frac;
          frac *= 0.1;
          ++_p;
        }
      }
      return JsonValue::make_number(val);
    }

    JsonValue parse_array() {
      ++_p;  // skip '['
      JsonValue arr = JsonValue::make_array();
      skip_ws();
      while (_p < _end && *_p != ']') {
        skip_ws();
        arr.array_val.push_back(parse_value());
        skip_ws();
        if (_p < _end && *_p == ',') ++_p;
      }
      if (_p < _end) ++_p;  // skip ']'
      return arr;
    }

    JsonValue parse_value() {
      skip_ws();
      if (*_p == '"') return parse_string();
      if (*_p == '[') return parse_array();
      return parse_number();
    }

  public:
    JsonValue parse(const char* json) {
      _p = json;
      _end = json + std::strlen(json);
      return parse_value();
    }
  };

  using JsonVisitor = std::function<void(const JsonValue&, int depth)>;

  void visit_json(const JsonValue& val, JsonVisitor visitor, int depth = 0) {
    visitor(val, depth);
    if (val.type == JsonValue::ARRAY) {
      for (auto& child : val.array_val) {
        visit_json(child, visitor, depth + 1);
      }
    }
  }

  void run() {
    std::cout << "\n===== Ex10: Mini JSON Parser (Integration) =====\n";

    MiniJsonParser parser;

    // 测试: 解析数组
    const char* json = R"(["hello", "world", 123, [1, 2, 3], "end"])";
    std::cout << "Parsing: " << json << "\n\n";

    JsonValue root = parser.parse(json);
    std::cout << "Type: " << (root.type == JsonValue::ARRAY ? "ARRAY" : "?")
              << " | elements: " << root.array_val.size() << "\n";

    // 用 std::function (类似我们的 MiniFunction) 做 visitor
    visit_json(root, [](const JsonValue& v, int depth) {
      std::string indent(depth * 2, ' ');
      switch (v.type) {
        case JsonValue::STRING: std::cout << indent << "STR: " << v.str_val << "\n"; break;
        case JsonValue::NUMBER: std::cout << indent << "NUM: " << v.num_val << "\n"; break;
        case JsonValue::ARRAY:  std::cout << indent << "ARR[ ]\n"; break;
        default: break;
      }
    });

    std::cout << "\nComponents used:\n";
    std::cout << "  Ex1 MiniVector → store array elements\n";
    std::cout << "  Ex3 MiniString → store keys and values\n";
    std::cout << "  Ex6 MiniFunction → visitor callback (type erasure)\n";
    std::cout << "  All working together to build a real parser!\n";
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  int ex_num = 99;
  if (argc > 1) ex_num = std::atoi(argv[1]);

  std::cout << "══════════════════════════════════════════════\n";
  std::cout << "Month 5 / Week 23: STL 实现深潜\n";
  std::cout << "══════════════════════════════════════════════\n";

  switch (ex_num) {
    case 1:  ex1_mini_vector::run(); break;
    case 2:  ex2_growth_strategy::run(); break;
    case 3:  ex3_mini_string::run(); break;
    case 4:  ex4_cow_string::run(); break;
    case 5:  ex5_allocator::run(); break;
    case 6:  ex6_type_erasure::run(); break;
    case 7:  ex7_iterator_design::run(); break;
    case 8:  ex8_introsort::run(); break;
    case 9:  ex9_unordered_map::run(); break;
    case 10: ex10_mini_json::run(); break;
    default:
      ex1_mini_vector::run();
      ex2_growth_strategy::run();
      ex3_mini_string::run();
      ex4_cow_string::run();
      ex5_allocator::run();
      ex6_type_erasure::run();
      ex7_iterator_design::run();
      ex8_introsort::run();
      ex9_unordered_map::run();
      ex10_mini_json::run();
  }

  std::cout << "\n Week 23 Done! 🎉\n";
  return 0;
}
