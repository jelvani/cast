#!/usr/bin/env bash
# run_cast_test.sh — used by CTest to check one .cast example
# Usage: run_cast_test.sh <castc> <file.cast> [<expected_output>]
#
# Stages:
#   1. cast compile  — castc --tb [--duration=100] <file.cast>  (exit code must be 0)
#   2. sv elaborate  — iverilog <output.sv>                     (exit code must be 0)
#   3. simulate+diff — vvp <sim> | filter | diff <expected>     (only if expected given)
# Output from each stage is shown only on failure.

set -euo pipefail

CASTC="$1"
CAST_FILE="$2"
EXPECTED="${3:-}"
BASE="$(basename "$CAST_FILE" .cast)"
SV_OUT="/tmp/cast_check_${BASE}.sv"
SIM_OUT="/tmp/cast_check_${BASE}_sim"
ACTUAL_OUT="/tmp/cast_check_${BASE}_actual"

fail() { echo "FAIL [$BASE]: $*" >&2; exit 1; }

# When simulating for correctness, bake in a fixed duration so vvp terminates.
if [[ -n "$EXPECTED" && -f "$EXPECTED" ]]; then
    CASTC_FLAGS="--tb --duration=100"
    STAGES="3"
else
    CASTC_FLAGS="--tb"
    STAGES="2"
fi

# ── stage 1: cast → verilog ───────────────────────────────────────────────────
echo "  [1/$STAGES] cast compile: $CAST_FILE"
# shellcheck disable=SC2086
if ! "$CASTC" $CASTC_FLAGS "$CAST_FILE" > "$SV_OUT" 2>/tmp/cast_check_${BASE}_cast_err; then
    echo "--- castc stderr ---"
    cat /tmp/cast_check_${BASE}_cast_err >&2
    fail "castc exited with non-zero status"
fi

# ── stage 2: verilog elaboration ──────────────────────────────────────────────
echo "  [2/$STAGES] iverilog elaborate: $SV_OUT"
if ! iverilog -g2012 -gno-assertions \
        -o "$SIM_OUT" "$SV_OUT" \
        2>/tmp/cast_check_${BASE}_iv_err; then
    echo "--- iverilog stderr ---"
    cat /tmp/cast_check_${BASE}_iv_err >&2
    fail "iverilog exited with non-zero status"
fi

# ── stage 3: simulate and verify output ───────────────────────────────────────
if [[ -n "$EXPECTED" && -f "$EXPECTED" ]]; then
    echo "  [3/3] simulate + verify: $(basename "$EXPECTED")"
    # Strip the VCD banner line and the $finish timing line — both are
    # simulator noise rather than program output.
    vvp "$SIM_OUT" 2>/dev/null \
        | grep -v '^VCD info:' \
        | grep -v '\$finish called' \
        > "$ACTUAL_OUT"

    if ! diff -u "$EXPECTED" "$ACTUAL_OUT" > /tmp/cast_check_${BASE}_diff 2>&1; then
        echo "--- expected vs actual (unified diff) ---" >&2
        cat /tmp/cast_check_${BASE}_diff >&2
        fail "simulation output does not match expected"
    fi
fi

echo "  PASS: $BASE"
