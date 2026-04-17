// ============================================================================
// wasm_api.cpp — C→WASM bridge for 5×5 Tic-Tac-Toe engine
//
// Differences from 4×4 version:
//   - wasm_getBestMove accepts maxDepth parameter for adaptive depth control
//   - JS calls with different depths based on remaining empty cells
// ============================================================================
// WASM_BUILD is defined by the Makefile via -DWASM_BUILD (emcc flag)
#include "../engine/Bitboard.hpp"
#include "../engine/Solver.hpp"
#include <emscripten.h>
#include <cstdlib>

static Solver* g_solver = nullptr;

extern "C" {

// Must be called once before any other function.
EMSCRIPTEN_KEEPALIVE
void wasm_init() {
    if (!g_solver) g_solver = new Solver();
}

// Find best move for the current player.
// cells: int array of length boardSize*boardSize, values 0=empty 1=X 2=O
// isX: 1 if it's X's turn, 0 for O
// maxDepth: search depth cap — JS uses adaptive depth based on empty cell count
// Returns: flat cell index of best move, or -1 on error
EMSCRIPTEN_KEEPALIVE
int wasm_getBestMove(int boardSize, int target, int* cells, int isX, int maxDepth) {
    if (!g_solver || !cells) return -1;

    Bitboard board(boardSize, target);
    for (int i = 0; i < boardSize * boardSize; i++) {
        if (cells[i] != 0)
            board.setPiece(i / boardSize, i % boardSize, cells[i]);
    }

    auto [move, score] = g_solver->getBestMoveSingleThreaded(board, isX == 1, maxDepth);
    (void)score;
    return move;
}

// Returns 1 if player (1=X, 2=O) has won, else 0
EMSCRIPTEN_KEEPALIVE
int wasm_checkWin(int boardSize, int target, int* cells, int player) {
    if (!cells) return 0;
    Bitboard board(boardSize, target);
    for (int i = 0; i < boardSize * boardSize; i++) {
        if (cells[i] != 0)
            board.setPiece(i / boardSize, i % boardSize, cells[i]);
    }
    return board.checkWin(player) ? 1 : 0;
}

// Returns 1 if board is full with no winner (draw), else 0
EMSCRIPTEN_KEEPALIVE
int wasm_isDraw(int boardSize, int* cells) {
    if (!cells) return 0;
    for (int i = 0; i < boardSize * boardSize; i++)
        if (cells[i] == 0) return 0;
    return 1;
}

} // extern "C"
