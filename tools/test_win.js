#!/usr/bin/env node
// ============================================================================
// test_win.js — unit tests for the JS findWinLine logic
//
// Runs with plain Node (no Jest, no dependencies):
//   node tools/test_win.js
//
// Tests all four win directions, near-misses, cross-player correctness,
// and the exact board from the anti-diagonal bug report.
// ============================================================================

const BOARD_SIZE = 5;
const TARGET     = 5;

// Pure reimplementation of findWinLine (identical logic to app.js).
// Takes cells[] and player (1=X, 2=O); returns winning flat indices or [].
function findWinLine(cells, player) {
  const line = [];
  const check = indices => {
    if (indices.every(i => cells[i] === player)) { line.push(...indices); return true; }
    return false;
  };
  // Rows
  for (let r = 0; r < BOARD_SIZE; r++)
    for (let c = 0; c <= BOARD_SIZE - TARGET; c++)
      if (check(Array.from({length: TARGET}, (_, i) => r * BOARD_SIZE + c + i))) return line;
  // Columns
  for (let c = 0; c < BOARD_SIZE; c++)
    for (let r = 0; r <= BOARD_SIZE - TARGET; r++)
      if (check(Array.from({length: TARGET}, (_, i) => (r + i) * BOARD_SIZE + c))) return line;
  // Diagonal ↘
  for (let r = 0; r <= BOARD_SIZE - TARGET; r++)
    for (let c = 0; c <= BOARD_SIZE - TARGET; c++)
      if (check(Array.from({length: TARGET}, (_, i) => (r + i) * BOARD_SIZE + c + i))) return line;
  // Diagonal ↙
  for (let r = TARGET - 1; r < BOARD_SIZE; r++)
    for (let c = 0; c <= BOARD_SIZE - TARGET; c++)
      if (check(Array.from({length: TARGET}, (_, i) => (r - i) * BOARD_SIZE + c + i))) return line;
  return line;
}

// Build a flat 25-cell board from lists of X and O indices.
function board(Xindices, Oindices = []) {
  const c = new Array(25).fill(0);
  Xindices.forEach(i => c[i] = 1);
  Oindices.forEach(i => c[i] = 2);
  return c;
}

let pass = 0, fail = 0;

function test(name, cells, player, expectWin) {
  const line   = findWinLine(cells, player);
  const won    = line.length > 0;
  const ok     = won === expectWin;
  const symbol = ok ? 'PASS' : 'FAIL';
  const detail = ok ? '' : `  ← expected ${expectWin ? 'win' : 'no-win'}, got ${won ? 'win (cells '+line+')' : 'no-win'}`;
  console.log(`  ${symbol}  ${name}${detail}`);
  ok ? pass++ : fail++;
}

// ── Rows ─────────────────────────────────────────────────────────────────────
console.log('-- Rows --');
test('X wins row 0',              board([0,1,2,3,4]),         1, true);
test('X wins row 2',              board([10,11,12,13,14]),    1, true);
test('X wins row 4',              board([20,21,22,23,24]),    1, true);
test('O wins row 1',              board([], [5,6,7,8,9]),     2, true);
test('O wins row 3',              board([], [15,16,17,18,19]),2, true);
test('X no win — 4 in row',       board([0,1,2,3]),           1, false);

// ── Columns ──────────────────────────────────────────────────────────────────
console.log('\n-- Columns --');
test('X wins col 0',              board([0,5,10,15,20]),      1, true);
test('X wins col 2',              board([2,7,12,17,22]),      1, true);
test('X wins col 4',              board([4,9,14,19,24]),      1, true);
test('O wins col 0',              board([], [0,5,10,15,20]),  2, true);
test('O wins col 4',              board([], [4,9,14,19,24]),  2, true);
test('X no win — 4 in col',       board([0,5,10,15]),         1, false);

// ── Diagonal ↘ ───────────────────────────────────────────────────────────────
console.log('\n-- Diagonal ↘ --');
test('X wins main diag ↘',        board([0,6,12,18,24]),     1, true);
test('O wins main diag ↘',        board([], [0,6,12,18,24]), 2, true);
test('X no win — 4 in diag↘',    board([0,6,12,18]),         1, false);

// ── Anti-diagonal ↙ ──────────────────────────────────────────────────────────
console.log('\n-- Anti-diagonal ↙ (the bug we fixed) --');
// Cells: (0,4)=4  (1,3)=8  (2,2)=12  (3,1)=16  (4,0)=20
test('X wins anti-diag ↙',        board([4,8,12,16,20]),     1, true);
test('O wins anti-diag ↙',        board([], [4,8,12,16,20]), 2, true);
test('X no win — 4 in anti-diag', board([4,8,12,16]),        1, false);
test('X no win — wrong shape',    board([3,8,12,16,20]),      1, false);

// ── Cross-player correctness ──────────────────────────────────────────────────
console.log('\n-- Cross-player --');
test('X row present, O no win',   board([0,1,2,3,4], [5,6,7,8]),   2, false);
test('O anti-diag, X no win',     board([5,6,7,8], [4,8,12,16,20]),1, false);

// ── Exact board from the bug report ──────────────────────────────────────────
// Row 0: X X O O X  → flat 0,1=X  2,3=O  4=X
// Row 1: O O O X X  → flat 5,6,7=O  8,9=X
// Row 2: O X X O O  → flat 10=O  11,12=X  13,14=O
// Row 3: X X O O X  → flat 15,16=X  17,18=O  19=X
// Row 4: X O O X X  → flat 20=X  21,22=O  23,24=X
// X anti-diagonal (4,8,12,16,20) — was wrongly declared a Draw.
console.log('\n-- Bug-report board --');
const bugBoard = board(
  [0,1,4,  8,9,  11,12,  15,16,19,  20,23,24],  // X
  [2,3,    5,6,7, 10,13,14, 17,18,   21,22]       // O
);
test('Bug board: X wins (anti-diagonal)', bugBoard, 1, true);
test('Bug board: O does NOT win',         bugBoard, 2, false);

// ── Empty board ───────────────────────────────────────────────────────────────
console.log('\n-- Edge cases --');
test('Empty board: no X win',     new Array(25).fill(0), 1, false);
test('Empty board: no O win',     new Array(25).fill(0), 2, false);

// ── Summary ──────────────────────────────────────────────────────────────────
console.log(`\n=== ${pass} passed, ${fail} failed ===`);
process.exit(fail > 0 ? 1 : 0);
