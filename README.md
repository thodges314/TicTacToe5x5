# Tic-Tac-Toe 5×5 — Depth-12 AI

A fully browser-playable 5×5 Tic-Tac-Toe game powered by a C++ minimax engine compiled to **WebAssembly**. The AI is calibrated to **depth 12**, confirmed by an overnight convergence experiment to play at effectively the same quality as depth 14 and 16. No server required — runs entirely in the browser.

🎮 **[Play it live →](https://thodges314.github.io/TicTacToe5x5/)**

---

## How It Works

### The Engine

The AI is a classic **minimax with alpha-beta pruning**, written in C++20 and compiled to WASM via [Emscripten](https://emscripten.org/).

Key optimisations:

| Technique | Effect |
|---|---|
| **Bitboards** | Board state stored as two `uint64_t` masks (X bits, O bits). All win checks, move generation, and hashing are single-instruction bitwise ops. |
| **Alpha-Beta pruning** | Eliminates branches that can't affect the result. Cuts the effective search tree by ~99% vs naive minimax. |
| **Transposition Table (TT)** | 128 MB Zobrist-hashed cache of previously evaluated positions. Shared across moves within a game session — warms up quickly and makes mid/late-game moves near-instant. |
| **D₈ Symmetry** | The dihedral group of the square has 8 elements (4 rotations × 2 reflections). Each position is stored and looked up in its canonical (minimum-hash) form, reducing the effective search space by up to 8×. |
| **Center-first move ordering** | Moves closer to the centre are tried first. Combined with alpha-beta, this dramatically increases the number of branches pruned at each node. |
| **Heuristic evaluation** | At the depth limit, instead of returning 0, the engine scores open lines (unblocked runs of 1–4 pieces) using weights 1 / 8 / 64 / 512. This makes depth-limited play genuinely stronger. |

### Depth Calibration

To find the ideal search depth, an **automated arena tournament** was run overnight on an Apple M2 Studio (12 cores):

```
Depths tested: 2, 4, 6, 8, 10, 12, 14, 16 (even only — odd depths
               introduce optimism bias by halting on the mover's ply)

Method: even-increment self-play (both players at depth D),
        then cross-depth matchups (D vs D+2)
```

Results:

| Depth | Self-play time | Move sequence same as D-2? |
|---|---|---|
| D=2 | 2 ms | — |
| D=4 | 21 ms | ❌ |
| D=6 | 133 ms | ❌ |
| D=8 | 2.1 s | ❌ |
| D=10 | 13.5 s | ❌ |
| **D=12** | **3.9 min** | ❌ |
| **D=14** | **28.6 min** | ✅ **CONVERGENCE** |
| **D=16** | 111 min | ✅ confirmed |

**D=12 is the convergence depth**: D=12, D=14, and D=16 all produce identical move sequences and outcomes. Playing deeper gains nothing in quality.

Separately, every cross-depth matchup across all tested depths (D=2 vs D=4 up to D=8 vs D=10) ended in a **draw**. This is strong evidence that 5×5 Tic-Tac-Toe with a winning line of 5 is a **theoretical draw under perfect play** — neither player can force a win.

### Opening Book

A depth-12 **6-ply opening book** is precomputed offline on the M2 Studio and shipped as a static JSON file. It covers the computer's optimal response to every possible sequence of the first 3 human moves, for both the "computer goes first" and "computer goes second" scenarios.

**Design:**
- **Key**: canonical 25-character board string (values `0`/`1`/`2` for empty/X/O), where "canonical" is the lexicographically minimum reading of the board over all 8 D₈ symmetry transforms.
- **Value**: best-move flat cell index in canonical board coordinates.
- **Lookup in JS**: compute the canonical transform, look up the key, apply the inverse transform to recover the actual board coordinate.

This approach avoids `BigInt` entirely and is efficient in plain JavaScript.

### Adaptive Search Depth

Once the opening book is exhausted, the live WASM engine uses **adaptive depth** based on how many empty cells remain:

| Empty cells | Search depth | Typical WASM time |
|---|---|---|
| ≥ 21 | D=6 | ~200 ms |
| 17–20 | D=8 | ~800 ms |
| 13–16 | D=10 | ~300 ms |
| 9–12 | D=12 | ~100 ms |
| ≤ 8 | D=20 | < 50 ms |

The TT warms up rapidly during a game session, so subsequent moves at the same depth are much faster than the first.

---

## Project Structure

```
TicTacToe5x5/
├── index.html              # Main page (GitHub Pages root)
├── style.css               # Dark navy/cyan/amber theme
├── app.js                  # Game controller: book lookup, adaptive depth, UI
│
├── engine/
│   ├── Bitboard.hpp        # Bitboard board representation + D₈ symmetry
│   ├── Solver.hpp          # Minimax, alpha-beta, TT, heuristic
│   └── wasm_api.cpp        # C→WASM bridge (wasm_init, wasm_getBestMove, …)
│
├── public/
│   ├── engine.js           # Emscripten-generated WASM loader  ← committed
│   ├── engine.wasm         # Compiled engine binary            ← committed
│   └── opening_book5x5.json  # Precomputed D=12 openings      ← committed
│
├── tools/
│   ├── calibrate.cpp       # Depth convergence calibration tool
│   └── gen_book5x5.cpp     # Exhaustive opening book generator (D=12, 6-ply)
│
├── results/                # Generated at runtime (gitignored)
│   ├── calibration_d20.txt # Convergence table from overnight run
│   └── gen_book5x5.log     # Book generation progress log
│
└── Makefile
```

> **Why are `engine.js`, `engine.wasm`, and `opening_book5x5.json` committed?**
> GitHub Pages serves static files only — there is no build step. The compiled WASM artifacts and precomputed JSON must live in the repository so Pages can serve them directly.

---

## Building Locally

### Prerequisites

- **clang++ / g++** with C++20 support
- **Emscripten** (for WASM build) — install via [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
- **Python 3** (for `make serve`)

### Commands

```bash
# Build native tools (calibrate + gen_book5x5)
make

# Build WASM engine (requires emcc on PATH or ~/emsdk)
source ~/emsdk/emsdk_env.sh
make wasm

# Start local dev server at http://localhost:8080/
make serve

# Run depth convergence calibration (outputs table to stdout)
make run-quiet      # suppress thread logs
make run            # include per-thread timing on stderr

# Generate opening book (runs overnight — nohup, safe to disconnect)
make book
# Progress: tail -f results/gen_book5x5.log
# Output:   public/opening_book5x5.json
```

### Updating the Opening Book After Generation

Once `gen_book5x5` finishes:

```bash
git add public/opening_book5x5.json
git commit -m "Add completed D=12 opening book (6-ply, N positions)"
git push
```

GitHub Pages redeploys automatically. The next page load will show **"Engine ready · book: N positions"**.

---

## Deployment (GitHub Pages)

1. Go to **Settings → Pages** in this repository
2. Set **Source** to `Deploy from branch: main / (root)`
3. Save — the site is live within ~60 seconds at:
   ```
   https://thodges314.github.io/TicTacToe5x5/
   ```

No build pipeline, CDN, or server needed.

---

## Calibration Methodology

The calibration tool (`tools/calibrate.cpp`) implements three principles drawn from chess engine testing:

1. **Even depths only** — Odd-depth searches halt on the moving player's ply, producing optimistic (biased) scores. Even depths halt after the opponent has replied, giving stable and honest evaluations.

2. **Heuristic evaluation at the depth limit** — Without a heuristic, all depth-limited nodes that don't reach a terminal state return 0. This makes every depth play identically (centre moves always chosen by tie-breaking). The open-line heuristic (weights 1 / 8 / 64 / 512 for 1 / 2 / 3 / 4 pieces in an unblocked line) differentiates depths meaningfully.

3. **Step size = 2** — Comparing D vs D+2 is more conservative and meaningful than D vs D+1, because it crosses two plies and requires both a maximiser and minimiser improvement to produce a different result.

**Convergence criterion**: depth D has converged when both:
- Self-play at D produces the same complete move sequence as self-play at D+2
- D can hold a draw against D+2 from both sides (playing as X and O)

---

## Relation to the 4×4 Project

This project is a sister to [TicTacToe4x4](https://github.com/thodges314/TicTacToe4x4), which uses the same engine architecture but is **fully and perfectly solved**:

| | 4×4 (line of 4) | 5×5 (line of 5) |
|---|---|---|
| Win target | 4 in a row | 5 in a row |
| Full game tree | Tractable (~hours) | Intractable at full depth |
| Engine depth | Full (∞) | Depth 12 (calibrated) |
| Opening book | 4 entries (trivial) | ~300–500 entries (6-ply) |
| Theoretical result | **Draw** (proven) | **Draw** (strongly implied) |
| Live WASM depth | Full | Adaptive D=6–20 |

---

## Tech Stack

| Layer | Technology |
|---|---|
| Game engine | C++20 |
| Build (WASM) | Emscripten (`emcc`) |
| Frontend | Vanilla HTML / CSS / JavaScript |
| Fonts | [Inter](https://fonts.google.com/specimen/Inter), [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (Google Fonts) |
| Hosting | GitHub Pages |
| Sound | Web Audio API (synthetic, no assets) |

---

## License

MIT — use freely, attribution appreciated.
