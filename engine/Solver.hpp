#pragma once
#include "Bitboard.hpp"
#include <algorithm>
#include <limits>
#include <vector>
#include <utility>
#include <atomic>
#include <functional>

#ifndef WASM_BUILD
#include <future>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#endif

// ============================================================================
// Transposition Table — lockless single-word atomic design
//
// bits  0–15 : score  (int16_t cast to uint16_t)
// bits 16–17 : Bound  (0=INVALID, 1=EXACT, 2=LOWER, 3=UPPER)
// bits 18–57 : tag    (key >> 24, upper 40 bits of canonical key)
// bits 58–63 : unused
//
// tableSize = 1<<24 = 16M entries × 8 bytes = 128 MB
// ============================================================================
enum class Bound : uint8_t { INVALID = 0, EXACT = 1, LOWER = 2, UPPER = 3 };

struct TTEntry {
    std::atomic<uint64_t> data{0};

    static uint64_t pack(uint64_t key, int score, Bound bound) {
        return ((key >> 24) << 18)
             | (static_cast<uint64_t>(static_cast<uint8_t>(bound)) << 16)
             | static_cast<uint16_t>(static_cast<int16_t>(score));
    }

    void store(uint64_t key, int score, Bound bound) {
        data.store(pack(key, score, bound), std::memory_order_relaxed);
    }

    bool lookup(uint64_t key, int& score, Bound& bound) const {
        uint64_t raw = data.load(std::memory_order_relaxed);
        Bound b = static_cast<Bound>((raw >> 16) & 0x3);
        if (b == Bound::INVALID)        return false;
        if ((raw >> 18) != (key >> 24)) return false;
        score = static_cast<int>(static_cast<int16_t>(raw & 0xFFFF));
        bound = b;
        return true;
    }
};

// ============================================================================
// Solver
// ============================================================================
class Solver {
public:
    static constexpr size_t tableSize = 1u << 24; // 128 MB, power of 2
    TTEntry* table;

    Solver()  { table = new TTEntry[tableSize]; }
    ~Solver() { delete[] table; }

    // Optional heuristic for depth-limited search.
    // Signature: f(board, isMaximizing) -> int in range (-999..999).
    // nullptr = return 0 at depth limit (correct for full-depth perfect play).
    // Set this for depth-capped searches where you want informed evaluation.
    std::function<int(const Bitboard&, bool)> heuristic = nullptr;

    // -------------------------------------------------------------------------
    // Core minimax — identical logic in both WASM and native builds.
    // maxDepth: INT_MAX for full perfect solve, any positive N for depth cap.
    // -------------------------------------------------------------------------
    int minimax(Bitboard board, int depth, int alpha, int beta, bool isMaximizing,
                int maxDepth = std::numeric_limits<int>::max()) {
        if (board.checkWin(1)) return  1000 - depth;
        if (board.checkWin(2)) return -1000 + depth;

        auto moves = board.getAvailableMoves();
        if (moves.empty()) return 0;
        if (depth >= maxDepth) return heuristic ? heuristic(board, isMaximizing) : 0;

        uint64_t key   = board.getCanonicalState();
        size_t   index = key & (tableSize - 1);

        const int origAlpha = alpha, origBeta = beta;
        {
            int cachedScore; Bound cachedBound;
            if (table[index].lookup(key, cachedScore, cachedBound)) {
                if (cachedBound == Bound::EXACT) return cachedScore;
                if (cachedBound == Bound::LOWER) alpha = std::max(alpha, cachedScore);
                if (cachedBound == Bound::UPPER) beta  = std::min(beta,  cachedScore);
                if (alpha >= beta) return cachedScore;
            }
        }

        int best;
        if (isMaximizing) {
            best = std::numeric_limits<int>::min();
            for (int m : moves) {
                Bitboard next = board;
                next.setPiece(m / board.size, m % board.size, 1);
                int eval = minimax(next, depth + 1, alpha, beta, false, maxDepth);
                best  = std::max(best, eval);
                alpha = std::max(alpha, eval);
                if (beta <= alpha) break;
            }
        } else {
            best = std::numeric_limits<int>::max();
            for (int m : moves) {
                Bitboard next = board;
                next.setPiece(m / board.size, m % board.size, 2);
                int eval = minimax(next, depth + 1, alpha, beta, true, maxDepth);
                best = std::min(best, eval);
                beta = std::min(beta, eval);
                if (beta <= alpha) break;
            }
        }

        Bound bound;
        if      (best <= origAlpha) bound = Bound::UPPER;
        else if (best >= origBeta)  bound = Bound::LOWER;
        else                        bound = Bound::EXACT;
        table[index].store(key, best, bound);
        return best;
    }

    // -------------------------------------------------------------------------
    // Single-threaded getBestMove — used in WASM build.
    // GitHub Pages / SharedArrayBuffer restrictions make std::async unreliable
    // in browsers without explicit COOP/COEP headers. Single-threaded is safe
    // and still fast: the TT persists across moves within the same session.
    // -------------------------------------------------------------------------
    std::pair<int,int> getBestMoveSingleThreaded(Bitboard board, bool isX,
                                                 int maxDepth = std::numeric_limits<int>::max()) {
        auto moves = board.getAvailableMoves();
        if (moves.empty()) return {-1, 0};

        int bestMove  = -1;
        int bestScore = isX ? std::numeric_limits<int>::min()
                             : std::numeric_limits<int>::max();

        for (int m : moves) {
            Bitboard next = board;
            next.setPiece(m / board.size, m % board.size, isX ? 1 : 2);
            int eval = minimax(next, 0,
                               std::numeric_limits<int>::min(),
                               std::numeric_limits<int>::max(),
                               !isX, maxDepth);
            if ( isX && eval > bestScore) { bestScore = eval; bestMove = m; }
            if (!isX && eval < bestScore) { bestScore = eval; bestMove = m; }
        }
        return {bestMove, bestScore};
    }

#ifndef WASM_BUILD
    // -------------------------------------------------------------------------
    // Multithreaded getBestMove — native build only.
    // Batches moves in groups of hardware_concurrency() to avoid thread thrash.
    // -------------------------------------------------------------------------
    std::pair<int,int> getBestMove(Bitboard board, bool isX,
                                   int maxDepth = std::numeric_limits<int>::max()) {
        auto moves = board.getAvailableMoves();
        if (moves.empty()) return {-1, 0};

        std::mutex cout_mutex;
        const unsigned int maxThreads = []() {
            unsigned int hw = std::thread::hardware_concurrency();
            return (hw > 0) ? hw : 12u;
        }();

        int bestMove  = -1;
        int bestScore = isX ? std::numeric_limits<int>::min()
                             : std::numeric_limits<int>::max();

        for (size_t i = 0; i < moves.size(); i += maxThreads) {
            size_t end = std::min(i + static_cast<size_t>(maxThreads), moves.size());
            std::vector<std::future<std::pair<int,int>>> futures;
            futures.reserve(end - i);

            for (size_t j = i; j < end; j++) {
                int m = moves[j];
                futures.push_back(std::async(std::launch::async,
                    [this, board, m, isX, maxDepth, &cout_mutex]() -> std::pair<int,int> {
                        if (board.size >= 4) {
                            std::lock_guard<std::mutex> lk(cout_mutex);
                            std::cerr << "  -> Thread starting branch ["
                                      << m / board.size << "," << m % board.size << "]...\n";
                        }
                        Bitboard next = board;
                        next.setPiece(m / board.size, m % board.size, isX ? 1 : 2);
                        auto t0 = std::chrono::high_resolution_clock::now();
                        int eval = minimax(next, 0,
                                          std::numeric_limits<int>::min(),
                                          std::numeric_limits<int>::max(),
                                          !isX, maxDepth);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        std::chrono::duration<double,std::milli> ms = t1 - t0;
                        if (board.size >= 4) {
                            std::lock_guard<std::mutex> lk(cout_mutex);
                            std::cerr << "  -> Thread finished  branch ["
                                      << m / board.size << "," << m % board.size
                                      << "] | Score: " << eval
                                      << " | Thread Time: " << ms.count() << " ms\n";
                        }
                        return {m, eval};
                    }
                ));
            }

            for (auto& f : futures) {
                auto [move, score] = f.get();
                if ( isX && score > bestScore) { bestScore = score; bestMove = move; }
                if (!isX && score < bestScore) { bestScore = score; bestMove = move; }
            }
        }
        return {bestMove, bestScore};
    }
#endif
};
