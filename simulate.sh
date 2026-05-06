#!/usr/bin/env bash
# simulate.sh — compile a .cast file and simulate it with iverilog
# Usage:
#   ./simulate.sh <file.cast> [--duration=<ns>] [--vcd=<path>]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CASTC="$SCRIPT_DIR/build/castc"

if [[ $# -lt 1 || "$1" == "--help" || "$1" == "-h" ]]; then
    echo "Usage: $0 <file.cast> [--duration=<ns>] [--vcd=<path>]"
    exit 0
fi

CAST_FILE="$1"; shift

if [[ ! -f "$CAST_FILE" ]]; then
    echo "error: file not found: $CAST_FILE"
    exit 1
fi
if [[ ! -x "$CASTC" ]]; then
    echo "error: castc not found at $CASTC — run 'ninja -C build' first"
    exit 1
fi

BASE="$(basename "$CAST_FILE" .cast)"
SV_OUT="/tmp/${BASE}.sv"
SIM_OUT="/tmp/${BASE}_sim"

# ── compile ───────────────────────────────────────────────────────────────────
# Any remaining args (--duration=, --vcd=) are forwarded to castc via "$@".
echo "==> compiling $CAST_FILE"
"$CASTC" --tb "$@" "$CAST_FILE" > "$SV_OUT"
echo "    verilog written to $SV_OUT"

# ── elaborate ─────────────────────────────────────────────────────────────────
echo "==> elaborating"
iverilog -g2012 -gno-assertions -o "$SIM_OUT" "$SV_OUT"

# ── simulate ──────────────────────────────────────────────────────────────────
echo "==> simulating"
vvp "$SIM_OUT"
