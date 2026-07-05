/*
  ESP32 RFID Attendance Logger (Clean, publishable version)
  - Uses RC522 RFID reader, SH1106 display, and Google Apps Script webhook
  - Non-intrusive WiFi connection handling, debounced scans, NTP time

  Author: cleaned & refactored
*/

//https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "time.h"
#include <Preferences.h>
#include <map>

// ------------------------
// Display configuration
// ------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ------------------------
// RFID pins
// ------------------------
constexpr uint8_t SS_PIN = 21;
constexpr uint8_t RST_PIN = 22;
MFRC522 rfid(SS_PIN, RST_PIN);

// ------------------------
// WiFi / Cloud
// ------------------------
const char* WIFI_SSID = "YOUR_WIFI_NAMW";         // update before publishing
const char* WIFI_PASSWORD = "PASSWORD";   // do NOT publish secrets in public repos
String SHEET_URL = "https://script.google.com/macros/s/AKfycbxGz.../exec"; // replace with your URL

// ------------------------
 // Add your rfid card id here......
std::map<String, String> uidToName = {

  {"E43E4101", "Shijaz"}

};

// ------------------------
// Time (NTP)
// ------------------------
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 19800; // IST (+5:30)
const int DAYLIGHT_OFFSET_SEC = 0;

// ------------------------
// Preferences and state
// ------------------------
Preferences prefs;
std::map<String, bool> entryState; // true = last was Entry, false = last was Exit

// ------------------------
// UI & debounce timing
// ------------------------
static const unsigned long SCAN_DEBOUNCE_MS = 2500UL;
static const unsigned long COLON_BLINK_MS = 500UL;
static const unsigned long SCAN_ANIM_INTERVAL = 500UL;

unsigned long lastColonToggle = 0;
bool colonVisible = true;

String lastSeenUID = "";
unsigned long lastSeenMillis = 0;

int animation_state = 0;
unsigned long scanAnimLastMillis = 0;

bool prevWiFiConnected = false;
unsigned long lastWiFiRetry = 0;

// ------------------------
// Utility helpers
// ------------------------
void centerText(const String &text, int y, int size = 1) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(max(0, (SCREEN_WIDTH - (int)w) / 2), y);
  display.print(text);
}

String urlEncode(const String &str) {
  String encoded = "";
  const char *p = str.c_str();
  while (*p) {
    char c = *p++;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') encoded += c;
    else { char buf[4]; sprintf(buf, "%%%02X", (uint8_t)c); encoded += buf; }
  }
  return encoded;
}

bool getHourMinute(String &hh, String &mm) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  char buf[3]; sprintf(buf, "%02d", timeinfo.tm_hour); hh = String(buf);
  sprintf(buf, "%02d", timeinfo.tm_min); mm = String(buf);
  return true;
}

String getDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return String("--/--/--");
  char buffer[9]; strftime(buffer, sizeof(buffer), "%d/%m/%y", &timeinfo);
  return String(buffer);
}

// ------------------------
// Display helpers
// ------------------------
void drawWiFiIcon(bool connected) {
  int x = SCREEN_WIDTH - 18;
  display.fillRect(x, 0, 18, 12, SH110X_BLACK);
  display.setTextColor(SH110X_WHITE);
  display.drawPixel(x + 9, 6, SH110X_WHITE);
  display.drawCircleHelper(x + 9, 6, 2, 0x1, SH110X_WHITE);
  display.drawCircleHelper(x + 9, 6, 4, 0x1, SH110X_WHITE);
  display.drawCircleHelper(x + 9, 6, 6, 0x1, SH110X_WHITE);
  if (!connected) {
    display.drawLine(x + 5, 2, x + 13, 10, SH110X_WHITE);
  }
}

void drawTimeBlinking(int y, int textSize = 3) {
  if (millis() - lastColonToggle >= COLON_BLINK_MS) {
    colonVisible = !colonVisible;
    lastColonToggle = millis();
  }
  String hh, mm; if (!getHourMinute(hh, mm)) { hh = "00"; mm = "00"; }
  String timeStr = colonVisible ? hh + ":" + mm : hh + " " + mm;
  display.setTextSize(textSize);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int x = max(0, (SCREEN_WIDTH - (int)w) / 2);
  display.setCursor(x, y);
  display.print(timeStr);
}

// ------------------------
// RFID
// ------------------------
bool readRFID(String &uidOut) {
  if (!rfid.PICC_IsNewCardPresent()) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    char buf[3]; sprintf(buf, "%02X", rfid.uid.uidByte[i]); uid += buf;
  }
  uid.toUpperCase(); uidOut = uid;
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
  return true;
}

// ------------------------
// Networking & logging
// ------------------------
void setupTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

void connectToWiFi(unsigned long timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(200);
  }
}

void logToSheet(const String &name, const String &entry, const String &uid) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping log: WiFi not connected.");
    return;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get time");
    return;
  }

  char dateBuf[9], timeBuf[9];
  strftime(dateBuf, sizeof(dateBuf), "%d/%m/%y", &timeinfo);
  strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
  String dateStr = String(dateBuf);
  String timeStr = String(timeBuf);

  String url = SHEET_URL +
               "?name=" + urlEncode(name) +
               "&entry=" + urlEncode(entry) +
               "&rfid=" + urlEncode(uid) +
               "&date=" + urlEncode(dateStr) +
               "&time=" + urlEncode(timeStr);

  Serial.println("Logging URL: " + url);

  WiFiClientSecure client;
  client.setInsecure(); // only for quick testing; prefer proper certificates in production
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return;
  }

  int httpCode = http.GET();
  Serial.printf("HTTP Code: %d\n", httpCode);
  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("Response: " + payload);
  }
  http.end();
}

// ------------------------
// UI screens
// ------------------------
void showStartingScreenAnimated(const String &staticText, unsigned long animationTickMs = 500) {
  static unsigned long lastAnimUpdate = 0;
  static int connectAnimState = 0;
  if (millis() - lastAnimUpdate > animationTickMs) {
    connectAnimState = (connectAnimState + 1) % 4; lastAnimUpdate = millis();
  }
  display.clearDisplay(); display.setTextColor(SH110X_WHITE);
  display.setTextSize(2); centerText(staticText, 18);
  display.setTextSize(1);
  String connectLine = "Connecting";
  for (int i = 0; i < connectAnimState; ++i) connectLine += ".";
  centerText(connectLine, 42);
  display.display();
}

void showWifiConnectedMessage(unsigned long showMs = 1200) {
  display.clearDisplay(); display.setTextSize(2); display.setTextColor(SH110X_WHITE);
  centerText("WiFi Connected", 26); display.display(); delay(showMs);
}

void showWiFiConnectingScreen() {
  display.clearDisplay(); display.setTextColor(SH110X_WHITE);
  drawWiFiIcon(false); drawTimeBlinking(20, 3);
  display.setTextSize(1); centerText("WiFi Disconnected", 44, 1);
  String anim = "Connecting";
  for (int i = 0; i < animation_state; i++) anim += ".";
  centerText(anim, 54, 1);
  display.display();
  if (millis() - scanAnimLastMillis > 500) { animation_state = (animation_state + 1) % 4; scanAnimLastMillis = millis(); }
}

void showDefaultScreen() {
  display.clearDisplay(); bool connected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(connected); display.setTextSize(3); display.setTextColor(SH110X_WHITE);
  drawTimeBlinking(12, 3);

  const int barY = 48, barH = 16, leftBoxW = 62, rightBoxW = 66;
  display.fillRect(0, barY, SCREEN_WIDTH, barH, SH110X_BLACK);
  display.drawRect(0, barY, leftBoxW, barH, SH110X_WHITE);
  display.drawRect(leftBoxW, barY, rightBoxW, barH, SH110X_WHITE);

  String dateStr = getDateString(); display.setTextSize(1);
  int16_t x1, y1; uint16_t w, h; display.getTextBounds(dateStr.c_str(), 0, 0, &x1, &y1, &w, &h);
  int leftBoxCenterX = leftBoxW / 2; int dateX = leftBoxCenterX - (w / 2);
  int dateY = barY + ((barH - h) / 2); if (dateX < 1) dateX = 1;
  display.setCursor(dateX, dateY); display.print(dateStr);

  String scanText = (animation_state == 0) ? "SCAN" : (animation_state == 1) ? "SCAN." : (animation_state == 2) ? "SCAN.." : "SCAN...";
  display.getTextBounds(scanText.c_str(), 0, 0, &x1, &y1, &w, &h);
  int rightBoxX = leftBoxW; int rightBoxCenterX = rightBoxX + (rightBoxW / 2);
  int scanX = rightBoxCenterX - (w / 2); int scanY = barY + ((barH - h) / 2);
  if (scanX < rightBoxX + 1) scanX = rightBoxX + 1; display.setCursor(scanX, scanY);
  display.print(scanText);

  if (millis() - scanAnimLastMillis > SCAN_ANIM_INTERVAL) { animation_state = (animation_state + 1) % 4; scanAnimLastMillis = millis(); }
  display.display();
}

void showRFIDScreen(const String &status, const String &name) {
  display.clearDisplay(); const int bandH = 18; display.fillRect(0, 0, SCREEN_WIDTH, bandH, SH110X_WHITE);
  display.setTextSize(1); display.setTextColor(SH110X_BLACK);
  centerText(status, (bandH - 8) / 2, 1);
  display.setTextColor(SH110X_WHITE); display.setTextSize(2);
  centerText(name, bandH + 12, 2);
  display.display();
}

// ------------------------
// Arduino core
// ------------------------
void setup() {
  Serial.begin(115200);
  Wire.begin(4, 5);
  if (!display.begin(0x3C, true)) {
    Serial.println("Display init failed!");
  } else {
    display.clearDisplay(); display.setRotation(0);
  }

  SPI.begin(); rfid.PCD_Init();

  lastColonToggle = scanAnimLastMillis = millis();

  // Start WiFi (non-blocking initial animation)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 8000) {
    showStartingScreenAnimated("Starting...", 400);
    delay(40);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setupTime(); showWifiConnectedMessage(1200); prevWiFiConnected = true;
  }
}

void loop() {
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  // If disconnected, try reconnecting periodically and show connecting UI
  if (!wifiConnected) {
    if (millis() - lastWiFiRetry > 5000) { connectToWiFi(3000); lastWiFiRetry = millis(); }
    showWiFiConnectingScreen(); delay(120); return;
  }

  if (!prevWiFiConnected && wifiConnected) { setupTime(); showWifiConnectedMessage(1000); }
  prevWiFiConnected = wifiConnected;

  showDefaultScreen();

  String uid;
  if (readRFID(uid)) {
    // Print the raw RFID UID to the Serial monitor (requested change)
    Serial.println("RFID UID: " + uid);

    unsigned long now = millis();
    if (uid == lastSeenUID && (now - lastSeenMillis) < SCAN_DEBOUNCE_MS) {
      Serial.println("Duplicate scan ignored (within debounce period)");
    } else {
      lastSeenUID = uid; lastSeenMillis = now;
      String name = (uidToName.count(uid) ? uidToName[uid] : String("Unknown"));

      // toggle entry/exit state (first scan will be ENTRY)
      bool isEntry = !entryState[uid];
      String status = isEntry ? "ENTRY" : "EXIT";
      entryState[uid] = isEntry;

      showRFIDScreen(status, name);
      logToSheet(name, status, uid);
      delay(1200); // short pause so user can see the result on screen
    }
  }

  delay(80);
}
