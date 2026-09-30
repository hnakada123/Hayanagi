#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "position.h"
#include "parallel.h"

namespace shogi {

struct SearchOptions {
    static constexpr int kMinStrength = -15;
    static constexpr int kMaxStrength = 6;
    static constexpr int kDefaultStrength = 1;

    int max_depth = kMaxDepth;
    int time_limit_ms = 0;
    bool infinite = false;
    bool ponder = false;
    std::uint64_t node_limit = 0;
    int multi_pv = 1;
    int threads = 1;
    int moves_to_go = 0;
    bool restrict_searchmoves = false;
    std::vector<std::string> searchmoves;
    bool show_currline = false;
    bool show_refutations = false;
    int aspiration_min_depth = 5;
    int aspiration_window_cp = 50;
    bool limit_strength = false;
    int strength = kDefaultStrength;

    void apply_strength_limit();
};

enum class ScoreBound { Exact, Lower, Upper };

struct SearchInfo {
    int depth = 0;
    int seldepth = 0;
    int score_cp = 0;
    bool has_score = true;
    ScoreBound score_bound = ScoreBound::Exact;
    std::uint64_t nodes = 0;
    int elapsed_ms = 0;
    int multipv = 1;
    bool show_multipv = false;
    int hashfull_permille = 0;
    std::string current_move;
    int current_move_number = 0;
    int cpuload_permille = -1;
    std::vector<std::string> current_lines;
    std::vector<std::string> refutations;
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
    static std::optional<int> mate_distance(int score);

    SearchResult find_best_move(const Position& root,
                                const SearchOptions& options,
                                std::atomic_bool& stop,
                                const std::function<void(const SearchInfo&)>& on_info);

private:
    static constexpr int kHistoryFromBuckets = kSquareCount + kHandPieceKinds;

    struct Progress {
        std::atomic_int next_ms{1000};
        std::mutex mutex;
        std::function<void(const SearchInfo&)> callback;
        Position root;
        int threads = 1;
        double cpu_start = 0;
        std::vector<std::string> lines;
        std::vector<std::string> refutations;
    };

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
        int see = 0;
    };

    std::atomic_bool* stop_ = nullptr;
    std::atomic<std::uint64_t>* shared_nodes_ = nullptr;
    SearchOptions options_{};
    std::chrono::steady_clock::time_point start_time_{};
    std::uint64_t nodes_ = 0;
    std::uint8_t tt_generation_ = 0;
    std::uint64_t tt_key_salt_ = 0;
    bool aborted_ = false;
    std::uint64_t pending_nodes_ = 0;
    std::uint64_t next_time_check_ = 0;
    int seldepth_ = 0;
    int iteration_depth_ = 0;
    std::string current_move_;
    int current_move_number_ = 0;
    std::size_t worker_index_ = 0;
    int current_ply_ = 0;
    int next_line_ms_ = 0;
    std::array<Move, kMaxDepth + 1> current_path_{};
    std::shared_ptr<Progress> progress_;
    std::unique_ptr<ParallelTeam> team_;
    const std::atomic_size_t* mate_cutoff_ = nullptr;
    std::size_t mate_index_ = 0;
    std::uint64_t mate_probe_node_limit_ = 0;
    int mate_probe_time_limit_ms_ = 0;
    std::array<std::array<Move, 2>, kMaxDepth> killer_moves_{};
    std::array<std::array<std::array<int, kSquareCount>, kHistoryFromBuckets>, 2> history_{};

    int negamax(const Position& position, int depth, int ply, int alpha, int beta);
    int quiescence(const Position& position, int ply, int alpha, int beta, int qply = 0);
    bool mate_in_one(const Position& position, Move* mating_move = nullptr);
    int evaluate(const Position& position) const;
    int elapsed_ms() const;
    bool should_stop();
    int move_order_score(const Position& position,
                         const Move& move,
                         int ply,
                         const Move& tt_move,
                         int* see_score = nullptr) const;
    std::vector<OrderedMove> score_moves(const Position& position,
                                         const std::vector<Move>& moves,
                                         int ply,
                                         const Move& tt_move) const;
    bool is_quiet(const Position& position, const Move& move) const;
    bool can_try_null_move(const Position& position, int depth, int beta) const;
    bool has_non_pawn_material(const Position& position, Color color) const;
    bool find_forced_mate(const Position& position, int max_ply, std::vector<Move>& pv);
    bool find_forced_mate_parallel(const Position& position, int max_ply,
                                  std::vector<Move>& pv, std::vector<Search>& workers);
    bool mate_search_attack(Position& position, int remaining_ply, std::vector<Move>* pv);
    bool mate_search_defense(Position& position, int remaining_ply, std::vector<Move>* pv);
    void record_killer(int ply, const Move& move);
    void record_history(Color color, const Move& move, int depth);
    int history_score(Color color, const Move& move) const;
    void reset_state(const SearchOptions& options,
                     std::atomic_bool& stop,
                     std::chrono::steady_clock::time_point start_time,
                     std::atomic<std::uint64_t>* shared_nodes,
                     std::uint8_t tt_generation);
    void count_node();
    void flush_nodes();
    std::uint64_t current_nodes() const;
    int search_root_move(const Position& root, const Move& root_move, int depth, int alpha, int beta);
    void begin_root_move(const Position& root, const Move& move, std::size_t index);
    void publish_line(bool idle = false);
    void record_refutation(const Position& root, const Move& move, int depth, std::size_t index);
    void emit_info(SearchInfo info, bool details = false);
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
