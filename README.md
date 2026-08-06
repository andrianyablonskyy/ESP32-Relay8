## Project Overview

Firmware for an ESP32-based 8-channel relay board. Provides a Bootstrap-based web UI to view/control relays (individually and all-at-once), WiFi provisioning via a captive portal, a POST-only REST API for relay status/control, and OTA firmware updates. PlatformIO + Arduino framework, single-file firmware (`src/main.cpp`).

**Hardware**
- ESP32 dev kit, no external flash — internal flash only, partitioned via the default `esp32dev` scheme (LittleFS for the web assets).
- 8 relays on GPIOs 32, 33, 25, 26, 27, 14, 12, 13 (index 0-7 → relay 1-8).
- Status LED on GPIO 23, driven in `loop()` with a distinct blink pattern per device state (booting, connecting, working, connection failed, error).

## Project Structure

```
.
├── platformio.ini      # PlatformIO project/env config (build, upload, OTA)
├── src/main.cpp         # All firmware logic (single file, no modules/libs yet)
├── include/              # No project headers yet (PlatformIO stub README only)
├── lib/                  # No private libraries yet (PlatformIO stub README only)
├── data/                 # LittleFS image: web UI assets, uploaded separately from firmware
│   ├── index.html         # Relay control page (Bootstrap UI, polls /status every 5s)
│   ├── bootstrap.min.css.gz
│   ├── bootstrap.min.js.gz
│   └── style.css.gz       # Unused leftover from an earlier template
└── test/                 # No unit tests yet (PlatformIO stub README only)
```

## Commands

All commands run via the PlatformIO CLI (`pio`) from the project root.

- Build: `pio run`
- Upload firmware over USB: `pio run -t upload`
- Upload the `data/` filesystem image (LittleFS — required for the web UI to be served): `pio run -t uploadfs`
- Serial monitor (115200 baud): `pio device monitor`
- OTA upload: `pio run -e OTA -t upload` — uploads to the IP set in `platformio.ini`'s `[env:OTA]` section; the device advertises itself via ArduinoOTA as `esp32-ota-device`.
- No unit tests exist yet (`test/` only has the PlatformIO stub README); `pio test` will find nothing to run.
- Format: `clang-format -i src/main.cpp` (LLVM-based style, see `.clang-format`; the configured `clang-format.executable` path in `.vscode/settings.json` is a placeholder and needs to be set locally).

## Architecture

Everything lives in `src/main.cpp`; there's no split into modules/libs yet.

- **WiFi provisioning**: `WiFiManager` (`wm.autoConnect(...)`) runs at boot. On first boot / lost credentials it opens a captive-portal AP (`Relay8_Config_AP` / `12345678`) for configuration; on success it reconnects using saved credentials. The web server and OTA are only started **after** a successful WiFi connection (see `setup()` — everything past `wm.autoConnect` is nested in the `if (success)` branch, so a failed/timed-out connect leaves the device with no HTTP/OTA access).
- **Web server**: `ESPAsyncWebServer` on port 80, serving:
  - `GET /` → `data/index.html` from LittleFS, rendered through the `processor()` template callback (maps `%R1%`…`%R8%` placeholders to `relayStates[]`).
  - `GET /bootstrap.min.css`, `GET /bootstrap.min.js` → static assets from LittleFS.
  - `POST /status` → JSON dump of device IP and all 8 relay states, e.g. `{"ip":"...","relays":[{"id":1,"state":"ON"},...]}`.
  - `POST /set` → sets relay state(s) from a JSON body: `{"state":"on"|"off"}` sets all relays, `{"relay":1-8|"all","state":"on"|"off"}` sets one (or all), or an array of such objects for multiple updates in one call. Returns `{"result":"ok"|"error", "message"?:"..."}`.
  - 404 fallback logs the URL and returns plain text.
- **Filesystem**: LittleFS (`board_build.filesystem = littlefs`) holds the static web assets in `data/`. It's mounted with `LittleFS.begin(true)` (auto-format on first/corrupt mount). Assets must be pushed separately with `pio run -t uploadfs` — they are not part of the firmware binary.
- **Relay state**: kept purely in RAM (`relayPins[]` / `relayStates[]` parallel arrays), not persisted across reboot; on boot all relays are forced OFF.
- **Status LED**: `currentStatus` state machine (`BOOTING`, `CONNECTING`, `WORKING`, `NOT_CONFIGURED`, `CONNECTION_FAILED`, `ERROR`) drives a distinct non-blocking blink pattern per state in `loop()`.
- **Watchdog**: `esp_task_wdt_reset()` is fed every `loop()` iteration; `WDT_TIMEOUT` is defined (5s) but the watchdog does not appear to be explicitly initialized in `setup()`.
- **OTA**: `ArduinoOTA`, hostname `esp32-ota-device`, no password set (commented out in `setup()`) — anyone on the network can push firmware. `ArduinoOTA.handle()` is polled every `loop()` iteration.

### Known gaps vs. stated goals (useful context, not yet done)

- `data/style.css.gz` is an unused leftover from an earlier template; the current UI is served entirely via `bootstrap.min.css`/`bootstrap.min.js`, and nothing in `main.cpp` routes `/style.css` anymore.
- There's no GET-relay-status-for-a-single-relay route; only the all-relays `POST /status`.
- No authentication on the web UI, REST API, or OTA upload — anyone on the same network can control relays or push firmware.
- Web server and OTA are plain HTTP — no TLS.

## Future Plans

- Add user authentication
- Add HTTPS web server
- Add user credentials changing capability
