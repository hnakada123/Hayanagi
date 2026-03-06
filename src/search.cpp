#include "search.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace shogi {

namespace {

constexpr int kMateThreshold = kMateScore - 256;
constexpr int kTempoBonus = 12;
constexpr int kNullMoveBaseReduction = 2;
constexpr int kMateSearchMaxPly = 5;

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
    return move.drop ? kSquareCount : move.from;
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

SearchResult Search::find_best_move(const Position& root,
                                    const SearchOptions& options,
                                    std::atomic_bool& stop,
                                    const std::function<void(const SearchInfo&)>& on_info) {
    stop_ = &stop;
    options_ = options;
    start_time_ = std::chrono::steady_clock::now();
    nodes_ = 0;
    aborted_ = false;
    transposition_table_.clear();
    if (transposition_table_.bucket_count() < 262144) {
        transposition_table_.reserve(262144);
    }
    for (auto& ply_killers : killer_moves_) {
        ply_killers = {Move{}, Move{}};
    }
    for (auto& color_history : history_) {
        for (auto& from_history : color_history) {
            from_history.fill(0);
        }
    }

    SearchResult result;
    result.terminal = root.terminal_status();
    if (result.terminal.is_terminal()) {
        result.score_cp = terminal_score(result.terminal, 0);
        result.elapsed_ms = elapsed_ms();
        result.hashfull_permille = hashfull_permille();
        return result;
    }

    auto legal_moves = root.generate_legal_moves();
    if (legal_moves.empty()) {
        result.elapsed_ms = elapsed_ms();
        return result;
    }

    std::vector<Move> mating_line;
    if ((options.infinite || options.max_depth >= 3 || options.time_limit_ms >= 200) &&
        find_forced_mate(root, kMateSearchMaxPly, mating_line) && !mating_line.empty()) {
        result.best_move = mating_line.front();
        result.has_best_move = true;
        result.score_cp = kMateScore - 1;
        result.completed_depth = kMateSearchMaxPly;
        result.nodes = nodes_;
        result.elapsed_ms = elapsed_ms();
        result.hashfull_permille = hashfull_permille();
        on_info(SearchInfo{
            kMateSearchMaxPly,
            result.score_cp,
            result.nodes,
            result.elapsed_ms,
            format_pv(root, mating_line),
        });
        return result;
    }

    Move fallback = legal_moves.front();
    Move previous_best = fallback;

    for (int depth = 1; depth <= options.max_depth; ++depth) {
        if (should_stop()) {
            break;
        }

        Move tt_move;
        if (const auto it = transposition_table_.find(root.position_key());
            it != transposition_table_.end()) {
            tt_move = it->second.best_move;
        }

        std::vector<RootMove> root_moves;
        root_moves.reserve(legal_moves.size());
        for (const Move& move : legal_moves) {
            int score = move_order_score(root, move, 0, tt_move);
            if (same_move(move, previous_best)) {
                score += 2000000;
            }
            root_moves.push_back(RootMove{move, score});
        }
        std::sort(root_moves.begin(), root_moves.end(), [](const RootMove& lhs, const RootMove& rhs) {
            return lhs.order_score > rhs.order_score;
        });

        Move best_move = previous_best;
        bool found_move = false;
        int best_score = -kInfinity;
        int alpha = -kInfinity;
        const int beta = kInfinity;

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
            }
            alpha = std::max(alpha, best_score);
        }

        if (aborted_ || !found_move) {
            break;
        }

        previous_best = best_move;
        store_tt(root.position_key(), depth, 0, best_score, -kInfinity, kInfinity, best_move);

        const std::string pv = build_pv(root, best_move, depth);
        result.best_move = best_move;
        result.has_best_move = true;
        result.score_cp = best_score;
        result.completed_depth = depth;
        result.nodes = nodes_;
        result.elapsed_ms = elapsed_ms();
        result.hashfull_permille = hashfull_permille();

        on_info(SearchInfo{
            depth,
            best_score,
            nodes_,
            result.elapsed_ms,
            pv,
        });

        if (options.infinite) {
            continue;
        }
    }

    if (!result.has_best_move) {
        result.best_move = fallback;
        result.has_best_move = true;
        result.elapsed_ms = elapsed_ms();
        result.nodes = nodes_;
    }
    result.hashfull_permille = hashfull_permille();
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

    ++nodes_;
    const bool in_check = position.is_in_check(position.side_to_move());
    const int alpha_original = alpha;
    const std::uint64_t key = position.position_key();
    Move tt_move;

    if (const auto it = transposition_table_.find(key); it != transposition_table_.end()) {
        tt_move = it->second.best_move;
        if (it->second.depth >= depth) {
            const int tt_score = score_from_tt(it->second.score, ply);
            switch (it->second.bound) {
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

    auto legal_moves = position.generate_legal_moves();
    if (legal_moves.empty()) {
        if (in_check) {
            return -kMateScore + ply;
        }
        return 0;
    }

    std::sort(legal_moves.begin(), legal_moves.end(), [&](const Move& lhs, const Move& rhs) {
        return move_order_score(position, lhs, ply, tt_move) >
               move_order_score(position, rhs, ply, tt_move);
    });

    Move best_move;
    int best_score = -kInfinity;

    for (std::size_t move_index = 0; move_index < legal_moves.size(); ++move_index) {
        const Move& move = legal_moves[move_index];
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

    ++nodes_;
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

    auto legal_moves = position.generate_legal_moves();
    if (legal_moves.empty()) {
        if (in_check) {
            return -kMateScore + ply;
        }
        return alpha;
    }

    std::vector<Move> tactical_moves;
    tactical_moves.reserve(legal_moves.size());
    for (const Move& move : legal_moves) {
        const int captured = position.piece_at(move.to);
        if (in_check || captured != 0 || move.promote) {
            if (!in_check && captured != 0 && position.static_exchange_eval(move) < -120) {
                continue;
            }
            tactical_moves.push_back(move);
        }
    }
    if (!in_check && tactical_moves.empty()) {
        return alpha;
    }

    const Move no_tt_move;
    std::sort(tactical_moves.begin(), tactical_moves.end(), [&](const Move& lhs, const Move& rhs) {
        return move_order_score(position, lhs, ply, no_tt_move) >
               move_order_score(position, rhs, ply, no_tt_move);
    });

    for (const Move& move : tactical_moves) {
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
    if (options_.node_limit > 0 && nodes_ >= options_.node_limit) {
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

    if (captured == 0 && !move.promote && !move.drop) {
        score += history_score(position.side_to_move(), move);
    }

    return score;
}

bool Search::is_quiet(const Position& position, const Move& move) const {
    return !move.drop && !move.promote && position.piece_at(move.to) == 0;
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
    ++nodes_;

    if (remaining_ply <= 0) {
        return false;
    }

    auto legal_moves = position.generate_legal_moves();
    std::vector<Move> checking_moves;
    checking_moves.reserve(legal_moves.size());
    for (const Move& move : legal_moves) {
        Position child = position;
        child.do_move(move);
        if (child.is_in_check(child.side_to_move())) {
            checking_moves.push_back(move);
        }
    }
    if (checking_moves.empty()) {
        return false;
    }

    const Move no_tt_move;
    std::sort(checking_moves.begin(), checking_moves.end(), [&](const Move& lhs, const Move& rhs) {
        return move_order_score(position, lhs, 0, no_tt_move) >
               move_order_score(position, rhs, 0, no_tt_move);
    });

    for (const Move& move : checking_moves) {
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
    ++nodes_;

    auto legal_moves = position.generate_legal_moves();
    if (legal_moves.empty()) {
        return position.is_in_check(position.side_to_move());
    }
    if (remaining_ply <= 0) {
        return false;
    }

    const Move no_tt_move;
    std::sort(legal_moves.begin(), legal_moves.end(), [&](const Move& lhs, const Move& rhs) {
        return move_order_score(position, lhs, 0, no_tt_move) >
               move_order_score(position, rhs, 0, no_tt_move);
    });

    Move selected_move;
    std::vector<Move> selected_line;
    std::size_t selected_length = 0;
    bool has_selected = false;

    for (const Move& move : legal_moves) {
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
    const auto buckets = transposition_table_.bucket_count();
    if (buckets == 0) {
        return 0;
    }
    return static_cast<int>(std::min<std::uint64_t>(
        1000, transposition_table_.size() * 1000ULL / buckets));
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

        const auto tt_it = transposition_table_.find(position.position_key());
        if (tt_it == transposition_table_.end()) {
            break;
        }
        move = tt_it->second.best_move;
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
    if (score <= alpha) {
        entry.bound = BoundType::Upper;
    } else if (score >= beta) {
        entry.bound = BoundType::Lower;
    } else {
        entry.bound = BoundType::Exact;
    }

    const auto it = transposition_table_.find(key);
    if (it == transposition_table_.end() || depth >= it->second.depth) {
        transposition_table_[key] = entry;
    }
}

}  // namespace shogi
