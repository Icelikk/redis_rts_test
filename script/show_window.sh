#!/usr/bin/env bash
set -euo pipefail

REDIS_HOST="${REDIS_HOST:-redis}"
TAG="${1:-0}"

WINDOW_MS=$((10*60*1000))
BUCKET_MS=20000

last_ms="$(redis-cli -h "$REDIS_HOST" GET rts:last_ms | tr -d '\r')"
if [[ -z "$last_ms" || "$last_ms" == "(nil)" ]]; then
  echo "rts:last_ms empty (worker ещё не обработал данные?)"
  exit 1
fi

end_bucket=$(( ((last_ms - BUCKET_MS) / BUCKET_MS) * BUCKET_MS ))
start_ms=$(( end_bucket - WINDOW_MS + BUCKET_MS ))

echo "tag=$TAG last_ms=$last_ms start_ms=$start_ms end_bucket=$end_bucket"
echo "points expected: $((WINDOW_MS / BUCKET_MS))"
echo

echo "AVG (20s):"
redis-cli -h "$REDIS_HOST" TS.RANGE "ts:avg:20s:$TAG" "$start_ms" "$end_bucket"
echo

echo "MIN (20s):"
redis-cli -h "$REDIS_HOST" TS.RANGE "ts:min:20s:$TAG" "$start_ms" "$end_bucket"
echo

echo "MAX (20s):"
redis-cli -h "$REDIS_HOST" TS.RANGE "ts:max:20s:$TAG" "$start_ms" "$end_bucket"
echo

echo "COUNT (20s):"
redis-cli -h "$REDIS_HOST" TS.RANGE "ts:cnt:20s:$TAG" "$start_ms" "$end_bucket"
echo

echo "CURRENT (hash current_values):"
redis-cli -h "$REDIS_HOST" HGET light:current_values "$TAG"