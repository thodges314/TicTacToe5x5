// ============================================================================
// app.js — 5×5 Tic-Tac-Toe game controller
//
// Architecture (mirrors 4×4 project, updated for 5×5):
//   1. WASM engine loaded via createEngineModule()
//   2. Opening book fetched from public/opening_book5x5.json
//      - Keys: 25-char canonical board strings (lex-min over 8 D₈ transforms)
//      - Values: best move flat index in canonical board coordinates
//   3. D₈ symmetry used to map actual board → canonical for lookup,
//      then inverse transform maps canonical response → actual move
//   4. Adaptive search depth: deeper when fewer empty cells remain
//   5. Sound effects via Web Audio API (same as 4x4)
// ============================================================================

// ── Sound FX (same design as 4x4) ──────────────────────────────────────────
const SoundFX = {
  ctx: null,
  init() {
    if (!this.ctx) this.ctx = new (window.AudioContext || window.webkitAudioContext)();
    if (this.ctx.state === 'suspended') this.ctx.resume();
  },
  playTone(freq, type, duration, vol = 0.08, sweep = 0) {
    if (!this.ctx) return;
    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();
    osc.type = type;
    osc.frequency.setValueAtTime(freq, this.ctx.currentTime);
    if (sweep) osc.frequency.exponentialRampToValueAtTime(freq * sweep, this.ctx.currentTime + duration);
    gain.gain.setValueAtTime(vol, this.ctx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.01, this.ctx.currentTime + duration);
    osc.connect(gain); gain.connect(this.ctx.destination);
    osc.start(); osc.stop(this.ctx.currentTime + duration);
  },
  playHumanMove() { this.playTone(600, 'sine', 0.1); },
  playCpuMove()   { this.playTone(300, 'triangle', 0.15); },
  playWin() {
    [0, 100, 200].forEach((d, i) =>
      setTimeout(() => this.playTone(400 + i * 100, 'sine', 0.3), d));
  },
  playLoss() { this.playTone(280, 'sawtooth', 0.5, 0.08, 0.5); },
  playDraw() {
    this.playTone(200, 'square', 0.4, 0.04);
    setTimeout(() => this.playTone(200, 'square', 0.4, 0.04), 200);
  },
};

// ── Constants ───────────────────────────────────────────────────────────────
const BOARD_SIZE = 5;
const TARGET     = 5;
const SZ         = BOARD_SIZE - 1;   // used in D₈ transform formulas

// ── D₈ symmetry helpers ─────────────────────────────────────────────────────
// Matches Bitboard::applySymmetry() exactly (same formulas, same sym indices).
// Returns flat index in a board of dimension BOARD_SIZE.
function applySymmetry(sym, r, c) {
  switch (sym) {
    case 0: return  r         * BOARD_SIZE +  c;
    case 1: return  c         * BOARD_SIZE + (SZ - r);
    case 2: return (SZ - r)   * BOARD_SIZE + (SZ - c);
    case 3: return (SZ - c)   * BOARD_SIZE +  r;
    case 4: return  r         * BOARD_SIZE + (SZ - c);
    case 5: return (SZ - r)   * BOARD_SIZE +  c;
    case 6: return  c         * BOARD_SIZE +  r;
    case 7: return (SZ - c)   * BOARD_SIZE + (SZ - r);
  }
}

// Inverse of each D₈ transform: reflections self-inverse, 90°/270° swap.
const INV_SYM = [0, 3, 2, 1, 4, 5, 6, 7];

// Compute canonical board string (25 chars, '0'/'1'/'2') and which sym gave it.
// The canonical string is the lex-min over all 8 D₈ views of the board.
// For transform sym, cell (r,c) in the transformed view = original cell T⁻¹(r,c).
function canonicalize(cells) {
  let bestStr = null, bestSym = 0;
  for (let sym = 0; sym < 8; sym++) {
    const invSym = INV_SYM[sym];
    let s = '';
    for (let r = 0; r < BOARD_SIZE; r++) {
      for (let c = 0; c < BOARD_SIZE; c++) {
        const origFlat = applySymmetry(invSym, r, c);
        s += cells[origFlat];
      }
    }
    if (bestStr === null || s < bestStr) { bestStr = s; bestSym = sym; }
  }
  return { str: bestStr, sym: bestSym };
}

// Look up current board in opening book.
// Returns actual-board flat index of the computer's response, or -1 if not found.
function openingBookLookup(cells) {
  if (!openingBook) return -1;
  const { str, sym } = canonicalize(cells);
  const canonFlat = openingBook.entries?.[str];
  if (canonFlat === undefined) return -1;

  // canonFlat is the move in canonical coordinates.
  // Apply inverse transform to recover actual-board coordinates.
  const canonRow = Math.floor(canonFlat / BOARD_SIZE);
  const canonCol = canonFlat % BOARD_SIZE;
  return applySymmetry(INV_SYM[sym], canonRow, canonCol);
}

// ── Adaptive depth ──────────────────────────────────────────────────────────
// Uses deeper search as the board empties (fewer cells = smaller search tree).
// Calibrated to stay within ~1-2s per move in WASM single-threaded:
//   D=6: ~200ms  (early game, book usually covers this)
//   D=8: ~800ms  (early-mid)
//   D=10: ~300ms (mid, smaller tree)
//   D=12+: fast  (late game)
function adaptiveDepth(cells) {
  const empty = cells.filter(c => c === 0).length;
  // D=6 only when ≤2 moves made (≥23 empty) — always covered by opening book.
  // The book→live transition zone (17–22 empty, 3–8 moves made) uses D=8
  // to close the most exploitable tactical gap without excessive WASM latency.
  if (empty >= 23) return 6;   // ≤2 moves made; opening book always covers this
  if (empty >= 17) return 8;   // book transition + early-mid game (~1–3s WASM)
  if (empty >= 13) return 10;  // mid game (~300ms WASM, TT warm)
  if (empty >= 9)  return 12;  // mid-late game (~100ms WASM)
  return 20;                   // endgame: full depth, trivially fast
}

// ── State ───────────────────────────────────────────────────────────────────
let engine      = null;         // WASM module
let openingBook = null;         // parsed JSON book
let engineReady = false;
let cells       = new Array(BOARD_SIZE * BOARD_SIZE).fill(0);
let humanPlayer = 0;
let cpuPlayer   = 0;
let gameOver    = false;

// WASM board buffer: Int32Array allocated in WASM heap for zero-copy passing
let wasmBuf     = null;
let wasmBufPtr  = null;

// ── DOM refs ────────────────────────────────────────────────────────────────
const sideSelect   = document.getElementById('side-select');
const boardWrap    = document.getElementById('board-wrap');
const boardEl      = document.getElementById('board');
const statusBar    = document.getElementById('status-bar');
const moveInfo     = document.getElementById('move-info');
const replayBtn    = document.getElementById('replay-btn');
const engineStatus = document.getElementById('engine-status');
const subtitle     = document.getElementById('subtitle');

document.getElementById('btn-x').addEventListener('click', () => startGame(1));
document.getElementById('btn-o').addEventListener('click', () => startGame(2));
replayBtn.addEventListener('click', resetToSideSelect);

// ── Board rendering ─────────────────────────────────────────────────────────
function buildBoard() {
  boardEl.innerHTML = '';
  for (let i = 0; i < BOARD_SIZE * BOARD_SIZE; i++) {
    const cell = document.createElement('div');
    cell.className = 'cell';
    cell.id = `cell-${i}`;
    cell.setAttribute('role', 'gridcell');
    cell.addEventListener('click', () => onCellClick(i));
    boardEl.appendChild(cell);
  }
}

function renderBoard(winLine = []) {
  for (let i = 0; i < BOARD_SIZE * BOARD_SIZE; i++) {
    const el = document.getElementById(`cell-${i}`);
    if (!el) continue;
    const v = cells[i];
    el.className = 'cell';
    el.textContent = '';
    el.removeAttribute('aria-label');

    if (v === 1)      { el.classList.add('x'); el.textContent = 'X'; }
    else if (v === 2) { el.classList.add('o'); el.textContent = 'O'; }
    else if (!gameOver) { el.classList.add('hoverable'); }

    if (winLine.includes(i)) el.classList.add('winner');
  }
}

function markThinking() {
  for (let i = 0; i < BOARD_SIZE * BOARD_SIZE; i++) {
    if (cells[i] === 0) document.getElementById(`cell-${i}`)?.classList.add('thinking');
  }
}

function unmarkThinking() {
  document.querySelectorAll('.cell.thinking').forEach(el => el.classList.remove('thinking'));
}

function setStatus(msg, cls = '') {
  statusBar.className = 'status-bar' + (cls ? ' ' + cls : '');
  statusBar.textContent = msg;
}

// Update the move-source indicator. mode: 'book' | 'engine' | '' (clear)
function setMoveInfo(mode, text) {
  moveInfo.classList.remove('update');
  void moveInfo.offsetWidth; // force reflow to restart animation
  moveInfo.className = 'move-info' + (mode ? ` ${mode} update` : '');
  moveInfo.textContent = text;
}

// ── Win / draw detection ────────────────────────────────────────────────────
function syncWasmBuffer() {
  if (!wasmBuf) return;
  for (let i = 0; i < cells.length; i++) wasmBuf[i] = cells[i];
}

function checkGameOver(lastPlayer) {
  syncWasmBuffer();
  if (engine.ccall('wasm_checkWin', 'number',
      ['number','number','number','number'],
      [BOARD_SIZE, TARGET, wasmBufPtr, lastPlayer])) {

    gameOver = true;
    // Find the winning line by testing all possible lines
    const winLine = findWinLine(lastPlayer);
    renderBoard(winLine);
    const human = lastPlayer === humanPlayer;
    if (human) SoundFX.playWin(); else SoundFX.playLoss();
    setStatus(human ? 'You win! 🎉' : 'Computer wins', lastPlayer === 1 ? 'x-wins' : 'o-wins');
    replayBtn.hidden = false;
    return true;
  }
  if (engine.ccall('wasm_isDraw', 'number', ['number','number'], [BOARD_SIZE, wasmBufPtr])) {
    gameOver = true;
    renderBoard();
    SoundFX.playDraw();
    setStatus('Draw — well played!', 'draw');
    replayBtn.hidden = false;
    return true;
  }
  return false;
}

// Find winning line indices for highlighting
function findWinLine(player) {
  const line = [];
  const check = indices => {
    if (indices.every(i => cells[i] === player)) { line.push(...indices); return true; }
    return false;
  };
  // Rows
  for (let r = 0; r < BOARD_SIZE; r++) {
    for (let c = 0; c <= BOARD_SIZE - TARGET; c++) {
      const idx = Array.from({length: TARGET}, (_, i) => r * BOARD_SIZE + c + i);
      if (check(idx)) return line;
    }
  }
  // Columns
  for (let c = 0; c < BOARD_SIZE; c++) {
    for (let r = 0; r <= BOARD_SIZE - TARGET; r++) {
      const idx = Array.from({length: TARGET}, (_, i) => (r + i) * BOARD_SIZE + c);
      if (check(idx)) return line;
    }
  }
  // Diagonal ↘
  for (let r = 0; r <= BOARD_SIZE - TARGET; r++) {
    for (let c = 0; c <= BOARD_SIZE - TARGET; c++) {
      const idx = Array.from({length: TARGET}, (_, i) => (r + i) * BOARD_SIZE + c + i);
      if (check(idx)) return line;
    }
  }
  // Diagonal ↙
  for (let r = TARGET - 1; r < BOARD_SIZE; r++) {
    for (let c = 0; c <= BOARD_SIZE - TARGET; c++) {
      const idx = Array.from({length: TARGET}, (_, i) => (r - i) * BOARD_SIZE + c + i);
      if (check(idx)) return line;
    }
  }
  return line;
}

// ── CPU turn ────────────────────────────────────────────────────────────────
function applyMove(idx, player) {
  cells[idx] = player;
}

async function cpuTurn() {
  if (gameOver) return;
  setStatus('Thinking\u2026', 'cpu-turn');
  markThinking();
  await new Promise(r => setTimeout(r, 30)); // yield to browser for UI update

  const t0 = performance.now();

  // 1. Check opening book first (instant lookup)
  let move = openingBookLookup(cells);
  let moveSource = 'book';
  let usedDepth = null;

  // 2. Fall back to live WASM engine with adaptive depth
  if (move < 0 || move >= BOARD_SIZE * BOARD_SIZE || cells[move] !== 0) {
    syncWasmBuffer();
    const empty = cells.filter(c => c === 0).length;
    usedDepth = adaptiveDepth(cells);
    const effectiveDepth = Math.min(usedDepth, empty); // can't exceed remaining cells
    moveSource = 'engine';
    move = engine.ccall('wasm_getBestMove', 'number',
      ['number','number','number','number','number'],
      [BOARD_SIZE, TARGET, wasmBufPtr, cpuPlayer === 1 ? 1 : 0, effectiveDepth]);
    usedDepth = effectiveDepth;
  }

  const ms = Math.round(performance.now() - t0);

  // Update move-source indicator
  if (moveSource === 'book') {
    setMoveInfo('book', '\u{1F4D6} Opening book');
  } else {
    setMoveInfo('engine', `\u{1F50D} Engine \u00B7 D=${usedDepth} \u00B7 ${ms}\u202Fms`);
  }

  unmarkThinking();
  if (move < 0) return; // shouldn't happen

  applyMove(move, cpuPlayer);
  SoundFX.playCpuMove();
  renderBoard();

  if (!checkGameOver(cpuPlayer)) {
    setStatus('Your turn', 'your-turn');
  }
}

// ── Human cell click ────────────────────────────────────────────────────────
function onCellClick(idx) {
  if (!engineReady || gameOver || cells[idx] !== 0) return;

  applyMove(idx, humanPlayer);
  SoundFX.playHumanMove();
  renderBoard();
  if (checkGameOver(humanPlayer)) return;

  setStatus('Thinking…', 'cpu-turn');
  setTimeout(cpuTurn, 60);
}

// ── Game lifecycle ──────────────────────────────────────────────────────────
function startGame(human) {
  SoundFX.init();
  humanPlayer = human;
  cpuPlayer   = human === 1 ? 2 : 1;
  cells.fill(0);
  gameOver = false;

  subtitle.textContent = `Depth-12 AI · 6-ply opening book · Play as ${human === 1 ? 'X' : 'O'}`;
  setMoveInfo('', '');  // clear indicator at new game start

  sideSelect.hidden = true;
  boardWrap.hidden  = false;
  replayBtn.hidden  = true;

  buildBoard();
  renderBoard();

  if (cpuPlayer === 1) {
    setStatus('Thinking…', 'cpu-turn');
    setTimeout(cpuTurn, 80);
  } else {
    setStatus('Your turn — you go first', 'your-turn');
  }
}

function resetToSideSelect() {
  boardWrap.hidden  = true;
  sideSelect.hidden = false;
  subtitle.textContent = 'Depth-12 AI · 6-ply opening book · Play as —';
}

// ── Engine init ─────────────────────────────────────────────────────────────
async function initEngine() {
  try {
    // Load WASM module
    engine = await createEngineModule();
    engine.ccall('wasm_init', null, [], []);

    // Allocate persistent board buffer in WASM heap
    wasmBufPtr = engine._malloc(BOARD_SIZE * BOARD_SIZE * 4);  // Int32
    wasmBuf    = new Int32Array(engine.HEAP32.buffer, wasmBufPtr, BOARD_SIZE * BOARD_SIZE);

    // Load opening book (may be absent or still generating — graceful fallback)
    try {
      const resp = await fetch('public/opening_book5x5.json');
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const text = await resp.text();
      if (!text.trim()) throw new Error('file is empty (still generating)');
      openingBook = JSON.parse(text);
      const n = Object.keys(openingBook.entries || {}).length;
      engineStatus.textContent = `Engine ready · book: ${n} positions`;
      console.log(`Book loaded: ${n} entries, searchDepth=${openingBook.searchDepth}`);
    } catch (e) {
      openingBook = null;
      engineStatus.textContent = 'Engine ready (book generating…)';
      console.info('Book unavailable:', e.message, '— live engine only.');
    }

    engineStatus.className = 'engine-status ready';
    engineReady = true;

  } catch (err) {
    engineStatus.textContent = `Engine failed: ${err.message}`;
    engineStatus.className = 'engine-status error';
    console.error('Engine init error:', err);
  }
}

initEngine();
