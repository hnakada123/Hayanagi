# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Hayanagi is a minimal USI-protocol Shogi (Japanese chess) engine written in C++17. It implements legal move generation, terminal detection, and alpha-beta search with iterative deepening.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is output to `build/hayanagi`.

## Running

```bash
./build/hayanagi
```

The engine reads USI commands from stdin. Key commands for testing:
- `bench nodes 10000` — fixed-node benchmark
- `perft depth N divide` — perft with per-move breakdown
- `go depth N` / `go nodes N` — search to fixed depth or node count

## Architecture

All code lives in `src/` under the `shogi` namespace. The main compilation units:

- **types.h** — Core types (`Color`, `PieceType`, `Move`, `TerminalStatus`), square/piece utilities, piece values. Pieces are encoded as signed ints (positive=Black, negative=White).
- **bitboard.h** — 81-square bitboard using two `uint64_t` fields (lo: 64 bits, hi: 17 bits). Provides set operations, pop (LSB extraction), and iteration via `BitboardIterator`.
- **position.h/cpp** — Board state, SFEN parsing, USI move application, legal move generation. Uses color/piece-type bitboards for check detection, pin computation, and direct move generation (no post-filter). Maintains a linked-list history (`HistoryNode`) for repetition detection. Includes SEE, null-move generation, and entering-king (impasse) rule variants.
- **tsume.h/cpp** — Depth-first iterative-deepening mate search (`TsumeSearch`) used by ShogiBoardQ through the `hayanagi_tsume` static library. Uses `make_move`/`unmake_move` and a fixed-size transposition table that keeps mate/no-mate proofs across depths; `Limit` (no mate within depth) must never be reported as `NoMate`.
- **search.h/cpp** — Iterative-deepening negamax with alpha-beta. Features: lockless shared transposition table (`Hash` option), PVS, quiescence search, SEE-based move ordering, killer/history heuristics, LMR, null-move pruning, dedicated mate search. Multi-threaded via root move splitting (`Threads` option). Hand-crafted evaluation (material + positional terms).
- **usi_engine.h/cpp** — USI protocol handler. Runs search on a worker thread with `stop` support. Implements `bench`, `perft`, ponder, MultiPV, and all engine options.

## Key Design Details

- Board is a flat `int[81]` array, row-major (row 0 = rank 'a' = Black's promotion zone). Squares map as `square = row * 9 + col`, with file = `9 - col`.
- Move generation directly handles check evasion, double-check, and pinned-piece restrictions rather than generating all pseudo-legal moves and filtering.
- The transposition table is a global lockless hash table shared across threads, with generation-based replacement.
- Compiler flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (GCC/Clang) or `/W4` (MSVC).

## Testing

```bash
./build/hayanagi_tests                    # C++ unit tests (built only when Hayanagi is the top-level project)
python3 tests/test_engine.py build/hayanagi   # USI regression tests
python3 tests/bench_tsume.py build/hayanagi   # tsume benchmark (see BENCHMARK.md)
```

`Position::generate_legal_moves()` / `generate_checking_moves()` output order is part of the contract with ShogiBoardQ; keep it stable.

## Language

Source code, comments, and commit messages are in Japanese. README is in Japanese.
