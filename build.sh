#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${JOBS:-2}"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${PREFIX:-/usr/local}"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
printf '\nBuild complete: %s\n' "$BUILD_DIR/reedbrowser"
