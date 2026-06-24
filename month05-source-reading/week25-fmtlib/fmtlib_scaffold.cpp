// ============================================================================
// Month 5 Week 25: fmtlib 源码阅读
// 日期: 2026-06-24
//
// 阅读目标: {fmt} — 现代 C++ 格式化库 (C++20 std::format 的基础)
// 源码位置: ../fmt/
// 源码规模: ~16,600 行 (include/fmt/ 15 headers + src/ 4 .cc files)
//
// {fmt} 是 Victor Zverovich 开发的格式化库, 已被采纳为 C++20 <format>。
// 特点是: 类型安全、编译期格式检查、极致的运行时性能。
//
// 核心架构:
//   format("{} + {} = {}", a, b, a+b)
//     → format_string 解析 → format_arg_store 类型擦除
//     → vformat_to(OutputIt, fmt, format_args)
//       → parse_format_specs (解析每个 {} 的格式说明)
//       → visit_format_arg (类型分派)
//         → formatter<T>::format (具体类型格式化)
//           → write_int / write_float / write_padded (底层写入)
//
// 10 个练习, 按处理管道顺序: 格式解析→类型擦除→格式化→输出→编译期
// ============================================================================

#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace std::literals;

// ============================================================================
// 辅助工具
// ============================================================================

template <typename... Args>
void println(Args&&... args) {
  if constexpr (sizeof...(args) > 0)
    ((std::cout << std::forward<Args>(args)), ...);
  std::cout << '\n';
}
template <typename... Args>
void print(Args&&... args) {
  ((std::cout << std::forward<Args>(args)), ...);
}
void HR(std::string_view title = "") {
  println("\n", std::string(72, '='), "\n  ", title, "\n", std::string(72, '='));
}

// ── 共用类型 (多个 exercise 依赖) ──────────────────────────────────
enum class Align : uint8_t { none = 0, left = 1, right = 2, center = 3, numeric = 4 };
enum class Sign : uint8_t { none = 0, plus = 1, minus = 2, space = 3 };

struct FormatSpecs {
  Align align = Align::none;
  Sign sign = Sign::none;
  bool alt_form = false;
  bool zero_pad = false;
  int width = 0;
  int precision = -1;

  bool has_width() const { return width > 0; }
  bool has_precision() const { return precision >= 0; }
};

// write_int 声明 (ex4 实现, ex10 引用)
namespace ex4 { std::string write_int(uint64_t value, int base, bool upper); }

// ============================================================================
// Exercise 1: fmtlib 概览
// ============================================================================
//
// 【阅读清单】
//   README.rst            — 项目概述, 示例
//   include/fmt/format.h  — 主头文件
//   include/fmt/base.h    — 基础工具和 API
//   include/fmt/core.h    — (旧版) 核心
//
// 【关键概念】
//   fmtlib 解决的核心问题: C 的 printf 不安全 (类型不匹配=UB),
//   iostream 太慢 (虚函数 + locale + 多次函数调用),
//   fmtlib = 类型安全 + 极速 + 可扩展。
//
//   API 层次:
//     Layer 1: fmt::format / fmt::print  (最常用)
//     Layer 2: fmt::format_to / fmt::format_to_n (输出迭代器)
//     Layer 3: fmt::formatter<T> 定制 (扩展新类型)
//     Layer 4: visit_format_arg / parse_format_specs (底层)
//
//   核心数据流:
//     format("x={} y={:.2f}", 42, 3.14)
//       → format_string<"x={} y={:.2f}"> (编译期解析)
//       → make_format_args(42, 3.14) → format_arg_store (类型擦除)
//       → vformat_to(buf, fmt.str, args)
//         → for each {}:
//             parse_format_specs → specs
//             visit_format_arg → call formatter<T>::format(out, specs, arg)

namespace ex1 {

void run() {
  HR("Ex1: fmtlib 概览");

  println("fmtlib 核心文件:");
  println("  include/fmt/base.h    — 基础工具, 格式说明解析, format_args");
  println("  include/fmt/format.h  — 格式化实现, format_to, write_int/float");
  println("  include/fmt/format-inl.h — 内联实现 (整数/浮点数格式化)");
  println("  include/fmt/args.h    — 参数存储");
  println("  include/fmt/compile.h — 编译期格式字符串");
  println("  include/fmt/chrono.h  — 时间格式化");
  println("  include/fmt/color.h   — 终端颜色");
  println("  include/fmt/ranges.h  — 容器格式化");
  println("  src/format.cc         — 浮点格式化 (Dragonbox), grisu");
  println();

  println("API 对比:");
  println("  printf:  不安全, 运行时格式, 快");
  println("  iostream: 类型安全, 运行时格式, 慢 (虚函数+locale)");
  println("  fmtlib:   类型安全, 可编译期检查, 快 (接近printf)");
  println();

  println("📖 阅读顺序:");
  println("  base.h: format_string → parse_format_specs → format_arg → visit_format_arg");
  println("  format.h: write_int → write_float → write_padded → format_handler");
  println("  args.h:   format_arg_store → make_format_args");
  println("  compile.h: FMT_COMPILE → compile-time format checking");
}

} // namespace ex1

// ============================================================================
// Exercise 2: Format String Parsing — 格式说明解析
// ============================================================================
//
// 【阅读清单】
//   base.h:  line 645-680 — presentation_type, align, sign 枚举
//   base.h:  line 1440+   — parse_format_specs 状态机
//   format.h: line 1714+  — write_padded (使用 specs)
//
// 【关键设计】
//   格式说明语法: {[arg_id][:fill align sign # 0 width .precision type]}
//   例: "{:>10.2f}" → align=right, width=10, precision=2, type=float
//
//   状态机解析: start → align → sign → hash → zero → width → precision → locale
//   每个状态读取一个"字段", 然后转移到下一状态。
//
//   presentation_type (默认值映射):
//     int → 'd', unsigned → 'd', float → 'g', bool → 's',
//     char → 'c', string → 's', pointer → 'p'

namespace ex2 {

// ── 格式说明的简化表示 ────────────────────────────────────────────
enum class PresType : uint8_t {
  none = 0, dec = 1, hex_lower = 2, hex_upper = 3, bin_lower = 4, bin_upper = 5, oct = 6,
  exp_lower = 7, exp_upper = 8, fixed_lower = 9, fixed_upper = 10,
  general_lower = 11, general_upper = 12, chr = 13, string = 14, pointer = 15
};

struct ParsedFormatSpec {
  Align align = Align::none;
  Sign sign = Sign::none;
  bool alt_form = false;
  bool zero_pad = false;
  int width = 0;
  int precision = -1;
  PresType type = PresType::none;
  char fill = ' ';

  bool has_width() const { return width > 0; }
  bool has_precision() const { return precision >= 0; }
};

// ── 简化的格式说明解析器 (状态机) ──────────────────────────────────
enum class ParseState { start, align, sign, hash, zero, width, precision, locale, done };

struct FormatSpecParser {
  const char* _p;
  const char* _end;

  explicit FormatSpecParser(std::string_view fmt) : _p(fmt.data()), _end(fmt.data() + fmt.size()) {}

  bool at_end() const { return _p >= _end; }
  char peek() const { return at_end() ? '\0' : *_p; }
  char next() { return at_end() ? '\0' : *_p++; }

  ParsedFormatSpec parse() {
    ParsedFormatSpec spec;
    ParseState state = ParseState::start;

    while (!at_end()) {
      char c = peek();
      switch (state) {
      case ParseState::start:
        if (!at_end_with(1) && is_align_char(_p[1])) {
          spec.fill = next(); c = next();
          spec.align = char_to_align(c);
          state = ParseState::sign;
        } else if (is_align_char(c)) {
          spec.align = char_to_align(next());
          state = ParseState::sign;
        } else { state = ParseState::sign; }
        break;
      case ParseState::sign:
        if (c == '+') { spec.sign = Sign::plus; next(); }
        else if (c == '-') { spec.sign = Sign::minus; next(); }
        else if (c == ' ') { spec.sign = Sign::space; next(); }
        state = ParseState::hash; break;
      case ParseState::hash:
        if (c == '#') { spec.alt_form = true; next(); }
        state = ParseState::zero; break;
      case ParseState::zero:
        if (c == '0') { spec.zero_pad = true; next(); }
        state = ParseState::width; break;
      case ParseState::width:
        if (c >= '0' && c <= '9') { spec.width = parse_int(); }
        else if (c == '{') { while (!at_end() && next() != '}') {} }
        state = ParseState::precision; break;
      case ParseState::precision:
        if (c == '.') { next(); spec.precision = parse_int(); }
        state = ParseState::locale; break;
      case ParseState::locale:
        if (c == 'L') { next(); }
        state = ParseState::done; break;
      case ParseState::done:
        spec.type = char_to_pres_type(next());
        return spec;
      }
    }
    return spec;
  }

private:
  bool at_end_with(int offset) const { return _p + offset >= _end; }
  static bool is_align_char(char c) { return c == '<' || c == '>' || c == '^'; }
  static Align char_to_align(char c) {
    switch (c) { case '<': return Align::left; case '>': return Align::right;
                 case '^': return Align::center; default: return Align::none; }
  }
  static PresType char_to_pres_type(char c) {
    switch (c) {
    case 'd': return PresType::dec; case 'x': return PresType::hex_lower;
    case 'X': return PresType::hex_upper; case 'b': return PresType::bin_lower;
    case 'B': return PresType::bin_upper; case 'o': return PresType::oct;
    case 'e': return PresType::exp_lower; case 'E': return PresType::exp_upper;
    case 'f': return PresType::fixed_lower; case 'F': return PresType::fixed_upper;
    case 'g': return PresType::general_lower; case 'G': return PresType::general_upper;
    case 'c': return PresType::chr; case 's': return PresType::string;
    case 'p': return PresType::pointer;
    default: return PresType::none;
    }
  }
  int parse_int() {
    int val = 0;
    while (!at_end() && *_p >= '0' && *_p <= '9') val = val * 10 + (next() - '0');
    return val;
  }
};

void run() {
  HR("Ex2: Format String Parsing");

  // 测试各种格式说明
  std::vector<std::pair<std::string_view, std::string_view>> tests = {
    {"d", "integer decimal"},
    {">10d", "right align, width 10, int"},
    {"<10.2f", "left align, width 10, precision 2, float"},
    {"^#08x", "center align, alt form, zero pad, width 8, hex lower"},
    {"+020.5e", "plus sign, zero pad, width 20, prec 5, exp lower"},
    {"1.3g", "width 1, precision 3, general"},
    {".2f", "precision 2, fixed"},
    {"s", "string type"},
  };

  for (auto [fmt_str, desc] : tests) {
    FormatSpecParser parser(fmt_str);
    auto s = parser.parse();
    print("  \"{:>10}\" →", fmt_str);
    if (s.fill != ' ') print(" fill='", s.fill, "'");
    if (s.align != Align::none) print(" align=", (int)s.align);
    if (s.sign != Sign::none) print(" sign=", (int)s.sign);
    if (s.alt_form) print(" alt");
    if (s.zero_pad) print(" zero");
    if (s.has_width()) print(" width=", s.width);
    if (s.has_precision()) print(" precision=", s.precision);
    print(" type=", (int)s.type);
    println("  (", desc, ")");
  }
  println();

  println("📖 状态机解析过程 (fmt/base.h:1440):");
  println("  start → align → sign → hash → zero → width → precision → locale → type");
  println("  每字段顺序固定, 不可重排 — 简洁且可预测");
  println();
  println("📖 精读 fmt/base.h parse_format_specs (rows 1440-1550)");
}

} // namespace ex2

// ============================================================================
// Exercise 3: Type Erasure — 类型擦除
// ============================================================================
//
// 【阅读清单】
//   base.h:  line 2451+ — basic_format_arg, basic_format_args
//   base.h:  line 2344+ — format_arg_store
//   base.h:  line 2757+ — make_format_args
//   base.h:  line 625+  — visit_format_arg
//
// 【关键设计】
//   format("{}, {}, {}", 42, 3.14, "hello")
//   需要把 int, double, const char* 存在一个数组里 → 类型擦除!
//
//   类型擦除方案: tagged union
//     basic_format_arg {
//       enum type { int_type, uint_type, double_type, string_type, ... }
//       union value { int int_value; unsigned uint_value; double double_value;
//                     string_view string_value; const void* pointer; ... }
//     }
//
//   format_arg_store<T...>:
//     - 编译期: 模板参数包展开, 构建 arg 数组
//     - 每个 T 映射到一个 value type
//     - 小对象 (int, double) 直接存值, 大对象 (string) 存指针
//
//   visit_format_arg(visitor, arg):
//     - Visitor 模式: 根据 arg.type() switch 到对应类型
//     - Visitor = formatter<T>::format 的包装

namespace ex3 {

// ── 简化的 Type-Erased Argument ─────────────────────────────────────
enum class ArgType : uint8_t {
  none, int_type, uint_type, long_long_type, ulong_long_type,
  double_type, long_double_type, bool_type, char_type,
  string_type, pointer_type, custom_type
};

struct SimpleFormatArg {
  ArgType type = ArgType::none;
  union {
    int int_val;
    unsigned uint_val;
    long long long_long_val;
    unsigned long long ulong_long_val;
    double double_val;
    long double long_double_val;
    bool bool_val;
    char char_val;
    const char* string_val;
    const void* pointer_val;
  } value = {};

  SimpleFormatArg() = default;
  SimpleFormatArg(int v) : type(ArgType::int_type) { value.int_val = v; }
  SimpleFormatArg(unsigned v) : type(ArgType::uint_type) { value.uint_val = v; }
  SimpleFormatArg(double v) : type(ArgType::double_type) { value.double_val = v; }
  SimpleFormatArg(bool v) : type(ArgType::bool_type) { value.bool_val = v; }
  SimpleFormatArg(const char* v) : type(ArgType::string_type) { value.string_val = v; }
  SimpleFormatArg(char v) : type(ArgType::char_type) { value.char_val = v; }

  std::string format() const {
    char buf[64];
    switch (type) {
    case ArgType::int_type:
      snprintf(buf, sizeof(buf), "%d", value.int_val); break;
    case ArgType::uint_type:
      snprintf(buf, sizeof(buf), "%u", value.uint_val); break;
    case ArgType::double_type:
      snprintf(buf, sizeof(buf), "%g", value.double_val); break;
    case ArgType::bool_type:
      return value.bool_val ? "true" : "false";
    case ArgType::string_type:
      return std::string(value.string_val);
    case ArgType::char_type:
      return std::string(1, value.char_val);
    default: return "<unknown>";
    }
    return buf;
  }
};

// ── Visitor ─────────────────────────────────────────────────────────
template <typename Visitor>
decltype(auto) visit(Visitor&& vis, const SimpleFormatArg& arg) {
  switch (arg.type) {
  case ArgType::int_type:         return vis(arg.value.int_val);
  case ArgType::uint_type:        return vis(arg.value.uint_val);
  case ArgType::double_type:      return vis(arg.value.double_val);
  case ArgType::bool_type:        return vis(arg.value.bool_val);
  case ArgType::string_type:      return vis(arg.value.string_val);
  case ArgType::char_type:        return vis(arg.value.char_val);
  case ArgType::long_long_type:   return vis(arg.value.long_long_val);
  default:                        return vis("???");
  }
}

// ── format_arg_store 简化版 ─────────────────────────────────────────
template <typename... Args>
struct SimpleFormatArgStore {
  SimpleFormatArg _args[sizeof...(Args)];

  explicit SimpleFormatArgStore(Args&&... args)
    : _args{SimpleFormatArg(std::forward<Args>(args))...} {}

  const SimpleFormatArg* data() const { return _args; }
  size_t size() const { return sizeof...(Args); }
};

void run() {
  HR("Ex3: Type Erasure — format_arg");

  // 构造不同类型的参数
  SimpleFormatArgStore<int, double, bool, const char*, char> store(42, 3.14159, true, "hello", 'X');

  println("Type-erased arguments (", store.size(), " args):");
  for (size_t i = 0; i < store.size(); i++) {
    auto& arg = store.data()[i];
    println("  arg[", i, "]: type=", (int)arg.type, " value=", arg.format());
  }
  println();

  // visit 演示
  println("Visit each arg:");
  for (size_t i = 0; i < store.size(); i++) {
    visit([i](auto&& val) {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, int>)
        println("  arg[", i, "] is int: ", val);
      else if constexpr (std::is_same_v<T, double>)
        println("  arg[", i, "] is double: ", val);
      else if constexpr (std::is_same_v<T, bool>)
        println("  arg[", i, "] is bool: ", val);
      else if constexpr (std::is_same_v<T, const char*>)
        println("  arg[", i, "] is string: ", val);
      else if constexpr (std::is_same_v<T, char>)
        println("  arg[", i, "] is char: ", val);
      else
        println("  arg[", i, "] is ???");
    }, store.data()[i]);
  }
  println();

  println("📖 类型映射规则 (fmt/base.h):");
  println("  int/bool/char → int_type     float/double → double_type");
  println("  const char*    → string_type  std::string   → string_type");
  println("  void*          → pointer_type 自定义类型   → custom_type");
  println();
  println("📖 精读 fmt/base.h: basic_format_arg + visit_format_arg");
}

} // namespace ex3

// ============================================================================
// Exercise 4: Integer Formatting — 整数格式化
// ============================================================================
//
// 【阅读清单】
//   format.h: line 1053+ — count_digits, count_digits_fallback
//   format.h: line 1231+ — format_decimal, do_format_decimal
//   format.h: line 1966+ — write_int
//
// 【关键设计】
//   关键性能洞察: 整数转字符串 = 除法 + 取模
//   瓶颈在除法指令 (div/idiv ~20-80 cycles)
//
//   fmtlib 的优化策略:
//   1. count_digits: 查表法 (O(log n) 次比较) 替代循环除法
//      - 32-bit: 用 __builtin_clz 定位, 然后查表
//      - 64-bit: 用 __builtin_clzll 定位, 然后查表
//   2. format_decimal: 每次处理 2 位数字 (查表一次 = 2 chars)
//      - 预计算表: digits[0] = "00", digits[1] = "01", ..., digits[99] = "99"
//      - 从右到左: 每次取 value % 100, 查表写入 2 chars
//   3. 特化: 32-bit / 64-bit / 128-bit 不同路径
//      - 64-bit 用 __uint128_t 一次性处理 8 chars!

namespace ex4 {

// ── fmtlib 的 2-digit 查表法 ───────────────────────────────────────
// fmt/format.h: 预计算的 digits table
static const char g_digits[] =
  "0001020304050607080910111213141516171819"
  "2021222324252627282930313233343536373839"
  "4041424344454647484950515253545556575859"
  "6061626364656667686970717273747576777879"
  "8081828384858687888990919293949596979899";

// count_digits — 查表法 (32-bit)
// fmtlib 实际用 __builtin_clz + lookup table, 这里用简化的二分查找
inline int count_digits_u32(uint32_t n) {
  if (n < 10) return 1;
  if (n < 100) return 2;
  if (n < 1000) return 3;
  if (n < 10000) return 4;
  if (n < 100000) return 5;
  if (n < 1000000) return 6;
  if (n < 10000000) return 7;
  if (n < 100000000) return 8;
  if (n < 1000000000) return 9;
  return 10;
}

// count_digits — 64-bit (用 2^32 划分)
inline int count_digits_u64(uint64_t n) {
  if (n < 10) return 1;
  if (n < 100) return 2;
  if (n < 1000) return 3;
  if (n < 10000) return 4;
  if (n < 100000) return 5;
  if (n < 1000000) return 6;
  if (n < 10000000) return 7;
  if (n < 100000000) return 8;
  if (n < 1000000000) return 9;
  if (n < 10000000000ULL) return 10;
  if (n < 100000000000ULL) return 11;
  if (n < 1000000000000ULL) return 12;
  if (n < 10000000000000ULL) return 13;
  if (n < 100000000000000ULL) return 14;
  if (n < 1000000000000000ULL) return 15;
  if (n < 10000000000000000ULL) return 16;
  if (n < 100000000000000000ULL) return 17;
  if (n < 1000000000000000000ULL) return 18;
  if (n < 10000000000000000000ULL) return 19;
  return 20;
}

// format_decimal — 从右到左写入 (每次 2 chars)
// fmt/format.h:1231 do_format_decimal
char* format_decimal(char* out, uint64_t value, int num_digits) {
  // 从右往左写
  char* end = out + num_digits;
  char* ptr = end;

  // 每次处理 2 个数字 (查表一次 = 2 chars)
  while (value >= 100) {
    auto idx = static_cast<unsigned>((value % 100) * 2);
    value /= 100;
    *--ptr = g_digits[idx + 1];
    *--ptr = g_digits[idx];
  }
  // 处理剩余 1-2 位
  if (value < 10) {
    *--ptr = static_cast<char>('0' + value);
  } else {
    auto idx = static_cast<unsigned>(value * 2);
    *--ptr = g_digits[idx + 1];
    *--ptr = g_digits[idx];
  }
  return end;
}

// write_int — 完整整数格式化 (简化版)
// fmt/format.h:1966
std::string write_int(uint64_t value, int base = 10, bool upper = false) {
  if (base == 10) {
    int nd = count_digits_u64(value);
    std::string result(nd, '\0');
    format_decimal(result.data(), value, nd);
    return result;
  }
  // 非十进制: 用标准算法
  if (value == 0) return "0";
  static const char* ldigits = "0123456789abcdef";
  static const char* udigits = "0123456789ABCDEF";
  const char* digits = upper ? udigits : ldigits;
  char buf[64];
  char* p = buf + sizeof(buf);
  *--p = '\0';
  while (value) {
    *--p = digits[value % base];
    value /= base;
  }
  return p;
}

void run() {
  HR("Ex4: Integer Formatting");

  // count_digits 演示
  println("count_digits (二分查找法):");
  for (auto n : {0u, 9u, 10u, 99u, 100u, 999u, 1000u, 9999u,
                 100000u, 999999u, 1000000u, 4294967295u}) {
    println("  ", n, " → ", count_digits_u32(n), " digits");
  }
  println();

  // format_decimal 演示
  std::string buf(20, '\0');
  auto* end = format_decimal(buf.data(), 1234567890, 10);
  println("format_decimal(1234567890, 10) = '",
          std::string_view(buf.data(), end - buf.data()), "'");
  println();

  // 不同进制
  println("write_int(255, base=...)");
  for (int base : {2, 8, 10, 16}) {
    println("  base ", base, ": ", write_int(255, base, base == 16));
  }
  println();

  // 性能对比: 二分 count_digits vs 循环除法
  constexpr int N = 1000000;
  {
    auto t0 = std::chrono::steady_clock::now();
    volatile int sum = 0;
    for (int i = 0; i < N; i++) {
      int nd = 0, n = i;
      do { n /= 10; nd++; } while (n);  // 循环除法
      sum += nd;
    }
    auto t1 = std::chrono::steady_clock::now();
    println("count_digits 循环除法: ", std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), "us");
  }
  {
    auto t0 = std::chrono::steady_clock::now();
    volatile int sum = 0;
    for (int i = 0; i < N; i++) sum += count_digits_u32(i);
    auto t1 = std::chrono::steady_clock::now();
    println("count_digits 二分查找: ", std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(), "us");
  }
  println();

  println("📖 fmtlib 的 2-digit 查表是整数格式化的核心优化:");
  println("  1. 用 value % 100 一次计算 2 个数字");
  println("  2. 查预计算表 digits[0..99] → 2 chars 直接 memcpy");
  println("  3. 对 64-bit 用 __uint128_t 进一步加速 (一次处理 8 chars)");
  println();
  println("📖 精读 fmt/format.h: count_digits + format_decimal + write_int");
}

} // namespace ex4

// ============================================================================
// Exercise 5: Float Formatting — 浮点数格式化
// ============================================================================
//
// 【阅读清单】
//   format.h:   line 1442-1527 — dragonbox: float_info, to_decimal
//   format.h:   line 1527+ — format_float (Grisu/Dragonbox 选择)
//   src/format.cc — dragonbox 的完整实现 (Julius/floor_log2_pow10 等)
//
// 【关键设计】
//   浮点格式化是最难的问题之一 (Grisu 的论文有 30 页!)
//
//   fmtlib 用了两种算法:
//   1. Grisu2: 快速但偶尔失败 → fallback to std::snprintf
//   2. Dragonbox (Junekey Jeon): 更快, 从不失败! → 默认算法
//
//   Dragonbox 核心:
//     - 输入: float/double 的 IEEE 754 位表示
//     - 利用 128-bit 整数运算算出十进制表示
//     - 不需要 bignum / 大整数除法
//     - 比 Grisu 快且更正确
//
//   简化: 这里用 charconv (C++17) 替代 Dragonbox 的核心
//   std::to_chars 也是用类似算法 (Ryu/Dragonbox)

namespace ex5 {

// ── IEEE 754 浮点数的位表示 ─────────────────────────────────────────
union FloatBits {
  float f;
  uint32_t u;
};
union DoubleBits {
  double d;
  uint64_t u;
};

struct IEEEFloatInfo {
  static constexpr uint32_t sign_mask = 0x80000000u;
  static constexpr uint32_t exp_mask  = 0x7F800000u;
  static constexpr uint32_t mant_mask = 0x007FFFFFu;
  static constexpr int exp_bias = 127;

  static void print(float f) {
    FloatBits fb{f};
    uint32_t sign = (fb.u >> 31) & 1;
    uint32_t exp  = (fb.u >> 23) & 0xFF;
    uint32_t mant = fb.u & 0x7FFFFF;
    double value = (sign ? -1.0 : 1.0) *
                   (exp == 0 ? 0 : 1.0 + mant / 8388608.0) *
                   std::pow(2.0, (int)exp - 127);
    println("  float ", f, ": sign=", sign, " exp=", exp, " (", (int)exp - 127, ") mant=",
            std::hex, mant, std::dec, " → value=", value);
  }
};

struct IEEEDoubleInfo {
  static constexpr uint64_t sign_mask = 0x8000000000000000ULL;
  static constexpr uint64_t exp_mask  = 0x7FF0000000000000ULL;
  static constexpr uint64_t mant_mask = 0x000FFFFFFFFFFFFFULL;
  static constexpr int exp_bias = 1023;

  static void print(double d) {
    DoubleBits db{d};
    uint64_t sign = (db.u >> 63) & 1;
    uint64_t exp  = (db.u >> 52) & 0x7FF;
    uint64_t mant = db.u & 0xFFFFFFFFFFFFFULL;
    println("  double ", d, ": sign=", sign, " exp=", exp, " (", (int64_t)exp - 1023,
            ") mant=", std::hex, mant, std::dec);
  }
};

// ── 使用 std::to_chars (C++17) 模拟 Gryphon/Dragonbox ──────────────
// fmtlib 用 Dragonbox 替代 std::to_chars 以支持 C++11 和不完善的 stdlib
std::string format_float(double value, int precision = -1) {
  char buf[64];
  std::to_chars_result r;
  if (precision >= 0) {
    r = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, precision);
  } else {
    r = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general);
  }
  if (r.ec == std::errc{})
    return std::string(buf, r.ptr);
  return std::to_string(value); // fallback
}

void run() {
  HR("Ex5: Float Formatting");

  // IEEE 754 位表示
  println("IEEE 754 float 结构 (sign:1, exp:8, mant:23):");
  IEEEFloatInfo::print(3.14f);
  IEEEFloatInfo::print(-0.5f);
  IEEEFloatInfo::print(0.0f);
  println();

  println("IEEE 754 double 结构 (sign:1, exp:11, mant:52):");
  IEEEDoubleInfo::print(3.141592653589793);
  IEEEDoubleInfo::print(1.0 / 3.0);
  println();

  // 格式化演示
  println("Float formatting (via std::to_chars / Dragonbox):");
  for (auto v : {3.14159, 0.1, 1.0 / 3.0, 1e308, 1e-308, 42.0}) {
    println("  ", v, " → '", format_float(v), "'");
    println("  ", v, " fixed(4) → '", format_float(v, 4), "'");
  }
  println();

  println("📖 为什么浮点格式化这么难?");
  println("  1. 二进制小数 → 十进制 不是精确转换 (0.1 在二进制中是无限循环)");
  println("  2. 找到一个最短的十进制表示 (能 round-trip 回相同浮点数)");
  println("  3. 需要正确处理 subnormal, infinity, NaN");
  println("  4. Grisu/Dragonbox/Ryu 是 30 页论文级别的算法");
  println();
  println("📖 Dragonbox 关键洞察 (fmt/format.h:1442-1527):");
  println("  - 用 128-bit 整数乘法模拟大数运算");
  println("  - log10(2) 的精确有理逼近");
  println("  - 两个缓存表: pow10_ceil/pow10_floor");
  println();
  println("📖 精读 fmt/format.h: namespace dragonbox + format_float");
}

} // namespace ex5

// ============================================================================
// Exercise 6: Output Iterator — 输出迭代器模式
// ============================================================================
//
// 【阅读清单】
//   base.h:   line 600+  — basic_appender
//   base.h:   line 2820+ — vformat_to, format_to_n
//   format.h: line 2578+ — format_handler (格式化循环)
//
// 【关键设计】
//   fmtlib 的输出基于迭代器模式, 不依赖 std::string 或 FILE*:
//
//   basic_appender<Char>:
//     - 缓冲的 output iterator (类似 std::back_insert_iterator + 小 buffer)
//     - 小字符串用栈 buffer (SSO), 大字符串动态扩容
//
//   format_to(OutputIt, fmt, args...):
//     - 通用的"格式化到任意迭代器"
//     - format_to(std::back_inserter(vec), "{}", 42)  → vector
//     - format_to(buf, "{}", 42)  → char array
//
//   format_to_n(OutputIt, size_t n, fmt, args...):
//     - 限制输出长度 (安全版本)
//     - 返回 format_to_n_result{out, size}

namespace ex6 {

// ── 简化的 appender (类似 fmt::basic_appender) ──────────────────────
template <typename Char = char>
struct SimpleAppender {
  Char* _buf;
  size_t _capacity;
  size_t _size = 0;

  SimpleAppender(Char* buf, size_t cap) : _buf(buf), _capacity(cap) {}

  // output iterator 接口
  using value_type = Char;
  SimpleAppender& operator*() { return *this; }
  SimpleAppender& operator++() { return *this; }
  SimpleAppender& operator++(int) { return *this; }
  SimpleAppender& operator=(Char c) {
    if (_size < _capacity) _buf[_size++] = c;
    return *this;
  }

  size_t size() const { return _size; }
  std::string_view view() const { return {_buf, _size}; }
};

// ── 使用 appender 的简化 format_to ──────────────────────────────────
template <typename T>
std::string to_str(const T& v) { return std::to_string(v); }
inline std::string to_str(const char* s) { return s; }
inline std::string to_str(const std::string& s) { return s; }

template <typename OutputIt, typename... Args>
OutputIt simple_format_to(OutputIt out, const char* fmt, Args&&... args) {
  std::string_view fmt_sv(fmt);
  size_t arg_idx = 0;

  std::string arg_strs[] = {to_str(args)...};

  size_t pos = 0;
  while (pos < fmt_sv.size()) {
    if (fmt_sv[pos] == '{' && pos + 1 < fmt_sv.size() && fmt_sv[pos + 1] == '}') {
      const auto& s = arg_strs[arg_idx++];
      for (char c : s) *out++ = c;
      pos += 2;
    } else {
      *out++ = fmt_sv[pos++];
    }
  }
  return out;
}

void run() {
  HR("Ex6: Output Iterator — format_to");

  // 格式化到 char 数组
  char buf[256] = {};
  SimpleAppender<char> appender(buf, sizeof(buf));
  simple_format_to(appender, "Hello, {}! The answer is {}.", "world", 42);
  println("format_to: '", appender.view(), "'");
  println();

  // 格式化到 std::string (通过 back_insert_iterator)
  std::string str;
  simple_format_to(std::back_inserter(str), "x={} y={} z={}", 1, 2.5, "three");
  println("format_to(string): '", str, "'");
  println();

  println("📖 fmtlib 的 iterator 模式优势:");
  println("  1. 不绑定特定容器 → 可以输出到 string/vector/char[]/FILE*");
  println("  2. basic_appender 有 internal buffer → 减少 iterator 调用次数");
  println("  3. format_to_n 限制输出长度 → 防止缓冲区溢出");
  println("  4. 单次遍历 → O(n) 时间复杂度");
  println();
  println("📖 精读 fmt/base.h: basic_appender + vformat_to");
}

} // namespace ex6

// ============================================================================
// Exercise 7: Alignment & Padding — 对齐和填充
// ============================================================================
//
// 【阅读清单】
//   format.h: line 1714+ — write_padded (对齐/填充的核心函数)
//
// 【关键设计】
//   write_padded 处理 format spec 中的 fill/align/width:
//
//   - 左对齐 "<": val + padding       (123     )
//   - 右对齐 ">": padding + val       (     123)
//   - 居中对齐 "^": padL + val + padR (  123   )
//   - 数字对齐 "=" (numeric): 符号+填充+数值 (+000123)
//
//   实现: lambda + reserve_iterator
//     write_padded(out, specs, size, [=](reserve_iterator it) {
//       // 实际写入 value
//       write_int(it, value, ...);
//     });
//     → 先调用 lambda 到 temp buffer 获取实际宽度
//     → 然后计算 padding 并写入

namespace ex7 {

// ── write_padded 简化实现 ──────────────────────────────────────────
template <typename OutputIt, typename WriteFunc>
auto write_padded(OutputIt out, int width, Align align, char fill,
                  int value_size, WriteFunc&& write_value) {
  int padding = width > value_size ? width - value_size : 0;

  if (align == Align::left || align == Align::none) {
    // 先写值, 再写填充
    write_value(out);
    for (int i = 0; i < padding; i++) *out++ = fill;
  } else if (align == Align::right) {
    // 先写填充, 再写值
    for (int i = 0; i < padding; i++) *out++ = fill;
    write_value(out);
  } else if (align == Align::center) {
    // 左填充一半, 右填充一半
    int left_pad = padding / 2;
    int right_pad = padding - left_pad;
    for (int i = 0; i < left_pad; i++) *out++ = fill;
    write_value(out);
    for (int i = 0; i < right_pad; i++) *out++ = fill;
  }
  return out;
}

// 演示: 格式化到 string
std::string format_aligned(std::string_view value, int width, Align align, char fill = ' ') {
  std::string result;
  write_padded(std::back_inserter(result), width, align, fill, value.size(),
               [&](auto it) {
                 for (char c : value) *it = c;
               });
  return result;
}

void run() {
  HR("Ex7: Alignment & Padding");

  println("对齐演示 (width=15):");
  println("  left:   '", format_aligned("hello", 15, Align::left), "'");
  println("  right:  '", format_aligned("hello", 15, Align::right), "'");
  println("  center: '", format_aligned("hello", 15, Align::center), "'");
  println();

  println("不同填充字符 (width=12):");
  println("  fill='-':  '", format_aligned("abc", 12, Align::right, '-'), "'");
  println("  fill='.':  '", format_aligned("abc", 12, Align::left, '.'), "'");
  println("  fill='*':  '", format_aligned("abc", 12, Align::center, '*'), "'");
  println();

  println("数字右对齐 (典型表格输出):");
  for (auto [label, val] : {std::pair{"apples", 42}, std::pair{"oranges", 7}, std::pair{"bananas", 153}}) {
    print("  ", format_aligned(label, 10, Align::left), " ");
    println(format_aligned(std::to_string(val), 5, Align::right));
  }
  println();

  println("📖 fmtlib 的 write_padded 亮点:");
  println("  1. Lambda 延迟计算 → 先确定 value 宽度, 再计算 padding");
  println("  2. reserve_iterator → 预计算容量, 一次分配 (减少 realloc)");
  println("  3. 数字特殊处理 → align=numeric 时先写符号, 再写 0 填充");
  println();
  println("📖 精读 fmt/format.h: write_padded (line 1714)");
}

} // namespace ex7

// ============================================================================
// Exercise 8: Compile-Time Format Checking — 编译期格式检查
// ============================================================================
//
// 【阅读清单】
//   base.h:    line 1679+ — format_string_checker
//   compile.h:            — FMT_COMPILE (编译期编译格式字符串)
//
// 【关键设计】
//   C++20 之前:
//     - 无法在编译期检查格式字符串 (格式字符串是运行时 string)
//     - FMT_STRING(s) 宏: 在构造时做运行时检查 (抛异常)
//
//   C++20 之后:
//     - consteval constructor → format_string 在编译期解析格式
//     - 类型不匹配 → 编译错误 (不是运行时抛异常!)
//     - fmt::format("{:d}", "hello") → **编译错误**: "string can't be formatted as integer"
//
//   实现: format_string_checker 在编译期遍历格式字符串
//     - 解析每个 {} 的 argument_id
//     - 检查 argument_id 是否在有效范围
//     - 检查 format spec 是否与参数类型兼容
//     - 有错误 → 触发 static_assert / consteval throw

namespace ex8 {

// ── 简化的编译期格式字符串检查器 ────────────────────────────────────
// 实际 fmtlib 使用 FMT_CONSTEVAL + detail::format_string_checker
// 这里用 constexpr 模拟概念

struct SimpleFormatError {
  const char* msg;
  int position;
};

// 编译期检查: 数格式字符串中的 {} 数量
consteval int count_args(const char* fmt, size_t len) {
  int count = 0;
  for (size_t i = 0; i < len; i++) {
    if (fmt[i] == '{' && i + 1 < len && fmt[i + 1] == '}') {
      count++;
      i++;
    }
  }
  return count;
}

// 包装类: 在编译期检查参数数量
template <size_t N>
struct CheckedFormatString {
  const char* str;
  size_t len;

  // consteval 构造: 编译期强制检查
  template <size_t M>
  consteval CheckedFormatString(const char (&s)[M]) : str(s), len(M - 1) {
    int args = count_args(s, M - 1);
    if (args != (int)N) {
      // consteval 中 throw = 编译错误
      throw "format string argument count mismatch!";
    }
  }
};

// 简化: 对 C++20 之前的编译器, 用运行时 assert
template <size_t N>
struct RuntimeCheckedFormatString {
  std::string_view str;

  RuntimeCheckedFormatString(std::string_view s) : str(s) {
    // 运行时数 {} 并检查
    int count = 0;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '}') {
        count++;
        i++;
      }
    }
    if (count != (int)N) {
      println("❌ Error: format string expects ", count, " args but ", N, " provided");
      // 实际 fmtlib 这里会抛异常
    }
  }
};

void run() {
  HR("Ex8: Compile-Time Format Checking");

  // 演示 1: 编译期检查 (C++20 consteval)
  // 下面这行如果取消注释, compiles fine:
  // CheckedFormatString<2> fmt("{} + {} = {}");

  // 下面这行: 编译错误! (3 {} but template says 2 args)
  // CheckedFormatString<2> bad("{} + {} = {}");
  // 错误: "format string argument count mismatch!"

  println("编译期格式检查 (C++20 consteval):");
  println("  CheckedFormatString<2> ok(\"{} + {}\");  // ✅ 编译通过");
  println("  CheckedFormatString<2> bad(\"{}, {}\");  // ❌ 编译错误");
  println();

  // 演示 2: 运行时检查
  println("运行时格式检查:");
  RuntimeCheckedFormatString<2> ok("x={} y={}");
  println("  x={} y={} with 2 args: ✅");
  RuntimeCheckedFormatString<3> bad("only two {}");
  println();

  println("📖 fmtlib 的编译期检查层次:");
  println("  1. FMT_STRING(s) — 构造时运行时解析 + 抛异常 (C++11/14)");
  println("  2. format_string<T...> — consteval 构造, 编译期错误 (C++20)");
  println("  3. FMT_COMPILE(s) — 编译期生成优化代码 (skip 运行时解析)");
  println("  4. format_string_checker — 类型兼容性检查 (int 不能 format as string)");
  println();
  println("📖 精读 fmt/base.h: format_string_checker + compile.h: FMT_COMPILE");
}

} // namespace ex8

// ============================================================================
// Exercise 9: Custom Formatters — 自定义格式化器
// ============================================================================
//
// 【阅读清单】
//   base.h:   line 2344+ — formatter<T> 基础
//   format.h: line 3819+ — default formatter 特化 (int, float, string, etc.)
//   chrono.h:            — std::chrono 类型的 formatter 特化
//   ranges.h:            — 容器类型的 formatter 特化
//
// 【关键设计】
//   formatter<T> 协议 (两个方法):
//     1. parse(ParseContext& ctx) → 解析格式说明, 返回 end iterator
//        - 用户可以用自定义格式说明语法
//        - 返回 ctx.begin() = 不用格式说明
//     2. format(const T& val, FormatContext& ctx) → 格式化写入
//        - 使用 ctx.out() 获取输出迭代器
//
//   例: 为 Point 类型实现 formatter:
//     template <> struct formatter<Point> {
//       constexpr auto parse(ParseContext& ctx) { ... }
//       auto format(const Point& p, FormatContext& ctx) { ... }
//     };

namespace ex9 {

// ── 自定义类型 ──────────────────────────────────────────────────────
struct Point {
  int x, y;
};

struct Person {
  std::string name;
  int age;
};

// ── formatter<Point> 协议 ───────────────────────────────────────────
// 实际 fmtlib 中 formatter 是一个模板, 对每个类型做特化
// 这里用函数重载模拟格式说明解析

struct SimpleFormatter {
  // format spec: "x,y" (逗号分隔) or "x y" (空格分隔)
  char _sep = ',';

  static SimpleFormatter parse(std::string_view spec) {
    SimpleFormatter f;
    if (spec.size() == 1) f._sep = spec[0];
    return f;
  }

  std::string format(const Point& p) const {
    return "(" + std::to_string(p.x) + _sep + " " + std::to_string(p.y) + ")";
  }

  std::string format(const Person& p) const {
    return p.name + " (" + std::to_string(p.age) + " yrs)";
  }
};

void run() {
  HR("Ex9: Custom Formatters");

  // 自定义 Point 格式化
  Point p{10, 20};
  auto pf = SimpleFormatter::parse(",");
  println("Point with ',': ", pf.format(p));

  auto pf2 = SimpleFormatter::parse("|");
  println("Point with '|': ", pf2.format(p));
  println();

  // 自定义 Person 格式化
  Person alice{"Alice", 30};
  println("Person: ", SimpleFormatter{}.format(alice));
  println();

  println("📖 formatter 协议 (fmtlib 实际 API):");
  println("  template <> struct fmt::formatter<Point> {");
  println("    constexpr auto parse(format_parse_context& ctx) {");
  println("      // 解析用户提供的格式说明");
  println("      auto it = ctx.begin();");
  println("      if (it != ctx.end() && (*it == ',' || *it == '|'))");
  println("        sep_ = *it++;");
  println("      return it;  // 返回消费后的 iterator");
  println("    }");
  println();
  println("    auto format(const Point& p, format_context& ctx) const {");
  println("      return format_to(ctx.out(), \"({}{} {})\", p.x, sep_, p.y);");
  println("    }");
  println("    char sep_ = ',';");
  println("  };");
  println();
  println("📖 预定义的 formatter 特化 (fmt/format.h):");
  println("  - 整数/浮点/布尔 → write_int / write_float");
  println("  - string / const char* / string_view → write_padded");
  println("  - void* / nullptr_t → write_ptr");
  println("  - chrono types → chrono.h 中特化");
  println("  - containers → ranges.h 中特化");
}

} // namespace ex9

// ============================================================================
// Exercise 10: Performance & Architecture — 性能与架构全景
// ============================================================================
//
// 【阅读清单】
//   README.rst — 性能 benchmark
//   format.h/format-inl.h — 所有优化技巧
//   src/format.cc — 编译期隔离的浮点算法
//
// 【关键决策】
//   fmtlib 为什么快?
//   1. 类型安全 = 编译期知道类型 → 不需要运行时格式字符串解析类型
//   2. Iterator 模式 → 不绑定 std::string (无多次 realloc)
//   3. 查表法 → count_digits + 2-digit table → 除法最小化
//   4. Dragonbox → 浮点格式化比 printf 还快
//   5. 编译期格式检查 → 跳过运行时解析 (FMT_COMPILE)
//   6. 小对象优化 → format_arg_store 栈上存储, 无堆分配
//   7. Header-only (大部分) → 全内联优化
//
//   对比 benchmark (fmtlib README):
//     fmt::format:     ~100 ns  (baseline)
//     std::ostringstream: ~1100 ns  (11x slower)
//     sprintf:         ~500 ns (5x slower, 且不安全)

namespace ex10 {

// 简易 benchmark 工具
template <typename Func>
double benchmark(Func&& f, int iterations = 100000) {
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; i++) {
    volatile auto _ = f(i);
    (void)_;
  }
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / iterations;
}

void run() {
  HR("Ex10: Performance & Architecture");

  println("简易 benchmark (每操作耗时):");
  println();

  // int → string
  double fmt_int = benchmark([](int i) {
    return ex4::write_int(i * 1000 + 42, 10, false);
  });
  double snprintf_int = benchmark([](int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", i * 1000 + 42);
    return std::string(buf);
  });
  println("  int->string:");
  println("    write_int (fmtlib) : ", (int)fmt_int, " ns/op");
  println("    snprintf           : ", (int)snprintf_int, " ns/op");
  println("    speedup            : ", snprintf_int / fmt_int, "x");
  println();

  // count_digits
  double cd_lookup = benchmark([](int i) {
    return ex4::count_digits_u32(i);
  });
  double cd_loop = benchmark([](int i) {
    int nd = 0, n = i;
    do { n /= 10; nd++; } while (n);
    return nd;
  });
  println("  count_digits:");
  println("    二分查找           : ", (int)cd_lookup, " ns/op");
  println("    循环除法           : ", (int)cd_loop, " ns/op");
  println("    speedup            : ", cd_loop / cd_lookup, "x");
  println();

  println("╔══════════════════════════════════════════════════════╗");
  println("║        fmtlib 完整数据流                              ║");
  println("╠══════════════════════════════════════════════════════╣");
  println("║                                                      ║");
  println("║  format(\"x={} y={:.2f}\", 42, 3.14)                    ║");
  println("║    │                                                 ║");
  println("║    ├─► format_string (编译期解析格式)                  ║");
  println("║    │     - count_args: 验证参数数量                    ║");
  println("║    │     - parse_format_specs 对每个 {}                ║");
  println("║    │                                                 ║");
  println("║    ├─► make_format_args (类型擦除)                    ║");
  println("║    │     - format_arg_store<42, 3.14>                 ║");
  println("║    │     - basic_format_arg{type, union value}        ║");
  println("║    │                                                 ║");
  println("║    └─► vformat_to(buf, fmt, args)                    ║");
  println("║          │                                           ║");
  println("║          ├─► for each {}:                            ║");
  println("║          │    visit_format_arg → formatter<T>::format  ║");
  println("║          │      │                                    ║");
  println("║          │      ├─ int → write_int                   ║");
  println("║          │      │    count_digits + format_decimal    ║");
  println("║          │      │    2-digit 查表法                   ║");
  println("║          │      │                                    ║");
  println("║          │      ├─ float → write_float               ║");
  println("║          │      │    Dragonbox 算法                   ║");
  println("║          │      │    128-bit 整数运算                 ║");
  println("║          │      │                                    ║");
  println("║          │      └─ string → write_padded             ║");
  println("║          │            fill + align + width            ║");
  println("║          │                                           ║");
  println("║          └─► format_to(appender)                     ║");
  println("║                buffer → back_insert → string/vector   ║");
  println("║                                                      ║");
  println("╚══════════════════════════════════════════════════════╝");
  println();

  println("📊 核心文件概览:");
  println("  base.h      ~2800行 — format_string, parse_format_specs, format_args");
  println("  format.h    ~4800行 — write_int, write_float, write_padded, formatters");
  println("  format-inl.h ~380行 — 内联整数/浮点格式化");
  println("  args.h       ~150行 — arg storage");
  println("  compile.h    ~200行 — 编译期格式检查");
  println("  chrono.h     ~700行 — 时间/日期格式化");
  println("  color.h      ~300行 — 终端颜色");
  println("  ranges.h     ~870行 — 容器格式化");
  println("  src/format.cc ~300行 — 编译期隔离的浮点算法");
  println();

  println("🔑 fmtlib 最值得学习的 10 个设计:");
  println("  1. compile-time format checking → 类型安全, 零运行时开销");
  println("  2. type erasure via tagged union → 异构参数统一存储");
  println("  3. 2-digit lookup table → 整数格式化极致优化");
  println("  4. Dragonbox algorithm → 浮点格式化比 printf 更快");
  println("  5. Iterator-based output → 解耦容器, 一次遍历");
  println("  6. parse/format 两阶段协议 → 自定义格式化器简洁优雅");
  println("  7. consteval format_string → C++20 的 killer feature 实践");
  println("  8. Header-only by default → 全内联 + 零依赖");
  println("  9. backward compatibility → C++11/14/17/20 无缝支持");
  println("  10. std::format based on fmt → 事实上的 ISO C++ 标准库");
}

} // namespace ex10

// ============================================================================
// Main
// ============================================================================
int main() {
  println(R"(
╔══════════════════════════════════════════════════════════════╗
║     Month 5 Week 25: fmtlib 源码阅读                           ║
║     "看透实现 — 现代 C++ 格式化库的极致设计"                    ║
╚══════════════════════════════════════════════════════════════╝)");

  ex1::run();
  ex2::run();
  ex3::run();
  ex4::run();
  ex5::run();
  ex6::run();
  ex7::run();
  ex8::run();
  ex9::run();
  ex10::run();

  HR("Week 25 完成!");
  println("✅ 理解了 format string parsing 状态机");
  println("✅ 理解了 type erasure (format_arg + visit)");
  println("✅ 理解了 integer formatting (count_digits + 2-digit table)");
  println("✅ 理解了 float formatting (Dragonbox/Grisu)");
  println("✅ 理解了 output iterator 模式 (format_to/appender)");
  println("✅ 理解了 alignment & padding (write_padded)");
  println("✅ 理解了 compile-time format checking (consteval)");
  println("✅ 理解了 custom formatter 协议 (parse/format)");
  println("✅ 理解了整体性能架构");
  println("✅ 下一步: Week 26 — libevent 源码阅读");
  println();
  println("📖 推荐继续阅读:");
  println("  1. std::format (C++20 <format>) — fmtlib 的标准化版本");
  println("  2. fmt/compile.h — FMT_COMPILE 编译期代码生成");
  println("  3. fmt/chrono.h — 时间格式化的 formatter 特化");
  println("  4. fmt/ranges.h — 容器格式化的 formatter 特化");
  return 0;
}
