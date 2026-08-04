#!/usr/bin/env python3
"""The measurement gate: run this identically before and after every change.

Why a gate exists at all: WiFi latency and throughput vary enough run to run
that a single sample cannot tell an improvement from noise. Every figure here
is the median of --runs repetitions, and a change is only credited when it
moves the median further than the spread seen at baseline. The raw per-run
numbers are always written out so that spread stays visible instead of being
hidden behind the median.

The rig, which is what shapes everything below:

    wired client   this machine, on the bridge's Ethernet side
    WiFi client    an ESP32 running the ESP-IDF iperf example, over serial
    bridge         the device under test, polled over HTTP

The ESP32 is driven over UART, and opening that port reboots it - the CP2102N
drives EN from RTS (see serial_cmd.py). So the peer is opened exactly once per
gate run and held for the whole thing, and it has to be re-joined to the AP
after that reboot. Everything the peer does happens inside that one session.

Bridge statistics come over HTTP rather than the bridge's own UART for the same
reason: opening its console would reboot the device under test in the middle of
measuring it.

Usage:
    scripts/gate.py --label baseline
    scripts/gate.py --label step1-fdb --runs 3

Credentials come from the environment, never the repo:
    BRIDGE_USER (default admin), BRIDGE_PASS
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


# ---------------------------------------------------------------- bridge (HTTP)

class Bridge:
    def __init__(self, host, user, password):
        self.host = host
        mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
        mgr.add_password(None, f"http://{host}/", user, password)
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPBasicAuthHandler(mgr))

    def get(self, path, timeout=5):
        with self.opener.open(f"http://{self.host}{path}", timeout=timeout) as r:
            return json.loads(r.read().decode())

    def system(self):
        return self.get("/api/system")

    def clients(self):
        return self.get("/api/clients")


# ------------------------------------------------------------------ peer (UART)

class Peer:
    """The ESP32 iperf client, held open for the life of the gate run.

    A background thread drains the port continuously. Without it a long local
    iperf run would overflow the kernel's serial buffer and the peer's own
    report - the other half of every throughput figure - would be lost.
    """

    def __init__(self, port, baud=115200):
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()
        self.ser = ser
        self._buf = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self):
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(4096)
            except Exception:
                return
            if chunk:
                with self._lock:
                    self._buf.append(chunk.decode("utf-8", errors="replace"))

    def _raw(self):
        with self._lock:
            return "".join(self._buf)

    def mark(self):
        """Offset into the raw stream, so a later read sees only what came
        after. Marks index the raw text, not the ANSI-stripped view, or they
        would drift by however many escapes had been removed."""
        return len(self._raw())

    def since(self, mark):
        return ANSI.sub("", self._raw()[mark:])

    def send(self, cmd):
        self.ser.write((cmd + "\r\n").encode())
        self.ser.flush()

    def wait_for(self, pattern, timeout, mark=0):
        """Wait for pattern to appear after mark. Returns the match or None."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            m = rx.search(self.since(mark))
            if m:
                return m
            time.sleep(0.2)
        return None

    def join(self, ssid, psk, timeout=45):
        """Re-join the AP after the reboot that opening the port caused."""
        self.wait_for(r"iperf>", timeout=20)
        mark = self.mark()
        self.send(f'sta_connect "{ssid}" {psk}')
        m = self.wait_for(r"sta ip:\s*(\d+\.\d+\.\d+\.\d+)", timeout, mark)
        if not m:
            raise RuntimeError("peer did not get an IP - check SSID/PSK and AP")
        return m.group(1)

    def close(self):
        self._stop.set()
        self._reader.join(timeout=2)
        self.ser.close()


# ------------------------------------------------------------------ local tools

def run(cmd, timeout):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          timeout=timeout).stdout


PING_STATS = re.compile(
    r"(\d+) packets transmitted, (\d+) received.*?(\d+(?:\.\d+)?)% packet loss"
    r".*?rtt min/avg/max/mdev = "
    r"([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)", re.S)


def ping(host, count, interval):
    out = run(f"ping -c {count} -i {interval} {host}", timeout=count * interval + 30)
    m = PING_STATS.search(out)
    if not m:
        return None
    return {"sent": int(m.group(1)), "recv": int(m.group(2)),
            "loss_pct": float(m.group(3)), "min": float(m.group(4)),
            "avg": float(m.group(5)), "max": float(m.group(6)),
            "mdev": float(m.group(7))}


IPERF_LOCAL = re.compile(r"([\d.]+)\s+([KMG])bits/sec")


def iperf_mbits(out):
    """Last bandwidth figure in an iperf2 report, normalised to Mbit/s."""
    hits = IPERF_LOCAL.findall(out)
    if not hits:
        return None
    val, unit = hits[-1]
    scale = {"K": 0.001, "M": 1.0, "G": 1000.0}[unit]
    return round(float(val) * scale, 2)


# ------------------------------------------------------------------- the phases

def phase_ping_idle(peer_ip, bridge_host, count, interval):
    return {
        "wired_to_bridge": ping(bridge_host, count, interval),
        "wired_to_wifi": ping(peer_ip, count, interval),
    }


def phase_throughput(peer, peer_ip, wired_ip, secs):
    """Both directions. 'up' is WiFi->wired (the peer is the iperf client),
    'down' is wired->WiFi (this machine is the client). Named for the client's
    direction of travel, matching how the bridge's own tx/rx are labelled.

    Each direction is read from *its own client*, never from the server: an
    iperf client always prints a closing summary, whereas a server that is
    killed to end the run may not have flushed one. For the up direction that
    client is the ESP32, so the figure comes back over serial."""
    result = {}

    # up: peer is the client, this machine just receives.
    srv = subprocess.Popen(f"iperf -s -i 1", shell=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    mark = peer.mark()
    peer.send(f"iperf -c {wired_ip} -t {secs} -i 1")
    time.sleep(secs + 6)
    peer_out = peer.since(mark)
    result["up_mbits"] = iperf_mbits(peer_out)
    result["up_peer_report"] = peer_out[-800:]
    peer.send("iperf --abort")
    srv.kill()
    time.sleep(2)

    # down: this machine is the client, the peer just receives.
    #
    # Retransmits are recorded here and nowhere else because this is the
    # direction that fails: at baseline, TCP downlink collapsed to ~1 Mbit/s
    # while paced UDP over the same path ran clean at 20. That rules out a
    # lossy link and points at burst loss - TCP hands the bridge a whole
    # congestion window at 100 Mbit/s line rate, which has to buffer down to a
    # ~27 Mbit/s WiFi egress. bytes_retrans is what makes that visible, and it
    # moves long before the throughput average does, so it is the earliest
    # signal that a change helped or hurt.
    peer.send("iperf -s")
    time.sleep(2)
    client = subprocess.Popen(f"iperf -c {peer_ip} -t {secs} -i 1", shell=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True)
    time.sleep(1)
    retrans, cwnd_min = 0, None
    deadline = time.monotonic() + secs
    while time.monotonic() < deadline and client.poll() is None:
        ss = run(f"ss -tin dst {peer_ip}", 10)
        m = re.search(r"bytes_retrans:(\d+)", ss)
        if m:
            retrans = max(retrans, int(m.group(1)))
        m = re.search(r"cwnd:(\d+)", ss)
        if m:
            cwnd_min = min(cwnd_min, int(m.group(1))) if cwnd_min else int(m.group(1))
        time.sleep(1)
    out = client.communicate(timeout=secs + 30)[0]
    result["down_mbits"] = iperf_mbits(out)
    result["down_bytes_retrans"] = retrans
    result["down_cwnd_min"] = cwnd_min
    peer.send("iperf --abort")
    time.sleep(1)
    return result


def phase_ping_loaded(peer, peer_ip, count, interval, secs):
    """Latency while the bridge is saturated - where jitter actually shows."""
    peer.send("iperf -s")
    time.sleep(2)
    load = subprocess.Popen(f"iperf -c {peer_ip} -t {secs} -i 5", shell=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    stats = ping(peer_ip, count, interval)
    load.wait(timeout=secs + 30)
    peer.send("iperf --abort")
    time.sleep(1)
    return stats


# ------------------------------------------------------------------- aggregation

def median_of(runs, *path):
    vals = []
    for r in runs:
        cur = r
        for k in path:
            if cur is None:
                break
            cur = cur.get(k)
        if isinstance(cur, (int, float)):
            vals.append(cur)
    if not vals:
        return None
    return {"median": round(statistics.median(vals), 3),
            "runs": vals,
            "spread": round(max(vals) - min(vals), 3)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="names the output directory")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--bridge", default="192.168.5.1")
    ap.add_argument("--peer-port", default="/dev/ttyUSB2")
    ap.add_argument("--ssid", default="AOG hub")
    ap.add_argument("--psk", default="password")
    ap.add_argument("--ping-count", type=int, default=200)
    ap.add_argument("--ping-interval", type=float, default=0.1)
    ap.add_argument("--iperf-secs", type=int, default=10)
    ap.add_argument("--outdir", default="bench")
    ap.add_argument("--wired-ip", default=None,
                    help="this machine's address on the bridged LAN; "
                         "derived from the route to the bridge if omitted")
    args = ap.parse_args()

    wired_ip = args.wired_ip
    if not wired_ip:
        m = re.search(r"src (\d+\.\d+\.\d+\.\d+)",
                      run(f"ip route get {args.bridge}", 10))
        if not m:
            print("gate: cannot work out this machine's address on the bridged "
                  "LAN - pass --wired-ip", file=sys.stderr)
            return 2
        wired_ip = m.group(1)

    user = os.environ.get("BRIDGE_USER", "admin")
    password = os.environ.get("BRIDGE_PASS")
    if not password:
        print("gate: set BRIDGE_PASS (and BRIDGE_USER if not 'admin')",
              file=sys.stderr)
        return 2

    bridge = Bridge(args.bridge, user, password)
    try:
        before = bridge.system()
    except (urllib.error.URLError, OSError) as e:
        print(f"gate: cannot reach the bridge at {args.bridge}: {e}",
              file=sys.stderr)
        return 2

    print(f"gate: bridge up - {before['version']}, "
          f"{before['cpu_freq_mhz']} MHz, uptime {before['uptime_s']}s, "
          f"drops {before['traffic_drops']}")
    print(f"gate: wired client is {wired_ip}")

    print(f"gate: opening peer on {args.peer_port} (this reboots it)")
    peer = Peer(args.peer_port)
    try:
        peer_ip = peer.join(args.ssid, args.psk)
        print(f"gate: peer joined as {peer_ip}")

        runs = []
        for i in range(1, args.runs + 1):
            print(f"gate: run {i}/{args.runs}")
            r = {}
            r["ping_idle"] = phase_ping_idle(peer_ip, args.bridge,
                                             args.ping_count, args.ping_interval)
            print(f"       idle  wired->wifi avg "
                  f"{(r['ping_idle']['wired_to_wifi'] or {}).get('avg')} ms")
            r["throughput"] = phase_throughput(peer, peer_ip, wired_ip,
                                               args.iperf_secs)
            print(f"       tput  up {r['throughput']['up_mbits']} / "
                  f"down {r['throughput']['down_mbits']} Mbit/s")
            r["ping_loaded"] = phase_ping_loaded(peer, peer_ip,
                                                 args.ping_count,
                                                 args.ping_interval,
                                                 args.iperf_secs + 15)
            print(f"       load  wired->wifi avg "
                  f"{(r['ping_loaded'] or {}).get('avg')} ms")
            r["system"] = bridge.system()
            runs.append(r)

        after = bridge.system()
        summary = {
            "label": args.label,
            "when": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "git": run("git rev-parse --short HEAD", 10).strip(),
            "bridge_before": before,
            "bridge_after": after,
            "drops_delta": after["traffic_drops"] - before["traffic_drops"],
            # Frames the bridge refused to forward over the run, per port and
            # direction. Unlike traffic_drops above these are real losses, so
            # they are the figure to read first. Absent on firmware older than
            # the counters, hence the guard rather than a plain subtraction.
            "fwd_drops_delta": {
                k: after[k] - before[k]
                for k in ("fwd_drop_eth_in", "fwd_drop_wifi_in",
                          "fwd_drop_eth_out", "fwd_drop_wifi_out",
                          "wifi_tx_total", "wifi_tx_failed")
                if k in after and k in before
            },
            "peer_ip": peer_ip,
            "medians": {
                "idle_wired_to_wifi_avg_ms":
                    median_of(runs, "ping_idle", "wired_to_wifi", "avg"),
                "idle_wired_to_wifi_max_ms":
                    median_of(runs, "ping_idle", "wired_to_wifi", "max"),
                "idle_wired_to_bridge_avg_ms":
                    median_of(runs, "ping_idle", "wired_to_bridge", "avg"),
                "loaded_wired_to_wifi_avg_ms":
                    median_of(runs, "ping_loaded", "avg"),
                "loaded_wired_to_wifi_max_ms":
                    median_of(runs, "ping_loaded", "max"),
                "loaded_loss_pct":
                    median_of(runs, "ping_loaded", "loss_pct"),
                "up_mbits": median_of(runs, "throughput", "up_mbits"),
                "down_mbits": median_of(runs, "throughput", "down_mbits"),
                "down_bytes_retrans":
                    median_of(runs, "throughput", "down_bytes_retrans"),
                "down_cwnd_min": median_of(runs, "throughput", "down_cwnd_min"),
                # Sampled straight after the loaded phase, so this is CPU under
                # load rather than at idle. cpu_pct is a two-element list, which
                # median_of() cannot walk into, so the cores are split out here.
                "cpu0_pct": median_of(
                    [{"v": r["system"]["cpu_pct"][0]} for r in runs], "v"),
                "cpu1_pct": median_of(
                    [{"v": r["system"]["cpu_pct"][1]} for r in runs], "v"),
            },
            "runs": runs,
        }
    finally:
        peer.close()

    outdir = Path(args.outdir) / args.label
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2))

    print(f"\ngate: {args.label} -> {outdir}/summary.json")
    print(f"gate: traffic_drops over the run: {summary['drops_delta']}")
    fwd = summary["fwd_drops_delta"]
    if fwd:
        print("gate: frames the bridge did not forward: " +
              ", ".join(f"{k.removeprefix('fwd_drop_')} {v}"
                        for k, v in fwd.items() if k.startswith("fwd_drop_")))
        total, failed = fwd.get("wifi_tx_total"), fwd.get("wifi_tx_failed")
        if total:
            print(f"gate: wifi frames the radio gave up on: {failed}/{total} "
                  f"({100.0 * failed / total:.2f}%)")
    for k, v in summary["medians"].items():
        if v:
            print(f"       {k:34s} {v['median']:>9}  (spread {v['spread']}, "
                  f"{v['runs']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
