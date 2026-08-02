/**
 * ESP32 Aquaponics LoRa Gateway Firmware (Pengganti Raspberry Pi) - CHANNEL 65 FIX
 * 
 * Fungsi:
 * 1. Menerima data sensor via LoRa Nirkabel dari ESP32 #1 (Transmitter di Kolam)
 * 2. Terhubung ke WiFi (thecia99)
 * 3. Mengunggah data sensor ke Firebase Realtime Database Cloud secara HTTPS SSL real-time
 * 4. Membaca instruksi kontrol tombol baru dari Firebase dan memancarkannya via LoRa ke ESP32 #1
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "LoRa_E220.h"

// --- KONFIGURASI WIFI & FIREBASE ---
const char* WIFI_SSID = "thecia99";
const char* WIFI_PASS = "Rumahorens1969";
const char* FIREBASE_URL = "https://aquaponics-system-8d6f6-default-rtdb.asia-southeast1.firebasedatabase.app";

// --- PINOUT LORA E220 / E32 PADA ESP32 #2 ---
#define E220_RX_PIN  16   // ESP32 RX2 connected to LoRa TXD
#define E220_TX_PIN  17   // ESP32 TX2 connected to LoRa RXD
#define E220_AUX_PIN 14   // LoRa AUX
#define E220_M0_PIN  5    // LoRa M0
#define E220_M1_PIN  2    // LoRa M1

#define LORA_CHANNEL 65   // Channel 65 = 915.125 MHz (Sama persis dengan ESP32 #1)

// --- INITIALIZATION ---
LoRa_E220 e220(&Serial2, E220_AUX_PIN, E220_M0_PIN, E220_M1_PIN);

unsigned long lastFirebasePollTime = 0;
const unsigned long POLL_INTERVAL = 1500; // Cek perintah dari Firebase setiap 1.5 detik
String lastControlPayload = "";

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Terhubung! Alamat IP ESP32 Gateway: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi Warning] Belum terhubung, akan dicoba lagi...");
  }
}

void uploadTelemetryToFirebase(String jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }
  
  WiFiClientSecure client;
  client.setInsecure(); // SSL Bypass untuk Firebase REST API

  HTTPClient http;
  String fullUrl = String(FIREBASE_URL) + "/sensor_data.json";
  http.begin(client, fullUrl);
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode = http.PUT(jsonPayload);
  if (httpResponseCode > 0) {
    Serial.printf("[Firebase] Telemetry sensor BERHASIL diunggah ke Cloud! Code HTTP: %d\n", httpResponseCode);
  } else {
    Serial.printf("[Firebase Error] Gagal mengunggah: %d (%s)\n", httpResponseCode, http.errorToString(httpResponseCode).c_str());
  }
  http.end();
}

void checkFirebaseControls() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String fullUrl = String(FIREBASE_URL) + "/control.json";
  http.begin(client, fullUrl);
  
  int httpResponseCode = http.GET();
  if (httpResponseCode == 200) {
    String responseStr = http.getString();
    // Hanya pancarkan via LoRa jika ada PERUBAHAN tombol kontrol baru dari Web
    if (responseStr.length() > 2 && responseStr != "null" && responseStr != lastControlPayload) {
      lastControlPayload = responseStr;
      ResponseStatus status = e220.sendMessage(responseStr);
      if (status.code == 1) {
        Serial.println("[LoRa TX] Perintah kontrol BARU dari Web berhasil dipancarkan ke ESP32 #1!");
      }
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=============================================");
  Serial.println("   ESP32 AQUAPONICS LORA GATEWAY STARTING    ");
  Serial.println("=============================================");

  // Inisialisasi LoRa E220 Serial2 (Pin 16 RX, Pin 17 TX)
  Serial2.begin(9600, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  e220.begin();

  // Konfigurasi Frekuensi LoRa ke Channel 65 (915.125 MHz)
  ResponseStructContainer rsc = e220.getConfiguration();
  if (rsc.status.code == 1) {
    Configuration configuration = *(Configuration*)rsc.data;
    configuration.ADDH = 0x00;
    configuration.ADDL = 0x00;
    configuration.CHAN = LORA_CHANNEL; // Set Channel 65 (915.125 MHz)
    configuration.SPED.uartBaudRate = UART_BPS_9600;
    configuration.SPED.uartParity = MODE_00_8N1;
    configuration.SPED.airDataRate = AIR_DATA_RATE_010_24;
    configuration.OPTION.subPacketSetting = SPS_200_00;
    configuration.OPTION.transmissionPower = POWER_22;
    configuration.TRANSMISSION_MODE.fixedTransmission = FT_TRANSPARENT_TRANSMISSION;
    e220.setConfiguration(configuration, WRITE_CFG_PWR_DWN_SAVE);
    Serial.println("[LoRa Gateway] Frekuensi LoRa disamakan ke Channel 65 (915 MHz) OK.");
  }
  rsc.close();

  // Hubungkan ke WiFi
  connectWiFi();
}

void loop() {
  // 1. Menerima Paket Data Telemetry LoRa dari ESP32 #1 di Kolam secara Real-Time
  if (e220.available() > 0) {
    ResponseContainer response = e220.receiveMessage();
    if (response.status.code == 1) {
      String incomingMsg = response.data;
      Serial.println("\n[LoRa RX REALTIME] Menerima data sensor baru dari ESP32 #1:");
      Serial.println(incomingMsg);

      // Verifikasi format JSON valid sebelum diunggah ke Firebase
      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, incomingMsg);
      if (!err) {
        doc["status"] = "Aktif";
        
        String outputJson;
        serializeJson(doc, outputJson);
        
        // Unggah langsung ke Firebase Realtime Database secara SSL Secure
        uploadTelemetryToFirebase(outputJson);
      } else {
        Serial.println("[JSON Error] Data LoRa yang diterima cacat/tidak lengkap.");
      }
    }
  }

  // 2. Cek Perintah Kontrol dari Firebase untuk Dipancarkan Balik via LoRa ke ESP32 #1
  if (millis() - lastFirebasePollTime >= POLL_INTERVAL) {
    lastFirebasePollTime = millis();
    checkFirebaseControls();
  }

  // Pastikan WiFi tetap terhubung
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
}
