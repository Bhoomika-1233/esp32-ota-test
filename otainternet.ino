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
String currentVersion = "1.0.0";
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
void handleRoot();
void handleDashboard();
void handleConnect();
void handleNotFound();
void countPulse();
void getCurrentTime(int &hour, int &minute, int &second);
void getCurrentDate(int &year, int &month, int &day);
void setupOTA();
void checkForOTAUpdate();
String scanNetworks();

// ------------------ ISR ------------------
void IRAM_ATTR countPulse() { pulseCount++; }

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
    appendLog("WiFi not configured - skipping schedule fetch");
    return false;
  }
  
  if(!timeConfigured) {
    appendLog("Time not configured - skipping schedule fetch");
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
    appendLog("Response: " + payload);

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if(error) {
      appendLog("JSON parse error: " + String(error.c_str()));
      http.end();
      return false;
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
    } else {
      appendLog("No valid schedule format found in response");
    }

    appendLog("Loaded " + String(scheduleCount) + " schedule(s)");
    for(int i=0; i<scheduleCount; i++) {
      String schedStr = String(schedules[i].startHour) + ":" + 
                       (schedules[i].startMinute<10?"0":"") + String(schedules[i].startMinute) + 
                       " to " + 
                       String(schedules[i].endHour) + ":" + 
                       (schedules[i].endMinute<10?"0":"") + String(schedules[i].endMinute);
      appendLog("  Schedule " + String(i+1) + ": " + schedStr);
    }

    http.end();
    lastScheduleFetch = millis();
    return true;

  } else if (code > 0) {
    String errorMsg = http.getString();
    appendLog("HTTP error " + String(code) + ": " + errorMsg);
  } else {
    String errorDetails = "";
    if(code == -1) errorDetails = " (Connection refused)";
    else if(code == -2) errorDetails = " (Send header failed)";
    else if(code == -3) errorDetails = " (Send payload failed)";
    else if(code == -4) errorDetails = " (Not connected)";
    else if(code == -5) errorDetails = " (Connection lost)";
    else if(code == -6) errorDetails = " (No stream)";
    else if(code == -7) errorDetails = " (No HTTP server)";
    else if(code == -8) errorDetails = " (Too less RAM)";
    else if(code == -9) errorDetails = " (Encoding error)";
    else if(code == -10) errorDetails = " (Stream write error)";
    else if(code == -11) errorDetails = " (Timeout)";
    
    appendLog("Connection failed: " + String(code) + errorDetails);
  }
  
  http.end();
  lastScheduleFetch = millis();
  
  // Add test schedule if server is not reachable
  if(scheduleCount == 0) {
    appendLog("Adding test schedule: 16:15-16:16");
    schedules[0].startHour = 16;
    schedules[0].startMinute = 15;
    schedules[0].endHour = 16;
    schedules[0].endMinute = 16;
    schedules[0].running = false;
    schedules[0].completed = false;
    scheduleCount = 1;
  }
  
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
  page += ".nav-links a { background-color: #2196F3; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; margin: 0 10px; }";
  page += "</style></head><body>";
  
  page += "<div class='container'>";
  page += "<h1>Smart Irrigation Dashboard</h1>";
  
  page += "<div class='nav-links'>";
  page += "<a href='/'>WiFi Config</a>";
  page += "<a href='/dashboard'>Refresh</a>";
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

  page += "<h3>Active Schedules</h3>";
  if(scheduleCount > 0) {
    for(int i=0;i<scheduleCount;i++){
      page += "<p>Schedule "+String(i+1)+": "+
        String(schedules[i].startHour)+":"+String(schedules[i].startMinute<10?"0":"")+String(schedules[i].startMinute)+
        " - "+
        String(schedules[i].endHour)+":"+String(schedules[i].endMinute<10?"0":"")+String(schedules[i].endMinute)+"</p>";
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

  // Clear any saved WiFi credentials on startup for testing
  preferences.begin("wifi", false);
  preferences.clear(); // Remove this line after testing
  wifiSSID = preferences.getString("ssid", "");
  wifiPASS = preferences.getString("pass", "");
  preferences.end();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleDashboard);
  server.on("/connect", HTTP_POST, handleConnect);
  server.onNotFound(handleNotFound);
  server.begin();
  appendLog("Web server started");

  if(wifiSSID.length() > 0){
    appendLog("Found saved WiFi credentials, attempting connection...");
    connectToWiFi();
    if(wifiConfigured) {
      delay(2000);
      fetchSchedules();
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
  
  // Fetch schedules periodically
  if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
    fetchSchedules();
  }
  
  // Add your irrigation scheduling logic here
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
        appendLog("Started irrigation for schedule " + String(i+1) + " (" + 
                 String(schedules[i].startHour) + ":" + 
                 (schedules[i].startMinute<10?"0":"") + String(schedules[i].startMinute) + 
                 "-" + String(schedules[i].endHour) + ":" + 
                 (schedules[i].endMinute<10?"0":"") + String(schedules[i].endMinute) + ")");
      } else if (!shouldRun && schedules[i].running) {
        schedules[i].running = false;
        schedules[i].completed = true;
        stopIrrigation();
        appendLog("Completed irrigation for schedule " + String(i+1));
      }
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
  
  if (day != currentDay) {
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
  }
  
  delay(100);
}
