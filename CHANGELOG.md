# Cardputer-Hermes CHANGELOG

## v0.2.4 — 2026-04-19

**Blue/turquoise color unification and code cleanup.**

### Changed
- Color palette unified to blue/turquoise — all system text, icons, and voice
  animations now use cyan (0x07FF) instead of mixed green/red/yellow
- System messages (SYS:, !!) now cyan instead of green
- Input bar prompt changed from yellow (0xFFE0) to cyan (0x07FF)
- WiFi signal "good" and API checkmark icons changed from green to cyan
- WiFi "medium" signal changed from yellow to deep blue (0x041F)
- Voice mode REC indicator changed from red pulse to cyan pulse
- Voice think dots changed from yellow to dim gray (0x8410)
- Voice waveform bars changed from green to cyan
- Consolidated 4 text-wrapping functions into 1 (~100 lines removed)
- `reportError()` now logs locally only (was sending to API — recursion risk)
- `drawVoiceAnim()` redundant inner-loop calls removed (3 of 11)
- Stream display refresh interval increased from 2 to 4 chars
- `handleApiRead()` now caps at 64KB to prevent heap exhaustion

### Fixed
- System message color inconsistency (green → cyan per theme)

### Removed
- Dead `AppMode` enum and `appMode` variable (only MODE_CHAT, never used)
- Dead `prevKeys` array and copy logic (104 bytes freed, never read)
- Dead `getShiftedChar()` forward declaration
- Dead `wrapAndAdd()` function (zero callers)
- Dead `wrapReplace()` function (zero callers)

### Technical Notes
- `reportError()` no longer sends errors to API — logs locally via Serial + chat
- `wrapToBuf()` is the single wrapping function; `streamRefreshDisplay` and
  `streamRenderFinal` are thin wrappers around it
- Firmware size: 1,386,143 bytes (70% of 1.9MB partition)
- Uploaded via HTTP OTA to 192.168.0.203

---

## v0.2.3 — 2026-04-17

**Web portal file manager.** Replaced on-device file browser/editor with a browser-based portal served from the ESP32.

### Added
- **Web portal** at `http://<device-ip>/` — SD card file manager in any browser
  - File list with directory navigation
  - Text editor with line numbers, Tab→spaces, Ctrl+S to save
  - Create new files, delete files, save edits
  - Blue/turquoise dark theme matching device aesthetics
- **REST API** for SD card operations:
  - `GET /api/files` — list files as JSON
  - `GET /api/read?path=` — read file content
  - `POST /api/write` — write file (JSON body: `{path, content}`)
  - `POST /api/delete` — delete file (JSON body: `{path}`)
- **Portal mode** — `/files` shows folder icon + IP on device screen
  - Enter key returns to chat (server stays running for OTA)
  - Prevents accidental chat input while browsing files on phone/laptop
- **Config auto-reload** — saving config.json via portal reloads settings

### Removed
- **On-device file browser** (MODE_BROWSER) — directory navigation on 240x135 screen
- **On-device line editor** (MODE_EDITOR) — line-by-line editing on tiny screen
- **Browser input prompt** (MODE_BROWSER_INPUT) — new file creation dialog
- All browser/editor state: `browserFiles[64]`, `editLines[200]`, dirty flags
- `ZZ_cardputer_fs.ino` conflicting stub

### Changed
- **Version string:** v0.2.2 → v0.2.3
- **AppMode enum:** simplified to `{ MODE_CHAT }` (removed 3 browser/editor modes)
- **Firmware size:** 1,358,091 → 1,365,887 bytes
- **Heap freed:** ~32KB from removed String arrays
- Web server always running (required for HTTP OTA `/update` endpoint)

### Technical Notes
- Portal HTML embedded as `PROGMEM` string (~7KB) — served from flash
- JSON escaping done char-by-char (no ArduinoJson for read responses to save heap)
- `SD.open()` requires leading `/` — `entry.name()` returns relative paths, normalized in all API handlers
- `portalActive` flag controls screen display, not server lifecycle
- `drawPortalScreen()` draws pixel-art folder icon, WiFi arcs, IP, hint text
- Uploaded via HTTP OTA to 192.168.0.203

---

## v0.2.2 — 2026-04-17

**Font size revert.** Reverted chat display from 2x to 1x for maximum information density.

### Changed
- **Chat font size:** textSize(2) → textSize(1) in drawChat()
- **Layout constants:** CHAR_W 12→6, LINE_H 18→10, CHARS_PER_LINE 19→39, VISIBLE_LINES 6→11
- **Display density:** ~2.5x more characters per line, ~1.8x more visible lines

### Technical Notes
- Firmware size: 1,358,091 bytes (69%)
- Uploaded via HTTP OTA to 192.168.0.203

---

## v0.2.1 — 2026-04-16

**Visual identity upgrade.** Replaced generic circle animation with Hermes caduceus.

### Changed
- **Boot animation:** Expanding circle → caduceus staff drawing animation
  - Phase 1: Staff rod draws downward (cyan vertical line)
  - Phase 2: Two intertwining serpents wind upward (cyan + deep blue sinusoidal curves)
  - Phase 3: Wings fan outward from staff top (3 feather lines per side)
  - Phase 4-7: Typewriter "HERMES" → subtitle pulse → progress bar → "Booting..."
- **Color palette:** All blue/turquoise — 0x07FF (cyan), 0x041F (deep blue), 0x07E0 (green status)

### Technical Notes
- Caduceus uses `sin()` for serpent curves — 2.5 windings with 12px amplitude
- Animation is ~2.3s total

---

## v0.2 — 2026-04-16

**Major checkpoint.** WiFi-only workflow achieved.

### Added
- **HTTP OTA firmware updates** — push .bin files over WiFi via `curl -F "file=@..." http://<ip>/update`
- **ArduinoOTA support** — network port discovery (blocked by macOS firewall, HTTP OTA is primary)
- **Boot animation** — expanding circle, typewriter "HERMES", progress bar, ~2.5s
- **Custom partition scheme** — `min_spiffs` (1.9MB app, 190KB SPIFFS) to fit OTA libraries

### Changed
- **Partition scheme:** Default (1.2MB) → min_spiffs (1.9MB)
- **Flashing workflow:** USB flash only for first-time setup. All subsequent updates via HTTP OTA.

---

## v0.1 — 2026-04-15

**Initial working version.** WiFi chat with Hermes.

### Features
- WiFi direct connection to Hermes API (192.168.0.240:11434)
- Streaming SSE responses with real-time display
- Scrollable chat buffer (200 lines)
- Status bar with WiFi RSSI, battery, API state
- Short/long press keyboard handling
- Fn key combos for navigation
- Multi-turn conversation context (10 exchanges)
- SD card config loading (/config.json)
- Voice mode (Deepgram STT + ElevenLabs TTS)
- IMU tilt-to-scroll (Cardputer-Adv only)
