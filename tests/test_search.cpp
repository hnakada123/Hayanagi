// 探索窓の境界値、再探索、初手制限による置換表への影響を検証する。
#include "search.h"
#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <vector>

using namespace shogi;

int run_search_tests() {
    for (const int threads : {1, 4}) {
        Position root;
        root.set_startpos();
        SearchOptions options;
        options.threads = threads;
        options.max_depth = 4;
        options.aspiration_min_depth = 2;
        options.aspiration_window_cp = 1;
        Search search;
        std::atomic_bool stop{false};
        CHECK(Search::set_hash_size_mb(1));
        std::vector<SearchInfo> reports;
        const auto result = search.find_best_move(root, options, stop, [&](const SearchInfo& info) {
            reports.push_back(info);
        });
        CHECK(result.completed_depth == options.max_depth);
        bool lower = false;
        bool upper = false;
        for (const auto& info : reports) {
            if (info.score_bound == ScoreBound::Exact) continue;
            const auto exact = std::find_if(reports.begin(), reports.end(), [&](const SearchInfo& candidate) {
                return candidate.depth == info.depth && candidate.score_bound == ScoreBound::Exact;
            });
            CHECK(exact != reports.end());
            if (exact == reports.end()) continue;
            if (info.score_bound == ScoreBound::Lower) {
                lower = true;
                CHECK(info.score_cp <= exact->score_cp);
            } else {
                upper = true;
                CHECK(info.score_cp >= exact->score_cp);
            }
        }
        CHECK(lower && upper);
        CHECK(Search::set_hash_size_mb(1));
        SearchOptions full_window = options;
        full_window.aspiration_window_cp = 0;
        const auto reference = search.find_best_move(root, full_window, stop, [](const SearchInfo&) {});
        CHECK(reference.score_cp == result.score_cp);

        // 境界値の通知直後に止めても、未完了の反復を確定結果にしない。
        CHECK(Search::set_hash_size_mb(1));
        int interrupted_depth = 0;
        const auto interrupted = search.find_best_move(root, options, stop, [&](const SearchInfo& info) {
            if (info.score_bound != ScoreBound::Exact) {
                interrupted_depth = info.depth;
                stop.store(true);
            }
        });
        CHECK(interrupted_depth > 0);
        CHECK(interrupted.completed_depth == interrupted_depth - 1);
        stop.store(false);

        // 制限した初手の評価を、制限なしの局面全体の確定値として再利用しない。
        SearchOptions restricted = full_window;
        restricted.restrict_searchmoves = true;
        restricted.searchmoves = {"9g9f"};
        CHECK(Search::set_hash_size_mb(1));
        const auto selected = search.find_best_move(root, restricted, stop, [](const SearchInfo&) {});
        CHECK(selected.has_best_move && root.move_to_usi(selected.best_move) == "9g9f");
        const auto unrestricted = search.find_best_move(root, full_window, stop, [](const SearchInfo&) {});
        CHECK(unrestricted.score_cp == reference.score_cp);
        restricted.searchmoves.clear();
        const auto empty = search.find_best_move(root, restricted, stop, [](const SearchInfo&) {});
        CHECK(!empty.has_best_move);
    }
    return test_support::failure_count();
}
