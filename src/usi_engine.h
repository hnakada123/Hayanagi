#pragma once

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
    std::thread search_thread_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool searching_{false};
    std::atomic_int multi_pv_{1};
    PositionRules position_rules_{};
    int minimum_thinking_time_ms_ = 0;
    int network_delay_ms_ = 0;
    int network_delay2_ms_ = 0;
    int slow_mover_ = 100;
    int resign_value_ = 99999;

    void handle_line(const std::string& line);
    void set_option(const std::string& line);
    void apply_position_rules(Position& position) const;
    void stop_search();
    void start_search(const std::string& line);
    void run_bench(const std::string& line);
    void run_perft(const std::string& line);
    bool set_position(const std::string& line);
    SearchOptions parse_go_options(const std::string& line) const;
    void print_info(const SearchInfo& info) const;
};

}  // namespace shogi
