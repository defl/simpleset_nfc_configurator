"""
Extract NFC RF passwords from a Signify NfcCommandsHandler.dll binary.

This utility locates the password table embedded in the original DLL and
extracts the RF passwords needed for communicating with Signify LED drivers.

You must supply your own copy of NfcCommandsHandler.dll from a legitimate
Signify MultiOne installation.

Usage:
    python extract_passwords.py <path_to_NfcCommandsHandler.dll>
    python extract_passwords.py              (auto-detect in current dir)
"""

import sys
import os
import json


def find_hex_string_clusters(data, min_strings=3, window=128):
    """Find clusters of ASCII hex strings in a binary, without knowing any password.

    Scans for regions containing multiple 8-char or 16-char ASCII hex strings
    separated by null bytes — the structural signature of a password table.

    Returns list of (offset, [strings]) tuples for each cluster found.
    """
    # Find all 8-char and 16-char ASCII hex strings in the binary
    hex_chars = set(b'0123456789ABCDEFabcdef')
    candidates = []
    i = 0
    while i < len(data) - 7:
        # Check if we're at a null->hex boundary or start of file
        if i == 0 or data[i - 1] == 0:
            # Try to read an ASCII hex string
            end = i
            while end < len(data) and data[end] in hex_chars:
                end += 1
            length = end - i
            if length in (8, 16) and (end >= len(data) or data[end] == 0):
                s = data[i:end].decode('ascii')
                candidates.append((i, s))
                i = end
                continue
        i += 1

    if not candidates:
        return []

    # Cluster nearby candidates (within `window` bytes of each other)
    clusters = []
    current_cluster = [candidates[0]]
    for j in range(1, len(candidates)):
        offset, s = candidates[j]
        prev_offset = current_cluster[-1][0]
        if offset - prev_offset <= window:
            current_cluster.append(candidates[j])
        else:
            if len(current_cluster) >= min_strings:
                clusters.append((current_cluster[0][0], current_cluster))
            current_cluster = [candidates[j]]
    if len(current_cluster) >= min_strings:
        clusters.append((current_cluster[0][0], current_cluster))

    return clusters


def extract_rf_passwords(hex_strings):
    """Extract RF passwords (4-byte / 8 hex char) from the found strings."""
    rf_passwords = []

    for s in hex_strings:
        # Skip obvious placeholders
        if all(c == '0' for c in s):
            continue
        if s == "12345678":
            continue

        if len(s) == 8:
            rf_passwords.append(s.upper())

    # Deduplicate while preserving order
    seen = set()
    rf_unique = []
    for p in rf_passwords:
        if p not in seen:
            seen.add(p)
            rf_unique.append(p)

    return rf_unique


def extract_passwords(dll_path):
    """Extract all passwords from the DLL.

    Returns dict with 'rf_passwords' and 'i2c_passwords' lists, or None.
    """
    with open(dll_path, "rb") as f:
        data = f.read()

    clusters = find_hex_string_clusters(data)
    if not clusters:
        return None

    # Use the largest cluster (most likely the password table)
    best_cluster = max(clusters, key=lambda c: len(c[1]))
    table_offset = best_cluster[0]
    all_strings = [s for _, s in best_cluster[1]]

    rf = extract_rf_passwords(all_strings)

    if not rf:
        return None

    return {
        "dll_path": dll_path,
        "dll_size": len(data),
        "table_offset": table_offset,
        "rf_passwords": rf,
    }


def find_dll():
    """Try to auto-detect NfcCommandsHandler.dll in common locations."""
    candidates = [
        "NfcCommandsHandler.dll",
        "NfcCommandsHandler.dll.original",
        os.path.join("..", "NfcCommandsHandler.dll"),
    ]

    # Check common MultiOne install paths
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    for pf in [program_files, program_files_x86]:
        candidates.append(os.path.join(pf, "Signify", "MultiOne", "NfcCommandsHandler.dll"))
        candidates.append(os.path.join(pf, "MultiOne", "NfcCommandsHandler.dll"))

    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return

    dll_path = sys.argv[1] if len(sys.argv) > 1 else find_dll()
    if not dll_path or not os.path.exists(dll_path):
        print("Error: NfcCommandsHandler.dll not found.")
        print("Usage: python extract_passwords.py <path_to_dll>")
        print()
        print("Provide the ORIGINAL DLL from your Signify MultiOne installation.")
        print("If you've already installed the shim, look for NfcCommandsHandler.dll.original")
        sys.exit(1)

    print(f"Extracting passwords from: {dll_path}")
    result = extract_passwords(dll_path)

    if result is None:
        print("Error: Password table not found in this DLL.")
        print("This may not be a Signify NfcCommandsHandler.dll, or it may use")
        print("a different password storage format.")
        sys.exit(1)

    print(f"Table found at offset 0x{result['table_offset']:08X}\n")

    print("RF Passwords (4-byte, for ISO 15693 PresentPassword):")
    for i, pwd in enumerate(result["rf_passwords"], 1):
        print(f"  {i}. {pwd}")

    # Output as JSON for programmatic use
    json_path = "passwords.json"
    with open(json_path, "w") as f:
        json.dump({
            "rf_passwords": result["rf_passwords"],
        }, f, indent=2)
    print(f"\nPasswords saved to: {json_path}")


if __name__ == "__main__":
    main()
