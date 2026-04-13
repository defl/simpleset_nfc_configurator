/*
 * NfcCommandsHandler.dll bridge — redirects NFC operations to ESP32+PN5180 via serial
 *
 * Drop-in replacement for the original FEIG-based NfcCommandsHandler.dll.
 * All exported functions match the original signatures and calling convention (__cdecl).
 *
 * Configuration:
 *   Set environment variable MULTIONE_BRIDGE_PORT to the COM port (e.g., "COM3").
 *   Or create multione_bridge.ini next to the DLL with contents: port=COM3
 *   If neither is set, the bridge auto-scans COM3..COM20 looking for the ESP32.
 *
 * Logging:
 *   All calls are logged to multione_bridge.log in the DLL's directory.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- NfcResult enum (must match .NET NfcCommunication.NfcInterop.NfcResult) ---------- */
#define NFCRESULT_OK                  0
#define NFCRESULT_COMMUNICATION_ERROR 1
#define NFCRESULT_PROTOCOL_ERROR      2
#define NFCRESULT_SECURITY_ERROR      3
#define NFCRESULT_DATA_ERROR          4
#define NFCRESULT_CONNECTION_ERROR    5
#define NFCRESULT_FIRMWARE_MISMATCH   6
#define NFCRESULT_UNKNOWN             7

/* ---------- build-time config ---------- */
#define LOG_FILE_NAME       "multione_bridge.log"
#define INI_FILE_NAME       "multione_bridge.ini"
#define SERIAL_BAUD         115200
#define SERIAL_TIMEOUT_MS   2000
#define REPLY_BUF_SIZE      512
#define MAX_UID_HEX         20   /* 8 bytes = 16 hex chars + margin */
#define MAX_BLOCK_HEX       10   /* 4 bytes = 8 hex chars + margin */
#define SCAN_PORT_MIN       3
#define SCAN_PORT_MAX       20

/* ---------- globals ---------- */
static HANDLE   g_serial   = INVALID_HANDLE_VALUE;
static FILE    *g_log      = NULL;
static char     g_port[32] = {0};
static int      g_odmId    = 0;
static char     g_uid[MAX_UID_HEX] = {0};
static int      g_connected = 0;

/* RF password (4 bytes) — loaded at runtime from INI/JSON/env, never hardcoded */
static unsigned char g_rf_password[4] = {0};
static int           g_rf_password_loaded = 0;

/* block security cache: last query result (up to 256 bytes) */
static unsigned char g_blksec[256];
static int           g_blksec_start = 0;
static int           g_blksec_count = 0;

/* selected reader info (from SetSelectedReader) */
static char g_reader_uid[128]  = {0};
static char g_reader_name[128] = {0};

/* static strings returned by pointer */
static const char g_dll_version[]   = "ShimDLL 1.0 (ESP32+PN5180)";
static const char g_fw_revision[]   = "PN5180-ESP32";
static const char g_hw_name[]       = "ESP32+PN5180 NFC Bridge";
static const char g_hw_version[]    = "1.0";

/* reader list returned by GetSupportedReaders — tab-separated UID\tName pairs, double-null terminated */
static char g_reader_list[512] = {0};
static int  g_reader_list_len  = 0;

/* ---------- forward declarations ---------- */
static void   log_open(void);
static void   log_msg(const char *fmt, ...);
static int    serial_open(const char *port);
static void   serial_close(void);
static int    serial_write_bytes(const unsigned char *data, int len);
static int    serial_read_line(char *buf, int bufsize, int timeout_ms);
static int    serial_probe(const char *port);
static int    hex_to_bytes(const char *hex, unsigned char *out, int maxout);
static void   bytes_to_hex(const unsigned char *data, int len, char *out);
static void   resolve_port(void);
static char  *get_dll_dir(void);

/* ================================================================
 *  Logging
 * ================================================================ */
static char g_dll_dir[MAX_PATH] = {0};

static char *get_dll_dir(void) {
    if (g_dll_dir[0] == '\0') {
        /* fallback: current directory */
        GetCurrentDirectoryA(MAX_PATH, g_dll_dir);
    }
    return g_dll_dir;
}

static void log_open(void) {
    if (g_log) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", get_dll_dir(), LOG_FILE_NAME);
    g_log = fopen(path, "a");
    if (g_log) {
        fprintf(g_log, "\n===== NFC Bridge DLL loaded =====\n");
        fflush(g_log);
    }
}

static void log_msg(const char *fmt, ...) {
    if (!g_log) log_open();
    if (!g_log) return;

    /* timestamp */
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);

    fprintf(g_log, "\n");
    fflush(g_log);
}

/* ================================================================
 *  Serial helpers
 * ================================================================ */
static int serial_open(const char *port) {
    char full[64];
    snprintf(full, sizeof(full), "\\\\.\\%s", port);

    g_serial = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (g_serial == INVALID_HANDLE_VALUE) {
        log_msg("serial_open: CreateFile(%s) failed err=%lu", port, GetLastError());
        return 0;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(g_serial, &dcb);
    dcb.BaudRate = SERIAL_BAUD;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;  /* needed for some ESP32 dev boards */
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(g_serial, &dcb);

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout         = 50;
    timeouts.ReadTotalTimeoutConstant    = SERIAL_TIMEOUT_MS;
    timeouts.ReadTotalTimeoutMultiplier  = 10;
    timeouts.WriteTotalTimeoutConstant   = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(g_serial, &timeouts);

    /* purge any stale data */
    PurgeComm(g_serial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    log_msg("serial_open: opened %s OK", port);
    strncpy(g_port, port, sizeof(g_port) - 1);
    return 1;
}

static void serial_close(void) {
    if (g_serial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_serial);
        g_serial = INVALID_HANDLE_VALUE;
        log_msg("serial_close: closed %s", g_port);
    }
}

static int serial_write_bytes(const unsigned char *data, int len) {
    if (g_serial == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(g_serial, data, (DWORD)len, &written, NULL);
    if (!ok || (int)written != len) {
        log_msg("serial_write: failed (wrote %d/%d, err=%lu)", (int)written, len, GetLastError());
        return 0;
    }
    return 1;
}

/* Read a line (terminated by \n) from serial. Returns number of chars read (excl. terminator), or -1 on timeout. */
static int serial_read_line(char *buf, int bufsize, int timeout_ms) {
    if (g_serial == INVALID_HANDLE_VALUE) return -1;

    DWORD start = GetTickCount();
    int pos = 0;

    while (pos < bufsize - 1) {
        DWORD elapsed = GetTickCount() - start;
        if ((int)elapsed > timeout_ms) {
            log_msg("serial_read_line: timeout after %d ms, got %d bytes: [%.*s]", timeout_ms, pos, pos, buf);
            buf[pos] = '\0';
            return (pos > 0) ? pos : -1;
        }

        /* Update timeout for remaining time */
        COMMTIMEOUTS ct = {0};
        int remaining = timeout_ms - (int)elapsed;
        if (remaining < 50) remaining = 50;
        ct.ReadIntervalTimeout        = 50;
        ct.ReadTotalTimeoutConstant   = (DWORD)remaining;
        ct.ReadTotalTimeoutMultiplier = 0;
        SetCommTimeouts(g_serial, &ct);

        unsigned char ch;
        DWORD nread = 0;
        BOOL ok = ReadFile(g_serial, &ch, 1, &nread, NULL);
        if (!ok) {
            log_msg("serial_read_line: ReadFile error=%lu", GetLastError());
            buf[pos] = '\0';
            return -1;
        }
        if (nread == 0) continue; /* timeout on this byte, loop checks overall timeout */

        if (ch == '\n') {
            buf[pos] = '\0';
            /* strip trailing \r */
            if (pos > 0 && buf[pos - 1] == '\r') {
                buf[--pos] = '\0';
            }
            return pos;
        }
        if (ch != '\r') { /* skip lone \r */
            buf[pos++] = (char)ch;
        }
    }

    buf[pos] = '\0';
    return pos;
}

/* Probe a COM port: open, send 'P', expect 'PONG'. Returns 1 if this is our ESP32. */
static int serial_probe(const char *port) {
    if (!serial_open(port)) return 0;

    /* Small delay for ESP32 to become ready after DTR toggle */
    Sleep(100);
    PurgeComm(g_serial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    unsigned char cmd = 'P';
    if (!serial_write_bytes(&cmd, 1)) {
        serial_close();
        return 0;
    }

    char reply[64];
    int n = serial_read_line(reply, sizeof(reply), 1000);
    if (n > 0 && strncmp(reply, "PONG", 4) == 0) {
        log_msg("serial_probe: %s responded with PONG — match!", port);
        return 1;
    }

    /* Some boards emit "READY:..." on reset from DTR, try reading more lines */
    for (int attempt = 0; attempt < 3; attempt++) {
        n = serial_read_line(reply, sizeof(reply), 500);
        if (n > 0) {
            log_msg("serial_probe: %s extra line: %s", port, reply);
            if (strncmp(reply, "PONG", 4) == 0) return 1;
        }
        /* Re-send ping */
        serial_write_bytes(&cmd, 1);
    }

    serial_close();
    return 0;
}

/* ================================================================
 *  Hex helpers
 * ================================================================ */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, unsigned char *out, int maxout) {
    int i = 0;
    while (*hex && i < maxout) {
        /* Skip spaces, dashes, colons */
        while (*hex == ' ' || *hex == '-' || *hex == ':') hex++;
        if (!*hex || !*(hex + 1)) break;
        int hi = hex_nibble(*hex);
        int lo = hex_nibble(*(hex + 1));
        if (hi < 0 || lo < 0) break;
        out[i++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return i;
}

static void bytes_to_hex(const unsigned char *data, int len, char *out) {
    for (int i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02X", data[i]);
    }
    out[len * 2] = '\0';
}

/* ================================================================
 *  Port resolution: env var > ini file > auto-scan
 * ================================================================ */
static void resolve_port(void) {
    /* 1) Environment variable */
    const char *env = getenv("MULTIONE_BRIDGE_PORT");
    if (env && env[0]) {
        log_msg("resolve_port: using env MULTIONE_BRIDGE_PORT=%s", env);
        strncpy(g_port, env, sizeof(g_port) - 1);
        return;
    }

    /* 2) INI file next to DLL */
    char ini_path[MAX_PATH];
    snprintf(ini_path, sizeof(ini_path), "%s\\%s", get_dll_dir(), INI_FILE_NAME);
    FILE *f = fopen(ini_path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            /* strip whitespace */
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "port=", 5) == 0) {
                p += 5;
                /* trim trailing whitespace/newline */
                char *end = p + strlen(p) - 1;
                while (end > p && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                if (*p) {
                    log_msg("resolve_port: using INI port=%s", p);
                    strncpy(g_port, p, sizeof(g_port) - 1);
                    fclose(f);
                    return;
                }
            }
        }
        fclose(f);
    }

    /* 3) Auto-scan */
    log_msg("resolve_port: auto-scanning COM%d..COM%d", SCAN_PORT_MIN, SCAN_PORT_MAX);
    for (int i = SCAN_PORT_MIN; i <= SCAN_PORT_MAX; i++) {
        char port[16];
        snprintf(port, sizeof(port), "COM%d", i);
        if (serial_probe(port)) {
            /* serial_probe already opened it and left it open */
            strncpy(g_port, port, sizeof(g_port) - 1);
            g_connected = 1;
            return;
        }
    }
    log_msg("resolve_port: no ESP32 found on any COM port");
}

/* ================================================================
 *  Password loading: INI file > JSON file > environment variable
 *  Password is NEVER hardcoded in this source file.
 * ================================================================ */
static void load_rf_password(void) {
    if (g_rf_password_loaded) return;
    char *dir = get_dll_dir();

    /* 1) INI file: password=<hex8> */
    {
        char ini_path[MAX_PATH];
        snprintf(ini_path, sizeof(ini_path), "%s\\%s", dir, INI_FILE_NAME);
        FILE *f = fopen(ini_path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (strncmp(p, "password", 8) == 0) {
                    p += 8;
                    while (*p == ' ' || *p == '=') p++;
                    char *end = p + strlen(p) - 1;
                    while (end > p && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                    if (strlen(p) >= 8) {
                        int n = hex_to_bytes(p, g_rf_password, 4);
                        if (n == 4) {
                            g_rf_password_loaded = 1;
                            log_msg("load_rf_password: from INI (%02X%02X%02X%02X)",
                                    g_rf_password[0], g_rf_password[1],
                                    g_rf_password[2], g_rf_password[3]);
                            fclose(f);
                            return;
                        }
                    }
                }
            }
            fclose(f);
        }
    }

    /* 2) JSON file: {"rf_passwords": ["<hex8>", ...]} — minimal parser */
    {
        const char *json_names[] = {
            "passwords.json",
            "NfcCommandsHandler.dll_passwords.json",
            "NfcCommandsHandler_passwords.json",
            NULL
        };
        for (int i = 0; json_names[i]; i++) {
            char json_path[MAX_PATH];
            snprintf(json_path, sizeof(json_path), "%s\\%s", dir, json_names[i]);
            FILE *f = fopen(json_path, "r");
            if (!f) continue;

            char buf[2048];
            int len = (int)fread(buf, 1, sizeof(buf) - 1, f);
            buf[len] = '\0';
            fclose(f);

            /* Find "rf_passwords" then first quoted 8-char hex string */
            char *p = strstr(buf, "rf_passwords");
            if (!p) continue;
            p = strchr(p, '[');
            if (!p) continue;
            p = strchr(p, '"');
            if (!p) continue;
            p++; /* skip opening quote */
            char *q = strchr(p, '"');
            if (!q || (q - p) < 8) continue;

            char hex[16] = {0};
            strncpy(hex, p, (q - p) < 15 ? (q - p) : 15);
            int n = hex_to_bytes(hex, g_rf_password, 4);
            if (n == 4) {
                g_rf_password_loaded = 1;
                log_msg("load_rf_password: from JSON %s (%02X%02X%02X%02X)",
                        json_names[i], g_rf_password[0], g_rf_password[1],
                        g_rf_password[2], g_rf_password[3]);
                return;
            }
        }
    }

    /* 3) Environment variable */
    {
        const char *env = getenv("SIGNIFY_RF_PASSWORD");
        if (env && strlen(env) >= 8) {
            int n = hex_to_bytes(env, g_rf_password, 4);
            if (n == 4) {
                g_rf_password_loaded = 1;
                log_msg("load_rf_password: from env SIGNIFY_RF_PASSWORD");
                return;
            }
        }
    }

    log_msg("load_rf_password: NO PASSWORD FOUND — add password=XXXXXXXX to multione_bridge.ini");
}

/* ================================================================
 *  ESP32 command helpers
 * ================================================================ */

/* Send a single-byte command and read one reply line. Returns reply length or -1. */
static int esp_cmd_simple(char cmd, char *reply, int replysize) {
    unsigned char c = (unsigned char)cmd;
    log_msg("esp_cmd_simple: sending '%c' (0x%02X)", cmd, c);
    if (!serial_write_bytes(&c, 1)) return -1;
    int n = serial_read_line(reply, replysize, SERIAL_TIMEOUT_MS);
    if (n > 0) log_msg("esp_cmd_simple: reply='%s'", reply);
    return n;
}

/* Send command byte + additional binary payload, read one reply line. */
static int esp_cmd_with_data(char cmd, const unsigned char *data, int datalen, char *reply, int replysize) {
    unsigned char buf[128];
    buf[0] = (unsigned char)cmd;
    if (datalen > 0 && datalen < (int)sizeof(buf) - 1) {
        memcpy(buf + 1, data, datalen);
    }
    int total = 1 + datalen;

    /* log hex of what we're sending */
    char hexbuf[300];
    bytes_to_hex(buf, total, hexbuf);
    log_msg("esp_cmd_with_data: sending %d bytes: %s", total, hexbuf);

    if (!serial_write_bytes(buf, total)) return -1;
    int n = serial_read_line(reply, replysize, SERIAL_TIMEOUT_MS);
    if (n > 0) log_msg("esp_cmd_with_data: reply='%s'", reply);
    return n;
}

/* ================================================================
 *  Exported DLL functions
 * ================================================================ */

/*
 * Connect() — open the serial port to the ESP32.
 * Returns: 0 on success, non-zero on failure.
 */
__declspec(dllexport) int __cdecl Connect(void) {
    log_msg(">>> Connect()");

    if (g_connected && g_serial != INVALID_HANDLE_VALUE) {
        /* Already connected — ping to verify */
        char reply[64];
        int n = esp_cmd_simple('P', reply, sizeof(reply));
        if (n > 0 && strncmp(reply, "PONG", 4) == 0) {
            log_msg("Connect: already connected to %s", g_port);
            return 0;
        }
        /* Connection lost, re-open */
        serial_close();
        g_connected = 0;
    }

    /* If auto-scan already found and opened the port during resolve_port, we're done */
    if (g_connected && g_serial != INVALID_HANDLE_VALUE) {
        log_msg("Connect: using auto-scanned port %s", g_port);
        return 0;
    }

    /* Resolve port if not yet known */
    if (g_port[0] == '\0') {
        resolve_port();
        /* resolve_port may have connected via auto-scan */
        if (g_connected && g_serial != INVALID_HANDLE_VALUE) {
            log_msg("Connect: auto-scan connected to %s", g_port);
            return 0;
        }
    }

    if (g_port[0] == '\0') {
        log_msg("Connect: FAILED — no port configured and auto-scan found nothing");
        return NFCRESULT_CONNECTION_ERROR;
    }

    /* Open the port (if not already open from auto-scan) */
    if (g_serial == INVALID_HANDLE_VALUE) {
        if (!serial_open(g_port)) {
            log_msg("Connect: FAILED to open %s", g_port);
            return NFCRESULT_CONNECTION_ERROR;
        }
    }

    /* Small settle time */
    Sleep(100);
    PurgeComm(g_serial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    /* Verify with ping — try a few times (ESP32 may emit READY line on DTR reset) */
    for (int attempt = 0; attempt < 5; attempt++) {
        char reply[128];
        int n = esp_cmd_simple('P', reply, sizeof(reply));
        if (n > 0 && strncmp(reply, "PONG", 4) == 0) {
            g_connected = 1;
            log_msg("Connect: SUCCESS on %s (attempt %d)", g_port, attempt);
            return 0;
        }
        if (n > 0) {
            log_msg("Connect: got unexpected line: '%s', retrying", reply);
        }
        Sleep(200);
    }

    log_msg("Connect: FAILED — no PONG from %s", g_port);
    serial_close();
    return NFCRESULT_COMMUNICATION_ERROR;
}

/*
 * Disconnect() — close the serial port.
 * Returns: 1 on success, 0 on failure.
 */
__declspec(dllexport) unsigned char __cdecl Disconnect(void) {
    log_msg(">>> Disconnect()");
    serial_close();
    g_connected = 0;
    g_uid[0] = '\0';
    log_msg("Disconnect: OK");
    return 1;
}

/*
 * DetectDevice(StringBuilder uidReply) — inventory the tag.
 * Sends 'I' to ESP32, parses "UID:xxxxxxxx" response.
 * Writes UID hex string into uidReply buffer.
 * Returns: 1 if tag found, 0 if not.
 */
/*
 * present_pwd_slot — present RF password on a given slot via raw ISO 15693.
 * Password is loaded at runtime (never hardcoded).
 * Returns 1 on success, 0 on failure.  Requires g_uid to be set.
 */
static int present_pwd_slot(int slot) {
    if (!g_rf_password_loaded) {
        load_rf_password();
        if (!g_rf_password_loaded) {
            log_msg("present_pwd_slot: no password available!");
            return 0;
        }
    }

    unsigned char uid_bytes[8];
    int uid_len = hex_to_bytes(g_uid, uid_bytes, 8);
    if (uid_len != 8) return 0;

    unsigned char pwd_cmd[16];
    pwd_cmd[0]  = 0x22;  /* flags: addressed, high data rate */
    pwd_cmd[1]  = 0xB3;  /* PresentPassword (ST custom) */
    pwd_cmd[2]  = 0x02;  /* ST manufacturer code */
    memcpy(&pwd_cmd[3], uid_bytes, 8);
    pwd_cmd[11] = (unsigned char)(slot & 0x03);
    memcpy(&pwd_cmd[12], g_rf_password, 4);

    unsigned char raw_buf[18];
    raw_buf[0] = 'C';
    raw_buf[1] = 16;
    memcpy(&raw_buf[2], pwd_cmd, 16);
    serial_write_bytes(raw_buf, 18);

    char resp[REPLY_BUF_SIZE];
    serial_read_line(resp, sizeof(resp), SERIAL_TIMEOUT_MS);
    log_msg("present_pwd_slot(%d) -> %s", slot, resp);
    return (strncmp(resp, "RAWOK:", 6) == 0) ? 1 : 0;
}

__declspec(dllexport) unsigned char __cdecl DetectDevice(char *uidReply) {
    log_msg(">>> DetectDevice(uidReply=%p)", uidReply);

    if (!g_connected || g_serial == INVALID_HANDLE_VALUE) {
        log_msg("DetectDevice: not connected");
        if (uidReply) uidReply[0] = '\0';
        return 0;
    }

    char reply[REPLY_BUF_SIZE];
    int n = esp_cmd_simple('I', reply, sizeof(reply));
    if (n <= 0) {
        log_msg("DetectDevice: no reply from ESP32");
        if (uidReply) uidReply[0] = '\0';
        return 0;
    }

    if (strncmp(reply, "UID:", 4) == 0) {
        const char *uid_hex = reply + 4;
        strncpy(g_uid, uid_hex, MAX_UID_HEX - 1);
        g_uid[MAX_UID_HEX - 1] = '\0';
        if (uidReply) {
            strcpy(uidReply, uid_hex);
        }
        log_msg("DetectDevice: tag found, UID=%s", g_uid);

        /* Present Signify RF password to unlock all sectors.
         * CRITICAL: Only present slot 1!
         * pwd_isolation_test.py proved:
         *   - Slot 1 alone: unlocks read+write for ALL blocks (0-63)
         *   - Presenting slot 0, 2, or 3 AFTER slot 1 REVOKES
         *     read access to blocks 1-31 (sector 0)
         *   - Slot 2 is only needed for WRITING blocks 32-63
         *     (present it just before a write, not at detect time)
         */
        present_pwd_slot(1);

        return 1;
    }

    /* NOTAG or error */
    log_msg("DetectDevice: no tag — reply='%s'", reply);
    if (uidReply) uidReply[0] = '\0';
    g_uid[0] = '\0';
    return 0;
}

/*
 * IsConnected() — check if serial port is open and ESP32 responds.
 * Returns: 1 if connected, 0 if not.
 */
__declspec(dllexport) unsigned char __cdecl IsConnected(void) {
    log_msg(">>> IsConnected()");

    if (!g_connected || g_serial == INVALID_HANDLE_VALUE) {
        log_msg("IsConnected: no (not connected)");
        return 0;
    }

    /* Quick ping check */
    char reply[64];
    int n = esp_cmd_simple('P', reply, sizeof(reply));
    unsigned char result = (n > 0 && strncmp(reply, "PONG", 4) == 0) ? 1 : 0;
    log_msg("IsConnected: %s", result ? "yes" : "no");
    return result;
}

/*
 * ReadBlock(int block, StringBuilder reply) — read a 4-byte block from the tag.
 * Sends 'R' + block_num to ESP32, expects "OK:AABBCCDD".
 * Writes 8 hex chars into reply buffer.
 * Returns: 0 on success, non-zero on failure.
 */
__declspec(dllexport) int __cdecl ReadBlock(int block, char *reply) {
    log_msg(">>> ReadBlock(block=%d, reply=%p)", block, reply);

    if (!g_connected || g_serial == INVALID_HANDLE_VALUE) {
        log_msg("ReadBlock: not connected");
        if (reply) reply[0] = '\0';
        return NFCRESULT_CONNECTION_ERROR;
    }

    /* Synthetic responses for blocks beyond physical tag memory.
     * The M24LR04E has 128 blocks (0-127). The tag directory maps the PWM
     * memory bank (MB 38144/0x9500) to block 248, which doesn't physically
     * exist. FeatureAoc.dll requires this bank to be readable (lock=0x55
     * = unlocked) before it will write AOC data. The FEIG CPR30 reader
     * somehow handles this; we synthesize valid memory bank data instead.
     *
     * PWM memory bank structure (block 248):
     *   byte 0: length (0x04 = 4 data bytes after header)
     *   byte 1: checksum (XOR of all other bytes = 0x50)
     *   byte 2: lock byte (0x55 = unlocked)
     *   byte 3: version (0x01)
     * PWM data (block 249):
     *   4 bytes of zeros (default/disabled PWM state)
     */
    if (block >= 128) {
        if (reply) {
            if (block == 248) {
                strncpy(reply, "04505501", 8);
            } else {
                strncpy(reply, "00000000", 8);
            }
            reply[8] = '\0';
        }
        log_msg("ReadBlock: block %d = synthetic %s", block,
                block == 248 ? "04505501 (PWM header)" : "00000000");
        return NFCRESULT_OK;
    }

    unsigned char payload[1] = { (unsigned char)(block & 0xFF) };
    char resp[REPLY_BUF_SIZE];
    int n = esp_cmd_with_data('R', payload, 1, resp, sizeof(resp));
    if (n <= 0) {
        log_msg("ReadBlock: no reply from ESP32");
        if (reply) reply[0] = '\0';
        return NFCRESULT_COMMUNICATION_ERROR;
    }

    if (strncmp(resp, "OK:", 3) == 0) {
        const char *hexdata = resp + 3;
        if (reply) {
            strncpy(reply, hexdata, 8);
            reply[8] = '\0';

            /* Unlock memory bank headers in-flight.
             * MultiOne's ReadMemoryBankLayout() checks byte 2 (lock byte)
             * of each memory bank header. If lock != 0x55 ("unlocked"),
             * the bank is excluded from the writable set and writes are
             * silently skipped. Factory tags ship with lock=0x00.
             * We patch the lock byte to 0x55 so MultiOne will write AOC
             * and other locked banks. Checksum (byte 1) is XOR of all
             * other bytes in the bank, so flipping the lock byte requires:
             *   new_cksum = old_cksum ^ old_lock ^ 0x55
             * This is safe even when the block is not a MB header — the
             * data is only used by MultiOne's layout parser for blocks it
             * identifies as headers via the directory. We limit this to
             * known MB header blocks to avoid unintended side effects. */
            if (block == 48) { /* AOC memory bank header */
                unsigned char b[4];
                if (hex_to_bytes(reply, b, 4) == 4 && b[2] != 0x55) {
                    unsigned char old_lock = b[2];
                    unsigned char old_cksum = b[1];
                    b[2] = 0x55;
                    b[1] = old_cksum ^ old_lock ^ 0x55;
                    sprintf(reply, "%02X%02X%02X%02X",
                            b[0], b[1], b[2], b[3]);
                    log_msg("ReadBlock: block %d lock byte patched "
                            "0x%02X->0x55 (cksum 0x%02X->0x%02X): %s",
                            block, old_lock, old_cksum, b[1], reply);
                }
            }
        }
        log_msg("ReadBlock: block %d = %s", block, reply);
        return NFCRESULT_OK;
    }

    /* Error — map ISO 15693 errors to NfcResult */
    log_msg("ReadBlock: error — '%s'", resp);
    if (reply) reply[0] = '\0';

    /* Parse error code: "ERR:read_XX" where XX is hex ISO error */
    if (strncmp(resp, "ERR:read_", 9) == 0) {
        int ec = (int)strtol(resp + 9, NULL, 16);
        if (ec == 0x12 || ec == 0x15) /* block locked or access denied */
            return NFCRESULT_SECURITY_ERROR;
        if (ec == 0x0F || ec == 0x10) /* crypto / no response */
            return NFCRESULT_COMMUNICATION_ERROR;
    }
    return NFCRESULT_PROTOCOL_ERROR;
}

/*
 * WriteBlock(int block, string values) — write 4 bytes to a block.
 * The 'values' string is 8 hex characters (e.g., "AABBCCDD").
 * Sends 'W' + block_num + 4 binary bytes.
 * Returns: 0 on success, non-zero on failure.
 */
__declspec(dllexport) int __cdecl WriteBlock(int block, const char *values) {
    log_msg(">>> WriteBlock(block=%d, values='%s')", block, values ? values : "(null)");

    if (!g_connected || g_serial == INVALID_HANDLE_VALUE) {
        log_msg("WriteBlock: not connected");
        return NFCRESULT_CONNECTION_ERROR;
    }

    if (!values || strlen(values) < 8) {
        log_msg("WriteBlock: invalid values string (need 8 hex chars)");
        return NFCRESULT_DATA_ERROR;
    }

    /* Silently accept writes to synthetic blocks (>= 128) — they don't
     * physically exist on the tag but MultiOne may try to write them. */
    if (block >= 128) {
        log_msg("WriteBlock: block %d = synthetic accept (not on physical tag)", block);
        return NFCRESULT_OK;
    }

    unsigned char data[5]; /* block_num + 4 data bytes */
    data[0] = (unsigned char)(block & 0xFF);
    int nbytes = hex_to_bytes(values, data + 1, 4);
    if (nbytes != 4) {
        log_msg("WriteBlock: hex parse failed (got %d bytes from '%s')", nbytes, values);
        return NFCRESULT_DATA_ERROR;
    }

    /* Present password slot 2 before writing to unlock write access.
     * Slot 2 enables writes to blocks 32-63 (sector 1).
     * Slot 1 (already presented at detect time) enables writes to blocks 1-31.
     * NOTE: presenting slot 2 revokes slot 1 read access, so we must
     * re-present slot 1 after the write to restore readability. */
    int need_slot2 = (block >= 32 && block <= 63);
    if (need_slot2) {
        log_msg("WriteBlock: presenting pwd slot 2 for sector 1 write");
        present_pwd_slot(2);
    }

    char resp[REPLY_BUF_SIZE];
    int n = esp_cmd_with_data('W', data, 5, resp, sizeof(resp));

    /* Restore slot 1 read access if we presented slot 2 */
    if (need_slot2) {
        log_msg("WriteBlock: re-presenting pwd slot 1 to restore read access");
        present_pwd_slot(1);
    }

    if (n <= 0) {
        log_msg("WriteBlock: no reply from ESP32");
        return NFCRESULT_COMMUNICATION_ERROR;
    }

    if (strcmp(resp, "OK") == 0) {
        log_msg("WriteBlock: block %d written OK", block);
        return NFCRESULT_OK;
    }

    log_msg("WriteBlock: error — '%s'", resp);
    if (strncmp(resp, "ERR:write_", 10) == 0) {
        int ec = (int)strtol(resp + 10, NULL, 16);
        if (ec == 0x12 || ec == 0x15)
            return NFCRESULT_SECURITY_ERROR;
    }
    return NFCRESULT_PROTOCOL_ERROR;
}

/*
 * ReadBlockSecurity() — read block security status for the entire tag.
 * Sends 'B' + start(0) + count(64) to ESP32 (multiple calls if needed).
 * Caches the result. Returns 0 on success.
 *
 * Note: The original DLL caches this and the .NET code reads individual
 * block security via some other mechanism. We read all 128 blocks in two
 * 64-block chunks and cache the result.
 */
__declspec(dllexport) int __cdecl ReadBlockSecurity(void) {
    log_msg(">>> ReadBlockSecurity()");

    if (!g_connected || g_serial == INVALID_HANDLE_VALUE) {
        log_msg("ReadBlockSecurity: not connected");
        return NFCRESULT_CONNECTION_ERROR;
    }

    memset(g_blksec, 0, sizeof(g_blksec));
    g_blksec_start = 0;
    g_blksec_count = 0;

    /* Read security in chunks of 64 blocks (max per protocol) */
    for (int start = 0; start < 256; start += 64) {
        int count = 64;
        unsigned char payload[2] = { (unsigned char)start, (unsigned char)count };
        char resp[REPLY_BUF_SIZE];
        int n = esp_cmd_with_data('B', payload, 2, resp, sizeof(resp));
        if (n <= 0) {
            log_msg("ReadBlockSecurity: no reply for chunk at block %d", start);
            break;
        }

        /* Parse "BLKSEC:start:count:hexdata" */
        if (strncmp(resp, "BLKSEC:", 7) == 0) {
            /* Find the hex data after the third colon */
            char *p = resp + 7;
            /* skip start number */
            while (*p && *p != ':') p++;
            if (*p == ':') p++;
            /* skip count number */
            while (*p && *p != ':') p++;
            if (*p == ':') p++;
            /* p now points to hex data */
            int nbytes = hex_to_bytes(p, g_blksec + start, count);
            g_blksec_count = start + nbytes;
            log_msg("ReadBlockSecurity: got %d bytes for blocks %d+", nbytes, start);
        } else if (strncmp(resp, "ERR:", 4) == 0) {
            log_msg("ReadBlockSecurity: error at block %d — '%s', stopping", start, resp);
            break;
        }
    }

    log_msg("ReadBlockSecurity: cached %d block security bytes", g_blksec_count);
    return 0;
}

/*
 * ResetSecurity() — clear the internal block security cache.
 *
 * RE of the original DLL shows this is just a 7-byte function:
 *   mov word ptr [ecx+0Ch], 0   ; clear two flag bytes
 *   ret
 * It does NO NFC communication. It just clears the DLL's cached knowledge of
 * which blocks are protected, so WriteBlock/ReadBlock skip the client-side
 * security gate and go directly to the hardware.
 */
__declspec(dllexport) void __cdecl ResetSecurity(void) {
    log_msg(">>> ResetSecurity()");
    /* Clear our cached block security data — matches original behavior */
    memset(g_blksec, 0, sizeof(g_blksec));
    g_blksec_count = 0;
    log_msg("ResetSecurity: cache cleared (no RF communication, matching original DLL)");
}

/*
 * SetOdmId(int odmId) — store the ODM ID (log only, the ESP32 doesn't need it).
 * .NET signature: byte SetOdmId(int odmId) — returns uint8.
 */
__declspec(dllexport) unsigned char __cdecl SetOdmId(int odmId) {
    log_msg(">>> SetOdmId(odmId=%d [0x%08X])", odmId, odmId);
    g_odmId = odmId;
    return 1; /* success */
}

/*
 * GetSupportedReaders(bool flag) — return a pointer to a list of available readers.
 *
 * Decompiled .NET shows the return IntPtr points to:
 *   struct { IntPtr NfcReaders; int NumberOfReaders; }
 * where NfcReaders points to an array of:
 *   struct NfcReader { uint32 Type; char* UniqueId; char* ReaderName; }
 * NfcReaderType: 0=Feig, 1=St
 *
 * We return a static buffer with one reader entry, type=Feig (0).
 */

/* Static storage for the return structures */
typedef struct { unsigned int type; const char *uid; const char *name; } ShimNfcReader;
typedef struct { ShimNfcReader *readers; int count; } ShimReaderList;

static ShimNfcReader g_shim_reader;
static ShimReaderList g_shim_reader_list;
static char g_reader_uid_str[128] = "ESP32-PN5180";
static char g_reader_name_str[128] = "ESP32+PN5180 NFC Reader";

__declspec(dllexport) void* __cdecl GetSupportedReaders(int flag) {
    log_msg(">>> GetSupportedReaders(flag=%d)", flag);

    g_shim_reader.type = 0; /* Feig = 0, St = 1 */
    g_shim_reader.uid = g_reader_uid_str;
    g_shim_reader.name = g_reader_name_str;

    g_shim_reader_list.readers = &g_shim_reader;
    g_shim_reader_list.count = 1;

    log_msg("GetSupportedReaders: returning 1 reader (type=%d, uid='%s', name='%s')",
            g_shim_reader.type, g_shim_reader.uid, g_shim_reader.name);
    return (void*)&g_shim_reader_list;
}

/*
 * SetSelectedReader(string uid, string name) — select which reader to use.
 * We just log it and store the values.
 */
__declspec(dllexport) void __cdecl SetSelectedReader(const char *uid, const char *name) {
    log_msg(">>> SetSelectedReader(uid='%s', name='%s')",
            uid ? uid : "(null)", name ? name : "(null)");
    if (uid) strncpy(g_reader_uid, uid, sizeof(g_reader_uid) - 1);
    if (name) strncpy(g_reader_name, name, sizeof(g_reader_name) - 1);
}

/*
 * GetDllVersion(StringBuilder sb) — write version string into sb.
 * .NET: void GetDllVersion(StringBuilder)
 */
__declspec(dllexport) void __cdecl GetDllVersion(char *sb) {
    log_msg(">>> GetDllVersion(sb=%p)", sb);
    if (sb) strcpy(sb, g_dll_version);
}

/*
 * GetFirmwareRevision(StringBuilder sb) — write firmware revision into sb.
 * .NET: int GetFirmwareRevision(StringBuilder)
 */
__declspec(dllexport) int __cdecl GetFirmwareRevision(char *sb) {
    log_msg(">>> GetFirmwareRevision(sb=%p)", sb);
    if (sb) strcpy(sb, g_fw_revision);
    return 0;
}

/*
 * GetHardwareName(StringBuilder sb) — write hardware name into sb.
 * .NET: int GetHardwareName(StringBuilder)
 */
__declspec(dllexport) int __cdecl GetHardwareName(char *sb) {
    log_msg(">>> GetHardwareName(sb=%p)", sb);
    if (sb) strcpy(sb, g_hw_name);
    return 0;
}

/*
 * GetHardwareVersion(out byte version) — write hardware version byte.
 * .NET: int GetHardwareVersion(out byte)
 */
__declspec(dllexport) int __cdecl GetHardwareVersion(unsigned char *version) {
    log_msg(">>> GetHardwareVersion(version=%p)", version);
    if (version) *version = 1;
    return 0;
}

/*
 * RefreshConnection() — verify the connection is still alive.
 * Returns: 1 if OK, 0 if not.
 */
__declspec(dllexport) unsigned char __cdecl RefreshConnection(void) {
    log_msg(">>> RefreshConnection()");

    if (!g_connected || g_serial == INVALID_HANDLE_VALUE) {
        log_msg("RefreshConnection: not connected");
        return 0;
    }

    char reply[64];
    int n = esp_cmd_simple('P', reply, sizeof(reply));
    unsigned char result = (n > 0 && strncmp(reply, "PONG", 4) == 0) ? 1 : 0;
    log_msg("RefreshConnection: %s", result ? "OK" : "FAILED");
    return result;
}

/*
 * FreeScanForReaderReturnValue(IntPtr ptr) — free memory returned by GetSupportedReaders.
 * Since we use a static buffer, this is a no-op.
 */
__declspec(dllexport) void __cdecl FreeScanForReaderReturnValue(void *ptr) {
    log_msg(">>> FreeScanForReaderReturnValue(ptr=%p)", ptr);
    /* No-op: our reader list is a static buffer */
}

/* ================================================================
 *  DllMain
 * ================================================================ */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        /* Get the DLL's directory for log/ini file location.
         * Do NOT open files or COM ports here — DllMain runs under
         * the OS loader lock, so complex I/O can deadlock or crash. */
        GetModuleFileNameA(hModule, g_dll_dir, MAX_PATH);
        {
            char *last_slash = strrchr(g_dll_dir, '\\');
            if (last_slash) *last_slash = '\0';
        }
        break;

    case DLL_PROCESS_DETACH:
        log_msg("DllMain: DLL_PROCESS_DETACH");
        serial_close();
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
        break;
    }

    return TRUE;
}
