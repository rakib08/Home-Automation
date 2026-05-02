/*
=========================================================
 Smart Home Automation System
 ESP8266 Based Controller

 Features implemented so far:
 - Cooling Fan Control (Relay)
 - 14 Time Slots with Scheduler
 - Weather Based Temperature Control
 - Web Interface
 - 433MHz Motor Control
 - EEPROM Configuration Storage
 - NTP Time Synchronization
 - Automatic WiFi Reconnection
 - Manual Weather Update
=========================================================
*/



/* ======================================================
   Libraries
====================================================== */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <RCSwitch.h>


/* ======================================================
   Hardware Pin Configuration
====================================================== */

#define RELAY_PIN D1          // Relay controlling cooling fan
#define RF_PIN D2             // 433MHz RF transmitter
#define CHICKEN_LIGHT_PIN D6  // Relay controlling chicken house light


/* ======================================================
   System Constants
====================================================== */

#define EEPROM_SIZE 512  // EEPROM storage size
#define SLOT_COUNT 14    // Maximum programmable time slots


/* ======================================================
   WiFi Credentials
====================================================== */

const char* WIFI_PASS = "@162Blueberry";
String ssid = "Dynamic";


/* ======================================================
   Weather API Configuration
====================================================== */

const char* WEATHER_KEY = "4562aa3f4e8ca6f027286708c2cb6a84";
const char* CITY = "Saidpur";
const char* COUNTRY = "BD";


/* ======================================================
   Core System Objects
====================================================== */

ESP8266WebServer server(6843);  // Web server running on port 6843

WiFiUDP ntpUDP;                                       // UDP object for NTP
NTPClient timeClient(ntpUDP, "pool.ntp.org", 21600);  // GMT+6

RCSwitch rf = RCSwitch();  // RF transmitter object


/* ======================================================
   Data Structures
====================================================== */

/* Time Slot Structure
   Defines one automation slot
*/
struct Slot {
  int sh;   // start hour
  int sm;   // start minute
  int eh;   // end hour
  int em;   // end minute
  bool en;  // slot enabled
};


/* System Configuration Structure
   Stored inside EEPROM
*/
struct Config {

  uint16_t marker;  // used to detect valid EEPROM block

  bool autoMode;    // auto / manual mode
  float threshold;  // temperature threshold
  bool lastState;   // last fan state

  // Chicken house light settings
  bool chickenAutoMode;
  bool chickenState;
  float chickenThreshold;

  Slot slots[SLOT_COUNT];  // time slot list
};

Config cfg;


/* ======================================================
   Runtime Variables
====================================================== */

float currentTemp = 0;  // current weather temperature
bool fanState = false;  // current relay state

/* ======================================================
   Chicken House Light Variables
====================================================== */

bool chickenLightState = false;  // Current light state (ON/OFF)
bool chickenAutoMode = true;     // Auto or manual mode

float chickenSummerThreshold = 18.0;  // Temperature threshold for summer
int lastChickenCheckHour = -1;        // Tracks the last hour when temperature was checked Prevents repeated checks within the same hour


// unsigned long lastChickenCheck = 0;  // last hourly temperature check

String weatherDesc = "Unknown";
String lastWeatherTime = "--:--";

unsigned long lastWeather = 0;
unsigned long weatherInterval = 600000;  // weather refresh interval (10 min)


/* ======================================================
   Function Prototypes for Slot Management and Threshhold management
====================================================== */
void handleSlots();
void handleSlotUpdate();
void incThreshold();
void decThreshold();


/* ======================================================
   EEPROM Functions
====================================================== */

/* ------------------------------------------------------
   EEPROM Wear Leveling Settings
------------------------------------------------------ */

#define CONFIG_BLOCK_SIZE sizeof(Config)
#define CONFIG_BLOCK_COUNT (EEPROM_SIZE / CONFIG_BLOCK_SIZE)

int currentConfigBlock = 0;


//Save configuration using wear leveling

void saveConfig() {

  // Move to next block
  currentConfigBlock++;

  if (currentConfigBlock >= CONFIG_BLOCK_COUNT)
    currentConfigBlock = 0;

  int address = currentConfigBlock * CONFIG_BLOCK_SIZE;

  EEPROM.put(address, cfg);

  EEPROM.commit();
}


/* ------------------------------------------------------
   Load configuration from EEPROM with wear leveling
------------------------------------------------------ */

void loadConfig() {

  bool found = false;  // indicates if valid config found

  /* ------------------------------------------------------
     Scan all EEPROM blocks to find the latest valid config
  ------------------------------------------------------ */

  for (int i = 0; i < CONFIG_BLOCK_COUNT; i++) {

    int address = i * CONFIG_BLOCK_SIZE;

    Config temp;

    EEPROM.get(address, temp);

    // Check if this block contains valid configuration
    if (temp.marker == 12345) {

      cfg = temp;              // copy to active config
      currentConfigBlock = i;  // remember block index
      found = true;
    }
  }

  /* ------------------------------------------------------
     If no valid configuration found
     initialize default settings
  ------------------------------------------------------ */

  if (!found) {

    cfg.marker = 12345;  // mark EEPROM block as valid

    cfg.autoMode = true;
    cfg.threshold = 20;

    // Default chicken light configuration
    cfg.chickenAutoMode = true;
    cfg.chickenState = false;
    cfg.chickenThreshold = 18.0;

    // Default time slots
    int def[7][4] = {
      { 8, 0, 10, 0 },
      { 10, 10, 12, 0 },
      { 12, 10, 14, 0 },
      { 14, 20, 17, 0 },
      { 17, 20, 19, 0 },
      { 19, 10, 21, 0 },
      { 21, 10, 23, 59 }
    };

    // Enable first 7 default slots
    for (int i = 0; i < 7; i++) {

      cfg.slots[i].sh = def[i][0];
      cfg.slots[i].sm = def[i][1];
      cfg.slots[i].eh = def[i][2];
      cfg.slots[i].em = def[i][3];
      cfg.slots[i].en = true;
    }

    // Disable remaining slots
    for (int i = 7; i < SLOT_COUNT; i++) {

      cfg.slots[i].sh = 0;
      cfg.slots[i].sm = 0;
      cfg.slots[i].eh = 0;
      cfg.slots[i].em = 0;
      cfg.slots[i].en = false;
    }

    /* ------------------------------------------------------
       Apply chicken settings to runtime variables
    ------------------------------------------------------ */

    chickenAutoMode = cfg.chickenAutoMode;
    chickenLightState = cfg.chickenState;
    chickenSummerThreshold = cfg.chickenThreshold;

    /* ------------------------------------------------------
       Save initial configuration to EEPROM
    ------------------------------------------------------ */

    saveConfig();
  }
}

/* ======================================================
   Function for Save Slot Changes
====================================================== */
void handleSlotUpdate() {

  for (int i = 0; i < SLOT_COUNT; i++) {

    String start = server.arg("start" + String(i));
    String end = server.arg("end" + String(i));

    cfg.slots[i].sh = start.substring(0, 2).toInt();
    cfg.slots[i].sm = start.substring(3, 5).toInt();

    cfg.slots[i].eh = end.substring(0, 2).toInt();
    cfg.slots[i].em = end.substring(3, 5).toInt();

    cfg.slots[i].en = server.hasArg("en" + String(i));
  }

  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

/* ======================================================
   Function for Threshold Increase/Decrese
====================================================== */
void incThreshold() {

  cfg.threshold += 0.5;

  if (cfg.threshold > 25) cfg.threshold = 25;

  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

void decThreshold() {

  cfg.threshold -= 0.5;

  if (cfg.threshold < 15) cfg.threshold = 15;

  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

/* ======================================================
   Fan Control
====================================================== */

/* Set relay state */
void setFan(bool s) {

  // Prevent unnecessary operations
  if (fanState == s)
    return;

  fanState = s;

  digitalWrite(RELAY_PIN, s ? HIGH : LOW);

  cfg.lastState = s;
}

/* ======================================================
   Chicken House Light Control
====================================================== */

void setChickenLight(bool state) {

  // Do nothing if state is already the same
  if (chickenLightState == state)
    return;

  chickenLightState = state;

  digitalWrite(CHICKEN_LIGHT_PIN, state ? HIGH : LOW);

  // Save only when state actually changed
  cfg.chickenState = state;

  saveConfig();
}


/* ======================================================
   Scheduler
====================================================== */

/* Check if current time falls inside an enabled slot */
bool inSlot() {

  int h = timeClient.getHours();
  int m = timeClient.getMinutes();

  int now = h * 60 + m;

  for (int i = 0; i < SLOT_COUNT; i++) {

    if (!cfg.slots[i].en) continue;

    int s = cfg.slots[i].sh * 60 + cfg.slots[i].sm;
    int e = cfg.slots[i].eh * 60 + cfg.slots[i].em;

    if (now >= s && now <= e) return true;
  }

  return false;
}

/* ======================================================
   Determine if current month is summer season
   Summer = March (3) → October (10)

   NTPClient does not provide month directly,
   so we calculate it from Unix Epoch time.
====================================================== */

bool isSummer() {

  time_t epochTime = timeClient.getEpochTime();

  struct tm* ptm = gmtime((time_t*)&epochTime);

  int month = ptm->tm_mon + 1;  // tm_mon is 0–11, so add 1

  if (month >= 3 && month <= 10)
    return true;
  else
    return false;
}

/* ======================================================
   Weather Fetch Function
====================================================== */

void getWeather(bool force = false) {

  if (!force && millis() - lastWeather < weatherInterval) return;

  if (WiFi.status() != WL_CONNECTED) return;

  lastWeather = millis();

  WiFiClient client;
  HTTPClient http;

  String url;
  url.reserve(150);
  url = "http://api.openweathermap.org/data/2.5/weather?q=";
  url += CITY;
  url += ",";
  url += COUNTRY;
  url += "&units=metric&appid=";
  url += WEATHER_KEY;

  http.begin(client, url);

  http.setTimeout(5000);  // prevent long blocking
  http.setReuse(false);   // avoid socket memory leaks

  int code = http.GET();

  if (code == 200) {

    String payload = http.getString();

    DynamicJsonDocument doc(512);

    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      http.end();
      return;  // stop if JSON parsing failed
    }

    currentTemp = doc["main"]["temp"].as<float>();
    weatherDesc = doc["weather"][0]["description"].as<String>();

    lastWeatherTime = timeClient.getFormattedTime().substring(0, 5);
  }

  http.end();
}


/* ======================================================
   Cooling System Logic
====================================================== */

void controlLogic() {

  if (!cfg.autoMode) return;

  bool slot = inSlot();

  if (slot && currentTemp >= cfg.threshold) {
    setFan(true);
  } else {
    setFan(false);
  }
}

/* ======================================================
   Chicken House Light Automation Logic
====================================================== */

void chickenLightLogic() {

  // Manual mode overrides everything
  if (!chickenAutoMode) return;

  int h = timeClient.getHours();
  int m = timeClient.getMinutes();

  int now = h * 60 + m;

  bool summer = isSummer();


  /* =========================
   SUMMER MODE
   Schedule: 10:30PM → 6:00AM
==========================*/

  if (summer) {

    int start = 22 * 60 + 30;  // 22:30
    int end = 6 * 60;          // 06:00

    bool inTime = (now >= start || now <= end);

    if (!inTime) {
      setChickenLight(false);
      return;
    }

    // Check temperature once every new hour
    int currentHour = timeClient.getHours();

    if (currentHour != lastChickenCheckHour) {

      lastChickenCheckHour = currentHour;

      if (currentTemp < chickenSummerThreshold)
        setChickenLight(true);
      else
        setChickenLight(false);
    }
  }


  /* =========================
     WINTER MODE
     Schedule: 6:30PM → 8:00AM
  ==========================*/

  else {

    int start = 18 * 60 + 30;  // 18:30
    int end = 8 * 60;          // 08:00

    bool inTime = (now >= start || now <= end);

    setChickenLight(inTime);
  }
}


/* ======================================================
   Web Interface
====================================================== */

void handleRoot() {
  /* ======================================================
     Dynamic System Data
  ====================================================== */

  String wifiStatus = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Disconnected";
  String fanStatus = fanState ? "ON" : "OFF";
  String chickenStatus = chickenLightState ? "ON" : "OFF";
  String currentTime = timeClient.getFormattedTime().substring(0, 5);
  uint32_t heap = ESP.getFreeHeap();
  unsigned long uptimeSeconds = millis() / 1000;
  int upHours = uptimeSeconds / 3600;
  int upMinutes = (uptimeSeconds % 3600) / 60;
  String uptimeString = String(upHours) + "h " + String(upMinutes) + "m";

  /* ======================================================
     Begin HTML Page
  ====================================================== */

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Smart Home Automation · Static Preview</title>";
  html += "<link rel='icon' href='https://cdn-icons-png.flaticon.com/128/7733/7733361.png'>";

  /* ======================================================
     CSS Styling (ESP-Friendly)
  ====================================================== */

  html += "<style>";

  // Base styles
  html += "*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{font-family:'Times New Roman',Times,serif;background:#f0f2f5;margin:0;padding:0;line-height:1.5;}";
  html += ".container{max-width:900px;margin:0 auto;padding:20px;}";
  html += "h1{text-align:center;margin-bottom:20px;font-size:1.9rem;color:#1e2a3a;}";
  html += "h3{font-size:1.3rem;margin-bottom:10px;color:#2c3e50;display:flex;align-items:center;gap:4px;}";

  // Card styles
  html += ".card{background:#ffffff;padding:18px 20px;border-radius:12px;margin-bottom:10px;";
  html += "box-shadow:0 4px 12px rgba(0,0,0,0.05);transition:box-shadow 0.2s;}";

  // Grid layout
  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;}";

  // Button styles
  html += "button{padding:10px 16px;border:none;border-radius:40px;font-size:14px;font-weight:500;";
  html += "cursor:pointer;margin:5px 5px 5px 0;background:#e9ecef;color:#1e293b;transition:0.15s;";
  html += "box-shadow:0 2px 4px rgba(0,0,0,0.02);border:1px solid rgba(0,0,0,0.03);}";
  html += "button:hover{filter:brightness(0.97);transform:translateY(-1px);box-shadow:0 6px 12px rgba(0,0,0,0.04);}";

  // Button colors
  html += ".green{background:#2ecc71;color:white;border:none;}";
  html += ".red{background:#e74c3c;color:white;border:none;}";

  // Text styles
  html += ".small{color:#4b5563;margin:8px 0;font-size:0.95rem;display:flex;align-items:center;gap:6px;}";

  // Inline group for thresholds
  html += ".inline-group{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:12px 0 8px;}";
  html += ".inline-group button{padding:6px 14px;margin:0;background:#dee2e6;}";

  // Value badge
  html += ".value-badge{font-weight:600;background:#f1f5f9;padding:4px 10px;border-radius:30px;font-size:0.9rem;}";

  // Links
  html += "a{text-decoration:none;display:inline-block;}";
  html += "hr{margin:12px 0;border:0;height:1px;background:#e2e8f0;}";

  // Mobile responsive
  html += "@media(max-width:600px){";
  html += "body{font-size:14px;}";
  html += "button{padding:12px 24px;font-size:14px;}";
  html += ".grid{gap:3px;grid-template-columns:1fr;}";
  html += ".container{padding:5px;}";
  html += ".card{padding:5px;}";
  html += "h1{font-size:26px;}";
  html += "h3{font-size:24px;}";
  html += ".header-block .small{font-size:13.5px;}";
  html += "}";

  // Fake link style
  html += ".fake-link{background:transparent;border:1px solid #bac8d9;color:#1e293b;}";

  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";

  /* ======================================================
     Page Header
  ====================================================== */

  html += "<h1>🏠 Smart Home Automation System</h1>";

  /* ======================================================
     Status Card
  ====================================================== */

  html += "<div class='card header-block' id='statusCard'>";
  html += "<div class='small'>🕒 Time: <span id='currentTime'>" + currentTime + "</span></div>";
  html += "<div class='small'>🌤 Weather: <span id='weatherTemp'>" + String(currentTemp, 1) + "°C</span> | <span id='weatherDesc'>" + weatherDesc + "</span> | Last update: <span id='weatherUpdate'>" + lastWeatherTime + "</span></div>";
  html += "<div class='small'>💾 Heap: <span id='heapBytes'>" + String(heap) + "</span> bytes</div>";
  html += "<div class='small'>⏱ System Uptime: <span id='uptime'>" + uptimeString + "</span></div>";
  html += "</div>";

  /* ======================================================
     Device Grid
  ====================================================== */

  html += "<div class='grid'>";

  /* ======================================================
     Water Pump Card
  ====================================================== */

  html += "<div style='padding-bottom:20px;' class='card'>";
  html += "<h3>💧Water Pump Switch</h3>";
  html += "<button id='motorBtn' class='green' style='padding:10px 40px;font-size:16px;' onclick='toggleMotor()'>ON</button>";
  html += "<p class='small' style='margin-top:12px;'>Click to Turn ON/OFF Water Pump</p>";
  html += "</div>";

  /* ======================================================
     Cooling System Card
  ====================================================== */

  html += "<div class='card'>";
  html += "<h3>❄️ Cooling System</h3>";
  html += "<p style='margin:4px 0'>Status: <b id='fanStatus'>" + fanStatus + "</b></p>";
  html += "<p style='margin:4px 0'>Mode: <span id='fanMode'>" + String(cfg.autoMode ? "AUTO" : "MANUAL") + "</span></p>";

  // Threshold controls
  html += "<div class='inline-group'>";
  html += "<span>Threshold: <span id='fanThreshold' class='value-badge'>" + String(cfg.threshold, 1) + "°C</span></span>";
  html += "<a href='/thUp'><button class='fake-link' style='min-width:40px;'>+</button></a>";
  html += "<a href='/thDown'><button class='fake-link' style='min-width:40px;'>–</button></a>";
  html += "</div>";

  // Control buttons
  html += "<div style='margin:10px 0;'>";
  html += "<a href='/fan/on'><button class='green'>ON</button></a>";
  html += "<a href='/fan/off'><button class='red'>OFF</button></a>";
  html += "<a href='/mode'><button>Switch Mode</button></a>";
  html += "</div>";

  // Extra actions
  html += "<div style='margin-top:8px;'>";
  html += "<a href='/updateWeather'><button>⏳ Update Weather</button></a>";
  html += "<a href='/slots'><button>📋 Manage Time Slots</button></a>";
  html += "</div>";
  html += "</div>";

  /* ======================================================
     Chicken House Light Card
  ====================================================== */

  html += "<div class='card'>";
  html += "<h3>🐔 Chicken House Light</h3>";
  html += "<p style='margin:4px 0'>Status: <b id='chickenStatus'>" + chickenStatus + "</b></p>";
  html += "<p style='margin:4px 0'>Mode: <span id='chickenMode'>" + String(chickenAutoMode ? "AUTO" : "MANUAL") + "</span></p>";

  // Summer threshold
  html += "<div class='inline-group'>";
  html += "<span>Summer threshold: <span id='chickenThreshold' class='value-badge'>" + String(chickenSummerThreshold, 1) + "°C</span></span>";
  html += "<a href='/chickenThUp'><button class='fake-link'>+</button></a>";
  html += "<a href='/chickenThDown'><button class='fake-link'>–</button></a>";
  html += "</div>";

  // Chicken buttons
  html += "<div style='margin:10px 0;'>";
  html += "<a href='/chicken/on'><button class='green'>ON</button></a>";
  html += "<a href='/chicken/off'><button class='red'>OFF</button></a>";
  html += "<a href='/chicken/mode'><button>Switch Mode</button></a>";
  html += "</div>";
  html += "</div>";

  html += "</div>";  // end grid

  /* ======================================================
     Footer
  ====================================================== */

  html += "<div style='text-align:center;margin-top:24px;opacity:0.7;font-size:0.85rem;'>";
  html += "⚡ ESP8266 Smart Home Controller<br>";
  html += " ©️ Rakib Uddin<br>";
  html += "</div>";

  html += "</div>";  // end container

  /* ======================================================
     JavaScript
  ====================================================== */

  html += "<script>";

  // Motor toggle function
  html += "let motorState=false;";
  html += "function toggleMotor(){";
  html += "fetch('/rf');";
  html += "let btn=document.getElementById('motorBtn');";
  html += "motorState=!motorState;";
  html += "if(motorState){";
  html += "btn.classList.remove('green');";
  html += "btn.classList.add('red');";
  html += "btn.innerText='OFF';";
  html += "}else{";
  html += "btn.classList.remove('red');";
  html += "btn.classList.add('green');";
  html += "btn.innerText='ON';";
  html += "}";
  html += "}";

  // Time update function
  html += "function refreshTime(){";
  html += "const d=new Date();";
  html += "let hours=d.getHours().toString().padStart(2,'0');";
  html += "let mins=d.getMinutes().toString().padStart(2,'0');";
  html += "document.getElementById('currentTime').innerText=hours+':'+mins;";
  html += "}";
  html += "setInterval(refreshTime,1000);";
  html += "refreshTime();";

  // Mode toggle function
  html += "window.toggleMode=function(spanId){";
  html += "if(spanId==='fanMode'){";
  html += "fetch('/mode');";
  html += "}else if(spanId==='chickenMode'){";
  html += "fetch('/chicken/mode');";
  html += "}";
  html += "}";

  html += "</script>";

  html += "</body></html>";

  /* ======================================================
     Send Page to Client
  ====================================================== */

  server.send(200, "text/html", html);
}

/* ======================================================
   Time Slot Management Page

   This page allows the user to:
   - View all 14 configured time slots
   - Edit start and end times
   - Enable/disable each slot

   UI Design Goals:
   - Lightweight HTML (important for ESP8266)
   - Mobile friendly
   - Table layout for compact slot viewing
====================================================== */

void handleSlots() {

  // HTML response string
  String html = "<html><head>";
  html += "<meta charset='UTF-8'>";

  // Mobile viewport scaling
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  // Browser page title
  html += "<title>Time Slot Management</title>";
  html += "<link rel='icon' href='https://cdn-icons-png.flaticon.com/128/7733/7733361.png'>";

  // ----------- CSS Styling (Lightweight) -----------

  html += "<style>";

  // Page background and base font
  //html += "body{font-family:Arial;background:#f0f2f5;margin:0;padding:20px;}";
  html += "body{font-family:'Times New Roman',Times,serif;background:#f0f2f5;margin:0;padding:0;line-height:1.5;}";

  // Page heading alignment
  html += "h2{text-align:center;margin-bottom:10px;}";

  // Main container card
  html += ".card{background:#fff;padding:20px;border-radius:10px;";
  html += "max-width:900px;margin:auto;box-shadow:0 2px 6px rgba(0,0,0,0.1);}";

  // Table styling
  html += "table{width:100%;border-collapse:collapse;margin-top:10px;}";

  // Table cell styling
  html += "th,td{padding:8px;text-align:center;border-bottom:1px solid #ddd;}";

  // Header row background
  html += "th{background:#f7f7f7;}";

  // Time input field styling
  html += "input[type=time]{padding:4px;}";

  // Make enable checkbox column smaller
  html += "td:last-child{width:70px;}";

  // Button styling
  html += "button{padding:10px 16px;border:none;border-radius:6px;margin:10px 5px;cursor:pointer;}";

  // Save button color
  html += ".save{background:#2ecc71;color:white;}";

  // Back button color
  html += ".back{background:#aaa;color:white;}";

  // Allow horizontal scrolling on small screens
  html += ".table-wrap{overflow-x:auto;}";

  html += "</style>";

  html += "</head><body>";

  // ----------- Page Card Container -----------

  html += "<div class='card'>";

  // Page title
  html += "<h2>⏱ Time Slot Management</h2>";

  // Form submits to /slotUpdate endpoint
  html += "<form action='/slotUpdate'>";

  // Wrapper enables horizontal scroll on mobile
  html += "<div class='table-wrap'>";

  html += "<table>";

  // ----------- Table Header -----------

  html += "<tr>";
  html += "<th>Slot</th>";    // Slot number
  html += "<th>Start</th>";   // Slot start time
  html += "<th>End</th>";     // Slot end time
  html += "<th>Enable</th>";  // Enable/disable checkbox
  html += "</tr>";

  // ----------- Generate 14 Slot Rows -----------

  for (int i = 0; i < SLOT_COUNT; i++) {

    // Temporary buffers for formatting HH:MM time
    char startTime[6];
    char endTime[6];

    // Convert stored hour/minute values to HH:MM format
    sprintf(startTime, "%02d:%02d", cfg.slots[i].sh, cfg.slots[i].sm);
    sprintf(endTime, "%02d:%02d", cfg.slots[i].eh, cfg.slots[i].em);

    html += "<tr>";

    // Display slot number (1–14)
    html += "<td>" + String(i + 1) + "</td>";

    // Start time input field
    html += "<td><input type='time' name='start" + String(i) + "' value='" + String(startTime) + "'></td>";

    // End time input field
    html += "<td><input type='time' name='end" + String(i) + "' value='" + String(endTime) + "'></td>";

    // Enable checkbox
    html += "<td><input type='checkbox' name='en" + String(i) + "' ";

    // If slot is enabled in config, check the checkbox
    if (cfg.slots[i].en) html += "checked";

    html += "></td>";

    html += "</tr>";
  }

  html += "</table>";

  html += "</div>";

  // ----------- Action Buttons -----------

  html += "<div style='text-align:center;'>";

  // Submit form to save slot changes
  html += "<button class='save' type='submit'>Save Slots</button>";

  html += "</form>";

  // Return to main dashboard
  html += "<a href='/'><button class='back'>Back</button></a>";

  html += "</div>";

  html += "</div>";

  html += "</body></html>";

  // Send HTML page to browser
  server.send(200, "text/html", html);
}


/* ======================================================
   Water Pump Control Page
   This page only shows the motor control card
   Used for quick access to pump control
====================================================== */

void waterPump() {

  // HTML response container
  String html = "<html><head>";
  html += "<meta charset='UTF-8'>";

  // Mobile responsive viewport
  html += "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=yes'>";

  // Page title with icon
  html += "<title>⚙ Water Pump Control</title>";
  html += "<link rel='icon' href='https://cdn-icons-png.flaticon.com/128/7733/7733361.png'>";

  // -------- Lightweight CSS --------
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{font-family:'Times New Roman',Times,serif;background:#f0f2f5;margin:0;padding:0;line-height:1.5;min-height:100vh;display:flex;align-items:center;justify-content:center;}";
  html += ".container{width:100%;max-width:400px;margin:0 auto;padding:15px;}";
  html += "h1{text-align:center;margin-bottom:25px;font-size:24px;color:#333;}";
  html += ".card{background:#fff;padding:30px 20px;border-radius:20px;box-shadow:0 10px 25px rgba(0,0,0,0.1);text-align:center;}";
  html += "h3{font-size:20px;margin-bottom:30px;color:#555;font-weight:normal;}";
  html += ".button-wrapper{display:flex;justify-content:center;align-items:center;min-height:200px;}";
  html += "button{width:200px;height:200px;border:none;border-radius:100px;cursor:pointer;font-size:24px;font-weight:bold;box-shadow:0 8px 15px rgba(0,0,0,0.2);transition:all 0.2s ease;border:4px solid #fff;}";
  html += "button:active{transform:scale(0.95);box-shadow:0 4px 8px rgba(0,0,0,0.2);}";
  html += ".green{background:#2ecc71;color:white;}";
  html += ".red{background:#e74c3c;color:white;}";
  html += "</style>";

  html += "</head><body>";

  html += "<div class='container'>";

  // Page heading
  html += "<h1>💧 Water Pump Control</h1>";

  // -------- Pump Control Card --------
  html += "<div class='card'>";
  html += "<h3>⚙ Motor Control</h3>";
  html += "<div class='button-wrapper'>";
  
  // Button that sends RF signal
  html += "<button id='motorBtn' class='green' onclick='toggleMotor()'>OFF</button>";
  
  html += "</div>"; // Close button-wrapper
  html += "</div>"; // Close card
  html += "</div>"; // Close container

  // -------- Pump Toggle Script --------
  html += "<script>";
  html += "var motorState=false;";
  html += "var btn=document.getElementById('motorBtn');";
  html += "function toggleMotor(){";
  // send RF command to ESP
  html += "fetch('/rf')";
  html += ".catch(function(err){console.log('RF Error:'+err);});";
  html += "motorState=!motorState;";
  html += "if(motorState){";
  html += "btn.classList.remove('green');";
  html += "btn.classList.add('red');";
  html += "btn.innerHTML='ON';";
  html += "}else{";
  html += "btn.classList.remove('red');";
  html += "btn.classList.add('green');";
  html += "btn.innerHTML='OFF';";
  html += "}";
  html += "}";
  html += "</script>";

  html += "</body></html>";

  // Send page to browser
  server.send(200, "text/html", html);
}



/* ======================================================
   Web Handlers
====================================================== */

void handleFanOn() {

  cfg.autoMode = false;
  setFan(true);
  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleFanOff() {

  cfg.autoMode = false;
  setFan(false);
  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMode() {

  cfg.autoMode = !cfg.autoMode;
  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}


/* RF Signal Sender */

void handleRF() {

  rf.send(10168801, 24);

  server.send(200, "text/plain", "Signal sent");
}

/* ======================================================
   Chicken Light Manual Controls
====================================================== */

void handleChickenOn() {

  chickenAutoMode = false;
  setChickenLight(true);

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleChickenOff() {

  chickenAutoMode = false;
  setChickenLight(false);

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleChickenMode() {

  chickenAutoMode = !chickenAutoMode;

  cfg.chickenAutoMode = chickenAutoMode;

  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

/* ======================================================
   Chicken Light Threshold Increase
====================================================== */

void chickenThUp() {

  chickenSummerThreshold += 0.5;

  if (chickenSummerThreshold > 25)
    chickenSummerThreshold = 25;

  cfg.chickenThreshold = chickenSummerThreshold;

  //#############################################################################################

  // This following line is needed for checking only, if want u can comment it out.
  lastChickenCheckHour = -1;  // force new evaluation

  //##############################################################################################
  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

/* ======================================================
   Chicken Light Threshold Decrease
====================================================== */

void chickenThDown() {

  chickenSummerThreshold -= 0.5;

  if (chickenSummerThreshold < 10)
    chickenSummerThreshold = 10;

  cfg.chickenThreshold = chickenSummerThreshold;

  //##################################################################################################

  // This following line is needed for checking only, if want u can comment it out.
  lastChickenCheckHour = -1;  // force new evaluation

  //###################################################################################################
  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}


/* ======================================================
   Setup Function
====================================================== */

void setup() {

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // Initialize chicken house light relay
  pinMode(CHICKEN_LIGHT_PIN, OUTPUT);
  digitalWrite(CHICKEN_LIGHT_PIN, LOW);  // Light OFF initially

  // Initialize built-in LED for WiFi status indication
  pinMode(LED_BUILTIN, OUTPUT);

  // ESP8266 LED is active LOW
  digitalWrite(LED_BUILTIN, HIGH);  // LED OFF initially


  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  /* ------------------------------------------------------
   Restore Cooling Fan State From EEPROM
------------------------------------------------------ */

  fanState = cfg.lastState;
  digitalWrite(RELAY_PIN, fanState ? HIGH : LOW);

  /* ------------------------------------------------------
   Restore Chicken Light Settings From EEPROM
------------------------------------------------------ */
  chickenAutoMode = cfg.chickenAutoMode;
  chickenSummerThreshold = cfg.chickenThreshold;
  setChickenLight(cfg.chickenState);

  rf.enableTransmit(RF_PIN);


  /* WiFi Connection */

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), WIFI_PASS);

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    //Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    // WiFi connected successfully
  } else {
    // WiFi not connected, system will continue offline
  }


  /* Time Sync */

  timeClient.begin();


  /* Get weather immediately on boot */

  getWeather(true);

  lastChickenCheckHour = -1;  //Force first chicken temperature check after boot

  /* Web Routes */

  server.on("/", handleRoot);
  server.on("/fan/on", handleFanOn);
  server.on("/fan/off", handleFanOff);
  server.on("/mode", handleMode);
  server.on("/rf", handleRF);

  server.on("/slots", handleSlots);
  server.on("/slotUpdate", handleSlotUpdate);

  // Wter Pump Single page
  server.on("/pump", waterPump);

  server.on("/thUp", incThreshold);
  server.on("/thDown", decThreshold);

  server.on("/updateWeather", []() {
    getWeather(true);
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/chicken/on", handleChickenOn);
  server.on("/chicken/off", handleChickenOff);
  server.on("/chicken/mode", handleChickenMode);
  server.on("/chickenThUp", chickenThUp);
  server.on("/chickenThDown", chickenThDown);

  server.begin();
}


/* ======================================================
   WiFi Reconnect Watchdog
====================================================== */

void checkWiFi() {

  static unsigned long lastCheck = 0;

  if (millis() - lastCheck < 400000) return;

  lastCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {

    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), WIFI_PASS);
  }
}

/* ======================================================
   WiFi Status LED Indicator
   - LED OFF when WiFi connected
   - LED fast blink when WiFi lost
====================================================== */

void wifiStatusLED() {

  static unsigned long lastBlink = 0;
  static bool ledState = false;

  // If WiFi is connected → LED OFF
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, HIGH);  // OFF
    return;
  }

  // If WiFi lost → blink fast
  if (millis() - lastBlink > 200) {

    lastBlink = millis();

    ledState = !ledState;

    digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
  }
}

/* ======================================================
   Main Loop
====================================================== */

void loop() {

  server.handleClient();

  timeClient.update();

  checkWiFi();

  wifiStatusLED();  // Led Indicator

  getWeather();

  controlLogic();

  chickenLightLogic();  // run chicken house light automation
}