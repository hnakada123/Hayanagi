#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace shogi {

constexpr int kBoardSize = 9;
constexpr int kSquareCount = kBoardSize * kBoardSize;
constexpr int kHandPieceKinds = 8;
constexpr int kMaxDepth = 64;
constexpr int kMateScore = 30000;
constexpr int kInfinity = 32000;
constexpr const char* kStartposSfen =
    "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1";

enum class Color : int {
    Black = 0,
    White = 1,
};

inline Color opposite(Color color) {
    return color == Color::Black ? Color::White : Color::Black;
}

enum class PieceType : int {
    Empty = 0,
    Pawn = 1,
    Lance = 2,
    Knight = 3,
    Silver = 4,
    Gold = 5,
    Bishop = 6,
    Rook = 7,
    King = 8,
    ProPawn = 9,
    ProLance = 10,
    ProKnight = 11,
    ProSilver = 12,
    Horse = 13,
    Dragon = 14,
};

inline int encode_piece(Color color, PieceType type) {
    if (type == PieceType::Empty) {
        return 0;
    }
    return color == Color::Black ? static_cast<int>(type) : -static_cast<int>(type);
}

inline bool is_empty(int piece) {
    return piece == 0;
}

inline Color piece_color(int piece) {
    return piece > 0 ? Color::Black : Color::White;
}

inline PieceType piece_type(int piece) {
    return static_cast<PieceType>(std::abs(piece));
}

inline bool is_promoted(PieceType type) {
    switch (type) {
        case PieceType::ProPawn:
        case PieceType::ProLance:
        case PieceType::ProKnight:
        case PieceType::ProSilver:
        case PieceType::Horse:
        case PieceType::Dragon:
            return true;
        default:
            return false;
    }
}

inline bool can_promote(PieceType type) {
    switch (type) {
        case PieceType::Pawn:
        case PieceType::Lance:
        case PieceType::Knight:
        case PieceType::Silver:
        case PieceType::Bishop:
        case PieceType::Rook:
            return true;
        default:
            return false;
    }
}

inline PieceType promote(PieceType type) {
    switch (type) {
        case PieceType::Pawn:
            return PieceType::ProPawn;
        case PieceType::Lance:
            return PieceType::ProLance;
        case PieceType::Knight:
            return PieceType::ProKnight;
        case PieceType::Silver:
            return PieceType::ProSilver;
        case PieceType::Bishop:
            return PieceType::Horse;
        case PieceType::Rook:
            return PieceType::Dragon;
        default:
            return type;
    }
}

inline PieceType unpromote(PieceType type) {
    switch (type) {
        case PieceType::ProPawn:
            return PieceType::Pawn;
        case PieceType::ProLance:
            return PieceType::Lance;
        case PieceType::ProKnight:
            return PieceType::Knight;
        case PieceType::ProSilver:
            return PieceType::Silver;
        case PieceType::Horse:
            return PieceType::Bishop;
        case PieceType::Dragon:
            return PieceType::Rook;
        default:
            return type;
    }
}

inline int square_row(int square) {
    return square / kBoardSize;
}

inline int square_col(int square) {
    return square % kBoardSize;
}

inline int make_square(int row, int col) {
    return row * kBoardSize + col;
}

inline bool is_on_board(int row, int col) {
    return row >= 0 && row < kBoardSize && col >= 0 && col < kBoardSize;
}

inline bool is_in_promotion_zone(Color color, int row) {
    return color == Color::Black ? row <= 2 : row >= 6;
}

inline bool is_last_rank(Color color, int row) {
    return color == Color::Black ? row == 0 : row == 8;
}

inline bool is_last_two_ranks(Color color, int row) {
    return color == Color::Black ? row <= 1 : row >= 7;
}

inline bool must_promote(PieceType type, Color color, int to_row) {
    switch (type) {
        case PieceType::Pawn:
        case PieceType::Lance:
            return is_last_rank(color, to_row);
        case PieceType::Knight:
            return is_last_two_ranks(color, to_row);
        default:
            return false;
    }
}

inline char piece_letter(PieceType type) {
    switch (unpromote(type)) {
        case PieceType::Pawn:
            return 'P';
        case PieceType::Lance:
            return 'L';
        case PieceType::Knight:
            return 'N';
        case PieceType::Silver:
            return 'S';
        case PieceType::Gold:
            return 'G';
        case PieceType::Bishop:
            return 'B';
        case PieceType::Rook:
            return 'R';
        case PieceType::King:
            return 'K';
        default:
            return '?';
    }
}

inline std::optional<PieceType> piece_from_letter(char letter) {
    switch (std::toupper(static_cast<unsigned char>(letter))) {
        case 'P':
            return PieceType::Pawn;
        case 'L':
            return PieceType::Lance;
        case 'N':
            return PieceType::Knight;
        case 'S':
            return PieceType::Silver;
        case 'G':
            return PieceType::Gold;
        case 'B':
            return PieceType::Bishop;
        case 'R':
            return PieceType::Rook;
        case 'K':
            return PieceType::King;
        default:
            return std::nullopt;
    }
}

struct Move {
    int from = -1;
    int to = -1;
    PieceType piece = PieceType::Empty;
    bool promote = false;
    bool drop = false;

    bool is_valid() const {
        return to >= 0 && to < kSquareCount && piece != PieceType::Empty;
    }
};

enum class TerminalOutcome : int {
    None = 0,
    Win,
    Draw,
    Loss,
};

enum class TerminalReason : int {
    None = 0,
    DeclarationWin,
    Repetition,
    PerpetualCheck,
    Impasse,
};

struct TerminalStatus {
    TerminalOutcome outcome = TerminalOutcome::None;
    TerminalReason reason = TerminalReason::None;

    bool is_terminal() const {
        return outcome != TerminalOutcome::None;
    }
};

inline std::string square_to_usi(int square) {
    const int file = 9 - square_col(square);
    const char rank = static_cast<char>('a' + square_row(square));
    return std::string{static_cast<char>('0' + file), rank};
}

inline std::optional<int> square_from_usi(const std::string& text) {
    if (text.size() != 2) {
        return std::nullopt;
    }
    const char file = text[0];
    const char rank = text[1];
    if (file < '1' || file > '9' || rank < 'a' || rank > 'i') {
        return std::nullopt;
    }
    const int col = '9' - file;
    const int row = rank - 'a';
    return make_square(row, col);
}

inline int hand_index(PieceType type) {
    return static_cast<int>(unpromote(type));
}

inline int piece_value(PieceType type) {
    static constexpr std::array<int, 15> kValues = {
        0, 100, 300, 320, 450, 550, 800, 1000, 20000, 550, 550, 550, 550, 950, 1150,
    };
    return kValues[static_cast<int>(type)];
}

inline int impasse_point_value(PieceType type) {
    switch (unpromote(type)) {
        case PieceType::Bishop:
        case PieceType::Rook:
            return 5;
        case PieceType::Pawn:
        case PieceType::Lance:
        case PieceType::Knight:
        case PieceType::Silver:
        case PieceType::Gold:
            return 1;
        default:
            return 0;
    }
}

}  // namespace shogi
