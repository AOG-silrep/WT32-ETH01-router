# wt32-bridge

Version: 1.0.1 (see [version.txt](version.txt); shown on the web UI as "Rev 1.0.1")

An ESP-IDF firmware project that turns a [WT32-ETH01](https://en.wireless-tag.com/product-item-2.html) (ESP32 + LAN8720 Ethernet PHY) module into an Ethernet↔WiFi bridge for [AgOpenGPS](https://github.com/AgOpenGPS-Official/AgOpenGPS). It bridges the wired Ethernet interface and a WiFi access point at the network layer, and serves a small web UI for configuration and live monitoring.

## Features

- Ethernet (LAN8720 PHY) ↔ WiFi AP network bridge (`esp_netif_br_glue`)
- Web UI (`main/webpage/index.html`) for WiFi configuration and live status
- Connected-client tracking and per-device traffic graphing
- System monitor (heap, uptime, etc.) surfaced on the web UI, refreshed every second
- Configurable WiFi channel (1, 6, or 11)

## Web UI login

The web UI (`/`) and settings page (`/admin`) are behind HTTP Basic Auth. Until changed, the
login is the compiled-in default:

- Username: `admin`
- Password: `admin`

Change it from `/admin` (linked via the "Update username & password" button on the main page)
or with the serial console's `admin` command (see below). Change the default before deploying
on any network you don't fully trust — anyone who can reach the web UI can use it.

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
| `loglevel [none\|error\|warn\|info\|debug\|verbose]` | Show or set log verbosity. |
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
- `main/webpage/index.html` — the web UI itself
- `main/webpage/admin.html` — the `/admin` settings page

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
