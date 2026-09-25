// Position の合法手生成・王手生成・make/unmake を、独立した単純な参照実装と突き合わせる。

#include "position.h"
#include "test_support.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace shogi;

namespace {

// 盤面配列だけを使う遅い攻撃判定（探索本体とは独立した参照実装）
bool reference_attacks(const Position& position, int from, int target) {
    const int piece = position.piece_at(from);
    if (piece == 0) return false;
    const Color color = piece_color(piece);
    const PieceType type = piece_type(piece);
    const int sign = color == Color::Black ? 1 : -1;
    const int dr = square_row(target) - square_row(from);
    const int dc = square_col(target) - square_col(from);

    const auto step = [&](std::initializer_list<std::pair<int, int>> deltas) {
        for (const auto& delta : deltas) {
            if (dr == delta.first * sign && dc == delta.second * sign) return true;
        }
        return false;
    };
    const auto slide = [&](std::initializer_list<std::pair<int, int>> deltas) {
        for (const auto& delta : deltas) {
            int row = square_row(from) + delta.first * sign;
            int col = square_col(from) + delta.second * sign;
            while (is_on_board(row, col)) {
                const int square = make_square(row, col);
                if (square == target) return true;
                if (position.piece_at(square) != 0) break;
                row += delta.first * sign;
                col += delta.second * sign;
            }
        }
        return false;
    };
    const std::initializer_list<std::pair<int, int>> gold = {{-1, -1}, {-1, 0}, {-1, 1},
                                                             {0, -1},  {0, 1},  {1, 0}};
    switch (type) {
        case PieceType::Pawn: return step({{-1, 0}});
        case PieceType::Lance: return slide({{-1, 0}});
        case PieceType::Knight: return step({{-2, -1}, {-2, 1}});
        case PieceType::Silver: return step({{-1, -1}, {-1, 0}, {-1, 1}, {1, -1}, {1, 1}});
        case PieceType::Gold:
        case PieceType::ProPawn:
        case PieceType::ProLance:
        case PieceType::ProKnight:
        case PieceType::ProSilver: return step(gold);
        case PieceType::Bishop: return slide({{-1, -1}, {-1, 1}, {1, -1}, {1, 1}});
        case PieceType::Rook: return slide({{-1, 0}, {1, 0}, {0, -1}, {0, 1}});
        case PieceType::King: return std::max(std::abs(dr), std::abs(dc)) == 1;
        case PieceType::Horse:
            return slide({{-1, -1}, {-1, 1}, {1, -1}, {1, 1}}) ||
                   step({{-1, 0}, {1, 0}, {0, -1}, {0, 1}});
        case PieceType::Dragon:
            return slide({{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) ||
                   step({{-1, -1}, {-1, 1}, {1, -1}, {1, 1}});
        default: return false;
    }
}

bool reference_square_attacked(const Position& position, int square, Color by) {
    for (int from = 0; from < kSquareCount; ++from) {
        const int piece = position.piece_at(from);
        if (piece != 0 && piece_color(piece) == by && reference_attacks(position, from, square)) {
            return true;
        }
    }
    return false;
}

bool reference_in_check(const Position& position, Color color) {
    const int king = position.find_king(color);
    return king >= 0 && reference_square_attacked(position, king, opposite(color));
}

std::string describe(const Position& position, const Move& move) {
    return position.to_sfen() + " " + position.move_to_usi(move);
}

bool same_move(const Move& lhs, const Move& rhs) {
    return lhs.from == rhs.from && lhs.to == rhs.to && lhs.piece == rhs.piece &&
           lhs.promote == rhs.promote && lhs.drop == rhs.drop;
}

std::string move_list(const Position& position, const std::vector<Move>& moves) {
    std::string text;
    for (const Move& move : moves) text += position.move_to_usi(move) + " ";
    return text;
}

// 1 局面について生成器の性質を検証する
void verify_position(const Position& position) {
    const Color mover = position.side_to_move();
    const Color enemy = opposite(mover);
    const std::vector<Move> legal = position.generate_legal_moves();
    CHECK_MSG(position.has_legal_move() == !legal.empty(), position.to_sfen());
    CHECK_MSG(position.is_in_check(mover) == reference_in_check(position, mover), position.to_sfen());

    std::vector<Move> buffer;
    position.generate_legal_moves(buffer);
    CHECK_MSG(buffer.size() == legal.size() &&
                  std::equal(buffer.begin(), buffer.end(), legal.begin(), same_move),
              position.to_sfen());

    // 合法手を指した後で自玉が取られないこと、make/unmake が局面を完全に戻すこと
    std::vector<Move> reference_checks;
    const std::string before = position.to_sfen();
    for (const Move& move : legal) {
        Position work = position;
        const std::uint64_t key = work.position_key();
        MoveUndo undo;
        work.make_move(move, undo);
        CHECK_MSG(!reference_in_check(work, mover), describe(position, move));
        CHECK_MSG(work.side_to_move() == enemy, describe(position, move));
        const bool gives_check = reference_in_check(work, enemy);
        CHECK_MSG(position.gives_check(move) == gives_check, describe(position, move));
        if (gives_check) reference_checks.push_back(move);
        // do_move と同じ局面になること
        Position copied = position;
        copied.do_move(move);
        CHECK_MSG(copied.position_key() == work.position_key(), describe(position, move));
        CHECK_MSG(copied.to_sfen() == work.to_sfen(), describe(position, move));
        work.unmake_move(move, undo);
        CHECK_MSG(work.position_key() == key, describe(position, move));
        CHECK_MSG(work.to_sfen() == before, describe(position, move));
        bool same_board = true;
        for (int square = 0; square < kSquareCount; ++square) {
            same_board = same_board && work.piece_at(square) == position.piece_at(square);
        }
        CHECK_MSG(same_board, describe(position, move));
    }

    // 王手生成は「合法手のうち王手になるもの」と一致し、順序も同じ
    std::vector<Move> checks = position.generate_checking_moves();
    CHECK_MSG(checks.size() == reference_checks.size() &&
                  std::equal(checks.begin(), checks.end(), reference_checks.begin(), same_move),
              position.to_sfen() + "\n  got: " + move_list(position, checks) +
                  "\n  ref: " + move_list(position, reference_checks));
    position.generate_checking_moves(buffer);
    CHECK_MSG(buffer.size() == checks.size() &&
                  std::equal(buffer.begin(), buffer.end(), checks.begin(), same_move),
              position.to_sfen());
}

struct RandomPositions {
    std::mt19937_64 rng{20260925};

    int random_int(int upper) {  // [0, upper)
        return static_cast<int>(rng() % static_cast<std::uint64_t>(upper));
    }

    // 詰将棋風のランダム局面（攻方玉は省略することがある）
    std::string tsume_like() {
        std::array<int, kSquareCount> board{};
        const int defender_king = random_int(kSquareCount);
        board[defender_king] = encode_piece(Color::White, PieceType::King);
        if (random_int(3) == 0) {
            int square = random_int(kSquareCount);
            while (board[square] != 0) square = random_int(kSquareCount);
            board[square] = encode_piece(Color::Black, PieceType::King);
        }
        std::array<int, 8> remaining = {0, 18, 4, 4, 4, 4, 2, 2};
        const int pieces = 1 + random_int(10);
        for (int i = 0; i < pieces; ++i) {
            const int type_index = 1 + random_int(7);
            if (remaining[type_index] == 0) continue;
            int square = random_int(kSquareCount);
            if (board[square] != 0) continue;
            const Color color = random_int(3) == 0 ? Color::White : Color::Black;
            PieceType type = static_cast<PieceType>(type_index);
            const int row = square_row(square);
            if (must_promote(type, color, row) || (can_promote(type) && random_int(3) == 0)) {
                type = promote(type);
            }
            board[square] = encode_piece(color, type);
            --remaining[type_index];
        }
        std::string sfen;
        for (int row = 0; row < kBoardSize; ++row) {
            int empty = 0;
            for (int col = 0; col < kBoardSize; ++col) {
                const int piece = board[make_square(row, col)];
                if (piece == 0) {
                    ++empty;
                    continue;
                }
                if (empty > 0) sfen += static_cast<char>('0' + empty);
                empty = 0;
                const PieceType type = piece_type(piece);
                if (is_promoted(type)) sfen += '+';
                char letter = piece_letter(type);
                if (piece_color(piece) == Color::White) {
                    letter = static_cast<char>(letter - 'A' + 'a');
                }
                sfen += letter;
            }
            if (empty > 0) sfen += static_cast<char>('0' + empty);
            if (row + 1 < kBoardSize) sfen += '/';
        }
        sfen += random_int(4) == 0 ? " w " : " b ";
        std::string hands;
        static constexpr char kLetters[] = {'\0', 'P', 'L', 'N', 'S', 'G', 'B', 'R'};
        for (int type_index = 1; type_index < 8; ++type_index) {
            const int total = remaining[type_index];
            const int black = random_int(total + 1);
            const int white = random_int(total - black + 1);
            if (black > 0) {
                if (black > 1) hands += std::to_string(black);
                hands += kLetters[type_index];
            }
            if (white > 0) {
                if (white > 1) hands += std::to_string(white);
                hands += static_cast<char>(kLetters[type_index] - 'A' + 'a');
            }
        }
        sfen += hands.empty() ? "-" : hands;
        sfen += " 1";
        return sfen;
    }
};

void test_random_positions() {
    RandomPositions generator;
    int verified = 0;
    for (int i = 0; i < 1500; ++i) {
        Position position;
        if (!position.set_sfen(generator.tsume_like(), true)) continue;
        // ランダムに数手進めながら検証し、王手・応手・成りを含む局面を幅広く通す
        for (int ply = 0; ply < 4; ++ply) {
            verify_position(position);
            ++verified;
            const auto moves = position.generate_legal_moves();
            if (moves.empty()) break;
            // 王手がかけられるときは高確率で王手を選び、詰み探索に近い局面を作る
            const auto checks = position.generate_checking_moves();
            const Move& move = (!checks.empty() && generator.random_int(4) != 0)
                                   ? checks[static_cast<std::size_t>(
                                         generator.random_int(static_cast<int>(checks.size())))]
                                   : moves[static_cast<std::size_t>(
                                         generator.random_int(static_cast<int>(moves.size())))];
            position.do_move(move);
        }
    }
    CHECK(verified > 3000);
}

void test_startpos_playouts() {
    std::mt19937_64 rng{7};
    for (int game = 0; game < 40; ++game) {
        Position position;
        position.set_startpos();
        for (int ply = 0; ply < 60; ++ply) {
            verify_position(position);
            const auto moves = position.generate_legal_moves();
            if (moves.empty()) break;
            position.do_move(moves[static_cast<std::size_t>(rng() % moves.size())]);
        }
    }
}

std::uint64_t perft(Position& position, int depth) {
    std::vector<Move> moves;
    position.generate_legal_moves(moves);
    if (depth == 1) return moves.size();
    std::uint64_t nodes = 0;
    for (const Move& move : moves) {
        MoveUndo undo;
        position.make_move(move, undo);
        nodes += perft(position, depth - 1);
        position.unmake_move(move, undo);
    }
    return nodes;
}

void test_perft() {
    Position position;
    position.set_startpos();
    CHECK(perft(position, 1) == 30);
    CHECK(perft(position, 2) == 900);
    CHECK(perft(position, 3) == 25470);
    CHECK(perft(position, 4) == 719731);
    CHECK(position.set_sfen(kStartposSfen));
    CHECK(position.apply_usi_move("7g7f") && position.apply_usi_move("3c3d") &&
          position.apply_usi_move("8h2b+") && position.apply_usi_move("3a2b") &&
          position.apply_usi_move("2g2f"));
    CHECK(perft(position, 3) == 277202);
    CHECK(position.set_sfen("4k4/9/9/9/4R4/9/9/9/4K4 w G2P 1"));
    CHECK(perft(position, 3) == 2702);
}

void test_special_rules() {
    Position position;
    // 打ち歩詰め: 1b に歩を打つと詰みなので合法手に含まれない
    CHECK(position.set_sfen("8k/6G2/7G1/9/9/9/9/9/9 b PR 1", true));
    for (const Move& move : position.generate_legal_moves()) {
        CHECK_MSG(position.move_to_usi(move) != "P*1b", "pawn drop mate must be excluded");
    }
    bool rook_drop = false;
    for (const Move& move : position.generate_checking_moves()) {
        CHECK_MSG(position.move_to_usi(move) != "P*1b", "pawn drop mate must be excluded");
        if (position.move_to_usi(move) == "R*1b") rook_drop = true;
    }
    CHECK(rook_drop);
    verify_position(position);

    // 二歩と行き所のない駒の打ち
    CHECK(position.set_sfen("k8/9/9/9/9/9/4P4/9/K8 b PLN 1"));
    for (const Move& move : position.generate_legal_moves()) {
        if (!move.drop) continue;
        const std::string usi = position.move_to_usi(move);
        CHECK_MSG(!(move.piece == PieceType::Pawn && square_col(move.to) == 4), usi);
        CHECK_MSG(!(move.piece == PieceType::Pawn && square_row(move.to) == 0), usi);
        CHECK_MSG(!(move.piece == PieceType::Lance && square_row(move.to) == 0), usi);
        CHECK_MSG(!(move.piece == PieceType::Knight && square_row(move.to) <= 1), usi);
    }
    verify_position(position);

    // 両王手は玉を動かすしかない
    CHECK(position.set_sfen("4k4/9/9/9/4R4/9/9/1B7/4K4 w - 1"));
    CHECK(position.is_in_check(Color::White));
    verify_position(position);

    // 玉のない攻方（詰将棋）でも生成できる
    CHECK(position.set_sfen("9/9/9/9/9/9/7+S1/5G2k/9 b RSr2b3g2s4n4l18p 1", true));
    verify_position(position);
    CHECK(!position.generate_checking_moves().empty());

    // 相手玉がない局面では王手は生成されない
    CHECK(position.set_sfen("9/9/9/9/9/9/9/9/K8 b R 1", true));
    CHECK(position.generate_checking_moves().empty());
}

}  // namespace

int run_position_tests() {
    test_perft();
    test_special_rules();
    test_startpos_playouts();
    test_random_positions();
    return test_support::failure_count();
}
