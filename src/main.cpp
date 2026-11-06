// ============================================================
//  Weather Station + Jam - LilyGo T-Display (ESP32, ST7789 135x240)
//  Data: Open-Meteo (tanpa API key). 5 halaman + menu + web setup.
//
//  Tombol bawah (BTN1, GPIO0)  : klik = halaman berikut | klik ganda = BUKA PENGATURAN
//                                tahan 3 dtk = reset WiFi
//  Tombol atas  (BTN2, GPIO35) : klik = refresh data | klik ganda = POWER OFF
//  Bangun dari power off: tekan sekali tombol atas.
//  Di menu: bawah = geser pilihan, atas = ubah nilai / pilih "Kembali",
//           tahan bawah 1 dtk = keluar (pintasan), otomatis keluar setelah 30 dtk
//  Pola power off & baca baterai mengikuti firmware Emochi.
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "web_ui.h"

#define BTN1 0
#define BTN2 35
#define BAT_PIN 34
#define ADC_EN 14                // BAT_EN: enable pengukuran baterai (HIGH hanya saat mengukur)
#define ROT 1                    // ganti ke 3 untuk layar terbalik
#define AP_NAME "Weather-Setup"
#define AUTO_ROTATE_MS 10000UL   // 0 = matikan auto ganti halaman
#define PAGES 5
#define MENU_N 6   // 5 pengaturan + "Kembali"
#define SW 240
#define SH 135

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
Preferences prefs;
WebServer server(80);
DNSServer dns;

// ---------------- config & data ----------------
struct Config {
  String ssid, pass, city;
  float lat, lon;
  uint16_t ivl;
  int32_t tz;
  bool needGeo;
  uint8_t briIdx;   // 0..4 -> 100,75,50,25,10 %
  uint8_t saver;    // 0 mati, 1 ringan, 2 sedang, 3 maksimal
  uint8_t sch;      // jadwal layar mati (indeks SCH)
  bool autoDim;     // redup otomatis saat malam
  bool showBat;     // tampilkan ikon baterai
} cfg;

const uint8_t BRI[5] = {255, 190, 128, 64, 25};
const char* SAV_TXT[4] = {"Mati", "Ringan", "Sedang", "Maksimal"};
struct Sch { int8_t from, to; };
const Sch SCH[4] = {{-1, -1}, {0, 5}, {23, 5}, {22, 6}};
const char* SCH_TXT[4] = {"Mati", "00:00-05:00", "23:00-05:00", "22:00-06:00"};

struct Weather {
  bool valid;
  float temp, feels, wind, pres;
  int hum, code, isDay, pop, uv, aqi;
  int nh;
  float hTemp[12];
  int hPop[12];
  int hHour[12];
  int nd;
  float dMax[5], dMin[5];
  int dCode[5], dPop[5], dDow[5];
  int sunrise, sunset;
} wx;

int page = 0;
uint32_t frame = 0;
bool apMode = false, fetching = false, fetchNow = true;
bool inMenu = false;
int menuSel = 0;
String lastErr;
uint32_t restartAt = 0, lastInput = 0, lastRot = 0, lastFetch = 0, lastReconn = 0, lastGeo = 0, lastFrame = 0;

// backlight
uint8_t curBri = 255, tgtBri = 255;
bool screenOff = false, dimmed = false;
uint32_t lastBriCalc = 0, lastFade = 0;

// baterai
float vbat = 0, vTrend = 0;
bool batPresent = false, charging = false;
uint32_t lastBat = 0, lastTrend = 0;

uint16_t C_BG, C_PANEL, C_TXT, C_MUT, C_ACC, C_BLUE, C_TEAL, C_AREA, C_TRACK, C_CLOUD, C_CLOUD2;

static inline int iround(float v) { return (int)lroundf(v); }

// ---------------- prefs ----------------
void loadCfg() {
  prefs.begin("wx", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.city = prefs.getString("city", "Jakarta");
  cfg.lat = prefs.getFloat("lat", -6.2088f);
  cfg.lon = prefs.getFloat("lon", 106.8456f);
  cfg.ivl = prefs.getUShort("ivl", 10);
  cfg.tz = prefs.getInt("tz", 7 * 3600);
  cfg.needGeo = prefs.getBool("geo", false);
  cfg.briIdx = prefs.getUChar("bi", 0);
  cfg.saver = prefs.getUChar("sav", 0);
  cfg.sch = prefs.getUChar("sch", 0);
  cfg.autoDim = prefs.getBool("adim", true);
  cfg.showBat = prefs.getBool("bat", true);
  prefs.end();
  if (cfg.briIdx > 4) cfg.briIdx = 0;
  if (cfg.saver > 3) cfg.saver = 0;
  if (cfg.sch > 3) cfg.sch = 0;
}

void saveCfg() {
  prefs.begin("wx", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("city", cfg.city);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putUShort("ivl", cfg.ivl);
  prefs.putInt("tz", cfg.tz);
  prefs.putBool("geo", cfg.needGeo);
  prefs.putUChar("bi", cfg.briIdx);
  prefs.putUChar("sav", cfg.saver);
  prefs.putUChar("sch", cfg.sch);
  prefs.putBool("adim", cfg.autoDim);
  prefs.putBool("bat", cfg.showBat);
  prefs.end();
}

// ---------------- tema ----------------
void setTheme(bool day) {
  if (day) {
    C_BG = tft.color565(13, 40, 70);
    C_PANEL = tft.color565(23, 62, 100);
    C_AREA = tft.color565(21, 64, 108);
    C_TRACK = tft.color565(38, 80, 122);
    C_MUT = tft.color565(150, 175, 205);
  } else {
    C_BG = tft.color565(10, 14, 28);
    C_PANEL = tft.color565(22, 32, 56);
    C_AREA = tft.color565(19, 37, 68);
    C_TRACK = tft.color565(36, 52, 82);
    C_MUT = tft.color565(127, 147, 176);
  }
  C_TXT = tft.color565(232, 238, 247);
  C_ACC = tft.color565(255, 184, 77);
  C_BLUE = tft.color565(77, 163, 255);
  C_TEAL = tft.color565(61, 220, 151);
  C_CLOUD = tft.color565(207, 217, 232);
  C_CLOUD2 = tft.color565(140, 155, 178);
}

// ---------------- util cuaca & waktu ----------------
enum Ico { I_SUN, I_MOON, I_PART, I_PARTN, I_CLOUD, I_RAIN, I_STORM, I_SNOW, I_FOG };

int codeToIco(int c, bool day) {
  if (c == 0) return day ? I_SUN : I_MOON;
  if (c == 1 || c == 2) return day ? I_PART : I_PARTN;
  if (c == 3) return I_CLOUD;
  if (c == 45 || c == 48) return I_FOG;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return I_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return I_SNOW;
  if (c >= 95) return I_STORM;
  return I_CLOUD;
}

const char* codeText(int c) {
  switch (c) {
    case 0: return "Cerah";
    case 1: return "Cerah berawan";
    case 2: return "Berawan";
    case 3: return "Mendung";
    case 45: case 48: return "Berkabut";
    case 51: case 53: case 55: return "Gerimis";
    case 56: case 57: return "Gerimis beku";
    case 61: return "Hujan ringan";
    case 63: return "Hujan sedang";
    case 65: return "Hujan lebat";
    case 66: case 67: return "Hujan beku";
    case 71: case 73: case 75: case 77: return "Salju";
    case 80: return "Hujan lokal";
    case 81: return "Hujan deras";
    case 82: return "Hujan lebat";
    case 85: case 86: return "Hujan salju";
    case 95: return "Badai petir";
    case 96: case 99: return "Petir + es";
  }
  return "Berawan";
}

int dowOf(int y, int m, int d) {  // 0 = Minggu
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
const char* DOW[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};
const char* DAYFULL[] = {"Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"};
const char* MON[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};

int hhmm(const char* s) {
  if (!s || strlen(s) < 16) return 0;
  return atoi(s + 11) * 60 + atoi(s + 14);
}

bool nowTm(struct tm& t) { return getLocalTime(&t, 0) && t.tm_year >= 124; }

uint32_t fetchIntervalMs() {
  uint32_t m = cfg.ivl;
  if (cfg.saver >= 2 && m < 30) m = 30;
  return m * 60000UL;
}

// ---------------- baterai ----------------
void sampleBattery() {
  digitalWrite(ADC_EN, HIGH);
  delay(3);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogReadMilliVolts(BAT_PIN); delay(1); }
  digitalWrite(ADC_EN, LOW);
  int mv = (int)(sum / 8) * 2;  // pembagi tegangan 2:1
  batPresent = mv > 2800;       // di bawah itu: tanpa baterai / tidak terbaca
  if (!batPresent) return;
  float v = mv / 1000.0f;
  vbat = (vbat < 0.1f) ? v : (vbat * 0.7f + v * 0.3f);
  if (vbat > 4.19f) charging = true;
  if (millis() - lastTrend > 60000UL) {  // tren tegangan tiap 1 menit
    lastTrend = millis();
    if (vTrend > 0.1f) {
      float d = vbat - vTrend;
      if (d > 0.012f) charging = true;
      else if (d < -0.012f) charging = false;
    }
    vTrend = vbat;
  }
}

int batBars() {
  if (vbat > 4.05f) return 4;
  if (vbat > 3.88f) return 3;
  if (vbat > 3.72f) return 2;
  if (vbat > 3.58f) return 1;
  return 0;
}

void drawBattery(int x, int y) {
  if (!cfg.showBat || !batPresent) return;
  int bars = batBars();
  bool low = (bars <= 1 && !charging);
  uint16_t col = C_TXT;
  if (low && ((frame / 10) % 2)) col = spr.color565(240, 70, 70);
  spr.drawRoundRect(x, y, 15, 9, 2, col);
  spr.fillRect(x + 15, y + 3, 2, 3, col);
  int show = bars;
  if (charging && bars < 4) show = bars + (int)((frame / 8) % (uint32_t)(5 - bars));
  for (int i = 0; i < 4; i++) {
    uint16_t c = (i < show) ? (charging ? C_TEAL : col) : C_TRACK;
    spr.fillRect(x + 2 + i * 3, y + 2, 2, 5, c);
  }
  if (charging) {  // petir
    spr.fillTriangle(x + 8, y + 1, x + 5, y + 5, x + 8, y + 5, C_ACC);
    spr.fillTriangle(x + 7, y + 4, x + 10, y + 4, x + 7, y + 8, C_ACC);
  }
}

// ---------------- gambar ikon ----------------
void drawCloud(int x, int y, int s, uint16_t c) {
  spr.fillCircle(x - (int)(s * 0.6f), y + (int)(s * 0.15f), (int)(s * 0.45f), c);
  spr.fillCircle(x + (int)(s * 0.05f), y - (int)(s * 0.15f), (int)(s * 0.6f), c);
  spr.fillCircle(x + (int)(s * 0.6f), y + (int)(s * 0.1f), (int)(s * 0.5f), c);
  spr.fillRect(x - (int)(s * 0.6f), y + (int)(s * 0.1f), (int)(s * 1.2f), (int)(s * 0.5f), c);
}

void drawSun(int x, int y, int r, float rot) {
  spr.fillCircle(x, y, r, C_ACC);
  int a1 = r + 2, a2 = r + 3 + r / 3;
  for (int i = 0; i < 8; i++) {
    float a = rot + i * 0.7853982f;
    float c = cosf(a), s = sinf(a);
    int x1 = x + (int)(c * a1), y1 = y + (int)(s * a1);
    int x2 = x + (int)(c * a2), y2 = y + (int)(s * a2);
    spr.drawLine(x1, y1, x2, y2, C_ACC);
    if (r >= 8) spr.drawLine(x1 + 1, y1, x2 + 1, y2, C_ACC);
  }
}

void drawMoon(int x, int y, int r) {
  spr.fillCircle(x, y, r, C_CLOUD);
  spr.fillCircle(x + (int)(r * 0.5f), y - (int)(r * 0.35f), (int)(r * 0.8f), C_BG);
}

void drawDrops(int x, int y, int s, bool anim, bool snow) {
  int m = (int)(s * 0.8f) + 1;
  for (int i = 0; i < 3; i++) {
    int dx = x - (int)(s * 0.55f) + i * (int)(s * 0.55f);
    int off = anim ? (int)((frame * 2 + i * 9) % (uint32_t)m) : (int)(s * 0.2f) * (i % 2);
    int dy = y + off;
    if (snow) spr.fillCircle(dx, dy, max(1, s / 10), C_TXT);
    else spr.drawLine(dx, dy, dx - 2, dy + (int)(s * 0.3f) + 1, C_BLUE);
  }
}

void drawIcon(int ic, int x, int y, int s, bool anim) {
  int wob = anim ? (int)(sinf(frame * 0.06f) * s * 0.12f) : 0;
  float rot = anim ? frame * 0.03f : 0;
  switch (ic) {
    case I_SUN: drawSun(x, y, (int)(s * 0.6f), rot); break;
    case I_MOON: drawMoon(x, y, (int)(s * 0.65f)); break;
    case I_PART:
      drawSun(x - (int)(s * 0.5f), y - (int)(s * 0.5f), (int)(s * 0.45f), rot);
      drawCloud(x + (int)(s * 0.2f) + wob, y + (int)(s * 0.25f), (int)(s * 0.7f), C_CLOUD);
      break;
    case I_PARTN:
      drawMoon(x - (int)(s * 0.5f), y - (int)(s * 0.5f), (int)(s * 0.55f));
      drawCloud(x + (int)(s * 0.2f) + wob, y + (int)(s * 0.25f), (int)(s * 0.7f), C_CLOUD);
      break;
    case I_CLOUD: drawCloud(x + wob, y - (int)(s * 0.1f), s, C_CLOUD); break;
    case I_RAIN:
      drawCloud(x + wob, y - (int)(s * 0.35f), (int)(s * 0.85f), C_CLOUD2);
      drawDrops(x, y + (int)(s * 0.45f), s, anim, false);
      break;
    case I_SNOW:
      drawCloud(x + wob, y - (int)(s * 0.35f), (int)(s * 0.85f), C_CLOUD2);
      drawDrops(x, y + (int)(s * 0.45f), s, anim, true);
      break;
    case I_STORM: {
      drawCloud(x + wob, y - (int)(s * 0.35f), (int)(s * 0.85f), C_CLOUD2);
      if (!anim || (frame % 20) < 14) {
        int by = y + (int)(s * 0.2f);
        spr.fillTriangle(x + 2, by, x - 3, by + (int)(s * 0.5f), x + 1, by + (int)(s * 0.5f), C_ACC);
        spr.fillTriangle(x + 1, by + (int)(s * 0.45f), x + 5, by + (int)(s * 0.45f), x - 2, by + (int)(s * 0.95f), C_ACC);
      }
      break;
    }
    case I_FOG:
      drawCloud(x + wob, y - (int)(s * 0.4f), (int)(s * 0.7f), C_CLOUD2);
      for (int i = 0; i < 3; i++) {
        int ly = y + (int)(s * 0.35f) + i * max(2, (int)(s * 0.3f));
        spr.drawFastHLine(x - s + i * 2, ly, (int)(s * 1.6f), C_CLOUD);
        spr.drawFastHLine(x - s + i * 2, ly + 1, (int)(s * 1.6f), C_CLOUD);
      }
      break;
  }
}

int drawDegNum(int x, int y, int v, int font, uint16_t col) {
  char b[8];
  snprintf(b, sizeof(b), "%d", v);
  spr.setTextColor(col);
  spr.setTextDatum(TL_DATUM);
  int w = spr.drawString(b, x, y, font);
  int ry = (font >= 4) ? 5 : 3;
  spr.drawCircle(x + w + 3, y + ry, 2, col);
  return x + w + 7;
}

// angka suhu + derajat, dipusatkan di cx
void drawDegNumC(int cx, int y, int v, int font, uint16_t col) {
  char b[8];
  snprintf(b, sizeof(b), "%d", v);
  spr.setTextColor(col);
  spr.setTextDatum(TL_DATUM);
  int w = spr.textWidth(b, font);
  int x0 = cx - (w + 5) / 2;
  spr.drawString(b, x0, y, font);
  spr.drawCircle(x0 + w + 3, y + ((font >= 4) ? 5 : 3), 2, col);
}

// ---------------- komponen UI ----------------
void drawHeader(const char* title, bool showClock = true) {
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_MUT);
  spr.drawString(title, 6, 2, 2);
  for (int i = 0; i < PAGES; i++) {
    int dx = 100 + i * 10;
    if (i == page) spr.fillCircle(dx, 9, 3, C_ACC);
    else spr.fillCircle(dx, 9, 2, C_TRACK);
  }
  drawBattery(showClock ? 150 : 198, 4);
  if (showClock) {
    struct tm ti;
    char b[8];
    if (nowTm(ti)) snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min);
    else strcpy(b, "--:--");
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(C_TXT);
    spr.drawString(b, 236, 2, 2);
  }
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int r = WiFi.RSSI();
    bars = r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
  }
  int wx0 = showClock ? 176 : 222;
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 2;
    spr.fillRect(wx0 + i * 4, 14 - h, 3, h, i < bars ? C_TEAL : C_TRACK);
  }
  if (fetching) spr.fillCircle(showClock ? 170 : 192, 9, 2, C_ACC);
}

void drawSpinner(int cx, int cy) {
  for (int i = 0; i < 8; i++) {
    float a = frame * 0.15f + i * 0.7853982f;
    int x = cx + (int)(cosf(a) * 16), y = cy + (int)(sinf(a) * 16);
    spr.fillCircle(x, y, i > 5 ? 3 : 2, spr.color565(40 + i * 25, 60 + i * 20, 90 + i * 20));
  }
}

void drawLoading() {
  drawSpinner(120, 62);
  spr.setTextDatum(MC_DATUM);
  if (lastErr.length()) {
    spr.setTextColor(C_ACC);
    spr.drawString("Gagal: " + lastErr, 120, 104, 2);
  } else {
    spr.setTextColor(C_MUT);
    spr.drawString("Memuat data cuaca...", 120, 104, 2);
  }
}

void chip(int x, int y, int w, int kind, const char* txt, uint16_t col) {
  spr.fillRoundRect(x, y, w, 24, 5, C_PANEL);
  int cx = x + 11, cy = y + 12;  // pusat ikon
  if (kind == 0) {  // tetes
    spr.fillCircle(cx, cy + 2, 4, col);
    spr.fillTriangle(cx, cy - 6, cx - 4, cy + 1, cx + 4, cy + 1, col);
  } else if (kind == 1) {  // angin
    spr.fillRoundRect(cx - 6, cy - 5, 11, 2, 1, col);
    spr.fillRoundRect(cx - 6, cy - 1, 14, 2, 1, col);
    spr.fillRoundRect(cx - 6, cy + 3, 8, 2, 1, col);
  } else {  // payung
    spr.fillCircle(cx, cy + 1, 7, col);
    spr.fillRect(cx - 8, cy + 1, 17, 8, C_PANEL);
    spr.drawFastVLine(cx, cy + 1, 7, col);
    spr.drawPixel(cx - 1, cy + 8, col);
    spr.drawPixel(cx - 2, cy + 7, col);
  }
  spr.setTextColor(C_TXT);
  spr.setTextDatum(ML_DATUM);
  spr.drawString(txt, x + 24, y + 12, 2);
}

// ---------------- halaman ----------------
// 0: jam besar + cuaca ringkas
void pageHome() {
  char city[16];
  strncpy(city, cfg.city.c_str(), 13);
  city[13] = 0;
  drawHeader(city, false);

  struct tm ti;
  bool ok = nowTm(ti);
  char b[24];
  if (ok) snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min);
  else strcpy(b, "--:--");
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TXT);
  int w = spr.drawString(b, 6, 28, 6);
  if (ok) {
    snprintf(b, sizeof(b), "%02d", ti.tm_sec);
    spr.setTextColor(C_ACC);
    spr.drawString(b, 6 + w + 4, 54, 4);
    snprintf(b, sizeof(b), "%s, %d %s", DAYFULL[ti.tm_wday % 7], ti.tm_mday, MON[ti.tm_mon % 12]);
    spr.setTextColor(C_MUT);
    spr.drawString(b, 8, 82, 2);
  }

  if (wx.valid) {
    const int IC = 200;  // sumbu tengah kolom cuaca
    drawIcon(codeToIco(wx.code, wx.isDay), IC, 38, 16, true);
    drawDegNumC(IC, 56, iround(wx.temp), 4, C_TXT);
    const char* ct = codeText(wx.code);
    int cw = spr.textWidth(ct, 2);
    int tx = IC;
    if (tx - cw / 2 < 126) tx = 126 + cw / 2;
    if (tx + cw / 2 > 236) tx = 236 - cw / 2;
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(C_ACC);
    spr.drawString(ct, tx, 89, 2);
    snprintf(b, sizeof(b), "%d%%", wx.hum);
    chip(6, 108, 72, 0, b, C_BLUE);
    snprintf(b, sizeof(b), "%dkm/j", iround(wx.wind));
    chip(84, 108, 72, 1, b, C_TEAL);
    snprintf(b, sizeof(b), "%d%%", wx.pop);
    chip(162, 108, 72, 2, b, C_ACC);
  } else {
    drawSpinner(200, 50);
    spr.setTextDatum(TR_DATUM);
    if (lastErr.length()) {
      spr.setTextColor(C_ACC);
      spr.drawString(lastErr, 236, 88, 2);
    } else {
      spr.setTextColor(C_MUT);
      spr.drawString("Memuat...", 236, 88, 2);
    }
  }
}

// 1: grafik per jam
void pageHourly() {
  drawHeader("Per jam");
  if (!wx.valid || wx.nh < 2) { drawLoading(); return; }
  int n = wx.nh;
  float mn = wx.hTemp[0], mx = wx.hTemp[0];
  for (int i = 1; i < n; i++) {
    if (wx.hTemp[i] < mn) mn = wx.hTemp[i];
    if (wx.hTemp[i] > mx) mx = wx.hTemp[i];
  }
  if (mx - mn < 4) { float m = (mx + mn) / 2; mn = m - 2; mx = m + 2; }
  const int X0 = 16, X1 = 224, YT = 46, YB = 84, BASE = 96;
  int px[12], py[12];
  for (int i = 0; i < n; i++) {
    px[i] = X0 + i * (X1 - X0) / (n - 1);
    py[i] = YB - (int)((wx.hTemp[i] - mn) / (mx - mn) * (YB - YT));
  }
  int seg = 0, prevy = py[0];
  for (int x = X0; x <= X1; x++) {
    while (seg < n - 2 && x >= px[seg + 1]) seg++;
    float t = (float)(x - px[seg]) / (float)(px[seg + 1] - px[seg]);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float s = (1.0f - cosf(t * PI)) * 0.5f;
    int y = py[seg] + (int)((py[seg + 1] - py[seg]) * s);
    spr.drawFastVLine(x, y, BASE - y, C_AREA);
    if (x > X0) {
      spr.drawLine(x - 1, prevy, x, y, C_BLUE);
      spr.drawLine(x - 1, prevy + 1, x, y + 1, C_BLUE);
    }
    prevy = y;
  }
  for (int i = 0; i < n; i++) {
    bool lab = (i % 2 == 0);
    spr.fillCircle(px[i], py[i], lab ? 3 : 2, C_ACC);
    char b[8];
    if (lab) {
      snprintf(b, sizeof(b), "%d", iround(wx.hTemp[i]));
      spr.setTextDatum(BC_DATUM);
      spr.setTextColor(C_TXT);
      spr.drawString(b, px[i], py[i] - 6, 1);
      snprintf(b, sizeof(b), "%02d", wx.hHour[i]);
      spr.setTextDatum(TC_DATUM);
      spr.setTextColor(C_MUT);
      spr.drawString(b, px[i], 120, 1);
    }
    int h = wx.hPop[i] * 14 / 100;
    if (h < 1) h = 1;
    spr.fillRect(px[i] - 3, 114 - h, 6, h, wx.hPop[i] > 0 ? C_TEAL : C_TRACK);
  }
}

// 2: 5 hari
void pageDays() {
  drawHeader("5 hari");
  if (!wx.valid || wx.nd < 1) { drawLoading(); return; }
  float mn = wx.dMin[0], mx = wx.dMax[0];
  for (int i = 1; i < wx.nd; i++) {
    if (wx.dMin[i] < mn) mn = wx.dMin[i];
    if (wx.dMax[i] > mx) mx = wx.dMax[i];
  }
  if (mx - mn < 3) mx = mn + 3;
  const int bx = 90, bw = 72;
  for (int r = 0; r < wx.nd; r++) {
    int y = 22 + r * 21;
    if (r % 2 == 0) spr.fillRoundRect(2, y - 1, 236, 20, 4, C_PANEL);
    drawIcon(codeToIco(wx.dCode[r], true), 18, y + 9, 7, false);
    char b[8];
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(r == 0 ? C_ACC : C_TXT);
    spr.drawString(DOW[wx.dDow[r] % 7], 32, y + 10, 2);
    spr.setTextColor(C_MUT);
    snprintf(b, sizeof(b), "%d", iround(wx.dMin[r]));
    spr.drawString(b, 64, y + 10, 2);
    int x1 = bx + (int)((wx.dMin[r] - mn) / (mx - mn) * bw);
    int x2 = bx + (int)((wx.dMax[r] - mn) / (mx - mn) * bw);
    int wb = max(x2 - x1, 5);
    spr.fillRoundRect(bx, y + 7, bw, 6, 3, C_TRACK);
    spr.fillRoundRect(x1, y + 7, wb, 6, 3, wx.dPop[r] >= 50 ? C_BLUE : C_ACC);
    spr.setTextColor(C_TXT);
    snprintf(b, sizeof(b), "%d", iround(wx.dMax[r]));
    spr.drawString(b, 168, y + 10, 2);
    spr.setTextDatum(MR_DATUM);
    spr.setTextColor(C_TEAL);
    snprintf(b, sizeof(b), "%d%%", wx.dPop[r]);
    spr.drawString(b, 234, y + 10, 2);
  }
}

// 3: matahari, UV, AQI
void pageSun() {
  drawHeader("Matahari");
  if (!wx.valid) { drawLoading(); return; }
  const int cx = 82, cy = 98, R = 56;
  struct tm ti;
  int nowMin = -1;
  if (nowTm(ti)) nowMin = ti.tm_hour * 60 + ti.tm_min;
  int span = wx.sunset - wx.sunrise;
  if (span < 60) span = 720;
  float f = (nowMin < 0) ? 0.5f : (float)(nowMin - wx.sunrise) / (float)span;
  bool up = (f >= 0.0f && f <= 1.0f);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  spr.drawFastHLine(14, cy, 136, C_TRACK);
  int done = (int)(f * 180);
  for (int a = 0; a <= 180; a += 6) {
    float rad = a * PI / 180.0f;
    int x = cx - (int)(cosf(rad) * R), y = cy - (int)(sinf(rad) * R);
    if (a <= done && up) spr.fillCircle(x, y, 1, C_ACC);
    else spr.drawPixel(x, y, C_MUT);
  }
  int sx = cx - (int)(cosf(f * PI) * R), sy = cy - (int)(sinf(f * PI) * R);
  if (up) drawSun(sx, sy, 6, frame * 0.03f);
  else drawMoon(sx, sy - 4, 6);
  char b[16];
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(C_MUT);
  snprintf(b, sizeof(b), "%02d:%02d", wx.sunrise / 60, wx.sunrise % 60);
  spr.drawString(b, 22, 104, 2);
  snprintf(b, sizeof(b), "%02d:%02d", wx.sunset / 60, wx.sunset % 60);
  spr.drawString(b, 142, 104, 2);
  int dm = wx.sunset - wx.sunrise;
  if (dm > 0) {
    snprintf(b, sizeof(b), "%dj %dm", dm / 60, dm % 60);
    spr.setTextColor(C_TXT);
    spr.drawString(b, cx, 76, 2);
  }

  const int RX = 160;
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_MUT);
  spr.drawString("UV", RX, 22, 1);
  uint16_t uvc[5] = {spr.color565(61, 220, 151), spr.color565(216, 224, 74), spr.color565(255, 184, 77),
                     spr.color565(255, 122, 69), spr.color565(240, 70, 70)};
  int ui = wx.uv <= 2 ? 0 : wx.uv <= 5 ? 1 : wx.uv <= 7 ? 2 : wx.uv <= 10 ? 3 : 4;
  const char* uvt[5] = {"Rendah", "Sedang", "Tinggi", "Sangat tinggi", "Ekstrem"};
  snprintf(b, sizeof(b), "%d", wx.uv);
  spr.setTextColor(uvc[ui]);
  spr.drawString(b, RX, 31, 4);
  for (int i = 0; i < 5; i++) spr.fillRect(RX + i * 15, 62, 13, 5, uvc[i]);
  int mxp = RX + ui * 15 + 6;
  spr.fillTriangle(mxp - 3, 56, mxp + 3, 56, mxp, 60, C_TXT);
  spr.setTextColor(uvc[ui]);
  spr.drawString(uvt[ui], RX, 70, 1);

  spr.setTextColor(C_MUT);
  spr.drawString("AQI", RX, 84, 1);
  if (wx.aqi >= 0) {
    int ai = wx.aqi <= 50 ? 0 : wx.aqi <= 100 ? 1 : wx.aqi <= 150 ? 2 : wx.aqi <= 200 ? 3 : wx.aqi <= 300 ? 4 : 5;
    uint16_t ac[6] = {spr.color565(61, 220, 151), spr.color565(216, 224, 74), spr.color565(255, 150, 60),
                      spr.color565(240, 70, 70), spr.color565(180, 90, 220), spr.color565(160, 40, 80)};
    const char* at[6] = {"Baik", "Sedang", "Sensitif", "Tidak sehat", "Sgt tdk sehat", "Berbahaya"};
    snprintf(b, sizeof(b), "%d", wx.aqi);
    spr.setTextColor(ac[ai]);
    spr.drawString(b, RX, 93, 4);
    spr.drawString(at[ai], RX, 120, 1);
  } else {
    spr.setTextColor(C_MUT);
    spr.drawString("--", RX, 93, 4);
  }
  snprintf(b, sizeof(b), "%dhPa", iround(wx.pres));
  spr.setTextColor(C_MUT);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(b, 236, 120, 1);
}

// 4: jam analog
void pageAnalog() {
  drawHeader("Jam", false);
  struct tm ti;
  bool ok = nowTm(ti);
  const int cx = 64, cy = 76, R = 50;
  spr.drawCircle(cx, cy, R, C_TRACK);
  spr.drawCircle(cx, cy, R - 1, C_TRACK);
  for (int i = 0; i < 60; i++) {
    float a = i * 0.10471976f;
    float s = sinf(a), c = cosf(a);
    if (i % 5 == 0) {
      bool q = (i % 15 == 0);
      int r1 = R - (q ? 10 : 7), r2 = R - 3;
      uint16_t col = q ? C_ACC : C_TXT;
      spr.drawLine(cx + (int)(s * r1), cy - (int)(c * r1), cx + (int)(s * r2), cy - (int)(c * r2), col);
      if (q) spr.drawLine(cx + (int)(s * r1) + 1, cy - (int)(c * r1), cx + (int)(s * r2) + 1, cy - (int)(c * r2), col);
    } else {
      spr.drawPixel(cx + (int)(s * (R - 4)), cy - (int)(c * (R - 4)), C_MUT);
    }
  }
  char b[24];
  if (ok) {
    float hDeg = ((ti.tm_hour % 12) + ti.tm_min / 60.0f) * 30.0f;
    float mDeg = (ti.tm_min + ti.tm_sec / 60.0f) * 6.0f;
    float sDeg = ti.tm_sec * 6.0f;
    float ha = hDeg * 0.017453293f, ma = mDeg * 0.017453293f, sa = sDeg * 0.017453293f;
    int hx = cx + (int)(sinf(ha) * 28), hy = cy - (int)(cosf(ha) * 28);
    int mx = cx + (int)(sinf(ma) * 40), my = cy - (int)(cosf(ma) * 40);
    int sx = cx + (int)(sinf(sa) * 46), sy = cy - (int)(cosf(sa) * 46);
    int tx = cx - (int)(sinf(sa) * 9), ty = cy + (int)(cosf(sa) * 9);
    spr.drawLine(cx, cy, hx, hy, C_TXT);
    spr.drawLine(cx + 1, cy, hx + 1, hy, C_TXT);
    spr.drawLine(cx, cy + 1, hx, hy + 1, C_TXT);
    spr.drawLine(cx, cy, mx, my, C_TXT);
    spr.drawLine(cx + 1, cy, mx + 1, my, C_TXT);
    spr.drawLine(tx, ty, sx, sy, C_ACC);
    snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min);
  } else {
    strcpy(b, "--:--");
  }
  spr.fillCircle(cx, cy, 3, C_ACC);

  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(C_TXT);
  spr.drawString(b, 181, 30, 4);
  if (ok) {
    snprintf(b, sizeof(b), "%s, %d %s", DAYFULL[ti.tm_wday % 7], ti.tm_mday, MON[ti.tm_mon % 12]);
    spr.setTextColor(C_MUT);
    spr.drawString(b, 181, 64, 2);
  }
  if (wx.valid) {
    drawIcon(codeToIco(wx.code, wx.isDay), 148, 106, 9, true);
    drawDegNum(168, 94, iround(wx.temp), 4, C_TXT);
  }
}

// ---------------- menu pengaturan ----------------
void pageMenu() {
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_MUT);
  spr.drawString("PENGATURAN", 6, 2, 2);
  const char* L[MENU_N] = {"Kecerahan", "Mode hemat", "Redup malam", "Layar mati", "Ikon baterai", "< Kembali"};
  for (int i = 0; i < MENU_N; i++) {
    int y = 19 + i * 17;
    if (i == menuSel) {
      spr.fillRoundRect(4, y, 232, 16, 4, C_PANEL);
      spr.fillRect(4, y, 3, 16, C_ACC);
    }
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(i == MENU_N - 1 ? C_ACC : C_TXT);
    spr.drawString(L[i], 14, y, 2);
    if (i == 0) {
      for (int k = 0; k < 5; k++) spr.fillRoundRect(170 + k * 13, y + 4, 11, 8, 2, k < 5 - cfg.briIdx ? C_ACC : C_TRACK);
    } else if (i < MENU_N - 1) {
      const char* v = "";
      uint16_t col = C_TXT;
      switch (i) {
        case 1: v = SAV_TXT[cfg.saver]; col = cfg.saver ? C_TEAL : C_MUT; break;
        case 2: v = cfg.autoDim ? "Aktif" : "Mati"; col = cfg.autoDim ? C_TEAL : C_MUT; break;
        case 3: v = SCH_TXT[cfg.sch]; col = cfg.sch ? C_TXT : C_MUT; break;
        default: v = cfg.showBat ? "Tampil" : "Sembunyi"; col = cfg.showBat ? C_TEAL : C_MUT; break;
      }
      spr.setTextDatum(TR_DATUM);
      spr.setTextColor(col);
      spr.drawString(v, 230, y, 2);
    }
  }
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(C_MUT);
  spr.drawString("bawah: geser   atas: ubah / pilih", 120, 124, 1);
}

void renderPage(int p) {
  setTheme(wx.valid ? (wx.isDay != 0) : true);
  spr.fillSprite(C_BG);
  switch (p) {
    case 0: pageHome(); break;
    case 1: pageHourly(); break;
    case 2: pageDays(); break;
    case 3: pageSun(); break;
    default: pageAnalog(); break;
  }
}

void renderMenu() {
  setTheme(wx.valid ? (wx.isDay != 0) : true);
  spr.fillSprite(C_BG);
  pageMenu();
}

void goPage(int p) {
  page = p;
  renderPage(page);
  for (int i = 1; i <= 7; i++) {
    float t = i / 7.0f;
    float e = 1.0f - (1.0f - t) * (1.0f - t);
    spr.pushSprite((int)(SW * (1.0f - e)), 0);
    delay(12);
  }
  spr.pushSprite(0, 0);
}

void msgScreen(const char* t, const char* a, const char* b) {
  setTheme(true);
  spr.fillSprite(C_BG);
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(C_ACC);
  spr.drawString(t, 120, 14, 4);
  spr.setTextColor(C_TXT);
  spr.drawString(a, 120, 62, 2);
  spr.setTextColor(C_MUT);
  spr.drawString(b, 120, 86, 2);
  spr.pushSprite(0, 0);
}

void drawSetup() {
  setTheme(true);
  spr.fillSprite(C_BG);
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(C_ACC);
  spr.drawString("SETUP", 120, 6, 4);
  spr.setTextColor(C_MUT);
  spr.drawString("1. Sambung HP ke WiFi", 120, 40, 2);
  spr.setTextColor(C_TXT);
  spr.drawString(AP_NAME, 120, 58, 4);
  spr.setTextColor(C_MUT);
  spr.drawString("2. Buka 192.168.4.1", 120, 92, 2);
  spr.drawString("atau tunggu portal muncul", 120, 112, 2);
  spr.pushSprite(0, 0);
}

// ---------------- jaringan & data ----------------
bool httpGetJson(const char* url, JsonDocument& doc) {
  WiFiClientSecure cl;
  cl.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(8000);
  if (!http.begin(cl, url)) { lastErr = "begin"; return false; }
  int code = http.GET();
  if (code != 200) {
    lastErr = "HTTP " + String(code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  DeserializationError e = deserializeJson(doc, body);
  if (e) { lastErr = "JSON"; return false; }
  return true;
}

bool fetchWeather() {
  char url[512];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m,surface_pressure"
           "&hourly=temperature_2m,precipitation_probability"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max"
           "&timezone=auto&forecast_days=5&forecast_hours=12",
           (double)cfg.lat, (double)cfg.lon);
  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;

  Weather n;
  memset(&n, 0, sizeof(n));
  n.aqi = wx.aqi;
  JsonObject c = doc["current"].as<JsonObject>();
  if (c.isNull()) { lastErr = "data kosong"; return false; }
  n.temp = c["temperature_2m"] | 0.0f;
  n.feels = c["apparent_temperature"] | 0.0f;
  n.hum = iround(c["relative_humidity_2m"] | 0.0f);
  n.isDay = iround(c["is_day"] | 1.0f);
  n.code = iround(c["weather_code"] | 0.0f);
  n.wind = c["wind_speed_10m"] | 0.0f;
  n.pres = c["surface_pressure"] | 0.0f;

  JsonArray ht = doc["hourly"]["temperature_2m"].as<JsonArray>();
  JsonArray hp = doc["hourly"]["precipitation_probability"].as<JsonArray>();
  JsonArray hti = doc["hourly"]["time"].as<JsonArray>();
  n.nh = min((int)ht.size(), 12);
  if (n.nh < 2) { lastErr = "data kosong"; return false; }
  for (int i = 0; i < n.nh; i++) {
    n.hTemp[i] = ht[i] | 0.0f;
    n.hPop[i] = iround(hp[i] | 0.0f);
    const char* ts = hti[i] | "";
    n.hHour[i] = (strlen(ts) >= 13) ? atoi(ts + 11) : 0;
  }
  n.pop = n.hPop[0];

  JsonArray dc = doc["daily"]["weather_code"].as<JsonArray>();
  JsonArray dx = doc["daily"]["temperature_2m_max"].as<JsonArray>();
  JsonArray dn = doc["daily"]["temperature_2m_min"].as<JsonArray>();
  JsonArray dp = doc["daily"]["precipitation_probability_max"].as<JsonArray>();
  JsonArray dt = doc["daily"]["time"].as<JsonArray>();
  n.nd = min((int)dx.size(), 5);
  for (int i = 0; i < n.nd; i++) {
    n.dCode[i] = iround(dc[i] | 0.0f);
    n.dMax[i] = dx[i] | 0.0f;
    n.dMin[i] = dn[i] | 0.0f;
    n.dPop[i] = iround(dp[i] | 0.0f);
    const char* d = dt[i] | "";
    n.dDow[i] = (strlen(d) >= 10) ? dowOf(atoi(d), atoi(d + 5), atoi(d + 8)) : 0;
  }
  const char* sr = doc["daily"]["sunrise"][0] | "";
  const char* ss = doc["daily"]["sunset"][0] | "";
  n.sunrise = hhmm(sr);
  n.sunset = hhmm(ss);
  n.uv = iround(doc["daily"]["uv_index_max"][0] | 0.0f);

  int off = doc["utc_offset_seconds"] | cfg.tz;
  n.valid = true;
  wx = n;
  lastErr = "";
  if (off != cfg.tz) {
    cfg.tz = off;
    prefs.begin("wx", false);
    prefs.putInt("tz", off);
    prefs.end();
    configTime(cfg.tz, 0, "pool.ntp.org", "time.google.com");
  }
  return true;
}

bool fetchAqi() {
  char url[200];
  snprintf(url, sizeof(url),
           "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f&current=us_aqi",
           (double)cfg.lat, (double)cfg.lon);
  JsonDocument doc;
  String keep = lastErr;
  if (!httpGetJson(url, doc)) { lastErr = keep; return false; }
  float a = doc["current"]["us_aqi"] | -1.0f;
  wx.aqi = (a < 0) ? -1 : iround(a);
  return true;
}

String urlEnc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') o += c;
    else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      o += b;
    }
  }
  return o;
}

bool geocode() {
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=id&format=json&name=" + urlEnc(cfg.city);
  JsonDocument doc;
  if (!httpGetJson(url.c_str(), doc)) return false;
  JsonObject r = doc["results"][0].as<JsonObject>();
  if (r.isNull()) { lastErr = "kota tidak ada"; return false; }
  cfg.lat = r["latitude"] | cfg.lat;
  cfg.lon = r["longitude"] | cfg.lon;
  const char* nm = r["name"] | "";
  if (strlen(nm)) cfg.city = String(nm);
  cfg.needGeo = false;
  saveCfg();
  return true;
}

// ---------------- web server ----------------
String hesc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') o += "&quot;";
    else if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

String jesc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    if ((unsigned char)c >= 32) o += c;
  }
  return o;
}

void handleRoot() {
  String h = INDEX_HTML;
  h.replace("%SSID%", hesc(cfg.ssid));
  h.replace("%CITY%", hesc(cfg.city));
  h.replace("%LAT%", String(cfg.lat, 4));
  h.replace("%LON%", String(cfg.lon, 4));
  h.replace("%IVL%", String(cfg.ivl));
  h.replace("%BRI%", String(cfg.briIdx));
  h.replace("%SAV%", String(cfg.saver));
  h.replace("%ADIM%", cfg.autoDim ? "1" : "0");
  h.replace("%SCH%", String(cfg.sch));
  h.replace("%BAT%", cfg.showBat ? "1" : "0");
  server.send(200, "text/html; charset=utf-8", h);
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    j += "\"" + jesc(WiFi.SSID(i)) + "\"";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

void handleSave() {
  String s = server.arg("ssid"), p = server.arg("pass"), city = server.arg("city");
  String la = server.arg("lat"), lo = server.arg("lon");
  s.trim();
  city.trim();
  if (s.length()) {
    if (!(s == cfg.ssid && p.length() == 0)) cfg.pass = p;
    cfg.ssid = s;
  }
  bool have = la.length() && lo.length();
  bool cityChanged = city.length() && city != cfg.city;
  float nl = la.toFloat(), no = lo.toFloat();
  if (have && !(cityChanged && fabsf(nl - cfg.lat) < 0.0001f && fabsf(no - cfg.lon) < 0.0001f)) {
    cfg.lat = nl;
    cfg.lon = no;
    cfg.needGeo = false;
  } else if (cityChanged) {
    cfg.needGeo = true;
  }
  if (city.length()) cfg.city = city;
  int iv = server.arg("ivl").toInt();
  if (iv >= 5 && iv <= 120) cfg.ivl = iv;
  if (server.hasArg("bri")) cfg.briIdx = constrain(server.arg("bri").toInt(), 0, 4);
  if (server.hasArg("sav")) cfg.saver = constrain(server.arg("sav").toInt(), 0, 3);
  if (server.hasArg("adim")) cfg.autoDim = server.arg("adim").toInt() != 0;
  if (server.hasArg("sch")) cfg.sch = constrain(server.arg("sch").toInt(), 0, 3);
  if (server.hasArg("bat")) cfg.showBat = server.arg("bat").toInt() != 0;
  saveCfg();
  server.send(200, "text/html; charset=utf-8", SAVED_HTML);
  restartAt = millis() + 1500;
}

void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// ---------------- WiFi & daya ----------------
void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME);
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
}

bool connectSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("weather");
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    frame++;
    setTheme(true);
    spr.fillSprite(C_BG);
    drawSpinner(120, 56);
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(C_TXT);
    spr.drawString("Menyambung WiFi", 120, 90, 2);
    spr.setTextColor(C_MUT);
    spr.drawString(cfg.ssid.substring(0, 20), 120, 110, 2);
    spr.pushSprite(0, 0);
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

void applyPower() {
  if (apMode) return;
  setCpuFrequencyMhz(cfg.saver >= 1 ? 80 : 240);
  WiFi.setSleep(cfg.saver >= 1);
}

void resetWifi() {
  cfg.ssid = "";
  cfg.pass = "";
  saveCfg();
  msgScreen("Reset WiFi", "Kredensial dihapus", "Restart...");
  delay(900);
  ESP.restart();
}

// ---------------- backlight ----------------
bool inSchedule(int hour) {
  int f = SCH[cfg.sch].from, t = SCH[cfg.sch].to;
  if (f < 0) return false;
  if (f < t) return hour >= f && hour < t;
  return hour >= f || hour < t;
}

uint8_t calcTarget() {
  int t = BRI[cfg.briIdx];
  if (cfg.autoDim && wx.valid && !wx.isDay) t = max(8, t / 4);
  uint32_t idle = millis() - lastInput;
  dimmed = false;
  screenOff = false;
  if (inMenu) return (uint8_t)t;
  if (cfg.saver >= 1 && idle > 60000UL) {
    if (t > 35) t = 35;
    dimmed = true;
  }
  if (cfg.saver == 3 && idle > 300000UL) {
    t = 0;
    screenOff = true;
  }
  struct tm ti;
  if (cfg.sch && idle > 30000UL && nowTm(ti) && inSchedule(ti.tm_hour)) {
    t = 0;
    screenOff = true;
  }
  return (uint8_t)t;
}

void updateBacklight() {
  if (millis() - lastBriCalc > 200) {
    lastBriCalc = millis();
    tgtBri = calcTarget();
  }
  if (curBri != tgtBri && millis() - lastFade >= 12) {
    lastFade = millis();
    int d = (int)tgtBri - (int)curBri;
    if (d > 16) d = 16;
    if (d < -16) d = -16;
    curBri = (uint8_t)((int)curBri + d);
    ledcWrite(0, curBri);
  }
}

// ---------------- tombol & menu ----------------
// Tombol bawah, pola yang sama dengan tombol atas (klik pertama langsung beraksi).
// Return: 0, 1 klik, 2 tahan (longMs), 3 klik ganda (penekanan kedua < 450 ms).
bool prevBot = true, haveBot = false, botLongDone = false;
uint32_t lastBotPress = 0, botDownAt = 0;

int pollBot(uint32_t longMs) {
  bool b = digitalRead(BTN1);
  uint32_t now = millis();
  int ev = 0;
  if (!b && prevBot && now - lastBotPress >= 60) {  // tekan (buang bounce < 60 ms)
    botDownAt = now;
    botLongDone = false;
    if (haveBot && now - lastBotPress < 450) { haveBot = false; ev = 3; }
    else { haveBot = true; ev = 1; }
    lastBotPress = now;
  }
  if (!b && !botLongDone && botDownAt && now - botDownAt >= longMs) {
    botLongDone = true;
    haveBot = false;
    ev = 2;
  }
  if (b) botDownAt = 0;
  prevBot = b;
  return ev;
}

// ---------------- tombol atas + power off (pola Emochi) ----------------
// Klik langsung terdeteksi saat ditekan. Klik kedua dalam 450 ms = klik ganda (power off),
// dideteksi saat penekanan kedua. Return: 0, 1 klik, 3 ganda.
bool prevTop = true, haveTop = false;
uint32_t lastTopPress = 0;
uint32_t refreshAt = 0;  // refresh data ditunda sebentar supaya klik ganda tidak terganggu fetch

int pollTop() {
  bool top = digitalRead(BTN2);
  uint32_t now = millis();
  int ev = 0;
  if (!top && prevTop && now - lastTopPress >= 60) {  // tekan (buang bounce < 60 ms)
    if (haveTop && now - lastTopPress < 450) { haveTop = false; ev = 3; }
    else { haveTop = true; ev = 1; }
    lastTopPress = now;
  }
  prevTop = top;
  return ev;
}

void goSleep() {
  msgScreen("Mati", "Tekan tombol atas", "untuk menyala");
  delay(900);
  while (digitalRead(BTN2) == LOW) delay(10);  // tunggu tombol dilepas
  delay(80);
  ledcWrite(0, 0);
  ledcDetachPin(TFT_BL);  // lepas PWM supaya pin benar-benar bisa di-LOW-kan
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);
  tft.writecommand(TFT_DISPOFF);
  tft.writecommand(TFT_SLPIN);
  digitalWrite(ADC_EN, LOW);
  delay(120);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_35, 0);  // klik tombol atas = nyala
  esp_deep_sleep_start();
}

void exitMenu() {
  inMenu = false;
  saveCfg();
}

// e1 = tombol bawah (1 geser, 2 tahan = keluar), e2 = tombol atas (1 ubah / pilih)
void menuInput(int e1, int e2) {
  if (e1 == 1) menuSel = (menuSel + 1) % MENU_N;
  if (e1 == 2) { exitMenu(); return; }
  if (e2 == 1) {
    switch (menuSel) {
      case 0: cfg.briIdx = (cfg.briIdx + 1) % 5; break;
      case 1: cfg.saver = (cfg.saver + 1) % 4; applyPower(); break;
      case 2: cfg.autoDim = !cfg.autoDim; break;
      case 3: cfg.sch = (cfg.sch + 1) % 4; break;
      case 4: cfg.showBat = !cfg.showBat; break;
      default: exitMenu(); break;
    }
  }
}

// ---------------- setup & loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(ADC_EN, OUTPUT);
  digitalWrite(ADC_EN, LOW);  // HIGH hanya saat mengukur baterai
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT);  // GPIO35: pull-up eksternal di board
  prevTop = digitalRead(BTN2);  // supaya klik yang membangunkan tidak terhitung
  prevBot = digitalRead(BTN1);
  analogSetPinAttenuation(BAT_PIN, ADC_11db);

  tft.init();
  tft.setRotation(ROT);
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(16);
  if (!spr.createSprite(SW, SH)) {
    tft.drawString("Sprite gagal (RAM)", 10, 10, 2);
    while (1) delay(1000);
  }
  memset(&wx, 0, sizeof(wx));
  wx.aqi = -1;
  loadCfg();
  curBri = tgtBri = BRI[cfg.briIdx];
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, curBri);
  setTheme(true);
  delay(30);
  sampleBattery();

  msgScreen("Weather", "T-Display Station", "memulai...");
  delay(600);

  bool ok = false;
  if (cfg.ssid.length() == 0) {
    startAP();
  } else if (connectSTA()) {
    ok = true;
  } else {
    msgScreen("WiFi gagal", "Masuk mode setup", AP_NAME);
    delay(1500);
    startAP();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  if (ok) {
    if (MDNS.begin("weather")) MDNS.addService("http", "tcp", 80);
    configTime(cfg.tz, 0, "pool.ntp.org", "time.google.com");
    msgScreen("Terhubung", WiFi.localIP().toString().c_str(), "atau weather.local");
    delay(1800);
    fetchNow = true;
  }
  applyPower();
  lastInput = millis();
  lastRot = millis();
}

void loop() {
  server.handleClient();
  if (apMode) dns.processNextRequest();
  if (restartAt && millis() > restartAt) ESP.restart();

  int e1 = pollBot(inMenu ? 1000 : 3000);  // tombol bawah
  int e2 = pollTop();                 // tombol atas (klik ganda = power off)
  if (e1 || e2) {
    if (screenOff) { e1 = 0; e2 = 0; }  // tombol pertama hanya membangunkan layar
    lastInput = millis();
    lastRot = millis();
  }
  if (e1 == 2 && !inMenu) resetWifi();
  if (e2 == 3) refreshAt = 0;
  if (e2 == 3 && !inMenu) goSleep();

  if (apMode) {
    // di mode setup: kalau WiFi tersimpan ternyata nyambung, restart ke mode normal
    if (cfg.ssid.length() && WiFi.status() == WL_CONNECTED && !restartAt) restartAt = millis() + 500;
    if (millis() - lastFrame >= 250) { lastFrame = millis(); drawSetup(); }
    return;
  }

  if (inMenu) {
    menuInput(e1 == 3 ? 1 : e1, e2 == 3 ? 1 : e2);  // klik cepat di menu = dua kali aksi
    if (millis() - lastInput > 30000UL) exitMenu();
  } else {
    if (e1 == 1) goPage((page + 1) % PAGES);
    if (e1 == 3) {  // klik ganda bawah: buka pengaturan (batalkan geser halaman dari klik pertama)
      page = (page + PAGES - 1) % PAGES;
      inMenu = true;
      menuSel = 0;
    }
    if (e2 == 1) refreshAt = millis() + 500;  // dijalankan kalau tidak jadi klik ganda
  }

  if (refreshAt && millis() >= refreshAt) { refreshAt = 0; fetchNow = true; }

  if (millis() - lastBat > 5000UL) {
    lastBat = millis();
    sampleBattery();
  }
  updateBacklight();

  if (WiFi.status() == WL_CONNECTED) {
    if (cfg.needGeo && millis() - lastGeo > 15000UL) {
      lastGeo = millis();
      if (geocode()) { fetchNow = true; wx.valid = false; }
    }
    if (!cfg.needGeo && (fetchNow || millis() - lastFetch > fetchIntervalMs())) {
      fetchNow = false;
      fetching = true;
      if (!screenOff) {
        if (inMenu) renderMenu();
        else renderPage(page);
        spr.pushSprite(0, 0);
      }
      bool ok = fetchWeather();
      if (ok) fetchAqi();
      fetching = false;
      lastFetch = millis();
      if (!ok) lastFetch = millis() - fetchIntervalMs() + 30000UL;  // coba lagi 30 dtk
    }
  } else if (millis() - lastReconn > 15000UL) {
    lastReconn = millis();
    WiFi.reconnect();
  }

  if (AUTO_ROTATE_MS && cfg.saver < 2 && !inMenu && wx.valid && !screenOff &&
      millis() - lastInput > 30000UL && millis() - lastRot > AUTO_ROTATE_MS) {
    lastRot = millis();
    goPage((page + 1) % PAGES);
  }

  uint32_t fi = 50;
  if (cfg.saver == 1 && dimmed) fi = 100;
  else if (cfg.saver >= 2) fi = dimmed ? 250 : 100;
  if (millis() - lastFrame >= fi) {
    lastFrame = millis();
    frame++;
    if (!(screenOff && curBri == 0)) {
      if (inMenu) renderMenu();
      else renderPage(page);
      spr.pushSprite(0, 0);
    }
  }
}
