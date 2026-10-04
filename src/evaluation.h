#pragma once

namespace shogi {

class Position;

// 手番側から見た静的評価。終局判定と確定スコアの値域への制限は探索側で行う。
int evaluate_position(const Position& position);

}  // namespace shogi
