#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MOVE_ANYTHING_SRC="${MOVE_ANYTHING_SRC:-$ROOT_DIR/../move-anything/src}"
BIN="$ROOT_DIR/build/tests/test_eucalypso_drumpad_mode"

mkdir -p "$(dirname "$BIN")"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$MOVE_ANYTHING_SRC" \
  -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/test_eucalypso_drumpad_mode.c" \
  "$ROOT_DIR/src/dsp/eucalypso.c" \
  -o "$BIN" \
  -lm

"$BIN"
