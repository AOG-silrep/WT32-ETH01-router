#!/usr/bin/env bash
# Functional regression half of the gate: does the device still behave, not
# just still go fast. Run after every change, alongside scripts/gate.py.
#
# Checks the things a throughput or buffer change can plausibly break - the
# pages still render, every JSON endpoint still parses, and the two gates in
# front of the API (Basic auth, and the CSRF content-type/Origin pair) still
# refuse what they are supposed to refuse. A change that doubles throughput and
# quietly drops the 415 is not an improvement.
#
# Credentials come from the environment, never the repo:
#     BRIDGE_USER (default admin), BRIDGE_PASS
#
# Usage: scripts/smoke.sh [bridge-host]

set -uo pipefail

HOST="${1:-192.168.5.1}"
USER="${BRIDGE_USER:-admin}"
PASS="${BRIDGE_PASS:?set BRIDGE_PASS}"
AUTH=(-u "${USER}:${PASS}")

pass=0
fail=0

# Reports one check. Keeping the expected value in the output matters: a failure
# here is read by someone comparing against a previous run, and "want 415 got
# 200" is a diagnosis where "FAIL csrf" is only a prompt to go and re-run it by
# hand.
check() {
  local label="$1" want="$2" got="$3"
  if [[ "$got" == "$want" ]]; then
    printf '  ok    %-34s %s\n' "$label" "$got"
    pass=$((pass + 1))
  else
    printf '  FAIL  %-34s want %s, got %s\n' "$label" "$want" "$got"
    fail=$((fail + 1))
  fi
}

code() { curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$@"; }

echo "smoke: ${HOST} as ${USER}"

# --- pages ------------------------------------------------------------------
for path in / /admin /logs /leases; do
  check "GET ${path}" 200 "$(code "${AUTH[@]}" "http://${HOST}${path}")"
done

# --- JSON endpoints ---------------------------------------------------------
# Piped through jq -e, so malformed JSON fails here rather than silently in a
# browser. This is the check that would catch a response-buffer overflow.
for path in /api/status /api/clients /api/system /api/leases "/api/logs?since=0"; do
  if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}${path}" | jq -e . >/dev/null 2>&1; then
    check "GET ${path} parses" ok ok
  else
    check "GET ${path} parses" ok bad
  fi
done

# /api/leases is the one list endpoint that returns an object rather than a bare
# array, and its table can hold twice as many entries as /api/clients - so the
# shape is checked explicitly. A truncated response would have failed to parse
# above; this catches a field going missing while the JSON stays valid.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/leases" \
     | jq -e '(.max|type=="number") and (.restored|type=="number")
              and (.leases|type=="array")
              and (.leases|all(has("mac") and has("saved_ip") and has("stored")))' \
     >/dev/null 2>&1; then
  check "GET /api/leases shape" ok ok
else
  check "GET /api/leases shape" ok bad
fi

# /api/client/history needs a real client, so it is driven off whoever the
# device currently lists rather than a hardcoded MAC.
MAC="$(curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/clients" \
       | jq -r '.[0].mac // empty')"
if [[ -n "$MAC" ]]; then
  if curl -s --max-time 10 "${AUTH[@]}" \
       "http://${HOST}/api/client/history?mac=${MAC}" | jq -e . >/dev/null 2>&1; then
    check "GET /api/client/history parses" ok ok
  else
    check "GET /api/client/history parses" ok bad
  fi
else
  echo "  skip  /api/client/history            (no clients listed)"
fi

# --- authentication ---------------------------------------------------------
check "no credentials -> 401" 401 "$(code "http://${HOST}/api/status")"
check "bad credentials -> 401" 401 "$(code -u "${USER}:definitely-wrong" \
                                       "http://${HOST}/api/status")"

# --- CSRF gate --------------------------------------------------------------
# The state-changing POSTs must refuse a request this device's own pages did not
# make. Sent to /api/logs/level because it is the only one that changes nothing
# a later check depends on - and it is deliberately given a *valid* body, so a
# 200 here would mean the gate opened, not that the request was malformed.
check "POST no Content-Type -> 415" 415 \
  "$(code "${AUTH[@]}" -X POST -d '{"level":"info"}' \
     "http://${HOST}/api/logs/level")"
check "POST foreign Origin -> 403" 403 \
  "$(code "${AUTH[@]}" -X POST -H 'Content-Type: application/json' \
     -H 'Origin: http://evil.example' -d '{"level":"info"}' \
     "http://${HOST}/api/logs/level")"
check "POST correct headers -> 200" 200 \
  "$(code "${AUTH[@]}" -X POST -H 'Content-Type: application/json' \
     -d '{"level":"info"}' "http://${HOST}/api/logs/level")"
check "POST unknown level -> 400" 400 \
  "$(code "${AUTH[@]}" -X POST -H 'Content-Type: application/json' \
     -d '{"level":"nonsense"}' "http://${HOST}/api/logs/level")"

echo "smoke: ${pass} passed, ${fail} failed"
[[ "$fail" -eq 0 ]]
