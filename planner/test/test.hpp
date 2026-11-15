// Minimal test harness (no dependencies).
#pragma once
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace sbm2test {
struct Case {
  std::string name;
  std::function<void()> fn;
};
std::vector<Case>& cases();
extern int failures;
extern std::string worldsDir;
struct Reg {
  Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};
}  // namespace sbm2test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                              \
  static void TEST_CAT(test_fn_, __LINE__)();                                   \
  static sbm2test::Reg TEST_CAT(test_reg_, __LINE__)(name, TEST_CAT(test_fn_, __LINE__)); \
  static void TEST_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::printf("  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);           \
      sbm2test::failures++;                                                     \
    }                                                                           \
  } while (0)

#define CHECK_EQ(a, b)                                                          \
  do {                                                                          \
    auto _a = (a);                                                              \
    auto _b = (b);                                                              \
    if (!(_a == _b)) {                                                          \
      std::ostringstream _s;                                                    \
      _s << _a << " != " << _b;                                                 \
      std::printf("  %s:%d: %s == %s: %s\n", __FILE__, __LINE__, #a, #b, _s.str().c_str()); \
      sbm2test::failures++;                                                     \
    }                                                                           \
  } while (0)
