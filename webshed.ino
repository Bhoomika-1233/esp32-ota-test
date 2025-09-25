#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <HTTPUpdate.h>

// ------------------ AP CONFIG ------------------
const char* apSSID = "SmartIrrigation";
const char* apPassword = "12345678";

WebServer server(80);
Preferences preferences;

// ------------------ WIFI CONFIG ------------------
String wifiSSID = "";
String wifiPASS = "";
bool wifiConfigured = false;
bool wifiConnecting = false;

// ------------------ OTA CONFIG ------------------
const char* otaUpdateUrl = "https://raw.githubusercontent.com/Bhoomika-1233/esp32-ota-test/refs/heads/main/ota.txt"; // Replace with your server URL
const char* otaVersionUrl = "https://raw.githubusercontent.com/Bhoomika-1233/esp32-ota-test/refs/heads/main/otainternet.ino";  // Replace with your version check URL
String currentVersion = "1.0.1"; // Updated version to include schedule management
unsigned long lastOTACheck = 0;
const unsigned long OTA_CHECK_INTERVAL = 3600000; // Check every hour

// ------------------ LOGS ------------------
String logBuffer = "";
void appendLog(String msg) {
  String timestamp = "[" + String(millis()/1000) + "s] ";
  String fullMsg = timestamp + msg;
  Serial.println(fullMsg);
  logBuffer += fullMsg + "<br>";
  if (logBuffer.length() > 8000) logBuffer = logBuffer.substring(3000);
}

// ------------------ SERVER URL ------------------
const char* scheduleUrl = "http://192.168.68.115:4000/api/schedule";
const char* waterUrl    = "http://192.168.68.115:4000/api/water";

// ------------------ NTP CONFIG ------------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;  // GMT+5:30 India
const int daylightOffset_sec = 0;
bool timeConfigured = false;

// ------------------ PINS ------------------
#define RELAY_PIN 26
#define FLOW_SENSOR_PIN 5

// ------------------ FLOW VARIABLES ------------------
volatile int pulseCount = 0;
float totalLitres = 0;
float scheduleLitres = 0;
float dailyTotalLitres = 0;
unsigned long lastFlowCheck = 0;
const unsigned long FLOW_INTERVAL = 10000; // 10 sec
bool relayState = false;
int currentDay = 0;

// ------------------ SCHEDULE STRUCT ------------------
struct Schedule {
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  bool running;
  bool completed;
  String name;  // Added name field for better identification
  bool enabled; // Added enabled flag
};
#define MAX_SCHEDULES 10
Schedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
unsigned long lastScheduleFetch = 0;
const unsigned long SCHEDULE_FETCH_INTERVAL = 300000; // 5 min

// ------------------ FUNCTION PROTOTYPES ------------------
String getCurrentTimeString();
String getCurrentDateString();
void connectToWiFi();
void syncTime();
bool fetchSchedules();
void addScheduleFromJson(JsonObject obj);
void startIrrigation();
void stopIrrigation();
String getConfigHTML();
String getDashboardHTML();
String getScheduleManagerHTML();
void handleRoot();
void handleDashboard();
void handleConnect();
void handleScheduleManager();
void handleAddSchedule();
void handleDeleteSchedule();
void handleToggleSchedule();
void handleNotFound();
void countPulse();
void getCurrentTime(int &hour, int &minute, int &second);
void getCurrentDate(int &year, int &month, int &day);
void setupOTA();
void checkForOTAUpdate();
String scanNetworks();
void saveSchedulesToPreferences();
void loadSchedulesFromPreferences();

// ------------------ ISR ------------------
void IRAM_ATTR countPulse() { pulseCount++; }

// ------------------ SCHEDULE MANAGEMENT FUNCTIONS ------------------
void saveSchedulesToPreferences() {
  preferences.begin("schedules", false);
  preferences.putInt("count", scheduleCount);
  
  for(int i = 0; i < scheduleCount; i++) {
    String prefix = "sched" + String(i) + "_";
    preferences.putInt((prefix + "sh").c_str(), schedules[i].startHour);
    preferences.putInt((prefix + "sm").c_str(), schedules[i].startMinute);
    preferences.putInt((prefix + "eh").c_str(), schedules[i].endHour);
    preferences.putInt((prefix + "em").c_str(), schedules[i].endMinute);
    preferences.putString((prefix + "name").c_str(), schedules[i].name);
    preferences.putBool((prefix + "enabled").c_str(), schedules[i].enabled);
  }
  preferences.end();
  appendLog("Schedules saved to preferences");
}

void loadSchedulesFromPreferences() {
  preferences.begin("schedules", true);
  int savedCount = preferences.getInt("count", 0);
  
  if(savedCount > 0 && savedCount <= MAX_SCHEDULES) {
    scheduleCount = savedCount;
    for(int i = 0; i < scheduleCount; i++) {
      String prefix = "sched" + String(i) + "_";
      schedules[i].startHour = preferences.getInt((prefix + "sh").c_str(), 0);
      schedules[i].startMinute = preferences.getInt((prefix + "sm").c_str(), 0);
      schedules[i].endHour = preferences.getInt((prefix + "eh").c_str(), 0);
      schedules[i].endMinute = preferences.getInt((prefix + "em").c_str(), 0);
      schedules[i].name = preferences.getString((prefix + "name").c_str(), "Schedule " + String(i+1));
      schedules[i].enabled = preferences.getBool((prefix + "enabled").c_str(), true);
      schedules[i].running = false;
      schedules[i].completed = false;
    }
    appendLog("Loaded " + String(scheduleCount) + " schedules from preferences");
  }
  preferences.end();
}

// ------------------ TIME FUNCTIONS ------------------
void getCurrentTime(int &hour, int &minute, int &second) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) { hour=0; minute=0; second=0; return; }
  hour = timeinfo.tm_hour; 
  minute = timeinfo.tm_min; 
  second = timeinfo.tm_sec;
}

void getCurrentDate(int &year, int &month, int &day) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) { year=2024; month=1; day=1; return; }
  year = timeinfo.tm_year + 1900;
  month = timeinfo.tm_mon + 1;
  day = timeinfo.tm_mday;
}

String getCurrentTimeString() {
  int h, m, s;
  getCurrentTime(h, m, s);
  return String(h < 10 ? "0" : "") + String(h) + ":" + 
         String(m < 10 ? "0" : "") + String(m) + ":" + 
         String(s < 10 ? "0" : "") + String(s);
}

String getCurrentDateString() {
  int y, m, d;
  getCurrentDate(y, m, d);
  return String(d < 10 ? "0" : "") + String(d) + "/" + 
         String(m < 10 ? "0" : "") + String(m) + "/" + 
         String(y);
}

// ------------------ WIFI NETWORK SCANNER ------------------
String scanNetworks() {
  String networks = "";
  int n = WiFi.scanNetworks();
  if (n == 0) {
    networks = "<option value=''>No networks found</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      networks += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + WiFi.RSSI(i) + " dBm)</option>";
    }
  }
  return networks;
}

// ------------------ NTP SYNC ------------------
void syncTime() {
  if (!wifiConfigured) return;
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  int attempts = 0;
  while (attempts < 10) {
    delay(1000);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      timeConfigured = true;
      appendLog("NTP time synced: " + getCurrentDateString() + " " + getCurrentTimeString());
      return;
    }
    attempts++;
  }
  appendLog("NTP sync failed after attempts");
}

// ------------------ WIFI MANAGEMENT ------------------
void connectToWiFi() {
  wifiConnecting = true;
  WiFi.disconnect();
  delay(1000);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  
  appendLog("Connecting to WiFi: " + wifiSSID);
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
    server.handleClient();
  }

  wifiConnecting = false;

  if(WiFi.status() == WL_CONNECTED) {
    wifiConfigured = true;
    appendLog("WiFi connected: " + wifiSSID);
    appendLog("IP Address: " + WiFi.localIP().toString());
    appendLog("Signal: " + String(WiFi.RSSI()) + " dBm");
    syncTime();
    setupOTA();
  } else {
    appendLog("Failed to connect to WiFi: " + wifiSSID);
    wifiConfigured = false;
  }
}

// ------------------ INTERNET OTA UPDATE CHECK ------------------
void checkForOTAUpdate() {
  if (!wifiConfigured || WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  if (millis() - lastOTACheck < OTA_CHECK_INTERVAL && lastOTACheck != 0) {
    return;
  }
  
  appendLog("Checking for OTA updates...");
  
  HTTPClient http;
  http.begin(otaVersionUrl);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();
    
    appendLog("Current version: " + currentVersion);
    appendLog("Latest version: " + latestVersion);
    
    if (latestVersion != currentVersion) {
      appendLog("New version available! Starting update...");
      
      // Stop irrigation during update
      stopIrrigation();
      
      WiFiClient client;
      // httpUpdate.setLedPin(2, LOW);  // Uncomment if you want LED indication on GPIO 2
      
      t_httpUpdate_return ret = httpUpdate.update(client, otaUpdateUrl);
      
      switch (ret) {
        case HTTP_UPDATE_FAILED:
          appendLog("HTTP update failed: " + httpUpdate.getLastErrorString());
          break;
          
        case HTTP_UPDATE_NO_UPDATES:
          appendLog("No updates available");
          break;
          
        case HTTP_UPDATE_OK:
          appendLog("Update successful! Restarting...");
          ESP.restart();
          break;
      }
    } else {
      appendLog("Firmware is up to date");
    }
  } else {
    appendLog("Failed to check for updates: HTTP " + String(httpCode));
  }
  
  http.end();
  lastOTACheck = millis();
}

// ------------------ OTA SETUP ------------------
void setupOTA() {
  // Setup mDNS
  if (MDNS.begin("SmartIrrigation")) {
    appendLog("mDNS responder started: SmartIrrigation.local");
    MDNS.addService("http", "tcp", 80);
  }
  
  // Setup traditional Arduino OTA for local network
  ArduinoOTA.setHostname("SmartIrrigation");
  ArduinoOTA.setPassword("irrigation123");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    appendLog("OTA Update Started: " + type);
    // Stop all operations during update
    stopIrrigation();
  });

  ArduinoOTA.onEnd([]() {
    appendLog("OTA Update Complete");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    String progressStr = "OTA Progress: " + String((progress / (total / 100))) + "%";
    Serial.println(progressStr);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    String errorMsg = "OTA Error[" + String(error) + "]: ";
    if (error == OTA_AUTH_ERROR) errorMsg += "Auth Failed";
    else if (error == OTA_BEGIN_ERROR) errorMsg += "Begin Failed";
    else if (error == OTA_CONNECT_ERROR) errorMsg += "Connect Failed";
    else if (error == OTA_RECEIVE_ERROR) errorMsg += "Receive Failed";
    else if (error == OTA_END_ERROR) errorMsg += "End Failed";
    appendLog(errorMsg);
  });

  ArduinoOTA.begin();
  appendLog("Local OTA Ready - Host: SmartIrrigation.local");
  appendLog("Internet OTA enabled - Checking every hour");
  
  // Initial check for updates
  checkForOTAUpdate();
}

// ------------------ SCHEDULE FETCH ------------------
bool fetchSchedules() {
  if(!wifiConfigured) {
    appendLog("WiFi not configured - using local schedules");
    return false;
  }
  
  if(!timeConfigured) {
    appendLog("Time not configured - using local schedules");
    return false;
  }

  if (millis() - lastScheduleFetch < SCHEDULE_FETCH_INTERVAL && lastScheduleFetch != 0) return true;

  appendLog("Fetching schedules from: " + String(scheduleUrl));
  HTTPClient http;
  http.begin(scheduleUrl);
  http.setTimeout(10000);
  http.addHeader("User-Agent", "ESP32-SmartIrrigation");
  
  int code = http.GET();
  appendLog("HTTP response code: " + String(code));
  
  if(code == 200) {
    String payload = http.getString();
    appendLog("Schedule response length: " + String(payload.length()));

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if(error) {
      appendLog("JSON parse error: " + String(error.c_str()));
      http.end();
      return false;
    }

    // Backup current local schedules
    int localScheduleCount = scheduleCount;
    Schedule localSchedules[MAX_SCHEDULES];
    for(int i = 0; i < scheduleCount; i++) {
      localSchedules[i] = schedules[i];
    }

    scheduleCount = 0;

    if(doc.containsKey("schedules") && doc["schedules"].is<JsonArray>()) {
      JsonArray arr = doc["schedules"];
      appendLog("Found schedules array with " + String(arr.size()) + " items");
      for(JsonObject obj : arr) {
        if(scheduleCount >= MAX_SCHEDULES) break;
        addScheduleFromJson(obj);
      }
    } else if(doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      appendLog("Found root array with " + String(arr.size()) + " items");
      for(JsonObject obj : arr) {
        if(scheduleCount >= MAX_SCHEDULES) break;
        addScheduleFromJson(obj);
      }
    } else if(doc.containsKey("startTime") && doc.containsKey("endTime")) {
      appendLog("Found single schedule object");
      addScheduleFromJson(doc.as<JsonObject>());
    }

    // If server schedules found, merge with local schedules
    if(scheduleCount > 0) {
      // Add local schedules that don't conflict with server schedules
      for(int i = 0; i < localScheduleCount && scheduleCount < MAX_SCHEDULES; i++) {
        if(localSchedules[i].name.indexOf("Local") >= 0) {
          schedules[scheduleCount] = localSchedules[i];
          scheduleCount++;
        }
      }
    } else {
      // No server schedules, restore local schedules
      for(int i = 0; i < localScheduleCount; i++) {
        schedules[i] = localSchedules[i];
      }
      scheduleCount = localScheduleCount;
      appendLog("No server schedules found, using local schedules");
    }

    appendLog("Total active schedules: " + String(scheduleCount));
    http.end();
    lastScheduleFetch = millis();
    return true;

  } else {
    appendLog("Failed to fetch server schedules, using local schedules");
  }
  
  http.end();
  lastScheduleFetch = millis();
  return false;
}

void addScheduleFromJson(JsonObject obj) {
  String start = obj["startTime"] | "";
  String end   = obj["endTime"] | "";
  if(start == "" || end == "") return;

  sscanf(start.c_str(), "%d:%d", &schedules[scheduleCount].startHour, &schedules[scheduleCount].startMinute);
  sscanf(end.c_str(), "%d:%d", &schedules[scheduleCount].endHour, &schedules[scheduleCount].endMinute);
  schedules[scheduleCount].running = false;
  schedules[scheduleCount].completed = false;
  
  // Fix for ArduinoJson compatibility issue
  if(obj.containsKey("name")) {
    schedules[scheduleCount].name = obj["name"].as<String>();
  } else {
    schedules[scheduleCount].name = "Server Schedule " + String(scheduleCount + 1);
  }
  
  schedules[scheduleCount].enabled = obj["enabled"] | true;
  scheduleCount++;
}

// ------------------ FLOW & RELAY ------------------
void startIrrigation() {
  if (!relayState) {
    relayState = true;
    digitalWrite(RELAY_PIN, LOW);
    scheduleLitres = 0;
    appendLog("Water flow STARTED at: " + getCurrentTimeString());
    lastFlowCheck = millis();
    pulseCount = 0;
  }
}

void stopIrrigation() {
  if (relayState) {
    relayState = false;
    digitalWrite(RELAY_PIN, HIGH);
    appendLog("Water flow STOPPED at: " + getCurrentTimeString());
    appendLog("Water used this schedule: " + String(scheduleLitres, 3) + " L");

    // Send water data
    if(wifiConfigured && WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(waterUrl);
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(15000);
      
      DynamicJsonDocument doc(512);
      doc["timestamp"] = getCurrentDateString() + " " + getCurrentTimeString();
      doc["dischargedLiters"] = scheduleLitres;
      doc["waterUsed"] = scheduleLitres;
      doc["dailyTotal"] = dailyTotalLitres + totalLitres;
      
      String postData;
      serializeJson(doc, postData);
      
      int code = http.POST(postData);
      if(code == 200) {
        appendLog("Water data sent successfully");
      } else {
        appendLog("Failed to send water data: " + String(code));
      }
      http.end();
    }
  }
}

// ------------------ WEB INTERFACE ------------------
String getConfigHTML() {
  String page = "<!DOCTYPE html><html><head><title>Smart Irrigation - WiFi Setup</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f0f8f0; }";
  page += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  page += "h1 { color: #2e7d32; text-align: center; }";
  page += "form { margin: 20px 0; }";
  page += "label { display: block; margin: 10px 0 5px; font-weight: bold; }";
  page += "select, input { width: 100%; padding: 10px; margin-bottom: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }";
  page += "button { background-color: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 5px; cursor: pointer; width: 100%; font-size: 16px; }";
  page += "button:hover { background-color: #45a049; }";
  page += ".status { padding: 10px; margin: 10px 0; border-radius: 5px; }";
  page += ".info { background-color: #e3f2fd; border-left: 4px solid #2196F3; }";
  page += ".logs { background-color: #f5f5f5; padding: 15px; border-radius: 5px; height: 200px; overflow-y: auto; font-family: monospace; font-size: 12px; }";
  page += "</style></head><body>";
  
  page += "<div class='container'>";
  page += "<h1>Smart Irrigation System</h1>";
  page += "<h2>WiFi Configuration</h2>";
  
  if (wifiConnecting) {
    page += "<div class='status info'>Connecting to WiFi, please wait...</div>";
    page += "<script>setTimeout(function(){ location.reload(); }, 3000);</script>";
  } else if (wifiConfigured) {
    page += "<div class='status info'>Connected to: " + wifiSSID + "<br>IP: " + WiFi.localIP().toString() + "</div>";
    page += "<p><a href='/dashboard' style='background-color: #2196F3; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px;'>Go to Dashboard</a></p>";
  } else {
    page += "<form method='post' action='/connect'>";
    page += "<label for='wifi_select'>Select Network:</label>";
    page += "<select name='ssid' id='wifi_select' onchange='toggleManual()'>";
    page += "<option value=''>-- Scan for Networks --</option>";
    page += scanNetworks();
    page += "<option value='manual'>Enter manually</option>";
    page += "</select>";
    
    page += "<div id='manual_ssid' style='display:none;'>";
    page += "<label for='manual_ssid_input'>Network Name (SSID):</label>";
    page += "<input type='text' name='manual_ssid' id='manual_ssid_input' placeholder='Enter WiFi network name'>";
    page += "</div>";
    
    page += "<label for='password'>Password:</label>";
    page += "<input type='password' name='pass' id='password' placeholder='Enter WiFi password' required>";
    page += "<button type='submit'>Connect to WiFi</button>";
    page += "</form>";
    
    page += "<script>";
    page += "function toggleManual() {";
    page += "  var select = document.getElementById('wifi_select');";
    page += "  var manual = document.getElementById('manual_ssid');";
    page += "  if(select.value === 'manual') {";
    page += "    manual.style.display = 'block';";
    page += "  } else {";
    page += "    manual.style.display = 'none';";
    page += "  }";
    page += "}";
    page += "</script>";
  }
  
  page += "<h3>System Logs</h3>";
  page += "<div class='logs'>" + logBuffer + "</div>";
  page += "</div>";
  page += "</body></html>";
  return page;
}

String getDashboardHTML() {
  String page = "<!DOCTYPE html><html><head><title>Smart Irrigation - Dashboard</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<meta http-equiv='refresh' content='30'>";
  page += "<style>";
  page += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f0f8f0; }";
  page += ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  page += "h1 { color: #2e7d32; text-align: center; }";
  page += ".status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin: 20px 0; }";
  page += ".status-card { background: #f8f9fa; padding: 15px; border-radius: 8px; border-left: 4px solid #4CAF50; }";
  page += ".status-card h3 { margin: 0 0 10px; color: #2e7d32; }";
  page += ".pump-running { border-left-color: #ff9800; }";
  page += ".pump-stopped { border-left-color: #9e9e9e; }";
  page += ".logs { background-color: #f5f5f5; padding: 15px; border-radius: 5px; height: 300px; overflow-y: auto; font-family: monospace; font-size: 12px; }";
  page += ".nav-links { text-align: center; margin: 20px 0; }";
  page += ".nav-links a { background-color: #2196F3; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; margin: 0 5px; }";
  page += ".nav-links a.schedule { background-color: #FF9800; }";
  page += "</style></head><body>";
  
  page += "<div class='container'>";
  page += "<h1>Smart Irrigation Dashboard</h1>";
  
  page += "<div class='nav-links'>";
  page += "<a href='/'>WiFi Config</a>";
  page += "<a href='/dashboard'>Refresh</a>";
  page += "<a href='/schedules' class='schedule'>Manage Schedules</a>";
  page += "</div>";
  
  page += "<div class='status-grid'>";
  page += "<div class='status-card'>";
  page += "<h3>Connection</h3>";
  page += "<p>WiFi: " + wifiSSID + "<br>";
  page += "IP: " + WiFi.localIP().toString() + "<br>";
  page += "Signal: " + String(WiFi.RSSI()) + " dBm</p>";
  page += "</div>";
  
  page += "<div class='status-card'>";
  page += "<h3>Date & Time</h3>";
  page += "<p>Date: " + getCurrentDateString() + "<br>";
  page += "Time: " + getCurrentTimeString() + "</p>";
  page += "</div>";
  
  page += "<div class='status-card " + String(relayState ? "pump-running" : "pump-stopped") + "'>";
  page += "<h3>Pump Status</h3>";
  page += "<p>" + String(relayState ? "RUNNING" : "STOPPED") + "</p>";
  page += "</div>";
  
  page += "<div class='status-card'>";
  page += "<h3>Water Usage</h3>";
  page += "<p>Schedule: " + String(scheduleLitres, 2) + " L<br>";
  page += "Total: " + String(totalLitres, 2) + " L<br>";
  page += "Daily: " + String(dailyTotalLitres, 2) + " L</p>";
  page += "</div>";
  page += "</div>";

  page += "<h3>Active Schedules (" + String(scheduleCount) + ")</h3>";
  if(scheduleCount > 0) {
    for(int i=0;i<scheduleCount;i++){
      String status = "";
      if(!schedules[i].enabled) status = " (DISABLED)";
      else if(schedules[i].running) status = " (RUNNING)";
      else if(schedules[i].completed) status = " (COMPLETED)";
      
      page += "<p><strong>" + schedules[i].name + "</strong>: "+
        String(schedules[i].startHour)+":"+String(schedules[i].startMinute<10?"0":"")+String(schedules[i].startMinute)+
        " - "+
        String(schedules[i].endHour)+":"+String(schedules[i].endMinute<10?"0":"")+String(schedules[i].endMinute)+
        status + "</p>";
    }
  } else {
    page += "<p>No schedules loaded</p>";
  }

  page += "<h3>System Logs</h3>";
  page += "<div class='logs'>" + logBuffer + "</div>";
  
  page += "<div style='margin-top: 20px; padding: 10px; background-color: #e8f5e8; border-radius: 5px;'>";
  page += "<h4>OTA Update Info</h4>";
  page += "<p><strong>Local Network:</strong><br>";
  page += "Hostname: SmartIrrigation.local<br>";
  page += "For Arduino IDE: Use 'SmartIrrigation' in network ports<br>";
  page += "Password: irrigation123</p>";
  page += "<p><strong>Internet Updates:</strong><br>";
  page += "Current Version: " + currentVersion + "<br>";
  page += "Auto-check: Every hour<br>";
  String otaStatus = wifiConfigured ? "Enabled" : "Disabled - WiFi Required";
  page += "Status: " + otaStatus + "</p>";
  page += "</div>";
  
  page += "</div>";
  page += "</body></html>";
  return page;
}

String getScheduleManagerHTML() {
  String page = "<!DOCTYPE html><html><head><title>Smart Irrigation - Schedule Manager</title>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<style>";
  page += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f0f8f0; }";
  page += ".container { max-width: 900px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  page += "h1 { color: #2e7d32; text-align: center; }";
  page += ".nav-links { text-align: center; margin: 20px 0; }";
  page += ".nav-links a { background-color: #2196F3; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; margin: 0 5px; }";
  page += ".add-form { background: #f8f9fa; padding: 20px; border-radius: 8px; margin: 20px 0; border-left: 4px solid #4CAF50; }";
  page += ".schedule-list { margin: 20px 0; }";
  page += ".schedule-item { background: #ffffff; border: 1px solid #ddd; border-radius: 8px; padding: 15px; margin: 10px 0; display: flex; justify-content: space-between; align-items: center; }";
  page += ".schedule-item.disabled { background: #f5f5f5; border-color: #ccc; }";
  page += ".schedule-item.running { border-left: 4px solid #ff9800; }";
  page += ".schedule-info { flex-grow: 1; }";
  page += ".schedule-actions { display: flex; gap: 10px; }";
  page += "input, select { padding: 8px; margin: 5px; border: 1px solid #ddd; border-radius: 4px; }";
  page += "button { background-color: #4CAF50; color: white; padding: 8px 16px; border: none; border-radius: 4px; cursor: pointer; }";
  page += "button.delete { background-color: #f44336; }";
  page += "button.toggle { background-color: #ff9800; }";
  page += "button:hover { opacity: 0.9; }";
  page += ".form-group { display: inline-block; margin: 5px; }";
  page += ".time-input { width: 70px; }";
  page += ".name-input { width: 150px; }";
  page += "</style></head><body>";
  
  page += "<div class='container'>";
  page += "<h1>Schedule Manager</h1>";
  
  page += "<div class='nav-links'>";
  page += "<a href='/dashboard'>Back to Dashboard</a>";
  page += "<a href='/'>WiFi Config</a>";
  page += "</div>";
  
  // Add new schedule form
  page += "<div class='add-form'>";
  page += "<h3>Add New Schedule</h3>";
  page += "<form method='post' action='/add_schedule'>";
  page += "<div class='form-group'>";
  page += "<label>Name:</label><br>";
  page += "<input type='text' name='name' class='name-input' placeholder='Schedule name' required>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>Start Time:</label><br>";
  page += "<input type='number' name='start_hour' class='time-input' min='0' max='23' placeholder='HH' required>";
  page += " : ";
  page += "<input type='number' name='start_minute' class='time-input' min='0' max='59' placeholder='MM' required>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<label>End Time:</label><br>";
  page += "<input type='number' name='end_hour' class='time-input' min='0' max='23' placeholder='HH' required>";
  page += " : ";
  page += "<input type='number' name='end_minute' class='time-input' min='0' max='59' placeholder='MM' required>";
  page += "</div>";
  page += "<div class='form-group'>";
  page += "<button type='submit'>Add Schedule</button>";
  page += "</div>";
  page += "</form>";
  page += "</div>";
  
  // Current schedules
  page += "<h3>Current Schedules (" + String(scheduleCount) + ")</h3>";
  page += "<div class='schedule-list'>";
  
  if(scheduleCount > 0) {
    for(int i = 0; i < scheduleCount; i++) {
      String itemClass = "schedule-item";
      if(!schedules[i].enabled) itemClass += " disabled";
      if(schedules[i].running) itemClass += " running";
      
      page += "<div class='" + itemClass + "'>";
      page += "<div class='schedule-info'>";
      page += "<strong>" + schedules[i].name + "</strong><br>";
      page += "Time: " + String(schedules[i].startHour) + ":" + 
              (schedules[i].startMinute < 10 ? "0" : "") + String(schedules[i].startMinute) + 
              " - " + String(schedules[i].endHour) + ":" + 
              (schedules[i].endMinute < 10 ? "0" : "") + String(schedules[i].endMinute);
      
      String status = "";
      if(!schedules[i].enabled) status = " • DISABLED";
      else if(schedules[i].running) status = " • RUNNING";
      else if(schedules[i].completed) status = " • COMPLETED TODAY";
      
      page += "<span style='color: #666;'>" + status + "</span>";
      page += "</div>";
      
      page += "<div class='schedule-actions'>";
      page += "<form method='post' action='/toggle_schedule' style='display: inline;'>";
      page += "<input type='hidden' name='index' value='" + String(i) + "'>";
      page += "<button type='submit' class='toggle'>" + String(schedules[i].enabled ? "Disable" : "Enable") + "</button>";
      page += "</form>";
      
      // Only allow deletion of local schedules
      if(schedules[i].name.indexOf("Local") >= 0 || schedules[i].name.indexOf("Server") < 0) {
        page += "<form method='post' action='/delete_schedule' style='display: inline;' ";
        page += "onsubmit='return confirm(\"Are you sure you want to delete this schedule?\");'>";
        page += "<input type='hidden' name='index' value='" + String(i) + "'>";
        page += "<button type='submit' class='delete'>Delete</button>";
        page += "</form>";
      }
      page += "</div>";
      page += "</div>";
    }
  } else {
    page += "<p>No schedules configured. Add a schedule above to get started.</p>";
  }
  
  page += "</div>";
  
  page += "<div style='margin-top: 30px; padding: 15px; background-color: #e3f2fd; border-radius: 5px;'>";
  page += "<h4>Notes:</h4>";
  page += "<ul>";
  page += "<li>Local schedules are stored on the device and persist after restart</li>";
  page += "<li>Server schedules (if any) are fetched from the remote server</li>";
  page += "<li>You can disable schedules temporarily without deleting them</li>";
  page += "<li>Time format is 24-hour (0-23 hours, 0-59 minutes)</li>";
  page += "<li>Schedules reset their completion status daily at midnight</li>";
  page += "</ul>";
  page += "</div>";
  
  page += "</div>";
  page += "</body></html>";
  return page;
}

void handleRoot() {
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    server.sendHeader("Location", "/dashboard", true);
    server.send(302, "text/plain", "Redirecting to dashboard...");
  } else {
    server.send(200, "text/html", getConfigHTML());
  }
}

void handleDashboard() {
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    server.send(200, "text/html", getDashboardHTML());
  } else {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Redirecting to WiFi config...");
  }
}

void handleScheduleManager() {
  server.send(200, "text/html", getScheduleManagerHTML());
}

void handleAddSchedule() {
  if (server.method() == HTTP_POST) {
    if(scheduleCount >= MAX_SCHEDULES) {
      appendLog("Cannot add schedule: Maximum limit reached (" + String(MAX_SCHEDULES) + ")");
      server.sendHeader("Location", "/schedules", true);
      server.send(302, "text/plain", "Maximum schedules reached");
      return;
    }
    
    String name = server.arg("name");
    int startHour = server.arg("start_hour").toInt();
    int startMinute = server.arg("start_minute").toInt();
    int endHour = server.arg("end_hour").toInt();
    int endMinute = server.arg("end_minute").toInt();
    
    // Validate input
    if(name.length() == 0 || startHour < 0 || startHour > 23 || startMinute < 0 || startMinute > 59 ||
       endHour < 0 || endHour > 23 || endMinute < 0 || endMinute > 59) {
      appendLog("Invalid schedule parameters provided");
      server.sendHeader("Location", "/schedules", true);
      server.send(302, "text/plain", "Invalid parameters");
      return;
    }
    
    // Add "Local" prefix to distinguish from server schedules
    if(name.indexOf("Local") < 0) {
      name = "Local " + name;
    }
    
    // Add new schedule
    schedules[scheduleCount].name = name;
    schedules[scheduleCount].startHour = startHour;
    schedules[scheduleCount].startMinute = startMinute;
    schedules[scheduleCount].endHour = endHour;
    schedules[scheduleCount].endMinute = endMinute;
    schedules[scheduleCount].enabled = true;
    schedules[scheduleCount].running = false;
    schedules[scheduleCount].completed = false;
    
    scheduleCount++;
    
    // Save to preferences
    saveSchedulesToPreferences();
    
    appendLog("Added new schedule: " + name + " (" + 
             String(startHour) + ":" + (startMinute < 10 ? "0" : "") + String(startMinute) + 
             " - " + String(endHour) + ":" + (endMinute < 10 ? "0" : "") + String(endMinute) + ")");
  }
  
  server.sendHeader("Location", "/schedules", true);
  server.send(302, "text/plain", "Schedule added");
}

void handleDeleteSchedule() {
  if (server.method() == HTTP_POST) {
    int index = server.arg("index").toInt();
    
    if(index >= 0 && index < scheduleCount) {
      String deletedName = schedules[index].name;
      
      // Shift schedules down
      for(int i = index; i < scheduleCount - 1; i++) {
        schedules[i] = schedules[i + 1];
      }
      scheduleCount--;
      
      // Save to preferences
      saveSchedulesToPreferences();
      
      appendLog("Deleted schedule: " + deletedName);
    } else {
      appendLog("Invalid schedule index for deletion: " + String(index));
    }
  }
  
  server.sendHeader("Location", "/schedules", true);
  server.send(302, "text/plain", "Schedule deleted");
}

void handleToggleSchedule() {
  if (server.method() == HTTP_POST) {
    int index = server.arg("index").toInt();
    
    if(index >= 0 && index < scheduleCount) {
      schedules[index].enabled = !schedules[index].enabled;
      
      // Save to preferences
      saveSchedulesToPreferences();
      
      String action = schedules[index].enabled ? "enabled" : "disabled";
      appendLog("Schedule '" + schedules[index].name + "' " + action);
    } else {
      appendLog("Invalid schedule index for toggle: " + String(index));
    }
  }
  
  server.sendHeader("Location", "/schedules", true);
  server.send(302, "text/plain", "Schedule toggled");
}

void handleConnect() {
  if (server.method() == HTTP_POST) {
    String newSSID = server.arg("ssid");
    String manualSSID = server.arg("manual_ssid");
    String newPASS = server.arg("pass");

    if (manualSSID.length() > 0) newSSID = manualSSID;

    if (newSSID.length() > 0 && newPASS.length() > 0) {
      appendLog("Attempting connection to: " + newSSID);
      wifiSSID = newSSID;
      wifiPASS = newPASS;
      connectToWiFi();
      
      if (wifiConfigured) {
        preferences.begin("wifi", false);
        preferences.putString("ssid", wifiSSID);
        preferences.putString("pass", wifiPASS);
        preferences.end();
        appendLog("WiFi credentials saved successfully");
      } else {
        appendLog("WiFi connection failed - Check password");
        wifiSSID = "";
        wifiPASS = "";
      }
    } else {
      appendLog("Invalid WiFi credentials provided");
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Redirecting...");
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Page not found");
}

// ------------------ SETUP ------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  appendLog("ESP32 Smart Irrigation System Starting...");
  appendLog("Firmware Version: " + currentVersion);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), countPulse, RISING);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID, apPassword);
  appendLog("Access Point created: " + String(apSSID));
  appendLog("Connect to WiFi '" + String(apSSID) + "' (password: " + String(apPassword) + ")");
  appendLog("Then open: http://192.168.4.1");

  // Load saved WiFi credentials and schedules
  preferences.begin("wifi", false);
  wifiSSID = preferences.getString("ssid", "");
  wifiPASS = preferences.getString("pass", "");
  preferences.end();

  // Load saved schedules
  loadSchedulesFromPreferences();

  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleDashboard);
  server.on("/schedules", HTTP_GET, handleScheduleManager);
  server.on("/add_schedule", HTTP_POST, handleAddSchedule);
  server.on("/delete_schedule", HTTP_POST, handleDeleteSchedule);
  server.on("/toggle_schedule", HTTP_POST, handleToggleSchedule);
  server.on("/connect", HTTP_POST, handleConnect);
  server.onNotFound(handleNotFound);
  server.begin();
  appendLog("Web server started");

  if(wifiSSID.length() > 0){
    appendLog("Found saved WiFi credentials, attempting connection...");
    connectToWiFi();
    if(wifiConfigured) {
      delay(2000);
      fetchSchedules(); // This will merge server schedules with local ones
    }
  }

  appendLog("System ready!");
  appendLog("=====================================");
}

// ------------------ MAIN LOOP ------------------
void loop() {
  server.handleClient();
  
  // Handle OTA updates when WiFi is connected
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    
    // Check for internet-based OTA updates
    checkForOTAUpdate();
  }
  
  // Fetch schedules periodically (merges with local schedules)
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    fetchSchedules();
  }
  
  // Irrigation scheduling logic
  if (timeConfigured && scheduleCount > 0) {
    int currentHour, currentMinute, currentSecond;
    getCurrentTime(currentHour, currentMinute, currentSecond);
    
    // Debug current time every 30 seconds
    static unsigned long lastTimeLog = 0;
    if(millis() - lastTimeLog > 30000) {
      appendLog("Current time: " + getCurrentTimeString() + " - Checking " + String(scheduleCount) + " schedules");
      lastTimeLog = millis();
    }
    
    // Check schedules
    for (int i = 0; i < scheduleCount; i++) {
      // Skip disabled schedules
      if(!schedules[i].enabled) continue;
      
      bool shouldRun = false;
      
      // Check if current time is within schedule
      if (schedules[i].startHour == schedules[i].endHour) {
        // Same hour
        if (currentHour == schedules[i].startHour &&
            currentMinute >= schedules[i].startMinute &&
            currentMinute < schedules[i].endMinute) {
          shouldRun = true;
        }
      } else {
        // Different hours
        if ((currentHour == schedules[i].startHour && currentMinute >= schedules[i].startMinute) ||
            (currentHour > schedules[i].startHour && currentHour < schedules[i].endHour) ||
            (currentHour == schedules[i].endHour && currentMinute < schedules[i].endMinute)) {
          shouldRun = true;
        }
      }
      
      if (shouldRun && !schedules[i].running && !schedules[i].completed) {
        schedules[i].running = true;
        startIrrigation();
        appendLog("Started irrigation for: " + schedules[i].name + " (" + 
                 String(schedules[i].startHour) + ":" + 
                 (schedules[i].startMinute<10?"0":"") + String(schedules[i].startMinute) + 
                 "-" + String(schedules[i].endHour) + ":" + 
                 (schedules[i].endMinute<10?"0":"") + String(schedules[i].endMinute) + ")");
      } else if (!shouldRun && schedules[i].running) {
        schedules[i].running = false;
        schedules[i].completed = true;
        stopIrrigation();
        appendLog("Completed irrigation for: " + schedules[i].name);
      }
    }
  } else if (scheduleCount > 0 && !timeConfigured) {
    // Work with local schedules even without NTP time (using millis-based time)
    static unsigned long lastMillisTimeLog = 0;
    if(millis() - lastMillisTimeLog > 60000) { // Every minute
      appendLog("Working with local schedules (NTP time not available)");
      lastMillisTimeLog = millis();
    }
  }
  
  // Handle flow sensor reading
  if (millis() - lastFlowCheck >= FLOW_INTERVAL) {
    if (pulseCount > 0) {
      // Calculate flow rate (adjust based on your flow sensor specifications)
      float flowRate = (pulseCount * 2.25) / 1000.0; // Convert pulses to liters
      totalLitres += flowRate;
      scheduleLitres += flowRate;
      
      appendLog("Flow: " + String(flowRate, 3) + " L (" + String(pulseCount) + " pulses)");
      pulseCount = 0;
    }
    lastFlowCheck = millis();
  }
  
  // Reset daily total at midnight
  int currentHour, currentMinute, currentSecond;
  int year, month, day;
  getCurrentTime(currentHour, currentMinute, currentSecond);
  getCurrentDate(year, month, day);
  
  if (day != currentDay && timeConfigured) {
    if (currentDay != 0) {
      dailyTotalLitres = totalLitres;
      totalLitres = 0;
      appendLog("Daily reset - Previous day total: " + String(dailyTotalLitres, 2) + " L");
    }
    currentDay = day;
    
    // Reset schedule completion flags
    for (int i = 0; i < scheduleCount; i++) {
      schedules[i].completed = false;
    }
    
    // Save schedules after daily reset
    saveSchedulesToPreferences();
  }
  
  delay(100);
}