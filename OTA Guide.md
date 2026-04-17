# OTA Guide

Over-the-air firmware updates via WiFi. No USB cable required after initial flash.

---

## How It Works

The firmware includes two OTA methods:

1. **HTTP OTA (primary)** — Web server on port 80 with `/update` endpoint. Accepts firmware via HTTP POST.
2. **ArduinoOTA (secondary)** — UDP protocol for Arduino IDE network port. May be blocked by firewalls.

Both run in the background every loop cycle. Updates take ~8 seconds for a 1.3MB binary.

---

## Workflow

### 1. Compile
```bash
cd firmware/cardputer_chat_wifi
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=min_spiffs --output-dir ./build .
```

**Must use `PartitionScheme=min_spiffs`.** Default scheme is too small (1.2MB) for the OTA-enabled firmware (1.35MB).

### 2. Find Cardputer IP
```bash
# ARP table — M5Stack MAC prefix is b0:81:84
arp -a | grep "b0:81:84"
```

### 3. Upload
```bash
curl -F "file=@build/cardputer_chat_wifi.ino.bin" http://<CARDPUTER_IP>/update
```

Expected response: `Update OK — rebooting...`

### 4. Verify

Device reboots automatically. Wait ~10 seconds, confirm it's back:
```bash
curl -s -m 3 -o /dev/null -w "%{http_code}" http://<CARDPUTER_IP>/
# Returns 404 (server is running, no GET handler registered)
```

---

## First-Time Flash (USB)

OTA only works after OTA-enabled firmware is installed. First flash requires USB:

```bash
# 1. Put Cardputer in bootloader mode:
#    Hold BOOT button → press RESET → release BOOT

# 2. Verify serial port:
ls /dev/cu.usbmodem*

# 3. Flash:
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 460800 \
  write_flash -z 0x0 build/cardputer_chat_wifi.ino.merged.bin
```

---

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| `Connection refused` on /update | Not running OTA firmware | Flash via USB first |
| Upload starts but no reboot | Update failed silently | Check serial output |
| `No response from device` (ArduinoOTA) | Firewall blocking callback | Use HTTP OTA instead |
| Device stuck after OTA | Bad firmware | Hold BOOT+RESET, flash via USB |
| IP changes between reboots | DHCP lease renewal | Set static IP in router |

---

## Recovery

If a bad OTA update bricks the device:

1. Hold **BOOT** + press **RESET** → enters serial bootloader
2. Serial port appears at `/dev/cu.usbmodem101`
3. Flash known-good firmware via esptool

The ESP32-S3 bootloader is in ROM — it can never be bricked by firmware updates.

---

## Partition Scheme: min_spiffs

| Partition | Offset | Size |
|-----------|--------|------|
| NVS | 0x9000 | 20KB |
| OTA Data | 0xe000 | 8KB |
| App 0 | 0x10000 | 1920KB |
| App 1 | 0x1F0000 | 1920KB |
| SPIFFS | 0x3D0000 | 192KB |

Standard scheme gives 1.2MB/app (too small). min_spiffs gives 1.9MB at cost of 190KB SPIFFS instead of 1.5MB.
