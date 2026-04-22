#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "lora_config.h"
#include "packet.h"

// ─── Konfigurasi Anchor ───────────────────────────────────────────────────────
#ifndef ANCHOR_ID
  #define ANCHOR_ID  1
#endif
#ifndef ANCHOR_X
  #define ANCHOR_X   0.0f
#endif
#ifndef ANCHOR_Y
  #define ANCHOR_Y   0.0f
#endif

// ─── Prototypes ──────────────────────────────────────────────────────────────
void     initLoRa();
bool     parsePacket(AnchorReport &report);
void     sendReportToServer(const AnchorReport &report);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial);

    Serial.printf("[ANCHOR-%d] Booting...\n", ANCHOR_ID);
    initLoRa();

    // TIDAK menggunakan LoRa.onReceive() — polling di loop() lebih stabil
    Serial.printf("[ANCHOR-%d] Ready. Polling on %.0f MHz\n",
                  ANCHOR_ID, (float)LORA_FREQ);
}

void loop() {
    // LoRa.parsePacket() mengembalikan ukuran paket jika ada, 0 jika tidak ada
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) return;

    AnchorReport report;
    if (parsePacket(report)) {
        sendReportToServer(report);
    }
}

// ─── Inisialisasi LoRa ───────────────────────────────────────────────────────
void initLoRa() {
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

    if (!LoRa.begin(LORA_FREQ)) {
        Serial.printf("[ANCHOR-%d] ERROR: LoRa init failed! Halting.\n", ANCHOR_ID);
        while (true);
    }

    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setTxPower(LORA_TX_POWER);
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.enableCrc();

    Serial.printf("[ANCHOR-%d] LoRa OK — SF%d, BW%.0fkHz, SyncWord=0x%02X\n",
                  ANCHOR_ID, LORA_SF, LORA_BW / 1000.0f, LORA_SYNC_WORD);
}

// ─── Parse paket LoRa yang masuk ─────────────────────────────────────────────
// Dipanggil dari loop() — sepenuhnya aman, tidak ada ISR
// Return true jika paket valid, false jika diabaikan
bool parsePacket(AnchorReport &report) {
    // Validasi ukuran
    int available = LoRa.available();
    if (available != sizeof(TagPacket)) {
        while (LoRa.available()) LoRa.read();
        Serial.printf("[ANCHOR-%d] WARN: Ukuran paket tidak valid (%d byte). Diabaikan.\n",
                      ANCHOR_ID, available);
        return false;
    }

    // Baca ke struct TagPacket
    TagPacket pkt;
    uint8_t  *buf = (uint8_t *)&pkt;
    for (size_t i = 0; i < sizeof(TagPacket); i++) {
        buf[i] = (uint8_t)LoRa.read();
    }

    // Validasi tipe paket
    if (pkt.pkt_type != PKT_TYPE_TAG_BROADCAST) {
        Serial.printf("[ANCHOR-%d] WARN: Tipe paket tidak dikenal (0x%02X). Diabaikan.\n",
                      ANCHOR_ID, pkt.pkt_type);
        return false;
    }

    // Ambil RSSI & SNR — harus dilakukan segera setelah parsePacket()
    int16_t rssi = LoRa.packetRssi();
    float   snr  = LoRa.packetSnr();

    // Isi struct report
    report.pkt_type  = PKT_TYPE_ANCHOR_REPORT;
    report.anchor_id = ANCHOR_ID;
    report.tag_id    = pkt.tag_id;
    report.seq       = pkt.seq;
    report.tag_ts    = pkt.timestamp;
    report.anchor_ts = (uint32_t)millis();
    report.rssi      = (int8_t)rssi;
    report.snr       = (int8_t)snr;

    return true;
}

// ─── Kirim laporan ke server via Serial JSON ─────────────────────────────────
// Format: {"anchor":1,"tag":1,"seq":42,"tag_ts":12345,"anchor_ts":12350,"rssi":-72,"snr":8,"ax":0.0,"ay":0.0}
void sendReportToServer(const AnchorReport &report) {
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

    serializeJson(doc, Serial);
    Serial.println();

    Serial.printf("[ANCHOR-%d] Tag=%d | Seq=%d | RSSI=%d dBm | SNR=%d dB\n",
                  report.anchor_id, report.tag_id,
                  report.seq, report.rssi, report.snr);
}
