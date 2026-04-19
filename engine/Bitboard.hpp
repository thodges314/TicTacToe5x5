#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>

#ifndef WASM_BUILD
#include <iostream>
#endif

// ============================================================================
// Bitboard — compact game state
//
// Layout A: `state`      — 2 bits per cell (00=empty, 01=X, 10=O)
// Layout B: `xBits/oBits`— 1 bit per cell, maintained for O(1) win checks
// Layout C: `zobristHashes[8]` — one per D₈ symmetry, O(1) canonical state
// Layout D: `moveOrder`  — precomputed center-first order, no per-node sort
// ============================================================================
class Bitboard {
public:
    uint64_t state  = 0;
    uint64_t xBits  = 0;
    uint64_t oBits  = 0;
    int size;
    int target;
    uint64_t zobristHashes[8] = {};
    std::vector<int> moveOrder;

    // Single shared Zobrist table — one entry per (cell, player).
    // Using ONE table (not per-symmetry) is required for correctness:
    // symmetric boards must share the same 8-hash set so min() is identical.
    static uint64_t zobristTable[25][2];
    static bool zobristInitialized;

    static void initZobrist() {
        if (zobristInitialized) return;
        std::mt19937_64 rng(0xDEADBEEFCAFE1234ULL);
        for (int cell = 0; cell < 25; cell++)
            for (int p = 0; p < 2; p++)
                zobristTable[cell][p] = rng();
        zobristInitialized = true;
    }

    static int applySymmetry(int sym, int r, int c, int sz) {
        switch (sym) {
            case 0: return r * sz + c;
            case 1: return c * sz + (sz - 1 - r);
            case 2: return (sz-1-r) * sz + (sz-1-c);
            case 3: return (sz-1-c) * sz + r;
            case 4: return r * sz + (sz-1-c);
            case 5: return (sz-1-r) * sz + c;
            case 6: return c * sz + r;
            case 7: return (sz-1-c) * sz + (sz-1-r);
            default: return -1;
        }
    }

    Bitboard(int s, int t) : size(s), target(t) {
        initZobrist();
        buildMoveOrder();
    }

    Bitboard(uint64_t s, int sz, int t) : state(s), size(sz), target(t) {
        initZobrist();
        rebuildDerived();
        buildMoveOrder();
    }

    void setPiece(int row, int col, int playerIdx) {
        int shift   = (row * size + col) * 2;
        int cellIdx = row * size + col;
        state |= (static_cast<uint64_t>(playerIdx) << shift);
        if (playerIdx == 1) xBits |= (1ULL << cellIdx);
        else                oBits |= (1ULL << cellIdx);
        for (int sym = 0; sym < 8; sym++) {
            int mapped = applySymmetry(sym, row, col, size);
            zobristHashes[sym] ^= zobristTable[mapped][playerIdx - 1];
        }
    }

    int getPiece(int row, int col) const {
        return (state >> ((row * size + col) * 2)) & 0b11;
    }

    uint64_t getCanonicalState() const {
        uint64_t minHash = zobristHashes[0];
        for (int i = 1; i < 8; i++)
            if (zobristHashes[i] < minHash) minHash = zobristHashes[i];
        return minHash;
    }

    std::vector<int> getAvailableMoves() const {
        std::vector<int> moves;
        moves.reserve(size * size);
        for (int m : moveOrder)
            if (((state >> (m * 2)) & 0b11) == 0)
                moves.push_back(m);
        return moves;
    }

    bool checkWin(int playerIdx) const {
        const uint64_t pm = (playerIdx == 1) ? xBits : oBits;
        for (int r = 0; r < size; r++)
            for (int c = 0; c <= size - target; c++) {
                uint64_t mask = ((1ULL << target) - 1) << (r * size + c);
                if ((pm & mask) == mask) return true;
            }
        uint64_t vT = 0;
        for (int i = 0; i < target; i++) vT |= (1ULL << (i * size));
        for (int c = 0; c < size; c++)
            for (int r = 0; r <= size - target; r++) {
                uint64_t mask = vT << (r * size + c);
                if ((pm & mask) == mask) return true;
            }
        uint64_t d1T = 0;
        for (int i = 0; i < target; i++) d1T |= (1ULL << (i * (size + 1)));
        for (int r = 0; r <= size - target; r++)
            for (int c = 0; c <= size - target; c++) {
                uint64_t mask = d1T << (r * size + c);
                if ((pm & mask) == mask) return true;
            }
        // Anti-diagonal ↙: template anchored at (0, target-1), steps down-left.
        // Formula: i*(size-1)+(target-1) places bits at flat indices
        //   (target-1), (target-1)+(size-1), ..., (target-1)+(target-1)*(size-1)
        // which map to cells (0,target-1), (1,target-2), ..., (target-1,0). Correct.
        uint64_t d2T = 0;
        for (int i = 0; i < target; i++) d2T |= (1ULL << (i * (size - 1) + (target - 1)));
        for (int r = 0; r <= size - target; r++)
            for (int c = 0; c <= size - target; c++) {
                uint64_t mask = d2T << (r * size + c);
                if ((pm & mask) == mask) return true;
            }
        return false;
    }

    // -------------------------------------------------------------------------
    // isTheoreticalDraw — O(lines) early-exit for the minimax search.
    //
    // A position is a "theoretical draw" (dead position) when every possible
    // winning line on the board contains at least one piece from EACH player,
    // making it permanently uncompletable by either side.  When this is true,
    // no sequence of future moves can produce a winner, so minimax can return
    // 0 immediately instead of exploring all remaining empty squares.
    //
    // Uses identical template/mask logic to checkWin() for consistency.
    // On a 5×5 target-5 board there are exactly 12 winning lines so this
    // costs ~12 × 2 bitwise operations — cheaper than one node expansion.
    // -------------------------------------------------------------------------
    bool isTheoreticalDraw() const {
        // A line is "alive" if at least one player has NO pieces in it
        //   (that player can still complete it → game is not a dead draw).
        // A line is "dead"  if BOTH players have ≥1 piece in it.
        // We return false as soon as we find any alive line.

        // Horizontal
        for (int r = 0; r < size; r++)
            for (int c = 0; c <= size - target; c++) {
                uint64_t mask = ((1ULL << target) - 1) << (r * size + c);
                if (!(xBits & mask) || !(oBits & mask)) return false;
            }
        // Vertical
        uint64_t vT = 0;
        for (int i = 0; i < target; i++) vT |= (1ULL << (i * size));
        for (int c = 0; c < size; c++)
            for (int r = 0; r <= size - target; r++) {
                uint64_t mask = vT << (r * size + c);
                if (!(xBits & mask) || !(oBits & mask)) return false;
            }
        // Main diagonal ↘
        uint64_t d1T = 0;
        for (int i = 0; i < target; i++) d1T |= (1ULL << (i * (size + 1)));
        for (int r = 0; r <= size - target; r++)
            for (int c = 0; c <= size - target; c++) {
                uint64_t mask = d1T << (r * size + c);
                if (!(xBits & mask) || !(oBits & mask)) return false;
            }
        // Anti-diagonal ↙ — same template as checkWin
        uint64_t d2T = 0;
        for (int i = 0; i < target; i++) d2T |= (1ULL << (i * (size - 1) + (target - 1)));
        for (int r = 0; r <= size - target; r++)
            for (int c = 0; c <= size - target; c++) {
                uint64_t mask = d2T << (r * size + c);
                if (!(xBits & mask) || !(oBits & mask)) return false;
            }
        return true;  // every line is dead → guaranteed draw
    }

#ifndef WASM_BUILD
    void display() const {
        for (int r = 0; r < size; r++) {
            for (int c = 0; c < size; c++) {
                int p = getPiece(r, c);
                if      (p == 1) std::cout << "X ";
                else if (p == 2) std::cout << "O ";
                else             std::cout << ". ";
            }
            std::cout << "\n";
        }
    }
#endif

private:
    void buildMoveOrder() {
        moveOrder.resize(size * size);
        std::iota(moveOrder.begin(), moveOrder.end(), 0);
        float center = (size - 1) / 2.0f;
        std::sort(moveOrder.begin(), moveOrder.end(), [&](int a, int b) {
            float da = static_cast<float>(a/size - center) * (a/size - center)
                     + static_cast<float>(a%size - center) * (a%size - center);
            float db = static_cast<float>(b/size - center) * (b/size - center)
                     + static_cast<float>(b%size - center) * (b%size - center);
            return da < db;
        });
    }

    void rebuildDerived() {
        xBits = 0; oBits = 0;
        for (int i = 0; i < 8; i++) zobristHashes[i] = 0;
        for (int i = 0; i < size * size; i++) {
            int p = (state >> (i * 2)) & 0b11;
            if (p == 0) continue;
            int r = i / size, c = i % size;
            if (p == 1) xBits |= (1ULL << i);
            else        oBits |= (1ULL << i);
            for (int sym = 0; sym < 8; sym++)
                zobristHashes[sym] ^= zobristTable[applySymmetry(sym, r, c, size)][p-1];
        }
    }
};

inline uint64_t Bitboard::zobristTable[25][2] = {};
inline bool     Bitboard::zobristInitialized   = false;
