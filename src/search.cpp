#include "search.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <thread>
#include <unordered_set>

namespace shogi {

namespace {

constexpr int kMateThreshold = kMateScore - 256;
constexpr int kTempoBonus = 12;
constexpr int kNullMoveBaseReduction = 2;
constexpr int kMateSearchMaxPly = 5;
constexpr std::size_t kTtClusterSize = 4;
constexpr std::size_t kDefaultHashSizeMb = 16;

constexpr int kKingDirections[][2] = {
    {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
    {0, 1},   {1, -1}, {1, 0},  {1, 1},
};
constexpr int kPawnDirections[][2] = {{-1, 0}};
constexpr int kKnightDirections[][2] = {{-2, -1}, {-2, 1}};
constexpr int kSilverDirections[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {1, -1}, {1, 1}};
constexpr int kGoldDirections[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, 0}};
constexpr int kBishopDirections[][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
constexpr int kRookDirections[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
constexpr int kLanceDirections[][2] = {{-1, 0}};

int terminal_score(const TerminalStatus& status, int ply) {
    switch (status.outcome) {
        case TerminalOutcome::Win:
            return kMateScore - ply;
        case TerminalOutcome::Loss:
            return -kMateScore + ply;
        case TerminalOutcome::Draw:
            return 0;
        case TerminalOutcome::None:
        default:
            return 0;
    }
}

bool same_move(const Move& lhs, const Move& rhs) {
    return lhs.from == rhs.from && lhs.to == rhs.to && lhs.piece == rhs.piece &&
           lhs.promote == rhs.promote && lhs.drop == rhs.drop;
}

int orientation(Color color) {
    return color == Color::Black ? 1 : -1;
}

int forward_progress(Color color, int row) {
    return color == Color::Black ? 8 - row : row;
}

int center_distance(int row, int col) {
    return std::abs(row - 4) + std::abs(col - 4);
}

int center_bonus(int row, int col) {
    return 8 - center_distance(row, col);
}

int from_bucket(const Move& move) {
    return move.drop ? kSquareCount + hand_index(move.piece) : move.from;
}

int score_to_tt(int score, int ply) {
    if (score >= kMateThreshold) {
        return score + ply;
    }
    if (score <= -kMateThreshold) {
        return score - ply;
    }
    return score;
}

int score_from_tt(int score, int ply) {
    if (score >= kMateThreshold) {
        return score - ply;
    }
    if (score <= -kMateThreshold) {
        return score + ply;
    }
    return score;
}

int hand_bonus(PieceType type) {
    switch (type) {
        case PieceType::Pawn:
            return 18;
        case PieceType::Lance:
        case PieceType::Knight:
            return 24;
        case PieceType::Silver:
            return 28;
        case PieceType::Gold:
            return 24;
        case PieceType::Bishop:
            return 36;
        case PieceType::Rook:
            return 42;
        default:
            return 0;
    }
}

int camp_bonus(PieceType type) {
    switch (type) {
        case PieceType::Pawn:
            return 10;
        case PieceType::Lance:
        case PieceType::Knight:
            return 8;
        case PieceType::Silver:
        case PieceType::Gold:
            return 10;
        case PieceType::Bishop:
        case PieceType::Rook:
            return 12;
        case PieceType::ProPawn:
        case PieceType::ProLance:
        case PieceType::ProKnight:
        case PieceType::ProSilver:
            return 14;
        case PieceType::Horse:
        case PieceType::Dragon:
            return 18;
        default:
            return 0;
    }
}

enum class BoundType : std::uint8_t {
    Exact = 0,
    Lower,
    Upper,
};

struct TTEntry {
    std::uint64_t key = 0;
    int depth = -1;
    int score = 0;
    BoundType bound = BoundType::Exact;
    Move best_move;
    std::uint8_t generation = 0;
};

struct AtomicTTEntry {
    std::atomic<std::uint64_t> key{0};
    std::atomic<std::uint64_t> payload{0};
};

struct AtomicTTCluster {
    std::array<AtomicTTEntry, kTtClusterSize> entries{};
};

struct TTStorage {
    explicit TTStorage(std::size_t clusters)
        : cluster_count(clusters), cluster_mask(clusters - 1),
          data(std::make_unique<AtomicTTCluster[]>(clusters)) {}

    std::size_t cluster_count = 0;
    std::size_t cluster_mask = 0;
    std::unique_ptr<AtomicTTCluster[]> data;
};

std::uint32_t encode_move(const Move& move) {
    const bool valid = move.is_valid();
    const std::uint32_t from = valid ? static_cast<std::uint32_t>(move.from + 1) : 0;
    const std::uint32_t to = valid ? static_cast<std::uint32_t>(move.to) : 0;
    const std::uint32_t piece = valid ? static_cast<std::uint32_t>(move.piece) : 0;
    return from | (to << 7) | (piece << 14) |
           (static_cast<std::uint32_t>(move.promote) << 18) |
           (static_cast<std::uint32_t>(move.drop) << 19);
}

Move decode_move(std::uint32_t encoded) {
    Move move;
    move.from = static_cast<int>(encoded & 0x7FU) - 1;
    move.to = static_cast<int>((encoded >> 7) & 0x7FU);
    move.piece = static_cast<PieceType>((encoded >> 14) & 0x0FU);
    move.promote = ((encoded >> 18) & 0x01U) != 0;
    move.drop = ((encoded >> 19) & 0x01U) != 0;
    return move;
}

std::uint64_t pack_tt_payload(const TTEntry& entry) {
    const std::uint16_t score_bits = static_cast<std::uint16_t>(static_cast<std::int16_t>(entry.score));
    const std::uint8_t depth_bits = static_cast<std::uint8_t>(std::max(entry.depth, -1) + 1);
    const std::uint64_t move_bits = encode_move(entry.best_move);

    return move_bits | (static_cast<std::uint64_t>(score_bits) << 20) |
           (static_cast<std::uint64_t>(depth_bits) << 36) |
           (static_cast<std::uint64_t>(static_cast<std::uint8_t>(entry.bound)) << 44) |
           (static_cast<std::uint64_t>(entry.generation) << 46);
}

TTEntry unpack_tt_entry(std::uint64_t key, std::uint64_t payload) {
    TTEntry entry;
    entry.key = key;
    entry.best_move = decode_move(static_cast<std::uint32_t>(payload & ((1ULL << 20) - 1)));
    entry.score = static_cast<int>(static_cast<std::int16_t>((payload >> 20) & 0xFFFFULL));
    entry.depth = static_cast<int>((payload >> 36) & 0xFFULL) - 1;
    entry.bound = static_cast<BoundType>((payload >> 44) & 0x03ULL);
    entry.generation = static_cast<std::uint8_t>((payload >> 46) & 0xFFULL);
    return entry;
}

class SharedTranspositionTable {
public:
    SharedTranspositionTable() {
        resize_mb(kDefaultHashSizeMb);
    }

    std::uint8_t next_generation() {
        while (true) {
            const std::uint64_t next =
                generation_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
            const std::uint8_t generation = static_cast<std::uint8_t>(next);
            if (generation != 0) {
                return generation;
            }
        }
    }

    std::size_t size_mb() const {
        return hash_size_mb_.load(std::memory_order_relaxed);
    }

    bool resize_mb(std::size_t hash_size_mb) {
        hash_size_mb = std::max<std::size_t>(1, hash_size_mb);
        const std::size_t requested_bytes = hash_size_mb * 1024ULL * 1024ULL;
        const std::size_t cluster_bytes = sizeof(AtomicTTCluster);
        std::size_t cluster_count = std::max<std::size_t>(1, requested_bytes / cluster_bytes);
        cluster_count = highest_power_of_two(cluster_count);

        try {
            TTStorage storage(cluster_count);
            storage_ = std::move(storage);
            hash_size_mb_.store(hash_size_mb, std::memory_order_relaxed);
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    bool probe(std::uint64_t key, TTEntry& out) const {
        const TTStorage& storage = storage_;
        const std::size_t cluster_index = index_for(key, storage.cluster_mask);
        const AtomicTTCluster& cluster = storage.data[cluster_index];

        for (const AtomicTTEntry& atomic_entry : cluster.entries) {
            const std::uint64_t stored_key = atomic_entry.key.load(std::memory_order_acquire);
            if (stored_key != key) {
                continue;
            }
            const std::uint64_t payload = atomic_entry.payload.load(std::memory_order_relaxed);
            if (atomic_entry.key.load(std::memory_order_acquire) == stored_key) {
                out = unpack_tt_entry(stored_key, payload);
                return true;
            }
        }
        return false;
    }

    void store(const TTEntry& entry) {
        const TTStorage& storage = storage_;
        const std::size_t cluster_index = index_for(entry.key, storage.cluster_mask);
        AtomicTTCluster& cluster = storage.data[cluster_index];
        AtomicTTEntry* replacement = &cluster.entries.front();
        TTEntry replacement_entry;
        bool replacement_initialized = false;

        for (AtomicTTEntry& atomic_entry : cluster.entries) {
            const std::uint64_t stored_key = atomic_entry.key.load(std::memory_order_acquire);
            const TTEntry current = unpack_tt_entry(stored_key,
                                                    atomic_entry.payload.load(std::memory_order_relaxed));
            if (stored_key == entry.key) {
                if (current.depth > entry.depth && current.generation == entry.generation) {
                    return;
                }
                atomic_entry.payload.store(pack_tt_payload(entry), std::memory_order_relaxed);
                atomic_entry.key.store(entry.key, std::memory_order_release);
                return;
            }
            if (stored_key == 0) {
                atomic_entry.payload.store(pack_tt_payload(entry), std::memory_order_relaxed);
                atomic_entry.key.store(entry.key, std::memory_order_release);
                return;
            }
            if (!replacement_initialized ||
                replacement_score(current, entry.generation) >
                    replacement_score(replacement_entry, entry.generation)) {
                replacement = &atomic_entry;
                replacement_entry = current;
                replacement_initialized = true;
            }
        }

        replacement->payload.store(pack_tt_payload(entry), std::memory_order_relaxed);
        replacement->key.store(entry.key, std::memory_order_release);
    }

    int hashfull_permille(std::uint8_t generation) const {
        if (generation == 0) {
            return 0;
        }

        const TTStorage& storage = storage_;
        constexpr std::size_t kSampleEntries = 1000;
        int used = 0;
        for (std::size_t sample = 0; sample < kSampleEntries; ++sample) {
            const std::size_t entry_index =
                sample * (storage.cluster_count * kTtClusterSize) / kSampleEntries;
            const std::size_t cluster_index = entry_index / kTtClusterSize;
            const std::size_t slot = entry_index % kTtClusterSize;
            const AtomicTTEntry& atomic_entry = storage.data[cluster_index].entries[slot];
            const std::uint64_t key = atomic_entry.key.load(std::memory_order_acquire);
            if (key == 0) {
                continue;
            }
            const TTEntry entry = unpack_tt_entry(key, atomic_entry.payload.load(std::memory_order_relaxed));
            if (entry.generation == generation) {
                ++used;
            }
        }
        return used;
    }

private:
    static std::size_t highest_power_of_two(std::size_t value) {
        std::size_t power = 1;
        while ((power << 1) != 0 && (power << 1) <= value) {
            power <<= 1;
        }
        return power;
    }

    static std::size_t replacement_score(const TTEntry& entry, std::uint8_t generation) {
        const std::size_t age_penalty = entry.generation == generation ? 0 : 1024;
        const int bounded_depth = std::max(entry.depth, -1);
        return age_penalty + static_cast<std::size_t>(255 - std::min(bounded_depth + 1, 255));
    }

    static std::size_t index_for(std::uint64_t key, std::size_t cluster_mask) {
        return static_cast<std::size_t>((key ^ (key >> 32)) & cluster_mask);
    }

    TTStorage storage_{1};
    std::atomic<std::uint64_t> generation_counter_{0};
    std::atomic<std::size_t> hash_size_mb_{kDefaultHashSizeMb};
};

SharedTranspositionTable& shared_tt() {
    static SharedTranspositionTable table;
    return table;
}

int piece_square_bonus(PieceType type, Color color, int row, int col) {
    const int progress = forward_progress(color, row);
    const int center = center_bonus(row, col);

    switch (type) {
        case PieceType::Pawn:
            return progress * 10 - std::abs(col - 4) * 2;
        case PieceType::Lance:
            return progress * 6 - std::abs(col - 4) * 2;
        case PieceType::Knight:
            return progress * 7 + center * 4;
        case PieceType::Silver:
            return progress * 5 + center * 5;
        case PieceType::Gold:
            return progress * 4 + center * 3;
        case PieceType::Bishop:
            return center * 8 + progress * 2;
        case PieceType::Rook:
            return center * 5 + progress * 2;
        case PieceType::ProPawn:
        case PieceType::ProLance:
        case PieceType::ProKnight:
        case PieceType::ProSilver:
            return progress * 4 + center * 5;
        case PieceType::Horse:
            return center * 12 + progress * 3;
        case PieceType::Dragon:
            return center * 10 + progress * 3;
        case PieceType::King:
        case PieceType::Empty:
        default:
            return 0;
    }
}

template <std::size_t N>
int count_step_mobility(const Position& position,
                        int square,
                        const int (&directions)[N][2],
                        Color color) {
    const int sign = orientation(color);
    const int row = square_row(square);
    const int col = square_col(square);
    int mobility = 0;

    for (const auto& direction : directions) {
        const int to_row = row + direction[0] * sign;
        const int to_col = col + direction[1] * sign;
        if (!is_on_board(to_row, to_col)) {
            continue;
        }
        const int target = position.piece_at(make_square(to_row, to_col));
        if (target == 0 || piece_color(target) != color) {
            ++mobility;
        }
    }
    return mobility;
}

template <std::size_t N>
int count_slider_mobility(const Position& position,
                          int square,
                          const int (&directions)[N][2],
                          Color color) {
    const int sign = orientation(color);
    const int row = square_row(square);
    const int col = square_col(square);
    int mobility = 0;

    for (const auto& direction : directions) {
        int to_row = row + direction[0] * sign;
        int to_col = col + direction[1] * sign;
        while (is_on_board(to_row, to_col)) {
            const int target = position.piece_at(make_square(to_row, to_col));
            if (target != 0 && piece_color(target) == color) {
                break;
            }
            ++mobility;
            if (target != 0) {
                break;
            }
            to_row += direction[0] * sign;
            to_col += direction[1] * sign;
        }
    }

    return mobility;
}

int mobility_bonus(const Position& position, int square, PieceType type, Color color) {
    switch (type) {
        case PieceType::Pawn:
            return count_step_mobility(position, square, kPawnDirections, color) * 2;
        case PieceType::Lance:
            return count_slider_mobility(position, square, kLanceDirections, color) * 2;
        case PieceType::Knight:
            return count_step_mobility(position, square, kKnightDirections, color) * 3;
        case PieceType::Silver:
            return count_step_mobility(position, square, kSilverDirections, color) * 3;
        case PieceType::Gold:
            return count_step_mobility(position, square, kGoldDirections, color) * 2;
        case PieceType::Bishop:
            return count_slider_mobility(position, square, kBishopDirections, color) * 5;
        case PieceType::Rook:
            return count_slider_mobility(position, square, kRookDirections, color) * 4;
        case PieceType::King:
            return 0;
        case PieceType::ProPawn:
        case PieceType::ProLance:
        case PieceType::ProKnight:
        case PieceType::ProSilver:
            return count_step_mobility(position, square, kGoldDirections, color) * 3;
        case PieceType::Horse:
            return count_slider_mobility(position, square, kBishopDirections, color) * 5 +
                   count_step_mobility(position, square, kRookDirections, color) * 2;
        case PieceType::Dragon:
            return count_slider_mobility(position, square, kRookDirections, color) * 4 +
                   count_step_mobility(position, square, kBishopDirections, color) * 3;
        case PieceType::Empty:
        default:
            return 0;
    }
}

int king_safety_score(const Position& position, Color color, int phase) {
    const int king_square = position.find_king(color);
    if (king_square == -1) {
        return 0;
    }

    const int row = square_row(king_square);
    const int col = square_col(king_square);
    const int opening_weight = std::min(phase, 96);
    const int endgame_weight = 96 - opening_weight;
    int defenders = 0;
    int enemy_attacks = 0;
    int safe_squares = 0;

    for (const auto& direction : kKingDirections) {
        const int to_row = row + direction[0];
        const int to_col = col + direction[1];
        if (!is_on_board(to_row, to_col)) {
            continue;
        }
        const int square = make_square(to_row, to_col);
        const int piece = position.piece_at(square);
        if (piece != 0 && piece_color(piece) == color) {
            ++defenders;
        }
        if (position.is_square_attacked(square, opposite(color))) {
            ++enemy_attacks;
        } else {
            ++safe_squares;
        }
    }

    int score = defenders * (6 + opening_weight / 12);
    score -= enemy_attacks * (10 + opening_weight / 10);
    score += safe_squares * 2;

    const int distance = center_distance(row, col);
    score += distance * (opening_weight / 6);
    score += center_bonus(row, col) * (endgame_weight / 6);

    const int front_row_delta = -orientation(color);
    for (int step = 1; step <= 2; ++step) {
        const int front_row = row + front_row_delta * step;
        if (!is_on_board(front_row, col)) {
            continue;
        }
        const int square = make_square(front_row, col);
        const int piece = position.piece_at(square);
        if (piece == 0) {
            score -= 4 + opening_weight / 16;
        } else if (piece_color(piece) != color) {
            score -= 8 + opening_weight / 14;
        }
        if (position.is_square_attacked(square, opposite(color))) {
            score -= 8;
        }
    }

    return score;
}

}  // namespace

std::size_t Search::hash_size_mb() {
    return shared_tt().size_mb();
}

bool Search::set_hash_size_mb(std::size_t hash_size_mb) {
    return shared_tt().resize_mb(hash_size_mb);
}

void Search::reset_state(const SearchOptions& options,
                         std::atomic_bool& stop,
                         std::chrono::steady_clock::time_point start_time,
                         std::atomic<std::uint64_t>* shared_nodes,
                         std::uint8_t tt_generation) {
    stop_ = &stop;
    shared_nodes_ = shared_nodes;
    options_ = options;
    start_time_ = start_time;
    nodes_ = 0;
    tt_generation_ = tt_generation;
    aborted_ = false;
    for (auto& ply_killers : killer_moves_) {
        ply_killers = {Move{}, Move{}};
    }
    for (auto& color_history : history_) {
        for (auto& from_history : color_history) {
            from_history.fill(0);
        }
    }
}

void Search::count_node() {
    ++nodes_;
    if (shared_nodes_ != nullptr) {
        shared_nodes_->fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t Search::current_nodes() const {
    if (shared_nodes_ != nullptr) {
        return shared_nodes_->load(std::memory_order_relaxed);
    }
    return nodes_;
}

int Search::search_root_move(const Position& root, const Move& root_move, int depth) {
    if (should_stop()) {
        aborted_ = true;
        return 0;
    }

    Position child = root;
    child.do_move(root_move);
    return -negamax(child, depth - 1, 1, -kInfinity, kInfinity);
}

SearchResult Search::find_best_move(const Position& root,
                                    const SearchOptions& options,
                                    std::atomic_bool& stop,
                                    const std::function<void(const SearchInfo&)>& on_info) {
    const std::uint8_t tt_generation = shared_tt().next_generation();
    reset_state(options, stop, std::chrono::steady_clock::now(), nullptr, tt_generation);

    SearchResult result;
    result.terminal = root.terminal_status();
    if (result.terminal.is_terminal()) {
        result.score_cp = terminal_score(result.terminal, 0);
        result.elapsed_ms = elapsed_ms();
        result.hashfull_permille = hashfull_permille();
        return result;
    }

    auto legal_moves = root.generate_search_legal_moves();
    if (legal_moves.empty()) {
        result.elapsed_ms = elapsed_ms();
        return result;
    }
    const int multi_pv = std::max(1, std::min(options.multi_pv, static_cast<int>(legal_moves.size())));

    std::vector<Move> mating_line;
    if ((options.infinite || options.max_depth >= 3 || options.time_limit_ms >= 200) &&
        find_forced_mate(root, kMateSearchMaxPly, mating_line) && !mating_line.empty()) {
        result.best_move = mating_line.front();
        result.has_best_move = true;
        result.score_cp = kMateScore - 1;
        result.completed_depth = kMateSearchMaxPly;
        result.nodes = current_nodes();
        result.elapsed_ms = elapsed_ms();
        result.hashfull_permille = hashfull_permille();
        SearchInfo info;
        info.depth = kMateSearchMaxPly;
        info.score_cp = result.score_cp;
        info.nodes = result.nodes;
        info.elapsed_ms = result.elapsed_ms;
        info.multipv = 1;
        info.show_multipv = multi_pv > 1;
        info.pv = format_pv(root, mating_line);
        result.pv = info.pv;
        on_info(info);
        return result;
    }

    const int thread_count = std::max(1, std::min(options.threads, static_cast<int>(legal_moves.size())));
    std::atomic<std::uint64_t> shared_nodes{nodes_};
    std::vector<Search> workers;
    if (thread_count > 1) {
        workers.resize(static_cast<std::size_t>(thread_count));
        for (Search& worker : workers) {
            worker.reset_state(options, stop, start_time_, &shared_nodes, tt_generation);
        }
    }

    const auto reported_nodes = [&]() {
        if (thread_count > 1) {
            return shared_nodes.load(std::memory_order_relaxed);
        }
        return current_nodes();
    };

    const auto coordinator_should_stop = [&]() {
        if (thread_count == 1) {
            return should_stop();
        }
        if (stop.load()) {
            return true;
        }
        if (options.node_limit > 0 && reported_nodes() >= options.node_limit) {
            return true;
        }
        if (!options.infinite && options.time_limit_ms > 0 && elapsed_ms() >= options.time_limit_ms) {
            return true;
        }
        return false;
    };

    Move fallback = legal_moves.front();
    Move previous_best = fallback;
    int last_hashfull = hashfull_permille();

    for (int depth = 1; depth <= options.max_depth; ++depth) {
        if (coordinator_should_stop()) {
            break;
        }

        Move tt_move;
        TTEntry tt_entry;
        if (shared_tt().probe(root.position_key(), tt_entry)) {
            tt_move = tt_entry.best_move;
        }

        std::vector<RootMove> root_moves;
        root_moves.reserve(legal_moves.size());
        for (const Move& move : legal_moves) {
            int score = move_order_score(root, move, 0, tt_move);
            if (same_move(move, previous_best)) {
                score += 2000000;
            }
            RootMove root_move;
            root_move.move = move;
            root_move.order_score = score;
            root_moves.push_back(root_move);
        }
        std::sort(root_moves.begin(), root_moves.end(), [](const RootMove& lhs, const RootMove& rhs) {
            return lhs.order_score > rhs.order_score;
        });

        Move best_move = previous_best;
        std::size_t best_index = 0;
        bool found_move = false;
        int best_score = -kInfinity;
        int alpha = -kInfinity;
        const int beta = kInfinity;

        if (thread_count == 1) {
            if (multi_pv == 1) {
                for (std::size_t move_index = 0; move_index < root_moves.size(); ++move_index) {
                    if (should_stop()) {
                        aborted_ = true;
                        break;
                    }

                    Position child = root;
                    child.do_move(root_moves[move_index].move);

                    int score = 0;
                    if (move_index == 0) {
                        score = -negamax(child, depth - 1, 1, -beta, -alpha);
                    } else {
                        score = -negamax(child, depth - 1, 1, -alpha - 1, -alpha);
                        if (!aborted_ && score > alpha && score < beta) {
                            score = -negamax(child, depth - 1, 1, -beta, -alpha);
                        }
                    }

                    if (aborted_) {
                        break;
                    }
                    if (!found_move || score > best_score) {
                        found_move = true;
                        best_score = score;
                        best_move = root_moves[move_index].move;
                        best_index = move_index;
                    }
                    alpha = std::max(alpha, best_score);
                }
            } else {
                for (std::size_t move_index = 0; move_index < root_moves.size(); ++move_index) {
                    RootMove& root_move = root_moves[move_index];
                    if (should_stop()) {
                        aborted_ = true;
                        break;
                    }

                    Position child = root;
                    child.do_move(root_move.move);
                    root_move.score = -negamax(child, depth - 1, 1, -kInfinity, kInfinity);
                    if (aborted_) {
                        break;
                    }

                    root_move.pv = build_pv(root, root_move.move, depth);
                    if (!found_move || root_move.score > best_score) {
                        found_move = true;
                        best_score = root_move.score;
                        best_move = root_move.move;
                        best_index = move_index;
                    }
                }
            }
            last_hashfull = hashfull_permille();
        } else {
            std::atomic_size_t next_index{0};
            std::vector<std::thread> threads;
            threads.reserve(workers.size() > 0 ? workers.size() - 1 : 0);

            auto worker_body = [&](std::size_t worker_index) {
                Search& worker = workers[worker_index];
                while (true) {
                    const std::size_t move_index =
                        next_index.fetch_add(1, std::memory_order_relaxed);
                    if (move_index >= root_moves.size()) {
                        break;
                    }
                    root_moves[move_index].score =
                        worker.search_root_move(root, root_moves[move_index].move, depth);
                    root_moves[move_index].worker_index = worker_index;
                    if (worker.aborted_) {
                        break;
                    }
                }
            };

            for (std::size_t worker_index = 1; worker_index < workers.size(); ++worker_index) {
                threads.emplace_back(worker_body, worker_index);
            }
            worker_body(0);
            for (std::thread& thread : threads) {
                thread.join();
            }

            last_hashfull = hashfull_permille();
            if (next_index.load(std::memory_order_relaxed) < root_moves.size()) {
                aborted_ = true;
            }
            for (const Search& worker : workers) {
                if (worker.aborted_) {
                    aborted_ = true;
                    break;
                }
            }
            if (!aborted_) {
                for (std::size_t move_index = 0; move_index < root_moves.size(); ++move_index) {
                    const RootMove& root_move = root_moves[move_index];
                    if (!found_move || root_move.score > best_score) {
                        found_move = true;
                        best_score = root_move.score;
                        best_move = root_move.move;
                        best_index = move_index;
                    }
                }
            }
        }

        if (aborted_ || !found_move) {
            break;
        }

        previous_best = best_move;
        store_tt(root.position_key(), depth, 0, best_score, -kInfinity, kInfinity, best_move);
        std::string pv;
        if (thread_count == 1) {
            pv = build_pv(root, best_move, depth);
        } else {
            pv = workers[root_moves[best_index].worker_index].build_pv(root, best_move, depth);
        }
        result.best_move = best_move;
        result.has_best_move = true;
        result.score_cp = best_score;
        result.completed_depth = depth;
        result.nodes = reported_nodes();
        result.elapsed_ms = elapsed_ms();
        result.hashfull_permille = last_hashfull;

        if (multi_pv == 1) {
            result.pv = pv;
            SearchInfo info;
            info.depth = depth;
            info.score_cp = best_score;
            info.nodes = result.nodes;
            info.elapsed_ms = result.elapsed_ms;
            info.pv = pv;
            on_info(info);
        } else {
            std::sort(root_moves.begin(), root_moves.end(), [](const RootMove& lhs, const RootMove& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score > rhs.score;
                }
                return lhs.order_score > rhs.order_score;
            });

            result.best_move = root_moves.front().move;
            result.score_cp = root_moves.front().score;

            for (int pv_index = 0; pv_index < multi_pv; ++pv_index) {
                if (thread_count > 1) {
                    root_moves[pv_index].pv = workers[root_moves[pv_index].worker_index].build_pv(
                        root, root_moves[pv_index].move, depth);
                }
                SearchInfo info;
                info.depth = depth;
                info.score_cp = root_moves[pv_index].score;
                info.nodes = result.nodes;
                info.elapsed_ms = result.elapsed_ms;
                info.multipv = pv_index + 1;
                info.show_multipv = true;
                info.pv = root_moves[pv_index].pv;
                on_info(info);
            }
            result.pv = root_moves.front().pv;
        }

        if (options.infinite) {
            continue;
        }
    }

    if (!result.has_best_move) {
        result.best_move = fallback;
        result.has_best_move = true;
        result.elapsed_ms = elapsed_ms();
        result.nodes = reported_nodes();
    }
    result.hashfull_permille = last_hashfull;
    return result;
}

int Search::negamax(const Position& position, int depth, int ply, int alpha, int beta) {
    if (should_stop()) {
        aborted_ = true;
        return evaluate(position);
    }

    const TerminalStatus terminal = position.terminal_status();
    if (terminal.is_terminal()) {
        return terminal_score(terminal, ply);
    }
    if (depth <= 0) {
        return quiescence(position, ply, alpha, beta);
    }

    count_node();
    const bool in_check = position.is_in_check(position.side_to_move());
    const int alpha_original = alpha;
    const std::uint64_t key = position.position_key();
    Move tt_move;

    TTEntry tt_entry;
    if (shared_tt().probe(key, tt_entry)) {
        tt_move = tt_entry.best_move;
        if (tt_entry.depth >= depth) {
            const int tt_score = score_from_tt(tt_entry.score, ply);
            switch (tt_entry.bound) {
                case BoundType::Exact:
                    return tt_score;
                case BoundType::Lower:
                    alpha = std::max(alpha, tt_score);
                    break;
                case BoundType::Upper:
                    beta = std::min(beta, tt_score);
                    break;
            }
            if (alpha >= beta) {
                return tt_score;
            }
        }
    }

    const int static_eval = evaluate(position);
    if (can_try_null_move(position, depth, beta, static_eval)) {
        Position null_position = position.make_null_move();
        const int reduction = kNullMoveBaseReduction + depth / 4;
        const int score =
            -negamax(null_position, depth - 1 - reduction, ply + 1, -beta, -beta + 1);
        if (aborted_) {
            return 0;
        }
        if (score >= beta) {
            return score;
        }
    }

    auto legal_moves = position.generate_search_legal_moves();
    if (legal_moves.empty()) {
        if (in_check) {
            return -kMateScore + ply;
        }
        return 0;
    }

    const auto ordered_moves = score_moves(position, legal_moves, ply, tt_move);

    Move best_move;
    int best_score = -kInfinity;

    for (std::size_t move_index = 0; move_index < ordered_moves.size(); ++move_index) {
        const Move& move = ordered_moves[move_index].move;
        Position child = position;
        child.do_move(move);

        const bool gives_check = child.is_in_check(child.side_to_move());
        const bool quiet = is_quiet(position, move) && !gives_check;
        const bool late_move = quiet && !in_check && depth >= 4 && move_index >= 4;
        const int reduction = late_move ? (depth >= 6 && move_index >= 8 ? 2 : 1) : 0;

        int score = 0;
        if (move_index == 0) {
            score = -negamax(child, depth - 1, ply + 1, -beta, -alpha);
        } else {
            const int reduced_depth = std::max(0, depth - 1 - reduction);
            score = -negamax(child, reduced_depth, ply + 1, -alpha - 1, -alpha);
            if (!aborted_ && score > alpha) {
                score = -negamax(child, depth - 1, ply + 1, -alpha - 1, -alpha);
                if (!aborted_ && score > alpha && score < beta) {
                    score = -negamax(child, depth - 1, ply + 1, -beta, -alpha);
                }
            }
        }

        if (aborted_) {
            return 0;
        }
        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
        }
        if (alpha >= beta) {
            if (quiet) {
                record_killer(ply, move);
                record_history(position.side_to_move(), move, depth);
            }
            store_tt(key, depth, ply, best_score, alpha_original, beta, best_move);
            return best_score;
        }
    }

    store_tt(key, depth, ply, best_score, alpha_original, beta, best_move);
    return best_score;
}

int Search::quiescence(const Position& position, int ply, int alpha, int beta) {
    if (should_stop()) {
        aborted_ = true;
        return evaluate(position);
    }

    count_node();
    const TerminalStatus terminal = position.terminal_status();
    if (terminal.is_terminal()) {
        return terminal_score(terminal, ply);
    }

    const bool in_check = position.is_in_check(position.side_to_move());
    if (!in_check) {
        const int stand_pat = evaluate(position);
        if (stand_pat >= beta) {
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
    }

    auto tactical_moves = position.generate_quiescence_moves();
    if (tactical_moves.empty()) {
        if (in_check) {
            return -kMateScore + ply;
        }
        return alpha;
    }

    const Move no_tt_move;
    const auto ordered_moves = score_moves(position, tactical_moves, ply, no_tt_move);
    for (const OrderedMove& ordered_move : ordered_moves) {
        const Move& move = ordered_move.move;
        const int captured = position.piece_at(move.to);
        if (!in_check && captured != 0 && position.static_exchange_eval(move) < -120) {
            continue;
        }
        Position child = position;
        child.do_move(move);
        const int score = -quiescence(child, ply + 1, -beta, -alpha);
        if (aborted_) {
            return 0;
        }
        if (score >= beta) {
            return score;
        }
        alpha = std::max(alpha, score);
    }

    return alpha;
}

int Search::evaluate(const Position& position) const {
    int black_score = 0;
    int white_score = 0;
    int phase = 0;

    for (int square = 0; square < kSquareCount; ++square) {
        const int piece = position.piece_at(square);
        if (piece == 0) {
            continue;
        }

        const Color color = piece_color(piece);
        const PieceType type = piece_type(piece);
        const int row = square_row(square);
        const int col = square_col(square);

        int contribution = piece_value(type);
        if (type != PieceType::King) {
            contribution += piece_square_bonus(type, color, row, col);
            contribution += mobility_bonus(position, square, type, color);
            if (is_in_promotion_zone(color, row)) {
                contribution += camp_bonus(type);
            }
            phase += piece_value(unpromote(type)) / 100;
        }

        if (color == Color::Black) {
            black_score += contribution;
        } else {
            white_score += contribution;
        }
    }

    for (int index = 1; index <= static_cast<int>(PieceType::Rook); ++index) {
        const PieceType piece = static_cast<PieceType>(index);
        const int material = piece_value(piece);
        const int bonus = hand_bonus(piece);
        const int black_count = position.hand_count(Color::Black, piece);
        const int white_count = position.hand_count(Color::White, piece);

        black_score += black_count * (material + bonus);
        white_score += white_count * (material + bonus);
        phase += (black_count + white_count) * (material / 100);
    }

    black_score += king_safety_score(position, Color::Black, phase);
    white_score += king_safety_score(position, Color::White, phase);

    if (position.is_in_check(Color::Black)) {
        black_score -= 40;
    }
    if (position.is_in_check(Color::White)) {
        white_score -= 40;
    }

    int score = black_score - white_score;
    score += position.side_to_move() == Color::Black ? kTempoBonus : -kTempoBonus;
    return position.side_to_move() == Color::Black ? score : -score;
}

int Search::elapsed_ms() const {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count());
}

bool Search::should_stop() {
    if (stop_ != nullptr && stop_->load()) {
        return true;
    }
    if (options_.node_limit > 0 && current_nodes() >= options_.node_limit) {
        return true;
    }
    if (!options_.infinite && options_.time_limit_ms > 0 && elapsed_ms() >= options_.time_limit_ms) {
        return true;
    }
    return false;
}

int Search::move_order_score(const Position& position,
                             const Move& move,
                             int ply,
                             const Move& tt_move) const {
    int score = 0;
    if (same_move(move, tt_move)) {
        score += 4000000;
    }

    const int captured = position.piece_at(move.to);
    if (captured != 0) {
        const int see = position.static_exchange_eval(move);
        score += 120000 + piece_value(piece_type(captured)) * 12 + see * 24;
        if (see < 0) {
            score += see * 48;
        }
    }
    if (move.promote) {
        score += 1200;
    }
    if (move.drop) {
        score += 200 + piece_value(move.piece) / 8;
    }

    if (ply < kMaxDepth) {
        if (same_move(move, killer_moves_[ply][0])) {
            score += 90000;
        } else if (same_move(move, killer_moves_[ply][1])) {
            score += 80000;
        }
    }

    if (captured == 0 && !move.promote) {
        score += history_score(position.side_to_move(), move);
    }

    return score;
}

std::vector<Search::OrderedMove> Search::score_moves(const Position& position,
                                                     const std::vector<Move>& moves,
                                                     int ply,
                                                     const Move& tt_move) const {
    std::vector<OrderedMove> ordered_moves;
    ordered_moves.reserve(moves.size());

    for (const Move& move : moves) {
        ordered_moves.push_back(OrderedMove{move, move_order_score(position, move, ply, tt_move)});
    }

    std::sort(ordered_moves.begin(),
              ordered_moves.end(),
              [](const OrderedMove& lhs, const OrderedMove& rhs) {
                  return lhs.score > rhs.score;
              });
    return ordered_moves;
}

bool Search::is_quiet(const Position& position, const Move& move) const {
    return !move.promote && position.piece_at(move.to) == 0;
}

bool Search::can_try_null_move(const Position& position,
                               int depth,
                               int beta,
                               int static_eval) const {
    if (depth < 3) {
        return false;
    }
    if (beta >= kMateThreshold || beta <= -kMateThreshold) {
        return false;
    }
    if (position.is_in_check(position.side_to_move())) {
        return false;
    }
    if (!has_non_pawn_material(position, position.side_to_move())) {
        return false;
    }
    return static_eval >= beta - 96 - depth * 16;
}

bool Search::has_non_pawn_material(const Position& position, Color color) const {
    for (int square = 0; square < kSquareCount; ++square) {
        const int piece = position.piece_at(square);
        if (piece == 0 || piece_color(piece) != color) {
            continue;
        }
        const PieceType type = unpromote(piece_type(piece));
        if (type != PieceType::King && type != PieceType::Pawn) {
            return true;
        }
    }
    for (int index = static_cast<int>(PieceType::Lance);
         index <= static_cast<int>(PieceType::Rook);
         ++index) {
        if (position.hand_count(color, static_cast<PieceType>(index)) > 0) {
            return true;
        }
    }
    return false;
}

bool Search::find_forced_mate(const Position& position, int max_ply, std::vector<Move>& pv) {
    pv.clear();
    const int normalized_ply = std::max(1, max_ply | 1);
    return mate_search_attack(position, normalized_ply, &pv);
}

bool Search::mate_search_attack(const Position& position,
                                int remaining_ply,
                                std::vector<Move>* pv) {
    if (should_stop()) {
        aborted_ = true;
        return false;
    }
    count_node();

    if (remaining_ply <= 0) {
        return false;
    }

    auto checking_moves = position.generate_checking_moves();
    if (checking_moves.empty()) {
        return false;
    }

    const Move no_tt_move;
    const auto ordered_moves = score_moves(position, checking_moves, 0, no_tt_move);
    for (const OrderedMove& ordered_move : ordered_moves) {
        const Move& move = ordered_move.move;
        Position child = position;
        child.do_move(move);
        std::vector<Move> defense_line;
        std::vector<Move>* next_pv = pv != nullptr ? &defense_line : nullptr;
        if (mate_search_defense(child, remaining_ply - 1, next_pv)) {
            if (pv != nullptr) {
                pv->clear();
                pv->push_back(move);
                pv->insert(pv->end(), defense_line.begin(), defense_line.end());
            }
            return true;
        }
        if (aborted_) {
            return false;
        }
    }

    return false;
}

bool Search::mate_search_defense(const Position& position,
                                 int remaining_ply,
                                 std::vector<Move>* pv) {
    if (should_stop()) {
        aborted_ = true;
        return false;
    }
    count_node();

    auto legal_moves = position.generate_search_legal_moves();
    if (legal_moves.empty()) {
        return position.is_in_check(position.side_to_move());
    }
    if (remaining_ply <= 0) {
        return false;
    }

    const Move no_tt_move;
    const auto ordered_moves = score_moves(position, legal_moves, 0, no_tt_move);

    Move selected_move;
    std::vector<Move> selected_line;
    std::size_t selected_length = 0;
    bool has_selected = false;

    for (const OrderedMove& ordered_move : ordered_moves) {
        const Move& move = ordered_move.move;
        Position child = position;
        child.do_move(move);
        std::vector<Move> attack_line;
        std::vector<Move>* next_pv = pv != nullptr ? &attack_line : nullptr;
        if (!mate_search_attack(child, remaining_ply - 1, next_pv)) {
            return false;
        }
        if (pv != nullptr) {
            const std::size_t candidate_length = 1 + attack_line.size();
            if (!has_selected || candidate_length > selected_length) {
                has_selected = true;
                selected_move = move;
                selected_length = candidate_length;
                selected_line = std::move(attack_line);
            }
        }
        if (aborted_) {
            return false;
        }
    }

    if (pv != nullptr) {
        pv->clear();
        if (has_selected) {
            pv->push_back(selected_move);
            pv->insert(pv->end(), selected_line.begin(), selected_line.end());
        }
    }

    return true;
}

void Search::record_killer(int ply, const Move& move) {
    if (ply >= kMaxDepth || same_move(move, killer_moves_[ply][0])) {
        return;
    }
    killer_moves_[ply][1] = killer_moves_[ply][0];
    killer_moves_[ply][0] = move;
}

void Search::record_history(Color color, const Move& move, int depth) {
    const int bucket = from_bucket(move);
    int& score = history_[static_cast<int>(color)][bucket][move.to];
    score += depth * depth * 8;
    score -= score / 16;
}

int Search::history_score(Color color, const Move& move) const {
    return history_[static_cast<int>(color)][from_bucket(move)][move.to];
}

int Search::hashfull_permille() const {
    return shared_tt().hashfull_permille(tt_generation_);
}

std::string Search::format_pv(const Position& root, const std::vector<Move>& moves) const {
    Position position = root;
    std::string pv;

    for (const Move& move : moves) {
        if (position.terminal_status().is_terminal() || !move.is_valid()) {
            break;
        }

        const auto legal_moves = position.generate_legal_moves();
        const auto move_it =
            std::find_if(legal_moves.begin(), legal_moves.end(), [&](const Move& candidate) {
                return same_move(candidate, move);
            });
        if (move_it == legal_moves.end()) {
            break;
        }

        if (!pv.empty()) {
            pv += ' ';
        }
        pv += position.move_to_usi(*move_it);
        position.do_move(*move_it);
    }

    return pv;
}

std::string Search::build_pv(const Position& root, const Move& root_move, int max_length) const {
    if (!root_move.is_valid() || max_length <= 0) {
        return "";
    }

    Position position = root;
    Move move = root_move;
    std::string pv;
    std::unordered_set<std::uint64_t> seen_keys;
    seen_keys.insert(position.position_key());

    for (int ply = 0; ply < max_length; ++ply) {
        if (position.terminal_status().is_terminal() || !move.is_valid()) {
            break;
        }

        const auto legal_moves = position.generate_legal_moves();
        const auto move_it =
            std::find_if(legal_moves.begin(), legal_moves.end(), [&](const Move& candidate) {
                return same_move(candidate, move);
            });
        if (move_it == legal_moves.end()) {
            break;
        }

        if (!pv.empty()) {
            pv += ' ';
        }
        pv += position.move_to_usi(*move_it);

        position.do_move(*move_it);
        if (!seen_keys.insert(position.position_key()).second) {
            break;
        }

        TTEntry tt_entry;
        if (!shared_tt().probe(position.position_key(), tt_entry) ||
            tt_entry.generation != tt_generation_) {
            break;
        }
        move = tt_entry.best_move;
    }

    return pv;
}

void Search::store_tt(std::uint64_t key,
                      int depth,
                      int ply,
                      int score,
                      int alpha,
                      int beta,
                      const Move& best_move) {
    TTEntry entry;
    entry.key = key;
    entry.depth = depth;
    entry.score = score_to_tt(score, ply);
    entry.best_move = best_move;
    entry.generation = tt_generation_;
    if (score <= alpha) {
        entry.bound = BoundType::Upper;
    } else if (score >= beta) {
        entry.bound = BoundType::Lower;
    } else {
        entry.bound = BoundType::Exact;
    }
    shared_tt().store(entry);
}

}  // namespace shogi
