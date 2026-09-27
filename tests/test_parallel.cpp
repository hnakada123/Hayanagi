#include "parallel.h"
#include "test_support.h"

#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

int run_parallel_tests() {
    shogi::ParallelTeam team(4);
    std::atomic_int entered{0};
    std::array<bool, 4> ran{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    team.run(4, [&](std::size_t, std::size_t worker) {
        ran[worker] = true;
        entered.fetch_add(1);
        while (entered.load() < 4 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    });
    for (bool value : ran) CHECK(value);

    // 例外を呼び出し元へ返し、全ワーカーの終了後に次の仕事を受け付ける。
    bool caught = false;
    try {
        team.run(100, [](std::size_t index, std::size_t) {
            if (index == 3) throw std::runtime_error("parallel test");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
    for (int repeat = 0; repeat < 100; ++repeat) {
        std::array<int, 137> results{};
        team.run(results.size(), [&](std::size_t i, std::size_t) { ++results[i]; });
        for (int value : results) CHECK(value == 1);
    }
    int single = 0;
    team.run(0, [&](std::size_t, std::size_t) { ++single; });
    team.run(1, [&](std::size_t, std::size_t) { ++single; });
    CHECK(single == 1);
    return test_support::failure_count();
}
