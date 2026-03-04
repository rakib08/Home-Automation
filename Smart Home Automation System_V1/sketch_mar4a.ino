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

ESP8266WebServer server(80);  // Web server running on port 80

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
  bool autoMode;           // auto / manual mode
  float threshold;         // temperature threshold
  bool lastState;          // last fan state
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

/* Save configuration to EEPROM */
void saveConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}


/* Load configuration from EEPROM */
void loadConfig() {

  EEPROM.get(0, cfg);

  /* If EEPROM is empty or corrupted
     initialize default settings */
  if (cfg.threshold < 10 || cfg.threshold > 40) {

    cfg.autoMode = true;
    cfg.threshold = 20;

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

    for (int i = 0; i < 7; i++) {
      cfg.slots[i] = { def[i][0], def[i][1], def[i][2], def[i][3], true };
    }

    // Remaining slots disabled
    for (int i = 7; i < 14; i++) {
      cfg.slots[i] = { 0, 0, 0, 0, false };
    }

    saveConfig();
  }
}

/* ======================================================
   Function for Save Slot Changes
====================================================== */
void handleSlotUpdate() {

  for (int i = 0; i < SLOT_COUNT; i++) {

    cfg.slots[i].sh = server.arg("sh" + String(i)).toInt();
    cfg.slots[i].sm = server.arg("sm" + String(i)).toInt();
    cfg.slots[i].eh = server.arg("eh" + String(i)).toInt();
    cfg.slots[i].em = server.arg("em" + String(i)).toInt();

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

  fanState = s;

  digitalWrite(RELAY_PIN, s ? HIGH : LOW);

  cfg.lastState = s;
}

/* ======================================================
   Chicken House Light Control
====================================================== */

void setChickenLight(bool state) {

  chickenLightState = state;

  digitalWrite(CHICKEN_LIGHT_PIN, state ? HIGH : LOW);  // Active HIGH relay
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

  String url = "http://api.openweathermap.org/data/2.5/weather?q=";
  url += CITY;
  url += ",";
  url += COUNTRY;
  url += "&units=metric&appid=";
  url += WEATHER_KEY;

  http.begin(client, url);

  int code = http.GET();

  if (code == 200) {

    String payload = http.getString();

    DynamicJsonDocument doc(768);
    deserializeJson(doc, payload);

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

  String wifiStatus = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Disconnected";

  String fanStatus = fanState ? "ON" : "OFF";
  // Get formatted time (HH:MM) from NTP client substring keeps only HH:MM and removes seconds
  String currentTime = timeClient.getFormattedTime().substring(0, 5);

  String html = "<!DOCTYPE html><html><head>";

  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Smart Home Automation</title>";

  html += "<style>";

  html += "body{font-family:Arial;background:#f0f2f5;margin:0;padding:0;}";
  html += ".container{max-width:900px;margin:auto;padding:20px;}";
  html += "h1{text-align:center;margin-bottom:20px;}";

  html += ".card{background:#fff;padding:18px;border-radius:10px;margin-bottom:20px;";
  html += "box-shadow:0 2px 6px rgba(0,0,0,0.08);}";

  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;}";

  html += "button{padding:10px 16px;border:none;border-radius:6px;font-size:15px;cursor:pointer;}";

  html += ".green{background:#2ecc71;color:white;}";
  html += ".red{background:#e74c3c;color:white;}";

  html += ".small{color:#555;margin:6px 0;}";

  html += "</style></head><body>";

  html += "<div class='container'>";

  html += "<h1>🏠 Smart Home Automation System</h1>";

  html += "<div class='card'>";

  html += "<div class='small'>🕒 Time: " + currentTime + "</div>";

  html += "<div class='small'>📶 WiFi Status: " + wifiStatus + "</div>";

  html += "<div class='small'>🌤 Weather: " + String(currentTemp, 2) + "°C - " + weatherDesc + " | Last Update: " + lastWeatherTime + "</div>";

  html += "</div>";

  html += "<div class='grid'>";


  /* Cooling System Card */

  html += "<div class='card'>";
  html += "<h3>❄ Cooling System</h3>";

  html += "<p>Status: <b>" + fanStatus + "</b></p>";
  html += "<p>Mode: " + String(cfg.autoMode ? "AUTO" : "MANUAL") + "</p>";
  html += "<p>Threshold: " + String(cfg.threshold) + "°C</p>";

  html += "<p>Threshold: " + String(cfg.threshold, 1) + "°C ";
  html += "<a href='/thUp'><button>+</button></a>";
  html += "<a href='/thDown'><button>-</button></a>";
  html += "</p>";

  html += "<a href='/fan/on'><button class='green'>ON</button></a>";
  html += "<a href='/fan/off'><button class='red'>OFF</button></a>";
  html += "<a href='/mode'><button>Toggle Mode</button></a>";
  html += "<a href='/updateWeather'><button>Update Weather</button></a>";
  html += "<a href='/slots'><button>Manage Time Slots</button></a>";

  html += "</div>";


  /* Motor RF Control Card */

  html += "<div class='card'>";
  html += "<h3>⚙ Motor Control Switch</h3>";

  html += "<button id='motorBtn' class='green' onclick='toggleMotor()'>ON/OFF</button>";

  html += "</div>";

  html += "</div></div>";

  /* Chicken House Light Card */

  html += "<div class='card'>";
  html += "<h3>🐔 Chicken House Light</h3>";

  html += "<p>Status: <b>" + String(chickenLightState ? "ON" : "OFF") + "</b></p>";
  html += "<p>Mode: " + String(chickenAutoMode ? "AUTO" : "MANUAL") + "</p>";
  html += "<p>Summer Threshold: " + String(chickenSummerThreshold, 1) + "°C ";
  html += "<a href='/chickenThUp'><button>+</button></a>";
  html += "<a href='/chickenThDown'><button>-</button></a>";
  html += "</p>";

  html += "<a href='/chicken/on'><button class='green'>ON</button></a>";
  html += "<a href='/chicken/off'><button class='red'>OFF</button></a>";
  html += "<a href='/chicken/mode'><button>Toggle Mode</button></a>";

  html += "</div>";


  /* Button toggle script */

  html += "<script>";

  html += "var motorState=false;";

  html += "function toggleMotor(){";

  html += "fetch('/rf');";

  html += "var btn=document.getElementById('motorBtn');";

  html += "motorState=!motorState;";

  html += "if(motorState){";
  html += "btn.classList.remove('green');";
  html += "btn.classList.add('red');";
  html += "}else{";
  html += "btn.classList.remove('red');";
  html += "btn.classList.add('green');";
  html += "}";

  html += "}";

  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

/* ======================================================
   Web Interface - Time Slots Management
====================================================== */
void handleSlots() {

  String html = "<html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Time Slot Manager</title>";

  html += "<style>";
  html += "body{font-family:Arial;background:#f0f2f5;padding:20px}";
  html += ".card{background:#fff;padding:15px;border-radius:8px;margin-bottom:10px}";
  html += "input{width:70px;padding:5px}";
  html += "button{padding:8px 10px;margin-top:5px}";
  html += "</style>";

  html += "</head><body>";

  html += "<h2>Time Slot Management</h2>";

  html += "<form action='/slotUpdate'>";

  for (int i = 0; i < SLOT_COUNT; i++) {

    html += "<div class='card'>";

    html += "Slot " + String(i + 1) + "<br>";

    html += "Start: <input name='sh" + String(i) + "' value='" + String(cfg.slots[i].sh) + "'>";
    html += ":<input name='sm" + String(i) + "' value='" + String(cfg.slots[i].sm) + "'>";

    html += " End: <input name='eh" + String(i) + "' value='" + String(cfg.slots[i].eh) + "'>";
    html += ":<input name='em" + String(i) + "' value='" + String(cfg.slots[i].em) + "'>";

    html += " Enabled: <input type='checkbox' name='en" + String(i) + "' ";

    if (cfg.slots[i].en) html += "checked";

    html += ">";

    html += "</div>";
  }

  html += "<button type='submit'>Save Slots</button>";

  html += "</form>";

  html += "<br><a href='/'><button>Back</button></a>";

  html += "</body></html>";

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

  server.sendHeader("Location", "/");
  server.send(303);
}

/* ======================================================
   Chicken Light Threshold Increase
====================================================== */

void chickenThUp() {

  chickenSummerThreshold += 0.5;

  // Prevent exceeding safe limit
  if (chickenSummerThreshold > 25)
    chickenSummerThreshold = 25;

  server.sendHeader("Location", "/");
  server.send(303);
}

/* ======================================================
   Chicken Light Threshold Decrease
====================================================== */

void chickenThDown() {

  chickenSummerThreshold -= 0.5;

  // Prevent going too low
  if (chickenSummerThreshold < 10)
    chickenSummerThreshold = 10;

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

  Serial.begin(115200);

  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  rf.enableTransmit(RF_PIN);


  /* WiFi Connection */

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), WIFI_PASS);

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi not connected. Continuing without WiFi.");
  }


  /* Time Sync */

  timeClient.begin();


  /* Get weather immediately on boot */

  getWeather(true);


  /* Web Routes */

  server.on("/", handleRoot);
  server.on("/fan/on", handleFanOn);
  server.on("/fan/off", handleFanOff);
  server.on("/mode", handleMode);
  server.on("/rf", handleRF);

  server.on("/slots", handleSlots);
  server.on("/slotUpdate", handleSlotUpdate);

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

    Serial.println("WiFi lost. Reconnecting...");

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