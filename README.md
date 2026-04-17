# Tic-Tac-Toe 5×5 — Depth-12 AI

A fully browser-playable 5×5 Tic-Tac-Toe game powered by a C++ minimax engine compiled to **WebAssembly**. The AI is calibrated to **depth 12**, confirmed by an overnight convergence experiment to produce identical play to depth 14 and 16. No server required — runs entirely in the browser.

🎮 **[Play it live →](https://thodges314.github.io/TicTacToe5x5/)**

---

## How It Works

### The Engine

The AI is a classic **minimax with alpha-beta pruning**, written in C++20 and compiled to WASM via [Emscripten](https://emscripten.org/). The WASM module runs in a **Web Worker** so the UI remains fully responsive while the engine thinks.

Key optimisations:

| Technique | Effect |
|---|---|
| **Bitboards** | Board state stored as two `uint64_t` masks (X bits, O bits). All win checks, move generation, and hashing are single-instruction bitwise ops. |
| **Alpha-Beta pruning** | Eliminates branches that can't affect the result. Cuts the effective search tree by ~99% vs naive minimax. |
| **Transposition Table (TT)** | 128 MB Zobrist-hashed cache. Uses **depth-preferred replacement**: a cached entry is only overwritten if the new search used equal or greater remaining depth, or if it's a different position entirely. This preserves high-quality results across moves. |
| **D₈ Symmetry** | The dihedral group of the square has 8 elements (4 rotations × 2 reflections). Each position is stored and looked up in its canonical (minimum-hash) form, reducing the effective search space by up to 8×. |
| **Center-first move ordering** | Moves closer to the centre are tried first, maximising alpha-beta pruning efficiency. A separate positional heuristic also rewards piece centrality, biasing the engine toward board control. |
| **Open-line heuristic** | At the depth limit, unblocked runs of 1–4 pieces are scored using weights 1 / 8 / 64 / 512. This makes depth-limited play genuinely stronger than returning 0 at every leaf. |
| **Web Worker** | The WASM engine runs in a dedicated background thread via the Web Worker API. The UI never blocks — "Thinking…" animations and piece placement remain smooth regardless of engine think time. |

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

A depth-12 **6-ply opening book** was precomputed offline on the M2 Studio (~3 hours, 12 cores) and ships as a static JSON file:

```
public/opening_book5x5.json — 824 canonical positions, ~30 KB
```

It covers the computer's optimal response to every reachable sequence of the first 3 human moves, for both the "computer goes first" and "computer goes second" scenarios.

**Design:**
- **Key**: canonical 25-character board string (values `0`/`1`/`2` for empty/X/O), where "canonical" is the lexicographically minimum reading of the board over all 8 D₈ symmetry transforms.
- **Value**: best-move flat cell index in canonical board coordinates.
- **Lookup in JS**: compute the canonical transform, look up the key, apply the inverse transform to recover the actual board coordinate.

This approach avoids `BigInt` entirely and is efficient in plain JavaScript.

### Adaptive Search Depth

Once the opening book is exhausted, the live WASM engine uses **adaptive depth** based on how many empty cells remain:

| Empty cells | Moves made | Search depth | Notes |
|---|---|---|---|
| ≥ 23 | ≤ 2 | D=6 | Book always covers this |
| 17–22 | 3–8 | D=8 | Book→live transition zone |
| 13–16 | 9–12 | D=10 | TT now warm, fast |
| 9–12 | 13–16 | D=12 | Near-convergence depth |
| ≤ 8 | 17+ | = remaining cells | Full exhaustive search, trivially fast |

> **Why D=8 at the book→live boundary?** The first live move (~19–22 empty cells) is the easiest place for an experienced player to exploit a weaker engine. D=8 closes that gap — an acceptable latency trade-off for the Hall of Fame tier of challenge.

The TT warms up during a game session, so subsequent moves at the same depth are much faster than the first (often appearing as "0 ms" in the move-info indicator — meaning the result was found instantly in cache).

---

## UI Features

### Move-Source Indicator

After every computer move, a small indicator below the status bar shows exactly how the move was decided:

| Display | Meaning |
|---|---|
| 📖 **Opening book** (amber) | Instant lookup from the precomputed D=12 book |
| 🔍 **Engine · D=N · Xms** (cyan) | Live WASM search at depth N, completed in X milliseconds |

The depth shown is `min(requested depth, remaining empty cells)` — accurately reflecting the actual search that ran.

### Forced-Draw Detection Toggle

A **"Detect forced draws"** toggle is visible during gameplay. When enabled, the game ends immediately if no 5-in-a-row line is mathematically possible for either player — before the board is completely full.

- **Off by default**: the AI can still sneak a win in positions that look dead, and you must stay alert
- **On**: draws are flagged early with `🤝 No winning lines remain for either side`
- **Persists** across sessions via `localStorage`
- Can be toggled mid-game

---

## Project Structure

```
TicTacToe5x5/
├── index.html                  # Main page (GitHub Pages root)
├── style.css                   # Dark navy/cyan/amber theme
├── app.js                      # Game controller: book lookup, adaptive depth,
│                               #   move-info indicator, forced-draw toggle
│
├── engine/
│   ├── Bitboard.hpp            # Bitboard representation + D₈ symmetry
│   ├── Solver.hpp              # Minimax, alpha-beta, depth-preferred TT,
│   │                           #   open-line + positional heuristics
│   └── wasm_api.cpp            # C→WASM bridge (wasm_init, wasm_getBestMove, …)
│
├── public/
│   ├── engine.js               # Emscripten-generated WASM loader  ← committed
│   ├── engine.wasm             # Compiled engine binary            ← committed
│   ├── worker.js               # Web Worker — loads WASM, handles
│   │                           #   getBestMove off the main thread  ← committed
│   └── opening_book5x5.json   # 824 D=12 canonical openings (6-ply) ← committed
│
├── tools/
│   ├── calibrate.cpp           # Depth convergence calibration tool
│   └── gen_book5x5.cpp         # Exhaustive opening book generator (D=12, 6-ply)
│
├── results/                    # Generated at runtime (gitignored)
│   ├── calibration_d20.txt     # Convergence table from overnight run
│   └── gen_book5x5.log         # Book generation progress log
│
└── Makefile
```

> **Why are `engine.js`, `engine.wasm`, `worker.js`, and `opening_book5x5.json` committed?**
> GitHub Pages serves static files only — there is no build step. All compiled artifacts and precomputed data must live in the repository.

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

### Updating the Opening Book

If you regenerate the book and want to deploy the update:

```bash
git add public/opening_book5x5.json
git commit -m "Update D=12 opening book (6-ply, N positions)"
git push
```

GitHub Pages redeploys automatically. Engine status will show **"Engine ready · book: N positions"**.

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

2. **Heuristic evaluation at the depth limit** — Without a heuristic, all depth-limited nodes return 0 identically. The open-line heuristic (weights 1 / 8 / 64 / 512 for 1–4 pieces in an unblocked line) differentiates depths meaningfully.

3. **Step size = 2** — Comparing D vs D+2 crosses two full plies, requiring both a maximiser and minimiser improvement to produce a different result — a more conservative and reliable convergence criterion than D vs D+1.

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
| Engine depth | Full (∞) | Depth 12 (calibrated convergence) |
| Opening book | 4 entries (trivial) | 824 entries, 6-ply, D=12 |
| Theoretical result | **Draw** (proven) | **Draw** (strongly implied) |
| Live WASM depth | Full | Adaptive D=8–(remaining cells) |
| Forced-draw detection | N/A | Optional toggle |
| Hall of Fame possible? | No (no wins possible) | Yes (rare human wins possible) |

---

## Hall of Fame — Can a Human Win?

### Why It's Nearly Impossible

The engine plays at **convergence depth (D=12)** throughout the opening book, and **D=8** the moment the live engine takes over. Combined with a warm transposition table, this makes the AI genuinely elite:

| Player type | Estimated win probability |
|---|---|
| Casual player | < 0.01% |
| Puzzle / strategy enthusiast | < 0.1% |
| Someone who studies the engine carefully | ~1–2% |
| Expected Hall of Fame entries / year (worldwide) | ~1–3 |

**Why a human can't simply draw:** "Draw under perfect play" applies only if *both* sides play perfectly. Any deviation by the human gives the engine a winning position it exploits perfectly. Successfully drawing still requires finding the optimal response at every single move.

### The One Realistic Path to a Win

The live engine's first moves after the book exhausts (~19 empty cells) are the most exploitable window. The adaptive depth was deliberately set to D=8 here (rather than D=6) to close the easiest tactical gap. A highly analytical player studying recorded games might identify a position where D=8 misses a tactical shot.

### Anti-Farming Design (Planned)

- **Hall of Fame backend**: AWS stores the complete move sequence of every winning game as JSON.
- **Pattern matching**: Before registering a new win, the sequence is compared against all stored games. An identical sequence (or one differing only by D₈ symmetry, handled via canonical hashing) is rejected.
- **Effect**: A discovered exploit can claim Hall of Fame once, then gets patched into the opening book for a future update — permanently closing it.

> The rarity of genuinely earned wins is what makes a Hall of Fame entry meaningful.

---

## Tech Stack

| Layer | Technology |
|---|---|
| Game engine | C++20 |
| Build (WASM) | Emscripten (`emcc`) |
| Concurrency | Web Worker API (engine off main thread) |
| Frontend | Vanilla HTML / CSS / JavaScript |
| Fonts | [Inter](https://fonts.google.com/specimen/Inter), [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (Google Fonts) |
| Hosting | GitHub Pages |
| Sound | Web Audio API (synthetic, no assets) |
| Persistence | `localStorage` (forced-draw toggle preference) |

---

## License

MIT — use freely, attribution appreciated.
