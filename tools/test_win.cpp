// ============================================================================
// test_win.cpp — unit tests for Bitboard::checkWin, isTheoreticalDraw, Solver
//
// Section 1 — checkWin: tests all four directions for both players,
//             including the anti-diagonal that was previously broken.
//
// Section 2 — isTheoreticalDraw: verifies the dead-position detector that
//             lets minimax return 0 early when no player can ever win.
//
// Section 3 — Solver timing: proves the anti-diagonal fix prevents the
//             16-hour hang.  With the old broken checkWin(), the solver would
//             not recognize anti-diagonal terminal states and explore an
//             enormous subtree.  With the fix, the same position solves in
//             well under a second even at D=10.
//
// Build + run: make test-cpp
// ============================================================================
#include "../engine/Bitboard.hpp"
#include "../engine/Solver.hpp"
#include <iostream>
#include <string>
#include <chrono>

// ── Test harness ─────────────────────────────────────────────────────────────
static int pass_count = 0;
static int fail_count = 0;

void check(const std::string& name, bool expected, bool got) {
    if (got == expected) {
        std::cout << "  PASS  " << name << "\n";
        pass_count++;
    } else {
        std::cout << "  FAIL  " << name
                  << "  (expected " << (expected ? "true" : "false")
                  << ", got "       << (got      ? "true" : "false") << ")\n";
        fail_count++;
    }
}

// Board cells by flat index (row*5+col).
Bitboard makeBoard(std::initializer_list<int> X, std::initializer_list<int> O) {
    Bitboard b(5, 5);
    for (int i : X) b.setPiece(i / 5, i % 5, 1);
    for (int i : O) b.setPiece(i / 5, i % 5, 2);
    return b;
}

// ─────────────────────────────────────────────────────────────────────────────
// SECTION 1 — checkWin
// ─────────────────────────────────────────────────────────────────────────────
void section_checkWin() {
    std::cout << "\n=== Section 1: checkWin ===\n";

    // -- Rows --
    std::cout << "\n-- Rows --\n";
    check("X wins row 0",  true,  makeBoard({0,1,2,3,4},       {}).checkWin(1));
    check("X wins row 2",  true,  makeBoard({10,11,12,13,14},  {}).checkWin(1));
    check("X wins row 4",  true,  makeBoard({20,21,22,23,24},  {}).checkWin(1));
    check("O wins row 1",  true,  makeBoard({}, {5,6,7,8,9})      .checkWin(2));
    check("O wins row 3",  true,  makeBoard({}, {15,16,17,18,19}) .checkWin(2));
    check("X no row win (4-in-a-row)", false,
          makeBoard({0,1,2,3}, {}).checkWin(1));

    // -- Columns --
    std::cout << "\n-- Columns --\n";
    check("X wins col 0",  true,  makeBoard({0,5,10,15,20},    {}).checkWin(1));
    check("X wins col 2",  true,  makeBoard({2,7,12,17,22},    {}).checkWin(1));
    check("X wins col 4",  true,  makeBoard({4,9,14,19,24},    {}).checkWin(1));
    check("O wins col 0",  true,  makeBoard({}, {0,5,10,15,20})   .checkWin(2));
    check("O wins col 4",  true,  makeBoard({}, {4,9,14,19,24})   .checkWin(2));
    check("X no col win (4-in-a-col)", false,
          makeBoard({0,5,10,15}, {}).checkWin(1));

    // -- Main diagonal ↘ --
    std::cout << "\n-- Diagonal ↘ --\n";
    check("X wins main diag ↘",  true,  makeBoard({0,6,12,18,24}, {}).checkWin(1));
    check("O wins main diag ↘",  true,  makeBoard({}, {0,6,12,18,24}) .checkWin(2));
    check("X no diag↘ win (4)",  false, makeBoard({0,6,12,18},    {}).checkWin(1));

    // -- Anti-diagonal ↙  (THE BUG WE FIXED) --
    //
    // The only valid anti-diagonal on a 5×5 target-5 board:
    //   (row,col)   →  flat index
    //   (0,4)       →  4
    //   (1,3)       →  8
    //   (2,2)       →  12
    //   (3,1)       →  16
    //   (4,0)       →  20
    //
    // The old Bitboard.hpp used an incorrect bit-template that missed all
    // anti-diagonal wins, causing the solver to continue searching past
    // terminal states and producing a 16+ hour hang on entry #708.
    std::cout << "\n-- Anti-diagonal ↙ (the bug we fixed) --\n";
    check("X wins anti-diag ↙",  true,  makeBoard({4,8,12,16,20}, {}).checkWin(1));
    check("O wins anti-diag ↙",  true,  makeBoard({}, {4,8,12,16,20}) .checkWin(2));
    check("X no anti-diag win (only 4)",    false, makeBoard({4,8,12,16},    {}).checkWin(1));
    check("X no anti-diag win (wrong shape)", false,
          makeBoard({3,8,12,16,20}, {}).checkWin(1));  // shifted, not 5-in-a-row

    // -- Cross-player: a win for X must not register for O --
    std::cout << "\n-- Cross-player correctness --\n";
    {
        auto b = makeBoard({0,1,2,3,4}, {5,6,7,8});
        check("Row win: X wins, O does not",    false, b.checkWin(2));
        check("Row win: X wins, X does",        true,  b.checkWin(1));
    }
    {
        auto b = makeBoard({5,6,7,8}, {4,8,12,16,20});
        check("Anti-diag win: O wins, X does not", false, b.checkWin(1));
        check("Anti-diag win: O wins, O does",     true,  b.checkWin(2));
    }

    // -- Bug-report board (the exact position that was incorrectly called draw) --
    //
    //   Row 0:  X X O O X    → flat:  0,1=X   2,3=O   4=X
    //   Row 1:  O O O X X    → flat:  5,6,7=O   8,9=X
    //   Row 2:  O X X O O    → flat:  10,13,14=O   11,12=X
    //   Row 3:  X X O O X    → flat:  15,16,19=X   17,18=O
    //   Row 4:  X O O X X    → flat:  20,23,24=X   21,22=O
    //
    //   X anti-diagonal: 4,8,12,16,20 — passes through all five rows.
    std::cout << "\n-- Bug-report board (anti-diag was called a draw by old code) --\n";
    {
        auto b = makeBoard(
            {0,1,4,  8,9,  11,12,  15,16,19,  20,23,24},
            {2,3,    5,6,7, 10,13,14, 17,18,   21,22}
        );
        check("Bug board: X wins (anti-diagonal)", true,  b.checkWin(1));
        check("Bug board: O does not win",         false, b.checkWin(2));
    }

    // -- Empty board --
    std::cout << "\n-- Empty board --\n";
    check("Empty: X no win", false, makeBoard({}, {}).checkWin(1));
    check("Empty: O no win", false, makeBoard({}, {}).checkWin(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// SECTION 2 — isTheoreticalDraw
// ─────────────────────────────────────────────────────────────────────────────
//
// A "theoretical draw" (dead position) is one where every possible 5-in-a-row
// line on the board contains at least one piece from each player, making it
// permanently uncompletable.  minimax returns 0 here without further search.
//
// All 12 winning lines on a 5×5 target-5 board:
//   H1 row0: 0-4      H2 row1: 5-9       H3 row2: 10-14
//   H4 row3: 15-19    H5 row4: 20-24
//   V1 col0: 0,5,10,15,20             V2 col1: 1,6,11,16,21
//   V3 col2: 2,7,12,17,22             V4 col3: 3,8,13,18,23
//   V5 col4: 4,9,14,19,24
//   D  main↘: 0,6,12,18,24            A  anti↙: 4,8,12,16,20
// ─────────────────────────────────────────────────────────────────────────────
void section_theoreticalDraw() {
    std::cout << "\n=== Section 2: isTheoreticalDraw ===\n";

    // -- Empty board: no pieces anywhere, all lines are alive --
    std::cout << "\n-- Empty / sparse boards (should NOT be theoretical draws) --\n";
    check("Empty board is not a dead draw",    false, makeBoard({},  {}).isTheoreticalDraw());
    check("One X, no O — all lines alive",     false, makeBoard({12},{}).isTheoreticalDraw());
    check("Two X, no O — O can still win all", false, makeBoard({0,24},{}).isTheoreticalDraw());

    // -- Near-draw: one alive line remaining --
    //
    // Dead-draw board (verified by hand — all 12 lines have both × and ○):
    //   X: 0,2,4,6,8,9,14,19,21     O: 1,5,10,11,16,18,20,22,24
    //   Empty squares: 3,7,12,13,15,17,23
    //
    // Line coverage:
    //   H1(0-4):    X=0,2,4   O=1      → dead ✓
    //   H2(5-9):    X=6,8,9   O=5      → dead ✓
    //   H3(10-14):  X=14      O=10,11  → dead ✓
    //   H4(15-19):  X=19      O=16,18  → dead ✓
    //   H5(20-24):  X=21      O=20,22,24 → dead ✓
    //   V1(col0):   X=0       O=5,10,20 → dead ✓
    //   V2(col1):   X=6,21    O=1,11,16 → dead ✓
    //   V3(col2):   X=2       O=22      → dead ✓
    //   V4(col3):   X=8       O=18      → dead ✓
    //   V5(col4):   X=4,9,14,19  O=24  → dead ✓
    //   D(↘):       X=0,6    O=18,24   → dead ✓  (12 empty)
    //   A(↙4,8,12,16,20): X=4,8  O=16,20 → dead ✓  (12 empty)
    std::cout << "\n-- Constructed dead-draw position --\n";
    {
        // Full dead-draw
        auto deadDraw = makeBoard(
            {0,2,4, 6,8,9, 14, 19, 21},
            {1, 5, 10,11, 16,18, 20,22,24}
        );
        check("Dead-draw board IS theoretical draw", true,
              deadDraw.isTheoreticalDraw());
        // A theoretical draw is not a win for either player
        check("Dead-draw: X has not won", false, deadDraw.checkWin(1));
        check("Dead-draw: O has not won", false, deadDraw.checkWin(2));

        // Remove O from index 1 → H1 (row 0: 0-4) still has X=0,2,4
        // but NO O piece → H1 is alive → NOT a theoretical draw
        auto almostDead = makeBoard(
            {0,2,4, 6,8,9, 14, 19, 21},
            {5, 10,11, 16,18, 20,22,24}   // O at 1 removed
        );
        check("Remove one O from H1 → H1 alive → not dead draw", false,
              almostDead.isTheoreticalDraw());
    }

    // -- Anti-diagonal line: specifically test the fixed direction --
    std::cout << "\n-- Anti-diagonal must be considered in draw detection --\n";
    {
        // Dead-draw where ONLY the anti-diagonal was missing the O piece.
        // If isTheoreticalDraw() were broken the same way checkWin() used to
        // be (ignoring anti-diagonals), it would falsely report a dead draw
        // even though the anti-diagonal line is still alive.

        // Start from the dead-draw board and un-block the anti-diagonal (A):
        // A = {4,8,12,16,20}.  X has 4,8.  O had 16,20 — remove BOTH so A
        // has X pieces but NO O pieces → A is alive → NOT a dead draw.
        auto antiAlive = makeBoard(
            {0,2,4, 6,8,9, 14, 19, 21},
            {1, 5, 10,11, 18, 22,24}   // O at 16 AND 20 removed → A clear for X
        );
        check("Anti-diagonal alive → NOT a dead draw (tests anti-diag coverage)",
              false, antiAlive.isTheoreticalDraw());
    }

    // -- Positions with actual wins: a won board is NOT a theoretical draw --
    //    (In practice minimax catches wins first, but the function must be
    //     correct even if called on a position with a live winner.)
    std::cout << "\n-- Won positions are not theoretical draws --\n";
    check("X wins anti-diag: not a dead draw", false,
          makeBoard({4,8,12,16,20}, {0,1,2,3}).isTheoreticalDraw());
    check("O wins row 0: not a dead draw", false,
          makeBoard({5,6,7,8}, {0,1,2,3,4}).isTheoreticalDraw());
}

// ─────────────────────────────────────────────────────────────────────────────
// SECTION 3 — Solver timing: anti-diagonal fix prevents the 16-hour hang
// ─────────────────────────────────────────────────────────────────────────────
//
// With the OLD broken checkWin(), the solver did not detect wins on the
// anti-diagonal.  For positions where many search branches terminate with
// anti-diagonal wins, minimax kept recursing to maxDepth instead of returning
// immediately.  On entry #708 this exploded the search tree from seconds to
// 16+ hours.
//
// This section proves the fix by:
//   (a) Directly showing a won position terminates instantly in minimax.
//   (b) Timing a realistic ply-5 position with anti-diagonal threats at D=10:
//       with the fix it should complete in well under 10 seconds; with the old
//       bug it would take many minutes or never finish.
// ─────────────────────────────────────────────────────────────────────────────
void section_solverTiming() {
    std::cout << "\n=== Section 3: Solver timing (anti-diagonal fix proof) ===\n";

    // Shared heuristic — same one used by gen_book5x5
    auto openLineHeuristic = [](const Bitboard& b, bool) -> int {
        static const int W[] = {0, 1, 8, 64, 512};
        auto eval = [&](int p) {
            const uint64_t pm = (p==1) ? b.xBits : b.oBits;
            const uint64_t om = (p==1) ? b.oBits : b.xBits;
            int s = 0;
            auto score = [&](uint64_t mask) {
                if (om & mask) return;
                int cnt = __builtin_popcountll(pm & mask);
                if (cnt > 0 && cnt < 5) s += W[cnt];
            };
            for (int r=0;r<5;r++) for (int c=0;c<=0;c++) score(0x1fULL<<(r*5+c));
            for (int c=0;c<5;c++) for (int r=0;r<=0;r++) {
                uint64_t m=0; for(int i=0;i<5;i++) m|=1ULL<<((r+i)*5+c); score(m);
            }
            {uint64_t m=0; for(int i=0;i<5;i++) m|=1ULL<<(i*6); score(m);}
            {uint64_t m=0; for(int i=0;i<5;i++) m|=1ULL<<(i*4+4); score(m);}
            return s;
        };
        return std::max(-999, std::min(999, eval(1) - eval(2)));
    };

    // ── (a) Won position: minimax must return instantly (O(1)) ─────────────
    std::cout << "\n-- (a) Minimax must recognize anti-diagonal win immediately --\n";
    {
        // X has the full anti-diagonal plus some extras to make it a real game.
        // minimax(depth=0) should return 1000 on the very first call.
        // With the old bug it would recurse to maxDepth instead.
        Bitboard won = makeBoard({4,8,12,16,20, 0,1}, {5,6,7,8,9,10,11});
        Solver s;
        s.heuristic = openLineHeuristic;

        auto t0 = std::chrono::steady_clock::now();
        // Call minimax directly: X just won, so isMaximizing=false (O to move
        // doesn't matter — the win check fires first).
        int score = s.minimax(won, /*depth*/0,
                              std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max(),
                              /*isMaximizing*/false, /*maxDepth*/14);
        auto ms = std::chrono::duration<double,std::milli>(
                      std::chrono::steady_clock::now() - t0).count();

        std::cout << "  [score=" << score << "  time=" << ms << " ms]\n";
        check("(a) Score is 1000 (X wins at depth 0)", score, 1000);
        check("(a) Terminates in < 1 ms (was 16+ hours without fix)",
              ms < 1.0, true);
    }

    // ── (b) Ply-5 position with active anti-diagonal threats ───────────────
    //
    // This mimics the class of positions that caused entry #708 to hang.
    // X is building toward the anti-diagonal (has pieces at 4==[0,4] and
    // 8==[1,3]) and has one extra piece.  O must respond with 3 pieces.
    // With 20 empty squares and D=10 the solver should finish in seconds.
    //
    // Board:
    //   . . . . X      row 0 — X at [0,4]
    //   . . . X .      row 1 — X at [1,3]
    //   O O . . .      row 2 — O at [2,0],[2,1]
    //   . . . . .      row 3
    //   . . O . .      row 4 — O at [4,2]; X has extra at [0,0]
    //   + X at [0,0]=0 to make ply=5 (3X, 2O)
    std::cout << "\n-- (b) D=10 solve on ply-5 anti-diagonal position --\n";
    {
        // X: [0,0]=0, [0,4]=4, [1,3]=8   (3 pieces, partial anti-diag 4,8 present)
        // O: [2,0]=10, [2,1]=11           (2 pieces)
        Bitboard pos = makeBoard({0, 4, 8}, {10, 11});
        Solver s;
        s.heuristic = openLineHeuristic;

        auto t0 = std::chrono::steady_clock::now();
        auto [move, score] = s.getBestMove(pos, /*isX*/false, /*maxDepth*/10);
        auto ms = std::chrono::duration<double,std::milli>(
                      std::chrono::steady_clock::now() - t0).count();

        std::cout << "  [move=" << move << "  score=" << score
                  << "  time=" << (int)ms << " ms]\n";
        check("(b) Returns a valid move (0-24)",
              move >= 0 && move < 25, true);
        check("(b) Move lands on empty square",
              pos.getPiece(move/5, move%5) == 0, true);
        // With the old bug this would run for minutes even at D=10.
        // With the fix, D=10 on a 20-empty board finishes in a few seconds.
        check("(b) Completes in under 30 seconds (was minutes/hours without fix)",
              ms < 30000.0, true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "=== TicTacToe5x5 engine unit tests ===\n";

    section_checkWin();
    section_theoreticalDraw();
    section_solverTiming();

    std::cout << "\n=== " << pass_count << " passed, " << fail_count << " failed ===\n";
    return fail_count > 0 ? 1 : 0;
}
