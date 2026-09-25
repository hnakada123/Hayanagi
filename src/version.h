#pragma once

// Hayanagi のバージョン。CMakeLists.txt はこの定義を読んでプロジェクトのバージョンにする。
// 更新時は README.md の記載と合わせ、リリース時に同じ番号のタグ（例: v1.0.0）を付ける。
#define HAYANAGI_VERSION "1.0.0"

namespace shogi {

constexpr const char* kEngineName = "Hayanagi";
constexpr const char* kEngineVersion = HAYANAGI_VERSION;

}  // namespace shogi
