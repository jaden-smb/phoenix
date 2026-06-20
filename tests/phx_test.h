// tests/phx_test.h — a tiny, dependency-free test harness (no GoogleTest, in the engine's
// spirit). Tests self-register at static-init; the runner in tests/main.cpp executes them.
#ifndef PHX_TEST_H
#define PHX_TEST_H

#include <cstdio>

namespace phxtest {

struct Case { const char* name; void (*fn)(); };

inline Case*& storage() { static Case* arr = new Case[512]; return arr; }
inline int&   count()   { static int c = 0; return c; }
inline int&   failures(){ static int f = 0; return f; }
inline int&   checks()  { static int n = 0; return n; }

inline bool reg(const char* name, void (*fn)()) {
    storage()[count()++] = Case{ name, fn };
    return true;
}

} // namespace phxtest

#define PHX_TEST(name)                                                         \
    static void name();                                                        \
    static bool PHX_CONCAT(_phx_reg_, name) = ::phxtest::reg(#name, name);      \
    static void name()

#define PHX_CONCAT_(a, b) a##b
#define PHX_CONCAT(a, b) PHX_CONCAT_(a, b)

#define CHECK(cond)                                                            \
    do {                                                                       \
        ::phxtest::checks()++;                                                 \
        if (!(cond)) {                                                         \
            ::phxtest::failures()++;                                           \
            std::printf("      FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ::phxtest::checks()++;                                                 \
        auto _a = (a); auto _b = (b);                                          \
        if (!(_a == _b)) {                                                     \
            ::phxtest::failures()++;                                           \
            std::printf("      FAIL %s:%d  CHECK_EQ(%s, %s)  [%lld != %lld]\n", \
                        __FILE__, __LINE__, #a, #b,                            \
                        (long long)_a, (long long)_b);                        \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
    do {                                                                       \
        ::phxtest::checks()++;                                                 \
        double _d = double(a) - double(b);                                     \
        if (_d < 0) _d = -_d;                                                  \
        if (_d > (eps)) {                                                      \
            ::phxtest::failures()++;                                           \
            std::printf("      FAIL %s:%d  CHECK_NEAR(%s, %s)  |%g - %g| > %g\n", \
                        __FILE__, __LINE__, #a, #b, double(a), double(b), double(eps)); \
        }                                                                      \
    } while (0)

#endif // PHX_TEST_H
