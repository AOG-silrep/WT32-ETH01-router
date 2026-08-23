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
for path in / /admin /wan /lan /logs /leases /resets; do
  check "GET ${path}" 200 "$(code "${AUTH[@]}" "http://${HOST}${path}")"
done

# --- JSON endpoints ---------------------------------------------------------
# Piped through jq -e, so malformed JSON fails here rather than silently in a
# browser. This is the check that would catch a response-buffer overflow.
for path in /api/status /api/clients /api/system /api/leases /api/resets "/api/logs?since=0" /api/syslog /api/wan; do
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

# The one list endpoint that is never legitimately empty: a device answering this
# request has by definition booted at least once, so "length > 0" is a stronger
# assertion here than it would be for leases or clients.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/resets" \
     | jq -e '(.max|type=="number") and (.count|type=="number")
              and (.resets|type=="array") and (.resets|length > 0)
              and (.resets|all(has("seq") and has("reason") and has("partition")
                              and has("uptime_s") and has("uptime_approx")
                              and has("uptime_max_s") and has("rail_held")
                              and (.rail_held|type=="boolean" or .==null)))' \
     >/dev/null 2>&1; then
  check "GET /api/resets shape" ok ok
else
  check "GET /api/resets shape" ok bad
fi

# jq -e . above passes on JSON that is valid but incomplete, and this is now the
# only thing standing between a dropped boot_* field and a blank Diagnostics row
# on the dashboard. has() rather than a type test for the uptime, because null is
# the correct value there after a power cycle.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/system" \
     | jq -e '(.boot_seq|type=="number") and (.boot_reason|type=="string")
              and (.boot_intent|type=="string") and has("boot_prev_uptime_s")
              and (.boot_prev_uptime_approx|type=="boolean")
              and has("boot_prev_uptime_max_s")
              and has("boot_prev_ready") and (.boot_rollback|type=="boolean")' \
     >/dev/null 2>&1; then
  check "GET /api/system boot fields" ok ok
else
  check "GET /api/system boot fields" ok bad
fi

# Same reasoning as the boot fields above: these three drive the Diagnostics
# panel's duplicate-address row, and a dropped field there would read as "no
# conflicts" rather than as a fault. ip_conflicts is a count of conflicts live
# right now, so 0 is the normal answer on a healthy bridge and this only checks
# the shape.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/system" \
     | jq -e '(.ip_conflicts|type=="number") and (.ip_conflict_drops|type=="number")
              and (.ip_conflict_enforced|type=="boolean")' \
     >/dev/null 2>&1; then
  check "GET /api/system conflict fields" ok ok
else
  check "GET /api/system conflict fields" ok bad
fi

# The syslog counters are what tell a fault from an idle sender, and a dropped
# field there reads as "nothing wrong" rather than as a gap. Types only, not
# values: a device with syslog switched off is the normal case, not a failure.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/syslog" \
     | jq -e '(.enabled|type=="boolean") and (.server|type=="string")
              and (.port|type=="number") and (.facility|type=="number")
              and (.min_severity|type=="number") and (.hostname|type=="string")
              and (.subnet|type=="string") and (.bound|type=="boolean")
              and (.sent|type=="number") and (.aged_out|type=="number")
              and (.send_fail|type=="number") and (.backlog|type=="number")' \
     >/dev/null 2>&1; then
  check "GET /api/syslog shape" ok ok
else
  check "GET /api/syslog shape" ok bad
fi

# The uplink's filter counters are the only evidence that traffic is being
# dropped on purpose rather than lost, so a missing field there reads as "the
# port list is not doing anything". Types only: a device with no uplink
# configured is the normal case, not a failure.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/wan" \
     | jq -e '(.enabled|type=="boolean") and (.ssid|type=="string")
              and (.ports|type=="string") and (.state|type=="string")
              and (.lan|type=="string") and (.napt|type=="boolean")
              and (.channel|type=="number") and (.retry_in_s|type=="number")
              and (.tx_allowed|type=="number") and (.tx_blocked|type=="number")
              and (.rx_allowed|type=="number") and (.rx_blocked|type=="number")' \
     >/dev/null 2>&1; then
  check "GET /api/wan shape" ok ok
else
  check "GET /api/wan shape" ok bad
fi

# The password must never leave the device, in either direction. This is the
# check that catches somebody adding it to the GET for the convenience of the
# form - see wan_get_handler().
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/wan" \
     | jq -e 'has("password")|not' >/dev/null 2>&1; then
  check "GET /api/wan withholds password" ok ok
else
  check "GET /api/wan withholds password" ok bad
fi

# The same counters ride on /api/logs, which is where the log page reads them
# from - so they can go missing there while /api/syslog stays intact.
if curl -s --max-time 10 "${AUTH[@]}" "http://${HOST}/api/logs?since=0" \
     | jq -e '(.syslog_enabled|type=="boolean") and (.syslog_sent|type=="number")
              and (.syslog_aged|type=="number") and (.syslog_failed|type=="number")
              and (.syslog_errno|type=="number") and (.syslog_backlog|type=="number")' \
     >/dev/null 2>&1; then
  check "GET /api/logs syslog fields" ok ok
else
  check "GET /api/logs syslog fields" ok bad
fi

# --- log download -----------------------------------------------------------
# Deliberately not piped through jq: this is the one route that is not JSON.
check "GET /api/logs/download" 200 \
  "$(code "${AUTH[@]}" "http://${HOST}/api/logs/download")"

# The header is what makes a browser save this as a file rather than render it,
# and it is silently droppable - the body looks identical either way.
if curl -s -D- -o /dev/null --max-time 10 "${AUTH[@]}" \
     "http://${HOST}/api/logs/download" | grep -qi '^content-disposition:.*attachment'; then
  check "download sets Content-Disposition" ok ok
else
  check "download sets Content-Disposition" ok bad
fi

# A device answering this has by definition booted, so the header block alone
# guarantees more than ten lines - the same reasoning as the /api/resets length
# check above. This is what catches a chunked response that terminated early.
DL_LINES="$(curl -s --max-time 10 "${AUTH[@]}" \
            "http://${HOST}/api/logs/download" | wc -l)"
if [[ "$DL_LINES" -gt 10 ]]; then
  check "download body has content" ok ok
else
  check "download body has content" ok "bad (${DL_LINES} lines)"
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

# --- syslog gate ------------------------------------------------------------
# Negative paths only, deliberately. A POST that actually enabled syslog would
# leave a test device shipping its log to whatever address this script invented,
# and the settings survive a reboot. The 415 and 403 never reach the save path;
# the 400 is a request guaranteed to be rejected before anything is written.
check "POST /api/syslog no Content-Type -> 415" 415 \
  "$(code "${AUTH[@]}" -X POST -d '{"enabled":0}' "http://${HOST}/api/syslog")"
check "POST /api/syslog foreign Origin -> 403" 403 \
  "$(code "${AUTH[@]}" -X POST -H 'Content-Type: application/json' \
     -H 'Origin: http://evil.example' -d '{"enabled":0}' "http://${HOST}/api/syslog")"
# The on-subnet rule is the one piece of validation a user will actually hit,
# and it is the entire reason this feature has a validator.
check "POST /api/syslog off-subnet -> 400" 400 \
  "$(code "${AUTH[@]}" -X POST -H 'Content-Type: application/json' \
     -d '{"enabled":1,"server":"10.0.0.5"}' "http://${HOST}/api/syslog")"

echo "smoke: ${pass} passed, ${fail} failed"
[[ "$fail" -eq 0 ]]
