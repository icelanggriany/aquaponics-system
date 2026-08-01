/**
 * ESP32 Aquaponics Transmitter Firmware
 * 
 * Membaca data sensor (Suhu Air DS18B20, TDS Analog, Kelembapan/Suhu Udara DHT22)
 * Menampilkan data ke LCD I2C
 * Mengirimkan data sensor ke Raspberry Pi via LoRa
 * Menerima perintah kontrol (Pompa, Lampu, Feeder) dari Raspberry Pi via LoRa
 */

#include "LoRa_E220.h"
#include <Wire.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>

// --- PINOUT DEFINITION ---
// E220 UART LoRa Module
#define E220_RX_PIN  16   // ESP32 RX2 connected to E220 TXD
#define E220_TX_PIN  17   // ESP32 TX2 connected to E220 RXD
#define E220_AUX_PIN 14   // E220 AUX
#define E220_M0_PIN  5    // E220 M0
#define E220_M1_PIN  2    // E220 M1

// Sensors
#define DHT_PIN    15
#define DHT_TYPE   DHT11     // Diubah ke DHT11 sesuai request user
#define ONE_WIRE_BUS 4       // DS18B20 Water Temp (Butuh Resistor 4.7k ke 3.3V)
#define TDS_PIN    34        // Analog TDS Sensor (ADC1_CH6)
#define TRIG_PIN 32          // Sensor Ultrasonik AJ-SR04M Trig Pin
#define ECHO_PIN 33          // Sensor Ultrasonik AJ-SR04M Echo Pin

// Relays / Actuators (6 Channels)
#define RELAY_LAMPU         27   // Relay 1
#define RELAY_PEMBESARAN    12   // Relay 2 (sebelumnya RELAY_POMPA)
#define RELAY_PEREMAJAAN    25   // Relay 3
#define RELAY_SIRKULASI     26   // Relay 4 (sebelumnya RELAY_DINAMO)
#define RELAY_AERATOR       23   // Relay 5
#define RELAY_FEEDER        13   // Relay 6

// Modul relay yang dipakai bersifat active-low:
// LOW menyalakan relay dan HIGH mematikan relay.
const uint8_t RELAY_ON = LOW;
const uint8_t RELAY_OFF = HIGH;

// --- SETTINGS ---
#define LORA_CHANNEL  65     // Channel 65 = 915.125 MHz
#define SEND_INTERVAL 5000   // Interval pengiriman data sensor (5 detik)

// --- OBJECT INITIALIZATION ---
DHT dht(DHT_PIN, DHT_TYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// LoRa E220 using HardwareSerial2 (Serial2)
LoRa_E220 e220(&Serial2, E220_AUX_PIN, E220_M0_PIN, E220_M1_PIN);

// LCD I2C 16x2
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- GLOBAL VARIABLES ---
float waterTemp = 0.0;
float tdsValue = 0.0;
float airTemp = 0.0;
float airHum = 0.0;
float waterLevel = 0.0;      // Ketinggian Air (0.0% - 100.0%)

bool statusLampu = false;
bool statusPembesaran = false;
bool statusPeremajaan = false;
bool statusSirkulasi = false;
bool statusAerator = false;
bool statusFeeder = false;
unsigned long lastSendTime = 0;
unsigned long lastLcdUpdateTime = 0;
unsigned long lastPiCommTime = 0; // Waktu terakhir menerima data dari Pi
bool isSystemOffline = false;      // Status koneksi ke Pi
int lcdScreen = 0;
int lastFeedConfirm = 0;

// Membaca sensor TDS Analog dengan kompensasi suhu air (referensi 25 C)
float readTDS(float temperature) {
  const int NUM_TDS_SAMPLES = 30;
  uint32_t totalMilliVolts = 0;
  for (int i = 0; i < NUM_TDS_SAMPLES; i++) {
    totalMilliVolts += analogReadMilliVolts(TDS_PIN);
    delay(10);
  }
  float voltage = (totalMilliVolts / (float)NUM_TDS_SAMPLES) / 1000.0;
  int rawAdc = analogRead(TDS_PIN);
  
  // Debug print untuk melacak tegangan fisik yang masuk ke pin GPIO 34
  Serial.printf("[TDS Debug] Raw ADC: %d | Voltage: %.3f V\n", rawAdc, voltage);

  if (rawAdc <= 5 || voltage <= 0.005) {
    Serial.println("[TDS Warning] Tidak ada tegangan pada GPIO 34. Periksa VCC, GND, dan kabel A/O sensor.");
    return 0.0;
  }
  
  // Kompensasi suhu air (TDS sangat dipengaruhi suhu)
  if (temperature < 0.0 || temperature > 60.0) temperature = 25.0;
  float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
  float compensatedVoltage = voltage / compensationCoefficient;
  
  // Rumus konversi tegangan ke TDS dalam ppm (sensor TDS DFRobot standar)
  float tds = (133.33 * compensatedVoltage * compensatedVoltage * compensatedVoltage 
               - 255.86 * compensatedVoltage * compensatedVoltage 
               + 857.39 * compensatedVoltage) * 0.5;
               
  if (tds < 0.0) tds = 0.0;
  return tds;
}

// Membaca level air kolam menggunakan sensor ultrasonik AJ-SR04M
float readWaterLevel() {
  float totalLevel = 0.0;
  int validReadings = 0;
  
  for (int i = 0; i < 3; i++) {
    // Kirim pulsa trigger
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    // Baca durasi pulsa echo (timeout diatur ke 30000 µs atau sekitar 5 meter)
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    
    if (duration > 0) {
      // Hitung jarak (cm)
      float distance = duration * 0.0343 / 2.0;
      
      float maxDistance = 170.0; // cm
      float minDistance = 20.0;  // cm
      
      float level = ((maxDistance - distance) / (maxDistance - minDistance)) * 100.0;
      
      if (level < 0.0) level = 0.0;
      if (level > 100.0) level = 100.0;
      
      totalLevel += level;
      validReadings++;
    }
    delay(5); // Jeda singkat untuk gema
  }
  
  // Jika timeout atau semua pembacaan gagal
  if (validReadings == 0) {
    Serial.println("Error: Sensor AJ-SR04M tidak merespon / di luar jangkauan!");
    return -1.0;
  }
  
  return totalLevel / validReadings;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("--- Aquaponics ESP32 Node Setup ---");

  // Initialize Relays (6 Channels)
  pinMode(RELAY_LAMPU, OUTPUT);
  pinMode(RELAY_PEMBESARAN, OUTPUT);
  pinMode(RELAY_PEREMAJAAN, OUTPUT);
  pinMode(RELAY_SIRKULASI, OUTPUT);
  pinMode(RELAY_AERATOR, OUTPUT);
  pinMode(RELAY_FEEDER, OUTPUT);

  // Pastikan semua relay mati saat ESP32 mulai.
  digitalWrite(RELAY_LAMPU, RELAY_OFF);
  digitalWrite(RELAY_PEMBESARAN, RELAY_OFF);
  digitalWrite(RELAY_PEREMAJAAN, RELAY_OFF);
  digitalWrite(RELAY_SIRKULASI, RELAY_OFF);
  digitalWrite(RELAY_AERATOR, RELAY_OFF);
  digitalWrite(RELAY_FEEDER, RELAY_OFF);

  // Initialize Sensors
  dht.begin();
  sensors.begin();
  sensors.setWaitForConversion(false); // Non-blocking temperature read
  sensors.requestTemperatures();       // Trigger first conversion
  pinMode(TDS_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW); // Pastikan trig pin dimulai dari LOW

  // Initialize I2C if needed
  Wire.begin(21, 22); // SDA=21, SCL=22

  // Initialize LCD I2C
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Aquaponic");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  // Initialize LoRa E220 (UART)
  Serial2.begin(9600, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  e220.begin();

  // Configure E220 module parameters
  ResponseStructContainer rsc = e220.getConfiguration();
  if (rsc.status.code != 1) {
    Serial.print("Warning: Gagal membaca E220 config. Code: ");
    Serial.println(rsc.status.code);
  } else {
    Configuration configuration = *(Configuration*)rsc.data;
    configuration.ADDH = 0x00;
    configuration.ADDL = 0x00;
    configuration.CHAN = LORA_CHANNEL; // Set Channel to 65 (915.125 MHz)
    
    configuration.SPED.uartBaudRate = UART_BPS_9600;
    configuration.SPED.uartParity = MODE_00_8N1;
    configuration.SPED.airDataRate = AIR_DATA_RATE_010_24;
    
    configuration.OPTION.subPacketSetting = SPS_200_00;
    configuration.OPTION.RSSIAmbientNoise = RSSI_AMBIENT_NOISE_DISABLED;
    configuration.OPTION.transmissionPower = POWER_22;
    
    configuration.TRANSMISSION_MODE.fixedTransmission = FT_TRANSPARENT_TRANSMISSION;
    
    ResponseStatus rs = e220.setConfiguration(configuration, WRITE_CFG_PWR_DWN_SAVE);
    if (rs.code != 1) {
      Serial.print("Warning: Gagal menyimpan E220 config. Code: ");
      Serial.println(rs.code);
    }
  }
  rsc.close();

  Serial.println("LoRa E220 Initialized OK.");
  lastPiCommTime = millis();
}

void loop() {
  // 1. Mengirimkan Data Sensor berkala (setiap SEND_INTERVAL / 5 detik)
  if (millis() - lastSendTime >= SEND_INTERVAL) {
    sendSensorData();
    lastSendTime = millis();
  }

  // 2. Menerima Perintah dari Raspberry Pi
  if (e220.available() > 0) {
    receiveCommand();
  }

  // 3. Update Tampilan LCD bergantian (setiap 2.5 detik)
  if (millis() - lastLcdUpdateTime >= 2500) {
    updateLcdDisplay();
    lastLcdUpdateTime = millis();
  }

  // 4. Deteksi Offline (Hanya untuk Tampilan LCD Status)
  if (millis() - lastPiCommTime > 60000) {
    if (!isSystemOffline) {
      Serial.println("[Offline] Terputus dari Raspberry Pi!");
      isSystemOffline = true;
    }
  }
}



void readSensors() {
  // Read DS18B20 Water Temperature (non-blocking)
  float tempWaterRead = sensors.getTempCByIndex(0);
  if (tempWaterRead != DEVICE_DISCONNECTED_C && tempWaterRead > -50.0) {
    waterTemp = tempWaterRead;
  }
  sensors.requestTemperatures(); // Minta konversi untuk loop berikutnya

  // Read DHT11 Air Temp & Humidity
  float tempAirRead = dht.readTemperature();
  float humAirRead = dht.readHumidity();
  if (!isnan(tempAirRead) && !isnan(humAirRead)) {
    airTemp = tempAirRead;
    airHum = humAirRead;
  } else {
    Serial.println("Error: DHT11 sensor gagal membaca (menggunakan nilai sebelumnya)!");
  }

  // Read TDS
  tdsValue = readTDS(waterTemp);

  // Read Water Level
  float tempWaterLevel = readWaterLevel();
  if (tempWaterLevel >= 0.0) {
    waterLevel = tempWaterLevel;
  } else {
    Serial.println("Error: AJ-SR04M gagal membaca (menggunakan nilai sebelumnya)!");
  }

  // Print to Serial Monitor
  Serial.printf("Sensors -> WaterTemp: %.2fC | TDS: %.1f ppm | AirTemp: %.2fC | Hum: %.2f%% | WaterLevel: %.1f%%\n",
                waterTemp, tdsValue, airTemp, airHum, waterLevel);
}

void sendSensorData() {
  readSensors(); // Ambil data sensor terbaru secara real-time sebelum dikirim
  
  // Menggunakan JSON untuk pengiriman data agar mudah di-parse di RPi
  StaticJsonDocument<384> doc;
  doc["type"] = "sensor_data";
  doc["temp_w"] = round(waterTemp * 10) / 10.0;
  doc["tds"] = round(tdsValue);
  doc["temp_a"] = round(airTemp * 10) / 10.0;
  doc["hum"] = (int)airHum;
  doc["water_level"] = round(waterLevel * 10) / 10.0;
  
  // Status 6 Actuators
  doc["lamp"] = statusLampu ? 1 : 0;
  doc["pump_b"] = statusPembesaran ? 1 : 0;
  doc["pump_p"] = statusPeremajaan ? 1 : 0;
  doc["pump_s"] = statusSirkulasi ? 1 : 0;
  doc["aerator"] = statusAerator ? 1 : 0;
  doc["feeder"] = statusFeeder ? 1 : 0;
  
  doc["feed_confirm"] = lastFeedConfirm;
  lastFeedConfirm = 0;

  String payload;
  serializeJson(doc, payload);

  // Kirim data via LoRa E220
  ResponseStatus rs = e220.sendMessage(payload);
  if (rs.code != 1) {
    Serial.print("LoRa send failed! Code: ");
    Serial.println(rs.code);
  }

  Serial.print("Sent LoRa Packet: ");
  Serial.println(payload);
}

void receiveCommand() {
  ResponseContainer rc = e220.receiveMessage();
  if (rc.status.code != 1) {
    Serial.print("LoRa receive failed! Code: ");
    Serial.println(rc.status.code);
    return;
  }

  String incoming = rc.data;

  Serial.print("Received LoRa Packet: ");
  Serial.println(incoming);

  StaticJsonDocument<384> doc;
  DeserializationError error = deserializeJson(doc, incoming);

  if (error) {
    Serial.print("JSON Deserialization failed: ");
    Serial.println(error.c_str());
    return;
  }

  // Cek jika ini adalah paket bertipe "control"
  if (doc.containsKey("type") && strcmp(doc["type"], "control") == 0) {
    lastPiCommTime = millis();
    isSystemOffline = false;
    if (doc.containsKey("lamp")) {
      statusLampu = (doc["lamp"] == 1);
      digitalWrite(RELAY_LAMPU, statusLampu ? RELAY_ON : RELAY_OFF);
      Serial.printf("Command Lampu: %s\n", statusLampu ? "ON" : "OFF");
    }
    if (doc.containsKey("pump_b")) {
      statusPembesaran = (doc["pump_b"] == 1);
      digitalWrite(RELAY_PEMBESARAN, statusPembesaran ? RELAY_ON : RELAY_OFF);
      Serial.printf("Command Pompa Pembesaran: %s\n", statusPembesaran ? "ON" : "OFF");
    }
    if (doc.containsKey("pump_p")) {
      statusPeremajaan = (doc["pump_p"] == 1);
      digitalWrite(RELAY_PEREMAJAAN, statusPeremajaan ? RELAY_ON : RELAY_OFF);
      Serial.printf("Command Pompa Peremajaan: %s\n", statusPeremajaan ? "ON" : "OFF");
    }
    if (doc.containsKey("pump_s")) {
      statusSirkulasi = (doc["pump_s"] == 1);
      digitalWrite(RELAY_SIRKULASI, statusSirkulasi ? RELAY_ON : RELAY_OFF);
      Serial.printf("Command Pompa Sirkulasi: %s\n", statusSirkulasi ? "ON" : "OFF");
    }
    if (doc.containsKey("aerator")) {
      statusAerator = (doc["aerator"] == 1);
      digitalWrite(RELAY_AERATOR, statusAerator ? RELAY_ON : RELAY_OFF);
      Serial.printf("Command Aerator: %s\n", statusAerator ? "ON" : "OFF");
    }
    if (doc.containsKey("feed")) {
      int porsi = doc["feed"];
      triggerFeeder(porsi);
    }
    
    // Kirim balik status terbaru secara instan sebagai konfirmasi
    sendSensorData();
  }
}

void triggerFeeder(int porsi) {
  Serial.printf("Mengaktifkan Feeder Pakan sebanyak %d porsi...\n", porsi);
  statusFeeder = true;
  sendSensorData();

  // Nyalakan relay feeder pakan (motor/servo)
  digitalWrite(RELAY_FEEDER, RELAY_ON);
  
  // Nyalakan selama 5 detik per porsi pakan (dinamo menyala 5 detik)
  delay(5000 * porsi);
  
  digitalWrite(RELAY_FEEDER, RELAY_OFF);
  statusFeeder = false;
  Serial.println("Feeder Selesai.");
  lastFeedConfirm = porsi;
}

void updateLcdDisplay() {
  lcd.clear();
  switch (lcdScreen) {
    case 0:
      // Screen 1: Water Temp & TDS
      lcd.setCursor(0, 0);
      lcd.print("Suhu Air: ");
      lcd.print(waterTemp, 1);
      lcd.print(" C");
      
      lcd.setCursor(0, 1);
      lcd.print("TDS Air : ");
      lcd.print(round(tdsValue));
      lcd.print(" ppm");
      
      lcdScreen = 1;
      break;
    case 1:
      // Screen 2: Air Temp & Humidity
      lcd.setCursor(0, 0);
      lcd.print("Suhu Udr: ");
      lcd.print(airTemp, 1);
      lcd.print(" C");
      
      lcd.setCursor(0, 1);
      lcd.print("Kelembap: ");
      lcd.print(round(airHum));
      lcd.print(" %");
      
      lcdScreen = 2;
      break;
    case 2:
      // Screen 3: Water Level & Connection Status
      lcd.setCursor(0, 0);
      lcd.print("Tinggi Air: ");
      lcd.print(waterLevel, 1);
      lcd.print(" %");
      
      lcd.setCursor(0, 1);
      if (isSystemOffline) {
        lcd.print("Koneksi: OFFLINE");
      } else {
        lcd.print("Koneksi: ONLINE ");
      }
      
      lcdScreen = 0;
      break;
  }
}
