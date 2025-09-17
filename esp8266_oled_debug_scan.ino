/*
  ESP8266 OLED Debug Scanner
  - Prints logs to Serial (115200)
  - Scans common I2C pin pairs to find SSD1306
  - Detects address (0x3C or 0x3D)
  - If found, initializes OLED and shows a test screen
  - Then tries Wi-Fi connect (optional) and prints status

  Select Board: NodeMCU 1.0 (ESP-12E Module)
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>

// ===== USER: Wi-Fi (optional for quick test) =====
const char* WIFI_SSID = "ddwrt";
const char* WIFI_PASS = "";

// Candidate I2C pin pairs (SDA,SCL)
struct PinPair { uint8_t sda; uint8_t scl; const char* name; };
PinPair candidates[] = {
  {4, 5,  "SDA=GPIO4(D2), SCL=GPIO5(D1)"},      // Most common
  {0, 2,  "SDA=GPIO0(D3), SCL=GPIO2(D4)"},      // Some boards
  {2, 14, "SDA=GPIO2(D4), SCL=GPIO14(D5)"},     // Rare
  {14, 12,"SDA=GPIO14(D5), SCL=GPIO12(D6)"},    // Rare
  {5, 4,  "SDA=GPIO5(D1), SCL=GPIO4(D2)"}       // Fallback try
};

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306* oled = nullptr;
uint8_t foundSDA = 0xFF, foundSCL = 0xFF, foundAddr = 0xFF;

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

bool scanOnPins(uint8_t sda, uint8_t scl) {
  Wire.begin(sda, scl);
  delay(20);
  Serial.printf("Scanning on %s...\n", (String("SDA=")+sda+", SCL="+scl).c_str());

  bool hit = false;
  for (uint8_t addr = 0x08; addr <= 0x7F; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  I2C device found at 0x%02X\n", addr);
      if (addr == 0x3C || addr == 0x3D) {
        foundSDA = sda;
        foundSCL = scl;
        foundAddr = addr;
        hit = true;
        break;
      }
    }
    delay(2);
  }
  return hit;
}

void tryFindOled() {
  Serial.println("\n=== I2C OLED auto-detect ===");
  for (auto &p : candidates) {
    Serial.printf("Trying pins: %s\n", p.name);
    if (scanOnPins(p.sda, p.scl)) {
      Serial.printf("SSD1306 candidate found at addr 0x%02X on %s\n",
        foundAddr, p.name);
      return;
    }
  }
}

void drawTestScreen() {
  oled->clearDisplay();
  oled->setTextSize(1);
  oled->setTextColor(SSD1306_WHITE);
  oled->setCursor(0,0);
  oled->println(F("ESP8266 OLED Test"));
  oled->drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
  oled->setCursor(0, 16);
  oled->println(F("HELLO OLED :)"));
  oled->setCursor(0, 28);
  oled->print(F("I2C: SDA=")); oled->print(foundSDA);
  oled->print(F(" SCL=")); oled->println(foundSCL);
  oled->setCursor(0, 40);
  oled->print(F("Addr: 0x")); oled->println(foundAddr, HEX);

  // Wi-Fi quick status
  oled->setCursor(0, 52);
  if (WiFi.isConnected()) {
    oled->print(F("WiFi OK: "));
    oled->print(WiFi.localIP());
  } else {
    oled->print(F("WiFi: not connected"));
  }
  oled->display();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\nESP8266 OLED Debug Scanner starting...");
  pinMode(LED_BUILTIN, OUTPUT);

  tryFindOled();

  if (foundAddr == 0xFF) {
    Serial.println("ERROR: SSD1306 not found on common pins/addresses.");
    Serial.println("Tips:");
    Serial.println(" - Check 3.3V and GND");
    Serial.println(" - Verify I2C pull-ups (usually onboard)");
    Serial.println(" - Some modules use 0x3D, others 0x3C");
    Serial.println(" - Try re-soldering or reseating jumper wires");
    // Blink LED fast to signal failure
    while (true) {
      digitalWrite(LED_BUILTIN, LOW); delay(120);
      digitalWrite(LED_BUILTIN, HIGH); delay(120);
    }
  }

  // Init OLED on found pins/address
  Wire.begin(14, 12);  // SDA=GPIO14, SCL=GPIO12
  oled = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  if (!oled->begin(SSD1306_SWITCHCAPVCC, foundAddr)) {
    Serial.println("ERROR: SSD1306 begin() failed even though address replied.");
    while (true) {
      digitalWrite(LED_BUILTIN, LOW); delay(500);
      digitalWrite(LED_BUILTIN, HIGH); delay(500);
    }
  }
  Serial.printf("SSD1306 OK at 0x%02X using SDA=%u SCL=%u\n", foundAddr, foundSDA, foundSCL);

  // Optional: Wi-Fi connect (non-blocking-ish)
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi not connected (timeout).");
  }

  drawTestScreen();
  Serial.println("OLED test screen drawn.");
}

void loop() {
  static uint32_t last = 0;
  static bool ledState = false;

  if (millis() - last > 1000) {
    last = millis();
    ledState = !ledState;                 // обръщаме състоянието
    digitalWrite(LED_BUILTIN, ledState);  // записваме го
  }
}
