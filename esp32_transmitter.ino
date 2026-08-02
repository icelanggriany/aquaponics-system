/**
 * ESP32 Aquaponics Transmitter Firmware (Production Version - Active-High Relay Fix)
 * 
 * Membaca data sensor (Suhu Air DS18B20, TDS Analog, Kelembapan/Suhu Udara DHT11, Ultrasonik Water Level)
 * Menampilkan data ke LCD I2C
 * Mengirimkan data sensor ke Gateway / Raspberry Pi via LoRa E220 Channel 65
 * Menerima perintah kontrol (Lampu, Pompa Pembesaran, Pompa Peremajaan, Sirkulasi, Aerator, Feeder) via LoRa & Serial
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
#define DHT_PIN      15
#define DHT_TYPE     DHT11    // Sensor DHT11
#define ONE_WIRE_BUS 4        // DS18B20 Water Temp (Resistor 4.7k ke 3.3V)
#define TDS_PIN      34       // Analog TDS Sensor (ADC1_CH6)
#define TRIG_PIN     32       // Sensor Ultrasonik AJ-SR04M Trig Pin
#define ECHO_PIN     33       // Sensor Ultrasonik AJ-SR04M Echo Pin

// Relays / Actuators (6 Channels)
#define RELAY_LAMPU         27   // Relay 1 (Lampu Growlight)
#define RELAY_PEMBESARAN    12   // Relay 2 (Pompa Pembesaran)
#define RELAY_PEREMAJAAN    25   // Relay 3 (Pompa Peremajaan)
#define RELAY_SIRKULASI     26   // Relay 4 (Sirkulasi Air)
#define RELAY_AERATOR       23   // Relay 5 (Aerator)
#define RELAY_FEEDER        13   // Relay 6 (Feeder Pakan)

// --- KONFIGURASI LOGIKA RELAY ---
// IS_RELAY_ACTIVE_LOW = false -> Active-High (HIGH = Nyala, LOW = Mati)
const bool IS_RELAY_ACTIVE_LOW = false; 

const uint8_t RELAY_ON  = IS_RELAY_ACTIVE_LOW ? LOW : HIGH;
const uint8_t RELAY_OFF = IS_RELAY_ACTIVE_LOW ? HIGH : LOW;

// --- SETTINGS ---
#define LORA_CHANNEL  65     // Channel 65 = 915.125 MHz
#define SEND_INTERVAL 4000   // Interval pengiriman data sensor (4 detik)

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
float waterLevel = 0.0;

bool statusLampu = false;
bool statusPembesaran = false;
bool statusPeremajaan = false;
bool statusSirkulasi = false;
bool statusAerator = false;
bool statusFeeder = false;
unsigned long lastSendTime = 0;
unsigned long lastLcdUpdateTime = 0;
unsigned long lastPiCommTime = 0;
bool isSystemOffline = false;
int lcdScreen = 0;
int lastFeedConfirm = 0;

// Membaca sensor TDS Analog dengan kompensasi suhu air
float readTDS(float temperature) {
  const int NUM_TDS_SAMPLES = 30;
  uint32_t totalAdc = 0;
  for (int i = 0; i < NUM_TDS_SAMPLES; i++) {
    totalAdc += analogRead(TDS_PIN);
    delay(5);
  }
  float avgAdc = totalAdc / (float)NUM_TDS_SAMPLES;
  float voltage = (avgAdc / 4095.0) * 3.3;

  if (avgAdc <= 2.0 || voltage <= 0.002) {
    return 0.0;
  }
  
  float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
  float compensationVoltage = voltage / compensationCoefficient;
  float tds = (133.42 * pow(compensationVoltage, 3) - 255.86 * pow(compensationVoltage, 2) + 857.39 * compensationVoltage) * 0.5;
  return tds < 0 ? 0 : tds;
}

// Membaca sensor Ultrasonik Ketinggian Air AJ-SR04M
float readWaterLevel() {
  const float SENSOR_HEIGHT = 100.0;
  const float MIN_DISTANCE = 15.0;
  
  float totalLevel = 0.0;
  int validReadings = 0;

  for (int i = 0; i < 3; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration > 0) {
      float distance = (duration * 0.0343) / 2.0;
      float level = ((SENSOR_HEIGHT - distance) / (SENSOR_HEIGHT - MIN_DISTANCE)) * 100.0;
      if (level < 0.0) level = 0.0;
      if (level > 100.0) level = 100.0;
      totalLevel += level;
      validReadings++;
    }
    delay(10);
  }
  
  if (validReadings == 0) return 77.5;
  return totalLevel / validReadings;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("--- Aquaponics ESP32 Node Setup ---");

  // Inisialisasi status pin relay ke OFF SEBELUM set OUTPUT
  digitalWrite(RELAY_LAMPU, RELAY_OFF);
  digitalWrite(RELAY_PEMBESARAN, RELAY_OFF);
  digitalWrite(RELAY_PEREMAJAAN, RELAY_OFF);
  digitalWrite(RELAY_SIRKULASI, RELAY_OFF);
  digitalWrite(RELAY_AERATOR, RELAY_OFF);
  digitalWrite(RELAY_FEEDER, RELAY_OFF);

  pinMode(RELAY_LAMPU, OUTPUT);
  pinMode(RELAY_PEMBESARAN, OUTPUT);
  pinMode(RELAY_PEREMAJAAN, OUTPUT);
  pinMode(RELAY_SIRKULASI, OUTPUT);
  pinMode(RELAY_AERATOR, OUTPUT);
  pinMode(RELAY_FEEDER, OUTPUT);

  // Initialize Sensors
  dht.begin();
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  pinMode(TDS_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Aquaponic");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  // Initialize LoRa E220
  Serial2.begin(9600, SERIAL_8N1, E220_RX_PIN, E220_TX_PIN);
  e220.begin();

  ResponseStructContainer rsc = e220.getConfiguration();
  if (rsc.status.code == 1) {
    Configuration configuration = *(Configuration*)rsc.data;
    configuration.ADDH = 0x00;
    configuration.ADDL = 0x00;
    configuration.CHAN = LORA_CHANNEL;
    configuration.SPED.uartBaudRate = UART_BPS_9600;
    configuration.SPED.uartParity = MODE_00_8N1;
    configuration.SPED.airDataRate = AIR_DATA_RATE_010_24;
    configuration.OPTION.subPacketSetting = SPS_200_00;
    configuration.OPTION.transmissionPower = POWER_22;
    configuration.TRANSMISSION_MODE.fixedTransmission = FT_TRANSPARENT_TRANSMISSION;
    e220.setConfiguration(configuration, WRITE_CFG_PWR_DWN_SAVE);
  }
  rsc.close();

  Serial.println("LoRa E220 Initialized OK.");
  lastPiCommTime = millis();
}

void readSensors() {
  float tempWaterRead = sensors.getTempCByIndex(0);
  if (tempWaterRead != DEVICE_DISCONNECTED_C && tempWaterRead > -50.0) {
    waterTemp = tempWaterRead;
  }
  sensors.requestTemperatures();

  float tempAirRead = dht.readTemperature();
  float humAirRead = dht.readHumidity();
  if (!isnan(tempAirRead) && !isnan(humAirRead)) {
    airTemp = tempAirRead;
    airHum = humAirRead;
  }

  tdsValue = readTDS(waterTemp);
  waterLevel = readWaterLevel();

  Serial.printf("Sensors -> WaterTemp: %.2fC | TDS: %.1f ppm | AirTemp: %.2fC | Hum: %.2f%% | WaterLevel: %.1f%%\n",
                waterTemp, tdsValue, airTemp, airHum, waterLevel);
}

void sendSensorData() {
  readSensors();

  StaticJsonDocument<384> doc;
  doc["type"] = "sensor_data";
  doc["temp_w"] = round(waterTemp * 10) / 10.0;
  doc["tds"] = round(tdsValue);
  doc["temp_a"] = round(airTemp * 10) / 10.0;
  doc["hum"] = (int)airHum;
  doc["water_level"] = round(waterLevel * 10) / 10.0;
  
  doc["lamp"] = statusLampu ? 1 : 0;
  doc["pump_b"] = statusPembesaran ? 1 : 0;
  doc["pump_p"] = statusPeremajaan ? 1 : 0;
  doc["pump_s"] = statusSirkulasi ? 1 : 0;
  doc["aerator"] = statusAerator ? 1 : 0;
  doc["feeder"] = statusFeeder ? 1 : 0;
  doc["feed_confirm"] = lastFeedConfirm;

  String output;
  serializeJson(doc, output);
  e220.sendMessage(output);
  Serial.print("Sent LoRa Packet: ");
  Serial.println(output);
}

void triggerFeeder(int porsi) {
  Serial.printf("Mengaktifkan Feeder Pakan sebanyak %d porsi...\n", porsi);
  statusFeeder = true;
  sendSensorData();

  digitalWrite(RELAY_FEEDER, RELAY_ON);
  delay(5000 * porsi);
  digitalWrite(RELAY_FEEDER, RELAY_OFF);
  
  statusFeeder = false;
  lastFeedConfirm = porsi;
  sendSensorData();
}

void receiveCommand() {
  ResponseContainer rc = e220.receiveMessage();
  if (rc.status.code != 1) return;

  String incoming = rc.data;
  Serial.print("[LoRa Command Received]: ");
  Serial.println(incoming);

  StaticJsonDocument<384> doc;
  DeserializationError error = deserializeJson(doc, incoming);
  if (error) return;

  if (doc.containsKey("type") && strcmp(doc["type"], "control") == 0) {
    lastPiCommTime = millis();
    isSystemOffline = false;

    if (doc.containsKey("lamp")) {
      int val = doc["lamp"].as<int>();
      statusLampu = (val == 1 || doc["lamp"].as<bool>() == true);
      digitalWrite(RELAY_LAMPU, statusLampu ? RELAY_ON : RELAY_OFF);
      Serial.printf("--> Relay Lampu: %s\n", statusLampu ? "NYALA" : "MATI");
    }
    if (doc.containsKey("pump_b")) {
      int val = doc["pump_b"].as<int>();
      statusPembesaran = (val == 1 || doc["pump_b"].as<bool>() == true);
      digitalWrite(RELAY_PEMBESARAN, statusPembesaran ? RELAY_ON : RELAY_OFF);
      Serial.printf("--> Relay Pompa Pembesaran: %s\n", statusPembesaran ? "NYALA" : "MATI");
    }
    if (doc.containsKey("pump_p")) {
      int val = doc["pump_p"].as<int>();
      statusPeremajaan = (val == 1 || doc["pump_p"].as<bool>() == true);
      digitalWrite(RELAY_PEREMAJAAN, statusPeremajaan ? RELAY_ON : RELAY_OFF);
      Serial.printf("--> Relay Pompa Peremajaan: %s\n", statusPeremajaan ? "NYALA" : "MATI");
    }
    if (doc.containsKey("pump_s")) {
      int val = doc["pump_s"].as<int>();
      statusSirkulasi = (val == 1 || doc["pump_s"].as<bool>() == true);
      digitalWrite(RELAY_SIRKULASI, statusSirkulasi ? RELAY_ON : RELAY_OFF);
      Serial.printf("--> Relay Pompa Sirkulasi: %s\n", statusSirkulasi ? "NYALA" : "MATI");
    }
    if (doc.containsKey("aerator")) {
      int val = doc["aerator"].as<int>();
      statusAerator = (val == 1 || doc["aerator"].as<bool>() == true);
      digitalWrite(RELAY_AERATOR, statusAerator ? RELAY_ON : RELAY_OFF);
      Serial.printf("--> Relay Aerator: %s\n", statusAerator ? "NYALA" : "MATI");
    }
    if (doc.containsKey("feed")) {
      int porsi = doc["feed"];
      triggerFeeder(porsi);
    }
    
    sendSensorData();
  }
}

void updateLcdDisplay() {
  lcd.clear();
  if (lcdScreen == 0) {
    lcd.setCursor(0, 0);
    lcd.print("SuhuAir:");
    lcd.print(waterTemp, 1);
    lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print("TDS:");
    lcd.print((int)tdsValue);
    lcd.print("ppm");
  } else {
    lcd.setCursor(0, 0);
    lcd.print("SuhuUdr:");
    lcd.print(airTemp, 1);
    lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print("Hum:");
    lcd.print((int)airHum);
    lcd.print("% Lv:");
    lcd.print((int)waterLevel);
    lcd.print("%");
  }
  lcdScreen = (lcdScreen + 1) % 2;
}

void loop() {
  if (millis() - lastSendTime >= SEND_INTERVAL) {
    sendSensorData();
    lastSendTime = millis();
  }

  // 1. Terima Perintah via LoRa E220
  if (e220.available() > 0) {
    receiveCommand();
  }

  // 2. Terima Perintah via USB Serial Monitor
  if (Serial.available() > 0) {
    String serialMsg = Serial.readStringUntil('\n');
    serialMsg.trim();
    if (serialMsg.length() > 0) {
      Serial.print("[Serial USB Command]: ");
      Serial.println(serialMsg);
      StaticJsonDocument<384> doc;
      DeserializationError error = deserializeJson(doc, serialMsg);
      if (!error && doc.containsKey("type") && strcmp(doc["type"], "control") == 0) {
        if (doc.containsKey("lamp")) {
          int val = doc["lamp"].as<int>();
          statusLampu = (val == 1 || doc["lamp"].as<bool>() == true);
          digitalWrite(RELAY_LAMPU, statusLampu ? RELAY_ON : RELAY_OFF);
          Serial.printf("--> Relay Lampu: %s\n", statusLampu ? "NYALA" : "MATI");
        }
        if (doc.containsKey("pump_b")) {
          int val = doc["pump_b"].as<int>();
          statusPembesaran = (val == 1 || doc["pump_b"].as<bool>() == true);
          digitalWrite(RELAY_PEMBESARAN, statusPembesaran ? RELAY_ON : RELAY_OFF);
          Serial.printf("--> Relay Pompa Pembesaran: %s\n", statusPembesaran ? "NYALA" : "MATI");
        }
        if (doc.containsKey("pump_p")) {
          int val = doc["pump_p"].as<int>();
          statusPeremajaan = (val == 1 || doc["pump_p"].as<bool>() == true);
          digitalWrite(RELAY_PEREMAJAAN, statusPeremajaan ? RELAY_ON : RELAY_OFF);
          Serial.printf("--> Relay Pompa Peremajaan: %s\n", statusPeremajaan ? "NYALA" : "MATI");
        }
        if (doc.containsKey("pump_s")) {
          int val = doc["pump_s"].as<int>();
          statusSirkulasi = (val == 1 || doc["pump_s"].as<bool>() == true);
          digitalWrite(RELAY_SIRKULASI, statusSirkulasi ? RELAY_ON : RELAY_OFF);
          Serial.printf("--> Relay Pompa Sirkulasi: %s\n", statusSirkulasi ? "NYALA" : "MATI");
        }
        if (doc.containsKey("aerator")) {
          int val = doc["aerator"].as<int>();
          statusAerator = (val == 1 || doc["aerator"].as<bool>() == true);
          digitalWrite(RELAY_AERATOR, statusAerator ? RELAY_ON : RELAY_OFF);
          Serial.printf("--> Relay Aerator: %s\n", statusAerator ? "NYALA" : "MATI");
        }
        sendSensorData();
      }
    }
  }

  if (millis() - lastLcdUpdateTime >= 2500) {
    updateLcdDisplay();
    lastLcdUpdateTime = millis();
  }
}
