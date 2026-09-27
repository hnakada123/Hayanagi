#include "usi_engine.h"
#include "tsume.h"
#include "version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

namespace shogi {

namespace {

// 一行分を組み立ててから排他出力する。isready と探索通知の混在を防ぐ。
class ProtocolOutput : public std::ostringstream {
public:
    ~ProtocolOutput() {
        static std::mutex output_mutex;
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << str() << std::flush;
    }
};

constexpr int kDefaultMultiPv = 1;
constexpr int kMaxMultiPv = 32;
constexpr int kDefaultThreads = 1;
constexpr int kMaxThreads = 128;
constexpr bool kDefaultUsiPonder = false;
constexpr std::size_t kMinHashSizeMb = 1;
constexpr std::size_t kMaxHashSizeMb = 65536;
constexpr int kDefaultMinimumThinkingTimeMs = 0;
constexpr int kDefaultNetworkDelayMs = 0;
constexpr int kDefaultNetworkDelay2Ms = 0;
constexpr int kDefaultSlowMover = 100;
constexpr int kDefaultResignValue = 99999;
constexpr int kMaxOptionMillis = 600000;
constexpr int kMaxSlowMover = 1000;
constexpr int kMaxResignValue = 99999;

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

bool parse_bool_option(const std::string& token, bool fallback = false) {
    if (token == "true" || token == "1") {
        return true;
    }
    if (token == "false" || token == "0") {
        return false;
    }
    return fallback;
}

std::optional<EnteringKingRule> parse_entering_king_rule(const std::string& token) {
    if (token == "NoEnteringKing") {
        return EnteringKingRule::NoEnteringKing;
    }
    if (token == "CSARule24") {
        return EnteringKingRule::CSARule24;
    }
    if (token == "CSARule24H") {
        return EnteringKingRule::CSARule24H;
    }
    if (token == "CSARule27") {
        return EnteringKingRule::CSARule27;
    }
    if (token == "CSARule27H") {
        return EnteringKingRule::CSARule27H;
    }
    if (token == "TryRule") {
        return EnteringKingRule::TryRule;
    }
    return std::nullopt;
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
        case TerminalReason::MoveLimit:
            return "move_limit";
        case TerminalReason::TryRule:
            return "try_rule";
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

// bench tsume の固定局面。BENCHMARK.md の計測に使う
struct TsumeBenchCase {
    const char* name;
    const char* sfen;
    const char* moves;
    bool attack;
    int depth;
};

const std::vector<TsumeBenchCase>& tsume_bench_suite() {
    static const std::vector<TsumeBenchCase> suite = {
        {"mate5-1", "9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1", "", true, 5},
        {"mate5-2", "9/9/9/R8/9/9/1k2+S4/9/3+N5 b RG2b3g3s3n4l18p 1", "", true, 5},
        {"mate5-3", "9/9/9/9/9/9/7+S1/5G2k/9 b RSr2b3g2s4n4l18p 1", "", true, 5},
        {"mate5-4", "5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1", "", true, 5},
        {"mate5-5", "1l7/k8/9/G8/3+R5/9/9/9/9 b R2b3g4s4n3l18p 1", "", true, 5},
        {"defense6", "5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1", "S*4b", false, 6},
        {"nomate3", "9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1", "", true, 3},
        {"nomate5", "9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1", "", true, 5},
    };
    return suite;
}

const char* tsume_status_text(TsumeStatus status) {
    switch (status) {
        case TsumeStatus::Mate: return "mate";
        case TsumeStatus::NoMate: return "nomate";
        case TsumeStatus::Limit: return "depthlimit";
        case TsumeStatus::Timeout: return "timeout";
        case TsumeStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

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
                               Position& next, bool tsume = false) {
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
        if (!next.set_sfen(sfen, tsume)) {
            return false;
        }
        index += 5;
    } else {
        return false;
    }

    if (index < tokens.size()) {
        if (tokens[index] != "moves") return false;
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

PerftStats perft_stats(Position& position, int depth, std::vector<std::vector<Move>>& buffers) {
    PerftStats stats;
    if (depth <= 0) {
        stats.nodes = 1;
        return stats;
    }

    auto& moves = buffers[static_cast<std::size_t>(depth)];
    position.generate_legal_moves(moves);
    if (depth == 1) {
        for (const Move& move : moves) {
            ++stats.nodes;
            if (position.piece_at(move.to) != 0) {
                ++stats.captures;
            }
            if (move.promote) {
                ++stats.promotions;
            }

            MoveUndo undo;
            position.make_move(move, undo);
            const bool gives_check = position.is_in_check(position.side_to_move());
            if (gives_check) {
                ++stats.checks;
                if (!position.has_legal_move()) {
                    ++stats.mates;
                }
            }
            position.unmake_move(move, undo);
        }
        return stats;
    }

    for (const Move& move : moves) {
        MoveUndo undo;
        position.make_move(move, undo);
        stats += perft_stats(position, depth - 1, buffers);
        position.unmake_move(move, undo);
    }
    return stats;
}

PerftStats perft_stats_for_root_move(const Position& position, const Move& move, int depth) {
    Position child = position;
    MoveUndo undo;
    child.make_move(move, undo);
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
            if (!child.has_legal_move()) {
                ++stats.mates;
            }
        }
        return stats;
    }
    std::vector<std::vector<Move>> buffers(static_cast<std::size_t>(depth));
    return perft_stats(child, depth - 1, buffers);
}

std::uint64_t compute_nps(std::uint64_t nodes, std::uint64_t elapsed_ms) {
    if (elapsed_ms == 0) {
        return nodes * 1000;
    }
    return nodes * 1000 / elapsed_ms;
}

std::string extract_ponder_move(const std::string& pv) {
    std::istringstream iss(pv);
    std::string bestmove;
    std::string ponder;
    if (!(iss >> bestmove) || !(iss >> ponder)) {
        return {};
    }
    return ponder;
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
        // CRLF とコマンド前後の空白を許容する。
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        handle_line(line);
        if (line == "quit") {
            break;
        }
    }
}

void UsiEngine::handle_line(const std::string& line) {
    const auto command_tokens = split_tokens(line);
    if (command_tokens.empty()) return;
    if (command_tokens[0] == "debug") {
        if (command_tokens.size() == 2 &&
            (command_tokens[1] == "on" || command_tokens[1] == "off")) {
            debug_.store(command_tokens[1] == "on");
            if (debug_.load()) ProtocolOutput{} << "info string debug on" << std::endl;
        }
        return;
    }
    if (debug_.load()) ProtocolOutput{} << "info string debug received " << line << std::endl;
    if (line == "usi") {
        ProtocolOutput{} << "id name " << kEngineName << " " << kEngineVersion << std::endl;
        ProtocolOutput{} << "id author OpenAI" << std::endl;
        ProtocolOutput{} << "option name USI_Ponder type check default "
                  << (kDefaultUsiPonder ? "true" : "false") << std::endl;
        ProtocolOutput{} << "option name MultiPV type spin default " << kDefaultMultiPv << " min 1 max "
                  << kMaxMultiPv << std::endl;
        ProtocolOutput{} << "option name USI_ShowCurrLine type check default false" << std::endl;
        ProtocolOutput{} << "option name USI_ShowRefutations type check default false" << std::endl;
        ProtocolOutput{} << "option name Threads type spin default " << kDefaultThreads << " min 1 max "
                  << kMaxThreads << std::endl;
        ProtocolOutput{} << "option name Hash type spin default " << Search::hash_size_mb() << " min "
                  << kMinHashSizeMb << " max " << kMaxHashSizeMb << std::endl;
        ProtocolOutput{} << "option name USI_Hash type spin default " << Search::hash_size_mb() << " min "
                  << kMinHashSizeMb << " max " << kMaxHashSizeMb << std::endl;
        ProtocolOutput{} << "option name MinimumThinkingTime type spin default "
                  << kDefaultMinimumThinkingTimeMs << " min 0 max " << kMaxOptionMillis
                  << std::endl;
        ProtocolOutput{} << "option name NetworkDelay type spin default " << kDefaultNetworkDelayMs
                  << " min 0 max " << kMaxOptionMillis << std::endl;
        ProtocolOutput{} << "option name NetworkDelay2 type spin default " << kDefaultNetworkDelay2Ms
                  << " min 0 max " << kMaxOptionMillis << std::endl;
        ProtocolOutput{} << "option name SlowMover type spin default " << kDefaultSlowMover << " min 1 max "
                  << kMaxSlowMover << std::endl;
        ProtocolOutput{} << "option name ResignValue type spin default " << kDefaultResignValue
                  << " min 0 max " << kMaxResignValue << std::endl;
        ProtocolOutput{} << "option name MaxMovesToDraw type spin default " << position_rules_.max_moves_to_draw
                  << " min 0 max " << kMaxOptionMillis << std::endl;
        ProtocolOutput{} << "option name EnteringKingRule type combo default CSARule24"
                  << " var NoEnteringKing var CSARule24 var CSARule24H var CSARule27"
                  << " var CSARule27H var TryRule" << std::endl;
        ProtocolOutput{} << "option name GenerateAllLegalMoves type check default "
                  << (position_rules_.generate_all_legal_moves ? "true" : "false") << std::endl;
        ProtocolOutput{} << "option name USI_OwnBook type check default true" << std::endl;
        ProtocolOutput{} << "option name BookDir type string default book" << std::endl;
        ProtocolOutput{} << "option name BookFile type combo default standard_book.db"
                  << " var no_book"
                  << " var standard_book.db"
                  << " var yaneura_book1.db"
                  << " var yaneura_book2.db"
                  << " var yaneura_book3.db"
                  << " var yaneura_book4.db"
                  << " var user_book1.db"
                  << " var user_book2.db"
                  << " var user_book3.db" << std::endl;
        ProtocolOutput{} << "option name TsumeMode type check default false" << std::endl;
        ProtocolOutput{} << "usiok" << std::endl;
        return;
    }
    if (line == "isready") {
        if (!book_loaded_ && usi_own_book_ && book_file_ != "no_book") {
            namespace fs = std::filesystem;
            fs::path book_path;
            fs::path dir(book_dir_);
            if (dir.is_absolute()) {
                book_path = dir / book_file_;
            } else {
                auto exe_dir = fs::path("/proc/self/exe");
                std::error_code ec;
                auto resolved = fs::read_symlink(exe_dir, ec);
                if (!ec) {
                    book_path = resolved.parent_path() / dir / book_file_;
                } else {
                    book_path = dir / book_file_;
                }
            }
            if (book_.load(book_path.string())) {
                ProtocolOutput{} << "info string book loaded " << book_path.string() << std::endl;
            } else {
                ProtocolOutput{} << "info string book not found " << book_path.string() << std::endl;
            }
            book_loaded_ = true;
        }
        ProtocolOutput{} << "readyok" << std::endl;
        return;
    }
    if (line == "usinewgame") {
        stop_search();
        std::lock_guard<std::mutex> lock(mutex_);
        position_.set_startpos();
        position_valid_ = true;
        mate_position_valid_ = true;
        return;
    }
    if (line.rfind("position ", 0) == 0) {
        stop_search();
        position_valid_ = set_position(line);
        if (!position_valid_ && !mate_position_valid_) {
            ProtocolOutput{} << "info string invalid position" << std::endl;
        }
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
    if (line.rfind("go tsume ", 0) == 0) {
        start_tsume(line);
        return;
    }
    if (line == "go mate" || line.rfind("go mate ", 0) == 0) {
        start_mate(line);
        return;
    }
    if (line == "go" || line.rfind("go ", 0) == 0) {
        start_search(line);
        return;
    }
    if (line == "stop") {
        stop_search(true);
        return;
    }
    if (line == "ponderhit") {
        std::lock_guard<std::mutex> lock(search_state_mutex_);
        if (pondering_) {
            ponderhit_ = true;
            search_state_cv_.notify_all();
        }
        return;
    }
    if (line.rfind("setoption ", 0) == 0) {
        set_option(line);
        return;
    }
    if (line == "gameover" || line.rfind("gameover ", 0) == 0) {
        stop_search();
        return;
    }
    if (line == "quit") {
        stop_search();
    }
}

void UsiEngine::set_option(const std::string& line) {
    const auto tokens = split_tokens(line);
    if (tokens.size() < 3 || tokens[0] != "setoption" || tokens[1] != "name") {
        return;
    }

    std::string value_token;
    for (std::size_t i = 3; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "value") {
            value_token = tokens[i + 1];
            for (std::size_t j = i + 2; j < tokens.size(); ++j) value_token += " " + tokens[j];
            break;
        }
    }

    bool rules_changed = false;
    if (tokens[2] == "TsumeMode") {
        stop_search();
        tsume_mode_ = parse_bool_option(value_token);
    } else if (tokens[2] == "USI_Ponder") {
        usi_ponder_.store(parse_bool_option(value_token, kDefaultUsiPonder));
    } else if (tokens[2] == "USI_ShowCurrLine") {
        show_currline_.store(parse_bool_option(value_token));
    } else if (tokens[2] == "USI_ShowRefutations") {
        show_refutations_.store(parse_bool_option(value_token));
    } else if (tokens[2] == "MultiPV" || tokens[2] == "USI_MultiPV") {
        multi_pv_.store(std::clamp(parse_int(value_token, kDefaultMultiPv), 1, kMaxMultiPv));
    } else if (tokens[2] == "Threads") {
        threads_.store(std::clamp(parse_int(value_token, kDefaultThreads), 1, kMaxThreads));
    } else if (tokens[2] == "Hash" || tokens[2] == "USI_Hash") {
        const std::size_t hash_size_mb = std::clamp<std::size_t>(
            parse_uint64(value_token, Search::hash_size_mb()), kMinHashSizeMb, kMaxHashSizeMb);
        stop_search();
        if (!Search::set_hash_size_mb(hash_size_mb)) {
            ProtocolOutput{} << "info string hash resize_failed " << hash_size_mb << std::endl;
        }
    } else if (tokens[2] == "MinimumThinkingTime") {
        minimum_thinking_time_ms_ =
            std::clamp(parse_int(value_token, kDefaultMinimumThinkingTimeMs), 0, kMaxOptionMillis);
    } else if (tokens[2] == "NetworkDelay") {
        network_delay_ms_ =
            std::clamp(parse_int(value_token, kDefaultNetworkDelayMs), 0, kMaxOptionMillis);
    } else if (tokens[2] == "NetworkDelay2") {
        network_delay2_ms_ =
            std::clamp(parse_int(value_token, kDefaultNetworkDelay2Ms), 0, kMaxOptionMillis);
    } else if (tokens[2] == "SlowMover") {
        slow_mover_ = std::clamp(parse_int(value_token, kDefaultSlowMover), 1, kMaxSlowMover);
    } else if (tokens[2] == "ResignValue") {
        resign_value_ = std::clamp(parse_int(value_token, kDefaultResignValue), 0, kMaxResignValue);
    } else if (tokens[2] == "MaxMovesToDraw") {
        position_rules_.max_moves_to_draw =
            std::clamp(parse_int(value_token, position_rules_.max_moves_to_draw), 0, kMaxOptionMillis);
        rules_changed = true;
    } else if (tokens[2] == "EnteringKingRule") {
        if (const auto rule = parse_entering_king_rule(value_token); rule.has_value()) {
            position_rules_.entering_king_rule = *rule;
            rules_changed = true;
        }
    } else if (tokens[2] == "GenerateAllLegalMoves") {
        position_rules_.generate_all_legal_moves =
            parse_bool_option(value_token, position_rules_.generate_all_legal_moves);
        rules_changed = true;
    } else if (tokens[2] == "USI_OwnBook") {
        usi_own_book_ = parse_bool_option(value_token, usi_own_book_);
    } else if (tokens[2] == "BookDir") {
        if (!value_token.empty()) {
            book_dir_ = value_token;
            book_loaded_ = false;
        }
    } else if (tokens[2] == "BookFile") {
        if (!value_token.empty()) {
            book_file_ = value_token;
            book_loaded_ = false;
        }
    }

    if (rules_changed) {
        std::lock_guard<std::mutex> lock(mutex_);
        apply_position_rules(position_);
    }
}

void UsiEngine::apply_position_rules(Position& position) const {
    position.set_rules(position_rules_);
}

void UsiEngine::stop_search(bool report_bestmove) {
    {
        std::lock_guard<std::mutex> lock(search_state_mutex_);
        suppress_bestmove_ = !report_bestmove;
    }
    stop_requested_.store(true);
    search_state_cv_.notify_all();
    if (search_thread_.joinable()) {
        search_thread_.join();
    }
    searching_.store(false);
    std::lock_guard<std::mutex> lock(search_state_mutex_);
    pondering_ = false;
    ponderhit_ = false;
    suppress_bestmove_ = false;
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
    if (debug_.load()) {
        ProtocolOutput{} << "info string debug search time_limit_ms " << options.time_limit_ms
            << " movestogo " << options.moves_to_go << " threads " << options.threads
            << " restricted " << options.restrict_searchmoves << std::endl;
    }
    const bool valid = position_valid_ && snapshot.find_king(Color::Black) >= 0 &&
                       snapshot.find_king(Color::White) >= 0;

    if (valid && usi_own_book_ && book_.is_loaded() && !options.ponder && !options.infinite) {
        const std::string sfen = snapshot.to_sfen();
        const auto* entries = book_.lookup(sfen);
        if (entries && !entries->empty()) {
            const auto selected = std::find_if(entries->begin(), entries->end(), [&](const BookEntry& entry) {
                if (options.restrict_searchmoves && std::find(options.searchmoves.begin(),
                        options.searchmoves.end(), entry.best_move) == options.searchmoves.end()) return false;
                Position check = snapshot;
                return check.apply_usi_move(entry.best_move);
            });
            if (selected != entries->end()) {
                const BookEntry& entry = *selected;
                ProtocolOutput{} << "info string book hit " << entry.best_move << " score " << entry.score
                          << " depth " << entry.depth << " count " << entry.count << std::endl;
                ProtocolOutput out;
                out << "bestmove " << entry.best_move;
                if (usi_ponder_.load() && entry.ponder_move != "none") {
                    out << " ponder " << entry.ponder_move;
                }
                out << std::endl;
                return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(search_state_mutex_);
        pondering_ = options.ponder;
        ponderhit_ = false;
        suppress_bestmove_ = false;
    }
    const int resign_value = resign_value_;
    searching_.store(true);

    search_thread_ = std::thread([this, snapshot, options, resign_value, valid]() mutable {
        SearchResult result;
        if (valid) {
            result = search_.find_best_move(snapshot, options, stop_requested_, [&](const SearchInfo& info) {
                print_info(info);
            });
        } else {
            ProtocolOutput{} << "info string invalid position for normal search" << std::endl;
        }

        {
            std::unique_lock<std::mutex> lock(search_state_mutex_);
            search_state_cv_.wait(lock, [this, &options]() {
                return stop_requested_.load() || suppress_bestmove_ ||
                       (!options.infinite && (!options.ponder || ponderhit_));
            });
            if (!suppress_bestmove_) report_bestmove(result, snapshot, resign_value);
            pondering_ = false;
            ponderhit_ = false;
        }

        searching_.store(false);
        search_state_cv_.notify_all();
    });
}

void UsiEngine::start_mate(const std::string& line) {
    stop_search();
    const auto tokens = split_tokens(line);
    const bool infinite = tokens.size() == 3 && tokens[2] == "infinite";
    const int millis = tokens.size() == 3 ? parse_int(tokens[2], -1) : -1;
    if (!mate_position_valid_ || position_.find_king(opposite(position_.side_to_move())) < 0 ||
        (!infinite && millis < 0)) {
        ProtocolOutput{} << "info string invalid go mate or position" << std::endl;
        ProtocolOutput{} << "checkmate timeout" << std::endl;
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = infinite ? Clock::time_point::max()
                                  : Clock::now() + std::chrono::milliseconds(millis);
    const Position snapshot = position_;
    const int threads = std::clamp(threads_.load(), 1, kMaxThreads);
    stop_requested_.store(false);
    searching_.store(true);
    search_thread_ = std::thread([this, snapshot, threads, deadline, infinite]() {
        TsumeSearch solver;
        Position work = snapshot;
        const Color attacker = snapshot.side_to_move();
        const auto solve = [&](int depth) {
            if (!infinite && Clock::now() >= deadline) {
                TsumeResult expired;
                expired.status = TsumeStatus::Timeout;
                return expired;
            }
            const int remaining = infinite ? 0 : std::max(1, static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
            return solver.solve(work, attacker, depth, remaining, stop_requested_, threads);
        };

        auto result = solve(63);
        const bool depth_limit = result.status == TsumeStatus::Limit;
        std::string response = "timeout";
        if (result.status == TsumeStatus::NoMate) {
            response = "nomate";
        } else if (result.status == TsumeStatus::Mate) {
            // 子局面の証明を再利用して、最長抵抗を含む合法な全手順を再構成する。
            // 再構成にも同じ時間制限と stop を適用する。
            std::string pv;
            while (result.status == TsumeStatus::Mate && result.plies > 0) {
                if (!result.move.is_valid()) break;
                const std::string move = work.move_to_usi(result.move);
                if (!work.apply_usi_move(move)) break;
                if (!pv.empty()) pv += ' ';
                pv += move;
                result = solve(result.plies - 1);
            }
            if (result.status == TsumeStatus::Mate && result.plies == 0 && !pv.empty() &&
                work.is_in_check(work.side_to_move()) && !work.has_legal_move()) {
                response = pv;
            }
        }

        std::unique_lock<std::mutex> lock(search_state_mutex_);
        if (depth_limit && !suppress_bestmove_) {
            ProtocolOutput{} << "info string mate search depth limit 63 reached" << std::endl;
            const auto stopped = [this]() { return stop_requested_.load() || suppress_bestmove_; };
            if (infinite) search_state_cv_.wait(lock, stopped);
            else search_state_cv_.wait_until(lock, deadline, stopped);
        }
        if (!suppress_bestmove_) ProtocolOutput{} << "checkmate " << response << std::endl;
        searching_.store(false);
        search_state_cv_.notify_all();
    });
}

void UsiEngine::start_tsume(const std::string& line) {
    stop_search();
    const auto tokens = split_tokens(line);
    if (!position_valid_ || tokens.size() < 3 || (tokens[2] != "attack" && tokens[2] != "defense")) {
        ProtocolOutput{} << "tsume invalid" << std::endl;
        return;
    }
    const Position snapshot = position_;
    const Color attacker = tokens[2] == "attack" ? snapshot.side_to_move()
                                                : opposite(snapshot.side_to_move());
    if (snapshot.find_king(opposite(attacker)) < 0) {
        ProtocolOutput{} << "tsume invalid" << std::endl;
        return;
    }
    int depth = 31;
    int millis = 5000;
    for (std::size_t i = 3; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "depth") depth = std::clamp(parse_int(tokens[i + 1], 31), 1, 63);
        if (tokens[i] == "movetime") millis = std::clamp(parse_int(tokens[i + 1], 5000), 1, 600000);
    }
    stop_requested_.store(false);
    searching_.store(true);
    const int threads = std::clamp(threads_.load(), 1, kMaxThreads);
    search_thread_ = std::thread([this, snapshot, attacker, depth, millis, threads]() {
        TsumeSearch solver;
        const auto result = solver.solve(snapshot, attacker, depth, millis, stop_requested_, threads);
        std::lock_guard<std::mutex> lock(search_state_mutex_);
        if (!suppress_bestmove_) {
            ProtocolOutput{} << "tsume " << tsume_status_text(result.status) << " move "
                      << (result.move.is_valid() ? snapshot.move_to_usi(result.move) : "none")
                      << " plies " << result.plies << " nodes " << result.nodes << std::endl;
        }
        searching_.store(false);
    });
}

void UsiEngine::report_bestmove(const SearchResult& result,
                                const Position& snapshot,
                                int resign_value) const {
    if (result.terminal.outcome == TerminalOutcome::Win &&
        result.terminal.reason == TerminalReason::DeclarationWin) {
        ProtocolOutput{} << "bestmove win" << std::endl;
        return;
    }
    if (result.terminal.is_terminal()) {
        ProtocolOutput{} << "info string terminal " << terminal_reason_text(result.terminal) << " "
                  << terminal_outcome_text(result.terminal) << std::endl;
        ProtocolOutput{} << "bestmove resign" << std::endl;
        return;
    }
    if (!result.has_best_move || result.score_cp <= -resign_value) {
        ProtocolOutput{} << "bestmove resign" << std::endl;
        return;
    }

    ProtocolOutput out;
    out << "bestmove " << snapshot.move_to_usi(result.best_move);
    if (usi_ponder_.load()) {
        const std::string ponder = extract_ponder_move(result.pv);
        if (!ponder.empty()) {
            out << " ponder " << ponder;
        }
    }
    out << std::endl;
}

void UsiEngine::run_bench(const std::string& line) {
    stop_search();

    const auto tokens = split_tokens(line);
    if (tokens.size() >= 2 && tokens[1] == "tsume") {
        run_tsume_bench(tokens);
        return;
    }

    int depth = 5;
    bool depth_explicit = false;
    bool current_only = false;
    std::uint64_t node_limit = 0;
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
            apply_position_rules(position);
            if (!load_position_from_command(entry.position_command, position)) {
                ProtocolOutput{} << "info string bench setup_failed " << entry.name << std::endl;
                continue;
            }
            items.push_back(BenchWorkItem{entry.name, position});
        }
    }

    if (items.empty()) {
        ProtocolOutput{} << "info string bench no_positions" << std::endl;
        return;
    }

    SearchOptions options;
    options.max_depth = depth;
    options.node_limit = node_limit;
    options.threads = std::clamp(threads_.load(), 1, kMaxThreads);

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

        ProtocolOutput{} << "info string bench " << (i + 1) << "/" << items.size() << " "
                  << items[i].name << " depth " << depth << " nodes " << result.nodes
                  << " time " << result.elapsed_ms << " nps "
                  << compute_nps(result.nodes, result.elapsed_ms) << " hashfull "
                  << result.hashfull_permille << " bestmove " << bestmove
                  << std::endl;
    }

    const auto total_end = std::chrono::steady_clock::now();
    const auto total_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count());
    ProtocolOutput out;
    out << "info string bench total positions " << items.size() << " depth " << depth
              << " nodes " << total_nodes << " time " << total_ms << " nps "
              << compute_nps(total_nodes, total_ms);
    if (node_limit > 0) {
        out << " node_limit " << node_limit;
    }
    out << " hashfull_avg " << (total_hashfull / static_cast<int>(items.size()))
              << " hashfull_max " << max_hashfull << std::endl;
}

// bench tsume [movetime M]: 固定局面の詰み探索を順に実行し、結果とノード数・時間を出力する
void UsiEngine::run_tsume_bench(const std::vector<std::string>& tokens) {
    int millis = 600000;
    for (std::size_t i = 2; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "movetime") {
            millis = std::clamp(parse_int(tokens[i + 1], millis), 1, kMaxOptionMillis);
        }
    }

    const auto& suite = tsume_bench_suite();
    std::uint64_t total_nodes = 0;
    std::uint64_t total_ms = 0;
    for (std::size_t i = 0; i < suite.size(); ++i) {
        const TsumeBenchCase& entry = suite[i];
        Position position;
        std::string command = std::string("position sfen ") + entry.sfen;
        if (entry.moves[0] != '\0') {
            command += std::string(" moves ") + entry.moves;
        }
        if (!load_position_from_tokens(split_tokens(command), 1, position, true)) {
            ProtocolOutput{} << "info string bench tsume setup_failed " << entry.name << std::endl;
            continue;
        }
        const Color attacker =
            entry.attack ? position.side_to_move() : opposite(position.side_to_move());
        std::atomic_bool stop{false};
        TsumeSearch solver;
        const auto start = std::chrono::steady_clock::now();
        const TsumeResult result = solver.solve(position, attacker, entry.depth, millis, stop,
                                               std::clamp(threads_.load(), 1, kMaxThreads));
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        total_nodes += result.nodes;
        total_ms += elapsed_ms;
        ProtocolOutput{} << "info string bench tsume " << (i + 1) << "/" << suite.size() << " "
                  << entry.name << " depth " << entry.depth << " status "
                  << tsume_status_text(result.status) << " move "
                  << (result.move.is_valid() ? position.move_to_usi(result.move) : "none")
                  << " plies " << result.plies << " nodes " << result.nodes << " time "
                  << elapsed_ms << " nps " << compute_nps(result.nodes, elapsed_ms) << std::endl;
    }
    ProtocolOutput{} << "info string bench tsume total positions " << suite.size() << " nodes "
              << total_nodes << " time " << total_ms << " nps "
              << compute_nps(total_nodes, total_ms) << std::endl;
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

    const int requested_threads = std::clamp(threads_.load(), 1, kMaxThreads);
    if (depth > 0 && (divide || (requested_threads > 1 && depth >= 3))) {
        const auto moves = snapshot.generate_legal_moves();
        std::vector<PerftStats> results(moves.size());
        const int count = depth >= 3 ? std::min(requested_threads, static_cast<int>(moves.size())) : 1;
        if (count > 1) {
            if (!perft_team_ || perft_team_->size() != static_cast<std::size_t>(count)) {
                perft_team_ = std::make_unique<ParallelTeam>(count);
            }
            perft_team_->run(moves.size(), [&](std::size_t index, std::size_t) {
                results[index] = perft_stats_for_root_move(snapshot, moves[index], depth);
            });
        } else {
            for (std::size_t i = 0; i < moves.size(); ++i) {
                results[i] = perft_stats_for_root_move(snapshot, moves[i], depth);
            }
        }
        for (std::size_t i = 0; i < moves.size(); ++i) {
            const PerftStats& child_stats = results[i];
            total += child_stats;
            if (!divide) continue;
            ProtocolOutput{} << snapshot.move_to_usi(moves[i]) << ": " << child_stats.nodes
                      << " captures " << child_stats.captures << " promotions "
                      << child_stats.promotions << " checks " << child_stats.checks
                      << " mates " << child_stats.mates << std::endl;
        }
    } else {
        std::vector<std::vector<Move>> buffers(static_cast<std::size_t>(depth) + 1);
        total = perft_stats(snapshot, depth, buffers);
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    ProtocolOutput out;
    out << "info string perft depth " << depth << " nodes " << total.nodes
              << " captures " << total.captures << " promotions " << total.promotions
              << " checks " << total.checks << " mates " << total.mates << " time "
              << elapsed_ms << " nps " << compute_nps(total.nodes, elapsed_ms);
    if (divide) {
        out << " divide";
    }
    out << std::endl;
}

bool UsiEngine::set_position(const std::string& line) {
    mate_position_valid_ = false;
    const auto tokens = split_tokens(line);
    if (tokens.size() < 2) {
        return false;
    }

    Position next;
    apply_position_rules(next);
    const bool valid = load_position_from_tokens(tokens, 1, next, tsume_mode_);
    if (!valid) {
        // 標準 go mate は専用オプションなしで攻方玉のない局面を受け取れる。
        next = Position{};
        apply_position_rules(next);
        if (!load_position_from_tokens(tokens, 1, next, true)) return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    position_ = next;
    mate_position_valid_ = true;
    return valid;
}

SearchOptions UsiEngine::parse_go_options(const std::string& line) const {
    SearchOptions options;
    options.show_currline = show_currline_.load();
    options.show_refutations = show_refutations_.load();
    options.multi_pv = std::clamp(multi_pv_.load(), 1, kMaxMultiPv);
    options.threads = std::clamp(threads_.load(), 1, kMaxThreads);
    const auto tokens = split_tokens(line);

    int movetime = 0;
    int black_time = 0;
    int white_time = 0;
    int black_inc = 0;
    int white_inc = 0;
    int byoyomi = 0;
    bool has_clock = false;

    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token == "searchmoves") {
            options.restrict_searchmoves = true;
            const std::vector<std::string> keywords = {"searchmoves", "depth", "movetime", "ponder",
                "nodes", "btime", "wtime", "binc", "winc", "byoyomi", "infinite", "movestogo", "mate"};
            while (i + 1 < tokens.size() && std::find(keywords.begin(), keywords.end(), tokens[i + 1]) == keywords.end()) {
                options.searchmoves.push_back(tokens[++i]);
            }
        } else if (token == "movestogo" && i + 1 < tokens.size()) {
            options.moves_to_go = std::max(0, parse_int(tokens[++i]));
        } else if (token == "depth" && i + 1 < tokens.size()) {
            options.max_depth = std::clamp(parse_int(tokens[++i], options.max_depth), 1, kMaxDepth);
        } else if (token == "movetime" && i + 1 < tokens.size()) {
            movetime = std::max(1, parse_int(tokens[++i]));
        } else if (token == "ponder") {
            options.ponder = true;
        } else if (token == "nodes" && i + 1 < tokens.size()) {
            options.node_limit = std::max<std::uint64_t>(1, parse_uint64(tokens[++i], 0));
        } else if (token == "btime" && i + 1 < tokens.size()) {
            has_clock = true;
            black_time = parse_int(tokens[++i]);
        } else if (token == "wtime" && i + 1 < tokens.size()) {
            has_clock = true;
            white_time = parse_int(tokens[++i]);
        } else if (token == "binc" && i + 1 < tokens.size()) {
            has_clock = true;
            black_inc = parse_int(tokens[++i]);
        } else if (token == "winc" && i + 1 < tokens.size()) {
            has_clock = true;
            white_inc = parse_int(tokens[++i]);
        } else if (token == "byoyomi" && i + 1 < tokens.size()) {
            has_clock = true;
            byoyomi = parse_int(tokens[++i]);
        } else if (token == "infinite") {
            options.infinite = true;
            options.max_depth = kMaxDepth;
        }
    }

    if (movetime > 0) {
        options.time_limit_ms = movetime;
    } else if (!options.infinite && has_clock) {
        bool black_to_move = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            black_to_move = position_.side_to_move() == Color::Black;
        }
        // 残り時間 0 は無制限ではない。加算・秒読みを含む計算は 64 ビットで行う。
        const std::int64_t remaining = std::max(0, black_to_move ? black_time : white_time);
        const std::int64_t increment = std::max(0, black_to_move ? black_inc : white_inc);
        const std::int64_t safe_byoyomi = std::max(0, byoyomi);
        const auto slice = remaining > 0 ? std::max<std::int64_t>(
            remaining / (options.moves_to_go > 0 ? options.moves_to_go : 30), 50) : 0;
        const auto soft_target = std::max<std::int64_t>(
            0, slice * slow_mover_ / 100 + increment + safe_byoyomi - network_delay_ms_);
        const auto hard_cap = std::max<std::int64_t>(
            0, remaining + increment + safe_byoyomi - network_delay2_ms_);
        const auto minimum_time = std::max<std::int64_t>(
            0, minimum_thinking_time_ms_ - network_delay_ms_ - network_delay2_ms_);
        options.time_limit_ms = static_cast<int>(std::clamp<std::int64_t>(
            std::min(std::max(soft_target, minimum_time), hard_cap),
            1, std::numeric_limits<int>::max()));
    }

    return options;
}

void UsiEngine::print_info(const SearchInfo& info) const {
    ProtocolOutput out;
    out << "info depth " << info.depth << " seldepth " << info.seldepth;
    if (info.show_multipv) {
        out << " multipv " << info.multipv;
    }
    if (info.has_score) {
        if (const auto mate = Search::mate_distance(info.score_cp)) {
            out << " score mate " << *mate;
        } else {
            out << " score cp " << info.score_cp;
        }
        if (info.score_bound == ScoreBound::Lower) out << " lowerbound";
        if (info.score_bound == ScoreBound::Upper) out << " upperbound";
    }
    out << " nodes " << info.nodes << " time " << info.elapsed_ms
        << " nps " << compute_nps(info.nodes, info.elapsed_ms)
        << " hashfull " << info.hashfull_permille;
    if (info.cpuload_permille >= 0) out << " cpuload " << info.cpuload_permille;
    if (!info.current_move.empty()) out << " currmove " << info.current_move;
    if (info.current_move_number > 0) out << " currmovenumber " << info.current_move_number;
    if (!info.pv.empty()) out << " pv " << info.pv;
    out << std::endl;
    for (std::size_t i = 0; i < info.current_lines.size(); ++i) {
        out << "info currline " << (i + 1);
        if (!info.current_lines[i].empty()) out << " " << info.current_lines[i];
        out << '\n';
    }
    for (const auto& line : info.refutations) {
        if (!line.empty()) out << "info refutation " << line << '\n';
    }
}

}  // namespace shogi
