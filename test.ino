// =====================================================================
//  test.ino  —  BLIP IMAGE CAPTIONING TEST
//  Hardware: XIAO ESP32S3 Sense (OV2640 camera built-in)
//  Baud: 115200
//
//  PURPOSE:
//    Tests the full image captioning pipeline:
//      ESP32 camera → GCP server /upload → HuggingFace BLIP API
//    Then fetches back the AI caption from /api/latest-image
//
//  HOW TO USE:
//    1. Flash to device, open Serial Monitor @ 115200
//    2. Device auto-connects WiFi, inits camera
//    3. Type 'C' + Enter  →  Capture & upload image
//    4. Type 'R' + Enter  →  Check latest AI caption result
//    5. Type 'A' + Enter  →  Auto test (capture + wait 10s + fetch caption)
//    6. Watch serial output for full flow status
//
//  EXPECTED OUTPUT:
//    ✅ WiFi connected
//    ✅ Camera initialized
//    📸 Captured image: XXXXX bytes
//    📤 Uploading to server...
//    ✅ Upload OK (200): {"status":"success","image_id":XX,...}
//    🤖 Fetching AI caption...
//    🧠 AI Caption: "a person sitting at a desk with a laptop"
//    💖 Emotion: HAPPY (matched: person)
// =====================================================================

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ================= WiFi CONFIG =================
#define WIFI_SSID     "Airtel_BumbleBee-777"
#define WIFI_PASSWORD "kya karoge ."

// ================= SERVER CONFIG =================
const char* SERVER_BASE = "https://kakuproject-90943350924.asia-south1.run.app";

// ================= CAMERA PINS (XIAO ESP32 S3 Sense) =================
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

// ── GLOBALS ──────────────────────────────────────────────────
WiFiClientSecure sslClient;
bool cameraReady = false;
int lastImageId = -1;

// ── FUNCTION DECLARATIONS ────────────────────────────────────
bool initCamera();
bool captureAndUpload();
bool fetchLatestCaption();
void runAutoTest();
void connectWiFi();

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║  BLIP IMAGE CAPTIONING TEST          ║");
    Serial.println("║  XIAO ESP32S3 Sense → GCP → BLIP    ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.println();
    Serial.println("Commands:");
    Serial.println("  C = Capture & Upload image");
    Serial.println("  R = Read latest AI caption");
    Serial.println("  A = Auto test (capture + wait + read)");
    Serial.println("──────────────────────────────────────\n");

    // Connect WiFi
    connectWiFi();

    // Init camera
    if (initCamera()) {
        Serial.println("✅ Camera initialized (QQVGA 160x120 JPEG)");
    } else {
        Serial.println("❌ Camera init FAILED — cannot test");
    }

    // Skip SSL certificate verification (for testing)
    sslClient.setInsecure();
    sslClient.setTimeout(10);  // 10s SSL handshake timeout (matches main sketch)

    Serial.println("\n🟢 Ready! Send C/R/A via Serial Monitor.\n");
}

// ─────────────────────────────────────────────────────────────
void loop() {
    if (Serial.available()) {
        char c = toupper((char)Serial.read());
        // Flush remaining chars (newline etc)
        while (Serial.available()) Serial.read();

        switch (c) {
            case 'C':
                Serial.println("\n═══ CAPTURE & UPLOAD ═══");
                captureAndUpload();
                break;
            case 'R':
                Serial.println("\n═══ READ CAPTION ═══");
                fetchLatestCaption();
                break;
            case 'A':
                Serial.println("\n═══ AUTO TEST (capture → wait 10s → read) ═══");
                runAutoTest();
                break;
            default:
                if (c >= 32) { // printable char
                    Serial.printf("Unknown command: '%c'. Use C/R/A\n", c);
                }
                break;
        }
    }
    delay(50);
}

// ═════════════════════════════════════════════════════════════
//  WiFi CONNECTION
// ═════════════════════════════════════════════════════════════
void connectWiFi() {
    Serial.printf("📶 Connecting to WiFi: %s ", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n✅ WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n❌ WiFi connection FAILED!");
    }
}

// ═════════════════════════════════════════════════════════════
//  CAMERA INIT
// ═════════════════════════════════════════════════════════════
bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;

    config.xclk_freq_hz = 10000000;        // 10 MHz
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_QQVGA;  // 160x120
    config.jpeg_quality = 15;               // Decent quality for captioning
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("❌ Camera init error: 0x%x\n", err);
        return false;
    }
    cameraReady = true;
    return true;
}

// ═════════════════════════════════════════════════════════════
//  CAPTURE & UPLOAD to /upload endpoint
// ═════════════════════════════════════════════════════════════
bool captureAndUpload() {
    if (!cameraReady) {
        Serial.println("❌ Camera not ready!");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi not connected!");
        return false;
    }

    // Capture image
    Serial.println("📸 Capturing image...");
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("❌ Camera capture FAILED");
        return false;
    }
    Serial.printf("📸 Captured: %d bytes (JPEG %dx%d)\n", fb->len, fb->width, fb->height);

    // Upload to server as binary (with retry)
    bool success = false;
    for (int attempt = 1; attempt <= 3 && !success; attempt++) {
        if (attempt > 1) {
            Serial.printf("🔄 Retry attempt %d/3...\n", attempt);
            delay(2000);
        }
        
        Serial.printf("📤 Uploading to %s/upload ...\n", SERVER_BASE);
        
        sslClient.stop();
        delay(100);
        sslClient.setInsecure();
        sslClient.setTimeout(10);
        
        HTTPClient http;
        http.setTimeout(10000);
        http.setConnectTimeout(5000);

        String url = String(SERVER_BASE) + "/upload?device_id=ESP32_TEST";
        if (!http.begin(sslClient, url)) {
            Serial.println("❌ HTTP begin failed");
            continue;
        }

        http.addHeader("Content-Type", "application/octet-stream");
        
        unsigned long startMs = millis();
        int httpCode = http.sendRequest("POST", fb->buf, fb->len);
        unsigned long elapsed = millis() - startMs;

        if (httpCode == 200) {
            String response = http.getString();
            Serial.printf("✅ Upload OK (%lu ms)\n", elapsed);
            Serial.printf("📝 Server response: %s\n", response.c_str());
            
            // Parse image_id from response
            StaticJsonDocument<512> doc;
            DeserializationError jsonErr = deserializeJson(doc, response);
            if (!jsonErr && doc.containsKey("image_id")) {
                lastImageId = doc["image_id"].as<int>();
                Serial.printf("🆔 Image ID: %d\n", lastImageId);
            }
            success = true;
        } else {
            Serial.printf("❌ Upload FAILED! HTTP %d (%lu ms)\n", httpCode, elapsed);
            if (httpCode > 0) {
                Serial.printf("   Response: %s\n", http.getString().c_str());
            } else {
                Serial.printf("   Error: connection failed (SSL handshake timeout?)\n");
            }
        }
        http.end();
    }

    esp_camera_fb_return(fb);
    return success;
}

// ═════════════════════════════════════════════════════════════
//  FETCH LATEST AI CAPTION from /api/latest-image
// ═════════════════════════════════════════════════════════════
bool fetchLatestCaption() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi not connected!");
        return false;
    }

    Serial.println("🤖 Fetching latest AI caption...");
    
    sslClient.stop();
    delay(100);
    sslClient.setInsecure();
    sslClient.setTimeout(10);
    
    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);

    String url = String(SERVER_BASE) + "/api/latest-image?caption_only=1";
    if (!http.begin(sslClient, url)) {
        Serial.println("❌ HTTP begin failed");
        return false;
    }

    int httpCode = http.GET();
    
    if (httpCode == 200) {
        String response = http.getString();
        
        StaticJsonDocument<2048> doc;
        DeserializationError jsonErr = deserializeJson(doc, response);
        
        if (!jsonErr) {
            bool success     = doc["success"] | false;
            int imageId      = doc["image_id"] | -1;
            const char* caption  = doc["ai_caption"] | "N/A";
            const char* filename = doc["filename"] | "N/A";
            const char* source   = doc["source"] | "N/A";
            bool hasImage    = doc.containsKey("image_url");

            Serial.println("┌──────────────────────────────────┐");
            Serial.println("│     📊 LATEST IMAGE RESULT       │");
            Serial.println("├──────────────────────────────────┤");
            Serial.printf( "│ Success  : %s\n", success ? "YES ✅" : "NO ❌");
            Serial.printf( "│ Image ID : %d\n", imageId);
            Serial.printf( "│ Filename : %s\n", filename);
            Serial.printf( "│ Source   : %s\n", source);
            Serial.printf( "│ Has Image: %s\n", hasImage ? "YES" : "NO");
            Serial.println("├──────────────────────────────────┤");
            Serial.printf( "│ 🧠 AI Caption: %s\n", caption);
            Serial.println("└──────────────────────────────────┘");

            // Check if caption is real (not placeholder)
            String captionStr = String(caption);
            if (captionStr == "Waiting for AI analysis..." || captionStr == "N/A") {
                Serial.println("⏳ Caption not ready yet — BLIP may still be processing");
                Serial.println("   Try again in a few seconds (send 'R')");
            } else {
                Serial.println("✅ BLIP CAPTIONING IS WORKING!");
                
                // Check emotion keywords
                captionStr.toLowerCase();
                const char* positiveWords[] = {"food", "fruit", "bottle", "toy", "person", "man", "woman", "face", "dog", "cat", "snack"};
                const char* negativeWords[] = {"trash", "dirty", "fire", "dark", "scary"};
                
                bool emotionFound = false;
                for (int i = 0; i < 11; i++) {
                    if (captionStr.indexOf(positiveWords[i]) >= 0) {
                        Serial.printf("💖 Emotion trigger: HAPPY (matched keyword: '%s')\n", positiveWords[i]);
                        emotionFound = true;
                        break;
                    }
                }
                if (!emotionFound) {
                    for (int i = 0; i < 5; i++) {
                        if (captionStr.indexOf(negativeWords[i]) >= 0) {
                            Serial.printf("😢 Emotion trigger: CRY (matched keyword: '%s')\n", negativeWords[i]);
                            emotionFound = true;
                            break;
                        }
                    }
                }
                if (!emotionFound) {
                    Serial.println("😐 No emotion keyword matched — neutral response");
                }
            }
        } else {
            Serial.printf("❌ JSON parse error: %s\n", jsonErr.c_str());
            Serial.println(response.substring(0, 200));
        }
    } else if (httpCode == 404) {
        Serial.println("📭 No images found in database (404)");
    } else {
        Serial.printf("❌ Fetch FAILED! HTTP %d\n", httpCode);
    }

    http.end();
    return (httpCode == 200);
}

// ═════════════════════════════════════════════════════════════
//  AUTO TEST: Capture → Wait → Fetch caption
// ═════════════════════════════════════════════════════════════
void runAutoTest() {
    Serial.println("\n🔄 === FULL PIPELINE TEST ===\n");
    
    // Step 1: Capture & upload
    Serial.println("── STEP 1/3: Capture & Upload ──");
    bool uploaded = captureAndUpload();
    if (!uploaded) {
        Serial.println("❌ Auto test ABORTED — upload failed");
        return;
    }

    // Step 2: Wait for BLIP processing
    Serial.println("\n── STEP 2/3: Waiting 12s for BLIP to process... ──");
    for (int i = 12; i > 0; i--) {
        Serial.printf("   ⏳ %d seconds remaining...\n", i);
        delay(1000);
    }

    // Step 3: Fetch caption
    Serial.println("\n── STEP 3/3: Fetch AI Caption ──");
    bool gotCaption = fetchLatestCaption();

    // Summary
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.printf( "║  Upload:  %s\n", uploaded ? "✅ SUCCESS" : "❌ FAILED");
    Serial.printf( "║  Caption: %s\n", gotCaption ? "✅ RECEIVED" : "⏳ PENDING");
    Serial.println("╚══════════════════════════════════════╝\n");   
}
