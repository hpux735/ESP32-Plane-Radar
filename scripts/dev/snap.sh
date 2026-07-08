#!/usr/bin/env bash
# Grab the actual SDL emulator framebuffer as a PNG.
#
# The emulator auto-dumps its own SDL texture (i.e. the exact bytes being
# displayed) to /tmp/plane-radar-screenshot.ppm every ~200 ms. This script
# waits for a fresh frame and converts to PNG for the debug harness.
#
# Zero macOS permissions required — the emulator reads its own SDL texture
# with SDL_LockTexture. No screencapture, no accessibility hooks.
#
# Usage:
#   scripts/dev/snap.sh                    # capture (launches emulator if not running)
#   scripts/dev/snap.sh /path/to/out.png   # custom output
set -euo pipefail

PPM=/tmp/plane-radar-screenshot.ppm
OUT="${1:-/tmp/plane-radar-screenshot.png}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO_ROOT/.pio/build/native/program"

launched=0
if ! pgrep -f "$BIN" >/dev/null; then
  if [[ ! -x "$BIN" ]]; then
    echo "snap: $BIN not built. Run: pio run -e native" >&2
    exit 2
  fi
  rm -f "$PPM"
  "$BIN" >/dev/null 2>&1 &
  launched=1
  for _ in {1..40}; do
    [[ -s "$PPM" ]] && break
    sleep 0.1
  done
fi

# Wait for a fresh frame (mtime within the last second).
if [[ -f "$PPM" ]]; then
  cutoff=$(($(date +%s) - 1))
  for _ in {1..40}; do
    mtime=$(stat -f %m "$PPM")
    (( mtime >= cutoff )) && break
    sleep 0.05
  done
fi

if [[ ! -s "$PPM" ]]; then
  echo "snap: no frame captured at $PPM" >&2
  (( launched )) && pkill -f "$BIN" || true
  exit 1
fi

sips -s format png "$PPM" --out "$OUT" >/dev/null

if (( launched )); then
  pkill -f "$BIN" || true
fi

echo "$OUT"
