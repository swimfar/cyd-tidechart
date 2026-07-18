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
//#include <Free_Fonts.h>  // Load GFX fonts

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
// const char* time_zone = "PST8PDT,M3.2.0,M11.1.0";  // Los Angeles (Pacific Time)
// Change to get the time zone settings from the station location.
const bool display24hour = true;  // set to true to display time in 24-hour format

const unsigned long UPDATE_INTERVAL = 15UL * 60UL * 1000UL; // 15 minutes (in ms)
const char *DATA_INTERVAL = "10";  // data interval (resolution) in minutes (5, 10, 15, 30 or 60)
const char *WIND_DATA_INTERVAL = "h";  // data interval (resolution) in minutes (5, 10, 15, 30 or h)

// ================================================================
// Screen Setup
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

// ================================================================
// Data Variables
// ================================================================
float stationLat = 360;  // NOAA station latitude and longitude (initialize to invalid value)
float stationLon = 360;
const char* stationName;  // NOAA station name based on station ID
JsonDocument doc;  // full JSON tide data stream

struct TideDatum {
  char time[24];  // Date string (e.g. 2026-07-10 06:12)
  float tideHeight;  // Tide height in ft or m
};
const int MAX_PREDICTIONS = 250;  // max number of predictions for a day
TideDatum tideData[MAX_PREDICTIONS];
int numTideData = 0;

struct WindDatum {
  char time[24];  // Date string (e.g. 2026-07-10 06:12)
  float windSpeed;  // Wind speed in knots or m/s
  float windDir;  // Wind direction in degrees
};
WindDatum windData[MAX_PREDICTIONS];
int numWindData = 0;

float minTide, maxTide;
String highTideEvents[2];
String lowTideEvents[2];
char windMaxAr[4][10];  // array for wind speed strings (e.g. 2.8 kts).
char windDirAr[4][5];  // array for wind direction strings (e.g. WNW).
int dawnMinutes = 0;
int duskMinutes = 0;

// ================================================================
// State Variables
// ================================================================
unsigned long lastUpdateTime = 0;
int dayOffset = 0;  // Used to get data for tomorrow or yesterday
int tideDataDayOfYear = -1;  // date for currently stored tide data
int windDataDayOfYear = -1;  // date for currently stored wind data
bool showWind = false;




void connectToWiFi();
void initTime();
void fetchAndDisplayTides();
bool getTidePredictions(const struct tm &timeinfo);
bool getWindPredictions(const struct tm &timeinfo);
void processTidePredictions();
void processWindPredictions();
const char* getWindDirection(float degrees);
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
        showWind = false;
      } else if (touch_x > 260) {
        Serial.println("Add a day");
        dayOffset += 1;
        showWind = false;
      } else {
        dayOffset = 0;  // Reset the offset
      }
    } else if (touch_y < 100) {
      Serial.println("Changing between wind and tide reports.");
      showWind = !showWind;
      if (dayOffset > 0) showWind = false;  // only historical data available
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
  configTime(0, 0, ntpServer);  // no hard-coded timezone offsets
  setenv("TZ", time_zone, 1);  // set the time zone settings
  tzset();
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("\nFailed to obtain time from NTP server!");
    tft.drawCentreString("Time Sync Failed!", SCREEN_WIDTH / 2, 4 * STARTUP_FONT_PXL, STARTUP_FONT_SIZE);
    return;
  }
  Serial.println("\nTime synchronized.");
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

  if (offsetTimeInfo.tm_yday != tideDataDayOfYear) {
    Serial.println("New day detected. Fetching fresh tide data...");
    tft.fillScreen(TFT_BLACK); // Clear screen to show status
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Fetching new tide data...", tft.width()/2, tft.height()/2, STARTUP_FONT_SIZE);

    if (getTidePredictions(offsetTimeInfo)) {
      processTidePredictions();
      tideDataDayOfYear = offsetTimeInfo.tm_yday;
      Serial.println("Successfully fetched and processed new data.");
    } else {
      Serial.println("Failed to fetch new tide data. Will retry later.");
      delay(1000);
      drawTideChart(offsetTimeInfo);
      return;  // maybe add status icons for these kinds of things.
    }
  } else {
    Serial.println("Tide data for today already cached. Redrawing chart.");
  }

  if ((offsetTimeInfo.tm_yday != windDataDayOfYear) && showWind==true) {
    Serial.println("Fetching fresh wind data...");
    tft.fillScreen(TFT_BLACK); // Clear screen to show status
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Fetching new wind data...", tft.width()/2, tft.height()/2, STARTUP_FONT_SIZE);

    if (getWindPredictions(offsetTimeInfo)) {
      processWindPredictions();
      windDataDayOfYear = offsetTimeInfo.tm_yday;
      Serial.println("Successfully fetched and processed new wind data.");
    } else {
      Serial.println("Failed to fetch new wind data. Will retry later.");
      delay(1000);
      drawTideChart(offsetTimeInfo);
      return;  // maybe add status icons for these kinds of things.
    }
  } else {
    Serial.println("Wind data for today already cached. Redrawing chart.");
  }
  drawTideChart(offsetTimeInfo);
}


/**
 * Fetches and parses tide prediction data from the NOAA API.
 * @return True on success, false on failure.
 */
bool getTidePredictions(const struct tm &timeinfo) {
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  const char* product = "predictions";

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=" + String(product) + "&application=NOAA.TidesAndCurrents&station=" +
               String(stationId) + "&begin_date=" + String(dateBuffer) +
               "&end_date=" + String(dateBuffer) +
               "&datum=MLLW&units=english&time_zone=lst&format=json&interval=" + String(DATA_INTERVAL);

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

  JsonArray predictions;  // parsed JSON data array (pointer to doc)
  if (product=="predictions") {
    predictions = doc["predictions"].as<JsonArray>();
  } else if (product=="water_level") {
    predictions = doc["data"].as<JsonArray>();  // different data format than predictions
  }
  if (predictions.size() < 3) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Not enough tide data.", 10, 10, STARTUP_FONT_SIZE);
    Serial.println(predictions.size());
    return false;
  }
  Serial.print("Tide data array size: ");
  Serial.println(predictions.size());

  // Load JSON data into data struct
  numTideData = 0;
  for (JsonObject prediction: predictions) {
    if (numTideData >= MAX_PREDICTIONS) break;  // array is full
    const char* jsonTimeStr = prediction["t"];
    if (jsonTimeStr != nullptr) {
      strlcpy(tideData[numTideData].time, jsonTimeStr, sizeof(tideData[0].time));
    } else {
      // If time key is missing or null,
      tideData[numTideData].time[0] = '\0';
    }

    tideData[numTideData].tideHeight = prediction["v"].as<float>();
    numTideData++;
  }

  // 1. Get the current free RAM right now
  size_t freeHeapNow = ESP.getFreeHeap();
  // 2. Get the lowest RAM point reached since bootup (Crucial for debugging crashes)
  size_t lowestHeapEver = ESP.getMinFreeHeap();
  
  Serial.printf("\n--- ESP32 MEMORY REPORT ---\n");
  Serial.printf("Current Free Heap: %u bytes\n", freeHeapNow);
  Serial.printf("Lowest Historic Free Heap: %u bytes\n", lowestHeapEver);
  Serial.printf("---------------------------\n\n");
  return true;
}


/**
 * Fetches and parses tide prediction data from the NOAA API.
 * @return True on success, false on failure.
 */
bool getWindPredictions(const struct tm &timeinfo) {
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  const char* product = "wind";
  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=" + String(product) + "&application=NOAA.TidesAndCurrents&station=" +
               String(stationId) + "&begin_date=" + String(dateBuffer) +
               "&end_date=" + String(dateBuffer) +
               "&datum=MLLW&units=english&time_zone=lst&format=json&interval=" + String(WIND_DATA_INTERVAL);

  Serial.println(product);
  Serial.println(url);
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
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
  JsonDocument filter;
  filter["data"][0]["t"] = true;  // keep the time data
  filter["data"][0]["s"] = true;  // keep the speed data
  filter["data"][0]["d"] = true;  // keep the direction data
  doc.clear(); // Clear previous JSON data
  DeserializationError error = deserializeJson(
    doc,
    http.getStream(),
    DeserializationOption::Filter(filter));
  http.end();

  if (error) {
    tft.fillScreen(TFT_RED);
    tft.drawString("JSON Error!", 10, 10, STARTUP_FONT_SIZE);
    tft.drawString(error.c_str(), 10, 30, STARTUP_FONT_SIZE);
    return false;
  }

  JsonArray predictions;  // parsed JSON data array (pointer to doc)
  predictions = doc["data"].as<JsonArray>();  // different data format than predictions
  if (predictions.size() < 3) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Not wind enough data.", 10, 10, STARTUP_FONT_SIZE);
    Serial.println(predictions.size());
    return false;
  }
  Serial.print("Wind data array size: ");
  Serial.println(predictions.size());

  // Load JSON data into data struct
  numWindData = 0;
  for (JsonObject prediction : predictions) {
    if (numWindData >= MAX_PREDICTIONS) break;  // array if full
    const char* jsonTimeStr = prediction["t"];  // time string
    if (jsonTimeStr != nullptr) {
      strlcpy(windData[numWindData].time, jsonTimeStr, sizeof(windData[numWindData].time));
    } else {  // If time key is missing or null,
      continue;  // Skip this data point
    }

    windData[numWindData].windSpeed = prediction["s"].as<float>();
    windData[numWindData].windDir = prediction["d"].as<float>();
    numWindData++;
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
    for (int i = 0; i < numTideData; i++) {
    float tideHeight = tideData[i].tideHeight;
    if (tideHeight < minTide) minTide = tideHeight;
    if (tideHeight > maxTide) maxTide = tideHeight;
  }

  int lastTrend = 0;
  for (int i = 1; i < numTideData; i++) {
    float prevHeight = tideData[i - 1].tideHeight;
    float currentHeight = tideData[i].tideHeight;
    int currentTrend = (currentHeight > prevHeight) ? 1 : ((currentHeight < prevHeight) ? -1 : lastTrend);

    // Check if the tide is changing direction
    if (currentTrend != lastTrend && lastTrend != 0) {
      String displayTime;
      TideDatum peak = tideData[i - 1];  // get time and tide height for this moment

      int hour24 = atoi(&tideData[i].time[11]);  // extract hour (reads 11 until non-numeric)
      int minute = atoi(&tideData[i].time[14]);  // extract minute

      char timeDisplay[16];
      char eventStr[32];
      
      if (display24hour == false){
        const char* suffix = (hour24 < 12) ? "am" : "pm";
        int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
        snprintf(timeDisplay, sizeof(timeDisplay), "%d;%02d%s", hour12, minute, suffix);
      } else {
        snprintf(timeDisplay, sizeof(timeDisplay), "%d;%02d%s", hour24, minute);
      }
      snprintf(eventStr, sizeof(eventStr), "%s (%.1fft)", timeDisplay, tideData[i].tideHeight);


      if (lastTrend == 1 && highTideCount < 2) {
        highTideEvents[highTideCount++] = eventStr;
      } else if (lastTrend == -1 && lowTideCount < 2) {
        lowTideEvents[lowTideCount++] = eventStr;
      }
    }
    lastTrend = currentTrend;
  }
}


const char* getWindDirection(float degrees) {
  // Array of 16 possible directions
  static const char* directions[] = {
    "N", "NNE", "NE", "ENE",
    "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW",
    "W", "WNW", "NW", "NNW"
  };

  // Keep angle between 0 and 360
  float safeDegree = fmod(degrees, 360.0f);
  if (safeDegree < 0) safeDegree += 360.0f;

  // Calculate 16-point index (22.5 degrees per step)
  int index = round(safeDegree / 22.5f);
  index = index % 16;  // Wrap 16 (350-360) back to 0 (N)
  return directions[index];
}


/**
 * Analyzes the raw prediction data to generate wind speed and direction strings.
 */
void processWindPredictions() {
  float maxWind[] = {0.0f, 0.0f, 0.0f, 0.0f};  // max. wind speed during each 6-hour interval
  float windAngleSum[] = {0.0f, 0.0f, 0.0f, 0.0f};  // running sum of wind direction angles
  int windAngleCount[] = {0, 0, 0, 0};  // number of wind directions added to count

  Serial.println("Processing wind data");
  Serial.println(numWindData);
  // Find max wind speed for each 6-hour interval
  for (int i = 0; i < numWindData; i++) {
    if (windData[i].time[0] == '\0') continue;  // This time wasn't parsed
    int hour24 = atoi(&windData[i].time[11]);  // extract hour (reads 11 until non-numeric)
    float windSpeed = windData[i].windSpeed;
    float windAngle = windData[i].windDir;
    int intervalIdx;
    if (hour24 >= 0 && hour24 < 6) intervalIdx = 0;
    else if (hour24 < 12) intervalIdx = 1;
    else if (hour24 < 18)  intervalIdx = 2;
    else if (hour24 < 24) intervalIdx = 3;
    else {
      Serial.print("Invalid hour24 in processWindPredictions(): ");
      Serial.println(hour24);
      continue;
    }
    if (windSpeed > maxWind[intervalIdx]) maxWind[intervalIdx] = windSpeed;
    windAngleSum[intervalIdx] += windAngle;
    windAngleCount[intervalIdx] ++;
  }

  // Save data to global string arrays
  for (int i = 0; i < 4; i++) {
    Serial.println(maxWind[i]);
    Serial.println(windMaxAr[i]);
    // Convert wind values to strings
    snprintf(windMaxAr[i], sizeof(windMaxAr[i]), "%.1f kts", maxWind[i]);

    // Calculate average wind dir. and convert to string
    float windAngle = windAngleSum[i] / windAngleCount[i];
    snprintf(windDirAr[i], sizeof(windDirAr[i]), "%s", getWindDirection(windAngle));
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
  
  // ================================================================
  // Draw Date and Tide Levels
  // ================================================================
  // Draw date change arrows (could this be done near touch screen code?)  None of this works
  tft.setFreeFont(&FreeSans9pt7b);  // requires LOAD_FTXFF in User_Setup.h
  tft.setCursor(10, 10);
  tft.print("\x1B");  // left arrow
  tft.setCursor(tft.width() - 20, 10);
  tft.print("\x1A");  // right arrow
  tft.setFreeFont(NULL);
  
  char displayDate[20];
  strftime(displayDate, sizeof(displayDate), "%A, %b %d", &timeinfo);  // create date string
  tft.setTextColor(TFT_WHITE);
  tft.drawCentreString(displayDate, tft.width() / 2, 10, font_size_date);

    if (showWind == true) {
      tft.drawCentreString("Max. Wind Speed", 160, 45, 4);
      tft.drawCentreString(windMaxAr[0], 47, 72, 2);
      tft.drawCentreString(windDirAr[0], 47, 92, 2);
      tft.drawCentreString(windMaxAr[1], 122, 72, 2);
      tft.drawCentreString(windDirAr[1], 122, 92, 2);
      tft.drawCentreString(windMaxAr[2], 198, 72, 2);
      tft.drawCentreString(windDirAr[2], 198, 92, 2);
      tft.drawCentreString(windMaxAr[3], 273, 72, 2);
      tft.drawCentreString(windDirAr[3], 273, 92, 2);
    } else {  // Show the tide values
      tft.setTextFont(4);
      tft.drawString("High", 15, 45);
      tft.setTextFont(2);
      tft.drawString(highTideEvents[0], 95, 50);
      if (highTideEvents[1] != "") tft.drawString(highTideEvents[1], 205, 50);
    
      tft.setTextFont(4);
      tft.drawString("Low", 15, 80);
      tft.setTextFont(2);
      tft.drawString(lowTideEvents[0], 95, 85);
      if (lowTideEvents[1] != "") tft.drawString(lowTideEvents[1], 205, 85);
  }
  // ================================================================
  // Draw the Tide Graph
  // ================================================================
  const int graphX = 10, graphY = 115;  // top left coordinates of graph
  const int graphW = tft.width() - 20, graphH = tft.height() - 125;

  tft.fillRect(graphX, graphY, graphW, graphH, TFT_SKYBLUE);  // background daytime color

  if (dawnMinutes > 0) {
    int dawnW = int((dawnMinutes / 1440.0) * graphW);  // pixel width of midnight to dawn
    tft.fillRect(graphX, graphY, dawnW, graphH, TFT_DARKGREY);
  }
  if (duskMinutes > 0) {
    int duskW = int(((1440 - duskMinutes) / 1440.0) * graphW);  // pixel width of dusk to midnight
    int duskX = graphX + (graphW - duskW);
    tft.fillRect(duskX, graphY, duskW, graphH, TFT_DARKGREY);
  }

  int graphBotCoord = graphY + graphH;
  int lastX = -1, lastY = -1;
  for (int i = 0; i < numTideData; i++) {
    float tide = tideData[i].tideHeight;
    int hour24 = atoi(&tideData[i].time[11]);  // extract hour (reads 11 until non-numeric)
    int minutes = atoi(&tideData[i].time[14]);  // extract minute
    int totalMinutes = hour24 * 60 + minutes;
    float tideRange = maxTide - minTide;
    int x = map(totalMinutes, 0, 1440, graphX, graphX + graphW);
    int y = graphBotCoord - (int)round(((tide - minTide) / (tideRange)) * graphH);

    if (lastX != -1) {  // if this isn't the first point.
      int dx = x - lastX;
      int lowLimitY = graphBotCoord - (int)round(((lowLimit - minTide) / (tideRange)) * graphH);
      if (lastY < lowLimitY || x < lowLimitY){
        float lowLimitH = (int)round(((lowLimit - minTide) / (tideRange)) * graphH);
        tft.fillRect(lastX, lowLimitY, dx, lowLimitH, TFT_RED);
      }
      // Fill below the line with a different color (remember y coord is from the top)
      if (y < lastY){  // Rising tide (upward line slope)
        tft.fillTriangle(lastX, lastY, x, y, x, lastY, TFT_BLUE);
        int fillHeight = graphBotCoord - lastY;
        tft.fillRect(lastX, lastY, dx, fillHeight, TFT_BLUE);
      } else if (y > lastY) {  // Dropping tide (downward line slope)
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
//  DynamicJsonDocument doc(8192);  // full JSON tide data stream
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  const char* product = "water_level";
  JsonDocument filter;
  filter["metadata"] = true;  // only keep the metadat

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
  DeserializationError error = deserializeJson(
    doc,
    http.getStream(),
    DeserializationOption::Filter(filter));
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

  JsonObject stationData;  // for Latitude and Longitude values
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