# Codebase Walkthrough — 5×5 Tic-Tac-Toe Engine

This document is written for someone who already built a working minimax engine
(the original Java project, [github.com/thodges314/TicTacToe](https://github.com/thodges314/TicTacToe)).
You understand recursion, game trees, and alpha-beta pruning. The goal here is
to explain every technique, data structure, and language feature in this newer
project that might be unfamiliar — *why* it exists, *what problem it solves*,
and *how* it works in the actual code.

---

## The Starting Problem

Your original Java engine could make its first 5×5 move in **~2.5 hours**.
That is the natural result of minimax on a naive board representation. The
5×5 game tree (25 moves possible, branching factor shrinks each ply) has
roughly 25! ÷ (average game length) ≈ **10¹⵰ nodes** at the root. Alpha-beta
pruning at its theoretical best reduces that to the square root, ~10⁵, but
the constant factor — cost per node — still matters enormously.

This engine makes the same first move in **~3.9 minutes** (multithreaded
native) or **~1–3 seconds** (WASM). Every technique below is a direct answer
to reducing either the *number of nodes visited* or the *cost per node*.

---

## Part 1: The Board Representation — `Bitboard.hpp`

### What you had: a byte array

```java
// Your Java GameBoard
byte[][] board = new byte[size][size];
```

Checking for a win meant looping through rows, columns, and diagonals — maybe
50–100 individual memory accesses per win check.

### What this engine has: three parallel representations

```cpp
uint64_t state  = 0;  // 2 bits per cell — the "source of truth"
uint64_t xBits  = 0;  // 1 bit per cell — 1 = X is there
uint64_t oBits  = 0;  // 1 bit per cell — 1 = O is there
```

A `uint64_t` is an unsigned 64-bit integer. The 5×5 board has 25 cells. That
fits inside one 64-bit number with bits to spare.

**`state`** uses 2 bits per cell (50 bits total):
```
cell 0 occupies bits 1-0:  00=empty, 01=X, 10=O
cell 1 occupies bits 3-2:  same
cell 2 occupies bits 5-4:  same
...
cell 24 occupies bits 49-48
```

**`xBits` and `oBits`** each use 1 bit per cell:
```
bit 0 of xBits = 1 if cell 0 holds X, else 0
bit 1 of xBits = 1 if cell 1 holds X, else 0
...
```

### Why this makes win checking fast

To check if X has won a specific row, you check whether a contiguous block of
5 bits in `xBits` are all 1. This is done with a **bitmask**:

```cpp
// For row r, starting at column c:
uint64_t mask = ((1ULL << target) - 1) << (r * size + c);
if ((xBits & mask) == mask) { /* X has won this row */ }
```

`(1ULL << 5) - 1` in binary is `0b11111` — exactly 5 set bits. Shifting it
to the right position and ANDing with `xBits` lets you check five cells in a
single CPU instruction. No loop. This is called **bitwise win detection**.

The same approach works for columns and diagonals using different shift amounts.

### The `uint64_t` type

`uint64_t` is a standard C++ type meaning "unsigned integer, exactly 64 bits."
It's guaranteed to be the same size on every platform. The `U` means unsigned
(no negative numbers), `LL` means "long long" on older compilers, and `64`
means 64 bits. You'll see `1ULL << n` frequently — this creates a 64-bit value
with exactly bit `n` set.

### `setPiece` — updating all three representations at once

```cpp
void setPiece(int row, int col, int playerIdx) {
    int shift   = (row * size + col) * 2;
    int cellIdx = row * size + col;
    state |= (static_cast<uint64_t>(playerIdx) << shift);   // update state
    if (playerIdx == 1) xBits |= (1ULL << cellIdx);          // update xBits
    else                oBits |= (1ULL << cellIdx);           // update oBits
    // ...also updates Zobrist hashes (explained later)
}
```

`|=` is bitwise OR-assignment: it turns specific bits on without touching the
others. The `<<` operator shifts bits left. `(1ULL << cellIdx)` creates a mask
with exactly one bit set at position `cellIdx`.

### Center-first move ordering

```cpp
void buildMoveOrder() {
    moveOrder.resize(size * size);
    std::iota(moveOrder.begin(), moveOrder.end(), 0);  // fill with 0,1,2,...,24
    float center = (size - 1) / 2.0f;
    std::sort(moveOrder.begin(), moveOrder.end(), [&](int a, int b) {
        float da = (a/size - center)*(a/size - center)
                 + (a%size - center)*(a%size - center);
        float db = (b/size - center)*(b/size - center)
                 + (b%size - center)*(b%size - center);
        return da < db;
    });
}
```

`std::iota` fills the vector with consecutive values starting at 0 — a quick
way to say "all cell indices, in order." `std::sort` then reorders them by
distance from the center, closest first.

The `[&]` introduces a **lambda** (anonymous function). The `&` means it can
see local variables from the surrounding scope. The lambda receives two cell
indices `a` and `b` and returns `true` when `a` should come before `b`.

**Why center-first?** Alpha-beta pruning works best when you explore the *best
move first*. Strong moves almost always involve the center of the board. By
generating center moves first, you tend to find tight bounds early, which lets
alpha-beta prune more of the remaining tree. The improvement is large — in
practice, moving from random order to center-first can cut runtime by 5–20×.

---

## Part 2: Zobrist Hashing

### The problem: identifying a position cheaply

The Transposition Table (explained next) needs to look up whether a board
position has been seen before. How do you turn a whole board into a number
fast enough to use as a hash key?

The naive approach — convert `state` to a string, hash the string — works but
is slow. **Zobrist hashing** gives us a number that:
- uniquely identifies a board position (with very high probability)
- can be updated incrementally in O(1) as pieces are placed

### How it works

**Setup:** Generate one random 64-bit number for each (cell, player) pair:

```cpp
static uint64_t zobristTable[25][2];  // [cell 0..24][player 0=X, 1=O]

static void initZobrist() {
    std::mt19937_64 rng(0xDEADBEEFCAFE1234ULL);  // seeded pseudo-random
    for (int cell = 0; cell < 25; cell++)
        for (int p = 0; p < 2; p++)
            zobristTable[cell][p] = rng();
}
```

`std::mt19937_64` is a Mersenne Twister — a high-quality pseudo-random number
generator. The fixed seed means the same table is always generated. The table
is declared `static` (shared across all instances) and initialized only once.

**In action:** The hash of a board is the XOR of the random numbers for every
occupied cell:

```cpp
// When placing X at cell idx:
zobristHashes[0] ^= zobristTable[idx][0];

// When placing O at cell idx:
zobristHashes[0] ^= zobristTable[idx][1];
```

XOR (`^`) is its own inverse. If you XOR the same value twice, the bits cancel
out. This property makes Zobrist hashing incremental — you don't need to
re-hash the whole board when one piece is added.

**Why one table works across all 8 symmetries:** The engine maintains 8
separate hashes — one for each D₈ symmetry transform (explained below). When
placing a piece at physical cell `(r, c)`, it looks up where that cell maps
to under each transform, and XORs the corresponding random value into each of
the 8 hashes. Because both players share one consistent table, equivalent
boards produce the same set of 8 hashes regardless of how they arrived.

### `static` class members

```cpp
static uint64_t zobristTable[25][2];
static bool zobristInitialized;
```

`static` inside a class means there is only one copy shared by all instances.
Without `static`, every `Bitboard` object would have its own private copy of
the table — a waste of memory and a source of inconsistency. One table ensures
all boards use the same random numbers.

The definitions at the bottom of the file:
```cpp
inline uint64_t Bitboard::zobristTable[25][2] = {};
inline bool     Bitboard::zobristInitialized   = false;
```
This gives the static members their storage and initial values. C++17 requires
this even for members that are just declared inside the class.

---

## Part 3: D₈ Symmetry — Avoiding Redundant Work

### The idea

A tic-tac-toe board has eight equivalent views: 4 rotations (0°, 90°, 180°,
270°) and 4 reflections (each rotation mirrored). These 8 transformations form
the **dihedral group D₈** — the symmetry group of the square.

Two boards that are rotations or reflections of each other have the same game-
theoretic value. The best move in one is the mirror of the best move in the
other. If you've already solved a position, you've implicitly solved all 7 of
its equivalents.

### How it's implemented

```cpp
static int applySymmetry(int sym, int r, int c, int sz) {
    switch (sym) {
        case 0: return r * sz + c;           // identity (no change)
        case 1: return c * sz + (sz-1-r);    // 90° rotation
        case 2: return (sz-1-r)*sz+(sz-1-c); // 180° rotation
        case 3: return (sz-1-c)*sz + r;      // 270° rotation
        case 4: return r * sz + (sz-1-c);    // horizontal mirror
        case 5: return (sz-1-r)*sz + c;      // vertical mirror
        case 6: return c * sz + r;           // diagonal mirror
        case 7: return (sz-1-c)*sz+(sz-1-r); // anti-diagonal mirror
    }
}
```

Each case maps a cell at `(r, c)` to the corresponding cell under that
transform. For example, a 90° clockwise rotation sends `(r, c)` to `(c, sz-1-r)`.

The engine maintains **8 independent Zobrist hashes**, one per transform. Each
hash is computed by inserting pieces into their transformed positions:

```cpp
for (int sym = 0; sym < 8; sym++) {
    int mapped = applySymmetry(sym, row, col, size);
    zobristHashes[sym] ^= zobristTable[mapped][playerIdx - 1];
}
```

The **canonical state** is the minimum of the 8 hashes:

```cpp
uint64_t getCanonicalState() const {
    uint64_t minHash = zobristHashes[0];
    for (int i = 1; i < 8; i++)
        if (zobristHashes[i] < minHash) minHash = zobristHashes[i];
    return minHash;
}
```

This is a compact, collision-resistant way to say: "across all 8 views of this
board, give me a single identifier." Two boards that are D₈-equivalent will
produce the same `canonicalState`, even though they look different on screen.

### What this buys you

The Transposition Table stores results keyed by canonical state. If the engine
has already solved rotation-A of a position, a lookup for rotation-B (a
different physical board) returns the same cached answer instantly. In practice
this can increase TT hit rates by a factor of 3–8×.

The opening book also uses canonical board strings for exactly the same reason:
824 canonical entries cover over 6,000 actual board positions.

---

## Part 4: The Transposition Table — `Solver.hpp`

### What a TT is

A transposition table is a large key-value cache. Key: a board position
(canonicalized). Value: the result of the minimax search from that position.
When you encounter a position during search, you check the TT first. If it's
there, you skip the recursive search entirely.

### Why "transposition"?

In game theory, two sequences of moves that reach the same board position are
called "transpositions." The same physical board can be reached via many
different move orders. Without a TT, you'd recompute the same position hundreds
or thousands of times. With a TT, you compute it once and cache it.

### The packing trick — one `uint64_t` stores everything

```cpp
// bits  0–15 : score  (int16_t — a signed 16-bit integer)
// bits 16–17 : Bound  (0=INVALID, 1=EXACT, 2=LOWER, 3=UPPER)
// bits 18–57 : tag    (key >> 24, upper 40 bits of canonical key)
// bits 58–63 : unused

static uint64_t pack(uint64_t key, int score, Bound bound) {
    return ((key >> 24) << 18)
         | (static_cast<uint64_t>(static_cast<uint8_t>(bound)) << 16)
         | static_cast<uint16_t>(static_cast<int16_t>(score));
}
```

Everything about a cached result is compressed into one 64-bit integer.

- The **score** occupies bits 0–15. It's cast to `int16_t` (signed 16-bit int)
  then to `uint16_t` to put negative values in the lower bits without sign-
  extension issues.
- The **bound type** (2 bits) occupies bits 16–17.
- The **tag** (upper 40 bits of the key) occupies bits 18–57. It's used for
  verification: when reading from the TT, we check that the stored tag matches
  the key we're looking for, preventing false hits when two keys collide to the
  same table index.

### Alpha-beta bounds — EXACT, LOWER, UPPER

This is the most subtle TT concept. Alpha-beta pruning doesn't always produce
the exact best score — sometimes it only proves that the score is "at least X"
or "at most Y" before cutting off. Three types of result are stored:

| Bound | Meaning |
|---|---|
| `EXACT` | This IS the true minimax score for this position |
| `LOWER` | The true score is ≥ this value (a pruning short-cut) |
| `UPPER` | The true score is ≤ this value (a pruning short-cut) |

When you look up a position and find a cached result:
```cpp
if (cachedBound == Bound::EXACT) return cachedScore;         // use directly
if (cachedBound == Bound::LOWER) alpha = max(alpha, score);  // tighten window
if (cachedBound == Bound::UPPER) beta  = min(beta, score);   // tighten window
if (alpha >= beta) return cachedScore;                        // prune early
```

Even a non-EXACT cached result lets you narrow the search window faster, which
means more branches get pruned in the full search.

After the search, you record which type the result was:
```cpp
if      (best <= origAlpha) bound = Bound::UPPER;  // never improved alpha
else if (best >= origBeta)  bound = Bound::LOWER;  // cut-off: real score unknown
else                        bound = Bound::EXACT;  // stayed within window
```

### `std::atomic<uint64_t>` — lockless concurrent access

```cpp
struct TTEntry {
    std::atomic<uint64_t> data{0};
    ...
};
```

`std::atomic` guarantees that reads and writes to this variable are indivisible
— no thread can see a partially-written value. Without it, when multiple CPU
threads write to nearby memory simultaneously, you can get data corruption.

`std::memory_order_relaxed` allows the compiler maximum freedom to reorder
memory operations for performance, while still guaranteeing atomicity of the
individual read or write. For the TT this is correct: a "torn" read in a race
condition just gives you a stale or wrong entry, which the tag check will
catch and reject — not a crash.

This design is called a **lockless hash table**. Traditional thread safety uses
`mutex` (locks), which force threads to wait in line. Lockless tables let all
threads read and write simultaneously at the cost of occasionally reading an
inconsistent entry — which the validation logic handles gracefully.

### Table size: `1 << 24`

```cpp
static constexpr size_t tableSize = 1u << 24; // 16,777,216 entries × 8 bytes = 128 MB
```

`1 << 24` is 2²⁴ = 16,777,216. Using a power of 2 is intentional:

```cpp
size_t index = key & (tableSize - 1); // extremely fast modulo
```

`tableSize - 1` in binary is `0b111...111` (24 ones). ANDing with this is
equivalent to taking `key % tableSize` but uses a single bitwise operation
instead of a division. Division is 20–40× slower than AND on most CPUs.

---

## Part 5: The Minimax Algorithm — `Solver.hpp`

You already know minimax and alpha-beta. Here's how this version differs from
your Java implementation.

### The full function signature

```cpp
int minimax(Bitboard board, int depth, int alpha, int beta, bool isMaximizing,
            int maxDepth = std::numeric_limits<int>::max())
```

`std::numeric_limits<int>::max()` is the largest possible `int` value — used
as a default to mean "no depth limit." The argument `= std::numeric_limits<...>`
makes it optional: callers can omit it and get unlimited depth.

### The score encoding

```cpp
if (board.checkWin(1)) return  1000 - depth;
if (board.checkWin(2)) return -1000 + depth;
```

A win is scored as 1000 *minus* the depth. This means winning *sooner* gets a
higher score than winning later. An engine that finds a forced win in 3 moves
scores it higher than in 5 moves, so the engine will always choose the fastest
win — exactly what you want.

Similarly `-1000 + depth` means a loss *as late as possible* is preferred to
an immediate loss. This is standard minimax practice.

### Copying the board

```cpp
Bitboard next = board;
next.setPiece(m / board.size, m % board.size, 1);
int eval = minimax(next, depth + 1, ...);
```

`Bitboard next = board;` creates a copy of the entire board object (all three
`uint64_t` fields, the hash arrays, etc.). Because `uint64_t` is just a number,
this copy is fast — much faster than deep-copying a Java object hierarchy.
`setPiece` then modifies just the copy, leaving the original intact for trying
other moves in the loop.

### The heuristic slot

```cpp
std::function<int(const Bitboard&, bool)> heuristic = nullptr;
...
if (depth >= maxDepth) return heuristic ? heuristic(board, isMaximizing) : 0;
```

`std::function<int(const Bitboard&, bool)>` declares a variable that can hold
any callable — a function, lambda, or functor — that takes a `Bitboard` and a
`bool`, and returns an `int`. Setting it to `nullptr` means "no heuristic."

The ternary `heuristic ? ... : 0` checks if `heuristic` is set before calling
it. The calibration and book-generation tools set a heuristic; the WASM build
does not, so non-terminal depth-limit nodes return 0.

---

## Part 6: Multithreading — Native Build Only

### Why multithreading only in native?

Multithreading in WebAssembly requires `SharedArrayBuffer`, which browsers only
allow when specific security headers (`COOP` and `COEP`) are set. GitHub Pages
doesn't support custom headers, so the WASM build uses single-threaded search.
The native build (for calibration and book generation) uses all available cores.

### `#ifndef WASM_BUILD`

```cpp
#ifndef WASM_BUILD
// ... multithreaded code ...
#endif
```

`#ifndef` means "if this macro is NOT defined." The Makefile passes
`-DWASM_BUILD` when compiling for WASM, making `WASM_BUILD` defined. The
`#ifndef` block is then skipped by the compiler for WASM builds. This is
called **conditional compilation** — one codebase produces two different
programs depending on build-time flags.

### `std::async` and `std::future`

```cpp
futures.push_back(std::async(std::launch::async,
    [this, board, m, isX, maxDepth, &cout_mutex]() -> std::pair<int,int> {
        Bitboard next = board;
        next.setPiece(m / board.size, m % board.size, isX ? 1 : 2);
        int eval = minimax(next, 0, INT_MIN, INT_MAX, !isX, maxDepth);
        return {m, eval};
    }
));
```

`std::async(std::launch::async, callable)` runs `callable` in a new thread and
returns a `std::future<T>`. A future is a promise of a result that will be
available *sometime in the future* — the current thread can do other things
while the background thread computes.

The lambda captures `[this, board, m, isX, maxDepth, &cout_mutex]`:
- `this`: the `Solver` pointer (so the lambda can call `this->minimax`)
- `board`, `m`, `isX`, `maxDepth`: copied by value (each thread gets its own)
- `&cout_mutex`: captured by reference (shared across threads for safe output)

```cpp
for (auto& f : futures) {
    auto [move, score] = f.get();  // blocks until that thread finishes
    ...
}
```

`f.get()` waits for the thread to complete and retrieves the result. The
`auto [move, score] = ...` is a C++17 **structured binding** — it unpacks the
`std::pair<int,int>` into two named variables at once.

### Batch processing

```cpp
for (size_t i = 0; i < moves.size(); i += maxThreads) {
    size_t end = std::min(i + maxThreads, moves.size());
    // launch batch of threads, wait for all, then launch next batch
}
```

Rather than launching all 25 threads at once, moves are processed in batches
of `maxThreads` (equal to the number of CPU cores). This avoids thread
contention: beyond a certain point, more threads means more context-switching
overhead than computation. Batching keeps thread count ≤ core count.

---

## Part 7: WebAssembly and the WASM Bridge — `wasm_api.cpp`

### What is WebAssembly?

WebAssembly (WASM) is a compact binary format that browsers can execute at
near-native speed. Emscripten (`emcc`) compiles C++ source files into a
`.wasm` binary plus a JavaScript glue file (`.js`) that handles loading.
The result: C++ code running directly in the browser, with none of the
performance limitations of JavaScript.

### `extern "C"` — speaking JavaScript's language

```cpp
extern "C" {
    EMSCRIPTEN_KEEPALIVE
    int wasm_getBestMove(...) { ... }
}
```

C++ **mangles** function names — a function called `getBestMove` might become
`_ZN6Solver12getBestMoveE...` in the compiled binary. JavaScript can't easily
call a function with that name.

`extern "C"` disables name mangling for the enclosed functions, making them
accessible by their plain names (`wasm_getBestMove`) from JavaScript.

### `EMSCRIPTEN_KEEPALIVE`

Without this annotation, Emscripten might decide that a function isn't used by
C++ code directly and remove it from the output (dead code elimination).
`EMSCRIPTEN_KEEPALIVE` tells Emscripten: "this function IS used — from the
outside (JavaScript) — keep it."

### Passing the board from JavaScript to C++

```cpp
int wasm_getBestMove(int boardSize, int target, int* cells, int isX, int maxDepth)
```

JavaScript can't directly pass a `Bitboard` object to C++. Instead, it passes
a flat `int*` array: 25 integers, one per cell. The WASM bridge reconstructs
the `Bitboard` from that array:

```cpp
Bitboard board(boardSize, target);
for (int i = 0; i < boardSize * boardSize; i++)
    if (cells[i] != 0)
        board.setPiece(i / boardSize, i % boardSize, cells[i]);
```

On the JavaScript side, the board is written into a WASM heap buffer:

```javascript
let wasmBufPtr = engine._malloc(BOARD_SIZE * BOARD_SIZE * 4);  // 4 bytes per int
let wasmBuf    = new Int32Array(engine.HEAP32.buffer, wasmBufPtr, 25);

// Before calling wasm_getBestMove:
for (let i = 0; i < cells.length; i++) wasmBuf[i] = cells[i];
```

`engine._malloc` allocates memory in the WASM heap. `engine.HEAP32` is
Emscripten's view of that heap as 32-bit integers. This zero-copy approach
(write array directly into WASM memory, pass pointer) is much faster than
serializing data.

```javascript
engine.ccall('wasm_getBestMove', 'number',
    ['number','number','number','number','number'],
    [BOARD_SIZE, TARGET, wasmBufPtr, cpuPlayer === 1 ? 1 : 0, depth]);
```

`ccall` is Emscripten's helper for calling C functions from JavaScript.
Arguments: function name, return type, array of argument types, array of
argument values.

### The global solver `g_solver`

```cpp
static Solver* g_solver = nullptr;

void wasm_init() {
    if (!g_solver) g_solver = new Solver();
}
```

The `Solver` object contains the 128 MB transposition table. Creating it once
and keeping it alive for the session means the table warms up as the game
progresses — positions evaluated early are cached and reused for later moves.
Recreating the solver each call would reset the TT and lose all that warm-up.

`static` here means the variable has file scope — it exists for the lifetime
of the program and is invisible outside this translation unit. It's effectively
a module-level global without polluting the global namespace.

---

## Part 8: The Opening Book

### Why a book instead of live search?

With 25 empty cells, even D=8 search in WASM takes 1–3 seconds. The *best*
possible response to every first human move is always the same (determined by
our D=12 calibration). Pre-computing these responses offline and looking them
up in microseconds is strictly better.

### The gen_book5x5 generator

The tool in `tools/gen_book5x5.cpp` runs the full D=12 multithreaded engine on
every reachable board position for the first 6 plies (3 moves per side). It
builds a `std::map<std::string, int>` in memory, then writes it to JSON:

```json
{
  "searchDepth": 12,
  "entries": {
    "0000000000010000000000000": 12,
    ...
  }
}
```

Each key is a 25-character string (one character per cell: `0`, `1`, or `2`),
representing the **canonical** form of the board. The value is the flat index
of the best move in canonical coordinates.

### Canonical board strings and D₈ in JavaScript

The JavaScript `canonicalize()` function mirrors `getCanonicalState()` in C++,
but works with strings instead of hash values:

```javascript
function canonicalize(cells) {
    let bestStr = null, bestSym = 0;
    for (let sym = 0; sym < 8; sym++) {
        let s = '';
        for (let r = 0; r < 5; r++)
            for (let c = 0; c < 5; c++) {
                const origFlat = applySymmetry(INV_SYM[sym], r, c);
                s += cells[origFlat];
            }
        if (bestStr === null || s < bestStr) { bestStr = s; bestSym = sym; }
    }
    return { str: bestStr, sym: bestSym };
}
```

It tries all 8 transforms, builds the board string for each, and picks the
lexicographically smallest one. The `sym` value records *which* transform gave
the minimum — needed for the inverse lookup.

### Inverse transforms

The book stores a move in canonical coordinates. The actual board has been
rotated/reflected to produce the canonical form. To recover the physical move:

```javascript
const INV_SYM = [0, 3, 2, 1, 4, 5, 6, 7];  // inverse of each transform

const canonFlat = openingBook.entries[str];
const canonRow  = Math.floor(canonFlat / 5);
const canonCol  = canonFlat % 5;
return applySymmetry(INV_SYM[sym], canonRow, canonCol);
```

Rotations 90° and 270° are inverses of each other (sym 1↔3). 180° is its own
inverse (sym 2). All four reflections are self-inverse (sym 4,5,6,7 → same).

---

## Part 9: Adaptive Depth in JavaScript

### The problem

Searching 19 empty cells at D=12 would take many seconds in WASM. But 6 empty
cells at D=12 takes microseconds because the tree is tiny. The depth limit
should scale with how much of the tree actually exists.

```javascript
function adaptiveDepth(cells) {
    const empty = cells.filter(c => c === 0).length;
    if (empty >= 23) return 6;   // ≤2 moves made; book always covers this
    if (empty >= 17) return 8;   // book→live transition
    if (empty >= 13) return 10;  // mid game
    if (empty >= 9)  return 12;  // mid-late
    return empty;                // ≤8 left: search the whole remaining tree
}
```

`cells.filter(c => c === 0).length` counts how many cells are still 0 (empty).

The last line returns `empty` itself — when 8 or fewer cells remain, the
requested depth cap equals the maximum possible tree depth. It's equivalent to
D=∞ but written more honestly: "I want to search as deep as there are moves."

### The warm TT effect

The TT persists across moves within a game session. By move 10, the engine has
already searched many sub-trees of the current game. When the root position
changes by one move, large portions of the TT from previous searches are still
valid. This is why moves-10 onward (even at D=12) often complete in
milliseconds — the answer is already essentially cached.

---

## Part 10: Compile-Time Constants and `#pragma once`

### `#pragma once`

```cpp
#pragma once
```

This tells the compiler: "include this file only once, even if multiple source
files `#include` it." Without it, you'd need the traditional:

```cpp
#ifndef BITBOARD_HPP
#define BITBOARD_HPP
// ... file contents ...
#endif
```

`#pragma once` is simpler and supported by all major compilers.

### `constexpr`

```cpp
static constexpr size_t tableSize = 1u << 24;
```

`constexpr` means "constant expression — known at compile time." The compiler
replaces every use of `tableSize` with the literal value `16777216` during
compilation, just like a `#define` but type-safe. `size_t` is the type for
memory sizes — typically `uint64_t` on 64-bit systems.

### `std::numeric_limits<int>::max()`

This returns the largest value that an `int` can hold (2,147,483,647 on 32-bit
int systems). It's used as the initial "best score" for the minimizer (the
minimizer starts at the highest possible value and drives it down), and as the
default `maxDepth` meaning "no limit."

---

## Part 11: Putting it All Together

Here is the flow of a single computer move in the WASM game:

```
1. User clicks a cell
   └─ app.js: onCellClick() → cells[idx] = humanPlayer

2. Check game over (JS win detection using findWinLine)
   └─ If game continues → cpuTurn()

3. Check opening book (microseconds)
   └─ canonicalize(cells) → look up in openingBook.entries
   └─ If found: inverse transform the move index → use it (📖 Book)
   └─ If not found: proceed to live search

4. Live WASM search
   └─ adaptiveDepth(cells) → choose D=8..empty_cells
   └─ Copy cells[] into WASM heap buffer (wasmBuf)
   └─ engine.ccall('wasm_getBestMove', ..., [depth])
      ├─ wasm_api.cpp: reconstruct Bitboard from int array
      ├─ Solver.getBestMoveSingleThreaded()
      │   ├─ For each candidate move (center-first order):
      │   │   ├─ Clone board, setPiece()
      │   │   └─ minimax(next, depth=0, alpha, beta, !isX, maxDepth)
      │   │       ├─ Check win → return ±(1000 - depth)
      │   │       ├─ Check draw → return 0
      │   │       ├─ Check depth limit → return 0
      │   │       ├─ computeCanonicalState() → TT lookup
      │   │       │   └─ If EXACT: return cached score immediately
      │   │       │   └─ If LOWER/UPPER: tighten alpha/beta
      │   │       ├─ Recursively search children (alpha-beta cutoffs prune)
      │   │       └─ Store result in TT → return best score
      │   └─ Return move with best score
      └─ Return flat cell index
   └─ Record elapsed time → setMoveInfo('engine', `🔍 Engine · D=N · Xms`)

5. Apply move, render board, check game over
```

Every technique in the codebase is one layer of this pipeline, removing either
the number of nodes visited (D₈ symmetry, TT, alpha-beta, move ordering) or
the cost per node (bitboards, Zobrist hashing, lockless TT).

---

## Further Reading

These are the concepts this codebase draws from. If you want to go deeper on
any of them:

| Topic | What to look for |
|---|---|
| Bitboards | "Chess bitboard programming" — chess engines are the canonical reference |
| Zobrist hashing | [Wikipedia: Zobrist hashing](https://en.wikipedia.org/wiki/Zobrist_hashing) |
| Transposition tables | [Chess Programming Wiki: Transposition Table](https://www.chessprogramming.org/Transposition_Table) |
| D₈ symmetry | "Dihedral group" — group theory for the symmetries of a square |
| Alpha-beta bounds (UPPER/LOWER) | "Fail-soft alpha-beta" — the standard reference implementation |
| `std::atomic` | cppreference.com: `std::atomic` |
| Emscripten / WASM | [emscripten.org/docs](https://emscripten.org/docs/getting_started/Tutorial.html) |
| Opening books in game engines | "Chess opening book format" — the same concept applied to Go/Chess |
