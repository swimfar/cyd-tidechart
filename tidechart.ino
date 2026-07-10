/*
 * Tide chart for CYD based on https://github.com/kronsby/esp-tidechart/blob/main/tidechart.ino
 * and CYD setup info from https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/
 * 
 * Setup:
 * 1. Enter wifi credentials
 * 2. Set NOAA station ID
 * 3. Set time zone
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// CYD Libraries
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// Do some compilation-time library checks
#if SPI_FREQUENCY > 27000000
  Serial.println("Warning: SPI freq. is greater than 27 MHz.  This can cause screen issues");
#endif

// Replace with your WiFi network credentials
const char* ssid = "ssid";
const char* password = "password";

// You can find station IDs at: https://tidesandcurrents.noaa.gov/stations.html
const char* stationId = "9415144";  // Chicago

// Time zone settings for Central Time
// GMT offset in seconds: -6 hours * 3600 seconds/hour = -21600
const long gmtOffset_sec = -(6 * 3600);  //6 hour shift
// Daylight Saving Time offset in seconds: 1 hour * 3600 seconds/hour = 3600
const int daylightOffset_sec = 3600;
const char* ntpServer = "pool.ntp.org"; // NTP server for time synchronization

TFT_eSPI tft = TFT_eSPI();  // Create a TFT_eSPI instance (Display)

// Touchscreen pins
#define XPT2046_IRQ 36   // T_IRQ
#define XPT2046_MOSI 32  // T_DIN
#define XPT2046_MISO 39  // T_OUT
#define XPT2046_CLK 25   // T_CLK
#define XPT2046_CS 33    // T_CS

SPIClass touchscreenSPI = SPIClass(VSPI);  // SPI communication channel
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);  // Touchscreen

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FONT_SIZE 2


DynamicJsonDocument doc(8192);  // full JSON tide data stream
JsonArray predictions;  // parsed JSON tide data array
float minTide, maxTide;
String highTideEvents[2];
String lowTideEvents[2];
int dataDayOfYear = -1;

// DigiCert Global Root G2 certificate
//#include "cert.h"

void connectToWiFi();
void initTime();
void fetchAndDisplayTides();
bool getTidePredictions();
void processTidePredictions();
void drawTideChart(const struct tm& timeinfo);

void setup() {
  Serial.begin(115200);

  // Debug
  Serial.println(SPI_FREQUENCY);
  Serial.println(TFT_CS);
  Serial.println(TOUCH_CS);

  // ================================================================
  // STEP 1: FORCE RESET & BUS CLEAR (Do this first!)
  // ================================================================
  // Force both Chip Select pins HIGH to cleanly disconnect both chips from the SPI bus
  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH); // Disable TFT Display CS
  pinMode(TOUCH_CS, OUTPUT); digitalWrite(TOUCH_CS, HIGH); // Disable Touchscreen CS
  
  // Give the physical hardware a moment to clear any residual voltage lines
  delay(100); 

 
  // ================================================================
  // STEP 2: TOUCHSCREEN SETUP
  // ================================================================
  // Start the SPI bus for the touchscreen and initialize the controller
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);  // Set the Touchscreen rotation in landscape mode

  // ================================================================
  // STEP 3: DISPLAY INITIALIZATION
  // ================================================================
  // Finally, initialize the TFT display on the clean bus
  tft.init();
  tft.setRotation(1);  // Set the TFT display rotation in landscape mode
  tft.fillScreen(TFT_BLUE);  // Clear the screen before writing to it
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);  // explicitly turn on the backlight
  
  // Set X and Y coordinates for center of display
  int centerX = SCREEN_WIDTH / 2;
  int centerY = SCREEN_HEIGHT / 2;

  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawCentreString("Connecting to WiFi...", centerX, 10, FONT_SIZE);
  connectToWiFi();
  tft.drawCentreString("WiFi Connected!", centerX, 30, FONT_SIZE);
//
  initTime();
  delay(500);
  tft.fillScreen(TFT_BLUE);  // Clear the screen before writing to it
  tft.drawCentreString("Hello, tide!", centerX, 30, FONT_SIZE);
  //tft.drawCentreString("Touch screen to test", centerX, centerY, FONT_SIZE);

  fetchAndDisplayTides();
}


void loop() {
  delay(5 * 60 * 1000);  // every 5 min
  fetchAndDisplayTides();
}


void connectToWiFi() {
  Serial.println("Connecting to WiFi...");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);
  WiFi.mode(WIFI_STA);  // force board into Station (Client) mode
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    tft.drawString(".", 10 + (attempts * 10), 20, FONT_SIZE);
    attempts++;
  }
  Serial.println();
  Serial.println(WiFi.status());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
    tft.fillScreen(TFT_RED);
    tft.drawString("WiFi Failed!", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, FONT_SIZE);
    while (true) delay(1000);
  }
}


void initTime() {
  Serial.print("Synchronizing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("\nFailed to obtain time from NTP server!");
    tft.fillRect(0, 50, tft.width(), 30, TFT_RED);
    tft.drawString("Time Sync Failed!", 10, 50, FONT_SIZE);
    return;
  }
  Serial.println("\nTime synchronized.");
  tft.fillRect(0, 50, tft.width(), 40, TFT_BLACK);
  tft.drawString("Time Synced!", 10, 60, FONT_SIZE);
}


void fetchAndDisplayTides() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Failed to get time!", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, FONT_SIZE);
    return;  // This should fail more gracefully instead of blanking the screen
  }

  if (timeinfo.tm_yday != dataDayOfYear) {
    Serial.println("New day detected. Fetching fresh tide data...");
    tft.fillScreen(TFT_BLACK); // Clear screen to show status
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Fetching new tide data...", tft.width()/2 - 100, tft.height()/2, FONT_SIZE);

    if (getTidePredictions()) {
      processTidePredictions();
      dataDayOfYear = timeinfo.tm_yday;
      Serial.println("Successfully fetched and processed new data.");
    } else {
      Serial.println("Failed to fetch new tide data. Will retry later.");
      return;  // maybe add status icons for these kinds of things.
    }
  } else {
    Serial.println("Data for today already cached. Redrawing chart.");
  }

  drawTideChart(timeinfo);
}


/**
 * Fetches and parses tide prediction data from the NOAA API.
 * @return True on success, false on failure.
 */
bool getTidePredictions() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=predictions&application=NOAA.TidesAndCurrents&station=" +
               String(stationId) + "&begin_date=" + String(dateBuffer) +
               "&end_date=" + String(dateBuffer) +
               "&datum=MLLW&units=english&time_zone=lst&format=json";

  Serial.println(url);
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  //http.begin(url, root_ca);
  http.begin(client, url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    tft.fillScreen(TFT_RED);
    tft.drawString("HTTP Error: " + String(httpCode), 10, 10, FONT_SIZE);
    http.end();
    return false;
  }
  doc.clear(); // Clear previous JSON data
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    tft.fillScreen(TFT_RED);
    tft.drawString("JSON Error!", 10, 10, FONT_SIZE);
    tft.drawString(error.c_str(), 10, 30, FONT_SIZE);
    return false;
  }

  predictions = doc["predictions"].as<JsonArray>();
  if (predictions.size() < 3) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Not enough data.", 10, 10, FONT_SIZE);
    return false;
  }
  return true;
}


/**
 * Analyzes the raw prediction data to find min/max and high/low tide events.
 */
void processTidePredictions() {
  minTide = 10.0;
  maxTide = -10.0;
  highTideEvents[0] = ""; highTideEvents[1] = "";
  lowTideEvents[0] = ""; lowTideEvents[1] = "";
  int highTideCount = 0, lowTideCount = 0;

  // Find min and max tide heights
  for (JsonObject p : predictions) {
    float tideHeight = p["v"].as<float>();
    if (tideHeight < minTide) minTide = tideHeight;
    if (tideHeight > maxTide) maxTide = tideHeight;
  }

  int lastTrend = 0;
  for (int i = 1; i < predictions.size(); i++) {
    float prevHeight = predictions[i - 1]["v"].as<float>();
    float currentHeight = predictions[i]["v"].as<float>();
    int currentTrend = (currentHeight > prevHeight) ? 1 : ((currentHeight < prevHeight) ? -1 : lastTrend);

    if (currentTrend != lastTrend && lastTrend != 0) {
      JsonObject peak = predictions[i - 1];
      int hour24 = peak["t"].as<String>().substring(11, 13).toInt();
      String minutes = peak["t"].as<String>().substring(14, 16);
      String suffix = (hour24 < 12) ? "am" : "pm";
      int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
      String displayTime = String(hour12) + ":" + minutes + suffix;
      String eventStr = displayTime + " (" + String(peak["v"].as<float>(), 1) + "ft)";

      if (lastTrend == 1 && highTideCount < 2) {
        highTideEvents[highTideCount++] = eventStr;
      } else if (lastTrend == -1 && lowTideCount < 2) {
        lowTideEvents[lowTideCount++] = eventStr;
      }
    }
    lastTrend = currentTrend;
  }
}


/**
 * Renders the complete tide chart UI on the TFT display.
 * @param timeinfo A struct containing the current time for the display.
 */
void drawTideChart(const struct tm& timeinfo) {
  tft.fillScreen(TFT_BLACK);
  
  char displayDate[20];
  strftime(displayDate, sizeof(displayDate), "%A, %b %d", &timeinfo);
  tft.setTextColor(TFT_YELLOW);
  tft.drawCentreString(displayDate, tft.width() / 2, 10, 4);

  tft.setTextColor(TFT_CYAN);
  tft.setTextFont(4);
  tft.drawString("High", 15, 45);
  tft.setTextFont(2);
  tft.drawString(highTideEvents[0], 95, 50);
  if (highTideEvents[1] != "") tft.drawString(highTideEvents[1], 205, 50);

  tft.setTextColor(TFT_MAGENTA);
  tft.setTextFont(4);
  tft.drawString("Low", 15, 80);
  tft.setTextFont(2);
  tft.drawString(lowTideEvents[0], 95, 85);
  if (lowTideEvents[1] != "") tft.drawString(lowTideEvents[1], 205, 85);

  const int graphX = 10, graphY = 115;
  const int graphW = tft.width() - 20, graphH = tft.height() - 125;
  tft.drawRect(graphX, graphY, graphW, graphH, TFT_DARKGREY);

  int lastX = -1, lastY = -1;
  for (JsonObject p : predictions) {
    String timeStr = p["t"].as<String>();
    int totalMinutes = timeStr.substring(11, 13).toInt() * 60 + timeStr.substring(14, 16).toInt();
    int x = map(totalMinutes, 0, 1440, graphX, graphX + graphW);
    int y = map(p["v"].as<float>(), minTide, maxTide, graphY + graphH, graphY);

    if (lastX != -1) tft.drawLine(lastX, lastY, x, y, TFT_SKYBLUE);
    lastX = x;
    lastY = y;
  }

  int currentMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int timeX = map(currentMinute, 0, 1440, graphX, graphX + graphW);
  tft.drawFastVLine(timeX, graphY, graphH, TFT_RED);
}
