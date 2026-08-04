# Investigation brief: TCP downlink collapses to ~1.4 Mbit/s

Written to hand this problem to a fresh session. It is self-contained: symptom,
rig, everything already eliminated and how, and where to pick up. Nothing here
is remembered — every number is from a recorded run in [`bench/`](../bench) or
from a transcript of the command that produced it.

**Status: unresolved.** The bridge is confirmed at fault. Nine hypotheses have
been tested and eliminated. The leading untested suspect is the lwIP bridge
layer itself.

---

## Symptom

Wired client → bridge → WiFi client, plain TCP:

| Direction | Throughput | Retransmitted | Congestion window |
| --- | --- | --- | --- |
| WiFi → wired (up) | **27.0 Mbit/s** (spread 0.03) | — | healthy |
| wired → WiFi (down) | **1.14 Mbit/s** (spread 0.58) | 437 KB | collapses to **1 segment** |

A ~24× asymmetry. CPU during this is **8% / 8%**, so the device is nowhere near
compute-bound — this is not something a faster clock or better codegen fixes,
which is why `-O2` and 240 MHz were explicitly dropped from the work plan.

Reproduced on every one of nine gate runs across five different builds
(`bench/*/summary.json`, `medians.down_mbits`): 0.64–1.38 Mbit/s throughout,
`down_cwnd_min` = 1 in every single run.

## Test rig

- **Wired client**: Linux box on the bridge's Ethernet side, `192.168.5.2`.
- **WiFi client**: an ESP32 running the stock ESP-IDF `iperf` example, driven
  over UART. Joins as `192.168.5.4`.
- **Both ends need iperf 2.x.** The ESP-IDF example does not speak the iperf3
  protocol.
- Opening the ESP32's serial port **reboots it** (CP2102N drives EN from RTS;
  clearing DTR/RTS before open and `stty -hupcl` were both tried and neither
  prevents it). So one serial session must carry every command, and the peer
  must be re-joined to the AP after each open. `scripts/gate.py`'s `Peer` class
  handles this.

Reproduce the headline number:

```sh
BRIDGE_PASS=<password> scripts/gate.py --label repro --runs 3
```

## Ruled out

Each was applied, measured, and reverted. None moved `down_mbits`.

| Hypothesis | How it was tested | Result |
| --- | --- | --- |
| WiFi TX buffers too small | `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` 32→64, rebuilt | 1.05 Mbit/s (`bench/01-wifi-tx-64`) |
| Block-ack window too small | `CONFIG_ESP_WIFI_TX_BA_WIN` 6→32, rebuilt | 1.4 Mbit/s, retrans 681 KB |
| TCP burst arrival at line rate | `iperf -b` pacing the sender to 5/10/15/20 Mbit/s | 0.96–1.56 Mbit/s at every rate |
| Ethernet DMA ring (only ~3 full frames: 10 × 512 B, and a 1514 B frame spans 3) | MSS swept 1440→1000→700→400→200, so frames span 3→1 buffers | 0.33–0.9 Mbit/s, no trend |
| Concurrent reverse traffic | UDP downlink 10 Mbit/s + uplink ping flood from the peer | downlink unaffected: 10.01 / 10.00 / 10.00 |
| This project's accounting hooks | `netif->input` / `netif->linkoutput` wrappers **not installed at all** (bisect build) | 0.8 Mbit/s, cwnd `[6,6,2,2,1]` |
| Rate mismatch (100 Mbit ingress → ~27 Mbit egress) | see the three discriminating facts below | not the cause |
| Device crashing or rebooting | uptime polled through runs | climbs steadily, no resets |
| Peer is a poor TCP receiver | **same peer, same binary, different AP** | **64.0 Mbit/s, 0 retransmits, cwnd steady 48** |

That last row is the one that proves the bridge is at fault. The ESP32 joined a
different access point (`AOG internet`, routed via `192.168.7.1`) and did
64 Mbit/s cleanly. Its TCP receive path is fine.

## The three facts that constrain any explanation

Any theory has to account for all three simultaneously.

1. **UDP over the identical path is clean.** Offered vs received at the peer:
   2 M → 2.01/2.00, 5 M → 5.01/5.00, 10 M → 10.06/10.00, 20 M → 19.89/19.87.
   Offering 40 M yields ~27 received, which is simply the WiFi ceiling. So the
   path is not lossy, and it sustains 20× the traffic TCP manages.

2. **ICMP is clean *during* a TCP collapse.** 400 pings, wired → WiFi, run
   concurrently with a collapsing TCP transfer: **400/400 replies, 0 out of
   order, 0 duplicates, 0% loss.** Same direction, same moment, same path.

3. **The bridge forwards what it is handed.** Its own per-client counters,
   sampled through a downlink run, show ingress ≈ egress at every point —
   1.39 Mb/s in against 1.37 Mb/s out, 1.85 → 1.83, and so on. The bridge is
   not swallowing the traffic; the sender is stalled and never offers more.

Note the tension in (1) and (2): a layer-2 bridge cannot distinguish TCP from
UDP, yet only TCP fails. Whatever the mechanism is, it must be triggered by
something about the *pattern* of a TCP flow rather than by the frames
themselves — or it must not be in frame forwarding at all.

## Sender-side detail

From `ss -tiom` on the Linux sender during a paced-5 Mbit/s downlink transfer
(full transcript reproducible with the snippet in "Next steps"):

```
cwnd:5 ssthresh:5 bytes_sent:532888 bytes_retrans:167774 ... rtt:4.682/0.691
  rwnd_limited:920ms(30.8%) retrans:1/117 lost:36 reordering:45 reord_seen:5
cwnd:1 ssthresh:4 bytes_sent:2329274 bytes_retrans:846014 ... rto:208
  rwnd_limited:2860ms(23.8%) sacked:26 reordering:29 reord_seen:10
  delivery_rate 59.7kbps  minrtt:2.691
```

Salient points:

- **36% of all bytes retransmitted.**
- `rto:832 backoff:2` seen mid-run — these are retransmission *timeouts*, not
  just fast-retransmits, meaning whole windows disappear rather than isolated
  segments.
- `reordering:45`, `reord_seen:10`, `sacked:26` — Linux detected genuine
  reordering, and DSACK told it some of its retransmits had been **spurious**,
  i.e. the data did arrive. That points partly at the ACK path, not only the
  data path.
- `rwnd_limited` 24–31% — the receiver's advertised window is also a brake a
  quarter of the time.
- RTT is fine throughout (`minrtt:2.691`, ~5 ms under load).

Measured separately: uplink loses **1% of pings** while a 20 Mbit/s downlink
runs (197/200, versus 200/200 idle). Real, but not obviously enough on its own
to explain a 24× collapse.

Also measured: TCP that **terminates on the bridge** (120 sequential
`GET /logs`) shows **no retransmits at all**. So the device's own Ethernet
ingress and TCP stack are healthy; the fault is in forwarding.

## Leading hypothesis

**The lwIP bridge layer** — `esp_netif_br_glue` and lwIP's `bridgeif` — is the
one major component on the eth→WiFi path that has not been eliminated. It is
also the component a stock access point does not have, and a stock access point
does 64 Mbit/s.

The decisive experiment is to **take the bridge out of the path**: bring the
WiFi AP up as an ordinary SoftAP with its own subnet and DHCP server (no
`esp_netif_br_glue`, no bridge netif), and measure TCP downlink to the same peer.

- If it is fast → the bridge layer is confirmed, and the fix is either
  architectural or an upstream ESP-IDF/lwIP bug worth reporting.
- If it is still ~1.4 Mbit/s → the fault is below the bridge, in the WiFi
  driver's TX path as this firmware drives it, and the bridge is exonerated.

This is a bigger change than a config tweak, which is why it was not attempted
in the previous session — it needs a scratch branch, not a one-line edit.

## Other things worth trying

Cheaper than the above, roughly in order of expected value:

- **Get a packet capture.** Everything so far is inference from endpoint
  counters. `tcpdump` on the wired side needs root, which was not available in
  the previous session; with it you could see directly whether retransmitted
  segments were ever ACKed, and whether ACKs are lost or merely late.
- **A second, non-ESP32 WiFi client** (laptop or phone running iperf). It would
  confirm the fault is not an interaction specific to this peer. The user's
  other WiFi device is already associated as `192.168.5.77` but nothing runs on
  it.
- **`CONFIG_ESP_WIFI_RX_BA_WIN`** was left at 6; only the TX side was tried.
  The peer advertises 32–64.
- **`esp_wifi_set_ps(WIFI_PS_NONE)`** is never called; the default is
  `WIFI_PS_MIN_MODEM`. Effect in pure-AP mode is limited but it is untried.
- **A power-saving station stalling the AP's queues.** `192.168.5.77` is
  associated throughout every test and was never controlled for.

## Already fixed along the way (do not re-investigate)

The AP was running an 802.11w SA Query against re-associating stations and
**disassociating them every few seconds** on a loop, because PMF is on by
default and `pmf_cfg.capable` is documented as "deprecated and set to true
internally" — so leaving the field alone does not leave PMF alone. Fixed in
`wifi_cfg_apply()` with `esp_wifi_disable_pmf_config()` (commit `a0b950e`).

That was a real fault — packet loss under load went 0.5% → 0, and worst-case
latency spread across runs 5.8 ms → 0.2 ms — but it is **not** this one. The
downlink was unchanged by it (`bench/01-pmf-off`: 1.38 Mbit/s). Mentioned here
only so the SA Query lines in old logs are not chased again.

## Harness

| Script | Use |
| --- | --- |
| `scripts/gate.py` | the measurement gate; median of N runs into `bench/<label>/` |
| `scripts/smoke.sh` | functional regression (pages, JSON, auth, CSRF) |
| `scripts/ota.sh` | flash over `/api/ota`; refuses stale images |
| `scripts/serial_cmd.py` | drive a serial console (one invocation = one session) |

`bench/00-baseline` is the reference. `BRIDGE_PASS` comes from the environment.

For ad-hoc probing, the pattern used throughout was to import the harness
directly:

```python
import sys, time, subprocess
sys.path.insert(0, "scripts")
from gate import Bridge, Peer, iperf_mbits, run

peer = Peer("/dev/ttyUSB2")
ip = peer.join("AOG hub", "password")     # re-join after the open-reset
peer.send("iperf -s"); time.sleep(3)
out = run(f"iperf -c {ip} -t 12 -i 6", timeout=60)
print(iperf_mbits(out))
peer.close()
```

Reading the bridge's own view of a transfer — which is what showed ingress ≈
egress — is `Bridge(...).clients()`, polled while traffic runs.
