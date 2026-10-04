// =====================================================================
//  ESP32-S3-N16R8 + ILI9488 3.5" (480x320)
//  PC Monitor + LIVE media (image / GIF / video) + Web control panel
//
//  * Media FLASH-E SAVE HOY NA. Upload korle PSRAM-e jay ar sathe sathe dekhay.
//  * Pages : Overview, Temperature, Tasks, Media
//  * Web   : http://ESP32_IP/  ba  http://pcmonitor.local/
//  * WiFi  : connect na hole hotspot "PC-Monitor-Setup" (captive portal)
//  * BOOT button (GPIO0): short = next page | 1-4s = auto on/off | 4s+ = WiFi setup
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "display.h"
#include <WebServer.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include <AnimatedGIF.h>
#include <FS.h>
#include <vector>
#include <algorithm>
#ifdef SD_USE_SPI
#include <SPI.h>
#include <SD.h>
#define SDFS SD
#else
#include <SD_MMC.h>
#define SDFS SD_MMC
#endif
#include "webui.h"

// ---------------- first-boot defaults (web theke bodlano jay) ----------------
#define DEF_SSID  ""                  // khali = prothom boot-e setup hotspot cholbe
#define DEF_PASS  ""
#define DEF_PC_IP "192.168.5.104"     // pc_agent.py cholche je PC-te
#define AP_NAME   "PC-Monitor-Setup"
#define MDNS_NAME "pcmonitor"
#define BTN_PIN   0
#define SCR_W     480
#define SCR_H     320
#define MAX_LIVE  (5UL * 1024 * 1024 + 512UL * 1024)
#define MAX_GIF   (3UL * 1024 * 1024)
#define CFG_MAGIC 0xC0F10008UL
#define STREAM_PORT 8081                       // PC live-stream (WiFi) port
#define MAX_SD_FILE (1024UL * 1024 * 1024)     // sanity limit per file
#define RB_N   6                               // read-ahead ring entries (SD / live stream)
#define RB_CAP (128UL * 1024)                  // max encoded size of one tile/frame in the ring

// ---- microSD pins. Default = SD_MMC 1-bit (fastest, own bus, NOT shared with the display).
// Override with -D flags in platformio.ini. -DSD_USE_SPI = SPI module on its own pins. -DSD_4BIT = 4-bit SD_MMC.
#ifndef SD_CLK
#define SD_CLK 39
#endif
#ifndef SD_CMD
#define SD_CMD 38
#endif
#ifndef SD_D0
#define SD_D0 40
#endif
#ifndef SD_D1
#define SD_D1 41
#endif
#ifndef SD_D2
#define SD_D2 42
#endif
#ifndef SD_D3
#define SD_D3 21
#endif
#ifndef SD_SCK
#define SD_SCK 39
#endif
#ifndef SD_MISO
#define SD_MISO 40
#endif
#ifndef SD_MOSI
#define SD_MOSI 38
#endif
#ifndef SD_CS
#define SD_CS 41
#endif
#ifndef SD_SPI_HZ
#define SD_SPI_HZ 20000000
#endif
#ifdef SD_4BIT
#define SD_1BIT_MODE false
#else
#define SD_1BIT_MODE true
#endif

enum Page { P_OVERVIEW, P_TEMP, P_TASKS, P_DISK, P_ACTIVITY, P_MEDIA, P_COUNT };
static const char* PAGE_NAME[P_COUNT] = {"Overview", "Temperature", "Tasks", "Disk I/O", "Activity", "Media"};

// ---------------- persistent settings (NVS) ----------------
struct Cfg {
  uint32_t magic;
  uint16_t pageSecs[P_COUNT];
  uint8_t  pageOn[P_COUNT];
  uint8_t  autoRotate;
  uint8_t  gifSkip;
  uint8_t  holdOnUpload;
  uint8_t  sdLoops, sdPreload;
  uint16_t sdStillSecs;
  uint8_t  bright, rotation;
  uint16_t pcPort, pollMs;
  char pcIp[16];
  char ssid[33];
  char pass[65];
};
static Cfg cfg;

static void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CFG_MAGIC;
  const uint16_t secs[P_COUNT] = {12, 8, 10, 10, 10, 30};
  for (int i = 0; i < P_COUNT; i++) { cfg.pageSecs[i] = secs[i]; cfg.pageOn[i] = 1; }
  cfg.autoRotate = 1; cfg.gifSkip = 1; cfg.holdOnUpload = 1;
  cfg.sdLoops = 1; cfg.sdPreload = 1; cfg.sdStillSecs = 8;
  cfg.bright = 255; cfg.rotation = 1;
  cfg.pcPort = 8080; cfg.pollMs = 1000;
  strlcpy(cfg.pcIp, DEF_PC_IP, sizeof(cfg.pcIp));
  strlcpy(cfg.ssid, DEF_SSID, sizeof(cfg.ssid));
  strlcpy(cfg.pass, DEF_PASS, sizeof(cfg.pass));
}
static void loadCfg() {
  cfgDefaults();
  Preferences p;
  if (!p.begin("pcmon", true)) return;
  Cfg t;
  size_t n = p.getBytes("cfg", &t, sizeof(t));
  p.end();
  if (n == sizeof(t) && t.magic == CFG_MAGIC) cfg = t;
}
static void saveCfg() {
  Preferences p;
  if (p.begin("pcmon", false)) { p.putBytes("cfg", &cfg, sizeof(cfg)); p.end(); }
}

// ---------------- backlight (PWM via LovyanGFX Light_PWM) ----------------
static void blInit() {}
// ---------------- globals ----------------
bool gDirty = false;
static Display tft;                       // the real panel (media is pushed straight to it)
static Canvas gfx(&tft);                  // PSRAM sprite: every page is drawn here, then sent as ONE bulk transfer
static void gfxPresent() { if (gDirty) { gDirty = false; gfx.pushSprite(0, 0); } }
static void blSet(uint8_t v) { tft.setBrightness(v); }
static WebServer web(80);
static DNSServer dns;
static AnimatedGIF gif;

struct Proc  { char n[28]; float c; uint32_t m; };
struct Drive { char n[8]; float p, u, t; };
struct Rp    { char n[28]; uint32_t m; };
struct Dp    { char n[28]; float r; };
struct Stats {
  char host[24] = "-";
  char name[40] = "-";
  float cpu = 0, freq = 0, temp = -1, ram = 0, ramUsed = 0, ramTotal = 0, disk = 0;
  float up = 0, down = 0;
  float gpu = -1, gpuTemp = -1, gpuMemUsed = 0, gpuMemTotal = 0;
  float dr = 0, dw = 0, swap = 0;
  int16_t bat = -1;
  bool plug = false;
  char win[52] = "";
  char wapp[24] = "";
  char clk[12] = "--:--";
  char dat[16] = "";
  uint32_t uptime = 0;
  uint16_t nproc = 0;
  uint8_t nCores = 0;
  float cores[64];
  uint8_t nProcs = 0;
  Proc procs[8];
  uint8_t nDrives = 0;
  Drive drives[4];
  uint8_t nRp = 0;
  Rp rprocs[4];
  uint8_t nDp = 0;
  Dp dprocs[4];
};
static Stats S;    // display snapshot
static Stats SH;   // shared with poll task
static SemaphoreHandle_t mtx;
static volatile bool newData = false, pcOnline = false, lastOk = false;
static volatile uint8_t failCount = 0;

static const int HN = 114;
static float cpuHist[HN], tempHist[HN], gpuTHist[HN], dRHist[HN], dWHist[HN];

static int curPage = P_OVERVIEW;
static bool autoRotate = true;
static uint32_t pageStart = 0;
static String lastErr = "";

// WiFi / AP state
enum { WS_IDLE, WS_TRYING, WS_OK, WS_FAIL };
static bool apMode = false, setupShown = false;
static uint8_t wifiTry = WS_IDLE;
static char tryS[33], tryP[65];
static uint32_t tryT0 = 0, apStopAt = 0, lastRetry = 0;

static String myIp() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (apMode) return WiFi.softAPIP().toString();
  return String("0.0.0.0");
}

// ---------------- small helpers ----------------
static uint16_t lvlColor(float p) { return p < 60 ? TFT_GREEN : (p < 85 ? TFT_YELLOW : TFT_RED); }
static uint16_t tempColor(float t) { return t < 60 ? TFT_GREEN : (t < 75 ? TFT_YELLOW : (t < 85 ? TFT_ORANGE : TFT_RED)); }

static void drawBar(int x, int y, int w, int h, float pct) {
  pct = constrain(pct, 0, 100);
  gfx.drawRect(x, y, w, h, TFT_DARKGREY);
  int fw = (int)((w - 2) * pct / 100.0f);
  gfx.fillRect(x + 1, y + 1, fw, h - 2, lvlColor(pct));
  gfx.fillRect(x + 1 + fw, y + 1, w - 2 - fw, h - 2, TFT_BLACK);
}

static String fmtUptime(uint32_t s) {
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  char b[24];
  if (d) snprintf(b, sizeof(b), "%ud %02uh %02um", (unsigned)d, (unsigned)h, (unsigned)m);
  else   snprintf(b, sizeof(b), "%02uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)(s % 60));
  return String(b);
}
static String fmtRate(float kb) {
  char b[16];
  if (kb >= 1024) snprintf(b, sizeof(b), "%.1f MB/s", kb / 1024.0f);
  else            snprintf(b, sizeof(b), "%.0f KB/s", kb);
  return String(b);
}

// Rotation (+4 = mirrored; build with -DDISPLAY_FLIP_Y if the header shows at the bottom)
static void applyRot(uint8_t r) {
#ifdef DISPLAY_FLIP_Y
  tft.setRotation((r & 3) + 4);
#else
  tft.setRotation(r & 3);
#endif
}

// ---- power-on splash (about 6 seconds): name, slogan and a loading bar ----
#define SPLASH_MS 6000
static void bootTest() {
  const uint16_t cy = TFT_CYAN, bl = 0x2D7F, pu = 0x8010;
  gfx.fillScreen(TFT_BLACK);
  gfx.setTextPadding(0);
  gfx.fillRect(0, 0, 160, 5, cy);   gfx.fillRect(160, 0, 160, 5, bl);   gfx.fillRect(320, 0, 160, 5, pu);
  gfx.fillRect(0, 315, 160, 5, pu); gfx.fillRect(160, 315, 160, 5, bl); gfx.fillRect(320, 315, 160, 5, cy);
  gfx.setTextDatum(MC_DATUM);
  gfx.setTextSize(1.7f);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("LUMINA VISUAL", SCR_W / 2, 105, 4);
  gfx.setTextSize(1);
  gfx.drawFastHLine(90, 150, 300, bl);
  gfx.setTextColor(cy, TFT_BLACK);
  gfx.drawString("Your System Monitor", SCR_W / 2, 182, 4);
  gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
  gfx.drawString("See your system in a new light", SCR_W / 2, 224, 2);
  gfx.drawRect(140, 270, 200, 8, 0x7BEF);
  gfx.setTextDatum(TL_DATUM);
  gfxPresent();
  const int steps = 50;                                // loading bar fills over SPLASH_MS
  for (int k = 1; k <= steps; k++) {
    tft.fillRect(142, 272, (196 * k) / steps, 4, cy);
    delay(SPLASH_MS / steps);
  }
  gfx.fillScreen(TFT_BLACK);
  gfxPresent();
}

static void showMsg(const char* a, const String& b) {
  gfx.fillScreen(TFT_BLACK);
  gfx.setTextPadding(0);
  gfx.setTextDatum(MC_DATUM);
  gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
  gfx.drawString(a, SCR_W / 2, 125, 4);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString(b.substring(0, 60), SCR_W / 2, 170, 2);
  if (b.length() > 60) gfx.drawString(b.substring(60, 120), SCR_W / 2, 190, 2);
  gfx.setTextDatum(TL_DATUM);
}

// =====================================================================
//  HEADER
// =====================================================================
static char pollErr[16] = "start";      // why the PC is offline (shown in the header)
static void drawHeader() {
  gfx.fillRect(0, 0, SCR_W, 26, TFT_NAVY);
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_WHITE, TFT_NAVY);
  gfx.drawString(String(S.host).substring(0, 9) + " " + myIp(), 6, 5, 2);

  for (int i = 0; i < P_COUNT; i++) {
    int cx = 232 + i * 14;
    if (i == curPage) gfx.fillCircle(cx, 13, 4, TFT_CYAN);
    else if (cfg.pageOn[i]) gfx.drawCircle(cx, 13, 4, TFT_LIGHTGREY);
    else gfx.drawCircle(cx, 13, 2, TFT_DARKGREY);
  }
  if (!autoRotate) {
    gfx.setTextColor(TFT_YELLOW, TFT_NAVY);
    gfx.drawString("HOLD", 322, 5, 2);
  }
  gfx.setTextDatum(TR_DATUM);
  if (WiFi.status() == WL_CONNECTED) {
    gfx.setTextColor(pcOnline ? TFT_GREEN : TFT_ORANGE, TFT_NAVY);
    gfx.drawString(pcOnline ? (String("LIVE ") + WiFi.RSSI() + "dBm") : (String("NO PC: ") + pollErr), 474, 5, 2);
  } else {
    gfx.setTextColor(TFT_RED, TFT_NAVY);
    gfx.drawString("WiFi OFF", 474, 5, 2);
  }
  gfx.setTextDatum(TL_DATUM);
}

// =====================================================================
//  PAGE 0: OVERVIEW
// =====================================================================
static void drawStaticOverview() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString("CPU", 10, 32, 2);
  gfx.drawString("CPU HISTORY", 10, 140, 2);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("RAM", 10, 226, 2);
  gfx.drawString("DISK", 10, 250, 2);
  gfx.drawString("NET", 10, 278, 2);
  gfx.drawString("UP", 10, 298, 2);
}

static void drawCpuBlock() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(lvlColor(S.cpu), TFT_BLACK);
  gfx.setTextPadding(110);
  char b[24];
  snprintf(b, sizeof(b), "%d", (int)round(S.cpu));
  gfx.drawString(b, 10, 52, 7);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("%", 125, 80, 4);
  gfx.setTextPadding(150);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  snprintf(b, sizeof(b), "%.0f MHz", S.freq);
  gfx.drawString(b, 10, 108, 2);
  if (S.temp > 0) {
    gfx.setTextColor(tempColor(S.temp), TFT_BLACK);
    snprintf(b, sizeof(b), "%.0f C", S.temp);
  } else {
    gfx.setTextColor(TFT_DARKGREY, TFT_BLACK);
    snprintf(b, sizeof(b), "Temp: n/a");
  }
  gfx.drawString(b, 10, 124, 2);
  gfx.setTextPadding(0);
}

static void drawCores() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.setTextPadding(300);
  gfx.drawString(String("CORES  ") + String(S.name).substring(0, 26), 170, 32, 2);
  gfx.setTextPadding(0);
  const int x0 = 170, y0 = 50, w = 300, h = 84;
  gfx.fillRect(x0, y0, w, h, TFT_BLACK);
  if (S.nCores == 0) return;
  int n = S.nCores, gap = 3;
  int bw = (w - gap * (n - 1)) / n;
  if (bw > 28) bw = 28;
  if (bw < 3) bw = 3;
  for (int i = 0; i < n; i++) {
    int x = x0 + i * (bw + gap);
    int fh = (int)((h - 2) * constrain(S.cores[i], 0, 100) / 100.0f);
    gfx.drawRect(x, y0, bw, h, TFT_DARKGREY);
    gfx.fillRect(x + 1, y0 + h - 1 - fh, bw - 2, fh, lvlColor(S.cores[i]));
  }
}

static void drawGraph() {
  const int gx = 10, gy = 158, gw = 460, gh = 60;
  gfx.fillRect(gx, gy, gw, gh, TFT_BLACK);
  gfx.drawRect(gx, gy, gw, gh, TFT_DARKGREY);
  gfx.drawFastHLine(gx + 1, gy + gh / 2, gw - 2, 0x2104);
  for (int i = 0; i < HN; i++) {
    int bh = (int)((gh - 2) * constrain(cpuHist[i], 0, 100) / 100.0f);
    if (bh < 1) continue;
    gfx.fillRect(gx + 2 + i * 4, gy + gh - 1 - bh, 3, bh, lvlColor(cpuHist[i]));
  }
}

static void drawBottom() {
  char b[40];
  gfx.setTextDatum(TL_DATUM);
  drawBar(60, 224, 270, 18, S.ram);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setTextPadding(140);
  snprintf(b, sizeof(b), "%.0f%%  %.1f/%.1fG", S.ram, S.ramUsed, S.ramTotal);
  gfx.drawString(b, 336, 226, 2);
  drawBar(60, 248, 270, 18, S.disk);
  snprintf(b, sizeof(b), "%.0f%%", S.disk);
  gfx.drawString(b, 336, 250, 2);
  gfx.setTextPadding(170);
  gfx.setTextColor(TFT_GREEN, TFT_BLACK);
  gfx.drawString(String("UP ") + fmtRate(S.up), 60, 278, 2);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.setTextPadding(200);
  gfx.drawString(String("DOWN ") + fmtRate(S.down), 240, 278, 2);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.setTextPadding(300);
  gfx.drawString(fmtUptime(S.uptime), 60, 298, 2);
  gfx.setTextPadding(0);
}

// =====================================================================
//  PAGE 1: TEMPERATURE
// =====================================================================
static void tempStatic() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString("CPU TEMP", 10, 34, 2);
  gfx.drawString("GPU TEMP", 250, 34, 2);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("TEMPERATURE HISTORY", 10, 146, 2);
  gfx.setTextColor(TFT_ORANGE, TFT_BLACK);
  gfx.drawString("CPU", 360, 146, 2);
  gfx.setTextColor(TFT_MAGENTA, TFT_BLACK);
  gfx.drawString("GPU", 420, 146, 2);
}

static void drawTempPanel(int x, float t, const char* extra) {
  char b[16];
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextPadding(130);
  if (t > 0) { gfx.setTextColor(tempColor(t), TFT_BLACK); snprintf(b, sizeof(b), "%d", (int)round(t)); }
  else       { gfx.setTextColor(TFT_DARKGREY, TFT_BLACK); strcpy(b, "--"); }
  gfx.drawString(b, x, 54, 7);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("C", x + 140, 62, 4);
  drawBar(x, 112, 210, 14, t > 0 ? t : 0);
  gfx.setTextPadding(210);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.drawString(extra, x, 130, 2);
  gfx.setTextPadding(0);
}

static void plotSeries(const float* a, int gx, int gy, int gw, int gh, uint16_t color) {
  int px = -1, py = -1;
  for (int i = 0; i < HN; i++) {
    if (a[i] <= 0) { px = -1; continue; }
    float t = constrain(a[i], 20, 100);
    int x = gx + 2 + i * 4;
    int y = gy + gh - 2 - (int)((t - 20) / 80.0f * (gh - 4));
    if (px >= 0) gfx.drawLine(px, py, x, y, color);
    px = x; py = y;
  }
}

static void tempDynamic() {
  char ex[40];
  if (S.temp > 0) snprintf(ex, sizeof(ex), "%.0f MHz  Load %.0f%%", S.freq, S.cpu);
  else            snprintf(ex, sizeof(ex), "No sensor - use LHM");
  drawTempPanel(10, S.temp, ex);
  if (S.gpuTemp > 0 || S.gpu >= 0)
    snprintf(ex, sizeof(ex), "Load %.0f%%  %.1f/%.0fG", S.gpu < 0 ? 0 : S.gpu, S.gpuMemUsed, S.gpuMemTotal);
  else
    snprintf(ex, sizeof(ex), "No GPU data");
  drawTempPanel(250, S.gpuTemp, ex);

  const int gx = 10, gy = 166, gw = 460, gh = 130;
  gfx.fillRect(gx, gy, gw, gh, TFT_BLACK);
  gfx.drawRect(gx, gy, gw, gh, TFT_DARKGREY);
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_DARKGREY, TFT_BLACK);
  for (int t = 40; t <= 80; t += 20) {
    int y = gy + gh - 2 - (int)((t - 20) / 80.0f * (gh - 4));
    gfx.drawFastHLine(gx + 1, y, gw - 2, 0x2104);
    gfx.drawString(String(t), gx + 4, y - 9, 1);
  }
  plotSeries(tempHist, gx, gy, gw, gh, TFT_ORANGE);
  plotSeries(gpuTHist, gx, gy, gw, gh, TFT_MAGENTA);
}

// =====================================================================
//  PAGE 2: TASKS
// =====================================================================
static void tasksStatic() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString("PROCESS", 10, 32, 2);
  gfx.drawString("CPU", 262, 32, 2);
  gfx.setTextDatum(TR_DATUM);
  gfx.drawString("RAM", 470, 32, 2);
  gfx.setTextDatum(TL_DATUM);
  gfx.drawFastHLine(0, 50, SCR_W, TFT_DARKGREY);
  gfx.drawFastHLine(0, 286, SCR_W, TFT_DARKGREY);
}

static void tasksDynamic() {
  char b[24];
  for (int i = 0; i < 8; i++) {
    int y = 56 + i * 28;
    if (i < S.nProcs) {
      const Proc& p = S.procs[i];
      gfx.setTextDatum(TL_DATUM);
      gfx.setTextColor(TFT_WHITE, TFT_BLACK);
      gfx.setTextPadding(245);
      gfx.drawString(p.n, 10, y + 4, 2);
      drawBar(262, y + 2, 90, 18, p.c);
      gfx.setTextColor(lvlColor(p.c), TFT_BLACK);
      gfx.setTextPadding(56);
      snprintf(b, sizeof(b), "%.1f%%", p.c);
      gfx.drawString(b, 358, y + 4, 2);
      gfx.setTextDatum(TR_DATUM);
      gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      gfx.setTextPadding(62);
      if (p.m >= 1024) snprintf(b, sizeof(b), "%.1f GB", p.m / 1024.0f);
      else             snprintf(b, sizeof(b), "%u MB", (unsigned)p.m);
      gfx.drawString(b, 470, y + 4, 2);
    } else {
      gfx.fillRect(0, y, SCR_W, 28, TFT_BLACK);
    }
  }
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.setTextPadding(470);
  char f[64];
  snprintf(f, sizeof(f), "Processes: %u   CPU %.0f%%   RAM %.0f%%", (unsigned)S.nproc, S.cpu, S.ram);
  gfx.drawString(f, 10, 294, 2);
  gfx.setTextPadding(0);
}

// =====================================================================
//  PAGE 3: DISK I/O (read/write speed, history, drives)
// =====================================================================
static float histMax(const float* a, float minv) {
  float m = minv;
  for (int i = 0; i < HN; i++) if (a[i] > m) m = a[i];
  return m;
}

static void plotAuto(const float* a, int gx, int gy, int gw, int gh, uint16_t color, float vmax) {
  int px = -1, py = -1;
  for (int i = 0; i < HN; i++) {
    float v = constrain(a[i], 0, vmax);
    int x = gx + 2 + i * 4;
    int y = gy + gh - 2 - (int)(v / vmax * (gh - 4));
    if (px >= 0) gfx.drawLine(px, py, x, y, color);
    px = x; py = y;
  }
}

static void diskStatic() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);   gfx.drawString("DISK READ", 10, 32, 2);
  gfx.setTextColor(TFT_ORANGE, TFT_BLACK); gfx.drawString("DISK WRITE", 250, 32, 2);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);  gfx.drawString("I/O HISTORY", 10, 126, 2);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);   gfx.drawString("READ", 350, 126, 2);
  gfx.setTextColor(TFT_ORANGE, TFT_BLACK); gfx.drawString("WRITE", 410, 126, 2);
}

static void drawIoPanel(int x, float v, uint16_t color, float scale) {
  char b[16];
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(color, TFT_BLACK);
  gfx.setTextPadding(150);
  if (v < 100) snprintf(b, sizeof(b), "%.1f", v); else snprintf(b, sizeof(b), "%.0f", v);
  gfx.drawString(b, x, 50, 6);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("MB/s", x + 160, 72, 2);
  float pct = scale > 0 ? v / scale * 100.0f : 0;
  gfx.drawRect(x, 106, 210, 12, TFT_DARKGREY);
  int fw = (int)(208 * constrain(pct, 0, 100) / 100.0f);
  gfx.fillRect(x + 1, 107, fw, 10, color);
  gfx.fillRect(x + 1 + fw, 107, 208 - fw, 10, TFT_BLACK);
}

static void diskDynamic() {
  float scale = histMax(dRHist, 5);
  float sw = histMax(dWHist, 5);
  if (sw > scale) scale = sw;
  drawIoPanel(10, S.dr, TFT_CYAN, scale);
  drawIoPanel(250, S.dw, TFT_ORANGE, scale);

  const int gx = 10, gy = 144, gw = 460, gh = 104;
  gfx.fillRect(gx, gy, gw, gh, TFT_BLACK);
  gfx.drawRect(gx, gy, gw, gh, TFT_DARKGREY);
  gfx.drawFastHLine(gx + 1, gy + gh / 2, gw - 2, 0x2104);
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_DARKGREY, TFT_BLACK);
  gfx.drawString(String((int)scale) + " MB/s", gx + 4, gy + 3, 1);
  plotAuto(dRHist, gx, gy, gw, gh, TFT_CYAN, scale);
  plotAuto(dWHist, gx, gy, gw, gh, TFT_ORANGE, scale);

  char b[40];
  for (int i = 0; i < 4; i++) {
    int cx = 10 + (i % 2) * 240, cy = 256 + (i / 2) * 30;
    if (i < S.nDrives) {
      const Drive& d = S.drives[i];
      gfx.setTextDatum(TL_DATUM);
      gfx.setTextColor(TFT_WHITE, TFT_BLACK);
      gfx.setTextPadding(28);
      gfx.drawString(d.n, cx, cy + 2, 2);
      drawBar(cx + 32, cy + 3, 100, 14, d.p);
      gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      gfx.setTextPadding(100);
      snprintf(b, sizeof(b), "%.0f%% %.0f/%.0fG", d.p, d.u, d.t);
      gfx.drawString(b, cx + 138, cy + 2, 2);
      gfx.setTextPadding(0);
    } else {
      gfx.fillRect(cx, cy, 236, 28, TFT_BLACK);
    }
  }
}

// =====================================================================
//  PAGE 4: ACTIVITY (active window, top RAM, top I/O, swap, battery, clock)
// =====================================================================
static void activityStatic() {
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString("ACTIVE WINDOW", 10, 32, 2);
  gfx.drawFastHLine(0, 98, SCR_W, TFT_DARKGREY);
  gfx.drawString("TOP RAM", 10, 104, 2);
  gfx.drawString("TOP I/O (MB/s)", 250, 104, 2);
  gfx.drawFastHLine(0, 234, SCR_W, TFT_DARKGREY);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("SWAP", 10, 244, 2);
  gfx.drawString("BAT", 10, 268, 2);
}


// ---- clock: 12h / 24h, optional seconds, ticks every second on its own ----
static bool clock12 = false, clockSec = true;
static int32_t clkBase = -1;            // seconds since midnight at last sync (-1 = no time yet)
static uint32_t clkMs = 0;
static int lastClkKey = -1;
static void clockLoadPrefs() {
  Preferences p;
  if (p.begin("pcmon", true)) { clock12 = p.getBool("c12", false); clockSec = p.getBool("csec", true); p.end(); }
}
static void clockSavePrefs() {
  Preferences p;
  if (p.begin("pcmon", false)) { p.putBool("c12", clock12); p.putBool("csec", clockSec); p.end(); }
}
static void clockSync(const char* t) {                 // "HH:MM" or "HH:MM:SS" from the PC agent
  int h = 0, m = 0, sc = 0;
  int n = sscanf(t, "%d:%d:%d", &h, &m, &sc);
  if (n < 2 || h < 0 || h > 23 || m < 0 || m > 59) return;
  if (n < 3) sc = 0;
  clkBase = h * 3600 + m * 60 + sc;
  clkMs = millis();
}
static int32_t clockNow() {
  if (clkBase < 0) return -1;
  return (int32_t)((clkBase + (millis() - clkMs) / 1000UL) % 86400UL);
}
static void drawClock() {
  gfx.fillRect(296, 230, 184, 58, TFT_BLACK);
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextPadding(0);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  int32_t t = clockNow();
  if (t < 0) { gfx.drawString("--:--", 300, 240, 6); return; }
  int h = t / 3600, m = (t / 60) % 60, sc = t % 60;
  char b[16];
  const char* ap = h >= 12 ? "PM" : "AM";
  if (clock12) { h %= 12; if (h == 0) h = 12; snprintf(b, sizeof(b), "%d:%02d", h, m); }
  else         { snprintf(b, sizeof(b), "%02d:%02d", h, m); }
  gfx.drawString(b, 300, 240, 6);
  if (clockSec) { snprintf(b, sizeof(b), "%02d", sc); gfx.setTextColor(TFT_YELLOW, TFT_BLACK); gfx.drawString(b, 440, 262, 4); }
  if (clock12) { gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK); gfx.drawString(ap, 440, 240, 2); }
}
static void clockTick() {                               // call from loop() while the Activity page is shown
  int32_t t = clockNow();
  if (t < 0) return;
  int key = clockSec ? (int)t : (int)(t / 60);
  key = key * 2 + (clock12 ? 1 : 0);
  if (key == lastClkKey) return;
  lastClkKey = key;
  drawClock();
}

static void activityDynamic() {
  char b[32];
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setTextPadding(470);
  String t = strlen(S.win) ? String(S.win) : String(S.wapp);
  if (!t.length()) t = "(no data - Windows agent only)";
  gfx.drawString(t.substring(0, 32), 10, 52, 4);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.drawString(strlen(S.wapp) ? String("App: ") + S.wapp : String(" "), 10, 80, 2);

  for (int i = 0; i < 4; i++) {
    int y = 126 + i * 26;
    if (i < S.nRp) {
      gfx.setTextDatum(TL_DATUM);
      gfx.setTextPadding(150);
      gfx.setTextColor(TFT_WHITE, TFT_BLACK);
      gfx.drawString(S.rprocs[i].n, 10, y, 2);
      uint32_t m = S.rprocs[i].m;
      if (m >= 1024) snprintf(b, sizeof(b), "%.1f GB", m / 1024.0f); else snprintf(b, sizeof(b), "%u MB", (unsigned)m);
      gfx.setTextDatum(TR_DATUM);
      gfx.setTextPadding(70);
      gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      gfx.drawString(b, 235, y, 2);
    } else {
      gfx.fillRect(0, y, 240, 24, TFT_BLACK);
    }
    if (i < S.nDp) {
      gfx.setTextDatum(TL_DATUM);
      gfx.setTextPadding(150);
      gfx.setTextColor(TFT_WHITE, TFT_BLACK);
      gfx.drawString(S.dprocs[i].n, 250, y, 2);
      snprintf(b, sizeof(b), "%.1f", S.dprocs[i].r);
      gfx.setTextDatum(TR_DATUM);
      gfx.setTextPadding(70);
      gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
      gfx.drawString(b, 470, y, 2);
    } else {
      gfx.fillRect(244, y, 236, 24, TFT_BLACK);
    }
  }

  gfx.setTextDatum(TL_DATUM);
  drawBar(60, 242, 150, 14, S.swap);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.setTextPadding(80);
  snprintf(b, sizeof(b), "%.0f%%", S.swap);
  gfx.drawString(b, 218, 244, 2);

  if (S.bat >= 0) {
    gfx.drawRect(60, 266, 150, 14, TFT_DARKGREY);
    uint16_t bc = S.bat < 20 ? TFT_RED : (S.bat < 50 ? TFT_YELLOW : TFT_GREEN);
    int fw = (int)(148 * S.bat / 100.0f);
    gfx.fillRect(61, 267, fw, 12, bc);
    gfx.fillRect(61 + fw, 267, 148 - fw, 12, TFT_BLACK);
    snprintf(b, sizeof(b), "%d%% %s", (int)S.bat, S.plug ? "AC" : "BAT");
  } else {
    gfx.fillRect(60, 266, 150, 14, TFT_BLACK);
    snprintf(b, sizeof(b), "No battery");
  }
  gfx.drawString(b, 218, 268, 2);

  gfx.setTextPadding(250);
  snprintf(b, sizeof(b), "Processes: %u", (unsigned)S.nproc);
  gfx.drawString(b, 10, 294, 2);

  drawClock();
  lastClkKey = -1;
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.setTextPadding(150);
  gfx.drawString(S.dat, 322, 292, 2);
  gfx.setTextPadding(0);
}

// =====================================================================
//  PAGE 5: LIVE MEDIA  (RAM-only: RGB5 still / MJV clip / GIF / JPEG)
// =====================================================================
enum { LT_NONE, LT_STILL, LT_CLIP, LT_GIF, LT_JPEG, LT_STREAM };
static void enterPage(int p);

static uint8_t* liveBuf = nullptr;
static size_t liveSize = 0, liveCap = 0;
static uint8_t liveType = LT_NONE;
static uint16_t liveW = 0, liveH = 0;
static uint32_t liveFrames = 0;
static String liveName = "";

// shared PSRAM frame buffer (480x320 RGB565)
static uint16_t* canvas = nullptr;
static int cvW = SCR_W, cvH = SCR_H;

// ---- JPEG decode callbacks ----
static bool canvasOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!canvas) return 0;
  for (int r = 0; r < h; r++) {
    int yy = y + r;
    if (yy < 0 || yy >= cvH) continue;
    int xx = x, ww = w;
    if (xx < 0 || xx >= cvW) continue;
    if (xx + ww > cvW) ww = cvW - xx;
    memcpy(canvas + (size_t)yy * cvW + xx, bmp + (size_t)r * w, ww * 2);
  }
  return 1;
}
static bool tftDirectOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bmp);
  return 1;
}

// ---- STILL (raw RGB565 from browser: no decoding needed) ----
static bool showStill() {
  tft.fillScreen(TFT_BLACK);
  int ox = (SCR_W - liveW) / 2, oy = (SCR_H - liveH) / 2;
  tft.pushImage(ox, oy, liveW, liveH, (uint16_t*)(liveBuf + 8));
  return true;
}

// ---- direct JPEG (fallback, e.g. curl upload) ----
static bool showJpegBuf(const uint8_t* b, size_t sz) {
  uint16_t w = 0, h = 0;
  int r = TJpgDec.getJpgSize(&w, &h, b, sz);
  if (r != 0 || !w || !h) { lastErr = "Invalid/progressive JPEG (code " + String(r) + ")"; return false; }
  uint8_t sc = 1;
  while ((w / sc > SCR_W || h / sc > SCR_H) && sc < 8) sc *= 2;
  TJpgDec.setJpgScale(sc);
  int iw = (w + sc - 1) / sc, ih = (h + sc - 1) / sc;
  int ox = (SCR_W - iw) / 2, oy = (SCR_H - ih) / 2;
  if (ox < 0) ox = 0;
  if (oy < 0) oy = 0;
  tft.fillScreen(TFT_BLACK);
  if (canvas) {
    cvW = iw; cvH = ih;
    TJpgDec.setCallback(canvasOut);
    r = TJpgDec.drawJpg(0, 0, b, sz);
    if (r == 0) tft.pushImage(ox, oy, iw, ih, canvas);
  } else {
    TJpgDec.setCallback(tftDirectOut);
    r = TJpgDec.drawJpg(ox, oy, b, sz);
    TJpgDec.setCallback(canvasOut);
  }
  if (r != 0) { lastErr = "JPEG decode failed (code " + String(r) + ")"; return false; }
  return true;
}

// ---- GIF (decode into PSRAM canvas, push only the changed rectangle) ----
static bool gifOpen = false;
static int gifOffX = 0, gifOffY = 0;
static uint32_t gifDue = 0;
static uint8_t gifSkipped = 0;
static int dX0, dY0, dX1, dY1;

static inline void dirtyReset() { dX0 = SCR_W; dY0 = SCR_H; dX1 = -1; dY1 = -1; }

static void GIFDraw(GIFDRAW* pDraw) {
  if (!canvas) return;
  uint8_t* s = pDraw->pPixels;
  uint16_t* pal = pDraw->pPalette;
  int y = pDraw->iY + pDraw->y + gifOffY;
  int baseX = pDraw->iX + gifOffX;
  int iw = pDraw->iWidth;
  if (baseX + iw > SCR_W) iw = SCR_W - baseX;
  if (y < 0 || y >= SCR_H || baseX < 0 || baseX >= SCR_W || iw < 1) return;
  uint16_t* row = canvas + (size_t)y * SCR_W + baseX;

  if (pDraw->ucDisposalMethod == 2) {
    for (int x = 0; x < iw; x++) if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    pDraw->ucHasTransparency = 0;
  }
  if (pDraw->ucHasTransparency) {
    uint8_t t = pDraw->ucTransparent;
    for (int x = 0; x < iw; x++) { uint8_t c = s[x]; if (c != t) row[x] = pal[c]; }
  } else {
    for (int x = 0; x < iw; x++) row[x] = pal[s[x]];
  }
  if (baseX < dX0) dX0 = baseX;
  if (baseX + iw - 1 > dX1) dX1 = baseX + iw - 1;
  if (y < dY0) dY0 = y;
  if (y > dY1) dY1 = y;
}

static void gifPush() {
  if (dX1 < dX0 || dY1 < dY0 || !canvas) return;
  int w = dX1 - dX0 + 1;
  tft.startWrite();
  for (int y = dY0; y <= dY1; y++) tft.pushImage(dX0, y, w, 1, canvas + (size_t)y * SCR_W + dX0);
  tft.endWrite();
  dirtyReset();
}

static void gifStop() {
  if (gifOpen) { gif.close(); gifOpen = false; }
}

static bool gifStart() {
  gifStop();
  if (!canvas) { lastErr = "PSRAM nei - GIF cholbe na (platformio.ini check koro)"; return false; }
  if (!liveBuf || !gif.open(liveBuf, (int)liveSize, GIFDraw)) { lastErr = "GIF decode open failed"; return false; }
  int w = gif.getCanvasWidth(), h = gif.getCanvasHeight();
  gifOffX = w < SCR_W ? (SCR_W - w) / 2 : 0;
  gifOffY = h < SCR_H ? (SCR_H - h) / 2 : 0;
  memset(canvas, 0, (size_t)SCR_W * SCR_H * 2);
  tft.fillScreen(TFT_BLACK);
  dirtyReset();
  gifOpen = true;
  gifSkipped = 0;
  gifDue = millis();
  return true;
}

static void gifTick() {
  if (!gifOpen) return;
  uint32_t now = millis();
  if ((int32_t)(now - gifDue) < 0) return;

  int fd = 0;
  int r = gif.playFrame(false, &fd);     // shudhu canvas-e decode
  if (fd < 20) fd = 100;                  // browser-er moto: 0/10ms delay = 100ms
  gifDue += fd;
  if (r == 0) gif.reset();
  else if (r < 0) { gifStop(); return; }

  bool behind = (int32_t)(millis() - gifDue) > 0;
  if (cfg.gifSkip && behind && gifSkipped < 4) {
    gifSkipped++;                         // frame drop kore speed thik rakhi
  } else {
    gifPush();
    gifSkipped = 0;
    if (!cfg.gifSkip && behind) gifDue = millis();
  }
  if ((int32_t)(millis() - gifDue) > 400) gifDue = millis();   // resync
}

// =====================================================================
//  SD CARD  (SD_MMC 1-bit by default | -DSD_USE_SPI | -DSD_4BIT)
//  Layout: /media/*.mjv (clips) *.rgb (stills) *.gif *.jpg
// =====================================================================
static fs::FS& sdfs = SDFS;
static SemaphoreHandle_t sdMtx = nullptr;
static bool sdOk = false;
static uint64_t sdTotal = 0, sdUsed = 0;
static String sdErr = "no card";
static uint8_t sdKind = 0;
static uint32_t sdLastMount = 0;
#ifdef SD_USE_SPI
static SPIClass sdSpi(FSPI);
#endif
static inline void sdLock()   { xSemaphoreTake(sdMtx, portMAX_DELAY); }
static inline void sdUnlock() { xSemaphoreGive(sdMtx); }

static void sdRefreshSpace() {
  sdTotal = SDFS.totalBytes();
  sdUsed = SDFS.usedBytes();
}

static bool sdBegin() {
  sdOk = false;
  sdLastMount = millis();
  sdLock();
#ifdef SD_USE_SPI
  SD.end();
  sdSpi.end();
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  bool ok = SD.begin(SD_CS, sdSpi, SD_SPI_HZ);
  uint8_t t = ok ? SD.cardType() : 0;
#else
  SD_MMC.end();
#ifdef SD_4BIT
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
#else
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
#endif
  bool ok = SD_MMC.begin("/sdcard", SD_1BIT_MODE, false, SDMMC_FREQ_HIGHSPEED);
  if (!ok) { SD_MMC.end(); ok = SD_MMC.begin("/sdcard", SD_1BIT_MODE, false, SDMMC_FREQ_DEFAULT); }
  uint8_t t = ok ? SD_MMC.cardType() : 0;
#endif
  if (ok && t == CARD_NONE) ok = false;
  if (ok) {
    sdfs.mkdir("/media");
    sdRefreshSpace();
    sdKind = t;
    sdErr = "";
    sdOk = true;
  } else {
    sdErr = "no card / mount failed (check wiring + 10k pull-ups)";
  }
  sdUnlock();
  Serial.printf("SD: %s (%llu MB)\n", sdOk ? "mounted" : "not available", (unsigned long long)(sdTotal >> 20));
  return sdOk;
}

static int sdKindOf(const String& n) {
  String l = n; l.toLowerCase();
  if (l.endsWith(".mjv")) return 1;
  if (l.endsWith(".rgb")) return 2;
  if (l.endsWith(".gif")) return 3;
  if (l.endsWith(".jpg") || l.endsWith(".jpeg")) return 4;
  return 0;
}
static String sdBaseName(const char* p) {
  String n = p;
  int i = n.lastIndexOf('/');
  return i >= 0 ? n.substring(i + 1) : n;
}
static String sdSafeName(const String& in) {
  String fn = in;
  int sl = fn.lastIndexOf('/');
  int bs = fn.lastIndexOf('\\');
  if (bs > sl) sl = bs;
  if (sl >= 0) fn = fn.substring(sl + 1);
  String c = "";
  for (unsigned i = 0; i < fn.length(); i++) {
    char ch = fn[i];
    c += (isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_') ? ch : '_';
  }
  while (c.startsWith(".")) c.remove(0, 1);
  if (c.length() > 40) {
    int dot = c.lastIndexOf('.');
    String ext = dot >= 0 ? c.substring(dot) : String("");
    c = c.substring(0, 40 - ext.length()) + ext;
  }
  return sdKindOf(c) ? c : String("");
}

static void sdListNames(std::vector<String>& out) {
  out.clear();
  if (!sdOk) return;
  sdLock();
  File dir = sdfs.open("/media");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        String n = sdBaseName(f.name());
        if (sdKindOf(n)) out.push_back(n);
      }
      f = dir.openNextFile();
    }
  }
  sdUnlock();
  std::sort(out.begin(), out.end(), [](const String& a, const String& b) { return a.compareTo(b) < 0; });
}

// write a RAM buffer to /media/<name> (via .part + rename)
static bool sdSaveBuf(const String& name, const uint8_t* data, size_t len, String& err) {
  if (!sdOk) { err = "SD card not ready"; return false; }
  if (len + 1048576ULL > sdTotal - sdUsed) { err = "SD card full"; return false; }
  String fin = "/media/" + name, part = fin + ".part";
  bool ok = false;
  sdLock();
  {
    File f = sdfs.open(part, FILE_WRITE);
    if (f) {
      size_t off = 0;
      ok = true;
      while (off < len) {
        size_t m = min((size_t)16384, len - off);
        if (f.write(data + off, m) != m) { ok = false; break; }
        off += m;
      }
      f.close();
    }
  }
  if (ok) {
    if (sdfs.exists(fin)) sdfs.remove(fin);
    ok = sdfs.rename(part, fin);
  } else {
    sdfs.remove(part);
  }
  if (ok) sdRefreshSpace();
  sdUnlock();
  if (!ok) err = "SD write failed";
  return ok;
}

// =====================================================================
//  VIDEO / smooth animation (MJV2)
//  File: [MJV2][w][h][delay][flags][n] + n*{x,y,w,h,size} + JPEG tiles
//  Sources : RAM (preloaded) | SD (streamed, read-ahead ring) | live stream (USB / WiFi)
//  Pipeline: producer (SD reader / stream task) -> ring -> decode task (core 0) -> 2 slots -> loop pushes to TFT (core 1)
//  Lock order: pipeMtx -> sdMtx. Never wait on a semaphore while holding pipeMtx.
// =====================================================================
enum { CS_NONE, CS_RAM, CS_SD, CS_STREAM };
struct ClipEnt { uint16_t x, y, w, h; uint32_t off, size; };
struct Slot { uint16_t x, y, w, h; uint32_t due; bool skip, end; uint16_t* pix; };
struct RbEnt { uint8_t* buf; uint32_t len; uint16_t x, y, w, h; uint32_t seq; bool end; };

static ClipEnt* clipEnt = nullptr;
static Slot slots[2];
static RbEnt rb[RB_N];
static uint32_t rbW = 0, rbR = 0;
static SemaphoreHandle_t semFree = nullptr, semFull = nullptr, rbFree = nullptr, rbFull = nullptr, pipeMtx = nullptr;
static volatile bool pipeRun = false;
static volatile uint8_t clipSrc = CS_NONE;
static volatile bool clipEndFlag = false;
static volatile bool strmReady = false;
static volatile uint32_t clipT0 = 0;
static uint32_t decFc = 0;
static uint32_t rdSeq = 0, rdPasses = 0;
static bool rdEndSent = false, ramEndSent = false;
static uint8_t wrI = 0, rdI = 0;
static bool held = false, clipIsOpen = false, clipAllKey = false, clipPaced = true;
static uint16_t clipW = 0, clipH = 0, clipDelay = 66;
static uint32_t clipFrames = 0, clipLoopsTarget = 0;
static int clipOX = 0, clipOY = 0;
static uint16_t* decDst = nullptr;
static int decW = 0, decH = 0;
static File clipFile;
static uint32_t sdReadErrs = 0;
static struct Perf { float dec = 0, push = 0, fps = 0; uint32_t drops = 0, cnt = 0, t0 = 0; } perf;

// shared state between web / io tasks and the loop task
static String sdStreamPath = "";
static bool plActive = false;
static int plIdx = -1;
static uint32_t stillUntil = 0;
static volatile bool strmReq = false, strmEndReq = false;
static volatile uint16_t strmW = 0, strmH = 0;
static volatile uint8_t pendState = 0;          // 0 idle, 1 ready (io->loop), 2 done ok, 3 done error
static uint8_t* pendBuf = nullptr;
static size_t pendLen = 0;
static uint8_t pendFlags = 0;
static char pendName[48] = "";
static char pendMsg[96] = "";
static volatile uint32_t lastPktMs = 0;

static inline void perfEma(float& a, float v) { a = (a == 0) ? v : a * 0.9f + v * 0.1f; }

static bool slotOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!decDst) return 0;
  for (int r = 0; r < h; r++) {
    int yy = y + r;
    if (yy < 0 || yy >= decH) continue;
    int xx = x, ww = w;
    if (xx < 0 || xx >= decW) continue;
    if (xx + ww > decW) ww = decW - xx;
    memcpy(decDst + (size_t)yy * decW + xx, bmp + (size_t)r * w, ww * 2);
  }
  return 1;
}

static void ringFree() {
  for (int i = 0; i < RB_N; i++) if (rb[i].buf) { free(rb[i].buf); rb[i].buf = nullptr; }
}
static bool ringAlloc() {
  for (int i = 0; i < RB_N; i++) {
    rb[i].buf = (uint8_t*)ps_malloc(RB_CAP);
    if (!rb[i].buf) { ringFree(); return false; }
    rb[i].len = 0; rb[i].end = false;
  }
  while (xSemaphoreTake(rbFull, 0) == pdTRUE) {}
  while (xSemaphoreTake(rbFree, 0) == pdTRUE) {}
  for (int i = 0; i < RB_N; i++) xSemaphoreGive(rbFree);
  rbW = rbR = 0;
  return true;
}

// ---- decoder task (core 0): ring/RAM entry -> JPEG decode -> back slot ----
static void decTask(void*) {
  for (;;) {
    if (!pipeRun) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    if (xSemaphoreTake(semFree, pdMS_TO_TICKS(10)) != pdTRUE) continue;
    bool ring = (clipSrc != CS_RAM);
    if (ring && xSemaphoreTake(rbFull, pdMS_TO_TICKS(10)) != pdTRUE) { xSemaphoreGive(semFree); continue; }
    if (xSemaphoreTake(pipeMtx, pdMS_TO_TICKS(50)) != pdTRUE) {
      if (ring) xSemaphoreGive(rbFull);
      xSemaphoreGive(semFree);
      continue;
    }
    if (!pipeRun) {
      xSemaphoreGive(pipeMtx);
      if (ring) xSemaphoreGive(rbFull);
      xSemaphoreGive(semFree);
      continue;
    }

    Slot& s = slots[wrI];
    const uint8_t* jp = nullptr;
    uint32_t jl = 0, due = 0;
    uint16_t ex = 0, ey = 0, ew = 0, eh = 0;
    bool isEnd = false, skipThis = false;

    if (clipSrc == CS_RAM) {
      if (clipAllKey) {                 // full-frame clip: pichhiye porle frame skip (decode-o lage na)
        uint32_t guard = 0;
        while ((int32_t)(millis() - (clipT0 + decFc * clipDelay)) > (int32_t)clipDelay && guard++ < clipFrames) { decFc++; perf.drops++; }
      }
      if (clipLoopsTarget && decFc >= clipFrames * clipLoopsTarget) {
        if (ramEndSent) { xSemaphoreGive(pipeMtx); xSemaphoreGive(semFree); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        ramEndSent = true;
        isEnd = true;
      } else {
        const ClipEnt& e = clipEnt[decFc % clipFrames];
        due = clipT0 + decFc * clipDelay;
        if (e.size) { jp = liveBuf + e.off; jl = e.size; }
        ex = e.x; ey = e.y; ew = e.w; eh = e.h;
        decFc++;
      }
    } else {
      RbEnt& r = rb[rbR % RB_N];
      if (r.end) {
        isEnd = true;
      } else {
        due = clipPaced ? clipT0 + r.seq * clipDelay : (uint32_t)millis();
        if (clipPaced && clipAllKey && (int32_t)(millis() - due) > (int32_t)clipDelay && uxSemaphoreGetCount(rbFull) > 0) {
          rbR++;                          // pichhiye achi + aro frame ache: ei frame drop
          xSemaphoreGive(rbFree);
          perf.drops++;
          xSemaphoreGive(pipeMtx);
          xSemaphoreGive(semFree);
          continue;
        }
        if (r.len) { jp = r.buf; jl = r.len; }
        ex = r.x; ey = r.y; ew = r.w; eh = r.h;
      }
    }

    if (jl && (ew == 0 || eh == 0 || ew > clipW || eh > clipH)) skipThis = true;   // never overflow slot buffer
    s.x = ex; s.y = ey; s.w = ew; s.h = eh; s.due = due;
    s.end = isEnd;
    s.skip = isEnd || skipThis || jl == 0;
    if (!s.skip) {
      decDst = s.pix; decW = ew; decH = eh;
      uint32_t t0 = micros();
      TJpgDec.setCallback(slotOut);
      TJpgDec.setJpgScale(1);
      if (TJpgDec.drawJpg(0, 0, jp, jl) != 0) s.skip = true;
      perfEma(perf.dec, (micros() - t0) / 1000.0f);
    }
    if (ring) { rbR++; xSemaphoreGive(rbFree); }
    wrI ^= 1;
    xSemaphoreGive(semFull);
    xSemaphoreGive(pipeMtx);
    vTaskDelay(1);        // let the idle task / WiFi run: big frames keep this task 100% busy -> watchdog reboot otherwise
  }
}

// ---- SD reader task (core 0): file -> ring (read-ahead, absorbs SD latency spikes) ----
static void rdTask(void*) {
  for (;;) {
    if (!pipeRun || clipSrc != CS_SD || rdEndSent) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (xSemaphoreTake(rbFree, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    if (xSemaphoreTake(pipeMtx, pdMS_TO_TICKS(50)) != pdTRUE) { xSemaphoreGive(rbFree); continue; }
    if (!pipeRun || clipSrc != CS_SD || rdEndSent) { xSemaphoreGive(pipeMtx); xSemaphoreGive(rbFree); continue; }

    RbEnt& r = rb[rbW % RB_N];
    r.len = 0; r.end = false; r.x = r.y = r.w = r.h = 0;
    if (clipLoopsTarget && rdPasses >= clipLoopsTarget) {
      r.end = true;
      rdEndSent = true;
    } else {
      const ClipEnt& e = clipEnt[rdSeq % clipFrames];
      r.x = e.x; r.y = e.y; r.w = e.w; r.h = e.h; r.seq = rdSeq;
      if (e.size) {
        sdLock();
        bool ok = clipFile && ((uint32_t)clipFile.position() == e.off || clipFile.seek(e.off));
        size_t got = ok ? clipFile.read(r.buf, e.size) : 0;
        sdUnlock();
        if (got == e.size) r.len = e.size; else sdReadErrs++;
      }
      rdSeq++;
      if (rdSeq % clipFrames == 0) rdPasses++;
    }
    rbW++;
    xSemaphoreGive(pipeMtx);
    xSemaphoreGive(rbFull);
    vTaskDelay(1);
  }
}

static void clipClose() {
  pipeRun = false;
  clipIsOpen = false;
  strmReady = false;
  bool got = pipeMtx && xSemaphoreTake(pipeMtx, pdMS_TO_TICKS(4000)) == pdTRUE;   // wait for producers/decoder to leave
  for (int i = 0; i < 2; i++) if (slots[i].pix) { free(slots[i].pix); slots[i].pix = nullptr; }
  if (clipEnt) { free(clipEnt); clipEnt = nullptr; }
  ringFree();
  if (clipFile) { sdLock(); clipFile.close(); sdUnlock(); }
  clipSrc = CS_NONE;
  held = false;
  if (got) xSemaphoreGive(pipeMtx);
}

static bool clipPrep(uint16_t w, uint16_t h, uint16_t d, bool key, uint32_t n, uint8_t srcKind, bool needRing) {
  size_t cap = (size_t)w * h * 2;
  for (int i = 0; i < 2; i++) {
    slots[i].pix = (uint16_t*)ps_malloc(cap);
    if (!slots[i].pix) { lastErr = "No RAM for frame buffers"; clipClose(); return false; }
  }
  if (needRing && !ringAlloc()) { lastErr = "No RAM for read-ahead ring"; clipClose(); return false; }
  clipW = w; clipH = h; clipDelay = d; clipFrames = n; clipAllKey = key;
  clipOX = (SCR_W - w) / 2;
  clipOY = (SCR_H - h) / 2;
  tft.fillScreen(TFT_BLACK);
  while (xSemaphoreTake(semFull, 0) == pdTRUE) {}
  while (xSemaphoreTake(semFree, 0) == pdTRUE) {}
  xSemaphoreGive(semFree);
  xSemaphoreGive(semFree);
  held = false; wrI = 0; rdI = 0; decFc = 0; rdSeq = 0; rdPasses = 0;
  rdEndSent = false; ramEndSent = false; clipEndFlag = false; sdReadErrs = 0;
  perf.dec = perf.push = perf.fps = 0; perf.drops = 0; perf.cnt = 0; perf.t0 = millis();
  clipSrc = srcKind;
  clipPaced = (srcKind != CS_STREAM);
  clipLoopsTarget = (plActive && srcKind != CS_STREAM) ? (uint32_t)max(1, (int)cfg.sdLoops) : 0;
  clipT0 = millis() + (srcKind == CS_SD ? 250 : 60);
  clipIsOpen = true;
  pipeRun = true;
  return true;
}

static bool clipHeader(const uint8_t* hd, uint16_t& w, uint16_t& h, uint16_t& d, uint16_t& fl, uint32_t& n, uint32_t maxN) {
  if (memcmp(hd, "MJV2", 4) != 0) { lastErr = "Not an MJV2 clip"; return false; }
  memcpy(&w, hd + 4, 2);
  memcpy(&h, hd + 6, 2);
  memcpy(&d, hd + 8, 2);
  memcpy(&fl, hd + 10, 2);
  memcpy(&n, hd + 12, 4);
  if (w < 8 || h < 8 || w > SCR_W || h > SCR_H || n == 0 || n > maxN || d < 15) { lastErr = "Bad clip header"; return false; }
  return true;
}

static bool clipStartRam() {
  clipClose();
  if (!canvas) { lastErr = "PSRAM nei - Video cholbe na"; return false; }
  if (!liveBuf || liveSize < 32) { lastErr = "Not an MJV2 clip"; return false; }
  uint16_t w, h, d, fl;
  uint32_t n;
  if (!clipHeader(liveBuf, w, h, d, fl, n, 20000)) return false;
  size_t off = 16 + (size_t)n * 12;
  if (off > liveSize) { lastErr = "Clip truncated"; return false; }
  clipEnt = (ClipEnt*)ps_malloc((size_t)n * sizeof(ClipEnt));
  if (!clipEnt) { lastErr = "No RAM for clip index"; return false; }
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t* t = liveBuf + 16 + (size_t)i * 12;
    ClipEnt& e = clipEnt[i];
    memcpy(&e.x, t, 2);
    memcpy(&e.y, t + 2, 2);
    memcpy(&e.w, t + 4, 2);
    memcpy(&e.h, t + 6, 2);
    memcpy(&e.size, t + 8, 4);
    if (e.size && (e.w == 0 || e.h == 0 || e.x + e.w > w || e.y + e.h > h)) { lastErr = "Clip rect error"; clipClose(); return false; }
    if (off + e.size > liveSize) { lastErr = "Clip corrupt"; clipClose(); return false; }
    e.off = (uint32_t)off;
    off += e.size;
  }
  return clipPrep(w, h, d, (fl & 1) != 0, n, CS_RAM, false);
}

static bool clipStartSd(const String& path) {
  clipClose();
  if (!canvas) { lastErr = "PSRAM nei - Video cholbe na"; return false; }
  if (!sdOk) { lastErr = "SD card not ready"; return false; }
  uint8_t hd[16];
  size_t fsz = 0;
  bool ok;
  sdLock();
  clipFile = sdfs.open(path, "r");
  ok = clipFile && clipFile.read(hd, 16) == 16;
  if (clipFile) fsz = clipFile.size();
  sdUnlock();
  if (!ok) { lastErr = "Cannot open " + path; clipClose(); return false; }
  uint16_t w, h, d, fl;
  uint32_t n;
  if (!clipHeader(hd, w, h, d, fl, n, 60000)) { clipClose(); return false; }
  size_t off = 16 + (size_t)n * 12;
  if (off > fsz) { lastErr = "Clip truncated"; clipClose(); return false; }
  clipEnt = (ClipEnt*)ps_malloc((size_t)n * sizeof(ClipEnt));
  uint8_t* tmp = (uint8_t*)malloc(340 * 12);
  if (!clipEnt || !tmp) { if (tmp) free(tmp); lastErr = "No RAM for clip index"; clipClose(); return false; }
  uint32_t i = 0;
  while (i < n) {
    uint32_t m = min((uint32_t)340, n - i);
    sdLock();
    size_t got = clipFile.read(tmp, m * 12);
    sdUnlock();
    if (got != m * 12) { free(tmp); lastErr = "SD read error (index)"; clipClose(); return false; }
    for (uint32_t k = 0; k < m; k++, i++) {
      const uint8_t* t = tmp + (size_t)k * 12;
      ClipEnt& e = clipEnt[i];
      memcpy(&e.x, t, 2);
      memcpy(&e.y, t + 2, 2);
      memcpy(&e.w, t + 4, 2);
      memcpy(&e.h, t + 6, 2);
      memcpy(&e.size, t + 8, 4);
      if (e.size && (e.w == 0 || e.h == 0 || e.x + e.w > w || e.y + e.h > h)) { free(tmp); lastErr = "Clip rect error"; clipClose(); return false; }
      if (e.size > RB_CAP) { free(tmp); lastErr = "Frame too big for SD streaming (use a lower quality preset)"; clipClose(); return false; }
      if (off + e.size > fsz) { free(tmp); lastErr = "Clip corrupt/truncated"; clipClose(); return false; }
      e.off = (uint32_t)off;
      off += e.size;
    }
  }
  free(tmp);
  return clipPrep(w, h, d, (fl & 1) != 0, n, CS_SD, true);
}

static bool clipStartStream() {
  clipClose();
  if (!canvas) { lastErr = "PSRAM nei - Stream cholbe na"; return false; }
  uint16_t w = liveW, h = liveH;
  if (w < 16 || h < 16 || w > SCR_W || h > SCR_H) { lastErr = "Bad stream size"; return false; }
  if (!clipPrep(w, h, 66, true, 0, CS_STREAM, true)) return false;
  lastPktMs = millis();
  strmReady = true;
  return true;
}

static void clipTick() {
  if (!clipIsOpen) return;
  if (!held) {
    if (xSemaphoreTake(semFull, 0) != pdTRUE) return;
    held = true;
  }
  Slot& s = slots[rdI];
  if (s.end) {
    clipEndFlag = true;
    held = false;
    rdI ^= 1;
    xSemaphoreGive(semFree);
    return;
  }
  int32_t early = (int32_t)(s.due - millis());
  if (early > 0) return;                         // frame-er shomoy hoy ni
  int32_t late = -early;
  bool drop = clipPaced && clipAllKey && late > (int32_t)clipDelay;
  if (!s.skip && !drop) {
    uint32_t t0 = micros();
    tft.pushImage(clipOX + s.x, clipOY + s.y, s.w, s.h, s.pix);
    perfEma(perf.push, (micros() - t0) / 1000.0f);
  }
  if (drop) perf.drops++;
  else perf.cnt++;
  if (clipPaced && !clipAllKey && late > 1000) clipT0 += (uint32_t)late;   // onek pichhiye gele timeline resync
  uint32_t now = millis();
  if (now - perf.t0 >= 1000) { perf.fps = perf.cnt * 1000.0f / (now - perf.t0); perf.cnt = 0; perf.t0 = now; }
  held = false;
  rdI ^= 1;
  xSemaphoreGive(semFree);
}

// ---- live slot management ----
static void mediaClear() {
  gifStop();
  clipClose();
  if (liveBuf) { free(liveBuf); liveBuf = nullptr; }
  liveSize = 0; liveCap = 0; liveType = LT_NONE;
  liveW = liveH = 0; liveFrames = 0;
  liveName = "";
  sdStreamPath = "";
  clipEndFlag = false;
  stillUntil = 0;
}

static bool liveFinalize() {
  if (liveSize < 16) { lastErr = "File too small"; return false; }
  const uint8_t* b = liveBuf;
  if (!memcmp(b, "RGB5", 4)) {
    uint16_t w, h;
    memcpy(&w, b + 4, 2);
    memcpy(&h, b + 6, 2);
    if (w < 1 || h < 1 || w > SCR_W || h > SCR_H || liveSize < 8 + (size_t)w * h * 2) { lastErr = "Bad image data"; return false; }
    liveType = LT_STILL; liveW = w; liveH = h; liveFrames = 1;
    return true;
  }
  if (!memcmp(b, "MJV2", 4)) {
    uint16_t w, h;
    uint32_t n;
    memcpy(&w, b + 4, 2);
    memcpy(&h, b + 6, 2);
    memcpy(&n, b + 12, 4);
    if (!canvas) { lastErr = "PSRAM missing"; return false; }
    liveType = LT_CLIP; liveW = w; liveH = h; liveFrames = n;
    return true;
  }
  if (!memcmp(b, "GIF8", 4)) {
    if (!canvas) { lastErr = "PSRAM missing"; return false; }
    if (liveSize > MAX_GIF) { lastErr = "GIF >3MB: Chrome/Edge diye 'smooth clip' convert koro"; return false; }
    uint16_t w, h;
    memcpy(&w, b + 6, 2);
    memcpy(&h, b + 8, 2);
    liveType = LT_GIF; liveW = w; liveH = h; liveFrames = 0;
    return true;
  }
  if (b[0] == 0xFF && b[1] == 0xD8) {
    uint16_t w = 0, h = 0;
    int r = TJpgDec.getJpgSize(&w, &h, b, liveSize);
    if (r != 0 || !w || !h) { lastErr = "Invalid/progressive JPEG"; return false; }
    liveType = LT_JPEG; liveW = w; liveH = h; liveFrames = 1;
    return true;
  }
  lastErr = "Unknown file format";
  return false;
}

static bool mediaStart() {
  lastErr = "";
  switch (liveType) {
    case LT_STILL: return showStill();
    case LT_JPEG:  return showJpegBuf(liveBuf, liveSize);
    case LT_GIF:   return gifStart();
    case LT_CLIP:  return sdStreamPath.length() ? clipStartSd(sdStreamPath) : clipStartRam();
    case LT_STREAM: return clipStartStream();
    default: break;
  }
  return false;
}

// ---- play a file from the SD card (small clips are preloaded to RAM, big ones are streamed) ----
static bool sdPlayFile(const String& name) {
  lastErr = "";
  if (!sdOk) { lastErr = "SD card not ready"; return false; }
  int k = sdKindOf(name);
  if (!k || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) { lastErr = "bad file"; return false; }
  String path = "/media/" + name;
  size_t sz = 0;
  uint8_t hd[16];
  bool hdOk = false;
  sdLock();
  {
    File f = sdfs.open(path, "r");
    if (f) {
      sz = f.size();
      if (k == 1) hdOk = f.read(hd, 16) == 16;
    }
  }
  sdUnlock();
  if (!sz) { lastErr = "cannot open " + name; return false; }
  bool stream = false;
  if (k == 1) {
    if (!hdOk || memcmp(hd, "MJV2", 4) != 0) { lastErr = "not an MJV2 clip"; return false; }
    stream = !(sz <= MAX_LIVE && cfg.sdPreload);
  } else if (sz > MAX_LIVE) {
    lastErr = "file too large";
    return false;
  }
  mediaClear();
  if (stream) {
    uint16_t w, h;
    uint32_t n;
    memcpy(&w, hd + 4, 2);
    memcpy(&h, hd + 6, 2);
    memcpy(&n, hd + 12, 4);
    liveType = LT_CLIP; liveW = w; liveH = h; liveFrames = n; liveSize = sz;
    sdStreamPath = path;
  } else {
    liveBuf = (uint8_t*)ps_malloc(sz);
    if (!liveBuf) { lastErr = "not enough PSRAM"; return false; }
    size_t got = 0;
    sdLock();
    {
      File f = sdfs.open(path, "r");
      if (f) got = f.read(liveBuf, sz);
    }
    sdUnlock();
    if (got != sz) { lastErr = "SD read error"; mediaClear(); return false; }
    liveSize = sz; liveCap = sz;
    if (!liveFinalize()) { mediaClear(); return false; }
  }
  liveName = name;
  stillUntil = (plActive && liveType != LT_CLIP) ? millis() + (uint32_t)cfg.sdStillSecs * 1000UL : 0;
  if (cfg.holdOnUpload) autoRotate = false;
  enterPage(P_MEDIA);
  return lastErr.length() == 0;
}

static void sdPlayNext() {
  std::vector<String> names;
  sdListNames(names);
  if (names.empty()) { plActive = false; lastErr = "no media on SD card"; return; }
  for (size_t t = 0; t < names.size(); t++) {
    plIdx = (plIdx + 1) % (int)names.size();
    if (sdPlayFile(names[plIdx])) return;
  }
  plActive = false;
}

static bool sdSaveLive(const String& base, String& err) {
  const char* ext = liveType == LT_CLIP ? ".mjv" : liveType == LT_STILL ? ".rgb" : liveType == LT_GIF ? ".gif" : ".jpg";
  String n = sdSafeName(base + ext);
  if (!n.length()) { err = "bad name"; return false; }
  if (!liveBuf) { err = "nothing in RAM"; return false; }
  return sdSaveBuf(n, liveBuf, liveSize, err);
}

// io task handed us a finished media buffer: show it (and optionally save to SD)
static void commitPending() {
  mediaClear();
  liveBuf = pendBuf; pendBuf = nullptr;
  liveSize = pendLen; liveCap = pendLen;
  lastErr = "";
  if (!liveFinalize()) {
    snprintf(pendMsg, sizeof(pendMsg), "ERR %s", lastErr.c_str());
    mediaClear();
    pendState = 3;
    return;
  }
  liveName = pendName;
  plActive = false;
  const char* note = "OK";
  if (pendFlags & 2) {
    String e;
    if (!sdSaveLive(liveName, e)) note = "OK (shown, but SD save failed)";
  }
  if (cfg.holdOnUpload) autoRotate = false;
  enterPage(P_MEDIA);
  snprintf(pendMsg, sizeof(pendMsg), "%s", note);
  pendState = 2;
}

static void mediaService() {
  if (strmEndReq) {
    strmEndReq = false;
    if (liveType == LT_STREAM) {
      mediaClear();
      autoRotate = cfg.autoRotate;
      enterPage(P_OVERVIEW);
    }
  }
  if (strmReq) {
    strmReq = false;
    mediaClear();
    plActive = false;
    liveType = LT_STREAM; liveW = strmW; liveH = strmH; liveName = "Live stream";
    if (cfg.holdOnUpload) autoRotate = false;
    enterPage(P_MEDIA);
  }
  if (pendState == 1) commitPending();
  if (clipEndFlag) {
    clipEndFlag = false;
    if (plActive) sdPlayNext();
  }
  if (plActive && stillUntil && (int32_t)(millis() - stillUntil) > 0) {
    stillUntil = 0;
    sdPlayNext();
  }
}

// =====================================================================
//  PAGE MANAGER
// =====================================================================
static void renderData() {
  switch (curPage) {
    case P_OVERVIEW: drawHeader(); drawCpuBlock(); drawCores(); drawGraph(); drawBottom(); break;
    case P_TEMP:     drawHeader(); tempDynamic(); break;
    case P_TASKS:    drawHeader(); tasksDynamic(); break;
    case P_DISK:     drawHeader(); diskDynamic(); break;
    case P_ACTIVITY: drawHeader(); activityDynamic(); break;
    default: break;
  }
}

static void drawSetupScreen() {
  gfx.fillScreen(TFT_BLACK);
  gfx.setTextPadding(0);
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.drawString("WiFi Setup", 20, 20, 4);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("1. Phone/PC-te WiFi join koro:", 20, 80, 2);
  gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
  gfx.drawString(AP_NAME, 40, 105, 4);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("2. Browser-e kholo:", 20, 160, 2);
  gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
  gfx.drawString("http://192.168.4.1", 40, 185, 4);
  gfx.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  gfx.drawString("3. Scan -> WiFi select -> Connect", 20, 240, 2);
  if (strlen(cfg.ssid)) gfx.drawString(String("Saved WiFi: ") + cfg.ssid + " (retrying...)", 20, 270, 2);
}

static void enterPage(int p) {
  if (curPage == P_MEDIA && p != P_MEDIA) { gifStop(); clipClose(); }
  curPage = p;
  pageStart = millis();
  gfx.setTextPadding(0);
  gfx.setTextDatum(TL_DATUM);
  switch (p) {
    case P_OVERVIEW: gfx.fillScreen(TFT_BLACK); drawStaticOverview(); renderData(); break;
    case P_TEMP:     gfx.fillScreen(TFT_BLACK); tempStatic();  renderData(); break;
    case P_TASKS:    gfx.fillScreen(TFT_BLACK); tasksStatic(); renderData(); break;
    case P_DISK:     gfx.fillScreen(TFT_BLACK); diskStatic(); renderData(); break;
    case P_ACTIVITY: gfx.fillScreen(TFT_BLACK); activityStatic(); renderData(); break;
    case P_MEDIA:
      if (liveType == LT_NONE) showMsg("No media yet", String("Upload: http://") + myIp() + "/  (Media tab)");
      else if (!mediaStart()) showMsg("Media error", lastErr);
      break;
  }
}

static int stepPage(int dir, bool autoMode) {
  for (int i = 1; i <= P_COUNT; i++) {
    int p = ((curPage + dir * i) % P_COUNT + P_COUNT) % P_COUNT;
    if (!cfg.pageOn[p]) continue;
    if (autoMode && p == P_MEDIA && liveType == LT_NONE) continue;
    return p;
  }
  return curPage;
}

// =====================================================================
//  WiFi / AP / captive portal
// =====================================================================
static void startMdns() {
  MDNS.end();
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}

static void startAP() {
  if (apMode) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME);
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
  setupShown = false;
  apStopAt = 0;
  Serial.print("Setup AP: "); Serial.println(WiFi.softAPIP());
}

static void stopAP() {
  if (!apMode) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
  apStopAt = 0;
}

static void beginWifiTry(const char* ssid, const char* pass) {
  strlcpy(tryS, ssid, sizeof(tryS));
  strlcpy(tryP, pass, sizeof(tryP));
  if (!apMode) startAP();          // fail hole jeno panel-e ferot ashte pare
  WiFi.disconnect(false);
  WiFi.begin(tryS, tryP);
  wifiTry = WS_TRYING;
  tryT0 = millis();
}

static void wifiConnect() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setHostname(MDNS_NAME);
  if (strlen(cfg.ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextDatum(TL_DATUM);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.drawString("Connecting to WiFi...", 20, 110, 4);
    gfx.drawString(cfg.ssid, 20, 150, 4);
    gfxPresent();
    uint32_t t0 = millis();
    int dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      delay(300);
      gfx.drawString(".", 20 + (dots++ % 30) * 12, 200, 4);
      gfxPresent();
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    startMdns();
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextColor(TFT_GREEN, TFT_BLACK);
    gfx.drawString("WiFi connected", 20, 110, 4);
    gfx.drawString(WiFi.localIP().toString(), 20, 150, 4);
    gfxPresent();
    Serial.print("Panel: http://"); Serial.println(WiFi.localIP());
    delay(1500);
  } else {
    startAP();
  }
}

static void wifiTick() {
  uint32_t now = millis();
  if (wifiTry == WS_TRYING) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(tryS)) {
      strlcpy(cfg.ssid, tryS, sizeof(cfg.ssid));
      strlcpy(cfg.pass, tryP, sizeof(cfg.pass));
      saveCfg();
      wifiTry = WS_OK;
      apStopAt = now + 180000;     // 3 min por hotspot bondho
      startMdns();
    } else if (now - tryT0 > 15000) {
      wifiTry = WS_FAIL;
      WiFi.disconnect(false);
      if (strlen(cfg.ssid)) WiFi.begin(cfg.ssid, cfg.pass);
    }
    return;
  }
  bool conn = WiFi.status() == WL_CONNECTED;
  if (apMode) {
    if (conn && apStopAt == 0) apStopAt = now + 120000;
    if (conn && apStopAt && now > apStopAt) stopAP();
    else if (!conn && WiFi.softAPgetStationNum() == 0 && strlen(cfg.ssid) && now - lastRetry > 60000) {
      lastRetry = now;
      WiFi.begin(cfg.ssid, cfg.pass);
    }
  } else if (!conn && now - lastRetry > 5000) {
    lastRetry = now;
    WiFi.reconnect();
  }
}

// =====================================================================
//  POLL TASK (PC agent)
// =====================================================================
static bool fetchStats(const char* ip, uint16_t port) {
  if (WiFi.status() != WL_CONNECTED) { strcpy(pollErr, "no wifi"); return false; }
  HTTPClient http;
  String url = String("http://") + ip + ":" + port + "/stats";
  http.setConnectTimeout(1200);
  http.setTimeout(1500);
  if (!http.begin(url)) { strcpy(pollErr, "bad url"); return false; }
  int code = http.GET();
  if (code != 200) { snprintf(pollErr, sizeof(pollErr), "http %d", code); http.end(); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { strcpy(pollErr, "bad json"); return false; }
  pollErr[0] = 0;

  Stats T;
  strlcpy(T.host, doc["host"] | "-", sizeof(T.host));
  strlcpy(T.name, doc["name"] | "-", sizeof(T.name));
  T.cpu = doc["cpu"] | 0.0f;
  T.freq = doc["freq"] | 0.0f;
  T.temp = doc["temp"] | -1.0f;
  T.ram = doc["ram"] | 0.0f;
  T.ramUsed = doc["ram_used"] | 0.0f;
  T.ramTotal = doc["ram_total"] | 0.0f;
  T.disk = doc["disk"] | 0.0f;
  T.up = doc["up"] | 0.0f;
  T.down = doc["down"] | 0.0f;
  T.uptime = doc["uptime"] | 0;
  T.nproc = doc["nproc"] | 0;
  T.gpu = doc["gpu"] | -1.0f;
  T.gpuTemp = doc["gpu_temp"] | -1.0f;
  T.gpuMemUsed = doc["gpu_mem_used"] | 0.0f;
  T.gpuMemTotal = doc["gpu_mem_total"] | 0.0f;
  T.dr = doc["dr"] | 0.0f;
  T.dw = doc["dw"] | 0.0f;
  T.swap = doc["swap"] | 0.0f;
  T.bat = doc["bat"] | -1;
  T.plug = doc["plug"] | false;
  strlcpy(T.win, doc["win"] | "", sizeof(T.win));
  strlcpy(T.wapp, doc["wapp"] | "", sizeof(T.wapp));
  strlcpy(T.clk, doc["time"] | "--:--", sizeof(T.clk));
  clockSync(T.clk);
  strlcpy(T.dat, doc["date"] | "", sizeof(T.dat));
  T.nDrives = 0;
  for (JsonObject o : doc["drives"].as<JsonArray>()) {
    if (T.nDrives >= 4) break;
    Drive& dv = T.drives[T.nDrives++];
    strlcpy(dv.n, o["n"] | "?", sizeof(dv.n));
    dv.p = o["p"] | 0.0f; dv.u = o["u"] | 0.0f; dv.t = o["t"] | 0.0f;
  }
  T.nRp = 0;
  for (JsonObject o : doc["rprocs"].as<JsonArray>()) {
    if (T.nRp >= 4) break;
    Rp& rp = T.rprocs[T.nRp++];
    strlcpy(rp.n, o["n"] | "?", sizeof(rp.n));
    rp.m = o["m"] | 0;
  }
  T.nDp = 0;
  for (JsonObject o : doc["dprocs"].as<JsonArray>()) {
    if (T.nDp >= 4) break;
    Dp& dp = T.dprocs[T.nDp++];
    strlcpy(dp.n, o["n"] | "?", sizeof(dp.n));
    dp.r = o["r"] | 0.0f;
  }
  JsonArray c = doc["cores"].as<JsonArray>();
  T.nCores = min((int)c.size(), 64);
  for (int i = 0; i < T.nCores; i++) T.cores[i] = c[i] | 0.0f;
  T.nProcs = 0;
  JsonArray pa = doc["procs"].as<JsonArray>();
  for (JsonObject o : pa) {
    if (T.nProcs >= 8) break;
    Proc& pr = T.procs[T.nProcs++];
    strlcpy(pr.n, o["n"] | "?", sizeof(pr.n));
    pr.c = o["c"] | 0.0f;
    pr.m = o["m"] | 0;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  SH = T;
  xSemaphoreGive(mtx);
  return true;
}

static void pollTask(void*) {
  for (;;) {
    uint32_t t0 = millis();
    char ip[16];
    strlcpy(ip, cfg.pcIp, sizeof(ip));
    bool ok = fetchStats(ip, cfg.pcPort);
    if (ok) { failCount = 0; pcOnline = true; lastOk = true; }
    else {
      lastOk = false;
      if (failCount < 250) failCount++;
      if (failCount >= 3) pcOnline = false;
    }
    newData = true;
    uint32_t el = millis() - t0;
    uint32_t iv = cfg.pollMs;
    vTaskDelay(pdMS_TO_TICKS(el < iv ? iv - el : 20));
  }
}

static void takeSnapshot() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  S = SH;
  xSemaphoreGive(mtx);
  if (!lastOk) return;
  memmove(cpuHist, cpuHist + 1, sizeof(float) * (HN - 1));
  memmove(tempHist, tempHist + 1, sizeof(float) * (HN - 1));
  memmove(gpuTHist, gpuTHist + 1, sizeof(float) * (HN - 1));
  memmove(dRHist, dRHist + 1, sizeof(float) * (HN - 1));
  memmove(dWHist, dWHist + 1, sizeof(float) * (HN - 1));
  cpuHist[HN - 1] = S.cpu;
  tempHist[HN - 1] = S.temp > 0 ? S.temp : 0;
  gpuTHist[HN - 1] = S.gpuTemp > 0 ? S.gpuTemp : 0;
  dRHist[HN - 1] = S.dr;
  dWHist[HN - 1] = S.dw;
}

// =====================================================================
//  WEB API
// =====================================================================
static bool liveOk = false;
static String liveUpErr = "";

static void sendJsonDoc(JsonDocument& d) {
  String o;
  serializeJson(d, o);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", o);
}
static void sendOk() { web.send(200, "application/json", "{\"ok\":true}"); }
static void sendFail(const String& e) {
  JsonDocument d;
  d["ok"] = false;
  d["err"] = e;
  sendJsonDoc(d);
}

static void handleState() {
  JsonDocument d;
  d["page"] = curPage;
  d["auto"] = autoRotate;
  JsonArray pa = d["pages"].to<JsonArray>();
  for (int i = 0; i < P_COUNT; i++) {
    JsonObject o = pa.add<JsonObject>();
    o["n"] = PAGE_NAME[i];
    o["on"] = (bool)cfg.pageOn[i];
    o["s"] = cfg.pageSecs[i];
  }
  d["gifSkip"] = (bool)cfg.gifSkip;
  d["clock12"] = clock12;
  d["clockSec"] = clockSec;
  d["hold"] = (bool)cfg.holdOnUpload;
  d["bright"] = cfg.bright;
  d["rot"] = cfg.rotation;
  JsonObject pc = d["pc"].to<JsonObject>();
  pc["ip"] = cfg.pcIp;
  pc["port"] = cfg.pcPort;
  pc["poll"] = cfg.pollMs;
  pc["online"] = (bool)pcOnline;
  pc["host"] = S.host;
  JsonObject w = d["wifi"].to<JsonObject>();
  w["ssid"] = cfg.ssid;
  w["rssi"] = WiFi.RSSI();
  w["ip"] = WiFi.localIP().toString();
  w["ap"] = apMode;
  w["connected"] = WiFi.status() == WL_CONNECTED;
  w["mdns"] = String(MDNS_NAME) + ".local";
  JsonObject sy = d["sys"].to<JsonObject>();
  sy["up"] = millis() / 1000;
  sy["heap"] = ESP.getFreeHeap();
  sy["psram"] = ESP.getFreePsram();
  sy["psramOk"] = canvas != nullptr;
  sy["build"] = String(__DATE__) + " " + __TIME__;
  JsonObject m = d["media"].to<JsonObject>();
  const char* tn = "none";
  if (liveType == LT_STILL || liveType == LT_JPEG) tn = "image";
  else if (liveType == LT_CLIP) tn = "video";
  else if (liveType == LT_GIF) tn = "gif";
  else if (liveType == LT_STREAM) tn = "stream";
  m["type"] = tn;
  m["name"] = liveName;
  m["w"] = liveW;
  m["h"] = liveH;
  m["frames"] = liveFrames;
  m["bytes"] = (uint32_t)liveSize;
  const char* sn = clipSrc == CS_RAM ? "RAM" : clipSrc == CS_SD ? "SD stream" : clipSrc == CS_STREAM ? "live stream" : "-";
  JsonObject pf = m["perf"].to<JsonObject>();
  pf["src"] = sn;
  pf["dec"] = perf.dec;
  pf["push"] = perf.push;
  pf["fps"] = perf.fps;
  pf["drops"] = perf.drops;
  pf["delay"] = clipDelay;
  pf["sdErrs"] = sdReadErrs;
  JsonObject sd = d["sd"].to<JsonObject>();
  sd["ok"] = sdOk;
#ifdef SD_USE_SPI
  sd["mode"] = "SPI";
#elif defined(SD_4BIT)
  sd["mode"] = "SD_MMC 4-bit";
#else
  sd["mode"] = "SD_MMC 1-bit";
#endif
  sd["total"] = (uint64_t)sdTotal;
  sd["used"] = (uint64_t)sdUsed;
  sd["err"] = sdErr;
  sd["playlist"] = plActive;
  sd["loops"] = cfg.sdLoops;
  sd["preload"] = (bool)cfg.sdPreload;
  sd["still"] = cfg.sdStillSecs;
  sd["playing"] = sdStreamPath;
  d["err"] = lastErr;
  sendJsonDoc(d);
}

static void handleStats() {
  JsonDocument d;
  d["host"] = S.host; d["name"] = S.name;
  d["cpu"] = S.cpu; d["freq"] = S.freq; d["temp"] = S.temp;
  d["ram"] = S.ram; d["ram_used"] = S.ramUsed; d["ram_total"] = S.ramTotal;
  d["disk"] = S.disk; d["up"] = S.up; d["down"] = S.down; d["uptime"] = S.uptime;
  d["gpu"] = S.gpu; d["gpu_temp"] = S.gpuTemp;
  d["gpu_mem_used"] = S.gpuMemUsed; d["gpu_mem_total"] = S.gpuMemTotal;
  d["nproc"] = S.nproc; d["online"] = (bool)pcOnline;
  d["dr"] = S.dr; d["dw"] = S.dw; d["swap"] = S.swap; d["bat"] = S.bat; d["plug"] = S.plug;
  d["win"] = S.win; d["wapp"] = S.wapp; d["time"] = S.clk; d["date"] = S.dat;
  JsonArray dvs = d["drives"].to<JsonArray>();
  for (int i = 0; i < S.nDrives; i++) {
    JsonObject o = dvs.add<JsonObject>();
    o["n"] = S.drives[i].n; o["p"] = S.drives[i].p; o["u"] = S.drives[i].u; o["t"] = S.drives[i].t;
  }
  JsonArray rps = d["rprocs"].to<JsonArray>();
  for (int i = 0; i < S.nRp; i++) { JsonObject o = rps.add<JsonObject>(); o["n"] = S.rprocs[i].n; o["m"] = S.rprocs[i].m; }
  JsonArray dps = d["dprocs"].to<JsonArray>();
  for (int i = 0; i < S.nDp; i++) { JsonObject o = dps.add<JsonObject>(); o["n"] = S.dprocs[i].n; o["r"] = S.dprocs[i].r; }
  JsonArray c = d["cores"].to<JsonArray>();
  for (int i = 0; i < S.nCores; i++) c.add((int)S.cores[i]);
  JsonArray pr = d["procs"].to<JsonArray>();
  for (int i = 0; i < S.nProcs; i++) {
    JsonObject o = pr.add<JsonObject>();
    o["n"] = S.procs[i].n; o["c"] = S.procs[i].c; o["m"] = S.procs[i].m;
  }
  sendJsonDoc(d);
}

static void handleSettings() {
  JsonDocument d;
  if (deserializeJson(d, web.arg("plain"))) { sendFail("bad json"); return; }

  if (d["page"].is<JsonObject>()) {                 // single page update
    JsonObject o = d["page"];
    int i = o["i"] | -1;
    if (i >= 0 && i < P_COUNT) {
      if (o["on"].is<bool>()) cfg.pageOn[i] = o["on"].as<bool>();
      if (o["s"].is<int>()) cfg.pageSecs[i] = constrain(o["s"].as<int>(), 2, 3600);
    }
    bool any = false;
    for (int k = 0; k < P_COUNT; k++) any |= cfg.pageOn[k];
    if (!any) cfg.pageOn[P_OVERVIEW] = 1;
  }
  if (d["gifSkip"].is<bool>()) cfg.gifSkip = d["gifSkip"].as<bool>();
  if (d["hold"].is<bool>()) cfg.holdOnUpload = d["hold"].as<bool>();
  if (d["auto"].is<bool>()) { cfg.autoRotate = d["auto"].as<bool>(); autoRotate = cfg.autoRotate; pageStart = millis(); }
  if (d["bright"].is<int>()) { cfg.bright = constrain(d["bright"].as<int>(), 5, 255); blSet(cfg.bright); }
  if (d["rot"].is<int>()) {
    int r = d["rot"].as<int>() == 3 ? 3 : 1;
    if (r != cfg.rotation) { cfg.rotation = r; applyRot(r); enterPage(curPage); }
  }
  if (d["pcIp"].is<const char*>()) strlcpy(cfg.pcIp, d["pcIp"].as<const char*>(), sizeof(cfg.pcIp));
  if (d["pcPort"].is<int>()) cfg.pcPort = constrain(d["pcPort"].as<int>(), 1, 65535);
  if (d["pollMs"].is<int>()) cfg.pollMs = constrain(d["pollMs"].as<int>(), 300, 10000);
  if (d["sdLoops"].is<int>()) cfg.sdLoops = constrain(d["sdLoops"].as<int>(), 1, 50);
  if (d["sdStill"].is<int>()) cfg.sdStillSecs = constrain(d["sdStill"].as<int>(), 2, 3600);
  if (d["sdPreload"].is<bool>()) cfg.sdPreload = d["sdPreload"].as<bool>();
  if (d["clock12"].is<bool>()) { clock12 = d["clock12"].as<bool>(); clockSavePrefs(); lastClkKey = -1; }
  if (d["clockSec"].is<bool>()) { clockSec = d["clockSec"].as<bool>(); clockSavePrefs(); lastClkKey = -1; }
  if (d["save"] | false) saveCfg();
  renderData();
  sendOk();
}

static void handleReset() {
  Cfg keep = cfg;
  cfgDefaults();
  strlcpy(cfg.ssid, keep.ssid, sizeof(cfg.ssid));
  strlcpy(cfg.pass, keep.pass, sizeof(cfg.pass));
  strlcpy(cfg.pcIp, keep.pcIp, sizeof(cfg.pcIp));
  cfg.pcPort = keep.pcPort;
  autoRotate = cfg.autoRotate;
  blSet(cfg.bright);
  applyRot(cfg.rotation);
  saveCfg();
  enterPage(curPage);
  sendOk();
}

static void handleScan() {
  if (web.arg("start") == "1") {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
  int n = WiFi.scanComplete();
  JsonDocument d;
  if (n == WIFI_SCAN_RUNNING) d["state"] = "scanning";
  else if (n < 0) d["state"] = "idle";
  else {
    d["state"] = "done";
    JsonArray a = d["nets"].to<JsonArray>();
    for (int i = 0; i < n && i < 30; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      JsonObject o = a.add<JsonObject>();
      o["s"] = s;
      o["r"] = WiFi.RSSI(i);
      o["e"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
  }
  sendJsonDoc(d);
}

static void handleWifiSet() {
  JsonDocument d;
  if (deserializeJson(d, web.arg("plain")) || !d["ssid"].is<const char*>()) { sendFail("ssid missing"); return; }
  String ssid = d["ssid"].as<const char*>();
  String pass = d["pass"].is<const char*>() ? String(d["pass"].as<const char*>()) : String("");
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) { sendFail("bad ssid/password length"); return; }
  if (pass.length() == 0 && ssid == String(cfg.ssid)) pass = cfg.pass;   // khali = ager password
  sendOk();
  beginWifiTry(ssid.c_str(), pass.c_str());
}

static void handleWifiStatus() {
  JsonDocument d;
  const char* st = wifiTry == WS_TRYING ? "trying" : wifiTry == WS_OK ? "ok" : wifiTry == WS_FAIL ? "fail" : "idle";
  d["state"] = st;
  d["ip"] = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["connected"] = WiFi.status() == WL_CONNECTED;
  d["ap"] = apMode;
  d["mdns"] = String(MDNS_NAME) + ".local";
  sendJsonDoc(d);
}

// ---- live media upload: /live?size=N&hold=1&name=xyz  (multipart, PSRAM-e jomay) ----
static void handleLiveData() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    liveOk = false;
    liveUpErr = "";
    size_t want = (size_t)web.arg("size").toInt();
    if (want < 16 || want > MAX_LIVE) { liveUpErr = "bad size (max 6MB)"; return; }
    mediaClear();
    liveBuf = (uint8_t*)ps_malloc(want);
    if (!liveBuf) { liveUpErr = "not enough PSRAM"; return; }
    liveCap = want;
    liveSize = 0;
    liveOk = true;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (liveOk) {
      if (liveSize + u.currentSize > liveCap) { liveOk = false; liveUpErr = "size mismatch"; }
      else { memcpy(liveBuf + liveSize, u.buf, u.currentSize); liveSize += u.currentSize; }
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (liveOk) {
      lastErr = "";
      if (!liveFinalize()) { liveOk = false; liveUpErr = lastErr; }
    }
    if (!liveOk) mediaClear();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    mediaClear();
    liveOk = false;
    liveUpErr = "aborted";
  }
}

static void handleLiveDone() {
  if (!liveOk) { sendFail(liveUpErr.length() ? liveUpErr : String("upload failed")); return; }
  liveName = web.arg("name").substring(0, 40);
  if (web.arg("hold") != "0") autoRotate = false;       // hold (flash-e save hoy na)
  lastErr = "";
  enterPage(P_MEDIA);
  if (lastErr.length()) sendFail(lastErr); else sendOk();
}

// =====================================================================
//  SD WEB API
// =====================================================================
static void handleSdList() {
  if (!sdOk && millis() - sdLastMount > 3000) sdBegin();       // hot-plug: panel opened -> try mount
  JsonDocument d;
  d["ok"] = sdOk;
  d["err"] = sdErr;
  d["total"] = (uint64_t)sdTotal;
  d["used"] = (uint64_t)sdUsed;
  JsonArray a = d["files"].to<JsonArray>();
  if (sdOk) {
    sdLock();
    File dir = sdfs.open("/media");
    if (dir && dir.isDirectory()) {
      int cnt = 0;
      File f = dir.openNextFile();
      while (f && cnt < 200) {
        if (!f.isDirectory()) {
          String n = sdBaseName(f.name());
          int k = sdKindOf(n);
          if (k) {
            JsonObject o = a.add<JsonObject>();
            o["n"] = n;
            o["s"] = (uint32_t)f.size();
            o["k"] = k;
            if (k == 1) {
              uint8_t hd[16];
              if (f.read(hd, 16) == 16 && !memcmp(hd, "MJV2", 4)) {
                uint16_t w, h, dl, fl;
                uint32_t nf;
                memcpy(&w, hd + 4, 2); memcpy(&h, hd + 6, 2); memcpy(&dl, hd + 8, 2); memcpy(&fl, hd + 10, 2); memcpy(&nf, hd + 12, 4);
                o["w"] = w; o["h"] = h; o["fr"] = nf; o["ms"] = (uint32_t)nf * dl; o["fps"] = dl ? 1000 / dl : 0;
              }
            }
            cnt++;
          }
        }
        f = dir.openNextFile();
      }
    }
    sdUnlock();
  }
  sendJsonDoc(d);
}

static void handleSdPlay() {
  plActive = false;
  String n = web.arg("f");
  if (sdPlayFile(n)) sendOk(); else sendFail(lastErr.length() ? lastErr : String("play failed"));
}

static void handleSdPlayAll() {
  if (web.arg("on") == "1") {
    plActive = true;
    plIdx = -1;
    if (cfg.holdOnUpload) autoRotate = false;
    sdPlayNext();
    if (!plActive) { sendFail(lastErr.length() ? lastErr : String("nothing to play")); return; }
  } else {
    plActive = false;
    stillUntil = 0;
    if (liveType == LT_CLIP) clipLoopsTarget = 0;
  }
  sendOk();
}

static void handleSdDelete() {
  String n = web.arg("f");
  if (!sdKindOf(n) || n.indexOf('/') >= 0 || n.indexOf("..") >= 0) { sendFail("bad name"); return; }
  String path = "/media/" + n;
  if (sdStreamPath == path) {
    plActive = false;
    mediaClear();
    if (curPage == P_MEDIA) enterPage(P_MEDIA);
  }
  bool ok;
  sdLock();
  ok = sdfs.remove(path);
  if (ok) sdRefreshSpace();
  sdUnlock();
  if (ok) sendOk(); else sendFail("delete failed");
}

static void handleSdMount() {
  bool ok = sdBegin();
  if (ok) sendOk(); else sendFail(sdErr);
}

// ---- SD upload (multipart, streamed to the card through a 32 KB buffer) ----
#define SD_WB_CAP 32768
static File sdUp;
static bool sdUpOk = false;
static String sdUpErr = "", sdUpName = "", sdUpFinal = "", sdUpPart = "";
static size_t sdUpBytes = 0, sdUpWant = 0, sdWbLen = 0;
static uint8_t* sdWb = nullptr;

static bool sdWbFlush() {
  if (!sdWbLen) return true;
  sdLock();
  size_t w = sdUp ? sdUp.write(sdWb, sdWbLen) : 0;
  sdUnlock();
  bool ok = (w == sdWbLen);
  sdWbLen = 0;
  return ok;
}
static void sdUpAbort(const char* e) {
  sdUpOk = false;
  sdUpErr = e;
  sdLock();
  if (sdUp) sdUp.close();
  sdfs.remove(sdUpPart);
  sdUnlock();
  if (sdWb) { free(sdWb); sdWb = nullptr; }
  sdWbLen = 0;
}
static void handleSdUploadData() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    sdUpOk = false; sdUpErr = ""; sdUpBytes = 0; sdWbLen = 0;
    if (!sdOk) { sdUpErr = "no SD card"; return; }
    sdUpName = sdSafeName(web.arg("name"));
    if (!sdUpName.length()) { sdUpErr = "bad file name/type (use .mjv .rgb .gif .jpg)"; return; }
    sdUpWant = (size_t)strtoul(web.arg("size").c_str(), nullptr, 10);
    if (sdUpWant < 16 || sdUpWant > MAX_SD_FILE) { sdUpErr = "bad size"; return; }
    if (sdUpWant + 1048576ULL > sdTotal - sdUsed) { sdUpErr = "SD card full"; return; }
    sdWb = (uint8_t*)ps_malloc(SD_WB_CAP);
    if (!sdWb) { sdUpErr = "no RAM"; return; }
    sdUpFinal = "/media/" + sdUpName;
    sdUpPart = sdUpFinal + ".part";
    sdLock();
    sdUp = sdfs.open(sdUpPart, FILE_WRITE);
    sdUnlock();
    if (!sdUp) { free(sdWb); sdWb = nullptr; sdUpErr = "cannot create file"; return; }
    sdUpOk = true;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!sdUpOk) return;
    size_t off = 0;
    while (off < u.currentSize) {
      size_t m = min((size_t)(SD_WB_CAP - sdWbLen), (size_t)u.currentSize - off);
      memcpy(sdWb + sdWbLen, u.buf + off, m);
      sdWbLen += m; off += m; sdUpBytes += m;
      if (sdWbLen == SD_WB_CAP && !sdWbFlush()) { sdUpAbort("write failed (card full or removed)"); return; }
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (!sdUpOk) return;
    if (!sdWbFlush()) { sdUpAbort("write failed"); return; }
    bool ok = (sdUpBytes == sdUpWant);
    sdLock();
    sdUp.close();
    if (ok) {
      if (sdfs.exists(sdUpFinal)) sdfs.remove(sdUpFinal);
      ok = sdfs.rename(sdUpPart, sdUpFinal);
      if (ok) sdRefreshSpace();
    } else {
      sdfs.remove(sdUpPart);
    }
    sdUnlock();
    free(sdWb); sdWb = nullptr;
    if (!ok) { sdUpOk = false; sdUpErr = (sdUpBytes != sdUpWant) ? "size mismatch" : "rename failed"; }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    sdUpAbort("aborted");
  }
}
static void handleSdUploadDone() {
  if (!sdUpOk) { sendFail(sdUpErr.length() ? sdUpErr : String("upload failed")); return; }
  plActive = false;
  if (web.arg("play") == "1") {
    if (!sdPlayFile(sdUpName)) { sendFail("saved to SD, but play failed: " + lastErr); return; }
  }
  sendOk();
}

// =====================================================================
//  LIVE STREAM + REMOTE UPLOAD over USB cable and WiFi (same packet protocol)
//  Packet: 'P''M''U''X' | type u8 | flags u8 | len u32 LE | payload
//   'B' begin stream  payload: u16 w, u16 h, u16 fps
//   'F' frame/tile    payload: u16 x, u16 y, u16 w, u16 h, JPEG   (device answers "@PMUX A\n" per frame = flow control)
//   'E' end stream
//   'M' media upload  flags: bit0 show, bit1 also save to SD, bit2 SD only
//                     payload: u8 nameLen, name, data (RGB5 / MJV2 / GIF / JPEG)
//   'P' ping
// =====================================================================
static WiFiServer streamSrv(STREAM_PORT);
static WiFiClient strmCli;

static bool ioReadN(Stream& s, uint8_t* dst, size_t n, uint32_t toMs = 1500) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n) {
    int av = s.available();
    if (av <= 0) {
      if (millis() - t0 > toMs) return false;
      vTaskDelay(1);
      continue;
    }
    size_t m = s.readBytes((char*)dst + got, min((size_t)av, n - got));
    got += m;
    if (m) t0 = millis();
  }
  return true;
}
static void ioDiscard(Stream& s, size_t n) {
  static uint8_t tmp[512];
  while (n) {
    size_t m = min(n, sizeof(tmp));
    if (!ioReadN(s, tmp, m)) return;
    n -= m;
  }
}
static bool ioMagic(Stream& s) {
  static const char M[4] = {'P', 'M', 'U', 'X'};
  int k = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 250) {
    if (!s.available()) { vTaskDelay(1); continue; }
    int c = s.read();
    if (c < 0) continue;
    if (c == M[k]) { if (++k == 4) return true; }
    else k = (c == M[0]) ? 1 : 0;
    t0 = millis();
  }
  return false;
}
static void ioReply(Stream& s, const char* msg) {
  char b[128];
  int n = snprintf(b, sizeof(b), "@PMUX %s\n", msg);
  s.write((const uint8_t*)b, n);
}

static void ioFrame(Stream& s, uint32_t plen) {
  uint8_t h8[8];
  if (plen < 9 || !ioReadN(s, h8, 8)) { ioDiscard(s, plen > 8 ? plen - 8 : 0); return; }
  uint16_t x, y, w, h;
  memcpy(&x, h8, 2); memcpy(&y, h8 + 2, 2); memcpy(&w, h8 + 4, 2); memcpy(&h, h8 + 6, 2);
  uint32_t jl = plen - 8;
  bool rectOk = w && h && (uint32_t)x + w <= clipW && (uint32_t)y + h <= clipH;
  if (rectOk && jl <= RB_CAP && strmReady && pipeRun && clipSrc == CS_STREAM) {
    for (uint32_t t0 = millis(); millis() - t0 < 1500;) {
      if (!(pipeRun && clipSrc == CS_STREAM)) break;
      if (xSemaphoreTake(rbFree, pdMS_TO_TICKS(20)) != pdTRUE) continue;
      if (xSemaphoreTake(pipeMtx, pdMS_TO_TICKS(100)) != pdTRUE) { xSemaphoreGive(rbFree); continue; }
      if (pipeRun && clipSrc == CS_STREAM) {
        RbEnt& r = rb[rbW % RB_N];
        bool ok = ioReadN(s, r.buf, jl);
        if (ok) {
          r.len = jl; r.x = x; r.y = y; r.w = w; r.h = h; r.end = false; r.seq = 0;
          rbW++;
          xSemaphoreGive(rbFull);
        } else {
          xSemaphoreGive(rbFree);
        }
        xSemaphoreGive(pipeMtx);
        if (ok) ioReply(s, "A");
        return;
      }
      xSemaphoreGive(pipeMtx);
      xSemaphoreGive(rbFree);
    }
  }
  ioDiscard(s, jl);          // not displayed (page left / too big / bad rect): keep the wire in sync
  ioReply(s, "A");
}

static void ioMedia(Stream& s, uint8_t flags, uint32_t plen) {
  uint8_t nl = 0;
  if (plen < 18 || !ioReadN(s, &nl, 1)) { ioDiscard(s, plen > 1 ? plen - 1 : 0); ioReply(s, "ERR bad packet"); return; }
  char name[48];
  size_t nn = min((size_t)nl, sizeof(name) - 1);
  if (!ioReadN(s, (uint8_t*)name, nl)) { ioReply(s, "ERR timeout"); return; }
  name[nn] = 0;
  uint32_t dlen = plen - 1 - nl;
  bool sdOnly = (flags & 4) != 0;
  if (sdOnly) {
    String fn = sdSafeName(String(name));
    if (!sdOk || !fn.length() || dlen + 1048576ULL > sdTotal - sdUsed) {
      ioDiscard(s, dlen);
      ioReply(s, !sdOk ? "ERR no SD card" : !fn.length() ? "ERR bad name/type" : "ERR SD full");
      return;
    }
    String fin = "/media/" + fn, part = fin + ".part";
    uint8_t* buf = (uint8_t*)ps_malloc(16384);
    if (!buf) { ioDiscard(s, dlen); ioReply(s, "ERR no RAM"); return; }
    sdLock();
    File f = sdfs.open(part, FILE_WRITE);
    sdUnlock();
    bool ok = (bool)f;
    uint32_t left = dlen;
    while (ok && left) {
      size_t m = min((uint32_t)16384, left);
      if (!ioReadN(s, buf, m, 3000)) { ok = false; break; }
      sdLock();
      size_t w = f.write(buf, m);
      sdUnlock();
      if (w != m) { ok = false; break; }
      left -= m;
    }
    sdLock();
    if (f) f.close();
    if (ok) {
      if (sdfs.exists(fin)) sdfs.remove(fin);
      ok = sdfs.rename(part, fin);
      if (ok) sdRefreshSpace();
    } else {
      sdfs.remove(part);
    }
    sdUnlock();
    free(buf);
    ioReply(s, ok ? "OK saved to SD" : "ERR SD write/transfer failed");
    return;
  }
  if (dlen < 16 || dlen > MAX_LIVE || pendState != 0) {
    ioDiscard(s, dlen);
    ioReply(s, pendState != 0 ? "ERR busy" : "ERR bad size (max 5.5MB for RAM)");
    return;
  }
  uint8_t* buf = (uint8_t*)ps_malloc(dlen);
  if (!buf) { ioDiscard(s, dlen); ioReply(s, "ERR not enough PSRAM"); return; }
  uint32_t got = 0;
  bool ok = true;
  while (got < dlen) {
    size_t m = min((uint32_t)8192, dlen - got);
    if (!ioReadN(s, buf + got, m, 3000)) { ok = false; break; }
    got += m;
  }
  if (!ok) { free(buf); ioReply(s, "ERR transfer timeout"); return; }
  pendBuf = buf; pendLen = dlen; pendFlags = flags;
  strlcpy(pendName, name, sizeof(pendName));
  pendState = 1;                                         // loop task takes over
  for (uint32_t t0 = millis(); pendState == 1 && millis() - t0 < 30000;) vTaskDelay(5);
  if (pendState == 1) { ioReply(s, "ERR device busy"); return; }
  ioReply(s, pendMsg);
  pendState = 0;
}

static void ioPacket(Stream& s) {
  uint8_t h[6];
  if (!ioReadN(s, h, 6)) return;
  char type = (char)h[0];
  uint8_t flags = h[1];
  uint32_t plen = h[2] | ((uint32_t)h[3] << 8) | ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 24);
  lastPktMs = millis();
  switch (type) {
    case 'P': {
      ioDiscard(s, plen);
      char b[48];
      snprintf(b, sizeof(b), "PONG v1 sd=%d", sdOk ? 1 : 0);
      ioReply(s, b);
      break;
    }
    case 'B': {
      uint8_t p[6];
      if (plen != 6 || !ioReadN(s, p, 6)) { ioDiscard(s, plen > 6 ? plen - 6 : 0); ioReply(s, "ERR bad begin"); break; }
      uint16_t w, hh;
      memcpy(&w, p, 2); memcpy(&hh, p + 2, 2);
      if (w < 16 || hh < 16 || w > SCR_W || hh > SCR_H) { ioReply(s, "ERR size 16..480 x 16..320"); break; }
      strmReady = false;
      strmW = w; strmH = hh;
      strmReq = true;
      for (uint32_t t0 = millis(); !strmReady && millis() - t0 < 4000;) vTaskDelay(5);
      ioReply(s, strmReady ? "OK stream" : "ERR cannot start (PSRAM?)");
      break;
    }
    case 'F': ioFrame(s, plen); break;
    case 'E': ioDiscard(s, plen); strmEndReq = true; ioReply(s, "OK end"); break;
    case 'M': ioMedia(s, flags, plen); break;
    default:  ioDiscard(s, plen); ioReply(s, "ERR unknown type"); break;
  }
}

static void ioTask(void*) {
  for (;;) {
    if (!strmCli || !strmCli.connected()) {
      if (strmCli) { strmCli.stop(); if (liveType == LT_STREAM) strmEndReq = true; }
      WiFiClient c = streamSrv.available();
      if (c) { strmCli = c; strmCli.setNoDelay(true); }
    }
    Stream* src = nullptr;
    if (strmCli && strmCli.connected() && strmCli.available()) src = &strmCli;
    else if (Serial.available()) src = &Serial;
    if (!src) {
      if (liveType == LT_STREAM && millis() - lastPktMs > 10000) { strmEndReq = true; lastPktMs = millis(); }
      vTaskDelay(2);
      continue;
    }
    if (ioMagic(*src)) ioPacket(*src);
    vTaskDelay(1);
  }
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", PANEL_HTML); });
  web.on("/stats", HTTP_GET, handleStats);
  web.on("/api/state", HTTP_GET, handleState);
  web.on("/api/settings", HTTP_POST, handleSettings);
  web.on("/api/reset", HTTP_POST, handleReset);
  web.on("/api/page", HTTP_GET, []() {
    int n = web.arg("n").toInt();
    if (n >= 0 && n < P_COUNT) enterPage(n);
    sendOk();
  });
  web.on("/api/next", HTTP_GET, []() { enterPage(stepPage(1, false)); sendOk(); });
  web.on("/api/prev", HTTP_GET, []() { enterPage(stepPage(-1, false)); sendOk(); });
  web.on("/api/media/clear", HTTP_POST, []() {
    mediaClear();
    if (curPage == P_MEDIA) enterPage(P_MEDIA);
    sendOk();
  });
  web.on("/api/scan", HTTP_GET, handleScan);
  web.on("/api/wifi", HTTP_POST, handleWifiSet);
  web.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  web.on("/api/ap/stop", HTTP_POST, []() { sendOk(); apStopAt = 1; });
  web.on("/api/reboot", HTTP_POST, []() { sendOk(); delay(300); ESP.restart(); });
  web.on("/live", HTTP_POST, handleLiveDone, handleLiveData);
  web.on("/sd/upload", HTTP_POST, handleSdUploadDone, handleSdUploadData);
  web.on("/api/sd/list", HTTP_GET, handleSdList);
  web.on("/api/sd/play", HTTP_POST, handleSdPlay);
  web.on("/api/sd/playall", HTTP_POST, handleSdPlayAll);
  web.on("/api/sd/delete", HTTP_POST, handleSdDelete);
  web.on("/api/sd/mount", HTTP_POST, handleSdMount);
  web.onNotFound([]() {
    if (apMode) { web.sendHeader("Location", "http://192.168.4.1/", true); web.send(302, "text/plain", ""); }
    else web.send(404, "text/plain", "not found");
  });
  web.begin();
}

// =====================================================================
//  BUTTON / SETUP / LOOP
// =====================================================================
static void handleButton() {
  static bool down = false;
  static uint32_t t0 = 0;
  bool pressed = digitalRead(BTN_PIN) == LOW;
  if (pressed && !down) { down = true; t0 = millis(); }
  else if (!pressed && down) {
    down = false;
    uint32_t d = millis() - t0;
    if (d > 4000) { startAP(); setupShown = false; }
    else if (d > 1000) { autoRotate = !autoRotate; cfg.autoRotate = autoRotate; saveCfg(); pageStart = millis(); renderData(); }
    else if (d > 30) enterPage(stepPage(1, false));
  }
}

void setup() {
#if ESP_ARDUINO_VERSION_MAJOR < 3
  disableCore0WDT();                      // heavy JPEG decoding on core 0 must never trigger the idle-task watchdog
#endif
  Serial.setRxBufferSize(16384);          // USB stream/upload needs a big RX buffer (must be before begin)
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);               // never block when nobody listens on the USB port
  pinMode(BTN_PIN, INPUT_PULLUP);
  mtx = xSemaphoreCreateMutex();
  sdMtx = xSemaphoreCreateMutex();
  pipeMtx = xSemaphoreCreateMutex();
  semFree = xSemaphoreCreateCounting(2, 2);
  semFull = xSemaphoreCreateCounting(2, 0);
  rbFree = xSemaphoreCreateCounting(RB_N, RB_N);
  rbFull = xSemaphoreCreateCounting(RB_N, 0);
  loadCfg();
  autoRotate = cfg.autoRotate;

  clockLoadPrefs();
  tft.init();
  blInit();
  blSet(cfg.bright);
  applyRot(cfg.rotation);
  tft.setSwapBytes(true);
  gfx.setPsram(true);
  gfx.setColorDepth(16);
  if (!gfx.createSprite(SCR_W, SCR_H)) { gfx.setPsram(false); gfx.createSprite(SCR_W / 2, SCR_H / 2); }
  gfx.setTextWrap(false);
  bootTest();
  gfx.fillScreen(TFT_BLACK);
  gfxPresent();

  TJpgDec.setCallback(canvasOut);
  gif.begin(LITTLE_ENDIAN_PIXELS);      // GIF-er rong ulta hole BIG_ENDIAN_PIXELS koro

  if (psramFound()) canvas = (uint16_t*)ps_malloc((size_t)SCR_W * SCR_H * 2);
  if (canvas) memset(canvas, 0, (size_t)SCR_W * SCR_H * 2);
  Serial.printf("PSRAM: %s, canvas: %s\n", psramFound() ? "yes" : "NO", canvas ? "ok" : "NO");
#ifndef DIAG_NO_SD
  sdBegin();
#endif

  memset(cpuHist, 0, sizeof(cpuHist));
  memset(tempHist, 0, sizeof(tempHist));
  memset(gpuTHist, 0, sizeof(gpuTHist));
  memset(dRHist, 0, sizeof(dRHist));
  memset(dWHist, 0, sizeof(dWHist));

#ifndef DIAG_NO_WIFI
  wifiConnect();
  setupWeb();
#ifdef WIFI_TX_LOW
  WiFi.setTxPower(WIFI_POWER_11dBm);   // less RF/current spikes near the display wires
#endif
#ifndef DIAG_NO_TASKS
  xTaskCreatePinnedToCore(pollTask, "poll", 10240, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(decTask, "dec", 12288, NULL, 2, NULL, 0);   // video/GIF decode (core 0)
  xTaskCreatePinnedToCore(rdTask, "sdrd", 6144, NULL, 2, NULL, 0);    // SD read-ahead (core 0)
  streamSrv.begin();
  xTaskCreatePinnedToCore(ioTask, "io", 8192, NULL, 2, NULL, 0);      // USB + WiFi stream/upload (core 0)
#endif
#endif
  enterPage(P_OVERVIEW);
}

void loop() {
#ifndef DIAG_NO_WIFI
  web.handleClient();
  if (apMode) dns.processNextRequest();
#endif
  handleButton();
#ifndef DIAG_NO_WIFI
  wifiTick();
#endif

  // WiFi setup mode: connect na thakle shudhu setup screen dekhao
  if (apMode && WiFi.status() != WL_CONNECTED) {
    if (!setupShown) { drawSetupScreen(); setupShown = true; }
    gfxPresent();
    delay(2);
    return;
  }
  if (setupShown) { setupShown = false; enterPage(curPage); }

  if (newData) {
    newData = false;
    takeSnapshot();
    renderData();
  }

  if (curPage == P_ACTIVITY) clockTick();
  mediaService();
  if (curPage == P_MEDIA) {
    if (liveType == LT_GIF) gifTick();
    else if (liveType == LT_CLIP || liveType == LT_STREAM) clipTick();
  }

  if (autoRotate && millis() - pageStart > (uint32_t)cfg.pageSecs[curPage] * 1000UL) {
    int np = stepPage(1, true);
    if (np != curPage) enterPage(np);
    else pageStart = millis();
  }
  gfxPresent();                       // one bulk SPI transfer per update (robust against bus noise)
  delay(1);
}
