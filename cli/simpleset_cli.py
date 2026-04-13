"""
SimpleSet NFC CLI Tool

Standalone tool to read/write SimpleSet LED driver configuration
via ESP32+PN5180 over USB serial.

Usage:
    python simpleset_cli.py info               Show device information
    python simpleset_cli.py getcurrent         Read current AOC setting
    python simpleset_cli.py setcurrent <mA>    Set AOC output current
    python simpleset_cli.py dump               Dump all readable blocks

Prerequisites:
    1. ESP32+PN5180 flashed with the bridge firmware and connected via USB
    2. RF password extracted (run extract_passwords.py first)
    3. pyserial installed: pip install pyserial

License: MIT
"""

import sys
import os
import time
import json
import serial as pyserial

BAUD = 115200
TIMEOUT = 2.0

# Password is loaded at runtime — never hardcoded.
# Use extract_passwords.py to extract from your own NfcCommandsHandler.dll.
PWD = None  # set by load_password()

# Block numbers for dirty flags
DIRTY_BLOCK = 33
MO_DIRTY_BLOCK = 63

# JSON file names to search for
PASSWORD_JSON_NAMES = [
    "passwords.json",
    "NfcCommandsHandler.dll_passwords.json",
    "NfcCommandsHandler_passwords.json",
]


def load_password(pwd_arg=None):
    """Load RF password from command-line arg, env var, or JSON file.

    Priority:
      1. --password CLI argument (hex string, e.g. "AABBCCDD")
      2. SIGNIFY_RF_PASSWORD environment variable
      3. passwords.json in current dir or script dir
      4. Auto-extract from NfcCommandsHandler.dll if found
    """
    global PWD

    # 1. CLI argument
    if pwd_arg:
        PWD = bytes.fromhex(pwd_arg)
        return

    # 2. Environment variable
    env_pwd = os.environ.get("SIGNIFY_RF_PASSWORD")
    if env_pwd:
        PWD = bytes.fromhex(env_pwd)
        return

    # 3. JSON file
    search_dirs = [
        os.getcwd(),
        os.path.dirname(os.path.abspath(__file__)),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
    ]
    for d in search_dirs:
        for name in PASSWORD_JSON_NAMES:
            json_path = os.path.join(d, name)
            if os.path.exists(json_path):
                with open(json_path) as f:
                    data = json.load(f)
                rf_list = data.get("rf_passwords", [])
                if rf_list:
                    PWD = bytes.fromhex(rf_list[0])
                    print(f"  Password loaded from: {json_path}")
                    return

    # 4. Try auto-extracting from DLL
    dll_candidates = []
    for d in search_dirs:
        dll_candidates.append(os.path.join(d, "NfcCommandsHandler.dll"))
        dll_candidates.append(os.path.join(d, "NfcCommandsHandler.dll.original"))
    for dll_path in dll_candidates:
        if os.path.exists(dll_path):
            try:
                from extract_passwords import extract_passwords
                result = extract_passwords(dll_path)
                if result and result["rf_passwords"]:
                    PWD = bytes.fromhex(result["rf_passwords"][0])
                    print(f"  Password extracted from: {dll_path}")
                    return
            except Exception:
                continue

    print("  ERROR: No RF password available.")
    print("  Run extract_passwords.py on your NfcCommandsHandler.dll first,")
    print("  or pass --password <hex> on the command line.")
    sys.exit(1)


class NfcDriver:
    """Low-level ESP32+PN5180 serial driver with correct password slot management.

    Key discovery: On the ST M24LR tag used in SimpleSet drivers, password slots
    have mutual exclusion — presenting a password on one slot REVOKES access
    granted by another slot. Therefore:
      - Slot 1 is presented at connect time (unlocks read+write for all blocks)
      - Slot 2 is presented on-demand only for writes to blocks 32-63,
        then slot 1 is immediately re-presented to restore read access
    """

    def __init__(self, port):
        self.ser = None
        self.uid = None
        self.port = port

    def connect(self):
        self.ser = pyserial.Serial(self.port, BAUD, timeout=TIMEOUT)
        time.sleep(1.5)
        while self.ser.in_waiting:
            self.ser.readline()
        self.ser.reset_input_buffer()

        # Ping
        assert self._cmd(b'P') == "PONG", "ESP32 not responding"

        # Inventory
        r = self._cmd(b'I')
        if not r.startswith("UID:"):
            raise RuntimeError(f"No tag found: {r}")
        uid_hex = r[4:]
        self.uid = [int(uid_hex[i:i+2], 16) for i in range(0, len(uid_hex), 2)]

        # Present password slot 1 ONLY (unlocks read+write for all blocks)
        self._present_password(1)
        return self.uid

    def disconnect(self):
        if self.ser:
            self.ser.close()
            self.ser = None

    def _cmd(self, data):
        self.ser.reset_input_buffer()
        self.ser.write(data)
        return self.ser.readline().decode('ascii', errors='replace').strip()

    def _present_password(self, slot):
        """Present RF password on the given slot via raw ISO 15693 command."""
        frame = bytearray()
        frame.append(0x22)   # flags
        frame.append(0xB3)   # PresentPassword (ST custom)
        frame.append(0x02)   # ST manufacturer code
        frame.extend(self.uid)
        frame.append(slot & 0x03)
        frame.extend(PWD)
        msg = bytes([ord('C'), len(frame)]) + bytes(frame)
        r = self._cmd(msg)
        if not r.startswith("RAWOK:"):
            raise RuntimeError(f"Password slot {slot} failed: {r}")

    def read_block(self, block_num):
        r = self._cmd(b'R' + bytes([block_num & 0xFF]))
        if r.startswith("OK:"):
            h = r[3:]
            return [int(h[i:i+2], 16) for i in range(0, len(h), 2)]
        raise RuntimeError(f"Read block {block_num} failed: {r}")

    def write_block(self, block_num, data):
        """Write 4 bytes. For sector 1 (blocks 32-63), handles slot 2 automatically."""
        need_slot2 = 32 <= block_num <= 63
        if need_slot2:
            self._present_password(2)

        r = self._cmd(b'W' + bytes([block_num & 0xFF]) + bytes(data))

        if need_slot2:
            self._present_password(1)  # restore read access

        if r != "OK":
            raise RuntimeError(f"Write block {block_num} failed: {r}")

    def read_blocks(self, start, count):
        """Read multiple blocks, return as flat bytearray."""
        buf = bytearray()
        for i in range(count):
            buf.extend(self.read_block(start + i))
        return buf


def hex_dump(data):
    return " ".join(f"{b:02X}" for b in data)


def ascii_str(data):
    return "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in data)


# ═══════════════════════════════════════════════════════════════
# Device Info
# ═══════════════════════════════════════════════════════════════

def read_device_info(nfc):
    """Read device info that matches what MultiOne shows at startup."""
    info = {}

    # UID
    info["device_identifier"] = "".join(f"{b:02X}" for b in nfc.uid)

    # Block 1: current value + mode
    blk1 = nfc.read_block(1)
    info["current_mA"] = (blk1[2] << 8) | blk1[3]
    info["current_mode"] = blk1[1]

    # Block 3: version + device type
    blk3 = nfc.read_block(3)
    info["version"] = f"{blk3[0]}.{blk3[1]}"
    device_types = {0: "Unknown", 4: "LED", 8: "LED Module"}
    info["device_type"] = device_types.get(blk3[3], f"Type_{blk3[3]}")

    # Block 4: firmware revision
    blk4 = nfc.read_block(4)
    info["firmware_revision"] = blk4[3]

    # Blocks 5-6: 12NC (12-digit number code, BCD encoded)
    blk5 = nfc.read_block(5)
    blk6 = nfc.read_block(6)
    nc_bytes = blk5 + blk6[:2]
    info["12nc"] = "".join(f"{b:02X}" for b in nc_bytes)

    # Blocks 6-10: Product name (ASCII)
    name_bytes = blk6[2:4]
    for blk in range(7, 11):
        name_bytes += nfc.read_block(blk)
    name = ""
    for b in name_bytes:
        if b == 0:
            break
        if 0x20 <= b < 0x7F:
            name += chr(b)
    info["product_name"] = name

    return info


def print_device_info(info):
    print(f"  Selected          : {info['product_name']}")
    print(f"  Version           : {info['version']}")
    print(f"  12NC              : {info['12nc']}")
    print(f"  Device type       : {info['device_type']}")
    print(f"  Firmware revision : {info['firmware_revision']}")
    print(f"  Device identifier : {info['device_identifier']}")


# ═══════════════════════════════════════════════════════════════
# AOC Current Read/Write
# ═══════════════════════════════════════════════════════════════

def read_aoc_current(nfc):
    """Read Adjustable Output Current from block 1 and config mailbox (block 48)."""
    blk1 = nfc.read_block(1)
    actual_mA = (blk1[2] << 8) | blk1[3]
    mode = blk1[1]

    blk48 = nfc.read_block(48)
    mb_len = blk48[0]

    return {
        "actual_mA": actual_mA,
        "mode": mode,
        "mode_str": "enabled" if mode == 2 else ("disabled" if mode == 0 else f"0x{mode:02X}"),
        "mb_configured": mb_len > 0,
        "blk1_raw": blk1,
        "blk48_raw": blk48,
    }


def write_aoc_current(nfc, target_mA, verify=True):
    """Write a new AOC current value.

    Writes to:
    1. Block 1 (direct current register)
    2. AOC mailbox at blocks 48-49 (configuration for the driver MCU)
    3. Dirty flags (signal MCU to pick up changes)

    If verify=True (default), reads back and confirms the write succeeded.
    """
    if target_mA < 0 or target_mA > 4000:
        raise ValueError(f"Current must be 0-4000 mA, got {target_mA}")

    print(f"\n  Setting AOC current to {target_mA} mA...")

    blk1 = nfc.read_block(1)
    old_mA = (blk1[2] << 8) | blk1[3]
    print(f"  Current value: {old_mA} mA")

    # Write block 1 (current register in sector 0)
    new_blk1 = list(blk1)
    new_blk1[1] = 0x02 if target_mA > 0 else 0x00
    new_blk1[2] = (target_mA >> 8) & 0xFF
    new_blk1[3] = target_mA & 0xFF
    print(f"  Writing block  1: {hex_dump(new_blk1)}")
    nfc.write_block(1, new_blk1)

    # Write AOC mailbox at blocks 48-49 (sector 1 config)
    blk48 = nfc.read_block(48)
    mb_ver = blk48[3] if blk48[3] != 0 else 0x1A

    raw = bytearray(8)
    raw[0] = 4              # length = 4 data bytes
    raw[2] = 0x00           # lock byte
    raw[3] = mb_ver         # MB version
    raw[4] = 0x00           # PWM control
    raw[5] = 0x02 if target_mA > 0 else 0x00
    raw[6] = (target_mA >> 8) & 0xFF
    raw[7] = target_mA & 0xFF

    # XOR checksum
    cksum = 0
    for i, b in enumerate(raw):
        if i != 1:
            cksum ^= b
    raw[1] = cksum & 0xFF

    print(f"  Writing block 48: {hex_dump(raw[0:4])}")
    nfc.write_block(48, list(raw[0:4]))
    print(f"  Writing block 49: {hex_dump(raw[4:8])}")
    nfc.write_block(49, list(raw[4:8]))

    # Update dirty flags
    try:
        blk33 = nfc.read_block(DIRTY_BLOCK)
        dirty = list(blk33)
        dirty[1] = dirty[1] | 0x01
        dirty[2] = dirty[2] | 0x80
        print(f"  Writing block 33: {hex_dump(dirty)} (dirty flags)")
        nfc.write_block(DIRTY_BLOCK, dirty)
    except Exception as e:
        print(f"  Warning: dirty flags update failed: {e}")

    try:
        blk63 = nfc.read_block(MO_DIRTY_BLOCK)
        mo_dirty = list(blk63)
        if mo_dirty == [0xFF, 0xFF, 0xFF, 0xFF]:
            mo_dirty = [0xFF, 0xFF, 0xFF, 0xB7]
            print(f"  Writing block 63: {hex_dump(mo_dirty)} (MO dirty)")
            nfc.write_block(MO_DIRTY_BLOCK, mo_dirty)
    except Exception as e:
        print(f"  Warning: MO dirty update failed: {e}")

    if verify:
        # Read back and confirm the write
        readback = nfc.read_block(1)
        readback_mA = (readback[2] << 8) | readback[3]
        print(f"\n  Verify block 1: {hex_dump(readback)} = {readback_mA} mA")

        # Also verify the AOC mailbox
        rb48 = nfc.read_block(48)
        rb49 = nfc.read_block(49)
        mb_mA = (rb49[2] << 8) | rb49[3]
        print(f"  Verify block 48: {hex_dump(rb48)}")
        print(f"  Verify block 49: {hex_dump(rb49)} = {mb_mA} mA")

        if readback_mA == target_mA and mb_mA == target_mA:
            print(f"\n  VERIFIED: AOC current set to {target_mA} mA")
        elif readback_mA == target_mA:
            print(f"\n  PARTIAL: Block 1 OK ({readback_mA} mA), mailbox mismatch ({mb_mA} mA)")
        else:
            print(f"\n  FAILED: Expected {target_mA} mA, read back {readback_mA} mA")
            sys.exit(1)

        return readback_mA
    else:
        print(f"\n  Written (verification skipped)")
        return target_mA


# ═══════════════════════════════════════════════════════════════
# Dump
# ═══════════════════════════════════════════════════════════════

def dump_all(nfc):
    """Dump all 64 readable blocks."""
    print("\n  Block | Hex Data    | ASCII | Notes")
    print("  ------+-------------+-------+------")
    for blk in range(64):
        try:
            data = nfc.read_block(blk)
            h = hex_dump(data)
            a = ascii_str(data)
            note = ""
            if blk == 0: note = "Block 0 (UID/system)"
            elif blk == 1: note = f"Current: {(data[2]<<8)|data[3]} mA"
            elif blk == 3: note = f"Version {data[0]}.{data[1]}, Type={data[3]}"
            elif blk == 4: note = f"FW rev {data[3]}"
            elif blk == 5: note = "12NC (bytes 1-4)"
            elif blk == 6: note = "12NC (bytes 5-6) + product name start"
            elif 7 <= blk <= 10: note = f"Product name: \"{a}\""
            elif blk == 32: note = "Sector 1 header"
            elif blk == 33: note = "Dirty flags"
            elif blk == 48: note = "AOC mailbox header"
            elif blk == 63: note = "MultiOne dirty flags"
            print(f"  {blk:5d} | {h} | {a} | {note}")
        except RuntimeError:
            print(f"  {blk:5d} | -- error -- |       |")


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

def auto_detect_port():
    """Try to auto-detect the ESP32 serial port."""
    import serial.tools.list_ports
    for port in serial.tools.list_ports.comports():
        desc = (port.description or "").lower()
        if any(kw in desc for kw in ["cp210", "ch340", "ftdi", "usb serial", "silicon labs"]):
            return port.device
    # Fallback: try common ports
    for p in ["COM4", "COM3", "COM5", "COM6"]:
        try:
            s = pyserial.Serial(p, BAUD, timeout=0.5)
            s.close()
            return p
        except Exception:
            continue
    return "COM4"


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="SimpleSet NFC CLI Tool",
        epilog="Run extract_passwords.py first to extract the RF password from your MultiOne installation."
    )
    parser.add_argument("command", choices=["info", "getcurrent", "setcurrent", "dump"],
                        help="Command to execute")
    parser.add_argument("value", nargs="?", type=int, help="Value for setcurrent (mA)")
    parser.add_argument("--password", "-p", help="RF password as hex (8 chars)")
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument("--no-verify", action="store_true",
                        help="Skip read-back verification after setcurrent")
    args = parser.parse_args()

    port = args.port or auto_detect_port()
    nfc = NfcDriver(port=port)

    try:
        print("=" * 60)
        print("SimpleSet NFC CLI Tool")
        print("=" * 60)
        print(f"  Port: {port}")

        load_password(args.password)
        uid = nfc.connect()
        print(f"  Tag : {' '.join(f'{b:02X}' for b in uid)}")
        print(f"  Auth: OK")

        if args.command == "info":
            print("\n--- Device Information ---")
            info = read_device_info(nfc)
            print_device_info(info)
            print(f"\n--- AOC Current ---")
            aoc = read_aoc_current(nfc)
            print(f"  Output current    : {aoc['actual_mA']} mA")
            print(f"  AOC mode          : {aoc['mode_str']}")

        elif args.command == "getcurrent":
            aoc = read_aoc_current(nfc)
            print(f"\n  AOC Output Current: {aoc['actual_mA']} mA")
            print(f"  Mode             : {aoc['mode_str']}")

        elif args.command == "setcurrent":
            if args.value is None:
                print("Usage: simpleset_cli.py setcurrent <mA>")
                return
            write_aoc_current(nfc, args.value, verify=not args.no_verify)

        elif args.command == "dump":
            dump_all(nfc)

    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
    finally:
        nfc.disconnect()
        print("\n" + "=" * 60)


if __name__ == "__main__":
    main()
