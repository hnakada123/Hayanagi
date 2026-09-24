#include "tsume.h"
#include <algorithm>

namespace shogi {

TsumeResult TsumeSearch::solve(const Position& position, Color attacker, int max_plies,
                              int time_limit_ms, const std::atomic_bool& stop) {
    attacker_ = attacker;
    stop_ = &stop;
    nodes_ = 0;
    max_plies = std::clamp(max_plies, 0, 63);
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max(1, time_limit_ms));
    cache_.clear();
    cache_.resize(static_cast<std::size_t>(max_plies + 1));
    TsumeResult result;
    const int first = position.side_to_move() == attacker ? 1 : 0;
    for (int depth = first; depth <= max_plies; depth += 2) {
        result = visit(position, depth);
        if (result.status != TsumeStatus::Limit) break;
    }
    result.nodes = nodes_;
    return result;
}

TsumeResult TsumeSearch::visit(const Position& position, int remaining) {
    ++nodes_;
    if (stop_->load()) return {TsumeStatus::Cancelled, {}, 0, 0};
    if (std::chrono::steady_clock::now() >= deadline_)
        return {TsumeStatus::Timeout, {}, 0, 0};

    const bool attack = position.side_to_move() == attacker_;
    if (!attack && !position.is_in_check(position.side_to_move()))
        return {TsumeStatus::NoMate, {}, 0, 0};

    auto& table = cache_[static_cast<std::size_t>(remaining)];
    const auto found = table.find(position.position_key());
    if (found != table.end()) return found->second;

    const auto moves = attack ? position.generate_checking_moves()
                              : position.generate_legal_moves();
    if (moves.empty())
        return {attack ? TsumeStatus::NoMate : TsumeStatus::Mate, {}, 0, 0};
    if (remaining == 0) return {TsumeStatus::Limit, {}, 0, 0};

    TsumeResult result{attack ? TsumeStatus::NoMate : TsumeStatus::Mate, {}, 0, 0};
    for (const Move& move : moves) {
        Position child = position;
        child.do_move(move);
        const auto reply = visit(child, remaining - 1);
        if (reply.status == TsumeStatus::Timeout || reply.status == TsumeStatus::Cancelled)
            return reply;
        if ((attack && reply.status == TsumeStatus::Mate) ||
            (!attack && reply.status == TsumeStatus::NoMate)) {
            result = {reply.status, move, reply.plies + 1, 0};
            break;
        }
        if (reply.status == TsumeStatus::Limit) {
            result = {TsumeStatus::Limit, move, 0, 0};
        } else if (!attack && result.status == TsumeStatus::Mate &&
                   reply.plies + 1 > result.plies) {
            result = {TsumeStatus::Mate, move, reply.plies + 1, 0};
        }
    }
    // Bound memory even when a difficult problem runs for a long time.
    if (table.size() < 100000) table[position.position_key()] = result;
    return result;
}

} // namespace shogi
