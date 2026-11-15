// Planner unit tests: build/planner/test [worlds dir]
// Tests register with TEST(name) { ... } and use CHECK / CHECK_EQ.
#include "test.hpp"

#include <cstdio>
#include <string>

namespace sbm2test {
std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
int failures = 0;
std::string worldsDir = "planner/test/worlds";
}  // namespace sbm2test

int main(int argc, char** argv) {
  using namespace sbm2test;
  std::string only;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("-k", 0) == 0) only = a.substr(2);
    else worldsDir = a;
  }
  int passed = 0, failed = 0;
  for (auto& c : cases()) {
    if (!only.empty() && c.name.find(only) == std::string::npos) continue;
    int before = failures;
    c.fn();
    if (failures == before) passed++;
    else {
      failed++;
      std::printf("FAIL %s\n", c.name.c_str());
    }
  }
  std::printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
