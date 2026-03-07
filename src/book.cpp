#include "book.h"

#include <fstream>
#include <sstream>

namespace shogi {

bool Book::load(const std::string& path) {
    entries_.clear();
    loaded_ = false;

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
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind("sfen ", 0) == 0) {
            current_sfen = line.substr(5);
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
    const auto it = entries_.find(sfen);
    if (it == entries_.end()) {
        return nullptr;
    }
    return &it->second;
}

}  // namespace shogi
