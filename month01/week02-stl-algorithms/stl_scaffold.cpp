// Day 3: STL Algorithms — 告别手写循环
// 编译: cmake -B build && cmake --build build
// 运行: ./build/stl

#include <algorithm>
#include <cctype>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <string>
#include <vector>

// ============================================================
// 练习 1: 热身 — vector + 基础算法
// 用 STL 算法替代手写循环
// ============================================================
void exercise1_sort_and_find() {
  std::cout << "=== 练习 1: sort + find_if ===\n";

  std::vector<int> v = {42, 7, 13, 99, 3, 17, 56, 23};

  // TODO 1.1: 用 std::sort 对 v 升序排序
  // YOUR CODE HERE
  std::sort(v.begin(), v.end());

  std::cout << "排序后: ";
  for (int x : v)
    std::cout << x << " ";
  std::cout << "\n";

  // TODO 1.2: 用 std::find_if 找到第一个 > 50 的元素，打印它
  // 提示: 需要 lambda [](int x) { return x > 50; }
  // YOUR CODE HERE
  auto it = std::find_if(v.begin(), v.end(), [](int x) { return x > 50; });
  std::cout << "first number greater 50: " << *it << std::endl;
  // TODO 1.3: 用 std::count_if 统计有多少个偶数
  // YOUR CODE HERE
  int x = std::count_if(v.begin(), v.end(), [](int x) { return x % 2 == 0; });
  std::cout << "偶数个数: " << x << std::endl;
}

// ============================================================
// 练习 2: transform + accumulate — 数据转换与归约
// ============================================================
void exercise2_transform_accumulate() {
  std::cout << "\n=== 练习 2: transform + accumulate ===\n";

  std::vector<int> nums = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  // TODO 2.1: 用 std::transform 把 nums 每个元素平方，存入 squared
  std::vector<int> squared(nums.size());
  // YOUR CODE HERE
  std::transform(nums.begin(), nums.end(), squared.begin(),
                 [](int x) { return x * x; });

  std::cout << "平方: ";
  for (int x : squared)
    std::cout << x << " ";
  std::cout << "\n";

  // TODO 2.2: 用 std::accumulate 求 squared 的总和
  // 提示: 第三个参数是初始值 0，在 <numeric> 头文件中
  // YOUR CODE HERE
  auto sum = std::accumulate(squared.begin(), squared.end(), 0);
  std::cout << "平方和: " << sum << std::endl;

  // TODO 2.3: 用 std::transform + ::toupper 把字符串转大写
  // 提示: ::toupper 需要 <cctype>，且需要处理 unsigned char 陷阱
  //       可以用 lambda: [](unsigned char c) { return std::toupper(c); }
  std::string s = "Hello Modern C++";
  // YOUR CODE HERE
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  std::cout << "大写: " << s << "\n";
}

// ============================================================
// 练习 3: erase-remove 惯用法 — 正确删除容器元素
// ============================================================
void exercise3_erase_remove() {
  std::cout << "\n=== 练习 3: erase-remove 惯用法 ===\n";

  // 需求: 删除 vector 中所有的奇数
  std::vector<int> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  // TODO 3.1: 用 erase-remove 惯用法删除所有奇数
  // 步骤: 先用 std::remove_if 把奇数移到末尾，再用 v.erase 真正删除
  // std::remove_if 返回指向"新末尾"的迭代器
  // YOUR CODE HERE
  auto it =
      std::remove_if(v.begin(), v.end(), [](int x) { return x % 2 != 0; });
  v.erase(it, v.end());

  std::cout << "删除奇数后: ";
  for (int x : v)
    std::cout << x << " ";
  std::cout << "\n";

  // 删除特定值 4
  auto it2 = std::remove(v.begin(), v.end(), 4);
  v.erase(it2, v.end());
  // YOUR CODE HERE
  std::cout << "删除4后: ";
  for (int x : v)
    std::cout << x << " ";
  std::cout << "\n";
}

// ============================================================
// 练习 4: std::map + 算法 — 词频统计
// ============================================================
void exercise4_word_frequency() {
  std::cout << "\n=== 练习 4: 词频统计 ===\n";

  std::vector<std::string> words = {"apple",     "banana", "apple", "orange",
                                    "banana",    "banana", "grape", "orange",
                                    "pineapple", "apple"};

  // TODO 4.1: 用 std::map 统计每个单词出现的次数
  // 提示: map<string, int>，遍历 words，对每个词 map[word]++
  // YOUR CODE HERE
  std::map<std::string, int> freq{};
  for (auto &s : words) {
    freq[s]++;
  }

  // TODO 4.2: 用 std::max_element 找到出现次数最多的单词
  // 提示: map 的 value_type 是 pair<const string, int>，
  //       比较 .second，也就是 pair 的第二个元素
  // YOUR CODE HERE
  auto max_it = std::max_element(
      freq.begin(), freq.end(),
      [](const auto &a, const auto &b) { return a.second < b.second; });

  // 打印结果 (先完成 4.1 和 4.2)
  for (auto &[word, count] : freq) {
    std::cout << word << " : " << count << std::endl;
  }
  std::cout << "最多: " << max_it->first << " (" << max_it->second << " 次)\n";
}

// ============================================================
// 练习 5 (进阶): std::copy_if + std::for_each
// 从数据中筛选并处理
// ============================================================
void exercise5_filter_and_process() {
  std::cout << "\n=== 练习 5: copy_if + for_each ===\n";

  struct Person {
    std::string name;
    int age;
  };

  std::vector<Person> people = {
      {"Alice", 25}, {"Bob", 17}, {"Charlie", 30},
      {"Diana", 22}, {"Eve", 16}, {"Frank", 45},
  };

  // TODO 5.1: 用 std::copy_if 将 age >= 18 的人复制到 adults
  // 提示: 用 std::back_inserter(adults) 作为输出迭代器
  std::vector<Person> adults;
  // YOUR CODE HERE
  std::copy_if(people.begin(), people.end(), std::back_inserter(adults),
               [](const Person &p) { return p.age >= 18; });

  std::cout << "成年人: ";
  for (const auto &p : adults) {
    std::cout << p.name << "(" << p.age << ") ";
  }
  std::cout << "\n";

  // TODO 5.2: 用 std::for_each 给每个成年人的 age +1
  // 提示: lambda 参数用引用 Person& p
  // YOUR CODE HERE
  std::for_each(adults.begin(), adults.end(), [](Person &p) { p.age++; });

  std::cout << "一年后: ";
  for (const auto &p : adults) {
    std::cout << p.name << "(" << p.age << ") ";
  }
  std::cout << "\n";
}

// ============================================================
int main() {
  exercise1_sort_and_find();
  exercise2_transform_accumulate();
  exercise3_erase_remove();
  exercise4_word_frequency();
  exercise5_filter_and_process();

  std::cout << "\n全部练习完成！\n";
  return 0;
}
