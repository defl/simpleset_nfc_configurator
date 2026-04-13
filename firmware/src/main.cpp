/*
 * ESP32 + PN5180 — ISO 15693 NFC Serial Bridge
 *
 * Exposes a simple binary serial protocol so that host tools (CLI, shim DLL)
 * can read/write ISO 15693 NFC tags over USB serial. This replaces a $500+
 * FEIG NFC reader with ~$5 of hardware.
 *
 * Hardware:
 *   ESP32-WROOM-32 dev board + PN5180 NFC module
 *
 * Wiring (ESP32 VSPI → PN5180):
 *   MOSI  → GPIO 23
 *   MISO  → GPIO 19
 *   SCK   → GPIO 18
 *   NSS   → GPIO  5
 *   BUSY  → GPIO 16
 *   RST   → GPIO 17
 *   VIN   → 5V   (NOT 3.3V! PN5180 has onboard regulator, needs 5V input)
 *   GND   → GND
 *
 * Serial protocol (115200 baud, 8N1):
 *   Host → ESP32:  <CMD> [params...]
 *   ESP32 → Host:  <RSP>\n
 *
 * Commands:
 *   'P'                       → Ping.          Response: "PONG"
 *   'I'                       → Inventory.     Response: "UID:<16hex>" or "NOTAG:<err>"
 *   'R' <block:1>             → Read block.    Response: "OK:<8hex>" or "ERR:..."
 *   'W' <block:1> <data:4>    → Write block.   Response: "OK" or "ERR:..."
 *   'D' <start:1> <count:1>   → Dump blocks.   Response: "BLK:nn:<8hex>" per block, then "END"
 *   'S'                       → Status.        Response: "INFO:tag=yes/no,uid=..."
 *   'X' <slot:1> <pwd:4>      → Present pwd.   Response: "PWDOK" or "PWDFAIL:<err>"
 *   'G'                       → System info.   Response: "SYSINFO:blocks=N,blocksize=N"
 *   'B' <start:1> <count:1>   → Block security. Response: "BLKSEC:start:count:<hex>"
 *   'F'                       → Reset RF field. Response: "FIELDRESET"
 *   'C' <len:1> <raw:N>       → Raw ISO 15693. Response: "RAWOK:len:<hex>" or "RAWERR:<err>"
 *
 * All numeric params are single raw bytes. All responses are newline-terminated ASCII.
 *
 * License: MIT
 */

#include <Arduino.h>
#include <SPI.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>

// ─── Pin definitions (ESP32-WROOM-32, VSPI bus) ───
#define PN5180_NSS    5
#define PN5180_BUSY  16
#define PN5180_RST   17

PN5180ISO15693 nfc(PN5180_NSS, PN5180_BUSY, PN5180_RST);

uint8_t tagUID[8] = {0};
bool tagPresent = false;

// ─── Forward declarations ───
void handleCommand(char cmd);
int waitByte(unsigned long timeout_ms);
bool waitBytes(uint8_t* buf, int count, unsigned long timeout_ms);
void printHex(uint8_t* data, int len);
void doInventory();
void doReadBlock();
void doWriteBlock();
void doDump();
void doStatus();
void doPresentPassword();
void doGetSystemInfo();
void doGetBlockSecurityStatus();
void doResetField();
void doRawISO();

// ─── Setup ───
void setup() {
    Serial.begin(115200);
    while (!Serial);

    nfc.begin();
    nfc.reset();

    uint8_t productVersion[2];
    nfc.readEEprom(PRODUCT_VERSION, productVersion, sizeof(productVersion));
    uint8_t firmwareVersion[2];
    nfc.readEEprom(FIRMWARE_VERSION, firmwareVersion, sizeof(firmwareVersion));

    Serial.print("READY:PN5180 v");
    Serial.print(productVersion[1]); Serial.print("."); Serial.print(productVersion[0]);
    Serial.print(" fw=");
    Serial.print(firmwareVersion[1]); Serial.print("."); Serial.println(firmwareVersion[0]);

    nfc.setupRF();
}

// ─── Main loop ───
void loop() {
    if (Serial.available() > 0) {
        char cmd = Serial.read();
        handleCommand(cmd);
    }
}

void handleCommand(char cmd) {
    switch (cmd) {
        case 'P': Serial.println("PONG"); break;
        case 'I': doInventory(); break;
        case 'R': doReadBlock(); break;
        case 'W': doWriteBlock(); break;
        case 'D': doDump(); break;
        case 'S': doStatus(); break;
        case 'X': doPresentPassword(); break;
        case 'G': doGetSystemInfo(); break;
        case 'B': doGetBlockSecurityStatus(); break;
        case 'F': doResetField(); break;
        case 'C': doRawISO(); break;
        case '\n': case '\r': break;
        default:
            Serial.print("ERR:unknown_cmd_0x");
            Serial.println(cmd, HEX);
            break;
    }
}

// ─── Helpers ───

int waitByte(unsigned long timeout_ms) {
    unsigned long start = millis();
    while (!Serial.available()) {
        if (millis() - start > timeout_ms) return -1;
    }
    return Serial.read();
}

bool waitBytes(uint8_t* buf, int count, unsigned long timeout_ms) {
    for (int i = 0; i < count; i++) {
        int b = waitByte(timeout_ms);
        if (b < 0) return false;
        buf[i] = (uint8_t)b;
    }
    return true;
}

void printHex(uint8_t* data, int len) {
    for (int i = 0; i < len; i++) {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
    }
}

// ─── Inventory ───
void doInventory() {
    nfc.reset();
    nfc.setupRF();

    ISO15693ErrorCode ec = nfc.getInventory(tagUID);
    if (ec == ISO15693_EC_OK) {
        tagPresent = true;
        Serial.print("UID:");
        printHex(tagUID, 8);
        Serial.println();
    } else {
        tagPresent = false;
        memset(tagUID, 0, 8);
        Serial.print("NOTAG:");
        Serial.println(ec, HEX);
    }
}

// ─── Read single block ───
void doReadBlock() {
    int blockNum = waitByte(1000);
    if (blockNum < 0) { Serial.println("ERR:timeout"); return; }

    uint8_t blockData[4];
    ISO15693ErrorCode ec = nfc.readSingleBlock(tagUID, (uint8_t)blockNum, blockData, sizeof(blockData));
    if (ec == ISO15693_EC_OK) {
        Serial.print("OK:");
        printHex(blockData, 4);
        Serial.println();
    } else {
        Serial.print("ERR:read_");
        Serial.println(ec, HEX);
    }
}

// ─── Write single block ───
void doWriteBlock() {
    int blockNum = waitByte(1000);
    if (blockNum < 0) { Serial.println("ERR:timeout_block"); return; }

    uint8_t data[4];
    if (!waitBytes(data, 4, 1000)) { Serial.println("ERR:timeout_data"); return; }

    // Try standard write first
    ISO15693ErrorCode ec = nfc.writeSingleBlock(tagUID, (uint8_t)blockNum, data, sizeof(data));
    if (ec == ISO15693_EC_OK) {
        Serial.println("OK");
        return;
    }

    // If block is locked, retry with Option flag (0x62).
    // M24LR requires Option flag for writes to password-protected sectors.
    if (ec == ISO15693_EC_BLOCK_IS_LOCKED) {
        uint8_t cmd[15];
        cmd[0] = 0x62;  // flags: addressed + high data rate + OPTION
        cmd[1] = 0x21;  // WriteSingleBlock
        memcpy(&cmd[2], tagUID, 8);
        cmd[10] = (uint8_t)blockNum;
        memcpy(&cmd[11], data, 4);

        uint8_t* resultPtr;
        ec = nfc.issueISO15693Command(cmd, sizeof(cmd), &resultPtr);
        if (ec == ISO15693_EC_OK) {
            Serial.println("OK");
            return;
        }
    }

    Serial.print("ERR:write_");
    Serial.println(ec, HEX);
}

// ─── Dump range of blocks ───
void doDump() {
    int startBlock = waitByte(1000);
    if (startBlock < 0) { Serial.println("ERR:timeout_start"); return; }
    int count = waitByte(1000);
    if (count < 0) { Serial.println("ERR:timeout_count"); return; }

    uint8_t blockData[4];
    for (int i = 0; i < count; i++) {
        int bn = startBlock + i;
        ISO15693ErrorCode ec = nfc.readSingleBlock(tagUID, (uint8_t)bn, blockData, sizeof(blockData));
        Serial.print("BLK:");
        if (bn < 0x10) Serial.print('0');
        Serial.print(bn, HEX);
        Serial.print(":");
        if (ec == ISO15693_EC_OK) {
            printHex(blockData, 4);
        } else {
            Serial.print("ERR_");
            Serial.print(ec, HEX);
        }
        Serial.println();
    }
    Serial.println("END");
}

// ─── Status ───
void doStatus() {
    Serial.print("INFO:tag=");
    Serial.print(tagPresent ? "yes" : "no");
    if (tagPresent) {
        Serial.print(",uid=");
        printHex(tagUID, 8);
    }
    Serial.println();
}

// ─── Present Password (ST M24LR custom cmd 0xB3) ───
void doPresentPassword() {
    int pwdNum = waitByte(1000);
    if (pwdNum < 0) { Serial.println("ERR:timeout_sector"); return; }

    uint8_t pwd[4];
    if (!waitBytes(pwd, 4, 1000)) { Serial.println("ERR:timeout_pwd"); return; }

    uint8_t cmd[16];
    cmd[0] = 0x22;  // flags: addressed, high data rate
    cmd[1] = 0xB3;  // PresentPassword (ST custom)
    cmd[2] = 0x02;  // ST manufacturer code
    memcpy(&cmd[3], tagUID, 8);
    cmd[11] = (uint8_t)(pwdNum & 0x03);
    memcpy(&cmd[12], pwd, 4);

    uint8_t* resultPtr;
    ISO15693ErrorCode ec = nfc.issueISO15693Command(cmd, sizeof(cmd), &resultPtr);
    if (ec == ISO15693_EC_OK) {
        Serial.println("PWDOK");
    } else {
        Serial.print("PWDFAIL:");
        Serial.println(ec, HEX);
    }
}

// ─── Get System Info ───
void doGetSystemInfo() {
    uint8_t blockSize = 0;
    uint8_t numBlocks = 0;
    ISO15693ErrorCode ec = nfc.getSystemInfo(tagUID, &blockSize, &numBlocks);
    if (ec == ISO15693_EC_OK) {
        Serial.print("SYSINFO:blocks=");
        Serial.print(numBlocks);
        Serial.print(",blocksize=");
        Serial.println(blockSize);
    } else {
        Serial.print("ERR:sysinfo_");
        Serial.println(ec, HEX);
    }
}

// ─── Get Block Security Status ───
void doGetBlockSecurityStatus() {
    int startBlock = waitByte(1000);
    if (startBlock < 0) { Serial.println("ERR:timeout_start"); return; }
    int count = waitByte(1000);
    if (count < 0) { Serial.println("ERR:timeout_count"); return; }
    if (count == 0) count = 1;
    if (count > 64) count = 64;

    uint8_t cmd[12];
    cmd[0] = 0x22;  // flags: addressed, high data rate
    cmd[1] = 0x2C;  // Get Multiple Block Security Status
    memcpy(&cmd[2], tagUID, 8);
    cmd[10] = (uint8_t)(startBlock & 0xFF);
    cmd[11] = (uint8_t)((count - 1) & 0xFF);

    uint8_t* resultPtr;
    ISO15693ErrorCode ec = nfc.issueISO15693Command(cmd, sizeof(cmd), &resultPtr);
    if (ec == ISO15693_EC_OK) {
        Serial.print("BLKSEC:");
        Serial.print(startBlock);
        Serial.print(":");
        Serial.print(count);
        Serial.print(":");
        for (int i = 0; i < count; i++) {
            if (resultPtr[1 + i] < 0x10) Serial.print('0');
            Serial.print(resultPtr[1 + i], HEX);
        }
        Serial.println();
    } else {
        Serial.print("ERR:blksec_");
        Serial.println(ec, HEX);
    }
}

// ─── Raw ISO 15693 command passthrough ───
void doRawISO() {
    int len = waitByte(1000);
    if (len < 0) { Serial.println("ERR:timeout_len"); return; }
    if (len > 64) { Serial.println("ERR:too_long"); return; }

    uint8_t cmd[64];
    if (!waitBytes(cmd, len, 2000)) { Serial.println("ERR:timeout_data"); return; }

    uint8_t* resultPtr;
    ISO15693ErrorCode ec = nfc.issueISO15693Command(cmd, len, &resultPtr);
    if (ec == ISO15693_EC_OK) {
        uint32_t rxStat;
        nfc.readRegister(RX_STATUS, &rxStat);
        uint16_t respLen = (uint16_t)(rxStat & 0x000001ff);
        if (respLen == 0 || respLen > 64) respLen = 32;

        Serial.print("RAWOK:");
        Serial.print(respLen);
        Serial.print(":");
        for (uint16_t i = 0; i < respLen; i++) {
            if (resultPtr[i] < 0x10) Serial.print('0');
            Serial.print(resultPtr[i], HEX);
        }
        Serial.println();
    } else {
        Serial.print("RAWERR:");
        Serial.println(ec, HEX);
    }
}

// ─── Reset RF field (re-locks all passwords) ───
void doResetField() {
    nfc.reset();
    delay(50);
    nfc.setupRF();
    tagPresent = false;
    memset(tagUID, 0, 8);
    Serial.println("FIELDRESET");
}
