#pragma once

#include "position.h"
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace shogi {

// Limit means no mate within the requested depth, NOT a proof of no mate.
enum class TsumeStatus { Mate, NoMate, Limit, Timeout, Cancelled };

struct TsumeResult {
    TsumeStatus status = TsumeStatus::Limit;
    Move move{}; // attack: mating move; defense: longest resistance or refutation
    int plies = 0;
    std::uint64_t nodes = 0;
};

class TsumeSearch {
public:
    TsumeResult solve(const Position& position, Color attacker, int max_plies,
                      int time_limit_ms, const std::atomic_bool& stop);

private:
    TsumeResult visit(const Position& position, int remaining);
    Color attacker_ = Color::Black;
    const std::atomic_bool* stop_ = nullptr;
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t nodes_ = 0;
    // Separate tables per remaining depth; never reuse a depth cutoff as a proof.
    std::vector<std::unordered_map<std::uint64_t, TsumeResult>> cache_;
};

} // namespace shogi
