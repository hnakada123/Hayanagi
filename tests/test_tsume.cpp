// TsumeSearch の契約（Limit と NoMate の区別、最短手数、stop、打ち歩詰め、玉なし局面など）を検証する。

#include "position.h"
#include "test_support.h"
#include "tsume.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace shogi;

namespace {

struct Problem {
    const char* sfen;
    const char* solution;  // 5 手詰の手順
};

constexpr Problem kProblems[] = {
    {"9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1", "3c5c+ 4d4e 1c4c P*4d 4c4d"},
    {"9/9/9/R8/9/9/1k2+S4/9/3+N5 b RG2b3g3s3n4l18p 1", "G*9g 8g8h R*8g 8h9i 9g9h"},
    {"9/9/9/9/9/9/7+S1/5G2k/9 b RSr2b3g2s4n4l18p 1", "R*1g 1h2i S*3h 2i3i 4h4i"},
    {"5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1", "S*3b 4a4b R*4a 4b5b 4d5c+"},
    {"1l7/k8/9/G8/3+R5/9/9/9/9 b R2b3g4s4n3l18p 1", "R*9c 9b8b 6e6b P*7b 9d8c"},
};

// 攻方の持駒が多く、深さ 5 以内には詰まない局面
constexpr const char* kNoMateWithinFive = "9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1";

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    for (const char ch : text) {
        if (ch == ' ') {
            if (!token.empty()) tokens.push_back(token);
            token.clear();
        } else {
            token += ch;
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

Position load(const std::string& sfen, const std::string& moves = "") {
    Position position;
    CHECK_MSG(position.set_sfen(sfen, true), sfen);
    for (const std::string& move : split(moves)) {
        CHECK_MSG(position.apply_usi_move(move), sfen + " " + move);
    }
    return position;
}

TsumeResult solve(const Position& position, Color attacker, int depth, int millis = 5000) {
    std::atomic_bool stop{false};
    TsumeSearch solver;
    return solver.solve(position, attacker, depth, millis, stop);
}

std::string usi(const Position& position, const TsumeResult& result) {
    return result.move.is_valid() ? position.move_to_usi(result.move) : "none";
}

void test_five_problems() {
    for (const Problem& problem : kProblems) {
        const Position position = load(problem.sfen);
        const TsumeResult result = solve(position, Color::Black, 5);
        CHECK_MSG(result.status == TsumeStatus::Mate, problem.sfen);
        CHECK_MSG(result.plies == 5, problem.sfen);
        const auto solution = split(problem.solution);
        CHECK_MSG(usi(position, result) == solution[0], problem.sfen + std::string(" ") + usi(position, result));
        // 3 手詰までは深さ 3 で見つからない（最短手数の保証）
        CHECK_MSG(solve(position, Color::Black, 3).status == TsumeStatus::Limit, problem.sfen);

        // 玉方手番: 残り手数ちょうどで詰み、最終局面では手がない
        for (int count = 1; count <= 5; count += 2) {
            std::string played;
            for (int i = 0; i < count; ++i) played += solution[static_cast<std::size_t>(i)] + " ";
            const Position after = load(problem.sfen, played);
            const TsumeResult reply = solve(after, Color::Black, 5 - count);
            CHECK_MSG(reply.status == TsumeStatus::Mate, problem.sfen + std::string(" ") + played);
            CHECK_MSG(reply.plies <= 5 - count, problem.sfen + std::string(" ") + played);
            if (count == 5) CHECK_MSG(!reply.move.is_valid(), played);
        }
    }
}

// ShogiBoardQ の解析と同じ手順で PV を再構成できること（各局面で plies が残り手数と一致する）
void test_pv_reconstruction_with_shared_solver() {
    for (const Problem& problem : kProblems) {
        std::atomic_bool stop{false};
        TsumeSearch solver;
        Position position = load(problem.sfen);
        TsumeResult result = solver.solve(position, Color::Black, 31, 5000, stop);
        CHECK_MSG(result.status == TsumeStatus::Mate && result.plies == 5, problem.sfen);
        std::string pv;
        for (int remaining = 5; remaining > 0; --remaining) {
            CHECK_MSG(result.status == TsumeStatus::Mate, problem.sfen);
            CHECK_MSG(result.plies == remaining, problem.sfen + std::string(" remaining ") +
                                                     std::to_string(remaining) + " got " +
                                                     std::to_string(result.plies));
            if (!result.move.is_valid()) break;
            pv += position.move_to_usi(result.move) + " ";
            position.do_move(result.move);
            if (remaining > 1) {
                result = solver.solve(position, Color::Black, remaining - 1, 5000, stop);
            }
        }
        CHECK_MSG(position.side_to_move() == Color::White && position.is_in_check(Color::White) &&
                      position.generate_legal_moves().empty(),
                  problem.sfen + std::string(" pv ") + pv);
    }
}

void test_limit_is_not_nomate() {
    // S*4b は 7 手詰なので深さ 4 では打ち切り、深さ 6 で詰み
    const Position after = load(kProblems[3].sfen, "S*4b");
    TsumeResult result = solve(after, Color::Black, 4);
    CHECK(result.status == TsumeStatus::Limit);
    CHECK(result.move.is_valid());
    result = solve(after, Color::Black, 6);
    CHECK(result.status == TsumeStatus::Mate);
    CHECK(result.plies == 6);
    // 最長抵抗の応手の後は残り 5 手ちょうどで詰む
    Position next = after;
    next.do_move(result.move);
    const TsumeResult follow = solve(next, Color::Black, 5);
    CHECK(follow.status == TsumeStatus::Mate);
    CHECK(follow.plies == 5);

    // 攻方の持駒が多い不詰局面: 深さ内に詰みがないだけで不詰の証明ではない
    const Position many = load(kNoMateWithinFive);
    CHECK(solve(many, Color::Black, 3).status == TsumeStatus::Limit);
    CHECK(solve(many, Color::Black, 5).status == TsumeStatus::Limit);

    // 王手が尽きる局面は不詰として確定し、深さ 1 では確定しない
    const Position gold_only = load("k8/9/9/9/9/9/9/9/9 b G 1");
    CHECK(solve(gold_only, Color::Black, 1).status == TsumeStatus::Limit);
    CHECK(solve(gold_only, Color::Black, 3).status == TsumeStatus::NoMate);
    CHECK(solve(gold_only, Color::Black, 31).status == TsumeStatus::NoMate);
}

void test_alternative_and_escape() {
    const Position alternative = load(kProblems[1].sfen, "R*9g");
    TsumeResult result = solve(alternative, Color::Black, 4);
    CHECK(result.status == TsumeStatus::Mate);
    CHECK(result.plies == 4);

    const Position escape = load(kProblems[2].sfen, "R*1i");
    result = solve(escape, Color::Black, 4);
    CHECK(result.status == TsumeStatus::NoMate);
    CHECK_MSG(usi(escape, result) == "1h1i", usi(escape, result));
    // 逃れの手の後は攻方に詰みがない
    Position next = escape;
    next.do_move(result.move);
    CHECK(solve(next, Color::Black, 3).status == TsumeStatus::NoMate);
}

void test_pawn_drop_mate_and_white_attacker() {
    Position position = load("8k/6G2/7G1/9/9/9/9/9/9 b PR 1");
    CHECK(!position.apply_usi_move("P*1b"));  // 打ち歩詰め
    TsumeResult result = solve(position, Color::Black, 1);
    CHECK(result.status == TsumeStatus::Mate);
    CHECK(result.plies == 1);
    // 詰む手は複数あるが（3b2b と R*1b）、打ち歩詰めの P*1b は選ばれない
    CHECK_MSG(usi(position, result) != "P*1b", usi(position, result));
    Position mated = position;
    CHECK(mated.do_move(result.move) && mated.generate_legal_moves().empty());

    // 後手が攻方で、先手玉がある局面
    const Position white = load("9/9/9/9/9/9/1g7/2g6/K8 w r 1");
    result = solve(white, Color::White, 1);
    CHECK(result.status == TsumeStatus::Mate);
    CHECK(result.plies == 1);
    CHECK(result.move.is_valid());

    // 攻方に玉があり、玉方の応手で攻方玉に王手がかかる場合も扱える
    const Position both_kings = load("4k4/9/9/9/9/9/9/4r4/4K4 b R 1");
    result = solve(both_kings, Color::Black, 3);
    CHECK(result.status != TsumeStatus::Timeout && result.status != TsumeStatus::Cancelled);
}

void test_stop_and_timeout() {
    const Position position = load(kProblems[0].sfen, "3c4c");
    std::atomic_bool stop{true};
    TsumeSearch solver;
    TsumeResult result = solver.solve(position, Color::Black, 30, 5000, stop);
    CHECK(result.status == TsumeStatus::Cancelled);

    stop.store(false);
    result = solver.solve(position, Color::Black, 30, 1, stop);
    CHECK(result.status == TsumeStatus::Timeout);

    // 別スレッドからの stop に速やかに応答する
    std::thread stopper([&stop]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    result = solver.solve(position, Color::Black, 30, 60000, stop);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();
    CHECK(result.status == TsumeStatus::Cancelled);
    CHECK(elapsed < std::chrono::seconds(2));

    // 中断後も同じインスタンスで正しく解ける
    stop.store(false);
    result = solver.solve(load(kProblems[0].sfen), Color::Black, 5, 5000, stop);
    CHECK(result.status == TsumeStatus::Mate && result.plies == 5);
}

void test_solver_reuse_is_consistent() {
    std::atomic_bool stop{false};
    TsumeSearch solver;
    // 同じ局面を続けて解いても結果が変わらない
    const Position position = load(kProblems[2].sfen);
    const TsumeResult first = solver.solve(position, Color::Black, 5, 5000, stop);
    const TsumeResult second = solver.solve(position, Color::Black, 5, 5000, stop);
    CHECK(first.status == second.status && first.plies == second.plies);
    CHECK(usi(position, first) == usi(position, second));

    // 攻方を入れ替えても証明が混ざらない
    const Position white = load("9/9/9/9/9/9/1g7/2g6/K8 w r 1");
    CHECK(solver.solve(white, Color::White, 1, 5000, stop).status == TsumeStatus::Mate);
    CHECK(solver.solve(position, Color::Black, 5, 5000, stop).plies == 5);
    // 先手が攻方として同じ局面（後手番）を見ると王手がかかっていないので不詰
    CHECK(solver.solve(white, Color::Black, 4, 5000, stop).status == TsumeStatus::NoMate);

    // 詰む初手の数え上げ（選別処理と同じ使い方）
    int mating = 0;
    for (const Move& move : position.generate_checking_moves()) {
        Position child = position;
        child.do_move(move);
        if (solver.solve(child, Color::Black, 4, 5000, stop).status == TsumeStatus::Mate) ++mating;
    }
    CHECK_MSG(mating == 1, std::to_string(mating));
}

void test_depth_zero_and_clamping() {
    const Position position = load(kProblems[0].sfen);
    // 攻方手番で深さ 0 は探索せず打ち切り
    const TsumeResult zero = solve(position, Color::Black, 0);
    CHECK(zero.status == TsumeStatus::Limit && zero.nodes == 0);
    // 63 を超える深さは丸められ、結果は同じ
    CHECK(solve(position, Color::Black, 1000).plies == 5);

    // 玉方手番で深さ 0: 詰んでいれば Mate、手があれば Limit
    const Position mated = load(kProblems[0].sfen, "3c5c+ 4d4e 1c4c P*4d 4c4d");
    CHECK(solve(mated, Color::Black, 0).status == TsumeStatus::Mate);
    const Position alive = load(kProblems[0].sfen, "3c5c+");
    CHECK(solve(alive, Color::Black, 0).status == TsumeStatus::Limit);
}

}  // namespace

int run_tsume_tests() {
    test_five_problems();
    test_pv_reconstruction_with_shared_solver();
    test_limit_is_not_nomate();
    test_alternative_and_escape();
    test_pawn_drop_mate_and_white_attacker();
    test_stop_and_timeout();
    test_solver_reuse_is_consistent();
    test_depth_zero_and_clamping();
    return test_support::failure_count();
}
