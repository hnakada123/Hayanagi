#include "book.h"

#include <fstream>
#include <sstream>

namespace shogi {

namespace {
// Book positions share a key across move orders; actual game history stays in Position.
std::string book_key(const std::string& sfen, int& ply) {
    std::istringstream in(sfen);
    std::string board, side, hand, extra;
    if (!(in >> board >> side >> hand >> ply) || ply < 1 || (in >> extra) ||
        (side != "b" && side != "w")) return {};
    return board + " " + side + " " + hand;
}
}  // namespace

bool Book::load(const std::string& path) {
    entries_.clear();
    loaded_ = false;
    max_ply_ = 0;

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    std::string current_sfen;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("# HAYANAGI_MAX_PLY ", 0) == 0) {
            std::istringstream limit(line.substr(19));
            int value = 0;
            if (limit >> value && value > 0) max_ply_ = value;
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind("sfen ", 0) == 0) {
            int ply = 0;
            current_sfen = book_key(line.substr(5), ply);
            continue;
        }

        if (current_sfen.empty()) {
            continue;
        }

        std::istringstream iss(line);
        BookEntry entry;
        if (!(iss >> entry.best_move >> entry.ponder_move >> entry.score >> entry.depth >>
              entry.count)) {
            continue;
        }

        entries_[current_sfen].push_back(entry);
    }

    loaded_ = true;
    return true;
}

const std::vector<BookEntry>* Book::lookup(const std::string& sfen) const {
    int ply = 0;
    const std::string key = book_key(sfen, ply);
    if (key.empty() || (max_ply_ > 0 && ply > max_ply_)) return nullptr;
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return nullptr;
    }
    return &it->second;
}

}  // namespace shogi
