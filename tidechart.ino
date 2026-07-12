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

#include <SolarCalculator.h>  // for dusk/dawn times

// CYD Libraries
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#define TFT_SLATE_BLUE  0x951B  // graph background (daytime)

// Do some compilation-time library checks
#if SPI_FREQUENCY > 27000000
  Serial.println("Warning: SPI freq. is greater than 27 MHz.  This can cause screen issues");
#endif

// ================================================================
// User Settings
// ================================================================
// Replace with your WiFi network credentials
const char* ssid = "ssid";
const char* password = "password";

// You can find station IDs at: https://tidesandcurrents.noaa.gov/stations.html
const char* stationId = "9415144";  // Chicago
const float lowLimit = -1.0;  // Low tide limit (show red below this value)

const char* ntpServer = "pool.ntp.org"; // NTP server for time synchronization
//const char* time_zone = "EST5EDT,M3.2.0,M11.1.0";  // New York (Eastern Time)
const char* time_zone = "CST6CDT,M3.2.0,M11.1.0";  // Chicago (Central Time)
//const char* time_zone = "MST7MDT,M3.2.0,M11.1.0";  // Denver (Mountain Time)
//const char* time_zone = "PST8PDT,M3.2.0,M11.1.0";  // Los Angeles (Pacific Time)
const bool display24hour = true;  // set to true to display time in 24-hour format
// ================================================================
// ================================================================

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
#define STARTUP_FONT_SIZE 4  // 26 px font
#define STARTUP_FONT_PXL 26

unsigned long lastUpdateTime = 0;
const unsigned long UPDATE_INTERVAL = 15UL * 60UL * 1000UL; // 15 minutes (in ms)

float stationLat = 360;  // NOAA station latitude and longitude (initialize to invalid value)
float stationLon = 360;
const char* stationName;  // NOAA station name based on station ID
DynamicJsonDocument doc(8192);  // full JSON tide data stream
JsonArray predictions;  // parsed JSON tide data array
JsonObject stationData;  // for Latitude and Longitude values
float minTide, maxTide;
String highTideEvents[2];
String lowTideEvents[2];
int dataDayOfYear = -1;
int dawnMinutes = 0;
int duskMinutes = 0;
int dayOffset = 0;  // Used to get data for tomorrow or yesterday

void connectToWiFi();
void initTime();
void fetchAndDisplayTides();
bool getTidePredictions(const struct tm &timeinfo);
void processTidePredictions();
void drawTideChart(const struct tm &timeinfo);
bool getLocalDawnDusk(double lat, double lon);
bool getStationLatLon();


void setup() {
  Serial.begin(115200);

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
  //pinMode(TFT_BL, OUTPUT);
  //digitalWrite(TFT_BL, HIGH);  // explicitly turn on the backlight
  
  // Set X and Y coordinates for center of display
  int centerX = SCREEN_WIDTH / 2;
  int centerY = SCREEN_HEIGHT / 2;

  tft.setTextColor(TFT_BLACK, TFT_BLUE);
  connectToWiFi();
  initTime();  // get current time from NTP
  delay(1000);

  if (!getStationLatLon()) {
    Serial.println("Couldn't get station lat., lon.");
  }
  else {
    if (!getLocalDawnDusk(stationLat, stationLon)) {
      Serial.println("Couldn't get local dusk, dawn times.");
    }
  }
  fetchAndDisplayTides();
}


void loop() {
  unsigned long currentMillis = millis();

  // 15-minute update interval for display
  if (currentMillis - lastUpdateTime >= UPDATE_INTERVAL) {
    fetchAndDisplayTides();
    lastUpdateTime = currentMillis;
  }

  // Check for touch-screen touch
  uint16_t t_x = 0, t_y = 0;
  if (touchscreen.tirqTouched() && touchscreen.touched()){
    Serial.println("Screen touched.");
    TS_Point p = touchscreen.getPoint();
    int touch_x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    int touch_y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    Serial.print("X: ");
    Serial.print(touch_x);
    Serial.print("  Y: ");
    Serial.println(touch_y);
    if (touch_y < 45) {
      Serial.println("Top of screen touched.");
      if (touch_x < 60) {
        Serial.println("Subtract a day");
        dayOffset -= 1;
      } else if (touch_x > 260) {
        Serial.println("Add a day");
        dayOffset += 1;
      } else {
        dayOffset = 0;  // Reset the offset
      }
    }
 
    fetchAndDisplayTides();  // update tides
    lastUpdateTime = currentMillis;

    delay(500);  // debounce delay to avoid a double-trigger
  }
}


void connectToWiFi() {
  Serial.println("Connecting to WiFi...");
  tft.drawCentreString("Connecting to WiFi...", SCREEN_WIDTH / 2, 1 * STARTUP_FONT_PXL, STARTUP_FONT_SIZE);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    tft.drawString(".", 10 + (attempts * 10), 2 * (STARTUP_FONT_PXL + 2), STARTUP_FONT_SIZE);
    attempts++;
  }
  Serial.println();
  Serial.println(WiFi.status());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    tft.drawCentreString("WiFi Connected!", SCREEN_WIDTH / 2, 3 * (STARTUP_FONT_PXL + 2), STARTUP_FONT_SIZE);
  } else {
    Serial.println("\nFailed to connect to WiFi!");
    tft.fillScreen(TFT_RED);
    tft.drawCentreString("WiFi Failed!", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, STARTUP_FONT_SIZE);
    while (true) delay(1000);
  }
}

/**
 * Test connection to NTP time server.
 */
void initTime() {
  Serial.print("Synchronizing time with NTP...");
//  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  configTime(0, 0, ntpServer);  // no hard-coded timezone offsets
  setenv("TZ", time_zone, 1);  // set the time zone settings
  tzset();
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("\nFailed to obtain time from NTP server!");
//    tft.fillRect(0, 50, tft.width(), 30, TFT_RED);
    tft.drawCentreString("Time Sync Failed!", SCREEN_WIDTH / 2, 4 * STARTUP_FONT_PXL, STARTUP_FONT_SIZE);
    return;
  }
  Serial.println("\nTime synchronized.");
//  tft.fillRect(0, 50, tft.width(), 40, TFT_BLACK);
  tft.drawCentreString("Time Synced!", SCREEN_WIDTH / 2, 4 * (STARTUP_FONT_PXL + 2), STARTUP_FONT_SIZE);
}


void fetchAndDisplayTides() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Failed to get time!", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, STARTUP_FONT_SIZE);
    return;  // This should fail more gracefully instead of blanking the screen. Don't need time to display chart
  }

  // Calculate offset time (for looking at different days predictions)
  time_t epoch_time = mktime(&timeinfo);  // convert to epoch time to avoid year rollovers
  epoch_time += dayOffset * 86400;
  struct tm offsetTimeInfo;
  localtime_r(&epoch_time, &offsetTimeInfo);  // convert back to tm struct

//  if (timeinfo.tm_yday != dataDayOfYear) {
  if (offsetTimeInfo.tm_yday != dataDayOfYear) {
    Serial.println("New day detected. Fetching fresh tide data...");
    tft.fillScreen(TFT_BLACK); // Clear screen to show status
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Fetching new tide data...", tft.width()/2, tft.height()/2, STARTUP_FONT_SIZE);

    if (getTidePredictions(offsetTimeInfo)) {
      processTidePredictions();
      dataDayOfYear = offsetTimeInfo.tm_yday;
      Serial.println("Successfully fetched and processed new data.");
    } else {
      Serial.println("Failed to fetch new tide data. Will retry later.");
      return;  // maybe add status icons for these kinds of things.
    }
  } else {
    Serial.println("Data for today already cached. Redrawing chart.");
  }

//  drawTideChart(timeinfo);
  drawTideChart(offsetTimeInfo);
}


/**
 * Fetches and parses tide prediction data from the NOAA API.
 * @return True on success, false on failure.
 */
bool getTidePredictions(const struct tm &timeinfo) {
//  struct tm timeinfo;
//  getLocalTime(&timeinfo);
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  const char* product = "predictions";
//  const char* product = "water_level";

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=" + String(product) + "&application=NOAA.TidesAndCurrents&station=" +
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
    tft.drawString("Couldn't get NOAA data.", 10, (STARTUP_FONT_PXL + 2) * 1, STARTUP_FONT_SIZE);
    tft.drawString("HTTP Error: " + String(httpCode), 10, (STARTUP_FONT_PXL + 2) * 2, STARTUP_FONT_SIZE);
    http.end();
    return false;
  }
  doc.clear(); // Clear previous JSON data
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    tft.fillScreen(TFT_RED);
    tft.drawString("JSON Error!", 10, 10, STARTUP_FONT_SIZE);
    tft.drawString(error.c_str(), 10, 30, STARTUP_FONT_SIZE);
    return false;
  }

  if (product=="predictions") {
    predictions = doc["predictions"].as<JsonArray>();
  } else if (product=="water_level") {
    predictions = doc["data"].as<JsonArray>();  // different data format than predictions
  }
  if (predictions.size() < 3) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Not enough data.", 10, 10, STARTUP_FONT_SIZE);
    return false;
  }
  Serial.print("Data array size: ");
  Serial.println(predictions.size());
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
      String displayTime;
      JsonObject peak = predictions[i - 1];  // get time and tide height for this moment
      int hour24 = peak["t"].as<String>().substring(11, 13).toInt();  // extract hour
      String minutes = peak["t"].as<String>().substring(14, 16);  // extract minute
      if (display24hour == false){
        String suffix = (hour24 < 12) ? "am" : "pm";
        int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
        displayTime = String(hour12) + ":" + minutes + suffix;
      } else {
        displayTime = String(hour24) + ":" + minutes;
      }
      // Todo: add option for 24 hour format
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
 * @param timeinfo A struct containing the date to display at the top.
 */
void drawTideChart(const struct tm &timeinfo) {
  const uint16_t color_bgd = TFT_BLACK;
  const int font_size_date = 4;
  tft.fillScreen(color_bgd);
  
  char displayDate[20];
  strftime(displayDate, sizeof(displayDate), "%A, %b %d", &timeinfo);  // create date string
  tft.setTextColor(TFT_WHITE);
  tft.drawCentreString(displayDate, tft.width() / 2, 10, font_size_date);

  tft.setTextColor(TFT_WHITE);
  tft.setTextFont(4);
  tft.drawString("High", 15, 45);
  tft.setTextFont(2);
  tft.drawString(highTideEvents[0], 95, 50);
  if (highTideEvents[1] != "") tft.drawString(highTideEvents[1], 205, 50);

  tft.setTextColor(TFT_WHITE);
  tft.setTextFont(4);
  tft.drawString("Low", 15, 80);
  tft.setTextFont(2);
  tft.drawString(lowTideEvents[0], 95, 85);
  if (lowTideEvents[1] != "") tft.drawString(lowTideEvents[1], 205, 85);

  // ================================================================
  // Draw the Tide Graph
  // ================================================================
//  const int graph_w = 20;
//  const int graph_h = 125;
  const int graphX = 10, graphY = 115;  // top left coordinates of graph
  const int graphW = tft.width() - 20, graphH = tft.height() - 125;

  tft.fillRect(graphX, graphY, graphW, graphH, TFT_SKYBLUE);  // background daytime color
//  int noon_coord_x = graphX + graphW / 2;
//  int six_coord_x = graphX + graphW / 4;
//  int eighteen_coord_x = graphX + 3 * graphW / 4;
//  tft.drawFastVLine(noon_coord_x, graphY, graphH, TFT_DARKGREY);  // line at noon
//  tft.drawFastVLine(six_coord_x, graphY, 4, TFT_DARKGREY);  // upper tick at 6am
//  tft.drawFastVLine(six_coord_x, graphY + graphH - 4, 4, TFT_DARKGREY);  // lower tick at 6am
//  tft.drawFastVLine(eighteen_coord_x, graphY, 4, TFT_DARKGREY);  // upper tick at 6pm
//  tft.drawFastVLine(eighteen_coord_x, graphY + graphH - 4, 4, TFT_DARKGREY);  // lower tick at 6pm

  if (dawnMinutes > 0) {
    int dawnW = int((dawnMinutes / 1440.0) * graphW);  // pixel width of midnight to dawn
    tft.fillRect(graphX, graphY, dawnW, graphH, TFT_DARKGREY);
    Serial.println("Dawn Minutes and pixel width: ");
    Serial.println(dawnMinutes);
    Serial.println(dawnW);
  }
  if (duskMinutes > 0) {
    int duskW = int(((1440 - duskMinutes) / 1440.0) * graphW);  // pixel width of dusk to midnight
    int duskX = graphX + (graphW - duskW);
    tft.fillRect(duskX, graphY, duskW, graphH, TFT_DARKGREY);
    Serial.println("Dusk Minutes and pixel width: ");
    Serial.println(duskMinutes);
    Serial.println(duskW);
  }
//  tft.drawRect(graphX - 1, graphY - 1, graphW + 2, graphH + 2, TFT_LIGHTGREY);  // Border

  int graphBotCoord = graphY + graphH;
  int lastX = -1, lastY = -1;
  for (JsonObject p : predictions) {
    String timeStr = p["t"].as<String>();
    int totalMinutes = timeStr.substring(11, 13).toInt() * 60 + timeStr.substring(14, 16).toInt();
    int x = map(totalMinutes, 0, 1440, graphX, graphX + graphW);
    int y = graphBotCoord - (int)round(((p["v"].as<float>() - minTide) / (maxTide - minTide)) * graphH);

    if (lastX != -1) {
      int dx = x - lastX;
      if (p["v"].as<float>() < lowLimit){
        float limitH = (int)round(((lowLimit - minTide) / (maxTide - minTide)) * graphH);
        float limitY = graphBotCoord - limitH;  // top coordinate of limit
        tft.fillRect(lastX, limitY, dx, limitH, TFT_RED);
      }
      // Fill below the line with a different color
      if (y > lastY){  // Rising tide (upward line slope)
        tft.fillTriangle(lastX, lastY, x, y, x, lastY, TFT_BLUE);
        int fillHeight = graphBotCoord - lastY;
        tft.fillRect(lastX, lastY, dx, fillHeight, TFT_BLUE);
      } else if (y < lastY) {  // Dropping tide (downward line slope)
        tft.fillTriangle(lastX, lastY, x, y, lastX, y, TFT_BLUE);
        int fillHeight = graphBotCoord - y;
        tft.fillRect(lastX, y, dx, fillHeight, TFT_BLUE);        
      } else {
        int fillHeight = graphBotCoord - y;
        tft.fillRect(lastX, lastY, dx, fillHeight, TFT_BLUE);
      }
      tft.drawLine(lastX, lastY, x, y, TFT_BLACK);
      tft.drawLine(lastX, lastY+1, x, y+1, TFT_BLACK);  // make the line thicker
    }
    lastX = x;
    lastY = y;
  }

  // Draw hour markers
  int noon_coord_x = graphX + graphW / 2;
  int six_coord_x = graphX + graphW / 4;
  int eighteen_coord_x = graphX + 3 * graphW / 4;
  tft.drawFastVLine(noon_coord_x, graphY, graphH, TFT_DARKGREY);  // line at noon
  tft.drawFastVLine(six_coord_x, graphY, 4, TFT_DARKGREY);  // upper tick at 6am
  tft.drawFastVLine(six_coord_x, graphY + graphH - 4, 4, TFT_DARKGREY);  // lower tick at 6am
  tft.drawFastVLine(eighteen_coord_x, graphY, 4, TFT_DARKGREY);  // upper tick at 6pm
  tft.drawFastVLine(eighteen_coord_x, graphY + graphH - 4, 4, TFT_DARKGREY);  // lower tick at 6pm
  tft.drawRect(graphX - 1, graphY - 1, graphW + 2, graphH + 2, TFT_LIGHTGREY);  // Border

  // Draw red line at current time
  if (dayOffset == 0) {
    int currentMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int timeX = map(currentMinute, 0, 1440, graphX, graphX + graphW);
    tft.drawFastVLine(timeX, graphY, graphH, TFT_RED);
  }
}


/**
 * Calculates local dawn and dusk times using getLocalTime().
 * Returns true if successful, false if NTP time is not yet available.
 * Modifies the global dawnMinutes and duskMinutes variables.
 * Does not modify the variables if unsuccessful.
 */
bool getLocalDawnDusk(double lat, double lon) {
  if (lat > 90.0 || lat < -90 || lon > 180.0 || lon < -180) {
    Serial.println("Invalid latitude or longitude values received.");
    return false;  // invalid values
  }
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    Serial.println("Couldn't sync with NPT while getting dusk/dawn.");
    return false; // Time not synced or available yet
  }

  // 1. Extract clean date integers directly from timeinfo
  int year  = timeinfo.tm_year + 1900;
  int month = timeinfo.tm_mon + 1;
  int day   = timeinfo.tm_mday;

  // 2. Fetch the automatic UTC timezone offset (in hours) via local vs UTC system time
  time_t local_epoch = mktime(&timeinfo);
  struct tm *utc_timeinfo = gmtime(&local_epoch);
  utc_timeinfo->tm_isdst = -1; 
  time_t utc_epoch = mktime(utc_timeinfo);
  double tz_offset = (double)(local_epoch - utc_epoch) / 3600.0;

  // 3. Calculate dusk/dawn (Library accepts date integers, returns UTC decimal hours)
  double transit, dawnDecimal, duskDecimal;
  calcCivilDawnDusk(year, month, day, lat, lon, transit, dawnDecimal, duskDecimal);

  // 5. Apply the local timezone offset
  dawnDecimal += tz_offset;
  duskDecimal += tz_offset;

  // 6. Handle 24-hour wrap-arounds
  if (dawnDecimal < 0)  dawnDecimal += 24; 
  if (dawnDecimal >= 24) dawnDecimal -= 24;
  if (duskDecimal < 0)  duskDecimal += 24; 
  if (duskDecimal >= 24) duskDecimal -= 24;

  // 7. Convert decimal hours to minutes
  dawnMinutes = (int)(dawnDecimal * 60);
  duskMinutes = (int)(duskDecimal * 60);

  return true;
}


bool getStationLatLon() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  const char* product = "water_level";

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=" + String(product) + "&application=NOAA.TidesAndCurrents&station=" +
               String(stationId) + "&begin_date=" + String(dateBuffer) +
               "&end_date=" + String(dateBuffer) +
               "&datum=MLLW&units=english&time_zone=lst&format=json";

  Serial.println(url);
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.println("httpCode not OK");
    stationLat = 360;  // set to an invalid number
    stationLon = 360;
    return false;
  }
  doc.clear(); // Clear previous JSON data
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    Serial.println("JSON Error");
    tft.fillScreen(TFT_RED);
    tft.drawString("JSON Error!", 10, 10, STARTUP_FONT_SIZE);
    tft.drawString(error.c_str(), 10, 30, STARTUP_FONT_SIZE);
    stationLat = 360;  // set to an invalid number
    stationLon = 360;
    return false;
  }

  stationData = doc["metadata"].as<JsonObject>();
  if (stationData.isNull() || stationData.size() < 4) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Couldn't get station data.", 10, 10, STARTUP_FONT_SIZE);
    stationLat = 360;  // set to an invalid number
    stationLon = 360;
    return false;
  }
  stationLat = stationData["lat"].as<float>();
  stationLon = stationData["lon"].as<float>();
  Serial.println("Station Latitude and Longitude:");
  Serial.print(stationLat);
  Serial.print(", ");
  Serial.println(stationLon);
  return true;
}
