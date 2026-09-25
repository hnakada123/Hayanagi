// Hayanagi の単体テスト実行ファイル。引数なしで全テストを実行する。

#include "test_support.h"

#include <cstdio>

int run_position_tests();
int run_tsume_tests();

int main() {
    run_position_tests();
    run_tsume_tests();
    std::printf("%d checks, %d failures\n", test_support::check_count(),
                test_support::failure_count());
    return test_support::failure_count() == 0 ? 0 : 1;
}
