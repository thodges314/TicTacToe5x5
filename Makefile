CXX      = clang++
CXXFLAGS = -O3 -std=c++20 -Wall -Wextra -march=native -mtune=native

# ── Locate emsdk ─────────────────────────────────────────────────────────────
EMCC := $(shell command -v emcc 2>/dev/null)
ifeq ($(EMCC),)
  EMCC := $(HOME)/emsdk/upstream/emscripten/emcc
endif

EMFLAGS = -O3 -std=c++20 -DWASM_BUILD -I. \
          -s EXPORTED_FUNCTIONS='["_wasm_init","_wasm_getBestMove","_wasm_checkWin","_wasm_isDraw","_malloc","_free"]' \
          -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAP32"]' \
          -s ALLOW_MEMORY_GROWTH=1 \
          -s INITIAL_MEMORY=268435456 \
          -s MODULARIZE=1 \
          -s EXPORT_NAME='createEngineModule'

.PHONY: all book wasm serve run run-quiet test test-cpp test-js clean

# ── Default: build everything ─────────────────────────────────────────────────
all: tools/calibrate tools/gen_book5x5

# ── Calibration tool ──────────────────────────────────────────────────────────
tools/calibrate: tools/calibrate.cpp engine/Bitboard.hpp engine/Solver.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

# ── Opening book generator ────────────────────────────────────────────────────
# Outputs JSON to stdout, thread logs to stderr.
# Expected runtime: 3-5 hours on M2 Studio (12 cores, D=12 search).
tools/gen_book5x5: tools/gen_book5x5.cpp engine/Bitboard.hpp engine/Solver.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

# Background book generation — survives terminal disconnect.
# Progress streams to results/gen_book5x5.log AND to your terminal live.
# Ctrl-C stops the tail but NOT the background process.
book: tools/gen_book5x5
	@mkdir -p public results
	@echo "=== Generating opening book (D=14, 6-ply) ==="
	@echo "    Progress: results/gen_book5x5.log"
	@echo "    Output:   public/opening_book5x5.json"
	@echo "    Monitor:  tail -f results/gen_book5x5.log"
	nohup sh -c './tools/gen_book5x5 >public/opening_book5x5.json 2>results/gen_book5x5.log' &
	@echo "Started PID: $$!  (following log — Ctrl-C stops tail, not the process)"
	tail -f results/gen_book5x5.log

# ── WASM build ────────────────────────────────────────────────────────────────
wasm: engine/wasm_api.cpp engine/Bitboard.hpp engine/Solver.hpp
	@mkdir -p public
	$(EMCC) $(EMFLAGS) engine/wasm_api.cpp -o public/engine.js
	@echo "Built: public/engine.js + public/engine.wasm"

# ── Local dev server ──────────────────────────────────────────────────────────
# Serves project root so /public/engine.wasm and index.html both work.
# Open: http://localhost:8080/
serve:
	@echo "Serving at http://localhost:8080/"
	python3 -m http.server 8080

# ── Calibration shortcuts ─────────────────────────────────────────────────────
run: tools/calibrate
	./tools/calibrate

run-quiet: tools/calibrate
	./tools/calibrate 2>/dev/null

# Tees stdout to a log file and follows it live — lets you see per-move progress.
# Open a second terminal and run:  tail -f results/calibrate.log
run-monitor: tools/calibrate
	@mkdir -p results
	./tools/calibrate 2>/dev/null | tee results/calibrate.log

# ── Win-detection unit tests ─────────────────────────────────────────────────
tools/test_win: tools/test_win.cpp engine/Bitboard.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

test-cpp: tools/test_win
	./tools/test_win

test-js:
	/opt/homebrew/bin/node tools/test_win.js

test: test-cpp test-js

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f tools/calibrate tools/gen_book5x5 tools/test_win public/engine.js public/engine.wasm
