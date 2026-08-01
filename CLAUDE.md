# wt32-bridge

## Network

The end device is on the `192.168.5.x` network.

## ESP-IDF location

The ESP-IDF path is machine-specific (not checked into this repo) — don't
assume a fixed path or search the whole filesystem for it. To find it:

1. If `build/` exists, read `idf_path` from `build/project_description.json`
   — this is the authoritative source, since it's what the last build
   actually used.
2. Otherwise, check the `IDF_PATH` env var, or `idf.currentSetup` /
   `idf.espIdfPath` in `.vscode/settings.json` (note: on at least one dev
   machine, `idf.espIdfPath` there is stale and points at a path that
   doesn't exist — prefer `idf.currentSetup` or step 1 if they disagree).

Component sources (e.g. `esp_http_server`, `esp_wifi`, `driver`) live under
`<idf_path>/components/`.

Toolchain/tools root is set separately via `idf.toolsPath` in
`.vscode/settings.json` (also machine-specific).
