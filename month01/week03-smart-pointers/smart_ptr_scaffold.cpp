// Day 4: Smart Pointers — 告别 new/delete
// 编译: cmake -B build && cmake --build build
// 运行: ./build/smartptr

#include <algorithm>
#include <atomic>
#include <ios>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using std::cout;
using std::endl;
using std::make_shared;
using std::make_unique;
using std::shared_ptr;
using std::unique_ptr;
// ============================================================
// 练习 1: std::unique_ptr — 独占所有权，零开销
// ============================================================
void exercise1_unique_ptr() {
  std::cout << "=== 练习 1: unique_ptr ===\n";

  // TODO 1.1: 用 std::make_unique<int>(42) 创建一个 unique_ptr
  // 然后打印 *p1
  // YOUR CODE HERE
  auto p1 = make_unique<int>(42);

  // TODO 1.2: 把 p1 的所有权转移给 p2（用 std::move）
  // 检查 p1 是否为空（p1 == nullptr 或 !p1）
  // YOUR CODE HERE
  decltype(p1) p2{};
  if (p1 != nullptr) {
    p2 = std::move(p1);
  }
  // TODO 1.3: 创建管理数组的 unique_ptr（用 make_unique<int[]>(5)）
  // 给 arr[0] 赋值 100, arr[4] 赋值 500，并打印验证
  // YOUR CODE HERE
  auto p3 = make_unique<int[]>(5);
  p3[0] = 100;
  p3[4] = 500;
  std::cout << "p3[0] : " << p3[0] << " p3[4] : " << p3[4] << std::endl;
  // TODO 1.4: 体会「独占」— 试试把 p2 直接赋值给 p3（不加 move）
  // 编译会报什么错？读懂错误信息后注销这行
  // auto p3 = p2;  // 编译错误！
}

// ============================================================
// 练习 2: std::shared_ptr — 共享所有权 + 引用计数
// ============================================================
void exercise2_shared_ptr() {
  std::cout << "\n=== 练习 2: shared_ptr ===\n";

  // TODO 2.1: 用 std::make_shared<std::string>("hello") 创建 shared_ptr
  // 打印 *s1 和 s1.use_count()
  // YOUR CODE HERE
  auto p1 = make_shared<std::string>("hello");
  cout << "p1 : " << p1 << " : " << p1.use_count() << endl;

  // TODO 2.2: 把 s1 拷贝给 s2（不需要 move，shared_ptr 可以拷贝）
  // 观察 use_count 变为 2
  // YOUR CODE HERE
  auto p2 = p1;
  cout << "p2 : " << *p2 << " : " << p2.use_count() << endl;

  // TODO 2.3: 用花括号 { } 创建一个内层作用域
  // 在内层拷贝 s1 给 s3，观察计数为 3
  // 离开内层作用域后，观察计数降回 2
  // YOUR CODE HERE
  {
    auto p3 = p1;
    cout << "p3 : " << *p3 << " : " << p3.use_count() << endl;
  }

  cout << "p2 : " << *p2 << " : " << p2.use_count() << endl;
}

// ============================================================
// 练习 3: std::weak_ptr — 打破循环引用
// ============================================================
// 场景: Person 拥有一辆车，Car 有一个车主 → 互相引用

struct Car; // 前置声明

struct Person {
  std::string _name;
  shared_ptr<Car> _car;
  // TODO 3.1: 选择合适的类型 —
  //   方案 A: std::shared_ptr<Car> _car;   // 会造成循环引用！
  //   方案 B: std::weak_ptr<Car> _car;     // 不增加引用计数
  // 先用方案 A 观察问题，再改成方案 B 修复
  // YOUR CODE HERE

  Person(const std::string &name) : _name(name) {}
  ~Person() { std::cout << "  Person(" << _name << ") 析构\n"; }

  void drive_car(); // 定义移到 Car 之后
};

struct Car {
  std::string _model;

  // TODO 3.2: Car 拥有 Person 的引用 — 用 shared_ptr 还是 weak_ptr？
  // 提示: 这里是"[车]拥有[人]"的关系，一个人可以没有车，车一定有个车主
  // YOUR CODE HERE
  std::weak_ptr<Person> _owner;

  Car(const std::string &model) : _model(model) {}
  ~Car() { std::cout << "  Car(" << _model << ") 析构\n"; }
};

void exercise3_weak_ptr() {
  std::cout << "\n=== 练习 3: weak_ptr 打破循环引用 ===\n";

  std::cout << "--- 测试循环引用 ---\n";
  // TODO 3.3: 创建 Person("Alice") 和 Car("Tesla")
  // 让它们互相引用，然后离开作用域
  // 观察: 析构函数被调用了吗？
  {
    // YOUR CODE HERE
    auto p = make_shared<Person>("Alice");
    auto c = make_shared<Car>("Tesla");
    p->_car = c;
    c->_owner = p;
  }
  std::cout << "  已离开作用域 ← 上面应该看到析构消息\n";
}

// ============================================================
// 练习 4: weak_ptr::lock() — 安全「升级」为 shared_ptr
// ============================================================
void exercise4_weak_lock() {
  std::cout << "\n=== 练习 4: weak_ptr::lock() ===\n";

  std::weak_ptr<int> wp;

  // TODO 4.1: 创建内层作用域，在里面:
  //   1. 创建一个 shared_ptr<int> 指向值 99
  //   2. 让 wp 观察它
  //   3. 用 wp.lock() 获取 shared_ptr，检查是否为空，打印值
  // YOUR CODE HERE
  {

    auto p = make_shared<int>(99);
    wp = p;
    if (wp.lock() != nullptr) {
      cout << *wp.lock() << endl;
    }
  }
  // TODO 4.2: 离开内层作用域后（shared_ptr 已销毁）
  //   用 wp.lock() 再试一次，检查返回的 shared_ptr 是否为空
  //   用 wp.expired() 检查是否已过期
  // YOUR CODE HERE
  if (wp.lock() == nullptr) {
    cout << std::boolalpha << "shared_ptr 为空 wp.expired : " << wp.expired()
         << std::noboolalpha << endl;
  }
}

// ============================================================
// 练习 5: 自定义删除器
// ============================================================
void exercise5_custom_deleter() {
  std::cout << "\n=== 练习 5: 自定义删除器 ===\n";

  // TODO 5.1: unique_ptr 带自定义删除器
  // 创建 unique_ptr<int, void(*)(int*)>，删除器打印 "deleting N"
  // 提示: auto deleter = [](int *p) { std::cout << "deleting " << *p << "\n";
  // delete p; };
  //       unique_ptr<int, decltype(deleter)> p(new int(42), deleter);
  // 或者更简单: 用 std::function<void(int*)> 或 void(*)(int*)
  // YOUR CODE HERE
  auto deleter = [](int *p) -> void {
    cout << "deleting " << *p << "\n";
    delete p;
  };
  unique_ptr<int, decltype(deleter)> p(new int(42), deleter);
  // TODO 5.2: no-op 删除器 — 观察但不是所有者
  // 场景: 有一个栈变量 int x = 10，想用 unique_ptr 包装它但不想 delete
  // 提示: lambda [](int*){} — 什么都不做
  // YOUR CODE HERE
  int x{10};
  auto noop = [](int *) { ; };
  std::unique_ptr<int, decltype(noop)> p2(&x, noop);
}

// ============================================================
// 练习 6 (综合): 用智能指针构建树结构
// 父节点拥有子节点 (unique_ptr)，子节点用裸指针观察父节点
// ============================================================
struct TreeNode {
  std::string _name;

  // 6.1: children 用 unique_ptr（独占），parent 用裸指针（观察）
  std::vector<std::unique_ptr<TreeNode>> _children;
  TreeNode *_parent = nullptr;

  TreeNode(const std::string &name) : _name(name) {}
  ~TreeNode() { std::cout << "  TreeNode(" << _name << ") 析构\n"; }

  // TODO 6.2: 实现 add_child
  void add_child(const std::string &name) {
    // 1. 用 std::make_unique<TreeNode>(name) 创建子节点
    // 2. 设置子节点的 _parent = this
    // 3. push_back 到 _children
    // YOUR CODE HERE
    auto child = make_unique<TreeNode>(name);
    child->_parent = this;
    _children.push_back(std::move(child));
  }

  void print(int depth = 0) const {
    for (int i = 0; i < depth; ++i)
      std::cout << "  ";
    std::cout << _name;
    if (_parent)
      std::cout << " (parent: " << _parent->_name << ")";
    std::cout << "\n";
    for (const auto &child : _children) {
      child->print(depth + 1);
    }
  }
};

void exercise6_tree() {
  std::cout << "\n=== 练习 6: 智能指针管理树结构 ===\n";

  // 构建树:
  //   root
  //   ├── child1
  //   │   └── grandchild
  //   └── child2

  auto root = std::make_unique<TreeNode>("root");

  // TODO 6.3: 用 add_child 构建上面的树
  // YOUR CODE HERE
  root->add_child("child1");
  root->add_child("child2");
  auto it =
      std::find_if(root->_children.begin(), root->_children.end(),
                   [](const auto &child) { return child->_name == "child1"; });
  (*it)->add_child("grandchild");
  std::cout << "树结构:\n";
  root->print();

  std::cout << "\n离开作用域，自动销毁整棵树:\n";
  // root 析构 → 递归析构所有子节点 → 无需手动 delete
}

// ============================================================
// 总结思考（不写代码，理解即可）:
// 1. unique_ptr 为什么不能拷贝？
// 2. shared_ptr 的引用计数存在哪里？（提示: 控制块）
// 3. make_shared 比 new shared_ptr 好在哪？
// 4. 什么场景「必须」用 weak_ptr？
// 5. 裸指针在智能指针时代还有用吗？
// 6. sizeof(unique_ptr) vs sizeof(shared_ptr) 哪个大？为什么？
// ============================================================

int main() {
  exercise1_unique_ptr();
  exercise2_shared_ptr();
  exercise3_weak_ptr();
  exercise4_weak_lock();
  exercise5_custom_deleter();
  exercise6_tree();

  std::cout << "\n全部练习完成！\n";
  return 0;
}
