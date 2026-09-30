// Line protocol: "csa <moves...>" or "usi <moves...>" in; one TSV response out.
// Validate the entire game before returning any opening positions.
#include "position.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace shogi;

std::string csa_move(const Position& pos, const Move& move) {
    static constexpr const char* names[] = {
        "", "FU", "KY", "KE", "GI", "KI", "KA", "HI", "OU",
        "TO", "NY", "NK", "NG", "UM", "RY"};
    const auto square = [](int sq) {
        return std::to_string(9 - square_col(sq)) + std::to_string(square_row(sq) + 1);
    };
    const PieceType piece = move.promote ? promote(move.piece) : move.piece;
    return std::string(pos.side_to_move() == Color::Black ? "+" : "-") +
        (move.drop ? "00" : square(move.from)) + square(move.to) + names[static_cast<int>(piece)];
}

std::string convert(const std::string& line, int max_ply) {
    std::istringstream in(line);
    std::string mode, token;
    in >> mode;
    if (mode != "csa" && mode != "usi") return "error\tunknown format";
    Position pos;
    pos.set_startpos();
    PositionRules rules;
    rules.max_moves_to_draw = 0;
    // Impasse/declaration policies differ between data sources. The importer validates
    // legal play and repetition; it does not adjudicate a recorded resignation.
    rules.entering_king_rule = EnteringKingRule::NoEnteringKing;
    pos.set_rules(rules);
    std::vector<std::string> positions, moves;
    while (in >> token) {
        if (moves.size() >= 2048) return "error\tgame too long";
        if (pos.terminal_status().is_terminal()) return "error\tmove after terminal position";
        const auto legal = pos.generate_legal_moves();
        const auto it = std::find_if(legal.begin(), legal.end(), [&](const Move& move) {
            return (mode == "csa" ? csa_move(pos, move) : pos.move_to_usi(move)) == token;
        });
        if (it == legal.end()) return "error\tillegal move at ply " + std::to_string(moves.size() + 1);
        if (static_cast<int>(moves.size()) < max_ply) positions.push_back(pos.to_sfen());
        moves.push_back(pos.move_to_usi(*it));
        pos.do_move(*it);
    }
    if (moves.empty()) return "error\tempty game";
    std::string result = "ok\t";
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (i) result += ' ';
        result += moves[i];
    }
    for (const auto& sfen : positions) result += '\t' + sfen;
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    int max_ply = 30;
    try {
        if (argc != 2) throw std::invalid_argument("usage");
        std::size_t end = 0;
        max_ply = std::stoi(argv[1], &end);
        if (end != std::string(argv[1]).size() || max_ply < 1 || max_ply > 512)
            throw std::invalid_argument("range");
    } catch (...) {
        std::cerr << "Usage: hayanagi_book_positions MAX_PLY (1..512)\n";
        return 2;
    }
    std::string line;
    while (std::getline(std::cin, line)) std::cout << convert(line, max_ply) << std::endl;
    return 0;
}
