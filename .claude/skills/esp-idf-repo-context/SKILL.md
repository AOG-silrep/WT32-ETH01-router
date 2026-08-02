---
name: esp-idf-repo-context
description: "Use when working on this ESP-IDF firmware project for builds, flashing, secure-boot signing, source navigation, or repo-specific troubleshooting. Captures the project target, signing flow, build commands, and source layout for future reuse."
argument-hint: "build | flash | diagnose | review"
user-invocable: true
---

# ESP-IDF Repo Context

## When to Use
- Building, flashing, or monitoring the WT32-ETH01 bridge firmware
- Reproducing the project setup on a new machine
- Diagnosing bridge behavior, latency, or upload problems
- Reviewing repository structure before making changes

## Environment

**The ESP-IDF path is machine-specific and is not recorded here.** Follow the discovery
procedure in `CLAUDE.md` at the repo root — it is the source of truth, and duplicating a
path or version here would just create a second answer that goes stale.

Target is `esp32` (`CONFIG_IDF_TARGET` in `sdkconfig`), so `idf.py set-target` is only
needed on a fresh checkout with no `sdkconfig`.

## Firmware Signing

Images are signed with ECDSA (secure boot v1 scheme, `--scheme ecdsa256`). The relevant
sdkconfig options are `CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME`,
`CONFIG_SECURE_SIGNED_ON_BOOT`, and `CONFIG_SECURE_SIGNED_ON_UPDATE`, with
`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y` — signatures are verified on boot and on OTA
update, but hardware secure boot is not burned into efuse.

The key is `secure_boot_signing_key.pem`. It is gitignored and must stay private.
Generate it once per machine if missing:

```sh
python -m espsecure generate-signing-key --version 1 --scheme ecdsa256 secure_boot_signing_key.pem
```

**Non-obvious:** because the bootloader verifies the app signature on boot, a device that
was previously flashed with an unsigned image cannot be moved onto signed firmware by OTA
— it needs a full serial reflash (bootloader included) to start enforcing the policy.

## Standard Workflow

```sh
idf.py build
idf.py -p <PORT> flash monitor
```

## Source Layout

All firmware sources live in `main/` and are listed in `main/CMakeLists.txt`.

- `main.c` — startup and bridge setup
- `wifi_cfg.c` / `.h` — soft-AP configuration, persisted in NVS
- `auth_cfg.c` / `.h` — admin credential storage in NVS, plus `auth_cfg_validate()` and
  `auth_cfg_password_is_default()`. Both the web handler and the serial console validate
  through here, so credential rules only need changing in one place.
- `web_server.c` / `.h` — HTTP server, Basic Auth, REST API, and OTA upload
- `serial_console.c` / `.h` — console commands: `wifi`, `admin`, `sysinfo`, `clients`,
  `loglevel`, `reboot`, `factory-reset`
- `client_track.c` / `.h` — per-client accounting and traffic history
- `sys_monitor.c` / `.h` — CPU load, heap, and throughput sampling
- `webpage/index.html` — dashboard UI
- `webpage/admin.html` — credential-change page

The two HTML files are embedded into the binary via `EMBED_TXTFILES` in
`main/CMakeLists.txt` and served from `web_server.c` through the `_binary_*_start` /
`_binary_*_end` symbols. **Editing them requires a rebuild and reflash** — there is no
filesystem to drop them into.

## Troubleshooting Notes

- `192.168.5.1/api/system` is the first stop for `cpu_pct` and `traffic_drops`. It needs
  Basic Auth: `curl -u <user>:<pass> http://192.168.5.1/api/system`.
- **A `403` from any `/api/*` route is not an auth failure** — it means the admin password
  is still the compiled-in default, and the device refuses to serve anything but `/admin`
  until a new one is set. A `401` is the actual bad-credentials case.
- Latency investigations should not suspect the packet-forwarding accounting path as a
  blocker — per-client accounting is intentionally decoupled from forwarding.
- The HTTP server runs a **single worker task**, and failed auth is deliberately delayed
  by one second. Anything that can issue overlapping requests has to account for that, or
  requests queue and time out in ways that look like the device is unreachable.

## Completion Checks

- ESP-IDF environment resolved via the `CLAUDE.md` procedure, not a hardcoded path
- A valid signing key exists before building
- Flash and monitor commands include the intended serial port
- Web UI changes were rebuilt and reflashed, not just edited on disk
- Source navigation stays aligned with the layout above
