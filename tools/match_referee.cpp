// Stateful match referee using the engine's rules, independently of USI bestmove/score.
// Input: position startpos [moves ...], position sfen <sfen> [moves ...], move <usi>.
// Output TSV: state, side, outcome, reason, SFEN, space-separated legal moves.
#include "position.h"

#include <iostream>
#include <sstream>
#include <string>

namespace {
using namespace shogi;

const char* reason_name(TerminalReason reason) {
    switch (reason) {
        case TerminalReason::DeclarationWin: return "declaration_win";
        case TerminalReason::Repetition: return "repetition";
        case TerminalReason::PerpetualCheck: return "perpetual_check";
        case TerminalReason::Impasse: return "impasse";
        case TerminalReason::MoveLimit: return "move_limit";
        case TerminalReason::TryRule: return "try_rule";
        default: return "none";
    }
}

void state(const Position& pos) {
    const auto terminal = pos.terminal_status();
    const auto moves = pos.generate_legal_moves();
    const char* outcome = "none";
    const char* reason = reason_name(terminal.reason);
    if (terminal.outcome == TerminalOutcome::Win) outcome = "win";
    if (terminal.outcome == TerminalOutcome::Loss) outcome = "loss";
    if (terminal.outcome == TerminalOutcome::Draw) outcome = "draw";
    if (!terminal.is_terminal() && moves.empty()) {
        outcome = "loss";
        reason = pos.is_in_check(pos.side_to_move()) ? "checkmate" : "no_legal_moves";
    }
    std::cout << "state\t" << (pos.side_to_move() == Color::Black ? "b" : "w")
              << '\t' << outcome << '\t' << reason << '\t' << pos.to_sfen() << '\t';
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (i) std::cout << ' ';
        std::cout << pos.move_to_usi(moves[i]);
    }
    std::cout << std::endl;
}

bool set_position(Position& pos, std::istringstream& in) {
    Position next;
    PositionRules rules;
    rules.max_moves_to_draw = 0;  // The runner records its explicit ply-cap draws.
    rules.entering_king_rule = EnteringKingRule::CSARule24;
    next.set_rules(rules);
    std::string token;
    if (!(in >> token)) return false;
    if (token == "startpos") {
        next.set_startpos();
    } else if (token == "sfen") {
        std::string sfen, field;
        for (int i = 0; i < 4; ++i) {
            if (!(in >> field)) return false;
            if (i) sfen += ' ';
            sfen += field;
        }
        if (!next.set_sfen(sfen)) return false;
    } else {
        return false;
    }
    if (in >> token) {
        if (token != "moves") return false;
        while (in >> token) {
            if (next.terminal_status().is_terminal() || !next.apply_usi_move(token)) return false;
        }
    }
    pos = next;
    return true;
}
}  // namespace

int main() {
    shogi::Position pos;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command, move, extra;
        in >> command;
        bool valid = false;
        if (command == "quit") break;
        if (command == "position") valid = set_position(pos, in);
        if (command == "move" && (in >> move) && !(in >> extra) &&
            !pos.terminal_status().is_terminal()) valid = pos.apply_usi_move(move);
        if (valid) state(pos);
        else std::cout << "error\tinvalid command or illegal move" << std::endl;
    }
}
