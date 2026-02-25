/*
  test_ota.ino — Standalone OTA Update Test for XIAO ESP32S3 Sense
  
  Purpose: Verify OTA firmware update flow independently of the full project.
  
  How to use:
  1. Upload this sketch to your ESP32 (it becomes v1.0.0)
  2. Upload a new .bin via the dashboard (e.g. v1.0.1)
  3. Press "Enable OTA" on the dashboard
  4. Watch Serial Monitor — device will check, download, flash, and reboot
  5. After reboot, Serial will print the new version

  Hardware: XIAO ESP32S3 Sense (no sensors needed — WiFi only)
  Board:    esp32 > XIAO_ESP32S3
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// ================= CONFIG =================
#define FIRMWARE_VERSION "1.0.0"
#define WIFI_SSID        "123"
#define WIFI_PASSWORD    "KUNAL 26"

const char* SERVER_BASE      = "https://kakuproject-90943350924.asia-south1.run.app";
const char* OTA_AUTH_TOKEN   = "kaku-ota-2025";

// Polling interval — check every 10 seconds in test mode
const unsigned long CHECK_INTERVAL_MS = 10000;
unsigned long lastCheckTime = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("========================================");
    Serial.println("  OTA Test Sketch for ESP32S3");
    Serial.printf("  Firmware Version: %s\n", FIRMWARE_VERSION);
    Serial.println("========================================");
    
    // Connect WiFi
    Serial.printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n✅ WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n❌ WiFi failed. Restarting in 5s...");
        delay(5000);
        ESP.restart();
    }
    
    Serial.println("Checking for OTA updates every 10 seconds...");
    Serial.println("Press 'Enable OTA' on the dashboard to trigger update.");
}

void loop() {
    if (millis() - lastCheckTime >= CHECK_INTERVAL_MS) {
        lastCheckTime = millis();
        checkOLEDForOTA();
    }
    delay(100);
}

// Simulate the OLED polling to check for ota_update flag
void checkOLEDForOTA() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️ WiFi disconnected");
        return;
    }
    
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    
    String oledUrl = String(SERVER_BASE) + "/api/oled-display/get?device_id=ESP32_001";
    if (!http.begin(oledUrl)) return;
    
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        StaticJsonDocument<1024> doc;
        if (!deserializeJson(doc, body)) {
            bool otaFlag = doc["ota_update"] | false;
            Serial.printf("📡 OLED poll — ota_update: %s\n", otaFlag ? "TRUE" : "false");
            
            if (otaFlag) {
                Serial.println("🔄 OTA flag detected! Starting update...");
                http.end();
                performOTA();
                return;
            }
        }
    } else {
        Serial.printf("⚠️ OLED poll HTTP %d\n", code);
    }
    http.end();
}

void performOTA() {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("  Starting OTA Update Process");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // Step 1: Check /api/firmware/latest
    String checkUrl = String(SERVER_BASE) + "/api/firmware/latest?device_id=ESP32_001&current_version=" + FIRMWARE_VERSION;
    
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    
    if (!http.begin(checkUrl)) {
        Serial.println("❌ Cannot connect to firmware server");
        return;
    }
    
    http.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    int code = http.GET();
    
    if (code != 200) {
        Serial.printf("❌ Firmware check failed: HTTP %d\n", code);
        http.end();
        return;
    }
    
    String response = http.getString();
    http.end();
    
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, response)) {
        Serial.println("❌ JSON parse error");
        return;
    }
    
    bool updateAvailable = doc["update_available"] | false;
    if (!updateAvailable) {
        Serial.println("✅ Already on latest firmware");
        return;
    }
    
    const char* newVersion  = doc["version"] | "?";
    const char* downloadUrl = doc["download_url"] | "";
    int fileSize            = doc["file_size"] | 0;
    const char* checksum    = doc["checksum"] | "";
    
    Serial.printf("🆕 New firmware: v%s (%d bytes, md5:%s)\n", newVersion, fileSize, checksum);
    Serial.printf("📥 Downloading from: %s\n", downloadUrl);
    
    // Step 2: Download and flash
    HTTPClient httpDL;
    httpDL.setConnectTimeout(15000);
    httpDL.setTimeout(60000);
    
    if (!httpDL.begin(downloadUrl)) {
        Serial.println("❌ Cannot connect for download");
        return;
    }
    
    httpDL.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    int dlCode = httpDL.GET();
    
    if (dlCode != 200) {
        Serial.printf("❌ Download failed: HTTP %d\n", dlCode);
        httpDL.end();
        return;
    }
    
    int contentLength = httpDL.getSize();
    if (contentLength <= 0) {
        Serial.println("❌ Invalid content length");
        httpDL.end();
        return;
    }
    
    if (!Update.begin(contentLength)) {
        Serial.printf("❌ Not enough space: %s\n", Update.errorString());
        httpDL.end();
        return;
    }
    
    Serial.printf("📦 Flashing %d bytes...\n", contentLength);
    
    WiFiClient *stream = httpDL.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    
    while (httpDL.connected() && written < contentLength) {
        size_t available = stream->available();
        if (available) {
            int bytesRead = stream->readBytes(buf, min((size_t)sizeof(buf), available));
            Update.write(buf, bytesRead);
            written += bytesRead;
            
            int pct = (written * 100) / contentLength;
            if (pct % 10 == 0) {
                Serial.printf("   %d%% (%d/%d)\n", pct, written, contentLength);
            }
        }
        delay(1);
    }
    
    httpDL.end();
    
    if (written != contentLength) {
        Serial.printf("❌ Incomplete download (%d/%d)\n", written, contentLength);
        Update.abort();
        return;
    }
    
    if (!Update.end(true)) {
        Serial.printf("❌ Flash error: %s\n", Update.errorString());
        return;
    }
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  ✅ OTA Success! v%s → v%s\n", FIRMWARE_VERSION, newVersion);
    Serial.println("  Rebooting in 2 seconds...");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    delay(2000);
    ESP.restart();
}
