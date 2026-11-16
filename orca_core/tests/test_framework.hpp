// Minimal self-contained test framework (no external dependencies).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace orca::test {

struct Registry {
    std::vector<std::pair<std::string, std::function<void()>>> tests;
    int failures = 0;
    int checks = 0;
    static Registry& get() { static Registry r; return r; }
};

struct AutoRegister {
    AutoRegister(const char* name, std::function<void()> fn) { Registry::get().tests.emplace_back(name, std::move(fn)); }
};

inline int run_all(const char* suite) {
    auto& r = Registry::get();
    for (auto& [name, fn] : r.tests) {
        const int before = r.failures;
        fn();
        std::printf("[%s] %-60s %s\n", suite, name.c_str(), r.failures == before ? "ok" : "FAILED");
    }
    std::printf("[%s] %zu tests, %d checks, %d failures\n", suite, r.tests.size(), r.checks, r.failures);
    return r.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace orca::test

#define TEST_CASE(name)                                                              \
    static void name();                                                              \
    static ::orca::test::AutoRegister name##_reg(#name, name);                       \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++::orca::test::Registry::get().checks;                                      \
        if (!(cond)) {                                                               \
            ++::orca::test::Registry::get().failures;                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        ++::orca::test::Registry::get().checks;                                      \
        const auto va_ = (a);                                                        \
        const auto vb_ = (b);                                                        \
        if (!(va_ == vb_)) {                                                         \
            ++::orca::test::Registry::get().failures;                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (0x%llx vs 0x%llx)\n",     \
                         __FILE__, __LINE__, #a, #b,                                 \
                         static_cast<unsigned long long>(va_),                       \
                         static_cast<unsigned long long>(vb_));                      \
        }                                                                            \
    } while (0)

#define TEST_MAIN(suite) int main() { return ::orca::test::run_all(suite); }
