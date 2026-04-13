# ESP32 + PN5180 NFC Bridge Firmware

Serial-to-NFC bridge firmware for ESP32 + PN5180. Exposes ISO 15693 NFC
operations over USB serial so that host tools can communicate with NFC tags.

## Hardware

- ESP32-WROOM-32 dev board (any variant with VSPI pins exposed)
- PN5180 NFC module

### Wiring

```
ESP32          PN5180
─────          ──────
GPIO 23 ────── MOSI
GPIO 19 ────── MISO
GPIO 18 ────── SCK
GPIO  5 ────── NSS (CS)
GPIO 16 ────── BUSY
GPIO 17 ────── RST
VIN (5V) ───── 5V        << NOT 3.3V!
GND    ─────── GND
```

> **IMPORTANT: Use VIN (5V), not 3.3V for power!** The PN5180 module has its own
> onboard 3.3V regulator and expects 5V input. Powering from the ESP32's 3.3V
> pin will cause the module to appear to work (SPI responds) but
> NFC transactions will silently fail. The VIN pin provides 5V from USB.

## Flashing

### Option 1: Pre-built Binary (easiest)

Download `firmware.bin` from the `bin/` folder and flash with esptool:

```bash
pip install esptool
esptool.py --chip esp32 --port COM4 --baud 460800 write_flash 0x10000 bin/firmware.bin
```

Replace `COM4` with your ESP32's serial port.

### Option 2: Build from Source (PlatformIO)

```bash
pip install platformio
cd firmware
pio run                    # build
pio run --target upload    # build and flash
pio device monitor         # open serial monitor
```

### Option 3: Build from Source (Arduino IDE)

1. Install the ESP32 board package in Arduino IDE
2. Install the [PN5180 library](https://github.com/ATrappmann/PN5180-Library)
3. Open `src/main.cpp` (rename to `.ino` if needed)
4. Select board "ESP32 Dev Module", set baud to 115200
5. Upload

## Verification

After flashing, open a serial monitor at 115200 baud. You should see:

```
READY:PN5180 v4.1 fw=3.5
```

(Version numbers may vary.)

Place an NFC tag near the PN5180 antenna and the host tools will be able to
detect and communicate with it.

## Serial Protocol

115200 baud, 8N1. All responses are newline-terminated ASCII.

| Command | Params | Response | Description |
|---------|--------|----------|-------------|
| `P` | — | `PONG` | Ping/health check |
| `I` | — | `UID:<hex>` or `NOTAG:<err>` | Inventory (detect tag) |
| `R` | block(1) | `OK:<hex>` or `ERR:...` | Read single block |
| `W` | block(1) + data(4) | `OK` or `ERR:...` | Write single block |
| `D` | start(1) + count(1) | `BLK:nn:<hex>`... `END` | Dump block range |
| `S` | — | `INFO:tag=yes/no,...` | Status |
| `X` | slot(1) + pwd(4) | `PWDOK` or `PWDFAIL:<err>` | Present password |
| `G` | — | `SYSINFO:blocks=N,...` | Get system info |
| `B` | start(1) + count(1) | `BLKSEC:...` | Block security status |
| `F` | — | `FIELDRESET` | Reset RF field |
| `C` | len(1) + raw(N) | `RAWOK:len:<hex>` | Raw ISO 15693 passthrough |

Numeric parameters are single raw bytes. Hex responses use uppercase ASCII.
