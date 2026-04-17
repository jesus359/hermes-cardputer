# Changelog

## v0.2 — OTA + Boot Animation

### Added
- HTTP OTA firmware updates — push .bin files over WiFi
- ArduinoOTA support (UDP network port discovery)
- Boot animation — circle reveal, typewriter "HERMES", progress bar
- Custom partition scheme (min_spiffs: 1.9MB app, 190KB SPIFFS)

### Changed
- Partition scheme: default (1.2MB) → min_spiffs (1.9MB) to fit OTA code
- Build command now requires `PartitionScheme=min_spiffs`
- USB flash only needed for first-time setup

## v0.1 — Initial Release

### Features
- WiFi direct to OpenAI-compatible API
- Streaming SSE responses (character-by-character)
- Scrollable chat with word-boundary text wrapping
- Status bar: WiFi RSSI, battery, API state
- Short/long press keyboard with key repeat
- Fn key combos (scroll, clear, reset, history)
- Multi-turn conversation context (10 exchanges)
- Voice mode (Deepgram STT + ElevenLabs TTS)
- SD card config (/config.json)
- File browser + editor mode
- IMU tilt-to-scroll (Cardputer-Adv only)
