#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <GxEPD2_BW.h>
#include <Adafruit_SHTC3.h>
#include <UniversalTelegramBot.h>
#include "config.h"

// ----------- EPD pins (ESP32-S3) -----------
#define EPD_DC      10
#define EPD_CS      11
#define EPD_SCK     12
#define EPD_MOSI    13
#define EPD_RST      9
#define EPD_BUSY     8
#define EPD_PWR      6    // ACTIVE-LOW (ON = LOW)
#define VBAT_PWR    17    // rail enable (ON = HIGH)

// ----------- I2C pins -----------
#define I2C_SDA     47
#define I2C_SCL     48

// ----------- Settings -----------
#define SPI_CLOCK_HZ            4000000
#define SLEEP_SECONDS           60
#define WIFI_TIMEOUT_MS         15000
#define TELEGRAM_HOT_INTERVAL_S  60
#define TELEGRAM_CHECK_INTERVAL_S 5

// ----------- EPD (1.54" D67) -----------
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
  GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ----------- Sensors / Telegram -----------
Adafruit_SHTC3 shtc3;
WiFiClientSecure wifiClient;
UniversalTelegramBot bot(BOT_TOKEN, wifiClient);

float maxTempC = -1000.0f;
float maxHumPct = -1000.0f;
uint32_t lastTelegramHotMs = 0;
uint32_t lastSensorUpdateMs = 0;
uint32_t lastTelegramCheckMs = 0;

float tempC = NAN;
float humPct = NAN;
String tempString;
String humString;
uint8_t partialRefreshCount = 0;
#define FULL_REFRESH_EVERY 10

bool connectToWifi(uint32_t timeoutMs);
bool readSensor(float& outTempC, float& outHumPct);
bool hasTelegramConfig();
String buildStatusMessage(bool isHot);
void handleTelegramCommands(bool isHot);
void maybeSendTelegramAlert(bool isHot);
void printOnSerial(bool isHot);
void epdDraw(bool isHot);

void setup() {
  Serial.begin(115200);
  Serial.println("Starting up...");

  pinMode(VBAT_PWR, OUTPUT);
  digitalWrite(VBAT_PWR, HIGH);

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, LOW); // EPD ON

  pinMode(3, OUTPUT);
  digitalWrite(3, HIGH);

  delay(10);

  Wire.begin(I2C_SDA, I2C_SCL);
  shtc3.begin();

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.epd2.selectSPI(SPI, SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(3);

  connectToWifi(WIFI_TIMEOUT_MS);
  wifiClient.setInsecure();
}

void loop() {
  const uint32_t now = millis();
  const bool wifiOk = connectToWifi(WIFI_TIMEOUT_MS);

  if (wifiOk && (lastTelegramCheckMs == 0 || (now - lastTelegramCheckMs) >= (uint32_t)TELEGRAM_CHECK_INTERVAL_S * 1000UL)) {
    lastTelegramCheckMs = now;
    const bool isHot = (!isnan(tempC) && tempC >= THRESHOLD);
    handleTelegramCommands(isHot);
  }

  if (lastSensorUpdateMs == 0 || (now - lastSensorUpdateMs) >= (uint32_t)SLEEP_SECONDS * 1000UL) {
    lastSensorUpdateMs = now;

    if (!readSensor(tempC, humPct)) {
      Serial.println("SHTC3 read failed.");
    }

    if (!isnan(tempC) && (maxTempC < -999.0f || tempC > maxTempC)) maxTempC = tempC;
    if (!isnan(humPct) && (maxHumPct < -999.0f || humPct > maxHumPct)) maxHumPct = humPct;

    tempString = isnan(tempC) ? "--.-" : String(tempC, 0);
    humString  = isnan(humPct) ? "--.-" : String(humPct, 0);

    const bool isHot = (!isnan(tempC) && tempC >= THRESHOLD);

    printOnSerial(isHot);
    if (wifiOk) {
      maybeSendTelegramAlert(isHot);
    } else {
      Serial.println("WiFi not connected, skipping Telegram.");
    }
    epdDraw(isHot);
  }
}

bool connectToWifi(uint32_t timeoutMs)
{
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("WiFi connect timeout.");
  return false;
}

bool readSensor(float& outTempC, float& outHumPct)
{
  sensors_event_t humEvent, tempEvent;
  bool ok = shtc3.getEvent(&humEvent, &tempEvent);
  if (!ok) {
    delay(5);
    ok = shtc3.getEvent(&humEvent, &tempEvent);
  }

  if (!ok) {
    return false;
  }

  outTempC = tempEvent.temperature;
  outHumPct = humEvent.relative_humidity;
  return true;
}

bool hasTelegramConfig()
{
  return strlen(BOT_TOKEN) > 0 && strlen(CHAT_ID) > 0;
}

String buildStatusMessage(bool isHot)
{
  String msg;
  msg += isHot ? "Fenny is HOT!\n" : "Fenny is OK:\n";
  msg += "Temp: ";
  if (isnan(tempC)) {
    msg += "n/a";
  } else {
    msg += String(tempC, 1);
    msg += "C";
  }
  msg += ", Humidity: ";
  if (isnan(humPct)) {
    msg += "n/a";
  } else {
    msg += String(humPct, 1);
    msg += "%";
  }
  msg += "\nMax Temp: ";
  if (maxTempC < -999.0f) {
    msg += "n/a";
  } else {
    msg += String(maxTempC, 1);
    msg += "C";
  }
  msg += ", Max Humidity: ";
  if (maxHumPct < -999.0f) {
    msg += "n/a";
  } else {
    msg += String(maxHumPct, 1);
    msg += "%";
  }
  return msg;
}

static const char* KEYBOARD = "[[{\"text\":\"status\"},{\"text\":\"uptime\"}]]";

void replyWithKeyboard(const String& chat_id, const String& msg)
{
  bot.sendMessageWithReplyKeyboard(chat_id, msg, "", KEYBOARD, true, false);
}

void handleTelegramCommands(bool isHot)
{
  if (!hasTelegramConfig()) return;

  const int numMessages = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < numMessages; i++) {
    const String& chat_id = bot.messages[i].chat_id;
    const String& text    = bot.messages[i].text;
    if (text == "/start") {
      replyWithKeyboard(chat_id, "Hot Dog 2 - usa i bottoni per i comandi.");
    } else if (text == "status") {
      replyWithKeyboard(chat_id, buildStatusMessage(isHot));
    } else if (text == "uptime") {
      const uint32_t totalSec = millis() / 1000UL;
      const uint32_t h  = totalSec / 3600;
      const uint32_t m  = (totalSec % 3600) / 60;
      const uint32_t s  = totalSec % 60;
      char buf[32];
      snprintf(buf, sizeof(buf), "Uptime: %02lu:%02lu:%02lu", h, m, s);
      replyWithKeyboard(chat_id, buf);
    }
  }
}

void maybeSendTelegramAlert(bool isHot)
{
  if (!isHot) {
    return;
  }

  if (!hasTelegramConfig()) {
    Serial.println("Telegram disabled: BOT_TOKEN/CHAT_ID not set in config.h");
    return;
  }

  const uint32_t intervalMs = (uint32_t)TELEGRAM_HOT_INTERVAL_S * 1000UL;
  if (lastTelegramHotMs != 0 && (millis() - lastTelegramHotMs) < intervalMs) {
    return;
  }


  const String message = buildStatusMessage(isHot);
  const bool sent = bot.sendMessage(CHAT_ID, message, "");

  if (sent) {
    lastTelegramHotMs = millis();
    Serial.println("Telegram message sent.");
  } else {
    Serial.println("Telegram send failed.");
  }
}

void printOnSerial(bool isHot)
{
  Serial.println("---- HOT DOG EINK ----");
  Serial.print("Temp:      ");
  Serial.println(isnan(tempC) ? String("n/a") : String(tempC, 1));
  Serial.print("Max Temp:  ");
  Serial.println((maxTempC < -999.0f) ? String("n/a") : String(maxTempC, 1));
  Serial.print("Humid:     ");
  Serial.println(isnan(humPct) ? String("n/a") : String(humPct, 1));
  Serial.print("Max Humid: ");
  Serial.println((maxHumPct < -999.0f) ? String("n/a") : String(maxHumPct, 1));
  Serial.print("Status:    ");
  Serial.println(isHot ? "FENNY HOT" : "FENNY OK");
}

void epdDraw(bool isHot)
{
  if (partialRefreshCount == 0) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }
  partialRefreshCount = (partialRefreshCount + 1) % FULL_REFRESH_EVERY;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setTextColor(GxEPD_BLACK);

    // Line 1: temp and humidity
    display.setTextSize(3);
    display.setCursor(5, 10);
    display.print(tempString);
    display.print("C ");
    display.print(humString);
    display.print("%");

    // Lines 2-4: status
    display.setTextSize(5);
    display.setCursor(5, 55);
    display.print("FENNY");
    if (isHot) {
      display.setCursor(5, 110);
      display.print("TROPPO");
      display.setCursor(5, 160);
      display.print("CALDO!");
    } else {
      display.setCursor(5, 110);
      display.print("STA");
      display.setCursor(5, 160);
      display.print("BENE");
    }
  } while (display.nextPage());
}


