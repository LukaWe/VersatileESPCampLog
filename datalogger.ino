// Wemos D1 mini + SHT3x + DS3231 + SSD1306 + SD card + DS18B20
// Power saving: Display off, timed light sleep, button wake (20s display on)
// Auto mode: No SD=1min monitoring, With SD 4min logging
// First boot: WiFi NTP sync (fixed with magic byte + commit)
// RTC policy: DS3231 stores UTC; display shows local time (CET/CEST)
// CSV format: wide format with UTC ISO8601 + epoch_ms
// Optimized SHT3x retry logic + error tracking in CSV
// Log counter: Displays number of data entries in CSV
// Button: Short press = display + fresh measure (no CSV); Long press (15s) =
// rotate CSV Display: 10 updates current data, then 10 updates last 3
// measurements from CSV v17: FIXED all identified bugs - underflow protection,
// iterative retries, strncpy safety

#include <Adafruit_GFX.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_SSD1306.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <OneWire.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <time.h>

extern "C" {
#include "user_interface.h"
}

// WiFi Credentials
#define WIFI_SSID "hp.w"
#define WIFI_PASS "moep1337rofl"

// Pin Definitions
#define PIN_SD_CS D8
#define PIN_BUTTON D3
#define PIN_DS18B20 D4
#define PIN_WAKE_RX 3

// Display Configuration
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// Sensor Objects
Adafruit_SHT31 sht31;
RTC_DS3231 rtc;
OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

// System State Variables
bool sdAvailable = false;
bool shtOK = false;
bool rtcOK = false;
bool ds18b20OK = false;
bool displayOn = false;
bool isMeasuring = false;
bool firstBootDone = false;

// Display Timing
uint32_t displayUpdateCount = 0;
const uint32_t DISPLAY_UPDATE_INTERVAL = 1000UL;
uint32_t lastDisplayUpdate = 0;

// Sensor Retry Optimization
uint32_t sht_retry_counter = 0;
const uint32_t SHT_RETRY_INTERVAL = 10;

// Log Counter
uint32_t logCount = 0;
uint32_t lastLogCountUpdate = 0;
const uint32_t LOG_COUNT_UPDATE_INTERVAL = 30000UL;

// Button Configuration
const uint32_t LONG_PRESS_MS = 15000UL;

// Measurement Intervals
uint32_t lastMeasureUnix = 0;
const uint32_t INTERVAL_NO_SD = 60;
const uint32_t INTERVAL_SD = 240;
uint32_t interval = INTERVAL_NO_SD;

// EEPROM Configuration
#define EEPROM_SIZE 512
#define EEPROM_FIRST_BOOT_ADDR 0
#define EEPROM_MAGIC 0xA5

// CSV Header
const char *CSV_HEADER = "ts_utc,epoch_ms,timestamp_local,temperature_C,"
                         "humidity_pct,dewpoint_C,water_temp_C,sht_ok,water_ok";

// DS18B20 Error Codes
const float DS18B20_ERROR_DISCONNECTED = -127.0f;
const float DS18B20_ERROR_POWER_ON_RESET = 85.0f;

// Unix timestamp constants
const time_t UNIX_EPOCH_2021 = 1609459200UL; // 2021-01-01 00:00:00 UTC

// Historical Data Structure
struct HistoricalData {
  float t[3];
  float h[3];
  float tWater[3];
  bool valid;
} historicalData;

// Current Measurement Data
struct MeasurementData {
  DateTime ts;
  float t, h, td;
  float tWater;
  bool shtValid;
  bool waterTempValid;
} data;

// DS18B20 Async State
static bool dsConvPending = false;
static uint32_t dsConvReadyAt = 0;

// SD Card Recovery
uint8_t sdFailCount = 0;
const uint8_t SD_MAX_RETRIES = 5;
uint32_t lastSDRetry = 0;
const uint32_t SD_RETRY_INTERVAL = 60000UL;
bool isBootPhase = true;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

float dewpoint(float t, float h) {
  const float b = 17.62f, c = 243.12f;
  float g = log(h / 100.0f) + (b * t) / (c + t);
  return (c * g) / (b - g);
}

static bool isRtcPlausible() {
  if (!rtcOK)
    return false;
  DateTime now = rtc.now();
  int y = now.year();
  return (y >= 2023 && y <= 2099);
}

static DateTime unixToLocal(time_t t) {
  struct tm *ti = localtime(&t);
  return DateTime(1900 + ti->tm_year, 1 + ti->tm_mon, ti->tm_mday, ti->tm_hour,
                  ti->tm_min, ti->tm_sec);
}

static DateTime nowLocalFromRTC() {
  DateTime utc = rtc.now();
  time_t u = utc.unixtime();
  return unixToLocal(u);
}

// ============================================================================
// SD CARD FUNCTIONS
// ============================================================================

// Force SD card hardware shutdown - always runs regardless of sdAvailable state
void forceEndSD() {
  Serial.println("[SD] Forcing SD card shutdown...");

  SD.end();
  delay(50);

  // Ensure CS is high after shutdown
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  delay(10);

  SPI.end();
  delay(10);

  Serial.println("[SD] SD card shutdown complete");
}

void endSD() {
  if (!sdAvailable) {
    // Still need to reset SPI state even if SD was marked unavailable
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    SPI.end();
    return;
  }

  forceEndSD();
}

bool reinitSD() {
  if (!isBootPhase && sdFailCount > 0 &&
      (millis() - lastSDRetry) < SD_RETRY_INTERVAL) {
    return false;
  }

  // Use iterative retry instead of recursion to prevent stack overflow
  int maxBootRetries = isBootPhase ? 3 : 1;

  for (int retryAttempt = 0; retryAttempt < maxBootRetries; retryAttempt++) {
    if (retryAttempt > 0) {
      delay(500);
    }

    lastSDRetry = millis();

    Serial.println("[SD] Attempting SD card initialization...");

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    delay(50);

    SPI.end();
    delay(10);
    SPI.begin();
    SPI.setFrequency(4000000);
    delay(50);

    for (int i = 0; i < 10; i++) {
      SPI.transfer(0xFF);
    }
    delay(10);

    if (!SD.begin(PIN_SD_CS)) {
      sdFailCount++;
      Serial.printf("[SD] Init failed (attempt %d)\n", sdFailCount);
      continue; // Try next iteration instead of recursive call
    }

    delay(200);

    uint8_t cardType = SD.type();
    if (cardType == 0) {
      Serial.println("[SD] No SD card detected");
      sdFailCount++;
      SD.end();
      continue; // Try next iteration
    }

    Serial.printf("[SD] Card type: %s\n", cardType == 1   ? "SD1"
                                          : cardType == 2 ? "SD2"
                                                          : "SDHC");

    bool fileExists = false;
    for (int attempt = 0; attempt < 3; attempt++) {
      fileExists = SD.exists("/data.csv");
      if (fileExists)
        break;
      delay(50);
    }

    if (!fileExists) {
      Serial.println("[SD] data.csv not found, creating new file");

      File root = SD.open("/");
      if (!root) {
        Serial.println("[SD] Failed to access root directory");
        sdFailCount++;
        SD.end();
        continue; // Try next iteration
      }
      root.close();
      delay(50);

      File f = SD.open("/data.csv", FILE_WRITE);
      if (!f) {
        Serial.println("[SD] Failed to create data.csv");
        sdFailCount++;
        SD.end();
        continue; // Try next iteration
      }

      size_t written = f.println(CSV_HEADER);
      f.flush();
      delay(50);
      f.close();
      delay(200);

      bool verified = false;
      for (int attempt = 0; attempt < 3; attempt++) {
        if (SD.exists("/data.csv")) {
          File testFile = SD.open("/data.csv", FILE_READ);
          if (testFile && testFile.size() > 0) {
            testFile.close();
            verified = true;
            Serial.printf("[SD] Created data.csv verified (%d bytes header)\n",
                          written);
            break;
          }
          if (testFile)
            testFile.close();
        }
        delay(100);
      }

      if (!verified) {
        Serial.println("[SD] Failed to verify data.csv creation");
        sdFailCount++;
        SD.end();
        continue; // Try next iteration
      }

    } else {
      File f = SD.open("/data.csv", FILE_READ);
      if (!f) {
        Serial.println("[SD] data.csv exists but cannot be opened");

        Serial.println("[SD] Attempting recovery: removing corrupt file");
        SD.remove("/data.csv");
        delay(100);

        File newFile = SD.open("/data.csv", FILE_WRITE);
        if (!newFile) {
          Serial.println("[SD] Recovery failed");
          sdFailCount++;
          SD.end();
          continue; // Try next iteration
        }
        newFile.println(CSV_HEADER);
        newFile.flush();
        delay(50);
        newFile.close();
        delay(200);

        Serial.println("[SD] Recovery successful - new file created");
      } else {
        size_t fileSize = f.size();
        f.close();
        Serial.printf("[SD] Existing data.csv verified (%d bytes)\n", fileSize);
      }
    }

    // Success - break out of retry loop
    sdAvailable = true;
    sdFailCount = 0;
    interval = INTERVAL_SD;
    Serial.println("[SD] SD card initialized successfully");
    return true;
  }

  // All retries failed
  return false;
}

uint32_t countLogEntries() {
  if (!sdAvailable)
    return 0;

  File f = SD.open("/data.csv", FILE_READ);
  if (!f) {
    Serial.println("[SD] Failed to open data.csv for counting");
    return 0;
  }

  uint32_t count = 0;
  bool isHeader = true;

  while (f.available()) {
    int c = f.read();
    if (c == '\n') {
      if (!isHeader) {
        count++;
      } else {
        isHeader = false;
      }
    }
  }

  f.close();
  return count;
}

// Helper function for safe strncpy with guaranteed null termination
static void safeStrncpy(char *dest, const char *src, size_t destSize) {
  if (destSize == 0)
    return;
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

void readLastMeasurements() {
  historicalData.valid = false;
  for (int i = 0; i < 3; i++) {
    historicalData.t[i] = 0;
    historicalData.h[i] = 0;
    historicalData.tWater[i] = 0;
  }

  if (!sdAvailable)
    return;

  File f = SD.open("/data.csv", FILE_READ);
  if (!f) {
    Serial.println("[SD] Failed to open data.csv for reading history");
    return;
  }

  while (f.available() && f.read() != '\n') {
  }

  char lines[3][220];
  int lineCount = 0;
  char currentLine[220];
  int charIdx = 0;

  while (f.available()) {
    char c = f.read();
    if (c == '\n') {
      if (charIdx > 0) {
        currentLine[charIdx] = '\0';

        if (lineCount < 3) {
          safeStrncpy(lines[lineCount], currentLine, sizeof(lines[0]));
        } else {
          safeStrncpy(lines[0], lines[1], sizeof(lines[0]));
          safeStrncpy(lines[1], lines[2], sizeof(lines[1]));
          safeStrncpy(lines[2], currentLine, sizeof(lines[2]));
        }
        lineCount++;
        charIdx = 0;
      }
    } else if (c != '\r' && charIdx < 219) {
      currentLine[charIdx++] = c;
    }
  }

  if (charIdx > 0) {
    currentLine[charIdx] = '\0';
    if (lineCount < 3) {
      safeStrncpy(lines[lineCount], currentLine, sizeof(lines[0]));
    } else {
      safeStrncpy(lines[0], lines[1], sizeof(lines[0]));
      safeStrncpy(lines[1], lines[2], sizeof(lines[1]));
      safeStrncpy(lines[2], currentLine, sizeof(lines[2]));
    }
    lineCount++;
  }

  f.close();

  int count = min(lineCount, 3);
  for (int i = 0; i < count; i++) {
    int fieldIdx = 0;
    char field[32];
    int fieldCharIdx = 0;
    int dataIdx = count - 1 - i;

    for (int j = 0; lines[i][j] != '\0'; j++) {
      char ch = lines[i][j];
      if (ch == ',') {
        field[fieldCharIdx] = '\0';

        if (fieldIdx == 3 && fieldCharIdx > 0) {
          historicalData.t[dataIdx] = atof(field);
        } else if (fieldIdx == 4 && fieldCharIdx > 0) {
          historicalData.h[dataIdx] = atof(field);
        } else if (fieldIdx == 6 && fieldCharIdx > 0) {
          historicalData.tWater[dataIdx] = atof(field);
        }

        fieldIdx++;
        fieldCharIdx = 0;
      } else if (fieldCharIdx < 31) {
        field[fieldCharIdx++] = ch;
      }
    }

    // FIXED: Handle last field (water_ok) which doesn't end with comma
    if (fieldCharIdx > 0) {
      field[fieldCharIdx] = '\0';
      if (fieldIdx == 6 && fieldCharIdx > 0) {
        historicalData.tWater[dataIdx] = atof(field);
      }
    }
  }

  historicalData.valid = (count > 0);
  Serial.printf("[SD] Read %d historical measurements\n", count);
}

void updateLogCount() {
  if (millis() - lastLogCountUpdate >= LOG_COUNT_UPDATE_INTERVAL) {
    logCount = countLogEntries();
    lastLogCountUpdate = millis();
    Serial.printf("[LOG] Entry count: %lu\n", logCount);
  }
}

// FIXED: Corrected verification logic with retry for SD card timing issues
bool verifyLastWrite() {
  if (!sdAvailable) {
    Serial.println("[VERIFY] SD not available");
    return false;
  }

  // Retry opening file - SD card may need time after write
  File f;
  for (int attempt = 0; attempt < 5; attempt++) {
    f = SD.open("/data.csv", FILE_READ);
    if (f)
      break;
    Serial.printf("[VERIFY] Open attempt %d failed, retrying...\n",
                  attempt + 1);
    delay(150);
  }

  if (!f) {
    Serial.println("[VERIFY] Cannot open data.csv after retries");
    // Force proper shutdown before marking unavailable
    forceEndSD();
    sdAvailable = false;
    return false;
  }

  uint32_t size = f.size();
  f.close();
  delay(50);

  if (size < 10) {
    Serial.println("[VERIFY] File too small");
    return false;
  }

  uint32_t actualCount = countLogEntries();

  // FIXED: Check if actual count matches or exceeds expected count
  // (logCount was already incremented in logData)
  if (actualCount >= logCount) {
    logCount = actualCount; // Sync to actual count
    Serial.printf("[VERIFY] Write verified, LC=%lu (file: %lu bytes)\n",
                  logCount, size);
    return true;
  }

  // If actual count is less than expected, something went wrong
  Serial.printf("[VERIFY] Count mismatch! Expected: %lu, Actual: %lu\n",
                logCount, actualCount);
  logCount = actualCount; // Resync to actual
  return false;
}

void logData() {
  // FIXED: Removed recursion - use iterative approach
  int retryCount = 0;
  const int MAX_LOG_RETRIES = 2;

  while (retryCount < MAX_LOG_RETRIES) {
    if (!sdAvailable) {
      if (sdFailCount < SD_MAX_RETRIES) {
        Serial.println("[LOG] SD not available, attempting recovery...");
        if (!reinitSD()) {
          retryCount++;
          continue;
        }
      } else {
        interval = INTERVAL_NO_SD;
        return;
      }
    }

    bool fileExists = false;
    for (int attempt = 0; attempt < 3; attempt++) {
      fileExists = SD.exists("/data.csv");
      if (fileExists)
        break;
      delay(50);
    }

    if (!fileExists) {
      Serial.println("[LOG] data.csv disappeared, attempting recovery...");

      File f = SD.open("/data.csv", FILE_WRITE);
      if (!f) {
        Serial.println("[LOG] Cannot create data.csv, reinitializing SD...");
        sdAvailable = false;
        retryCount++;
        continue;
      }

      size_t written = f.println(CSV_HEADER);
      f.flush();
      delay(50);
      f.close();
      delay(100);

      if (SD.exists("/data.csv")) {
        Serial.printf("[LOG] Created missing data.csv (%d bytes)\n", written);
        logCount = 0;
      } else {
        Serial.println("[LOG] Failed to create data.csv");
        sdAvailable = false;
        retryCount++;
        continue;
      }
    }

    File f = SD.open("/data.csv", FILE_WRITE);
    if (!f) {
      Serial.println("[LOG] Failed to open data.csv for writing");
      Serial.println("[LOG] Attempting SD recovery...");
      sdAvailable = false;
      retryCount++;
      continue;
    }

    time_t utcNow = rtc.now().unixtime();
    struct tm *tmg = gmtime(&utcNow);

    char ts_utc[25];
    snprintf(ts_utc, sizeof(ts_utc), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             1900 + tmg->tm_year, 1 + tmg->tm_mon, tmg->tm_mday, tmg->tm_hour,
             tmg->tm_min, tmg->tm_sec);

    // FIXED: Use uint32_t for epoch_ms formatting on ESP8266
    uint32_t epoch_sec = (uint32_t)utcNow;

    char ts_local[20];
    snprintf(ts_local, sizeof(ts_local), "%04d-%02d-%02d %02d:%02d:%02d",
             data.ts.year(), data.ts.month(), data.ts.day(), data.ts.hour(),
             data.ts.minute(), data.ts.second());

    char tempStr[16] = "";
    char humStr[16] = "";
    char dewStr[16] = "";
    char waterStr[16] = "";

    if (data.shtValid) {
      snprintf(tempStr, sizeof(tempStr), "%.2f", data.t);
      snprintf(humStr, sizeof(humStr), "%.1f", data.h);
      snprintf(dewStr, sizeof(dewStr), "%.2f", data.td);
    }
    if (data.waterTempValid) {
      snprintf(waterStr, sizeof(waterStr), "%.2f", data.tWater);
    }

    int sht_ok = data.shtValid ? 1 : 0;
    int water_ok = data.waterTempValid ? 1 : 0;

    char buf[220];
    // FIXED: Format epoch_ms as two parts to avoid %llu issues on ESP8266
    snprintf(buf, sizeof(buf), "%s,%lu000,%s,%s,%s,%s,%s,%d,%d", ts_utc,
             epoch_sec, ts_local, tempStr, humStr, dewStr, waterStr, sht_ok,
             water_ok);

    size_t written = f.println(buf);

    f.flush();
    delay(50); // Allow flush to complete
    f.close();
    delay(200); // FIXED: Longer delay for SD card to sync FAT table

    if (written == 0) {
      Serial.println("[LOG] Write failed - zero bytes written");
      sdAvailable = false;
      retryCount++;
      continue;
    }

    // FIXED: Increment BEFORE verification so counts match
    logCount++;
    Serial.printf("[LOG] Data committed: %d bytes (expected LC=%lu)\n", written,
                  logCount);
    return; // Success - exit the retry loop
  }

  // All retries failed
  interval = INTERVAL_NO_SD;
}

bool rotateLogFile() {
  if (!sdAvailable && !reinitSD()) {
    Serial.println("[CSV] SD not available for rotation");
    return false;
  }

  DateTime ts = nowLocalFromRTC();
  char newName[40];
  snprintf(newName, sizeof(newName), "/data_%04d%02d%02d_%02d%02d%02d.csv",
           ts.year(), ts.month(), ts.day(), ts.hour(), ts.minute(),
           ts.second());

  if (SD.exists("/data.csv")) {
    if (SD.exists(newName)) {
      SD.remove(newName);
      delay(100);
    }

    if (!SD.rename("/data.csv", newName)) {
      SD.remove("/data.csv");
      delay(100);
      Serial.println("[CSV] Rename failed, removed old file");
    } else {
      Serial.printf("[CSV] Rotated: %s\n", newName);
      delay(100);
    }
  }

  File f = SD.open("/data.csv", FILE_WRITE);
  if (!f) {
    Serial.println("[CSV] Failed to create fresh data.csv");
    sdAvailable = false;
    return false;
  }

  size_t written = f.println(CSV_HEADER);
  f.flush();
  delay(50);
  f.close();
  delay(200);

  bool verified = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (SD.exists("/data.csv")) {
      File testFile = SD.open("/data.csv", FILE_READ);
      if (testFile && testFile.size() > 0) {
        testFile.close();
        verified = true;
        Serial.printf("[CSV] New file verified (%d bytes)\n", written);
        break;
      }
      if (testFile)
        testFile.close();
    }
    delay(100);
  }

  if (!verified) {
    Serial.println("[CSV] New file creation verification failed");
    sdAvailable = false;
    return false;
  }

  logCount = 0;
  lastLogCountUpdate = millis();
  historicalData.valid = false;

  Serial.println("[CSV] New log file created and verified");
  return true;
}

// ============================================================================
// DISPLAY FUNCTIONS
// ============================================================================

void displayOff() {
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  displayOn = false;
  Serial.println("[DISPLAY] OFF");
}

void displayOnFunc() {
  delay(2);
  display.ssd1306_command(SSD1306_DISPLAYON);
  displayOn = true;
  displayUpdateCount = 0;
  lastDisplayUpdate = millis();

  display.clearDisplay();
  display.display();

  logCount = countLogEntries();
  lastLogCountUpdate = millis();
  readLastMeasurements();

  Serial.println("[DISPLAY] ON - Data refreshed");
}

void updateDisplay() {
  if (!displayOn)
    return;

  if (dsConvPending && (millis() >= dsConvReadyAt)) {
    float tWater = ds18b20.getTempCByIndex(0);
    if (tWater != DS18B20_ERROR_DISCONNECTED &&
        tWater != DS18B20_ERROR_POWER_ON_RESET) {
      data.tWater = tWater;
      data.waterTempValid = true;
      Serial.printf("[SENSOR] DS18B20 async done: %.2f°C\n", data.tWater);
    } else {
      Serial.println("[SENSOR] DS18B20 invalid reading");
      ds18b20OK = false;
    }
    dsConvPending = false;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  DateTime nowLocal = nowLocalFromRTC();
  char buf[32];

  display.setTextSize(1);
  display.setCursor(0, 0);
  if (sdAvailable) {
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d SDY %02d:%02d:%02d",
             nowLocal.day(), nowLocal.month(), nowLocal.year() % 100,
             nowLocal.hour(), nowLocal.minute(), nowLocal.second());
  } else {
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d NO1 %02d:%02d:%02d",
             nowLocal.day(), nowLocal.month(), nowLocal.year() % 100,
             nowLocal.hour(), nowLocal.minute(), nowLocal.second());
  }
  display.print(buf);
  display.drawLine(0, 8, 127, 8, SSD1306_WHITE);

  if (displayUpdateCount < 10) {
    display.setTextSize(1);
    display.setCursor(0, 14);
    if (data.waterTempValid) {
      snprintf(buf, sizeof(buf), "LC:%lu W:%.1fC", logCount, data.tWater);
    } else if (dsConvPending) {
      snprintf(buf, sizeof(buf), "LC:%lu W:...", logCount);
    } else {
      snprintf(buf, sizeof(buf), "LC:%lu W:ERROR", logCount);
    }
    display.print(buf);

    if (!data.shtValid) {
      display.setTextSize(1);
      display.setCursor(0, 30);
      display.print("SHT3x ERROR");
    } else {
      display.setTextSize(2);
      display.setCursor(0, 30);
      snprintf(buf, sizeof(buf), "T:%.1fC", data.t);
      display.print(buf);

      int16_t x = display.getCursorX();
      display.setTextSize(1);
      display.setCursor(x + 2, 33);
      snprintf(buf, sizeof(buf), "TP:%.1f", data.td);
      display.print(buf);

      display.setTextSize(2);
      display.setCursor(0, 48);
      snprintf(buf, sizeof(buf), "H:%.1f%%", data.h);
      display.print(buf);
    }
  } else {
    if (historicalData.valid) {
      display.setTextSize(1);

      display.setCursor(0, 16);
      snprintf(buf, sizeof(buf), "DSB %.1f %.1f %.1f", historicalData.tWater[0],
               historicalData.tWater[1], historicalData.tWater[2]);
      display.print(buf);

      display.setCursor(0, 28);
      snprintf(buf, sizeof(buf), "TMP %.1f %.1f %.1f", historicalData.t[0],
               historicalData.t[1], historicalData.t[2]);
      display.print(buf);

      display.setCursor(0, 40);
      snprintf(buf, sizeof(buf), "RLF %.0f %.0f %.0f", historicalData.h[0],
               historicalData.h[1], historicalData.h[2]);
      display.print(buf);
    } else {
      display.setTextSize(1);
      display.setCursor(0, 24);
      display.print("No history");
    }
  }

  display.display();
  displayUpdateCount++;
}

// ============================================================================
// SENSOR FUNCTIONS
// ============================================================================

void measureAndLog() {
  isMeasuring = true;

  data.shtValid = false;
  data.waterTempValid = false;

  sht_retry_counter++;
  if (!shtOK && (sht_retry_counter % SHT_RETRY_INTERVAL == 0)) {
    shtOK = sht31.begin(0x44) || sht31.begin(0x45);
    Serial.printf("[SENSOR] SHT3x retry #%lu: %s\n",
                  sht_retry_counter / SHT_RETRY_INTERVAL,
                  shtOK ? "OK" : "FAIL");
  }

  if (shtOK) {
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      data.t = t;
      data.h = h;
      data.td = dewpoint(t, h);
      data.shtValid = true;
      Serial.printf("[SENSOR] SHT3x: T=%.2f°C H=%.1f%% TD=%.2f°C\n", t, h,
                    data.td);
    } else {
      Serial.println("[SENSOR] SHT3x returned NaN");
      shtOK = false;
    }
  }

  if (ds18b20OK) {
    ds18b20.requestTemperatures();
    float tWater = ds18b20.getTempCByIndex(0);
    if (tWater != DS18B20_ERROR_DISCONNECTED &&
        tWater != DS18B20_ERROR_POWER_ON_RESET) {
      data.tWater = tWater;
      data.waterTempValid = true;
      Serial.printf("[SENSOR] DS18B20: %.2f°C\n", tWater);
    } else {
      Serial.println("[SENSOR] DS18B20 invalid");
      ds18b20OK = false;
    }
  }

  // FIXED: Guard against invalid RTC state
  uint32_t nowUnix = rtcOK ? rtc.now().unixtime() : 0;
  data.ts = rtcOK ? unixToLocal(nowUnix) : DateTime(2023, 1, 1, 0, 0, 0);

  if (sdAvailable) {
    logData();
    verifyLastWrite();
  }

  lastMeasureUnix = nowUnix;
  isMeasuring = false;
}

void measureDisplay() {
  Serial.println("[BUTTON] Display measurement");

  data.shtValid = false;
  data.waterTempValid = false;

  if (!shtOK) {
    shtOK = sht31.begin(0x44) || sht31.begin(0x45);
  }

  if (shtOK) {
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      data.t = t;
      data.h = h;
      data.td = dewpoint(t, h);
      data.shtValid = true;
      Serial.printf("[BUTTON] SHT3x: T=%.2f°C H=%.1f%%\n", t, h);
    } else {
      shtOK = false;
    }
  }

  if (ds18b20OK) {
    dsConvPending = true;
    int res = ds18b20.getResolution();
    uint32_t convMs = (res == 9)    ? 94
                      : (res == 10) ? 188
                      : (res == 11) ? 375
                                    : 750;
    ds18b20.setWaitForConversion(false);
    ds18b20.requestTemperatures();
    dsConvReadyAt = millis() + convMs;
    Serial.printf("[BUTTON] DS18B20 async: %d-bit, %lums\n", res,
                  (unsigned long)convMs);
  }

  uint32_t nowUnix = rtc.now().unixtime();
  data.ts = unixToLocal(nowUnix);
}

// ============================================================================
// NTP AND RTC INITIALIZATION
// ============================================================================

bool syncTimeWithNTP() {
  Serial.println("\n[BOOT] Starting WiFi NTP sync...");

  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000UL) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BOOT] WiFi connection failed");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    return false;
  }

  Serial.printf("[BOOT] WiFi connected: %s IP=%s RSSI=%d dBm\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.RSSI());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  Serial.println("[BOOT] Waiting for NTP...");
  time_t now = time(nullptr);
  t0 = millis();
  while ((now < UNIX_EPOCH_2021) && (millis() - t0 < 15000UL)) {
    delay(250);
    now = time(nullptr);
    Serial.print(".");
  }
  Serial.println();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  if (now < UNIX_EPOCH_2021) {
    Serial.println("[BOOT] NTP sync failed");
    return false;
  }

  struct tm *ti = gmtime(&now);
  DateTime utcTime(1900 + ti->tm_year, 1 + ti->tm_mon, ti->tm_mday, ti->tm_hour,
                   ti->tm_min, ti->tm_sec);
  rtc.adjust(utcTime);

  Serial.printf("[BOOT] RTC set to: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                utcTime.year(), utcTime.month(), utcTime.day(), utcTime.hour(),
                utcTime.minute(), utcTime.second());

  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_FIRST_BOOT_ADDR, EEPROM_MAGIC);
  EEPROM.commit();
  EEPROM.end();

  Serial.println("[BOOT] First boot completed\n");
  return true;
}

void initSensors() {
  shtOK = sht31.begin(0x44) || sht31.begin(0x45);

  ds18b20.begin();
  ds18b20OK = (ds18b20.getDeviceCount() > 0);

  rtcOK = rtc.begin();

  EEPROM.begin(EEPROM_SIZE);
  byte eepromValue = EEPROM.read(EEPROM_FIRST_BOOT_ADDR);
  firstBootDone = (eepromValue == EEPROM_MAGIC);
  Serial.printf("[BOOT] EEPROM: 0x%02X => firstBootDone=%s\n", eepromValue,
                firstBootDone ? "true" : "false");
  EEPROM.end();

  bool needNtp =
      rtcOK && (!firstBootDone || rtc.lostPower() || !isRtcPlausible());

  if (needNtp) {
    Serial.println("[BOOT] Running NTP sync...");
    if (!syncTimeWithNTP()) {
      if (rtc.lostPower() || !isRtcPlausible()) {
        // NOTE: __DATE__ and __TIME__ are in build machine's local time, not
        // UTC This may cause a timezone offset, but it's better than an invalid
        // time
        DateTime build(F(__DATE__), F(__TIME__));
        rtc.adjust(build);
        Serial.println(
            "[BOOT] Fallback: RTC set to build time (local, not UTC)");
      }
    }
  } else {
    Serial.println("[BOOT] NTP skipped (RTC valid)");
  }

  if (rtcOK) {
    lastMeasureUnix = rtc.now().unixtime();
  }

  isBootPhase = true;
  sdFailCount = 0;
  sdAvailable = reinitSD();
  isBootPhase = false;

  interval = sdAvailable ? INTERVAL_SD : INTERVAL_NO_SD;

  if (sdAvailable) {
    logCount = countLogEntries();
  }

  Serial.printf("[INIT] Sensors: SHT=%d DS18B20=%d RTC=%d SD=%d\n", shtOK,
                ds18b20OK, rtcOK, sdAvailable);
}

// ============================================================================
// SLEEP AND WAKE FUNCTIONS
// ============================================================================

void enterLightSleep() {
  if (dsConvPending) {
    uint32_t waitStart = millis();
    while (dsConvPending && (millis() - waitStart < 2000UL)) {
      // FIXED: Use subtraction-based comparison for millis() rollover safety
      if ((int32_t)(millis() - dsConvReadyAt) >= 0) {
        float tWater = ds18b20.getTempCByIndex(0);
        if (tWater != DS18B20_ERROR_DISCONNECTED &&
            tWater != DS18B20_ERROR_POWER_ON_RESET) {
          data.tWater = tWater;
          data.waterTempValid = true;
        }
        dsConvPending = false;
      }
      delay(10);
    }
    dsConvPending = false;
  }

  uint32_t waitStart = millis();
  while (isMeasuring && (millis() - waitStart < 5000UL)) {
    delay(50);
  }

  const uint32_t MAX_SLEEP_SEC = 268;
  uint32_t remaining = interval;

  if (rtcOK) {
    uint32_t nowUnix = rtc.now().unixtime();
    // FIXED: Guard against integer underflow
    uint32_t elapsed =
        (nowUnix >= lastMeasureUnix) ? (nowUnix - lastMeasureUnix) : 0;
    remaining = (elapsed < interval) ? (interval - elapsed) : 1;
  }
  if (remaining > MAX_SLEEP_SEC)
    remaining = MAX_SLEEP_SEC;
  if (remaining == 0)
    remaining = 1;

  bool longSleep = (remaining > 30);

  if (longSleep && sdAvailable) {
    endSD();
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);

  wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
  wifi_fpm_open();

  // FIXED: Always use button (GPIO0) as wake source
  // RTC wake is checked manually after waking
  bool useRtcWake = (remaining > 250);

  // Re-initialize button pin before sleep to ensure clean state
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  delay(1);

  if (useRtcWake) {
    pinMode(PIN_WAKE_RX, INPUT_PULLUP);
  }

  // Always enable GPIO0 (button) as the wake source
  gpio_pin_wakeup_enable(GPIO_ID_PIN(0), GPIO_PIN_INTR_LOLEVEL);

  Serial.printf("[SLEEP] Light sleep: %lus, wake=GPIO0(btn), RTC=%s, SD=%s\n",
                remaining, useRtcWake ? "enabled" : "disabled",
                longSleep ? "shutdown" : "kept");
  Serial.flush();

  while (remaining > 0) {
    // Check button before sleep
    if (digitalRead(PIN_BUTTON) == LOW)
      break;

    uint32_t slice = (remaining > 5) ? 5 : remaining;
    wifi_fpm_do_sleep(slice * 1000000UL);
    delay(10); // Small delay after SDK sleep call

    // Check wake sources
    bool buttonLow = (digitalRead(PIN_BUTTON) == LOW);
    bool rtcLow = (useRtcWake && (digitalRead(PIN_WAKE_RX) == LOW));

    if (!buttonLow && !rtcLow) {
      delay(slice * 1000UL);
      remaining -= slice;
    } else {
      break;
    }
  }

  gpio_pin_wakeup_disable();
  wifi_fpm_close();

  delay(5);

  bool buttonLow = (digitalRead(PIN_BUTTON) == LOW);
  bool rtcLow = (digitalRead(PIN_WAKE_RX) == LOW);

  if (buttonLow) {
    Serial.println("[WAKE] Button wake");

    if (longSleep || !sdAvailable) {
      Serial.println("[WAKE] Reinitializing SD...");
      reinitSD();
    }

    displayOnFunc();
    measureDisplay();
    updateDisplay();

    uint32_t holdStart = millis();
    while (digitalRead(PIN_BUTTON) == LOW &&
           (millis() - holdStart) < LONG_PRESS_MS) {
      if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
        updateDisplay();
        lastDisplayUpdate = millis();
      }
      delay(50);
    }

    if (digitalRead(PIN_BUTTON) == LOW) {
      if (!sdAvailable) {
        reinitSD();
      }

      bool ok = rotateLogFile();
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("NEW CSV");
      display.setCursor(0, 14);
      display.print(ok ? "LOG STARTED" : "Error");
      display.setCursor(0, 28);
      display.print("Release Button!");
      display.display();
      delay(2000);
      displayOnFunc();
    }

    uint32_t debounceStart = millis();
    while (digitalRead(PIN_BUTTON) == LOW &&
           (millis() - debounceStart < 5000UL)) {
      delay(50);
    }

  } else {
    Serial.println(rtcLow ? "[WAKE] RTC wake" : "[WAKE] Timer wake");

    if (longSleep || !sdAvailable) {
      Serial.println("[WAKE] Reinitializing SD card...");
      if (!reinitSD()) {
        Serial.println("[WAKE] SD reinit failed");
        sdAvailable = false;
        interval = INTERVAL_NO_SD;
      }
    } else {
      Serial.println("[WAKE] SD card still initialized");
    }

    if (rtcLow) {
      rtc.clearAlarm(1);
      rtc.clearAlarm(2);
      rtc.writeSqwPinMode(DS3231_OFF);
    }

    measureAndLog();
  }
}

// ============================================================================
// SETUP AND LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n[SETUP] System starting...");

  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_WAKE_RX, INPUT_PULLUP);

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Wire.begin();
  Wire.setClock(100000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[SETUP] Display init failed");
  }
  display.clearDisplay();
  display.display();
  displayOff();

  initSensors();
  measureAndLog();

  Serial.println("[SETUP] Startup complete\n");
}

void loop() {
  if (displayOn) {
    if (displayUpdateCount >= 20) {
      displayOff();
    } else {
      if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
        updateDisplay();
        lastDisplayUpdate = millis();
        updateLogCount();
      }
    }
  }

  if (rtcOK) {
    uint32_t nowUnix = rtc.now().unixtime();
    // FIXED: Guard against integer underflow
    uint32_t elapsed =
        (nowUnix >= lastMeasureUnix) ? (nowUnix - lastMeasureUnix) : 0;

    if (elapsed >= interval) {
      Serial.printf("[LOOP] Measurement due: %lu >= %lu\n", elapsed, interval);
      measureAndLog();
    }
  }

  if (!displayOn) {
    enterLightSleep();
  } else {
    delay(100);
  }
}