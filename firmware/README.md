# Firmware

Hermes-Cardputer WiFi chat firmware for M5Stack Cardputer (ESP32-S3).

## Active Firmware

- **cardputer_chat_wifi/** — v0.2.4
  - `cardputer_chat_wifi.ino` — source (single file, ~2300 lines)
  - `config.json.example` — SD card config template

## Features

- WiFi direct to OpenAI-compatible API (no host bridge)
- Streaming responses via SSE
- Over-the-air firmware updates (HTTP POST to /update)
- Boot animation (circle reveal, typewriter, progress bar)
- Scrollable chat with word-boundary text wrapping
- Status bar: WiFi signal, battery, API state
- Short/long press keyboard with key repeat
- Fn combos (scroll, clear, reset, history)
- Multi-turn conversation context (10 exchanges)
- Auto-reconnect on WiFi drops
- Voice mode (Deepgram STT + ElevenLabs TTS)
- SD card config (/config.json)
- Web portal for SD card file management (via browser)

## Build

### Compile
```bash
cd firmware/cardputer_chat_wifi
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=min_spiffs --output-dir ./build .
```

### Upload (WiFi OTA)
```bash
curl -F "file=@build/cardputer_chat_wifi.ino.bin" http://<CARDPUTER_IP>/update
```

### Flash (USB, first-time only)
```bash
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 460800 \
  write_flash -z 0x0 build/cardputer_chat_wifi.ino.merged.bin
```

## Libraries

```bash
arduino-cli lib install "M5Cardputer"
arduino-cli lib install "M5Unified"
arduino-cli lib install "M5GFX"
arduino-cli lib install "ArduinoJson"
```

## Board Source

```bash
arduino-cli config set board_manager.additional_urls \
  https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json
```

## Configuration

Config loaded from `/config.json` on SD card at boot. Falls back to hardcoded defaults.

```json
{
  "ssid": "YOUR_WIFI_SSID",
  "password": "YOUR_WIFI_PASSWORD",
  "apiKey": "YOUR_API_KEY",
  "host": "YOUR_API_SERVER_IP",
  "port": 11434,

  "sttHost": "api.deepgram.com",
  "sttKey": "YOUR_DEEPGRAM_API_KEY",

  "ttsHost": "api.elevenlabs.io",
  "ttsKey": "YOUR_ELEVENLABS_API_KEY"
}
```

## Serial Debug

Connect via USB at 115200 baud:

| Command | Action |
|---------|--------|
| `status` | WiFi, RSSI, chat lines, heap, stream state |
| `clear` | Clear chat display |
| `test` | Send test message to API |
