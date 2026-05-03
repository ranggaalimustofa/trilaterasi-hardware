#pragma once

// ─── Pin LoRa (ESP32) ────────────────────────────────────────────────────────
#define LORA_SS    5    // NSS
#define LORA_RST   14   // RST
#define LORA_DIO0  2    // DIO0

// ─── Parameter LoRa ──────────────────────────────────────────────────────────
#define LORA_FREQ        433E6   // Frekuensi: 433E6 / 868E6 / 915E6
#define LORA_SF          7       // Spreading Factor (6–12)
#define LORA_BW          125E3   // Bandwidth (Hz)
#define LORA_TX_POWER    17      // Daya transmisi (dBm)
#define LORA_SYNC_WORD   0x12    // Sync word — harus sama di semua perangkat

// ─── Protokol Paket ──────────────────────────────────────────────────────────
#define PKT_TYPE_TAG_BROADCAST  0x01
#define PKT_TYPE_ANCHOR_REPORT  0x02

// ─── Serial ──────────────────────────────────────────────────────────────────
#define SERIAL_BAUD  115200

// ─── WiFiManager ─────────────────────────────────────────────────────────────
#define WIFI_AP_PASSWORD  "anchor1234"  // Password portal konfigurasi WiFi
#define WIFI_TIMEOUT_SEC  180           // Timeout portal (detik), 0 = tunggu selamanya

// ─── Tombol BOOT (GPIO0) — satu tombol dua fungsi ────────────────────────────
#define RESET_BUTTON_PIN     0
#define HOLD_PORTAL_IP_MS    3000   // 3 detik → portal Server IP
#define HOLD_RESET_TOTAL_MS  6000   // 6 detik → reset total

// ─── HTTP Server ─────────────────────────────────────────────────────────────
// Jika user tidak mengisi Server IP di portal, IP ini yang digunakan.
// Ganti sesuai IP PC/laptop Anda.
#define SERVER_IP_DEFAULT  "192.168.1.194"
#define SERVER_PORT        5000
#define SERVER_PATH        "/api/anchor-report"
#define HTTP_TIMEOUT_MS    3000