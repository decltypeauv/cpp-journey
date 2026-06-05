# C++ 练习总结思考

---

## Week 03 — 智能指针

### 1. unique_ptr 为什么不能拷贝？

因为 `unique_ptr` 的设计语义是**独占所有权**。它的拷贝构造和拷贝赋值被 `= delete` 了：

```cpp
unique_ptr(const unique_ptr&) = delete;
unique_ptr& operator=(const unique_ptr&) = delete;
```

如果允许拷贝，就会出现两个 `unique_ptr` 指向同一块内存 → 两者析构时 double free。只能通过 `std::move` 转移所有权。

### 2. shared_ptr 的引用计数存在哪里？

存在**控制块（control block）**里。控制块是一块独立于被管理对象的堆内存，包含：

- **强引用计数**（shared count）— 有几个 `shared_ptr` 指向对象
- **弱引用计数**（weak count）— 有几个 `weak_ptr` 观察对象
- **删除器**（deleter）
- **分配器**（allocator）

当 `shared_ptr` 拷贝时，指针和控制块指针都被拷贝，引用计数 +1。当引用计数归零时，对象被销毁；当弱引用计数也归零时，控制块被释放。

### 3. make_shared 比 new shared_ptr 好在哪？

| | `make_shared<T>(args)` | `shared_ptr<T>(new T(args))` |
|---|---|---|
| **内存分配次数** | 1 次（对象 + 控制块合并） | 2 次（对象一次，控制块一次） |
| **异常安全** | ✅ 安全 | ❌ 有窗口期 |
| **缓存局部性** | ✅ 更好（紧邻） | ❌ 较差（分散） |

异常安全问题：`shared_ptr<T>(new T(args))` 中，`new T` 和 `shared_ptr` 构造是两个步骤，中间可能发生异常导致裸指针泄漏。

### 4. 什么场景「必须」用 weak_ptr？

**打破循环引用**。当两个对象互相持有 `shared_ptr` 时，引用计数永远不为零 → 内存泄漏。用 `weak_ptr` 替换其中一个方向的引用：

```cpp
struct Node {
    shared_ptr<Node> _next;   // 强引用
    weak_ptr<Node> _prev;     // 弱引用 ← 打破循环
};
```

也用于**观察者模式**：想访问一个对象但不控制其生命周期（缓存、回调、观察者列表）。

### 5. 裸指针在智能指针时代还有用吗？

有用。裸指针的正确使用场景：

- **非拥有型观察** — 只是"看一眼"，不参与生命周期管理
- **函数参数** — 当函数不接管所有权时，用 `T*` 或 `const T&`
- **与 C API 交互** — `unique_ptr::get()` 返回裸指针传给 C 函数
- **this 指针** — 永远不可能是智能指针

原则：**裸指针 = 非拥有（non-owning）引用，智能指针 = 拥有（owning）引用。**

### 6. sizeof(unique_ptr) vs sizeof(shared_ptr) 哪个大？为什么？

**`shared_ptr` 更大。** 典型实现：

```cpp
sizeof(unique_ptr<T>)   ≈ sizeof(T*)           // 默认删除器: 8 字节
sizeof(unique_ptr<T, D>) ≈ sizeof(T*) + sizeof(D) // 自定义删除器可能更大
sizeof(shared_ptr<T>)   ≈ sizeof(T*) * 2        // 16 字节
```

`shared_ptr` 需要同时存储**对象指针**和**控制块指针**（两个指针 = 16 字节，64 位系统）。`unique_ptr` 默认删除器是空基类优化的，只存一个指针。

---

## Week 04 — 模板

### 1. 模板的核心价值是什么？

**编译期多态（静态多态）**。与运行期多态（virtual 函数）对比：

| | 编译期多态（模板） | 运行期多态（virtual） |
|---|---|---|
| **决议时机** | 编译期 | 运行期 |
| **运行时开销** | 零（内联展开） | 虚函数表查表 |
| **类型安全** | 编译期报错 | 可能运行时 dynamic_cast 失败 |
| **代码膨胀** | 每实例化一份 | 一份代码 |
| **灵活性** | 任意类型，只要满足语法 | 必须继承同一基类 |

核心价值：**零开销抽象** — 写一份泛型代码，编译器为每种具体类型生成最优版本。

### 2. 模板代码为什么通常是 header-only？

因为模板不是真正的代码，而是**代码的蓝图**。编译器只有在看到具体模板实参时才会实例化出真正的代码。如果模板定义在 `.cpp` 里，其他翻译单元看不到定义 → 链接时找不到实例化版本 → 链接错误。

两种替代方案：
- **显式实例化**：在 `.cpp` 里 `template class MyTemplate<int>;`，但只能覆盖已知的几组类型
- **C++20 modules**：有望彻底解决这个问题

### 3. 什么场景用全特化，什么场景用部分特化？

- **全特化** `template<>`：当某种具体类型需要完全不同的实现。例如 `std::vector<bool>` 用位压缩存储，和其他 vector 完全不同。
- **部分特化**：当匹配一组类型模式时。例如针对所有指针类型、所有 `const` 类型、模板参数个数不同的情况：

```cpp
// 部分特化: 匹配所有指针类型
template<typename T>
class MyClass<T*> { ... };

// 部分特化: 匹配所有引用类型
template<typename T>
class MyClass<T&> { ... };
```

### 4. 非类型模板参数只能是整数吗？

C++20 之前：只能是整型、枚举、指针/引用、`std::nullptr_t`。

C++20 扩展：允许**浮点数**和**字面量类类型（literal class types）**：

```cpp
template<double Value>       // C++20 ✅
template<MyLiteralClass C>   // C++20 ✅
```

### 5. 变参模板在哪些 STL 设施中使用？

- `std::tuple<T...>` — 任意类型和数量的元素
- `std::variant<T...>` — 类型安全的 union
- `make_shared<T>(args...)` / `make_unique<T>(args...)` — 完美转发构造参数
- `std::thread(f, args...)` — 线程参数传递
- `std::function<R(Args...)>` — 类型擦除的可调用对象
- `emplace_back(args...)` — 容器原位构造
- `std::apply` / `std::invoke` — 参数包展开

### 6. concept 比传统的 enable_if/SFINAE 好在哪里？

| | concept (C++20) | enable_if/SFINAE |
|---|---|---|
| **可读性** | 像接口声明一样清晰 | 模板元编程咒语 |
| **错误信息** | 精确告诉你不满足哪个约束 | 几百行的模板实例化回溯 |
| **重载决议** | 直接参与重载排序 | 间接通过 SFINAE 排除 |
| **IDE 支持** | auto-complete 能理解 | IDE 几乎帮不上忙 |
| **约束位置** | requires 子句，多种形式 | 只能塞在模板参数/返回值里 |

```cpp
// concept: 意图一目了然
template<std::integral T>
auto add(T a, T b) { return a + b; }

// enable_if: 需要解密
template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
auto add(T a, T b) { return a + b; }
```

---

## Week 05 — Lambda

### 1. 为什么 lambda 的捕获发生在「定义时」而不是「调用时」？

因为 lambda 本质上是编译器生成的**匿名函数对象**：

```cpp
int x = 10;
auto lam = [x]() { return x; };
```

等价于：

```cpp
struct __anonymous {
    int x;                              // 成员变量在构造时初始化
    __anonymous(int x) : x(x) {}        // 捕获 = 构造函数参数传递
    auto operator()() const { return x; }
};
auto lam = __anonymous{x};             // 定义时拷贝，不是调用时
```

捕获是成员变量的初始化，自然发生在对象构造那一刻。要"调用时取值"就需要用引用捕获 `[&x]`。

### 2. 什么时候必须用 mutable？

当需要**修改按值捕获的副本**时。默认 `operator()` 是 `const` 的，按值捕获的变量是只读的：

```cpp
int counter = 0;
auto gen = [counter]() mutable { return ++counter; };
//                    ^^^^^^^ 去掉 operator() 的 const
```

典型场景：有状态回调——计数器、唯一 ID 生成器、状态机。

### 3. 初始化捕获 `[p = std::move(ptr)]` 能做什么按值/按引用做不到的事？

**把不可拷贝、只能移动的对象移入 lambda**。最典型的是 `unique_ptr`：

```cpp
auto ptr = make_unique<int>(42);
auto lam = [p = std::move(ptr)]() { return *p; };  // 所有权转移进 lambda
// ptr 现在是 nullptr
```

按值做不到（`unique_ptr` 不能拷贝），按引用很危险（引用的对象可能已被销毁）。初始化捕获还能做**表达式求值**：昂贵计算只执行一次，结果直接存在 lambda 内部。

### 4. std::function 和直接 auto 接 lambda 的区别是什么？

| | `auto` 接 lambda | `std::function` |
|---|---|---|
| **类型** | 独一无二的匿名类型 | 统一的具名类型 |
| **开销** | 零开销（编译器内联） | 类型擦除 + 可能堆分配 |
| **用途** | 传参给模板/算法 | 存为成员变量、回调注册 |
| **泛型支持** | ✅ auto 参数完全支持 | ❌ 必须指定固定签名 |

**`auto` 是「拥有」lambda，`std::function` 是「包装」lambda。** 需要把不同类型的可调用对象存在同一个容器或成员变量里时才用 `std::function`。

### 5. 泛型 lambda 比普通 lambda 多了什么能力？

让 **lambda 变成模板**。同一个 lambda 对象，编译器为每种参数组合生成不同的 `operator()` 实例：

```cpp
auto add = [](auto a, auto b) { return a + b; };
add(1, 2);          // T=int, U=int
add(1.5, 2.3);      // T=double, U=double
add(string("A"), string("B")); // T=string, U=string
```

配合 `decltype` 推导返回类型、配合折叠表达式处理变参，写出的泛型代码既灵活又零开销。

### 6. 在 sort 之类的算法中用 lambda 比写函数指针好在哪？

- **定义在使用点旁边** — 不用跳到文件顶部找比较函数
- **可以捕获上下文** — 函数指针做不到（没地方存额外状态）
- **更高效** — lambda 的类型是具名匿名类，编译器知道具体类型可以直接内联；函数指针是间接调用，编译器需要别名分析才能内联

---

## Week 06 — 异常安全

### 1. 为什么说「RAII 是为异常安全而生的」？

因为异常通过**栈展开**来跳转控制流 — throw 和 catch 之间所有的局部对象都会被自动析构。RAII 正好利用了这个机制：

```cpp
{
    auto res = make_unique<Resource>();  // RAII 接管资源
    do_something_that_may_throw();       // 即使 throw...
}  // ← res 的析构必被调用，资源必被释放
```

没有 RAII 你需要手动在每一条可能的异常路径上写 `delete/close/unlock`，在实际代码中不可行。**RAII 是因，异常安全是果。**

### 2. delete 可能抛异常吗？为什么析构函数默认 noexcept 是安全的？

**`delete` 和 `delete[]` 本身不抛异常。** C++ 标准保证 `delete` 表达式调用析构时不抛异常，`operator delete` 的内存释放函数也不抛。

C++11 起析构函数默认是 `noexcept(true)`，因为这个设计假设：析构主要做的是释放资源（内存、文件句柄、锁），这些操作本身不应该失败。如果析构抛异常，在栈展开过程中再抛一个异常 = 直接 `std::terminate()`，程序必死。

### 3. 为什么 vector 扩容时关心移动构造是否 noexcept？

`std::vector` 扩容策略：

```
T 的移动构造是 noexcept → 用移动（快，只是指针交换）
T 的移动构造不是 noexcept → 用拷贝（慢，但安全）
```

原因：**Strong Guarantee 的约束。** 扩容中如果某个元素的移动抛了异常，已经移走的元素无法"移回去"——强保证就破了。拷贝抛异常则安全得多：新空间拷贝失败 → 丢弃新空间 → 旧数据完好。

`noexcept` 标记直接影响容器的运行时性能。**移动构造不写 noexcept，你的 vector 扩容时就默默地走拷贝路径。**

### 4. copy-and-swap 如何用「空间换安全」实现 strong guarantee？

三步走：

```cpp
SafeBuffer& operator=(const SafeBuffer &rhs) {
    SafeBuffer temp(rhs);   // ① 先拷贝 → 可能抛异常，但 *this 毫发无伤
    swap(temp);             // ② 交换 → noexcept，绝不失败
    return *this;           // ③ temp 析构，带走旧数据
}
```

"空间"代价：多了一个临时对象的分配和拷贝。

"安全"收益：把**"可能失败的操作"**（拷贝）和**"不可逆的操作"**（替换旧数据）分离 — 拷贝确认成功后才 swap。一份额外内存空间，换"要么全成功，要么原样保留"的强保证。

### 5. 什么函数应该标记 noexcept？

| 函数类型 | 是否标记 noexcept | 理由 |
|---|---|---|
| **析构函数** | ✅ 隐式 noexcept | 析构抛异常 = terminate |
| **移动构造/赋值** | ✅ **必须写** | 决定 vector 是否用移动 |
| **swap** | ✅ **必须写** | copy-and-swap 基石 |
| **简单 getter** | ✅ 应该写 | 返回基本类型/指针不会失败 |
| **默认构造** | 视情况 | `int()` 不抛，分配内存的则可能抛 |
| **可能抛异常的函数** | ❌ 不写 | 诚实表达可能失败 |

判断准则：**函数内部只做指针交换、整数运算、返回成员 — 标记 noexcept；涉及动态分配、I/O、可能抛异常的操作 — 不标记。**

---

## Week 07 — 文件 I/O

### 1. C++ iostream 的 RAII 如何保证文件描述符不泄漏？

fstream 的构造和析构直接捆绑了文件的打开和关闭：

```cpp
{
  std::ofstream out("file.txt");  // 构造 → 调用 open()
  out << "data";
}  // 析构 → 调用 close()
```

**关键机制**：无论 `{ }` 作用域如何退出（正常结束、`return`、`throw` 异常），局部对象的析构函数都**必定被调用**（栈展开）。

没有 RAII 的世界（C 风格）：
```cpp
FILE *fp = fopen("file.txt", "w");
do_something_that_may_throw(fp);  // 抛异常 → 跳过 fclose！
fclose(fp);                        // 可能永远执行不到 → 泄漏
```

有 RAII 的世界：`do_something_that_may_throw` 即使抛异常，栈展开会调用 `ofstream` 的析构 → `close()` 必定执行。

这和 `unique_ptr` 保证 `delete` 被调用的原理完全一样 — **iostream 就是文件句柄的 RAII 包装器**。

每一个 fstream 对象在内部持有：
- 一个文件缓冲区（`std::filebuf`）— RAII
- 通过 `filebuf` 间接持有操作系统文件描述符 — `filebuf` 析构时调用 `close()`

所以即使你忘了手动 `close()`，fstream 析构也会自动关闭文件。

---

### 2. failbit / badbit / eofbit 的区别？什么时候用 exceptions()？

每个流内部维护一个位掩码 `iostate`，三个位各有不同含义：

| 位 | 含义 | 严重性 | 流是否可用 | 例 |
|----|------|--------|-----------|-----|
| `goodbit` = 0 | 正常 | — | ✅ | — |
| `eofbit` | 到达文件末尾 | 低 | ⚠️ 不能继续读，但可 seek | 读完最后一行后继续读 |
| `failbit` | 操作失败 | 中 | ✅ 清除后可恢复 | `>> int` 遇到 `"abc"` |
| `badbit` | 流内部错误 | 高 | ❌ 流已损坏 | 写失败，缓冲区损坏 |

**区别的核心要点**：

- `eofbit`：不是错误。读完文件是正常现象。`while (getline(in, line))` 不会误判。
- `failbit`：格式/逻辑错误。流本身没坏，`clear()` 后跳过错位数据即可继续读。
- `badbit`：物理/系统错误。流已经坏了，唯一的做法是放弃。

**eofbit 的特殊性**：读操作不会先设 eofbit 再立即停止。而是：
1. 尝试读 → 发现 EOF → 设置 eofbit
2. 如果同时什么都没读到 → 也设置 failbit

这意味着 `while (in >> val)` 自然终止时，是 failbit 先触发终止，eofbit 只是附带信息。

**什么时候用 `exceptions()`？**

两种场景：

1. **启用的场景**：当文件格式错误意味程序逻辑错误，不应该 quietly 跳过。比如：
   - 读取配置文件：格式必须正确，出错应立即报错
   - 读取二进制协议：每个字节都要对
   
2. **不启用的场景**：当格式错误是预期内的，有回退逻辑。比如：
   - 用户输入验证：先试 `>> int`，失败再提示重新输入
   - 格式探测：尝试多种解析方式，失败就换下一种

```cpp
// 启用异常：配置文件解析
in.exceptions(std::ios::failbit | std::ios::badbit);
try {
  in >> config_value;  // 格式不对 → 立即抛异常
} catch (const std::ios_base::failure &e) {
  // 配置文件损坏，无法继续
}

// 不用异常：用户输入
int val;
if (!(std::cin >> val)) {  // 格式不对 → 返回 false，不抛异常
  std::cin.clear();        // 清除 failbit
  std::cin.ignore(...);   // 跳过坏数据
  std::cout << "请输入整数\n";
}
```

默认情况下流不启用异常 — 这是有道理的，因为大多数场景用 flow control (`if (stream)`) 比 try-catch 更自然。

---

### 3. 二进制 I/O 比格式化 I/O 快在哪里？什么时候用哪个？

**快在哪里？** 三个层面：

| | 二进制 I/O (`read/write`) | 格式化 I/O (`<< / >>`) |
|---|---|---|
| **CPU 开销** | 直接 `memcpy`，无转换 | 数字→字符串解析/格式化 |
| **数据大小** | int=4字节，double=8字节 | `"42"`=2字节，`"3.14159"`=9字节 |
| **IO 次数** | 通常更少（数据紧凑） | 通常更多（文本膨胀） |

**本质上**：

```
二进制:  内存字节 ──[直接搬]──→ 磁盘字节
格式化:  内存字节 ──[格式化]──→ 文本 ──[解析]──→ 内存字节
                 ↑ CPU 做了额外工作 ↑
```

**什么时候用二进制？**
- 存/读固定结构的记录（struct 数组）
- 序列化/反序列化（游戏存档、矩阵数据）
- 不需要人类阅读的中间数据、缓存
- 性能敏感的批量数据处理

**什么时候用文本/格式化？**
- 配置文件（.json, .toml, .ini）
- 日志文件
- 人类需要阅读或编辑的数据
- 跨平台交换（文本的换行符问题有 iostream 自动处理）
- 需要用 `grep`/`diff`/`vim` 检查的数据

**关键判断**：

> 如果数据需要被人类（或文本工具）直接阅读、编辑 → 格式化 I/O。
> 如果数据只在程序之间流动、对大小/速度敏感 → 二进制 I/O。

还有一个 subtler 的点：二进制 I/O **不是可移植的**。`int` 的大小、字节序、struct 的 padding 都依赖于具体平台和编译器。跨平台交换用二进制需要序列化协议（protobuf, flatbuffers 等），而不是直接 `write(&myStruct, sizeof(myStruct))`。

---

### 4. std::filesystem 相比 POSIX stat/opendir 有什么优势？

| | `std::filesystem` | POSIX (`stat`, `opendir`, ...) |
|---|---|---|
| **类型安全** | `fs::path` 对象，`operator/` 拼接 | `const char*` 字符串拼接，易出错 |
| **跨平台** | ✅ Windows/Linux/macOS 同一套 API | ❌ Linux 用 `/`，Windows 用 `\` |
| **错误处理** | `fs::filesystem_error` 异常 或 `error_code` | `errno` 全局变量，需手动 `strerror` |
| **目录遍历** | `for (auto &entry : fs::directory_iterator(p))` | `opendir` → `readdir` 循环 → `closedir` |
| **操作组合** | `copy_file`, `rename`, `remove_all` 一个调用 | 需要组合 `open`/`read`/`write`/`close` |
| **RAII** | 迭代器析构自动清理 | 必须手动 `closedir` |
| **路径操作** | `.filename()`, `.extension()`, `.parent_path()` | 手动 `basename()`/`dirname()` 或字符串切割 |

**最大优势**：把文件系统操作从"系统调用思维"提升到"对象思维"。

以前：
```cpp
DIR *dir = opendir("/path/to/dir");
struct dirent *entry;
while ((entry = readdir(dir)) != NULL) {
  struct stat st;
  stat(entry->d_name, &st);  // 还要拼接完整路径
  if (S_ISREG(st.st_mode)) { ... }
}
closedir(dir);  // 忘了就泄漏
```

现在：
```cpp
for (const auto &entry : fs::directory_iterator("/path/to/dir")) {
  if (entry.is_regular_file()) { ... }
}  // 自动清理，不用想 closedir
```

**次要看点**：`fs::path` 的 `operator/` 自动处理分隔符：
```cpp
auto p = fs::path("/tmp") / "sub" / "file.txt";
//             → /tmp/sub/file.txt  (Linux)
//             → \tmp\sub\file.txt  (Windows)
```

这比 `"/tmp/" + dir + "/" + file` 安全且可移植得多。

---

### 5. 如果要在没有异常的环境中使用文件 I/O，怎么设计 RAII？

有些环境禁用异常（嵌入式、游戏引擎、内核模块、`-fno-exceptions`）。但 RAII 本身**不依赖异常** — 它依赖的是**析构函数必定被调用**。

挑战在**构造阶段**：如果 `open()` 失败，不能抛异常，怎么通知调用者？

**方案 A：双阶段构造 + 有效性检查**

```cpp
class FileDescriptor {
  int _fd = -1;

public:
  FileDescriptor() = default;  // 构造不打开文件，永远成功

  bool open(const char *path, int flags) {  // 返回 bool 代替抛异常
    _fd = ::open(path, flags, 0644);
    return _fd >= 0;  // false = 打开失败
  }

  bool valid() const { return _fd >= 0; }

  ~FileDescriptor() {
    if (_fd >= 0) ::close(_fd);  // 析构仍然保证释放
  }
};

// 使用：
FileDescriptor fd;
if (fd.open("file.txt", O_RDONLY)) {
  // 使用 fd
} else {
  // 处理错误
}  // fd 析构 → close（如果打开过）
```

**方案 B：工厂函数 + optional**

```cpp
std::optional<FileDescriptor> open_file(const char *path, int flags) {
  int fd = ::open(path, flags, 0644);
  if (fd < 0) return std::nullopt;  // 失败：不返回对象
  return FileDescriptor(fd);         // 成功：构造 + RVO
}
```

**方案 C：error_code 风格（模仿 std::filesystem 的重载）**

```cpp
FileDescriptor(const char *path, int flags, std::error_code &ec)
  : _fd(::open(path, flags, 0644)) {
  if (_fd < 0) ec = std::error_code(errno, std::generic_category());
}
```

**核心思想**：

> RAII 的是**释放**，不是**获取**。
> 获取可以失败（怎么报告失败可以灵活选择），
> 但一旦获取成功，释放必须自动且绝不失败。

所以即使没有异常，RAII 的"析构释放"部分依然是不可替代的安全保障。

---

### 6. std::endl vs `'\n'` — 什么时候该用哪个？

```cpp
std::endl  =  '\n' + std::flush()
```

| | `std::endl` | `'\n'` |
|---|---|---|
| **写换行符** | ✅ | ✅ |
| **强制刷新缓冲区** | ✅ | ❌ |
| **性能** | 慢（每次刷新 = 一次系统调用） | 快（缓冲区满了才刷） |
| **数据安全** | 高（立即落盘） | 低（程序崩溃=缓冲区内数据丢失） |

**什么时候用 `std::endl`？**

- **日志**：每条日志立即刷到磁盘/终端，crash 时不会丢失关键信息
- **交互式程序**：输出提示符后用户需要立即看到（`cout << "> " << endl;`）
- **调试**：在每个检查点后 flush，确保 crash 前最后一条输出可见
- **进程间通信**：通过管道通信时，另一端需要尽快看到数据

**什么时候用 `'\n'`？**

- **大批量输出**：写文件、输出大量计算结果
- **性能敏感场景**：循环内频繁输出（缓冲区满才写盘，减少系统调用次数）
- **不关心即时性**：数据丢失可接受，或程序本身就是一次性运行

**实际中的权衡**：

```cpp
// ❌ 慢：每次写一行就刷新一次（1000 行 = 1000 次系统调用）
for (int i = 0; i < 1000; ++i)
  std::cout << data[i] << std::endl;

// ✅ 快：攒到缓冲区满再刷（1000 行 ≈ 1-2 次系统调用）
for (int i = 0; i < 1000; ++i)
  std::cout << data[i] << '\n';

// ✅ 折中：每 100 行显式刷新一次
for (int i = 0; i < 1000; ++i) {
  std::cout << data[i] << '\n';
  if (i % 100 == 0) std::cout << std::flush;
}
```

**关键判断**：

> 日志/交互/调试 → `std::endl` 或手动 `std::flush`。
> 其他一切 → `'\n'`。
> `std::endl` 的默认使用（尤其是新手）是对性能的无意识浪费。

另外注意：`std::cerr` 默认是 unit-buffered（每个字符都刷新），所以 `std::cerr << "error\n"` 不需要 `std::endl`。

---

## Week 08 — 进程

### 1. fork() 为什么调用一次却返回两次？

因为 fork() 做的事情是**克隆当前进程**。调用 fork() 的进程（父进程）进入内核，内核创建一个几乎完全相同的子进程，然后**两个进程都从 fork() 的返回点继续执行**。

```
调用 fork() 前:    [父进程] ──执行中──

调用 fork():       [父进程] ──进入内核──
                         │
                   内核克隆进程
                         │
                   ┌─────┴─────┐
                   ▼           ▼
              [父进程]      [子进程]
              fork() 返回   fork() 返回
              子进程 PID        0
```

**fork() 返回值**：

| 哪个进程 | fork() 返回 | 含义 |
|---------|-----------|------|
| 父进程 | > 0 | 刚创建的子进程的 PID |
| 子进程 | 0 | 标识"我是子进程" |
| 出错 | -1 | fork 失败，没有创建子进程 |

所以不是"一个调用返回两次"，更准确的描述是"**一个调用，两个进程各自收到一个返回值**"。

---

### 2. fork 之后父子进程的地址空间是什么关系？什么是 Copy-on-Write？

fork 后子进程获得父进程地址空间的**逻辑副本**，但物理内存通过 **Copy-on-Write (COW)** 延迟复制：

```
fork 刚完成时（未写入）:
  父进程虚拟页 ──→ [物理页框 A] ←── 子进程虚拟页
                   （共享，只读）

某一方写入时:
  父进程虚拟页 ──→ [物理页框 A]     ← 原来的
  子进程虚拟页 ──→ [物理页框 A']    ← 内核复制一份新的
                   （各自独立，可写）
```

**核心优势**：
- fork() 本身极快 — 只复制页表，不复制数据
- 初始内存开销极小 — 父子共享物理页
- 只有真正写入的页才会被复制

这就是为什么 fork 之后即使分配了 10MB 内存，如果没有写入，fork 也不会真的复制 10MB。

**重要推论 — fork + 缓冲 I/O 的陷阱**：
fork 会复制整个地址空间，包括 `std::cout` / `printf` 的**用户态缓冲区**。如果 fork 前缓冲区里有未刷新的数据，子进程也会有一份 → 子进程退出（或 flush）时，父进程的缓冲数据被重复输出。

解决方案：
1. fork 之前总是 `fflush(stdout)` 或 `cout << flush`
2. 设 stdout 为无缓冲模式：`cout << unitbuf` 或 `setvbuf(stdout, NULL, _IONBF, 0)`
3. 子进程用 `_exit()` 而非 `exit()` — 前者不刷新继承来的缓冲区

---

### 3. exec() 和 fork() 的本质区别是什么？为什么通常组合使用？

| | fork() | exec() |
|---|---|---|
| **是否创建新进程** | ✅ 是（克隆当前进程） | ❌ 否 |
| **进程映像** | 和父进程相同 | 被新程序替换 |
| **PID** | 新 PID | PID 不变 |
| **成功后** | 父子都从 fork 后继续 | **不返回**（代码被替换了） |
| **失败后** | 返回 -1 | 返回 -1，继续执行原代码 |

**fork + exec 是 Linux 创建新程序的唯一方式**：

```cpp
pid_t pid = fork();         // ① 创建新进程（克隆）
if (pid == 0) {
    execl("/bin/ls", "ls", "-l", nullptr);  // ② 替换为 ls 的代码
    _exit(1);               // ③ exec 失败才到这里
}
// 父进程继续
waitpid(pid, &status, 0);
```

为什么不能直接 exec？因为 exec 替换**当前**进程 — 如果不用 fork 就直接 exec，当前进程就变成了 `ls`，原来的代码就没了。

为什么 fork 后不直接用？因为子进程是父进程的克隆，执行的是同样的代码 — 只有通过 exec 换上别的程序，才能让子进程「与众不同」。

**exec 家族记忆技巧**：

| 后缀 | 含义 | 示例 |
|------|------|------|
| `l` (list) | 参数逐个列出 | `execl("/bin/echo", "echo", "hi", nullptr)` |
| `v` (vector) | 参数用数组 | `execv("/bin/echo", argv)` |
| `p` (PATH) | 在 PATH 中搜索 | `execvp("echo", argv)` |
| `e` (env) | 自定义环境变量 | `execle("/bin/echo", ..., envp)` |

真正的系统调用是 `execve`，其余都是 libc 封装。

---

### 4. 僵尸进程是怎么产生的？如何避免？

**僵尸进程**：子进程已经结束（exit / 被信号杀死），但父进程还没有调用 `wait/waitpid` 来读取它的退出状态。内核需要保留子进程的 PCB（进程控制块）中的退出码等信息，直到父进程来"收尸"。

```
子进程生命周期:
  运行中 (R/S) → 退出 → 僵尸 (Z) → 父进程 wait → 彻底消失
                          ↑
                    父进程不 wait = 一直僵尸
```

**查看僵尸**：`ps aux | grep Z` — STAT 列为 Z。

**危害**：僵尸占用 PCB 条目，大量僵尸会耗尽 PID 资源（`/proc/sys/kernel/pid_max`），导致无法创建新进程。

**孤儿进程 vs 僵尸进程**：

| | 孤儿 (Orphan) | 僵尸 (Zombie) |
|---|---|---|
| **定义** | 父进程先于子进程结束 | 子进程已结束，父进程未 wait |
| **危害** | 无害 | 占用系统资源 |
| **处理** | 自动被 init (PID=1) 收养并收割 | 必须父进程主动 wait |

**避免僵尸的方法**：

1. **父进程调用 wait/waitpid** — 最基本的做法
2. **忽略 SIGCHLD 信号**：`signal(SIGCHLD, SIG_IGN)` — 内核自动回收，不产生僵尸
3. **双重 fork**：父进程 fork 子进程，子进程立即 fork 孙进程后退出 → 孙进程变孤儿被 init 收养
4. **SIGCHLD 处理器中循环 waitpid**：`while (waitpid(-1, &status, WNOHANG) > 0);`

---

### 5. waitpid 的 WNOHANG 选项有什么用？

`WNOHANG` 让 waitpid 变成**非阻塞**：如果子进程还没结束，立即返回 0 而不是阻塞等待。

三种返回值对比：

| 返回值 | 含义 |
|--------|------|
| > 0 | 收割了一个子进程，返回值 = 子进程 PID |
| 0 | WNOHANG 模式下，子进程**尚未**结束 |
| -1 | 出错（没有子进程 / 已被其他 wait 收割） |

**使用场景**：

```cpp
// ① 轮询：父进程在等待的同时可以做别的事
while (waitpid(pid, &status, WNOHANG) == 0) {
    do_other_work();  // 渲染帧、检查网络、更新 UI...
}

// ② 批量收割：一次性收割所有已结束的子进程
while (waitpid(-1, &status, WNOHANG) > 0) {
    // -1 = 等待任意子进程
}

// ③ 阻塞等待：WNOHANG 不设 = 0
waitpid(pid, &status, 0);  // 子进程不结束就不返回
```

**退出状态的解析**：

```cpp
if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);  // 正常退出的退出码
} else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);      // 被哪个信号杀死
}
```

---

### 6. pipe 为什么需要两个 fd？关闭不用的 fd 为什么重要？

`pipe()` 创建一个**单向**数据通道，返回两个 fd：

```cpp
int pipefd[2];
pipe(pipefd);
// pipefd[0] — 读端（数据从这里读出来）
// pipefd[1] — 写端（数据从这里写进去）
```

```
父进程                         子进程
   │                             │
   ├─ write(pipefd[1], ...) ─→  ├─ read(pipefd[0], ...)
   │         写端                  │         读端
   │                             │
   └─ read(pipefd[0], ...) ←──  └─ write(pipefd[1], ...)
             读端                           写端
```

**为什么需要两个 fd？** 因为 pipe 是**单向**的 — 数据只能从写端流向读端。需要双向通信就要两个 pipe（4 个 fd）。

**关闭不用的 fd 为什么重要？**

1. **读端不关 → 写端永远不会收到 EPIPE/SIGPIPE**：如果所有读端都关闭了，再 write 就会收到 SIGPIPE 信号（默认终止进程）。不关读端就不会有这个保护。

2. **写端不关 → 读端永远等不到 EOF**：`read()` 返回 0 表示 EOF（管道写端全部关闭）。只要还有一个写端开着（即使 fork 后的子进程持有一个），读端就会一直阻塞等数据。

```cpp
// fork 后立即关闭不用的 fd 是铁律
if (pid == 0) {
    close(pipefd[1]);  // 子进程只读 → 关写端
    // ... read ...
    close(pipefd[0]);
} else {
    close(pipefd[0]);  // 父进程只写 → 关读端
    // ... write ...
    close(pipefd[1]);  // 写完关 → 子进程 read 收到 EOF
}
```

---

### 7. 子进程为什么应该用 `_exit()` 而不是 `exit()`？

| 函数 | 行为 |
|------|------|
| `exit(n)` | ① 调用 `atexit` 注册的函数 ② 刷新所有 stdio 缓冲区 ③ 调用 `_exit(n)` |
| `_exit(n)` | 直接进入内核，不做任何用户态清理 |
| `_Exit(n)` | 同 `_exit(n)`（C11 标准名） |

**fork 后子进程用 `exit()` 的两个问题**：

**问题 1：重复刷新缓冲区**。fork 时子进程继承了父进程的 stdio 缓冲区内容。如果子进程调用 `exit()`，它会把这些继承来的数据刷到文件描述符 → **同样的数据被写了两次**（父进程 exit 时再写一次）。

**问题 2：重复执行 atexit 回调**。父进程注册的 `atexit` 回调被 fork 复制到了子进程。子进程 `exit()` 时会执行这些回调 → **清理代码被执行两次**（可能 double-free、重复删文件等）。

```cpp
// ❌ 错误：子进程用 exit()
if (pid == 0) {
    do_child_work();
    exit(0);  // 可能刷新父进程的缓冲区内容！
}

// ✅ 正确：子进程用 _exit()
if (pid == 0) {
    do_child_work();
    std::cout << std::flush;  // 手动刷新自己的输出
    _exit(0);                 // 直接退出，不碰父进程的数据
}
```

**规则**：子进程用 `_exit()`，父进程用 `exit()`。exec 失败后的子进程也一样 — `_exit(127)` 而不是 `exit(127)`。

---

### 8. dup2 在 pipe + exec 模式中扮演什么角色？

`dup2(oldfd, newfd)` 把 `newfd` 变成 `oldfd` 的副本（先 close(newfd) 再复制）。在 fork + exec + pipe 模式中，它用来**重定向子进程的标准输入/输出到 pipe**。

**为什么需要 dup2？** exec 替换了进程映像后，子进程的代码完全变了（变成了 `ls`、`grep` 等），这些程序只能通过 stdin/stdout/stderr（fd 0/1/2）进行 I/O。pipe 的 fd 通常是 3、4、5… 子进程不知道去读/写这些 fd。解决方案：用 dup2 把 pipe 的 fd "搬到" 0/1/2 的位置。

```cpp
// 实现 "ls | wc -l" 的核心逻辑
int pipefd[2];
pipe(pipefd);

pid_t pid = fork();
if (pid == 0) {
    // 子进程: 执行 ls，把它的 stdout 重定向到 pipe 写端
    close(pipefd[0]);                    // 不需要读端
    dup2(pipefd[1], STDOUT_FILENO);      // stdout → pipe 写端
    close(pipefd[1]);                    // 关闭原始 fd（已复制到 1 了）
    execl("/bin/ls", "ls", nullptr);
    _exit(1);
}

// 父进程: 从 pipe 读端读取 ls 的输出
close(pipefd[1]);                        // 不需要写端
// ... read(pipefd[0], buf, size) ...
close(pipefd[0]);
waitpid(pid, &status, 0);
```

**这就是 shell 管道 `|` 的底层实现**！ shell 为每个 `|` 创建一个 pipe，然后用 dup2 把左边命令的 stdout 和右边命令的 stdin 都重定向到 pipe。

重定向前后对比：

```
exec 前（子进程）:
  fd 0 → 终端输入
  fd 1 → 终端输出          ← cout/printf 写到这里
  fd 3 → pipe 写端         ← 没人知道要用 fd 3！

dup2(3, 1) 后:
  fd 0 → 终端输入
  fd 1 → pipe 写端         ← cout/printf 自动进入 pipe！
  fd 3 → (已关闭)

exec ls 后:
  ls 的 printf 写到 fd 1 → 进入 pipe → 父进程从 pipe 读到 ls 的输出
```

---

## Week 09 — 信号

### 1. 信号是什么？和硬件中断有什么区别？

信号是 OS 对进程的**异步软件通知**。它打断进程的正常执行流，让进程"暂停手头的事"去处理信号，处理完再继续。

| | 硬件中断 | 信号（软件中断） |
|---|---|---|
| **触发源** | 外部硬件（键盘、网卡、时钟） | 内核 / 其他进程 |
| **处理者** | 内核 ISR（中断服务例程） | 用户态信号处理器 |
| **上下文** | 中断上下文（不是进程上下文） | 进程上下文（但有严重限制） |
| **可屏蔽** | 可屏蔽中断 / 不可屏蔽中断 | sigprocmask 可阻塞大部分信号 |
| **嵌套** | 高优先级可抢占低优先级 ISR | sa_mask 控制在处理器期间阻塞哪些 |

**类比**：信号是 OS 拍你肩膀说"嘿，有事"——你放下手里的活，处理这件事，然后回来继续。硬件中断是 CPU 的引脚被拉高——CPU 暂停当前指令流，跳到中断向量表。

**信号的生成 vs 递送**：

```
生成 (Generate)  →  内核标记进程的 pending 位图
                        │
                  ┌─────┴─────┐
                  │ 被阻塞？    │
                  │ sigprocmask │
                  └─────┬─────┘
                   是       否
                    │        │
                    ▼        ▼
              保持在        递送 (Deliver)
              pending        │
              中等待    ┌────┴────┐
                        │ 忽略？    │ → SIG_IGN: 丢弃
                        │ 默认？    │ → SIG_DFL: 通常是终止
                        │ 处理器？  │ → 调用 sa_handler
                        └─────────┘
```

---

### 2. signal() 和 sigaction() 的区别？为什么应该用 sigaction？

| | `signal()` | `sigaction()` |
|---|---|---|
| **标准** | C89/C99（最小公分母） | POSIX.1 |
| **跨平台一致性** | ❌ System V vs BSD 行为不同 | ✅ 行为确定 |
| **自动重置** | System V 会，Linux/BSD 不会 | 默认不会（除非设 SA_RESETHAND） |
| **信号阻塞** | 不能控制在处理器期间阻塞哪些信号 | sa_mask 精确控制 |
| **扩展信息** | 只有 signum | SA_SIGINFO → siginfo_t（发送者 PID、UID 等） |
| **SA_RESTART** | 不可用 | 自动重启被中断的系统调用 |
| **推荐程度** | ❌ 历史遗留 | ✅ 生产代码必须用 |

**signal() 的核心问题 — 语义不确定**：

```cpp
// System V: 处理一次后自动重置为 SIG_DFL
//   信号 → 调用 handler → 信号恢复 SIG_DFL → 再发信号 → 进程死
//   必须在 handler 里再次 signal(SIGINT, handler) — 但有窗口期！

// BSD/Linux: 保持注册，不需要重新 signal
//   但 signal() 仍然缺少 sa_mask / SA_RESTART 等控制
```

**sigaction 的正确打开方式**：

```cpp
struct sigaction sa {};
sa.sa_sigaction = my_handler;   // 三参数版本（含 siginfo_t）
sa.sa_flags = SA_SIGINFO        // 启用扩展信息
            | SA_RESTART        // 自动重启被中断的 syscall
            | SA_NOCLDSTOP;     // 子进程停止时不通知（仅 SIGCHLD）
sigemptyset(&sa.sa_mask);
sigaddset(&sa.sa_mask, SIGTERM); // 处理本信号时暂阻塞 SIGTERM
sigaction(SIGINT, &sa, nullptr);
```

---

### 3. 信号处理器中哪些操作是安全的（async-signal-safe）？

信号处理器是**异步**执行的 — 它可能在程序执行任意代码时闯入。如果你在 `malloc` 中途被信号打断，处理器里又调用 `malloc` → 死锁（malloc 的内部锁已被持有）。

**POSIX 规定的 async-signal-safe 函数**（部分）：

| 类别 | 安全函数 |
|------|---------|
| **退出** | `_exit()`, `_Exit()` |
| **文件 I/O** | `open()`, `read()`, `write()`, `close()`, `lseek()`, `fsync()` |
| **进程** | `fork()`, `execve()`, `waitpid()`, `getpid()`, `getppid()` |
| **信号** | `kill()`, `raise()`, `sigaction()`, `sigprocmask()`, `sigdelset()` |
| **时间** | `time()`, `clock_gettime()` |
| **管道** | `pipe()` |
| **socket** | `socketpair()` |
| **变量** | 读写 `volatile sig_atomic_t` |
| **错误** | `errno` (先保存后恢复) |

**绝对不能用的**：

```
❌ malloc / free / new / delete   — 有锁
❌ printf / cout / fprintf        — 有锁 + 缓冲区
❌ fopen / fread / fwrite         — 有锁
❌ pthread_mutex_lock             — 有锁
❌ std::string 构造/析构          — 内部分配内存
❌ std::vector / std::map 操作    — 分配内存 + 可能抛异常
```

**本质上**：信号处理器中只能调**系统调用**和操作**无锁的全局变量**。任何有用户态锁或可能分配内存的函数都不能碰。

**唯一安全的数据通道**：

```
信号处理器                   主循环
    │                          │
    ├─ g_flag = 1;  ──→ 轮询 g_flag
    │  (sig_atomic_t)
    │
    ├─ write(pipefd) ──→ read(pipefd)  (self-pipe trick)
    │
    └─ 尽量少做事              做所有重活
```

---

### 4. 被阻塞的信号去哪了？解除阻塞后会发生什么？

被 `sigprocmask(SIG_BLOCK, ...)` 阻塞的信号**不会丢失**，它们处于 **pending（待处理）**状态。内核在每个进程的 PCB 中维护一个 pending 位图。

```
sigprocmask 阻塞 SIGINT:

  kill(pid, SIGINT) ──→ 内核: pending 位图打标记
                         因为 SIGINT 被阻塞 → 不递送
                         SIGINT 成为 pending 信号

sigprocmask 解除阻塞:

  内核: 检查 pending 位图 → 发现 SIGINT
        → 立即递送（调用 handler / SIG_DFL）

sigpending: 随时查询当前 pending 的信号集合
```

**注意**：标准信号（非实时信号）不排队。如果在阻塞期间发送了 100 个 SIGINT，解除阻塞后只递送**一次**。内核只记录"有没有 pending"，不记录"pending 了几个"。

```cpp
sigset_t set, old;
sigemptyset(&set);
sigaddset(&set, SIGINT);
sigprocmask(SIG_BLOCK, &set, &old);

for (int i = 0; i < 100; ++i)
    raise(SIGINT);  // 发送 100 次...

sigprocmask(SIG_SETMASK, &old, nullptr);
// → 信号处理器只被调用 1 次！（不是 100 次）
```

这就是为什么 SIGCHLD 处理器必须用 `while(waitpid(...WNOHANG) > 0)` — 多个子进程退出可能只触发一次 SIGCHLD。

---

### 5. SIGCHLD 为什么需要循环 waitpid？

标准信号（非实时信号）在 pending 位图中只占一个 bit。如果多个子进程几乎同时退出，可能只触发**一次** SIGCHLD。

```cpp
// ❌ 错误：只收割一个子进程
void on_sigchld(int signo) {
    pid_t pid = waitpid(-1, &status, WNOHANG);
    // 如果 3 个子进程同时退出，只收割了 1 个
    // 剩下 2 个变成僵尸！
}

// ✅ 正确：循环收割直到没有更多退出的子进程
void on_sigchld(int signo) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // WNOHANG: 如果没有更多已退出的 → 返回 0 → 循环结束
        // 每次收割一个，直到所有已退出的子进程都被收割
    }
}
```

**信号到达和 wait 的竞态**：

```
子进程退出 → SIGCHLD pending

还有更多子进程未退出...

处理器运行 → while(waitpid()) → 收割了已退出的 → 退出循环

又一个子进程退出 → 再发 SIGCHLD → 处理器再次运行 → 收割
```

关键：一次 SIGCHLD 对应"至少一个"子进程退出，但可能有多个。循环 waitpid 确保"每次收到信号就收割所有已退出的"。

---

### 6. Self-Pipe Trick 是怎么解决信号处理器限制的？

**问题**：信号处理器不能调 `printf`/`malloc`/加锁，但实际应用中收到信号后通常需要做复杂操作（写日志、更新数据结构、触发回调）。

**解决思路**：信号处理器只做最小的事情 — 把信号编号写入一个 pipe。主循环（或 event loop）读到 pipe 中的数据后，在正常上下文中处理。

```
信号到达 ▼
    │
信号处理器（只做 1 件事）:
    write(self_pipe[1], &signum, sizeof(signum))  ← async-signal-safe ✅
    │
    ▼
[ pipe 内核缓冲区 ]
    │
    ▼
主循环 / event loop（可以安全地做任何事）:
    read(self_pipe[0], &sig, sizeof(sig))
    switch (sig) {
        case SIGTERM:  graceful_shutdown();  break;  // 可以 malloc/加锁 ✅
        case SIGHUP:   reload_config();      break;
        case SIGCHLD:  reap_children();      break;
    }
```

**完整实现**：

```cpp
class SelfPipe {
    int _pipefd[2];
public:
    bool init() {
        if (pipe(_pipefd) == -1) return false;
        fcntl(_pipefd[0], F_SETFL, O_NONBLOCK); // 非阻塞
        fcntl(_pipefd[1], F_SETFL, O_NONBLOCK); // 防止处理器阻塞
        return true;
    }

    int readFd() const { return _pipefd[0]; }

    // 在信号处理器中调用
    void notify(int signum) {
        write(_pipefd[1], &signum, sizeof(signum)); // async-signal-safe
    }

    // 在主循环中调用
    int consume() {
        int sig;
        if (read(_pipefd[0], &sig, sizeof(sig)) == sizeof(sig))
            return sig;
        return -1;
    }
};
```

**为什么 pipe 要设非阻塞？** 信号处理器中 write 不能阻塞 — 如果 pipe 满了，处理器就会卡住，而处理器卡住意味着整个进程无法继续（包括那个唯一能 drain pipe 的主循环）。非阻塞 write 在 pipe 满时返回 EAGAIN，信号丢失但不阻塞。更好的做法是给 pipe 足够的缓冲区。

---

### 7. signalfd 相比 self-pipe 有什么优势？

signalfd 是 Linux 特有的系统调用，把信号变成**文件描述符**。不需要信号处理器 — 直接 `read(fd)` 就能收到信号。

```cpp
// 1. 阻塞信号（不让它走默认/处理器路径）
sigset_t mask;
sigemptyset(&mask);
sigaddset(&mask, SIGTERM);
sigaddset(&mask, SIGINT);
sigprocmask(SIG_BLOCK, &mask, nullptr);

// 2. 创建 signalfd — 被阻塞的信号会"进入"这个 fd
int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

// 3. 在 event loop 中读（可以和 epoll 统一处理）
struct signalfd_siginfo fdsi;
read(sfd, &fdsi, sizeof(fdsi));
// fdsi.ssi_signo → 信号编号
// fdsi.ssi_pid   → 发送者 PID
// fdsi.ssi_uid   → 发送者 UID
```

**signalfd vs self-pipe**：

| | Self-Pipe | signalfd |
|---|---|---|
| **可移植性** | ✅ 所有 Unix | ❌ Linux only |
| **需要信号处理器** | ✅ 需要（write pipe） | ❌ 不需要！ |
| **信号信息** | 只有信号编号 | siginfo_t 完整信息 |
| **丢失风险** | pipe 满时可能丢 | 内核保证不丢（排队在 fd 缓冲） |
| **epoll 集成** | ✅ 读端 fd 可 epoll | ✅ signalfd 可 epoll |
| **复杂度** | 需要 pipe + 处理器 + 非阻塞 | 只需 read(fd) |

**signalfd 的关键设计**：必须先 `sigprocmask` 阻塞信号，否则信号走默认/处理器路径，不会进入 signalfd。signalfd 从被阻塞的 pending 信号中"偷走"信号。

**一句话总结**：能只用 Linux → signalfd（简单、信息全）；需要跨平台 → self-pipe（古老但有效）。

---

### 8. SA_RESTART 有什么用？哪些系统调用不会被自动重启？

当一个进程在**慢系统调用**（可能阻塞的调用，如 `read`、`accept`）中阻塞时收到信号，信号处理器执行完毕后：

- **有 SA_RESTART**：内核自动重启被中断的系统调用（进程感觉不到被打断）
- **无 SA_RESTART**：系统调用返回 -1，`errno = EINTR`（需要手动重启）

```cpp
// 无 SA_RESTART: 必须手动处理 EINTR
ssize_t n;
while ((n = read(fd, buf, size)) < 0) {
    if (errno == EINTR) continue;  // 被信号打断，重试
    break;                          // 真正的错误
}

// 有 SA_RESTART: 内核帮你重试，代码更简洁
ssize_t n = read(fd, buf, size);
```

**SA_RESTART 不能重启的调用**（必须手动处理 EINTR）：

| 系统调用 | 为什么不自动重启 |
|----------|----------------|
| `poll()`, `select()` | 可能已经等待了一段时间，重启要重新计时 |
| `epoll_wait()` | 同上，超时语义 |
| `sleep()` | 返回剩余秒数，让调用者决定 |
| `nanosleep()` | 返回剩余时间 |
| `recv()`, `send()` | 带超时的 socket 调用 |

**处理 EINTR 的标准模式**：

```cpp
while ((n = read(fd, buf, size)) < 0) {
    if (errno == EINTR) continue;
    perror("read");
    break;
}
```

---

### 9. SIGTERM vs SIGKILL vs SIGINT 的使用场景？

| 信号 | 编号 | 可捕获？ | 典型用途 |
|------|------|----------|----------|
| **SIGINT** | 2 | ✅ | 用户按 Ctrl+C，交互式终止 |
| **SIGTERM** | 15 | ✅ | systemd/docker/k8s 的默认停止信号，要求进程优雅退出 |
| **SIGKILL** | 9 | ❌ | 必杀，内核直接销毁进程，管理员最后的手段 |
| **SIGHUP** | 1 | ✅ | 终端断开；守护进程约定为 reload 配置 |
| **SIGQUIT** | 3 | ✅ | Ctrl+\，和 SIGINT 类似但产生 core dump |

**优雅退出流程（所有服务程序的标准）**：

```
systemd / docker 要停止服务:
  1. 发送 SIGTERM → 给进程清理资源的机会
     - 关闭监听 socket（不再接受新连接）
     - 等待正在处理的请求完成
     - flush 日志
     - 关闭数据库连接
  2. 等待超时（默认 90s）→ 还没退出？
  3. 发送 SIGKILL → 不废话，直接杀
```

**SIGINT vs SIGTERM**：
- `SIGINT` = "用户在终端按了 Ctrl+C" → 交互式程序用
- `SIGTERM` = "系统希望你退出" → 服务/守护进程用

所以生产代码应该同时处理这两个信号退出：

```cpp
sigaction(SIGTERM, &sa, nullptr); // docker stop
sigaction(SIGINT, &sa, nullptr);  // Ctrl+C (开发时)
```

---

### 10. 信号处理中常见的 race condition？

**Race 1：信号处理器检查和修改全局变量**

```cpp
// ❌ 竞态！
volatile sig_atomic_t flag = 0;

void handler(int) { flag = 1; }

int main() {
    if (flag == 0) {          // ← 检查
        // ← 信号可能在这中间到达！flag 变成 1
        pause();              // ← 但已经过了检查，pause 会永远阻塞
    }
}
```

**解决**：用 `sigsuspend` / `pselect` / `epoll_pwait` 原子地解除阻塞 + 等待。

```cpp
// ✅ sigsuspend: 原子地替换信号掩码并暂停
sigset_t mask, old;
sigprocmask(SIG_BLOCK, &mask, &old);
// ... 临界区 ...
sigsuspend(&old); // 原子操作: 解除阻塞 + 暂停等待信号
```

**Race 2：fork 和信号处理器的交互**

```cpp
// ❌ fork 后子进程继承父进程的信号处理器
// 但如果信号处理器操作全局状态，子进程也有那份状态
```

**解决**：fork 后子进程应该重置信号处理器，或者直接用 `_exit()`。

**Race 3：waitpid 和 SIGCHLD 的竞态**

```cpp
// 父进程: 注册 SIGCHLD 处理器 → 然后创建子进程
// 子进程: 可能在父进程注册处理器之前就退出了
```

**解决**：先阻塞 SIGCHLD，注册处理器，创建子进程，再解除阻塞。

**Race 4：self-pipe write 时 pipe 满了**

非阻塞 write 在 pipe 满时返回 EAGAIN → 信号丢失。缓解：增大 pipe 缓冲区（`fcntl(fd, F_SETPIPE_SZ, 65536)`），或用 signalfd。

**总则**：信号处理中，任何"先检查再操作"的模式都天然有竞态。唯一的正确做法是用**原子的**系统调用（sigsuspend, pselect, signalfd 等）一次性完成"等待 + 解除阻塞"。

---

## Week 10 — 多线程

### 1. std::thread 创建后为什么必须 join 或 detach？

`std::thread` 对象在析构时如果 `joinable()` 返回 true（即线程还在运行且未被 join 也未 detach），析构函数会调用 `std::terminate()` → 程序崩溃。

```cpp
{
    std::thread t([](){ do_work(); });
    // 忘记 join() 或 detach() → t 析构 → terminate!
}
```

**两个选择**：
- **join()**：等待线程完成，获取其结果（如果有的话），线程结束后 `std::thread` 对象变为非 joinable
- **detach()**：线程与 `std::thread` 对象脱钩，独立运行，对象变为非 joinable。但之后无法再控制或等待它

> C++20 的 `std::jthread` 解决了这个问题：析构时自动 join，并支持 `stop_token` 优雅停止。

### 2. 为什么数据竞争 (data race) 是未定义行为？

两个或多个线程同时访问同一内存位置，且**至少有一个是写操作**，且**没有同步机制** → 未定义行为。

```
线程 A: counter = counter + 1   // 读 → +1 → 写（3 条 CPU 指令）
线程 B: counter = counter + 1   // 交错执行 → 丢失一次计数
```

**CPU 层面**：`++i` 不是原子的。它是三条指令的复合操作（load → add → store），线程可能在任意一条之间被切换。

**编译器层面**：编译器有权做激进的优化，在单线程视角合法的重排，在多线程中可能导致看到半初始化的对象。

**解决**：`std::mutex`（锁）或 `std::atomic`（原子操作）。

### 3. lock_guard vs unique_lock vs scoped_lock 的区别？

| | lock_guard | unique_lock | scoped_lock (C++17) |
|---|---|---|---|
| **加锁时机** | 构造时立即加锁 | 可延迟 (`defer_lock`) | 构造时立即加锁 |
| **手动解锁** | ❌ 不能 | ✅ lock()/unlock() | ❌ 不能 |
| **移动语义** | ❌ 不可移动 | ✅ 可移动 | ❌ 不可移动 |
| **配合条件变量** | ❌ 不能 | ✅ 可以 | ❌ 不能 |
| **多 mutex 死锁避免** | ❌ | ❌ | ✅ 自动用 std::lock |
| **开销** | 最小 | 多一个 bool (owns) | 同 lock_guard |

**选择指南**：
- 简单的临界区保护 → `lock_guard`
- 需要配合 `condition_variable` → `unique_lock`
- 需要手动 unlock 或延迟加锁 → `unique_lock` + `defer_lock`
- 同时锁多个 mutex → `scoped_lock`

### 4. condition_variable 的 wait 为什么必须有「谓词」？

因为**虚假唤醒 (spurious wakeup)** — OS 可能在没有 `notify` 的情况下唤醒 `wait` 中的线程。

```cpp
// ❌ 错误：没有循环检查
cv.wait(lock);

// ✅ 正确：带谓词的 wait
cv.wait(lock, []{ return ready; });
// 等价于: while (!ready) { cv.wait(lock); }
```

**notify_one vs notify_all**：
- `notify_one()`：唤醒**一个**等待的线程（不确定是哪个）— 适合每个任务只需一个消费者
- `notify_all()`：唤醒**所有**等待的线程 — 适合状态变化对所有等待者都有意义（如 shutdown）

### 5. std::atomic 和 mutex 的选择边界在哪？

| | atomic | mutex |
|---|---|---|
| **适用范围** | 单个变量 / 简单操作 | 多变量 / 复杂临界区 |
| **性能** | 极高（CPU 指令级，无锁） | 有锁开销 + 可能的上下文切换 |
| **阻塞** | 从不阻塞 | 会阻塞等待的线程 |
| **组合操作** | CAS (compare_exchange) | 任意复杂操作 |

**选择规则**：
1. 单个 `int`/`bool`/`指针` 的读写/加减 → `atomic`
2. 多个变量需要一致性（如转账：A 减钱同时 B 加钱）→ `mutex`
3. 读多写少 → `shared_mutex`（读写锁）
4. 需要等待条件 → `condition_variable` + `mutex`

**Memory Order 快速指南**：
- 默认 `seq_cst` — 永远正确，入门首选
- `acquire-release` — 生产者-消费者场景，更快
- `relaxed` — 仅计数器等无依赖场景

### 6. async / future / promise 之间是什么关系？

```
promise         →  future       ← async / packaged_task
(手动 set)          (被动 get)     (自动)
```

- **`std::async`**：启动一个异步任务，返回 `std::future`。最简方式。
- **`std::promise`**：手动在某个时刻 `set_value()`，另一端通过 `future.get()` 获取。
- **`std::future`**：只读的"未来值"句柄，`get()` 阻塞直到结果就绪。
- **`std::shared_future`**：可以被多次 `get()`、多个线程同时等待。
- **`std::packaged_task`**：把可调用对象包装成 promise-like 的 task。

**`std::launch::async` vs `std::launch::deferred`**：
- `async`：在新线程中执行
- `deferred`：推迟到 `get()` 被调用时才在当前线程执行（懒求值）

### 7. 死锁的四个必要条件是什么？如何打破？

死锁四条件（四个全部满足才发生）：

1. **互斥**：资源不能被共享
2. **持有并等待**：持有一个锁，等待另一个锁
3. **不可抢占**：锁只能由持有者释放
4. **循环等待**：A 等 B，B 等 A → 形成环

**打破方法**：

| 打破条件 | 方法 |
|---------|------|
| 互斥 | 用 atomic 或 lock-free 数据结构替代 |
| 持有并等待 | 一次性获取所有锁 (`std::scoped_lock`) |
| 不可抢占 | 用 `try_lock`，失败就释放已持有的 |
| 循环等待 | 所有线程按相同顺序加锁（最实用） |

### 8. thread_local 变量存在哪？和 static 有什么区别？

`thread_local` 变量存储在线程的 TLS (Thread-Local Storage) 区域，每个线程有独立的副本。

| | static | thread_local |
|---|---|---|
| **副本数** | 全局唯一 | 每线程一份 |
| **同步需求** | 需要 mutex/atomic | 无需同步 |
| **生命周期** | 程序开始→结束 | 线程开始→结束 |
| **用途** | 全局状态、单例 | per-thread 缓存、上下文 |

**典型用途**：`errno` 就是 thread_local 的（在现代系统中），每个线程有自己独立的错误码。

### 9. C++20 新增了哪些线程同步原语？

| 原语 | 用途 | 典型场景 |
|------|------|----------|
| **`std::jthread`** | 自动 join + stop_token | 替代 std::thread |
| **`std::latch`** | 一次性倒计数门闩 | 等待所有线程就绪 |
| **`std::barrier`** | 可复用的阶段同步点 | 分阶段并行算法 |
| **`std::counting_semaphore`** | 限制并发访问数量 | 连接池、限流 |
| **`std::binary_semaphore`** | 互斥（无所有权） | 轻量级信号 |

**`std::stop_token` 模式**：

```cpp
std::jthread worker([](std::stop_token stoken) {
    while (!stoken.stop_requested()) {
        // 干活
    }
    // 清理
});
worker.request_stop(); // 请求停止
worker.join();
```

### 10. 多线程编程的核心安全法则

1. **能不共享就不共享** — message passing、per-thread data、channel
2. **必须共享时，优先 atomic** — 更轻量、无死锁风险
3. **atomic 不够用才加锁** — 锁的粒度尽量小
4. **锁的嵌套是死锁的温床** — 用 `scoped_lock` 或统一加锁顺序
5. **用 condition_variable 而不是 busy-waiting**
6. **用 RAII 锁保证异常安全** — `lock_guard` / `unique_lock` / `scoped_lock`
7. **新代码用 `std::jthread`** — 不会忘记 join
8. **线程安全第一原则：不要有多个线程同时写同一块数据**
