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

// make_move / unmake_move で局面を戻すための情報
struct MoveUndo {
    int captured = 0;  // 取った駒（符号付き）。取っていなければ 0
};

class Position {
public:
    Position();

    void set_startpos();
    bool set_sfen(const std::string& sfen, bool tsume = false);
    bool apply_usi_move(const std::string& move_text);
    void set_rules(const PositionRules& rules) { rules_ = rules; }
    const PositionRules& rules() const { return rules_; }

    std::vector<Move> generate_legal_moves() const;
    std::vector<Move> generate_legal_moves(bool include_drops, bool enforce_pawn_drop_mate) const;
    std::vector<Move> generate_search_legal_moves() const;
    std::vector<Move> generate_quiescence_moves() const;
    std::vector<Move> generate_checking_moves() const;
    // 呼び出し側のバッファに生成する版（内容と並び順は上の関数と同じ）
    void generate_legal_moves(std::vector<Move>& moves) const;
    void generate_checking_moves(std::vector<Move>& moves) const;
    // generate_legal_moves().empty() の否定を、手を列挙せずに求める
    bool has_legal_move() const;

    bool do_move(const Move& move);
    // 履歴を更新しない軽量な着手と待った。千日手判定を伴わない探索用で、
    // unmake_move は対応する make_move の直後に同じ手で呼ぶこと。
    void make_move(const Move& move, MoveUndo& undo);
    void unmake_move(const Move& move, const MoveUndo& undo);
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

    // 手番側の合法手生成に共通する情報
    struct GenState {
        Color color = Color::Black;
        Bitboard own_occ;
        Bitboard checkers;
        Bitboard pinned;
        Bitboard move_mask;  // 王手回避時に玉以外の駒が動ける升
        bool in_check = false;
        // ピンされた駒は方向ごとに高々 1 枚なので 8 枚まで
        int pin_count = 0;
        std::array<int, 8> pin_squares{};
        std::array<Bitboard, 8> pin_lines{};
        Bitboard pin_line(int square) const;
    };

    // 王手生成のための相手玉まわりの情報
    struct CheckInfo {
        int king = -1;         // 相手玉の升
        bool filter = false;   // true なら gives_check で後段判定する
        Bitboard discovered;   // 開き王手候補の自駒
        int discovered_count = 0;
        std::array<int, 8> discovered_squares{};
        std::array<Bitboard, 8> discovered_lines{};  // 候補ごとに遮っている線
        std::array<Bitboard, 15> drop_targets{};     // 駒種ごとに打って王手になる升
        Bitboard discovered_line(int square) const;
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
    void generate_moves(std::vector<Move>& moves,
                        bool include_drops,
                        bool enforce_pawn_drop_mate,
                        MoveSelection selection) const;
    void prepare_gen_state(Color color, GenState& state) const;
    void prepare_check_info(Color color, CheckInfo& info) const;
    Bitboard check_targets(PieceType landing_type,
                           Color color,
                           int enemy_king,
                           const Bitboard& occupied) const;
    void generate_king_moves(const GenState& state,
                             MoveSelection selection,
                             const CheckInfo* check_info,
                             std::vector<Move>& moves) const;
    void generate_piece_moves(const GenState& state,
                              PieceType type,
                              MoveSelection selection,
                              const CheckInfo* check_info,
                              std::vector<Move>& moves) const;
    void add_move_variants(int from,
                           int to,
                           PieceType piece,
                           MoveSelection selection,
                           bool in_check,
                           std::vector<Move>& moves) const;
    void add_checking_variants(int from,
                               int to,
                               PieceType piece,
                               bool plain_checks,
                               bool promoted_checks,
                               std::vector<Move>& moves) const;
    void add_drop_moves(const GenState& state,
                        bool enforce_pawn_drop_mate,
                        MoveSelection selection,
                        const CheckInfo* check_info,
                        std::vector<Move>& moves) const;
    bool should_keep_generated_move(const Move& move,
                                    MoveSelection selection,
                                    bool in_check) const;
    bool has_pawn_on_file(Color color, int col) const;
    bool is_legal_move(const Move& move, bool enforce_pawn_drop_mate) const;
    bool is_pawn_drop_mate(const Move& move) const;
    bool has_evasion(int king_square, bool allow_drops) const;
    void do_move_unchecked(const Move& move);
    Bitboard attacks_from(int from, PieceType type, Color color, const Bitboard& occupied) const;
    Bitboard rook_attacks(int from, const Bitboard& occupied) const;
    Bitboard bishop_attacks(int from, const Bitboard& occupied) const;
    Bitboard lance_attacks(int from, Color color, const Bitboard& occupied) const;
    Bitboard attackers_to(int square, Color by) const;
    Bitboard attackers_to(int square, Color by, const Bitboard& occupied) const;
    Bitboard slider_attackers_to(int square, Color by, const Bitboard& occupied) const;
    Bitboard aligned_sliders(int square, Color by) const;
    bool is_square_attacked_after_move(int square,
                                       Color by,
                                       const Move& move,
                                       PieceType moved_type) const;
    bool is_square_attacked_after_king_move(int square, Color by, int king_from) const;
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
