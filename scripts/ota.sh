#!/usr/bin/env bash
# Flash the current build to the device over its own OTA endpoint, and wait for
# it to come back.
#
# Used instead of serial flashing because the WT32-ETH01 has no auto-reset
# wiring for boot mode, and because this exercises the update path the device
# actually ships with - a step that breaks OTA gets caught by the step that
# follows it rather than at the end of the sequence.
#
# The image must be signed with secure_boot_signing_key.pem, which `idf.py
# build` does; the bootloader rejects anything else. If a flashed image fails to
# boot, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE reverts to the previous one, so a
# bad config change costs a reboot rather than a trip to the serial cable.
#
# Credentials come from the environment, never the repo:
#     BRIDGE_USER (default admin), BRIDGE_PASS
#
# Usage: scripts/ota.sh [bridge-host]

set -uo pipefail

HOST="${1:-192.168.5.1}"
USER="${BRIDGE_USER:-admin}"
PASS="${BRIDGE_PASS:?set BRIDGE_PASS}"
IMAGE="build/wt32-bridge.bin"

[[ -f "$IMAGE" ]] || { echo "ota: no $IMAGE - run idf.py build first" >&2; exit 1; }

before="$(curl -s --max-time 10 -u "${USER}:${PASS}" "http://${HOST}/api/system")"
echo "ota: before - $(jq -r '"v\(.version), up \(.uptime_s)s, \(.cpu_freq_mhz)MHz"' <<<"$before")"
echo "ota: uploading $(stat -c%s "$IMAGE") bytes"

resp="$(curl -s --max-time 180 -u "${USER}:${PASS}" \
        -H 'Content-Type: application/octet-stream' \
        --data-binary "@${IMAGE}" "http://${HOST}/api/ota")"

if ! jq -e '.ok == true' >/dev/null 2>&1 <<<"$resp"; then
  echo "ota: upload refused: ${resp}" >&2
  exit 1
fi
echo "ota: accepted, waiting for reboot"

# The device answers {"ok":true} and only then restarts, so a poll started
# immediately can be answered by the *old* image still shutting down. Waiting
# for the endpoint to go away first makes "it came back" mean something.
for _ in $(seq 30); do
  sleep 1
  curl -s -o /dev/null --max-time 2 "http://${HOST}/api/status" || break
done

for i in $(seq 60); do
  sleep 2
  after="$(curl -s --max-time 5 -u "${USER}:${PASS}" "http://${HOST}/api/system" 2>/dev/null)"
  if jq -e '.uptime_s' >/dev/null 2>&1 <<<"$after"; then
    # Re-read after a settle rather than reporting this first answer. The
    # device serves requests before it has finished coming up, and a reply
    # caught in that window can be short of fields that are present a second
    # later - which showed up here as a blank status line.
    sleep 3
    after="$(curl -s --max-time 5 -u "${USER}:${PASS}" "http://${HOST}/api/system")"
    echo "ota: back after ${i} polls - $(jq -r '"v\(.version), up \(.uptime_s)s, \(.cpu_freq_mhz)MHz, heap \(.free_heap), drops \(.traffic_drops)"' <<<"$after")"
    exit 0
  fi
done

echo "ota: device did not come back - check serial, rollback may have fired" >&2
exit 1
