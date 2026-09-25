#include "position.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <sstream>

namespace shogi {

namespace {

enum Direction : int {
    North = 0,
    South = 1,
    West = 2,
    East = 3,
    NorthWest = 4,
    NorthEast = 5,
    SouthWest = 6,
    SouthEast = 7,
};

constexpr std::array<int, 8> kDirRow = {-1, 1, 0, 0, -1, -1, 1, 1};
constexpr std::array<int, 8> kDirCol = {0, 0, -1, 1, -1, 1, -1, 1};
// 方向に進むと升番号が増えるか（飛び利きの最初の遮蔽駒を lsb/msb で求めるため）
constexpr std::array<bool, 8> kDirIncreasing = {false, true, false, true,
                                                false, false, true, true};
constexpr int kStepDirections[][2] = {
    {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
    {0, 1},   {1, -1}, {1, 0},  {1, 1},
};
constexpr int kBlackTrySquare = 4;
constexpr int kWhiteTrySquare = 8 * kBoardSize + 4;
constexpr std::array<int, kHandPieceKinds> kFullPieceCounts = {0, 9, 2, 2, 2, 2, 1, 1};

// 玉以外の駒種を生成順に並べたもの（この順序は公開 API の生成順として維持する）
constexpr std::array<PieceType, 13> kPieceOrder = {
    PieceType::Pawn,     PieceType::Lance,     PieceType::Knight,   PieceType::Silver,
    PieceType::Gold,     PieceType::Bishop,    PieceType::Rook,     PieceType::ProPawn,
    PieceType::ProLance, PieceType::ProKnight, PieceType::ProSilver, PieceType::Horse,
    PieceType::Dragon,
};

// 利きが近接 1 升で完結する駒種（step_to テーブルだけで攻撃判定できる）
constexpr std::array<PieceType, 11> kStepPieces = {
    PieceType::Pawn,      PieceType::Knight,    PieceType::Silver, PieceType::Gold,
    PieceType::King,      PieceType::ProPawn,   PieceType::ProLance,
    PieceType::ProKnight, PieceType::ProSilver, PieceType::Horse,  PieceType::Dragon,
};

int orientation(Color color) {
    return color == Color::Black ? 1 : -1;
}

int color_index(Color color) {
    return static_cast<int>(color);
}

int type_index(PieceType type) {
    return static_cast<int>(type);
}

std::vector<std::string> split_tokens(const std::string& text) {
    std::istringstream iss(text);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

bool same_move(const Move& lhs, const Move& rhs) {
    return lhs.from == rhs.from && lhs.to == rhs.to && lhs.piece == rhs.piece &&
           lhs.promote == rhs.promote && lhs.drop == rhs.drop;
}

struct Tables {
    Bitboard all_squares;
    std::array<Bitboard, kSquareCount> square_bb{};
    std::array<Bitboard, kBoardSize> file_bb{};
    std::array<std::array<std::array<Bitboard, kSquareCount>, 15>, 2> step_from{};
    std::array<std::array<std::array<Bitboard, kSquareCount>, 15>, 2> step_to{};
    std::array<std::array<Bitboard, 8>, kSquareCount> rays{};
    std::array<std::array<Bitboard, kSquareCount>, kSquareCount> between{};
    // 遮蔽を無視した飛び利きの線（ピン・開き王手の候補検出用）
    std::array<Bitboard, kSquareCount> rook_lines{};
    std::array<Bitboard, kSquareCount> bishop_lines{};
    std::array<std::array<Bitboard, kSquareCount>, 2> lance_lines{};
    // 行き所のない駒を除いた打てる升（色・駒種別）
    std::array<std::array<Bitboard, kHandPieceKinds>, 2> drop_masks{};
};

constexpr int kMaxHandCount = 18;

struct ZobristTables {
    std::array<std::array<std::array<std::uint64_t, kSquareCount>, 15>, 2> board{};
    std::array<std::array<std::array<std::uint64_t, kMaxHandCount>, kHandPieceKinds>, 2> hand{};
    std::uint64_t side_to_move = 0;
};

std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t value = (state += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

void add_step_attack(Tables& tables,
                     Color color,
                     PieceType type,
                     int square,
                     int row_delta,
                     int col_delta) {
    const int sign = orientation(color);
    const int row = square_row(square) + row_delta * sign;
    const int col = square_col(square) + col_delta * sign;
    if (!is_on_board(row, col)) {
        return;
    }
    const int to = make_square(row, col);
    tables.step_from[color_index(color)][type_index(type)][square].set(to);
    tables.step_to[color_index(color)][type_index(type)][to].set(square);
}

Tables build_tables() {
    Tables tables;

    for (int square = 0; square < kSquareCount; ++square) {
        tables.square_bb[square].set(square);
        tables.all_squares.set(square);
        tables.file_bb[square_col(square)].set(square);
    }

    for (int square = 0; square < kSquareCount; ++square) {
        const int row = square_row(square);
        const int col = square_col(square);
        for (int dir = 0; dir < 8; ++dir) {
            int next_row = row + kDirRow[dir];
            int next_col = col + kDirCol[dir];
            Bitboard between;
            while (is_on_board(next_row, next_col)) {
                const int to = make_square(next_row, next_col);
                tables.rays[square][dir].set(to);
                tables.between[square][to] = between;
                between.set(to);
                next_row += kDirRow[dir];
                next_col += kDirCol[dir];
            }
        }
        tables.rook_lines[square] = tables.rays[square][North] | tables.rays[square][South] |
                                    tables.rays[square][West] | tables.rays[square][East];
        tables.bishop_lines[square] =
            tables.rays[square][NorthWest] | tables.rays[square][NorthEast] |
            tables.rays[square][SouthWest] | tables.rays[square][SouthEast];
        tables.lance_lines[color_index(Color::Black)][square] = tables.rays[square][North];
        tables.lance_lines[color_index(Color::White)][square] = tables.rays[square][South];
    }

    static constexpr int kPawn[][2] = {{-1, 0}};
    static constexpr int kKnight[][2] = {{-2, -1}, {-2, 1}};
    static constexpr int kSilver[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {1, -1}, {1, 1}};
    static constexpr int kGold[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, 0}};
    static constexpr int kRookStep[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    static constexpr int kBishopStep[][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

    for (int color_id = 0; color_id < 2; ++color_id) {
        const Color color = static_cast<Color>(color_id);
        for (int square = 0; square < kSquareCount; ++square) {
            for (const auto& delta : kPawn) {
                add_step_attack(tables, color, PieceType::Pawn, square, delta[0], delta[1]);
            }
            for (const auto& delta : kKnight) {
                add_step_attack(tables, color, PieceType::Knight, square, delta[0], delta[1]);
            }
            for (const auto& delta : kSilver) {
                add_step_attack(tables, color, PieceType::Silver, square, delta[0], delta[1]);
            }
            for (const auto& delta : kGold) {
                add_step_attack(tables, color, PieceType::Gold, square, delta[0], delta[1]);
                add_step_attack(tables, color, PieceType::ProPawn, square, delta[0], delta[1]);
                add_step_attack(tables, color, PieceType::ProLance, square, delta[0], delta[1]);
                add_step_attack(tables, color, PieceType::ProKnight, square, delta[0], delta[1]);
                add_step_attack(tables, color, PieceType::ProSilver, square, delta[0], delta[1]);
            }
            for (const auto& delta : kStepDirections) {
                add_step_attack(tables, color, PieceType::King, square, delta[0], delta[1]);
            }
            for (const auto& delta : kRookStep) {
                add_step_attack(tables, color, PieceType::Horse, square, delta[0], delta[1]);
            }
            for (const auto& delta : kBishopStep) {
                add_step_attack(tables, color, PieceType::Dragon, square, delta[0], delta[1]);
            }

            const int row = square_row(square);
            for (int index = 1; index < kHandPieceKinds; ++index) {
                const PieceType type = static_cast<PieceType>(index);
                const bool forbidden =
                    ((type == PieceType::Pawn || type == PieceType::Lance) &&
                     is_last_rank(color, row)) ||
                    (type == PieceType::Knight && is_last_two_ranks(color, row));
                if (!forbidden) {
                    tables.drop_masks[color_id][index].set(square);
                }
            }
        }
    }

    return tables;
}

const Tables& tables() {
    static const Tables instance = build_tables();
    return instance;
}

ZobristTables build_zobrist() {
    ZobristTables zobrist;
    std::uint64_t seed = 0x7F4A7C159E3779B9ULL;

    for (int color = 0; color < 2; ++color) {
        for (int type = 0; type < 15; ++type) {
            for (int square = 0; square < kSquareCount; ++square) {
                zobrist.board[color][type][square] = splitmix64(seed);
            }
        }
        for (int index = 0; index < kHandPieceKinds; ++index) {
            for (int count = 0; count < kMaxHandCount; ++count) {
                zobrist.hand[color][index][count] = splitmix64(seed);
            }
        }
    }

    zobrist.side_to_move = splitmix64(seed);
    return zobrist;
}

const ZobristTables& zobrist() {
    static const ZobristTables instance = build_zobrist();
    return instance;
}

// from から dir 方向の飛び利き。最初の遮蔽駒の升を含む
#if defined(__GNUC__)
[[gnu::always_inline]]
#endif
inline Bitboard ray_attack(const Tables& t, int from, int dir, const Bitboard& occupied) {
    const Bitboard ray = t.rays[from][dir];
    const Bitboard blockers = ray & occupied;
    if (blockers.none()) {
        return ray;
    }
    const int first = kDirIncreasing[dir] ? blockers.lsb() : blockers.msb();
    return ray ^ t.rays[first][dir];
}

inline Bitboard rook_attacks_from(const Tables& t, int from, const Bitboard& occupied) {
    return ray_attack(t, from, North, occupied) | ray_attack(t, from, South, occupied) |
           ray_attack(t, from, West, occupied) | ray_attack(t, from, East, occupied);
}

inline Bitboard bishop_attacks_from(const Tables& t, int from, const Bitboard& occupied) {
    return ray_attack(t, from, NorthWest, occupied) | ray_attack(t, from, NorthEast, occupied) |
           ray_attack(t, from, SouthWest, occupied) | ray_attack(t, from, SouthEast, occupied);
}

inline Bitboard lance_attacks_from(const Tables& t, int from, Color color, const Bitboard& occupied) {
    return ray_attack(t, from, color == Color::Black ? North : South, occupied);
}

bool piece_attacks_square_on_board(const std::array<int, kSquareCount>& board,
                                   int from,
                                   int target,
                                   PieceType type,
                                   Color color) {
    const int sign = orientation(color);
    const int from_row = square_row(from);
    const int from_col = square_col(from);
    const int to_row = square_row(target);
    const int to_col = square_col(target);
    const int dr = to_row - from_row;
    const int dc = to_col - from_col;

    auto matches_step = [&](const int directions[][2], int count) {
        for (int i = 0; i < count; ++i) {
            if (dr == directions[i][0] * sign && dc == directions[i][1] * sign) {
                return true;
            }
        }
        return false;
    };

    auto matches_slider = [&](const std::initializer_list<int>& directions) {
        for (const int dir : directions) {
            int row = from_row + kDirRow[dir];
            int col = from_col + kDirCol[dir];
            while (is_on_board(row, col)) {
                const int square = make_square(row, col);
                if (square == target) {
                    return true;
                }
                if (!is_empty(board[square])) {
                    break;
                }
                row += kDirRow[dir];
                col += kDirCol[dir];
            }
        }
        return false;
    };

    static constexpr int kPawn[][2] = {{-1, 0}};
    static constexpr int kKnight[][2] = {{-2, -1}, {-2, 1}};
    static constexpr int kSilver[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {1, -1}, {1, 1}};
    static constexpr int kGold[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, 0}};
    static constexpr int kRookStep[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    static constexpr int kBishopStep[][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

    switch (type) {
        case PieceType::Pawn:
            return matches_step(kPawn, 1);
        case PieceType::Lance:
            return matches_slider({color == Color::Black ? North : South});
        case PieceType::Knight:
            return matches_step(kKnight, 2);
        case PieceType::Silver:
            return matches_step(kSilver, 5);
        case PieceType::Gold:
        case PieceType::ProPawn:
        case PieceType::ProLance:
        case PieceType::ProKnight:
        case PieceType::ProSilver:
            return matches_step(kGold, 6);
        case PieceType::Bishop:
            return matches_slider({NorthWest, NorthEast, SouthWest, SouthEast});
        case PieceType::Rook:
            return matches_slider({North, South, West, East});
        case PieceType::King:
            return std::max(std::abs(dr), std::abs(dc)) == 1;
        case PieceType::Horse:
            return matches_slider({NorthWest, NorthEast, SouthWest, SouthEast}) ||
                   matches_step(kRookStep, 4);
        case PieceType::Dragon:
            return matches_slider({North, South, West, East}) || matches_step(kBishopStep, 4);
        case PieceType::Empty:
        default:
            return false;
    }
}

struct AttackerCandidate {
    int from = -1;
    PieceType landing_type = PieceType::Empty;
    int value = 0;
};

std::optional<AttackerCandidate> least_valuable_attacker_on_board(
    const std::array<int, kSquareCount>& board, int target, Color color) {
    std::optional<AttackerCandidate> best;
    for (int from = 0; from < kSquareCount; ++from) {
        const int piece = board[from];
        if (is_empty(piece) || piece_color(piece) != color) {
            continue;
        }

        const PieceType type = piece_type(piece);
        if (type == PieceType::King) {
            continue;
        }
        if (!piece_attacks_square_on_board(board, from, target, type, color)) {
            continue;
        }

        PieceType landing_type = type;
        if (must_promote(type, color, square_row(target))) {
            landing_type = promote(type);
        }

        const int value = piece_value(type);
        if (!best.has_value() || value < best->value ||
            (value == best->value && piece_value(landing_type) < piece_value(best->landing_type))) {
            best = AttackerCandidate{from, landing_type, value};
        }
    }
    return best;
}

}  // namespace

Position::Position() {
    set_startpos();
}

void Position::clear() {
    board_.fill(0);
    for (auto& hand : hands_) {
        hand.fill(0);
    }
    for (auto& color_bits : piece_bb_) {
        for (auto& piece_bits : color_bits) {
            piece_bits = Bitboard{};
        }
    }
    color_bb_[0] = Bitboard{};
    color_bb_[1] = Bitboard{};
    occupied_ = Bitboard{};
    king_square_ = {-1, -1};
    side_to_move_ = Color::Black;
    ply_count_ = 0;
    position_key_ = 0;
    history_.reset();
}

void Position::add_piece(int square, Color color, PieceType type) {
    position_key_ ^= zobrist().board[static_cast<int>(color)][static_cast<int>(type)][square];
    board_[square] = encode_piece(color, type);
    occupied_.set(square);
    color_bb_[static_cast<int>(color)].set(square);
    piece_bb_[static_cast<int>(color)][static_cast<int>(type)].set(square);
    if (type == PieceType::King) {
        king_square_[static_cast<int>(color)] = square;
    }
}

void Position::remove_piece(int square) {
    const int piece = board_[square];
    if (is_empty(piece)) {
        return;
    }
    const Color color = piece_color(piece);
    const PieceType type = piece_type(piece);
    position_key_ ^= zobrist().board[static_cast<int>(color)][static_cast<int>(type)][square];
    occupied_.reset(square);
    color_bb_[static_cast<int>(color)].reset(square);
    piece_bb_[static_cast<int>(color)][static_cast<int>(type)].reset(square);
    board_[square] = 0;
    if (type == PieceType::King) {
        king_square_[static_cast<int>(color)] = -1;
    }
}

void Position::add_hand_piece(Color color, PieceType type) {
    const int index = hand_index(type);
    int& count = hands_[static_cast<int>(color)][index];
    position_key_ ^= zobrist().hand[static_cast<int>(color)][index][count];
    ++count;
}

void Position::remove_hand_piece(Color color, PieceType type) {
    const int index = hand_index(type);
    int& count = hands_[static_cast<int>(color)][index];
    --count;
    position_key_ ^= zobrist().hand[static_cast<int>(color)][index][count];
}

void Position::set_startpos() {
    set_sfen(kStartposSfen);
}

bool Position::set_sfen(const std::string& sfen, bool tsume) {
    clear();
    const auto tokens = split_tokens(sfen);
    if (tokens.size() != 4) {
        return false;
    }

    int row = 0;
    int col = 0;
    bool promoted = false;
    std::array<int, 9> totals{};
    constexpr std::array<int, 9> limits{0, 18, 4, 4, 4, 4, 2, 2, 2};
    for (char ch : tokens[0]) {
        if (ch == '/') {
            if (col != kBoardSize || promoted || row >= 8) {
                return false;
            }
            ++row;
            col = 0;
            continue;
        }
        if (ch == '+') {
            if (promoted) {
                return false;
            }
            promoted = true;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            if (promoted || ch == '0' || col + ch - '0' > 9) return false;
            col += ch - '0';
            continue;
        }
        if (!is_on_board(row, col)) {
            return false;
        }
        const auto base = piece_from_letter(ch);
        if (!base.has_value()) {
            return false;
        }
        PieceType type = base.value();
        if (++totals[static_cast<int>(type)] > limits[static_cast<int>(type)]) return false;
        if (promoted) {
            if (!can_promote(type)) return false;
            type = promote(type);
        }
        const Color color =
            std::isupper(static_cast<unsigned char>(ch)) ? Color::Black : Color::White;
        if (type == PieceType::King && find_king(color) != -1) return false;
        add_piece(make_square(row, col), color, type);
        ++col;
        promoted = false;
    }
    if (row != 8 || col != kBoardSize || promoted) {
        return false;
    }

    if (tokens[1] == "b") {
        side_to_move_ = Color::Black;
    } else if (tokens[1] == "w") {
        side_to_move_ = Color::White;
        position_key_ ^= zobrist().side_to_move;
    } else {
        return false;
    }

    if (tokens[2] != "-") {
        int count = 0;
        for (char ch : tokens[2]) {
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                if (count > 18 || (count == 0 && ch == '0')) return false;
                count = count * 10 + (ch - '0');
                continue;
            }
            const auto piece = piece_from_letter(ch);
            if (!piece.has_value() || piece.value() == PieceType::King) {
                return false;
            }
            const int type_index = static_cast<int>(piece.value());
            totals[type_index] += std::max(count, 1);
            if (totals[type_index] > limits[type_index]) return false;
            const Color color =
                std::isupper(static_cast<unsigned char>(ch)) ? Color::Black : Color::White;
            for (int remaining = std::max(count, 1); remaining > 0; --remaining) {
                add_hand_piece(color, piece.value());
            }
            count = 0;
        }
        if (count != 0) {
            return false;
        }
    }

    const bool black_missing = find_king(Color::Black) == -1;
    const bool white_missing = find_king(Color::White) == -1;
    if (tsume ? (black_missing && white_missing) : (black_missing || white_missing)) {
        return false;
    }

    try {
        std::size_t used = 0;
        const int number = std::stoi(tokens[3], &used);
        if (used != tokens[3].size() || number < 1) return false;
        ply_count_ = number - 1;
    } catch (...) {
        return false;
    }

    rebuild_history();
    return true;
}

int Position::hand_count(Color color, PieceType type) const {
    return hands_[static_cast<int>(color)][hand_index(type)];
}

bool Position::do_move(const Move& move) {
    if (!move.is_valid()) {
        return false;
    }
    do_move_unchecked(move);
    return true;
}

void Position::make_move(const Move& move, MoveUndo& undo) {
    undo.captured = 0;
    const Color mover = side_to_move_;
    if (move.drop) {
        add_piece(move.to, mover, move.piece);
        remove_hand_piece(mover, move.piece);
    } else {
        const PieceType moving_type = piece_type(board_[move.from]);
        const int captured_piece = board_[move.to];
        if (!is_empty(captured_piece)) {
            undo.captured = captured_piece;
            add_hand_piece(mover, unpromote(piece_type(captured_piece)));
            remove_piece(move.to);
        }
        remove_piece(move.from);
        add_piece(move.to, mover, move.promote ? promote(moving_type) : moving_type);
    }
    ++ply_count_;
    side_to_move_ = opposite(side_to_move_);
    position_key_ ^= zobrist().side_to_move;
}

void Position::unmake_move(const Move& move, const MoveUndo& undo) {
    side_to_move_ = opposite(side_to_move_);
    position_key_ ^= zobrist().side_to_move;
    --ply_count_;
    const Color mover = side_to_move_;
    if (move.drop) {
        remove_piece(move.to);
        add_hand_piece(mover, move.piece);
        return;
    }
    PieceType moving_type = piece_type(board_[move.to]);
    if (move.promote) {
        moving_type = unpromote(moving_type);
    }
    remove_piece(move.to);
    add_piece(move.from, mover, moving_type);
    if (!is_empty(undo.captured)) {
        add_piece(move.to, piece_color(undo.captured), piece_type(undo.captured));
        remove_hand_piece(mover, unpromote(piece_type(undo.captured)));
    }
}

void Position::do_move_unchecked(const Move& move) {
    MoveUndo undo;
    make_move(move, undo);
    append_history();
}

TerminalStatus Position::terminal_status() const {
    if (is_perpetual_check_loss_for_opponent()) {
        return TerminalStatus{TerminalOutcome::Win, TerminalReason::PerpetualCheck};
    }
    if (rules_.entering_king_rule == EnteringKingRule::TryRule) {
        if (is_try_rule_win(Color::Black)) {
            return TerminalStatus{
                side_to_move_ == Color::Black ? TerminalOutcome::Win : TerminalOutcome::Loss,
                TerminalReason::TryRule,
            };
        }
        if (is_try_rule_win(Color::White)) {
            return TerminalStatus{
                side_to_move_ == Color::White ? TerminalOutcome::Win : TerminalOutcome::Loss,
                TerminalReason::TryRule,
            };
        }
    }
    if (is_repetition_draw()) {
        return TerminalStatus{TerminalOutcome::Draw, TerminalReason::Repetition};
    }
    if (rules_.max_moves_to_draw > 0 && ply_count_ >= rules_.max_moves_to_draw &&
        !is_in_check(side_to_move_)) {
        return TerminalStatus{TerminalOutcome::Draw, TerminalReason::MoveLimit};
    }
    if (rules_.entering_king_rule != EnteringKingRule::NoEnteringKing &&
        rules_.entering_king_rule != EnteringKingRule::TryRule) {
        if (can_declare_win()) {
            return TerminalStatus{TerminalOutcome::Win, TerminalReason::DeclarationWin};
        }
        if (is_impasse_position()) {
            const int own_points =
                impasse_points(side_to_move_) + handicap_entering_king_bonus(side_to_move_);
            const int opponent_points = impasse_points(opposite(side_to_move_)) +
                                        handicap_entering_king_bonus(opposite(side_to_move_));
            const int own_threshold = impasse_threshold(side_to_move_);
            const int opponent_threshold = impasse_threshold(opposite(side_to_move_));
            if (own_points >= own_threshold && opponent_points >= opponent_threshold) {
                return TerminalStatus{TerminalOutcome::Draw, TerminalReason::Impasse};
            }
            if (own_points < own_threshold && opponent_points >= opponent_threshold) {
                return TerminalStatus{TerminalOutcome::Loss, TerminalReason::Impasse};
            }
            if (own_points >= own_threshold && opponent_points < opponent_threshold) {
                return TerminalStatus{TerminalOutcome::Win, TerminalReason::Impasse};
            }
        }
    }
    return TerminalStatus{};
}

bool Position::can_declare_win() const {
    if (rules_.entering_king_rule == EnteringKingRule::NoEnteringKing ||
        rules_.entering_king_rule == EnteringKingRule::TryRule) {
        return false;
    }
    const Color color = side_to_move_;
    const int king_square = find_king(color);
    if (king_square == -1 || !is_in_promotion_zone(color, square_row(king_square))) {
        return false;
    }
    if (is_in_check(color)) {
        return false;
    }
    if (pieces_in_opponent_camp(color) < 10) {
        return false;
    }
    if (declaration_points(color) + handicap_entering_king_bonus(color) < declaration_threshold(color)) {
        return false;
    }
    return !opponent_has_mate_in_one(color);
}

bool Position::gives_check(const Move& move) const {
    if (!move.is_valid()) {
        return false;
    }

    const Color attacker = side_to_move_;
    const int king_square = find_king(opposite(attacker));
    if (king_square == -1) {
        return false;
    }

    PieceType moved_type = move.piece;
    if (!move.drop) {
        const int moving_piece = board_[move.from];
        if (is_empty(moving_piece) || piece_color(moving_piece) != attacker) {
            return false;
        }
        moved_type = move.promote ? promote(piece_type(moving_piece)) : piece_type(moving_piece);
    }

    return is_square_attacked_after_move(king_square, attacker, move, moved_type);
}

int Position::static_exchange_eval(const Move& move) const {
    if (move.drop || move.from < 0 || move.from >= kSquareCount) {
        return 0;
    }

    const int moving_piece = board_[move.from];
    if (is_empty(moving_piece)) {
        return 0;
    }

    const int captured_piece = board_[move.to];
    const PieceType moving_type = piece_type(moving_piece);
    const PieceType landing_type = move.promote ? promote(moving_type) : moving_type;

    if (is_empty(captured_piece)) {
        return move.promote ? piece_value(landing_type) - piece_value(moving_type) : 0;
    }

    std::array<int, kSquareCount> board = board_;
    std::array<int, 64> gains{};

    gains[0] = piece_value(piece_type(captured_piece));
    board[move.from] = 0;
    board[move.to] = encode_piece(side_to_move_, landing_type);

    Color side = opposite(side_to_move_);
    PieceType occupant_type = landing_type;
    int depth = 0;

    while (depth + 1 < static_cast<int>(gains.size())) {
        const auto attacker = least_valuable_attacker_on_board(board, move.to, side);
        if (!attacker.has_value()) {
            break;
        }

        ++depth;
        gains[depth] = piece_value(occupant_type) - gains[depth - 1];
        if (std::max(-gains[depth - 1], gains[depth]) < 0) {
            break;
        }

        board[attacker->from] = 0;
        board[move.to] = encode_piece(side, attacker->landing_type);
        occupant_type = attacker->landing_type;
        side = opposite(side);
    }

    while (depth > 0) {
        gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
        --depth;
    }

    return gains[0];
}

Position Position::make_null_move() const {
    Position next = *this;
    next.side_to_move_ = opposite(next.side_to_move_);
    next.position_key_ ^= zobrist().side_to_move;
    return next;
}

std::string Position::move_to_usi(const Move& move) const {
    if (!move.is_valid()) {
        return "resign";
    }
    if (move.drop) {
        return std::string{piece_letter(move.piece)} + "*" + square_to_usi(move.to);
    }
    std::string text = square_to_usi(move.from) + square_to_usi(move.to);
    if (move.promote) {
        text += '+';
    }
    return text;
}

std::string Position::to_sfen() const {
    std::string sfen;
    for (int row = 0; row < kBoardSize; ++row) {
        if (row > 0) {
            sfen += '/';
        }
        int empty_count = 0;
        for (int col = 0; col < kBoardSize; ++col) {
            const int piece = board_[make_square(row, col)];
            if (is_empty(piece)) {
                ++empty_count;
                continue;
            }
            if (empty_count > 0) {
                sfen += static_cast<char>('0' + empty_count);
                empty_count = 0;
            }
            const PieceType type = piece_type(piece);
            if (is_promoted(type)) {
                sfen += '+';
            }
            char letter = piece_letter(type);
            if (piece_color(piece) == Color::White) {
                letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
            }
            sfen += letter;
        }
        if (empty_count > 0) {
            sfen += static_cast<char>('0' + empty_count);
        }
    }

    sfen += ' ';
    sfen += (side_to_move_ == Color::Black) ? 'b' : 'w';
    sfen += ' ';

    std::string hand_str;
    static constexpr PieceType kHandOrder[] = {
        PieceType::Rook, PieceType::Bishop, PieceType::Gold, PieceType::Silver,
        PieceType::Knight, PieceType::Lance, PieceType::Pawn,
    };
    for (Color color : {Color::Black, Color::White}) {
        for (PieceType type : kHandOrder) {
            const int count = hand_count(color, type);
            if (count <= 0) {
                continue;
            }
            if (count > 1) {
                hand_str += std::to_string(count);
            }
            char letter = piece_letter(type);
            if (color == Color::White) {
                letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
            }
            hand_str += letter;
        }
    }
    sfen += hand_str.empty() ? "-" : hand_str;

    sfen += ' ';
    sfen += std::to_string(ply_count_ + 1);

    return sfen;
}

bool Position::apply_usi_move(const std::string& move_text) {
    const auto legal_moves = generate_legal_moves();
    const auto it = std::find_if(legal_moves.begin(), legal_moves.end(), [&](const Move& move) {
        return move_to_usi(move) == move_text;
    });
    if (it == legal_moves.end()) {
        return false;
    }
    do_move_unchecked(*it);
    return true;
}

Bitboard Position::rook_attacks(int from, const Bitboard& occupied) const {
    return rook_attacks_from(tables(), from, occupied);
}

Bitboard Position::bishop_attacks(int from, const Bitboard& occupied) const {
    return bishop_attacks_from(tables(), from, occupied);
}

Bitboard Position::lance_attacks(int from, Color color, const Bitboard& occupied) const {
    return lance_attacks_from(tables(), from, color, occupied);
}

Bitboard Position::attacks_from(int from, PieceType type, Color color, const Bitboard& occupied) const {
    const auto& t = tables();
    Bitboard attacks = t.step_from[color_index(color)][type_index(type)][from];
    switch (type) {
        case PieceType::Lance:
            return attacks | lance_attacks(from, color, occupied);
        case PieceType::Bishop:
        case PieceType::Horse:
            return attacks | bishop_attacks(from, occupied);
        case PieceType::Rook:
        case PieceType::Dragon:
            return attacks | rook_attacks(from, occupied);
        default:
            return attacks;
    }
}

// by の飛び駒（飛・龍・角・馬・香）のうち、occupied を遮蔽として square に利いているもの。
// 同一線上の飛び駒は少ないので、駒ごとに間の升が空いているかを調べる
Bitboard Position::slider_attackers_to(int square, Color by, const Bitboard& occupied) const {
    const auto& t = tables();
    Bitboard candidates = aligned_sliders(square, by);
    Bitboard attackers;
    while (candidates.any()) {
        const int from = candidates.pop_lsb();
        if ((t.between[square][from] & occupied).none()) {
            attackers.set(from);
        }
    }
    return attackers;
}

Bitboard Position::attackers_to(int square, Color by, const Bitboard& occupied) const {
    const auto& t = tables();
    const int c = color_index(by);
    const auto& bb = piece_bb_[color_index(by)];
    const auto& step_to = t.step_to[c];
    // 金と成駒は利きが同じなのでまとめ、玉の利きは馬・龍の近接利きの和として扱う
    const Bitboard golds = bb[type_index(PieceType::Gold)] | bb[type_index(PieceType::ProPawn)] |
                           bb[type_index(PieceType::ProLance)] |
                           bb[type_index(PieceType::ProKnight)] |
                           bb[type_index(PieceType::ProSilver)];
    const Bitboard king = bb[type_index(PieceType::King)];
    Bitboard attackers =
        (bb[type_index(PieceType::Pawn)] & step_to[type_index(PieceType::Pawn)][square]) |
        (bb[type_index(PieceType::Knight)] & step_to[type_index(PieceType::Knight)][square]) |
        (bb[type_index(PieceType::Silver)] & step_to[type_index(PieceType::Silver)][square]) |
        (golds & step_to[type_index(PieceType::Gold)][square]) |
        ((bb[type_index(PieceType::Horse)] | king) &
         step_to[type_index(PieceType::Horse)][square]) |
        ((bb[type_index(PieceType::Dragon)] | king) &
         step_to[type_index(PieceType::Dragon)][square]);
    return attackers | slider_attackers_to(square, by, occupied);
}

Bitboard Position::attackers_to(int square, Color by) const {
    return attackers_to(square, by, occupied_);
}

// 遮蔽を無視して square と同じ線上にある by の飛び駒（ピン・開き王手の候補）
Bitboard Position::aligned_sliders(int square, Color by) const {
    const auto& t = tables();
    const auto& bb = piece_bb_[color_index(by)];
    return ((bb[type_index(PieceType::Rook)] | bb[type_index(PieceType::Dragon)]) &
            t.rook_lines[square]) |
           ((bb[type_index(PieceType::Bishop)] | bb[type_index(PieceType::Horse)]) &
            t.bishop_lines[square]) |
           (bb[type_index(PieceType::Lance)] & t.lance_lines[color_index(opposite(by))][square]);
}

bool Position::is_square_attacked_after_move(int square,
                                             Color by,
                                             const Move& move,
                                             PieceType moved_type) const {
    const auto& t = tables();
    const int c = color_index(by);
    const Bitboard to_bb = t.square_bb[move.to];
    Bitboard occupied = occupied_ | to_bb;
    Bitboard removed;
    if (!move.drop) {
        removed = t.square_bb[move.from];
        occupied &= ~removed;
    }
    const auto pieces_after = [&](PieceType type) {
        Bitboard bb = piece_bb_[c][type_index(type)] & ~removed;
        if (type == moved_type) {
            bb |= to_bb;
        }
        return bb;
    };

    for (const PieceType type : kStepPieces) {
        if ((pieces_after(type) & t.step_to[c][type_index(type)][square]).any()) {
            return true;
        }
    }
    const Bitboard rooks = pieces_after(PieceType::Rook) | pieces_after(PieceType::Dragon);
    if (rooks.any() && (rooks & rook_attacks(square, occupied)).any()) {
        return true;
    }
    const Bitboard bishops = pieces_after(PieceType::Bishop) | pieces_after(PieceType::Horse);
    if (bishops.any() && (bishops & bishop_attacks(square, occupied)).any()) {
        return true;
    }
    const Bitboard lances = pieces_after(PieceType::Lance);
    return lances.any() && (lances & lance_attacks(square, opposite(by), occupied)).any();
}

bool Position::is_square_attacked_after_king_move(int square, Color by, int king_from) const {
    return attackers_to(square, by, occupied_ ^ tables().square_bb[king_from]).any();
}

void Position::prepare_gen_state(Color color, GenState& state) const {
    const auto& t = tables();
    const int c = color_index(color);
    state.color = color;
    state.own_occ = color_bb_[c];
    state.checkers = Bitboard{};
    state.pinned = Bitboard{};
    state.pin_count = 0;
    state.move_mask = t.all_squares;
    state.in_check = false;

    const int king_square = find_king(color);
    if (king_square < 0) {
        return;
    }
    const Color enemy = opposite(color);
    state.checkers = attackers_to(king_square, enemy);
    state.in_check = state.checkers.any();
    if (state.in_check && !state.checkers.more_than_one()) {
        // 単王手: 玉以外の駒は王手駒を取るか、飛び利きを遮る升にしか動けない
        const int checker = state.checkers.lsb();
        state.move_mask = t.square_bb[checker] | t.between[king_square][checker];
    }

    Bitboard snipers = aligned_sliders(king_square, enemy);
    while (snipers.any()) {
        const int sniper = snipers.pop_lsb();
        const Bitboard blockers = t.between[king_square][sniper] & occupied_;
        if (blockers.none() || blockers.more_than_one()) {
            continue;
        }
        const int blocker = blockers.lsb();
        if (piece_color(board_[blocker]) != color) {
            continue;
        }
        state.pinned.set(blocker);
        state.pin_squares[state.pin_count] = blocker;
        state.pin_lines[state.pin_count] = t.between[king_square][sniper] | t.square_bb[sniper];
        ++state.pin_count;
    }
}

Bitboard Position::GenState::pin_line(int square) const {
    for (int i = 0; i < pin_count; ++i) {
        if (pin_squares[i] == square) {
            return pin_lines[i];
        }
    }
    return Bitboard{};
}

Bitboard Position::CheckInfo::discovered_line(int square) const {
    for (int i = 0; i < discovered_count; ++i) {
        if (discovered_squares[i] == square) {
            return discovered_lines[i];
        }
    }
    return Bitboard{};
}

// landing_type の駒が color 側の駒として置かれたとき、enemy_king に王手となる升
Bitboard Position::check_targets(PieceType landing_type,
                                 Color color,
                                 int enemy_king,
                                 const Bitboard& occupied) const {
    const auto& t = tables();
    const Bitboard steps = t.step_to[color_index(color)][type_index(landing_type)][enemy_king];
    switch (landing_type) {
        case PieceType::Lance:
            return lance_attacks(enemy_king, opposite(color), occupied);
        case PieceType::Bishop:
            return bishop_attacks(enemy_king, occupied);
        case PieceType::Rook:
            return rook_attacks(enemy_king, occupied);
        case PieceType::Horse:
            return steps | bishop_attacks(enemy_king, occupied);
        case PieceType::Dragon:
            return steps | rook_attacks(enemy_king, occupied);
        case PieceType::King:
            return Bitboard{};
        default:
            return steps;
    }
}

void Position::prepare_check_info(Color color, CheckInfo& info) const {
    const auto& t = tables();
    info.king = find_king(opposite(color));
    info.filter = false;
    info.discovered = Bitboard{};
    info.discovered_count = 0;
    if (info.king < 0) {
        return;
    }
    // 相手玉に既に王手がかかっている（本来は不正な）局面では従来どおり後段判定にする
    if (attackers_to(info.king, color).any()) {
        info.filter = true;
        return;
    }

    Bitboard snipers = aligned_sliders(info.king, color);
    while (snipers.any()) {
        const int sniper = snipers.pop_lsb();
        const Bitboard blockers = t.between[info.king][sniper] & occupied_;
        if (blockers.none() || blockers.more_than_one()) {
            continue;
        }
        const int blocker = blockers.lsb();
        if (piece_color(board_[blocker]) != color) {
            continue;
        }
        info.discovered.set(blocker);
        info.discovered_squares[info.discovered_count] = blocker;
        info.discovered_lines[info.discovered_count] = t.between[info.king][sniper];
        ++info.discovered_count;
    }

    for (int index = 1; index < kHandPieceKinds; ++index) {
        info.drop_targets[index] =
            check_targets(static_cast<PieceType>(index), color, info.king, occupied_);
    }
}

void Position::generate_king_moves(const GenState& state,
                                   MoveSelection selection,
                                   const CheckInfo* check_info,
                                   std::vector<Move>& moves) const {
    const auto& t = tables();
    const Color color = state.color;
    const int from = find_king(color);
    if (from < 0) {
        return;
    }
    const Color enemy = opposite(color);
    Bitboard targets = t.step_from[color_index(color)][type_index(PieceType::King)][from];
    targets &= ~state.own_occ;
    // 玉同士が隣接する不正局面でも相手玉を取る手は生成しない
    targets &= ~piece_bb_[color_index(enemy)][type_index(PieceType::King)];
    const Bitboard occupied_without_king = occupied_ ^ t.square_bb[from];
    const bool direct_checks = check_info != nullptr && !check_info->filter;
    Bitboard open_line;
    if (direct_checks) {
        if (!check_info->discovered.test(from)) {
            return;  // 玉自身は直接王手できないので、開き王手にならなければ生成しない
        }
        open_line = check_info->discovered_line(from);
    }

    while (targets.any()) {
        const int to = targets.pop_lsb();
        if (attackers_to(to, enemy, occupied_without_king).any()) {
            continue;
        }
        const Move move{from, to, PieceType::King, false, false};
        if (direct_checks) {
            if (!open_line.test(to)) {
                moves.push_back(move);
            }
        } else if (should_keep_generated_move(move, selection, state.in_check)) {
            moves.push_back(move);
        }
    }
}

void Position::generate_piece_moves(const GenState& state,
                                    PieceType type,
                                    MoveSelection selection,
                                    const CheckInfo* check_info,
                                    std::vector<Move>& moves) const {
    const auto& t = tables();
    const Color color = state.color;
    const bool direct_checks = check_info != nullptr && !check_info->filter;
    Bitboard pieces = piece_bb_[color_index(color)][type_index(type)];
    while (pieces.any()) {
        const int from = pieces.pop_lsb();
        Bitboard targets = attacks_from(from, type, color, occupied_);
        targets &= ~state.own_occ;
        targets &= state.move_mask;
        if (state.pinned.test(from)) {
            targets &= state.pin_line(from);
        }
        if (targets.none()) {
            continue;
        }

        if (!direct_checks) {
            while (targets.any()) {
                const int to = targets.pop_lsb();
                add_move_variants(from, to, type, selection, state.in_check, moves);
            }
            continue;
        }

        // 移動元が空く前提で、移動先に置いた駒が相手玉に利く升と、開き王手を集める
        const Bitboard occupied_after = occupied_ ^ t.square_bb[from];
        const Bitboard plain = check_targets(type, color, check_info->king, occupied_after);
        const Bitboard promoted =
            can_promote(type)
                ? check_targets(promote(type), color, check_info->king, occupied_after)
                : Bitboard{};
        const bool discovered = check_info->discovered.test(from);
        const Bitboard open_line = discovered ? check_info->discovered_line(from) : Bitboard{};
        Bitboard candidates = targets & (plain | promoted);
        if (discovered) {
            candidates |= targets & ~open_line;
        }
        while (candidates.any()) {
            const int to = candidates.pop_lsb();
            const bool opens = discovered && !open_line.test(to);
            add_checking_variants(from, to, type, plain.test(to) || opens,
                                  promoted.test(to) || opens, moves);
        }
    }
}

void Position::add_move_variants(int from,
                                 int to,
                                 PieceType piece,
                                 MoveSelection selection,
                                 bool in_check,
                                 std::vector<Move>& moves) const {
    const int target = board_[to];
    if (!is_empty(target) && piece_type(target) == PieceType::King) {
        return;
    }

    const Color color = side_to_move_;
    const int from_row = square_row(from);
    const int to_row = square_row(to);
    const bool promotion_available =
        can_promote(piece) &&
        (is_in_promotion_zone(color, from_row) || is_in_promotion_zone(color, to_row));
    const bool promotion_required = must_promote(piece, color, to_row);

    if (!promotion_required) {
        const Move move{from, to, piece, false, false};
        if (should_keep_generated_move(move, selection, in_check)) {
            moves.push_back(move);
        }
    }
    if (promotion_available) {
        const Move move{from, to, piece, true, false};
        if (should_keep_generated_move(move, selection, in_check)) {
            moves.push_back(move);
        }
    }
}

// 王手になることが分かっている変化（不成・成）だけを add_move_variants と同じ順で追加する
void Position::add_checking_variants(int from,
                                     int to,
                                     PieceType piece,
                                     bool plain_checks,
                                     bool promoted_checks,
                                     std::vector<Move>& moves) const {
    const int target = board_[to];
    if (!is_empty(target) && piece_type(target) == PieceType::King) {
        return;
    }

    const Color color = side_to_move_;
    const int to_row = square_row(to);
    const bool promotion_available =
        can_promote(piece) &&
        (is_in_promotion_zone(color, square_row(from)) || is_in_promotion_zone(color, to_row));
    if (plain_checks && !must_promote(piece, color, to_row)) {
        moves.push_back(Move{from, to, piece, false, false});
    }
    if (promoted_checks && promotion_available) {
        moves.push_back(Move{from, to, piece, true, false});
    }
}

void Position::add_drop_moves(const GenState& state,
                              bool enforce_pawn_drop_mate,
                              MoveSelection selection,
                              const CheckInfo* check_info,
                              std::vector<Move>& moves) const {
    if (selection == MoveSelection::Tactical && !state.in_check) {
        return;
    }

    const auto& t = tables();
    const Color color = state.color;
    const int c = color_index(color);
    const bool direct_checks = check_info != nullptr && !check_info->filter;
    const Bitboard empty = t.all_squares & ~occupied_ & state.move_mask;

    Bitboard pawn_files;
    Bitboard pawns = piece_bb_[c][type_index(PieceType::Pawn)];
    while (pawns.any()) {
        pawn_files |= t.file_bb[square_col(pawns.pop_lsb())];
    }

    for (int index = 1; index < kHandPieceKinds; ++index) {
        const PieceType piece = static_cast<PieceType>(index);
        if (hands_[c][index] == 0) {
            continue;
        }
        Bitboard targets = empty & t.drop_masks[c][index];
        if (piece == PieceType::Pawn) {
            targets &= ~pawn_files;  // 二歩
        }
        if (direct_checks) {
            targets &= check_info->drop_targets[index];
        }

        while (targets.any()) {
            const int to = targets.pop_lsb();
            const Move move{-1, to, piece, false, true};
            if (piece == PieceType::Pawn && enforce_pawn_drop_mate && is_pawn_drop_mate(move)) {
                continue;
            }
            if (direct_checks || should_keep_generated_move(move, selection, state.in_check)) {
                moves.push_back(move);
            }
        }
    }
}

std::vector<Move> Position::generate_legal_moves() const {
    return generate_legal_moves(true, true, MoveSelection::All);
}

std::vector<Move> Position::generate_legal_moves(bool include_drops,
                                                 bool enforce_pawn_drop_mate) const {
    return generate_legal_moves(include_drops, enforce_pawn_drop_mate, MoveSelection::All);
}

std::vector<Move> Position::generate_search_legal_moves() const {
    return generate_legal_moves(true, true, MoveSelection::All);
}

std::vector<Move> Position::generate_quiescence_moves() const {
    return generate_legal_moves(true, true, MoveSelection::Tactical);
}

std::vector<Move> Position::generate_checking_moves() const {
    return generate_legal_moves(true, true, MoveSelection::Checking);
}

void Position::generate_legal_moves(std::vector<Move>& moves) const {
    generate_moves(moves, true, true, MoveSelection::All);
}

void Position::generate_checking_moves(std::vector<Move>& moves) const {
    generate_moves(moves, true, true, MoveSelection::Checking);
}

std::vector<Move> Position::generate_legal_moves(bool include_drops,
                                                 bool enforce_pawn_drop_mate,
                                                 MoveSelection selection) const {
    std::vector<Move> moves;
    moves.reserve(128);
    generate_moves(moves, include_drops, enforce_pawn_drop_mate, selection);
    return moves;
}

void Position::generate_moves(std::vector<Move>& moves,
                              bool include_drops,
                              bool enforce_pawn_drop_mate,
                              MoveSelection selection) const {
    moves.clear();
    const Color color = side_to_move_;
    GenState state;
    prepare_gen_state(color, state);

    CheckInfo info;
    const CheckInfo* check_info = nullptr;
    if (selection == MoveSelection::Checking) {
        prepare_check_info(color, info);
        if (info.king < 0) {
            return;  // 王手をかける相手玉がない
        }
        check_info = &info;
    }

    generate_king_moves(state, selection, check_info, moves);
    if (state.checkers.more_than_one()) {
        return;  // 両王手は玉を動かすしかない
    }
    for (const PieceType type : kPieceOrder) {
        generate_piece_moves(state, type, selection, check_info, moves);
    }
    if (include_drops) {
        add_drop_moves(state, enforce_pawn_drop_mate, selection, check_info, moves);
    }
}

std::vector<Move> Position::generate_pseudo_legal_moves(Color color, bool include_drops) const {
    Position copy = *this;
    if (copy.side_to_move_ != color) {
        copy.position_key_ ^= zobrist().side_to_move;
    }
    copy.side_to_move_ = color;
    return copy.generate_legal_moves(include_drops, false, MoveSelection::All);
}

bool Position::should_keep_generated_move(const Move& move,
                                          MoveSelection selection,
                                          bool in_check) const {
    switch (selection) {
        case MoveSelection::All:
            return true;
        case MoveSelection::Tactical:
            return in_check || board_[move.to] != 0 || move.promote;
        case MoveSelection::Checking:
            return gives_check(move);
        default:
            return true;
    }
}

bool Position::has_pawn_on_file(Color color, int col) const {
    return (piece_bb_[color_index(color)][type_index(PieceType::Pawn)] & tables().file_bb[col])
        .any();
}

bool Position::is_legal_move(const Move& move, bool enforce_pawn_drop_mate) const {
    const auto legal_moves = generate_legal_moves(true, enforce_pawn_drop_mate);
    return std::any_of(legal_moves.begin(), legal_moves.end(), [&](const Move& legal_move) {
        return same_move(legal_move, move);
    });
}

// 歩を打った後に相手玉が王手を受けていて、打つ手以外の応手がない（打ち歩詰め）か。
// 打つ手は元から王手を受けている不正局面でも考慮しない（従来の判定と同じ）
bool Position::is_pawn_drop_mate(const Move& move) const {
    const Color attacker = side_to_move_;
    const Color defender = opposite(attacker);
    const int king_square = find_king(defender);
    if (king_square < 0) {
        return false;
    }
    // 打つ手は利きの線を開かないので、王手になるのは歩自身の利きか元からの王手だけ
    const bool pawn_checks =
        tables().step_to[color_index(attacker)][type_index(PieceType::Pawn)][king_square].test(
            move.to);
    if (!pawn_checks && !attackers_to(king_square, attacker).any()) {
        return false;
    }
    Position next = *this;
    MoveUndo undo;
    next.make_move(move, undo);
    return !next.has_evasion(king_square, false);
}

bool Position::has_legal_move() const {
    const int king_square = find_king(side_to_move_);
    if (king_square >= 0 && attackers_to(king_square, opposite(side_to_move_)).any()) {
        return has_evasion(king_square, true);
    }
    return !generate_legal_moves().empty();
}

// 王手がかかっている手番側に合法な応手があるか（手を列挙せずに判定する）。
// allow_drops が false なら打ち合いを考慮しない
bool Position::has_evasion(int king_square, bool allow_drops) const {
    const auto& t = tables();
    const Color color = side_to_move_;
    const Color enemy = opposite(color);
    const int c = color_index(color);

    // 玉の移動（王手駒を玉で取る手を含む）
    Bitboard king_targets = t.step_from[c][type_index(PieceType::King)][king_square];
    king_targets &= ~color_bb_[c];
    king_targets &= ~piece_bb_[color_index(enemy)][type_index(PieceType::King)];
    const Bitboard occupied_without_king = occupied_ ^ t.square_bb[king_square];
    while (king_targets.any()) {
        const int to = king_targets.pop_lsb();
        if (!attackers_to(to, enemy, occupied_without_king).any()) {
            return true;
        }
    }

    const Bitboard checkers = attackers_to(king_square, enemy);
    if (checkers.more_than_one()) {
        return false;
    }
    const int checker = checkers.lsb();
    if (piece_type(board_[checker]) == PieceType::King) {
        return false;  // 玉同士が隣接する不正局面: 駒で相手玉は取れない
    }

    // ピンされた駒は王手駒を取ることも合駒することもできない
    Bitboard pinned;
    Bitboard snipers = aligned_sliders(king_square, enemy);
    while (snipers.any()) {
        const int sniper = snipers.pop_lsb();
        const Bitboard blockers = t.between[king_square][sniper] & occupied_;
        if (blockers.any() && !blockers.more_than_one() &&
            piece_color(board_[blockers.lsb()]) == color) {
            pinned |= blockers;
        }
    }
    const Bitboard movers = color_bb_[c] & ~pinned & ~t.square_bb[king_square];

    // 王手駒を取る
    if ((attackers_to(checker, color) & movers).any()) {
        return true;
    }

    // 合駒（移動合い・打ち合い）
    const Bitboard between = t.between[king_square][checker];
    if (between.none()) {
        return false;
    }
    Bitboard squares = between;
    while (squares.any()) {
        if ((attackers_to(squares.pop_lsb(), color) & movers).any()) {
            return true;
        }
    }
    if (!allow_drops) {
        return false;
    }
    Bitboard pawn_files;
    Bitboard pawns = piece_bb_[c][type_index(PieceType::Pawn)];
    while (pawns.any()) {
        pawn_files |= t.file_bb[square_col(pawns.pop_lsb())];
    }
    for (int index = 1; index < kHandPieceKinds; ++index) {
        if (hands_[c][index] == 0) {
            continue;
        }
        Bitboard drops = between & t.drop_masks[c][index];
        if (index == type_index(PieceType::Pawn)) {
            drops &= ~pawn_files;
            while (drops.any()) {
                const Move move{-1, drops.pop_lsb(), PieceType::Pawn, false, true};
                if (!is_pawn_drop_mate(move)) {
                    return true;
                }
            }
            continue;
        }
        if (drops.any()) {
            return true;
        }
    }
    return false;
}

int Position::find_king(Color color) const {
    return king_square_[static_cast<int>(color)];
}

bool Position::is_in_check(Color color) const {
    const int king_square = find_king(color);
    if (king_square == -1) {
        return false;
    }
    return attackers_to(king_square, opposite(color)).any();
}

bool Position::is_square_attacked(int square, Color by) const {
    return attackers_to(square, by).any();
}

bool Position::piece_attacks_square(int from, int target, PieceType type, Color color) const {
    return attacks_from(from, type, color, occupied_).test(target);
}

void Position::rebuild_history() {
    history_ = std::make_shared<HistoryNode>(
        HistoryNode{repetition_key(), side_to_move_, is_in_check(side_to_move_), nullptr});
}

void Position::append_history() {
    history_ = std::make_shared<HistoryNode>(
        HistoryNode{repetition_key(), side_to_move_, is_in_check(side_to_move_), history_});
}

std::uint64_t Position::repetition_key() const {
    return position_key_;
}

bool Position::is_repetition_draw() const {
    if (history_ == nullptr) {
        return false;
    }

    int occurrences = 0;
    for (auto node = history_; node != nullptr; node = node->previous) {
        if (node->key == history_->key && ++occurrences >= 4) {
            return true;
        }
    }
    return false;
}

bool Position::is_try_rule_win(Color color) const {
    const int king_square = find_king(color);
    if (king_square == -1) {
        return false;
    }
    return king_square == (color == Color::Black ? kBlackTrySquare : kWhiteTrySquare);
}

bool Position::is_perpetual_check_loss_for_opponent() const {
    if (history_ == nullptr || !history_->side_in_check) {
        return false;
    }

    int occurrences = 0;
    std::shared_ptr<const HistoryNode> earliest;
    for (auto node = history_; node != nullptr; node = node->previous) {
        if (node->key == history_->key) {
            ++occurrences;
            earliest = node;
            if (occurrences >= 4) {
                break;
            }
        }
    }
    if (occurrences < 4 || earliest == nullptr) {
        return false;
    }

    for (auto node = history_; node != nullptr; node = node->previous) {
        if (node->side_to_move == side_to_move_ && !node->side_in_check) {
            return false;
        }
        if (node == earliest) {
            break;
        }
    }
    return true;
}

int Position::handicap_entering_king_bonus(Color color) const {
    if (color != Color::White) {
        return 0;
    }
    if (rules_.entering_king_rule != EnteringKingRule::CSARule24H &&
        rules_.entering_king_rule != EnteringKingRule::CSARule27H) {
        return 0;
    }

    std::array<int, kHandPieceKinds> total_counts{};
    for (int square = 0; square < kSquareCount; ++square) {
        const int piece = board_[square];
        if (is_empty(piece)) {
            continue;
        }
        const PieceType type = unpromote(piece_type(piece));
        if (type == PieceType::King) {
            continue;
        }
        ++total_counts[hand_index(type)];
    }
    for (int owner = 0; owner < 2; ++owner) {
        for (int index = 1; index < kHandPieceKinds; ++index) {
            total_counts[index] += hands_[owner][index];
        }
    }

    int bonus = 0;
    for (int index = 1; index < kHandPieceKinds; ++index) {
        const int missing = std::max(0, kFullPieceCounts[index] - total_counts[index]);
        bonus += missing * impasse_point_value(static_cast<PieceType>(index));
    }
    return bonus;
}

int Position::declaration_threshold(Color color) const {
    switch (rules_.entering_king_rule) {
        case EnteringKingRule::CSARule24:
        case EnteringKingRule::CSARule24H:
            return 31;
        case EnteringKingRule::CSARule27:
        case EnteringKingRule::CSARule27H:
            return color == Color::Black ? 28 : 27;
        case EnteringKingRule::NoEnteringKing:
        case EnteringKingRule::TryRule:
        default:
            return kInfinity;
    }
}

int Position::impasse_threshold(Color color) const {
    switch (rules_.entering_king_rule) {
        case EnteringKingRule::CSARule24:
        case EnteringKingRule::CSARule24H:
            return 24;
        case EnteringKingRule::CSARule27:
        case EnteringKingRule::CSARule27H:
            return color == Color::Black ? 28 : 27;
        case EnteringKingRule::NoEnteringKing:
        case EnteringKingRule::TryRule:
        default:
            return kInfinity;
    }
}

bool Position::is_impasse_position() const {
    const int black_king = find_king(Color::Black);
    const int white_king = find_king(Color::White);
    if (black_king == -1 || white_king == -1) {
        return false;
    }
    if (!is_in_promotion_zone(Color::Black, square_row(black_king)) ||
        !is_in_promotion_zone(Color::White, square_row(white_king))) {
        return false;
    }
    if (is_in_check(Color::Black) || is_in_check(Color::White)) {
        return false;
    }
    return true;
}

int Position::impasse_points(Color color) const {
    int points = 0;
    for (int square = 0; square < kSquareCount; ++square) {
        const int piece = board_[square];
        if (is_empty(piece) || piece_color(piece) != color) {
            continue;
        }
        points += impasse_point_value(piece_type(piece));
    }
    for (int index = 1; index < kHandPieceKinds; ++index) {
        const PieceType piece = static_cast<PieceType>(index);
        points += hands_[static_cast<int>(color)][index] * impasse_point_value(piece);
    }
    return points;
}

int Position::declaration_points(Color color) const {
    int points = 0;
    for (int square = 0; square < kSquareCount; ++square) {
        const int piece = board_[square];
        if (is_empty(piece) || piece_color(piece) != color || piece_type(piece) == PieceType::King) {
            continue;
        }
        if (is_in_promotion_zone(color, square_row(square))) {
            points += impasse_point_value(piece_type(piece));
        }
    }
    for (int index = 1; index < kHandPieceKinds; ++index) {
        const PieceType piece = static_cast<PieceType>(index);
        points += hands_[static_cast<int>(color)][index] * impasse_point_value(piece);
    }
    return points;
}

int Position::pieces_in_opponent_camp(Color color) const {
    int count = 0;
    for (int square = 0; square < kSquareCount; ++square) {
        const int piece = board_[square];
        if (is_empty(piece) || piece_color(piece) != color || piece_type(piece) == PieceType::King) {
            continue;
        }
        if (is_in_promotion_zone(color, square_row(square))) {
            ++count;
        }
    }
    return count;
}

bool Position::opponent_has_mate_in_one(Color defender) const {
    Position attacker_position = *this;
    if (attacker_position.side_to_move_ != opposite(defender)) {
        attacker_position.position_key_ ^= zobrist().side_to_move;
    }
    attacker_position.side_to_move_ = opposite(defender);

    std::vector<Move> checking_moves;
    attacker_position.generate_checking_moves(checking_moves);
    for (const Move& move : checking_moves) {
        MoveUndo undo;
        attacker_position.make_move(move, undo);
        const bool mated = !attacker_position.has_legal_move();
        attacker_position.unmake_move(move, undo);
        if (mated) {
            return true;
        }
    }
    return false;
}

}  // namespace shogi
