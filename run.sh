#!/usr/bin/env bash
# run.sh v1.1 — start cpp_edge with the Redis password loaded and basic checks done.
# Usage: ./run.sh [config/config.json]
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

CONFIG="${1:-config/config.json}"
ENV_FILE="$HOME/.config/xts-nautilas/redis.env"
BIN="build/cpp_edge"

die() { echo "run.sh: $*" >&2; exit 1; }

# 1. Redis password
[[ -f "$ENV_FILE" ]] || die "missing $ENV_FILE"
set -a; . "$ENV_FILE"; set +a
[[ -n "${REDISCLI_AUTH:-}" ]] || die "REDISCLI_AUTH not set in $ENV_FILE"

# 2. Config
[[ -f "$CONFIG" ]] || die "missing config $CONFIG"
cfg() { sed -nE "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"?([^\",}]+)\"?.*/\1/p" "$CONFIG" | head -1; }
REDIS_HOST="$(sed -nE '/"redis"/,/}/ s/.*"host"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' "$CONFIG")"
REDIS_PORT="$(cfg port)"
REDIS_DB="$(cfg db)"
IA_TOKEN_KEY="$(cfg ia_token)"
IA_USER_KEY="$(cfg ia_user_id)"
LIVE="$(cfg live_orders_enabled)"

# 3. Redis reachable and today's Interactive session present
rc() { redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" -n "$REDIS_DB" "$@"; }
[[ "$(rc ping 2>/dev/null)" == "PONG" ]] || die "cannot reach Redis at $REDIS_HOST:$REDIS_PORT (wrong password?)"
[[ "$(rc exists "$IA_TOKEN_KEY")" == "1" ]] || die "no Interactive token in Redis ($IA_TOKEN_KEY): do the daily XTS login first"
[[ "$(rc exists "$IA_USER_KEY")" == "1" ]] || die "no Interactive user id in Redis ($IA_USER_KEY)"

# 4. Binary
if [[ ! -x "$BIN" ]]; then
  echo "run.sh: $BIN not found, building..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release $(command -v ninja >/dev/null && echo "-G Ninja")
  cmake --build build -j"$(nproc)"
fi

echo "run.sh: starting $BIN with $CONFIG (live orders: $LIVE)"
exec "$BIN" "$CONFIG"
