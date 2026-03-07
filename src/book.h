#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace shogi {

struct BookEntry {
    std::string best_move;
    std::string ponder_move;
    int score = 0;
    int depth = 0;
    int count = 0;
};

class Book {
public:
    bool load(const std::string& path);
    bool is_loaded() const { return loaded_; }
    const std::vector<BookEntry>* lookup(const std::string& sfen) const;

private:
    std::unordered_map<std::string, std::vector<BookEntry>> entries_;
    bool loaded_ = false;
};

}  // namespace shogi
