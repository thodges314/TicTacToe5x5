// ============================================================================
// gen_book5x5.cpp — Exhaustive D=12 Opening Book Generator for 5×5 Tic-Tac-Toe
//
// Precomputes the computer's optimal move for every board state reachable
// within the first BOOK_PLIES plies, for both computer-as-X and computer-as-O.
//
// Key design:
//  - Book key  : canonical board string (25 chars, each '0'/'1'/'2')
//                = lexicographic minimum over all 8 D₈ symmetry transforms
//  - Book value: best move flat index in CANONICAL board coordinates
//  - In JS: look up canonical key, then apply inverse D₈ transform to get
//           the actual move in real board coordinates
//
// D₈ inverse table: T^{-1} of each transform sym:
//   sym:  0  1  2  3  4  5  6  7
//   inv:  0  3  2  1  4  5  6  7
// (Reflections are self-inverse; 90°CW and 270°CW are mutual inverses)
//
// Usage: ./tools/gen_book5x5 > public/opening_book5x5.json 2>gen_book5x5.log
//        Takes ~3-5 hours on M2 Studio with 12-thread solver.
// ============================================================================
#include "../engine/Bitboard.hpp"
#include "../engine/Solver.hpp"
#include <iostream>
#include <map>
#include <string>
#include <chrono>
#include <iomanip>

static constexpr int BOARD      = 5;
static constexpr int WIN_TARGET = 5;
static constexpr int SEARCH_DEPTH = 12;  // calibrated convergence depth
static constexpr int BOOK_PLIES = 6;     // cover first 3 moves per side

// Inverse of each D₈ transform: INV[sym] = sym^{-1}
static constexpr int INV[8] = {0, 3, 2, 1, 4, 5, 6, 7};

// ── Canonicalize ─────────────────────────────────────────────────────────────
// Returns {canonical_string, which_sym_was_applied}.
// The canonical string is the lex-minimum board reading over all 8 views.
//
// Derivation: viewing board through transform sym means reading each output
// position (r,c) from the original position T^{-1}(r,c).
// So for each (r,c) in row-major order: str[r*sz+c] = board[T^{-1}(r,c)]
struct Canon { std::string str; int sym; };

static Canon canonicalize(const Bitboard& board) {
    Canon best{"", 0};
    for (int sym = 0; sym < 8; sym++) {
        const int invSym = INV[sym];
        std::string s;
        s.reserve(BOARD * BOARD);
        for (int r = 0; r < BOARD; r++) {
            for (int c = 0; c < BOARD; c++) {
                int orig = Bitboard::applySymmetry(invSym, r, c, BOARD);
                s += (char)('0' + board.getPiece(orig / BOARD, orig % BOARD));
            }
        }
        if (best.str.empty() || s < best.str) { best.str = std::move(s); best.sym = sym; }
    }
    return best;
}

// ── Recursive book builder ────────────────────────────────────────────────────
// Traverses the game tree to depth BOOK_PLIES.
// At each computer turn: compute D=SEARCH_DEPTH best move, store in book.
// At each human turn: recurse over ALL available human moves.
static void buildBook(Bitboard board, int ply, bool compIsX, bool isXTurn,
                      Solver& solver, std::map<std::string, int>& book) {
    if (ply >= BOOK_PLIES) return;

    const bool compTurn = (isXTurn == compIsX);

    if (compTurn) {
        Canon c = canonicalize(board);
        if (book.count(c.str)) return;  // already computed (canonical duplicate)

        auto t0 = std::chrono::high_resolution_clock::now();
        auto [move, score] = solver.getBestMove(board, isXTurn, SEARCH_DEPTH);
        (void)score;
        auto ms = std::chrono::duration<double,std::milli>(
                      std::chrono::high_resolution_clock::now() - t0).count();

        // Transform actual move to canonical board coordinates
        int mr = move / BOARD, mc = move % BOARD;
        int canonFlat = Bitboard::applySymmetry(c.sym, mr, mc, BOARD);
        book[c.str] = canonFlat;

        std::cerr << "  ply=" << ply
                  << "  key=" << c.str.substr(0,12) << "..."
                  << "  sym=" << c.sym
                  << "  move=[" << mr << "," << mc << "]"
                  << "  canonMove=" << canonFlat
                  << "  total_book=" << book.size()
                  << "  time=" << std::fixed << std::setprecision(0) << ms << "ms\n";

        // Make the computer's move and recurse for human responses
        Bitboard next = board;
        next.setPiece(mr, mc, isXTurn ? 1 : 2);
        if (!next.checkWin(1) && !next.checkWin(2))
            buildBook(next, ply + 1, compIsX, !isXTurn, solver, book);

    } else {
        // Human's turn: try every possible move
        for (int m : board.getAvailableMoves()) {
            Bitboard next = board;
            next.setPiece(m / BOARD, m % BOARD, isXTurn ? 1 : 2);
            if (next.checkWin(1) || next.checkWin(2)) continue;
            buildBook(next, ply + 1, compIsX, !isXTurn, solver, book);
        }
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::cerr << "=== 5×5 Opening Book Generator ===\n"
              << "  Board: " << BOARD << "×" << BOARD << "  target=" << WIN_TARGET << "\n"
              << "  Search depth: " << SEARCH_DEPTH << " (calibrated convergence)\n"
              << "  Book depth: " << BOOK_PLIES << " plies\n\n";

    Solver solver;
    solver.heuristic = [](const Bitboard& b, bool) -> int {
        // Use the same heuristic as calibrate.cpp so the search is guided
        auto openLine = [&](int p) {
            static const int W[] = {0, 1, 8, 64, 512};
            const uint64_t pm = (p==1) ? b.xBits : b.oBits;
            const uint64_t om = (p==1) ? b.oBits : b.xBits;
            int s = 0;
            auto eval = [&](uint64_t mask) {
                if (om & mask) return;
                int cnt = __builtin_popcountll(pm & mask);
                if (cnt > 0 && cnt < WIN_TARGET) s += W[cnt];
            };
            for (int r=0;r<BOARD;r++) for (int c=0;c<=BOARD-WIN_TARGET;c++)
                eval(((1ULL<<WIN_TARGET)-1)<<(r*BOARD+c));
            for (int c=0;c<BOARD;c++) for (int r=0;r<=BOARD-WIN_TARGET;r++) {
                uint64_t mask=0; for(int i=0;i<WIN_TARGET;i++) mask|=1ULL<<((r+i)*BOARD+c);
                eval(mask);
            }
            for (int r=0;r<=BOARD-WIN_TARGET;r++) for (int c=0;c<=BOARD-WIN_TARGET;c++) {
                uint64_t mask=0; for(int i=0;i<WIN_TARGET;i++) mask|=1ULL<<((r+i)*BOARD+(c+i));
                eval(mask);
            }
            for (int r=WIN_TARGET-1;r<BOARD;r++) for (int c=0;c<=BOARD-WIN_TARGET;c++) {
                uint64_t mask=0; for(int i=0;i<WIN_TARGET;i++) mask|=1ULL<<((r-i)*BOARD+(c+i));
                eval(mask);
            }
            return s;
        };
        int raw = openLine(1) - openLine(2);
        return std::max(-999, std::min(999, raw));
    };

    std::map<std::string, int> book;

    // ── Computer goes FIRST (as X) ────────────────────────────────────────────
    std::cerr << "Building book: computer as X (goes first)...\n";
    buildBook(Bitboard(BOARD, WIN_TARGET), 0, true, true, solver, book);

    // ── Computer goes SECOND (as O) ───────────────────────────────────────────
    std::cerr << "\nBuilding book: computer as O (goes second)...\n";
    buildBook(Bitboard(BOARD, WIN_TARGET), 0, false, true, solver, book);

    std::cerr << "\nTotal unique canonical entries: " << book.size() << "\n";
    std::cerr << "Writing JSON to stdout...\n";

    // ── Output JSON ───────────────────────────────────────────────────────────
    std::cout << "{\n"
              << "  \"boardSize\": "     << BOARD       << ",\n"
              << "  \"target\": "        << WIN_TARGET  << ",\n"
              << "  \"searchDepth\": "   << SEARCH_DEPTH << ",\n"
              << "  \"bookDepth\": "     << BOOK_PLIES  << ",\n"
              << "  \"totalEntries\": "  << book.size() << ",\n"
              << "  \"entries\": {\n";

    bool first = true;
    for (const auto& [key, move] : book) {
        if (!first) std::cout << ",\n";
        std::cout << "    \"" << key << "\": " << move;
        first = false;
    }
    std::cout << "\n  }\n}\n";

    std::cerr << "Done.\n";
    return 0;
}
