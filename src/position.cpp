#include "position.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
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
constexpr int kStepDirections[][2] = {
    {-1, -1}, {-1, 0}, {-1, 1}, {0, -1},
    {0, 1},   {1, -1}, {1, 0},  {1, 1},
};

int orientation(Color color) {
    return color == Color::Black ? 1 : -1;
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
    std::array<std::array<std::array<int, 8>, 8>, kSquareCount> ray_squares{};
    std::array<std::array<int, 8>, kSquareCount> ray_lengths{};
    std::array<std::array<Bitboard, kSquareCount>, kSquareCount> between{};
};

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
    tables.step_from[static_cast<int>(color)][static_cast<int>(type)][square].set(to);
    tables.step_to[static_cast<int>(color)][static_cast<int>(type)][to].set(square);
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
            int length = 0;
            while (is_on_board(next_row, next_col)) {
                const int to = make_square(next_row, next_col);
                tables.rays[square][dir].set(to);
                tables.ray_squares[square][dir][length++] = to;
                next_row += kDirRow[dir];
                next_col += kDirCol[dir];
            }
            tables.ray_lengths[square][dir] = length;
        }
    }

    for (int from = 0; from < kSquareCount; ++from) {
        for (int dir = 0; dir < 8; ++dir) {
            Bitboard between;
            for (int index = 0; index < tables.ray_lengths[from][dir]; ++index) {
                const int to = tables.ray_squares[from][dir][index];
                tables.between[from][to] = between;
                between |= tables.square_bb[to];
            }
        }
    }

    static constexpr int kPawn[][2] = {{-1, 0}};
    static constexpr int kKnight[][2] = {{-2, -1}, {-2, 1}};
    static constexpr int kSilver[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {1, -1}, {1, 1}};
    static constexpr int kGold[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, 0}};
    static constexpr int kRookStep[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    static constexpr int kBishopStep[][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

    for (int color_index = 0; color_index < 2; ++color_index) {
        const Color color = static_cast<Color>(color_index);
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
        }
    }

    return tables;
}

const Tables& tables() {
    static const Tables instance = build_tables();
    return instance;
}

Bitboard ray_attacks_from(int from,
                          const Bitboard& occupied,
                          const std::initializer_list<int>& directions) {
    const auto& t = tables();
    Bitboard attacks;
    for (const int dir : directions) {
        for (int index = 0; index < t.ray_lengths[from][dir]; ++index) {
            const int to = t.ray_squares[from][dir][index];
            attacks.set(to);
            if (occupied.test(to)) {
                break;
            }
        }
    }
    return attacks;
}

bool is_slider_for_direction(PieceType type, Color color, int direction) {
    switch (direction) {
        case North:
        case South:
        case West:
        case East:
            if (type == PieceType::Rook || type == PieceType::Dragon) {
                return true;
            }
            if (type == PieceType::Lance) {
                return direction == (color == Color::Black ? South : North);
            }
            return false;
        case NorthWest:
        case NorthEast:
        case SouthWest:
        case SouthEast:
            return type == PieceType::Bishop || type == PieceType::Horse;
        default:
            return false;
    }
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
    history_.reset();
}

void Position::add_piece(int square, Color color, PieceType type) {
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
    occupied_.reset(square);
    color_bb_[static_cast<int>(color)].reset(square);
    piece_bb_[static_cast<int>(color)][static_cast<int>(type)].reset(square);
    board_[square] = 0;
    if (type == PieceType::King) {
        king_square_[static_cast<int>(color)] = -1;
    }
}

void Position::set_startpos() {
    set_sfen(kStartposSfen);
}

bool Position::set_sfen(const std::string& sfen) {
    clear();
    const auto tokens = split_tokens(sfen);
    if (tokens.size() < 4) {
        return false;
    }

    int row = 0;
    int col = 0;
    bool promoted = false;
    for (char ch : tokens[0]) {
        if (ch == '/') {
            if (col != kBoardSize) {
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
            col += ch - '0';
            promoted = false;
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
        if (promoted) {
            type = promote(type);
        }
        const Color color =
            std::isupper(static_cast<unsigned char>(ch)) ? Color::Black : Color::White;
        add_piece(make_square(row, col), color, type);
        ++col;
        promoted = false;
    }
    if (row != 8 || col != kBoardSize) {
        return false;
    }

    if (tokens[1] == "b") {
        side_to_move_ = Color::Black;
    } else if (tokens[1] == "w") {
        side_to_move_ = Color::White;
    } else {
        return false;
    }

    if (tokens[2] != "-") {
        int count = 0;
        for (char ch : tokens[2]) {
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                count = count * 10 + (ch - '0');
                continue;
            }
            const auto piece = piece_from_letter(ch);
            if (!piece.has_value() || piece.value() == PieceType::King) {
                return false;
            }
            const Color color =
                std::isupper(static_cast<unsigned char>(ch)) ? Color::Black : Color::White;
            hands_[static_cast<int>(color)][hand_index(piece.value())] += std::max(count, 1);
            count = 0;
        }
        if (count != 0) {
            return false;
        }
    }

    if (king_square_[static_cast<int>(Color::Black)] == -1 ||
        king_square_[static_cast<int>(Color::White)] == -1) {
        return false;
    }

    try {
        ply_count_ = std::max(0, std::stoi(tokens[3]) - 1);
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

void Position::do_move_unchecked(const Move& move) {
    const Color mover = side_to_move_;
    if (move.drop) {
        add_piece(move.to, mover, move.piece);
        --hands_[static_cast<int>(mover)][hand_index(move.piece)];
    } else {
        const int moving_piece = board_[move.from];
        const PieceType moving_type = piece_type(moving_piece);
        const int captured_piece = board_[move.to];
        if (!is_empty(captured_piece)) {
            const PieceType captured_type = unpromote(piece_type(captured_piece));
            ++hands_[static_cast<int>(mover)][hand_index(captured_type)];
            remove_piece(move.to);
        }
        remove_piece(move.from);
        add_piece(move.to, mover, move.promote ? promote(moving_type) : moving_type);
    }
    ++ply_count_;
    side_to_move_ = opposite(side_to_move_);
    append_history();
}

TerminalStatus Position::terminal_status() const {
    if (is_perpetual_check_loss_for_opponent()) {
        return TerminalStatus{TerminalOutcome::Win, TerminalReason::PerpetualCheck};
    }
    if (is_repetition_draw()) {
        return TerminalStatus{TerminalOutcome::Draw, TerminalReason::Repetition};
    }
    if (ply_count_ >= 500 && !is_in_check(side_to_move_)) {
        return TerminalStatus{TerminalOutcome::Draw, TerminalReason::Impasse};
    }
    if (can_declare_win()) {
        return TerminalStatus{TerminalOutcome::Win, TerminalReason::DeclarationWin};
    }
    if (is_impasse_position()) {
        const int own_points = impasse_points(side_to_move_);
        const int opponent_points = impasse_points(opposite(side_to_move_));
        if (own_points >= 24 && opponent_points >= 24) {
            return TerminalStatus{TerminalOutcome::Draw, TerminalReason::Impasse};
        }
        if (own_points < 24 && opponent_points >= 24) {
            return TerminalStatus{TerminalOutcome::Loss, TerminalReason::Impasse};
        }
        if (own_points >= 24 && opponent_points < 24) {
            return TerminalStatus{TerminalOutcome::Win, TerminalReason::Impasse};
        }
    }
    return TerminalStatus{};
}

bool Position::can_declare_win() const {
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
    if (declaration_points(color) < 31) {
        return false;
    }
    return !opponent_has_mate_in_one(color);
}

bool Position::gives_check(const Move& move) const {
    Position next = *this;
    next.do_move_unchecked(move);
    return next.is_in_check(next.side_to_move_);
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

std::vector<Move> Position::generate_legal_moves() const {
    return generate_legal_moves(true, true);
}

Bitboard Position::rook_attacks(int from, const Bitboard& occupied) const {
    return ray_attacks_from(from, occupied, {North, South, West, East});
}

Bitboard Position::bishop_attacks(int from, const Bitboard& occupied) const {
    return ray_attacks_from(from, occupied, {NorthWest, NorthEast, SouthWest, SouthEast});
}

Bitboard Position::lance_attacks(int from, Color color, const Bitboard& occupied) const {
    return ray_attacks_from(from, occupied, {color == Color::Black ? North : South});
}

Bitboard Position::attacks_from(int from, PieceType type, Color color, const Bitboard& occupied) const {
    const auto& t = tables();
    Bitboard attacks = t.step_from[static_cast<int>(color)][static_cast<int>(type)][from];
    switch (type) {
        case PieceType::Lance:
            return attacks | lance_attacks(from, color, occupied);
        case PieceType::Bishop:
            return attacks | bishop_attacks(from, occupied);
        case PieceType::Rook:
            return attacks | rook_attacks(from, occupied);
        case PieceType::Horse:
            return attacks | bishop_attacks(from, occupied);
        case PieceType::Dragon:
            return attacks | rook_attacks(from, occupied);
        default:
            return attacks;
    }
}

Bitboard Position::attackers_to(int square, Color by) const {
    const auto& t = tables();
    const int color_index = static_cast<int>(by);
    Bitboard attackers;

    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::Pawn)] &
                 t.step_to[color_index][static_cast<int>(PieceType::Pawn)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::Knight)] &
                 t.step_to[color_index][static_cast<int>(PieceType::Knight)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::Silver)] &
                 t.step_to[color_index][static_cast<int>(PieceType::Silver)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::Gold)] &
                 t.step_to[color_index][static_cast<int>(PieceType::Gold)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::King)] &
                 t.step_to[color_index][static_cast<int>(PieceType::King)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::ProPawn)] &
                 t.step_to[color_index][static_cast<int>(PieceType::ProPawn)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::ProLance)] &
                 t.step_to[color_index][static_cast<int>(PieceType::ProLance)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::ProKnight)] &
                 t.step_to[color_index][static_cast<int>(PieceType::ProKnight)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::ProSilver)] &
                 t.step_to[color_index][static_cast<int>(PieceType::ProSilver)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::Horse)] &
                 t.step_to[color_index][static_cast<int>(PieceType::Horse)][square];
    attackers |= piece_bb_[color_index][static_cast<int>(PieceType::Dragon)] &
                 t.step_to[color_index][static_cast<int>(PieceType::Dragon)][square];

    for (int dir = 0; dir < 8; ++dir) {
        for (int index = 0; index < t.ray_lengths[square][dir]; ++index) {
            const int from = t.ray_squares[square][dir][index];
            const int piece = board_[from];
            if (is_empty(piece)) {
                continue;
            }
            if (piece_color(piece) == by &&
                is_slider_for_direction(piece_type(piece), by, dir)) {
                attackers.set(from);
            }
            break;
        }
    }

    return attackers;
}

bool Position::is_square_attacked_after_king_move(int square, Color by, int king_from) const {
    const auto& t = tables();
    const int color_index = static_cast<int>(by);

    if ((piece_bb_[color_index][static_cast<int>(PieceType::Pawn)] &
         t.step_to[color_index][static_cast<int>(PieceType::Pawn)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::Knight)] &
         t.step_to[color_index][static_cast<int>(PieceType::Knight)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::Silver)] &
         t.step_to[color_index][static_cast<int>(PieceType::Silver)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::Gold)] &
         t.step_to[color_index][static_cast<int>(PieceType::Gold)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::King)] &
         t.step_to[color_index][static_cast<int>(PieceType::King)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::ProPawn)] &
         t.step_to[color_index][static_cast<int>(PieceType::ProPawn)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::ProLance)] &
         t.step_to[color_index][static_cast<int>(PieceType::ProLance)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::ProKnight)] &
         t.step_to[color_index][static_cast<int>(PieceType::ProKnight)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::ProSilver)] &
         t.step_to[color_index][static_cast<int>(PieceType::ProSilver)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::Horse)] &
         t.step_to[color_index][static_cast<int>(PieceType::Horse)][square])
            .any()) {
        return true;
    }
    if ((piece_bb_[color_index][static_cast<int>(PieceType::Dragon)] &
         t.step_to[color_index][static_cast<int>(PieceType::Dragon)][square])
            .any()) {
        return true;
    }

    for (int dir = 0; dir < 8; ++dir) {
        for (int index = 0; index < t.ray_lengths[square][dir]; ++index) {
            const int from = t.ray_squares[square][dir][index];
            if (from == king_from) {
                continue;
            }
            const int piece = board_[from];
            if (is_empty(piece)) {
                continue;
            }
            if (piece_color(piece) == by &&
                is_slider_for_direction(piece_type(piece), by, dir)) {
                return true;
            }
            break;
        }
    }

    return false;
}

void Position::compute_pins_and_checks(Color color,
                                       Bitboard& checkers,
                                       Bitboard& pinned,
                                       std::array<Bitboard, kSquareCount>& pin_lines) const {
    const auto& t = tables();
    const Color enemy = opposite(color);
    const int king_square = find_king(color);

    checkers = attackers_to(king_square, enemy);
    pinned = Bitboard{};
    for (auto& line : pin_lines) {
        line = t.all_squares;
    }

    for (int dir = 0; dir < 8; ++dir) {
        int candidate = -1;
        for (int index = 0; index < t.ray_lengths[king_square][dir]; ++index) {
            const int sq = t.ray_squares[king_square][dir][index];
            const int piece = board_[sq];
            if (is_empty(piece)) {
                continue;
            }
            if (piece_color(piece) == color) {
                if (candidate != -1) {
                    break;
                }
                candidate = sq;
                continue;
            }
            if (!is_slider_for_direction(piece_type(piece), enemy, dir)) {
                break;
            }
            if (candidate != -1) {
                pinned.set(candidate);
                pin_lines[candidate] = t.between[king_square][sq] | t.square_bb[sq];
            }
            break;
        }
    }
}

void Position::generate_king_moves(Color color, std::vector<Move>& moves) const {
    const int from = find_king(color);
    Bitboard targets = attacks_from(from, PieceType::King, color, occupied_);
    targets &= ~color_bb_[static_cast<int>(color)];
    const Color enemy = opposite(color);

    while (targets.any()) {
        const int to = targets.pop_lsb();
        if (!is_square_attacked_after_king_move(to, enemy, from)) {
            moves.push_back(Move{from, to, PieceType::King, false, false});
        }
    }
}

void Position::generate_piece_moves(Color color,
                                    PieceType type,
                                    Bitboard pieces,
                                    const Bitboard& own_occ,
                                    const Bitboard& move_mask,
                                    const Bitboard& pinned,
                                    const std::array<Bitboard, kSquareCount>& pin_lines,
                                    std::vector<Move>& moves) const {
    while (pieces.any()) {
        const int from = pieces.pop_lsb();
        Bitboard targets = attacks_from(from, type, color, occupied_);
        targets &= ~own_occ;
        targets &= move_mask;
        if (pinned.test(from)) {
            targets &= pin_lines[from];
        }

        while (targets.any()) {
            const int to = targets.pop_lsb();
            add_move_variants(from, to, type, moves);
        }
    }
}

void Position::add_move_variants(int from, int to, PieceType piece, std::vector<Move>& moves) const {
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
        moves.push_back(Move{from, to, piece, false, false});
    }
    if (promotion_available) {
        moves.push_back(Move{from, to, piece, true, false});
    }
}

void Position::add_drop_moves(Color color,
                              const Bitboard& move_mask,
                              bool enforce_pawn_drop_mate,
                              std::vector<Move>& moves) const {
    const auto& t = tables();
    Bitboard empty = t.all_squares & ~occupied_;
    empty &= move_mask;

    for (int index = 1; index <= static_cast<int>(PieceType::Rook); ++index) {
        const PieceType piece = static_cast<PieceType>(index);
        if (piece == PieceType::King || hand_count(color, piece) == 0) {
            continue;
        }

        Bitboard targets = empty;
        for (int file = 0; file < kBoardSize; ++file) {
            if (piece == PieceType::Pawn &&
                (piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Pawn)] &
                 t.file_bb[file])
                    .any()) {
                targets &= ~t.file_bb[file];
            }
        }

        while (targets.any()) {
            const int to = targets.pop_lsb();
            const int row = square_row(to);
            if ((piece == PieceType::Pawn || piece == PieceType::Lance) &&
                is_last_rank(color, row)) {
                continue;
            }
            if (piece == PieceType::Knight && is_last_two_ranks(color, row)) {
                continue;
            }

            const Move move{-1, to, piece, false, true};
            if (piece == PieceType::Pawn && enforce_pawn_drop_mate && is_pawn_drop_mate(move)) {
                continue;
            }
            moves.push_back(move);
        }
    }
}

std::vector<Move> Position::generate_legal_moves(bool include_drops,
                                                 bool enforce_pawn_drop_mate) const {
    std::vector<Move> moves;
    moves.reserve(256);
    const Color color = side_to_move_;
    const auto& t = tables();
    const Bitboard own_occ = color_bb_[static_cast<int>(color)];

    Bitboard checkers;
    Bitboard pinned;
    std::array<Bitboard, kSquareCount> pin_lines{};
    compute_pins_and_checks(color, checkers, pinned, pin_lines);

    generate_king_moves(color, moves);

    const int check_count = checkers.count();
    if (check_count >= 2) {
        return moves;
    }

    Bitboard move_mask = t.all_squares;
    if (check_count == 1) {
        Bitboard single = checkers;
        const int checker_square = single.pop_lsb();
        move_mask = t.square_bb[checker_square];
        const PieceType checker_type = piece_type(board_[checker_square]);
        if (checker_type == PieceType::Lance || checker_type == PieceType::Bishop ||
            checker_type == PieceType::Rook || checker_type == PieceType::Horse ||
            checker_type == PieceType::Dragon) {
            move_mask |= t.between[find_king(color)][checker_square];
        }
    }

    generate_piece_moves(color,
                         PieceType::Pawn,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Pawn)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Lance,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Lance)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Knight,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Knight)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Silver,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Silver)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Gold,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Gold)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Bishop,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Bishop)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Rook,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Rook)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::ProPawn,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::ProPawn)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::ProLance,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::ProLance)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::ProKnight,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::ProKnight)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::ProSilver,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::ProSilver)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Horse,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Horse)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);
    generate_piece_moves(color,
                         PieceType::Dragon,
                         piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Dragon)],
                         own_occ,
                         move_mask,
                         pinned,
                         pin_lines,
                         moves);

    if (include_drops) {
        add_drop_moves(color, move_mask, enforce_pawn_drop_mate, moves);
    }

    return moves;
}

std::vector<Move> Position::generate_pseudo_legal_moves(Color color, bool include_drops) const {
    Position copy = *this;
    copy.side_to_move_ = color;
    return copy.generate_legal_moves(include_drops, false);
}

bool Position::has_pawn_on_file(Color color, int col) const {
    return (piece_bb_[static_cast<int>(color)][static_cast<int>(PieceType::Pawn)] &
            tables().file_bb[col])
        .any();
}

bool Position::is_legal_move(const Move& move, bool enforce_pawn_drop_mate) const {
    const auto legal_moves = generate_legal_moves(true, enforce_pawn_drop_mate);
    return std::any_of(legal_moves.begin(), legal_moves.end(), [&](const Move& legal_move) {
        return same_move(legal_move, move);
    });
}

bool Position::is_pawn_drop_mate(const Move& move) const {
    Position next = *this;
    next.do_move_unchecked(move);
    const Color attacker = side_to_move_;
    const Color defender = next.side_to_move_;
    const int king_square = next.find_king(defender);
    if (king_square == -1 || !next.is_square_attacked(king_square, attacker)) {
        return false;
    }
    return next.generate_legal_moves(false, false).empty();
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
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffset;
    const auto mix = [&](std::uint64_t value, std::uint64_t& current) {
        current ^= value;
        current *= kPrime;
    };

    for (int piece : board_) {
        mix(static_cast<std::uint64_t>(piece + 16), hash);
    }
    for (int color = 0; color < 2; ++color) {
        for (int index = 1; index < kHandPieceKinds; ++index) {
            mix(static_cast<std::uint64_t>(hands_[color][index] + 32 * color + index), hash);
        }
    }
    mix(static_cast<std::uint64_t>(side_to_move_ == Color::Black ? 1 : 2), hash);
    return hash;
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
    attacker_position.side_to_move_ = opposite(defender);

    const auto checking_moves = attacker_position.generate_legal_moves();
    for (const Move& move : checking_moves) {
        Position child = attacker_position;
        child.do_move_unchecked(move);
        if (!child.is_in_check(defender)) {
            continue;
        }
        if (child.generate_legal_moves().empty()) {
            return true;
        }
    }
    return false;
}

}  // namespace shogi
