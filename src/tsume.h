#pragma once

#include "position.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace shogi {

// Limit は「指定深さ内に詰みがない」ことを表し、不詰の証明ではない。
enum class TsumeStatus { Mate, NoMate, Limit, Timeout, Cancelled };

struct TsumeResult {
    TsumeStatus status = TsumeStatus::Limit;
    Move move{};  // 攻方手番: 詰む手 / 玉方手番: 最長抵抗または逃れの手
    int plies = 0;
    std::uint64_t nodes = 0;
};

// 王手の連続による詰みを深さ優先の反復深化で探索する。
// 1 つのインスタンスを同時に複数スレッドから使ってはならない（スレッドごとに別インスタンスを持つ）。
class TsumeSearch {
public:
    TsumeSearch();

    // position から attacker が王手を続けて max_plies 手以内に詰むかを調べる。
    // 深さ 1,3,5,…（玉方手番なら 0,2,4,…）と延ばし、最初に確定した結果を返す。
    TsumeResult solve(const Position& position, Color attacker, int max_plies,
                      int time_limit_ms, const std::atomic_bool& stop);

private:
    // 置換表エントリ（16 バイト）。詰み・不詰の証明は深さによらず再利用し、
    // 深さ打ち切りは「その深さまで詰みなし」としてだけ使う。
    struct Entry {
        std::uint64_t key = 0;
        std::uint16_t move = 0;        // 圧縮した最善手
        std::uint16_t generation = 0;  // 0 は空
        std::uint8_t mate_plies = 0;   // 証明済みの詰み手数（kUnknown なら未証明）
        std::uint8_t limit_depth = 0;  // この深さまで詰みなし（kUnknown なら未証明）
        std::uint8_t flags = 0;        // kFlagNoMate: 不詰確定
        std::uint8_t padding = 0;
    };

    static constexpr int kMaxPlies = 63;
    static constexpr std::uint8_t kUnknown = 0xFF;
    static constexpr std::uint8_t kFlagNoMate = 1;
    static constexpr std::size_t kBucketSize = 4;
    static constexpr std::size_t kInitialTableSize = std::size_t{1} << 12;
    static constexpr std::size_t kMaxTableSize = std::size_t{1} << 21;

    TsumeResult visit(int remaining, int ply);
    bool probe(std::uint64_t key, int remaining, TsumeResult& result) const;
    void store(std::uint64_t key, int remaining, const TsumeResult& result);
    const Entry* find_entry(std::uint64_t key) const;
    Entry* find_slot(std::uint64_t key);
    void insert_entry(const Entry& entry);
    void grow_table();
    void new_generation();
    static std::uint16_t pack_move(const Move& move);
    static Move unpack_move(std::uint16_t packed, const Position& position);

    Position position_;
    Color attacker_ = Color::Black;
    Color table_attacker_ = Color::Black;
    const std::atomic_bool* stop_ = nullptr;
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t nodes_ = 0;
    std::array<std::vector<Move>, kMaxPlies + 1> move_stack_;
    std::vector<Entry> table_;
    std::uint16_t generation_ = 1;
    std::size_t live_entries_ = 0;
};

}  // namespace shogi
