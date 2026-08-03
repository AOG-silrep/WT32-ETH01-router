# wt32-bridge

Version: 1.1.0 (see [version.txt](version.txt); shown on the web UI as "Rev 1.1.0")

An ESP-IDF firmware project that turns a [WT32-ETH01](https://en.wireless-tag.com/product-item-2.html) (ESP32 + LAN8720 Ethernet PHY) module into an Ethernet↔WiFi bridge for [AgOpenGPS](https://github.com/AgOpenGPS-Official/AgOpenGPS). It bridges the wired Ethernet interface and a WiFi access point at the network layer, and serves a small web UI for configuration and live monitoring.

## Features

- Ethernet (LAN8720 PHY) ↔ WiFi AP network bridge (`esp_netif_br_glue`)
- Web UI (`main/webpage/index.html`) for WiFi configuration and live status
- Connected-client tracking and per-device traffic graphing
- System monitor (heap, uptime, etc.) surfaced on the web UI, refreshed every second
- Device log viewable in a browser (`/logs`), not just over serial
- Configurable WiFi channel (1, 6, or 11)

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

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (v5.3+).

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
| `loglevel [none\|error\|warn\|info\|debug\|verbose]` | Show or set how much log output reaches this serial console. Raises the web log page's capture level too, if that is what's holding the output back — see [Device log](#device-log). |
| `reboot` | Restart the device. |
| `factory-reset yes` | Erase saved WiFi and admin credentials, restoring compiled-in defaults (WiFi `AOG hub`/`password`; admin `admin`/`admin`), then reboot. Bare `factory-reset` (no `yes`) just prints this warning and changes nothing. |

**Locked out of the web UI?** Connect over serial and run `factory-reset yes`. The device
reboots with the default WiFi AP and `admin`/`admin` web login restored.

## Source layout

- `main/main.c` — startup, Ethernet/WiFi bridge setup
- `main/wifi_cfg.c` / `.h` — WiFi AP configuration (SSID, channel, credentials)
- `main/web_server.c` / `.h` — HTTP server backing the web UI
- `main/client_track.c` / `.h` — connected-client and traffic tracking
- `main/sys_monitor.c` / `.h` — system stats (heap, uptime) for the web UI
- `main/serial_console.c` / `.h` — interactive UART console for recovery/diagnostics
- `main/auth_cfg.c` / `.h` — admin username/password storage (NVS) with compiled-in defaults
- `main/log_buf.c` / `.h` — in-memory log ring behind `/logs`, and the serial/capture level split
- `main/webpage/index.html` — the web UI itself
- `main/webpage/admin.html` — the `/admin` settings page
- `main/webpage/logs.html` — the `/logs` device log page

## Troubleshooting high/jittery ping latency

Check `192.168.5.1/api/system` first: `cpu_pct` should be near-idle and `traffic_drops`
should be ~0 under normal load. If both look fine, the bridge's software
isn't the bottleneck — check WiFi channel congestion/interference next.

Per-client traffic accounting (`client_track.c`) is intentionally decoupled
from the packet-forwarding path: `traffic_input_wrapper`/`traffic_output_wrapper`
hand events to a bounded queue with a non-blocking send (drop-and-count on
full, never wait), and a dedicated low-priority task drains it. This means
forwarding can never block on accounting, so it shouldn't need to be a
suspect in future latency investigations — see the comments at those call
sites and around the `traffic_account_task` creation in `client_track_init()`.
