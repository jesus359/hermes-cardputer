# Technical Reference

## Architecture

```
[Cardputer] --WiFi--> [OpenAI-compatible API Server]
                          /v1/chat/completions
```

**Connection:** WiFi direct to API server. USB serial for debugging only.

---

## Firmware: cardputer_chat_wifi v0.2.4

### Configuration

Loaded from `/config.json` on SD card. Falls back to hardcoded defaults in source.

```json
{
  "ssid": "YOUR_WIFI_SSID",
  "password": "YOUR_WIFI_PASSWORD",
  "apiKey": "YOUR_API_KEY",
  "host": "YOUR_API_SERVER_IP",
  "port": 11434
}
```

### Display Layout (240x135 landscape)

```
|<--- 240px --->|
+------------------------------------+--+  ^
| WiFi ▎-65dB  Battery ▓▓▓▓░  API ✓ |  |  | 12px status bar
+------------------------------------+--+  
|                                    |  |
|  Chat content area                 |  |  110px chat
|  (word-wrapped, scrollable)        |  |
|                                    |  |
+------------------------------------+--+  
| > input text here_                 |  |  v 13px input bar
+------------------------------------+--+  ~5px scrollbar
```

### Layout Constants

```cpp
#define SCREEN_W       240
#define SCREEN_H       135
#define STATUS_H       12    // Top status bar
#define INPUT_H        13    // Bottom input bar
#define CHAT_H         110   // SCREEN_H - STATUS_H - INPUT_H
#define CHAR_W         6     // textSize(1) char width
#define LINE_H         10    // textSize(1) line height
#define CHARS_PER_LINE 39    // Max chars per line
#define VISIBLE_LINES  11    // Lines visible on screen
#define SCROLLBAR_X    235   // Scrollbar position
```

---

## Key Behaviors

### Keyboard: Position-Based Key-Down Detection

**Problem:** `M5Cardputer.update()` adds ~2s delay due to IMU/power/speaker/touch overhead.
**Solution:** Call components separately:

```cpp
// In loop():
M5.update();                                    // Display, buttons, power
M5Cardputer.Keyboard.updateKeyList();           // GPIO matrix scan
M5Cardputer.Keyboard.updateKeysState();         // Process into keysState
```

**Problem:** `status.word` only holds keys *currently held* during scan. Quick taps (<20ms) fall between cycles.
**Solution:** Track `Point2D_t` positions, not characters. Detect key-down by comparing current vs previous scan.

```cpp
struct KeyInfo {
  Point2D_t pos;
  unsigned long pressTime;
  bool consumed;
};
```

**Why position-based > character-based:**
- Same key pressed twice: char tracking skips second press (char matches prev scan)
- Position tracking correctly detects re-press at same physical location
- `getKey(pos)` re-evaluates modifier state each call

### Short/Long Press Differentiation

- **Short press:** New position detected in current scan that wasn't in previous scan
- **Long press:** Key held > 600ms, `consumed` flag prevents double-trigger
- **Long Backspace:** Clears entire input buffer
- **Long character:** Key repeat at 100ms rate

### Fn Key Combos

| Combo | Action |
|-------|--------|
| Fn + , | Scroll to bottom |
| Fn + ; | Scroll up 3 lines |
| Fn + . | Scroll down 3 lines |
| Fn + Backspace | Clear chat + conversation |
| Fn + Enter | Full reset |

---

## Streaming Responses

### HTTP Streaming (WiFiClient, not HTTPClient)

`HTTPClient` buffers the entire response. Use `WiFiClient` directly for streaming.

**Request format:**
```json
{
  "model": "your-model-name",
  "stream": true,
  "messages": [
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": "previous response"},
    {"role": "user", "content": "new message"}
  ]
}
```

**Response format:** Chunked transfer encoding. Each chunk is raw text (Ollama/Hermes format) or SSE (`data: {...}\n\n`).

**Key implementation:**
1. Send raw HTTP POST with `Connection: close`
2. Parse headers, detect `Transfer-Encoding: chunked`
3. Read chunk size (hex), read chunk data, repeat until size=0
4. Also handles SSE format as fallback

### Stream Display Strategy

**Don't** patch `chatBuf[targetLine]` in-place during streaming — old content bleeds through.

**Do** keep a raw text buffer and rewrap the entire thing on each refresh:

```cpp
String streamRawText = "";
int streamStartLine = 0;

void streamRefreshDisplay(String& rawText) {
  String displayText = "HERMES: " + rawText;
  int lineIdx = streamStartLine;
  int pos = 0;

  while (pos < (int)displayText.length()) {
    // word-boundary wrap at CHARS_PER_LINE
    // write to chatBuf[lineIdx] or addToChat if overflow
    lineIdx++;
  }

  scrollToBottom();
  drawChat();
}
```

Refresh every 2 characters for smooth streaming without flicker.

---

## Text Wrapping

### Word-Boundary Wrap

```cpp
int end = pos + CHARS_PER_LINE;  // 39 chars
int lastSpace = text.lastIndexOf(' ', end);
if (lastSpace <= pos) {
  // No space in range — hard break (word longer than line)
  chunk = text.substring(pos, end);
  pos = end;
} else {
  // Break at last space
  chunk = text.substring(pos, lastSpace);
  pos = lastSpace + 1;
}
```

**Edge cases handled:**
- Word longer than line width: hard break at line boundary
- No space in range: hard break (prevents infinite loop)
- Trailing space: skipped on next iteration

---

## Status Bar Icons

### WiFi Icon (4-bar signal strength)

```cpp
void drawWifiIcon(int x, int y, int rssi, bool connected) {
  // 4 bars, height 3/5/7/9px
  // Green >-55dBm, Yellow >-75dBm, Red (weak), X overlay if disconnected
}
```

### Battery Icon (outline + fill)

```cpp
void drawBatteryIcon(int x, int y, int pct, bool charging) {
  // 14x7 outline, 2px nub
  // Green >50%, Yellow >20%, Red, Cyan when charging
  // Charging bolt overlay when connected to power
}
```

### API Status Icon

```cpp
void drawApiIcon(int x, int y) {
  // Streaming: pulsing dot (cyan)
  // Waiting: 3 dots (yellow)
  // OK: checkmark (green)
  // Error: X (red)
}
```

Status bar refreshes every 2 seconds (not every loop — too expensive).

---

## Conversation History

Maintains last 10 user/assistant pairs. Each API request includes full history for multi-turn context.

```cpp
#define CONV_BUF_SIZE 10
String convRole[CONV_BUF_SIZE];
String convContent[CONV_BUF_SIZE];
int convCount = 0;
```

Sent with every request — API server handles context window management.

---

## Display Buffers

| Buffer | Size | Purpose |
|--------|------|---------|
| `chatBuf[]` | 200 lines | Wrapped display lines |
| `convRole[]` | 10 entries | Conversation role history |
| `convContent[]` | 10 entries | Conversation content history |
| `inputBuf` | 255 chars | Current user input |
| `streamRawText` | 4096 chars | Raw streamed response text |

**Auto-scroll:** Only scrolls to bottom if user was already at bottom (won't interrupt reading).

---

## Over-the-Air Updates (OTA)

### HTTP OTA — Primary Method

The Cardputer runs a web server on port 80 with an `/update` endpoint.

```bash
# Compile
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=min_spiffs --output-dir ./build .

# Upload
curl -F "file=@build/cardputer_chat_wifi.ino.bin" http://<CARDPUTER_IP>/update
```

Device auto-reboots after upload. ~8 seconds for 1.3MB binary.

### ArduinoOTA — Secondary Method

UDP-based protocol for Arduino IDE/CLI network port discovery. May be blocked by firewalls that reject callback connections.

### Boot Sequence

1. Power on → Display init
2. Boot animation (~2.5s): circle reveal → "HERMES" typewriter → progress bar
3. SD card config load
4. WiFi connect (up to 15s)
5. OTA servers start (port 80 HTTP, port 3232 UDP)
6. Chat UI ready

---

## Build & Flash

### Compile

```bash
cd firmware/cardputer_chat_wifi
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=min_spiffs --output-dir ./build .
```

### Upload via HTTP OTA

```bash
curl -F "file=@build/cardputer_chat_wifi.ino.bin" http://<CARDPUTER_IP>/update
```

### Flash via USB (first-time only)

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 460800 write_flash -z 0x0 build/cardputer_chat_wifi.ino.merged.bin
```

### Required Libraries

```bash
arduino-cli lib install "M5Cardputer"
arduino-cli lib install "M5Unified"
arduino-cli lib install "M5GFX"
arduino-cli lib install "ArduinoJson"
```

### Board Source

```bash
arduino-cli config set board_manager.additional_urls \
  https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json
```

**Note:** URL uses lowercase `arduino` (capital A returns 404).

---

## Quick Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Keyboard 2s delay | `M5Cardputer.update()` overhead | Use `M5.update()` + keyboard methods separately |
| Quick taps dropped | `status.word` only holds held keys | Position-based key-down detection |
| Same key double-press skipped | Character tracking can't distinguish re-press | Track `Point2D_t` positions |
| Stream text garbled | In-place line patching leaves old chars | Rewrap raw text buffer from scratch |
| Long responses crash | JSON buffer too small | ArduinoJson v7 auto-sizes, but check heap |
| WiFi won't connect | ESP32 only supports 2.4GHz | Verify network band |
| HTTP timeout | Long API response | 60s stream timeout, 30s connect timeout |
| OTA upload fails | Firewall blocks callback | Use HTTP OTA (curl) instead of ArduinoOTA |
| Device unresponsive after OTA | Bad firmware flashed | Hold BOOT+RESET, flash via USB |

---

## Hardware API Quick Reference

| Component | Access | Key Methods | Docs |
|-----------|--------|-------------|------|
| Display | `M5Cardputer.Display` | `setCursor()`, `print()`, `fillRect()`, `drawLine()` | [display](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/display) |
| Keyboard | `M5Cardputer.Keyboard` | `updateKeyList()`, `updateKeysState()`, `keysState()`, `keyList()`, `getKey()` | [keyboard](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/keyboard) |
| Speaker | `M5Cardputer.Speaker` | `tone()`, `begin()`, `end()` | [speaker](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/speaker) |
| Mic | `M5Cardputer.Mic` | `record()`, `begin()`, `end()` | [mic](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/mic) |
| Battery | `M5Cardputer.Power` | `getBatteryLevel()`, `getBatteryVoltage()`, `isCharging()` | [battery](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/battery) |
| SD Card | `SD` (Arduino lib) | `begin()`, `open()`, `listDir()` | [sdcard](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/sdcard) |
| IR | `IrSender` (Arduino-IRremote) | `sendNEC()`, `begin()`, `setSendPin()` | [ir](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/ir_nec) |
| IMU | `M5.Imu` (Adv only) | `update()`, `getImuData()` | [imu](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/imu) |
| Button | `M5Cardputer.BtnA` | `wasPressed()`, `wasReleased()` | [button](https://docs.m5stack.switch-science.com/en/arduino/m5cardputer/button) |
