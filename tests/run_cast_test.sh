#!/usr/bin/env bash
# run_cast_test.sh — used by CTest to check one .cast example
# Usage: run_cast_test.sh <castc> <file.cast>
#
# Two stages are tested:
#   1. cast compile  — castc --tb <file.cast>  (exit code must be 0)
#   2. sv elaborate  — iverilog <output.sv>    (exit code must be 0)
# Output from each stage is shown only on failure.

set -euo pipefail

CASTC="$1"
CAST_FILE="$2"
BASE="$(basename "$CAST_FILE" .cast)"
SV_OUT="/tmp/cast_check_${BASE}.sv"
SIM_OUT="/tmp/cast_check_${BASE}_sim"

fail() { echo "FAIL [$BASE]: $*" >&2; exit 1; }

# ── stage 1: cast → verilog ───────────────────────────────────────────────────
echo "  [1/2] cast compile: $CAST_FILE"
if ! "$CASTC" --tb "$CAST_FILE" > "$SV_OUT" 2>/tmp/cast_check_${BASE}_cast_err; then
    echo "--- castc stderr ---"
    cat /tmp/cast_check_${BASE}_cast_err >&2
    fail "castc exited with non-zero status"
fi

# ── stage 2: verilog elaboration ──────────────────────────────────────────────
echo "  [2/2] iverilog elaborate: $SV_OUT"
if ! iverilog -g2012 -gno-assertions \
        -o "$SIM_OUT" "$SV_OUT" \
        2>/tmp/cast_check_${BASE}_iv_err; then
    echo "--- iverilog stderr ---"
    cat /tmp/cast_check_${BASE}_iv_err >&2
    fail "iverilog exited with non-zero status"
fi

echo "  PASS: $BASE"
