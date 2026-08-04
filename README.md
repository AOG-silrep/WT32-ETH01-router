# wt32-bridge

Version: 1.2.0 (see [version.txt](version.txt); shown on the web UI as "Rev 1.2.0")

An ESP-IDF firmware project that turns a [WT32-ETH01](https://en.wireless-tag.com/product-item-2.html) (ESP32 + LAN8720 Ethernet PHY) module into an Ethernet↔WiFi bridge for [AgOpenGPS](https://github.com/AgOpenGPS-Official/AgOpenGPS). It bridges the wired Ethernet interface and a WiFi access point at the network layer, and serves a small web UI for configuration and live monitoring.

## Features

- Ethernet (LAN8720 PHY) ↔ WiFi AP network bridge (`esp_netif_br_glue`)
- Web UI (`main/webpage/index.html`) for WiFi configuration and live status
- Connected-client tracking and per-device traffic graphing
- System monitor (heap, uptime, etc.) surfaced on the web UI, refreshed every second
- Device log viewable in a browser (`/logs`), not just over serial
- Configurable WiFi channel (1, 6, or 11)

## First boot

Out of the box the device brings up a WiFi access point and a wired port on one bridged
`192.168.5.0/24` network, with compiled-in defaults:

| | Default |
| --- | --- |
| WiFi SSID | `AOG hub` |
| WiFi password | `password` |
| Web UI login | `admin` / `admin` |
| Bridge address | `192.168.5.1` |

Changing the admin password is forced before the device will do anything else (see
[Web UI login](#web-ui-login)). **Changing it does not change the WiFi password** — the two
are separate, and a device left on the default PSK is joinable by anyone in range whatever
the admin password says. Set both, from `/admin` and the `wifi` console command respectively.

## Network and capacity

The addressing is compiled in (`main/main.c`) and not configurable from the UI or console:

| | Value | Set by |
| --- | --- | --- |
| Bridge address / netmask | `192.168.5.1` / `255.255.255.0` | `BRIDGE_IP`, `BRIDGE_NETMASK` |
| DHCP pool | `192.168.5.2` – `192.168.5.101` | `DHCP_START`, `DHCP_END` |
| Concurrent DHCP leases | 100 (the whole pool) | `DHCP_START`, `DHCP_END` |
| Addresses remembered across a reboot | 32 | `DHCP_SERVER_MAX_LEASES` |
| WiFi stations | 6 | `WIFI_CFG_MAX_STA_CONN` |
| Clients shown in the table | 16 | `CLIENT_TRACK_MAX_CLIENTS` |
| Bridge forwarding table | 32 MACs | `max_fdb_dyn_entries` |
| Client forgotten after | 5 minutes of silence | `CLIENT_AGE_OUT_US` |

Static addresses outside the pool work fine and are not subject to any of these limits.

### Clients keep their address across a reboot

The DHCP server (`main/dhcp_server.c`) records each MAC → IP mapping in flash and consults
it before allocating anything, so a client gets the same address back after the bridge
reboots — or after it does — no matter what order the clients come up in. This is automatic;
there is nothing to configure. `leases` on the serial console shows the table:

```
MAC                IP                EXPIRES SEEN          STATE
fc:e8:c0:4d:ab:94  192.168.5.2         7194s now           active
a4:83:e7:11:22:33  192.168.5.3             - 2 boots ago   reserved
```

`reserved` is a mapping held for a client that isn't currently here. It is still that
client's address when it returns, and it is only given to someone else if the pool runs
out. `factory-reset yes` erases the table along with the rest of the saved config.

### Reservations are not expired on a timer

They are dropped only when something else needs the room — a new client with the table
full, or an address request with the pool exhausted — taking whichever entry has gone
longest without being heard from, and preferring plain leases over manually-addressed
clients. Nothing is deleted merely because time passed.

That is a deliberate choice on two counts. The lease is two hours, so expiring
reservations with it would mean a laptop closed overnight loses its address on every
reboot — precisely what this feature exists to prevent. And the device cannot measure
elapsed time anyway: `esp_timer_get_time()` restarts at zero each boot, RTC memory is
wiped by power-on, there is no RTC battery, and a transparent bridge has no uplink it can
count on for SNTP. A board unplugged for a month is indistinguishable from one
power-cycled a second ago.

Reclaiming only needs *ordering*, though, and that much does survive. Each save stamps
the table with a generation number; an entry carries the generation it was last heard
from in. The `SEEN` column reports the difference — `now` for a client seen since boot,
which also makes it ineligible for reclaiming, or `N boots ago` for one that has not
been. Those counts are boots that wrote the table, not hours.

Writes are debounced and only happen when the *mapping* changes: lease renewals, which are
almost all DHCP traffic, never touch flash. Past 32 remembered clients the least recently
seen mapping is dropped — those clients still get addresses, they just stop being sticky.
See [Reservations are not expired on a timer](#reservations-are-not-expired-on-a-timer)
for what "least recently seen" means on a device with no clock.

This server replaces ESP-IDF's, which is why the two limits that used to be documented here
are gone. IDF's kept its leases in RAM only, capped the pool at 100 addresses
(`DHCPS_MAX_LEASE`) while silently substituting a default range if you asked for more, and
evicted the oldest of 16 leases without telling anyone. `CONFIG_LWIP_DHCPS_*` in `sdkconfig`
no longer has any effect on this device.

### Protected Management Frames are off

The access point disables PMF (802.11w) explicitly, in `wifi_cfg_apply()`. With it enabled —
which is ESP-IDF's default, and cannot be avoided by leaving the config field alone, since
`pmf_cfg.capable` is documented as "deprecated and set to true internally" — the AP runs an
SA Query against re-associating stations, and disassociates any that fail to answer. In
practice stations failed it every few seconds, giving an access point that ejected its own
clients on a loop. The cost of turning it off is PMF's protection against forged
deauthentication frames; WPA2 encryption of data is unaffected.

## Web UI

Browse to the device (`192.168.5.1` by default) for WiFi settings, firmware update, and live
system stats:

![The web UI: WiFi settings, firmware update, and system stats](docs/web-ui.png)

Further down the same page, every connected client (WiFi and Ethernet) is listed with its
live throughput. "Graph" plots that client's traffic over the last 30 seconds, switchable
between bytes/s and packets/s:

![Connected clients table with a per-client traffic graph](docs/client-traffic.png)

### Device log

"Device log →" in the header opens `/logs`, a live tail of the same `ESP_LOG` output the
serial console carries — useful when the device is deployed somewhere you can't easily reach
with a cable. The last 128 lines are kept on the device and polled once a second.

Two verbosity settings, which is worth understanding before wondering why a level change
didn't do what you expected:

- **Capture level** (on the log page) — how much the device records for the web log. Defaults
  to `info`. It is also the ceiling: serial output can never be more verbose than this.
- **Serial level** (the `loglevel` console command) — how much of that also reaches UART0.
  Defaults to `warn`, so routine INFO chatter doesn't bury the `aog-bridge>` prompt.

They're independent in one direction only. Since nothing above the capture level reaches the
serial path either, asking `loglevel` for more than is being captured would otherwise do
nothing — so it raises the capture level to match. It never lowers it: that would let someone
at the cable blind a log page they can't see. Setting the capture level from the page leaves
the serial level alone.

The page's "Show" dropdown is neither of those: it filters what's already on screen, and
changes nothing on the device.

`none` is a valid capture level over the API and at the console, but deliberately absent from
the page's dropdown — it silences the serial console too, and `loglevel` is then the only way
back. The page shows the real level in its status line if something else sets it there.

Levels above `info` are compiled out (`CONFIG_LOG_MAXIMUM_LEVEL`), so `debug` and `verbose`
currently have nothing to show. Both the page and `loglevel` still accept them, and both say
so when the level you asked for can't produce anything.

## Web UI login

The web UI (`/`) and settings page (`/admin`) are behind HTTP Basic Auth. Until changed, the
login is the compiled-in default:

- Username: `admin`
- Password: `admin`

That default is bootstrap-only: while the password is still `admin`, the device serves nothing
but the pages needed to change it. `/` redirects to `/admin`, and every `/api/*` route answers
`403` — including firmware upload — so the bridge can't be driven by a browser or a script
until setup is finished. Setting a new password is what unlocks it; the username is not a
secret and changing it alone doesn't count.

Change it from `/admin` (linked via the "Update username & password" button on the main page)
or with the serial console's `admin` command (see below). The console applies the same rule, so
`admin -u <name>` on a fresh device fails until you pass `-p <password>` too.

Because a browser attaches those credentials to *any* request it makes to the bridge — including
one started by a page on some other site — the routes that change something (`/api/wifi`,
`/api/admin`, `/api/logs/level`, `/api/ota`) additionally require the right `Content-Type`, and
refuse a request whose `Origin` names somewhere other than the bridge. A script sends no `Origin`
and is unaffected by the second rule, but it does have to set the header:

```sh
curl -u admin:<password> -H 'Content-Type: application/json' \
     -d '{"level":"warn"}' http://192.168.5.1/api/logs/level
```

Without it the answer is `415`. `/api/ota` wants `application/octet-stream`; the other three want
`application/json`.

## HTTP API

Every route is behind Basic Auth, and every one answers `403` while the admin password is
still the default. `GET`s need nothing else; the four `POST`s need the `Content-Type` above.

| Route | Method | Body / query | Returns |
| --- | --- | --- | --- |
| `/` `/admin` `/logs` | GET | — | the three HTML pages |
| `/api/status` | GET | — | `{ssid, channel}` |
| `/api/system` | GET | — | see the table below |
| `/api/clients` | GET | — | array, one object per client |
| `/api/client/history?mac=&since=` | GET | `mac` required, `since` optional | fine-grained traffic history |
| `/api/logs?since=` | GET | `since` optional | log lines newer than the cursor |
| `/api/logs/level` | POST | `{"level":"info"}` | sets the capture level |
| `/api/wifi` | POST | `{"ssid":…,"password":…,"channel":…}` | saves and **reboots** |
| `/api/admin` | POST | `{"new_admin_user":…,"new_admin_password":…}` | saves, no reboot |
| `/api/ota` | POST | raw firmware image | flashes and **reboots** |

`/api/logs` and `/api/client/history` share a cursor contract: echo the `seq` from the
previous response back as `since` and you get only what you have not seen. Both cap the
response size, so `seq` is **the last line in this response, not the newest on the device** —
when `more` is `true` there is more waiting, and you should poll again immediately rather
than wait for the next interval. `/api/logs` also reports `lost` (lines that aged out of the
ring before this batch), in-band `{"gap":N}` entries where lines were overwritten mid-batch,
and `restarted` when the device rebooted under your cursor.

### `/api/system` fields

| Field | Meaning |
| --- | --- |
| `uptime_s`, `free_heap`, `min_free_heap` | seconds since boot; heap now and its low-water mark |
| `cpu_pct` | `[core0, core1]` percent busy, sampled over the last second |
| `cpu_freq_mhz` | *measured*, not configured — a cycle count timed against a hardware timer |
| `net_rx_bps`, `net_tx_bps` | bridge-wide bytes/sec, summed over tracked clients |
| `net_rx_pps`, `net_tx_pps` | the same as packet rates, all protocols |
| `traffic_drops` | dropped **accounting events** — see the caveat below |
| `version` | contents of `version.txt` at build time |

`traffic_drops` is the one that misleads. It counts events the per-client accounting queue
could not keep up with; it never counts forwarded frames, and nothing in that path can drop a
packet. It scales with packet rate rather than with anything being wrong — measured on this
device with no data lost in any case: 0 at 2 Mbit/s, ~4k over eight seconds at 20 Mbit/s,
~33k at 40, and 50–130k across a three-run throughput test. A large number means the
per-client byte and packet figures undercount, and nothing more. On an idle bridge it stays
at 0.

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
**v6.0 or newer**. This is not a soft floor: `dependencies.lock` pins IDF 6.0.2, the
`espressif/lan87xx` managed component declares `idf >= 6.0`, and `main/log_buf.c` depends on
`CONFIG_LOG_VERSION_1` behaviour for its one-hook-call-per-line assumption. Older versions
will not resolve dependencies.

Firmware images are signed (ECDSA), so a local private key is required to build. Generate
one once per machine you build from:

```sh
python -m espsecure generate-signing-key --version 1 --scheme ecdsa256 secure_boot_signing_key.pem
```

`secure_boot_signing_key.pem` is gitignored — never commit it. It's what makes the device
trust "this project"'s firmware specifically: the bootloader verifies every image's signature
before booting it (on serial flash and on OTA update via `/api/ota`), and rejects anything not
signed with this key. Back the key up somewhere private (password manager, encrypted drive) if
you build from more than one machine; losing it just means generating a new one and
re-flashing once over serial — it's a software check, not a hardware fuse, so there's no risk
of bricking.

```sh
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

Because this changes the bootloader itself, a device previously flashed without signing
enabled needs one full serial reflash (as above, which writes bootloader + partition table +
app) to start enforcing it. After that, only images signed with `secure_boot_signing_key.pem`
will boot.

### Updating over the air

Once a device is deployed, `/api/ota` is the update path — no cable, and no boot-mode jumper
(the WT32-ETH01 has no auto-reset wiring, so serial flashing means shorting IO0 to ground by
hand):

```sh
curl -u admin:<password> -H 'Content-Type: application/octet-stream' \
     --data-binary @build/wt32-bridge.bin http://192.168.5.1/api/ota
```

It answers `{"ok":true}` and reboots about half a second later. The image must be signed with
the same key — the bootloader rejects anything else, so an OTA from a machine without the key
will upload happily and then fail to boot.

Failing to boot is survivable. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on, so a new image
is booted "pending verify"; `app_main()` only calls `esp_ota_mark_app_valid_cancel_rollback()`
after every startup-critical subsystem has come up. An image that panics or hangs before that
point is rolled back to the previous one on the next boot. A bad *configuration* that still
boots is not caught by this — that you fix with another OTA, or over serial.

`scripts/ota.sh` wraps the above: it refuses to flash an image older than the sources, waits
for the device to come back, and reports the version and heap it came back with.

## Hardware

Built for the [WT32-ETH01](https://en.wireless-tag.com/product-item-2.html) (ESP32-WROOM +
LAN8720A). The pin assignments are compiled in (`main/main.c`) and are specific to this
board — a lookalike with different strapping needs these changed:

| Function | GPIO |
| --- | --- |
| PHY power enable | 16 |
| RMII MDC | 23 |
| RMII MDIO | 18 |
| PHY address | 1 (SMI) |
| Console / flashing UART | UART0, 115200 8N1 |

Flash layout is 4 MB with a custom `partitions.csv`: the partition table sits at `0x10000`
(moved down to make room for the signed bootloader), and there are two 1.5 MB OTA app slots.
1.5 MB is therefore the hard ceiling on firmware size; the current image uses about
two-thirds of it, leaving roughly 500 KB of headroom.

## Diagnostics harness

`scripts/` holds what was used to measure this device, so a change can be shown not to have
made it worse:

| Script | Purpose |
| --- | --- |
| `gate.py` | ping (idle and loaded), throughput both directions, TCP retransmits, CPU, drops — median of N runs, written to `bench/<label>/` |
| `smoke.sh` | functional regression: pages load, JSON parses, auth and CSRF still refuse what they should |
| `ota.sh` | build-freshness-checked OTA flash |
| `serial_cmd.py` | drives a serial console; note that opening the port reboots the target, so one invocation must carry every command |

`gate.py` expects a wired client on the Ethernet side and a WiFi client it can drive over
serial (an ESP32 running the ESP-IDF `iperf` example). Both ends need iperf **2.x** — the
ESP-IDF example does not speak the iperf3 protocol. Credentials come from `BRIDGE_PASS` in
the environment, never the repo.

```sh
BRIDGE_PASS=<password> scripts/gate.py --label my-change --runs 3
BRIDGE_PASS=<password> scripts/smoke.sh
```

Recorded runs live in `bench/`; `00-baseline` is the reference.

## Serial console (recovery & diagnostics)

The firmware exposes an interactive command console on the same UART used for flashing
(UART0, 115200 baud) — connect with `idf.py -p <PORT> monitor`, or any serial terminal at
115200 8N1, no extra wiring needed.

This console is **unauthenticated**: anyone with physical/serial access to the board has full
control, including the ability to wipe saved config. That's by design — physical access
already wins, and this is the intended recovery path when the web UI is unreachable (lost
admin password, bad WiFi config). Type `help` at the `aog-bridge>` prompt for the full command
list; the main ones:

| Command | Purpose |
| --- | --- |
| `wifi [-s <ssid>] [-p <password>] [-c <1\|6\|11>]` | Show or change the WiFi AP SSID/password/channel. Omitted fields keep their current value; changing any reboots the device. |
| `admin [-u <user>] [-p <password>]` | Show or change the web UI admin username/password. |
| `sysinfo` | Uptime, heap, CPU load, network traffic. |
| `clients` | List active bridge clients (WiFi + Ethernet). |
| `leases` | List DHCP leases and the MAC → IP reservations kept in flash — see [Clients keep their address across a reboot](#clients-keep-their-address-across-a-reboot). |
| `loglevel [none\|error\|warn\|info\|debug\|verbose]` | Show or set how much log output reaches this serial console. Raises the web log page's capture level too, if that is what's holding the output back — see [Device log](#device-log). |
| `reboot` | Restart the device. |
| `factory-reset yes` | Erase saved WiFi and admin credentials and the saved DHCP reservations, restoring compiled-in defaults (WiFi `AOG hub`/`password`; admin `admin`/`admin`), then reboot. Clients are given fresh addresses. Bare `factory-reset` (no `yes`) just prints this warning and changes nothing. |

**Locked out of the web UI?** Connect over serial and run `factory-reset yes`. The device
reboots with the default WiFi AP and `admin`/`admin` web login restored.

## Source layout

- `main/main.c` — startup, Ethernet/WiFi bridge setup
- `main/wifi_cfg.c` / `.h` — WiFi AP configuration (SSID, channel, credentials)
- `main/web_server.c` / `.h` — HTTP server backing the web UI
- `main/client_track.c` / `.h` — connected-client and traffic tracking
- `main/dhcp_server.c` / `.h` — DHCP server for the LAN, with MAC → IP reservations in NVS
- `main/sys_monitor.c` / `.h` — system stats (heap, uptime) for the web UI
- `main/serial_console.c` / `.h` — interactive UART console for recovery/diagnostics
- `main/auth_cfg.c` / `.h` — admin username/password storage (NVS) with compiled-in defaults
- `main/log_buf.c` / `.h` — in-memory log ring behind `/logs`, and the serial/capture level split
- `main/webpage/index.html` — the web UI itself
- `main/webpage/admin.html` — the `/admin` settings page
- `main/webpage/logs.html` — the `/logs` device log page
- `scripts/` — measurement gate, smoke tests, OTA helper (see [Diagnostics harness](#diagnostics-harness))
- `bench/` — recorded gate runs; `00-baseline` is the reference

## Known limitation: slow TCP downlink

**Wired → WiFi TCP runs at roughly 1.4 Mbit/s**, against 27 Mbit/s in the other direction.
This is a real, reproducible fault in the bridge and is not yet fixed.

What is known, all measured (`bench/`):

- It is the bridge, not the client. The same WiFi client, same iperf binary, gets
  64 Mbit/s with zero retransmits through an ordinary access point.
- It is specific to TCP. Paced UDP over the identical path is clean at 20 Mbit/s, and ICMP
  through it loses nothing and arrives in order *while a TCP transfer is collapsing on the
  same path*.
- It is not a rate mismatch between the fast wired side and the slower WiFi side. TCP paced
  down to 5 Mbit/s — far below WiFi capacity — collapses just the same.
- The bridge forwards what it is handed: its own per-client counters show 1.39 Mb/s in
  against 1.37 Mb/s out.
- Ruled out by measurement, each tried and reverted: WiFi TX buffer count, block-ack window
  size, Ethernet DMA buffer count, TCP segment size down to 200 bytes, concurrent reverse
  traffic, and this project's own accounting hooks (removed entirely — no change).

The untested suspect is the lwIP bridge layer itself (`esp_netif_br_glue` / `bridgeif`).
Uplink, UDP, ICMP and anything terminating on the device are all unaffected, so a deployment
that mostly sends *from* the WiFi side — which is the common AgOpenGPS direction — will not
notice it.

Full investigation brief, including the rig, every hypothesis already eliminated and how,
and the experiment to run next: [docs/downlink-fault.md](docs/downlink-fault.md).

## Troubleshooting high/jittery ping latency

Check `192.168.5.1/api/system` first: `cpu_pct` should be near-idle. **Ignore
`traffic_drops`** — it counts accounting events, not packets, and runs to tens of thousands
under normal load with nothing wrong (see [`/api/system` fields](#apisystem-fields)). If the
CPU looks fine, the bridge's software isn't the bottleneck — check WiFi channel
congestion/interference next.

Per-client traffic accounting (`client_track.c`) is intentionally decoupled
from the packet-forwarding path: `traffic_input_wrapper`/`traffic_output_wrapper`
hand events to a bounded queue with a non-blocking send (drop-and-count on
full, never wait), and a dedicated low-priority task drains it. This means
forwarding can never block on accounting, so it shouldn't need to be a
suspect in future latency investigations — see the comments at those call
sites and around the `traffic_account_task` creation in `client_track_init()`.
