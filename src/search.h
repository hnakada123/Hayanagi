#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "position.h"

namespace shogi {

struct SearchOptions {
    int max_depth = kMaxDepth;
    int time_limit_ms = 0;
    bool infinite = false;
    bool ponder = false;
    std::uint64_t node_limit = 0;
    int multi_pv = 1;
    int threads = 1;
};

struct SearchInfo {
    int depth = 0;
    int score_cp = 0;
    std::uint64_t nodes = 0;
    int elapsed_ms = 0;
    int multipv = 1;
    bool show_multipv = false;
    std::string pv;
};

struct SearchResult {
    Move best_move;
    bool has_best_move = false;
    TerminalStatus terminal;
    int score_cp = 0;
    int completed_depth = 0;
    std::uint64_t nodes = 0;
    int elapsed_ms = 0;
    int hashfull_permille = 0;
    std::string pv;
};

class Search {
public:
    static std::size_t hash_size_mb();
    static bool set_hash_size_mb(std::size_t hash_size_mb);

    SearchResult find_best_move(const Position& root,
                                const SearchOptions& options,
                                std::atomic_bool& stop,
                                const std::function<void(const SearchInfo&)>& on_info);

private:
    static constexpr int kHistoryFromBuckets = kSquareCount + kHandPieceKinds;

    struct RootMove {
        Move move;
        int order_score = 0;
        int score = -kInfinity;
        std::string pv;
        std::size_t worker_index = 0;
    };

    struct OrderedMove {
        Move move;
        int score = 0;
    };

    std::atomic_bool* stop_ = nullptr;
    std::atomic<std::uint64_t>* shared_nodes_ = nullptr;
    SearchOptions options_{};
    std::chrono::steady_clock::time_point start_time_{};
    std::uint64_t nodes_ = 0;
    std::uint8_t tt_generation_ = 0;
    bool aborted_ = false;
    std::array<std::array<Move, 2>, kMaxDepth> killer_moves_{};
    std::array<std::array<std::array<int, kSquareCount>, kHistoryFromBuckets>, 2> history_{};

    int negamax(const Position& position, int depth, int ply, int alpha, int beta);
    int quiescence(const Position& position, int ply, int alpha, int beta);
    int evaluate(const Position& position) const;
    int elapsed_ms() const;
    bool should_stop();
    int move_order_score(const Position& position,
                         const Move& move,
                         int ply,
                         const Move& tt_move) const;
    std::vector<OrderedMove> score_moves(const Position& position,
                                         const std::vector<Move>& moves,
                                         int ply,
                                         const Move& tt_move) const;
    bool is_quiet(const Position& position, const Move& move) const;
    bool can_try_null_move(const Position& position, int depth, int beta, int static_eval) const;
    bool has_non_pawn_material(const Position& position, Color color) const;
    bool find_forced_mate(const Position& position, int max_ply, std::vector<Move>& pv);
    bool mate_search_attack(const Position& position, int remaining_ply, std::vector<Move>* pv);
    bool mate_search_defense(const Position& position, int remaining_ply, std::vector<Move>* pv);
    void record_killer(int ply, const Move& move);
    void record_history(Color color, const Move& move, int depth);
    int history_score(Color color, const Move& move) const;
    void reset_state(const SearchOptions& options,
                     std::atomic_bool& stop,
                     std::chrono::steady_clock::time_point start_time,
                     std::atomic<std::uint64_t>* shared_nodes,
                     std::uint8_t tt_generation);
    void count_node();
    std::uint64_t current_nodes() const;
    int search_root_move(const Position& root, const Move& root_move, int depth);
    int hashfull_permille() const;
    std::string format_pv(const Position& root, const std::vector<Move>& moves) const;
    std::string build_pv(const Position& root, const Move& root_move, int max_length) const;
    void store_tt(std::uint64_t key,
                  int depth,
                  int ply,
                  int score,
                  int alpha,
                  int beta,
                  const Move& best_move);
};

}  // namespace shogi
