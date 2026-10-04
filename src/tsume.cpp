#include "tsume.h"

#include <algorithm>

namespace shogi {

namespace {

constexpr std::uint16_t kMoveValid = 1U << 15;
constexpr std::uint16_t kMovePromote = 1U << 14;
constexpr int kDropFromBase = kSquareCount;  // 打つ手は from 欄に 81 + 駒種を入れる

bool is_abort(TsumeStatus status) {
    return status == TsumeStatus::Timeout || status == TsumeStatus::Cancelled;
}

bool is_decisive_reply(bool attack, TsumeStatus status) {
    return status == (attack ? TsumeStatus::Mate : TsumeStatus::NoMate);
}

// 候補順に集約し、結論または中断が確定したら true を返す。
// 逐次探索と並列探索で、打ち切り・逃れ・最長抵抗の選択規則を共有する。
bool accumulate_reply(bool attack, const Move& move, const TsumeResult& reply,
                      TsumeResult& result) {
    if (is_abort(reply.status)) {
        result = reply;
        return true;
    }
    if (is_decisive_reply(attack, reply.status)) {
        result = TsumeResult{reply.status, move, reply.plies + 1, 0};
        return true;
    }
    if (reply.status == TsumeStatus::Limit) {
        result = TsumeResult{TsumeStatus::Limit, move, 0, 0};
    } else if (!attack && result.status == TsumeStatus::Mate && reply.plies + 1 > result.plies) {
        // 玉方は詰みまでの手数が最も長い応手を選ぶ。
        result = TsumeResult{TsumeStatus::Mate, move, reply.plies + 1, 0};
    }
    return false;
}

}  // namespace

TsumeSearch::TsumeSearch() = default;

std::uint16_t TsumeSearch::pack_move(const Move& move) {
    if (!move.is_valid()) {
        return 0;
    }
    const int from = move.drop ? kDropFromBase + static_cast<int>(move.piece) : move.from;
    return static_cast<std::uint16_t>(kMoveValid | (move.promote ? kMovePromote : 0U) |
                                      static_cast<unsigned>(from << 7) |
                                      static_cast<unsigned>(move.to));
}

Move TsumeSearch::unpack_move(std::uint16_t packed, const Position& position) {
    if ((packed & kMoveValid) == 0) {
        return Move{};
    }
    Move move;
    move.to = packed & 0x7F;
    const int from = (packed >> 7) & 0x7F;
    move.promote = (packed & kMovePromote) != 0;
    if (from >= kDropFromBase) {
        move.drop = true;
        move.from = -1;
        move.piece = static_cast<PieceType>(from - kDropFromBase);
    } else {
        move.from = from;
        move.piece = piece_type(position.piece_at(from));
    }
    return move;
}

void TsumeSearch::new_generation() {
    live_entries_ = 0;
    if (++generation_ == 0) {
        // 世代番号が一周したら全消去してやり直す
        std::fill(table_.begin(), table_.end(), Entry{});
        generation_ = 1;
    }
}

// key のエントリを返す。なければバケット内で最も価値の低いものを置き換えて確保する
TsumeSearch::Entry* TsumeSearch::find_slot(std::uint64_t key) {
    const std::size_t buckets = table_.size() / kBucketSize;
    Entry* bucket = &table_[(key & (buckets - 1)) * kBucketSize];
    Entry* victim = nullptr;
    int victim_value = 0x7FFFFFFF;
    for (std::size_t i = 0; i < kBucketSize; ++i) {
        Entry& entry = bucket[i];
        if (entry.generation == generation_ && entry.key == key) {
            return &entry;
        }
        // 空きは最優先、次に証明を持たない浅い深さ打ち切りのエントリから置き換える
        int value = -1;
        if (entry.generation == generation_) {
            value = (entry.mate_plies != kUnknown || (entry.flags & kFlagNoMate) != 0)
                        ? 1000
                        : (entry.limit_depth == kUnknown ? 0 : entry.limit_depth + 1);
        }
        if (value < victim_value) {
            victim_value = value;
            victim = &entry;
        }
    }
    if (victim_value < 0) {
        ++live_entries_;
    }
    *victim = Entry{};
    victim->key = key;
    victim->generation = generation_;
    victim->mate_plies = kUnknown;
    victim->limit_depth = kUnknown;
    return victim;
}

void TsumeSearch::insert_entry(const Entry& entry) {
    Entry* slot = find_slot(entry.key);
    *slot = entry;
}

void TsumeSearch::grow_table() {
    std::vector<Entry> old = std::move(table_);
    table_.assign(old.size() * 2, Entry{});
    live_entries_ = 0;
    for (const Entry& entry : old) {
        if (entry.generation == generation_) {
            insert_entry(entry);
        }
    }
}

const TsumeSearch::Entry* TsumeSearch::find_entry(std::uint64_t key) const {
    const std::size_t buckets = table_.size() / kBucketSize;
    const Entry* bucket = &table_[(key & (buckets - 1)) * kBucketSize];
    for (std::size_t i = 0; i < kBucketSize; ++i) {
        if (bucket[i].generation == generation_ && bucket[i].key == key) {
            return &bucket[i];
        }
    }
    return nullptr;
}

bool TsumeSearch::probe(std::uint64_t key, int remaining, TsumeResult& result) const {
    const Entry* entry = find_entry(key);
    if (entry == nullptr) {
        return false;
    }
    if ((entry->flags & kFlagNoMate) != 0) {
        result = TsumeResult{TsumeStatus::NoMate, unpack_move(entry->move, position_), 0, 0};
        return true;
    }
    if (entry->mate_plies != kUnknown && entry->mate_plies <= remaining) {
        result = TsumeResult{TsumeStatus::Mate, unpack_move(entry->move, position_),
                             entry->mate_plies, 0};
        return true;
    }
    if (entry->limit_depth != kUnknown && entry->limit_depth >= remaining) {
        result = TsumeResult{TsumeStatus::Limit, unpack_move(entry->move, position_), 0, 0};
        return true;
    }
    return false;
}

void TsumeSearch::store(std::uint64_t key, int remaining, const TsumeResult& result) {
    Entry* entry = find_slot(key);
    switch (result.status) {
        case TsumeStatus::Mate:
            if (entry->mate_plies == kUnknown || result.plies < entry->mate_plies) {
                entry->mate_plies = static_cast<std::uint8_t>(result.plies);
                entry->move = pack_move(result.move);
            }
            break;
        case TsumeStatus::NoMate:
            entry->flags = static_cast<std::uint8_t>(entry->flags | kFlagNoMate);
            entry->move = pack_move(result.move);
            break;
        case TsumeStatus::Limit:
            if (entry->limit_depth == kUnknown || remaining > entry->limit_depth) {
                entry->limit_depth = static_cast<std::uint8_t>(remaining);
                if (entry->mate_plies == kUnknown && (entry->flags & kFlagNoMate) == 0) {
                    entry->move = pack_move(result.move);
                }
            }
            break;
        default:
            break;
    }
    if (live_entries_ * 2 > table_.size() && table_.size() < kMaxTableSize) {
        grow_table();
    }
}

void TsumeSearch::prepare(Color attacker, const std::atomic_bool& stop,
                          std::chrono::steady_clock::time_point deadline) {
    attacker_ = attacker;
    stop_ = &stop;
    nodes_ = 0;
    deadline_ = deadline;
    root_cutoff_ = nullptr;
    if (table_.empty()) {
        table_.assign(kInitialTableSize, Entry{});
        table_attacker_ = attacker;
    } else if (table_attacker_ != attacker) {
        // 証明は攻方が同じ間だけ有効なので、攻方が変わったら世代を進めて捨てる
        table_attacker_ = attacker;
        new_generation();
    }
}

TsumeResult TsumeSearch::solve(const Position& position, Color attacker, int max_plies,
                              int time_limit_ms, const std::atomic_bool& stop, int threads) {
    prepare(attacker, stop, time_limit_ms == 0 ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, time_limit_ms)));
    max_plies = std::clamp(max_plies, 0, kMaxPlies);
    threads_ = std::clamp(threads, 1, 128);
    position_ = position;

    TsumeResult result;
    const int first = position_.side_to_move() == attacker ? 1 : 0;
    for (int depth = first; depth <= max_plies; depth += 2) {
        result = visit(depth, 0);
        if (result.status != TsumeStatus::Limit) break;
    }
    result.nodes = nodes_;
    return result;
}

TsumeResult TsumeSearch::visit_parallel(int remaining, int ply, const std::vector<Move>& moves) {
    const int count = std::min(threads_, static_cast<int>(moves.size()));
    if (!team_ || team_->size() != static_cast<std::size_t>(count)) {
        team_ = std::make_unique<ParallelTeam>(count);
        workers_.resize(static_cast<std::size_t>(count));
        // 浅い反復で得た証明を引き継ぐ。巨大な表の複製は避ける。
        if (table_.size() <= kInitialTableSize * 4) {
            for (TsumeSearch& worker : workers_) {
                worker.table_ = table_;
                worker.table_attacker_ = table_attacker_;
                worker.generation_ = generation_;
                worker.live_entries_ = live_entries_;
            }
        }
    }
    const bool attack = position_.side_to_move() == attacker_;
    std::vector<TsumeResult> replies(moves.size());
    std::atomic_size_t cutoff{moves.size()};
    for (TsumeSearch& worker : workers_) worker.prepare(attacker_, *stop_, deadline_);
    team_->run(moves.size(), [&](std::size_t index, std::size_t worker_index) {
        if (index > cutoff.load(std::memory_order_relaxed)) return;
        TsumeSearch& worker = workers_[worker_index];
        worker.position_ = position_;
        worker.root_cutoff_ = &cutoff;
        worker.root_index_ = index;
        MoveUndo undo;
        worker.position_.make_move(moves[index], undo);
        replies[index] = worker.visit(remaining - 1, ply + 1);
        if (is_decisive_reply(attack, replies[index].status)) {
            std::size_t previous = cutoff.load(std::memory_order_relaxed);
            while (index < previous && !cutoff.compare_exchange_weak(
                       previous, index, std::memory_order_relaxed)) {}
        }
    });
    for (TsumeSearch& worker : workers_) {
        nodes_ += worker.nodes_;
        worker.root_cutoff_ = nullptr;
    }
    // 元の候補順で集約する。先着順にすると最善手の選択が不安定になる。
    TsumeResult result{attack ? TsumeStatus::NoMate : TsumeStatus::Mate, {}, 0, 0};
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (accumulate_reply(attack, moves[i], replies[i], result)) break;
    }
    return result;
}

TsumeResult TsumeSearch::visit(int remaining, int ply) {
    ++nodes_;
    if (stop_->load(std::memory_order_relaxed)) {
        return TsumeResult{TsumeStatus::Cancelled, {}, 0, 0};
    }
    if (root_cutoff_ != nullptr && root_index_ > root_cutoff_->load(std::memory_order_relaxed)) {
        return TsumeResult{TsumeStatus::Cancelled, {}, 0, 0};
    }
    if ((nodes_ & 127) == 0 && std::chrono::steady_clock::now() >= deadline_) {
        return TsumeResult{TsumeStatus::Timeout, {}, 0, 0};
    }

    const bool attack = position_.side_to_move() == attacker_;
    if (!attack) {
        // 根以外の玉方局面は直前の王手で必ず王手がかかっている
        if (ply == 0 && !position_.is_in_check(position_.side_to_move())) {
            return TsumeResult{TsumeStatus::NoMate, {}, 0, 0};
        }
        if (remaining == 0) {
            return position_.has_legal_move() ? TsumeResult{TsumeStatus::Limit, {}, 0, 0}
                                              : TsumeResult{TsumeStatus::Mate, {}, 0, 0};
        }
    }

    const std::uint64_t key = position_.position_key();
    TsumeResult cached;
    if (probe(key, remaining, cached)) {
        return cached;
    }

    std::vector<Move>& moves = move_stack_[static_cast<std::size_t>(ply)];
    if (attack) {
        position_.generate_checking_moves(moves);
    } else {
        position_.generate_legal_moves(moves);
    }
    if (moves.empty()) {
        const TsumeResult result{attack ? TsumeStatus::NoMate : TsumeStatus::Mate, {}, 0, 0};
        store(key, remaining, result);
        return result;
    }
    if (remaining == 0) {
        return TsumeResult{TsumeStatus::Limit, {}, 0, 0};
    }

    // 小さい木は逐次処理。玉方の応手が少ない場合は次の攻方の王手で分割する。
    if (threads_ > 1 && (ply == 0 || (ply == 1 && attack)) && remaining >= 3 &&
        moves.size() >= (attack ? 2U : 4U) && (nodes_ >= 512 || moves.size() >= 24)) {
        const TsumeResult result = visit_parallel(remaining, ply, moves);
        if (!is_abort(result.status)) store(key, remaining, result);
        return result;
    }

    TsumeResult result{attack ? TsumeStatus::NoMate : TsumeStatus::Mate, {}, 0, 0};
    for (const Move& move : moves) {
        MoveUndo undo;
        position_.make_move(move, undo);
        const TsumeResult reply = visit(remaining - 1, ply + 1);
        position_.unmake_move(move, undo);
        if (accumulate_reply(attack, move, reply, result)) break;
    }
    if (!is_abort(result.status)) store(key, remaining, result);
    return result;
}

}  // namespace shogi
