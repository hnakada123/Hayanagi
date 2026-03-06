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
    std::mutex mutex_;
    std::thread search_thread_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool searching_{false};

    void handle_line(const std::string& line);
    void stop_search();
    void start_search(const std::string& line);
    void run_bench(const std::string& line);
    void run_perft(const std::string& line);
    bool set_position(const std::string& line);
    SearchOptions parse_go_options(const std::string& line) const;
    void print_info(const SearchInfo& info) const;
};

}  // namespace shogi
