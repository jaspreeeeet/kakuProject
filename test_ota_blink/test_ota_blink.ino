/*
  test_ota_blink.ino — OTA Target: Blinking LED v1.0.2
  
  This is flashed via OTA onto the device running v1.0.0/v1.0.1.
  After OTA reboot, the onboard LED will blink AND OTA stays active
  for future updates.

  XIAO ESP32S3 onboard LED pin: 21
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// ================= CONFIG =================
#define FIRMWARE_VERSION "1.0.2"
#define WIFI_SSID        "123"
#define WIFI_PASSWORD    "KUNAL 26"
#define LED_PIN          21          // XIAO ESP32S3 onboard LED

const char* SERVER_BASE    = "https://kakuproject-90943350924.asia-south1.run.app";
const char* OTA_AUTH_TOKEN = "kaku-ota-2025";

const unsigned long CHECK_INTERVAL_MS = 10000;  // OTA check every 10s
const unsigned long BLINK_INTERVAL_MS = 500;    // Blink every 500ms

unsigned long lastCheckTime = 0;
unsigned long lastBlinkTime = 0;
bool ledState = false;

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.println("========================================");
    Serial.println("  Blinking LED Firmware via OTA");
    Serial.printf("  Firmware Version: %s\n", FIRMWARE_VERSION);
    Serial.println("========================================");

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
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                    IPAddress(8, 8, 8, 8), IPAddress(8, 8, 4, 4));
    } else {
        Serial.println("\n❌ WiFi failed. Restarting in 5s...");
        delay(5000);
        ESP.restart();
    }

    Serial.println("LED blinking started. OTA check every 10s.");
}

void loop() {
    // ── Blink LED ──────────────────────────────────────────
    if (millis() - lastBlinkTime >= BLINK_INTERVAL_MS) {
        lastBlinkTime = millis();
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
    }

    // ── OTA check ─────────────────────────────────────────
    if (millis() - lastCheckTime >= CHECK_INTERVAL_MS) {
        lastCheckTime = millis();
        checkOLEDForOTA();
    }

    delay(10);
}

void checkOLEDForOTA() {
    if (WiFi.status() != WL_CONNECTED) return;

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
            Serial.printf("📡 OTA poll — ota_update: %s\n", otaFlag ? "TRUE" : "false");
            if (otaFlag) {
                http.end();
                performOTA();
                return;
            }
        }
    } else {
        Serial.printf("⚠️ OTA poll HTTP %d\n", code);
    }
    http.end();
}

void performOTA() {
    Serial.println("🔄 OTA update starting...");
    digitalWrite(LED_PIN, HIGH);  // LED solid while flashing

    String checkUrl = String(SERVER_BASE) + "/api/firmware/latest?device_id=ESP32_001&current_version=" + FIRMWARE_VERSION;

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    if (!http.begin(checkUrl)) return;

    http.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    int code = http.GET();
    if (code != 200) { http.end(); return; }

    String response = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, response)) return;

    if (!(doc["update_available"] | false)) {
        Serial.println("✅ Already on latest firmware");
        return;
    }

    const char* newVersion  = doc["version"] | "?";
    const char* downloadUrl = doc["download_url"] | "";
    int fileSize            = doc["file_size"] | 0;

    Serial.printf("🆕 Updating to v%s (%d bytes)\n", newVersion, fileSize);

    HTTPClient httpDL;
    httpDL.setConnectTimeout(15000);
    httpDL.setTimeout(60000);
    if (!httpDL.begin(downloadUrl)) return;

    httpDL.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    if (httpDL.GET() != 200) { httpDL.end(); return; }

    int contentLength = httpDL.getSize();
    if (contentLength <= 0 || !Update.begin(contentLength)) { httpDL.end(); return; }

    WiFiClient* stream = httpDL.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;

    while (httpDL.connected() && written < contentLength) {
        size_t available = stream->available();
        if (available) {
            int bytesRead = stream->readBytes(buf, min((size_t)sizeof(buf), available));
            Update.write(buf, bytesRead);
            written += bytesRead;
            Serial.printf("   %d%%\n", (written * 100) / contentLength);
        }
        delay(1);
    }
    httpDL.end();

    if (written == contentLength && Update.end(true)) {
        Serial.println("✅ OTA done! Rebooting...");
        delay(2000);
        ESP.restart();
    } else {
        Serial.println("❌ OTA failed");
        Update.abort();
    }
}
