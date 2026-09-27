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

    // 制限を切ったときは棋力値を無視し、明示的な探索上限は制限中も守る。
    SearchOptions limits;
    limits.strength = SearchOptions::kMinStrength;
    limits.max_depth = 4;
    limits.node_limit = 100000;
    limits.apply_strength_limit();
    CHECK(limits.max_depth == 4 && limits.node_limit == 100000);
    limits.limit_strength = true;
    limits.max_depth = 1;
    limits.node_limit = 1;
    limits.apply_strength_limit();
    CHECK(limits.max_depth == 1 && limits.node_limit == 1);

    Position root;
    root.set_startpos();
    Search search;
    std::atomic_bool stop{false};
    SearchOptions strong;
    strong.max_depth = 5;
    search.find_best_move(root, strong, stop, [](const SearchInfo&) {});
    SearchOptions weak;
    weak.limit_strength = true;
    weak.strength = -9;
    // 強い探索の後でも、同じ弱い探索を繰り返しても、深い置換表を流用しない。
    const auto after_strong = search.find_best_move(root, weak, stop, [](const SearchInfo&) {});
    const auto repeated = search.find_best_move(root, weak, stop, [](const SearchInfo&) {});
    CHECK(Search::set_hash_size_mb(1));
    const auto fresh = search.find_best_move(root, weak, stop, [](const SearchInfo&) {});
    CHECK(fresh.has_best_move);
    for (const auto& result : {after_strong, repeated}) {
        CHECK(result.score_cp == fresh.score_cp);
        CHECK(result.pv == fresh.pv);
        CHECK(result.completed_depth == fresh.completed_depth);
        CHECK(result.nodes == fresh.nodes);
    }
    // 制限中の共有置換表からも複数手の合法な読み筋を再構成できる。
    CHECK(fresh.completed_depth >= 2);
    CHECK(fresh.pv.find(' ') != std::string::npos);

    for (const int threads : {1, 4}) {
        weak.threads = threads;
        weak.strength = -15;
        weak.multi_pv = 2;
        weak.restrict_searchmoves = true;
        weak.searchmoves = {"7g7f", "2g2f"};
        const auto limited = search.find_best_move(root, weak, stop, [&](const SearchInfo& info) {
            CHECK(info.depth <= 1);
            CHECK(info.nodes <= 256 + static_cast<std::uint64_t>(threads));
        });
        CHECK(limited.has_best_move);
        CHECK(limited.completed_depth == 1);
        const auto move = root.move_to_usi(limited.best_move);
        CHECK(move == "7g7f" || move == "2g2f");

        // 時間指定があっても、5手詰の事前確認で棋力制限を迂回しない。
        Position mate;
        CHECK(mate.set_sfen("9/9/6R1+R/5k3/9/7+S1/9/9/4K4 b 2b4g3s4n4l18p 1"));
        weak.restrict_searchmoves = false;
        weak.time_limit_ms = 5000;
        weak.show_refutations = true;
        const auto limited_mate = search.find_best_move(mate, weak, stop, [&](const SearchInfo& info) {
            CHECK(info.depth <= 1);
            CHECK(info.nodes <= 256 + static_cast<std::uint64_t>(threads));
        });
        CHECK(limited_mate.has_best_move);
        CHECK(limited_mate.completed_depth <= 1);
    }
    return test_support::failure_count();
}
