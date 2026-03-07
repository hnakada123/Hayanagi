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

All code lives in `src/` under the `shogi` namespace. There are four compilation units:

- **types.h** — Core types (`Color`, `PieceType`, `Move`, `TerminalStatus`), square/piece utilities, piece values. Pieces are encoded as signed ints (positive=Black, negative=White).
- **bitboard.h** — 81-square bitboard using two `uint64_t` fields (lo: 64 bits, hi: 17 bits). Provides set operations, pop (LSB extraction), and iteration via `BitboardIterator`.
- **position.h/cpp** — Board state, SFEN parsing, USI move application, legal move generation. Uses color/piece-type bitboards for check detection, pin computation, and direct move generation (no post-filter). Maintains a linked-list history (`HistoryNode`) for repetition detection. Includes SEE, null-move generation, and entering-king (impasse) rule variants.
- **search.h/cpp** — Iterative-deepening negamax with alpha-beta. Features: lockless shared transposition table (`Hash` option), PVS, quiescence search, SEE-based move ordering, killer/history heuristics, LMR, null-move pruning, dedicated mate search. Multi-threaded via root move splitting (`Threads` option). Hand-crafted evaluation (material + positional terms).
- **usi_engine.h/cpp** — USI protocol handler. Runs search on a worker thread with `stop` support. Implements `bench`, `perft`, ponder, MultiPV, and all engine options.

## Key Design Details

- Board is a flat `int[81]` array, row-major (row 0 = rank 'a' = Black's promotion zone). Squares map as `square = row * 9 + col`, with file = `9 - col`.
- Move generation directly handles check evasion, double-check, and pinned-piece restrictions rather than generating all pseudo-legal moves and filtering.
- The transposition table is a global lockless hash table shared across threads, with generation-based replacement.
- Compiler flags: `-Wall -Wextra -Wpedantic` (GCC/Clang) or `/W4` (MSVC).

## Language

Source code, comments, and commit messages are in Japanese. README is in Japanese.
