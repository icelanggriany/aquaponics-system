#!/usr/bin/env python3
"""
Raspberry Pi 3 Aquaponics Gateway
Menghubungkan jaringan LoRa nirkabel (ESP32) dengan Firebase Realtime Database.
"""

import os
import sys
import time
import json
import datetime
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

# --- KONFIGURASI FIREBASE REALTIME DATABASE ---
FIREBASE_DATABASE_URL = "https://aquaponics-system-8d6f6-default-rtdb.asia-southeast1.firebasedatabase.app"

# GPIO & Serial (Hanya diaktifkan jika di Raspberry Pi)
is_raspberry_pi = False
try:
    import RPi.GPIO as GPIO
    import serial
    is_raspberry_pi = True
except ImportError:
    print("Menjalankan dalam MODUL SIMULASI (Non-RPi Hardware).")
    print("Gunakan Raspberry Pi asli dengan pin UART/GPIO terpasang untuk mode produksi.")

# --- KONFIGURASI PIN LORA E220 (RASPBERRY PI) ---
# Penomoran di bawah menggunakan GPIO BCM (karena pustaka RPi.GPIO diatur ke GPIO.BCM).
# Rujukan pinout WiringPi / Pi4J (sesuai gambar diagram):
# - E220 TXD -> RPi RXD (Pin Fisik 10 | BCM GPIO 15 | WiringPi GPIO 16)
# - E220 RXD -> RPi TXD (Pin Fisik 8  | BCM GPIO 14 | WiringPi GPIO 15)
# - E220 AUX -> BCM 24 (Pin Fisik 18 | WiringPi GPIO 5) - Input
# - E220 M0  -> BCM 22 (Pin Fisik 15 | WiringPi GPIO 3) - Output
# - E220 M1  -> BCM 23 (Pin Fisik 16 | WiringPi GPIO 4) - Output
LORA_AUX = 24
LORA_M0 = 22
LORA_M1 = 23
LORA_PORT = "/dev/serial0" # Port serial default Raspberry Pi 3

# --- KONFIGURASI LORA ---
LORA_CHANNEL = 65 # Channel 65 = 915.125 MHz

# --- DRIVER LORA SEDERHANA / MOCK ---
class E220LoRaSimulator:
    """Kelas simulasi LoRa jika berjalan di PC/Testing Environment"""
    def __init__(self):
        self.on_receive_callback = None
        self._running = True
        self._thread = threading.Thread(target=self._mock_loop)
        self._thread.daemon = True

    def begin(self):
        print("[LoRa Sim] Driver simulasi E220 aktif.")
        self._thread.start()

    def set_on_receive(self, callback):
        self.on_receive_callback = callback

    def send_packet(self, data):
        print(f"[LoRa Sim] Mengirim perintah RF E220: {data}")

    def _mock_loop(self):
        # Loop dinonaktifkan atas request user untuk menghindari pengiriman data dummy.
        # Simulator hanya berjalan pasif tanpa mengirimkan data tiruan.
        while self._running:
            time.sleep(1)

# Variabel Global untuk menyimpan status kontrol dan penjadwalan dari Firebase
firebase_pump_state = False
firebase_light_state = False
firebase_dinamo_state = False
schedules_list = []
feed_mode = "Auto"
last_triggered_time = ""

# Melacak status kontrol terakhir yang berhasil dikirim ke ESP32 untuk menghindari paket duplikat redundan
last_sent_states = {
    "pump": None,
    "light": None,
    "dinamo": None
}

class E220LoRaHW:
    """Kelas driver LoRa E220-900T22D menggunakan UART Serial di Raspberry Pi"""
    def __init__(self, port=LORA_PORT, baudrate=9600):
        self.port = port
        self.baudrate = baudrate
        self.on_receive_callback = None
        self.ser = None
        self._running = False
        self._recv_thread = None

    def begin(self):
        GPIO.setmode(GPIO.BCM)
        GPIO.setup(LORA_AUX, GPIO.IN)
        GPIO.setup(LORA_M0, GPIO.OUT)
        GPIO.setup(LORA_M1, GPIO.OUT)
        
        # Atur E220 ke mode Konfigurasi (M0=1, M1=1) untuk mengirim byte perintah
        print("[LoRa HW] Mengatur E220 ke mode konfigurasi...")
        GPIO.output(LORA_M0, GPIO.HIGH)
        GPIO.output(LORA_M1, GPIO.HIGH)
        time.sleep(0.1) # Tunggu modul stabil
        
        # Buka Serial Port (Konfigurasi selalu 9600 bps)
        self.ser = serial.Serial(self.port, baudrate=9600, timeout=1)
        
        # Kirim byte konfigurasi E220 (6 register starting dari 00H): 
        # C0 (Register write)
        # 00 (Start address)
        # 06 (Length)
        # 00 00 (Address 0x0000)
        # 62 (REG0 - Speed: UART 9600, 8N1; Air rate: 2.4kbps)
        # 00 (REG1 - Configuration: 200 bytes subpacket, RSSI disabled, Power 22dBm)
        # LORA_CHANNEL (REG2 - Channel 65 = 915.125 MHz)
        # 00 (REG3 - Transmission Mode: Transparent)
        config_cmd = bytes([0xC0, 0x00, 0x06, 0x00, 0x00, 0x62, 0x00, LORA_CHANNEL, 0x00])
        print(f"[LoRa HW] Mengirim perintah konfigurasi: {config_cmd.hex()}")
        self.ser.write(config_cmd)
        time.sleep(0.1)
        
        # Baca respon balik (harusnya C1 00 06 ...)
        response = self.ser.read(9)
        if len(response) == 9 and response[0] == 0xC1:
            print(f"[LoRa HW] Konfigurasi E220 sukses! Respon: {response.hex()}")
        else:
            print(f"[LoRa HW] WARNING: Konfigurasi E220 gagal atau tidak direspon. Respon: {response.hex()}")
            
        # Kembalikan E220 ke mode Normal (M0=0, M1=0)
        print("[LoRa HW] Mengatur E220 ke mode normal...")
        GPIO.output(LORA_M0, GPIO.LOW)
        GPIO.output(LORA_M1, GPIO.LOW)
        
        # Tunggu sampai pin AUX kembali HIGH
        timeout = 50
        while GPIO.input(LORA_AUX) == GPIO.LOW and timeout > 0:
            time.sleep(0.01)
            timeout -= 1
            
        # Mulai thread background untuk menerima data serial
        self._running = True
        self._recv_thread = threading.Thread(target=self._recv_loop)
        self._recv_thread.daemon = True
        self._recv_thread.start()
        print("[LoRa HW] Penerima data serial LoRa E220 siap.")

    def send_packet(self, data):
        if not self.ser:
            print("[LoRa HW] Serial port belum siap, gagal mengirim.")
            return

        # Tunggu AUX kembali HIGH (Modul idle/siap kirim)
        timeout = 100 # 1 detik timeout
        while GPIO.input(LORA_AUX) == GPIO.LOW and timeout > 0:
            time.sleep(0.01)
            timeout -= 1
            
        # Kirim data transparently
        payload_bytes = data.encode('utf-8')
        self.ser.write(payload_bytes)
        print(f"[LoRa HW] Data dikirim ke serial: {data}")

    def set_on_receive(self, callback):
        self.on_receive_callback = callback

    def _recv_loop(self):
        buffer = b""
        while self._running:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    # Baca byte yang tersedia
                    data = self.ser.read(self.ser.in_waiting)
                    buffer += data
                    
                    # Ekstrak paket JSON utuh jika ditemukan '{' dan '}'
                    while b"{" in buffer and b"}" in buffer:
                        start = buffer.index(b"{")
                        end = buffer.index(b"}", start) + 1
                        packet = buffer[start:end]
                        buffer = buffer[end:]
                        
                        try:
                            decoded = packet.decode('utf-8')
                            if self.on_receive_callback:
                                self.on_receive_callback(decoded)
                        except Exception as e:
                            print(f"[LoRa HW] Gagal decoding payload: {e}")
                else:
                    time.sleep(0.1)
            except Exception as e:
                print(f"[LoRa HW] Error di receive loop: {e}")
                time.sleep(1)

class E220LoRaSerialOnly:
    """Driver LoRa E220 menggunakan UART Serial saja (untuk PC/Windows dengan USB-to-UART)"""
    def __init__(self, port, baudrate=9600):
        self.port = port
        self.baudrate = baudrate
        self.on_receive_callback = None
        self.ser = None
        self._running = False
        self._recv_thread = None

    def begin(self):
        print(f"[LoRa Serial] Membuka port serial {self.port} pada {self.baudrate} bps...")
        try:
            self.ser = serial.Serial(self.port, baudrate=self.baudrate, timeout=1)
            self._running = True
            self._recv_thread = threading.Thread(target=self._recv_loop)
            self._recv_thread.daemon = True
            self._recv_thread.start()
            print(f"[LoRa Serial] Port {self.port} berhasil dibuka. Menunggu data...")
        except Exception as e:
            print(f"[LoRa Serial] ERROR: Gagal membuka port {self.port}: {e}")
            print("[LoRa Serial] Pastikan modul USB-to-UART sudah terpasang dan nama port benar.")

    def send_packet(self, data):
        if not self.ser:
            print("[LoRa Serial] Port serial belum siap, gagal mengirim.")
            return
        try:
            payload_bytes = data.encode('utf-8')
            self.ser.write(payload_bytes)
            print(f"[LoRa Serial] Data dikirim: {data}")
        except Exception as e:
            print(f"[LoRa Serial] Error saat mengirim data: {e}")

    def set_on_receive(self, callback):
        self.on_receive_callback = callback

    def _recv_loop(self):
        buffer = b""
        while self._running:
            try:
                if self.ser and self.ser.in_waiting > 0:
                    data = self.ser.read(self.ser.in_waiting)
                    buffer += data
                    
                    # Ekstrak paket JSON utuh jika ditemukan '{' dan '}'
                    while b"{" in buffer and b"}" in buffer:
                        start = buffer.index(b"{")
                        end = buffer.index(b"}", start) + 1
                        packet = buffer[start:end]
                        buffer = buffer[end:]
                        
                        try:
                            decoded = packet.decode('utf-8')
                            if self.on_receive_callback:
                                self.on_receive_callback(decoded)
                        except Exception as e:
                            print(f"[LoRa Serial] Gagal decoding payload: {e}")
                else:
                    time.sleep(0.1)
            except Exception as e:
                print(f"[LoRa Serial] Error di receive loop: {e}")
                time.sleep(1)

def detect_serial_port():
    try:
        import serial.tools.list_ports
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if ports:
            print(f"[LoRa Init] Port serial terdeteksi otomatis: {ports[0]}")
            return ports[0]
    except Exception as e:
        print(f"[LoRa Init] Error mendeteksi port serial: {e}")
    return None

# --- INISIALISASI LORA DRIVER ---
lora = None

def init_lora():
    global lora
    port = config_data.get("serial_port", "AUTO")
    use_sim = config_data.get("use_simulation", False)
    
    if use_sim:
        print("[LoRa Init] Mengaktifkan Mode Simulasi (data dummy) sesuai konfigurasi.")
        lora = E220LoRaSimulator()
        return

    if is_raspberry_pi:
        print("[LoRa Init] Menjalankan di Raspberry Pi. Menggunakan driver hardware dengan GPIO.")
        lora = E220LoRaHW(port=LORA_PORT)
    else:
        # Jika di PC/Windows, gunakan Serial-Only
        if port == "AUTO" or not port:
            detected = detect_serial_port()
            if detected:
                port = detected
            else:
                print("[LoRa Init] WARNING: Tidak ada port serial (COM) terdeteksi.")
                print("[LoRa Init] Sistem tidak menggunakan data dummy. Harap colokkan USB-to-UART LoRa")
                print("[LoRa Init] atau ubah 'use_simulation': true di gateway_config.json jika ingin menguji dengan data simulasi.")
                # Tetap inisialisasi driver dengan port default agar tidak crash
                port = "COM3" 
        
        lora = E220LoRaSerialOnly(port=port)

# --- LOCAL CONFIGURATION PERSISTENCE ---
CONFIG_FILE = "gateway_config.json"

config_data = {
    "lamp_mode": "Auto",
    "lamp_state": 0,
    "lamp_schedule": {"start": "06:00", "end": "18:00"},
    "pump_b_mode": "Auto",
    "pump_b_state": 0,
    "pump_b_schedule": {"start": "07:00", "end": "17:00"},
    "pump_p_state": 0,
    "pump_s_state": 0,
    "aerator_state": 0,
    "feed_mode": "Auto",
    "schedules_list": [],
    "serial_port": "AUTO",
    "use_simulation": False
}

def load_config():
    global config_data, latest_sensor_data
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r") as f:
                loaded = json.load(f)
                config_data.update(loaded)
            print("[Config] Konfigurasi lokal berhasil dimuat.")
        except Exception as e:
            print(f"[Config] Gagal memuat konfigurasi: {e}")
    else:
        save_config()
        
    latest_sensor_data["lamp_mode"] = config_data["lamp_mode"]
    latest_sensor_data["lamp_schedule"] = config_data["lamp_schedule"]
    latest_sensor_data["pump_b_mode"] = config_data["pump_b_mode"]
    latest_sensor_data["pump_b_schedule"] = config_data["pump_b_schedule"]
    latest_sensor_data["feed_mode"] = config_data["feed_mode"]
    latest_sensor_data["schedules"] = config_data["schedules_list"]

def save_config():
    try:
        with open(CONFIG_FILE, "w") as f:
            json.dump(config_data, f, indent=4)
        print("[Config] Konfigurasi lokal disimpan.")
    except Exception as e:
        print(f"[Config] Gagal menyimpan konfigurasi: {e}")

def is_time_in_range(start_str, end_str, check_time):
    try:
        start = datetime.datetime.strptime(start_str, "%H:%M").time()
        end = datetime.datetime.strptime(end_str, "%H:%M").time()
        if start <= end:
            return start <= check_time <= end
        else:
            return check_time >= start or check_time <= end
    except Exception as e:
        print(f"[Scheduler] Error parsing time range {start_str} - {end_str}: {e}")
        return False

def evaluate_scheduler_and_send(send_feed=0):
    global config_data
    now = datetime.datetime.now()
    current_time = now.time()
    
    # 1. Evaluate Lamp Schedule if Auto Mode
    if config_data["lamp_mode"] == "Auto":
        sched = config_data["lamp_schedule"]
        if is_time_in_range(sched.get("start", "06:00"), sched.get("end", "18:00"), current_time):
            config_data["lamp_state"] = 1
        else:
            config_data["lamp_state"] = 0
            
    # 2. Evaluate Grow-out Pump Schedule if Auto Mode
    if config_data["pump_b_mode"] == "Auto":
        sched = config_data["pump_b_schedule"]
        if is_time_in_range(sched.get("start", "07:00"), sched.get("end", "17:00"), current_time):
            config_data["pump_b_state"] = 1
        else:
            config_data["pump_b_state"] = 0
            
    # Prepare LoRa control packet
    cmd = {
        "type": "control",
        "lamp": config_data["lamp_state"],
        "pump_b": config_data["pump_b_state"],
        "pump_p": config_data["pump_p_state"],
        "pump_s": config_data["pump_s_state"],
        "aerator": config_data["aerator_state"]
    }
    
    if send_feed > 0:
        cmd["feed"] = send_feed
        
    payload = json.dumps(cmd)
    lora.send_packet(payload)
    print(f"[Scheduler] Mengirim kontrol LoRa ke ESP32: {payload}")

# --- LOCAL API SERVER VARIABLES & HANDLER ---
latest_sensor_data = {
    "type": "sensor_data",
    "temp_w": 0.0,
    "tds": 0.0,
    "temp_a": 0.0,
    "hum": 0,
    "water_level": 0.0,
    
    # 6 Relay states
    "lamp": 0,
    "pump_b": 0,
    "pump_p": 0,
    "pump_s": 0,
    "aerator": 0,
    "feeder": 0,
    
    # Configuration modes
    "lamp_mode": "Auto",
    "lamp_schedule": {"start": "06:00", "end": "18:00"},
    "pump_b_mode": "Auto",
    "pump_b_schedule": {"start": "07:00", "end": "17:00"},
    "feed_mode": "Auto",
    "schedules": [],
    
    "status": "Mati",
    "timestamp": ""
}
sensor_history = []

class LocalAPIServer(BaseHTTPRequestHandler):
    def _set_headers(self, status_code=200, content_type="application/json"):
        self.send_response(status_code)
        self.send_header("Content-type", content_type)
        self.send_header("Access-Control-Allow-Origin", "*")  # CORS
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_OPTIONS(self):
        self._set_headers(200)

    def do_GET(self):
        if self.path == "/api/status":
            self._set_headers(200)
            # Tentukan status keaktifan alat fisik secara dinamis berdasarkan timestamp telemetri terakhir
            current_data = latest_sensor_data.copy()
            if current_data.get("timestamp"):
                try:
                    last_time = datetime.datetime.fromisoformat(current_data["timestamp"])
                    elapsed = (datetime.datetime.now() - last_time).total_seconds()
                    if elapsed > 15.0:
                        current_data["status"] = "Mati"
                except Exception as e:
                    print(f"[API Server] Error parsing timestamp: {e}")
                    current_data["status"] = "Mati"
            else:
                current_data["status"] = "Mati"
            self.wfile.write(json.dumps(current_data).encode("utf-8"))
        elif self.path == "/api/history":
            self._set_headers(200)
            self.wfile.write(json.dumps(sensor_history[-20:]).encode("utf-8"))
        else:
            self._set_headers(404, "text/plain")
            self.wfile.write(b"Not Found")

    def do_POST(self):
        if self.path == "/api/control":
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length)
            try:
                cmd = json.loads(post_data.decode("utf-8"))
                print(f"[API Server] Menerima data kontrol: {cmd}")
                
                global config_data
                
                if "lamp_mode" in cmd:
                    config_data["lamp_mode"] = cmd["lamp_mode"]
                if "lamp" in cmd:
                    config_data["lamp_state"] = 1 if (cmd["lamp"] == 1 or cmd["lamp"] is True) else 0
                if "lamp_schedule" in cmd:
                    config_data["lamp_schedule"] = cmd["lamp_schedule"]
                    
                if "pump_b_mode" in cmd:
                    config_data["pump_b_mode"] = cmd["pump_b_mode"]
                if "pump_b" in cmd:
                    config_data["pump_b_state"] = 1 if (cmd["pump_b"] == 1 or cmd["pump_b"] is True) else 0
                if "pump_b_schedule" in cmd:
                    config_data["pump_b_schedule"] = cmd["pump_b_schedule"]
                    
                if "pump_p" in cmd:
                    config_data["pump_p_state"] = 1 if (cmd["pump_p"] == 1 or cmd["pump_p"] is True) else 0
                if "pump_s" in cmd:
                    config_data["pump_s_state"] = 1 if (cmd["pump_s"] == 1 or cmd["pump_s"] is True) else 0
                if "aerator" in cmd:
                    config_data["aerator_state"] = 1 if (cmd["aerator"] == 1 or cmd["aerator"] is True) else 0
                    
                if "feed_mode" in cmd:
                    config_data["feed_mode"] = cmd["feed_mode"]
                if "schedules" in cmd:
                    config_data["schedules_list"] = cmd["schedules"]
                    
                save_config()
                
                send_feed = 0
                if "feed" in cmd:
                    send_feed = int(cmd["feed"])
                
                evaluate_scheduler_and_send(send_feed)
                
                self._set_headers(200)
                self.wfile.write(json.dumps({"status": "success", "received": cmd}).encode("utf-8"))
            except Exception as e:
                self._set_headers(400)
                self.wfile.write(json.dumps({"status": "error", "message": str(e)}).encode("utf-8"))
        else:
            self._set_headers(404, "text/plain")
            self.wfile.write(b"Not Found")

def run_api_server():
    server_address = ('0.0.0.0', 5000)
    httpd = HTTPServer(server_address, LocalAPIServer)
    print("[API Server] Web API lokal aktif di port 5000.")
    httpd.serve_forever()

# --- DATABASE HANDLER ---
def upload_sensor_data(sensor_data_json):
    """Mengupdate cache lokal dengan data dari LoRa"""
    global latest_sensor_data, sensor_history, config_data
    try:
        data = json.loads(sensor_data_json)
        if data.get("type") != "sensor_data":
            return
            
        timestamp = datetime.datetime.now().isoformat()
        data["timestamp"] = timestamp
        data["status"] = "Aktif"
        
        # Inject current configs
        data["lamp_mode"] = config_data["lamp_mode"]
        data["lamp_schedule"] = config_data["lamp_schedule"]
        data["pump_b_mode"] = config_data["pump_b_mode"]
        data["pump_b_schedule"] = config_data["pump_b_schedule"]
        data["feed_mode"] = config_data["feed_mode"]
        data["schedules"] = config_data["schedules_list"]
        
        # Simpan ke cache lokal
        latest_sensor_data.update(data)
        
        # Tambahkan ke history local
        time_str = datetime.datetime.now().strftime("%H:%M:%S")
        sensor_history.append({
            "time": time_str,
            "temp_w": data.get("temp_w"),
            "tds": data.get("tds"),
            "temp_a": data.get("temp_a"),
            "hum": data.get("hum"),
            "water_level": data.get("water_level")
        })
        if len(sensor_history) > 100:
            sensor_history.pop(0)
            
        print(f"[{time_str}] Data di-cache lokal: SuhuAir={data.get('temp_w')}C, TDS={data.get('tds')} ppm, TinggiAir={data.get('water_level')}%")
        
    except json.JSONDecodeError:
        print(f"Error parsing JSON LoRa packet: {sensor_data_json}")
    except Exception as e:
        print(f"Error Cache Data: {e}")

def main():
    print("======================================================")
    print("     AQUAPONICS LORA GATEWAY SERVICE UNTUK RPi 3       ")
    print("======================================================")
    
    global last_triggered_time
    
    # Muat konfigurasi persisten lokal
    load_config()
    
    # Inisialisasi driver LoRa berdasarkan konfigurasi
    init_lora()
    
    # Pasang handler untuk data LoRa masuk (selalu gunakan upload_sensor_data agar cache lokal terupdate)
    if lora:
        lora.set_on_receive(upload_sensor_data)
        
    # Jalankan Local API Server di thread background (port 5000)
    api_thread = threading.Thread(target=run_api_server)
    api_thread.daemon = True
    api_thread.start()
 
    # Jalankan modul LoRa
    if lora:
        lora.begin()
 
    try:
        # Loop utama tetap hidup
        while True:
            time.sleep(5) # Cek setiap 5 detik
            
            send_feed = 0
            
            # 1. Pengecekan Jadwal Pakan Otomatis (jika mode Auto)
            if config_data["feed_mode"] == "Auto" and config_data["schedules_list"]:
                now = datetime.datetime.now()
                current_time_str = now.strftime("%H:%M")
                current_date_hour_str = now.strftime("%Y-%m-%d %H:%M")
                
                # Cari apakah ada jadwal yang cocok dengan waktu saat ini
                for schedule in config_data["schedules_list"]:
                    if isinstance(schedule, dict) and schedule.get("time") == current_time_str:
                        if last_triggered_time != current_date_hour_str:
                            portion = int(schedule.get("portion", 1))
                            print(f"[AUTO FEED] Waktu cocok ({current_time_str})! Memberikan {portion} porsi pakan...")
                            send_feed = portion
                            
                            # Update status terakhir
                            last_triggered_time = current_date_hour_str
                            break
            
            # 2. Evaluasi berkala untuk Lampu & Pompa Pembesaran, lalu kirim status sync via LoRa
            evaluate_scheduler_and_send(send_feed)
            
    except KeyboardInterrupt:
        print("\nGateway dimatikan.")
    finally:
        if is_raspberry_pi:
            GPIO.cleanup()

if __name__ == "__main__":
    main()
