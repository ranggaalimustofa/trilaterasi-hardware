#pragma once

// ─── Pin LoRa (ESP32) ────────────────────────────────────────────────────────
#define LORA_SS 5   // NSS
#define LORA_RST 14 // RST
#define LORA_DIO0 2 // DIO0

// ─── Parameter LoRa ──────────────────────────────────────────────────────────
#define LORA_FREQ 433E6     // Frekuensi: 433E6 / 868E6 / 915E6
#define LORA_SF 7           // Spreading Factor (6–12)
#define LORA_BW 125E3       // Bandwidth (Hz)
#define LORA_TX_POWER 17    // Daya transmisi (dBm)
#define LORA_SYNC_WORD 0x12 // Sync word — harus sama di semua perangkat

// ─── Protokol Paket ──────────────────────────────────────────────────────────
#define PKT_TYPE_TAG_BROADCAST 0x01
#define PKT_TYPE_ANCHOR_REPORT 0x02

// ─── Serial ──────────────────────────────────────────────────────────────────
#define SERIAL_BAUD 115200
