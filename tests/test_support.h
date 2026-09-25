#pragma once

// 依存ライブラリなしの最小テスト補助。CHECK が失敗すると内容を表示して失敗数を数える。

#include <cstdio>
#include <string>

namespace test_support {

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline int& check_count() {
    static int count = 0;
    return count;
}

inline void report_failure(const char* file, int line, const char* expression,
                           const std::string& detail) {
    ++failure_count();
    std::printf("FAILED %s:%d: %s%s%s\n", file, line, expression, detail.empty() ? "" : " -- ",
                detail.c_str());
}

}  // namespace test_support

#define CHECK(expression)                                                            \
    do {                                                                             \
        ++test_support::check_count();                                               \
        if (!(expression)) {                                                         \
            test_support::report_failure(__FILE__, __LINE__, #expression, "");       \
        }                                                                            \
    } while (false)

#define CHECK_MSG(expression, detail)                                                \
    do {                                                                             \
        ++test_support::check_count();                                               \
        if (!(expression)) {                                                         \
            test_support::report_failure(__FILE__, __LINE__, #expression, (detail)); \
        }                                                                            \
    } while (false)
