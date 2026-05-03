#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "lora_config.h"
#include "packet.h"

// ─── Konfigurasi Anchor ───────────────────────────────────────────────────────
#ifndef ANCHOR_ID
  #define ANCHOR_ID  1
#endif
#ifndef ANCHOR_X
  #define ANCHOR_X   0.0
#endif
#ifndef ANCHOR_Y
  #define ANCHOR_Y   0.0
#endif

// ─── Prototypes ──────────────────────────────────────────────────────────────
void   checkResetButton();
void   initWiFi();
void   openServerIPPortal();
void   initLoRa();
bool   parsePacket(AnchorReport &report);
void   sendReportToServer(const AnchorReport &report);
void   saveServerIP(const char *ip);
String loadServerIP();
bool   isValidIP(const String &ip);

// ─── Global ──────────────────────────────────────────────────────────────────
Preferences            prefs;
String                 serverIP = "";
WiFiManagerParameter  *paramServerIP;

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial);

    char apName[20];
    snprintf(apName, sizeof(apName), "Anchor-%d", ANCHOR_ID);
    Serial.printf("\n[ANCHOR-%d] Booting...\n", ANCHOR_ID);

    // Cek tombol dulu sebelum hal lain
    checkResetButton();

    // Init WiFi
    initWiFi();

    // Tampilkan Server IP yang akan digunakan
    Serial.printf("[ANCHOR-%d] Server IP: %s\n", ANCHOR_ID, serverIP.c_str());

    // Init LoRa
    initLoRa();

    Serial.printf("[ANCHOR-%d] Ready | IP: %s | Server: %s:%d\n",
                  ANCHOR_ID,
                  WiFi.localIP().toString().c_str(),
                  serverIP.c_str(),
                  SERVER_PORT);
}

void loop() {
    // Reconnect WiFi jika putus
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[ANCHOR-%d] WiFi putus, reconnecting...\n", ANCHOR_ID);
        WiFi.reconnect();
        unsigned long t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(500);
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[ANCHOR-%d] Gagal reconnect, restart...\n", ANCHOR_ID);
            ESP.restart();
        }
    }

    // Polling LoRa
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) return;

    AnchorReport report;
    if (parsePacket(report)) {
        sendReportToServer(report);
    }
}

// ─── Cek Tombol BOOT saat startup ────────────────────────────────────────────
//
//  Tidak ditekan          → boot normal
//  Tahan 3 detik          → buka portal update Server IP (WiFi tetap)
//  Tahan 6 detik          → reset TOTAL (hapus WiFi + Server IP)
//
void checkResetButton() {
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    if (digitalRead(RESET_BUTTON_PIN) != LOW) return;  // Tidak ditekan

    Serial.printf("[ANCHOR-%d] Tombol BOOT ditekan...\n", ANCHOR_ID);
    Serial.printf("[ANCHOR-%d]  3 detik = Portal Server IP\n", ANCHOR_ID);
    Serial.printf("[ANCHOR-%d]  6 detik = Reset TOTAL\n", ANCHOR_ID);

    unsigned long pressTime = millis();
    bool          reached3s = false;

    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
        unsigned long held = millis() - pressTime;

        // Feedback setiap 1 detik
        static unsigned long lastPrint = 0;
        if (millis() - lastPrint >= 1000) {
            lastPrint = millis();
            Serial.printf("[ANCHOR-%d] Ditahan: %lu detik...\n", ANCHOR_ID, held / 1000 + 1);
        }

        // Tandai sudah lewat 3 detik
        if (!reached3s && held >= HOLD_PORTAL_IP_MS) {
            reached3s = true;
            Serial.printf("[ANCHOR-%d] >> Lepas sekarang untuk Portal Server IP\n", ANCHOR_ID);
            Serial.printf("[ANCHOR-%d] >> Tahan terus untuk Reset TOTAL\n", ANCHOR_ID);
        }

        // Sudah 6 detik → reset total
        if (held >= HOLD_RESET_TOTAL_MS) {
            Serial.printf("[ANCHOR-%d] RESET TOTAL — Menghapus WiFi + Server IP...\n", ANCHOR_ID);
            WiFiManager wm;
            wm.resetSettings();
            prefs.begin("anchor-cfg", false);
            prefs.clear();
            prefs.end();
            Serial.printf("[ANCHOR-%d] Selesai. Restart...\n", ANCHOR_ID);
            delay(300);
            ESP.restart();
        }
    }

    // Tombol dilepas antara 3–6 detik → portal Server IP saja
    unsigned long held = millis() - pressTime;
    if (held >= HOLD_PORTAL_IP_MS) {
        Serial.printf("[ANCHOR-%d] Membuka portal update Server IP...\n", ANCHOR_ID);
        // WiFi sudah tersimpan, konek dulu lalu buka portal
        WiFi.begin();
        unsigned long t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(300);
        openServerIPPortal();
        ESP.restart();
    }

    // Dilepas sebelum 3 detik → batal
    Serial.printf("[ANCHOR-%d] Tombol dilepas, boot normal.\n", ANCHOR_ID);
}

// ─── Portal khusus update Server IP (tanpa reset WiFi) ───────────────────────
void openServerIPPortal() {
    char apName[20];
    snprintf(apName, sizeof(apName), "Anchor-%d", ANCHOR_ID);

    char savedIP[40];
    serverIP.toCharArray(savedIP, sizeof(savedIP));

    WiFiManagerParameter paramIP("server_ip", "Server IP (contoh: 192.168.1.100)", savedIP, 39);

    WiFiManager wm;
    wm.addParameter(&paramIP);
    wm.setCustomHeadElement(
        "<style>body{font-family:sans-serif;} label{font-weight:bold;}</style>"
        "<h3 style='color:#1a73e8'>Update Server IP</h3>"
        "<p style='color:#555'>Isi IP laptop/PC yang menjalankan server Node.js.</p>"
    );

    // startConfigPortal: buka portal tanpa reset WiFi
    wm.setConfigPortalTimeout(WIFI_TIMEOUT_SEC);

    Serial.printf("[ANCHOR-%d] Portal aktif: sambung WiFi 'Anchor-%d' → buka http://192.168.4.1\n",
                  ANCHOR_ID, ANCHOR_ID);

    wm.startConfigPortal(apName, WIFI_AP_PASSWORD);

    // Ambil nilai yang diisi user
    String newIP = String(paramIP.getValue());
    newIP.trim();

    if (newIP.length() > 0 && isValidIP(newIP)) {
        // User mengisi IP yang valid — simpan ke NVS
        saveServerIP(newIP.c_str());
        serverIP = newIP;
        Serial.printf("[ANCHOR-%d] Server IP disimpan: %s\n", ANCHOR_ID, serverIP.c_str());
    } else if (newIP.length() > 0) {
        // User mengisi tapi format salah — tolak, pakai default
        Serial.printf("[ANCHOR-%d] ERROR: Format IP tidak valid: '%s'\n", ANCHOR_ID, newIP.c_str());
        Serial.printf("[ANCHOR-%d] Format benar: xxx.xxx.xxx.xxx\n", ANCHOR_ID);
        serverIP = SERVER_IP_DEFAULT;
        Serial.printf("[ANCHOR-%d] Menggunakan IP default: %s\n", ANCHOR_ID, serverIP.c_str());
    } else {
        // User tidak mengisi — pakai default
        serverIP = SERVER_IP_DEFAULT;
        Serial.printf("[ANCHOR-%d] Server IP tidak diisi, menggunakan default: %s\n",
                      ANCHOR_ID, serverIP.c_str());
    }
}

// ─── Inisialisasi WiFi dengan WiFiManager ────────────────────────────────────
void initWiFi() {
    serverIP = loadServerIP();

    char savedIP[40];
    serverIP.toCharArray(savedIP, sizeof(savedIP));

    paramServerIP = new WiFiManagerParameter(
        "server_ip", "Server IP (contoh: 192.168.1.100)", savedIP, 39
    );

    WiFiManager wm;
    wm.addParameter(paramServerIP);

    wm.setAPCallback([](WiFiManager *mgr) {
        Serial.printf("[ANCHOR-%d] Portal konfigurasi aktif!\n", ANCHOR_ID);
        Serial.printf("[ANCHOR-%d] Sambung ke WiFi 'Anchor-%d' (pass: %s)\n",
                      ANCHOR_ID, ANCHOR_ID, WIFI_AP_PASSWORD);
        Serial.printf("[ANCHOR-%d] Lalu buka browser: http://192.168.4.1\n", ANCHOR_ID);
        Serial.printf("[ANCHOR-%d] Isi SSID WiFi + Server IP lalu klik Save.\n", ANCHOR_ID);
    });

    wm.setSaveConfigCallback([]() {
        String newIP = String(paramServerIP->getValue());
        newIP.trim();
        if (newIP.length() > 0 && isValidIP(newIP)) {
            // User mengisi IP yang valid — simpan ke NVS
            saveServerIP(newIP.c_str());
            serverIP = newIP;
            Serial.printf("[ANCHOR-%d] Server IP disimpan: %s\n", ANCHOR_ID, serverIP.c_str());
        } else if (newIP.length() > 0) {
            // Format salah — tolak, pakai default
            Serial.printf("[ANCHOR-%d] ERROR: Format IP tidak valid: '%s'\n", ANCHOR_ID, newIP.c_str());
            Serial.printf("[ANCHOR-%d] Format benar: xxx.xxx.xxx.xxx\n", ANCHOR_ID);
            serverIP = SERVER_IP_DEFAULT;
            saveServerIP(SERVER_IP_DEFAULT);
            Serial.printf("[ANCHOR-%d] Menggunakan IP default: %s\n", ANCHOR_ID, serverIP.c_str());
        } else {
            // Tidak diisi — pakai default
            serverIP = SERVER_IP_DEFAULT;
            saveServerIP(SERVER_IP_DEFAULT);
            Serial.printf("[ANCHOR-%d] Server IP tidak diisi, menggunakan default: %s\n",
                          ANCHOR_ID, serverIP.c_str());
        }
    });

    if (WIFI_TIMEOUT_SEC > 0) wm.setConfigPortalTimeout(WIFI_TIMEOUT_SEC);

    char apName[20];
    snprintf(apName, sizeof(apName), "Anchor-%d", ANCHOR_ID);

    if (!wm.autoConnect(apName, WIFI_AP_PASSWORD)) {
        Serial.printf("[ANCHOR-%d] Timeout WiFi. Restart...\n", ANCHOR_ID);
        ESP.restart();
    }

    Serial.printf("[ANCHOR-%d] WiFi OK — SSID: %s | IP: %s\n",
                  ANCHOR_ID,
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
}

// ─── NVS: Simpan & Load Server IP ────────────────────────────────────────────
// ─── Validasi format IP (xxx.xxx.xxx.xxx) ────────────────────────────────────
bool isValidIP(const String &ip) {
    int dots = 0, start = 0;
    for (int i = 0; i <= (int)ip.length(); i++) {
        if (i == (int)ip.length() || ip[i] == '.') {
            if (i == start) return false;
            int val = ip.substring(start, i).toInt();
            if (val < 0 || val > 255) return false;
            if (i < (int)ip.length()) dots++;
            start = i + 1;
        } else if (!isdigit((unsigned char)ip[i])) {
            return false;
        }
    }
    return dots == 3;
}

void saveServerIP(const char *ip) {
    prefs.begin("anchor-cfg", false);
    prefs.putString("server_ip", ip);
    prefs.end();
}

String loadServerIP() {
    prefs.begin("anchor-cfg", true);
    // Jika NVS kosong, gunakan SERVER_IP_DEFAULT dari lora_config.h
    String ip = prefs.getString("server_ip", SERVER_IP_DEFAULT);
    prefs.end();
    return ip;
}

// ─── Inisialisasi LoRa ───────────────────────────────────────────────────────
void initLoRa() {
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

    if (!LoRa.begin(LORA_FREQ)) {
        Serial.printf("[ANCHOR-%d] ERROR: LoRa init failed!\n", ANCHOR_ID);
        while (true);
    }

    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();

    Serial.printf("[ANCHOR-%d] LoRa OK — SF%d, BW%.0fkHz\n",
                  ANCHOR_ID, LORA_SF, LORA_BW / 1000.0f);
}

// ─── Parse paket LoRa ────────────────────────────────────────────────────────
bool parsePacket(AnchorReport &report) {
    int available = LoRa.available();
    if (available != sizeof(TagPacket)) {
        while (LoRa.available()) LoRa.read();
        Serial.printf("[ANCHOR-%d] WARN: Ukuran paket tidak valid (%d byte).\n",
                      ANCHOR_ID, available);
        return false;
    }

    TagPacket pkt;
    uint8_t  *buf = (uint8_t *)&pkt;
    for (size_t i = 0; i < sizeof(TagPacket); i++) {
        buf[i] = (uint8_t)LoRa.read();
    }

    if (pkt.pkt_type != PKT_TYPE_TAG_BROADCAST) {
        Serial.printf("[ANCHOR-%d] WARN: Tipe paket tidak dikenal (0x%02X).\n",
                      ANCHOR_ID, pkt.pkt_type);
        return false;
    }

    report.pkt_type  = PKT_TYPE_ANCHOR_REPORT;
    report.anchor_id = ANCHOR_ID;
    report.tag_id    = pkt.tag_id;
    report.seq       = pkt.seq;
    report.tag_ts    = pkt.timestamp;
    report.anchor_ts = (uint32_t)millis();
    report.rssi      = (int8_t)LoRa.packetRssi();
    report.snr       = (int8_t)LoRa.packetSnr();

    return true;
}

// ─── Kirim laporan ke server via HTTP POST ───────────────────────────────────
void sendReportToServer(const AnchorReport &report) {
    if (serverIP.length() == 0) {
        Serial.printf("[ANCHOR-%d] WARN: Server IP kosong, paket dibuang.\n", ANCHOR_ID);
        return;
    }

    StaticJsonDocument<200> doc;
    doc["anchor"]    = report.anchor_id;
    doc["tag"]       = report.tag_id;
    doc["seq"]       = report.seq;
    doc["tag_ts"]    = report.tag_ts;
    doc["anchor_ts"] = report.anchor_ts;
    doc["rssi"]      = report.rssi;
    doc["snr"]       = report.snr;
    doc["ax"]        = ANCHOR_X;
    doc["ay"]        = ANCHOR_Y;

    String payload;
    serializeJson(doc, payload);

    String url = String("http://") + serverIP + ":" + SERVER_PORT + SERVER_PATH;
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

    int httpCode = http.POST(payload);

    if (httpCode == 200) {
        Serial.printf("[ANCHOR-%d] OK → Tag=%d | Seq=%d | RSSI=%d dBm | SNR=%d dB\n",
                      report.anchor_id, report.tag_id,
                      report.seq, report.rssi, report.snr);
    } else {
        Serial.printf("[ANCHOR-%d] HTTP ERROR %d — %s\n", ANCHOR_ID, httpCode, url.c_str());
    }

    http.end();
}

// ─── Validasi format IP (xxx.xxx.xxx.xxx) ────────────────────────────────────
// Return true jika IP valid (4 oktet, masing-masing 0-255)