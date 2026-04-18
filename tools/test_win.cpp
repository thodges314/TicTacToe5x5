// ============================================================================
// test_win.cpp — unit tests for Bitboard::checkWin
//
// Tests all four win directions (row, column, diagonal ↘, anti-diagonal ↙)
// for both players, plus near-miss and the specific board from the bug report.
//
// Build + run: make test
// ============================================================================
#include "../engine/Bitboard.hpp"
#include <iostream>
#include <string>

static int pass_count = 0;
static int fail_count = 0;

void check(const std::string& name, bool expected, bool got) {
    if (got == expected) {
        std::cout << "  PASS  " << name << "\n";
        pass_count++;
    } else {
        std::cout << "  FAIL  " << name
                  << "  (expected " << (expected ? "win" : "no-win")
                  << ", got "       << (got      ? "win" : "no-win") << ")\n";
        fail_count++;
    }
}

// Helper: sets cells from a flat list of indices for each player.
Bitboard makeBoard(std::initializer_list<int> X, std::initializer_list<int> O) {
    Bitboard b(5, 5);
    for (int i : X) b.setPiece(i / 5, i % 5, 1);
    for (int i : O) b.setPiece(i / 5, i % 5, 2);
    return b;
}

int main() {
    std::cout << "=== Bitboard::checkWin unit tests ===\n\n";

    // ── Rows ─────────────────────────────────────────────────────────────────
    std::cout << "-- Rows --\n";
    check("X wins row 0",  true,  makeBoard({0,1,2,3,4},       {}).checkWin(1));
    check("X wins row 2",  true,  makeBoard({10,11,12,13,14},  {}).checkWin(1));
    check("X wins row 4",  true,  makeBoard({20,21,22,23,24},  {}).checkWin(1));
    check("O wins row 1",  true,  makeBoard({}, {5,6,7,8,9})      .checkWin(2));
    check("O wins row 3",  true,  makeBoard({}, {15,16,17,18,19}) .checkWin(2));
    check("X no row win (4 in row)", false,
          makeBoard({0,1,2,3}, {}).checkWin(1));

    // ── Columns ──────────────────────────────────────────────────────────────
    std::cout << "\n-- Columns --\n";
    check("X wins col 0",  true,  makeBoard({0,5,10,15,20},    {}).checkWin(1));
    check("X wins col 2",  true,  makeBoard({2,7,12,17,22},    {}).checkWin(1));
    check("X wins col 4",  true,  makeBoard({4,9,14,19,24},    {}).checkWin(1));
    check("O wins col 0",  true,  makeBoard({}, {0,5,10,15,20})   .checkWin(2));
    check("O wins col 4",  true,  makeBoard({}, {4,9,14,19,24})   .checkWin(2));
    check("X no col win (4 in col)", false,
          makeBoard({0,5,10,15}, {}).checkWin(1));

    // ── Diagonal ↘ ───────────────────────────────────────────────────────────
    std::cout << "\n-- Diagonal \u2198 --\n";
    check("X wins main diag ↘",  true,  makeBoard({0,6,12,18,24}, {}).checkWin(1));
    check("O wins main diag ↘",  true,  makeBoard({}, {0,6,12,18,24}) .checkWin(2));
    check("X no diag↘ win (4)",  false, makeBoard({0,6,12,18},    {}).checkWin(1));

    // ── Anti-diagonal ↙ ──────────────────────────────────────────────────────
    std::cout << "\n-- Anti-diagonal \u2199 (the bug we fixed) --\n";
    // The only valid anti-diagonal on a 5x5 target-5 board: (0,4)(1,3)(2,2)(3,1)(4,0)
    // flat indices: 4, 8, 12, 16, 20
    check("X wins anti-diag ↙",  true,  makeBoard({4,8,12,16,20}, {}).checkWin(1));
    check("O wins anti-diag ↙",  true,  makeBoard({}, {4,8,12,16,20}) .checkWin(2));
    check("X no anti-diag win (4)", false,
          makeBoard({4,8,12,16}, {}).checkWin(1));
    check("X no anti-diag win (wrong shape)", false,
          makeBoard({3,8,12,16,20}, {}).checkWin(1));  // shifted — not 5-in-a-row

    // ── Cross-player correctness ──────────────────────────────────────────────
    std::cout << "\n-- Cross-player: winning board for X is not a win for O --\n";
    {
        auto b = makeBoard({0,1,2,3,4}, {5,6,7,8});
        check("X row, not O win",  false, b.checkWin(2));
        check("X row, IS X win",   true,  b.checkWin(1));
    }
    {
        auto b = makeBoard({5,6,7,8}, {4,8,12,16,20});
        check("O anti-diag, not X win",  false, b.checkWin(1));
        check("O anti-diag, IS O win",   true,  b.checkWin(2));
    }

    // ── Exact board from the bug report ──────────────────────────────────────
    // Row 0: X X O O X  (0,1,4=X  2,3=O)
    // Row 1: O O O X X  (8,9=X    5,6,7=O)
    // Row 2: O X X O O  (11,12=X  10,13,14=O)
    // Row 3: X X O O X  (15,16,19=X  17,18=O)
    // Row 4: X O O X X  (20,23,24=X  21,22=O)
    // X anti-diagonal: cells 4,8,12,16,20 — should be a win
    std::cout << "\n-- Bug-report board (anti-diag was called a draw) --\n";
    {
        auto bugBoard = makeBoard(
            {0,1,4,  8,9,  11,12,  15,16,19,  20,23,24},
            {2,3,    5,6,7, 10,13,14, 17,18,   21,22}
        );
        check("Bug board: X wins  (anti-diagonal)", true,  bugBoard.checkWin(1));
        check("Bug board: O no win",                false, bugBoard.checkWin(2));
    }

    // ── Empty board ───────────────────────────────────────────────────────────
    std::cout << "\n-- Empty board --\n";
    check("Empty: X no win", false, makeBoard({}, {}).checkWin(1));
    check("Empty: O no win", false, makeBoard({}, {}).checkWin(2));

    // ── Summary ───────────────────────────────────────────────────────────────
    std::cout << "\n=== " << pass_count << " passed, " << fail_count << " failed ===\n";
    return fail_count > 0 ? 1 : 0;
}
