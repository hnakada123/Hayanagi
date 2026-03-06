#include "usi_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

namespace shogi {

namespace {

std::vector<std::string> split_tokens(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

int parse_int(const std::string& token, int fallback = 0) {
    try {
        return std::stoi(token);
    } catch (...) {
        return fallback;
    }
}

std::uint64_t parse_uint64(const std::string& token, std::uint64_t fallback = 0) {
    try {
        return std::stoull(token);
    } catch (...) {
        return fallback;
    }
}

const char* terminal_reason_text(const TerminalStatus& status) {
    switch (status.reason) {
        case TerminalReason::DeclarationWin:
            return "declaration_win";
        case TerminalReason::Repetition:
            return "repetition";
        case TerminalReason::PerpetualCheck:
            return "perpetual_check";
        case TerminalReason::Impasse:
            return "impasse";
        case TerminalReason::None:
        default:
            return "none";
    }
}

const char* terminal_outcome_text(const TerminalStatus& status) {
    switch (status.outcome) {
        case TerminalOutcome::Win:
            return "win";
        case TerminalOutcome::Draw:
            return "draw";
        case TerminalOutcome::Loss:
            return "loss";
        case TerminalOutcome::None:
        default:
            return "none";
    }
}

struct BenchCase {
    const char* name;
    const char* position_command;
};

struct PerftStats {
    std::uint64_t nodes = 0;
    std::uint64_t captures = 0;
    std::uint64_t promotions = 0;
    std::uint64_t checks = 0;
    std::uint64_t mates = 0;

    PerftStats& operator+=(const PerftStats& other) {
        nodes += other.nodes;
        captures += other.captures;
        promotions += other.promotions;
        checks += other.checks;
        mates += other.mates;
        return *this;
    }
};

bool load_position_from_tokens(const std::vector<std::string>& tokens,
                               std::size_t index,
                               Position& next) {
    if (tokens.size() <= index) {
        return false;
    }

    if (tokens[index] == "startpos") {
        next.set_startpos();
        ++index;
    } else if (tokens[index] == "sfen") {
        if (tokens.size() < index + 5) {
            return false;
        }
        const std::string sfen =
            tokens[index + 1] + " " + tokens[index + 2] + " " + tokens[index + 3] + " " +
            tokens[index + 4];
        if (!next.set_sfen(sfen)) {
            return false;
        }
        index += 5;
    } else {
        return false;
    }

    if (index < tokens.size() && tokens[index] == "moves") {
        ++index;
        for (; index < tokens.size(); ++index) {
            if (!next.apply_usi_move(tokens[index])) {
                return false;
            }
        }
    }

    return true;
}

bool load_position_from_command(const std::string& line, Position& next) {
    const auto tokens = split_tokens(line);
    if (tokens.empty() || tokens[0] != "position") {
        return false;
    }
    return load_position_from_tokens(tokens, 1, next);
}

PerftStats perft_stats(const Position& position, int depth) {
    PerftStats stats;
    if (depth <= 0) {
        stats.nodes = 1;
        return stats;
    }

    const auto moves = position.generate_legal_moves();
    if (depth == 1) {
        for (const Move& move : moves) {
            ++stats.nodes;
            if (position.piece_at(move.to) != 0) {
                ++stats.captures;
            }
            if (move.promote) {
                ++stats.promotions;
            }

            Position child = position;
            child.do_move(move);
            const bool gives_check = child.is_in_check(child.side_to_move());
            if (gives_check) {
                ++stats.checks;
                if (child.generate_legal_moves().empty()) {
                    ++stats.mates;
                }
            }
        }
        return stats;
    }

    for (const Move& move : moves) {
        Position child = position;
        child.do_move(move);
        stats += perft_stats(child, depth - 1);
    }
    return stats;
}

PerftStats perft_stats_for_root_move(const Position& position, const Move& move, int depth) {
    Position child = position;
    child.do_move(move);
    if (depth == 1) {
        PerftStats stats;
        stats.nodes = 1;
        if (position.piece_at(move.to) != 0) {
            ++stats.captures;
        }
        if (move.promote) {
            ++stats.promotions;
        }
        const bool gives_check = child.is_in_check(child.side_to_move());
        if (gives_check) {
            ++stats.checks;
            if (child.generate_legal_moves().empty()) {
                ++stats.mates;
            }
        }
        return stats;
    }
    return perft_stats(child, depth - 1);
}

std::uint64_t compute_nps(std::uint64_t nodes, std::uint64_t elapsed_ms) {
    if (elapsed_ms == 0) {
        return nodes * 1000;
    }
    return nodes * 1000 / elapsed_ms;
}

const std::array<BenchCase, 6>& bench_suite() {
    static constexpr std::array<BenchCase, 6> kSuite{{
        {"startpos", "position startpos"},
        {"double-pawn-push",
         "position startpos moves 7g7f 3c3d 2g2f 8c8d 2f2e 8d8e 2e2d"},
        {"rook-pawn-race",
         "position startpos moves 2g2f 8c8d 2f2e 8d8e 2e2d 8e8f 2d2c+ 8f8g+"},
        {"rook-recapture",
         "position startpos moves 7g7f 8c8d 2g2f 3c3d 2f2e 4a3b 2e2d 2c2d 2h2d"},
        {"bishop-exchange",
         "position startpos moves 7g7f 3c3d 8h2b+ 3a2b"},
        {"castle-shape",
         "position startpos moves 7g7f 3c3d 6i7h 4a3b 7i6h 8c8d 5g5f 5c5d 6h7g"},
    }};
    return kSuite;
}

}  // namespace

UsiEngine::~UsiEngine() {
    stop_search();
}

void UsiEngine::loop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        handle_line(line);
        if (line == "quit") {
            break;
        }
    }
}

void UsiEngine::handle_line(const std::string& line) {
    if (line == "usi") {
        std::cout << "id name Hayanagi" << std::endl;
        std::cout << "id author OpenAI" << std::endl;
        std::cout << "usiok" << std::endl;
        return;
    }
    if (line == "isready") {
        std::cout << "readyok" << std::endl;
        return;
    }
    if (line == "usinewgame") {
        stop_search();
        std::lock_guard<std::mutex> lock(mutex_);
        position_.set_startpos();
        return;
    }
    if (line.rfind("position ", 0) == 0) {
        stop_search();
        set_position(line);
        return;
    }
    if (line.rfind("bench", 0) == 0) {
        run_bench(line);
        return;
    }
    if (line.rfind("perft", 0) == 0) {
        run_perft(line);
        return;
    }
    if (line.rfind("go", 0) == 0) {
        start_search(line);
        return;
    }
    if (line == "stop") {
        stop_search();
        return;
    }
    if (line == "ponderhit") {
        return;
    }
    if (line.rfind("setoption ", 0) == 0) {
        return;
    }
    if (line == "quit") {
        stop_search();
    }
}

void UsiEngine::stop_search() {
    stop_requested_.store(true);
    if (search_thread_.joinable()) {
        search_thread_.join();
    }
    searching_.store(false);
}

void UsiEngine::start_search(const std::string& line) {
    stop_search();
    stop_requested_.store(false);

    Position snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = position_;
    }
    const SearchOptions options = parse_go_options(line);
    searching_.store(true);

    search_thread_ = std::thread([this, snapshot, options]() mutable {
        const SearchResult result =
            search_.find_best_move(snapshot, options, stop_requested_, [&](const SearchInfo& info) {
                print_info(info);
            });
        if (result.terminal.outcome == TerminalOutcome::Win &&
            result.terminal.reason == TerminalReason::DeclarationWin) {
            std::cout << "bestmove win" << std::endl;
        } else if (result.terminal.is_terminal()) {
            std::cout << "info string terminal " << terminal_reason_text(result.terminal) << " "
                      << terminal_outcome_text(result.terminal) << std::endl;
            std::cout << "bestmove resign" << std::endl;
        } else if (result.has_best_move) {
            std::cout << "bestmove " << snapshot.move_to_usi(result.best_move) << std::endl;
        } else {
            std::cout << "bestmove resign" << std::endl;
        }
        searching_.store(false);
    });
}

void UsiEngine::run_bench(const std::string& line) {
    stop_search();

    int depth = 5;
    bool depth_explicit = false;
    bool current_only = false;
    std::uint64_t node_limit = 0;
    const auto tokens = split_tokens(line);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "depth" && i + 1 < tokens.size()) {
            depth = std::max(1, parse_int(tokens[++i], depth));
            depth_explicit = true;
        } else if (tokens[i] == "nodes" && i + 1 < tokens.size()) {
            node_limit = std::max<std::uint64_t>(1, parse_uint64(tokens[++i], 0));
        } else if (tokens[i] == "current") {
            current_only = true;
        } else if (i == 1) {
            depth = std::max(1, parse_int(tokens[i], depth));
            depth_explicit = true;
        }
    }
    if (node_limit > 0 && !depth_explicit) {
        depth = kMaxDepth;
    }

    struct BenchWorkItem {
        std::string name;
        Position position;
    };

    std::vector<BenchWorkItem> items;
    if (current_only) {
        Position snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = position_;
        }
        items.push_back(BenchWorkItem{"current", snapshot});
    } else {
        for (const BenchCase& entry : bench_suite()) {
            Position position;
            if (!load_position_from_command(entry.position_command, position)) {
                std::cout << "info string bench setup_failed " << entry.name << std::endl;
                continue;
            }
            items.push_back(BenchWorkItem{entry.name, position});
        }
    }

    if (items.empty()) {
        std::cout << "info string bench no_positions" << std::endl;
        return;
    }

    SearchOptions options;
    options.max_depth = depth;
    options.node_limit = node_limit;

    const auto total_start = std::chrono::steady_clock::now();
    std::uint64_t total_nodes = 0;
    int total_hashfull = 0;
    int max_hashfull = 0;

    for (std::size_t i = 0; i < items.size(); ++i) {
        std::atomic_bool stop{false};
        const SearchResult result =
            search_.find_best_move(items[i].position, options, stop, [](const SearchInfo&) {});
        total_nodes += result.nodes;
        total_hashfull += result.hashfull_permille;
        max_hashfull = std::max(max_hashfull, result.hashfull_permille);

        std::string bestmove = "resign";
        if (result.terminal.outcome == TerminalOutcome::Win &&
            result.terminal.reason == TerminalReason::DeclarationWin) {
            bestmove = "win";
        } else if (result.has_best_move) {
            bestmove = items[i].position.move_to_usi(result.best_move);
        }

        std::cout << "info string bench " << (i + 1) << "/" << items.size() << " "
                  << items[i].name << " depth " << depth << " nodes " << result.nodes
                  << " time " << result.elapsed_ms << " nps "
                  << compute_nps(result.nodes, result.elapsed_ms) << " hashfull "
                  << result.hashfull_permille << " bestmove " << bestmove
                  << std::endl;
    }

    const auto total_end = std::chrono::steady_clock::now();
    const auto total_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count());
    std::cout << "info string bench total positions " << items.size() << " depth " << depth
              << " nodes " << total_nodes << " time " << total_ms << " nps "
              << compute_nps(total_nodes, total_ms);
    if (node_limit > 0) {
        std::cout << " node_limit " << node_limit;
    }
    std::cout << " hashfull_avg " << (total_hashfull / static_cast<int>(items.size()))
              << " hashfull_max " << max_hashfull << std::endl;
}

void UsiEngine::run_perft(const std::string& line) {
    stop_search();

    int depth = 1;
    bool divide = false;
    const auto tokens = split_tokens(line);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "depth" && i + 1 < tokens.size()) {
            depth = std::max(0, parse_int(tokens[++i], depth));
        } else if (tokens[i] == "divide") {
            divide = true;
        } else if (i == 1) {
            depth = std::max(0, parse_int(tokens[i], depth));
        }
    }

    Position snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = position_;
    }

    const auto start = std::chrono::steady_clock::now();
    PerftStats total;

    if (divide && depth > 0) {
        const auto moves = snapshot.generate_legal_moves();
        for (const Move& move : moves) {
            const PerftStats child_stats = perft_stats_for_root_move(snapshot, move, depth);
            total += child_stats;
            std::cout << snapshot.move_to_usi(move) << ": " << child_stats.nodes
                      << " captures " << child_stats.captures << " promotions "
                      << child_stats.promotions << " checks " << child_stats.checks
                      << " mates " << child_stats.mates << std::endl;
        }
    } else {
        total = perft_stats(snapshot, depth);
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    std::cout << "info string perft depth " << depth << " nodes " << total.nodes
              << " captures " << total.captures << " promotions " << total.promotions
              << " checks " << total.checks << " mates " << total.mates << " time "
              << elapsed_ms << " nps " << compute_nps(total.nodes, elapsed_ms);
    if (divide) {
        std::cout << " divide";
    }
    std::cout << std::endl;
}

bool UsiEngine::set_position(const std::string& line) {
    const auto tokens = split_tokens(line);
    if (tokens.size() < 2) {
        return false;
    }

    Position next;
    if (!load_position_from_tokens(tokens, 1, next)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    position_ = next;
    return true;
}

SearchOptions UsiEngine::parse_go_options(const std::string& line) const {
    SearchOptions options;
    const auto tokens = split_tokens(line);

    int movetime = 0;
    int black_time = 0;
    int white_time = 0;
    int black_inc = 0;
    int white_inc = 0;
    int byoyomi = 0;

    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token == "depth" && i + 1 < tokens.size()) {
            options.max_depth = std::max(1, parse_int(tokens[++i], options.max_depth));
        } else if (token == "movetime" && i + 1 < tokens.size()) {
            movetime = std::max(1, parse_int(tokens[++i]));
        } else if (token == "nodes" && i + 1 < tokens.size()) {
            options.node_limit = std::max<std::uint64_t>(1, parse_uint64(tokens[++i], 0));
        } else if (token == "btime" && i + 1 < tokens.size()) {
            black_time = parse_int(tokens[++i]);
        } else if (token == "wtime" && i + 1 < tokens.size()) {
            white_time = parse_int(tokens[++i]);
        } else if (token == "binc" && i + 1 < tokens.size()) {
            black_inc = parse_int(tokens[++i]);
        } else if (token == "winc" && i + 1 < tokens.size()) {
            white_inc = parse_int(tokens[++i]);
        } else if (token == "byoyomi" && i + 1 < tokens.size()) {
            byoyomi = parse_int(tokens[++i]);
        } else if (token == "infinite") {
            options.infinite = true;
            options.max_depth = kMaxDepth;
        }
    }

    if (movetime > 0) {
        options.time_limit_ms = movetime;
    } else if (!options.infinite) {
        const bool black_to_move = position_.side_to_move() == Color::Black;
        const int remaining = black_to_move ? black_time : white_time;
        const int increment = black_to_move ? black_inc : white_inc;
        if (remaining > 0 || increment > 0 || byoyomi > 0) {
            const int slice = remaining > 0 ? std::max(remaining / 30, 50) : 0;
            options.time_limit_ms =
                std::max(100, std::min(slice + increment + byoyomi, 5000));
        }
    }

    return options;
}

void UsiEngine::print_info(const SearchInfo& info) const {
    std::cout << "info depth " << info.depth << " score cp " << info.score_cp << " nodes "
              << info.nodes << " time " << info.elapsed_ms << " pv " << info.pv << std::endl;
}

}  // namespace shogi
