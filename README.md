# Weather Station - LilyGo T-Display

Firmware stasiun cuaca untuk LilyGo T-Display (ESP32, ST7789 135x240). Data dari Open-Meteo (tanpa API key).

## Halaman
1. Sekarang (suhu, kondisi, lembap, angin, peluang hujan)
2. Per jam (grafik suhu 12 jam + bar hujan)
3. 5 hari (rentang suhu + hujan)
4. Matahari, UV, AQI, tekanan

## Tombol
- BTN1 (GPIO0): halaman berikut. Tahan 3 detik: hapus WiFi, masuk mode setup
- BTN2 (GPIO35): refresh data. Tahan 1 detik: ganti kecerahan
- Auto ganti halaman tiap 10 detik (atur `AUTO_ROTATE_MS`, 0 = mati)

## Setup pertama
1. Flash, lalu sambung HP ke WiFi `Weather-Setup`, buka `192.168.4.1` (portal biasanya muncul sendiri)
2. Pilih WiFi rumah, isi password, isi kota (tombol Cari, atau kosongkan koordinat dan perangkat mencari sendiri), Simpan
3. Setelah tersambung, pengaturan bisa diubah lagi di `http://weather.local` atau IP yang tampil saat boot

## Build
Push ke GitHub, workflow `Build firmware` jalan otomatis. Hasil di Artifacts:
- `weather-station-merged.bin`: flash di alamat 0x0
- `weather-station-app.bin`: hanya aplikasi (alamat 0x10000)

```
esptool.py --chip esp32 --port COMx write_flash 0x0 weather-station-merged.bin
```
Build lokal: `pio run -e tdisplay`
