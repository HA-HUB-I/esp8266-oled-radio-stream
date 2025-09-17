/*
  ESP8266 + SSD1306 128x64 (GPIO14/12, addr 0x3C)
  Boot -> Wi‑Fi -> NTP -> Ready + MP3 Stream test (no DAC/PCM5102 required)

  FIXED: Use AudioFileSourceBuffer with explicit static buffer (no begin()).
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ESP8266Audio
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputNull.h>            // no audio output (for testing)

// ===== USER SETTINGS =====
const char* WIFI_SSID = "ddwrt";
const char* WIFI_PASS = "";
const char* STATION_URL = "http://g5.turbohost.eu:8002/stream96";

// ===== Display config =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
#define OLED_ADDR    0x3C
#define I2C_SDA      14   // GPIO14 (D5)
#define I2C_SCL      12   // GPIO12 (D6)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ===== Timers =====
const uint32_t TIME_REFRESH_MS = 250;
const uint32_t WIFI_POLL_MS    = 1000;
const uint32_t IP_SHOW_MS      = 3000;
const uint32_t META_SHOW_MS    = 6000;

// ===== NTP servers =====
const char* NTP_1 = "pool.ntp.org";
const char* NTP_2 = "time.google.com";
const char* NTP_3 = "time.cloudflare.com";

// ===== State =====
enum UiState { BOOTING, WIFI_CONNECTING, TIME_SYNC, READY };
UiState uiState = BOOTING;

uint32_t lastTimeDraw = 0;
uint32_t lastWifiPoll = 0;
uint32_t connectStart = 0;
bool     haveIpSplash = false;
uint32_t ipSplashSince = 0;
String   ipStr;

String   icyName = "";
String   icyTitle = "";
uint32_t lastMetaTs = 0;
bool     showMeta = false;

const char spinnerFrames[] = {'|','/','-','\\'};
uint8_t spinnerIdx = 0;

String two(int v){ return (v<10) ? "0"+String(v) : String(v); }

// ===== Audio objects =====
AudioGeneratorMP3   *mp3 = nullptr;
AudioFileSourceICYStream *file = nullptr;
AudioFileSourceBuffer *buff = nullptr;
AudioOutputNull     *out = nullptr;

// Provide a static buffer for AudioFileSourceBuffer (8KB)
static uint8_t audioBuf[8192];

// Metadata callback
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  (void)cbData; (void)isUnicode;
  String t = String(type);
  String s = String(string);
  if (t.equalsIgnoreCase("StreamTitle")) {
    icyTitle = s;
    showMeta = true;
    lastMetaTs = millis();
  } else if (t.equalsIgnoreCase("StreamName")) {
    icyName = s;
    showMeta = true;
    lastMetaTs = millis();
  }
}

void drawWifiBars(int rssi, bool connected) {
  int bars = 0;
  if (connected) {
    if      (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -85) bars = 1;
    else                 bars = 0;
  }
  const int x = SCREEN_WIDTH - 18;
  const int y = 2;
  const int w = 3;
  const int s = 2;
  for (int i = 0; i < 4; i++) {
    int barH = (i+1) * 2;
    int bx = x + i*(w + s);
    int by = y + (8 - barH);
    if (i < bars) display.fillRect(bx, by, w, barH, SSD1306_WHITE);
    else          display.drawRect(bx, by, w, barH, SSD1306_WHITE);
  }
}

void drawBoot(const __FlashStringHelper* subtitle) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("ESP8266 Radio"));
  display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
  display.setCursor(0, 18);
  display.print(F("Booting "));
  display.write(spinnerFrames[spinnerIdx]);
  spinnerIdx = (spinnerIdx + 1) % 4;
  display.setCursor(0, 30);
  display.println(subtitle);
  display.display();
}

bool timeIsSet() {
  time_t now = time(nullptr);
  return now > 1700000000; // coarse check ~2023-11-14
}

void setupTimezone() {
  // Europe/Sofia: EET-2EEST,M3.5.0/3,M10.5.0/4
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();
}

void drawReady() {
  time_t now = time(nullptr);
  struct tm tm_info;
  localtime_r(&now, &tm_info);

  String hh = two(tm_info.tm_hour);
  String mm = two(tm_info.tm_min);
  String ss = two(tm_info.tm_sec);
  String DD = two(tm_info.tm_mday);
  String MM = two(tm_info.tm_mon + 1);
  int year  = tm_info.tm_year + 1900;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.print(F("Ready"));
  drawWifiBars(WiFi.isConnected() ? WiFi.RSSI() : -100, WiFi.isConnected());

  // Time
  display.setTextSize(2);
  display.setCursor(6,16);
  display.print(hh); display.print(":"); display.print(mm); display.print(":"); display.print(ss);

  // Date
  display.setTextSize(1);
  display.setCursor(8,42);
  display.print(DD); display.print("."); display.print(MM); display.print("."); display.print(year);

  // Stream line
  display.setCursor(0,54);
  if (mp3 && mp3->isRunning()) {
    if (showMeta && (millis() - lastMetaTs) < META_SHOW_MS) {
      String line = icyTitle.length() ? icyTitle : icyName;
      if (line.length() > 20) line = line.substring(0, 20);
      display.print(F("♪ "));
      display.print(line);
    } else {
      display.print(F("STREAM: playing"));
    }
  } else {
    display.print(F("STREAM: stopped"));
  }

  display.display();
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  connectStart = millis();
}

void startStream() {
  // Clean previous if any
  if (mp3) { mp3->stop(); delete mp3; mp3 = nullptr; }
  if (buff){ delete buff; buff = nullptr; }
  if (file){ delete file; file = nullptr; }
  if (out) { delete out;  out  = nullptr; }

  file = new AudioFileSourceICYStream(STATION_URL);
  file->RegisterMetadataCB(MDCallback, nullptr);

  // Use explicit static buffer, no begin() call needed
  buff = new AudioFileSourceBuffer(file, audioBuf, sizeof(audioBuf));

  out = new AudioOutputNull();   // no audio out (for now)

  mp3 = new AudioGeneratorMP3();
  mp3->begin(buff, out);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // OLED init
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 begin() failed."));
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
      digitalWrite(LED_BUILTIN, LOW); delay(150);
      digitalWrite(LED_BUILTIN, HIGH); delay(150);
    }
  }
  display.clearDisplay(); display.display();

  uiState = WIFI_CONNECTING;
  drawBoot(F("Wi-Fi connecting…"));
  connectWifi();
}

void loop() {
  if (uiState == WIFI_CONNECTING) {
    static uint32_t lastBootDraw = 0;
    if (millis() - lastBootDraw > 150) {
      lastBootDraw = millis();
      drawBoot(F("Wi-Fi connecting…"));
    }
    if (WiFi.status() == WL_CONNECTED) {
      ipStr = WiFi.localIP().toString();
      haveIpSplash = true;
      ipSplashSince = millis();
      uiState = TIME_SYNC;
      drawBoot(F("Sync time…"));
      setupTimezone();
      configTime(0, 0, NTP_1, NTP_2, NTP_3);
    } else if (millis() - connectStart > 20000) {
      WiFi.disconnect(true);
      delay(250);
      connectWifi();
    }
  }

  if (uiState == TIME_SYNC) {
    static uint32_t lastBootDraw = 0;
    if (millis() - lastBootDraw > 250) {
      lastBootDraw = millis();
      drawBoot(F("Sync time…"));
    }
    if (timeIsSet()) {
      uiState = READY;
      startStream();
    }
  }

  if (millis() - lastWifiPoll > WIFI_POLL_MS) {
    lastWifiPoll = millis();
    if (uiState == READY && WiFi.status() != WL_CONNECTED) {
      uiState = WIFI_CONNECTING;
      if (mp3) { mp3->stop(); }
      connectWifi();
    }
  }

  if (uiState == READY) {
    if (mp3) {
      if (mp3->isRunning()) {
        if (!mp3->loop()) {
          mp3->stop();
        }
      } else {
        static uint32_t lastTry = 0;
        if (millis() - lastTry > 3000) {
          lastTry = millis();
          startStream();
        }
      }
    }

    if (millis() - lastTimeDraw > TIME_REFRESH_MS) {
      lastTimeDraw = millis();
      drawReady();
    }
  }
}
