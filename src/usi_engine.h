#pragma once

#include <condition_variable>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "search.h"

namespace shogi {

class UsiEngine {
public:
    UsiEngine() = default;
    ~UsiEngine();

    void loop();

private:
    Position position_;
    Search search_;
    mutable std::mutex mutex_;
    mutable std::mutex search_state_mutex_;
    std::condition_variable search_state_cv_;
    std::thread search_thread_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool searching_{false};
    std::atomic_bool usi_ponder_{false};
    std::atomic_int multi_pv_{1};
    std::atomic_int threads_{1};
    PositionRules position_rules_{};
    bool pondering_ = false;
    bool ponderhit_ = false;
    bool suppress_bestmove_ = false;
    int minimum_thinking_time_ms_ = 0;
    int network_delay_ms_ = 0;
    int network_delay2_ms_ = 0;
    int slow_mover_ = 100;
    int resign_value_ = 99999;

    void handle_line(const std::string& line);
    void set_option(const std::string& line);
    void apply_position_rules(Position& position) const;
    void stop_search(bool report_bestmove = false);
    void start_search(const std::string& line);
    void run_bench(const std::string& line);
    void run_perft(const std::string& line);
    bool set_position(const std::string& line);
    SearchOptions parse_go_options(const std::string& line) const;
    void report_bestmove(const SearchResult& result, const Position& snapshot, int resign_value) const;
    void print_info(const SearchInfo& info) const;
};

}  // namespace shogi
