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

// ------------------ DEVICE CONFIG ------------------
String deviceId = "";
String deviceSecret = "";
String deviceToken = "";
bool deviceConfigured = false;
bool deviceAuthenticated = false;

// ------------------ OTA CONFIG ------------------
const char* otaUpdateUrl = "https://raw.githubusercontent.com/Bhoomika-1233/esp32-ota-test/refs/heads/main/ota.txt";
const char* otaVersionUrl = "https://raw.githubusercontent.com/Bhoomika-1233/esp32-ota-test/refs/heads/main/webshed.ino";
String currentVersion = "1.0.3";
unsigned long lastOTACheck = 0;
const unsigned long OTA_CHECK_INTERVAL = 3600000;

// ------------------ LOGS ------------------
String logBuffer = "";
void appendLog(String msg) {
  String timestamp = "[" + String(millis()/1000) + "s] ";
  String fullMsg = timestamp + msg;
  Serial.println(fullMsg);
  logBuffer += fullMsg + "<br>";
  if (logBuffer.length() > 8000) logBuffer = logBuffer.substring(3000);
}

// ------------------ SERVER CONFIG ------------------
String serverUrl = "http://192.168.68.115:4000";
String deviceLoginUrl = "/api/device/login";
String deviceScheduleUrl = "/api/device/schedule";
String waterUrl = "/api/water";

// ------------------ NTP CONFIG ------------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
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
const unsigned long FLOW_INTERVAL = 10000;
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
  String name;
  bool enabled;
};
#define MAX_SCHEDULES 10
Schedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
unsigned long lastScheduleFetch = 0;
const unsigned long SCHEDULE_FETCH_INTERVAL = 300000;

// ------------------ FUNCTION PROTOTYPES ------------------
String getCurrentTimeString();
String getCurrentDateString();
void connectToWiFi();
void syncTime();
bool authenticateDevice();
bool fetchSchedules();
void addScheduleFromJson(JsonObject obj);
void startIrrigation();
void stopIrrigation();
String getConfigHTML();
String getDeviceConfigHTML();
String getDashboardHTML();
String getScheduleManagerHTML();
void handleRoot();
void handleDashboard();
void handleConnect();
void handleDeviceConfig();
void handleSaveDevice();
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

// ------------------ SCHEDULE MANAGEMENT ------------------
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
  appendLog("Schedules saved");
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
    appendLog("Loaded " + String(scheduleCount) + " schedules");
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

// ------------------ WIFI SCANNER ------------------
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
      appendLog("NTP synced: " + getCurrentDateString() + " " + getCurrentTimeString());
      return;
    }
    attempts++;
  }
  appendLog("NTP sync failed");
}

// ------------------ WIFI MANAGEMENT ------------------
void connectToWiFi() {
  wifiConnecting = true;
  WiFi.disconnect();
  delay(1000);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  
  appendLog("Connecting to: " + wifiSSID);
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
    appendLog("IP: " + WiFi.localIP().toString());
    appendLog("Signal: " + String(WiFi.RSSI()) + " dBm");
    syncTime();
    setupOTA();
    
    if(deviceConfigured && !deviceAuthenticated) {
      authenticateDevice();
    }
  } else {
    appendLog("WiFi failed: " + wifiSSID);
    wifiConfigured = false;
  }
}

// ------------------ DEVICE AUTHENTICATION ------------------
bool authenticateDevice() {
  if(!wifiConfigured || WiFi.status() != WL_CONNECTED) {
    appendLog("WiFi not connected");
    return false;
  }
  
  if(!deviceConfigured) {
    appendLog("Device not configured");
    return false;
  }

  appendLog("Authenticating device...");
  HTTPClient http;
  http.begin(serverUrl + deviceLoginUrl);
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  
  DynamicJsonDocument doc(256);
  doc["deviceId"] = deviceId;
  doc["deviceSecret"] = deviceSecret;
  
  String postData;
  serializeJson(doc, postData);
  
  int code = http.POST(postData);
  appendLog("Auth response: " + String(code));
  
  if(code == 200) {
    String payload = http.getString();
    
    DynamicJsonDocument respDoc(512);
    DeserializationError error = deserializeJson(respDoc, payload);
    
    if(error) {
      appendLog("JSON error: " + String(error.c_str()));
      http.end();
      return false;
    }
    
    if(respDoc.containsKey("token")) {
      deviceToken = respDoc["token"].as<String>();
      deviceAuthenticated = true;
      appendLog("Device authenticated!");
      appendLog("Token: " + deviceToken.substring(0, 20) + "...");
      http.end();
      return true;
    } else {
      appendLog("No token in response");
    }
  } else {
    String errorMsg = http.getString();
    appendLog("Auth failed: " + errorMsg);
  }
  
  http.end();
  deviceAuthenticated = false;
  return false;
}

// ------------------ OTA UPDATE ------------------
void checkForOTAUpdate() {
  if (!wifiConfigured || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastOTACheck < OTA_CHECK_INTERVAL && lastOTACheck != 0) return;
  
  appendLog("Checking OTA updates...");
  
  HTTPClient http;
  http.begin(otaVersionUrl);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();
    
    appendLog("Current: " + currentVersion);
    appendLog("Latest: " + latestVersion);
    
    if (latestVersion != currentVersion) {
      appendLog("New version! Updating...");
      stopIrrigation();
      
      WiFiClient client;
      t_httpUpdate_return ret = httpUpdate.update(client, otaUpdateUrl);
      
      switch (ret) {
        case HTTP_UPDATE_FAILED:
          appendLog("Update failed: " + httpUpdate.getLastErrorString());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          appendLog("No updates");
          break;
        case HTTP_UPDATE_OK:
          appendLog("Update OK! Restarting...");
          ESP.restart();
          break;
      }
    } else {
      appendLog("Firmware up to date");
    }
  } else {
    appendLog("Update check failed: " + String(httpCode));
  }
  
  http.end();
  lastOTACheck = millis();
}

// ------------------ OTA SETUP ------------------
void setupOTA() {
  if (MDNS.begin("SmartIrrigation")) {
    appendLog("mDNS: SmartIrrigation.local");
    MDNS.addService("http", "tcp", 80);
  }
  
  ArduinoOTA.setHostname("SmartIrrigation");
  ArduinoOTA.setPassword("irrigation123");

  ArduinoOTA.onStart([]() {
    appendLog("OTA Start");
    stopIrrigation();
  });

  ArduinoOTA.onEnd([]() {
    appendLog("OTA Complete");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.println("OTA: " + String((progress / (total / 100))) + "%");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    appendLog("OTA Error: " + String(error));
  });

  ArduinoOTA.begin();
  appendLog("OTA Ready");
  checkForOTAUpdate();
}

// ------------------ SCHEDULE FETCH ------------------
bool fetchSchedules() {
  if(!deviceAuthenticated) {
    appendLog("Not authenticated");
    return false;
  }
  
  if(!timeConfigured) {
    appendLog("Time not configured");
    return false;
  }

  if (millis() - lastScheduleFetch < SCHEDULE_FETCH_INTERVAL && lastScheduleFetch != 0) return true;

  appendLog("Fetching schedules...");
  HTTPClient http;
  http.begin(serverUrl + deviceScheduleUrl);
  http.setTimeout(10000);
  http.addHeader("Authorization", "Bearer " + deviceToken);
  
  int code = http.GET();
  appendLog("Schedule response: " + String(code));
  
  if(code == 200) {
    String payload = http.getString();
    appendLog("Payload length: " + String(payload.length()));

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if(error) {
      appendLog("JSON error: " + String(error.c_str()));
      http.end();
      return false;
    }

    int localScheduleCount = scheduleCount;
    Schedule localSchedules[MAX_SCHEDULES];
    for(int i = 0; i < scheduleCount; i++) {
      localSchedules[i] = schedules[i];
    }

    scheduleCount = 0;

    if(doc.containsKey("schedules") && doc["schedules"].is<JsonArray>()) {
      JsonArray arr = doc["schedules"];
      appendLog("Found " + String(arr.size()) + " schedules");
      for(JsonObject obj : arr) {
        if(scheduleCount >= MAX_SCHEDULES) break;
        addScheduleFromJson(obj);
      }
    }

    if(scheduleCount > 0) {
      for(int i = 0; i < localScheduleCount && scheduleCount < MAX_SCHEDULES; i++) {
        if(localSchedules[i].name.indexOf("Local") >= 0) {
          schedules[scheduleCount] = localSchedules[i];
          scheduleCount++;
        }
      }
    } else {
      for(int i = 0; i < localScheduleCount; i++) {
        schedules[i] = localSchedules[i];
      }
      scheduleCount = localScheduleCount;
      appendLog("Using local schedules");
    }

    appendLog("Total schedules: " + String(scheduleCount));
    http.end();
    lastScheduleFetch = millis();
    return true;

  } else if(code == 401) {
    appendLog("Auth expired - reauth");
    deviceAuthenticated = false;
    authenticateDevice();
  } else {
    appendLog("Fetch failed - using local");
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
  
  if(obj.containsKey("name")) {
    schedules[scheduleCount].name = obj["name"].as<String>();
  } else {
    schedules[scheduleCount].name = "Server " + String(scheduleCount + 1);
  }
  
  schedules[scheduleCount].enabled = obj["enabled"] | true;
  scheduleCount++;
}

// ------------------ IRRIGATION CONTROL ------------------
void startIrrigation() {
  if (!relayState) {
    relayState = true;
    digitalWrite(RELAY_PIN, LOW);
    scheduleLitres = 0;
    appendLog("Irrigation STARTED");
    lastFlowCheck = millis();
    pulseCount = 0;
  }
}

void stopIrrigation() {
  if (relayState) {
    relayState = false;
    digitalWrite(RELAY_PIN, HIGH);
    appendLog("Irrigation STOPPED");
    appendLog("Water used: " + String(scheduleLitres, 3) + " L");

    if(deviceAuthenticated && WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(serverUrl + waterUrl);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", "Bearer " + deviceToken);
      http.setTimeout(15000);
      
      DynamicJsonDocument doc(256);
      doc["dischargedLiters"] = scheduleLitres;
      
      String postData;
      serializeJson(doc, postData);
      
      int code = http.POST(postData);
      if(code == 200) {
        appendLog("Water data sent");
      } else if(code == 401) {
        appendLog("Water auth failed");
        deviceAuthenticated = false;
        authenticateDevice();
      } else {
        appendLog("Water send failed: " + String(code));
      }
      http.end();
    }
  }
}

// ------------------ WEB PAGES ------------------
String getConfigHTML() {
  String page = "<!DOCTYPE html><html><head><title>Smart Irrigation - WiFi</title>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial;margin:20px;background:#f0f8f0}";
  page += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 4px 8px rgba(0,0,0,0.1)}";
  page += "h1{color:#2e7d32;text-align:center}";
  page += "label{display:block;margin:10px 0 5px;font-weight:bold}";
  page += "select,input{width:100%;padding:10px;margin-bottom:10px;border:1px solid #ddd;border-radius:5px;box-sizing:border-box}";
  page += "button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:5px;cursor:pointer;width:100%;font-size:16px}";
  page += "button:hover{background:#45a049}";
  page += ".info{background:#e3f2fd;border-left:4px solid #2196F3;padding:10px;margin:10px 0;border-radius:5px}";
  page += ".logs{background:#f5f5f5;padding:15px;border-radius:5px;height:200px;overflow-y:auto;font-family:monospace;font-size:12px}";
  page += "</style></head><body>";
  
  page += "<div class='container'><h1>Smart Irrigation</h1><h2>WiFi Setup</h2>";
  
  if (wifiConnecting) {
    page += "<div class='info'>Connecting...</div>";
    page += "<script>setTimeout(function(){location.reload()},3000)</script>";
  } else if (wifiConfigured) {
    page += "<div class='info'>Connected: " + wifiSSID + "<br>IP: " + WiFi.localIP().toString() + "</div>";
    page += "<p><a href='/device_config' style='background:#2196F3;color:white;padding:10px 20px;text-decoration:none;border-radius:5px'>Configure Device</a></p>";
  } else {
    page += "<form method='post' action='/connect'>";
    page += "<label>Network:</label><select name='ssid' id='s' onchange='t()'>";
    page += "<option value=''>-- Scan --</option>" + scanNetworks();
    page += "<option value='manual'>Manual</option></select>";
    page += "<div id='m' style='display:none'><label>SSID:</label>";
    page += "<input type='text' name='manual_ssid' placeholder='Network name'></div>";
    page += "<label>Password:</label><input type='password' name='pass' required>";
    page += "<button type='submit'>Connect</button></form>";
    page += "<script>function t(){document.getElementById('m').style.display=document.getElementById('s').value==='manual'?'block':'none'}</script>";
  }
  
  page += "<h3>Logs</h3><div class='logs'>" + logBuffer + "</div>";
  
  page += "<div style='margin-top:20px;padding:15px;background:#e8f5e8;border-radius:5px'>";
  page += "<h4>OTA Update Info</h4>";
  page += "<p><strong>Local Network OTA:</strong><br>";
  page += "Hostname: SmartIrrigation.local<br>";
  page += "Password: irrigation123<br>";
  page += "Use Arduino IDE Network port</p>";
  page += "<p><strong>Internet OTA:</strong><br>";
  page += "Current Version: " + currentVersion + "<br>";
  page += "Auto-check: Every hour<br>";
  page += "Status: " + String(wifiConfigured ? "Enabled" : "Disabled - WiFi Required") + "</p>";
  page += "<p><strong>API Server:</strong><br>" + serverUrl + "</p>";
  page += "</div>";
  
  page += "</div></body></html>";
  return page;
}

String getDeviceConfigHTML() {
  String page = "<!DOCTYPE html><html><head><title>Device Config</title>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial;margin:20px;background:#f0f8f0}";
  page += ".container{max-width:600px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 4px 8px rgba(0,0,0,0.1)}";
  page += "h1{color:#2e7d32;text-align:center}";
  page += "label{display:block;margin:10px 0 5px;font-weight:bold}";
  page += "input{width:100%;padding:10px;margin-bottom:10px;border:1px solid #ddd;border-radius:5px;box-sizing:border-box}";
  page += "button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:5px;cursor:pointer;width:100%;font-size:16px}";
  page += ".info{background:#e3f2fd;border-left:4px solid #2196F3;padding:15px;margin:15px 0;border-radius:5px}";
  page += ".success{background:#e8f5e8;border-left:4px solid #4CAF50;padding:15px;margin:15px 0;border-radius:5px}";
  page += ".warning{background:#fff3e0;border-left:4px solid #FF9800;padding:15px;margin:15px 0;border-radius:5px}";
  page += "</style></head><body>";
  
  page += "<div class='container'><h1>Device Configuration</h1>";
  
  if (deviceAuthenticated) {
    page += "<div class='success'><strong>Authenticated!</strong><br>Device: " + deviceId + "</div>";
    page += "<p><a href='/dashboard' style='background:#2196F3;color:white;padding:10px 20px;text-decoration:none;border-radius:5px'>Dashboard</a></p>";
  } else if (deviceConfigured) {
    page += "<div class='warning'><strong>Not Authenticated</strong><br>Device: " + deviceId + "<br>Check credentials</div>";
    page += "<p><a href='/device_config' style='background:#FF9800;color:white;padding:10px 20px;text-decoration:none;border-radius:5px'>Update</a></p>";
  } else {
    page += "<div class='info'><strong>Setup Device</strong><br>Enter credentials from admin portal</div>";
    page += "<form method='post' action='/save_device'>";
    page += "<label>Device ID:</label><input type='text' name='device_id' placeholder='687b297ca408' required>";
    page += "<label>Secret:</label><input type='password' name='device_secret' required>";
    page += "<label>Server (optional):</label><input type='text' name='server_url' value='" + serverUrl + "'>";
    page += "<button type='submit'>Save & Authenticate</button></form>";
  }
  
  page += "</div></body></html>";
  return page;
}

String getDashboardHTML() {
  String page = "<!DOCTYPE html><html><head><title>Dashboard</title>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<meta http-equiv='refresh' content='30'>";
  page += "<style>";
  page += "body{font-family:Arial;margin:20px;background:#f0f8f0}";
  page += ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 4px 8px rgba(0,0,0,0.1)}";
  page += "h1{color:#2e7d32;text-align:center}";
  page += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:15px;margin:20px 0}";
  page += ".card{background:#f8f9fa;padding:15px;border-radius:8px;border-left:4px solid #4CAF50}";
  page += ".card h3{margin:0 0 10px;color:#2e7d32}";
  page += ".running{border-left-color:#ff9800}";
  page += ".stopped{border-left-color:#9e9e9e}";
  page += ".auth-ok{border-left-color:#4CAF50}";
  page += ".auth-fail{border-left-color:#f44336}";
  page += ".logs{background:#f5f5f5;padding:15px;border-radius:5px;height:300px;overflow-y:auto;font-family:monospace;font-size:12px}";
  page += ".nav{text-align:center;margin:20px 0}";
  page += ".nav a{background:#2196F3;color:white;padding:10px 20px;text-decoration:none;border-radius:5px;margin:0 5px}";
  page += "</style></head><body>";
  
  page += "<div class='container'><h1>Dashboard</h1>";
  page += "<div class='nav'><a href='/'>WiFi</a><a href='/device_config'>Device</a><a href='/schedules'>Schedules</a></div>";
  
  page += "<div class='grid'>";
  page += "<div class='card " + String(deviceAuthenticated?"auth-ok":"auth-fail") + "'><h3>Device</h3>";
  page += "<p>ID: " + (deviceId.length()>0?deviceId:"None") + "<br>Auth: " + String(deviceAuthenticated?"OK":"Failed") + "</p></div>";
  
  page += "<div class='card'><h3>WiFi</h3><p>" + wifiSSID + "<br>" + WiFi.localIP().toString() + "<br>" + WiFi.RSSI() + " dBm</p></div>";
  page += "<div class='card'><h3>Time</h3><p>" + getCurrentDateString() + "<br>" + getCurrentTimeString() + "</p></div>";
  page += "<div class='card " + String(relayState?"running":"stopped") + "'><h3>Pump</h3><p>" + String(relayState?"RUNNING":"STOPPED") + "</p></div>";
  page += "<div class='card'><h3>Water</h3><p>Schedule: " + String(scheduleLitres,2) + "L<br>Total: " + String(totalLitres,2) + "L<br>Daily: " + String(dailyTotalLitres,2) + "L</p></div>";
  page += "</div>";

  page += "<h3>Schedules (" + String(scheduleCount) + ")</h3>";
  if(scheduleCount > 0) {
    for(int i=0;i<scheduleCount;i++){
      String st = schedules[i].running?" (RUNNING)":schedules[i].completed?" (DONE)":schedules[i].enabled?"":" (OFF)";
      page += "<p><strong>" + schedules[i].name + "</strong>: " + 
        String(schedules[i].startHour) + ":" + (schedules[i].startMinute<10?"0":"") + String(schedules[i].startMinute) + 
        " - " + String(schedules[i].endHour) + ":" + (schedules[i].endMinute<10?"0":"") + String(schedules[i].endMinute) + st + "</p>";
    }
  } else {
    page += "<p>No schedules</p>";
  }

  page += "<h3>Logs</h3><div class='logs'>" + logBuffer + "</div></div></body></html>";
  return page;
}

String getScheduleManagerHTML() {
  String page = "<!DOCTYPE html><html><head><title>Schedules</title>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<style>";
  page += "body{font-family:Arial;margin:20px;background:#f0f8f0}";
  page += ".container{max-width:900px;margin:0 auto;background:white;padding:20px;border-radius:10px}";
  page += "h1{color:#2e7d32;text-align:center}";
  page += ".form{background:#f8f9fa;padding:20px;border-radius:8px;margin:20px 0;border-left:4px solid #4CAF50}";
  page += ".item{background:white;border:1px solid #ddd;border-radius:8px;padding:15px;margin:10px 0;display:flex;justify-content:space-between;align-items:center}";
  page += ".item.disabled{background:#f5f5f5}";
  page += ".item.running{border-left:4px solid #ff9800}";
  page += "input,select{padding:8px;margin:5px;border:1px solid #ddd;border-radius:4px}";
  page += "button{background:#4CAF50;color:white;padding:8px 16px;border:none;border-radius:4px;cursor:pointer}";
  page += "button.del{background:#f44336}";
  page += "button.tog{background:#ff9800}";
  page += ".nav{text-align:center;margin:20px 0}";
  page += ".nav a{background:#2196F3;color:white;padding:10px 20px;text-decoration:none;border-radius:5px;margin:0 5px}";
  page += "</style></head><body>";
  
  page += "<div class='container'><h1>Schedule Manager</h1>";
  page += "<div class='nav'><a href='/dashboard'>Dashboard</a><a href='/'>WiFi</a></div>";
  
  page += "<div class='form'><h3>Add Schedule</h3>";
  page += "<form method='post' action='/add_schedule'>";
  page += "<input type='text' name='name' placeholder='Name' required>";
  page += "<input type='number' name='start_hour' min='0' max='23' placeholder='Start H' required>";
  page += "<input type='number' name='start_minute' min='0' max='59' placeholder='Start M' required>";
  page += "<input type='number' name='end_hour' min='0' max='23' placeholder='End H' required>";
  page += "<input type='number' name='end_minute' min='0' max='59' placeholder='End M' required>";
  page += "<button type='submit'>Add</button></form></div>";
  
  page += "<h3>Current Schedules (" + String(scheduleCount) + ")</h3>";
  
  if(scheduleCount > 0) {
    for(int i = 0; i < scheduleCount; i++) {
      String cls = "item";
      if(!schedules[i].enabled) cls += " disabled";
      if(schedules[i].running) cls += " running";
      
      page += "<div class='" + cls + "'><div>";
      page += "<strong>" + schedules[i].name + "</strong><br>";
      page += String(schedules[i].startHour) + ":" + (schedules[i].startMinute<10?"0":"") + String(schedules[i].startMinute);
      page += " - " + String(schedules[i].endHour) + ":" + (schedules[i].endMinute<10?"0":"") + String(schedules[i].endMinute);
      
      String st = "";
      if(!schedules[i].enabled) st = " • OFF";
      else if(schedules[i].running) st = " • RUNNING";
      else if(schedules[i].completed) st = " • DONE";
      page += "<span style='color:#666'>" + st + "</span></div><div>";
      
      page += "<form method='post' action='/toggle_schedule' style='display:inline'>";
      page += "<input type='hidden' name='index' value='" + String(i) + "'>";
      page += "<button type='submit' class='tog'>" + String(schedules[i].enabled?"Disable":"Enable") + "</button></form> ";
      
      if(schedules[i].name.indexOf("Local") >= 0 || schedules[i].name.indexOf("Server") < 0) {
        page += "<form method='post' action='/delete_schedule' style='display:inline' onsubmit='return confirm(\"Delete?\")'>";
        page += "<input type='hidden' name='index' value='" + String(i) + "'>";
        page += "<button type='submit' class='del'>Delete</button></form>";
      }
      page += "</div></div>";
    }
  } else {
    page += "<p>No schedules</p>";
  }
  
  page += "</div></body></html>";
  return page;
}

// ------------------ HANDLERS ------------------
void handleRoot() {
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    if (!deviceConfigured) {
      server.sendHeader("Location", "/device_config", true);
      server.send(302, "text/plain", "");
    } else {
      server.sendHeader("Location", "/dashboard", true);
      server.send(302, "text/plain", "");
    }
  } else {
    server.send(200, "text/html", getConfigHTML());
  }
}

void handleDashboard() {
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    server.send(200, "text/html", getDashboardHTML());
  } else {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  }
}

void handleDeviceConfig() {
  server.send(200, "text/html", getDeviceConfigHTML());
}

void handleSaveDevice() {
  if (server.method() == HTTP_POST) {
    String newId = server.arg("device_id");
    String newSecret = server.arg("device_secret");
    String newServer = server.arg("server_url");
    
    if (newId.length() > 0 && newSecret.length() > 0) {
      deviceId = newId;
      deviceSecret = newSecret;
      
      if(newServer.length() > 0) {
        serverUrl = newServer;
      }
      
      deviceConfigured = true;
      
      preferences.begin("device", false);
      preferences.putString("id", deviceId);
      preferences.putString("secret", deviceSecret);
      preferences.putString("server", serverUrl);
      preferences.end();
      
      appendLog("Device saved: " + deviceId);
      
      if(wifiConfigured && WiFi.status() == WL_CONNECTED) {
        authenticateDevice();
      }
    }
  }
  
  server.sendHeader("Location", "/device_config", true);
  server.send(302, "text/plain", "");
}

void handleConnect() {
  if (server.method() == HTTP_POST) {
    String newSSID = server.arg("ssid");
    String manualSSID = server.arg("manual_ssid");
    String newPASS = server.arg("pass");

    if (manualSSID.length() > 0) newSSID = manualSSID;

    if (newSSID.length() > 0 && newPASS.length() > 0) {
      wifiSSID = newSSID;
      wifiPASS = newPASS;
      connectToWiFi();
      
      if (wifiConfigured) {
        preferences.begin("wifi", false);
        preferences.putString("ssid", wifiSSID);
        preferences.putString("pass", wifiPASS);
        preferences.end();
      }
    }
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleScheduleManager() {
  server.send(200, "text/html", getScheduleManagerHTML());
}

void handleAddSchedule() {
  if (server.method() == HTTP_POST) {
    if(scheduleCount >= MAX_SCHEDULES) {
      appendLog("Max schedules reached");
      server.sendHeader("Location", "/schedules", true);
      server.send(302, "text/plain", "");
      return;
    }
    
    String name = server.arg("name");
    int sh = server.arg("start_hour").toInt();
    int sm = server.arg("start_minute").toInt();
    int eh = server.arg("end_hour").toInt();
    int em = server.arg("end_minute").toInt();
    
    if(name.length() == 0 || sh < 0 || sh > 23 || sm < 0 || sm > 59 || eh < 0 || eh > 23 || em < 0 || em > 59) {
      appendLog("Invalid schedule");
      server.sendHeader("Location", "/schedules", true);
      server.send(302, "text/plain", "");
      return;
    }
    
    if(name.indexOf("Local") < 0) name = "Local " + name;
    
    schedules[scheduleCount].name = name;
    schedules[scheduleCount].startHour = sh;
    schedules[scheduleCount].startMinute = sm;
    schedules[scheduleCount].endHour = eh;
    schedules[scheduleCount].endMinute = em;
    schedules[scheduleCount].enabled = true;
    schedules[scheduleCount].running = false;
    schedules[scheduleCount].completed = false;
    scheduleCount++;
    
    saveSchedulesToPreferences();
    appendLog("Added: " + name);
  }
  
  server.sendHeader("Location", "/schedules", true);
  server.send(302, "text/plain", "");
}

void handleDeleteSchedule() {
  if (server.method() == HTTP_POST) {
    int idx = server.arg("index").toInt();
    
    if(idx >= 0 && idx < scheduleCount) {
      String name = schedules[idx].name;
      
      for(int i = idx; i < scheduleCount - 1; i++) {
        schedules[i] = schedules[i + 1];
      }
      scheduleCount--;
      
      saveSchedulesToPreferences();
      appendLog("Deleted: " + name);
    }
  }
  
  server.sendHeader("Location", "/schedules", true);
  server.send(302, "text/plain", "");
}

void handleToggleSchedule() {
  if (server.method() == HTTP_POST) {
    int idx = server.arg("index").toInt();
    
    if(idx >= 0 && idx < scheduleCount) {
      schedules[idx].enabled = !schedules[idx].enabled;
      saveSchedulesToPreferences();
      appendLog("Toggled: " + schedules[idx].name);
    }
  }
  
  server.sendHeader("Location", "/schedules", true);
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  server.send(404, "text/plain", "404 Not Found");
}

// ------------------ SETUP ------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  appendLog("Smart Irrigation Starting...");
  appendLog("Version: " + currentVersion);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), countPulse, RISING);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID, apPassword);
  appendLog("AP: " + String(apSSID));
  appendLog("IP: 192.168.4.1");

  preferences.begin("wifi", false);
  wifiSSID = preferences.getString("ssid", "");
  wifiPASS = preferences.getString("pass", "");
  preferences.end();

  preferences.begin("device", false);
  deviceId = preferences.getString("id", "");
  deviceSecret = preferences.getString("secret", "");
  String savedServer = preferences.getString("server", "");
  if(savedServer.length() > 0) serverUrl = savedServer;
  preferences.end();
  
  if(deviceId.length() > 0) {
    deviceConfigured = true;
    appendLog("Device: " + deviceId);
  }

  loadSchedulesFromPreferences();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleDashboard);
  server.on("/device_config", HTTP_GET, handleDeviceConfig);
  server.on("/save_device", HTTP_POST, handleSaveDevice);
  server.on("/schedules", HTTP_GET, handleScheduleManager);
  server.on("/add_schedule", HTTP_POST, handleAddSchedule);
  server.on("/delete_schedule", HTTP_POST, handleDeleteSchedule);
  server.on("/toggle_schedule", HTTP_POST, handleToggleSchedule);
  server.on("/connect", HTTP_POST, handleConnect);
  server.onNotFound(handleNotFound);
  server.begin();
  appendLog("Web server started");

  if(wifiSSID.length() > 0){
    appendLog("Connecting to saved WiFi...");
    connectToWiFi();
    if(wifiConfigured && deviceConfigured) {
      delay(2000);
      if(authenticateDevice()) {
        fetchSchedules();
      }
    }
  }

  appendLog("System ready!");
}

// ------------------ MAIN LOOP ------------------
void loop() {
  server.handleClient();
  
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    checkForOTAUpdate();
  }
  
  static unsigned long lastAuthAttempt = 0;
  if(wifiConfigured && deviceConfigured && !deviceAuthenticated && WiFi.status() == WL_CONNECTED) {
    if(millis() - lastAuthAttempt > 60000) {
      authenticateDevice();
      lastAuthAttempt = millis();
    }
  }
  
  if (deviceAuthenticated && WiFi.status() == WL_CONNECTED) {
    fetchSchedules();
  }
  
  if (timeConfigured && scheduleCount > 0) {
    int h, m, s;
    getCurrentTime(h, m, s);
    
    static unsigned long lastLog = 0;
    if(millis() - lastLog > 30000) {
      appendLog("Time: " + getCurrentTimeString() + " - " + String(scheduleCount) + " schedules");
      lastLog = millis();
    }
    
    for (int i = 0; i < scheduleCount; i++) {
      if(!schedules[i].enabled) continue;
      
      bool shouldRun = false;
      
      if (schedules[i].startHour == schedules[i].endHour) {
        if (h == schedules[i].startHour && m >= schedules[i].startMinute && m < schedules[i].endMinute) {
          shouldRun = true;
        }
      } else {
        if ((h == schedules[i].startHour && m >= schedules[i].startMinute) ||
            (h > schedules[i].startHour && h < schedules[i].endHour) ||
            (h == schedules[i].endHour && m < schedules[i].endMinute)) {
          shouldRun = true;
        }
      }
      
      if (shouldRun && !schedules[i].running && !schedules[i].completed) {
        schedules[i].running = true;
        startIrrigation();
      } else if (!shouldRun && schedules[i].running) {
        schedules[i].running = false;
        schedules[i].completed = true;
        stopIrrigation();
      }
    }
  }
  
  if (millis() - lastFlowCheck >= FLOW_INTERVAL) {
    if (pulseCount > 0) {
      float flow = (pulseCount * 2.25) / 1000.0;
      totalLitres += flow;
      scheduleLitres += flow;
      appendLog("Flow: " + String(flow, 3) + " L");
      pulseCount = 0;
    }
    lastFlowCheck = millis();
  }
  
  int h, m, s, y, mo, d;
  getCurrentTime(h, m, s);
  getCurrentDate(y, mo, d);
  
  if (d != currentDay && timeConfigured) {
    if (currentDay != 0) {
      dailyTotalLitres = totalLitres;
      totalLitres = 0;
      appendLog("Daily reset: " + String(dailyTotalLitres, 2) + " L");
    }
    currentDay = d;
    
    for (int i = 0; i < scheduleCount; i++) {
      schedules[i].completed = false;
    }
    saveSchedulesToPreferences();
  }
  
  delay(100);
}