# Panduan Firmware ESP32 - Akuaponik Transmitter

Folder ini berisi kode program untuk mikrokontroler **ESP32** yang bertindak sebagai transmitter data sensor dan penerima perintah kontrol relai pakan, pompa, lampu, dan dinamo.

---

## 🔌 Skema Perkabelan (Wiring Diagram)

Hubungkan komponen-komponen Anda ke ESP32 berdasarkan tabel pinout berikut:

### 1. Modul LoRa E220 (UART)
| LoRa E220 Pin | ESP32 Pin | Deskripsi |
|---|---|---|
| **VCC** | 3.3V / 5.0V | Catu daya modul LoRa E220 |
| **GND** | GND | Ground |
| **TXD** | GPIO 16 (RX2) | Serial2 RX pada ESP32 |
| **RXD** | GPIO 17 (TX2) | Serial2 TX pada ESP32 |
| **AUX** | GPIO 14 | Pin status AUX LoRa |
| **M0** | GPIO 5 | Pin Mode 0 (Konfigurasi/Kerja) |
| **M1** | GPIO 2 | Pin Mode 1 (Konfigurasi/Kerja) |

### 2. Sensor-Sensor
| Sensor | Pin Sensor | ESP32 Pin | Catatan |
|---|---|---|---|
| **DS18B20** (Suhu Air) | Data (Kuning) | **GPIO 4** | Memerlukan resistor pull-up 4.7k ohm antara kabel data dan 3.3V. |
| **DHT11** (Suhu & Kelembapan Udara) | Out / Data | **GPIO 15** | Berikan daya 3.3V atau 5V sesuai modul DHT Anda. |
| **TDS Analog Sensor** | Signal (A/O) | **GPIO 34** | Terhubung ke pin Analog Input (ADC1_CH6). Hubungkan VCC modul ke 5V, GND ke GND bersama, dan A/O ke GPIO 34. Jangan pernah memasukkan tegangan di atas 3.3V ke GPIO 34. |
| **AJ-SR04M** (Tinggi Air) | Trig | **GPIO 32** | Pin trigger sensor ultrasonik. |
| **AJ-SR04M** (Tinggi Air) | Echo | **GPIO 33** | Pin echo sensor ultrasonik. |

### 3. Aktuator & Output (Relay Module 6 Channels)
| Aktuator | Relay Pin | ESP32 Pin | Deskripsi |
|---|---|---|---|
| **Lampu** | IN1 / Signal | **GPIO 27** | Mengaktifkan/mematikan lampu UV/tanaman. |
| **Pompa Pembesaran** | IN2 / Signal | **GPIO 12** | Mengaktifkan/mematikan pompa kolam pembesaran. |
| **Pompa Peremajaan** | IN3 / Signal | **GPIO 25** | Mengaktifkan/mematikan pompa kolam peremajaan. |
| **Pompa Sirkulasi** | IN4 / Signal | **GPIO 26** | Mengaktifkan/mematikan pompa sirkulasi air. |
| **Aerator** | IN5 / Signal | **GPIO 23** | Mengaktifkan/mematikan aerator. |
| **Feeder (Pakan)** | IN6 / Signal | **GPIO 13** | Mengaktifkan motor/servo pemberi pakan ikan. |

---

## 📚 Library Arduino yang Dibutuhkan

Sebelum melakukan upload kode ke ESP32 menggunakan Arduino IDE atau PlatformIO, pastikan Anda telah menginstal beberapa library berikut melalui **Library Manager** (Ctrl + Shift + I):

1. **LoRa_E220** (oleh Renzo Mischianti) - Untuk komunikasi dengan modul LoRa E220 UART.
2. **ArduinoJson** (oleh Benoit Blanchon) - Untuk serialization & deserialization format data JSON.
3. **DHT sensor library** (oleh Adafruit) + **Adafruit Unified Sensor** - Untuk membaca sensor DHT11.
4. **OneWire** (oleh Jim Studt, Paul Stoffregen, dkk) - Protokol komunikasi DS18B20.
5. **DallasTemperature** (oleh Miles Burton) - Library khusus sensor suhu air DS18B20.

---

## ⚙️ Langkah Kalibrasi & Upload

1. Hubungkan modul ESP32 ke laptop menggunakan kabel data Micro USB atau USB-C.
2. Buka berkas [esp32_transmitter.ino](file:///c:/Users/User/.gemini/antigravity-ide/scratch/aquaponics-system/esp32_firmware/esp32_transmitter.ino) di Arduino IDE.
3. Pilih Board `ESP32 Dev Module` dan Port yang sesuai di menu *Tools*, lalu klik tombol **Upload**.
