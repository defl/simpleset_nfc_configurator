# SimpleSet CLI Tool

Standalone command-line tool to read and write SimpleSet LED driver
configuration via ESP32+PN5180 — no MultiOne software needed.

## Prerequisites

1. **Hardware**: ESP32+PN5180 flashed with the bridge firmware (see `../firmware/`)
2. **Python 3.7+**
3. **pyserial**:
   ```
   pip install pyserial
   ```

## Password Setup (one-time)

The RF password must be extracted from your own Signify MultiOne installation.

If you have MultiOne installed:
```bash
python extract_passwords.py "C:\Program Files (x86)\Signify\MultiOne\NfcCommandsHandler.dll"
```

This creates `passwords.json` in the current directory. The CLI will
automatically find and use it.

Alternatively, pass the password directly:
```bash
python simpleset_cli.py --password AABBCCDD info
```

Or set an environment variable:
```bash
set SIGNIFY_RF_PASSWORD=AABBCCDD
```

## Usage

```bash
# Show device information (12NC, version, firmware, product name)
python simpleset_cli.py info

# Read current AOC (Adjustable Output Current) setting
python simpleset_cli.py getcurrent

# Set AOC to 800 mA
python simpleset_cli.py setcurrent 800

# Dump all 64 NFC memory blocks
python simpleset_cli.py dump
```

### Options

```
--port PORT       Serial port (default: auto-detect)
--password HEX    RF password as 8 hex characters
--no-verify       Skip read-back verification after setcurrent
```

### Write Verification

By default, `setcurrent` reads back both block 1 and the AOC mailbox after
writing to confirm the values were stored correctly. If verification fails, the
tool exits with an error code. Use `--no-verify` to skip this check.

### Batch Programming

The CLI is designed for fast batch programming. Place a driver on the reader,
run `setcurrent`, swap the next driver, run it again. No GUI, no waiting.
You can program dozens of drivers in minutes:

```bash
# Program a stack of drivers to 800 mA — just swap and repeat
python simpleset_cli.py setcurrent 800
# (swap driver)
python simpleset_cli.py setcurrent 800
# (swap driver)
python simpleset_cli.py setcurrent 800
```

## Example Output

```
============================================================
SimpleSet NFC CLI Tool
============================================================
  Port: COM4
  Password loaded from: passwords.json
  Tag : E0 02 38 00 C2 C9 1D E8
  Auth: OK

--- Device Information ---
  Selected          : XI150C200V050BPF1
  Version           : 1.0
  12NC              : 929002739413
  Device type       : LED
  Firmware revision : 1
  Device identifier : E0023800C2C91DE8

--- AOC Current ---
  Output current    : 800 mA
  AOC mode          : enabled
```

## Password Priority

The CLI searches for the RF password in this order:

1. `--password` command-line argument
2. `SIGNIFY_RF_PASSWORD` environment variable
3. `passwords.json` in the current directory or script directory
4. Auto-extract from `NfcCommandsHandler.dll` if found nearby

## Files

| File | Description |
|------|-------------|
| `simpleset_cli.py` | Main CLI tool |
| `extract_passwords.py` | Password extraction utility |
| `passwords.json` | Extracted passwords (generated, gitignored) |
