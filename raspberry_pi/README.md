# Panduan Raspberry Pi 3 - LoRa Gateway

Folder ini berisi script Python yang berjalan sebagai *gateway* di **Raspberry Pi 3**. Script ini bertindak sebagai jembatan yang menerima data dari modul LoRa Receiver (ESP32) dan mengunggahnya ke **Firebase Realtime Database**, serta mengirim balik instruksi kontrol aktuator dari database ke ESP32.

---

## 🔌 Skema Perkabelan (Wiring Diagram)

Hubungkan pin modul **LoRa SX1278** ke **Raspberry Pi 3** sesuai tabel di bawah ini:

| LoRa Pin | RPi 3 Pin Name | RPi Pin Number (Phys) | Deskripsi |
|---|---|---|---|
| **VCC** | 3.3V Power | Pin 1 atau Pin 17 | Catu daya modul LoRa |
| **GND** | Ground | Pin 25 atau Pin 9 | Grounding |
| **MISO** | GPIO 9 (MISO) | Pin 21 | SPI Master In Slave Out |
| **MOSI** | GPIO 10 (MOSI) | Pin 19 | SPI Master Out Slave In |
| **SCK** | GPIO 11 (SCLK) | Pin 23 | SPI Clock |
| **NSS / CS** | GPIO 8 (SPI0 CE0) | Pin 24 | SPI Chip Enable 0 |
| **RST** | GPIO 25 | Pin 22 | Hardware Reset |
| **DIO0** | GPIO 24 | Pin 18 | Interrupt Request (IRQ) |

---

## ⚙️ Persiapan Sistem Raspberry Pi

### 1. Aktifkan Interface SPI
Secara default, interface SPI di Raspberry Pi dinonaktifkan. Anda harus mengaktifkannya terlebih dahulu:
1. Jalankan konfigurasi sistem Raspberry Pi di terminal:
   ```bash
   sudo raspi-config
   ```
2. Pilih **3 Interface Options** -> **I4 SPI**.
3. Pilih **Yes** untuk mengaktifkan SPI.
4. Restart Raspberry Pi:
   ```bash
   sudo reboot
   ```

### 2. Download Firebase Service Account Key (JSON)
Agar script Python memiliki izin menulis & membaca ke Firebase Anda:
1. Masuk ke **Firebase Console** (https://console.firebase.google.com/).
2. Buka proyek Anda, lalu klik ikon **Settings (Gerigi)** -> **Project Settings**.
3. Buka tab **Service Accounts**.
4. Klik tombol **Generate New Private Key** di bagian bawah.
5. File `.json` akan terdownload. Ubah nama file tersebut menjadi `serviceAccountKey.json` dan letakkan di dalam folder `raspberry_pi/` ini bersama file `gateway.py`.
6. Salin URL database Firebase Anda (berformat `https://[nama-proyek]-default-rtdb.firebaseio.com/`) lalu buka file `gateway.py` dan ubah variabel `FIREBASE_DATABASE_URL` dengan URL database milik Anda.

---

## 🚀 Langkah Instalasi & Menjalankan Script

1. Salin seluruh isi folder `raspberry_pi/` ini ke dalam Raspberry Pi 3 Anda (misal ke direktori `/home/pi/aquaponics/`).
2. Masuk ke folder tersebut dan instal library python yang dibutuhkan:
   ```bash
   cd /home/pi/aquaponics
   pip3 install -r requirements.txt
   ```
3. Jalankan script gateway:
   ```bash
   python3 gateway.py
   ```
   *Catatan: Jika Anda menjalankan script ini di laptop/PC tanpa modul SPI LoRa terpasang, script akan secara otomatis mendeteksi bahwa ini bukan hardware Raspberry Pi dan akan berjalan dalam **Mode Simulasi (Mock)** dengan mengirimkan data acak ke Firebase agar Anda bisa melakukan pengujian UI/dashboard.*

---

## 🖥️ Menjalankan Gateway sebagai Background Service (Systemd)

Agar script ini berjalan otomatis sejak Raspberry Pi dinyalakan dan tetap hidup meskipun terminal ditutup, buat systemd service:

1. Buat file service baru:
   ```bash
   sudo nano /etc/systemd/system/aquaponics-gateway.service
   ```
2. Paste konfigurasi berikut (sesuaikan path folder jika berbeda):
   ```ini
   [Unit]
   Description=Aquaponics LoRa Firebase Gateway Service
   After=network.target

   [Service]
   Type=simple
   User=pi
   WorkingDirectory=/home/pi/aquaponics
   ExecStart=/usr/bin/python3 /home/pi/aquaponics/gateway.py
   Restart=on-failure
   RestartSec=5

   [Install]
   WantedBy=multi-user.target
   ```
3. Simpan (Ctrl+O, lalu Enter) dan keluar (Ctrl+X).
4. Aktifkan dan jalankan service:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable aquaponics-gateway.service
   sudo systemctl start aquaponics-gateway.service
   ```
5. Periksa status service untuk memastikan berjalan lancar:
   ```bash
   sudo systemctl status aquaponics-gateway.service
   ```
