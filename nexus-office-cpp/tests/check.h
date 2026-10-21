// Mini marco de pruebas: TEST("nombre") { CHECK(...); CHECK_EQ(a, b); }
#pragma once
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace nxt {
struct Case {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<Case>& cases() {
  static std::vector<Case> v;
  return v;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Reg {
  Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};
template <class A, class B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (!(a == b)) {
    std::ostringstream os;
    os << file << ":" << line << ": CHECK_EQ(" << ea << ", " << eb << ")\n    " << a << "\n != " << b;
    std::cerr << os.str() << "\n";
    ++failures();
  }
}
}  // namespace nxt

#define NXT_CAT2(a, b) a##b
#define NXT_CAT(a, b) NXT_CAT2(a, b)
#define TEST(name)                                                       \
  static void NXT_CAT(test_fn_, __LINE__)();                             \
  static nxt::Reg NXT_CAT(test_reg_, __LINE__)(name, NXT_CAT(test_fn_, __LINE__)); \
  static void NXT_CAT(test_fn_, __LINE__)()
#define CHECK(c)                                                                     \
  do {                                                                               \
    if (!(c)) {                                                                      \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #c ") fallo\n";         \
      ++nxt::failures();                                                             \
    }                                                                                \
  } while (0)
#define CHECK_EQ(a, b) nxt::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
