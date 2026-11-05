# Weather Station + Jam - LilyGo T-Display

Firmware stasiun cuaca dan jam untuk LilyGo T-Display (ESP32, ST7789 135x240). Data cuaca dari Open-Meteo (tanpa API key), waktu dari NTP.

## Halaman (5)
1. Jam besar + cuaca ringkas (tanggal, suhu, kondisi, lembap, angin, peluang hujan)
2. Per jam (grafik suhu 12 jam + bar hujan)
3. 5 hari (rentang suhu + hujan)
4. Matahari, UV, AQI, tekanan
5. Jam analog

Ikon baterai di header (tanpa persen; di halaman jam digeser ke kanan karena tidak ada jam di header), berubah menjadi animasi dengan petir saat sedang diisi.

## Tombol
- BTN1 (GPIO0, tombol atas): halaman berikut. Klik ganda: power off (deep sleep). Tahan 3 detik: hapus WiFi, masuk mode setup
- Bangun dari power off: tekan BTN2 (GPIO35, tombol bawah)
- BTN2 (GPIO35): refresh data. Tahan 1 detik: buka menu pengaturan
- Di menu: BTN1 pilih item, BTN2 ubah nilai, tahan BTN2 simpan dan keluar
- Layar yang mati/redup: tombol pertama hanya membangunkan layar
- Halaman ganti sendiri tiap 10 detik setelah 30 detik tanpa tombol (`AUTO_ROTATE_MS`, 0 = mati)

## Pengaturan (menu di perangkat dan web UI)
- Kecerahan 5 level (100/75/50/25/10 persen)
- Redup otomatis saat malam (mengikuti is_day dari data cuaca)
- Mode hemat: Mati, Ringan (redup setelah 1 menit), Sedang (+ animasi lambat, update data min. 30 menit, auto-ganti halaman mati), Maksimal (+ layar mati setelah 5 menit). Tingkat Ringan ke atas juga menurunkan CPU ke 80 MHz dan mengaktifkan WiFi modem sleep
- Layar mati terjadwal (00-05, 23-05, atau 22-06)
- Tampil/sembunyikan ikon baterai

## Catatan baterai
T-Display tidak punya pin status charger, jadi ikon "mengisi" ditebak dari tegangan di GPIO34: tegangan di atas sekitar 4,19 V atau naik dari menit ke menit dianggap sedang diisi. Ini perkiraan, bukan sinyal langsung dari charger. Tanpa baterai terpasang, ikon bisa terbaca penuh atau tidak muncul; matikan lewat menu kalau mengganggu.

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
