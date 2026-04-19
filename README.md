# Hermes-Cardputer

WiFi chat terminal for OpenAI-compatible API servers. Runs on M5Stack Cardputer (ESP32-S3).

![Firmware](https://img.shields.io/badge/firmware-v0.2.4-blue)
![Board](https://img.shields.io/badge/board-M5Stack%20Cardputer-green)
![Arduino](https://img.shields.io/badge/arduino-ESP32%20S3-orange)

## What It Does

A pocket-sized WiFi chat terminal that connects to any OpenAI-compatible API server. Streams responses character-by-character on a 240x135 color display. Over-the-air firmware updates — no USB cable needed after initial flash.

```
[Cardputer] ←WiFi→ [Any OpenAI-compatible API]
```

## Features

- **Streaming responses** — character-by-character via Server-Sent Events
- **OTA updates** — push firmware over WiFi via HTTP POST
- **Boot animation** — expanding circle, typewriter text, progress bar
- **Scrollable chat** — word-boundary wrapping, 200-line buffer
- **Status bar** — WiFi signal, battery level, API state
- **Smart keyboard** — short/long press, key repeat, Fn combos
- **Conversation context** — last 10 exchanges for multi-turn chat
- **Voice mode** — STT (Deepgram) + TTS (ElevenLabs)
- **Web portal** — SD card file management via browser
- **Auto-reconnect** — WiFi drop recovery

## Quick Start

### 1. Configure

Copy the example config to your SD card as `/config.json`:

```json
{
  "ssid": "YOUR_WIFI_SSID",
  "password": "YOUR_WIFI_PASSWORD",
  "apiKey": "YOUR_API_KEY",
  "host": "YOUR_API_SERVER_IP",
  "port": 11434
}
```

### 2. First Flash (USB)

Put Cardputer in bootloader mode (hold BOOT + press RESET), then:

```bash
cd firmware/cardputer_chat_wifi
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=min_spiffs --output-dir ./build .
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 460800 write_flash -z 0x0 build/cardputer_chat_wifi.ino.merged.bin
```

### 3. Update Firmware (WiFi OTA)

After first flash, all updates go over the air:

```bash
# Find Cardputer IP
arp -a | grep "b0:81:84"

# Push update
curl -F "file=@build/cardputer_chat_wifi.ino.bin" http://<CARDPUTER_IP>/update
```

Device auto-reboots. ~8 seconds for 1.3MB.

### 4. Update via M5 Launcher (Alternative)

If using [M5 Launcher](https://github.com/bmorcelli/Launcher), add this to your `config.conf` favorites:

```json
{
  "name": "Hermes-Cardputer",
  "fid": "",
  "link": "https://github.com/jesus359/hermes-cardputer/releases/latest/download/cardputer_chat_wifi.ino.bin"
}
```

## Controls

| Key | Action |
|-----|--------|
| Type + Enter | Send message |
| Backspace | Delete char |
| Hold Backspace | Clear input |
| Hold char | Key repeat |
| Fn + , | Jump to bottom |
| Fn + ; | Scroll up |
| Fn + . | Scroll down |
| Fn + Backspace | Clear chat |
| Fn + Enter | Full reset |
| Fn + Up (`) | Input history up |
| Fn + Down (Tab) | Input history down |
| /help | Show commands |
| /clear | Clear chat |
| /voice | Toggle voice mode |
| /files | Open web portal (shows URL) |
| /update | Check firmware version |

## Project Structure

```
├── README.md
├── CHANGELOG.md
├── Technical Reference.md       Architecture, display, protocols
├── OTA Guide.md                 Over-the-air update workflow
├── LICENSE
├── .gitignore
└── firmware/
    ├── README.md
    └── cardputer_chat_wifi/
        ├── cardputer_chat_wifi.ino
        └── config.json.example
```

## Documentation

- [Technical Reference](Technical%20Reference.md) — Architecture, display layout, streaming protocol
- [OTA Guide](OTA%20Guide.md) — Over-the-air update workflow and troubleshooting
- [Firmware README](firmware/README.md) — Build, flash, and library setup

## Requirements

- M5Stack Cardputer (ESP32-S3)
- WiFi network (2.4GHz)
- OpenAI-compatible API server (Ollama, LM Studio, vLLM, etc.)
- Optional: SD card (config persistence, web portal)
- Optional: Deepgram API key (voice STT)
- Optional: ElevenLabs API key (voice TTS)

## License

MIT
