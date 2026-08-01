/**
 * ESP32 Aquaponics LoRa Gateway Firmware (Pengganti Raspberry Pi)
 * 
 * Fungsi:
 * 1. Menerima data sensor via LoRa Nirkabel dari ESP32 #1 (Transmitter di Kolam)
 * 2. Terhubung ke WiFi (thecia99)
 * 3. Mengunggah data sensor ke Firebase Realtime Database Cloud secara real-time
 * 4. Membaca instruksi kontrol tombol dari Firebase dan memancarkannya via LoRa ke ESP32 #1
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "LoRa_E220.h"

// --- KONFIGURASI WIFI & FIREBASE ---
const char* WIFI_SSID = "thecia99";
const char* WIFI_PASS = "Rumahorens1969";
const char* FIREBASE_URL = "https://akuaponik-iot-default-rtdb.asia-southeast1.firebasedatabase.app";

// --- PINOUT LORA E220 / E32 PADA ESP32 #2 ---
#define E220_RX_PIN  16   // ESP32 RX2 connected to LoRa TXD
#define E220_TX_PIN  17   // ESP32 TX2 connected to LoRa RXD
#define E220_AUX_PIN 14   // LoRa AUX
#define E220_M0_PIN  5    // LoRa M0
#define E220_M1_PIN  2    // LoRa M1

// --- INITIALIZATION ---
LoRa_E220 e220(&Serial2, E220_AUX_PIN, E220_M0_PIN, E220_M1_PIN);

unsigned long lastFirebasePollTime = 0;
const unsigned long POLL_INTERVAL = 2000; // Cek perintah dari Firebase setiap 2 detik

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
  
  HTTPClient http;
  String fullUrl = String(FIREBASE_URL) + "/sensor_data.json";
  http.begin(fullUrl);
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode = http.PUT(jsonPayload);
  if (httpResponseCode > 0) {
    Serial.printf("[Firebase] Telemetry berhasil diunggah! Code: %d\n", httpResponseCode);
  } else {
    Serial.printf("[Firebase Error] Gagal mengunggah telemetry: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  http.end();
}

void checkFirebaseControls() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  String fullUrl = String(FIREBASE_URL) + "/control.json";
  http.begin(fullUrl);
  
  int httpResponseCode = http.GET();
  if (httpResponseCode == 200) {
    String responseStr = http.getString();
    // Jika ada data perintah kontrol dari Firebase, teruskan via LoRa ke ESP32 #1
    if (responseStr.length() > 2 && responseStr != "null") {
      ResponseStatus status = e220.sendMessage(responseStr);
      if (status.code == 1) {
        Serial.println("[LoRa TX] Perintah kontrol Firebase berhasil dipancarkan ke ESP32 #1!");
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
  Serial.println("[LoRa Gateway] Modul LoRa E220 Receiver Aktif.");

  // Hubungkan ke WiFi
  connectWiFi();
}

void loop() {
  // 1. Terima Paket Data Telemetry LoRa dari ESP32 #1 di Kolam
  if (e220.available() > 0) {
    ResponseContainer response = e220.receiveMessage();
    if (response.status.code == 1) {
      String incomingMsg = response.data;
      Serial.println("\n[LoRa RX] Menerima data sensor dari ESP32 #1:");
      Serial.println(incomingMsg);

      // Verifikasi format JSON valid sebelum diunggah ke Firebase
      DynamicJsonDocument doc(1024);
      DeserializationError err = deserializeJson(doc, incomingMsg);
      if (!err) {
        // Tambahkan timestamp dan status Aktif
        doc["status"] = "Aktif";
        doc["timestamp"] = millis();
        
        String outputJson;
        serializeJson(doc, outputJson);
        
        // Unggah langsung ke Firebase Realtime Database
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
