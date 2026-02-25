/*
  test_ota_v101.ino — OTA Target Firmware v1.0.3
  OLED Animation Demo

  Flashed via OTA onto device running any earlier version.
  After reboot: 64x32 OLED cycles through 3 animations:
    Scene 1 — Bouncing ball
    Scene 2 — Blinking smiley face
    Scene 3 — Scrolling version text
  OTA polling stays active every 10s for future updates.

  Hardware:
    Board : XIAO ESP32S3
    OLED  : SSD1306 64x32, I2C → SDA=GPIO5, SCL=GPIO6, addr=0x3C

  Required library: Adafruit SSD1306  (Tools → Manage Libraries → search "Adafruit SSD1306")
  Partition Scheme: Default with spiffs (3MB APP / 1.5MB SPIFFS)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================= OLED =================
#define SCREEN_WIDTH  64
#define SCREEN_HEIGHT 32
#define OLED_ADDR     0x3C
#define OLED_SDA      5
#define OLED_SCL      6

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= FIRMWARE / WIFI =================
#define FIRMWARE_VERSION "1.0.4"
#define WIFI_SSID        "123"
#define WIFI_PASSWORD    "KUNAL 26"

const char* SERVER_BASE    = "https://kakuproject-90943350924.asia-south1.run.app";
const char* OTA_AUTH_TOKEN = "kaku-ota-2025";

// ================= TIMING =================
const unsigned long OTA_CHECK_INTERVAL  = 10000;  // OTA poll every 10s
const unsigned long ANIM_FRAME_INTERVAL = 80;     // 80ms per frame (~12 fps)

unsigned long lastOtaCheck  = 0;
unsigned long lastAnimFrame = 0;

// Global SSL clients — WiFiClientSecure is ~16KB, MUST be global (stack overflow otherwise)
WiFiClientSecure sslPoll;
WiFiClientSecure sslOTA;
uint8_t otaBuf[4096];  // download buffer — global to save stack

// ================= ANIMATION STATE =================
int animScene  = 0;   // 0=ball  1=face  2=text
int sceneFrame = 0;

// Ball physics
float ballX = 10, ballY = 8;
float ballVX = 2.5, ballVY = 1.8;
#define BALL_R 3

// ── Scene: bouncing ball ────────────────────────────────
void drawBall() {
    display.clearDisplay();
    display.fillCircle((int)ballX, (int)ballY, BALL_R, SSD1306_WHITE);
    display.drawCircle((int)(ballX - ballVX * 1.5), (int)(ballY - ballVY * 1.5), BALL_R - 1, SSD1306_WHITE);
    display.display();
}

// ── Scene: smiley face with blink ──────────────────────
void drawFace(bool blink) {
    display.clearDisplay();
    int cx = 32, cy = 15;
    display.drawCircle(cx, cy, 13, SSD1306_WHITE);          // head outline

    if (!blink) {
        display.fillCircle(cx - 5, cy - 3, 2, SSD1306_WHITE); // left eye
        display.fillCircle(cx + 5, cy - 3, 2, SSD1306_WHITE); // right eye
    } else {
        // closed eyes
        display.drawLine(cx - 7, cy - 3, cx - 3, cy - 3, SSD1306_WHITE);
        display.drawLine(cx + 3, cy - 3, cx + 7, cy - 3, SSD1306_WHITE);
    }

    // Smile arc
    display.drawLine(cx - 5, cy + 4, cx - 3, cy + 7, SSD1306_WHITE);
    display.drawLine(cx - 3, cy + 7, cx,     cy + 8, SSD1306_WHITE);
    display.drawLine(cx,     cy + 8, cx + 3, cy + 7, SSD1306_WHITE);
    display.drawLine(cx + 3, cy + 7, cx + 5, cy + 4, SSD1306_WHITE);
    display.display();
}

// ── Scene: scrolling text ───────────────────────────────
void drawTextScroll(int offset) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(SCREEN_WIDTH - offset, 4);
    display.print("OTA v1.0.3");
    display.setCursor(SCREEN_WIDTH - offset + 6, 18);
    display.print("Kaku Pet");
    display.display();
}

// ── Advance one animation frame ─────────────────────────
void updateAnimation() {
    switch (animScene) {
        case 0: // Bouncing ball — 50 frames
            ballX += ballVX;
            ballY += ballVY;
            if (ballX - BALL_R <= 0 || ballX + BALL_R >= SCREEN_WIDTH)  ballVX = -ballVX;
            if (ballY - BALL_R <= 0 || ballY + BALL_R >= SCREEN_HEIGHT) ballVY = -ballVY;
            drawBall();
            if (++sceneFrame >= 50) { animScene = 1; sceneFrame = 0; }
            break;

        case 1: // Smiley face — 30 frames, blink twice
            drawFace(sceneFrame % 15 < 2);
            if (++sceneFrame >= 30) { animScene = 2; sceneFrame = 0; }
            break;

        case 2: // Scroll text right-to-left — 90 frames
            drawTextScroll(sceneFrame * 2);
            if (++sceneFrame >= 90) { animScene = 0; sceneFrame = 0; }
            break;
    }
}

// ===================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    // Init I2C + OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("❌ SSD1306 not found — check wiring");
    } else {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(4, 4);
        display.print("OTA v1.0.3");
        display.setCursor(4, 18);
        display.print("Starting...");
        display.display();
        Serial.println("✅ OLED ready");
    }

    Serial.println("========================================");
    Serial.println("  OLED Animation Demo — via OTA");
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

    Serial.println("Animation running. OTA check every 10s.");

    // Init global SSL clients
    sslPoll.setInsecure();
    sslPoll.setTimeout(10);
    sslOTA.setInsecure();
    sslOTA.setTimeout(120);
}

void loop() {
    unsigned long now = millis();

    // ── Animate OLED (non-blocking) ────────────────────────
    if (now - lastAnimFrame >= ANIM_FRAME_INTERVAL) {
        lastAnimFrame = now;
        updateAnimation();
    }

    // ── OTA check ─────────────────────────────────────────
    if (now - lastOtaCheck >= OTA_CHECK_INTERVAL) {
        lastOtaCheck = now;
        checkOLEDForOTA();
    }

    delay(5);
}

// ===================================================
void checkOLEDForOTA() {
    if (WiFi.status() != WL_CONNECTED) return;

    sslPoll.stop();
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);

    String oledUrl = String(SERVER_BASE) + "/api/oled-display/get?device_id=ESP32_001";
    if (!http.begin(sslPoll, oledUrl)) return;

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

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 4);
    display.print("OTA Update");
    display.setCursor(4, 16);
    display.print("Flashing...");
    display.display();

    String checkUrl = String(SERVER_BASE) + "/api/firmware/latest?device_id=ESP32_001&current_version=" + FIRMWARE_VERSION;

    sslOTA.stop();
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    if (!http.begin(sslOTA, checkUrl)) return;

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
    const char* dlUrl       = doc["download_url"] | "";
    String downloadUrl      = String(dlUrl);
    int fileSize            = doc["file_size"] | 0;

    // Force HTTPS — Cloud Run redirects http→https which ESP32 can't follow
    if (downloadUrl.startsWith("http://")) {
        downloadUrl = "https://" + downloadUrl.substring(7);
    }

    Serial.printf("🆕 Updating to v%s (%d bytes)\n", newVersion, fileSize);

    sslOTA.stop();
    HTTPClient httpDL;
    httpDL.setConnectTimeout(15000);
    httpDL.setTimeout(120000);
    if (!httpDL.begin(sslOTA, downloadUrl)) return;

    httpDL.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    if (httpDL.GET() != 200) { httpDL.end(); return; }

    int contentLength = httpDL.getSize();
    if (contentLength <= 0) {
        Serial.println("❌ Invalid content length");
        httpDL.end(); return;
    }
    if (!Update.begin(contentLength)) {
        Serial.printf("❌ Not enough space: %s\n", Update.errorString());
        httpDL.end(); return;
    }

    Serial.printf("📦 Flashing %d bytes...\n", contentLength);
    WiFiClient* stream = httpDL.getStreamPtr();

    // Chunked download with watchdog — writeStream times out on Cloud Run
    size_t written = 0;
    int lastPctPrint = -1;
    unsigned long lastDataTime = millis();

    while (written < (size_t)contentLength) {
        if (millis() - lastDataTime > 60000) {
            Serial.println("❌ Download stalled (60s no data)");
            break;
        }
        // Use read() directly — available() misses pending TLS records
        int bytesRead = stream->read(otaBuf, sizeof(otaBuf));
        if (bytesRead > 0) {
            Update.write(otaBuf, bytesRead);
            written += bytesRead;
            lastDataTime = millis();
            int pct = (written * 100) / contentLength;
            if (pct / 10 != lastPctPrint / 10) {
                lastPctPrint = pct;
                display.clearDisplay();
                display.setCursor(4, 4);
                display.print("Flashing...");
                display.setCursor(4, 16);
                display.printf("%d%%", pct);
                display.drawRect(0, 26, SCREEN_WIDTH, 6, SSD1306_WHITE);
                display.fillRect(1, 27, (SCREEN_WIDTH - 2) * pct / 100, 4, SSD1306_WHITE);
                display.display();
                Serial.printf("   %d%% (%d/%d)\n", pct, written, contentLength);
            }
        } else {
            delay(1);  // yield to WiFi stack, retry quickly
        }
        if (!httpDL.connected() && stream->available() == 0) break;
    }
    httpDL.end();

    if (written == (size_t)contentLength && Update.end(true)) {
        display.clearDisplay();
        display.setCursor(8, 10);
        display.print("OTA Done!");
        display.setCursor(6, 22);
        display.printf("-> v%s", newVersion);
        display.display();
        Serial.printf("✅ OTA done! v%s → v%s  Rebooting...\n", FIRMWARE_VERSION, newVersion);
        delay(2000);
        ESP.restart();
    } else {
        Serial.printf("❌ OTA failed: wrote %d/%d  err: %s\n", written, contentLength, Update.errorString());
        Update.abort();
    }
}
