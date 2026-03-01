// ╔══════════════════════════════════════════════════════════════╗
// ║  FORCE_EGG_HATCH — set to true to replay egg animation     ║
// ║  Set false for normal operation (egg plays once, then never)║
// ╚══════════════════════════════════════════════════════════════╝
#define FORCE_EGG_HATCH true   // ← Change to true to replay egg animation

/*
ESP32 Tamagotchi Client - Arduino C++ (XIAO ESP32 S3 Sense)

📡 ACTIVE FEATURES:
  - MPU6050: accelerometer/gyro (tilt gestures + game control)
  - Camera: on-demand only (food feeding gesture triggers capture)
  - OLED 64x32: pet animations, menus, icons
  - WiFi: sensor upload, pet state sync, image upload

🎮 GESTURE CONTROLS:
  - Right tilt + hold 2s → cycle menu
(MAIN→FOOD→TOILET→PLAY→HEALTH→STATUS→STATS→MAIN)
  - Left tilt + hold 3s  → context action (feed / clean / medicine depending on
menu)

⚡ CPU FREQUENCY:
  - Idle:          80 MHz
  - HTTP/JSON:    160 MHz
  - Camera capture: 240 MHz

📶 NETWORK SCHEDULE (staggered, no overlap):
  - SLOT A: Sensor batch   → every 2s
  - SLOT B: OLED + Events  → every 5s (combined single slot)
  - On demand: Image upload, clean, inject, happy

Required Libraries:
- ArduinoJson, WiFi, HTTPClient, I2Cdev, MPU6050, esp_camera
*/

#include "MPU6050.h"
#include "base64.h"
#include "driver/i2s_pdm.h"
#include "esp_camera.h"
#include "fb_gfx.h"
#include "img_converters.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

// ================= OLED & PET ANIMATIONS =================
#include "all_pets.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <QRCodeGFX.h>

// ================= FIRMWARE VERSION (increment before each upload)
// =================
#define FIRMWARE_VERSION "1.0.5"

// ================= WIFI =================
#define WIFI_SSID "Airtel_BumbleBee-777"
#define WIFI_PASSWORD "kya karoge ."

// ================= WIFI PROVISIONING =================
WebServer wifiConfigServer(80);
Preferences wifiPrefs;
bool wifiProvisioningMode = false; // True = AP mode active, normal tasks paused
bool phoneConnectedToAP = false;
#define MAX_STORED_WIFI 5      // Maximum stored WiFi networks in NVS
#define WIFI_CONNECT_RETRIES 3 // Retries per credential set
const char *AP_SSID = "KAKU_SETUP";
const char *AP_PASS = "12345678";
Preferences petPrefs;

// ================= API =================
const char *serverUrl = "https://kakuproject-90943350924.asia-south1.run.app/"
                        "api/sensor-data"; // Google Cloud Run Production
const char *eventsUrl = "https://kakuproject-90943350924.asia-south1.run.app/"
                        "api/events?device_id=ESP32_001"; // Events endpoint
const char *eventReceivedUrl =
    "https://kakuproject-90943350924.asia-south1.run.app/api/device/event/"
    "received"; // Event acknowledgment
const char *oledDisplayUrl =
    "https://kakuproject-90943350924.asia-south1.run.app/api/"
    "oled-display/get"; // OLED display animation endpoint
const char *firmwareCheckUrl =
    "https://kakuproject-90943350924.asia-south1.run.app/api/firmware/"
    "latest?device_id=ESP32_001&current_version=" FIRMWARE_VERSION;
const char *OTA_AUTH_TOKEN = "kaku-ota-2025"; // Must match server token
// NOTE: Orientation endpoint removed - server computes direction from sensor
// data

// ================= CAMERA PINS (XIAO ESP32 S3 Sense) =================
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 10
#define SIOD_GPIO_NUM 40
#define SIOC_GPIO_NUM 39
#define Y9_GPIO_NUM 48
#define Y8_GPIO_NUM 11
#define Y7_GPIO_NUM 12
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 16
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 17
#define Y2_GPIO_NUM 15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM 47
#define PCLK_GPIO_NUM 13

// ================= AUDIO SETTINGS =================
#define SAMPLE_RATE 16000
#define SAMPLE_BITS 16
#define RECORD_SECONDS 1 // 1 second of audio (4x smaller payload)
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN 2
#define AUDIO_DATA_SIZE (SAMPLE_RATE * 2 * RECORD_SECONDS)

// ================= I2S PDM MIC PINS (XIAO ESP32 S3 Sense) =================
#define PDM_CLK_GPIO (gpio_num_t)42 // PDM CLK
#define PDM_DIN_GPIO (gpio_num_t)41 // PDM DATA
#define I2S_NUM I2S_NUM_0

// ================= OLED DISPLAY SETUP =================
#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 32
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= SLEEPING ANIMATION (2 frames, 64x32) =================
#define SLEEPING_FRAME_COUNT 2
#define SLEEPING_WIDTH 64
#define SLEEPING_HEIGHT 32
const uint16_t sleeping_delays[SLEEPING_FRAME_COUNT] = {700, 900};
PROGMEM const uint8_t sleeping_frames[SLEEPING_FRAME_COUNT][256] = {
    {// Frame 1
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x07, 0x01, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
     0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x3b, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3b,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b,
     0xfb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x7c, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xdf, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,
     0xbf, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf7, 0xff, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xf7, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf7,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x07, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7f,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1f, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
     0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x60, 0x00, 0x00, 0x00},
    {// Frame 2
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x40,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x04, 0x01, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
     0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xfe, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x3b, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3b,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
     0xfb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x7c, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xdf, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xff, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x07, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0x3f,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
     0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x30, 0x00, 0x00, 0x00}};

// ================= WALKING ANIMATION (6 frames, 64x32 — frame 2 skipped)
// =================
#define WALKING_FRAME_COUNT 6
#define WALKING_WIDTH 64
#define WALKING_HEIGHT 32
const uint16_t walking_delays[WALKING_FRAME_COUNT] = {400, 400, 400,
                                                      400, 400, 400};
PROGMEM const uint8_t walking_animation[WALKING_FRAME_COUNT][256] = {
    {// Frame 0
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
     0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x37, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77,
     0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf3, 0x7e, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xef, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xe3, 0xf1, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xe3, 0xf1, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xe3,
     0xf1, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x02, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xfe,
     0x0f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff, 0x1f, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x0f, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xff,
     0xff, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xbf, 0xff, 0xa0, 0x00, 0x00,
     0x00, 0x00, 0x1f, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x36, 0xff,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x60,
     0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x02, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
     0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00},
    {// Frame 1
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
     0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xf8, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1b, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0xf8, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xf0, 0xf8, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0,
     0xf8, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff,
     0x87, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xff, 0xcf, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x1f, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff, 0xff, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
     0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x18, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
     0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00},
    {// Frame 2 (skipped at runtime)
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x01, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1c, 0x7f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0xfe, 0x40, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x3c, 0x7c, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c,
     0x7c, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xfe, 0x60, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
     0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xe0, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xff, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
     0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0xe0, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xff, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x3f, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfe, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x06, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
     0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00},
    {// Frame 3
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1f, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x9e, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xfd, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xfe, 0x7f, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xfe, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xfe,
     0x3e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xfe, 0x73, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7f,
     0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xfd, 0x88, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff,
     0xff, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x07, 0xef, 0xff, 0xa0, 0x00, 0x00,
     0x00, 0x00, 0x1f, 0xdf, 0xff, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xbf,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x10, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x0f, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xff,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
     0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x10, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
     0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00},
    {// Frame 4
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf0, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x3f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
     0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0xfe, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x01, 0x7e, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xc7, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xff, 0xc7, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff,
     0xc7, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xe6, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0xff,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a, 0xff, 0xff, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x0f, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff,
     0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x38,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
     0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x70, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00},
    {// Frame 5
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf8, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1b, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x7e, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x7f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf9, 0xf9, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xf1, 0xf8, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0xf1,
     0xf8, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xf9, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x01, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00,
     0x00, 0x00, 0x03, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x07, 0xdf,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0xff, 0xe0, 0x00, 0x00,
     0x00, 0x00, 0x0d, 0xff, 0xff, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x1e, 0xff,
     0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0x60, 0x00, 0x00,
     0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
     0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xfc, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x1f, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
     0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x80, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x10, 0x03, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30,
     0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00}};

// ================= PET CORE ENGINE (LOCAL) =================
struct PetLocalState {
  int hunger = 0;               // 0-100 (100 = starving)
  int health = 100;             // 0-100
  int energy = 100;             // 0-100
  int happiness = 100;          // 0-100
  int discipline = 100;         // 0-100
  int xp = 0;                   // Cumulative experience
  int level = 1;                // Pet level
  int ageInt = 0;               // Integer age in days
  uint32_t totalUptimeSecs = 0; // Cumulative active life time
  bool isSick = false;
  bool hasPoop = false;
  uint32_t version = 0; // State version for conflict resolution
};

PetLocalState g_petState;
unsigned long lastPhysioTick = 0;
#define PHYSIO_TICK_MS 360000 // Run physiology logic every 120 seconds

void savePetState() {
  petPrefs.begin("pet_state", false);
  petPrefs.putInt("hunger", g_petState.hunger);
  petPrefs.putInt("health", g_petState.health);
  petPrefs.putInt("energy", g_petState.energy);
  petPrefs.putInt("happiness", g_petState.happiness);
  petPrefs.putInt("discipline", g_petState.discipline);
  petPrefs.putInt("xp", g_petState.xp);
  petPrefs.putInt("level", g_petState.level);
  petPrefs.putInt("ageInt", g_petState.ageInt);
  petPrefs.putUInt("uptime", g_petState.totalUptimeSecs);
  petPrefs.putBool("isSick", g_petState.isSick);
  petPrefs.putBool("hasPoop", g_petState.hasPoop);
  petPrefs.putInt("version", g_petState.version);
  petPrefs.end();
  Serial.println("💾 Pet state saved to NVS");
}

void loadPetState() {
  petPrefs.begin("pet_state", true); // Read-only
  g_petState.hunger = petPrefs.getInt("hunger", 0);
  g_petState.health = petPrefs.getInt("health", 100);
  g_petState.energy = petPrefs.getInt("energy", 100);
  g_petState.happiness = petPrefs.getInt("happiness", 100);
  g_petState.discipline = petPrefs.getInt("discipline", 100);
  g_petState.xp = petPrefs.getInt("xp", 0);
  g_petState.level = petPrefs.getInt("level", 1);
  g_petState.ageInt = petPrefs.getInt("ageInt", 0);
  g_petState.totalUptimeSecs = petPrefs.getUInt("uptime", 0);
  g_petState.isSick = petPrefs.getBool("isSick", false);
  g_petState.hasPoop = petPrefs.getBool("hasPoop", false);
  g_petState.version = petPrefs.getInt("version", 0);
  petPrefs.end();
  Serial.printf("📂 Pet state loaded: Age %d, Lvl %d, XP %d\n",
                g_petState.ageInt, g_petState.level, g_petState.xp);
}

// ================= OLED ANIMATION DISPLAY =================
enum PetAge { INFANT = 0, CHILD = 1, ADULT = 2, OLD = 3 };
PetAge petAge = INFANT; // Default to INFANT - server manages aging
unsigned long lastAnimationTime = 0;
unsigned long lastDisplayCheckTime =
    0; // Track when we last checked server for OLED display state
const unsigned long DISPLAY_CHECK_INTERVAL =
    3000; // Poll server for OLED+events every 3 seconds (combined slot)
const unsigned long ANIMATION_DISPLAY_INTERVAL =
    100; // Display animation every 100ms (~10 FPS smooth)
uint8_t currentFrame = 0;
bool displayReady = false;
bool startupComplete = false; // Track if startup egg animation is done
bool showHomeIcon = true;     // ESP32 controls: Always show on MAIN screen
bool showFoodIcon = false;    // Show food icon when pet is hungry
bool showPoopIcon = false;    // Show poop icon when poop present
bool showSickIcon = false;    // Show heart/sick icon after poop ignored >15 min
                              // (only when poop cleared)
bool showPlayIcon = false;    // Show play icon 15 min after poop cleared
                              // (top-left, paused when sick)
unsigned long poopClearedTime = 0; // millis() when poop was last cleaned
String currentScreenType =
    "MAIN"; // Current menu — controlled locally via right-tilt gesture
String currentEmotion = "IDLE";  // Current emotion from server (IDLE, CRY, SAD,
                                 // HAPPY, EATING, SURPRISE)
bool justFinishedEating = false; // Show GOOD! text after eating animation
unsigned long eatingFinishTime =
    0; // When eating finished (for GOOD! text timer)
bool isServerEmotionOverride =
    false; // True if server has a sensory event override active

// Pet state tracking — updated from LOCAL g_petState
String currentMode = "HARDWARE"; // Now controlled locally
bool petIsHungry = false;        // Local hunger > 70
int petAgeInt = 0;               // Day count
int petHappiness = 100;          // 0-100 bar
int petDiscipline = 100;         // 0-100 bar
bool petIsSick = false;          // Track if pet is sick

// Helper to sync struct to legacy globals used by UI
void syncLocalStateToUI() {
  petAgeInt = g_petState.ageInt;
  petHappiness = g_petState.happiness;
  petDiscipline = g_petState.discipline;
  petIsHungry = (g_petState.hunger > 70);
  petIsSick = g_petState.isSick;
  showPoopIcon = g_petState.hasPoop;
  showSickIcon = petIsSick;  // Heart icon when pet is sick
  showFoodIcon = petIsHungry;

  // 🎮 Play icon: appears 15 min (900000ms) after poop is cleared
  if (poopClearedTime > 0 && !showPoopIcon && !petIsSick &&
      millis() - poopClearedTime >= 900000) {
    showPlayIcon = true;
  }
  // Reset play icon when poop reappears or pet gets sick
  if (showPoopIcon || petIsSick) {
    showPlayIcon = false;
    if (showPoopIcon)
      poopClearedTime = 0; // new poop invalidates timer
  }

  // 🧠 LOCAL EMOTION CALCULATION (Authoritative unless server overrides with
  // sensory event)
  if (!isServerEmotionOverride) {
    if (petIsSick)
      currentEmotion = "SICK";
    else if (showPoopIcon)
      currentEmotion = "POOP";
    else if (petIsHungry) {
      if (petAge == INFANT)
        currentEmotion = "CRY";
      else
        currentEmotion = "HUNGER";
    } else if (petHappiness > 80)
      currentEmotion = "HAPPY";
    else if (petHappiness < 40)
      currentEmotion = "SAD";
    else
      currentEmotion = "IDLE";
  }

  // Map integer age to PetAge enum for animations
  if (petAgeInt <= 5)
    petAge = INFANT;
  else if (petAgeInt <= 10)
    petAge = CHILD;
  else if (petAgeInt <= 17)
    petAge = ADULT;
  else
    petAge = OLD;
}

// Right-tilt hold for menu switching
bool holdingRightForMenu = false; // Track right-tilt hold for menu cycling
unsigned long menuTiltHoldStartTime = 0;        // When right tilt started
const unsigned long MENU_TILT_HOLD_TIME = 2000; // Hold 2 seconds to cycle menu
unsigned long lastMenuCycleTime = 0;            // Cooldown between menu cycles
const unsigned long MENU_CYCLE_COOLDOWN = 3000; // 3 seconds cooldown

volatile bool cameraCapturing =
    false; // Flag to prevent camera access conflicts
bool imageAlreadySentThisSession =
    false; // Track if image sent for current food session

// ================= SLEEP MODE STATE =================
bool isDeviceSleeping = false;      // True when in sleep mode (neutral >30s)
unsigned long neutralStartTime = 0; // When neutral state first detected
unsigned long sleepStartTime = 0;   // When sleep mode started
uint32_t accumulatedSleepSec =
    0; // Sleep seconds banked, sent on next sensor upload
const unsigned long NEUTRAL_SLEEP_TIMEOUT = 30000; // 30s inversion → sleep

// Walking state — driven by hardware step counter (not server)
bool petIsWalking = false;
unsigned long lastWalkingStepTime = 0; // millis() of last detected step
const unsigned long WALKING_WINDOW_MS =
    3000; // animate walking for 3s after last step

// ── HARDWARE STEP COUNTER ──────────────────────────────────────────────
uint32_t hwStepCount = 0;         // Steps accumulated since last sensor send
unsigned long lastHwStepTime = 0; // millis() of last detected step (debounce)
const float STEP_BARRIER_G2 =
    0.25f; // Increased to 0.25f to filter out touches/taps
const unsigned long STEP_MIN_MS =
    600; // Increased to 600ms to prevent double-bouncing from touches
const float LP_ALPHA_STEP =
    0.85f; // low-pass filter weight for gravity estimate

// ── OTA UPDATE STATE ───────────────────────────────────────────────────
bool otaUpdateRequested = false; // Set true when server sends ota_update flag

// Global SSL client for OTA — WiFiClientSecure is ~16KB, MUST be global (not on
// stack)
WiFiClientSecure sslOTA;
// Global SSL client for ALL network calls — shared across
// sensor/OLED/events/clean/inject
WiFiClientSecure sslNet;
uint8_t otaBuf[4096]; // OTA download buffer — global to save stack

// HTTP error tracking — auto-reset SSL after consecutive failures
int httpConsecutiveErrors = 0;
const int HTTP_ERROR_RESET_THRESHOLD =
    3; // Reset SSL after 3 consecutive errors

// Reset stale SSL connection (fixes HTTP -1 / connection refused after idle)
void resetSSLConnection() {
  Serial.println("🔄 Resetting SSL connection...");
  sslNet.stop();
  sslNet.setInsecure();
  sslNet.setTimeout(10);
  httpConsecutiveErrors = 0;
  Serial.println("✅ SSL connection reset");
}

// Call after every HTTP result to track errors and auto-reset
void trackHttpResult(int httpCode) {
  if (httpCode > 0) {
    httpConsecutiveErrors = 0; // Success — reset counter
  } else {
    httpConsecutiveErrors++;
    Serial.printf("⚠️ HTTP error %d (consecutive: %d/%d)\n", httpCode,
                  httpConsecutiveErrors, HTTP_ERROR_RESET_THRESHOLD);
    if (httpConsecutiveErrors >= HTTP_ERROR_RESET_THRESHOLD) {
      resetSSLConnection();
    }
  }
}

// CPU frequency guard — prevents Core 0/1 race condition on
// setCpuFrequencyMhz()
SemaphoreHandle_t cpuFreqMutex = NULL;
void safeCpuFreq(int mhz) {
  if (cpuFreqMutex &&
      xSemaphoreTake(cpuFreqMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    setCpuFrequencyMhz(mhz);
    xSemaphoreGive(cpuFreqMutex);
  }
}

// LP gravity estimate — tracks gravity at ANY tilt (initialised flat)
float stepGravX = 0.0f, stepGravY = 0.0f, stepGravZ = 1.0f;

// ================= PLAY MENU GAME STATE =================
enum GameState {
  GAME_IDLE,     // Not playing (shows static PLAY screen)
  GAME_PLAYING,  // Actively playing
  GAME_OVER_ANIM // Game over animation with coins
};

GameState playGameState = GAME_IDLE;

// Game variables
float playerX = 28;
const int playerWidth = 12;
const int playerHeight = 3;
const int playerY = 28;

int foodX = 10;
int foodY = 0;
const int foodSize = 4;

int gameScore = 0;
int missCount = 0;
int comboCount = 0;
int scoreIncrement = 1;

unsigned long lastFallTime = 0;
const int fallSpeed = 120;

unsigned long gameOverStartTime = 0;
unsigned long gameStartTime = 0;
unsigned long holdStartTime = 0;
bool holdingLeft = false;

// Which game is active: 0 = catch food, 1 = dodge obstacle
int activeGame = 0;

// Game 2 (Dodge) variables
int obsX = 0;
int obsY = 0;
const int obsSize = 4;
int dodgeFallSpeed = 120;
unsigned long lastObsFall = 0;
int dodgeScore = 0;
bool dodgeGameOver = false;
int walkFrame = 0;
unsigned long lastWalkFrameTime = 0;
bool dodgeGameOverAnimDone = false;

// Age Transition Animation
volatile bool pendingAgeTransition = false; // Set by physioTick (Core 1), consumed by OLED (Core 0)
int ageTransitionXP = 0;   // XP to show in counter
int ageTransitionPrevXP = 0; // XP before birthday bonus (counter start)
int ageTransitionAge = 0;  // New age to show in counter

// Health Menu Medicine Variables
bool givingMedicine = false;   // Track if medicine animation is playing
int medicineAnimLoopCount = 0; // Count how many times animation has looped
unsigned long medicineHoldStartTime = 0; // Track when tilt started for medicine
bool holdingLeftForMedicine = false; // Track if holding left tilt for medicine
unsigned long medicineAnimStartTime = 0; // Track animation start time
int currentInjectionFrame = 0;           // Current frame in injection animation

// Food Menu Feeding Gesture Variables
bool holdingLeftForFeeding = false; // Track if holding left tilt for feeding
unsigned long feedingHoldStartTime = 0; // Track when tilt started for feeding
bool capturingForFeeding =
    false; // Flag: feeding gesture active, camera task will capture
unsigned long feedingGestureStartTime =
    0;                        // Track when feeding gesture was triggered
#define FEEDING_TIMEOUT 30000 // 30 second timeout for feeding gesture
bool justFedPet = false; // Flag to ignore server hunger updates after feeding
unsigned long lastFeedTime = 0; // Track when pet was last fed
#define FEED_IGNORE_DURATION                                                   \
  10000 // Ignore server hunger updates for 10 seconds after feeding

// Toilet Menu Cleaning Gesture Variables
bool holdingLeftForCleaning = false; // Track if holding left tilt for cleaning
unsigned long cleaningHoldStartTime = 0; // Track when tilt started for cleaning
bool isCleaningPoop = false;             // Flag for cleaning in progress
int cleaningFadeStep = 0;                // Fade animation step (0-10)
unsigned long cleaningStartTime = 0;     // When cleaning animation started
// Clean slide animation vars
int cleanSlideX = 64; // Current x position of sliding sprite (starts off right)
int cleanSlideFrame = 1;              // Alternates between 1 and 2
unsigned long cleanSlideLastStep = 0; // Last time sprite moved
bool justCleanedPet = false;          // Flag to show "Cleared" after cleaning

// Smooth control
float filteredX = 0;
float velocity = 0;

// Falling coins animation
#define COIN_COUNT 6
int coinX[COIN_COUNT];
int coinY[COIN_COUNT];
int coinSpeed[COIN_COUNT];

// MPU6050 sensor
MPU6050 mpu;
bool mpuAvailable = false; // Track if MPU6050 is working

// Pins
const int LED_PIN = 2; // LED indicator

// Voice Activity Detection
volatile bool speechDetected = false;
volatile bool audioReady = false;
volatile int audioEnergyLevel = 0;
String detectedAudioData = "";

// Dual-core synchronization
SemaphoreHandle_t audioMutex;
SemaphoreHandle_t cameraMutex;
TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t cameraTaskHandle = NULL;
TaskHandle_t oledTaskHandle = NULL;

// OTA in-progress flag — pauses all other tasks
volatile bool otaInProgress = false;

// Camera data ready flag
volatile bool cameraImageReady = false;
uint8_t *capturedImageBuffer = NULL;
size_t capturedImageLength = 0;

// VAD Settings
#define VAD_THRESHOLD 1000   // Energy threshold for speech detection
#define VAD_MIN_DURATION 500 // Minimum 500ms of speech to trigger
#define SILENCE_TIMEOUT 2000 // 2s of silence before stopping recording

// Structure for single sensor reading with timestamp
struct SingleReading {
  unsigned long timestamp_ms; // Millisecond timestamp
  float accel_x, accel_y, accel_z;
  float gyro_x, gyro_y, gyro_z;
};

// Structure for buffered sensor data (batch of readings)
struct SensorDataBatch {
  static const int MAX_READINGS = 20; // Store up to 20 readings
  int reading_count;
  SingleReading readings[MAX_READINGS];
  float avg_mic_level;
  int sound_data;
};

// Timing
unsigned long lastSendTime = 0;
unsigned long lastImageCapture = 0;
unsigned long lastEventPoll = 0;        // Event polling timing
unsigned long lastInternalReadTime = 0; // Fast internal sensor reading timing
const unsigned long SEND_INTERVAL =
    2000; // Send sensor data batch every 2 seconds
const unsigned long INTERNAL_READ_INTERVAL =
    100; // Read sensor batch every 100ms internally
const unsigned long EVENT_POLL_INTERVAL =
    3000; // Poll for events every 3 seconds
unsigned long dynamicEventPollInterval =
    3000; // Dynamic backoff for event polling
// Audio now triggered by speech detection, not timer

// Sensor reading buffer (batched between network sends)
SensorDataBatch sensorBatch = {};
float totalMicLevel = 0.0;
int micReadingCount = 0;

// ⏸️ PAUSE CONTROL FOR UPLOADS
bool isUploadingImage = false; // Flag to pause sensor data during image upload

// Camera and audio status
bool cameraReady = false;
bool micReady = false;
uint8_t *audio_buffer = NULL;

// Audio processing buffers for VAD
int16_t *vad_buffer;
const int VAD_BUFFER_SIZE = 512;

// PDM microphone handle
i2s_chan_handle_t rx_handle = NULL;

// Structure for sensor data
struct SensorData {
  float accel_x, accel_y, accel_z;
  float gyro_x, gyro_y, gyro_z;
  float mic_level;
  int sound_data;
  String camera_image_b64; // Base64 encoded image
  String audio_data_b64;   // Base64 encoded audio
  bool has_new_image;
  bool has_new_audio;

  // Orientation tracking fields
  String device_orientation;
  float orientation_confidence;
  float calibrated_ax, calibrated_ay, calibrated_az;

  // ESP32 internal temperature
  float chip_temperature; // Internal chip temperature in °C

  // Batch of sensor readings for better step detection
  SensorDataBatch sensor_batch;
};

// ================= NETWORK TASK QUEUE =================
// All HTTP calls dispatched through this queue to networkTask (Core 1)
// Active types: NET_SENSOR, NET_OLED, NET_IMAGE, NET_CLEAN, NET_INJECT,
// NET_HAPPY NET_EVENTS unused (events polled inside NET_OLED handler
// back-to-back)
enum NetReqType : uint8_t {
  NET_SENSOR = 0, // Send sensor batch to server (every 2s)
  NET_OLED = 1,   // Poll OLED state + events combined (every 5s)
  NET_EVENTS = 2, // UNUSED — events now handled inside NET_OLED
  NET_IMAGE = 3,  // Upload camera image to server (on feeding gesture)
  NET_CLEAN = 4,  // Send cleaning request (left tilt 3s on TOILET_MENU)
  NET_INJECT = 5, // Send medicine given (left tilt 3s on HEALTH_MENU)
  NET_HAPPY = 6,       // Right tilt menu cycle interaction → happiness +5
  NET_GAME_REWARD = 7  // Send game score + KakuCoin reward after game over
};

// Pending game reward (written by OLED task, read by networkTask)
volatile int pendingRewardScore = 0;
volatile float pendingRewardKC = 0;

QueueHandle_t networkQueue;         // Queue: loop() → networkTask
SemaphoreHandle_t networkDataMutex; // Protect g_pendingSensor
SensorData g_pendingSensor;         // Shared sensor data for networkTask

// ================= NETWORK TASK QUEUE =================
// Global StaticJsonDocuments — allocated once, no heap fragmentation
StaticJsonDocument<768> g_oledDoc;    // Reused for OLED+events polling
StaticJsonDocument<4096> g_sensorDoc; // Reused for sensor data sending

// ================= ORIENTATION DETECTION =================
// NOTE: Direction detection moved to Flask server
// ESP32 now only sends raw MPU6050 data

// NOTE: Orientation computation moved to Flask server (see app.py)
// Server will compute direction from raw accel data

// ================= FORWARD DECLARATIONS =================
void displayPetAnimation();
SensorData readAllSensors();
String captureImageBase64();
bool sendSensorDataOnly(SensorData data);
void sendImageData(String imageBase64);
void sendAudioData(String audioBase64);
// void pollForEvents(); // Removed - now bundled with OLED poll
bool isServerAlive();
void scanI2CDevices();
bool initCamera();
bool initAudio();
void audioMonitorTask(void *parameter);
void cameraMonitorTask(void *parameter);
void processEvent(const char *event_type, const char *message);
void acknowledgeEvent(int event_id);
void generate_wav_header(uint8_t *wav_header, uint32_t wav_size,
                         uint32_t sample_rate);
void sendAllDataToServer(SensorData data);
String recordAudioBase64();
void notifyServerStartupComplete(); // Notify server startup is complete
void getOLEDDisplayFromServer(); // GET /api/oled-display/get → update pet state
                                 // vars
void drawHomeIcon();             // Draw home icon pixel-by-pixel
void drawFoodIcon();             // Draw food icon (bottom-right)
void drawPoopIcon();             // Draw poop icon (bottom-right)
void drawPlayIcon(); // Draw blinking play icon (top-left, 1hr after feed)
void drawSickIcon(); // Draw blinking sick/heart icon (bottom-right)
void playEatingAnimation();  // Play eating animation
void drawStaticFoodIcon();   // Static food icon at top-left (food menu)
void drawStaticToiletIcon(); // Static toilet icon at top-left (toilet menu)
void drawCleanSpriteFrame(int frame, int xOffset); // Clean slide sprite frame
void drawStaticPlayIcon();     // Static play icon at top-left (play menu)
void drawStaticHealthIcon();   // Static heart icon at top-left (health menu)
void playInjectionAnimation(); // Play injection/medicine animation
void playAgeTransitionAnimation(); // Age-up celebration animation
void sendInjectRequest();      // Notify server that injection was given
void displayFoodMenu();        // Display food menu screen
void displayToiletMenu();      // Display toilet menu screen
void displayPlayMenu();        // Display play menu screen
void displayHealthMenu();      // Display health menu screen
void displayStatusInfoMenu();  // Display status info menu (smiley + age + flash
                               // + score)
void displayStatsMenu();     // Display stats menu (happiness + discipline bars)
void checkMenuTiltGesture(); // Right tilt 2s hold → cycle menu
bool isDeviceNeutral(); // True when device lies flat (no significant X/Y tilt)
void detectHardwareStep(); // Count steps via MPU6050 stoss method
const char *detectDirection(SensorData data); // Orientation from accel data
bool isDeviceInverted();                      // True when device is face-down
void displaySleepingAnimation();              // 2-frame sleeping animation
void displayWalkingAnimation(); // 6-frame walking animation (skips frame 2)
void cycleMenu(); // Cycle menus: MAIN → FOOD → TOILET → PLAY → HEALTH → STATUS
                  // → STATS → MAIN
void oledTask(void *parameter); // OLED animation task on Core 0
void networkTask(
    void *parameter); // Dedicated HTTP task on Core 1 (queue-driven)\nvoid
                      // checkAndPerformOTA();           // Check firmware
                      // server and perform OTA if newer version exists

// ================= WIFI PROVISIONING FUNCTIONS =================

// QRCodeGFX object — initialised after display is ready
QRCodeGFX *qrcode = nullptr;

// Get count of stored WiFi credentials from NVS
int getStoredWiFiCount() {
  wifiPrefs.begin("wifi", true); // Read-only
  int count = wifiPrefs.getInt("wifi_count", 0);
  wifiPrefs.end();
  return count;
}

// Append a new WiFi credential to NVS (never overwrites previous entries)
void appendWiFiCredential(String ssid, String pass) {
  wifiPrefs.begin("wifi", false); // Read-write
  int count = wifiPrefs.getInt("wifi_count", 0);

  // Check for duplicate SSID — update password instead of adding duplicate
  for (int i = 0; i < count; i++) {
    String key_s = "wifi" + String(i) + "_s";
    String stored = wifiPrefs.getString(key_s.c_str(), "");
    if (stored == ssid) {
      // Update existing entry's password
      String key_p = "wifi" + String(i) + "_p";
      wifiPrefs.putString(key_p.c_str(), pass);
      wifiPrefs.end();
      Serial.printf("📶 Updated password for existing SSID: %s (slot %d)\n",
                    ssid.c_str(), i);
      return;
    }
  }

  if (count >= MAX_STORED_WIFI) {
    Serial.println("⚠️ Max stored WiFi networks reached, overwriting oldest");
    // Shift all entries down by 1 (remove slot 0, move 1→0, 2→1, etc.)
    for (int i = 0; i < count - 1; i++) {
      String src_s = "wifi" + String(i + 1) + "_s";
      String src_p = "wifi" + String(i + 1) + "_p";
      String dst_s = "wifi" + String(i) + "_s";
      String dst_p = "wifi" + String(i) + "_p";
      wifiPrefs.putString(dst_s.c_str(),
                          wifiPrefs.getString(src_s.c_str(), ""));
      wifiPrefs.putString(dst_p.c_str(),
                          wifiPrefs.getString(src_p.c_str(), ""));
    }
    count = MAX_STORED_WIFI - 1; // Will be incremented below
  }

  String key_s = "wifi" + String(count) + "_s";
  String key_p = "wifi" + String(count) + "_p";
  wifiPrefs.putString(key_s.c_str(), ssid);
  wifiPrefs.putString(key_p.c_str(), pass);
  wifiPrefs.putInt("wifi_count", count + 1);
  wifiPrefs.end();
  Serial.printf("📶 Saved WiFi credential: %s (slot %d)\n", ssid.c_str(),
                count);
}

// Try connecting to a specific WiFi network with retries
bool tryConnectWiFi(const char *ssid, const char *pass, int retries) {
  for (int attempt = 1; attempt <= retries; attempt++) {
    Serial.printf("📶 Trying '%s' (attempt %d/%d)...\n", ssid, attempt,
                  retries);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
      delay(500);
      Serial.print(".");
      timeout++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("✅ Connected to '%s' — IP: %s\n", ssid,
                    WiFi.localIP().toString().c_str());
      return true;
    }

    Serial.printf("❌ Failed to connect to '%s' (attempt %d)\n", ssid, attempt);
    WiFi.disconnect();
    delay(500);
  }
  return false;
}

// Try all stored credentials + hardcoded fallback
bool connectWithStoredCredentials() {
  int count = getStoredWiFiCount();
  Serial.printf("📶 Found %d stored WiFi network(s) in NVS\n", count);

  // 1. Try NVS-stored credentials (newest first for faster connect)
  if (count > 0) {
    wifiPrefs.begin("wifi", true);
    for (int i = count - 1; i >= 0; i--) {
      String key_s = "wifi" + String(i) + "_s";
      String key_p = "wifi" + String(i) + "_p";
      String ssid = wifiPrefs.getString(key_s.c_str(), "");
      String pass = wifiPrefs.getString(key_p.c_str(), "");
      wifiPrefs.end();

      if (ssid.length() > 0) {
        if (tryConnectWiFi(ssid.c_str(), pass.c_str(), WIFI_CONNECT_RETRIES)) {
          return true;
        }
      }

      wifiPrefs.begin("wifi", true); // Re-open for next iteration
    }
    wifiPrefs.end();
  }

  // 2. Try hardcoded fallback credentials
  Serial.println("📶 Trying hardcoded WiFi credentials...");
  if (tryConnectWiFi(WIFI_SSID, WIFI_PASSWORD, WIFI_CONNECT_RETRIES)) {
    return true;
  }

  Serial.println("❌ All WiFi credentials failed!");
  return false;
}

// Draw QR code on the 64x32 OLED
void drawProvisioningQR(String data) {
  if (!displayReady || !qrcode)
    return;

  display.clearDisplay();
  qrcode->setScale(1);
  qrcode->setBackgroundColor(WHITE);
  qrcode->generateData(data);
  qrcode->setRotation(QRCodeRotation::R90);
  qrcode->draw(0, 0, false);
  display.display();
}

// Show WiFi connected success screen on OLED
void showWiFiSuccess() {
  if (!displayReady)
    return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WiFi OK");
  display.setCursor(0, 12);
  display.print(WiFi.SSID());
  display.setCursor(0, 24);
  display.print(WiFi.localIP());
  display.display();
  delay(2000);
}

// WiFi event: detect phone connecting to our AP
void WiFiProvisioningEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    Serial.println("📱 Phone connected to KAKU_SETUP AP");
    phoneConnectedToAP = true;
    // Show URL QR so user can open config page
    drawProvisioningQR("http://192.168.4.1");
  }
}

// Generate HTML page with WiFi network list and form
String wifiConfigHtmlPage() {
  int n = WiFi.scanNetworks();

  String page = "<!DOCTYPE html><html><head>";
  page += "<meta charset='UTF-8'><meta name='viewport' "
          "content='width=device-width,initial-scale=1'>";
  page += "<title>KAKU WiFi Setup</title>";
  page += "<style>";
  page += "*{margin:0;padding:0;box-sizing:border-box}";
  // Animated gradient background
  page += "body{font-family:'Segoe "
          "UI',system-ui,-apple-system,sans-serif;min-height:100vh;";
  page += "background:linear-gradient(135deg,#0a0a1a 0%,#1a0a2e 25%,#0a1a2e "
          "50%,#1a0a1a 75%,#0a0a2e 100%);";
  page += "background-size:400% 400%;animation:bg 15s ease "
          "infinite;color:#e0e0e0;padding:20px}";
  page += "@keyframes bg{0%,100%{background-position:0% "
          "50%}50%{background-position:100% 50%}}";
  // Glassmorphism card
  page += ".card{max-width:420px;margin:20px "
          "auto;background:rgba(255,255,255,0.05);";
  page += "backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);";
  page += "border:1px solid "
          "rgba(255,255,255,0.08);border-radius:20px;padding:32px 24px;";
  page += "box-shadow:0 8px 32px rgba(0,0,0,0.4)}";
  // Logo / header
  page += ".logo{text-align:center;margin-bottom:24px}";
  page += ".logo span{font-size:40px;display:block;margin-bottom:8px}";
  page +=
      ".logo h1{font-size:22px;font-weight:700;color:#fff;letter-spacing:1px}";
  page += ".logo p{font-size:13px;color:#8888aa;margin-top:4px}";
  // Section title
  page += ".section-title{font-size:12px;text-transform:uppercase;letter-"
          "spacing:2px;color:#7c6fa0;margin:20px 0 10px;font-weight:600}";
  // Network list
  page += ".nets{max-height:220px;overflow-y:auto;margin-bottom:16px;scrollbar-"
          "width:thin;scrollbar-color:#333 transparent}";
  page += ".nets::-webkit-scrollbar{width:4px}.nets::-webkit-scrollbar-thumb{"
          "background:#444;border-radius:4px}";
  page += ".net{display:flex;align-items:center;justify-content:space-between;"
          "padding:12px 14px;margin:6px 0;";
  page += "background:rgba(255,255,255,0.04);border:1px solid "
          "rgba(255,255,255,0.06);border-radius:12px;";
  page += "cursor:pointer;transition:all 0.2s ease}";
  page += ".net:hover,.net:active{background:rgba(233,69,96,0.12);border-color:"
          "rgba(233,69,96,0.3);transform:scale(1.01)}";
  page += ".net.selected{background:rgba(233,69,96,0.18);border-color:#e94560}";
  page += ".net-name{font-size:14px;font-weight:500;color:#fff;flex:1;overflow:"
          "hidden;text-overflow:ellipsis;white-space:nowrap}";
  // Signal strength bars
  page += ".signal{display:flex;align-items:flex-end;gap:2px;margin-left:10px;"
          "min-width:20px}";
  page += ".bar{width:3px;background:#333;border-radius:1px;transition:"
          "background 0.3s}";
  page += ".bar.on{background:#e94560}";
  // Form
  page += ".input-group{position:relative;margin:10px 0}";
  page += ".input-group input{width:100%;padding:14px "
          "16px;background:rgba(255,255,255,0.06);";
  page += "border:1px solid "
          "rgba(255,255,255,0.1);border-radius:12px;color:#fff;font-size:15px;";
  page += "outline:none;transition:border-color 0.3s}";
  page += ".input-group input:focus{border-color:#e94560;box-shadow:0 0 0 3px "
          "rgba(233,69,96,0.15)}";
  page += ".input-group input::placeholder{color:#555}";
  // Password toggle
  page += ".eye{position:absolute;right:14px;top:50%;transform:translateY(-50%)"
          ";cursor:pointer;color:#666;font-size:18px;user-select:none}";
  page += ".eye:hover{color:#aaa}";
  // Submit button
  page += ".btn{width:100%;padding:14px;margin-top:20px;background:linear-"
          "gradient(135deg,#e94560,#c02050);";
  page += "border:none;border-radius:12px;color:#fff;font-size:16px;font-"
          "weight:600;cursor:pointer;";
  page += "letter-spacing:0.5px;transition:all "
          "0.3s;position:relative;overflow:hidden}";
  page += ".btn:hover{transform:translateY(-1px);box-shadow:0 6px 20px "
          "rgba(233,69,96,0.4)}";
  page += ".btn:active{transform:scale(0.98)}";
  page += ".btn.loading{pointer-events:none;opacity:0.7}";
  page += ".btn.loading::after{content:'';position:absolute;width:20px;height:"
          "20px;border:2px solid transparent;";
  page += "border-top-color:#fff;border-radius:50%;animation:spin 0.8s linear "
          "infinite;top:50%;left:50%;margin:-10px 0 0 -10px}";
  page += "@keyframes spin{to{transform:rotate(360deg)}}";
  // No networks message
  page += ".empty{text-align:center;padding:20px;color:#666;font-style:italic;"
          "font-size:13px}";
  // Footer
  page +=
      ".footer{text-align:center;margin-top:20px;font-size:11px;color:#444}";
  page += "</style></head><body>";

  page += "<div class='card'>";
  page += "<div class='logo'><span>&#x1F43E;</span><h1>KAKU</h1><p>WiFi "
          "Setup</p></div>";

  // Network list
  page += "<div class='section-title'>Available Networks</div>";
  page += "<div class='nets'>";
  if (n == 0) {
    page +=
        "<div class='empty'>No networks found. Type SSID manually below.</div>";
  }
  for (int i = 0; i < n; i++) {
    int rssi = WiFi.RSSI(i);
    // 4-level signal bars based on RSSI
    int level = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
    page += "<div class='net' onclick=\"pick('" + WiFi.SSID(i) + "',this)\">";
    page += "<span class='net-name'>" + WiFi.SSID(i) + "</span>";
    page += "<div class='signal'>";
    page += "<div class='bar" + String(level >= 1 ? " on" : "") +
            "' style='height:4px'></div>";
    page += "<div class='bar" + String(level >= 2 ? " on" : "") +
            "' style='height:7px'></div>";
    page += "<div class='bar" + String(level >= 3 ? " on" : "") +
            "' style='height:10px'></div>";
    page += "<div class='bar" + String(level >= 4 ? " on" : "") +
            "' style='height:13px'></div>";
    page += "</div></div>";
  }
  page += "</div>";

  // Form
  page += "<div class='section-title'>Credentials</div>";
  page += "<form action='/save' id='wf' onsubmit='return go()'>";
  page += "<div class='input-group'><input id='s' name='s' placeholder='WiFi "
          "Network Name' autocomplete='off' required></div>";
  page += "<div class='input-group'><input id='p' name='p' type='password' "
          "placeholder='Password'>";
  page += "<span class='eye' onclick='togglePw()'>&#x1F441;</span></div>";
  page += "<button type='submit' class='btn' id='btn'>Connect</button>";
  page += "</form>";

  page +=
      "<div class='footer'>KAKU Tamagotchi &bull; v" FIRMWARE_VERSION "</div>";
  page += "</div>";

  // JavaScript
  page += "<script>";
  page += "function pick(s,el){document.getElementById('s').value=s;";
  page += "document.querySelectorAll('.net').forEach(e=>e.classList.remove('"
          "selected'));";
  page += "el.classList.add('selected');document.getElementById('p').focus()}";
  page += "function togglePw(){var "
          "p=document.getElementById('p');p.type=p.type==='password'?'text':'"
          "password'}";
  page += "function go(){var "
          "s=document.getElementById('s').value;if(!s){alert('Enter a WiFi "
          "name');return false}";
  page += "document.getElementById('btn').classList.add('loading');document."
          "getElementById('btn').textContent='Connecting...';return true}";
  page += "</script>";
  page += "</body></html>";

  return page;
}

// Web server route: serve config page
void handleWifiConfigRoot() {
  wifiConfigServer.send(200, "text/html", wifiConfigHtmlPage());
}

// Web server route: save credentials and restart
void handleWifiConfigSave() {
  String ssid = wifiConfigServer.arg("s");
  String pass = wifiConfigServer.arg("p");

  if (ssid.length() == 0) {
    wifiConfigServer.send(
        400, "text/html",
        "<!DOCTYPE html><html><head><meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:'Segoe UI',sans-serif;min-height:100vh;"
        "background:linear-gradient(135deg,#0a0a1a,#1a0a2e,#0a1a2e);color:#fff;"
        "display:flex;align-items:center;justify-content:center}"
        ".box{text-align:center;padding:40px;background:rgba(255,255,255,0.05);"
        "border-radius:20px;border:1px solid rgba(255,255,255,0.08)}"
        "a{color:#e94560;text-decoration:none}</style></head>"
        "<body><div class='box'><div "
        "style='font-size:48px;margin-bottom:16px'>&#x26A0;</div>"
        "<h2>SSID Required</h2><p style='color:#888;margin:12px 0'>Please "
        "enter a WiFi network name.</p>"
        "<a href='/'>&#x2190; Go Back</a></div></body></html>");
    return;
  }

  // Append to NVS (does not overwrite existing entries)
  appendWiFiCredential(ssid, pass);

  wifiConfigServer.send(
      200, "text/html",
      "<!DOCTYPE html><html><head><meta name='viewport' "
      "content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:'Segoe UI',sans-serif;min-height:100vh;"
      "background:linear-gradient(135deg,#0a0a1a,#1a0a2e,#0a1a2e);color:#fff;"
      "display:flex;align-items:center;justify-content:center}"
      ".box{text-align:center;padding:40px;background:rgba(255,255,255,0.05);"
      "backdrop-filter:blur(20px);border-radius:20px;border:1px solid "
      "rgba(255,255,255,0.08);"
      "max-width:360px}"
      ".check{font-size:56px;margin-bottom:16px;animation:pop 0.5s ease}"
      "@keyframes "
      "pop{0%{transform:scale(0)}50%{transform:scale(1.2)}100%{transform:scale("
      "1)}}"
      "h2{margin-bottom:8px}p{color:#888;font-size:14px;margin:8px 0}"
      ".ssid{color:#e94560;font-weight:600}"
      ".bar{width:200px;height:4px;background:#222;border-radius:4px;margin:"
      "20px auto 8px;overflow:hidden}"
      ".fill{height:100%;background:linear-gradient(90deg,#e94560,#ff6b8a);"
      "border-radius:4px;"
      "animation:load 3s linear}</style></head>"
      "<body><div class='box'><div class='check'>&#x2705;</div>"
      "<h2>Saved!</h2>"
      "<p>Network: <span class='ssid'>" +
          ssid +
          "</span></p>"
          "<div class='bar'><div class='fill'></div></div>"
          "<p>KAKU is restarting...</p>"
          "<p style='font-size:12px;color:#555;margin-top:12px'>Connect your "
          "phone to <b>" +
          ssid +
          "</b></p>"
          "</div></body></html>");

  delay(2000);
  ESP.restart();
}

// Start WiFi AP mode for provisioning
void startWiFiProvisioningAP() {
  Serial.println("📡 Starting WiFi provisioning AP mode...");
  wifiProvisioningMode = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  WiFi.onEvent(WiFiProvisioningEvent);

  wifiConfigServer.on("/", handleWifiConfigRoot);
  wifiConfigServer.on("/save", handleWifiConfigSave);
  wifiConfigServer.begin();

  Serial.printf("📡 AP Started: %s (pass: %s)\n", AP_SSID, AP_PASS);
  Serial.println("📡 Config URL: http://192.168.4.1");

  // Show QR code with AP WiFi credentials for easy phone connection
  if (qrcode) {
    drawProvisioningQR("WIFI:T:WPA;S:KAKU_SETUP;P:12345678;;");
  } else {
    // Fallback: show text instructions on OLED
    if (displayReady) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("WiFi Setup");
      display.setCursor(0, 12);
      display.print(AP_SSID);
      display.setCursor(0, 24);
      display.print(AP_PASS);
      display.display();
    }
  }
}

// ================= OLED ANIMATION TASK (Core 0) =================
// Independent FreeRTOS task runs OLED animation on Core 0
// Decoupled from WiFi/HTTP calls on Core 1 for smooth 60 FPS display
void oledTask(void *parameter) {
  Serial.println("🎬 OLED Task started on Core 0");

  while (true) {
    if (displayReady && startupComplete) {
      displayPetAnimation(); // Draw animation (non-blocking)
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // ~10 FPS — sufficient for 64x32 OLED, saves CPU heat
  }
}

// ================= SETUP FUNCTION =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\nESP32 Dashboard Client Starting...");

  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // ================= WIFI PROVISIONING FLOW =================
  // 1. Try NVS stored credentials (3 retries each)
  // 2. Try hardcoded fallback credentials (3 retries)
  // 3. If all fail → start AP mode with QR code for provisioning
  Serial.println("📶 Starting WiFi provisioning flow...");

  // Initialize I2C and OLED FIRST so we can show QR code during provisioning
  Serial.println("Initializing I2C...");
  Wire.begin(5, 6); // XIAO ESP32 S3: SDA=5, SCL=6
  Wire.setClock(400000);
  delay(500);

  Serial.println("Initializing OLED Display...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("❌ OLED initialization failed!");
    displayReady = false;
  } else {
    Serial.println("✅ OLED initialized for provisioning");
    displayReady = true;
    // Create QRCodeGFX object now that display is ready
    qrcode = new QRCodeGFX(display);
    // Show "Connecting..." while trying credentials
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 8);
    display.println("KAKU");
    display.setCursor(0, 18);
    display.println("Connecting..");
    display.display();
  }

  if (!connectWithStoredCredentials()) {
    // All credentials failed — enter AP provisioning mode
    startWiFiProvisioningAP();
    Serial.println(
        "⚠️ Entered WiFi provisioning mode — loop() will serve config page");
    return; // Exit setup() early — loop() handles AP web server only
  }

  Serial.println("\n✅ WiFi Connected!");
  Serial.println(WiFi.localIP());
  if (displayReady)
    showWiFiSuccess();

  // Enable WiFi light-sleep — radio powers down between beacons (~80% power savings)
  // Connection stays alive (DTIM-based wake), latency increases by ~100ms
  WiFi.setSleep(true);
  // Auto-reconnect: SDK-level reconnect if AP drops (sealed device — no user intervention)
  WiFi.setAutoReconnect(true);
  // Reduced TX power to save energy (was 19.5dBm/MAX — overkill for most setups)
  // Options: WIFI_POWER_19_5dBm | _15dBm | _11dBm | _8_5dBm
  WiFi.setTxPower(WIFI_POWER_11dBm);
  Serial.println("📶 WiFi: light-sleep ON, auto-reconnect ON, TX 11dBm");
  vTaskDelay(pdMS_TO_TICKS(20)); // 20ms settle after WiFi config

  // Keep CPU at 240MHz during setup for stable I2C/camera/animation init
  // Will drop to 80MHz AFTER all initialization is complete
  Serial.println(
      "⚡ CPU at 240MHz during setup (will idle at 80MHz after init)");

  // I2C and OLED already initialized above (before WiFi provisioning)
  // No need to reinitialize Wire or display here

  // OLED already initialized above (before WiFi provisioning).
  // Run startup animation sequence now that WiFi is connected.
  if (displayReady) {
    // Check NVS: has the egg hatching animation already played?
    petPrefs.begin("pet_state", true); // read-only
    bool hasHatched = petPrefs.getBool("hasHatched", false);
    petPrefs.end();

    // FORCE_EGG_HATCH override — replays egg animation regardless of NVS
    if (FORCE_EGG_HATCH) hasHatched = false;

    if (!hasHatched) {
      // ============ FIRST-TIME STARTUP SEQUENCE ============
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 8);
      display.println("KAKU");
      display.setCursor(0, 18);
      display.println("Starting...");
      display.display();
      delay(1000);

      // Play egg cracking animation from all_pets.h
      Serial.println("🥚 Playing egg cracking animation...");
      playEggCrackingAnimation();

      // Egg screen slides to the left and exits
      Serial.println("🥚 Sliding egg screen out...");
      slideEggScreenOut();

      // Infant appears gradually from left side
      Serial.println("👶 Sliding infant in from left...");
      slideInfantSlowlyFromLeft();

      // Reset local pet state for fresh INFANT
      g_petState.hunger = 0;
      g_petState.health = 100;
      g_petState.energy = 100;
      g_petState.happiness = 100;
      g_petState.discipline = 100;
      g_petState.xp = 0;
      g_petState.level = 1;
      g_petState.ageInt = 0;
      g_petState.totalUptimeSecs = 0;
      g_petState.isSick = false;
      g_petState.hasPoop = false;
      savePetState(); // Save fresh stats to NVS

      // Mark hatching as done — will never play again
      petPrefs.begin("pet_state", false);
      petPrefs.putBool("hasHatched", true);  // FIX: was false (bug), now correctly true
      petPrefs.end();
      Serial.println("🥚 Hatching complete — NVS flag set, won't play again.");
    } else {
      // ============ SUBSEQUENT BOOT — LOAD SAVED STATE ============
      Serial.println(
          "📂 Pet already hatched — loading saved state from NVS...");
      loadPetState();
    }

    // Common for both paths
    startupComplete = true;
    showHomeIcon = true;
    syncLocalStateToUI();
    Serial.println("✅ Startup complete! Main screen ready.");
  }

  Serial.println("Scanning I2C devices...");
  scanI2CDevices();

  Serial.println("Initializing MPU6050...");
  bool mpuSuccess = false;

  // Try MPU6050 initialization with timeout
  unsigned long startTime = millis();
  while (!mpuSuccess && (millis() - startTime < 5000)) { // 5 second timeout
    mpu.initialize();
    delay(100);

    if (mpu.testConnection()) {
      Serial.println("✅ MPU6050 initialized successfully!");
      mpuSuccess = true;
      mpuAvailable = true; // Set flag for sensor readings

      // Warm up LP gravity filter (1s) — prevents cold-start false steps
      for (int i = 0; i < 40; i++) {
        int16_t wx, wy, wz;
        mpu.getAcceleration(&wx, &wy, &wz);
        float fx = wx / 16384.0f, fy = wy / 16384.0f, fz = wz / 16384.0f;
        stepGravX = LP_ALPHA_STEP * stepGravX + (1.0f - LP_ALPHA_STEP) * fx;
        stepGravY = LP_ALPHA_STEP * stepGravY + (1.0f - LP_ALPHA_STEP) * fy;
        stepGravZ = LP_ALPHA_STEP * stepGravZ + (1.0f - LP_ALPHA_STEP) * fz;
        delay(25);
      }
      Serial.println("✅ Step gravity filter warmed up");
    } else {
      Serial.print(".");
      delay(500);
    }
  }

  if (!mpuSuccess) {
    Serial.println("\n❌ MPU6050 initialization failed after 5 seconds");
    Serial.println(
        "⚠️  Continuing without MPU6050 (will send dummy sensor data)");
  }

  // Initialize Camera
  initCamera();

  // Initialize I2S Microphone
  initAudio();

  // NOW drop CPU to idle frequency — all hardware init is done
  setCpuFrequencyMhz(80);
  Serial.println("⚡ CPU idle at 80MHz (boosts to 160/240MHz for network/camera)");

  // Create mutexes for synchronization
  audioMutex = xSemaphoreCreateMutex();
  cameraMutex = xSemaphoreCreateMutex();
  cpuFreqMutex = xSemaphoreCreateMutex(); // CPU frequency race guard

  // Create network queue and mutex
  networkDataMutex = xSemaphoreCreateMutex();
  networkQueue =
      xQueueCreate(8, sizeof(uint8_t)); // Queue depth 8, each item 1 byte

  // Start audio monitoring task on Core 0 (dedicated to audio/VAD)
  xTaskCreatePinnedToCore(audioMonitorTask, // Task function
                          "AudioMonitor",   // Task name
                          8192,             // Stack size
                          NULL,             // Parameters
                          2, // Priority (high for real-time audio)
                          &audioTaskHandle, // Task handle
                          0                 // Core 0 (dedicated to audio)
  );

  // Start camera monitoring task on Core 0 (shares core with audio)
  xTaskCreatePinnedToCore(cameraMonitorTask, // Task function
                          "CameraMonitor",   // Task name
                          4096,              // Stack size
                          NULL,              // Parameters
                          1,                 // Priority (lower than audio)
                          &cameraTaskHandle, // Task handle
                          0                  // Core 0 (shared with audio)
  );

  // Start OLED animation task on Core 0 (independent of WiFi on Core 1)
  xTaskCreatePinnedToCore(
      oledTask,        // Task function
      "OLED",          // Task name
      4096,            // Stack size
      NULL,            // Parameters
      1,               // Priority (lower than audio)
      &oledTaskHandle, // Task handle — needed for OTA suspend
      0                // Core 0 (opposite of WiFi heavy Core 1)
  );

  // Init global OTA SSL client
  sslOTA.setInsecure();
  sslOTA.setTimeout(120);

  // Init global network SSL client (shared for sensor/OLED/events/etc)
  sslNet.setInsecure();
  sslNet.setTimeout(10);

  // Start dedicated network task on Core 1 (HTTP only, queue-driven)
  loadPetState();
  syncLocalStateToUI();

  xTaskCreatePinnedToCore(
      networkTask, // Task function
      "Network",   // Task name
      16384,       // Stack size (16K: WiFiClientSecure + HTTP + JSON + OTA)
      NULL,        // Parameters
      1,           // Priority
      NULL,        // Task handle
      1            // Core 1 (same as Arduino loop, shares cleanly)
  );

  Serial.println("System Ready!");
  Serial.println("🎤 Core 0: Audio + Camera + OLED");
  Serial.println("🌐 Core 1: Sensors + Network queue + HTTP");
}

void scanI2CDevices() {
  Serial.println("🔍 Scanning I2C devices...");
  byte error, address;
  int deviceCount = 0;

  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.printf("✅ I2C device found at address 0x%02X", address);
      if (address == 0x68 || address == 0x69) {
        Serial.print(" (MPU6050)");
      }
      Serial.println();
      deviceCount++;
    } else if (error == 4) {
      Serial.printf("❌ Unknown error at address 0x%02X\n", address);
    }
  }

  if (deviceCount == 0) {
    Serial.println("⚠️  No I2C devices found");
    Serial.println("   Check wiring: SDA->GPIO6, SCL->GPIO7");
  } else {
    Serial.printf("🎯 Found %d I2C device(s)\n", deviceCount);
  }
  Serial.println();
}

// ================= EGG CRACKING ANIMATION =================
void playEggCrackingAnimation() {
  // Display egg cracking animation from all_pets.h
  Serial.println("🥚 Egg animation starting...");

  if (displayReady) {
    // Display egg cracking sequence with all frames
    for (int i = 0; i < EGG_CRACK_FRAME_COUNT; i++) {
      display.clearDisplay();
      // Draw the actual egg crack frame from all_pets.h
      display.drawBitmap(0, 0, egg_crack_frames[i], EGG_CRACK_WIDTH,
                         EGG_CRACK_HEIGHT, SSD1306_WHITE);
      display.display();
      Serial.printf("🥚 Egg frame %d/%d\n", i + 1, EGG_CRACK_FRAME_COUNT);
      vTaskDelay(pdMS_TO_TICKS(
          egg_crack_delays[i])); // Non-blocking delay — feeds watchdog
    }
    Serial.println("🐣 Egg hatching complete!");
  }
}

// ================= SLIDE TRANSITION ANIMATION =================
void slideEggScreenOut() {
  // Egg screen slides to the left and exits
  if (!displayReady)
    return;

  Serial.println("🥚 Egg screen sliding left...");
  const int slideSteps = 8;

  for (int step = 0; step <= slideSteps; step++) {
    display.clearDisplay();

    // Calculate x position: starts at 0, ends at -64 (completely off left side)
    int xPos = -(step * SCREEN_WIDTH) / slideSteps;

    // Draw the final egg frame sliding left
    display.drawBitmap(xPos, 0, egg_crack_frames[EGG_CRACK_FRAME_COUNT - 1],
                       EGG_CRACK_WIDTH, EGG_CRACK_HEIGHT, SSD1306_WHITE);

    display.display();
    vTaskDelay(pdMS_TO_TICKS(80)); // Non-blocking — feeds watchdog
  }
  Serial.println("✅ Egg screen exit complete!");
}

void slideInfantSlowlyFromLeft() {
  // Infant appears gradually from left side (width gradually increasing)
  if (!displayReady)
    return;

  Serial.println("👶 Infant appearing slowly from left...");
  const int slideSteps = 15; // 15 steps @ 200ms = 3 seconds

  for (int step = 0; step <= slideSteps; step++) {
    display.clearDisplay();

    // Calculate x position: starts at -width (fully left of screen, invisible)
    // ends at 0 (fully visible on screen)
    int xPos = -INFANT_WIDTH + (step * INFANT_WIDTH) / slideSteps;

    // Draw infant gradually appearing from left
    display.drawBitmap(xPos, 0, infant_frames[0], INFANT_WIDTH, INFANT_HEIGHT,
                       SSD1306_WHITE);

    // Animation only - no text!
    display.display();
    vTaskDelay(pdMS_TO_TICKS(200)); // Non-blocking — feeds watchdog
  }

  // Final position - infant fully visible and centered
  display.clearDisplay();
  display.drawBitmap(0, 0, infant_frames[0], INFANT_WIDTH, INFANT_HEIGHT,
                     SSD1306_WHITE);
  display.display();

  petAge = INFANT;
  Serial.println("✅ Infant fully visible!");

  // REMOVED: Blocking server notification - too slow, not critical for ESP32
  // operation Menu cycling and OLED work independently now
}

// ================= HOME ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws home icon using pixel-by-pixel approach to prevent corruption with
// other animations
void drawHomeIcon() {
  int xOffset = 0; // Top-left corner
  int yOffset = 0;

  for (uint16_t y = 0; y < HOME_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < HOME_ICON_WIDTH; x++) {
      uint16_t byteIndex = (y / 8) * HOME_ICON_WIDTH + x;
      uint8_t bitIndex = y % 8;

      if (pgm_read_byte(&home_icon_frames[0][byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// ================= PLAY ICON DRAWING (BLINKING, TOP-LEFT) =================
// Blinks at top-left 1 hr after feeding — reminder to play with the pet
// Hidden when pet is sick (sick_pending active on server, showPlayIcon=false)
void drawPlayIcon() {
  // Blink: 600ms on, 600ms off
  if ((millis() / 600) % 2 == 1)
    return;

  int xOffset =
      0; // Top-left corner (same as home icon but home hidden when play shows)
  int yOffset = 0;

  uint8_t frame = (millis() / play_icon_delays[0]) % PLAY_ICON_FRAME_COUNT;

  for (uint16_t y = 0; y < PLAY_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < PLAY_ICON_WIDTH; x++) {
      uint16_t byteIndex = y * ((PLAY_ICON_WIDTH + 7) / 8) + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);
      if (pgm_read_byte(&play_icon_frames[frame][byteIndex]) &
          (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// ================= FOOD ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws food icon at bottom-right corner using pixel-by-pixel approach
void drawFoodIcon() {
  int xOffset = SCREEN_WIDTH - FOOD_ICON_WIDTH;   // Bottom-right: 64-24 = 40
  int yOffset = SCREEN_HEIGHT - FOOD_ICON_HEIGHT; // Bottom-right: 32-12 = 20

  // Draw current food icon frame (animate between 2 frames)
  uint8_t foodFrame = (millis() / food_icon_delays[0]) % FOOD_ICON_FRAME_COUNT;

  for (uint16_t y = 0; y < FOOD_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < FOOD_ICON_WIDTH; x++) {
      uint16_t byteIndex = (y / 8) * FOOD_ICON_WIDTH + x;
      uint8_t bitIndex = y % 8;

      if (pgm_read_byte(&food_icon_frames[foodFrame][byteIndex]) &
          (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// ================= SICK ICON DRAWING (BLINKING HEART) =================
// Shows blinking heart icon at bottom-right ONLY when poop was ignored >15 min
// AND poop has been cleared
void drawSickIcon() {
  // Blink: 500ms on, 500ms off
  if ((millis() / 500) % 2 == 1)
    return;

  int xOffset =
      SCREEN_WIDTH - HEART_ICON_WIDTH; // 64-24 = 40 (same spot as poop)
  int yOffset = SCREEN_HEIGHT - HEART_ICON_HEIGHT; // 32-12 = 20

  for (uint16_t y = 0; y < HEART_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < HEART_ICON_WIDTH; x++) {
      uint16_t byteIndex = y * ((HEART_ICON_WIDTH + 7) / 8) + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);
      if (pgm_read_byte(&heart_icon_frames[1][byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// ================= POOP ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws poop icon at bottom-right corner using pixel-by-pixel approach
void drawPoopIcon() {
  int xOffset = SCREEN_WIDTH - POOP_WIDTH;   // Bottom-right: 64-24 = 40
  int yOffset = SCREEN_HEIGHT - POOP_HEIGHT; // Bottom-right: 32-12 = 20

  // Draw current poop icon frame (animate between 2 frames)
  uint8_t poopFrame = (millis() / poop_delays[0]) % POOP_FRAME_COUNT;

  int bytesPerRow = POOP_WIDTH / 8; // 24/8 = 3 bytes per row
  for (uint16_t y = 0; y < POOP_HEIGHT; y++) {
    for (uint16_t x = 0; x < POOP_WIDTH; x++) {
      uint16_t byteIndex = y * bytesPerRow + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);

      if (pgm_read_byte(&poop_frames[poopFrame][byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// ================= EATING ANIMATION (Full Screen) =================
// Plays full-screen eating animation (5 frames)
void playEatingAnimation() {
  if (!displayReady)
    return;

  Serial.println("😋 Playing eating animation...");

  for (uint8_t frame = 0; frame < EATING_FRAME_COUNT; frame++) {
    display.clearDisplay();

    // Draw full-screen eating animation frame
    display.drawBitmap(0, 0, eating_frames[frame], EATING_WIDTH, EATING_HEIGHT,
                       SSD1306_WHITE);

    display.display();
    vTaskDelay(pdMS_TO_TICKS(eating_delays[frame])); // 100ms per frame
  }

  Serial.println("✅ Eating animation complete!");
}

// Draw static food icon at top-left (for food menu)
void drawStaticFoodIcon() {
  int xOffset = 0; // Top-left corner
  int yOffset = 0;

  // Use first frame of food icon animation (no animation in menu)
  for (uint16_t y = 0; y < FOOD_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < FOOD_ICON_WIDTH; x++) {
      uint16_t byteIndex = (y / 8) * FOOD_ICON_WIDTH + x;
      uint8_t bitIndex = y % 8;

      if (pgm_read_byte(&food_icon_frames[0][byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// Draw static toilet icon at top-left (for toilet menu)
void drawStaticToiletIcon() {
  int xOffset = 0; // Top-left corner
  int yOffset = 0;

  for (uint16_t y = 0; y < TOILET_HEIGHT; y++) {
    for (uint16_t x = 0; x < TOILET_WIDTH; x++) {
      uint16_t byteIndex = (y / 8) * TOILET_WIDTH + x;
      uint8_t bitIndex = y % 8;

      if (pgm_read_byte(&toilet_icon[byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// Helper function to draw simple sad face (circle with frown) for ages without
// SAD animation
void drawSimpleSadFace() {
  // Draw large sad circle at center of screen
  int centerX = SCREEN_WIDTH / 2;  // 32
  int centerY = SCREEN_HEIGHT / 2; // 16

  // Outer circle (face outline)
  display.drawCircle(centerX, centerY, 12, SSD1306_WHITE);

  // Eyes (small filled circles)
  display.fillCircle(centerX - 5, centerY - 3, 2, SSD1306_WHITE); // Left eye
  display.fillCircle(centerX + 5, centerY - 3, 2, SSD1306_WHITE); // Right eye

  // Sad frown (small arc - drawn with pixels)
  for (int x = -5; x <= 5; x++) {
    int y = 5 + (x * x) / 8; // Parabola for frown
    display.drawPixel(centerX + x, centerY + y, SSD1306_WHITE);
  }
}

// Display food menu screen
void displayFoodMenu() {
  display.clearDisplay();

  // Always draw food icon at top-left (menu identifier)
  drawStaticFoodIcon();

  // Show EATING ANIMATION while image is uploading
  // The captured frame IS the food - no AI detection needed
  if (isUploadingImage) {
    // Show looping eating animation (Pacman) - full screen, no food icon
    Serial.println("🍽️ FOOD_MENU: Showing EATING animation");
    display.clearDisplay(); // Clear to show full eating animation
    uint8_t eatingFrame =
        (millis() / 100) % EATING_FRAME_COUNT; // 100ms per frame
    display.drawBitmap(0, 0, eating_frames[eatingFrame], EATING_WIDTH,
                       EATING_HEIGHT, SSD1306_WHITE);
  }
  // Check if just finished eating (show GOOD for 3 seconds)
  else if (justFinishedEating && (millis() - eatingFinishTime < 3000)) {
    Serial.println("🍽️ FOOD_MENU: Showing GOOD text");
    // Show "GOOD" text after eating (NO newline to prevent cursor artifacts)
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(18, 12);
    display.print("GOOD!"); // Changed from println to print - prevents unwanted
                            // cursor movement
  } else if (justFinishedEating) {
    Serial.println("🍽️ FOOD_MENU: Showing age-based HAPPY face after eating");
    // After GOOD text expires, show age-appropriate HAPPY face
    justFinishedEating = false; // Reset flag

    // FIX: Use age-based HAPPY frame instead of hardcoded child_frames
    const uint8_t *frameData = nullptr;
    switch (petAge) {
    case INFANT:
      frameData = infant_frames[0]; // INFANT HAPPY
      display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT,
                         SSD1306_WHITE);
      break;
    case CHILD:
      frameData = child_frames[0]; // CHILD HAPPY
      display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT,
                         SSD1306_WHITE);
      break;
    case ADULT:
      frameData = adult_frames[0]; // ADULT HAPPY
      display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT,
                         SSD1306_WHITE);
      break;
    case OLD:
      frameData = old_happy[0]; // OLD HAPPY
      display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT,
                         SSD1306_WHITE);
      break;
    }
  } else if (showFoodIcon) {
    // Pet is HUNGRY - show CRYING animation and check for feeding gesture
    Serial.printf("🍽️ FOOD_MENU: Pet HUNGRY (Age: %d) - showing CRYING\n",
                  petAge);

    // Check for tilt gesture to trigger feeding
    checkFeedingGesture();

    switch (petAge) {
    case INFANT: {
      // INFANT has CRYING animation
      uint8_t cryFrame =
          (millis() / infant_cry_delays[0]) % INFANT_CRY_FRAME_COUNT;
      const uint8_t *frameData = infant_cry_frames[cryFrame];
      display.drawBitmap(0, 0, frameData, INFANT_CRY_WIDTH, INFANT_CRY_HEIGHT,
                         SSD1306_WHITE);
      break;
    }
    case OLD: {
      uint8_t cryFrame = (millis() / old_cry_delays[0]) % OLD_CRY_FRAME_COUNT;
      const uint8_t *frameData = old_cry[cryFrame];
      display.drawBitmap(0, 0, frameData, OLD_CRY_WIDTH, OLD_CRY_HEIGHT,
                         SSD1306_WHITE);
      break;
    }
    case CHILD: {
      uint8_t cryFrame = (millis() / cry_child_delays[0]) % CRY_CHILD_FRAME_COUNT;
      const uint8_t *frameData = cry_child[cryFrame];
      display.drawBitmap(0, 0, frameData, CRY_CHILD_WIDTH, CRY_CHILD_HEIGHT,
                         SSD1306_WHITE);
      break;
    }
    case ADULT: {
      uint8_t cryFrame = (millis() / cry_adult_delays[0]) % CRY_ADULT_FRAME_COUNT;
      const uint8_t *frameData = cry_adult[cryFrame];
      display.drawBitmap(0, 0, frameData, CRY_ADULT_WIDTH, CRY_ADULT_HEIGHT,
                         SSD1306_WHITE);
      break;
    }
    }
  } else {
    Serial.println("🍽️ FOOD_MENU: Pet NOT hungry - showing HAPPY face");
    // Pet is NOT hungry - show HAPPY face (excited about food menu)
    const uint8_t *frameData = nullptr;
    switch (petAge) {
    case INFANT:
      frameData = infant_frames[0]; // INFANT HAPPY
      display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT,
                         SSD1306_WHITE);
      break;
    case CHILD:
      frameData = child_frames[0]; // CHILD HAPPY
      display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT,
                         SSD1306_WHITE);
      break;
    case ADULT:
      frameData = adult_frames[0]; // ADULT HAPPY
      display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT,
                         SSD1306_WHITE);
      break;
    case OLD:
      frameData = old_happy[0]; // OLD HAPPY
      display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT,
                         SSD1306_WHITE);
      break;
    }
  }

  display.display();
}

// Display toilet menu screen
// Draw clean slide sprite at given x position
void drawCleanSpriteFrame(int frame, int xOffset) {
  for (uint16_t y = 0; y < CLEAN_SLIDE_HEIGHT; y++) {
    for (uint16_t x = 0; x < CLEAN_SLIDE_WIDTH; x++) {
      uint16_t byteIndex = y * ((CLEAN_SLIDE_WIDTH + 7) / 8) + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);
      if (pgm_read_byte(&clean_slide_frames[frame][byteIndex]) &
          (1 << bitIndex)) {
        int px = x + xOffset;
        int py = y + 4; // vertical centering on 32px screen
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT)
          display.drawPixel(px, py, SSD1306_WHITE);
      }
    }
  }
}

void displayToiletMenu() {
  display.clearDisplay();

  // Draw static toilet icon at top-left (no blinking)
  drawStaticToiletIcon();

  // Display text based on poop status
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (isCleaningPoop) {
    // Slide animation: sprite walks from right to left across screen
    if (cleanSlideX > -CLEAN_SLIDE_WIDTH) {
      // Move sprite every 80ms
      if (millis() - cleanSlideLastStep > 80) {
        cleanSlideX -= 2;
        cleanSlideFrame = (cleanSlideFrame == 1) ? 2 : 1; // toggle frames
        cleanSlideLastStep = millis();
      }
      // Draw sliding sprite
      drawCleanSpriteFrame(cleanSlideFrame, cleanSlideX);
    } else {
      // Sprite has exited - show "Cleared!" message
      display.setCursor(10, 12);
      display.print("Cleared!");

      // Stop animation after showing message
      if (millis() - cleaningStartTime > 2000) {
        isCleaningPoop = false;
        justCleanedPet = true;
      }
    }
  } else if (showPoopIcon) {
    // Poop present - show "Clean me" prompt
    display.setCursor(10, 12);
    display.print("Clean me");
  } else {
    // No poop - show "Cleared" status
    display.setCursor(10, 12);
    display.print("Cleared");
  }

  display.display();
}

// Draw static play icon at top-left (for play menu)
void drawStaticPlayIcon() {
  int xOffset = 0; // Top-left corner
  int yOffset = 0;

  // Use second frame of play icon animation (the animated play symbol)
  for (uint16_t y = 0; y < PLAY_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < PLAY_ICON_WIDTH; x++) {
      uint16_t byteIndex = y * ((PLAY_ICON_WIDTH + 7) / 8) + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);

      if (pgm_read_byte(&play_icon_frames[1][byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// ================= PLAY MENU GAME FUNCTIONS =================

// ---- Shared bitmaps for Dodge game character ----
// 8x8 walk frame 1: standing
const unsigned char charFrame1[] PROGMEM = {0x18, 0x3C, 0x18, 0x24,
                                            0x18, 0x24, 0x00, 0x00};
// 8x8 walk frame 2: striding
const unsigned char charFrame2[] PROGMEM = {0x18, 0x3C, 0x18, 0x24,
                                            0x3C, 0x12, 0x00, 0x00};

// Calculate KakuCoin reward
float calculateKakuCoin(int scoreValue) { return scoreValue * 0.80; }

// Read tilt for game control
void readTiltForGame() {
  if (!mpuAvailable)
    return;

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accelY = ay / 16384.0; // Forward/back tilt for gameplay

  filteredX = 0.6 * filteredX + 0.4 * accelY; // Snappy response (less smoothing)

  if (abs(filteredX) < 0.12) { // Tight dead zone — small tilts register
    velocity = 0;
  } else {
    velocity = filteredX * 6.0; // Fast movement
  }

  playerX += velocity;

  if (playerX < 0)
    playerX = 0;
  if (playerX > SCREEN_WIDTH - playerWidth)
    playerX = SCREEN_WIDTH - playerWidth;
}

// Update food falling
void updateFoodFalling() {
  if (millis() - lastFallTime > fallSpeed) {
    foodY += 2;
    lastFallTime = millis();
  }

  if (foodY > SCREEN_HEIGHT) {
    missCount++;
    comboCount = 0;
    scoreIncrement = 1;

    resetFood();

    if (missCount >= 3) {
      playGameState = GAME_OVER_ANIM;
      gameOverStartTime = millis();
    }
  }
}

// Check collision
void checkGameCollision() {
  if (foodY + foodSize >= playerY && foodX + foodSize >= playerX &&
      foodX <= playerX + playerWidth) {

    comboCount++;

    if (comboCount >= 5)
      scoreIncrement = 5;
    else
      scoreIncrement = 1;

    gameScore += scoreIncrement;

    resetFood();
  }
}

// Reset food position
void resetFood() {
  foodY = 0;
  foodX = random(0, SCREEN_WIDTH - foodSize);
}

// Draw game play screen
void drawGamePlay() {
  display.clearDisplay();

  display.fillRect(playerX, playerY, playerWidth, playerHeight, SSD1306_WHITE);
  display.fillRect(foodX, foodY, foodSize, foodSize, SSD1306_WHITE);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("S:");
  display.print(gameScore);

  display.setCursor(40, 0);
  display.print("M:");
  display.print(missCount);

  display.display();
}

// Update coins animation
void updateCoins() {
  for (int i = 0; i < COIN_COUNT; i++) {
    coinY[i] += coinSpeed[i];

    if (coinY[i] > SCREEN_HEIGHT) {
      coinY[i] = 0;
      coinX[i] = random(0, SCREEN_WIDTH);
      coinSpeed[i] = random(1, 3);
    }
  }
}

// Draw coins
void drawCoins() {
  for (int i = 0; i < COIN_COUNT; i++) {
    display.drawCircle(coinX[i], coinY[i], 2, SSD1306_WHITE);
    display.drawPixel(coinX[i], coinY[i], SSD1306_WHITE);
  }
}

// Draw game over screen
void drawGameOverScreen() {
  updateCoins();

  display.clearDisplay();
  drawCoins();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(6, 6);
  display.print("GAME OVER");

  display.setCursor(10, 16);
  display.print("Score:");
  display.print(gameScore);

  float kakuCoin = calculateKakuCoin(gameScore);

  display.setCursor(10, 24);
  display.print("KC:");
  display.print(kakuCoin, 1);

  display.display();
}

// Check start gesture (hold left tilt for 3 seconds)
void checkStartGesture() {
  if (!mpuAvailable)
    return;

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accelX = ax / 16384.0;

  if (accelX < -0.8) {
    if (!holdingLeft) {
      holdingLeft = true;
      holdStartTime = millis();
    }

    if (millis() - holdStartTime > 3000) {
      activeGame = random(0, 2); // 0 = catch food, 1 = dodge obstacle
      resetGameState();
      dodgeFallSpeed = 120; // reset dodge speed

      // Show "GET READY!" for 2 seconds so user can stabilize device
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(4, 8);
      display.print("GET READY!");
      display.setCursor(4, 20);
      display.print(activeGame == 0 ? "Catch Food" : "Dodge!");
      display.display();
      vTaskDelay(pdMS_TO_TICKS(2000));

      playGameState = GAME_PLAYING;
      gameStartTime = millis();
      Serial.printf("🎮 Game started! Type: %s\n",
                    activeGame == 0 ? "CATCH FOOD" : "DODGE");
    }
  } else {
    holdingLeft = false;
  }
}

// Reset game state
void resetGameState() {
  gameScore = 0;
  missCount = 0;
  comboCount = 0;
  scoreIncrement = 1;

  playerX = 28;
  foodY = 0;
  foodX = random(0, SCREEN_WIDTH - foodSize);

  // Reset dodge game state too
  dodgeScore = 0;
  dodgeGameOver = false;
  dodgeGameOverAnimDone = false;
  obsY = 0;
  obsX = random(0, SCREEN_WIDTH - obsSize);
  walkFrame = 0;
  playerX = 28;

  // Initialize coins
  for (int i = 0; i < COIN_COUNT; i++) {
    coinX[i] = random(0, SCREEN_WIDTH);
    coinY[i] = random(0, SCREEN_HEIGHT);
    coinSpeed[i] = random(1, 3);
  }
}

// ---- GAME 2: DODGE OBSTACLE FUNCTIONS ----

void readTiltForDodge() {
  if (!mpuAvailable)
    return;
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);
  float accelY = ay / 16384.0; // Forward/back tilt for gameplay
  playerX += accelY * 6.0; // Fast responsive movement
  if (playerX < 0)
    playerX = 0;
  if (playerX > SCREEN_WIDTH - 8)
    playerX = SCREEN_WIDTH - 8;
  if (abs(accelY) > 0.1) { // Tight threshold for walk animation
    if (millis() - lastWalkFrameTime > 120) {
      walkFrame = !walkFrame;
      lastWalkFrameTime = millis();
    }
  }
}

void updateObstacle() {
  if (millis() - lastObsFall > dodgeFallSpeed) {
    obsY += 2;
    lastObsFall = millis();
  }
  if (obsY > SCREEN_HEIGHT) {
    dodgeScore++;
    obsY = 0;
    obsX = random(0, SCREEN_WIDTH - obsSize);
    // Speed up slightly every 5 dodges
    if (dodgeScore % 5 == 0 && dodgeFallSpeed > 40)
      dodgeFallSpeed -= 8;
  }
}

void checkDodgeCollision() {
  const int charY = 24;
  if (obsY + obsSize >= charY && obsX + obsSize >= (int)playerX &&
      obsX <= (int)playerX + 8) {
    dodgeGameOver = true;
  }
}

void drawDodgeGame() {
  display.clearDisplay();
  // Draw character
  if (walkFrame == 0)
    display.drawBitmap((int)playerX, 19, charFrame1, 8, 8, SSD1306_WHITE);
  else
    display.drawBitmap((int)playerX, 19, charFrame2, 8, 8, SSD1306_WHITE);
  // Draw obstacle
  display.fillRect(obsX, obsY, obsSize, obsSize, SSD1306_WHITE);
  // Score
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("S:");
  display.print(dodgeScore);
  display.display();
}

void playDodgeGameOverAnim() {
  // FIX: Use vTaskDelay() instead of delay() to prevent TG1WDT_SYS_RST watchdog
  // reset delay() blocks RTOS scheduler → watchdog triggers after ~3.5s of
  // blocking Explosion rings
  for (int r = 2; r < 14; r += 2) {
    display.clearDisplay();
    display.drawCircle((int)playerX + 4, 23, r, SSD1306_WHITE);
    display.display();
    vTaskDelay(pdMS_TO_TICKS(70));
  }
  // Scatter pixels
  for (int i = 0; i < 8; i++) {
    display.clearDisplay();
    for (int p = 0; p < 15; p++) {
      int px = (int)playerX + random(-8, 8);
      int py = 23 + random(-8, 8);
      display.drawPixel(px, py, SSD1306_WHITE);
    }
    display.display();
    vTaskDelay(pdMS_TO_TICKS(60));
  }
  // Screen shake "CRASH!"
  for (int i = 0; i < 6; i++) {
    display.clearDisplay();
    int s = random(-2, 2);
    display.setCursor(10 + s, 10);
    display.print("CRASH!");
    display.display();
    vTaskDelay(pdMS_TO_TICKS(90));
  }
  // Final score screen
  display.clearDisplay();
  float kc = dodgeScore * 0.8;
  display.setCursor(4, 6);
  display.print("GAME OVER");
  display.setCursor(10, 18);
  display.print("KC:");
  display.print(kc, 1);
  display.display();
  vTaskDelay(pdMS_TO_TICKS(2500));
  dodgeGameOverAnimDone = true;
}

// Send KakuCoin reward to server
void sendKakuCoinReward(int score, float kakucoin) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected, can't send reward");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(5000);

  const char *rewardUrl =
      "https://kakuproject-90943350924.asia-south1.run.app/api/game/reward";

  if (!http.begin(sslNet, String(rewardUrl))) {
    Serial.println("❌ Failed to begin reward HTTP");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["device_id"] = "ESP32_001";
  doc["score"] = score;
  doc["kakucoin"] = kakucoin;
  doc["play_duration"] = (millis() - gameStartTime) / 1000; // seconds
  doc["game_type"] = (activeGame == 0) ? "catch_food" : "dodge_obstacle";

  String payload;
  serializeJson(doc, payload);

  Serial.printf("🎮 Sending game reward: Score=%d, KC=%.1f\n", score, kakucoin);

  int httpCode = http.POST(payload);
  trackHttpResult(httpCode);

  if (httpCode == 200) {
    Serial.println("✅ Game reward sent!");
  } else {
    Serial.printf("❌ Reward send failed: %d\n", httpCode);
  }

  http.end();
}

// Display play menu screen 🎮
void displayPlayMenu() {
  switch (playGameState) {
  case GAME_IDLE:
    // Show static PLAY screen and check for start gesture
    display.clearDisplay();
    drawStaticPlayIcon();

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 14);
    display.print("PLAY");

    display.display();

    // Check if user wants to start game
    checkStartGesture();
    break;

  case GAME_PLAYING:
    if (activeGame == 0) {
      // Game 1: Catch Food
      readTiltForGame();
      updateFoodFalling();
      checkGameCollision();
      drawGamePlay();
    } else {
      // Game 2: Dodge Obstacle
      if (!dodgeGameOver) {
        readTiltForDodge();
        updateObstacle();
        checkDodgeCollision();
        drawDodgeGame();
      } else {
        // Trigger game-over flow
        if (!dodgeGameOverAnimDone) {
          playDodgeGameOverAnim(); // blocking animation
        }
        // Queue reward to networkTask (Core 1) — NEVER call HTTP from OLED task (Core 0)!
        pendingRewardScore = dodgeScore;
        pendingRewardKC = dodgeScore * 0.8;
        uint8_t req = NET_GAME_REWARD;
        xQueueSend(networkQueue, &req, 0);
        playGameState = GAME_IDLE;
        Serial.println("🎮 Dodge game ended, reward queued");
      }
    }
    break;

  case GAME_OVER_ANIM:
    // Game 1 (catch food) game-over animation (coins screen)
    drawGameOverScreen();

    if (millis() - gameOverStartTime > 5000) {
      // Queue reward to networkTask (Core 1) — NEVER call HTTP from OLED task (Core 0)!
      pendingRewardScore = gameScore;
      pendingRewardKC = calculateKakuCoin(gameScore);
      uint8_t req = NET_GAME_REWARD;
      xQueueSend(networkQueue, &req, 0);

      // Resume normal operations
      playGameState = GAME_IDLE;
      Serial.println("🎮 Game ended, reward queued");
    }
    break;
  }
}

// Draw static heart icon at top-left (for health menu)
void drawStaticHealthIcon() {
  int xOffset = 0; // Top-left corner
  int yOffset = 0;

  // Use second frame of heart icon (the heart shape)
  for (uint16_t y = 0; y < HEART_ICON_HEIGHT; y++) {
    for (uint16_t x = 0; x < HEART_ICON_WIDTH; x++) {
      uint16_t byteIndex = y * ((HEART_ICON_WIDTH + 7) / 8) + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);

      if (pgm_read_byte(&heart_icon_frames[1][byteIndex]) & (1 << bitIndex)) {
        display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// Display health menu screen ❤️
void displayHealthMenu() {
  // If currently giving medicine (animation playing)
  if (givingMedicine) {
    playInjectionAnimation();
    return;
  }

  display.clearDisplay();

  // Draw static heart icon at top-left
  drawStaticHealthIcon();

  // 🏷️ Add "HEALTH" Title Header
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(28, 2); // To the right of the heart icon
  display.print("HEALTH");
  display.drawLine(28, 10, 64, 10, SSD1306_WHITE); // Small underline decoration

  // Display text based on pet health status
  if (petIsSick) {
    // Show "Give Medicine" text
    display.setCursor(4, 16);
    if (holdingLeftForMedicine) {
      display.print("HOLDING..."); // Visual feedback for gesture
    } else {
      display.print("Give Med");
    }

    // Check for tilt gesture to give medicine
    checkMedicineGesture();
    Serial.println("❤️ HEALTH_MENU: Pet is SICK - Give Medicine");
  } else {
    // Show "All Good" text
    display.setCursor(10, 16);
    display.print("All Good");
    Serial.println("❤️ HEALTH_MENU: Pet is healthy - All Good");
  }

  display.display();
}

// ================= STATUS INFO MENU =================
// 4-quadrant layout on 64x32 OLED:
//   Q1 top-left  (0,0)   : Smiley face
//   Q2 top-right (32,0)  : Pet age / stage label
//   Q3 bot-left  (0,16)  : Flash / energy icon (lightning bolt bitmap)
//   Q4 bot-right (32,16) : Game score + "pts" label

#define STATUS_FLASH_WIDTH 32
#define STATUS_FLASH_HEIGHT 16

PROGMEM const uint8_t statusFlashBitmap[64] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xbf, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xfe,
    0x7f, 0xff, 0xff, 0xfc, 0x7f, 0xff, 0xff, 0xfc, 0x1f, 0xff, 0xff,
    0xf8, 0x1f, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xfe, 0x7f, 0xff,
    0xff, 0xfe, 0xff, 0xff, 0xff, 0xfd, 0xff, 0xff, 0xff, 0xfd, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ================= PROGRESS BAR HELPER =================
// Draws a labelled progress bar. x,y = top-left corner, w = total width, h =
// bar height
void drawBar(int x, int y, int w, int h, int level, int maxLevel) {
  display.drawRect(x, y, w, h, SSD1306_WHITE); // outline
  int fill = (level * (w - 2)) / maxLevel;     // filled pixels
  if (fill > 0)
    display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE); // fill
}

// Draw simple smiley for status quadrant (Q1)
void drawSmileyStatus(int x, int y) {
  display.drawCircle(x + 8, y + 8, 7, SSD1306_WHITE);
  display.fillCircle(x + 5, y + 6, 1, SSD1306_WHITE);             // left eye
  display.fillCircle(x + 11, y + 6, 1, SSD1306_WHITE);            // right eye
  display.drawLine(x + 4, y + 11, x + 12, y + 11, SSD1306_WHITE); // smile
}

// Draw flash/lightning bolt bitmap for status quadrant (Q3)
// Renders PROGMEM bitmap inverted (0-bit = pixel ON)
void drawFlashStatus(int xOffset, int yOffset) {
  for (uint16_t row = 0; row < STATUS_FLASH_HEIGHT; row++) {
    for (uint16_t col = 0; col < STATUS_FLASH_WIDTH; col++) {
      uint16_t byteIndex = row * ((STATUS_FLASH_WIDTH + 7) / 8) + (col / 8);
      uint8_t bitIndex = 7 - (col % 8);
      if (!(pgm_read_byte(&statusFlashBitmap[byteIndex]) & (1 << bitIndex))) {
        display.drawPixel(col + xOffset, row + yOffset, SSD1306_WHITE);
      }
    }
  }
}

// STATUS INFO MENU — shows Happiness + Discipline as live progress bars
// STATUS INFO MENU — 4-quadrant: smiley / age / flash / calories
void displayStatusInfoMenu() {
  display.clearDisplay();

  // --- Q1: Smiley (top-left) ---
  drawSmileyStatus(0, 0);

  // --- Q2: Actual age from server (top-right) ---
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(30, 4);
  display.print(petAgeInt);
  display.print(" yrs");

  // --- Q3: Flash / energy icon (bottom-left) ---
  drawFlashStatus(0, 16);

  // --- Q4: Calories ---
  int calValue = (gameScore > 0) ? gameScore : 100;
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(32, 18);
  display.print(calValue);
  display.setCursor(34, 26);
  display.print("cal");

  display.display();
}

// STATS MENU — Happiness + Discipline as live progress bars (full 64x32)
void displayStatsMenu() {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Happiness label + bar
  display.setCursor(0, 0);
  display.print("Happy");
  drawBar(0, 8, SCREEN_WIDTH, 7, petHappiness, 100);

  // Discipline label + bar
  display.setCursor(0, 17);
  display.print("Discip");
  drawBar(0, 25, SCREEN_WIDTH, 7, petDiscipline, 100);

  display.display();
}

// Check for medicine gesture (hold left tilt for 3 seconds)
void checkMedicineGesture() {
  // Block medicine during walking (MPU data unreliable) or sleeping
  if (petIsWalking || isDeviceSleeping)
    return;
  if (!mpuAvailable || !petIsSick)
    return;
  if (currentScreenType != "HEALTH_MENU") {
    holdingLeftForMedicine = false;
    return;
  }

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accelX = ax / 16384.0;

  // Check if tilting left (same threshold as game)
  if (accelX < -0.8) {
    if (!holdingLeftForMedicine) {
      holdingLeftForMedicine = true;
      medicineHoldStartTime = millis();
      Serial.println("💊 Medicine gesture started...");
    }

    // Check if held for 3 seconds
    if (millis() - medicineHoldStartTime > 3000) {
      // Start medicine animation
      givingMedicine = true;
      medicineAnimLoopCount = 0;
      currentInjectionFrame = 0;
      medicineAnimStartTime = millis();
      holdingLeftForMedicine = false;
      Serial.println("💉 Starting medicine injection animation!");
    }
  } else {
    holdingLeftForMedicine = false;
  }
}

// Check for feeding gesture (tilt left + hold 3 seconds)
void checkFeedingGesture() {
  // Block feeding during walking (MPU data unreliable) or sleeping
  if (petIsWalking || isDeviceSleeping)
    return;
  if (!mpuAvailable)
    return;
  if (currentScreenType != "FOOD_MENU") {
    holdingLeftForFeeding = false;
    return;
  }

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accelX = ax / 16384.0;

  // Check if tilting left (same threshold as game)
  if (accelX < -0.8) {
    if (!holdingLeftForFeeding) {
      holdingLeftForFeeding = true;
      feedingHoldStartTime = millis();
      Serial.println("🍽️ Feeding gesture started...");
    }

    // Check if held for 3 seconds
    if (millis() - feedingHoldStartTime > 3000) {
      // Trigger image capture for feeding
      capturingForFeeding = true;
      feedingGestureStartTime = millis(); // Start timeout timer
      holdingLeftForFeeding = false;
      imageAlreadySentThisSession = true;

      // Start eating animation immediately
      isUploadingImage = true;
      Serial.println("📸 Triggering food image capture!");
      Serial.println("🍴 Starting eating animation!");

      // Local state update: Feeding reduces hunger and gives XP
      g_petState.hunger = max(0, g_petState.hunger - 40);
      g_petState.xp += 20;
      g_petState.energy = min(100, g_petState.energy + 20);
      savePetState();
      syncLocalStateToUI();

      // Queue image send
      uint8_t req = NET_IMAGE;
      xQueueSend(networkQueue, &req, 0);
    }
  } else {
    holdingLeftForFeeding = false;
  }
}

// Check for cleaning gesture in toilet menu (tilt left + hold 3 seconds)
void checkCleaningGesture() {
  // Block cleaning gesture during walking or sleeping
  if (petIsWalking || isDeviceSleeping)
    return;
  if (!mpuAvailable)
    return;
  if (!showPoopIcon)
    return; // Only clean if poop present

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accelX = ax / 16384.0;

  // Check if tilting left
  if (accelX < -0.8) {
    if (!holdingLeftForCleaning) {
      holdingLeftForCleaning = true;
      cleaningHoldStartTime = millis();
      Serial.println("🚽 Cleaning gesture started...");
    }

    // Check if held for 3 seconds
    if (millis() - cleaningHoldStartTime > 3000 && !isCleaningPoop) {
      // Start cleaning animation
      isCleaningPoop = true;
      cleaningFadeStep = 0;
      cleaningStartTime = millis();
      cleanSlideX = SCREEN_WIDTH; // Start sprite from right edge
      cleanSlideFrame = 1;
      cleanSlideLastStep = millis();
      holdingLeftForCleaning = false;

      Serial.println("🧹 Starting cleaning animation!");

      // Local state update: Cleaning clears poop and gives happiness
      g_petState.hasPoop = false;
      g_petState.happiness = min(100, g_petState.happiness + 20);
      g_petState.xp += 10;
      poopClearedTime = millis(); // Start 15-min play icon timer
      savePetState();
      syncLocalStateToUI();

      // Send cleaning request to server
      uint8_t req = NET_CLEAN;
      xQueueSend(networkQueue, &req, 0);
    }
  } else {
    holdingLeftForCleaning = false;
  }
}

// Play injection animation (34 frames, loop 3 times)
void playInjectionAnimation() {
  display.clearDisplay();

  // Draw current frame of injection animation
  for (uint16_t y = 0; y < INJECTION_HEIGHT; y++) {
    for (uint16_t x = 0; x < INJECTION_WIDTH; x++) {
      uint16_t byteIndex = y * ((INJECTION_WIDTH + 7) / 8) + (x / 8);
      uint8_t bitIndex = 7 - (x % 8);

      if (pgm_read_byte(&injection_frames[currentInjectionFrame][byteIndex]) &
          (1 << bitIndex)) {
        display.drawPixel(x, y, SSD1306_WHITE);
      }
    }
  }

  display.display();

  // Check if it's time to advance to next frame
  uint32_t delayMs = pgm_read_word(&injection_delays[currentInjectionFrame]);
  if (millis() - medicineAnimStartTime > delayMs) {
    currentInjectionFrame++;
    medicineAnimStartTime = millis();

    // Check if one loop is complete
    if (currentInjectionFrame >= INJECTION_FRAME_COUNT) {
      currentInjectionFrame = 0;
      medicineAnimLoopCount++;
      Serial.printf("💊 Medicine animation loop %d/3 complete\n",
                    medicineAnimLoopCount);

      // Check if all 3 loops are done
      if (medicineAnimLoopCount >= 3) {
        // Animation complete - cure the pet
        givingMedicine = false;
        petIsSick = false;
        showSickIcon = false; // Hide sick icon immediately
        medicineAnimLoopCount = 0;
        currentInjectionFrame = 0;
        Serial.println("✅ Medicine given! Pet is now healthy!");

        // Local state update: Medicine cures sickness and restores health
        g_petState.isSick = false;
        g_petState.health = min(100, g_petState.health + 30);
        savePetState();
        syncLocalStateToUI();

        // Send medicine request to server: sick_pending cleared, hunger resumes
        uint8_t req = NET_INJECT;
        xQueueSend(networkQueue, &req, 0);

        display.setCursor(10, 12);
        display.print("All Good");
        display.display();
        vTaskDelay(pdMS_TO_TICKS(2000)); // FIX: vTaskDelay prevents WDT reset
      }
    }
  }
}

// ================= MENU TILT GESTURE (RIGHT TILT 2 SEC) =================
// Right tilt + hold 2 seconds → cycle through menus
void checkMenuTiltGesture() {
  if (!mpuAvailable)
    return;
  if ((millis() - lastMenuCycleTime) < MENU_CYCLE_COOLDOWN)
    return; // Respect cooldown

  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  float accelX = ax / 16384.0;

  if (accelX > 0.8) {
    if (!holdingRightForMenu) {
      holdingRightForMenu = true;
      menuTiltHoldStartTime = millis();
      Serial.println("📱 Menu tilt right started...");
    }

    unsigned long heldFor = millis() - menuTiltHoldStartTime;
    if (heldFor >= MENU_TILT_HOLD_TIME) {
      Serial.println("🔄 Right tilt 2s held → Cycling menu...");
      cycleMenu();
      lastMenuCycleTime = millis();
      holdingRightForMenu = false;
      // Tilt interaction → happiness +5
      uint8_t req = NET_HAPPY;
      xQueueSend(networkQueue, &req, 0);
    }
  } else {
    holdingRightForMenu = false;
  }
}

// ── NEUTRAL / SLEEP HELPERS
// ───────────────────────────────────────────────────

// Hardware step counter — stoss/barrier method (same algorithm as original
// server detect_steps) Uses g-unit values so barrier is sensor-independent
void detectHardwareStep() {
  if (!mpuAvailable)
    return;
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);

  // Convert to g-units
  float gx = ax / 16384.0f;
  float gy = ay / 16384.0f;
  float gz = az / 16384.0f;

  // Low-pass filter — tracks gravity at ANY tilt orientation
  stepGravX = LP_ALPHA_STEP * stepGravX + (1.0f - LP_ALPHA_STEP) * gx;
  stepGravY = LP_ALPHA_STEP * stepGravY + (1.0f - LP_ALPHA_STEP) * gy;
  stepGravZ = LP_ALPHA_STEP * stepGravZ + (1.0f - LP_ALPHA_STEP) * gz;

  // Dynamic component only (gravity removed) — slow tilts disappear
  float dx = gx - stepGravX;
  float dy = gy - stepGravY;
  float dz = gz - stepGravZ;
  float stoss = dx * dx + dy * dy + dz * dz;

  unsigned long now = millis();
  if (stoss > STEP_BARRIER_G2 && (now - lastHwStepTime) > STEP_MIN_MS) {
    hwStepCount++;
    lastHwStepTime = now;
    lastWalkingStepTime = now;
    Serial.printf("👣 HW Step #%u  stoss=%.4f\n", hwStepCount, stoss);
  }
}

// Returns true when device lies flat (no significant X/Y tilt)
const char *detectDirection(SensorData data) {
  // Use calibrated accelerometer values in m/s²
  float ax = data.accel_x;
  float ay = data.accel_y;
  float az = data.accel_z;

  // 🧠 LOCAL ORIENTATION ENGINE (Restored from legacy patterns)
  float abs_ax = abs(ax);
  float abs_ay = abs(ay);
  float abs_az = abs(az);

  if (abs_az > abs_ax && abs_az > abs_ay) {
    if (az < -7.0f)
      return "INVERTED";
    if (az > 7.0f)
      return "NEUTRAL";
  }

  if (abs_ax > abs_ay && abs_ax > abs_az) {
    if (ax > 5.0f)
      return "RIGHT";
    if (ax < -5.0f)
      return "LEFT";
  }

  if (abs_ay > abs_ax && abs_ay > abs_az) {
    if (ay > 5.0f)
      return "BACK";
    if (ay < -5.0f)
      return "FORWARD";
  }

  return "NEUTRAL";
}

// Check if device is in inverted state (face-down)
bool isDeviceInverted() {
  SensorData data = readAllSensors();
  return (strcmp(detectDirection(data), "INVERTED") == 0);
}

// 2-frame sleeping animation (frame-timed)
void displaySleepingAnimation() {
  static uint8_t sleepFrame = 0;
  static unsigned long lastSleepFrameTime = 0;
  display.clearDisplay();
  display.drawBitmap(0, 0, sleeping_frames[sleepFrame], SLEEPING_WIDTH,
                     SLEEPING_HEIGHT, SSD1306_WHITE);
  display.display();
  if (millis() - lastSleepFrameTime >=
      pgm_read_word(&sleeping_delays[sleepFrame])) {
    lastSleepFrameTime = millis();
    sleepFrame = (sleepFrame + 1) % SLEEPING_FRAME_COUNT;
  }
}

// 6-frame walking animation (skips frame 2 at runtime)
void displayWalkingAnimation() {
  static uint8_t wFrame = 0;
  static unsigned long lastWFrameTime = 0;
  if (wFrame == 2)
    wFrame = 3; // skip frame 2 on entry guard
  display.clearDisplay();
  display.drawBitmap(0, 0, walking_animation[wFrame], WALKING_WIDTH,
                     WALKING_HEIGHT, SSD1306_WHITE);
  display.display();
  if (millis() - lastWFrameTime >= pgm_read_word(&walking_delays[wFrame])) {
    lastWFrameTime = millis();
    wFrame = (wFrame + 1) % WALKING_FRAME_COUNT;
    if (wFrame == 2)
      wFrame = 3; // skip frame 2
  }
}

// Cycle through menus: MAIN → FOOD_MENU → TOILET_MENU → PLAY_MENU → HEALTH_MENU
// → STATUS_INFO_MENU → MAIN
void cycleMenu() {
  String newMenu;

  if (currentScreenType == "MAIN") {
    newMenu = "FOOD_MENU";
  } else if (currentScreenType == "FOOD_MENU") {
    newMenu = "TOILET_MENU";
  } else if (currentScreenType == "TOILET_MENU") {
    newMenu = "PLAY_MENU";
  } else if (currentScreenType == "PLAY_MENU") {
    newMenu = "HEALTH_MENU";
  } else if (currentScreenType == "HEALTH_MENU") {
    newMenu = "STATUS_INFO_MENU";
  } else if (currentScreenType == "STATUS_INFO_MENU") {
    newMenu = "STATS_MENU";
  } else {
    newMenu = "MAIN";
  }

  Serial.printf("📡 Menu cycle: %s → %s\n", currentScreenType.c_str(),
                newMenu.c_str());

  // ✅ Reset animation state when LEAVING a menu (prevents stale animations)
  if (currentScreenType == "TOILET_MENU") {
    isCleaningPoop = false;
    cleanSlideX = SCREEN_WIDTH;
    cleanSlideFrame = 1;
    holdingLeftForCleaning = false;
  }
  if (currentScreenType == "HEALTH_MENU") {
    givingMedicine = false;
    holdingLeftForMedicine = false;
    currentInjectionFrame = 0;
    medicineAnimLoopCount = 0;
  }

  // ✅ CHANGE MENU LOCALLY FIRST (instant, no server dependency)
  currentScreenType = newMenu;

  // ✅ Reset gesture/animation states on ENTERING new menu (prevents stale triggers)
  if (newMenu == "TOILET_MENU") {
    isCleaningPoop = false;
    cleanSlideX = SCREEN_WIDTH;
    cleanSlideFrame = 1;
    holdingLeftForCleaning = false;
    cleaningHoldStartTime = 0;
  }
  if (newMenu == "HEALTH_MENU") {
    givingMedicine = false;
    holdingLeftForMedicine = false;
    medicineHoldStartTime = 0;
    currentInjectionFrame = 0;
    medicineAnimLoopCount = 0;
  }
  if (newMenu == "FOOD_MENU") {
    holdingLeftForFeeding = false;
    feedingHoldStartTime = 0;
  }
  if (newMenu == "PLAY_MENU") {
    playGameState = GAME_IDLE;
  }

  // ✅ ESP32 controls home icon based on menu (not server)
  if (newMenu == "MAIN") {
    showHomeIcon = true; // Show home icon on MAIN screen
    Serial.println("🏠 Home icon: ENABLED (MAIN screen)");
  } else {
    showHomeIcon = false; // Hide home icon on other menus
    Serial.println("🏠 Home icon: DISABLED (not MAIN)");
  }

  // Reset image send flag when leaving FOOD_MENU
  if (newMenu != "FOOD_MENU") {
    imageAlreadySentThisSession = false;
    Serial.println("🔄 Reset image send flag (left FOOD_MENU)");
  }

  Serial.printf("✅ Menu changed locally to: %s (OLED updates immediately)\n",
                newMenu.c_str());

  // ESP32 runs independently - no server notification needed
}

// ================= CAMERA COVER DETECTION (DISABLED — replaced by right-tilt
// gesture) ================= OLD: Continuous frame checking every loop (causes
// hardware heating) Old: Black frame detection — removed. Menu now uses
// right-tilt gesture.
/*
void checkCameraCover() {
    // Skip cover detection if camera is currently capturing (prevent conflict)
    if (cameraCapturing) return;

    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) return;

    bool isBlack = isFrameMostlyBlack(fb);
    unsigned long now = millis();

    if (isBlack) {
        if (!blackActive) {
            blackActive = true;
            blackStartTime = now;
        }
    } else {
        if (blackActive) {
            unsigned long holdTime = now - blackStartTime;

            // Check cooldown to prevent rapid switching
            if ((now - lastCoverActionTime) > COOLDOWN_MS) {
                if (holdTime >= HOLD_TIME_MS) {
                    // 3+ second hold → Cycle menu
                    cycleMenu();
                    lastCoverActionTime = now;
                }
            }

            blackActive = false;
        }
    }

    esp_camera_fb_return(fb);
}
*/

// ================= AGE TRANSITION ANIMATION =================
void ageSparkle() {
  for (int i = 0; i < 6; i++) {
    display.drawPixel(random(0, 64), random(0, 32), SSD1306_WHITE);
  }
}

void ageShockwave() {
  for (int r = 2; r < 20; r += 3) {
    display.clearDisplay();
    display.drawCircle(32, 16, r, SSD1306_WHITE);
    display.display();
    vTaskDelay(pdMS_TO_TICKS(60));
  }
}

void ageSmashEffect() {
  // Text fall (gravity)
  int y = -12;
  while (y < 12) {
    display.clearDisplay();
    display.setCursor(9, y);
    display.print("LEVEL UP");
    display.display();
    y += 3;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  // Impact shake
  for (int i = 0; i < 8; i++) {
    display.clearDisplay();
    display.setCursor(9, 12 + random(-2, 3));
    display.print("LEVEL UP");
    display.display();
    vTaskDelay(pdMS_TO_TICKS(35));
  }
  // Impact debris
  for (int frame = 0; frame < 8; frame++) {
    display.clearDisplay();
    display.setCursor(9, 12);
    display.print("LEVEL UP");
    for (int i = 0; i < 12; i++) {
      int px = 32 + random(-frame * 3, frame * 3);
      int py = 16 + random(-frame * 2, frame * 2);
      display.drawPixel(px, py, SSD1306_WHITE);
    }
    display.display();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void playAgeTransitionAnimation() {
  Serial.println("🎂 Playing age transition animation!");
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 1. "AGE UP!" text
  display.clearDisplay();
  display.setCursor(14, 8);
  display.print("AGE UP!");
  display.display();
  vTaskDelay(pdMS_TO_TICKS(1200));

  // 2. Star burst
  for (int frame = 0; frame < 12; frame++) {
    display.clearDisplay();
    for (int i = 0; i < 10; i++) {
      int px = 32 + random(-frame * 2, frame * 2);
      int py = 16 + random(-frame * 2, frame * 2);
      display.drawPixel(px, py, SSD1306_WHITE);
    }
    display.display();
    vTaskDelay(pdMS_TO_TICKS(70));
  }

  // 3. Confetti
  for (int frame = 0; frame < 18; frame++) {
    display.clearDisplay();
    for (int i = 0; i < 12; i++) {
      int px = random(0, 64);
      int py = (frame * 2 + i * 3) % 32;
      display.drawPixel(px, py, SSD1306_WHITE);
    }
    display.display();
    vTaskDelay(pdMS_TO_TICKS(60));
  }

  // 4. "HAPPY BDAY" with sparkle
  for (int i = 0; i < 20; i++) {
    display.clearDisplay();
    ageSparkle();
    display.setCursor(10, 6);
    display.print("HAPPY");
    display.setCursor(10, 18);
    display.print("BDAY");
    display.display();
    vTaskDelay(pdMS_TO_TICKS(90));
  }
  vTaskDelay(pdMS_TO_TICKS(500));

  // 5. LEVEL UP smash
  ageSmashEffect();

  // 6. Shockwave
  ageShockwave();

  // 7. XP counter (animate from previous XP to new XP)
  int currentXP = ageTransitionPrevXP;
  while (currentXP <= ageTransitionXP) {
    display.clearDisplay();
    ageSparkle();
    display.setCursor(4, 8);
    display.print("XP:");
    display.print(currentXP);
    display.setCursor(4, 20);
    display.print("AGE:");
    display.print(ageTransitionAge);
    display.display();
    currentXP += 5;
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  // Hold final XP screen
  vTaskDelay(pdMS_TO_TICKS(1000));

  Serial.println("🎂 Age transition animation complete!");
}

// ================= PET ANIMATION FUNCTION =================
void displayPetAnimation() {
  if (!displayReady)
    return;

  // Age transition animation — plays immediately, blocks OLED task until done
  if (pendingAgeTransition) {
    pendingAgeTransition = false;
    playAgeTransitionAnimation();
    return;
  }

  // Display animation every 150ms
  if (millis() - lastAnimationTime >= ANIMATION_DISPLAY_INTERVAL) {
    lastAnimationTime = millis();

    // Debug: Print current screen type every 3 seconds
    static unsigned long lastScreenDebug = 0;
    if (millis() - lastScreenDebug >= 3000) {
      Serial.printf("📺 Current Screen: %s | Age: %d | Emotion: %s\n",
                    currentScreenType.c_str(), petAge, currentEmotion.c_str());
      lastScreenDebug = millis();
    }

    // Check for menu tilt gesture — blocked during game, walking, or sleeping
    if (playGameState != GAME_PLAYING && !petIsWalking && !isDeviceSleeping) {
      checkMenuTiltGesture();
    }

    // Hardware step detection — update petIsWalking from local timing
    detectHardwareStep();
    petIsWalking = (millis() - lastWalkingStepTime) < WALKING_WINDOW_MS;

    // Check which screen to display
    if (currentScreenType == "FOOD_MENU") {
      displayFoodMenu();
      return; // Exit early
    } else if (currentScreenType == "TOILET_MENU") {
      // Check for cleaning gesture in toilet menu
      checkCleaningGesture();
      displayToiletMenu();
      return; // Exit early
    } else if (currentScreenType == "PLAY_MENU") {
      displayPlayMenu();
      return; // Exit early
    } else if (currentScreenType == "HEALTH_MENU") {
      displayHealthMenu();
      return; // Exit early
    } else if (currentScreenType == "STATUS_INFO_MENU") {
      displayStatusInfoMenu();
      return; // Exit early
    } else if (currentScreenType == "STATS_MENU") {
      displayStatsMenu();
      return; // Exit early
    }

    // ── SLEEP / WALKING overrides for MAIN screen ──────────────────────────
    if (isDeviceSleeping) {
      displaySleepingAnimation();
      return;
    }
    if (petIsWalking) {
      displayWalkingAnimation();
      return;
    }

    // Default: MAIN screen with pet animation
    display.clearDisplay();

    // Select frame based on pet age AND current emotion
    const uint8_t *frameData = nullptr;
    uint8_t frameCount = 0;

    // EMOTION-BASED ANIMATION SELECTION
    // Priority: Emotion > Age
    if (currentEmotion == "HAPPY") {
      switch (petAge) {
      case INFANT: {
        frameData = infant_happy[currentFrame % INFANT_HAPPY_FRAME_COUNT];
        frameCount = INFANT_HAPPY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, INFANT_HAPPY_WIDTH,
                           INFANT_HAPPY_HEIGHT, SSD1306_WHITE);
        break;
      }
      case CHILD: {
        frameData = happy_child[currentFrame % HAPPY_CHILD_FRAME_COUNT];
        frameCount = HAPPY_CHILD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, HAPPY_CHILD_WIDTH,
                           HAPPY_CHILD_HEIGHT, SSD1306_WHITE);
        break;
      }
      case ADULT: {
        frameData = happy_adult[currentFrame % HAPPY_ADULT_FRAME_COUNT];
        frameCount = HAPPY_ADULT_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, HAPPY_ADULT_WIDTH,
                           HAPPY_ADULT_HEIGHT, SSD1306_WHITE);
        break;
      }
      case OLD: {
        frameData = old_happy[currentFrame % OLD_HAPPY_FRAME_COUNT];
        frameCount = OLD_HAPPY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      }
    } else if (currentEmotion == "CRY") {
      switch (petAge) {
      case INFANT: {
        frameData = infant_cry_frames[currentFrame % INFANT_CRY_FRAME_COUNT];
        frameCount = INFANT_CRY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, INFANT_CRY_WIDTH, INFANT_CRY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case CHILD: {
        frameData = cry_child[currentFrame % CRY_CHILD_FRAME_COUNT];
        frameCount = CRY_CHILD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, CRY_CHILD_WIDTH, CRY_CHILD_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case ADULT: {
        frameData = cry_adult[currentFrame % CRY_ADULT_FRAME_COUNT];
        frameCount = CRY_ADULT_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, CRY_ADULT_WIDTH, CRY_ADULT_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case OLD: {
        frameData = old_cry[currentFrame % OLD_CRY_FRAME_COUNT];
        frameCount = OLD_CRY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, OLD_CRY_WIDTH, OLD_CRY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      }
    } else if (currentEmotion == "SURPRISE") {
      switch (petAge) {
      case INFANT: {
        frameData =
            infant_surprise_frames[currentFrame % INFANT_SURPRISE_FRAME_COUNT];
        frameCount = INFANT_SURPRISE_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, INFANT_SURPRISE_WIDTH,
                           INFANT_SURPRISE_HEIGHT, SSD1306_WHITE);
        break;
      }
      case CHILD: {
        frameData = child_surprise[currentFrame % CHILD_SURPRISE_FRAME_COUNT];
        frameCount = CHILD_SURPRISE_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, CHILD_SURPRISE_WIDTH,
                           CHILD_SURPRISE_HEIGHT, SSD1306_WHITE);
        break;
      }
      case ADULT: {
        frameData = surprise_adult[currentFrame % SURPRISE_ADULT_FRAME_COUNT];
        frameCount = SURPRISE_ADULT_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, SURPRISE_ADULT_WIDTH,
                           SURPRISE_ADULT_HEIGHT, SSD1306_WHITE);
        break;
      }
      case OLD: {
        frameData = old_surprise[currentFrame % OLD_SURPRISE_FRAME_COUNT];
        frameCount = OLD_SURPRISE_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, OLD_SURPRISE_WIDTH,
                           OLD_SURPRISE_HEIGHT, SSD1306_WHITE);
        break;
      }
      }
    } else if (currentEmotion == "SAD" || currentEmotion == "HUNGER" ||
               currentEmotion == "SICK") {
      // SAD/HUNGER/SICK - sad animation + heart icon (sick = sad face + blinking heart)
      switch (petAge) {
      case INFANT: {
        frameData = infant_sad_frames[currentFrame % INFANT_SAD_FRAME_COUNT];
        frameCount = INFANT_SAD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, INFANT_SAD_WIDTH, INFANT_SAD_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case CHILD: {
        frameData = child_sad[currentFrame % CHILD_SAD_FRAME_COUNT];
        frameCount = CHILD_SAD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, CHILD_SAD_WIDTH, CHILD_SAD_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case ADULT: {
        frameData = adult_sad[currentFrame % ADULT_SAD_FRAME_COUNT];
        frameCount = ADULT_SAD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, ADULT_SAD_WIDTH, ADULT_SAD_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case OLD: {
        frameData = old_sad[currentFrame % OLD_SAD_FRAME_COUNT];
        frameCount = OLD_SAD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, OLD_SAD_WIDTH, OLD_SAD_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      }
    } else if (currentEmotion == "POOP") {
      // POOP - angry animation + poop icon (pet is annoyed by poop)
      switch (petAge) {
      case INFANT: {
        frameData = infant_angry_frames[currentFrame % INFANT_ANGRY_FRAME_COUNT];
        frameCount = INFANT_ANGRY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, INFANT_ANGRY_WIDTH, INFANT_ANGRY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case CHILD: {
        frameData = child_angry[currentFrame % CHILD_ANGRY_FRAME_COUNT];
        frameCount = CHILD_ANGRY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, CHILD_ANGRY_WIDTH, CHILD_ANGRY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case ADULT: {
        frameData = angry_adult[currentFrame % ANGRY_ADULT_FRAME_COUNT];
        frameCount = ANGRY_ADULT_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, ANGRY_ADULT_WIDTH, ANGRY_ADULT_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      case OLD: {
        frameData = old_angry[currentFrame % OLD_ANGRY_FRAME_COUNT];
        frameCount = OLD_ANGRY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, OLD_ANGRY_WIDTH, OLD_ANGRY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
      }
    } else if (currentEmotion == "IDLE") {
      // IDLE face - pet is calm/relaxed (not hungry, not sick, not dirty)
      // Show first frame only (static calm face)
      switch (petAge) {
      case INFANT:
        frameData = infant_frames[0]; // INFANT IDLE
        display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT,
                           SSD1306_WHITE);
        break;
      case CHILD:
        frameData = child_frames[0]; // CHILD IDLE
        display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT,
                           SSD1306_WHITE);
        break;
      case ADULT:
        frameData = adult_frames[0]; // ADULT IDLE
        display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT,
                           SSD1306_WHITE);
        break;
      case OLD:
        frameData = old_happy[0]; // OLD IDLE (first happy frame, static)
        display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
    } else {
      // Default age-based animations (normal/happy)
      switch (petAge) {
      case INFANT:
        frameData = infant_frames[currentFrame % INFANT_FRAME_COUNT];
        frameCount = INFANT_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT,
                           SSD1306_WHITE);
        break;
      case CHILD:
        frameData = child_frames[currentFrame % CHILD_FRAME_COUNT];
        frameCount = CHILD_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT,
                           SSD1306_WHITE);
        break;
      case ADULT:
        frameData = adult_frames[currentFrame % ADULT_FRAME_COUNT];
        frameCount = ADULT_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT,
                           SSD1306_WHITE);
        break;
      case OLD:
        frameData = old_happy[currentFrame % OLD_HAPPY_FRAME_COUNT];
        frameCount = OLD_HAPPY_FRAME_COUNT;
        display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT,
                           SSD1306_WHITE);
        break;
      }
    }

    // Icons hidden during walking/sleeping — resume when animation ends
    bool iconsAllowed = !petIsWalking && !isDeviceSleeping;

    // Home icon at top-left (always on MAIN screen)
    if (iconsAllowed && showHomeIcon && currentScreenType == "MAIN") {
      drawHomeIcon();
    }

    // Status icons at bottom-right
    // STRICT MUTUAL EXCLUSION (Priority: SICK > POOP > HUNGER > PLAY)
    if (iconsAllowed && currentScreenType == "MAIN") {
      if (showSickIcon) {
        drawSickIcon();
      } else if (showPoopIcon) {
        drawPoopIcon();
      } else if (showFoodIcon) {
        drawFoodIcon();
      } else if (showPlayIcon) {
        drawPlayIcon();
      }
    }

    // Only animation - no text
    display.display();

    // Increment frame
    currentFrame++;
  }
}

// ================= NETWORK TASK (Core 1 — Queue-driven HTTP) =================
// All HTTP calls happen here, queue-driven from loop()
// Runs on Core 1 alongside loop() — decouples sensor reading from network
// blocking
void networkTask(void *parameter) {
  Serial.println("🌐 Network Task started on Core 1 (queue-driven HTTP)");
  uint8_t reqType;

  while (true) {
    // Block until a request arrives (or 200ms timeout)
    if (xQueueReceive(networkQueue, &reqType, pdMS_TO_TICKS(200)) == pdTRUE) {
      switch (reqType) {

      case NET_SENSOR: {
        // Copy pending sensor data under mutex, then send
        SensorData dataCopy;
        if (xSemaphoreTake(networkDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          dataCopy = g_pendingSensor;
          xSemaphoreGive(networkDataMutex);
        }
        safeCpuFreq(
            160); // FIX: Mutex-guarded CPU freq (prevents Core 0/1 race)
        sendSensorDataOnly(dataCopy);
        safeCpuFreq(80);
        break;
      }

      case NET_OLED:
        // Consolidated: fetch OLED state + bundled events in one SSL handshake
        getOLEDDisplayFromServer();
        // If server flagged OTA update, perform it now (with retry for sealed device)
        if (otaUpdateRequested) {
          safeCpuFreq(240);
          for (int otaAttempt = 1; otaAttempt <= 3 && otaUpdateRequested; otaAttempt++) {
            Serial.printf("🔄 OTA attempt %d/3\n", otaAttempt);
            checkAndPerformOTA();
            // If still requested after return, download failed — retry after delay
            if (otaUpdateRequested && otaAttempt < 3) {
              Serial.printf("⚠️ OTA attempt %d failed, retrying in 30s...\n", otaAttempt);
              // Show retry notice on OLED
              display.clearDisplay();
              display.setCursor(2, 4);
              display.println("OTA RETRY");
              display.setCursor(2, 16);
              display.printf("%d/3 in 30s", otaAttempt + 1);
              display.display();
              vTaskDelay(pdMS_TO_TICKS(30000)); // 30s between retries
            }
          }
          if (otaUpdateRequested) {
            Serial.println("❌ OTA: All 3 attempts failed — will retry on next server poll");
            otaUpdateRequested = false; // Clear so normal operation resumes
          }
          safeCpuFreq(80);
        }
        break;

      case NET_IMAGE:
        safeCpuFreq(160);  // FIX: Mutex-guarded CPU freq
        sendImageData(""); // Uses shared capturedImageBuffer
        safeCpuFreq(80);
        break;

      case NET_CLEAN:
        // Lightweight cleaning request
        sendCleanRequest();
        break;

      case NET_INJECT:
        // Notify server medicine was given
        sendInjectRequest();
        break;

      case NET_HAPPY:
        // Right-tilt menu cycle interaction → happiness +5 on server
        sendCoverHappyRequest();
        break;

      case NET_GAME_REWARD:
        // Game over — send score + KakuCoin reward (safe on Core 1)
        sendKakuCoinReward(pendingRewardScore, pendingRewardKC);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(20)); // Brief settle between requests
    }
  }
}

// ================= PHYSIOLOGY ENGINE (LOCAL) =================
void handlePhysiology() {
  if (millis() - lastPhysioTick < PHYSIO_TICK_MS)
    return;
  lastPhysioTick = millis();

  Serial.println("💓 Physiology Tick (120s)");

  // 1️⃣ Uptime & Aging
  g_petState.totalUptimeSecs += 120;
  int newAgeDays = g_petState.totalUptimeSecs / 86400; // 24 hours
  if (newAgeDays > g_petState.ageInt) {
    g_petState.ageInt = newAgeDays;
    Serial.printf("🎂 Pet aged! Now %d days old\n", g_petState.ageInt);
    // Level up on birthday
    ageTransitionPrevXP = g_petState.xp; // Capture XP BEFORE bonus
    g_petState.level++;
    g_petState.xp += 100;
    // Trigger age transition animation on OLED (Core 0)
    ageTransitionAge = g_petState.ageInt;
    ageTransitionXP = g_petState.xp;
    pendingAgeTransition = true;
  }

  // 2️⃣ Hunger Engine (Stage dependent)
  int hungerDecay = 8;
  if (petAge == INFANT) {
    hungerDecay = 15;
  } else if (petAge == CHILD) {
    hungerDecay = 10;
  } else if (petAge == OLD) {
    hungerDecay = 12;
  }

  // Increment stats
  g_petState.hunger = min(100, g_petState.hunger + hungerDecay);
  g_petState.energy = max(0, g_petState.energy - 2);

  // 3️⃣ Health Engine
  int healthPenalty = 0;
  if (g_petState.hunger > 90)
    healthPenalty += 5;
  // (thirst removed)
  if (g_petState.energy < 20)
    healthPenalty += 2;
  if (g_petState.hasPoop)
    healthPenalty += 10;

  // Sickness Engine (Hardware Authoritative)
  if (!g_petState.isSick) {
    if (g_petState.health <= 25 && random(0, 100) < 30) {
      // 30% chance if health <= 25
      g_petState.isSick = true;
      Serial.println("🤒 Pet became sick due to poor health/neglect!");
    } else if (g_petState.hasPoop && random(0, 100) < 15) {
      // 15% chance with poop present
      g_petState.isSick = true;
      Serial.println("🤒 Pet became sick from living with poop!");
    }
  }

  if (g_petState.isSick)
    healthPenalty += 5;

  g_petState.health = max(0, g_petState.health - healthPenalty);

  // Poop generation (30 min after feed)
  // Simplified: 5% chance per tick if hunger < 50 and no poop
  if (!g_petState.hasPoop && g_petState.hunger < 50 && random(0, 100) < 5) {
    g_petState.hasPoop = true;
    Serial.println("💩 Pet pooped!");
  }

  // Discipline penalty for neglect
  if (g_petState.hunger > 90 || g_petState.hasPoop) {
    g_petState.discipline = max(0, g_petState.discipline - 2);
  } else {
    g_petState.discipline = min(100, g_petState.discipline + 1);
  }

  // XP Growth
  if (g_petState.health > 80)
    g_petState.xp += 2;
  else
    g_petState.xp += 1;

  // Sync and Persist
  syncLocalStateToUI();
  savePetState();
}

void loop() {
  // ── PROVISIONING MODE GUARD ────────────────────────────────────────────
  // If in AP provisioning mode, only serve web config page — skip everything
  // else
  if (wifiProvisioningMode) {
    wifiConfigServer.handleClient();
    delay(10);
    return;
  }

  // Handle core pet physiology autonomously
  handlePhysiology();

  // This loop runs on Core 1 - handles sensors + queue dispatching ONLY
  // HTTP calls moved to networkTask on Core 1 — no blocking here

  // Check WiFi connection with timeout protection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi disconnected, attempting reconnect...");
    // Try stored credentials first, then hardcoded fallback
    bool reconnected = false;
    int count = getStoredWiFiCount();
    if (count > 0) {
      wifiPrefs.begin("wifi", true);
      // Try most recently added credential first (most likely current network)
      String key_s = "wifi" + String(count - 1) + "_s";
      String key_p = "wifi" + String(count - 1) + "_p";
      String ssid = wifiPrefs.getString(key_s.c_str(), "");
      String pass = wifiPrefs.getString(key_p.c_str(), "");
      wifiPrefs.end();
      if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long reconnectStart = millis();
        while (WiFi.status() != WL_CONNECTED &&
               (millis() - reconnectStart < 10000)) {
          Serial.print(".");
          vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (WiFi.status() == WL_CONNECTED)
          reconnected = true;
      }
    }
    // Fallback to hardcoded credentials
    if (!reconnected) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      unsigned long reconnectStart = millis();
      while (WiFi.status() != WL_CONNECTED &&
             (millis() - reconnectStart < 10000)) {
        Serial.print(".");
        vTaskDelay(pdMS_TO_TICKS(300));
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi Reconnected: " + WiFi.localIP().toString());
    } else {
      Serial.println("\n⚠️  WiFi reconnection failed — offline mode");
    }
  }

  // Debug: Print WiFi status + heap health every 10 seconds
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minHeap  = ESP.getMinFreeHeap();
    Serial.printf("🔗 WiFi: %s | IP: %s | RSSI: %d dBm\n",
                  WiFi.status() == WL_CONNECTED ? "✅" : "❌",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("🎤 Audio Energy: %d | 🧠 Heap: %u free / %u min\n",
                  audioEnergyLevel, freeHeap, minHeap);

    // SEALED DEVICE SAFETY: Reboot if heap critically low (memory leak guard)
    // ESP32-S3 has ~390KB total — below 20KB means imminent crash
    if (freeHeap < 20000) {
      Serial.println("🚨 CRITICAL: Heap below 20KB — rebooting to recover!");
      savePetState();  // Persist state before reboot
      delay(500);
      ESP.restart();
    }
  }

  // SEALED DEVICE SAFETY: Automatic daily reboot (clears memory fragmentation)
  // Reboots after 24 hours of uptime — only when device is sleeping (minimal disruption)
  static const unsigned long DAILY_REBOOT_MS = 86400000UL; // 24 hours
  if (millis() > DAILY_REBOOT_MS && isDeviceSleeping) {
    Serial.println("🔄 Daily maintenance reboot (24h uptime, device sleeping)");
    savePetState();
    delay(500);
    ESP.restart();
  }

  // ── SENSOR BATCH COLLECTION (every 100ms, fast I2C only, no HTTP) ──────────
  if (millis() - lastInternalReadTime >= INTERNAL_READ_INTERVAL) {
    lastInternalReadTime = millis();
    if (sensorBatch.reading_count < SensorDataBatch::MAX_READINGS) {
      SingleReading reading;
      reading.timestamp_ms = millis();
      if (mpuAvailable) {
        int16_t ax, ay, az, gx, gy, gz;
        mpu.getAcceleration(&ax, &ay, &az);
        reading.accel_x = ax / 16384.0 * 9.81;
        reading.accel_y = ay / 16384.0 * 9.81;
        reading.accel_z = az / 16384.0 * 9.81;
        mpu.getRotation(&gx, &gy, &gz);
        reading.gyro_x = gx / 131.0;
        reading.gyro_y = gy / 131.0;
        reading.gyro_z = gz / 131.0;
      } else {
        reading.accel_x = reading.accel_y = reading.accel_z = 0.0;
        reading.gyro_x = reading.gyro_y = reading.gyro_z = 0.0;
      }
      sensorBatch.readings[sensorBatch.reading_count++] = reading;
      if (micReady && audioEnergyLevel > 0)
        totalMicLevel += (20.0 + audioEnergyLevel / 100.0);
      micReadingCount++;
    }
  }

  // ── CAMERA IMAGE CHECK (DISABLED - Manual feeding via tilt gesture)
  // ─────────── Auto-send removed: User must tilt+hold left for 3 seconds to
  // feed

  // ── FEEDING GESTURE TIMEOUT CHECK ─────────────────────────────────────────
  // Reset feeding flag if stuck for more than 30 seconds
  if ((capturingForFeeding || isUploadingImage) &&
      (millis() - feedingGestureStartTime > FEEDING_TIMEOUT)) {
    Serial.println("⚠️ Feeding gesture timeout - resetting flags");
    capturingForFeeding = false;
    isUploadingImage = false; // Also reset eating animation
  }

  // ── NEUTRAL / SLEEP STATE DETECTION ──────────────────────────────────────
  // If the device stays flat (neutral) for 30 s enter sleep mode:
  //   • network paused, WiFi modem sleeps, display shows sleeping animation
  //   • sleep seconds accumulated and sent with the next sensor upload on wake
  if (!isDeviceSleeping) {
    if (isDeviceInverted()) {
      if (neutralStartTime == 0)
        neutralStartTime = millis();
      if (millis() - neutralStartTime >= NEUTRAL_SLEEP_TIMEOUT) {
        isDeviceSleeping = true;
        sleepStartTime = millis();
        Serial.println("😴 Inverted 30s → SLEEP MODE (network paused)");
      }
    } else {
      neutralStartTime = 0; // reset if device is moved
    }
  } else {
    // Already sleeping — wake on any meaningful movement (non-inverted state)
    if (!isDeviceInverted()) {
      uint32_t sleptSec = (millis() - sleepStartTime) / 1000;
      accumulatedSleepSec += sleptSec;
      Serial.printf("⏰ Woke up — slept %us (banked: %us)\n", sleptSec,
                    accumulatedSleepSec);
      isDeviceSleeping = false;
      neutralStartTime = 0;
      sleepStartTime = 0;
    }
  }

  // While sleeping: skip all network work, yield CPU, and let oledTask render
  // the animation
  if (isDeviceSleeping) {
    vTaskDelay(pdMS_TO_TICKS(500));
    return;
  }

  // ── STAGGERED NETWORK SCHEDULER ───────────────────────────────────────────
  // Uses 500ms tick slots to spread requests — no simultaneous HTTP bursts
  // Slot 0,20,40...  (t % 10000 ≈ 0)    → sensor every 10 s
  // Slot 5,15,25...  (t % 5000  ≈ 2500) → OLED + events every 5 s (2.5 s
  // offset)
  static uint32_t lastSensorTick = 0;
  static uint32_t lastOledTick = 0;
  uint32_t nowTick = millis() / 500; // 500 ms resolution ticks

  // SLOT A — Sensor data every 10 s (tick 0,20,40,...)
  if (nowTick % 20 == 0 && nowTick != lastSensorTick && !isUploadingImage) {
    lastSensorTick = nowTick;
    SensorData data = readAllSensors();
    data.sensor_batch = sensorBatch;
    data.sensor_batch.avg_mic_level =
        micReadingCount > 0 ? totalMicLevel / micReadingCount : 0.0;
    data.sensor_batch.sound_data = audioEnergyLevel;
    Serial.printf("📤 Queue: NET_SENSOR (%d readings)\n",
                  sensorBatch.reading_count);
    if (xSemaphoreTake(networkDataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      g_pendingSensor = data;
      xSemaphoreGive(networkDataMutex);
    }
    uint8_t req = NET_SENSOR;
    xQueueSend(networkQueue, &req, 0);
    sensorBatch.reading_count = 0;
    totalMicLevel = 0.0;
    micReadingCount = 0;
  }

  // SLOT B — OLED state + events combined every 2 s (tick 4,8,12,16...) -
  // FASTER POLLING
  if (nowTick % 4 == 0 && nowTick != lastOledTick && startupComplete &&
      !isUploadingImage) {
    lastOledTick = nowTick;
    uint8_t req = NET_OLED;
    xQueueSend(networkQueue, &req, 0);
  }

  vTaskDelay(pdMS_TO_TICKS(50)); // 50ms loop cadence — reduces CPU heat (was 20ms)
}

// ================= CAMERA INITIALIZATION =================
bool initCamera() {
  Serial.println("Initializing Camera...");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; // Fixed: was pin_sccb_sda
  config.pin_sscb_scl = SIOC_GPIO_NUM; // Fixed: was pin_sccb_scl
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz =
      10000000; // 10 MHz — reduced from 20 MHz (less heat, stable)
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QQVGA; // 160x120 - reduced for power
  config.jpeg_quality = 20;            // Lower quality = less heat
  config.fb_count = 1;                 // Single buffer = less memory
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  cameraReady = true;
  Serial.println("Camera initialized successfully");
  return true;
}

// ================= AUDIO INITIALIZATION =================
bool initAudio() {
  Serial.println("Initializing PDM Microphone...");

  // Create I2S channel configuration
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  // Create new I2S channel
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  if (err != ESP_OK) {
    Serial.printf("I2S new channel failed: %d\n", err);
    return false;
  }

  // Configure PDM RX mode
  i2s_pdm_rx_config_t pdm_rx_cfg = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                 I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .clk = PDM_CLK_GPIO,
              .din = PDM_DIN_GPIO,
              .invert_flags = {.clk_inv = false},
          },
  };

  err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
  if (err != ESP_OK) {
    Serial.printf("PDM RX mode init failed: %d\n", err);
    return false;
  }

  err = i2s_channel_enable(rx_handle);
  if (err != ESP_OK) {
    Serial.printf("I2S channel enable failed: %d\n", err);
    return false;
  }

  // Allocate VAD buffer
  vad_buffer = (int16_t *)malloc(VAD_BUFFER_SIZE * sizeof(int16_t));
  if (!vad_buffer) {
    Serial.println("❌ Failed to allocate VAD buffer");
    return false;
  }

  micReady = true;
  Serial.println("✅ PDM Microphone initialized successfully");
  return true;
}

// ================= DUAL-CORE CAMERA MONITORING TASK =================
void cameraMonitorTask(void *parameter) {
  Serial.println("📸 Core 0: Camera monitoring task started (on-demand only)");

  // Start at low frequency - camera is idle
  setCpuFrequencyMhz(80);
  Serial.println("⚡ CPU: 80MHz (camera idle)");

  while (true) {
    if (!cameraReady) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Only capture when feeding gesture is triggered
    if (capturingForFeeding && !cameraImageReady) {
      // Boost CPU for capture
      safeCpuFreq(240); // FIX: Mutex-guarded — prevents race with networkTask
      Serial.println("⚡ CPU: 240MHz (capturing)");

      Serial.println("📸 Core 0: Feeding triggered - capturing fresh image...");
      cameraCapturing = true;
      camera_fb_t *fb = esp_camera_fb_get();

      if (fb) {
        Serial.printf("✅ Core 0: Image captured: %d bytes\n", fb->len);

        if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          if (capturedImageBuffer != NULL) {
            free(capturedImageBuffer);
          }
          capturedImageBuffer = (uint8_t *)ps_malloc(fb->len);
          if (capturedImageBuffer) {
            memcpy(capturedImageBuffer, fb->buf, fb->len);
            capturedImageLength = fb->len;
            cameraImageReady = true;
            Serial.println("📦 Core 0: Image buffered for Core 1");
          } else {
            Serial.println("❌ Core 0: Failed to allocate image buffer");
          }
          xSemaphoreGive(cameraMutex);
        }
        esp_camera_fb_return(fb);
      } else {
        Serial.println("❌ Core 0: Camera capture failed");
      }
      cameraCapturing = false;
      capturingForFeeding = false; // Ensure it only captures once per gesture!

      // Drop back to low frequency after capture
      safeCpuFreq(80); // FIX: Mutex-guarded
      Serial.println("⚡ CPU: 80MHz (camera idle)");
    }

    // Poll every 50ms (low CPU, on-demand only)
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ================= DUAL-CORE AUDIO MONITORING TASK =================
void audioMonitorTask(void *parameter) {
  Serial.println("🎤 Core 0: Audio monitoring task started");

  size_t bytes_read;
  unsigned long speechStartTime = 0;
  unsigned long lastSoundTime = 0;
  bool currentlyRecording = false;

  // Audio recording buffer for when speech is detected
  uint8_t *recording_buffer = NULL;
  size_t recorded_bytes = 0;
  const size_t MAX_RECORDING_SIZE = SAMPLE_RATE * 2 * 5; // Max 5 seconds

  while (true) {
    if (!micReady) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Continuously read audio for VAD analysis
    esp_err_t err = i2s_channel_read(rx_handle, vad_buffer,
                                     VAD_BUFFER_SIZE * sizeof(int16_t),
                                     &bytes_read, pdMS_TO_TICKS(10));

    if (err != ESP_OK || bytes_read == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // Calculate audio energy for Voice Activity Detection
    int32_t energy = 0;
    int samples = bytes_read / sizeof(int16_t);

    for (int i = 0; i < samples; i++) {
      int16_t sample = vad_buffer[i];
      energy += abs(sample);
    }

    energy = energy / samples; // Average energy
    audioEnergyLevel = energy; // Update global energy level

    unsigned long currentTime = millis();

    // Voice Activity Detection
    if (energy > VAD_THRESHOLD) {
      lastSoundTime = currentTime;

      if (!currentlyRecording) {
        // Start recording when speech detected
        if (speechStartTime == 0) {
          speechStartTime = currentTime;
        }

        // Check if we've had continuous speech for minimum duration
        if (currentTime - speechStartTime >= VAD_MIN_DURATION) {
          Serial.println("🎤 Core 0: Speech detected! Starting recording...");
          currentlyRecording = true;

          // Allocate recording buffer
          recording_buffer =
              (uint8_t *)ps_malloc(MAX_RECORDING_SIZE + WAV_HEADER_SIZE);
          if (recording_buffer) {
            generate_wav_header(recording_buffer, MAX_RECORDING_SIZE,
                                SAMPLE_RATE);
            recorded_bytes = WAV_HEADER_SIZE;
          }
        }
      }
    } else {
      // Below threshold — reset speech start
      if (currentTime - lastSoundTime > 200) {
        speechStartTime = 0;
      }
    }

    // Mic sleep: if silent for 2+ seconds and not recording, slow-poll to save
    // power (major heat reduction — I2S idles instead of continuous read)
    if (!currentlyRecording && (currentTime - lastSoundTime) > 2000) {
      audioEnergyLevel = 0;           // Report silence
      vTaskDelay(pdMS_TO_TICKS(500)); // Sleep 500ms between reads (was 200ms)
      continue;
    }

    // If currently recording, add audio data to buffer
    if (currentlyRecording && recording_buffer &&
        (recorded_bytes + bytes_read) <
            (MAX_RECORDING_SIZE + WAV_HEADER_SIZE)) {

      // Apply volume gain and copy to recording buffer
      for (int i = 0; i < samples; i++) {
        int16_t sample = vad_buffer[i];
        int32_t amp = sample << VOLUME_GAIN;
        if (amp > 32767)
          amp = 32767;
        if (amp < -32768)
          amp = -32768;

        *((int16_t *)(recording_buffer + recorded_bytes)) = amp;
        recorded_bytes += sizeof(int16_t);
      }
    }

    // Stop recording after silence timeout or buffer full
    if (currentlyRecording &&
        ((currentTime - lastSoundTime > SILENCE_TIMEOUT) ||
         (recorded_bytes >= (MAX_RECORDING_SIZE + WAV_HEADER_SIZE - 1024)))) {

      Serial.printf("🎤 Core 0: Recording complete! %d bytes\n",
                    recorded_bytes);

      if (recording_buffer && recorded_bytes > WAV_HEADER_SIZE) {
        // Convert to base64 and store for Core 1
        String audioB64 = base64::encode(recording_buffer, recorded_bytes);

        if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          detectedAudioData = audioB64;
          speechDetected = true;
          Serial.printf(
              "🎤 Core 0: Audio ready for transmission (%d chars base64)\n",
              audioB64.length());
          xSemaphoreGive(audioMutex);
        }
      }

      // Clean up
      if (recording_buffer) {
        free(recording_buffer);
        recording_buffer = NULL;
      }
      recorded_bytes = 0;
      currentlyRecording = false;
      speechStartTime = 0;
    }

    // Increase VAD delay to 10ms (no quality loss, better power savings)
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

SensorData readAllSensors() {
  SensorData data = {0};

  if (mpuAvailable) {
    // Read actual sensor data from MPU6050
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    data.accel_x = ax / 16384.0 * 9.81;
    data.accel_y = ay / 16384.0 * 9.81;
    data.accel_z = az / 16384.0 * 9.81;

    int16_t gx, gy, gz;
    mpu.getRotation(&gx, &gy, &gz);
    data.gyro_x = gx / 131.0;
    data.gyro_y = gy / 131.0;
    data.gyro_z = gz / 131.0;
  } else {
    // MPU6050 not available - send zero values with error indication
    data.accel_x = 0.0;
    data.accel_y = 0.0;
    data.accel_z = 0.0;
    data.gyro_x = 0.0;
    data.gyro_y = 0.0;
    data.gyro_z = 0.0;
    Serial.println("⚠️  MPU6050 not available - sending zero values");
  }

  // Store calibrated accelerometer values (server will compute direction)
  data.calibrated_ax = data.accel_x;
  data.calibrated_ay = data.accel_y;
  data.calibrated_az = data.accel_z;
  data.device_orientation = "COMPUTING"; // Placeholder - server will compute
  data.orientation_confidence = 0.0;     // Placeholder - server will compute

  // Use real microphone energy level from audio monitoring
  if (micReady && audioEnergyLevel > 0) {
    // Convert audio energy to approximate dB level
    data.mic_level =
        20.0 + (audioEnergyLevel / 100.0); // Scale energy to dB range
    data.sound_data = audioEnergyLevel;
  } else {
    // Microphone not ready or no audio data
    data.mic_level = 0.0;
    data.sound_data = 0;
  }

  // Read ESP32 internal chip temperature
  data.chip_temperature = temperatureRead();

  // Initialize image/audio fields
  data.camera_image_b64 = "";
  data.audio_data_b64 = "";
  data.has_new_image = false;
  data.has_new_audio = false;

  // Add local pet state for server mirroring
  // Note: SensorData struct needs these fields or we send via JSON in
  // sendSensorDataOnly Log orientation for debugging (if not neutral/face-up)
  // if (abs(data.accel_x) > 3.0 || abs(data.accel_y) > 3.0 || data.accel_z
  // < 5.0) {
  //     Serial.printf("📐 Orientation Check: AX=%.1f, AY=%.1f, AZ=%.1f\n",
  //                   data.accel_x, data.accel_y, data.accel_z);
  // }

  return data;
}

// ================= SERVER HEALTH CHECK =================
bool isServerAlive() {
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);

  if (!http.begin(
          sslNet,
          "https://kakuproject-90943350924.asia-south1.run.app/api/health")) {
    return false;
  }

  int code = http.GET();
  trackHttpResult(code);
  http.end();

  return (code == 200);
}

// ================= OLED DISPLAY ANIMATION POLLING =================
void getOLEDDisplayFromServer() {
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(3000);
  http.setTimeout(3000);

  if (!http.begin(sslNet, String(oledDisplayUrl))) {
    Serial.println("⚠️ OLED poll: begin failed, resetting SSL");
    resetSSLConnection();
    return;
  }

  int httpCode = http.GET();
  trackHttpResult(httpCode);

  if (httpCode == 200) {
    String response = http.getString();

    // Reuse global StaticJsonDocument (no heap allocation)
    g_oledDoc.clear();
    DeserializationError error = deserializeJson(g_oledDoc, response);

    if (!error) {
      // Handle animation_id
      if (g_oledDoc.containsKey("animation_id")) {
        int newAnimationId = g_oledDoc["animation_id"].as<int>();
        String animationName = g_oledDoc["animation_name"] | "UNKNOWN";

        if (newAnimationId >= 0 && newAnimationId <= 3) {
          // SERVER OVERRIDE DISABLED: We trust local age calculation
          // petAge = (PetAge)newAnimationId;
        }
      }

      // Parse actual integer age from server (Now mirrored back from server)
      if (g_oledDoc.containsKey("age")) {
        int newAge = g_oledDoc["age"].as<int>();
        // Only sync if local age is behind server (e.g., first sync or remote
        // reset)
        if (g_petState.ageInt < newAge) {
          g_petState.ageInt = newAge;
          syncLocalStateToUI();
        }
      }

      // screen_type / screen_state override DISABLED — ESP32 controls menu
      // locally via tilt gesture
      /*
      if (g_oledDoc.containsKey("screen_type")) {
          String newScreenType = g_oledDoc["screen_type"].as<String>();
          if (currentScreenType == "FOOD_MENU" && newScreenType != "FOOD_MENU")
      { imageAlreadySentThisSession = false;
          }
          currentScreenType = newScreenType;
          Serial.printf("📺 Screen Type: %s\n", currentScreenType.c_str());
      } else if (g_oledDoc.containsKey("screen_state")) {
          String newScreenState = g_oledDoc["screen_state"].as<String>();
          if (currentScreenType == "FOOD_MENU" && newScreenState != "FOOD_MENU")
      { imageAlreadySentThisSession = false;
          }
          currentScreenType = newScreenState;
          Serial.printf("📺 Screen State: %s\n", currentScreenType.c_str());
      }
      */

      if (currentScreenType == "MAIN") {
        showHomeIcon = true;
      } else {
        showHomeIcon = false;
      }

      if (g_oledDoc.containsKey("show_food_icon")) {
        bool newShowFood = g_oledDoc["show_food_icon"].as<bool>();

        // Ignore server food icon updates for 10 seconds after feeding
        // This gives server time to process image and reduce hunger
        if (justFedPet && (millis() - lastFeedTime < FEED_IGNORE_DURATION)) {
          Serial.println(
              "🍽️  Ignoring server food icon (just fed, waiting for sync)");
        } else {
          justFedPet = false; // Resume accepting server updates
          if (showFoodIcon != newShowFood) {
            showFoodIcon = newShowFood;
            Serial.printf("🍽️  Food Icon: %s\n",
                          showFoodIcon ? "SHOW" : "HIDE");
          }
        }
      }

      if (g_oledDoc.containsKey("has_poop")) {
        showPoopIcon = g_oledDoc["has_poop"].as<bool>();
        Serial.printf("💩 Poop Icon: %s\n", showPoopIcon ? "SHOW" : "HIDE");
      }

      // is_sick is now controlled entirely locally by the ESP32 physiology
      // engine. Server is_sick field is ignored — hardware is authoritative.

      // Play icon — now controlled locally (15 min after poop cleared)
      // Server show_play_icon ignored — hardware is authoritative

      Serial.println("🤖 Mode: AUTOMATIC (forced)");

      // Parse happiness and discipline for status bars
      if (g_oledDoc.containsKey("happiness")) {
        int newHappy = g_oledDoc["happiness"].as<int>();
        if (petHappiness != newHappy) {
          petHappiness = newHappy;
          Serial.printf("😀 Happiness: %d\n", petHappiness);
        }
      }
      if (g_oledDoc.containsKey("discipline")) {
        int newDisc = g_oledDoc["discipline"].as<int>();
        if (petDiscipline != newDisc) {
          petDiscipline = newDisc;
          Serial.printf("📊 Discipline: %d\n", petDiscipline);
        }
      }

      if (g_oledDoc.containsKey("is_hungry")) {
        // Ignore server hunger updates for 10 seconds after feeding
        if (justFedPet && (millis() - lastFeedTime < FEED_IGNORE_DURATION)) {
          Serial.println(
              "🍽️  Ignoring server hunger status (just fed, waiting for sync)");
        } else {
          petIsHungry = g_oledDoc["is_hungry"].as<bool>();
          Serial.printf("🍽️  Hungry: %s\n", petIsHungry ? "YES" : "NO");
        }
      }

      if (g_oledDoc.containsKey("current_emotion")) {
        String emotion = g_oledDoc["current_emotion"].as<String>();
        if (emotion == "LOCAL") {
          isServerEmotionOverride = false;
        } else {
          isServerEmotionOverride = true;
          if (currentEmotion != emotion) {
            currentEmotion = emotion;
            Serial.printf("😊 Server Sensory Override: %s\n",
                          currentEmotion.c_str());
          }
        }

        if (emotion == "EATING" && currentScreenType == "FOOD_MENU") {
          Serial.println(
              "😋 Emotion: EATING - triggering animation on FOOD MENU!");
          playEatingAnimation();
          justFinishedEating = true;
          eatingFinishTime = millis();
        }
      }

      // is_walking is now driven by hardware step counter in loop()
      // Server is_walking field ignored — hardware has lower latency

      // OTA update flag — server sets this when dashboard "Enable OTA" is
      // pressed
      if (g_oledDoc.containsKey("ota_update")) {
        bool otaFlag = g_oledDoc["ota_update"].as<bool>();
        if (otaFlag && !otaUpdateRequested) {
          otaUpdateRequested = true;
          Serial.println("🔄 OTA update requested by server!");
        }
      }

      // Process bundled events (saves one SSL handshake!)
      if (g_oledDoc.containsKey("events")) {
        JsonArray events = g_oledDoc["events"];
        if (events.size() > 0) {
          Serial.printf("🚨 Bundled Events: %d\n", events.size());
          for (size_t i = 0; i < events.size(); i++) {
            JsonObject event = events[i];
            const char *event_type = event["event_type"];
            const char *message = event["message"];
            processEvent(event_type, message);
          }
        }
      }
    }
  }

  http.end();
}

// ================= OTA PROGRESS REPORTING =================
// Posts ESP32's live OTA state to the server so the dashboard can show
// progress. Uses sslNet (separate from sslOTA which is busy streaming
// firmware). Called at key milestones: checking, downloading (%), flashing,
// rebooting.
void postOTAProgress(const char *otaStatus, int progress,
                     const String &targetVersion, const char *message) {
  if (WiFi.status() != WL_CONNECTED)
    return;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  if (!http.begin(sslNet, "https://kakuproject-90943350924.asia-south1.run.app/"
                          "api/ota/progress")) {
    Serial.printf("⚠️ OTA progress report: connect failed (%s %d%%)\n",
                  otaStatus, progress);
    return;
  }

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["device_id"] = "ESP32_001";
  doc["ota_status"] = otaStatus;
  doc["progress"] = progress;
  doc["target_version"] = targetVersion;
  doc["message"] = message;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  trackHttpResult(code);
  http.end();

  Serial.printf("📡 OTA progress → server: %s %d%% (HTTP %d)\n", otaStatus,
                progress, code);
}

// ================= OTA FIRMWARE UPDATE =================
// Called from networkTask when ota_update flag is received from server.
// Uses ESP32 dual-partition OTA (automatic A/B swap) — safe rollback on
// failure. Flow: check /api/firmware/latest → if newer → download .bin → flash
// → reboot
void checkAndPerformOTA() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ OTA: WiFi not connected, aborting");
    otaUpdateRequested = false;
    return;
  }

  Serial.println("🔄 OTA: Checking for firmware update...");
  Serial.printf("   Current version: %s\n", FIRMWARE_VERSION);

  // Show OTA status on OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 4);
  display.println("OTA UPDATE");
  display.setCursor(2, 16);
  display.println("Checking...");
  display.display();

  // Step 1: Check /api/firmware/latest for newer version
  setCpuFrequencyMhz(240); // Max CPU for OTA

  sslOTA.stop(); // Reset connection state
  sslOTA.setInsecure();          // Re-apply after stop() clears SSL config
  sslOTA.setTimeout(120);
  sslOTA.setHandshakeTimeout(30);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  if (!http.begin(sslOTA, String(firmwareCheckUrl))) {
    Serial.println("❌ OTA: Failed to connect to firmware server");
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  http.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("❌ OTA: Firmware check failed, HTTP %d\n", httpCode);
    http.end();
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  String response = http.getString();
  http.end();

  // Parse response
  StaticJsonDocument<512> otaDoc;
  DeserializationError error = deserializeJson(otaDoc, response);
  if (error) {
    Serial.printf("❌ OTA: JSON parse error: %s\n", error.c_str());
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  bool updateAvailable = otaDoc["update_available"] | false;
  if (!updateAvailable) {
    Serial.println("✅ OTA: Firmware is up to date");
    display.clearDisplay();
    display.setCursor(2, 4);
    display.println("OTA UPDATE");
    display.setCursor(2, 16);
    display.println("Up to date!");
    display.display();
    delay(2000);
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  // Copy strings BEFORE otaDoc goes out of scope or gets reused
  String newVersion = String(otaDoc["version"] | "?");
  String downloadUrl = String(otaDoc["download_url"] | "");
  int fileSize = otaDoc["file_size"] | 0;
  String checksumStr = String(otaDoc["checksum"] | "");

  // Force HTTPS — Cloud Run redirects http→https, ESP32 can't follow 302
  if (downloadUrl.startsWith("http://")) {
    downloadUrl = "https://" + downloadUrl.substring(7);
  }

  Serial.printf("🆕 OTA: New firmware v%s available (%d bytes)\n",
                newVersion.c_str(), fileSize);
  Serial.printf("   Download: %s\n", downloadUrl.c_str());

  // Report to server: about to start downloading
  // MUST be done BEFORE sslOTA connects — ESP32 can't hold 2 concurrent SSL
  // connections
  postOTAProgress("downloading", 0, newVersion,
                  ("Downloading v" + newVersion + "...").c_str());

  // Step 2: Show downloading status on OLED
  display.clearDisplay();
  display.setCursor(2, 4);
  display.println("UPDATING");
  display.setCursor(2, 16);
  display.print("v");
  display.println(newVersion);
  display.display();

  // Step 3: Download and flash firmware using global sslOTA + Update library
  sslOTA.stop();
  sslOTA.setInsecure();          // re-apply after stop() clears state
  sslOTA.setTimeout(120);        // 120s socket-level read timeout (seconds)
  sslOTA.setHandshakeTimeout(30); // 30s TLS handshake timeout
  HTTPClient httpOTA;
  httpOTA.setConnectTimeout(15000);
  httpOTA.setTimeout(120000); // 120s HTTP-level timeout (ms)

  if (!httpOTA.begin(sslOTA, downloadUrl)) {
    Serial.println("❌ OTA: Failed to connect for firmware download");
    postOTAProgress("failed", 0, newVersion, "Failed to connect for download");
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  httpOTA.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
  int dlCode = httpOTA.GET();

  if (dlCode != 200) {
    Serial.printf("❌ OTA: Download failed, HTTP %d\n", dlCode);
    httpOTA.end();
    postOTAProgress("failed", 0, newVersion,
                    ("Download HTTP error " + String(dlCode)).c_str());
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  int contentLength = httpOTA.getSize();
  if (contentLength <= 0) {
    Serial.println("❌ OTA: Invalid content length");
    httpOTA.end();
    postOTAProgress("failed", 0, newVersion, "Invalid content length");
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  Serial.printf("📦 OTA: Downloading %d bytes...\n", contentLength);

  // ===== SUSPEND ALL OTHER TASKS DURING OTA =====
  // Free up CPU, memory, and WiFi bandwidth for reliable OTA flash
  otaInProgress = true;
  if (audioTaskHandle)
    vTaskSuspend(audioTaskHandle);
  if (cameraTaskHandle)
    vTaskSuspend(cameraTaskHandle);
  if (oledTaskHandle)
    vTaskSuspend(oledTaskHandle);
  Serial.println("⏸️  All tasks suspended for OTA");

  // Begin OTA update with Update library (ESP32 dual-partition)
  if (!Update.begin(contentLength)) {
    Serial.printf("❌ OTA: Not enough space for update: %s\n",
                  Update.errorString());
    httpOTA.end();
    postOTAProgress("failed", 0, newVersion, "Not enough space for update");
    // Resume tasks on failure
    otaInProgress = false;
    if (oledTaskHandle) vTaskResume(oledTaskHandle);
    if (cameraTaskHandle) vTaskResume(cameraTaskHandle);
    if (audioTaskHandle) vTaskResume(audioTaskHandle);
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  WiFiClient *stream = httpOTA.getStreamPtr();
  size_t written = 0;
  int lastPct = -1;
  unsigned long lastDataTime = millis();
  int readErrors = 0; // Track consecutive read errors

  // Use global otaBuf[4096] instead of local buf[1024] to save stack
  while (written < (size_t)contentLength) {
    // Watchdog — abort if no data for 120s (Cloud Run can pause on scaling)
    if (millis() - lastDataTime > 120000) {
      Serial.println("❌ OTA: Download stalled (120s no data)");
      break;
    }

    // Check if data is available before blocking read (avoids wasting retries)
    int avail = stream->available();
    if (avail <= 0) {
      // No data ready yet — brief yield without counting as error
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Read available data (capped to buffer size)
    int toRead = min((int)sizeof(otaBuf), avail);
    int bytesRead = stream->read(otaBuf, toRead);
    if (bytesRead > 0) {
      Update.write(otaBuf, bytesRead);
      written += bytesRead;
      lastDataTime = millis();
      readErrors = 0; // Reset error counter on success

      int pct = (written * 100) / contentLength;
      // Update OLED every 5% — lightweight I2C only, NO network calls during flash
      if (pct != lastPct && pct % 5 == 0) {
        lastPct = pct;
        Serial.printf("   OTA: %d%% (%d/%d)\n", pct, written, contentLength);

        // OLED progress bar only (no postOTAProgress — SSL blocks download stream)
        display.clearDisplay();
        display.setCursor(2, 4);
        display.println("FLASHING");
        display.setCursor(2, 16);
        display.printf("%d%%", pct);
        // Draw progress bar
        display.drawRect(2, 26, 60, 4, SSD1306_WHITE);
        display.fillRect(2, 26, (60 * pct) / 100, 4, SSD1306_WHITE);
        display.display();
      }
    } else if (bytesRead < 0) {
      // TLS read returned -1: transient error
      readErrors++;
      if (readErrors > 150) { // 150 × 200ms = 30s tolerance for Cloud Run pauses
        Serial.printf("❌ OTA: Stream read error (%d consecutive failures at %d%%)\n",
                      readErrors, (int)((written * 100) / contentLength));
        break;
      }
      if (readErrors % 25 == 0) {
        Serial.printf("⚠️ OTA: read()=-1, retry %d/150 (%d%% done, heap=%u)\n",
                      readErrors, (int)((written * 100) / contentLength),
                      ESP.getFreeHeap());
      }
      // Exponential backoff: 100ms → 200ms → 400ms (capped)
      int backoff = min(400, 100 * (1 + readErrors / 25));
      vTaskDelay(pdMS_TO_TICKS(backoff));
    } else {
      // bytesRead == 0: no data yet, yield and retry
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  httpOTA.end();

  if (written != (size_t)contentLength) {
    Serial.printf("❌ OTA: Download incomplete (%d/%d)\n", written,
                  contentLength);
    Update.abort();
    // Report failure to server
    postOTAProgress("failed", (written * 100) / contentLength, newVersion,
                    "Download incomplete — aborted");
    // Resume tasks on failure
    otaInProgress = false;
    if (oledTaskHandle)
      vTaskResume(oledTaskHandle);
    if (cameraTaskHandle)
      vTaskResume(cameraTaskHandle);
    if (audioTaskHandle)
      vTaskResume(audioTaskHandle);
    Serial.println("▶️  Tasks resumed after OTA failure");
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  if (!Update.end(true)) {
    Serial.printf("❌ OTA: Flash failed: %s\n", Update.errorString());
    // Report failure to server
    postOTAProgress("failed", 95, newVersion,
                    (String("Flash failed: ") + Update.errorString()).c_str());
    // Resume tasks on failure
    otaInProgress = false;
    if (oledTaskHandle)
      vTaskResume(oledTaskHandle);
    if (cameraTaskHandle)
      vTaskResume(cameraTaskHandle);
    if (audioTaskHandle)
      vTaskResume(audioTaskHandle);
    Serial.println("▶️  Tasks resumed after OTA failure");
    otaUpdateRequested = false;
    setCpuFrequencyMhz(80);
    return;
  }

  Serial.printf("✅ OTA: Firmware v%s flashed successfully! Rebooting...\n",
                newVersion.c_str());

  // sslOTA is now free (httpOTA.end() already called above) — report flash
  // success BEFORE reboot Server will auto-mark "done" when startup-complete
  // arrives, but also send "rebooting" as backup
  postOTAProgress("rebooting", 100, newVersion,
                  ("v" + newVersion + " flashed! Rebooting...").c_str());

  // Show success on OLED
  display.clearDisplay();
  display.setCursor(2, 4);
  display.println("OTA DONE!");
  display.setCursor(2, 16);
  display.print("v");
  display.println(newVersion);
  display.setCursor(2, 26);
  display.println("Rebooting..");
  display.display();
  delay(2000);

  ESP.restart();
}

// ================= STARTUP COMPLETE NOTIFICATION =================
void notifyServerStartupComplete() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi not connected, cannot notify server");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(2000); // Reduced from 5s
  http.setTimeout(2000);        // Reduced from 5s

  const char *startupUrl =
      "https://kakuproject-90943350924.asia-south1.run.app/api/device/"
      "startup-complete";

  if (!http.begin(sslNet, String(startupUrl))) {
    Serial.println("❌ Failed to connect for startup notification");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  // FIX: StaticJsonDocument — DynamicJsonDocument causes heap fragmentation at
  // startup
  StaticJsonDocument<256> doc;
  doc["device_id"] = "ESP32_001";
  doc["status"] = "startup_complete";
  doc["timestamp"] = millis();
  doc["pet_stage"] = petAge; // Send current pet stage

  String payload;
  serializeJson(doc, payload);

  Serial.printf("📤 Notifying server: Startup complete (payload: %d bytes)\n",
                payload.length());

  int httpCode = http.POST(payload);
  trackHttpResult(httpCode);

  if (httpCode == 200) {
    String response = http.getString();
    Serial.println("✅ Server acknowledged startup!");
    Serial.printf("   Response: %s\n", response.c_str());

    // Parse response - server might send initial state
    StaticJsonDocument<768> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);

    if (!error) {
      if (responseDoc.containsKey("animation_id")) {
        int animId = responseDoc["animation_id"].as<int>();
        petAge = (PetAge)animId;
        Serial.printf("   ✅ Initial animation set to: %d (INFANT)\n", animId);
      }
      if (responseDoc.containsKey("show_home_icon")) {
        showHomeIcon = responseDoc["show_home_icon"].as<bool>();
        Serial.printf("   Home icon: %s\n",
                      showHomeIcon ? "ENABLED" : "DISABLED");
      }

      // DISABLED: Server hunger state (controlled locally by feeding gesture)
      // if (responseDoc.containsKey("show_food_icon")) {
      //     // Ignore during startup or if just fed
      //     if (justFedPet && (millis() - lastFeedTime < FEED_IGNORE_DURATION))
      //     {
      //         Serial.println("🍽️  Ignoring startup food icon (just fed)");
      //     } else {
      //         showFoodIcon = responseDoc["show_food_icon"].as<bool>();
      //         Serial.printf("   Food icon: %s\n", showFoodIcon ? "ENABLED" :
      //         "DISABLED");
      //     }
      // }
    } else {
      Serial.printf("⚠️  JSON parse error: %s\n", error.c_str());
    }

    // Lock sync to INFANT for first 3 seconds after startup
    vTaskDelay(pdMS_TO_TICKS(3000));
    lastDisplayCheckTime =
        millis(); // Reset sync timer to prevent early override
    Serial.println("🔒 Startup complete - INFANT locked for 3 seconds");
  } else {
    Serial.printf("⚠️  Server response: %d\n", httpCode);
  }

  http.end();
}

// ================= CAMERA FUNCTIONS =================
String captureImageBase64() {
  Serial.println("📸 Capturing image...");

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("❌ Camera capture failed");
    return "";
  }

  // Convert to base64
  String imageB64 = base64::encode(fb->buf, fb->len);

  Serial.printf("✅ Image captured: %d bytes → %d chars base64\n", fb->len,
                imageB64.length());

  esp_camera_fb_return(fb);
  return imageB64;
}

// ================= AUDIO FUNCTIONS =================
// Note: Audio recording is now handled by audioMonitorTask on Core 0
// This function is kept for compatibility but not used in dual-core mode

String recordAudioBase64() {
  Serial.println(
      "⚠️  recordAudioBase64() called, but using VAD on Core 0 instead");
  return ""; // Return empty - audio handled by voice detection
}

// ================= UNIFIED DATA TRANSMISSION =================
bool sendSensorDataOnly(SensorData data) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("❌ WiFi not connected, skipping send\n");
    return false;
  }

  HTTPClient http;
  http.setReuse(true); // Reuse TCP/TLS connection
  http.setConnectTimeout(2000);
  http.setTimeout(5000);

  if (!http.begin(sslNet, String(serverUrl))) {
    Serial.println("❌ Failed to begin HTTP connection");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  // Reuse global StaticJsonDocument (no stack allocation each call)
  g_sensorDoc.clear();

  g_sensorDoc["accel_x"] = data.accel_x;
  g_sensorDoc["accel_y"] = data.accel_y;
  g_sensorDoc["accel_z"] = data.accel_z;
  g_sensorDoc["gyro_x"] = data.gyro_x;
  g_sensorDoc["gyro_y"] = data.gyro_y;
  g_sensorDoc["gyro_z"] = data.gyro_z;
  g_sensorDoc["mic_level"] = data.mic_level;
  g_sensorDoc["sound_data"] = data.sound_data;
  g_sensorDoc["chip_temperature"] =
      data.chip_temperature; // ESP32 internal temperature
  g_sensorDoc["sleep_seconds"] =
      accumulatedSleepSec; // Seconds slept since last upload
  accumulatedSleepSec = 0; // Reset counter after reporting
  g_sensorDoc["step_count"] =
      hwStepCount; // Steps counted on hardware since last send
  hwStepCount = 0; // Reset after reporting

  // Add pet local physiology state so the server dashboard updates
  JsonObject petObj = g_sensorDoc.createNestedObject("pet_state");
  petObj["hunger"] = g_petState.hunger;
  petObj["health"] = g_petState.health;
  petObj["happiness"] = g_petState.happiness;
  petObj["discipline"] = g_petState.discipline;
  petObj["energy"] = g_petState.energy;
  petObj["level"] = g_petState.level;
  petObj["xp"] = g_petState.xp;
  petObj["is_sick"] = g_petState.isSick;
  petObj["has_poop"] = g_petState.hasPoop;
  petObj["age"] = g_petState.ageInt;
  petObj["uptime"] = g_petState.totalUptimeSecs;

  // Add sensor batch with all buffered readings
  JsonObject batchObj = g_sensorDoc.createNestedObject("sensor_batch");
  batchObj["reading_count"] = data.sensor_batch.reading_count;
  batchObj["avg_mic_level"] = data.sensor_batch.avg_mic_level;
  batchObj["sound_data"] = data.sensor_batch.sound_data;

  // Serialize all readings in the batch
  JsonArray readingsArray = batchObj.createNestedArray("readings");
  for (int i = 0; i < data.sensor_batch.reading_count; i++) {
    JsonObject readingObj = readingsArray.createNestedObject();
    readingObj["timestamp_ms"] = data.sensor_batch.readings[i].timestamp_ms;
    readingObj["accel_x"] = data.sensor_batch.readings[i].accel_x;
    readingObj["accel_y"] = data.sensor_batch.readings[i].accel_y;
    readingObj["accel_z"] = data.sensor_batch.readings[i].accel_z;
    readingObj["gyro_x"] = data.sensor_batch.readings[i].gyro_x;
    readingObj["gyro_y"] = data.sensor_batch.readings[i].gyro_y;
    readingObj["gyro_z"] = data.sensor_batch.readings[i].gyro_z;
  }

  String payload;
  serializeJson(g_sensorDoc, payload);

  Serial.printf("📊 Sending sensor data: %d bytes\n", payload.length());

  int httpCode = http.POST(payload);
  trackHttpResult(httpCode);

  if (httpCode == 200) {
    Serial.printf("    Accel: X=%.2f, Y=%.2f, Z=%.2f m/s²\n", data.accel_x,
                  data.accel_y, data.accel_z);
    Serial.printf("    Gyro:  X=%.2f, Y=%.2f, Z=%.2f °/s\n", data.gyro_x,
                  data.gyro_y, data.gyro_z);
    Serial.printf("    Chip Temp: %.1f °C\n", data.chip_temperature);
    Serial.printf("    Orient: %s (%.1f%% confidence)\n",
                  data.device_orientation.c_str(), data.orientation_confidence);
    digitalWrite(LED_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(LED_PIN, LOW);
  } else {
    Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return (httpCode == 200);
}

void sendImageData(String imageBase64) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi not connected");
    isUploadingImage = false;    // Reset upload flag
    capturingForFeeding = false; // Reset feeding flag
    return;
  }

  isUploadingImage =
      true; // Ensure flag is set (may already be set from gesture)

  // Get binary data from Core 0 with mutex protection
  uint8_t *binary_data = NULL;
  size_t data_length = 0;

  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (capturedImageBuffer != NULL && capturedImageLength > 0) {
      // Copy data for sending
      binary_data = (uint8_t *)malloc(capturedImageLength);
      if (binary_data) {
        memcpy(binary_data, capturedImageBuffer, capturedImageLength);
        data_length = capturedImageLength;
      }

      // Free Core 0 buffer
      free(capturedImageBuffer);
      capturedImageBuffer = NULL;
      capturedImageLength = 0;
      cameraImageReady = false;
    }
    xSemaphoreGive(cameraMutex);
  }

  if (!binary_data || data_length == 0) {
    Serial.println("⚠️ No image data to send");
    isUploadingImage = false;
    capturingForFeeding = false; // Reset feeding flag
    return;
  }

  Serial.printf("🖼️ Sending image: %d bytes (raw binary from Core 0)\n",
                data_length);

  // NOTE: binary_data allocated with malloc() above — acceptable for network
  // send buffer The source (capturedImageBuffer) correctly uses ps_malloc() in
  // cameraMonitorTask
  // Use global sslNet instead of local WiFiClientSecure to avoid stack overflow
  // on 16KB network task (WiFiClientSecure uses ~10KB on ESP32)
  sslNet.stop(); // Reset connection state before reuse

  HTTPClient http;
  http.setTimeout(10000);       // Reduced from 30s
  http.setConnectTimeout(5000); // Reduced from 10s

  if (!http.begin(
          sslNet,
          "https://kakuproject-90943350924.asia-south1.run.app/upload")) {
    Serial.println("❌ HTTP begin failed");
    free(binary_data);
    isUploadingImage = false;
    capturingForFeeding = false; // Reset feeding flag
    return;
  }

  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Feeding-Action",
                 "true"); // Tell server this is a feeding action

  int httpCode = http.sendRequest("POST", binary_data, data_length);
  trackHttpResult(httpCode);

  if (httpCode == 200) {
    Serial.println("✅ Image uploaded successfully");

    // Reset feeding flag
    capturingForFeeding = false;

    // Reset hunger indicators locally (server will sync)
    showFoodIcon = false;
    petIsHungry = false;
    justFedPet = true; // Ignore server updates for 10 seconds
    lastFeedTime = millis();
    Serial.println(
        "🍽️ Feeding complete - hunger reset locally (ignoring server for 10s)");

    // Trigger eating-finished state to show GOOD! text
    // Image upload = feeding complete (the frame IS the food)
    if (currentScreenType == "FOOD_MENU") {
      justFinishedEating = true;
      eatingFinishTime = millis();
      Serial.println("🎉 Feeding complete! Showing GOOD! text...");
    }
  } else {
    Serial.printf("❌ Upload failed: %d (%s)\n", httpCode,
                  http.errorToString(httpCode).c_str());

    // Reset feeding flag even on failure
    capturingForFeeding = false;
  }

  http.end();
  free(binary_data);

  isUploadingImage = false;
}

void sendAudioData(String audioBase64) {
  if (WiFi.status() != WL_CONNECTED)
    return;

  if (audioBase64.length() == 0) {
    Serial.println("⚠️  Audio data empty, skipping");
    return;
  }

  Serial.printf("🎵 Sending audio: %d bytes base64\n", audioBase64.length());

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(15000); // Shorter timeout for smaller audio

  if (!http.begin(
          "https://kakuproject-90943350924.asia-south1.run.app/upload-audio")) {
    Serial.println("❌ Failed to connect to audio server");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  // Build JSON manually to avoid buffer overflow
  String payload = "{\"audio_data\":\"";
  payload += audioBase64;
  payload += "\"}";

  Serial.printf("📨 Payload size: %d bytes\n", payload.length());

  int httpCode = http.POST(payload);
  trackHttpResult(httpCode);

  if (httpCode == 200) {
    Serial.println("✅ Audio data sent!");
  } else {
    Serial.printf("❌ Audio send failed: %d (%s)\n", httpCode,
                  http.errorToString(httpCode).c_str());
  }

  http.end();
  vTaskDelay(pdMS_TO_TICKS(
      200)); // Server breathing room after audio upload (non-blocking)
}

void sendAllDataToServer(SensorData data) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("❌ WiFi not connected (status=%d), skipping data send\n",
                  WiFi.status());
    return;
  }

  Serial.printf("🌐 Connecting to server: %s\n", serverUrl);

  HTTPClient http;
  http.setConnectTimeout(10000); // 10 second connection timeout
  http.setTimeout(15000);        // 15 second read timeout (for large payloads)

  if (!http.begin(sslNet, String(serverUrl))) {
    Serial.println("❌ Failed to begin HTTP connection");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  // FIX: 8KB StaticJsonDocument on stack risks stack overflow on 16KB task
  // This function is rarely used (sendSensorDataOnly is primary) — keep but
  // warn If sending image/audio, use dedicated sendImageData/sendAudioData
  // instead
  StaticJsonDocument<4096>
      jsonDoc; // Reduced: image/audio sent via dedicated endpoints

  // Basic sensor data
  jsonDoc["accel_x"] = data.accel_x;
  jsonDoc["accel_y"] = data.accel_y;
  jsonDoc["accel_z"] = data.accel_z;
  jsonDoc["gyro_x"] = data.gyro_x;
  jsonDoc["gyro_y"] = data.gyro_y;
  jsonDoc["gyro_z"] = data.gyro_z;
  jsonDoc["mic_level"] = data.mic_level;
  jsonDoc["sound_data"] = data.sound_data;

  // Metadata
  jsonDoc["timestamp"] = millis();
  jsonDoc["device_id"] = "esp32_xiao_s3";

  // Add image data if available
  if (data.has_new_image && !data.camera_image_b64.isEmpty()) {
    jsonDoc["camera_image"] = data.camera_image_b64;
    Serial.println("📸 Including image data in payload");
  }

  // Add audio data if available
  if (data.has_new_audio && !data.audio_data_b64.isEmpty()) {
    jsonDoc["audio_data"] = data.audio_data_b64;
    Serial.println("🎵 Including audio data in payload");
  }

  // Add local pet state (Primary Authority)
  JsonObject pet = jsonDoc.createNestedObject("pet_state");
  pet["hunger"] = g_petState.hunger;
  pet["health"] = g_petState.health;
  pet["energy"] = g_petState.energy;
  pet["happiness"] = g_petState.happiness;
  pet["discipline"] = g_petState.discipline;
  pet["xp"] = g_petState.xp;
  pet["level"] = g_petState.level;
  pet["age"] = g_petState.ageInt;
  pet["is_sick"] = g_petState.isSick;
  pet["has_poop"] = g_petState.hasPoop;
  pet["uptime"] = g_petState.totalUptimeSecs;

  String payload;
  serializeJson(jsonDoc, payload);

  Serial.printf("\n📊 Sending data: %d bytes\n", payload.length());
  Serial.printf("    Sensors: ✅ | Image: %s | Audio: %s\n",
                data.has_new_image ? "✅" : "⬜",
                data.has_new_audio ? "✅" : "⬜");
  Serial.println("⏳ Waiting for server response...");

  int httpCode = http.POST(payload);
  trackHttpResult(httpCode);

  if (httpCode > 0) {
    Serial.printf("📤 POST Response: %d\n", httpCode);
    if (httpCode == 200) {
      Serial.println("✅ All data sent successfully!");
      Serial.printf("    Accel: X=%.2f, Y=%.2f, Z=%.2f m/s²\n", data.accel_x,
                    data.accel_y, data.accel_z);
      Serial.printf("    Gyro:  X=%.2f, Y=%.2f, Z=%.2f °/s\n", data.gyro_x,
                    data.gyro_y, data.gyro_z);
      Serial.printf("    Mic:   %.1f dB\n", data.mic_level);

      // Success LED blink
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(LED_PIN, LOW);
    } else {
      Serial.printf("❌ Server error: %s\n", http.getString().c_str());
    }
  } else {
    Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

// WAV Header generation function
void generate_wav_header(uint8_t *wav_header, uint32_t wav_size,
                         uint32_t sample_rate) {
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = sample_rate * SAMPLE_BITS / 8;

  const uint8_t header[] = {
      'R',
      'I',
      'F',
      'F',
      file_size,
      file_size >> 8,
      file_size >> 16,
      file_size >> 24,
      'W',
      'A',
      'V',
      'E',
      'f',
      'm',
      't',
      ' ',
      0x10,
      0x00,
      0x00,
      0x00,
      0x01,
      0x00,
      0x01,
      0x00,
      sample_rate,
      sample_rate >> 8,
      sample_rate >> 16,
      sample_rate >> 24,
      byte_rate,
      byte_rate >> 8,
      byte_rate >> 16,
      byte_rate >> 24,
      0x02,
      0x00,
      0x10,
      0x00,
      'd',
      'a',
      't',
      'a',
      wav_size,
      wav_size >> 8,
      wav_size >> 16,
      wav_size >> 24,
  };

  memcpy(wav_header, header, sizeof(header));
}

// ================= EVENT POLLING FUNCTIONS =================
/******** pollForEvents removed - bundled with OLED poll ********/

// Send cleaning request to server (remove poop)
void sendCleanRequest() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected, cannot send cleaning request");
    return;
  }

  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(5000);

  String cleanUrl =
      "https://kakuproject-90943350924.asia-south1.run.app/api/pet/clean";
  Serial.println("🧹 Sending cleaning request to server...");

  if (!http.begin(sslNet, cleanUrl)) {
    Serial.println("❌ Failed to initialize HTTP client for cleaning");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  // Empty POST body
  int httpCode = http.POST("{}");
  trackHttpResult(httpCode);

  if (httpCode > 0) {
    Serial.printf("📡 Server response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.println("✅ Cleaning successful!");
      Serial.println(response);

      // Reset local poop icon
      showPoopIcon = false;
      Serial.println("💩 Poop icon cleared locally");
    } else {
      Serial.printf("⚠️ Unexpected response code: %d\n", httpCode);
    }
  } else {
    Serial.printf("❌ Cleaning request failed: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ================= SEND INJECTION REQUEST =================
// Called after injection animation completes — clears sick_pending on server,
// resumes hunger
// ================= SEND COVER HAPPY REQUEST =================
// Called each time right-tilt gesture cycles the menu — happiness +5 on server
void sendCoverHappyRequest() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(5000);

  String url =
      "https://kakuproject-90943350924.asia-south1.run.app/api/pet/cover-happy";
  if (!http.begin(sslNet, url))
    return;

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST("{}");
  trackHttpResult(httpCode);

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("\U0001f600 Cover detected → happiness +5 sent to server");
  } else {
    Serial.printf("⚠️ cover-happy response: %d\n", httpCode);
  }
  http.end();
}

void sendInjectRequest() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected, cannot send injection request");
    return;
  }

  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(5000);

  String injectUrl =
      "https://kakuproject-90943350924.asia-south1.run.app/api/pet/inject";
  Serial.println("💉 Sending injection request to server...");

  if (!http.begin(sslNet, injectUrl)) {
    Serial.println("❌ Failed to initialize HTTP client for injection");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST("{}");
  trackHttpResult(httpCode);

  if (httpCode > 0) {
    Serial.printf("📡 Injection server response: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      Serial.println("✅ Server injection acknowledged — sick_pending cleared, "
                     "hunger resumes");
    } else {
      Serial.printf("⚠️ Injection unexpected response: %d\n", httpCode);
    }
  } else {
    Serial.printf("❌ Injection request failed: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
}

void processEvent(const char *event_type, const char *message) {
  if (!event_type || !message) {
    Serial.println("⚠️ processEvent: null event_type or message, skipping");
    return;
  }
  Serial.println("🔧 Processing event...");

  // Simple event processing - you can extend this based on your needs
  if (strcmp(event_type, "high_sound") == 0) {
    Serial.println("🔊 High sound detected - might want to take action!");
    digitalWrite(LED_PIN, HIGH); // Turn on LED for high sound
    vTaskDelay(pdMS_TO_TICKS(200));
    digitalWrite(LED_PIN, LOW); // Blink LED
  } else if (strcmp(event_type, "sudden_motion") == 0) {
    Serial.println("🏃 Sudden motion detected - something's happening!");
    // Blink LED multiple times for motion
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  } else if (strcmp(event_type, "alert") == 0) {
    Serial.println("⚠️ Generic alert received!");
    digitalWrite(LED_PIN, HIGH); // Solid LED for alert
    vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(LED_PIN, LOW);
  } else {
    Serial.printf("❓ Unknown event type: %s\n", event_type);
  }

  Serial.printf("   📝 Event message: %s\n", message);
}

void acknowledgeEvent(int event_id) {
  HTTPClient http;
  http.setReuse(true);

  Serial.printf("📤 Acknowledging event #%d...\n", event_id);

  if (!http.begin(sslNet, String(eventReceivedUrl))) {
    Serial.println("❌ Failed to initialize HTTP client for acknowledgment");
    return;
  }

  // Set timeout and headers
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-Dashboard/2.0");

  // FIX: StaticJsonDocument prevents heap fragmentation (called per event)
  StaticJsonDocument<256> doc;
  doc["device_id"] = "ESP32_001";
  doc["event_id"] = event_id;
  doc["status"] = "received";
  doc["timestamp"] = millis(); // Simple timestamp

  String jsonString;
  serializeJson(doc, jsonString);

  // Send POST request
  int httpCode = http.POST(jsonString);
  trackHttpResult(httpCode);

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK || httpCode == 200) {
      String response = http.getString();
      Serial.printf("✅ Event acknowledged successfully: %s\n",
                    response.c_str());
    } else {
      Serial.printf("⚠️ Acknowledgment response: %d\n", httpCode);
    }
  } else {
    Serial.printf("❌ Acknowledgment failed: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
}

/*
WIRING DIAGRAM for XIAO ESP32 S3 Sense:
===========================================

🔌 Built-in Components (No Wiring Needed):
- Camera Module (OV2640) - Built into XIAO ESP32 S3 Sense
- PDM Microphone - Built into XIAO ESP32 S3 Sense
- PSRAM - Built into XIAO ESP32 S3 Sense

🔧 External Components to Connect:

MPU6050 (Accelerometer/Gyroscope):
- VCC -> 3.3V
- GND -> GND
- SDA -> GPIO 21 (I2C Data)
- SCL -> GPIO 22 (I2C Clock)
- INT -> Not connected (optional)

LED Indicator:
- + -> GPIO 2 (with 220Ω resistor)
- - -> GND

📋 PIN ASSIGNMENTS (XIAO ESP32 S3 Sense):
==========================================

Camera Pins (Built-in OV2640):
- XCLK  -> GPIO 10
- SIOD  -> GPIO 40 (I2C SDA)
- SIOC  -> GPIO 39 (I2C SCL)
- Y9    -> GPIO 48
- Y8    -> GPIO 11
- Y7    -> GPIO 12
- Y6    -> GPIO 14
- Y5    -> GPIO 16
- Y4    -> GPIO 18
- Y3    -> GPIO 17
- Y2    -> GPIO 15
- VSYNC -> GPIO 38
- HREF  -> GPIO 47
- PCLK  -> GPIO 13

I2S PDM Microphone (Built-in):
- CLK -> GPIO 42 (I2S WS)
- DATA-> GPIO 41 (I2S SD)

MPU6050 (External):
- SDA -> GPIO 5  (XIAO ESP32 S3)
- SCL -> GPIO 6  (XIAO ESP32 S3)

Alternative I2C pins if GPIO 6/7 don't work:
- SDA -> GPIO 21, SCL -> GPIO 22 (Generic ESP32)
- Check your specific board's pinout diagram

Other:
- LED -> GPIO 2

⚠️  IMPORTANT NOTES:
===================
1. XIAO ESP32 S3 Sense has BUILT-IN camera and microphone
2. No external camera/mic wiring needed
3. Only connect MPU6050 externally via I2C
4. Camera uses JPEG compression for efficient transmission
5. 🎤 SMART AUDIO: Voice Activity Detection with dual-core processing
6. All data sent to dashboard via WiFi

🚀 FEATURES:
============
- ✅ MPU6050 sensor data (every 1 second) - Core 1
- ✅ Camera images (every 7 seconds) - Core 1
- ✅ 🎤 SMART AUDIO: Voice Activity Detection - Core 0
- ✅ Dual-core processing for optimal performance
- ✅ Real-time dashboard updates
- ✅ WiFi connectivity with auto-reconnect
- ✅ LED status indicators
- ✅ PSRAM for audio/image buffering

🧠 DUAL-CORE ARCHITECTURE:
==========================
**Core 0 (Audio Core):**
- Continuous microphone monitoring
- Voice Activity Detection (VAD)
- Energy-based speech detection
- Automatic audio recording when speech detected
- Real-time audio processing (high priority)

**Core 1 (Main Core):**
- WiFi management and HTTP transmission
- Sensor data collection (MPU6050)
- Camera image capture
- Dashboard communications
- LED status indicators

🎤 INTELLIGENT AUDIO SYSTEM:
============================
**Voice Activity Detection:**
- Continuously monitors audio energy levels
- Detects speech when energy > 1000 threshold
- Requires minimum 500ms of continuous speech
- Records up to 5 seconds of audio
- Stops recording after 2 seconds of silence
- Only transmits audio when speech is detected

**Energy-Based Detection:**
```
Audio Energy > 1000    → Speech Detected 🗣️
Audio Energy < 1000    → Silent 🔇
Continuous Speech 500ms+ → Start Recording 🎙️
Silence 2000ms+ → Stop Recording & Send 📤
```

**Benefits:**
- ⚡ No bandwidth wasted on silent audio
- 🔋 Power efficient - only records when needed
- 📡 Real-time speech detection and transmission
- 🎯 High accuracy voice activity detection
- 🚀 Multi-core performance optimization

📊 DATA TRANSMISSION SCHEDULE:
==============================
**Real-time (Core 1):**
- Sensor readings: Every 1 second (always)
- Camera images: Every 7 seconds

**Event-driven (Core 0 → Core 1):**
- Audio: Only when speech detected
- Voice detection: Continuous monitoring
- Inter-core communication via mutex/semaphores

🔧 SMART THRESHOLDS:
===================
- VAD_THRESHOLD: 1000 (adjust based on environment)
- VAD_MIN_DURATION: 500ms (minimum speech length)
- SILENCE_TIMEOUT: 2000ms (stop recording delay)
- MAX_RECORDING: 5 seconds (prevent buffer overflow)

🎛️ TUNING VOICE DETECTION:
===========================
**Quiet Environment:** Lower VAD_THRESHOLD to 500-800
**Noisy Environment:** Raise VAD_THRESHOLD to 1500-2000
**Sensitive Detection:** Decrease VAD_MIN_DURATION to 300ms
**Less False Triggers:** Increase VAD_MIN_DURATION to 800ms

💾 MEMORY USAGE:
===============
- Images: ~1-3KB (QQVGA 160x120, quality 20)
- Audio: ~32KB per 1-second recording (only when speech)
- VAD Buffer: 1KB for real-time energy analysis
- PSRAM: Dynamic allocation for recordings
- Automatic cleanup after transmission

🌐 NETWORK ENDPOINTS:
====================
- Sensors: POST /api/sensor-data (small, frequent)
- Images: POST /upload (binary, every 7s)
- Audio: POST /upload-audio (JSON, speech-triggered)

⚡ PERFORMANCE BENEFITS:
========================
1. **Dual-Core**: Audio processing doesn't block main operations
2. **Event-Driven**: Audio only sent when speech detected
3. **Energy-Efficient**: No continuous audio transmission
4. **Real-Time**: Voice detection with minimal latency
5. **Intelligent**: Automatic silence detection and recording stop

🔧 CONFIGURATION:
=================
Update these before uploading:
1. WIFI_SSID and WIFI_PASSWORD
2. Server URL: "https://kakuproject-90943350924.asia-south1.run.app"
3. Adjust VAD_THRESHOLD for your environment
4. Three separate optimized endpoints for different data types

**Data Sending Examples:**
- Sensor readings: Every 1 second (always) - 146 bytes JSON
- Camera images: Every 7 seconds - 1-3KB binary
- Audio recordings: Only when you speak - 32KB+ base64
- Silence periods: 0 bytes audio transmission ✨
*/
