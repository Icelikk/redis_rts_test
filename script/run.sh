#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$APP_DIR/build}"

LOADER_BIN="${LOADER_BIN:-$BUILD_DIR/loader}"
WORKER_BIN="${WORKER_BIN:-$BUILD_DIR/worker}"

LOADER_LOG=/tmp/loader.log
WORKER_LOG=/tmp/worker.log
LOADER_PID=/tmp/loader.pid
WORKER_PID=/tmp/worker.pid

stop_pid() {
  local pidfile="$1"
  if [[ -f "$pidfile" ]]; then
    local pid
    pid="$(cat "$pidfile" || true)"
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
  fi
}

echo "[stop] kill old worker/loader..."
stop_pid "$WORKER_PID"
stop_pid "$LOADER_PID"
echo "OK"

if [[ ! -x "$WORKER_BIN" || ! -x "$LOADER_BIN" ]]; then
  echo "[err] binaries not found or not executable:"
  echo "  WORKER_BIN=$WORKER_BIN"
  echo "  LOADER_BIN=$LOADER_BIN"
  echo ""
  echo "Build them inside container:"
  echo "  cd $BUILD_DIR && cmake .. && make -j\$(nproc)"
  exit 1
fi

: > "$WORKER_LOG"
: > "$LOADER_LOG"

echo "[start] worker..."
nohup "$WORKER_BIN" >"$WORKER_LOG" 2>&1 &
echo $! > "$WORKER_PID"

echo "[start] loader..."
nohup "$LOADER_BIN" >"$LOADER_LOG" 2>&1 &
echo $! > "$LOADER_PID"

echo "started."
echo "logs:"
echo "  tail -n 80 $WORKER_LOG"
echo "  tail -n 80 $LOADER_LOG"
echo "quick check:"
echo "  redis-cli -h redis XLEN sensor_data"
echo "  redis-cli -h redis HLEN light:current_values"
echo "  redis-cli -h redis GET rts:last_ms"