/*
 * ESP32 Temperature & Humidity Monitor  (with WiFi captive-portal setup)
 * ----------------------------------------------------------------------
 *  - Sensor : DHT11
 *  - Display: 1.3" OLED 128x64 (SH1106 controller, I2C)
 *  - Reads + refreshes the OLED every 10 seconds.
 *  - Uploads the AVERAGE and STANDARD DEVIATION of the samples in each
 *    window to a Google Sheet (Apps Script): the first reading is sent right
 *    after startup (once the NTP clock is set), then every 10 minutes.
 *  - OFFLINE BUFFERING: if the upload fails (no internet), records are queued
 *    in RAM (up to 24 h) and flushed automatically when WiFi returns. Each
 *    record carries an NTP (UTC) timestamp so buffered data lands in the
 *    sheet at its true measurement time, not the catch-up time.
 *  - POWER SAVING: CPU runs at 80 MHz, the WiFi radio is powered down between
 *    uploads, and the MCU light-sleeps between samples (the OLED keeps its
 *    image). The RTC clock keeps running, so timestamps stay valid with WiFi
 *    off. Lower power also means less self-heating -> better DHT accuracy.
 *
 *  CONFIGURATION IS DONE FROM YOUR PHONE — no hard-coded credentials.
 *  WiFi SSID / password / Google Script URL are stored in flash (NVS).
 *
 *  Setup / provisioning flow:
 *   1. On first boot (no saved config) the device starts its own WiFi
 *      hotspot  "LabMonitor-Setup"  (open network).
 *   2. Join it from your phone; a captive-portal page pops up
 *      (or browse to http://192.168.4.1 ).
 *   3. Enter SSID, password, and the Google Script URL -> Save & Reboot.
 *   4. The device reconnects to your WiFi and runs normally.
 *
 *  Offline mode:
 *   - The setup page has a "Run Offline (display only)" button. In offline
 *     mode the device never touches WiFi: it just shows temperature &
 *     humidity on the OLED.
 *
 *  OLED top-right status tag:
 *     OFF = offline mode          ... = starting (before first upload)
 *     NET = all data uploaded     BUF = data buffered, will retry
 *     CAP = captive portal detected -> a login page is blocking internet
 *           (e.g. behind a hotspot); log in / fix the uplink to clear it.
 *
 *  Reset / reconfigure:
 *   - AUTO-FALLBACK: if saved WiFi can't be joined at boot, it reopens the
 *     setup hotspot for 5 minutes, then reboots and retries (so a brief
 *     router outage self-heals instead of getting stuck in setup).
 *   - FACTORY RESET: let the board boot fully, THEN press and hold the BOOT
 *     button for ~3 s WHILE IT IS RUNNING -> stored config (incl. offline
 *     flag) is erased and it returns to setup mode. Works in any mode.
 *
 *   *** IMPORTANT — BOOT button (GPIO0) behaves differently by timing: ***
 *     - Held at POWER-UP / reset -> ESP32 enters the serial flash bootloader
 *       (download mode). The sketch does NOT run, so this does NOT reset
 *       config. This is the mode used for uploading firmware.
 *     - Held WHILE the sketch is RUNNING (~3 s) -> FACTORY RESET.
 *     If you ever get locked in (e.g. offline mode), erase NVS with:
 *       esptool --chip esp32 --port <PORT> erase-flash   then re-upload.
 *
 * Libraries (Library Manager):
 *  - "DHT sensor library" + "Adafruit Unified Sensor"  (Adafruit)
 *  - "U8g2" by oliver
 *  WebServer / DNSServer / Preferences ship with the ESP32 core.
 *
 *  NOTE ON FLASH: WiFi + web server + DHT + U8g2 is a large build. If it
 *  overflows, set  Tools -> Partition Scheme -> "Huge APP (3MB No OTA)".
 *
 *  BOARDS: builds for both classic ESP32 and ESP32-C3 (pins auto-select by
 *  chip). Just choose the right board in Tools -> Board.
 *  For ESP32-C3 also set  Tools -> "USB CDC On Boot: Enabled"  so the Serial
 *  Monitor works over the native USB port. (Max CPU on C3 is 160 MHz; the
 *  80 MHz power-saving setting used here is fine.)
 *
 * Wiring:
 *                     DHT11 DATA   OLED SDA   OLED SCL   BOOT btn
 *   Classic ESP32  ->   GPIO4       GPIO21     GPIO22     GPIO0
 *   ESP32-C3       ->   GPIO4       GPIO5      GPIO6      GPIO9
 *   (DHT11 needs a 10k pull-up DATA->3V3; OLED + DHT11 VCC->3V3, GND->GND)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <DHT.h>
#include <math.h>
#include <time.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <driver/gpio.h>         // gpio_wakeup_enable() for ESP32-C3 light sleep

// ---------------------- FIXED CONFIG ----------------------
// Pins — auto-selected per chip. Classic ESP32 and ESP32-C3 have different
// usable GPIOs and a different on-board BOOT button, so the sketch adapts.
#define DHTTYPE       DHT11      // DHT11 or DHT22
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3 DevKitM/DevKitC: BOOT button = GPIO9 (strapping pin, like the
  // classic ESP32's GPIO0). Avoid strapping pins 2/8/9 for I2C & the sensor.
  #define DHTPIN        4        // GPIO4
  #define I2C_SDA       5        // GPIO5
  #define I2C_SCL       6        // GPIO6
  #define BOOT_BTN_PIN  9        // on-board BOOT button = factory reset
#else
  // Classic ESP32 DevKit
  #define DHTPIN        4        // GPIO4
  #define I2C_SDA       21       // GPIO21
  #define I2C_SCL       22       // GPIO22
  #define BOOT_BTN_PIN  0        // on-board BOOT button = factory reset
#endif

// Captive-portal access point
const char* AP_SSID = "LabMonitor-Setup";   // open network (no password)

// NTP — used to timestamp records (UTC epoch). The Google Sheet shows them
// in the spreadsheet's own timezone, so no device timezone offset is needed.
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";

// Offline buffering: failed uploads are queued in RAM and flushed when WiFi
// returns. 96 slots x 15 min = up to 24 h of backlog. (RAM only — a power
// cycle clears the queue; see notes if you need flash/SD persistence.)
const int MAX_QUEUE = 96;

// Result of the connectivity probe (defined here so the auto-generated
// function prototypes can see it).
enum NetResult { NET_OK, NET_CAPTIVE, NET_DOWN };

// Timing
const unsigned long SAMPLE_INTERVAL_MS = 10000UL;     // read every 10 s
const unsigned long UPLOAD_INTERVAL_MS = 600000UL;    // upload every 10 min
const unsigned long SHIFT_INTERVAL_MS  = 180000UL;    // burn-in shift / 3 min

// Power saving: 1 = light-sleep between samples (WiFi off in the gaps).
// Set to 0 if you ever suspect the display/button/timing misbehaving.
#define ENABLE_LIGHT_SLEEP 1
const uint32_t CPU_FREQ_MHZ = 80;     // 80 MHz: enough for this + WiFi floor

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;   // boot connect wait
const unsigned long PORTAL_FALLBACK_TIMEOUT_MS = 300000UL;// 5 min then reboot
const unsigned long FACTORY_RESET_HOLD_MS = 3000UL;      // hold BOOT this long
const unsigned long NTP_FIRST_WAIT_MS = 30000UL;         // wait for clock sync
                                                         // before 1st upload
// ----------------------------------------------------------

// Runtime configuration (loaded from / saved to NVS)
String wifiSsid;
String wifiPass;
String gscriptUrl;
bool   offlineMode = false;    // true = display only, no WiFi / no uploads

Preferences prefs;
const char* NVS_NS = "monitor";

WebServer  server(80);
DNSServer  dnsServer;
const byte DNS_PORT = 53;

DHT dht(DHTPIN, DHTTYPE);

// SH1106 128x64 over hardware I2C. If your panel is actually SSD1306,
// swap the line below for: U8G2_SSD1306_128X64_NONAME_F_HW_I2C
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Running accumulators for the current hour (sum / sumSq method)
struct Accum {
  uint32_t n      = 0;
  double   sum    = 0.0;
  double   sumSq  = 0.0;

  void add(double x) { n++; sum += x; sumSq += x * x; }
  void reset()       { n = 0; sum = 0.0; sumSq = 0.0; }
  double mean() const { return (n > 0) ? (sum / n) : NAN; }
  double stddev() const {                 // sample SD (n-1); 0 if <2 samples
    if (n < 2) return 0.0;
    double m = mean();
    double var = (sumSq - n * m * m) / (n - 1);
    if (var < 0) var = 0;
    return sqrt(var);
  }
};

Accum tempAcc;
Accum humAcc;

// ----- pending-upload queue (RAM ring buffer) -----
struct Record {
  time_t   ts;                  // UTC epoch seconds (0 = unknown / no NTP yet)
  float    tMean, tStd, hMean, hStd;
  uint32_t n;
};
Record   queueBuf[MAX_QUEUE];
int      qHead = 0;             // index of oldest queued record
int      qCount = 0;           // number of records currently queued

void queuePush(const Record& r) {
  if (qCount < MAX_QUEUE) {
    queueBuf[(qHead + qCount) % MAX_QUEUE] = r;
    qCount++;
  } else {
    // Full: drop the oldest to make room (keep the most recent 24 h).
    queueBuf[qHead] = r;
    qHead = (qHead + 1) % MAX_QUEUE;
  }
}

// ----- mode / state -----
bool portalMode = false;             // true while running the setup portal
bool shouldReboot = false;           // deferred reboot after saving config
bool firstUploadDone = false;        // send one record right after startup
bool ntpStarted = false;             // configTime() called once
bool captivePortal = false;          // last upload found a login page in the way
unsigned long rebootAtMs = 0;
unsigned long portalStartMs = 0;
unsigned long portalTimeoutMs = 0;   // 0 = no timeout (wait forever for setup)

unsigned long lastSampleMs = 0;
unsigned long lastUploadMs = 0;
unsigned long lastShiftMs  = 0;

// Burn-in shift offsets (dx, dy). Small so nothing clips off the panel.
const int8_t SHIFT_OFFSETS[][2] = {
  {0, 0}, {4, 0}, {6, 3}, {2, 4}, {0, 2}, {4, 1}
};
const uint8_t NUM_SHIFTS = sizeof(SHIFT_OFFSETS) / sizeof(SHIFT_OFFSETS[0]);
uint8_t shiftIndex = 0;

// Last good reading, kept for the display
float lastTemp = NAN;
float lastHum  = NAN;

// ======================================================================
//  NVS (flash) configuration storage
// ======================================================================
void loadConfig() {
  prefs.begin(NVS_NS, true);                  // read-only
  wifiSsid    = prefs.getString("ssid", "");
  wifiPass    = prefs.getString("pass", "");
  gscriptUrl  = prefs.getString("url",  "");
  offlineMode = prefs.getBool("offline", false);
  prefs.end();
}

void saveConfig() {
  prefs.begin(NVS_NS, false);                 // read-write
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("url",  gscriptUrl);
  prefs.putBool("offline", offlineMode);
  prefs.end();
}

void clearConfig() {
  prefs.begin(NVS_NS, false);
  prefs.clear();                              // only THIS namespace
  prefs.end();
  wifiSsid = ""; wifiPass = ""; gscriptUrl = ""; offlineMode = false;
}

// ======================================================================
//  OLED helpers
// ======================================================================
void drawStatusLines(const char* l1, const char* l2 = nullptr,
                     const char* l3 = nullptr, const char* l4 = nullptr) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  if (l1) u8g2.drawStr(0, 14, l1);
  if (l2) u8g2.drawStr(0, 30, l2);
  if (l3) u8g2.drawStr(0, 46, l3);
  if (l4) u8g2.drawStr(0, 62, l4);
  u8g2.sendBuffer();
}

void drawSetupScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, "SETUP MODE");
  u8g2.drawHLine(0, 15, 120);
  u8g2.drawStr(0, 30, "Join WiFi:");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 44, AP_SSID);
  u8g2.drawStr(0, 62, "Open 192.168.4.1");
  u8g2.sendBuffer();
}

void drawDisplay(float t, float h) {
  int ox = SHIFT_OFFSETS[shiftIndex][0];
  int oy = SHIFT_OFFSETS[shiftIndex][1];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(ox + 0, oy + 10, "Temp / Humid");
  u8g2.drawHLine(ox + 0, oy + 13, 120);

  // Mode indicator, top-right corner:
  //   OFF = offline mode      | ... = starting (pre first upload)
  //   CAP = captive portal (login needed) | BUF = upload pending
  //   NET = all uploaded
  u8g2.setFont(u8g2_font_5x7_tr);
  const char* tag = offlineMode     ? "OFF" :
                    !firstUploadDone ? "..." :
                    captivePortal    ? "CAP" :
                    (qCount > 0)     ? "BUF" : "NET";
  int tagW = u8g2.getStrWidth(tag);
  u8g2.drawStr(ox + 120 - tagW, oy + 8, tag);

  char buf[24];
  u8g2.setFont(u8g2_font_logisoso16_tr);
  if (isnan(t)) {
    u8g2.drawStr(ox + 0, oy + 36, "T: --.- C");
  } else {
    snprintf(buf, sizeof(buf), "T:%4.1f C", t);
    u8g2.drawStr(ox + 0, oy + 36, buf);
  }
  if (isnan(h)) {
    u8g2.drawStr(ox + 0, oy + 58, "H: --.- %");
  } else {
    snprintf(buf, sizeof(buf), "H:%4.1f %%", h);
    u8g2.drawStr(ox + 0, oy + 58, buf);
  }
  u8g2.sendBuffer();
}

// ======================================================================
//  Factory reset — hold BOOT while the device is RUNNING (not at power-up!)
//
//  GPIO0/BOOT is the bootloader strapping pin: holding it during power-up
//  drops the ESP32 into serial-download mode, so the sketch never runs.
//  Therefore the reset is detected here in the main loop instead — let the
//  board boot fully, THEN press and hold BOOT for ~3 s.
// ======================================================================
bool btnWasDown = false;
unsigned long btnDownMs = 0;

void checkResetButtonHold() {
  bool down = (digitalRead(BOOT_BTN_PIN) == LOW);

  if (down) {
    if (!btnWasDown) {              // just pressed
      btnWasDown = true;
      btnDownMs = millis();
    }
    unsigned long held = millis() - btnDownMs;

    int remain = (int)((FACTORY_RESET_HOLD_MS - held + 999) / 1000);
    if (remain < 0) remain = 0;
    char line[24];
    snprintf(line, sizeof(line), "Reset in %d ...", remain);
    drawStatusLines("FACTORY RESET", "Keep holding BOOT", line);

    if (held >= FACTORY_RESET_HOLD_MS) {
      clearConfig();
      Serial.println("Configuration erased — rebooting to setup.");
      drawStatusLines("FACTORY RESET", "Config cleared", "Rebooting...");
      delay(900);
      ESP.restart();
    }
  } else {
    if (btnWasDown) {              // released before threshold -> abort
      btnWasDown = false;
      Serial.println("Reset aborted (released early).");
      drawDisplay(lastTemp, lastHum);   // restore normal screen
    }
  }
}

// ======================================================================
//  WiFi station connect
// ======================================================================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wifiSsid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.print("Connecting to '"); Serial.print(wifiSsid); Serial.print("' ");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    if (!ntpStarted) {
      configTime(0, 0, NTP_SERVER1, NTP_SERVER2);   // UTC; sync in background
      ntpStarted = true;
    }
    return true;
  }
  Serial.println("WiFi connect FAILED.");
  return false;
}

// Current UTC epoch, or 0 if the clock hasn't synced via NTP yet.
time_t nowEpochOrZero() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? t : 0;     // 1700000000 = Nov 2023 sanity check
}

// Power down the WiFi radio between uploads (biggest single power saving).
// The RTC clock keeps running, so timestamps stay valid while WiFi is off.
void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ======================================================================
//  Captive portal
// ======================================================================
String htmlForm(const char* note = nullptr) {
  String s;
  s.reserve(1500);
  s += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Lab Monitor Setup</title><style>"
         "body{font-family:sans-serif;margin:20px;max-width:440px}"
         "h2{margin-top:0}label{display:block;margin-top:14px;font-weight:bold}"
         "input{width:100%;padding:9px;box-sizing:border-box;font-size:16px}"
         "button{margin-top:20px;padding:11px 18px;font-size:16px;width:100%}"
         ".n{background:#ffe9b3;padding:8px;border-radius:6px}"
         "</style></head><body><h2>Lab Monitor Setup</h2>");
  if (note) { s += F("<p class='n'>"); s += note; s += F("</p>"); }
  s += F("<form method='POST' action='/save'>"
         "<label>WiFi SSID</label><input name='ssid' value='");
  s += wifiSsid;
  s += F("'><label>WiFi Password</label>"
         "<input name='pass' type='password' placeholder='(leave blank to keep)'>"
         "<label>Google Script URL</label><input name='url' value='");
  s += gscriptUrl;
  s += F("'><button type='submit'>Save &amp; Reboot</button></form>"
         "<form method='POST' action='/offline'>"
         "<button type='submit' style='background:#e0e0e0'>"
         "Run Offline (display only)</button></form>"
         "<p style='color:#666;font-size:13px'>Leave the password blank to keep "
         "the current one (or for an open network on first setup). Offline mode "
         "skips WiFi and Google Sheets &mdash; the OLED still shows temperature "
         "&amp; humidity. Hold BOOT ~3s while running to factory reset.</p>"
         "</body></html>");
  return s;
}

void handleRoot() {
  server.send(200, "text/html", htmlForm());
}

void handleSave() {
  String ns = server.arg("ssid");
  String np = server.arg("pass");
  String nu = server.arg("url");

  if (ns.length() == 0) {
    server.send(200, "text/html", htmlForm("SSID cannot be empty."));
    return;
  }
  wifiSsid = ns;
  if (np.length() > 0) wifiPass = np;     // blank = keep existing password
  gscriptUrl = nu;
  offlineMode = false;                    // providing WiFi exits offline mode
  saveConfig();
  Serial.println("Config saved via portal. Rebooting...");

  server.send(200, "text/html",
              F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "</head><body style='font-family:sans-serif;margin:20px'>"
                "<h2>Saved.</h2><p>The device is rebooting and will connect "
                "to your WiFi. You can close this page.</p></body></html>"));

  drawStatusLines("Saved!", "Rebooting...");
  shouldReboot = true;
  rebootAtMs = millis() + 1500;
}

void handleOffline() {
  offlineMode = true;
  saveConfig();
  Serial.println("Offline mode selected. Rebooting...");

  server.send(200, "text/html",
              F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "</head><body style='font-family:sans-serif;margin:20px'>"
                "<h2>Offline mode.</h2><p>The device is rebooting and will show "
                "temperature &amp; humidity on the display only (no WiFi, no "
                "uploads). Hold BOOT at power-up to return to setup.</p>"
                "</body></html>"));

  drawStatusLines("Offline mode", "Rebooting...");
  shouldReboot = true;
  rebootAtMs = millis() + 1500;
}

void startPortal(bool withTimeout) {
  Serial.println("Starting captive portal...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);                    // open AP
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(apIP);

  dnsServer.start(DNS_PORT, "*", apIP);    // redirect every host -> portal

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/offline", HTTP_POST, handleOffline);
  server.onNotFound(handleRoot);           // captive-portal catch-all
  server.begin();

  portalMode = true;
  portalStartMs = millis();
  portalTimeoutMs = withTimeout ? PORTAL_FALLBACK_TIMEOUT_MS : 0;
  drawSetupScreen();
}

void portalLoop() {
  checkResetButtonHold();        // long-press BOOT also works in setup mode
  dnsServer.processNextRequest();
  server.handleClient();

  if (shouldReboot && (long)(millis() - rebootAtMs) >= 0) {
    delay(100);
    ESP.restart();
  }
  // Auto-fallback portal: reboot after timeout so a transient WiFi
  // outage self-heals instead of staying stuck in setup mode.
  if (portalTimeoutMs > 0 &&
      millis() - portalStartMs >= portalTimeoutMs) {
    Serial.println("Portal timeout — rebooting to retry WiFi.");
    ESP.restart();
  }
}

// ======================================================================
//  Upload to Google Sheet (with offline buffering)
// ======================================================================

// Probe real internet access vs. a captive portal. Requests a well-known
// "generate_204" endpoint over plain HTTP without following redirects:
//   204            -> genuine internet
//   2xx/3xx + body -> a captive portal is intercepting (login required)
//   <=0            -> network down / no response
NetResult checkConnectivity() {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.begin(client, "http://connectivitycheck.gstatic.com/generate_204");
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // we want to SEE redirects
  int code = http.GET();
  http.end();
  Serial.printf("Connectivity check: HTTP %d\n", code);
  if (code == 204) return NET_OK;
  if (code <= 0)   return NET_DOWN;
  return NET_CAPTIVE;
}

// Send a single record. Returns true only on an accepted HTTP response.
bool sendRecord(const Record& r) {
  char url[460];
  snprintf(url, sizeof(url),
           "%s?temp=%.2f&temp_sd=%.2f&hum=%.2f&hum_sd=%.2f&n=%lu&ts=%ld",
           gscriptUrl.c_str(), r.tMean, r.tStd, r.hMean, r.hStd,
           (unsigned long)r.n, (long)r.ts);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  Serial.print("Uploading: "); Serial.println(url);
  int code = http.GET();
  Serial.printf("HTTP response: %d\n", code);
  if (code > 0) Serial.println(http.getString());
  http.end();

  return (code >= 200 && code < 400);   // 2xx/3xx = accepted
}

// Try to send every queued record, oldest first. Stops on the first failure
// so ordering is preserved and the rest stay buffered for next time.
void flushQueue() {
  while (qCount > 0) {
    if (sendRecord(queueBuf[qHead])) {
      qHead = (qHead + 1) % MAX_QUEUE;   // pop on success
      qCount--;
    } else {
      Serial.printf("Send failed; %d record(s) still buffered.\n", qCount);
      break;
    }
  }
}

// Close the current averaging window: enqueue it, then flush the backlog.
void uploadCycle() {
  // Enqueue the just-completed window (if it has any samples).
  if (tempAcc.n > 0) {
    Record r;
    r.ts    = nowEpochOrZero();
    r.tMean = (float)tempAcc.mean();
    r.tStd  = (float)tempAcc.stddev();
    r.hMean = (float)humAcc.mean();
    r.hStd  = (float)humAcc.stddev();
    r.n     = tempAcc.n;
    queuePush(r);
    tempAcc.reset();                     // start a fresh window immediately
    humAcc.reset();
  }

  if (qCount == 0) return;               // nothing to send
  if (gscriptUrl.length() == 0) {
    Serial.println("No Google Script URL set — keeping records buffered.");
    return;
  }
  if (!connectWiFi()) {
    Serial.printf("No WiFi — %d record(s) buffered, will retry.\n", qCount);
    captivePortal = false;               // it's a WiFi problem, not a portal
    wifiOff();                           // radio off until next attempt
    return;
  }

  NetResult net = checkConnectivity();
  captivePortal = (net == NET_CAPTIVE);
  if (net == NET_OK) {
    flushQueue();
  } else if (net == NET_CAPTIVE) {
    Serial.printf("Captive portal in the way — log in; %d record(s) held.\n",
                  qCount);
  } else {
    Serial.printf("No internet — %d record(s) buffered, will retry.\n", qCount);
  }
  wifiOff();                             // done — power the radio back down
}

// ======================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  setCpuFrequencyMhz(CPU_FREQ_MHZ);   // lower clock = less power & self-heating

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();
  u8g2.setContrast(80);          // dim panel to slow OLED burn-in
  dht.begin();

  loadConfig();

  // Decide mode:
  if (offlineMode) {
    // Display-only: skip WiFi entirely and keep the radio powered down.
    Serial.println("Offline mode — display only, no WiFi.");
    wifiOff();
  } else if (wifiSsid.length() == 0) {
    // No credentials yet -> wait in setup mode indefinitely.
    startPortal(false);
    return;
  } else {
    drawStatusLines("Starting...", "Connecting WiFi");
    if (!connectWiFi()) {
      // Saved WiFi unreachable -> setup portal with auto-reboot timeout.
      startPortal(true);
      return;
    }
  }

  // Normal monitoring mode (online or offline)
  unsigned long now = millis();
  lastSampleMs = now - SAMPLE_INTERVAL_MS;   // force an immediate reading
  lastUploadMs = now;
  lastShiftMs  = now;
}

// ======================================================================
//  Light sleep until the next scheduled event (timer), or until the BOOT
//  button is pressed (GPIO0 low). The OLED retains its image during sleep.
// ======================================================================
void powerNap(unsigned long ms) {
#if ENABLE_LIGHT_SLEEP
  if (ms < 30) return;                            // too short to bother
  if (digitalRead(BOOT_BTN_PIN) == LOW) return;   // button held -> stay awake
  if (WiFi.getMode() != WIFI_OFF) {               // WiFi up (brief): don't deep-
    delay(ms < 50 ? ms : 50);                     // sleep, keep association
    return;
  }
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // C3 (RISC-V) has no ext0/RTC-IO wakeup: use light-sleep GPIO wakeup.
  gpio_wakeup_enable((gpio_num_t)BOOT_BTN_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
#else
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BTN_PIN, 0);  // wake on press
#endif
  esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  gpio_wakeup_disable((gpio_num_t)BOOT_BTN_PIN);
#endif
#else
  delay(ms < 50 ? ms : 50);
#endif
}

// Milliseconds until the soonest upcoming scheduled event.
unsigned long msUntilNextEvent() {
  unsigned long now = millis();
  auto remain = [&](unsigned long last, unsigned long interval) -> unsigned long {
    unsigned long elapsed = now - last;
    return (elapsed >= interval) ? 0 : (interval - elapsed);
  };
  unsigned long nap = remain(lastSampleMs, SAMPLE_INTERVAL_MS);
  unsigned long s   = remain(lastShiftMs,  SHIFT_INTERVAL_MS);
  if (s < nap) nap = s;
  if (!offlineMode && firstUploadDone) {
    unsigned long u = remain(lastUploadMs, UPLOAD_INTERVAL_MS);
    if (u < nap) nap = u;
  }
  // While waiting for the first NTP sync, poll often so we don't delay it.
  if (!offlineMode && !firstUploadDone && nap > 1000) nap = 1000;
  return nap;
}

// ======================================================================
void loop() {
  if (portalMode) {
    portalLoop();
    return;
  }

  checkResetButtonHold();        // hold BOOT ~3 s during operation to reset

  unsigned long now = millis();

  // --- sampling + display ---
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (isnan(t) || isnan(h)) {
      Serial.println("DHT read failed.");
    } else {
      lastTemp = t;
      lastHum  = h;
      tempAcc.add(t);
      humAcc.add(h);
      Serial.printf("T=%.1f C  H=%.1f %%  (n=%lu)\n",
                    t, h, (unsigned long)tempAcc.n);
    }
    drawDisplay(lastTemp, lastHum);
  }

  // --- first upload shortly after startup, then every interval ---
  // Wait for the NTP clock to sync so the first record has a real timestamp;
  // if it hasn't synced within NTP_FIRST_WAIT_MS, send anyway (ts=0 -> the
  // Apps Script falls back to server time) rather than losing the reading.
  if (!offlineMode && !firstUploadDone && tempAcc.n >= 1 &&
      (nowEpochOrZero() != 0 || now >= NTP_FIRST_WAIT_MS)) {
    firstUploadDone = true;
    lastUploadMs = now;
    uploadCycle();             // send the first reading once the clock is set
  }

  // --- burn-in shift ---
  if (now - lastShiftMs >= SHIFT_INTERVAL_MS) {
    lastShiftMs = now;
    shiftIndex = (shiftIndex + 1) % NUM_SHIFTS;
    drawDisplay(lastTemp, lastHum);
  }

  // --- periodic upload (every 10 min; skipped in offline mode) ---
  if (!offlineMode && now - lastUploadMs >= UPLOAD_INTERVAL_MS) {
    lastUploadMs = now;
    uploadCycle();
  }

  // --- sleep until the next event (or a BOOT-button press) ---
  powerNap(msUntilNextEvent());
}
