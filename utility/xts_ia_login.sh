#!/usr/bin/env bash
# utility/xts_ia_login.sh v1.0 — daily XTS Interactive login (SGInterActiveApp) -> Redis.
#
# Step 1: open the printed URL in a browser and log in; the page returns
#         {"session":"{\"accessToken\":\"...\",\"UserID\":\"WBS18\",...}"}
# Step 2: paste that response (or just the accessToken). The script POSTs /interactive/user/session,
#         stores the session token + userID in Redis and checks them with /user/profile.
#
# The app key/secret are read from Redis (susanta:xts_ia_app_key / susanta:xts_ia_secret_key);
# they are never written in this file. Secrets are passed to curl/redis-cli on stdin, not argv.
#
# Usage: utility/xts_ia_login.sh [accessToken | response JSON]
#        XTS_KEYS=nifty utility/xts_ia_login.sh      # SGNiftyInteractiveApp key set
set -euo pipefail

HOST="https://xts.compositedge.com"
ENV_FILE="$HOME/.config/xts-nautilas/redis.env"
KEYSET="${XTS_KEYS:-}"
[[ "$KEYSET" == "default" || "$KEYSET" == "crude" ]] && KEYSET=""
P="susanta:xts_ia_${KEYSET:+${KEYSET}_}"
K_APP="${P}app_key" K_SECRET="${P}secret_key" K_TOKEN="${P}token" K_USER="${P}user_id"

die() { echo "xts_ia_login: $*" >&2; exit 1; }
for c in redis-cli curl jq; do command -v "$c" >/dev/null || die "$c not installed"; done

# ---- Redis
[[ -f "$ENV_FILE" ]] || die "missing $ENV_FILE"
set -a; . "$ENV_FILE"; set +a
rc() { redis-cli -n 0 "$@"; }
[[ "$(rc ping 2>/dev/null)" == "PONG" ]] || die "cannot reach Redis (wrong password?)"
APP_KEY="$(rc get "$K_APP")"
SECRET="$(rc get "$K_SECRET")"
[[ -n "$APP_KEY" && -n "$SECRET" ]] || die "app key/secret missing in Redis ($K_APP, $K_SECRET)"

# ---- Step 1: accessToken from the browser login
INPUT="${1:-}"
if [[ -z "$INPUT" ]]; then
  echo "Step 1: open this URL in a browser and log in:"
  echo
  echo "  $HOST/interactive/thirdparty?&appKey=$APP_KEY&returnURL=$HOST/interactive/testapi#!/logIn"
  echo
  read -r -p "Step 2: paste the response (or the accessToken): " INPUT
fi
# Accepts the full {"session":"{...}"} reply, the inner JSON, or the bare token.
ACCESS="$(jq -r '((.session | fromjson?) // .session // .) | .accessToken? // empty' <<<"$INPUT" 2>/dev/null || true)"
[[ -n "$ACCESS" ]] || ACCESS="$(tr -d '[:space:]' <<<"$INPUT")"
[[ "$ACCESS" =~ ^[0-9a-fA-F]{32,}$ ]] || die "no accessToken found in the input"

# ---- Step 2: session token
BODY="$(jq -nc --arg s "$SECRET" --arg a "$APP_KEY" --arg t "$ACCESS" \
  '{secretKey: $s, appKey: $a, source: "WebAPI", accessToken: $t}')"
RESP="$(curl -sS -m 15 -H 'Content-Type: application/json' --data-binary @- \
  "$HOST/interactive/user/session" <<<"$BODY")" || die "session call failed (network)"
[[ "$(jq -r '.type // empty' <<<"$RESP")" == "success" ]] \
  || die "session refused: $(jq -c '{code, description}' <<<"$RESP" 2>/dev/null || echo "$RESP" | head -c 300)"
TOKEN="$(jq -r '.result.token // empty' <<<"$RESP")"
USER_ID="$(jq -r '.result.userID // empty' <<<"$RESP")"
[[ -n "$TOKEN" && -n "$USER_ID" ]] || die "session reply has no token/userID"

printf '%s' "$TOKEN" | rc -x set "$K_TOKEN" >/dev/null
printf '%s' "$USER_ID" | rc -x set "$K_USER" >/dev/null

# ---- Verify
PROFILE="$(curl -sS -m 15 -H @- "$HOST/interactive/user/profile" <<<"authorization: $TOKEN")" || die "profile call failed (network)"
[[ "$(jq -r '.type // empty' <<<"$PROFILE")" == "success" ]] \
  || die "token stored but profile check failed: $(jq -c '{code, description}' <<<"$PROFILE")"
CLIENT="$(jq -r '.result.ClientId // "?"' <<<"$PROFILE")"
MCX="$(jq -r '.result.ClientExchangeDetailsList.MCXFO.Enabled // "?"' <<<"$PROFILE")"
NSE="$(jq -r '.result.ClientExchangeDetailsList.NSEFO.Enabled // "?"' <<<"$PROFILE")"
echo "OK: Interactive session for $USER_ID stored in Redis ($K_TOKEN)"
echo "    client $CLIENT, MCXFO enabled: $MCX, NSEFO enabled: $NSE"
