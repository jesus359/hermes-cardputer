/*
 * Cardputer-Hermes Chat v0.2.3 (WiFi - Full Featured)
 * 
 * Features:
 * - Streaming responses (chunked HTTP)
 * - Scrollable chat with word-boundary text wrapping
 * - Status bar: WiFi RSSI (icon), battery (icon), API state (icon)
 * - Fn+; scroll up / Fn+. scroll down / Fn+Enter=bottom
 * - Fn+Backspace clear chat / long Backspace clear input
 * - Short/long press differentiation (600ms threshold)
 * - Conversation history context (multi-turn)
 * - Web portal file manager (SD card edit via browser)
 * 
 * Hardware: M5Stack Cardputer
 * Connection: WiFi to Hermes API (OpenAI-compatible)
 */

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include <SD.h>
#include <SPI.h>
#include <WiFiClientSecure.h>

// OTA
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>

// ==================== SETTINGS (overridden by /config.json on SD) ===========
String ssid     = "";
String password = "";
String apiKey   = "";
String host     = "YOUR_API_SERVER_IP";
int    port     = 11434;

// Cloud STT (Deepgram)
String sttHost  = "api.deepgram.com";
int    sttPort  = 443;
String sttKey   = "";  // Set in config.json: "sttKey"
String sttModel = "nova-2";

// Cloud TTS (ElevenLabs)
String ttsHost  = "api.elevenlabs.io";
int    ttsPort  = 443;
String ttsKey   = "";  // Set in config.json: "ttsKey"
String ttsModel = "eleven_turbo_v2";
String ttsVoice = "21m00Tcm4TlvDq8ikWAM"; // Rachel
// ==================================================

// Display layout
#define SCREEN_W       240
#define SCREEN_H       135
#define STATUS_H       12
#define INPUT_H        13
#define CHAT_Y         STATUS_H
#define CHAT_H         (SCREEN_H - STATUS_H - INPUT_H)  // 110
#define CHAR_W         6
#define LINE_H         10
#define CHARS_PER_LINE 39   // (SCREEN_W - 6) / CHAR_W
#define VISIBLE_LINES  11   // CHAT_H / LINE_H
#define SCROLLBAR_X    (SCREEN_W - 5)

// Buffers
#define CHAT_BUF_SIZE  200
#define CONV_BUF_SIZE  10
#define MAX_INPUT      255
#define STREAM_BUF_SIZE 4096

// Colors
#define COL_STATUS_BG  0x2104
#define COL_INPUT_BG   0x0000
#define COL_YOU        0xFFFF
#define COL_HERMES     0x07FF
#define COL_ERR        0xF800
#define COL_SYS        0x07E0
#define COL_SCROLLBAR  0x4208
#define COL_SCROLL_TH  0xBDF7
#define COL_ICON_ON    0x07E0
#define COL_ICON_MED   0xFFE0
#define COL_ICON_WEAK  0xF800
#define COL_ICON_OFF   0x4208

// HTTP
#define HTTP_TIMEOUT   30000

// ============================================================

// HTTP OTA server (backup to ArduinoOTA)
WebServer otaServer(80);

// ============================================================
// STATE
// ============================================================

// Chat display buffer (wrapped lines)
String chatBuf[CHAT_BUF_SIZE];
int chatCount = 0;
int scrollPos = 0;

// Conversation history for API context
String convRole[CONV_BUF_SIZE];
String convContent[CONV_BUF_SIZE];
int convCount = 0;

// Input
String inputBuf = "";

// Status
bool isBusy = false;
bool apiStreaming = false;
int lastApiStatus = 0;
unsigned long lastApiTime = 0;

// Stream raw text buffer (for clean rewrap during streaming)
String streamRawText = "";
int streamStartLine = 0;  // First line index where stream content begins
bool streamActive = false;

// SD card state (lazy-mount: only begin once)
bool sdMounted = false;

// IMU availability (only Cardputer-Adv has IMU)
bool imuAvailable = false;

// App Modes
// App mode — browser/editor replaced by web portal (v0.2.3)
enum AppMode { MODE_CHAT };
AppMode appMode = MODE_CHAT;

// Voice mode toggle and state machine
bool voiceMode = false;
enum VoiceState { V_IDLE, V_LISTEN, V_UPLOAD, V_THINK, V_FETCH, V_SPEAK };
VoiceState vState = V_IDLE;

// Web portal handles file operations (no on-device browser/editor)
void drawPortalScreen();
uint8_t getShiftedChar(uint8_t key);

// Input history (5 entries, cycle with Fn+Up / Fn+Down)
#define HIST_SIZE 5
String inputHistory[HIST_SIZE];
int histCount = 0;
int histIdx   = -1;  // -1 = not browsing history

// Heap warning threshold
#define HEAP_WARN_BYTES 20000

// Web portal state — on-demand server (v0.2.3)
bool portalActive = false;
String portalIP = "";

// ============================================================
// KEYBOARD TRACKING
// ============================================================
#define MAX_KEYS 8
#define LONG_PRESS_MS 600

struct KeyInfo {
  Point2D_t pos;
  unsigned long pressTime;
  bool consumed;
};

KeyInfo activeKeys[MAX_KEYS];
int activeKeyCount = 0;
KeyInfo prevKeys[MAX_KEYS];
int prevKeyCount = 0;

bool keyInList(Point2D_t p, KeyInfo* list, int count) {
  for (int i = 0; i < count; i++) {
    if (p == list[i].pos) return true;
  }
  return false;
}

int findKeyIndex(Point2D_t p, KeyInfo* list, int count) {
  for (int i = 0; i < count; i++) {
    if (p == list[i].pos) return i;
  }
  return -1;
}

// ============================================================
// TEXT WRAPPING
// ============================================================

// Wrap text into lines, returning count added
// Respects word boundaries, handles words longer than line width
int wrapAndAdd(String text, uint16_t color) {
  if (text.length() == 0) {
    addToChat("");
    return 1;
  }
  
  int linesAdded = 0;
  int pos = 0;
  
  while (pos < (int)text.length()) {
    int remaining = text.length() - pos;
    
    if (remaining <= CHARS_PER_LINE) {
      addToChat(text.substring(pos));
      linesAdded++;
      break;
    }
    
    // Find best break point
    int end = pos + CHARS_PER_LINE;
    int lastSpace = text.lastIndexOf(' ', end);
    
    if (lastSpace <= pos) {
      // No space in range — hard break (word longer than line)
      addToChat(text.substring(pos, end));
      pos = end;
    } else {
      // Break at last space
      addToChat(text.substring(pos, lastSpace));
      pos = lastSpace + 1;  // Skip the space
    }
    linesAdded++;
  }
  
  return linesAdded;
}

// Wrap text and replace lines starting at startIdx, adding overflow
// Returns total lines consumed (some may be new)
int wrapReplace(String text, int startIdx) {
  int lineIdx = startIdx;
  int pos = 0;
  
  while (pos < (int)text.length()) {
    int remaining = text.length() - pos;
    String chunk;
    
    if (remaining <= CHARS_PER_LINE) {
      chunk = text.substring(pos);
      pos = text.length();
    } else {
      int end = pos + CHARS_PER_LINE;
      int lastSpace = text.lastIndexOf(' ', end);
      if (lastSpace <= pos) {
        chunk = text.substring(pos, end);
        pos = end;
      } else {
        chunk = text.substring(pos, lastSpace);
        pos = lastSpace + 1;
      }
    }
    
    if (lineIdx < chatCount) {
      chatBuf[lineIdx] = chunk;
    } else {
      addToChat(chunk);
    }
    lineIdx++;
  }
  
  // Clear any leftover lines from previous (longer) render
  while (lineIdx < chatCount && lineIdx < startIdx + 20) {
    // Only clear if this line was part of our previous render
    // (heuristic: if it starts with spaces or is after our content)
    // Actually, safer to just leave them — they're part of older content
    break;
  }
  
  return lineIdx - startIdx;
}

void addToChat(String line) {
  bool atBottom = (chatCount <= VISIBLE_LINES) || 
                  (scrollPos >= chatCount - VISIBLE_LINES);
  
  if (chatCount < CHAT_BUF_SIZE) {
    chatBuf[chatCount] = line;
    chatCount++;
  } else {
    // Shift buffer
    for (int i = 0; i < CHAT_BUF_SIZE - 1; i++) chatBuf[i] = chatBuf[i + 1];
    chatBuf[CHAT_BUF_SIZE - 1] = line;
    // Adjust stream start if active
    if (streamActive && streamStartLine > 0) streamStartLine--;
  }
  
  if (atBottom) scrollPos = max(0, chatCount - VISIBLE_LINES);
}

// ============================================================
// STATUS BAR ICONS
// ============================================================

void drawWifiIcon(int x, int y, int rssi, bool connected) {
  // 4-bar signal icon (9x9 pixels)
  int bars = 0;
  uint16_t col = COL_ICON_OFF;
  
  if (!connected) {
    col = COL_ICON_WEAK;
    bars = 0;
  } else if (rssi > -55) {
    col = COL_ICON_ON;
    bars = 4;
  } else if (rssi > -65) {
    col = COL_ICON_ON;
    bars = 3;
  } else if (rssi > -75) {
    col = COL_ICON_MED;
    bars = 2;
  } else {
    col = COL_ICON_WEAK;
    bars = 1;
  }
  
  // Draw signal bars (taller = stronger)
  int barH[] = {3, 5, 7, 9};
  for (int i = 0; i < 4; i++) {
    uint16_t c = (i < bars) ? col : COL_ICON_OFF;
    M5Cardputer.Display.fillRect(x + i * 3, y + (9 - barH[i]), 2, barH[i], c);
  }
  
  // If no connection, draw X
  if (!connected) {
    M5Cardputer.Display.drawLine(x, y, x + 8, y + 8, COL_ICON_WEAK);
    M5Cardputer.Display.drawLine(x + 8, y, x, y + 8, COL_ICON_WEAK);
  }
}

void drawBatteryIcon(int x, int y, int pct, bool charging) {
  // Battery outline (14x7)
  M5Cardputer.Display.drawRect(x, y, 14, 7, 0x8410);
  M5Cardputer.Display.fillRect(x + 14, y + 2, 2, 3, 0x8410);
  
  // Fill level
  int fillW = map(constrain(pct, 0, 100), 0, 100, 0, 12);
  uint16_t col;
  if (charging) {
    col = 0x07FF;  // Cyan when charging
  } else if (pct > 50) {
    col = COL_ICON_ON;
  } else if (pct > 20) {
    col = COL_ICON_MED;
  } else {
    col = COL_ICON_WEAK;
  }
  
  if (fillW > 0) {
    M5Cardputer.Display.fillRect(x + 1, y + 1, fillW, 5, col);
  }
  
  // Charging bolt indicator
  if (charging) {
    M5Cardputer.Display.drawLine(x + 6, y, x + 8, y + 3, WHITE);
    M5Cardputer.Display.drawLine(x + 8, y + 3, x + 6, y + 3, WHITE);
    M5Cardputer.Display.drawLine(x + 6, y + 3, x + 8, y + 6, WHITE);
  }
}

void drawApiIcon(int x, int y) {
  // Circle-based status indicator
  if (apiStreaming) {
    // Pulsing dot (just show filled)
    M5Cardputer.Display.fillCircle(x + 4, y + 4, 4, 0x07FF);
    M5Cardputer.Display.fillCircle(x + 4, y + 4, 2, WHITE);
  } else if (isBusy) {
    // Waiting: 3 dots
    M5Cardputer.Display.fillCircle(x + 1, y + 4, 2, 0xFFE0);
    M5Cardputer.Display.fillCircle(x + 7, y + 4, 2, 0xFFE0);
    M5Cardputer.Display.fillCircle(x + 13, y + 4, 2, 0xFFE0);
  } else if (lastApiStatus == 0 || lastApiStatus == 200) {
    // OK: checkmark
    M5Cardputer.Display.drawLine(x, y + 4, x + 3, y + 7, COL_ICON_ON);
    M5Cardputer.Display.drawLine(x + 3, y + 7, x + 9, y + 1, COL_ICON_ON);
  } else {
    // Error: X
    M5Cardputer.Display.drawLine(x, y, x + 8, y + 8, COL_ICON_WEAK);
    M5Cardputer.Display.drawLine(x + 8, y, x, y + 8, COL_ICON_WEAK);
  }
}

// ============================================================
// DISPLAY
// ============================================================

// Draw dynamic voice animations in the center of the status bar
void drawVoiceAnim() {
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < 100) return; // 10 FPS
  lastFrame = millis();
  
  static int frameIdx = 0;
  frameIdx++;

  // Clear animation rect (X: 90 to 140)
  M5Cardputer.Display.fillRect(90, 0, 50, STATUS_H, COL_STATUS_BG);
  
  if (vState == V_IDLE) {
    if (voiceMode) {
      M5Cardputer.Display.setCursor(104, 2);
      M5Cardputer.Display.setTextColor(0x07E0); // Bright green
      M5Cardputer.Display.print("[VOX]");
    }
    return;
  }
  
  int cx = 110; // Center X
  int cy = 6;   // Center Y
  
  if (vState == V_LISTEN) {
    // Pulsing recording circle
    M5Cardputer.Display.setTextColor(TFT_RED);
    int r = (frameIdx % 4) + 1;
    M5Cardputer.Display.fillCircle(cx, cy, r, TFT_RED);
    M5Cardputer.Display.setCursor(cx + 8, 2);
    M5Cardputer.Display.print("REC");
    
  } else if (vState == V_UPLOAD) {
    // Fast arrows up
    M5Cardputer.Display.setTextColor(TFT_CYAN);
    M5Cardputer.Display.setCursor(cx - 12, 2);
    int step = frameIdx % 3;
    if (step == 0) M5Cardputer.Display.print(" .^.");
    if (step == 1) M5Cardputer.Display.print(".^. ");
    if (step == 2) M5Cardputer.Display.print("^. .");
    
  } else if (vState == V_THINK) {
    // Pulsing dots
    M5Cardputer.Display.setTextColor(TFT_YELLOW);
    M5Cardputer.Display.setCursor(cx - 12, 2);
    int dots = (frameIdx % 4);
    String s = "";
    for(int i=0; i<dots; i++) s += ".";
    M5Cardputer.Display.print(s);
    
  } else if (vState == V_FETCH) {
    // Fast arrows down
    M5Cardputer.Display.setTextColor(TFT_ORANGE);
    M5Cardputer.Display.setCursor(cx - 12, 2);
    int step = frameIdx % 3;
    if (step == 0) M5Cardputer.Display.print(" .v.");
    if (step == 1) M5Cardputer.Display.print(".v. ");
    if (step == 2) M5Cardputer.Display.print("v. .");
    
  } else if (vState == V_SPEAK) {
    // Waveform
    M5Cardputer.Display.setTextColor(TFT_GREEN);
    for (int i=0; i<5; i++) {
      int h = (random(2, 6)); // Random wave heights
      M5Cardputer.Display.drawLine(cx - 8 + (i*4), cy - h, cx - 8 + (i*4), cy + h, TFT_GREEN);
    }
  }
}

void redrawAll() {
  M5Cardputer.Display.fillScreen(BLACK);
  drawStatusBar();
  drawChat();
  drawInputBar();
}

void drawStatusBar() {
  M5Cardputer.Display.fillRect(0, 0, SCREEN_W, STATUS_H, COL_STATUS_BG);
  M5Cardputer.Display.setTextSize(1);
  
  // WiFi icon (left)
  int rssi = WiFi.RSSI();
  drawWifiIcon(2, 1, rssi, WiFi.status() == WL_CONNECTED);
  
  // RSSI dBm text
  M5Cardputer.Display.setTextColor(0x8410);
  M5Cardputer.Display.setCursor(16, 2);
  if (WiFi.status() == WL_CONNECTED) {
    M5Cardputer.Display.print(String(rssi));
  } else {
    M5Cardputer.Display.print("--");
  }
  
  // Battery icon (center)
  int battPct = M5.Power.getBatteryLevel();
  bool charging = M5.Power.isCharging();
  drawBatteryIcon(108, 2, battPct, charging);
  
  M5Cardputer.Display.setCursor(126, 2);
  M5Cardputer.Display.setTextColor(0x8410);
  M5Cardputer.Display.print(String(battPct) + "%");
  
  // Heap low warning: overrides API label if memory is critical
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < HEAP_WARN_BYTES) {
    M5Cardputer.Display.setCursor(152, 2);
    M5Cardputer.Display.setTextColor(COL_ICON_WEAK);
    M5Cardputer.Display.print("MEM!");
  } else if (voiceMode) {
    M5Cardputer.Display.setCursor(146, 2);
    M5Cardputer.Display.setTextColor(0x07E0); // Bright green
    M5Cardputer.Display.print("[VOX]");
  } else {
    // API status icon (right)
    drawApiIcon(218, 1);
    
    // API text label
    M5Cardputer.Display.setCursor(170, 2);
    if (apiStreaming) {
      M5Cardputer.Display.setTextColor(0x07FF);
      M5Cardputer.Display.print("STRM");
    } else if (isBusy) {
      M5Cardputer.Display.setTextColor(0xFFE0);
      M5Cardputer.Display.print("WAIT");
    } else if (lastApiStatus == 0 || lastApiStatus == 200) {
      M5Cardputer.Display.setTextColor(COL_ICON_ON);
      M5Cardputer.Display.print("API");
    } else {
      M5Cardputer.Display.setTextColor(COL_ICON_WEAK);
      M5Cardputer.Display.print("E" + String(lastApiStatus));
    }
  }
  drawVoiceAnim();
}

void drawChat() {
  // Clear chat area
  M5Cardputer.Display.fillRect(0, CHAT_Y, SCROLLBAR_X, CHAT_H, BLACK);
  M5Cardputer.Display.setTextSize(1);
  
  int start = scrollPos;
  int end = min(start + VISIBLE_LINES, chatCount);
  
  for (int i = start; i < end; i++) {
    int y = CHAT_Y + (i - start) * LINE_H;
    M5Cardputer.Display.setCursor(0, y);
    
    String line = chatBuf[i];
    if (line.startsWith("YOU:")) {
      M5Cardputer.Display.setTextColor(COL_YOU);
    } else if (line.startsWith("HERMES:")) {
      M5Cardputer.Display.setTextColor(COL_HERMES);
    } else if (line.startsWith("!!") || line.startsWith("ERR:")) {
      M5Cardputer.Display.setTextColor(COL_ERR);
    } else {
      M5Cardputer.Display.setTextColor(COL_SYS);
    }
    
    // Truncate to screen width
    if (line.length() > CHARS_PER_LINE) {
      line = line.substring(0, CHARS_PER_LINE);
    }
    M5Cardputer.Display.print(line);
  }
  
  drawScrollbar();
}

void drawScrollbar() {
  M5Cardputer.Display.fillRect(SCROLLBAR_X, CHAT_Y, 5, CHAT_H, COL_SCROLLBAR);
  
  if (chatCount <= VISIBLE_LINES) return;
  
  float ratio = (float)VISIBLE_LINES / chatCount;
  int thumbH = max(8, (int)(CHAT_H * ratio));
  float scrollRatio = (float)scrollPos / max(1, chatCount - VISIBLE_LINES);
  int thumbY = CHAT_Y + (int)(scrollRatio * (CHAT_H - thumbH));
  
  M5Cardputer.Display.fillRect(SCROLLBAR_X, thumbY, 5, thumbH, COL_SCROLL_TH);
}

void drawInputBar() {
  M5Cardputer.Display.fillRect(0, SCREEN_H - INPUT_H, SCREEN_W, INPUT_H, COL_INPUT_BG);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(0, SCREEN_H - INPUT_H + 2);
  M5Cardputer.Display.setTextColor(0xFFE0);
  String display = "> " + inputBuf;
  if (display.length() > CHARS_PER_LINE) {
    display = ">" + display.substring(display.length() - CHARS_PER_LINE + 1);
  }
  M5Cardputer.Display.print(display + "_");
}

void scrollUp(int lines) {
  if (chatCount <= VISIBLE_LINES) return;
  scrollPos = max(0, scrollPos - lines);
  drawChat();
}

void scrollDown(int lines) {
  if (chatCount <= VISIBLE_LINES) return;
  scrollPos = min(chatCount - VISIBLE_LINES, scrollPos + lines);
  drawChat();
}

void scrollToBottom() {
  scrollPos = max(0, chatCount - VISIBLE_LINES);
}

// ============================================================
// CONVERSATION HISTORY
// ============================================================

// Push a prompt string into rolling input history
void pushHistory(const String& s) {
  // Shift down if full
  if (histCount == HIST_SIZE) {
    for (int i = 0; i < HIST_SIZE - 1; i++) inputHistory[i] = inputHistory[i + 1];
    histCount--;
  }
  inputHistory[histCount++] = s;
  histIdx = -1;  // Reset cursor whenever a new entry is added
}

void addConversation(String role, String content) {
  // Lazy SD log: only attempt if card was mounted at boot
  if (sdMounted) {
    File file = SD.open("/hermes_log.txt", FILE_APPEND);
    if (file) {
      file.println(role + ": " + content);
      file.close();
    }
  }

  if (convCount < CONV_BUF_SIZE) {
    convRole[convCount] = role;
    convContent[convCount] = content;
    convCount++;
  } else {
    for (int i = 0; i < CONV_BUF_SIZE - 1; i++) {
      convRole[i] = convRole[i + 1];
      convContent[i] = convContent[i + 1];
    }
    convRole[CONV_BUF_SIZE - 1] = role;
    convContent[CONV_BUF_SIZE - 1] = content;
  }
}

void clearConversation() {
  convCount = 0;
}

void clearChat() {
  chatCount = 0;
  scrollPos = 0;
  clearConversation();
  inputBuf = "";
  streamActive = false;
  streamRawText = "";
  streamStartLine = 0;
  redrawAll();
}

// ============================================================
// ERROR REPORTING
// ============================================================

// Central error handler: shows error on screen AND forwards to Hermes
// so the agent always knows what failed and where.
void reportError(const String& location, const String& detail) {
  String msg = "!! [" + location + "] " + detail;
  Serial.println(msg);
  addToChat(msg);
  drawChat();
  
  // Forward the traceback to Hermes as a system message
  // so it can acknowledge or help diagnose the issue
  String prompt = "[DEVICE ERROR] @ " + location + ": " + detail
                + " | Free heap: " + String(ESP.getFreeHeap())
                + " | WiFi: " + String(WiFi.status() == WL_CONNECTED ? "OK" : "DOWN")
                + " | SD: " + String(sdMounted ? "OK" : "MISSING");
  streamFromHermes(prompt);
}

// Read HTTP status code from a connected WiFiClient
// Leaves the stream positioned just after the status line.
int parseHttpStatus(WiFiClient& client) {
  unsigned long t = millis();
  while (!client.available() && millis() - t < HTTP_TIMEOUT) delay(5);
  if (!client.available()) return -1; // Timeout
  String statusLine = client.readStringUntil('\n');
  // Format: "HTTP/1.1 200 OK"
  int space1 = statusLine.indexOf(' ');
  if (space1 < 0) return -1;
  int space2 = statusLine.indexOf(' ', space1 + 1);
  if (space2 < 0) space2 = statusLine.length();
  return statusLine.substring(space1 + 1, space2).toInt();
}

// Skip remaining HTTP headers, return true if chunked transfer-encoding
bool skipHttpHeaders(WiFiClient& client, bool& chunkedOut) {
  chunkedOut = false;
  unsigned long t = millis();
  while (client.connected() || client.available()) {
    if (!client.available()) {
      if (millis() - t > HTTP_TIMEOUT) return false;
      delay(5);
      continue;
    }
    String h = client.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) return true; // blank line = end of headers
    String hl = h;
    hl.toLowerCase();
    if (hl.startsWith("transfer-encoding:") && hl.indexOf("chunked") >= 0) {
      chunkedOut = true;
    }
    t = millis();
  }
  return false;
}

// ============================================================
// STREAMING HTTP
// ============================================================

void streamFromHermes(String prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    addToChat("!! WiFi Lost !!");
    drawChat();
    return;
  }

  isBusy = true;
  apiStreaming = false;
  lastApiStatus = 0;
  lastApiTime = millis();
  drawStatusBar();
  
  // Display user message
  addToChat("YOU: " + prompt);
  addConversation("user", prompt);
  
  // Prepare stream display
  streamRawText = "";
  streamStartLine = chatCount;
  streamActive = true;
  
  // Reserve placeholder lines
  addToChat("HERMES: ");
  addToChat("");
  scrollToBottom();
  drawChat();
  
  WiFiClient client;
  if (!client.connect(host.c_str(), port, HTTP_TIMEOUT)) {
    chatBuf[streamStartLine] = "HERMES: Connection failed";
    // Clear extra placeholder
    if (streamStartLine + 1 < chatCount) {
      chatBuf[streamStartLine + 1] = "";
    }
    lastApiStatus = -1;
    isBusy = false;
    streamActive = false;
    drawStatusBar();
    drawChat();
    drawInputBar();
    return;
  }
  
  // Build JSON with conversation history
  JsonDocument doc;
  doc["model"] = "hermes-agent";
  doc["stream"] = true;
  JsonArray msgs = doc.createNestedArray("messages");
  
  for (int i = 0; i < convCount - 1; i++) {
    JsonObject m = msgs.createNestedObject();
    m["role"] = convRole[i];
    m["content"] = convContent[i];
  }
  JsonObject current = msgs.createNestedObject();
  current["role"] = "user";
  current["content"] = prompt;
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  // Send HTTP request
  client.print("POST /v1/chat/completions HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  client.print(":");
  client.print(port);
  client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Authorization: Bearer ");
  client.print(apiKey);
  client.print("\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: ");
  client.print(jsonStr.length());
  client.print("\r\n\r\n");
  client.print(jsonStr);
  
  // Wait for response headers
  unsigned long headerStart = millis();
  while (!client.available()) {
    drawVoiceAnim();
    if (millis() - headerStart > 30000) {
      chatBuf[streamStartLine] = "HERMES: Timeout";
      if (streamStartLine + 1 < chatCount) chatBuf[streamStartLine + 1] = "";
      lastApiStatus = -2;
      client.stop();
      isBusy = false;
      streamActive = false;
      drawStatusBar();
      drawChat();
      drawInputBar();
      return;
    }
    delay(10);
  }
  
  // Wait for and parse HTTP status
  int httpStatus = parseHttpStatus(client);
  if (httpStatus != 200) {
    // Read error body
    bool dummy; skipHttpHeaders(client, dummy);
    String errBody = "";
    unsigned long t = millis();
    while ((client.available() || client.connected()) && millis() - t < 5000) {
      if (client.available()) errBody += (char)client.read();
    }
    client.stop();
    String errMsg = "HTTP " + String(httpStatus) + " from Hermes";
    if (errBody.length() > 0 && errBody.length() < 120) errMsg += ": " + errBody;
    chatBuf[streamStartLine] = "HERMES: " + errMsg;
    if (streamStartLine + 1 < chatCount) chatBuf[streamStartLine + 1] = "";
    lastApiStatus = httpStatus;
    isBusy = false; streamActive = false; vState = V_IDLE;
    Serial.println("streamFromHermes HTTP error: " + errMsg);
    drawStatusBar(); drawChat(); drawInputBar();
    return;
  }
  
  // Parse headers
  bool chunked = false;
  while (client.available()) {
    drawVoiceAnim();
    String h = client.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    h.toLowerCase();
    if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }
  
  lastApiStatus = 200;
  apiStreaming = true;
  drawStatusBar();
  
  // Stream reader
  String fullReply = "";
  unsigned long lastCharTime = millis();
  int updateCounter = 0;
  int sseParseErrors = 0;
  
  // Refresh display every N chars
  #define STREAM_REFRESH_INTERVAL 2
  
  vState = (voiceMode) ? V_FETCH : V_THINK;
  
  while (client.connected() || client.available()) {
    drawVoiceAnim();
    if (!client.available()) {
      if (millis() - lastCharTime > 60000) {
        Serial.println("streamFromHermes: 60s idle timeout");
        break;
      }
      delay(5);
      continue;
    }
    
    lastCharTime = millis();
    
    if (chunked) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      int chunkSize = strtol(sizeLine.c_str(), NULL, 16);
      if (chunkSize == 0) break;
      
      int bytesRead = 0;
      while (bytesRead < chunkSize && client.available()) {
        String sseLine = client.readStringUntil('\n');
        bytesRead += sseLine.length() + 1;
        sseLine.trim();
        
        if (!sseLine.startsWith("data: ")) continue;
        String data = sseLine.substring(6);
        if (data == "[DONE]") break;
        
        JsonDocument sse;
        DeserializationError err = deserializeJson(sse, data);
        if (err) {
          sseParseErrors++;
          Serial.println("SSE parse error: " + String(err.c_str()) + " on: " + data.substring(0, 40));
          continue;
        }
        
        String delta = sse["choices"][0]["delta"]["content"] | "";
        if (delta.length() == 0) delta = sse["message"]["content"] | "";
        if (delta.length() > 0) {
          fullReply += delta;
          updateCounter++;
          if (updateCounter % STREAM_REFRESH_INTERVAL == 0) {
            streamRefreshDisplay(fullReply);
          }
        }
        if (sse["done"] == true) break;
      }
      if (client.available()) client.read();
      if (client.available()) client.read();
    } else {
      String line = client.readStringUntil('\n');
      line.trim();
      
      if (line.startsWith("data: ")) {
        String data = line.substring(6);
        if (data == "[DONE]") break;
        
        JsonDocument sse;
        DeserializationError err = deserializeJson(sse, data);
        if (!err) {
          String delta = sse["choices"][0]["delta"]["content"] | "";
          if (delta.length() == 0) delta = sse["message"]["content"] | "";
          if (delta.length() > 0) {
            fullReply += delta;
            updateCounter++;
            if (updateCounter % STREAM_REFRESH_INTERVAL == 0) {
              streamRefreshDisplay(fullReply);
            }
          }
        } else {
          sseParseErrors++;
          Serial.println("SSE parse error: " + String(err.c_str()) + " on: " + data.substring(0, 40));
        }
      }
    }
  }
  
  client.stop();
  
  if (sseParseErrors > 0) {
    Serial.println("Total SSE parse errors: " + String(sseParseErrors));
  }
  
  // Final clean render
  if (fullReply.length() == 0) {
    // Got headers/status but no content — report it
    chatBuf[streamStartLine] = "HERMES: [empty response]";
    if (streamStartLine + 1 < chatCount) chatBuf[streamStartLine + 1] = "";
    Serial.println("streamFromHermes: empty reply from server");
  } else {
    streamRenderFinal(fullReply);
    addConversation("assistant", fullReply);
  }
  
  isBusy = false;
  apiStreaming = false;
  streamActive = false;
  vState = V_IDLE;
  scrollToBottom();
  drawStatusBar();
  drawChat();
  drawInputBar();
  
  // Phase 3 Hook: If voice mode is on, TTS the response!
  if (voiceMode && fullReply.length() > 0) {
    streamTextToSpeech(fullReply);
  }
}

// Refresh stream display — rewrap raw text and update buffer
void streamRefreshDisplay(String& rawText) {
  if (!streamActive) return;
  
  // Rewrap the "HERMES: " + rawText into lines starting at streamStartLine
  String displayText = "HERMES: " + rawText;
  int lineIdx = streamStartLine;
  int pos = 0;
  
  while (pos < (int)displayText.length()) {
    String chunk;
    int remaining = displayText.length() - pos;
    
    if (remaining <= CHARS_PER_LINE) {
      chunk = displayText.substring(pos);
      pos = displayText.length();
    } else {
      int end = pos + CHARS_PER_LINE;
      int lastSpace = displayText.lastIndexOf(' ', end);
      if (lastSpace <= pos) {
        chunk = displayText.substring(pos, end);
        pos = end;
      } else {
        chunk = displayText.substring(pos, lastSpace);
        pos = lastSpace + 1;
      }
    }
    
    if (lineIdx < chatCount) {
      chatBuf[lineIdx] = chunk;
    } else {
      addToChat(chunk);
    }
    lineIdx++;
  }
  
  // Auto-scroll if user was at bottom
  scrollToBottom();
  drawChat();
}

// Final render after stream complete — clean up placeholder lines
void streamRenderFinal(String& fullReply) {
  String displayText = "HERMES: " + fullReply;
  int lineIdx = streamStartLine;
  int pos = 0;
  int linesUsed = 0;
  
  while (pos < (int)displayText.length()) {
    String chunk;
    int remaining = displayText.length() - pos;
    
    if (remaining <= CHARS_PER_LINE) {
      chunk = displayText.substring(pos);
      pos = displayText.length();
    } else {
      int end = pos + CHARS_PER_LINE;
      int lastSpace = displayText.lastIndexOf(' ', end);
      if (lastSpace <= pos) {
        chunk = displayText.substring(pos, end);
        pos = end;
      } else {
        chunk = displayText.substring(pos, lastSpace);
        pos = lastSpace + 1;
      }
    }
    
    if (lineIdx < chatCount) {
      chatBuf[lineIdx] = chunk;
    } else {
      addToChat(chunk);
    }
    lineIdx++;
    linesUsed++;
  }
  
  // Clear any leftover placeholder lines from the initial allocation
  // (we allocated 2 extra lines initially)
  // Don't clear — they may have been reused by addToChat shifting.
  // Just ensure we don't have stale "HERMES: " placeholder at the end.
}

// ============================================================
// VOICE (STT / TTS)
// ============================================================

bool isRecording = false;
File audioFile;
const int AUDIO_RATE = 16000;
int audioBytesWritten = 0;

void writeWavHeader(File& file, int dataSize) {
  uint32_t fileSize = dataSize + 36;
  uint32_t sampleRate = AUDIO_RATE;
  uint32_t byteRate = AUDIO_RATE * 2; // 16-bit mono
  
  uint8_t header[44] = {
    'R','I','F','F',
    fileSize & 0xFF, (fileSize >> 8) & 0xFF, (fileSize >> 16) & 0xFF, (fileSize >> 24) & 0xFF,
    'W','A','V','E',
    'f','m','t',' ',
    16, 0, 0, 0,
    1, 0,
    1, 0,
    sampleRate & 0xFF, (sampleRate >> 8) & 0xFF, (sampleRate >> 16) & 0xFF, (sampleRate >> 24) & 0xFF,
    byteRate & 0xFF, (byteRate >> 8) & 0xFF, (byteRate >> 16) & 0xFF, (byteRate >> 24) & 0xFF,
    2, 0,
    16, 0,
    'd','a','t','a',
    dataSize & 0xFF, (dataSize >> 8) & 0xFF, (dataSize >> 16) & 0xFF, (dataSize >> 24) & 0xFF
  };
  file.seek(0);
  file.write(header, 44);
}

void sendAudioToWhisper() {
  // Pre-flight checks — report all failures explicitly
  if (WiFi.status() != WL_CONNECTED) {
    addToChat("!! STT: WiFi not connected");
    drawChat();
    vState = V_IDLE; isBusy = false; drawStatusBar();
    return;
  }
  if (!sdMounted) {
    addToChat("!! STT: No SD card — cannot read audio");
    drawChat();
    vState = V_IDLE; isBusy = false; drawStatusBar();
    return;
  }
  if (!SD.exists("/record.wav")) {
    addToChat("!! STT: record.wav not found on SD");
    drawChat();
    vState = V_IDLE; isBusy = false; drawStatusBar();
    return;
  }
  
  vState = V_UPLOAD;
  isBusy = true;
  drawStatusBar();
  
  if (sttKey.length() == 0) {
    addToChat("!! STT: No sttKey in config.json");
    Serial.println("STT: sttKey is empty — set it in /config.json on SD");
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure(); // Skip cert verification (ESP32 has limited CA store)
  Serial.println("STT: connecting to " + sttHost + ":" + String(sttPort));
  if (!client.connect(sttHost.c_str(), sttPort)) {
    addToChat("!! STT: Cannot reach " + sttHost + ":" + String(sttPort));
    Serial.println("STT: connection refused");
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }

  File file = SD.open("/record.wav", FILE_READ);
  if (!file) {
    addToChat("!! STT: Failed to open record.wav");
    Serial.println("STT: file open failed");
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  uint32_t fileLen = file.size();
  if (fileLen <= 44) {
    addToChat("!! STT: Audio file empty (0 bytes recorded)");
    Serial.println("STT: record.wav has no audio data");
    file.close();
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  Serial.println("STT: audio file " + String(fileLen) + " bytes");
  
  // Deepgram: simple binary POST with audio/wav Content-Type
  String sttPath = "/v1/listen?model=" + sttModel + "&language=en&punctuate=true";
  client.print("POST " + sttPath + " HTTP/1.1\r\n");
  client.print("Host: " + sttHost + "\r\n");
  client.print("Authorization: Token " + sttKey + "\r\n");
  client.print("Content-Type: audio/wav\r\n");
  client.print("Content-Length: " + String(fileLen) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  
  // Stream WAV file from SD
  uint8_t xbuf[512];
  uint32_t bytesSent = 0;
  while (file.available()) {
    drawVoiceAnim();
    int c = file.read(xbuf, 512);
    if (c > 0) { client.write(xbuf, c); bytesSent += c; }
  }
  file.close();
  Serial.println("STT: sent " + String(bytesSent) + " bytes of audio");
  
  // Read HTTP status
  vState = V_THINK;
  int httpStatus = parseHttpStatus(client);
  Serial.println("STT: HTTP status " + String(httpStatus));
  
  // Read remaining headers
  bool dummy;
  skipHttpHeaders(client, dummy);
  
  // Collect body
  String response = "";
  unsigned long t = millis();
  while ((client.available() || client.connected()) && millis() - t < HTTP_TIMEOUT) {
    drawVoiceAnim();
    if (client.available()) {
      response += (char)client.read();
      t = millis();
    }
  }
  client.stop();
  Serial.println("STT response: " + response.substring(0, 120));
  
  if (httpStatus != 200) {
    String errMsg = "STT HTTP " + String(httpStatus);
    if (response.length() > 0 && response.length() < 120) errMsg += ": " + response;
    addToChat("!! " + errMsg);
    Serial.println("STT error: " + errMsg);
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, response);
  if (jsonErr) {
    String errMsg = "STT JSON parse failed: " + String(jsonErr.c_str());
    addToChat("!! " + errMsg);
    Serial.println(errMsg + " | raw: " + response.substring(0, 80));
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  // Deepgram response: {"results":{"channels":[{"alternatives":[{"transcript":"..."}]}]}}
  String text = "";
  if (doc.containsKey("results")) {
    text = doc["results"]["channels"][0]["alternatives"][0]["transcript"] | "";
  } else {
    text = doc["text"] | ""; // Fallback for OpenAI-compatible format
  }
  text.trim();
  
  if (text.length() == 0) {
    addToChat("!! STT: Empty transcript returned");
    Serial.println("STT: empty transcript. Full response: " + response.substring(0, 80));
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  Serial.println("STT transcript: " + text);
  isBusy = false; // streamFromHermes will re-set isBusy itself
  streamFromHermes(text);
}

void streamTextToSpeech(String text) {
  if (WiFi.status() != WL_CONNECTED) {
    addToChat("!! TTS: WiFi not connected");
    drawChat(); drawStatusBar();
    return;
  }
  if (text.length() == 0) return;
  
  vState = V_FETCH;
  isBusy = true;
  drawStatusBar();
  
  if (ttsKey.length() == 0) {
    addToChat("!! TTS: No ttsKey in config.json");
    Serial.println("TTS: ttsKey is empty — set it in /config.json on SD");
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure(); // Skip cert verification
  Serial.println("TTS: connecting to " + ttsHost + ":" + String(ttsPort));
  if (!client.connect(ttsHost.c_str(), ttsPort)) {
    addToChat("!! TTS: Cannot reach " + ttsHost + ":" + String(ttsPort));
    Serial.println("TTS: connection refused");
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  JsonDocument doc;
  doc["text"] = text;
  doc["model_id"] = ttsModel;
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  String ttsPath = "/v1/text-to-speech/" + ttsVoice + "/stream?output_format=pcm_24000";
  client.print("POST " + ttsPath + " HTTP/1.1\r\n");
  client.print("Host: " + ttsHost + "\r\n");
  client.print("xi-api-key: " + ttsKey + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(jsonStr.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(jsonStr);

  // Read and validate HTTP status
  int httpStatus = parseHttpStatus(client);
  Serial.println("TTS: HTTP status " + String(httpStatus));
  if (httpStatus != 200) {
    bool dummy; skipHttpHeaders(client, dummy);
    String errBody = "";
    unsigned long t = millis();
    while ((client.available() || client.connected()) && millis() - t < 5000) {
      if (client.available()) { errBody += (char)client.read(); t = millis(); }
    }
    client.stop();
    String errMsg = "TTS HTTP " + String(httpStatus);
    if (errBody.length() > 0 && errBody.length() < 120) errMsg += ": " + errBody;
    addToChat("!! " + errMsg);
    Serial.println("TTS error: " + errMsg);
    vState = V_IDLE; isBusy = false;
    drawChat(); drawStatusBar();
    return;
  }
  
  // Skip remaining headers (with timeout, matching skipHttpHeaders pattern)
  {
    unsigned long ht = millis();
    while (client.connected() || client.available()) {
      if (!client.available()) {
        if (millis() - ht > HTTP_TIMEOUT) break;
        delay(5);
        continue;
      }
      drawVoiceAnim();
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) break;
      ht = millis();
    }
  }
  
  vState = V_SPEAK;

  // Stream PCM bytes (24kHz 16-bit mono) directly to M5 Speaker
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(180);
  
  uint8_t xbuf[1024];
  uint32_t totalPcm = 0;
  while (client.available() || client.connected()) {
    drawVoiceAnim();
    if (client.available()) {
      int c = client.read(xbuf, 1024);
      if (c > 0) {
        totalPcm += c;
        while (M5Cardputer.Speaker.isPlaying()) {
          drawVoiceAnim();
          delay(5);
        }
        M5Cardputer.Speaker.playRaw((const int16_t*)xbuf, c / 2, 24000, false, 1);
      }
    }
  }
  client.stop();
  Serial.println("TTS: played " + String(totalPcm) + " PCM bytes");
  
  if (totalPcm == 0) {
    addToChat("!! TTS: No audio data received");
    Serial.println("TTS: zero bytes received from server");
    drawChat();
  }
  
  vState = V_IDLE;
  isBusy = false;
  drawStatusBar();
}

// ============================================================
// KEYBOARD
// ============================================================

void handleKeyboard() {
  // M5.update() handles these already. Removing redundant I2C calls speeds up loop execution.
  auto& state = M5Cardputer.Keyboard.keysState();
  const auto& keys = M5Cardputer.Keyboard.keyList();
  
  KeyInfo newActive[MAX_KEYS];
  int newCount = 0;
  unsigned long now = millis();
  
  // Build current key list, preserving press times
  for (int i = 0; i < (int)keys.size() && newCount < MAX_KEYS; i++) {
    newActive[newCount].pos = keys[i];
    newActive[newCount].consumed = false;
    
    int prevIdx = findKeyIndex(keys[i], activeKeys, activeKeyCount);
    if (prevIdx >= 0) {
      newActive[newCount].pressTime = activeKeys[prevIdx].pressTime;
      newActive[newCount].consumed = activeKeys[prevIdx].consumed;
    } else {
      newActive[newCount].pressTime = now;
    }
    newCount++;
  }
  
  // Process key events
  for (int i = 0; i < newCount; i++) {
    Point2D_t p = newActive[i].pos;
    int prevIdx = findKeyIndex(p, activeKeys, activeKeyCount);
    bool isNew = (prevIdx < 0);
    unsigned long holdTime = now - newActive[i].pressTime;
    uint8_t key = M5Cardputer.Keyboard.getKey(p);
    
    // ===== SHORT PRESS (new key-down) =====
    if (isNew && !newActive[i].consumed) {
      // Fn combos — always active even when busy
      if (state.fn) {
        if (key == ';') { scrollUp(3); newActive[i].consumed = true; continue; }
        if (key == '.') { scrollDown(3); newActive[i].consumed = true; continue; }
        if (key == ',') { scrollToBottom(); drawChat(); newActive[i].consumed = true; continue; }
        if (key == KEY_BACKSPACE) { clearChat(); newActive[i].consumed = true; continue; }
        if (key == KEY_ENTER) { clearChat(); addToChat("!! RESET !!"); addToChat("Ready."); redrawAll(); newActive[i].consumed = true; continue; }
        // History navigation: Fn+Up (key '`' on Cardputer) / Fn+Down
        if (key == '`') {  // Fn+` = history up (older)
          if (histCount > 0) {
            if (histIdx == -1) histIdx = histCount - 1;
            else if (histIdx > 0) histIdx--;
            inputBuf = inputHistory[histIdx];
            drawInputBar();
          }
          newActive[i].consumed = true; continue;
        }
        if (key == KEY_TAB) {  // Fn+Tab = history down (newer)
          if (histIdx >= 0) {
            histIdx++;
            if (histIdx >= histCount) { histIdx = -1; inputBuf = ""; }
            else inputBuf = inputHistory[histIdx];
            drawInputBar();
          }
          newActive[i].consumed = true; continue;
        }
        newActive[i].consumed = true;
        continue;
      }
      
      if (isBusy) { newActive[i].consumed = true; continue; }
      
      // Normal keys
      if (key == KEY_BACKSPACE) {
        if (inputBuf.length() > 0) {
          inputBuf.remove(inputBuf.length() - 1);
          drawInputBar();
        }
        histIdx = -1;  // Any edit breaks history navigation
        newActive[i].consumed = true;
      } else if (key == KEY_ENTER) {
        // Portal mode: Enter returns to chat
        if (portalActive) {
          portalActive = false;
          addToChat("SYS: Web Portal — back to chat");
          addToChat("SYS: Portal still accessible at http://" + portalIP);
          redrawAll();
          newActive[i].consumed = true;
          continue;
        }
        if (inputBuf.length() > 0) {
          String toSend = inputBuf;
          toSend.trim();
          pushHistory(toSend);
          inputBuf = "";
          drawInputBar();
          
          if (toSend == "/help") {
            addToChat("--- Commands ---");
            addToChat("/help   Show this list");
            addToChat("/clear  Clear chat");
            addToChat("/voice  Toggle voice mode");
            addToChat("/files  Web portal (SD card)");
            addToChat("/ir CMD Send IR command");
            addToChat("---");
            addToChat("Fn+; Scroll up  Fn+. Scroll down");
            addToChat("Fn+BS Clear chat  Fn+Enter Reset");
            addToChat("Fn+` History up  Fn+Tab History down");
            drawChat();
          } else if (toSend == "/clear") {
            clearChat();
          } else if (toSend == "/voice") {
            voiceMode = !voiceMode;
            addToChat((voiceMode) ? "SYS: Voice Mode ON" : "SYS: Voice Mode OFF");
            drawStatusBar();
            drawChat();
          } else if (toSend == "/files") {
            if (!portalActive) {
              portalIP = WiFi.localIP().toString();
              portalActive = true;
              addToChat("SYS: Web Portal → http://" + portalIP);
              addToChat("SYS: Press ENTER to return to chat");
              // Show folder icon screen
              M5Cardputer.Display.fillScreen(BLACK);
              drawStatusBar();
              drawPortalScreen();
            } else {
              addToChat("SYS: Portal already active at http://" + portalIP);
            }
            drawChat();
          } else if (toSend.startsWith("/ir ")) {
            String irCmd = toSend.substring(4);
            addToChat("SYS: IR TX -> " + irCmd + " (Pin 44)");
            drawChat();
          } else {
            streamFromHermes(toSend);
          }
        }
        newActive[i].consumed = true;
      } else if (key == KEY_TAB) {
        inputBuf += ' ';
        drawInputBar();
        newActive[i].consumed = true;
      } else if (key >= 32 && key < 127) {
        histIdx = -1;  // Any normal char breaks history navigation
        inputBuf += (char)key;
        drawInputBar();
        newActive[i].consumed = true;
      }
    }
    
    // ===== LONG PRESS (held > threshold, not yet consumed) =====
    if (holdTime > LONG_PRESS_MS && !newActive[i].consumed && !isBusy) {
      if (key == KEY_BACKSPACE && !state.fn) {
        // Long backspace = clear entire input
        inputBuf = "";
        drawInputBar();
        newActive[i].consumed = true;
      } else if (key >= 32 && key < 127 && !state.fn) {
        // Long character = key repeat
        inputBuf += (char)key;
        drawInputBar();
        newActive[i].pressTime = now - (LONG_PRESS_MS - 100);  // 100ms repeat rate
      }
    }
  }
  
  // Shift keys: current -> prev, new -> current
  prevKeyCount = activeKeyCount;
  for (int i = 0; i < activeKeyCount; i++) prevKeys[i] = activeKeys[i];
  
  activeKeyCount = newCount;
  for (int i = 0; i < newCount; i++) activeKeys[i] = newActive[i];
}

// ============================================================
// BOOT ANIMATION
// ============================================================

void bootAnimation() {
  M5Cardputer.Display.fillScreen(BLACK);

  // Caduceus center — staff is vertical, wings spread horizontally
  int cx = 120;   // horizontal center of 240px display
  int topY = 25;  // top of staff
  int botY = 100; // bottom of staff

  // Phase 1: Staff draws downward (~400ms)
  // Single vertical line — the asclepian rod
  for (int y = topY; y <= botY; y++) {
    M5Cardputer.Display.drawPixel(cx, y, 0x07FF);  // Cyan
    delay(5);
  }

  // Phase 2: Two serpents wind upward (~600ms)
  // Sinusoidal S-curves wrapping 2.5 times around the staff
  int prevX1 = cx, prevY1 = botY;
  int prevX2 = cx, prevY2 = botY;
  for (int i = 0; i <= 60; i++) {
    float t = (float)i / 60.0f;
    int y = botY - (int)(t * (botY - topY));
    // 2.5 full windings = 5π radians
    float wave1 = sin(t * 5.0f * PI) * 12.0f;
    float wave2 = sin(t * 5.0f * PI + PI) * 12.0f;  // 180° out of phase

    int x1 = cx + (int)wave1;
    int x2 = cx + (int)wave2;

    if (i > 0) {
      M5Cardputer.Display.drawLine(prevX1, prevY1, x1, y, 0x07FF);
      M5Cardputer.Display.drawLine(prevX2, prevY2, x2, y, 0x041F);  // Deep blue
    }
    prevX1 = x1; prevY1 = y;
    prevX2 = x2; prevY2 = y;
    delay(10);
  }

  // Phase 3: Wings fan outward from staff top (~300ms)
  // Three line segments per side, spreading upward like feathers
  int wy = topY + 12;
  for (int i = 0; i < 3; i++) {
    int spread = (i + 1) * 10;
    int dy = i * 4;
    // Right wing
    M5Cardputer.Display.drawLine(cx + 2, wy, cx + spread + 8, wy - 8 - dy, 0x07FF);
    // Left wing
    M5Cardputer.Display.drawLine(cx - 2, wy, cx - spread - 8, wy - 8 - dy, 0x07FF);
    delay(100);
  }

  delay(200);

  // Phase 4: "HERMES" typewriter effect
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(0x07FF);  // Cyan
  M5Cardputer.Display.setCursor(72, 30);

  String title = "HERMES";
  for (int i = 0; i < (int)title.length(); i++) {
    M5Cardputer.Display.print(title[i]);
    delay(60);
  }

  // Phase 5: Subtitle with pulse effect
  M5Cardputer.Display.setTextSize(1);
  for (int pass = 0; pass < 3; pass++) {
    M5Cardputer.Display.setTextColor(pass % 2 == 0 ? 0x8410 : 0x4208);
    M5Cardputer.Display.setCursor(58, 58);
    M5Cardputer.Display.print("Cardputer Chat v0.2.3");
    delay(150);
  }
  M5Cardputer.Display.setTextColor(0x8410);
  M5Cardputer.Display.setCursor(58, 58);
  M5Cardputer.Display.print("Cardputer Chat v0.2.3");

  // Phase 6: Progress bar
  M5Cardputer.Display.drawRect(30, 80, 180, 8, 0x4208);
  for (int x = 0; x < 178; x += 3) {
    M5Cardputer.Display.fillRect(31, 81, x, 6, 0x07FF);
    delay(12);
  }

  // Phase 7: Status text
  M5Cardputer.Display.setTextColor(0x07E0);  // Green
  M5Cardputer.Display.setCursor(70, 100);
  M5Cardputer.Display.print("Booting...");

  delay(300);
  M5Cardputer.Display.fillScreen(BLACK);
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  
  // Phase 4: Audio Layer Init
  // Speaker and Mic multiplex hardware — cannot both be active.
  // Default to Speaker; Mic is lazy-init'd when voice recording starts.
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(128);
  
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5.Power.begin();
  
  // Boot animation
  bootAnimation();
  // IMU only on Cardputer-Adv — base model has no IMU, begin() would crash
  if (M5.Imu.getType() != m5::imu_none) {
    M5.Imu.begin();
    imuAvailable = true;
  }
  
  // G0 button via direct GPIO (avoids M5Unified update() conflicts)
  pinMode(0, INPUT_PULLUP);
  
  Serial.begin(115200);
  Serial.setTimeout(10); // Prevent default 1s timeout blocking issue

  // Phase 1: SD Configuration (lazy-mount flag)
  SPI.begin(40, 39, 14, 12);
  sdMounted = SD.begin(12, SPI, 25000000);
  if (sdMounted) {
    if (SD.exists("/config.json")) {
      File file = SD.open("/config.json");
      JsonDocument doc;
      if (!deserializeJson(doc, file)) {
        if (doc.containsKey("ssid")) ssid = doc["ssid"].as<String>();
        if (doc.containsKey("password")) password = doc["password"].as<String>();
        if (doc.containsKey("apiKey")) apiKey = doc["apiKey"].as<String>();
        if (doc.containsKey("host")) host = doc["host"].as<String>();
        if (doc.containsKey("port")) port = doc["port"].as<int>();
        // Cloud STT
        if (doc.containsKey("sttHost")) sttHost = doc["sttHost"].as<String>();
        if (doc.containsKey("sttPort")) sttPort = doc["sttPort"].as<int>();
        if (doc.containsKey("sttKey")) sttKey = doc["sttKey"].as<String>();
        if (doc.containsKey("sttModel")) sttModel = doc["sttModel"].as<String>();
        // Cloud TTS
        if (doc.containsKey("ttsHost")) ttsHost = doc["ttsHost"].as<String>();
        if (doc.containsKey("ttsPort")) ttsPort = doc["ttsPort"].as<int>();
        if (doc.containsKey("ttsKey")) ttsKey = doc["ttsKey"].as<String>();
        if (doc.containsKey("ttsModel")) ttsModel = doc["ttsModel"].as<String>();
        if (doc.containsKey("ttsVoice")) ttsVoice = doc["ttsVoice"].as<String>();
        Serial.println("CONFIG: loaded from SD card");
        Serial.println("  hermes=" + host + ":" + String(port));
        Serial.println("  stt=" + sttHost + ":" + String(sttPort) + " model=" + sttModel);
        Serial.println("  tts=" + ttsHost + ":" + String(ttsPort) + " model=" + ttsModel + " voice=" + ttsVoice);
      }
      file.close();
    }
  }
  
  WiFi.begin(ssid.c_str(), password.c_str());
  
  // Draw initial empty screen with status bar while connecting
  redrawAll();
  
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 30) {
    delay(500);
    t++;
    // Let status bar reflect weak/connecting icons during delay
    drawStatusBar();
  }
  
  // Final draw once either connected or timed out
  redrawAll();
  
  // ==================== OTA INIT ====================
  if (WiFi.status() == WL_CONNECTED) {
    // 1) ArduinoOTA — shows as network port for arduino-cli upload
    ArduinoOTA.setHostname("cardputer");
    ArduinoOTA.onStart([]() {
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "spiffs";
      addToChat("OTA: Updating " + type + "...");
      drawChat();
    });
    ArduinoOTA.onEnd([]() {
      addToChat("OTA: Done! Rebooting...");
      drawChat();
      delay(500);
      ESP.restart();
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      static int lastPct = -1;
      int pct = (progress / (total / 100));
      if (pct != lastPct && pct % 25 == 0) {
        lastPct = pct;
        Serial.println("OTA progress: " + String(pct) + "%");
      }
    });
    ArduinoOTA.onError([](ota_error_t error) {
      String msg = "OTA Error: ";
      if      (error == OTA_AUTH_ERROR)    msg += "Auth failed";
      else if (error == OTA_BEGIN_ERROR)   msg += "Begin failed";
      else if (error == OTA_CONNECT_ERROR) msg += "Connect failed";
      else if (error == OTA_RECEIVE_ERROR) msg += "Receive failed";
      else if (error == OTA_END_ERROR)     msg += "End failed";
      addToChat("!! " + msg);
      drawChat();
      Serial.println(msg);
    });
    ArduinoOTA.begin();
    Serial.println("ArduinoOTA ready on: cardputer.local");
    
    // 2) HTTP OTA — browser-based backup at /update
    otaServer.on("/update", HTTP_POST, []() {
      otaServer.sendHeader("Connection", "close");
      if (Update.hasError()) {
        otaServer.send(500, "text/plain", "Update FAILED: " + String(Update.errorString()));
        addToChat("!! HTTP OTA failed");
        drawChat();
      } else {
        otaServer.send(200, "text/plain", "Update OK — rebooting...");
        addToChat("HTTP OTA: Done! Rebooting...");
        drawChat();
        delay(500);
        ESP.restart();
      }
    }, []() {
      HTTPUpload& upload = otaServer.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.println("HTTP OTA: Receiving " + upload.filename);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.println("HTTP OTA: " + String(upload.totalSize) + " bytes written");
        } else {
          Update.printError(Serial);
        }
      }
    });
    
    // 3) Web Portal — file manager at /
    otaServer.on("/", HTTP_GET, handlePortalRoot);
    otaServer.on("/api/files", HTTP_GET, handleApiFiles);
    otaServer.on("/api/read", HTTP_GET, handleApiRead);
    otaServer.on("/api/write", HTTP_POST, handleApiWrite);
    otaServer.on("/api/delete", HTTP_POST, handleApiDelete);
    
    otaServer.begin();
    Serial.println("HTTP OTA ready at: http://cardputer.local/update");
    Serial.println("Web Portal ready at: http://cardputer.local/");
  }
}

void loop() {
  // OTA + Web Portal handlers
  ArduinoOTA.handle();
  otaServer.handleClient();  // Always on for HTTP OTA /update endpoint
  
  // Call components separately to avoid M5Cardputer.update()'s ~2s overhead
  // (it processes IMU, power, speaker, mic, touch every cycle)
  M5.update();                                    // Display, buttons, power
  M5Cardputer.Keyboard.updateKeyList();           // GPIO matrix scan
  M5Cardputer.Keyboard.updateKeysState();         // Process into keysState
  handleKeyboard();

  // Phase 2: G0 Button via direct GPIO 0 (bypasses M5Unified update conflicts)
  static bool g0Last = true;  // pulled high = not pressed
  static unsigned long g0Debounce = 0;
  bool g0Now = digitalRead(0);
  
  // Periodic debug heartbeat: log G0 state + voice state every 2s
  static unsigned long lastG0Debug = 0;
  if (millis() - lastG0Debug > 2000) {
    lastG0Debug = millis();
    if (voiceMode) {
      Serial.printf("[DBG] G0pin=%d voiceMode=%d isRec=%d isBusy=%d vState=%d sdOK=%d heap=%u bytes=%d\n",
        g0Now, voiceMode, isRecording, isBusy, (int)vState, sdMounted, ESP.getFreeHeap(), audioBytesWritten);
    }
  }
  
  if (!g0Now && g0Last && (millis() - g0Debounce > 300) && voiceMode) {
    // Falling edge detected (button pressed), debounced
    g0Debounce = millis();
    Serial.println("G0: >>> PRESS DETECTED <<< isRecording=" + String(isRecording) + " isBusy=" + String(isBusy));
    
    if (isRecording) {
      // Stop recording and send
      Serial.println("G0: Stopping recording, bytes=" + String(audioBytesWritten));
      isRecording = false;
      writeWavHeader(audioFile, audioBytesWritten);
      audioFile.close();
      Serial.println("G0: WAV closed, switching to speaker");
      M5Cardputer.Mic.end();
      M5Cardputer.Speaker.begin();
      addToChat("SYS: Stopped. Sending " + String(audioBytesWritten) + "B");
      drawChat();
      sendAudioToWhisper();
    } else if (!isBusy) {
      if (!sdMounted) {
        Serial.println("G0: SD not mounted!");
        addToChat("!! SD Card Required for Mic buffer");
        drawChat();
      } else {
        Serial.println("G0: Starting recording...");
        M5Cardputer.Speaker.end();
        M5Cardputer.Mic.begin();
        delay(80); // Let mic hardware initialize
        Serial.println("G0: Mic initialized, opening WAV file");
        audioFile = SD.open("/record.wav", FILE_WRITE);
        if (audioFile) {
          for (int i=0; i<44; i++) audioFile.write(0);
          audioBytesWritten = 0;
          isRecording = true;
          vState = V_LISTEN;
          drawStatusBar();
          addToChat("SYS: Recording... press G0 to stop");
          drawChat();
          Serial.println("G0: Recording started OK");
        } else {
          Serial.println("G0: !!! SD file open FAILED");
          addToChat("!! SD Card Write Error");
          drawChat();
          M5Cardputer.Mic.end();
          M5Cardputer.Speaker.begin();
        }
      }
    } else {
      Serial.println("G0: Ignored — isBusy=true");
    }
  }
  g0Last = g0Now;
  
  if (isRecording) {
    int16_t micBuf[256];
    if (M5Cardputer.Mic.record(micBuf, 256, AUDIO_RATE)) {
      audioFile.write((uint8_t*)micBuf, 256 * 2);
      audioBytesWritten += 512;
    }
    drawVoiceAnim(); // Tick animation during recording
  }

  // IMU Tilt to Scroll — dead-zone ±0.35g, hysteresis: only fires once tilt exceeds threshold
  // Only available on Cardputer-Adv (base model has no IMU)
  static unsigned long lastScroll = 0;
  static bool imuScrolling = false;
  if (imuAvailable && millis() - lastScroll > 180 && !isBusy) {
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    const float TILT_ON  = 0.55f;  // Must reach this to activate
    const float TILT_OFF = 0.30f;  // Must fall below this to deactivate
    if (!imuScrolling) {
      if      (ay < -TILT_ON) { scrollUp(1);   imuScrolling = true; lastScroll = millis(); }
      else if (ay >  TILT_ON) { scrollDown(1); imuScrolling = true; lastScroll = millis(); }
    } else {
      // Hysteresis: keep scrolling while held, stop when returned to neutral
      if (ay < -TILT_ON)        { scrollUp(1);   lastScroll = millis(); }
      else if (ay >  TILT_ON)   { scrollDown(1); lastScroll = millis(); }
      else if (fabs(ay) < TILT_OFF) imuScrolling = false;
    }
  }
  
  // Auto-reconnect WiFi
  if (WiFi.status() != WL_CONNECTED && !isBusy) {
    static unsigned long lastRC = 0;
    if (millis() - lastRC > 10000) {
      lastRC = millis();
      WiFi.reconnect();
    }
  }
  
  // Refresh status bar every 2s (unless blocked by animations)
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 2000 && vState == V_IDLE) {
    lastStatus = millis();
    drawStatusBar();
  }
  
  drawVoiceAnim(); // Make sure animations always tick during idle too

  // Serial debug
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "status") {
      Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "OK" : "FAIL"));
      Serial.println("RSSI: " + String(WiFi.RSSI()));
      Serial.println("Chat: " + String(chatCount) + " lines, scroll=" + String(scrollPos));
      Serial.println("Conv: " + String(convCount) + " turns");
      Serial.println("Heap: " + String(ESP.getFreeHeap()));
      Serial.println("Stream: " + String(streamActive ? "active" : "idle"));
    } else if (cmd == "clear") {
      clearChat();
    } else if (cmd == "test") {
      streamFromHermes("Hello from Cardputer");
    }
  }
}

// Draw portal active screen — folder icon + IP + instructions
void drawPortalScreen() {
  int cx = SCREEN_W / 2;
  int cy = (SCREEN_H - STATUS_H) / 2 + STATUS_H;
  
  // Folder icon (pixel art, cyan)
  uint16_t col = 0x07FF;  // Cyan
  int fx = cx - 24;
  int fy = cy - 28;
  
  // Folder tab
  M5Cardputer.Display.fillRect(fx, fy, 18, 6, col);
  // Folder body
  M5Cardputer.Display.fillRect(fx, fy + 6, 48, 30, col);
  // Folder inner (dark)
  M5Cardputer.Display.fillRect(fx + 2, fy + 8, 44, 26, 0x0820);
  
  // WiFi waves (right side of folder)
  int wx = fx + 54;
  int wy = fy + 10;
  M5Cardputer.Display.drawArc(wx, wy, 6, 6, 225, 315, 0x07FF);
  M5Cardputer.Display.drawArc(wx, wy, 12, 12, 225, 315, 0x07FF);
  M5Cardputer.Display.drawArc(wx, wy, 18, 18, 225, 315, 0x07FF);
  // Dot
  M5Cardputer.Display.fillCircle(wx, wy + 22, 3, 0x07FF);
  
  // IP address
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(0x07FF);
  String url = "http://" + portalIP + "/";
  int tw = url.length() * 6;
  M5Cardputer.Display.setCursor(cx - tw / 2, cy + 16);
  M5Cardputer.Display.print(url);
  
  // Instructions
  M5Cardputer.Display.setTextColor(0x4208);
  String hint = "[ENTER] Stop Portal";
  tw = hint.length() * 6;
  M5Cardputer.Display.setCursor(cx - tw / 2, cy + 30);
  M5Cardputer.Display.print(hint);
}

// ============================================================
// WEB PORTAL — File Manager (replaces on-device browser/editor)
// ============================================================

// Embedded HTML for the file manager portal
const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hermes SD Card</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:14px/1.4 monospace;background:#0a1628;color:#b0e0e6;display:flex;height:100vh}
#sidebar{width:260px;background:#0d1f3c;border-right:1px solid #1a3a5c;display:flex;flex-direction:column;flex-shrink:0}
#sidebar h2{padding:12px 16px;color:#00e5ff;font-size:15px;border-bottom:1px solid #1a3a5c}
#filelist{flex:1;overflow-y:auto;padding:4px 0}
.file{padding:5px 16px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:13px}
.file:hover{background:#1a3a5c}
.file.active{background:#0e3a5e;color:#00e5ff}
.file.dir{color:#4dd0e1}
.actions{padding:8px;border-top:1px solid #1a3a5c}
.actions button{background:#0e4a6e;color:#b0e0e6;border:1px solid #1a5a7e;padding:5px 10px;cursor:pointer;font:12px monospace;margin:2px;border-radius:3px}
.actions button:hover{background:#1a6a8e}
#editor{flex:1;display:flex;flex-direction:column}
#toolbar{padding:8px 12px;background:#0d1f3c;border-bottom:1px solid #1a3a5c;display:flex;align-items:center;gap:8px}
#toolbar .path{color:#4dd0e1;font-size:13px;flex:1}
#toolbar button{background:#0e4a6e;color:#b0e0e6;border:1px solid #1a5a7e;padding:5px 12px;cursor:pointer;font:12px monospace;border-radius:3px}
#toolbar button:hover{background:#1a6a8e}
#toolbar button.save{background:#0e5a3e;border-color:#1a7a5e}
#toolbar button.save:hover{background:#1a7a5e}
#toolbar button.del{background:#5a0e0e;border-color:#7a1a1a}
#toolbar button.del:hover{background:#7a1a1a}
#content{flex:1;display:flex}
#linenos{width:40px;background:#0a1628;color:#3a5a7c;text-align:right;padding:8px 6px;font-size:13px;line-height:1.5;overflow:hidden;user-select:none}
textarea{flex:1;background:#0a1628;color:#b0e0e6;border:none;padding:8px;font:13px/1.5 monospace;resize:none;outline:none;tab-size:2}
#status{padding:6px 12px;background:#0d1f3c;border-top:1px solid #1a3a5c;font-size:12px;color:#3a7a5c}
#newdlg{display:none;position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);background:#0d1f3c;border:1px solid #1a5a7e;padding:20px;border-radius:6px;z-index:10}
#newdlg input{background:#0a1628;color:#b0e0e6;border:1px solid #1a3a5c;padding:6px;font:13px monospace;width:250px;outline:none}
#newdlg button{margin-top:8px;background:#0e4a6e;color:#b0e0e6;border:1px solid #1a5a7e;padding:5px 12px;cursor:pointer;font:12px monospace;border-radius:3px}
#overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.5);z-index:9}
</style>
</head>
<body>
<div id="sidebar">
  <h2>SD Card</h2>
  <div id="filelist"></div>
  <div class="actions">
    <button onclick="showNew()">+ New File</button>
    <button onclick="loadFiles()">Refresh</button>
  </div>
</div>
<div id="editor">
  <div id="toolbar">
    <span class="path" id="filepath">Select a file</span>
    <button class="save" onclick="saveFile()">Save</button>
    <button class="del" onclick="deleteFile()">Delete</button>
  </div>
  <div id="content">
    <div id="linenos"></div>
    <textarea id="ta" onscroll="syncLines()" oninput="updateLines()" placeholder="Select a file from the list..."></textarea>
  </div>
  <div id="status">Ready</div>
</div>
<div id="overlay" onclick="hideNew()"></div>
<div id="newdlg">
  <div style="color:#00e5ff;margin-bottom:8px">New filename:</div>
  <input id="newname" placeholder="config.json" onkeydown="if(event.key==='Enter')createFile()">
  <br><button onclick="createFile()">Create</button>
</div>
<script>
let currentPath='';
const ta=document.getElementById('ta');
const fl=document.getElementById('filelist');
const fp=document.getElementById('filepath');
const st=document.getElementById('status');
const ln=document.getElementById('linenos');

function status(m,c){st.textContent=m;st.style.color=c||'#3a7a5c'}

async function loadFiles(){
  try{
    const r=await fetch('/api/files');
    const d=await r.json();
    fl.innerHTML='';
    d.forEach(f=>{
      const el=document.createElement('div');
      el.className='file'+(f.dir?' dir':'');
      el.textContent=(f.dir?'\u{1F4C1} ':'\u{1F4C4} ')+f.name;
      if(!f.dir) el.onclick=()=>loadFile(f.path);
      fl.appendChild(el);
    });
    status('Files loaded');
  }catch(e){status('Error: '+e.message,'#e53935')}
}

async function loadFile(path){
  try{
    const r=await fetch('/api/read?path='+encodeURIComponent(path));
    const d=await r.json();
    if(d.error){status(d.error,'#e53935');return}
    currentPath=path;
    fp.textContent=path;
    ta.value=d.content;
    updateLines();
    status('Loaded: '+path);
  }catch(e){status('Error: '+e.message,'#e53935')}
}

async function saveFile(){
  if(!currentPath){status('No file selected','#e53935');return}
  try{
    const r=await fetch('/api/write',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({path:currentPath,content:ta.value})
    });
    const d=await r.json();
    status(d.ok?'Saved: '+currentPath:(d.error||'Save failed'),d.ok?'#3a7a5c':'#e53935');
  }catch(e){status('Error: '+e.message,'#e53935')}
}

async function deleteFile(){
  if(!currentPath){status('No file selected','#e53935');return}
  if(!confirm('Delete '+currentPath+'?'))return;
  try{
    const r=await fetch('/api/delete',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({path:currentPath})
    });
    const d=await r.json();
    if(d.ok){
      status('Deleted: '+currentPath);
      currentPath='';fp.textContent='Select a file';ta.value='';updateLines();
      loadFiles();
    }else{status(d.error||'Delete failed','#e53935')}
  }catch(e){status('Error: '+e.message,'#e53935')}
}

function showNew(){document.getElementById('overlay').style.display='block';document.getElementById('newdlg').style.display='block';document.getElementById('newname').focus()}
function hideNew(){document.getElementById('overlay').style.display='none';document.getElementById('newdlg').style.display='none'}

async function createFile(){
  const n=document.getElementById('newname').value.trim();
  if(!n)return;
  hideNew();
  try{
    const r=await fetch('/api/write',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({path:'/'+n,content:''})
    });
    const d=await r.json();
    if(d.ok){loadFiles();loadFile('/'+n)}
    else{status(d.error||'Create failed','#e53935')}
  }catch(e){status('Error: '+e.message,'#e53935')}
}

function updateLines(){
  const lines=ta.value.split('\n');
  ln.innerHTML=lines.map((_,i)=>'<div>'+(i+1)+'</div>').join('');
}

function syncLines(){
  ln.scrollTop=ta.scrollTop;
}

ta.addEventListener('keydown',function(e){
  if(e.key==='Tab'){
    e.preventDefault();
    const s=this.selectionStart,en=this.selectionEnd;
    this.value=this.value.substring(0,s)+'  '+this.value.substring(en);
    this.selectionStart=this.selectionEnd=s+2;
    updateLines();
  }
  if((e.ctrlKey||e.metaKey)&&e.key==='s'){
    e.preventDefault();saveFile();
  }
});

loadFiles();
</script>
</body>
</html>
)rawliteral";

// Serve the portal page
void handlePortalRoot() {
  if (!portalActive) { otaServer.send(403, "text/plain", "Portal not active. Type /files on device."); return; }
  otaServer.send_P(200, "text/html", PORTAL_HTML);
}

// List files on SD card as JSON
void handleApiFiles() {
  if (!portalActive) { otaServer.send(403, "application/json", "{\"error\":\"Portal not active\"}"); return; }
  if (!sdMounted) {
    otaServer.send(500, "application/json", "{\"error\":\"SD not mounted\"}");
    return;
  }
  
  String path = otaServer.hasArg("path") ? otaServer.arg("path") : "/";
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    otaServer.send(400, "application/json", "{\"error\":\"Not a directory\"}");
    return;
  }
  
  String json = "[";
  bool first = true;
  File entry = dir.openNextFile();
  while (entry) {
    if (!first) json += ",";
    first = false;
    String name = String(entry.name());
    if (name.startsWith("/")) name = name.substring(1);
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    json += "{\"name\":\"" + name + "\",\"dir\":" + (entry.isDirectory() ? "true" : "false");
    String entryPath = String(entry.name());
    if (!entryPath.startsWith("/")) entryPath = "/" + entryPath;
    json += ",\"path\":\"" + entryPath + "\"}";
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  json += "]";
  
  otaServer.send(200, "application/json", json);
}

// Read a file from SD card
void handleApiRead() {
  if (!portalActive) { otaServer.send(403, "application/json", "{\"error\":\"Portal not active\"}"); return; }
  if (!sdMounted) {
    otaServer.send(500, "application/json", "{\"error\":\"SD not mounted\"}");
    return;
  }
  if (!otaServer.hasArg("path")) {
    otaServer.send(400, "application/json", "{\"error\":\"Missing path\"}");
    return;
  }
  
  String path = otaServer.arg("path");
  if (!path.startsWith("/")) path = "/" + path;
  File file = SD.open(path);
  if (!file) {
    otaServer.send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
  }
  
  String content = "";
  while (file.available()) {
    content += (char)file.read();
  }
  file.close();
  
  // Escape JSON special chars
  String escaped = "";
  for (unsigned int i = 0; i < content.length(); i++) {
    char c = content[i];
    if (c == '"') escaped += "\\\"";
    else if (c == '\\') escaped += "\\\\";
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else if (c == '\t') escaped += "\\t";
    else escaped += c;
  }
  
  otaServer.send(200, "application/json", "{\"content\":\"" + escaped + "\"}");
}

// Write a file to SD card
void handleApiWrite() {
  if (!portalActive) { otaServer.send(403, "application/json", "{\"error\":\"Portal not active\"}"); return; }
  if (!sdMounted) {
    otaServer.send(500, "application/json", "{\"error\":\"SD not mounted\"}");
    return;
  }
  
  String body = otaServer.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    otaServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  String path = doc["path"].as<String>();
  if (!path.startsWith("/")) path = "/" + path;
  String content = doc["content"].as<String>();
  
  if (path.length() == 0) {
    otaServer.send(400, "application/json", "{\"error\":\"Missing path\"}");
    return;
  }
  
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    otaServer.send(500, "application/json", "{\"error\":\"Cannot write\"}");
    return;
  }
  
  file.print(content);
  file.close();
  
  otaServer.send(200, "application/json", "{\"ok\":true}");
  addToChat("SYS: Web saved " + path);
  
  // Auto-reload config.json
  if (path.endsWith("config.json")) {
    File cfg = SD.open(path);
    if (cfg) {
      JsonDocument cfgDoc;
      if (!deserializeJson(cfgDoc, cfg)) {
        if (cfgDoc.containsKey("ssid")) ssid = cfgDoc["ssid"].as<String>();
        if (cfgDoc.containsKey("password")) password = cfgDoc["password"].as<String>();
        if (cfgDoc.containsKey("apiKey")) apiKey = cfgDoc["apiKey"].as<String>();
        if (cfgDoc.containsKey("host")) host = cfgDoc["host"].as<String>();
        if (cfgDoc.containsKey("port")) port = cfgDoc["port"].as<int>();
        if (cfgDoc.containsKey("sttKey")) sttKey = cfgDoc["sttKey"].as<String>();
        if (cfgDoc.containsKey("ttsKey")) ttsKey = cfgDoc["ttsKey"].as<String>();
        addToChat("SYS: Config reloaded");
      }
      cfg.close();
    }
  }
  drawChat();
}

// Delete a file from SD card
void handleApiDelete() {
  if (!portalActive) { otaServer.send(403, "application/json", "{\"error\":\"Portal not active\"}"); return; }
  if (!sdMounted) {
    otaServer.send(500, "application/json", "{\"error\":\"SD not mounted\"}");
    return;
  }
  
  String body = otaServer.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    otaServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  String path = doc["path"].as<String>();
  if (path.length() == 0) {
    otaServer.send(400, "application/json", "{\"error\":\"Missing path\"}");
    return;
  }
  
  if (!SD.exists(path)) {
    otaServer.send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
  }
  
  SD.remove(path);
  otaServer.send(200, "application/json", "{\"ok\":true}");
  addToChat("SYS: Web deleted " + path);
  drawChat();
}

// Map unshifted key to shifted equivalent// Map unshifted key to shifted equivalent when Aa (shift) is active.
// The Cardputer keyboard returns base chars from getKey(); we apply
// the shift layer manually to get symbols like *, @, #, etc.
uint8_t getShiftedChar(uint8_t key) {
  switch (key) {
    case '`': return '~';
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    // Uppercase letters
    default:
      if (key >= 'a' && key <= 'z') return key - 32;
      return key;  // No shift mapping — return as-is
  }
}
