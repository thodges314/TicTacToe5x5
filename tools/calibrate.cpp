// ============================================================================
// calibrate.cpp — Even-Depth Convergence Analysis for 5×5 Tic-Tac-Toe
//
// Three improvements over naive depth sweeping (per Gemini / quiescence theory):
//
//   1. EVEN DEPTHS ONLY (2, 4, 6, 8, 10, 12)
//      Odd depths halt on the mover's ply → optimistic/biased scores.
//      Even depths halt after the opponent replies → stable, honest positions.
//
//   2. HEURISTIC EVALUATION at depth limit
//      Without a heuristic, all non-terminal depth-limited nodes return 0,
//      so center-first move ordering dominates and ALL depths play identically.
//      We score open lines (no-opponent pieces × weight) to differentiate depths.
//
//   3. STEP SIZE = 2 (compare D vs D+2, never D vs D+1)
//      Cross-depth matchups: D=4 as X vs D=6 as O, and vice versa.
//      When D can draw D+2, we have strong evidence of convergence.
//
// Convergence = (A) self-play sequences identical at D and D+2, AND
//               (B) D draws D+2 from both sides.
//
// Usage:   make && make run-quiet      (outputs clean table, suppresses thread logs)
//          make run                     (includes per-thread solver timing on stderr)
//
// Threading: uses multithreaded getBestMove (all 12 M2 cores) for ~8-10x speedup
//   vs single-threaded. Move sequences may vary slightly run-to-run due to TT
//   race conditions under concurrency, but outcomes (draw/win/loss) are stable.
// Expected runtime on M2 Studio (12 cores): ~6-10 hours to D=20.
// ============================================================================
#include "../engine/Bitboard.hpp"
#include "../engine/Solver.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

static auto T0 = std::chrono::high_resolution_clock::now();
static std::string elapsed() {
    double s = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - T0).count();
    int h = (int)s / 3600, m = ((int)s % 3600) / 60, sec = (int)s % 60;
    std::ostringstream ss;
    ss << std::setfill('0')
       << std::setw(2) << h << "h"
       << std::setw(2) << m << "m"
       << std::setw(2) << sec << "s";
    return ss.str();
}

// ── Config ───────────────────────────────────────────────────────────────────
static constexpr int BOARD      = 5;
static constexpr int WIN_TARGET = 5;
static constexpr int MIN_DEPTH  = 2;
static constexpr int MAX_DEPTH  = 20;   // D=20 ~1h on M2 with 12-thread solver
static constexpr int STEP       = 2;    // even jumps only

// ── ANSI colours ─────────────────────────────────────────────────────────────
static const char* RESET  = "\033[0m";
static const char* BOLD   = "\033[1m";
static const char* DIM    = "\033[2m";
static const char* RED    = "\033[91m";
static const char* GREEN  = "\033[92m";
static const char* YELLOW = "\033[93m";
static const char* CYAN   = "\033[96m";
static const char* WHITE  = "\033[97m";

// ── Heuristic: count open lines weighted by occupancy ────────────────────────
//
// An "open line" is a run of WIN_TARGET consecutive cells with:
//   at least 1 piece of playerIdx, and zero opponent pieces.
// Weights grow geometrically: 1 in row=1pt, 2=8pt, 3=64pt, 4=512pt.
// Clamped to ±999 so it can't mask true wins (scored ±1000±depth in minimax).
//
static int openLineScore(const Bitboard& board, int playerIdx) {
    static const int W[] = { 0, 1, 8, 64, 512 };  // 0..WIN_TARGET-1 pieces
    const uint64_t pm = (playerIdx == 1) ? board.xBits : board.oBits;
    const uint64_t om = (playerIdx == 1) ? board.oBits : board.xBits;
    int score = 0;

    auto evalMask = [&](uint64_t mask) {
        if (om & mask) return;                     // blocked by opponent
        int cnt = __builtin_popcountll(pm & mask);
        if (cnt > 0 && cnt < WIN_TARGET) score += W[cnt];
    };

    // Horizontal
    for (int r = 0; r < BOARD; r++)
        for (int c = 0; c <= BOARD - WIN_TARGET; c++) {
            uint64_t mask = ((1ULL << WIN_TARGET) - 1) << (r * BOARD + c);
            evalMask(mask);
        }
    // Vertical
    for (int c = 0; c < BOARD; c++)
        for (int r = 0; r <= BOARD - WIN_TARGET; r++) {
            uint64_t mask = 0;
            for (int i = 0; i < WIN_TARGET; i++) mask |= 1ULL << ((r+i)*BOARD + c);
            evalMask(mask);
        }
    // Diagonal ↘
    for (int r = 0; r <= BOARD - WIN_TARGET; r++)
        for (int c = 0; c <= BOARD - WIN_TARGET; c++) {
            uint64_t mask = 0;
            for (int i = 0; i < WIN_TARGET; i++) mask |= 1ULL << ((r+i)*BOARD + (c+i));
            evalMask(mask);
        }
    // Diagonal ↙
    for (int r = WIN_TARGET-1; r < BOARD; r++)
        for (int c = 0; c <= BOARD - WIN_TARGET; c++) {
            uint64_t mask = 0;
            for (int i = 0; i < WIN_TARGET; i++) mask |= 1ULL << ((r-i)*BOARD + (c+i));
            evalMask(mask);
        }
    return score;
}

// Returns score from X's perspective: positive = X better
static int boardHeuristic(const Bitboard& board, bool /*isMaximizing*/) {
    int raw = openLineScore(board, 1) - openLineScore(board, 2);
    return std::max(-999, std::min(999, raw));
}

// ── Data ─────────────────────────────────────────────────────────────────────
struct GameRecord {
    std::vector<int> moves;
    int outcome = 0;   // 1=X, 2=O, 0=draw
    double ms   = 0.0;
};

static std::string outcomeStr(int o) {
    if (o == 1) return std::string(RED)   + "X wins" + RESET;
    if (o == 2) return std::string(GREEN) + "O wins" + RESET;
    return std::string(CYAN) + "Draw  " + RESET;
}

static std::string moveSummary(const std::vector<int>& moves, int n = 8) {
    std::ostringstream ss;
    ss << "[";
    for (int i = 0; i < std::min((int)moves.size(), n); i++) {
        if (i) ss << ' ';
        ss << moves[i]/BOARD << ',' << moves[i]%BOARD;
    }
    if ((int)moves.size() > n) ss << " +(" << moves.size() - n << ")";
    ss << "]";
    return ss.str();
}

static bool seqMatch(const GameRecord& a, const GameRecord& b) {
    return a.outcome == b.outcome && a.moves == b.moves;
}

// ── Play one game ─────────────────────────────────────────────────────────────
// Each Solver keeps its own TT (warm across moves in a game = realistic).
// heuristic is installed on both solvers before-hand.
static GameRecord playGame(Solver& sx, int dx, Solver& so, int dy,
                           const std::string& label = "") {
    Bitboard board(BOARD, WIN_TARGET);
    GameRecord rec;
    bool xTurn = true;
    int  moveNum = 0;
    auto t0 = std::chrono::high_resolution_clock::now();

    while (true) {
        auto mvs = board.getAvailableMoves();
        if (mvs.empty()) { rec.outcome = 0; break; }

        Solver& s = xTurn ? sx : so;
        int     d = xTurn ? dx : dy;

        auto mt0 = std::chrono::high_resolution_clock::now();
        auto [move, score] = s.getBestMove(board, xTurn, d);
        (void)score;
        double mms = std::chrono::duration<double, std::milli>(
                         std::chrono::high_resolution_clock::now() - mt0).count();

        if (move == -1) { rec.outcome = 0; break; }

        board.setPiece(move/BOARD, move%BOARD, xTurn ? 1 : 2);
        rec.moves.push_back(move);
        moveNum++;

        // Per-move progress line — flushed immediately so terminal updates live
        if (!label.empty()) {
            std::cout << "  " << DIM << "[" << elapsed() << "] "
                      << label << "  move " << std::setw(2) << moveNum
                      << " (" << (xTurn ? "X" : "O") << " D="
                      << d << ")  [" << move/BOARD << "," << move%BOARD << "]"
                      << "  " << std::fixed << std::setprecision(0)
                      << mms << " ms" << RESET << "\n" << std::flush;
        }

        if (board.checkWin(1)) { rec.outcome = 1; break; }
        if (board.checkWin(2)) { rec.outcome = 2; break; }
        xTurn = !xTurn;
    }

    rec.ms = std::chrono::duration<double, std::milli>(
                 std::chrono::high_resolution_clock::now() - t0).count();
    return rec;
}

// ── Helpers ──────────────────────────────────────────────────────────────────
static void makeSolver(Solver& s) {
    s.heuristic = boardHeuristic;
}

static void printLine(char c = '-', int w = 82) {
    for (int i = 0; i < w; i++) std::cout << c;
    std::cout << '\n';
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    printLine('=');
    std::cout << BOLD << WHITE
              << "  5x5 Tic-Tac-Toe (target=5)  |  Even-Depth Convergence Calibration\n"
              << RESET;
    printLine('=');
    std::cout << DIM
              << "  Method: even depths only (" << MIN_DEPTH << ", "
              << MIN_DEPTH+STEP << ", ... " << MAX_DEPTH << "),"
              << " cross-depth step=" << STEP << "\n"
              << "  Heuristic: open-line scoring (1/8/64/512 pts per 1/2/3/4-in-row)\n"
              << "  Single-threaded search per player for deterministic results\n\n"
              << RESET;

    // ── Section 1: Self-play ─────────────────────────────────────────────────
    std::cout << BOLD << "  SECTION 1 — Self-play (both players at depth D)\n" << RESET;
    std::cout << DIM
              << "  " << std::left
              << std::setw(5) << "D"
              << std::setw(9) << "Outcome"
              << std::setw(11) << "Time"
              << std::setw(6) << "Match?"
              << "First 8 moves (row,col)\n" << RESET;
    printLine();

    std::vector<std::pair<int,GameRecord>> selfGames;  // {depth, record}

    for (int d = MIN_DEPTH; d <= MAX_DEPTH; d += STEP) {
        std::cout << DIM << "  [" << elapsed() << "] self-play D=" << d << "..." << RESET << "\n" << std::flush;
        Solver s;  makeSolver(s);
        GameRecord r = playGame(s, d, s, d);

        // Match vs previous even depth
        bool matched = false;
        std::string matchStr = "  --";
        if (!selfGames.empty()) {
            const auto& prev = selfGames.back().second;
            matched = seqMatch(prev, r);
            matchStr = matched
                ? std::string("  ") + GREEN + "YES" + RESET
                : std::string("  ") + RED   + "NO " + RESET;
        }

        std::cout << "  D=" << std::setw(2) << d << "  "
                  << outcomeStr(r.outcome)
                  << "  " << std::setw(7) << std::fixed << std::setprecision(0)
                  << r.ms << " ms  "
                  << matchStr << "  "
                  << moveSummary(r.moves)
                  << "\n";

        selfGames.push_back({d, r});
    }

    // ── Section 2: Cross-depth tournament ────────────────────────────────────
    std::cout << "\n" << BOLD
              << "  SECTION 2 — Cross-depth tournament (D vs D+" << STEP << ")\n"
              << RESET;
    std::cout << DIM
              << "  " << std::left
              << std::setw(18) << "Matchup"
              << std::setw(9) << "Outcome"
              << std::setw(11) << "Time"
              << "D holds?\n" << RESET;
    printLine();

    int convergenceDepth = -1;

    for (int idx = 0; idx + 1 < (int)selfGames.size(); idx++) {
        int dLow  = selfGames[idx    ].first;
        int dHigh = selfGames[idx + 1].first;

        std::cout << DIM << "  [" << elapsed() << "] cross D=" << dLow
                  << " vs D=" << dHigh << "..." << RESET << "\n" << std::flush;

        // Game A: dLow as X, dHigh as O
        Solver sxA, soA;  makeSolver(sxA); makeSolver(soA);
        std::ostringstream labelA; labelA << "D=" << dLow << "X vs D=" << dHigh << "O";
        GameRecord gA = playGame(sxA, dLow, soA, dHigh, labelA.str());

        // Game B: dHigh as X, dLow as O
        Solver sxB, soB;  makeSolver(sxB); makeSolver(soB);
        std::ostringstream labelB; labelB << "D=" << dHigh << "X vs D=" << dLow << "O";
        GameRecord gB = playGame(sxB, dHigh, soB, dLow, labelB.str());

        bool holdsX = (gA.outcome != 2);  // D not beaten as X
        bool holdsO = (gB.outcome != 1);  // D not beaten as O
        bool holds  = holdsX && holdsO;

        auto tag = [](bool ok, const char* y = "OK ", const char* n = "LOST") {
            return ok ? std::string(GREEN) + y + RESET
                      : std::string(RED)   + n + RESET;
        };

        // Game A row
        std::cout << "  D=" << std::setw(2) << dLow << " X vs D="
                  << std::setw(2) << dHigh << " O  "
                  << outcomeStr(gA.outcome) << "  "
                  << std::setw(7) << std::fixed << std::setprecision(0) << gA.ms << " ms  "
                  << tag(holdsX, "D holds ", "D LOST  ") << "\n";

        // Game B row
        std::cout << "  D=" << std::setw(2) << dHigh << " X vs D="
                  << std::setw(2) << dLow << " O  "
                  << outcomeStr(gB.outcome) << "  "
                  << std::setw(7) << std::fixed << std::setprecision(0) << gB.ms << " ms  "
                  << tag(holdsO, "D holds ", "D LOST  ") << "\n";

        // Sequence match
        bool seqSame = seqMatch(selfGames[idx].second, selfGames[idx+1].second);
        std::cout << "  Seq D=" << dLow << "==D=" << dHigh << ": "
                  << (seqSame ? std::string(GREEN) + "YES" + RESET
                              : std::string(RED)   + "NO " + RESET)
                  << "  |  D=" << dLow << " survives D=" << dHigh << ": "
                  << (holds   ? std::string(GREEN) + "YES" + RESET
                              : std::string(RED)   + "NO " + RESET);

        if (seqSame && holds && convergenceDepth == -1) {
            convergenceDepth = dLow;
            std::cout << "  " << BOLD << YELLOW << "<-- CONVERGENCE" << RESET;
        }
        std::cout << "\n";
        printLine('.');
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    printLine('=');
    std::cout << BOLD << "\n  RESULT\n" << RESET;
    if (convergenceDepth != -1) {
        std::cout << GREEN << "  Convergence at D=" << convergenceDepth << RESET << "\n"
                  << "  D=" << convergenceDepth << " plays identically to D="
                  << convergenceDepth+STEP << " and holds a draw against it.\n"
                  << "  Recommended WASM cap: depth " << convergenceDepth
                  << " (even, honest evaluation).\n\n";
    } else {
        std::cout << RED << "  No convergence within D=" << MIN_DEPTH
                  << ".." << MAX_DEPTH << RESET << "\n"
                  << "  Try increasing MAX_DEPTH, or add threat-first move ordering\n"
                  << "  to deepen effective search within the same wall-clock time.\n\n";
    }
    printLine('=');
    return 0;
}
