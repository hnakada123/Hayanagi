#include "evaluation.h"
#include "position.h"

#include <algorithm>
#include <cmath>

namespace shogi {

namespace {

constexpr int kTempoBonus = 12;

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

int rook_confinement_penalty(const Position& position, int square, Color color) {
    int safe = 0;
    for (const auto& direction : kRookDirections) {
        int row = square_row(square) + direction[0];
        int col = square_col(square) + direction[1];
        while (is_on_board(row, col)) {
            const int target = make_square(row, col);
            const int piece = position.piece_at(target);
            if (piece != 0 && piece_color(piece) == color) break;
            if (!position.is_square_attacked(target, opposite(color)) && ++safe >= 2) return 0;
            if (piece != 0) break;
            row += direction[0];
            col += direction[1];
        }
    }
    return safe == 0 ? 160 : 80;
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

int evaluate_position(const Position& position) {
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
            // 駒取りだけの静止探索では拾えない、守りのない駒への脅威を減点する。
            // まだ取られていないので、駒の価値をすべて差し引くことはしない。
            if (type != PieceType::Pawn &&
                position.is_square_attacked(square, opposite(color)) &&
                !position.is_square_attacked(square, color)) {
                contribution -= piece_value(type) / 2;
            }
            if (type == PieceType::Rook) {
                contribution -= rook_confinement_penalty(position, square, color);
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

}  // namespace shogi
