#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bitboard.h"
#include "types.h"

namespace shogi {

enum class EnteringKingRule : int {
    NoEnteringKing = 0,
    CSARule24,
    CSARule24H,
    CSARule27,
    CSARule27H,
    TryRule,
};

struct PositionRules {
    int max_moves_to_draw = 500;
    EnteringKingRule entering_king_rule = EnteringKingRule::CSARule24;
    bool generate_all_legal_moves = true;
};

class Position {
public:
    Position();

    void set_startpos();
    bool set_sfen(const std::string& sfen);
    bool apply_usi_move(const std::string& move_text);
    void set_rules(const PositionRules& rules) { rules_ = rules; }
    const PositionRules& rules() const { return rules_; }

    std::vector<Move> generate_legal_moves() const;
    std::vector<Move> generate_legal_moves(bool include_drops, bool enforce_pawn_drop_mate) const;
    std::vector<Move> generate_search_legal_moves() const;
    std::vector<Move> generate_quiescence_moves() const;
    std::vector<Move> generate_checking_moves() const;
    bool do_move(const Move& move);
    TerminalStatus terminal_status() const;
    bool can_declare_win() const;
    bool gives_check(const Move& move) const;
    int static_exchange_eval(const Move& move) const;
    Position make_null_move() const;

    std::string move_to_usi(const Move& move) const;
    std::string to_sfen() const;

    Color side_to_move() const { return side_to_move_; }
    int piece_at(int square) const { return board_[square]; }
    std::uint64_t position_key() const { return position_key_; }
    int hand_count(Color color, PieceType type) const;
    bool is_in_check(Color color) const;
    int find_king(Color color) const;
    bool is_square_attacked(int square, Color by) const;

private:
    struct HistoryNode {
        std::uint64_t key = 0;
        Color side_to_move = Color::Black;
        bool side_in_check = false;
        std::shared_ptr<const HistoryNode> previous;
    };

    enum class MoveSelection : int {
        All = 0,
        Tactical,
        Checking,
    };

    std::array<int, kSquareCount> board_{};
    std::array<std::array<int, kHandPieceKinds>, 2> hands_{};
    std::array<Bitboard, 2> color_bb_{};
    std::array<std::array<Bitboard, 15>, 2> piece_bb_{};
    std::array<int, 2> king_square_ = {-1, -1};
    Bitboard occupied_;
    Color side_to_move_ = Color::Black;
    int ply_count_ = 0;
    std::uint64_t position_key_ = 0;
    std::shared_ptr<const HistoryNode> history_;
    PositionRules rules_{};

    void clear();
    void add_piece(int square, Color color, PieceType type);
    void remove_piece(int square);
    void add_hand_piece(Color color, PieceType type);
    void remove_hand_piece(Color color, PieceType type);
    std::vector<Move> generate_pseudo_legal_moves(Color color, bool include_drops) const;
    void generate_king_moves(Color color,
                             MoveSelection selection,
                             bool in_check,
                             std::vector<Move>& moves) const;
    void generate_piece_moves(Color color,
                              PieceType type,
                              Bitboard pieces,
                              const Bitboard& own_occ,
                              const Bitboard& move_mask,
                              const Bitboard& pinned,
                              const std::array<Bitboard, kSquareCount>& pin_lines,
                              MoveSelection selection,
                              bool in_check,
                              std::vector<Move>& moves) const;
    void add_move_variants(int from,
                           int to,
                           PieceType piece,
                           MoveSelection selection,
                           bool in_check,
                           std::vector<Move>& moves) const;
    void add_drop_moves(Color color,
                        const Bitboard& move_mask,
                        bool enforce_pawn_drop_mate,
                        MoveSelection selection,
                        bool in_check,
                        std::vector<Move>& moves) const;
    bool should_keep_generated_move(const Move& move,
                                    MoveSelection selection,
                                    bool in_check) const;
    bool has_pawn_on_file(Color color, int col) const;
    bool is_legal_move(const Move& move, bool enforce_pawn_drop_mate) const;
    bool is_pawn_drop_mate(const Move& move) const;
    void do_move_unchecked(const Move& move);
    int piece_after_move_at(int square, const Move& move, Color mover, PieceType moved_type) const;
    Bitboard attacks_from(int from, PieceType type, Color color, const Bitboard& occupied) const;
    Bitboard rook_attacks(int from, const Bitboard& occupied) const;
    Bitboard bishop_attacks(int from, const Bitboard& occupied) const;
    Bitboard lance_attacks(int from, Color color, const Bitboard& occupied) const;
    Bitboard attackers_to(int square, Color by) const;
    bool is_square_attacked_after_move(int square,
                                       Color by,
                                       const Move& move,
                                       PieceType moved_type) const;
    bool is_square_attacked_after_king_move(int square, Color by, int king_from) const;
    void compute_pins_and_checks(Color color,
                                 Bitboard& checkers,
                                 Bitboard& pinned,
                                 std::array<Bitboard, kSquareCount>& pin_lines) const;
    bool piece_attacks_square(int from, int target, PieceType type, Color color) const;
    void rebuild_history();
    void append_history();
    std::uint64_t repetition_key() const;
    std::vector<Move> generate_legal_moves(bool include_drops,
                                           bool enforce_pawn_drop_mate,
                                           MoveSelection selection) const;
    bool is_repetition_draw() const;
    bool is_try_rule_win(Color color) const;
    bool is_perpetual_check_loss_for_opponent() const;
    bool is_impasse_position() const;
    int handicap_entering_king_bonus(Color color) const;
    int declaration_threshold(Color color) const;
    int impasse_threshold(Color color) const;
    int impasse_points(Color color) const;
    int declaration_points(Color color) const;
    int pieces_in_opponent_camp(Color color) const;
    bool opponent_has_mate_in_one(Color defender) const;
};

}  // namespace shogi
