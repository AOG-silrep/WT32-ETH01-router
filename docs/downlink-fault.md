# Investigation brief: TCP downlink collapses to ~1.4 Mbit/s

Written to hand this problem to a fresh session. It is self-contained: symptom,
rig, everything already eliminated and how, and where to pick up. Nothing here
is remembered — every number is from a recorded run in [`bench/`](../bench) or
from a transcript of the command that produced it.

**Status: the bridge is not at fault.** A packet capture on the wired side
settles it. The bridge drops nothing, the radio transmits everything, the
retransmissions are real rather than spurious — and **the peer discards
out-of-order data**, so every single lost segment costs a full retransmission
of the window behind it. The collapse is a property of the *test peer*, not of
this device. See "The capture" below for the numbers.

What this means for the headline figure: **1.4 Mbit/s is not a measurement of
this bridge.** It is a measurement of the ESP-IDF iperf example's receive path
under burst loss. Any work aimed at making the bridge faster on the strength of
that number was aimed at the wrong component — which is why nine rounds of
config changes moved nothing.

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

### Check the rig before trusting a number

The test machine is **dual-homed onto the bridged LAN** — wired on
`192.168.5.2`, and it also has a USB WiFi dongle with a NetworkManager profile
that auto-joins `AOG hub`. When both are up it is a second station on the AP
under test, and it draws an address from the same pool the peer does. It was
found holding `192.168.5.4` — the address every run in `bench/` records as
`peer_ip` — at which point `iperf -c 192.168.5.4` from this box runs over
loopback at 56 Gbit/s and measures nothing at all.

The duplicate is manufactured by the device: ESP-IDF's DHCP server keeps leases
in a RAM list with no persistence and no ARP probe before offering, so every
reboot (i.e. every OTA flash) forgets them and reissues from `DHCP_START`, while
clients that still hold an unexpired lease never re-ask. **This is a real defect
and is not fixed.**

Before any run:

```sh
ip route get <peer-ip>          # must be 'dev <wired-if>', never 'local ... dev lo'
nmcli device status             # nothing but the wired link on 'AOG hub'
```

For the record, the fault is **not** caused by this: a gate run with the dongle
disconnected and the peer alone on `192.168.5.3` reproduced it exactly
(`bench/06-clean-rig-baseline`, `down_mbits` 1.31 against baseline's 1.14). The
nine eliminations above stand.

## Ruled out

Each was applied, measured, and reverted. None moved `down_mbits`.

| Hypothesis | How it was tested | Result |
| --- | --- | --- |
| WiFi TX buffers too small | `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` 32→64, rebuilt | 1.05 Mbit/s (`bench/01-wifi-tx-64`) |
| Block-ack window too small | `CONFIG_ESP_WIFI_TX_BA_WIN` 6→32, rebuilt | 1.4 Mbit/s, retrans 681 KB |
| TCP burst arrival at line rate | `iperf -b` pacing the sender to 5/10/15/20 Mbit/s | 0.96–1.56 Mbit/s at every rate — but see the caveat below |
| Ethernet DMA ring (only ~3 full frames: 10 × 512 B, and a 1514 B frame spans 3) | MSS swept 1440→1000→700→400→200, so frames span 3→1 buffers | 0.33–0.9 Mbit/s, no trend |
| Concurrent reverse traffic | UDP downlink 10 Mbit/s + uplink ping flood from the peer | downlink unaffected: 10.01 / 10.00 / 10.00 |
| This project's accounting hooks | `netif->input` / `netif->linkoutput` wrappers **not installed at all** (bisect build) | 0.8 Mbit/s, cwnd `[6,6,2,2,1]` |
| Rate mismatch (100 Mbit ingress → ~27 Mbit egress) | see the three discriminating facts below | not the cause |
| Device crashing or rebooting | uptime polled through runs | climbs steadily, no resets |
| ~~Peer is a poor TCP receiver~~ | same peer, same binary, different AP | 64.0 Mbit/s — **this row was wrong**, see below |
| lwIP's tcpip mailbox tail-dropping forwarded frames | counter on `bridgeif_tcpip_input`'s return, both ports (`fwd_drop_*_in`) | **0** frames refused, either direction, whole run |
| WiFi TX buffer exhaustion | counter on the WiFi port's `linkoutput` return (`fwd_drop_wifi_out`) | **6** frames over three runs |
| Downlink lost on air (802.11 retries exhausted) | `esp_wifi_set_tx_done_cb()`, counting `txStatus == false` | **0 of 1504** during a collapsing TCP downlink |

**The "different AP" row is the one that misdirected this whole investigation.**
The ESP32 joined a different access point and did 64 Mbit/s cleanly, and that
was read as proving its TCP receive path is fine and therefore that the bridge
must be at fault. It proves no such thing. A receiver that cannot buffer
out-of-order data is perfect on a lossless path and catastrophic on a lossy one
— the 64 Mbit/s result only shows the other AP did not make it drop anything.
The conclusion needed a *client* swap, not an *AP* swap, and that test sat
untried at the bottom of "other things worth trying" through nine rounds of
config changes.

Two of these rows are softer than they look, and neither has been re-tested:

- **`iperf -b` does not cap the burst that matters.** The local iperf is 2.1.5,
  so `-b` does apply to TCP — but it paces *application writes*. The kernel
  still puts a whole congestion window on the wire back-to-back at 100 Mbit/s
  inside one RTT, which is the burst any buffering theory is about. To actually
  pace the wire, use `tc qdisc ... fq maxrate` or `SO_MAX_PACING_RATE` on the
  sender.
- **`traffic_drops` is not a forwarding counter** and never was. It counts
  accounting events `client_track.c` gave up on when its queue was full, and
  costs the traffic nothing. Deltas of 45k-164k per gate run are normal and say
  only that the accounting task lost the race. The counters that mean a frame
  was really lost are `fwd_drop_*` and `wifi_tx_failed`, added later; see
  "What the bridge itself reports".

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
ingress and TCP stack are healthy.

This section originally concluded "the fault is in forwarding". That no longer
follows: the forwarding path has since been instrumented end to end and drops
nothing. What survives is narrower — the fault appears only when a TCP flow is
*relayed* to the WiFi peer, not when one terminates on the device.

## What the bridge itself reports

Everything above this section was inference from endpoint counters. The device
now reports its own drop points directly, in `/api/system`:

| Field | What it counts |
| --- | --- |
| `fwd_drop_eth_in` / `fwd_drop_wifi_in` | frames the tcpip mailbox refused from that port |
| `fwd_drop_eth_out` / `fwd_drop_wifi_out` | frames that port's driver had no buffer for |
| `wifi_tx_total` / `wifi_tx_failed` | frames handed to the radio, and those it gave up on |

The first four exist because lwIP's bridge does not forward in the caller's
context: with `BRIDGEIF_PORT_NETIFS_OUTPUT_DIRECT == 0`, which is what ESP-IDF
builds, `bridgeif_add_port()` points each port's input at
`bridgeif_tcpip_input()` -> `tcpip_inpkt()` -> `sys_mbox_trypost()`. A full
mailbox drops the frame and returns `ERR_MEM`, and nothing in lwIP or ESP-IDF
counts that. Likewise `bridgeif_input()` discards whatever
`bridgeif_send_to_ports()` returned, so an `ERR_MEM` from a WiFi port with no TX
buffer is lost too. This project's accounting hooks already sat on both sides of
both and already returned the value; they just never looked at it.

`wifi_tx_*` exists because the other two cannot see the air.
`esp_wifi_internal_tx()` returning `ESP_OK` means the frame was queued, not
transmitted; a frame that exhausts its retry limit is discarded inside the
driver silently.

Measured per phase, sampled between phases so the polling adds no load:

| Phase | Frames to the radio | Failed | Forwarding drops | Result |
| --- | --- | --- | --- | --- |
| idle | 4 | 0 | 0 | — |
| **TCP downlink** | **1504** | **0** | **0** | 0.53 Mbit/s |
| UDP downlink 20M | 26828 | 0 | 0 | 21.0 Mbit/s |
| TCP uplink | 20974 | 0 | 28 eth / 41 wifi out | 27.9 Mbit/s |

Read the first two rows together. Over the identical path in the identical
direction, UDP had the radio send **26828** frames and TCP **1504** — and none
of either failed. The downlink is not being dropped anywhere on this device;
the sender is never asked for more. That is the same conclusion as fact (3)
above, but measured rather than inferred.

### The ACK path is not the problem either

Polling `/api/clients` through a downlink (`tx_*` is traffic *from* the client,
`rx_*` is traffic *to* it):

```
  t   tx_pps  rx_pps     tx_bps   rx_bps   sender
  4      156     213       8424   318222   cwnd=7 retrans=281534
 14       95     132       5130   195768   cwnd=6 retrans=810042
 20      131     167       7074   248764   cwnd=8 retrans=1185176
```

Three things fall out of this:

- The peer is **receiving up to 318 KB/s (2.5 Mbit/s)** while iperf reports
  0.82 Mbit/s of goodput. Two thirds of what the bridge delivers is duplicate
  data.
- The peer returns **one ACK per ~1.3 data packets**, far denser than the one
  per two a healthy delayed-ACK receiver sends. That density is what a receiver
  does at a sequence gap: duplicate ACKs. So ACKs are flowing, and the peer has
  a hole in its receive sequence.
- Throughput alternates between stalls (4-11 pps) and bursts (131-156 pps),
  which is RTO backoff rather than steady loss.

So data is delivered, ACKs come back, nothing on the device is dropped, and the
peer still sees gaps. Every remaining explanation is at one endpoint or the
other, and counters cannot separate them.

## The capture

Taken on the wired side during a collapse (`tcpdump` needs `CAP_NET_RAW`; grant
it once with `sudo setcap cap_net_raw,cap_net_admin+eip /usr/bin/tcpdump`).
20 seconds, 0.5 Mbit/s, kernel dropped nothing:

```
data segments sender->peer : 1717
retransmitted segments     : 708  (41.2%)
out-of-order first sends   : 0

retransmits of data the peer had ALREADY ACKED: 0 of 708  (0.0%)
```

**Not one retransmission was spurious.** The DSACK/`reord_seen` reading in the
section above was a red herring — the losses are real, and they start in the
very first window, six segments into the connection.

### The peer throws away out-of-order data

This is the fault. When a retransmission fills a hole, a receiver that buffered
what arrived behind the hole ACKs straight past all of it in one jump. Measured
over all 708 filled holes:

| How far the ACK reached past the hole | Count | |
| --- | --- | --- |
| stopped exactly at the hole | 321 | 45.3% |
| ≤ 1 MSS beyond | 313 | 44.2% |
| **> 1 MSS beyond** (data was buffered) | **74** | **10.5%** |

Median 1440 bytes — exactly one segment. The largest jump in the whole capture
was 7200 bytes, five segments, against a sender with twenty-plus in flight.

**That ceiling reproduces exactly.** A second capture, taken on a quieter rig,
came out at 976 retransmissions (29.2% of segments), 0.0% spurious again, 16.5%
jumping past 1 MSS — and the same **7200-byte maximum**, to the byte. Throughput
and loss rate move with run-to-run noise; the cap does not. That is a fixed
queue limit, not a measurement artefact. The laptop's equivalent figure under
the same conditions was 43440.

Visible directly in the trace, where a fast retransmit fills a hole and the ACK
advances by one segment instead of leaping to the ~19 already delivered behind
it:

```
.998111   sender  seq 4016057010:4016058450    (fast retransmit)
1.002182  peer    ack 4016058450               <- one segment, not 4016085810
```

So each loss forces the sender to resend everything after it. That is what
turns a modest loss rate into a 24x collapse, and it is why cwnd never recovers:
`CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS` defaults to 4 in ESP-IDF, and the peer is the
stock `examples/wifi/iperf` build.

Its own boot banner rules out the other peer-side suspects — power save is off
and the receive window is 64 KB, not the few-KB default:

```
wifi_init: rx ba win: 32      tcp rx win: 65534
Set ps type: 0                tcp tx win: 65534
```

### What is still open

Why the *initial* loss happens at all. The bridge forwards every frame and the
radio reports every frame delivered, so the drop is above the peer's MAC and
below its TCP — its WiFi driver RX queue or lwIP. Paced UDP at 21 Mbit/s is
clean while TCP fails at 1.4, and the difference is burst shape: iperf paces UDP
evenly, whereas TCP puts a whole window on the wire back-to-back at 100 Mbit/s.

That question is worth answering only if the peer still matters. It is a test
instrument, not a product.

## Confirmed against a third-party client

A Linux laptop joined the same AP as an ordinary station and ran `iperf -s`,
with the wired box sending to it exactly as it does to the ESP32. Same bridge,
same path, same direction, same 20-second test:

| | Laptop, `-68` dBm, 802.11g | ESP32 peer, `-18` dBm |
| --- | --- | --- |
| **TCP downlink** | **11.1 Mbit/s** | **0.5–1.3 Mbit/s** |
| `cwnd_min` | 4 | 1 |
| holes filled by retransmission | 397 | 708 |
| ACK jumped **> 1 MSS** (data was buffered) | **66.0%** | **10.5%** |
| bytes ACKed beyond the hole, p90 | **18824** | 2880 |
| max ever | **40544** | 7200 |

Both clients hit loss on this path — the laptop retransmitted 574 KB. Only one
survives it. The laptop buffers up to **40 KB** of out-of-order data and rides
over a gap; the ESP32 tops out at 7200 bytes, five segments, so nearly every
loss costs it the whole window behind it.

The laptop managed **10x the throughput on a radio link 47 dB weaker**, over an
802.11g-only adapter. Nothing about the bridge explains that gap; the receiver
does.

Note the laptop's idle latency was `min 2.3 / avg 345.6 / max 1029.0 ms`, which
looks like WiFi power save left on — so 11.1 Mbit/s is a *floor*, not this
client's best.

## The old leading hypothesis, kept for the record

**The lwIP bridge layer** — `esp_netif_br_glue` and lwIP's `bridgeif` — was the
one major component on the eth→WiFi path that had not been eliminated, on the
grounds that a stock access point does not have it and does 64 Mbit/s.

The counters in "What the bridge itself reports" closed that: nothing on the
eth→air path drops a frame. Taking the bridge out of the path by bringing the
WiFi AP up as an ordinary SoftAP is still a valid structural experiment, but it
is no longer the first thing to try.

## Other things worth trying

Cheaper than the above, roughly in order of expected value:

- ~~A second, non-ESP32 WiFi client.~~ **Done** — see "Confirmed against a
  third-party client" above. It was the decisive test and should have been the
  first one run, ahead of every config change in the table.
- **`CONFIG_ESP_WIFI_RX_BA_WIN`** was left at 6; only the TX side was tried.
  The peer advertises 32–64.
- **`esp_wifi_set_ps(WIFI_PS_NONE)`** is never called; the default is
  `WIFI_PS_MIN_MODEM`. Effect in pure-AP mode is limited but it is untried.
- ~~Another station stalling the AP's queues.~~ **Tested, no effect.** Taking
  the other associated station off the air changed nothing measurable for either
  client: laptop 11.1 → 10.1 Mbit/s (inside the noise; that run's own 5-second
  intervals spanned 7.97-15.1), ESP32 0.5 → 1.04 Mbit/s, still collapsed, still
  0.0% spurious retransmits, same 7200-byte buffering ceiling.

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
