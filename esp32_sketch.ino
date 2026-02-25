/*
ESP32 Tamagotchi Client - Arduino C++ (XIAO ESP32 S3 Sense)

📡 ACTIVE FEATURES:
  - MPU6050: accelerometer/gyro (tilt gestures + game control)
  - Camera: on-demand only (food feeding gesture triggers capture)
  - OLED 64x32: pet animations, menus, icons
  - WiFi: sensor upload, pet state sync, image upload

🎮 GESTURE CONTROLS:
  - Right tilt + hold 2s → cycle menu (MAIN→FOOD→TOILET→PLAY→HEALTH→STATUS→STATS→MAIN)
  - Left tilt + hold 3s  → context action (feed / clean / medicine depending on menu)

⚡ CPU FREQUENCY:
  - Idle:          40 MHz
  - HTTP/JSON:    160 MHz
  - Camera capture: 240 MHz

📶 NETWORK SCHEDULE (staggered, no overlap):
  - SLOT A: Sensor batch   → every 2s
  - SLOT B: OLED + Events  → every 5s (combined single slot)
  - On demand: Image upload, clean, inject, happy

Required Libraries:
- ArduinoJson, WiFi, HTTPClient, I2Cdev, MPU6050, esp_camera
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HttpsOTAUpdate.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "MPU6050.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "driver/i2s_pdm.h"
#include "base64.h"

// ================= OLED & PET ANIMATIONS =================
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "all_pets.h"

// ================= FIRMWARE VERSION (increment before each upload) =================
#define FIRMWARE_VERSION "1.0.0"

// ================= WIFI =================
#define WIFI_SSID     "123"
#define WIFI_PASSWORD "KUNAL 26"

// ================= API =================
const char* serverUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/sensor-data";  // Google Cloud Run Production
const char* eventsUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/events?device_id=ESP32_001";  // Events endpoint
const char* eventReceivedUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/device/event/received";  // Event acknowledgment
const char* oledDisplayUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/oled-display/get";  // OLED display animation endpoint
const char* firmwareCheckUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/firmware/latest?device_id=ESP32_001&current_version=" FIRMWARE_VERSION;
const char* OTA_AUTH_TOKEN = "kaku-ota-2025";  // Must match server token
// NOTE: Orientation endpoint removed - server computes direction from sensor data

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
#define SAMPLE_RATE     16000
#define SAMPLE_BITS     16
#define RECORD_SECONDS  1     // 1 second of audio (4x smaller payload)
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN     2
#define AUDIO_DATA_SIZE (SAMPLE_RATE * 2 * RECORD_SECONDS)

// ================= I2S PDM MIC PINS (XIAO ESP32 S3 Sense) =================
#define PDM_CLK_GPIO (gpio_num_t)42  // PDM CLK
#define PDM_DIN_GPIO (gpio_num_t)41  // PDM DATA
#define I2S_NUM I2S_NUM_0

// ================= OLED DISPLAY SETUP =================
#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 32
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= SLEEPING ANIMATION (2 frames, 64x32) =================
#define SLEEPING_FRAME_COUNT 2
#define SLEEPING_WIDTH  64
#define SLEEPING_HEIGHT 32
const uint16_t sleeping_delays[SLEEPING_FRAME_COUNT] = {700, 900};
PROGMEM const uint8_t sleeping_frames[SLEEPING_FRAME_COUNT][256] = {
  { // Frame 1
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x07,0x01,0xf0,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x1f,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0x3b,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x00,0x3b,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0x80,0x00,0x00,0x00,0x00,0x00,0x7b,0xfb,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7c,0x7c,0x80,0x00,0x00,0x00,0x00,0x00,0x7f,0xdf,0x80,0x00,0x00,
    0x00,0x00,0x00,0x3f,0xbf,0x80,0x00,0x00,0x00,0x00,0x00,0xf7,0xff,0x80,0x00,0x00,
    0x00,0x00,0x01,0xf7,0xc7,0xc0,0x00,0x00,0x00,0x00,0x03,0xf7,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,0x00,0x00,0x07,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x03,0x7f,0xff,0x00,0x00,0x00,0x00,0x00,0x07,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0xe0,0x00,0x00,0x00,
    0x00,0x00,0x00,0xc0,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x40,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x00,0x00,0x00
  },
  { // Frame 2
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00,
    0x00,0x00,0x07,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x04,0x01,0xf0,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x1f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x3b,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x00,0x3b,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0x80,0x00,0x00,0x00,0x00,0x00,0x7f,0xfb,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7c,0x7c,0x80,0x00,0x00,0x00,0x00,0x00,0x7f,0xdf,0x80,0x00,0x00,
    0x00,0x00,0x00,0x3f,0xff,0x80,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x01,0xff,0xc7,0xc0,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,0x00,0x00,0x07,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x03,0x3f,0xff,0x80,0x00,0x00,0x00,0x00,0x07,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x00,0x3f,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0xf0,0x00,0x00,0x00,
    0x00,0x00,0x00,0xc0,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x40,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x00,0x00
  }
};

// ================= WALKING ANIMATION (6 frames, 64x32 — frame 2 skipped) =================
#define WALKING_FRAME_COUNT 6
#define WALKING_WIDTH  64
#define WALKING_HEIGHT 32
const uint16_t walking_delays[WALKING_FRAME_COUNT] = {400,400,400,400,400,400};
PROGMEM const uint8_t walking_animation[WALKING_FRAME_COUNT][256] = {
  { // Frame 0
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,0xe0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x1f,0xf8,0x00,0x00,0x00,0x00,0x00,0x00,0x37,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x77,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0xf3,0x7e,0x00,0x00,0x00,
    0x00,0x00,0x00,0xef,0xfe,0x00,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x01,0xe3,0xf1,0x80,0x00,0x00,0x00,0x00,0x03,0xe3,0xf1,0x80,0x00,0x00,
    0x00,0x00,0x03,0xe3,0xf1,0x80,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x02,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x03,0xfe,0x0f,0x80,0x00,0x00,
    0x00,0x00,0x07,0xff,0x1f,0x80,0x00,0x00,0x00,0x00,0x0f,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x1f,0xff,0xff,0xa0,0x00,0x00,0x00,0x00,0x1f,0xbf,0xff,0xa0,0x00,0x00,
    0x00,0x00,0x1f,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x36,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x01,0xff,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x01,0xff,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x7f,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0xff,0xe0,0x00,0x00,0x00,0x00,0x00,0x03,0x60,0x18,0x00,0x00,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x18,0x00,0x00,0x00,
    0x00,0x00,0x02,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1e,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  },
  { // Frame 1
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0xf0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x07,0xf8,0x00,0x00,0x00,0x00,0x00,0x00,0x1b,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x3f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x7f,0xff,0x80,0x00,0x00,
    0x00,0x00,0x00,0xf0,0xf8,0x80,0x00,0x00,0x00,0x00,0x00,0xf0,0xf8,0x80,0x00,0x00,
    0x00,0x00,0x00,0xf0,0xf8,0xc0,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,0x00,0x00,0x07,0xff,0x87,0xc0,0x00,0x00,
    0x00,0x00,0x1f,0xff,0xcf,0xc0,0x00,0x00,0x00,0x00,0x1f,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x0f,0xff,0xff,0xc0,0x00,0x00,0x00,0x00,0x07,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x00,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x07,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0f,0xf0,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x30,0x00,0x00,0x00,
    0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x10,0x00,0x00,0x00,
    0x00,0x00,0x00,0x10,0x3c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  },
  { // Frame 2 (skipped at runtime)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xf8,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0f,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,0xff,0x80,0x00,0x00,
    0x00,0x00,0x00,0x1c,0x7f,0x80,0x00,0x00,0x00,0x00,0x00,0x1f,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x00,0x3c,0xfe,0x40,0x00,0x00,0x00,0x00,0x00,0x3c,0x7c,0x60,0x00,0x00,
    0x00,0x00,0x00,0x7c,0x7c,0x60,0x00,0x00,0x00,0x00,0x00,0x7c,0xfe,0x60,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0xe0,0x00,0x00,0x00,0x00,0x00,0x7f,0xff,0xe0,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0xe0,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xe0,0x00,0x00,
    0x00,0x00,0x00,0xff,0xff,0xe0,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0xe0,0x00,0x00,
    0x00,0x00,0x01,0xff,0xff,0xe0,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0xc0,0x00,0x00,0x00,0x00,0x00,0x3f,0xff,0x80,0x00,0x00,
    0x00,0x00,0x00,0x1f,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x00,0x07,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x0c,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0a,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  },
  { // Frame 3
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0f,0xf0,0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x3f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x7f,0x9e,0x00,0x00,0x00,
    0x00,0x00,0x00,0xfd,0xff,0x00,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x01,0xfe,0x7f,0x00,0x00,0x00,0x00,0x00,0x01,0xfe,0x3e,0x00,0x00,0x00,
    0x00,0x00,0x03,0xfe,0x3e,0x00,0x00,0x00,0x00,0x00,0x03,0xfe,0x73,0x00,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x03,0x7f,0xfe,0x80,0x00,0x00,
    0x00,0x00,0x01,0xff,0xfd,0x88,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0xa0,0x00,0x00,
    0x00,0x00,0x0f,0xff,0xff,0xb8,0x00,0x00,0x00,0x00,0x07,0xef,0xff,0xa0,0x00,0x00,
    0x00,0x00,0x1f,0xdf,0xff,0xa0,0x00,0x00,0x00,0x00,0x0f,0xbf,0xff,0x80,0x00,0x00,
    0x00,0x00,0x10,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x0f,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x07,0xff,0xfe,0x00,0x00,0x00,0x00,0x00,0x01,0xff,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0xff,0xf8,0x00,0x00,0x00,0x00,0x00,0x00,0x7f,0xc8,0x00,0x00,0x00,
    0x00,0x00,0x00,0xa0,0x10,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x05,0x00,0x00,0x00,
    0x00,0x00,0x01,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x06,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  },
  { // Frame 4
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x1f,0xf0,0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0xf8,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0xd7,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x01,0x7e,0xfe,0x00,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x01,0xff,0xc7,0x80,0x00,0x00,0x00,0x00,0x01,0xff,0xc7,0x80,0x00,0x00,
    0x00,0x00,0x03,0xff,0xc7,0x80,0x00,0x00,0x00,0x00,0x03,0xff,0xe6,0x00,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0x80,0x00,0x00,
    0x00,0x00,0x01,0xff,0xff,0x80,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x18,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x1a,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x0f,0xff,0xfe,0x00,0x00,0x00,
    0x00,0x00,0x03,0xff,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xf0,0x00,0x00,0x00,0x00,0x00,0x01,0x38,0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x20,0x00,0x00,0x00,
    0x00,0x00,0x01,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x70,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  },
  { // Frame 5
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0f,0xf8,0x00,0x00,0x00,0x00,0x00,0x00,0x1b,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x3f,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x7e,0x7e,0x00,0x00,0x00,
    0x00,0x00,0x00,0x7f,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0xf9,0xf9,0x80,0x00,0x00,0x00,0x00,0x01,0xf1,0xf8,0x80,0x00,0x00,
    0x00,0x00,0x01,0xf1,0xf8,0xc0,0x00,0x00,0x00,0x00,0x01,0xff,0xf9,0xc0,0x00,0x00,
    0x00,0x00,0x01,0xff,0xff,0xc0,0x00,0x00,0x00,0x00,0x01,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,0x00,0x00,0x03,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x07,0xdf,0xff,0xc0,0x00,0x00,0x00,0x00,0x0f,0xff,0xff,0xe0,0x00,0x00,
    0x00,0x00,0x0d,0xff,0xff,0xa0,0x00,0x00,0x00,0x00,0x1e,0xff,0xff,0xc0,0x00,0x00,
    0x00,0x00,0x01,0xff,0xff,0x60,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0xff,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x7f,0xfc,0x00,0x00,0x00,
    0x00,0x00,0x00,0x1f,0xf4,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x0c,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x80,0x00,0x00,0x00,0x00,0x00,0x10,0x03,0x80,0x00,0x00,
    0x00,0x00,0x00,0x30,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  }
};

// ================= OLED ANIMATION DISPLAY =================
enum PetAge { INFANT = 0, CHILD = 1, ADULT = 2, OLD = 3 };
PetAge petAge = INFANT;             // Default to INFANT - server manages aging
unsigned long lastAnimationTime = 0;
unsigned long lastDisplayCheckTime = 0;  // Track when we last checked server for OLED display state
const unsigned long DISPLAY_CHECK_INTERVAL = 5000;  // Poll server for OLED+events every 5 seconds (combined slot)
const unsigned long ANIMATION_DISPLAY_INTERVAL = 100;  // Display animation every 100ms (~10 FPS smooth)
uint8_t currentFrame = 0;
bool displayReady = false;
bool startupComplete = false;  // Track if startup egg animation is done
bool showHomeIcon = true;   // ESP32 controls: Always show on MAIN screen
bool showFoodIcon = false;  // Show food icon when pet is hungry
bool showPoopIcon = false;  // Show poop icon when poop present
bool showSickIcon = false;  // Show heart/sick icon after poop ignored >15 min (only when poop cleared)
bool showPlayIcon = false;  // Show play icon 1hr after last feed (blinks top-left, paused when sick)
String currentScreenType = "MAIN";  // Current menu — controlled locally via right-tilt gesture
String currentEmotion = "IDLE";    // Current emotion from server (IDLE, CRY, SAD, HAPPY, EATING, SURPRISE)
bool justFinishedEating = false;    // Show GOOD! text after eating animation
unsigned long eatingFinishTime = 0; // When eating finished (for GOOD! text timer)

// Pet state tracking — updated every 5s from server via NET_OLED
String currentMode = "AUTOMATIC";  // Mode from server (informational only — menu controlled locally)
bool petIsHungry = false;          // Hunger status from server (hunger > 70)
int  petAgeInt   = 0;              // Actual integer age from server (increments every 24 h)
int  petHappiness = 100;           // Happiness (0-100) from server, shown as bar
int  petDiscipline = 100;          // Discipline (0-100) from server, shown as bar

// Right-tilt hold for menu switching
bool holdingRightForMenu = false;            // Track right-tilt hold for menu cycling
unsigned long menuTiltHoldStartTime = 0;     // When right tilt started
const unsigned long MENU_TILT_HOLD_TIME = 2000;  // Hold 2 seconds to cycle menu
unsigned long lastMenuCycleTime = 0;         // Cooldown between menu cycles
const unsigned long MENU_CYCLE_COOLDOWN = 3000;  // 3 seconds cooldown

volatile bool cameraCapturing = false;  // Flag to prevent camera access conflicts
bool imageAlreadySentThisSession = false;  // Track if image sent for current food session

// ================= SLEEP MODE STATE =================
bool isDeviceSleeping = false;           // True when in sleep mode (neutral >30s)
unsigned long neutralStartTime = 0;      // When neutral state first detected
unsigned long sleepStartTime   = 0;      // When sleep mode started
uint32_t accumulatedSleepSec   = 0;      // Sleep seconds banked, sent on next sensor upload
const unsigned long NEUTRAL_SLEEP_TIMEOUT = 60000;  // 30s neutral → sleep

// Walking state — driven by hardware step counter (not server)
bool petIsWalking = false;
unsigned long lastWalkingStepTime = 0;  // millis() of last detected step
const unsigned long WALKING_WINDOW_MS = 3000;  // animate walking for 3s after last step

// ── HARDWARE STEP COUNTER ──────────────────────────────────────────────
uint32_t hwStepCount   = 0;       // Steps accumulated since last sensor send
unsigned long lastHwStepTime = 0; // millis() of last detected step (debounce)
const float   STEP_BARRIER_G2 = 0.10f;   // calibrated: 1400x above rest noise (0.00007g²), 3x below weakest step (0.296g²)
const unsigned long STEP_MIN_MS = 400;  // steps ~1000ms apart; 400ms debounce is safe
const float   LP_ALPHA_STEP = 0.85f;    // low-pass filter weight for gravity estimate

// ── OTA UPDATE STATE ───────────────────────────────────────────────────
bool otaUpdateRequested = false;       // Set true when server sends ota_update flag

// LP gravity estimate — tracks gravity at ANY tilt (initialised flat)
float stepGravX = 0.0f, stepGravY = 0.0f, stepGravZ = 1.0f;

// ================= PLAY MENU GAME STATE =================
enum GameState {
  GAME_IDLE,      // Not playing (shows static PLAY screen)
  GAME_PLAYING,   // Actively playing
  GAME_OVER_ANIM  // Game over animation with coins
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

// Health Menu Medicine Variables
bool petIsSick = false;  // Track if pet is sick (to be managed by backend in future)
bool givingMedicine = false;  // Track if medicine animation is playing
int medicineAnimLoopCount = 0;  // Count how many times animation has looped
unsigned long medicineHoldStartTime = 0;  // Track when tilt started for medicine
bool holdingLeftForMedicine = false;  // Track if holding left tilt for medicine
unsigned long medicineAnimStartTime = 0;  // Track animation start time
int currentInjectionFrame = 0;  // Current frame in injection animation

// Food Menu Feeding Gesture Variables
bool holdingLeftForFeeding = false;  // Track if holding left tilt for feeding
unsigned long feedingHoldStartTime = 0;  // Track when tilt started for feeding
bool capturingForFeeding = false;  // Flag: feeding gesture active, camera task will capture
unsigned long feedingGestureStartTime = 0;  // Track when feeding gesture was triggered
#define FEEDING_TIMEOUT 30000  // 30 second timeout for feeding gesture
bool justFedPet = false;  // Flag to ignore server hunger updates after feeding
unsigned long lastFeedTime = 0;  // Track when pet was last fed
#define FEED_IGNORE_DURATION 10000  // Ignore server hunger updates for 10 seconds after feeding

// Toilet Menu Cleaning Gesture Variables
bool holdingLeftForCleaning = false;  // Track if holding left tilt for cleaning
unsigned long cleaningHoldStartTime = 0;  // Track when tilt started for cleaning
bool isCleaningPoop = false;       // Flag for cleaning in progress
int cleaningFadeStep = 0;          // Fade animation step (0-10)
unsigned long cleaningStartTime = 0; // When cleaning animation started
// Clean slide animation vars
int cleanSlideX = 64;              // Current x position of sliding sprite (starts off right)
int cleanSlideFrame = 1;           // Alternates between 1 and 2
unsigned long cleanSlideLastStep = 0; // Last time sprite moved
bool justCleanedPet = false;  // Flag to show "Cleared" after cleaning

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
bool mpuAvailable = false;  // Track if MPU6050 is working

// Pins
const int LED_PIN = 2;         // LED indicator

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

// Camera data ready flag
volatile bool cameraImageReady = false;
uint8_t* capturedImageBuffer = NULL;
size_t capturedImageLength = 0;

// VAD Settings
#define VAD_THRESHOLD 1000        // Energy threshold for speech detection
#define VAD_MIN_DURATION 500      // Minimum 500ms of speech to trigger
#define SILENCE_TIMEOUT 2000      // 2s of silence before stopping recording

// Structure for single sensor reading with timestamp
struct SingleReading {
    unsigned long timestamp_ms;  // Millisecond timestamp
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
};

// Structure for buffered sensor data (batch of readings)
struct SensorDataBatch {
    static const int MAX_READINGS = 20;  // Store up to 20 readings
    int reading_count;
    SingleReading readings[MAX_READINGS];
    float avg_mic_level;
    int sound_data;
};

// Timing
unsigned long lastSendTime = 0;
unsigned long lastImageCapture = 0;
unsigned long lastEventPoll = 0;               // Event polling timing
unsigned long lastInternalReadTime = 0;        // Fast internal sensor reading timing
const unsigned long SEND_INTERVAL = 2000;      // Send sensor data batch every 2 seconds
const unsigned long INTERNAL_READ_INTERVAL = 100;  // Read sensor batch every 100ms internally
const unsigned long EVENT_POLL_INTERVAL = 5000; // Poll for events every 5 seconds
unsigned long dynamicEventPollInterval = 5000;  // Dynamic backoff for event polling
// Audio now triggered by speech detection, not timer

// Sensor reading buffer (batched between network sends)
SensorDataBatch sensorBatch = {};
float totalMicLevel = 0.0;
int micReadingCount = 0;

// ⏸️ PAUSE CONTROL FOR UPLOADS
bool isUploadingImage = false;                  // Flag to pause sensor data during image upload

// Camera and audio status
bool cameraReady = false;
bool micReady = false;
uint8_t* audio_buffer = NULL;

// Audio processing buffers for VAD
int16_t* vad_buffer;
const int VAD_BUFFER_SIZE = 512;

// PDM microphone handle
i2s_chan_handle_t rx_handle = NULL;

// Structure for sensor data
struct SensorData {
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    float mic_level;
    int sound_data;
    String camera_image_b64;  // Base64 encoded image
    String audio_data_b64;    // Base64 encoded audio
    bool has_new_image;
    bool has_new_audio;
    
    // Orientation tracking fields
    String device_orientation;
    float orientation_confidence;
    float calibrated_ax, calibrated_ay, calibrated_az;
    
    // ESP32 internal temperature
    float chip_temperature;  // Internal chip temperature in °C
    
    // Batch of sensor readings for better step detection
    SensorDataBatch sensor_batch;
};

// ================= NETWORK TASK QUEUE =================
// All HTTP calls dispatched through this queue to networkTask (Core 1)
// Active types: NET_SENSOR, NET_OLED, NET_IMAGE, NET_CLEAN, NET_INJECT, NET_HAPPY
// NET_EVENTS unused (events polled inside NET_OLED handler back-to-back)
enum NetReqType : uint8_t {
    NET_SENSOR = 0,  // Send sensor batch to server (every 2s)
    NET_OLED   = 1,  // Poll OLED state + events combined (every 5s)
    NET_EVENTS = 2,  // UNUSED — events now handled inside NET_OLED
    NET_IMAGE  = 3,  // Upload camera image to server (on feeding gesture)
    NET_CLEAN  = 4,  // Send cleaning request (left tilt 3s on TOILET_MENU)
    NET_INJECT = 5,  // Send medicine given (left tilt 3s on HEALTH_MENU)
    NET_HAPPY  = 6   // Right tilt menu cycle interaction → happiness +5
};

QueueHandle_t networkQueue;           // Queue: loop() → networkTask
SemaphoreHandle_t networkDataMutex;   // Protect g_pendingSensor
SensorData g_pendingSensor;           // Shared sensor data for networkTask

// ================= NETWORK TASK QUEUE =================
// Global StaticJsonDocuments — allocated once, no heap fragmentation
StaticJsonDocument<768>  g_oledDoc;   // Reused for OLED+events polling
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
void pollForEvents();
bool isServerAlive();
void scanI2CDevices();
bool initCamera();
bool initAudio();
void audioMonitorTask(void *parameter);
void cameraMonitorTask(void *parameter);
void processEvent(const char* event_type, const char* message);
void acknowledgeEvent(int event_id);
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate);
void sendAllDataToServer(SensorData data);
String recordAudioBase64();
void notifyServerStartupComplete();  // Notify server startup is complete
void getOLEDDisplayFromServer();     // GET /api/oled-display/get → update pet state vars
void drawHomeIcon();       // Draw home icon pixel-by-pixel
void drawFoodIcon();       // Draw food icon (bottom-right)
void drawPoopIcon();       // Draw poop icon (bottom-right)
void drawPlayIcon();       // Draw blinking play icon (top-left, 1hr after feed)
void drawSickIcon();       // Draw blinking sick/heart icon (bottom-right)
void playEatingAnimation(); // Play eating animation
void drawStaticFoodIcon();  // Static food icon at top-left (food menu)
void drawStaticToiletIcon(); // Static toilet icon at top-left (toilet menu)
void drawCleanSpriteFrame(int frame, int xOffset); // Clean slide sprite frame
void drawStaticPlayIcon();   // Static play icon at top-left (play menu)
void drawStaticHealthIcon(); // Static heart icon at top-left (health menu)
void playInjectionAnimation();  // Play injection/medicine animation
void sendInjectRequest();       // Notify server that injection was given
void displayFoodMenu();         // Display food menu screen
void displayToiletMenu();       // Display toilet menu screen
void displayPlayMenu();         // Display play menu screen
void displayHealthMenu();       // Display health menu screen
void displayStatusInfoMenu();   // Display status info menu (smiley + age + flash + score)
void displayStatsMenu();        // Display stats menu (happiness + discipline bars)
void checkMenuTiltGesture();    // Right tilt 2s hold → cycle menu
bool isDeviceNeutral();          // True when device lies flat (no significant X/Y tilt)
void detectHardwareStep();       // Count steps via MPU6050 stoss method
void displaySleepingAnimation(); // 2-frame sleeping animation
void displayWalkingAnimation();  // 6-frame walking animation (skips frame 2)
void cycleMenu();               // Cycle menus: MAIN → FOOD → TOILET → PLAY → HEALTH → STATUS → STATS → MAIN
void oledTask(void *parameter); // OLED animation task on Core 0
void networkTask(void *parameter);  // Dedicated HTTP task on Core 1 (queue-driven)\nvoid checkAndPerformOTA();           // Check firmware server and perform OTA if newer version exists

// ================= OLED ANIMATION TASK (Core 0) =================
// Independent FreeRTOS task runs OLED animation on Core 0
// Decoupled from WiFi/HTTP calls on Core 1 for smooth 60 FPS display
void oledTask(void *parameter) {
    Serial.println("🎬 OLED Task started on Core 0");
    
    while (true) {
        if (displayReady && startupComplete) {
            displayPetAnimation();  // Draw animation (non-blocking)
        }
        vTaskDelay(pdMS_TO_TICKS(60));  // ~16 FPS refresh rate
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
    
    // Connect to WiFi with timeout protection
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting WiFi");

    // FIX: Add 30-second timeout to prevent infinite loop
    unsigned long wifiStartTime = millis();
    const unsigned long WIFI_TIMEOUT = 30000;  // 30 seconds
    
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartTime < WIFI_TIMEOUT)) {
        Serial.print(".");
        delay(300);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n⚠️  WiFi connection timeout! Continuing without WiFi...");
        Serial.println("⚠️  Server communication disabled - running in offline mode");
    }
    
    // Enable WiFi modem sleep for power savings
    WiFi.setSleep(true);
    Serial.println("💤 WiFi sleep mode enabled");
    vTaskDelay(pdMS_TO_TICKS(20));  // 20ms settle after WiFi sleep config
    
    // Dynamic CPU: idle at 40MHz (boosts to 160MHz only during HTTP/JSON)
    setCpuFrequencyMhz(40);
    Serial.println("⚡ CPU idle at 40MHz (boosts to 160MHz during network ops)");
    
    // Initialize I2C and MPU6050 with timeout protection
    Serial.println("Initializing I2C...");
    Wire.begin(5, 6);  // XIAO ESP32 S3: SDA=5, SCL=6
    Wire.setClock(400000);  // Set I2C speed to 400kHz
    delay(1000);
    
    // Initialize OLED Display
    Serial.println("Initializing OLED Display...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("❌ OLED initialization failed!");
        displayReady = false;
    } else {
        Serial.println("✅ OLED initialized successfully!");
        displayReady = true;
        
        // ============ STARTUP SEQUENCE ============
        // Display startup text
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
        
        // Mark startup as complete
        startupComplete = true;
        showHomeIcon = true;  // ESP32 controls: Show home icon on MAIN screen by default
        Serial.println("✅ Startup complete! Main screen ready.");
    }
    
    Serial.println("Scanning I2C devices...");
    scanI2CDevices();
    
    Serial.println("Initializing MPU6050...");
    bool mpuSuccess = false;
    
    // Try MPU6050 initialization with timeout
    unsigned long startTime = millis();
    while (!mpuSuccess && (millis() - startTime < 5000)) {  // 5 second timeout
        mpu.initialize();
        delay(100);
        
        if (mpu.testConnection()) {
            Serial.println("✅ MPU6050 initialized successfully!");
            mpuSuccess = true;
            mpuAvailable = true;  // Set flag for sensor readings

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
        Serial.println("⚠️  Continuing without MPU6050 (will send dummy sensor data)");
    }
    
    // Initialize Camera
    initCamera();
    
    // Initialize I2S Microphone
    initAudio();
    
    // Create mutexes for synchronization
    audioMutex = xSemaphoreCreateMutex();
    cameraMutex = xSemaphoreCreateMutex();
    
    // Create network queue and mutex
    networkDataMutex = xSemaphoreCreateMutex();
    networkQueue = xQueueCreate(8, sizeof(uint8_t));  // Queue depth 8, each item 1 byte
    
    // Start audio monitoring task on Core 0 (dedicated to audio/VAD)
    xTaskCreatePinnedToCore(
        audioMonitorTask,    // Task function
        "AudioMonitor",      // Task name
        8192,               // Stack size
        NULL,               // Parameters
        2,                  // Priority (high for real-time audio)
        &audioTaskHandle,   // Task handle
        0                   // Core 0 (dedicated to audio)
    );
    
    // Start camera monitoring task on Core 0 (shares core with audio)
    xTaskCreatePinnedToCore(
        cameraMonitorTask,   // Task function
        "CameraMonitor",     // Task name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority (lower than audio)
        &cameraTaskHandle,  // Task handle
        0                   // Core 0 (shared with audio)
    );
    
    // Start OLED animation task on Core 0 (independent of WiFi on Core 1)
    xTaskCreatePinnedToCore(
        oledTask,           // Task function
        "OLED",            // Task name
        4096,              // Stack size
        NULL,              // Parameters
        1,                 // Priority (lower than audio)
        NULL,              // Task handle
        0                  // Core 0 (opposite of WiFi heavy Core 1)
    );
    
    // Start dedicated network task on Core 1 (HTTP only, queue-driven)
    xTaskCreatePinnedToCore(
        networkTask,         // Task function
        "Network",           // Task name
        8192,               // Stack size (large for HTTP + JSON)
        NULL,               // Parameters
        1,                  // Priority
        NULL,               // Task handle
        1                   // Core 1 (same as Arduino loop, shares cleanly)
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
        }
        else if (error == 4) {
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
            display.drawBitmap(0, 0, egg_crack_frames[i], EGG_CRACK_WIDTH, EGG_CRACK_HEIGHT, SSD1306_WHITE);
            display.display();
            Serial.printf("🥚 Egg frame %d/%d\n", i + 1, EGG_CRACK_FRAME_COUNT);
            delay(egg_crack_delays[i]);  // Use delay from all_pets.h (2 seconds each)
        }
        Serial.println("🐣 Egg hatching complete!");
    }
}

// ================= SLIDE TRANSITION ANIMATION =================
void slideEggScreenOut() {
    // Egg screen slides to the left and exits
    if (!displayReady) return;
    
    Serial.println("🥚 Egg screen sliding left...");
    const int slideSteps = 8;
    
    for (int step = 0; step <= slideSteps; step++) {
        display.clearDisplay();
        
        // Calculate x position: starts at 0, ends at -64 (completely off left side)
        int xPos = -(step * SCREEN_WIDTH) / slideSteps;
        
        // Draw the final egg frame sliding left
        display.drawBitmap(xPos, 0, egg_crack_frames[EGG_CRACK_FRAME_COUNT - 1], EGG_CRACK_WIDTH, EGG_CRACK_HEIGHT, SSD1306_WHITE);
        
        display.display();
        delay(80);
    }
    Serial.println("✅ Egg screen exit complete!");
}

void slideInfantSlowlyFromLeft() {
    // Infant appears gradually from left side (width gradually increasing)
    if (!displayReady) return;
    
    Serial.println("👶 Infant appearing slowly from left...");
    const int slideSteps = 15;  // 15 steps @ 200ms = 3 seconds
    
    for (int step = 0; step <= slideSteps; step++) {
        display.clearDisplay();
        
        // Calculate x position: starts at -width (fully left of screen, invisible)
        // ends at 0 (fully visible on screen)
        int xPos = -INFANT_WIDTH + (step * INFANT_WIDTH) / slideSteps;
        
        // Draw infant gradually appearing from left
        display.drawBitmap(xPos, 0, infant_frames[0], INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
        
        // Animation only - no text!
        display.display();
        delay(200);  // 15 steps × 200ms = 3 seconds total
    }
    
    // Final position - infant fully visible and centered
    display.clearDisplay();
    display.drawBitmap(0, 0, infant_frames[0], INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
    display.display();
    
    petAge = INFANT;
    Serial.println("✅ Infant fully visible!");
    
    // REMOVED: Blocking server notification - too slow, not critical for ESP32 operation
    // Menu cycling and OLED work independently now
}

// ================= HOME ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws home icon using pixel-by-pixel approach to prevent corruption with other animations
void drawHomeIcon() {
    int xOffset = 0;  // Top-left corner
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
    if ((millis() / 600) % 2 == 1) return;
    
    int xOffset = 0;  // Top-left corner (same as home icon but home hidden when play shows)
    int yOffset = 0;
    
    uint8_t frame = (millis() / play_icon_delays[0]) % PLAY_ICON_FRAME_COUNT;
    
    for (uint16_t y = 0; y < PLAY_ICON_HEIGHT; y++) {
        for (uint16_t x = 0; x < PLAY_ICON_WIDTH; x++) {
            uint16_t byteIndex = y * ((PLAY_ICON_WIDTH + 7) / 8) + (x / 8);
            uint8_t bitIndex = 7 - (x % 8);
            if (pgm_read_byte(&play_icon_frames[frame][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// ================= FOOD ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws food icon at bottom-right corner using pixel-by-pixel approach
void drawFoodIcon() {
    int xOffset = SCREEN_WIDTH - FOOD_ICON_WIDTH;   // Bottom-right: 64-24 = 40
    int yOffset = SCREEN_HEIGHT - FOOD_ICON_HEIGHT;  // Bottom-right: 32-12 = 20
    
    // Draw current food icon frame (animate between 2 frames)
    uint8_t foodFrame = (millis() / food_icon_delays[0]) % FOOD_ICON_FRAME_COUNT;
    
    for (uint16_t y = 0; y < FOOD_ICON_HEIGHT; y++) {
        for (uint16_t x = 0; x < FOOD_ICON_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * FOOD_ICON_WIDTH + x;
            uint8_t bitIndex = y % 8;
            
            if (pgm_read_byte(&food_icon_frames[foodFrame][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// ================= SICK ICON DRAWING (BLINKING HEART) =================
// Shows blinking heart icon at bottom-right ONLY when poop was ignored >15 min AND poop has been cleared
void drawSickIcon() {
    // Blink: 500ms on, 500ms off
    if ((millis() / 500) % 2 == 1) return;
    
    int xOffset = SCREEN_WIDTH - HEART_ICON_WIDTH;   // 64-24 = 40 (same spot as poop)
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
    int yOffset = SCREEN_HEIGHT - POOP_HEIGHT;  // Bottom-right: 32-12 = 20
    
    // Draw current poop icon frame (animate between 2 frames)
    uint8_t poopFrame = (millis() / poop_delays[0]) % POOP_FRAME_COUNT;
    
    for (uint16_t y = 0; y < POOP_HEIGHT; y++) {
        for (uint16_t x = 0; x < POOP_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * POOP_WIDTH + x;
            uint8_t bitIndex = 7 - (y % 8);  // Different bit indexing for poop
            
            if (pgm_read_byte(&poop_frames[poopFrame][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// ================= EATING ANIMATION (Full Screen) =================
// Plays full-screen eating animation (5 frames)
void playEatingAnimation() {
    if (!displayReady) return;
    
    Serial.println("😋 Playing eating animation...");
    
    for (uint8_t frame = 0; frame < EATING_FRAME_COUNT; frame++) {
        display.clearDisplay();
        
        // Draw full-screen eating animation frame
        display.drawBitmap(0, 0, eating_frames[frame], EATING_WIDTH, EATING_HEIGHT, SSD1306_WHITE);
        
        display.display();
        vTaskDelay(pdMS_TO_TICKS(eating_delays[frame]));  // 100ms per frame
    }
    
    Serial.println("✅ Eating animation complete!");
}

// Draw static food icon at top-left (for food menu)
void drawStaticFoodIcon() {
    int xOffset = 0;  // Top-left corner
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
    int xOffset = 0;  // Top-left corner
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

// Helper function to draw simple sad face (circle with frown) for ages without SAD animation
void drawSimpleSadFace() {
    // Draw large sad circle at center of screen
    int centerX = SCREEN_WIDTH / 2;  // 32
    int centerY = SCREEN_HEIGHT / 2; // 16
    
    // Outer circle (face outline)
    display.drawCircle(centerX, centerY, 12, SSD1306_WHITE);
    
    // Eyes (small filled circles)
    display.fillCircle(centerX - 5, centerY - 3, 2, SSD1306_WHITE);  // Left eye
    display.fillCircle(centerX + 5, centerY - 3, 2, SSD1306_WHITE);  // Right eye
    
    // Sad frown (small arc - drawn with pixels)
    for (int x = -5; x <= 5; x++) {
        int y = 5 + (x * x) / 8;  // Parabola for frown
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
        display.clearDisplay();  // Clear to show full eating animation
        uint8_t eatingFrame = (millis() / 100) % EATING_FRAME_COUNT;  // 100ms per frame
        display.drawBitmap(0, 0, eating_frames[eatingFrame], EATING_WIDTH, EATING_HEIGHT, SSD1306_WHITE);
    }
    // Check if just finished eating (show GOOD for 3 seconds)
    else if (justFinishedEating && (millis() - eatingFinishTime < 3000)) {
        Serial.println("🍽️ FOOD_MENU: Showing GOOD text");
        // Show "GOOD" text after eating (NO newline to prevent cursor artifacts)
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(18, 12);
        display.print("GOOD!");  // Changed from println to print - prevents unwanted cursor movement
    } else if (justFinishedEating) {
        Serial.println("🍽️ FOOD_MENU: Showing age-based HAPPY face after eating");
        // After GOOD text expires, show age-appropriate HAPPY face
        justFinishedEating = false;  // Reset flag
        
        // FIX: Use age-based HAPPY frame instead of hardcoded child_frames
        const uint8_t* frameData = nullptr;
        switch (petAge) {
            case INFANT:
                frameData = infant_frames[0];  // INFANT HAPPY
                display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
                break;
            case CHILD:
                frameData = child_frames[0];  // CHILD HAPPY
                display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT, SSD1306_WHITE);
                break;
            case ADULT:
                frameData = adult_frames[0];  // ADULT HAPPY
                display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT, SSD1306_WHITE);
                break;
            case OLD:
                frameData = old_happy[0];  // OLD HAPPY
                display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT, SSD1306_WHITE);
                break;
        }
    } else if (showFoodIcon) {
        // Pet is HUNGRY - show CRYING animation and check for feeding gesture
        Serial.printf("🍽️ FOOD_MENU: Pet HUNGRY (Age: %d) - showing CRYING\n", petAge);
        
        // Check for tilt gesture to trigger feeding
        checkFeedingGesture();
        
        switch (petAge) {
            case INFANT: {
                // INFANT has CRYING animation
                uint8_t cryFrame = (millis() / infant_cry_delays[0]) % INFANT_CRY_FRAME_COUNT;
                const uint8_t* frameData = infant_cry_frames[cryFrame];
                display.drawBitmap(0, 0, frameData, INFANT_CRY_WIDTH, INFANT_CRY_HEIGHT, SSD1306_WHITE);
                break;
            }
            case OLD: {
                uint8_t cryFrame = (millis() / old_cry_delays[0]) % OLD_CRY_FRAME_COUNT;
                const uint8_t* frameData = old_cry[cryFrame];
                display.drawBitmap(0, 0, frameData, OLD_CRY_WIDTH, OLD_CRY_HEIGHT, SSD1306_WHITE);
                break;
            }
            case CHILD:
            case ADULT:
                drawSimpleSadFace();
                break;
        }
    } else {
        Serial.println("🍽️ FOOD_MENU: Pet NOT hungry - showing HAPPY face");
        // Pet is NOT hungry - show HAPPY face (excited about food menu)
        const uint8_t* frameData = nullptr;
        switch (petAge) {
            case INFANT:
                frameData = infant_frames[0];  // INFANT HAPPY
                display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
                break;
            case CHILD:
                frameData = child_frames[0];  // CHILD HAPPY
                display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT, SSD1306_WHITE);
                break;
            case ADULT:
                frameData = adult_frames[0];  // ADULT HAPPY
                display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT, SSD1306_WHITE);
                break;
            case OLD:
                frameData = old_happy[0];  // OLD HAPPY
                display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT, SSD1306_WHITE);
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
            if (pgm_read_byte(&clean_slide_frames[frame][byteIndex]) & (1 << bitIndex)) {
                int px = x + xOffset;
                int py = y + 4;  // vertical centering on 32px screen
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
                cleanSlideFrame = (cleanSlideFrame == 1) ? 2 : 1;  // toggle frames
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
    int xOffset = 0;  // Top-left corner
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
const unsigned char charFrame1[] PROGMEM = {
    0x18, 0x3C, 0x18, 0x24, 0x18, 0x24, 0x00, 0x00
};
// 8x8 walk frame 2: striding
const unsigned char charFrame2[] PROGMEM = {
    0x18, 0x3C, 0x18, 0x24, 0x3C, 0x12, 0x00, 0x00
};

// Calculate KakuCoin reward
float calculateKakuCoin(int scoreValue) {
    return scoreValue * 0.80;
}

// Read tilt for game control
void readTiltForGame() {
    if (!mpuAvailable) return;
    
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    
    float accelX = ax / 16384.0;
    
    filteredX = 0.9 * filteredX + 0.1 * accelX;
    
    if (abs(filteredX) < 0.3) {
        velocity = 0;
    } else {
        velocity = filteredX * 3.0;
    }
    
    playerX += velocity;
    
    if (playerX < 0) playerX = 0;
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
    if (foodY + foodSize >= playerY &&
        foodX + foodSize >= playerX &&
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
    if (!mpuAvailable) return;
    
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    
    float accelX = ax / 16384.0;
    
    if (accelX < -0.8) {
        if (!holdingLeft) {
            holdingLeft = true;
            holdStartTime = millis();
        }
        
        if (millis() - holdStartTime > 3000) {
            activeGame = random(0, 2);  // 0 = catch food, 1 = dodge obstacle
            resetGameState();
            dodgeFallSpeed = 120;  // reset dodge speed
            playGameState = GAME_PLAYING;
            gameStartTime = millis();
            Serial.printf("🎮 Game started! Type: %s\n", activeGame == 0 ? "CATCH FOOD" : "DODGE");
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
    if (!mpuAvailable) return;
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    float accelX = ax / 16384.0;
    playerX += accelX * 3.0;
    if (playerX < 0) playerX = 0;
    if (playerX > SCREEN_WIDTH - 8) playerX = SCREEN_WIDTH - 8;
    if (abs(accelX) > 0.2) {
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
        if (dodgeScore % 5 == 0 && dodgeFallSpeed > 40) dodgeFallSpeed -= 8;
    }
}

void checkDodgeCollision() {
    const int charY = 24;
    if (obsY + obsSize >= charY &&
        obsX + obsSize >= (int)playerX &&
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
    // Explosion rings
    for (int r = 2; r < 14; r += 2) {
        display.clearDisplay();
        display.drawCircle((int)playerX + 4, 23, r, SSD1306_WHITE);
        display.display();
        delay(70);
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
        delay(60);
    }
    // Screen shake "CRASH!"
    for (int i = 0; i < 6; i++) {
        display.clearDisplay();
        int s = random(-2, 2);
        display.setCursor(10 + s, 10);
        display.print("CRASH!");
        display.display();
        delay(90);
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
    delay(2500);
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
    
    const char* rewardUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/game/reward";
    
    if (!http.begin(rewardUrl)) {
        Serial.println("❌ Failed to begin reward HTTP");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    StaticJsonDocument<256> doc;
    doc["device_id"] = "ESP32_001";
    doc["score"] = score;
    doc["kakucoin"] = kakucoin;
    doc["play_duration"] = (millis() - gameStartTime) / 1000;  // seconds
    doc["game_type"] = (activeGame == 0) ? "catch_food" : "dodge_obstacle";
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("🎮 Sending game reward: Score=%d, KC=%.1f\n", score, kakucoin);
    
    int httpCode = http.POST(payload);
    
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
                        playDodgeGameOverAnim();  // blocking animation
                    }
                    // Send reward and return to idle
                    float kc = dodgeScore * 0.8;
                    sendKakuCoinReward(dodgeScore, kc);
                    playGameState = GAME_IDLE;
                    Serial.println("🎮 Dodge game ended, resuming normal operations");
                }
            }
            break;
        
        case GAME_OVER_ANIM:
            // Game 1 (catch food) game-over animation (coins screen)
            drawGameOverScreen();
            
            if (millis() - gameOverStartTime > 5000) {
                // Send reward to server
                float kakuCoin = calculateKakuCoin(gameScore);
                sendKakuCoinReward(gameScore, kakuCoin);
                
                // Resume normal operations
                playGameState = GAME_IDLE;
                Serial.println("🎮 Game ended, resuming normal operations");
            }
            break;
    }
}

// Draw static heart icon at top-left (for health menu)
void drawStaticHealthIcon() {
    int xOffset = 0;  // Top-left corner
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
    
    // Display text based on pet health status
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    if (petIsSick) {
        // Show "Give Medicine" text at center
        display.setCursor(4, 12);
        display.print("Give Med");
        
        // Check for tilt gesture to give medicine
        checkMedicineGesture();
        Serial.println("❤️ HEALTH_MENU: Pet is SICK - Give Medicine");
    } else {
        // Show "All Good" text at center
        display.setCursor(10, 12);
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

#define STATUS_FLASH_WIDTH  32
#define STATUS_FLASH_HEIGHT 16

PROGMEM const uint8_t statusFlashBitmap[64] = {
    0xff,0xff,0xff,0xff,  0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,  0xff,0xff,0xbf,0xff,
    0xff,0xff,0x7f,0xff,  0xff,0xfe,0x7f,0xff,
    0xff,0xfc,0x7f,0xff,  0xff,0xfc,0x1f,0xff,
    0xff,0xf8,0x1f,0xff,  0xff,0xff,0x3f,0xff,
    0xff,0xfe,0x7f,0xff,  0xff,0xfe,0xff,0xff,
    0xff,0xfd,0xff,0xff,  0xff,0xfd,0xff,0xff,
    0xff,0xff,0xff,0xff,  0xff,0xff,0xff,0xff
};

// ================= PROGRESS BAR HELPER =================
// Draws a labelled progress bar. x,y = top-left corner, w = total width, h = bar height
void drawBar(int x, int y, int w, int h, int level, int maxLevel) {
    display.drawRect(x, y, w, h, SSD1306_WHITE);          // outline
    int fill = (level * (w - 2)) / maxLevel;              // filled pixels
    if (fill > 0)
        display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);  // fill
}

// Draw simple smiley for status quadrant (Q1)
void drawSmileyStatus(int x, int y) {
    display.drawCircle(x + 8, y + 8, 7, SSD1306_WHITE);
    display.fillCircle(x + 5, y + 6, 1, SSD1306_WHITE);   // left eye
    display.fillCircle(x + 11, y + 6, 1, SSD1306_WHITE);  // right eye
    display.drawLine(x + 4, y + 11, x + 12, y + 11, SSD1306_WHITE);  // smile
}

// Draw flash/lightning bolt bitmap for status quadrant (Q3)
// Renders PROGMEM bitmap inverted (0-bit = pixel ON)
void drawFlashStatus(int xOffset, int yOffset) {
    for (uint16_t row = 0; row < STATUS_FLASH_HEIGHT; row++) {
        for (uint16_t col = 0; col < STATUS_FLASH_WIDTH; col++) {
            uint16_t byteIndex = row * ((STATUS_FLASH_WIDTH + 7) / 8) + (col / 8);
            uint8_t  bitIndex  = 7 - (col % 8);
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
    if (!mpuAvailable || !petIsSick) return;
    
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
    if (petIsWalking || isDeviceSleeping) return;
    if (!mpuAvailable) return;
    
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
            feedingGestureStartTime = millis();  // Start timeout timer
            holdingLeftForFeeding = false;
            imageAlreadySentThisSession = true;
            
            // Start eating animation immediately
            isUploadingImage = true;
            Serial.println("📸 Triggering food image capture!");
            Serial.println("🍴 Starting eating animation!");
            
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
    if (petIsWalking || isDeviceSleeping) return;
    if (!mpuAvailable) return;
    if (!showPoopIcon) return;  // Only clean if poop present
    
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
            cleanSlideX = SCREEN_WIDTH;   // Start sprite from right edge
            cleanSlideFrame = 1;
            cleanSlideLastStep = millis();
            holdingLeftForCleaning = false;
            
            Serial.println("🧹 Starting cleaning animation!");
            
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
            
            if (pgm_read_byte(&injection_frames[currentInjectionFrame][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x, y, SSD1306_WHITE);
            }
        }
    }
    
    display.display();
    
    // Check if it's time to advance to next frame
    if (millis() - medicineAnimStartTime > pgm_read_word(&injection_delays[currentInjectionFrame])) {
        currentInjectionFrame++;
        medicineAnimStartTime = millis();
        
        // Check if one loop is complete
        if (currentInjectionFrame >= INJECTION_FRAME_COUNT) {
            currentInjectionFrame = 0;
            medicineAnimLoopCount++;
            Serial.printf("💊 Medicine animation loop %d/3 complete\n", medicineAnimLoopCount);
            
            // Check if all 3 loops are done
            if (medicineAnimLoopCount >= 3) {
                // Animation complete - cure the pet
                givingMedicine = false;
                petIsSick = false;
                showSickIcon = false;  // Hide sick icon immediately
                medicineAnimLoopCount = 0;
                currentInjectionFrame = 0;
                Serial.println("✅ Medicine given! Pet is now healthy!");
                
                // Notify server: sick_pending cleared, hunger resumes
                uint8_t req = NET_INJECT;
                xQueueSend(networkQueue, &req, 0);
                
                // Show "All Good" message for a moment
                display.clearDisplay();
                drawStaticHealthIcon();
                display.setTextSize(1);
                display.setTextColor(SSD1306_WHITE);
                display.setCursor(10, 12);
                display.print("All Good");
                display.display();
                delay(2000);  // Show for 2 seconds
            }
        }
    }
}

// ================= MENU TILT GESTURE (RIGHT TILT 2 SEC) =================
// Right tilt + hold 2 seconds → cycle through menus
void checkMenuTiltGesture() {
    if (!mpuAvailable) return;
    if ((millis() - lastMenuCycleTime) < MENU_CYCLE_COOLDOWN) return;  // Respect cooldown
    
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

// ── NEUTRAL / SLEEP HELPERS ───────────────────────────────────────────────────

// Hardware step counter — stoss/barrier method (same algorithm as original server detect_steps)
// Uses g-unit values so barrier is sensor-independent
void detectHardwareStep() {
    if (!mpuAvailable) return;
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
    float stoss = dx*dx + dy*dy + dz*dz;

    unsigned long now = millis();
    if (stoss > STEP_BARRIER_G2 && (now - lastHwStepTime) > STEP_MIN_MS) {
        hwStepCount++;
        lastHwStepTime    = now;
        lastWalkingStepTime = now;
        Serial.printf("👣 HW Step #%u  stoss=%.4f\n", hwStepCount, stoss);
    }
}

// Returns true when device lies flat (no significant X/Y tilt)
bool isDeviceNeutral() {
    if (!mpuAvailable) return false;
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    float x = ax / 16384.0f;
    float y = ay / 16384.0f;
    return (abs(x) < 0.3f && abs(y) < 0.3f);
}

// 2-frame sleeping animation (frame-timed)
void displaySleepingAnimation() {
    static uint8_t sleepFrame = 0;
    static unsigned long lastSleepFrameTime = 0;
    display.clearDisplay();
    display.drawBitmap(0, 0, sleeping_frames[sleepFrame], SLEEPING_WIDTH, SLEEPING_HEIGHT, SSD1306_WHITE);
    display.display();
    if (millis() - lastSleepFrameTime >= pgm_read_word(&sleeping_delays[sleepFrame])) {
        lastSleepFrameTime = millis();
        sleepFrame = (sleepFrame + 1) % SLEEPING_FRAME_COUNT;
    }
}

// 6-frame walking animation (skips frame 2 at runtime)
void displayWalkingAnimation() {
    static uint8_t wFrame = 0;
    static unsigned long lastWFrameTime = 0;
    if (wFrame == 2) wFrame = 3;   // skip frame 2 on entry guard
    display.clearDisplay();
    display.drawBitmap(0, 0, walking_animation[wFrame], WALKING_WIDTH, WALKING_HEIGHT, SSD1306_WHITE);
    display.display();
    if (millis() - lastWFrameTime >= pgm_read_word(&walking_delays[wFrame])) {
        lastWFrameTime = millis();
        wFrame = (wFrame + 1) % WALKING_FRAME_COUNT;
        if (wFrame == 2) wFrame = 3;  // skip frame 2
    }
}

// Cycle through menus: MAIN → FOOD_MENU → TOILET_MENU → PLAY_MENU → HEALTH_MENU → STATUS_INFO_MENU → MAIN
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
    
    Serial.printf("📡 Menu cycle: %s → %s\n", currentScreenType.c_str(), newMenu.c_str());
    
    // ✅ CHANGE MENU LOCALLY FIRST (instant, no server dependency)
    currentScreenType = newMenu;
    
    // ✅ ESP32 controls home icon based on menu (not server)
    if (newMenu == "MAIN") {
        showHomeIcon = true;   // Show home icon on MAIN screen
        Serial.println("🏠 Home icon: ENABLED (MAIN screen)");
    } else {
        showHomeIcon = false;  // Hide home icon on other menus
        Serial.println("🏠 Home icon: DISABLED (not MAIN)");
    }
    
    // Reset image send flag when leaving FOOD_MENU
    if (newMenu != "FOOD_MENU") {
        imageAlreadySentThisSession = false;
        Serial.println("🔄 Reset image send flag (left FOOD_MENU)");
    }
    
    Serial.printf("✅ Menu changed locally to: %s (OLED updates immediately)\n", newMenu.c_str());
    
    // ESP32 runs independently - no server notification needed
}

// ================= CAMERA COVER DETECTION (DISABLED — replaced by right-tilt gesture) =================
// OLD: Continuous frame checking every loop (causes hardware heating)
// Old: Black frame detection — removed. Menu now uses right-tilt gesture.
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

// ================= PET ANIMATION FUNCTION =================
void displayPetAnimation() {
    if (!displayReady) return;
    
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
            return;  // Exit early
        } else if (currentScreenType == "TOILET_MENU") {
            // Check for cleaning gesture in toilet menu
            checkCleaningGesture();
            displayToiletMenu();
            return;  // Exit early
        } else if (currentScreenType == "PLAY_MENU") {
            displayPlayMenu();
            return;  // Exit early
        } else if (currentScreenType == "HEALTH_MENU") {
            displayHealthMenu();
            return;  // Exit early
        } else if (currentScreenType == "STATUS_INFO_MENU") {
            displayStatusInfoMenu();
            return;  // Exit early
        } else if (currentScreenType == "STATS_MENU") {
            displayStatsMenu();
            return;  // Exit early
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
        const uint8_t* frameData = nullptr;
        uint8_t frameCount = 0;
        
        // EMOTION-BASED ANIMATION SELECTION
        // Priority: Emotion > Age
        if (currentEmotion == "HAPPY") {
            switch (petAge) {
                case INFANT: {
                    frameData = infant_happy[currentFrame % INFANT_HAPPY_FRAME_COUNT];
                    frameCount = INFANT_HAPPY_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, INFANT_HAPPY_WIDTH, INFANT_HAPPY_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case CHILD: {
                    frameData = happy_child[currentFrame % HAPPY_CHILD_FRAME_COUNT];
                    frameCount = HAPPY_CHILD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, HAPPY_CHILD_WIDTH, HAPPY_CHILD_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case ADULT: {
                    frameData = happy_adult[currentFrame % HAPPY_ADULT_FRAME_COUNT];
                    frameCount = HAPPY_ADULT_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, HAPPY_ADULT_WIDTH, HAPPY_ADULT_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case OLD: {
                    frameData = old_happy[currentFrame % OLD_HAPPY_FRAME_COUNT];
                    frameCount = OLD_HAPPY_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT, SSD1306_WHITE);
                    break;
                }
            }
        } else if (currentEmotion == "CRY") {
            switch (petAge) {
                case INFANT: {
                    frameData = infant_cry_frames[currentFrame % INFANT_CRY_FRAME_COUNT];
                    frameCount = INFANT_CRY_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, INFANT_CRY_WIDTH, INFANT_CRY_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case CHILD: {
                    frameData = cry_child[currentFrame % CRY_CHILD_FRAME_COUNT];
                    frameCount = CRY_CHILD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, CRY_CHILD_WIDTH, CRY_CHILD_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case ADULT: {
                    frameData = cry_adult[currentFrame % CRY_ADULT_FRAME_COUNT];
                    frameCount = CRY_ADULT_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, CRY_ADULT_WIDTH, CRY_ADULT_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case OLD: {
                    frameData = old_cry[currentFrame % OLD_CRY_FRAME_COUNT];
                    frameCount = OLD_CRY_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, OLD_CRY_WIDTH, OLD_CRY_HEIGHT, SSD1306_WHITE);
                    break;
                }
            }
        } else if (currentEmotion == "SURPRISE") {
            switch (petAge) {
                case INFANT: {
                    frameData = infant_surprise_frames[currentFrame % INFANT_SURPRISE_FRAME_COUNT];
                    frameCount = INFANT_SURPRISE_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, INFANT_SURPRISE_WIDTH, INFANT_SURPRISE_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case CHILD: {
                    frameData = child_surprise[currentFrame % CHILD_SURPRISE_FRAME_COUNT];
                    frameCount = CHILD_SURPRISE_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, CHILD_SURPRISE_WIDTH, CHILD_SURPRISE_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case ADULT: {
                    frameData = surprise_adult[currentFrame % SURPRISE_ADULT_FRAME_COUNT];
                    frameCount = SURPRISE_ADULT_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, SURPRISE_ADULT_WIDTH, SURPRISE_ADULT_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case OLD: {
                    frameData = old_surprise[currentFrame % OLD_SURPRISE_FRAME_COUNT];
                    frameCount = OLD_SURPRISE_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, OLD_SURPRISE_WIDTH, OLD_SURPRISE_HEIGHT, SSD1306_WHITE);
                    break;
                }
            }
        } else if (currentEmotion == "SAD" || currentEmotion == "HUNGER" || currentEmotion == "POOP") {
            // SAD/HUNGER/POOP - sad animation (poop present = pet is uncomfortable)
            switch (petAge) {
                case INFANT: {
                    frameData = infant_sad_frames[currentFrame % INFANT_SAD_FRAME_COUNT];
                    frameCount = INFANT_SAD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, INFANT_SAD_WIDTH, INFANT_SAD_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case CHILD: {
                    frameData = child_sad[currentFrame % CHILD_SAD_FRAME_COUNT];
                    frameCount = CHILD_SAD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, CHILD_SAD_WIDTH, CHILD_SAD_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case ADULT: {
                    frameData = adult_sad[currentFrame % ADULT_SAD_FRAME_COUNT];
                    frameCount = ADULT_SAD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, ADULT_SAD_WIDTH, ADULT_SAD_HEIGHT, SSD1306_WHITE);
                    break;
                }
                case OLD: {
                    frameData = old_sad[currentFrame % OLD_SAD_FRAME_COUNT];
                    frameCount = OLD_SAD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, OLD_SAD_WIDTH, OLD_SAD_HEIGHT, SSD1306_WHITE);
                    break;
                }
            }
        } else if (currentEmotion == "IDLE") {
            // IDLE face - pet is calm/relaxed (not hungry, not sick, not dirty)
            // Show first frame only (static calm face)
            switch (petAge) {
                case INFANT:
                    frameData = infant_frames[0];  // INFANT IDLE
                    display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
                    break;
                case CHILD:
                    frameData = child_frames[0];  // CHILD IDLE
                    display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT, SSD1306_WHITE);
                    break;
                case ADULT:
                    frameData = adult_frames[0];  // ADULT IDLE
                    display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT, SSD1306_WHITE);
                    break;
                case OLD:
                    frameData = old_happy[0];  // OLD IDLE (first happy frame, static)
                    display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT, SSD1306_WHITE);
                    break;
            }
        } else {
            // Default age-based animations (normal/happy)
            switch (petAge) {
                case INFANT:
                    frameData = infant_frames[currentFrame % INFANT_FRAME_COUNT];
                    frameCount = INFANT_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
                    break;
                case CHILD:
                    frameData = child_frames[currentFrame % CHILD_FRAME_COUNT];
                    frameCount = CHILD_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT, SSD1306_WHITE);
                    break;
                case ADULT:
                    frameData = adult_frames[currentFrame % ADULT_FRAME_COUNT];
                    frameCount = ADULT_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT, SSD1306_WHITE);
                    break;
                case OLD:
                    frameData = old_happy[currentFrame % OLD_HAPPY_FRAME_COUNT];
                    frameCount = OLD_HAPPY_FRAME_COUNT;
                    display.drawBitmap(0, 0, frameData, OLD_HAPPY_WIDTH, OLD_HAPPY_HEIGHT, SSD1306_WHITE);
                    break;
            }
        }
        
        // Draw home icon OR play icon at top-left (play icon overrides home when active)
        // Icons hidden during walking/sleeping — resume when animation ends
        bool iconsAllowed = !petIsWalking && !isDeviceSleeping;

        if (iconsAllowed && showPlayIcon && currentScreenType == "MAIN") {
            drawPlayIcon();  // Play reminder blinks at top-left
        } else if (iconsAllowed && showHomeIcon && currentScreenType == "MAIN") {
            drawHomeIcon();
        }
        
        // Draw food icon at bottom-right corner (shows when pet is hungry)
        // Priority: SICK > POOP > HUNGER — food hidden if poop or sick active (same as sick/poop mutual exclusion)
        if (iconsAllowed && showFoodIcon && !showPoopIcon && !showSickIcon && currentScreenType == "MAIN") {
            drawFoodIcon();
        }
        
        // Draw poop icon at bottom-right corner (shows when poop present)
        if (iconsAllowed && showPoopIcon && currentScreenType == "MAIN") {
            drawPoopIcon();
        }
        
        // Draw sick/heart icon at bottom-right (only when poop cleared but was ignored >15 min)
        if (iconsAllowed && showSickIcon && !showPoopIcon && currentScreenType == "MAIN") {
            drawSickIcon();
        }
        
        // Only animation - no text
        display.display();
        
        // Increment frame
        currentFrame++;
    }
}

// ================= NETWORK TASK (Core 1 — Queue-driven HTTP) =================
// All HTTP calls happen here, queue-driven from loop()
// Runs on Core 1 alongside loop() — decouples sensor reading from network blocking
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
                    setCpuFrequencyMhz(160);  // Boost for JSON serialization + HTTP
                    sendSensorDataOnly(dataCopy);
                    setCpuFrequencyMhz(40);   // Return to idle after send
                    break;
                }
                
                case NET_OLED:
                    // Combined: OLED state + events in one slot
                    getOLEDDisplayFromServer();
                    setCpuFrequencyMhz(160);
                    pollForEvents();
                    setCpuFrequencyMhz(40);
                    // If server flagged OTA update, perform it now
                    if (otaUpdateRequested) {
                        checkAndPerformOTA();
                    }
                    break;
                
                case NET_IMAGE:
                    setCpuFrequencyMhz(160);  // Boost for binary image upload
                    sendImageData("");         // Uses shared capturedImageBuffer
                    setCpuFrequencyMhz(40);
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
            }
            vTaskDelay(pdMS_TO_TICKS(20));  // Brief settle between requests
        }
    }
}

void loop() {
    // This loop runs on Core 1 - handles sensors + queue dispatching ONLY
    // HTTP calls moved to networkTask on Core 1 — no blocking here
    
    // Check WiFi connection with timeout protection
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi disconnected, attempting reconnect...");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        unsigned long reconnectStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - reconnectStart < 20000)) {
            Serial.print(".");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n✅ WiFi Reconnected: " + WiFi.localIP().toString());
        } else {
            Serial.println("\n⚠️  WiFi reconnection failed — offline mode");
        }
    }
    
    // Debug: Print WiFi status every 10 seconds
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 10000) {
        lastWiFiCheck = millis();
        Serial.printf("🔗 WiFi: %s | IP: %s | RSSI: %d dBm\n",
                      WiFi.status() == WL_CONNECTED ? "✅" : "❌",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        Serial.printf("🎤 Audio Energy: %d\n", audioEnergyLevel);
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
                reading.gyro_x  = reading.gyro_y  = reading.gyro_z  = 0.0;
            }
            sensorBatch.readings[sensorBatch.reading_count++] = reading;
            if (micReady && audioEnergyLevel > 0) totalMicLevel += (20.0 + audioEnergyLevel / 100.0);
            micReadingCount++;
        }
    }
    
    // ── CAMERA IMAGE CHECK (DISABLED - Manual feeding via tilt gesture) ───────────
    // Auto-send removed: User must tilt+hold left for 3 seconds to feed
    
    // ── FEEDING GESTURE TIMEOUT CHECK ─────────────────────────────────────────
    // Reset feeding flag if stuck for more than 30 seconds
    if (capturingForFeeding && (millis() - feedingGestureStartTime > FEEDING_TIMEOUT)) {
        Serial.println("⚠️ Feeding gesture timeout - resetting flags");
        capturingForFeeding = false;
        isUploadingImage = false;  // Also reset eating animation
    }
    
    // ── NEUTRAL / SLEEP STATE DETECTION ──────────────────────────────────────
    // If the device stays flat (neutral) for 30 s enter sleep mode:
    //   • network paused, WiFi modem sleeps, display shows sleeping animation
    //   • sleep seconds accumulated and sent with the next sensor upload on wake
    if (!isDeviceSleeping) {
        if (isDeviceNeutral()) {
            if (neutralStartTime == 0) neutralStartTime = millis();
            if (millis() - neutralStartTime >= NEUTRAL_SLEEP_TIMEOUT) {
                isDeviceSleeping = true;
                sleepStartTime   = millis();
                Serial.println("😴 Neutral 30s → SLEEP MODE (network paused)");
            }
        } else {
            neutralStartTime = 0;  // reset if device is moved
        }
    } else {
        // Already sleeping — wake on any meaningful movement
        if (!isDeviceNeutral()) {
            uint32_t sleptSec = (millis() - sleepStartTime) / 1000;
            accumulatedSleepSec += sleptSec;
            Serial.printf("⏰ Woke up — slept %us (banked: %us)\n", sleptSec, accumulatedSleepSec);
            isDeviceSleeping = false;
            neutralStartTime = 0;
            sleepStartTime   = 0;
        }
    }

    // While sleeping: skip all network work, yield CPU, and let oledTask render the animation
    if (isDeviceSleeping) {
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }

    // ── STAGGERED NETWORK SCHEDULER ───────────────────────────────────────────
    // Uses 500ms tick slots to spread requests — no simultaneous HTTP bursts
    // Slot 0,20,40...  (t % 10000 ≈ 0)    → sensor every 10 s
    // Slot 5,15,25...  (t % 5000  ≈ 2500) → OLED + events every 5 s (2.5 s offset)
    static uint32_t lastSensorTick = 0;
    static uint32_t lastOledTick   = 0;
    uint32_t nowTick = millis() / 500;  // 500 ms resolution ticks
    
    // SLOT A — Sensor data every 10 s (tick 0,20,40,...)
    if (nowTick % 20 == 0 && nowTick != lastSensorTick && !isUploadingImage) {
        lastSensorTick = nowTick;
        SensorData data = readAllSensors();
        data.sensor_batch = sensorBatch;
        data.sensor_batch.avg_mic_level = micReadingCount > 0 ? totalMicLevel / micReadingCount : 0.0;
        data.sensor_batch.sound_data    = audioEnergyLevel;
        Serial.printf("📤 Queue: NET_SENSOR (%d readings)\n", sensorBatch.reading_count);
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
    
    // SLOT B — OLED state + events combined every 5 s at +2.5 s offset (tick 5,15,25,...)
    if (nowTick % 10 == 5 && nowTick != lastOledTick && startupComplete && !isUploadingImage) {
        lastOledTick = nowTick;
        uint8_t req = NET_OLED;
        xQueueSend(networkQueue, &req, 0);
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));  // 20ms loop cadence (non-blocking)
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
    config.pin_sscb_sda = SIOD_GPIO_NUM;  // Fixed: was pin_sccb_sda
    config.pin_sscb_scl = SIOC_GPIO_NUM;  // Fixed: was pin_sccb_scl
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    
    config.xclk_freq_hz = 10000000;  // 10 MHz — reduced from 20 MHz (less heat, stable)
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QQVGA; // 160x120 - reduced for power
    config.jpeg_quality = 20;             // Lower quality = less heat
    config.fb_count = 1;                  // Single buffer = less memory
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
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
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
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_GPIO,
            .din = PDM_DIN_GPIO,
            .invert_flags = { .clk_inv = false },
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
    vad_buffer = (int16_t*)malloc(VAD_BUFFER_SIZE * sizeof(int16_t));
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
    setCpuFrequencyMhz(40);
    Serial.println("⚡ CPU: 40MHz (camera idle)");
    
    while (true) {
        if (!cameraReady) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // Only capture when feeding gesture is triggered
        if (capturingForFeeding && !isUploadingImage) {
            // Boost CPU for capture
            setCpuFrequencyMhz(240);
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
                    capturedImageBuffer = (uint8_t*)ps_malloc(fb->len);
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
            
            // Drop back to low frequency after capture
            setCpuFrequencyMhz(40);
            Serial.println("⚡ CPU: 40MHz (camera idle)");
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
    uint8_t* recording_buffer = NULL;
    size_t recorded_bytes = 0;
    const size_t MAX_RECORDING_SIZE = SAMPLE_RATE * 2 * 5; // Max 5 seconds
    
    while (true) {
        if (!micReady) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Continuously read audio for VAD analysis
        esp_err_t err = i2s_channel_read(rx_handle, vad_buffer, VAD_BUFFER_SIZE * sizeof(int16_t), 
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
                    recording_buffer = (uint8_t*)ps_malloc(MAX_RECORDING_SIZE + WAV_HEADER_SIZE);
                    if (recording_buffer) {
                        generate_wav_header(recording_buffer, MAX_RECORDING_SIZE, SAMPLE_RATE);
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
        
        // Mic sleep: if silent for 3+ seconds and not recording, slow-poll to save power
        if (!currentlyRecording && (currentTime - lastSoundTime) > 3000) {
            audioEnergyLevel = 0;  // Report silence
            vTaskDelay(pdMS_TO_TICKS(200));  // Sleep 200ms between reads
            continue;
        }
        
        // If currently recording, add audio data to buffer
        if (currentlyRecording && recording_buffer && 
            (recorded_bytes + bytes_read) < (MAX_RECORDING_SIZE + WAV_HEADER_SIZE)) {
            
            // Apply volume gain and copy to recording buffer
            for (int i = 0; i < samples; i++) {
                int16_t sample = vad_buffer[i];
                int32_t amp = sample << VOLUME_GAIN;
                if (amp > 32767) amp = 32767;
                if (amp < -32768) amp = -32768;
                
                *((int16_t*)(recording_buffer + recorded_bytes)) = amp;
                recorded_bytes += sizeof(int16_t);
            }
        }
        
        // Stop recording after silence timeout or buffer full
        if (currentlyRecording && 
            ((currentTime - lastSoundTime > SILENCE_TIMEOUT) || 
             (recorded_bytes >= (MAX_RECORDING_SIZE + WAV_HEADER_SIZE - 1024)))) {
            
            Serial.printf("🎤 Core 0: Recording complete! %d bytes\n", recorded_bytes);
            
            if (recording_buffer && recorded_bytes > WAV_HEADER_SIZE) {
                // Convert to base64 and store for Core 1
                String audioB64 = base64::encode(recording_buffer, recorded_bytes);
                
                if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    detectedAudioData = audioB64;
                    speechDetected = true;
                    Serial.printf("🎤 Core 0: Audio ready for transmission (%d chars base64)\n", audioB64.length());
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
    data.device_orientation = "COMPUTING";  // Placeholder - server will compute
    data.orientation_confidence = 0.0;       // Placeholder - server will compute
    
    // Use real microphone energy level from audio monitoring
    if (micReady && audioEnergyLevel > 0) {
        // Convert audio energy to approximate dB level
        data.mic_level = 20.0 + (audioEnergyLevel / 100.0); // Scale energy to dB range
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
    
    return data;
}

// ================= SERVER HEALTH CHECK =================
bool isServerAlive() {
    HTTPClient http;
    http.setConnectTimeout(2000);  // Reduced from 5s
    http.setTimeout(3000);         // Reduced from 8s

    if (!http.begin("https://kakuproject-90943350924.asia-south1.run.app/api/health")) {
        return false;
    }

    int code = http.GET();
    http.end();

    return (code == 200);
}

// ================= OLED DISPLAY ANIMATION POLLING =================
void getOLEDDisplayFromServer() {
    HTTPClient http;
    http.setReuse(true);  // Reuse TCP/TLS connection
    http.setConnectTimeout(3000);
    http.setTimeout(3000);

    if (!http.begin(oledDisplayUrl)) {
        return;  // Silently fail, keep showing current animation
    }

    int httpCode = http.GET();
    
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
                    if ((int)petAge != newAnimationId) {
                        petAge = (PetAge)newAnimationId;
                        Serial.printf("🎬 Animation: %s (id: %d)\n", animationName.c_str(), newAnimationId);
                    }
                }
            }
            
            // Parse actual integer age from server
            if (g_oledDoc.containsKey("age")) {
                int newAge = g_oledDoc["age"].as<int>();
                if (petAgeInt != newAge) {
                    petAgeInt = newAge;
                    Serial.printf("🗓️  Pet age updated: %d yrs\n", petAgeInt);
                }
            }
            
            // screen_type / screen_state override DISABLED — ESP32 controls menu locally via tilt gesture
            /*
            if (g_oledDoc.containsKey("screen_type")) {
                String newScreenType = g_oledDoc["screen_type"].as<String>();
                if (currentScreenType == "FOOD_MENU" && newScreenType != "FOOD_MENU") {
                    imageAlreadySentThisSession = false;
                }
                currentScreenType = newScreenType;
                Serial.printf("📺 Screen Type: %s\n", currentScreenType.c_str());
            } else if (g_oledDoc.containsKey("screen_state")) {
                String newScreenState = g_oledDoc["screen_state"].as<String>();
                if (currentScreenType == "FOOD_MENU" && newScreenState != "FOOD_MENU") {
                    imageAlreadySentThisSession = false;
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
                    Serial.println("🍽️  Ignoring server food icon (just fed, waiting for sync)");
                } else {
                    justFedPet = false;  // Resume accepting server updates
                    if (showFoodIcon != newShowFood) {
                        showFoodIcon = newShowFood;
                        Serial.printf("🍽️  Food Icon: %s\n", showFoodIcon ? "SHOW" : "HIDE");
                    }
                }
            }
            
            if (g_oledDoc.containsKey("show_poop_icon")) {
                showPoopIcon = g_oledDoc["show_poop_icon"].as<bool>();
                Serial.printf("💩 Poop Icon: %s\n", showPoopIcon ? "SHOW" : "HIDE");
            }
            
            // Sick icon — shown after poop ignored >15 min AND poop has been cleared
            if (g_oledDoc.containsKey("show_sick_icon")) {
                bool newSick = g_oledDoc["show_sick_icon"].as<bool>();
                if (showSickIcon != newSick) {
                    showSickIcon = newSick;
                    petIsSick = newSick;  // Sync health menu "Give Med" / "All Good"
                    Serial.printf("❤️  Sick Icon: %s\n", showSickIcon ? "SHOW" : "HIDE");
                }
            }
            
            // Play icon — 1hr after last feed, hidden when sick or poop present
            if (g_oledDoc.containsKey("show_play_icon")) {
                bool newPlay = g_oledDoc["show_play_icon"].as<bool>();
                if (showPlayIcon != newPlay) {
                    showPlayIcon = newPlay;
                    Serial.printf("🎮 Play Icon: %s\n", showPlayIcon ? "SHOW" : "HIDE");
                }
            }
            
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
                    Serial.println("🍽️  Ignoring server hunger status (just fed, waiting for sync)");
                } else {
                    petIsHungry = g_oledDoc["is_hungry"].as<bool>();
                    Serial.printf("🍽️  Hungry: %s\n", petIsHungry ? "YES" : "NO");
                }
            }
            
            if (g_oledDoc.containsKey("current_emotion")) {
                String emotion = g_oledDoc["current_emotion"].as<String>();
                if (currentEmotion != emotion) {
                    currentEmotion = emotion;
                    Serial.printf("😊 Emotion: %s\n", currentEmotion.c_str());
                }
                if (emotion == "EATING" && currentScreenType == "FOOD_MENU") {
                    Serial.println("😋 Emotion: EATING - triggering animation on FOOD MENU!");
                    playEatingAnimation();
                    justFinishedEating = true;
                    eatingFinishTime = millis();
                }
            }

            // is_walking is now driven by hardware step counter in loop()
            // Server is_walking field ignored — hardware has lower latency
            
            // OTA update flag — server sets this when dashboard "Enable OTA" is pressed
            if (g_oledDoc.containsKey("ota_update")) {
                bool otaFlag = g_oledDoc["ota_update"].as<bool>();
                if (otaFlag && !otaUpdateRequested) {
                    otaUpdateRequested = true;
                    Serial.println("🔄 OTA update requested by server!");
                }
            }
            
            // DISABLED: Server menu control — ESP32 controls menu locally via right-tilt gesture
            /*
            if (g_oledDoc.containsKey("current_menu")) {
                String menu = g_oledDoc["current_menu"].as<String>();
                
                // Don't allow server to override menu during feeding gesture
                if (capturingForFeeding) {
                    Serial.println("🍽️ Feeding in progress - ignoring server menu override");
                } else if (currentScreenType != menu) {
                    currentScreenType = menu;
                    Serial.printf("📱 Menu Changed: %s\n", currentScreenType.c_str());
                }
            }
            */
        }
    }
    
    http.end();
}

// ================= OTA FIRMWARE UPDATE =================
// Called from networkTask when ota_update flag is received from server.
// Uses ESP32 dual-partition OTA (automatic A/B swap) — safe rollback on failure.
// Flow: check /api/firmware/latest → if newer → download .bin → flash → reboot
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
    setCpuFrequencyMhz(240);  // Max CPU for OTA
    
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    
    if (!http.begin(firmwareCheckUrl)) {
        Serial.println("❌ OTA: Failed to connect to firmware server");
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    http.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    int httpCode = http.GET();
    
    if (httpCode != 200) {
        Serial.printf("❌ OTA: Firmware check failed, HTTP %d\n", httpCode);
        http.end();
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
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
        setCpuFrequencyMhz(40);
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
        setCpuFrequencyMhz(40);
        return;
    }
    
    const char* newVersion  = otaDoc["version"] | "?";
    const char* downloadUrl = otaDoc["download_url"] | "";
    int fileSize            = otaDoc["file_size"] | 0;
    const char* checksum    = otaDoc["checksum"] | "";
    
    Serial.printf("🆕 OTA: New firmware v%s available (%d bytes)\n", newVersion, fileSize);
    Serial.printf("   Download: %s\n", downloadUrl);
    
    // Step 2: Show downloading status on OLED
    display.clearDisplay();
    display.setCursor(2, 4);
    display.println("UPDATING");
    display.setCursor(2, 16);
    display.print("v");
    display.println(newVersion);
    display.display();
    
    // Step 3: Download and flash firmware using HTTPClient + Update library
    // This uses ESP32's dual-partition OTA — writes to inactive partition, then swaps on reboot
    HTTPClient httpOTA;
    httpOTA.setConnectTimeout(15000);
    httpOTA.setTimeout(60000);  // 60s timeout for large firmware downloads
    
    if (!httpOTA.begin(downloadUrl)) {
        Serial.println("❌ OTA: Failed to connect for firmware download");
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    httpOTA.addHeader("X-OTA-Token", OTA_AUTH_TOKEN);
    int dlCode = httpOTA.GET();
    
    if (dlCode != 200) {
        Serial.printf("❌ OTA: Download failed, HTTP %d\n", dlCode);
        httpOTA.end();
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    int contentLength = httpOTA.getSize();
    if (contentLength <= 0) {
        Serial.println("❌ OTA: Invalid content length");
        httpOTA.end();
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    Serial.printf("📦 OTA: Downloading %d bytes...\n", contentLength);
    
    // Begin OTA update with Update library (ESP32 dual-partition)
    if (!Update.begin(contentLength)) {
        Serial.printf("❌ OTA: Not enough space for update: %s\n", Update.errorString());
        httpOTA.end();
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    WiFiClient *stream = httpOTA.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    int lastPct = -1;
    
    while (httpOTA.connected() && (written < contentLength)) {
        size_t available = stream->available();
        if (available) {
            int bytesRead = stream->readBytes(buf, min((size_t)sizeof(buf), available));
            Update.write(buf, bytesRead);
            written += bytesRead;
            
            int pct = (written * 100) / contentLength;
            if (pct != lastPct && pct % 10 == 0) {
                lastPct = pct;
                Serial.printf("   OTA: %d%%\n", pct);
                // Update OLED progress
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
        }
        vTaskDelay(1);  // Yield to RTOS
    }
    
    httpOTA.end();
    
    if (written != contentLength) {
        Serial.printf("❌ OTA: Download incomplete (%d/%d)\n", written, contentLength);
        Update.abort();
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    if (!Update.end(true)) {
        Serial.printf("❌ OTA: Flash failed: %s\n", Update.errorString());
        otaUpdateRequested = false;
        setCpuFrequencyMhz(40);
        return;
    }
    
    Serial.printf("✅ OTA: Firmware v%s flashed successfully! Rebooting...\n", newVersion);
    
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
    http.setConnectTimeout(2000);  // Reduced from 5s
    http.setTimeout(2000);  // Reduced from 5s
    
    const char* startupUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/device/startup-complete";
    
    if (!http.begin(startupUrl)) {
        Serial.println("❌ Failed to connect for startup notification");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Create startup notification payload
    DynamicJsonDocument doc(256);
    doc["device_id"] = "ESP32_001";
    doc["status"] = "startup_complete";
    doc["timestamp"] = millis();
    doc["pet_stage"] = petAge;  // Send current pet stage
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("📤 Notifying server: Startup complete (payload: %d bytes)\n", payload.length());
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
        String response = http.getString();
        Serial.println("✅ Server acknowledged startup!");
        Serial.printf("   Response: %s\n", response.c_str());
        
        // Parse response - server might send initial state
        DynamicJsonDocument responseDoc(768);
        DeserializationError error = deserializeJson(responseDoc, response);
        
        if (!error) {
            if (responseDoc.containsKey("animation_id")) {
                int animId = responseDoc["animation_id"].as<int>();
                petAge = (PetAge)animId;
                Serial.printf("   ✅ Initial animation set to: %d (INFANT)\n", animId);
            }
            if (responseDoc.containsKey("show_home_icon")) {
                showHomeIcon = responseDoc["show_home_icon"].as<bool>();
                Serial.printf("   Home icon: %s\n", showHomeIcon ? "ENABLED" : "DISABLED");
            }
            
            // DISABLED: Server hunger state (controlled locally by feeding gesture)
            // if (responseDoc.containsKey("show_food_icon")) {
            //     // Ignore during startup or if just fed
            //     if (justFedPet && (millis() - lastFeedTime < FEED_IGNORE_DURATION)) {
            //         Serial.println("🍽️  Ignoring startup food icon (just fed)");
            //     } else {
            //         showFoodIcon = responseDoc["show_food_icon"].as<bool>();
            //         Serial.printf("   Food icon: %s\n", showFoodIcon ? "ENABLED" : "DISABLED");
            //     }
            // }
        } else {
            Serial.printf("⚠️  JSON parse error: %s\n", error.c_str());
        }
        
        // Lock sync to INFANT for first 3 seconds after startup
        vTaskDelay(pdMS_TO_TICKS(3000));
        lastDisplayCheckTime = millis();  // Reset sync timer to prevent early override
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
    
    Serial.printf("✅ Image captured: %d bytes → %d chars base64\n", fb->len, imageB64.length());
    
    esp_camera_fb_return(fb);
    return imageB64;
}

// ================= AUDIO FUNCTIONS =================
// Note: Audio recording is now handled by audioMonitorTask on Core 0
// This function is kept for compatibility but not used in dual-core mode

String recordAudioBase64() {
    Serial.println("⚠️  recordAudioBase64() called, but using VAD on Core 0 instead");
    return "";  // Return empty - audio handled by voice detection
}

// ================= UNIFIED DATA TRANSMISSION =================
bool sendSensorDataOnly(SensorData data) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("❌ WiFi not connected, skipping send\n");
        return false;
    }
    
    HTTPClient http;
    http.setReuse(true);  // Reuse TCP/TLS connection
    http.setConnectTimeout(2000);
    http.setTimeout(5000);
    
    if (!http.begin(serverUrl)) {
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
    g_sensorDoc["chip_temperature"] = data.chip_temperature;  // ESP32 internal temperature
    g_sensorDoc["sleep_seconds"] = accumulatedSleepSec;  // Seconds slept since last upload
    accumulatedSleepSec = 0;  // Reset counter after reporting
    g_sensorDoc["step_count"] = hwStepCount;  // Steps counted on hardware since last send
    hwStepCount = 0;  // Reset after reporting
    
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
    
    if (httpCode == 200) {
        Serial.println("✅ Sensor data sent!");
        Serial.printf("    Accel: X=%.2f, Y=%.2f, Z=%.2f m/s²\n", 
                     data.accel_x, data.accel_y, data.accel_z);
        Serial.printf("    Gyro:  X=%.2f, Y=%.2f, Z=%.2f °/s\n", 
                     data.gyro_x, data.gyro_y, data.gyro_z);
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
        isUploadingImage = false;  // Reset upload flag
        capturingForFeeding = false;  // Reset feeding flag
        return;
    }

    isUploadingImage = true;  // Ensure flag is set (may already be set from gesture)
    
    // Get binary data from Core 0 with mutex protection
    uint8_t* binary_data = NULL;
    size_t data_length = 0;
    
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (capturedImageBuffer != NULL && capturedImageLength > 0) {
            // Copy data for sending
            binary_data = (uint8_t*)malloc(capturedImageLength);
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
        capturingForFeeding = false;  // Reset feeding flag
        return;
    }

    Serial.printf("🖼️ Sending image: %d bytes (raw binary from Core 0)\n", data_length);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);  // Reduced from 30s
    http.setConnectTimeout(5000);  // Reduced from 10s

    if (!http.begin(client, "https://kakuproject-90943350924.asia-south1.run.app/upload")) {
        Serial.println("❌ HTTP begin failed");
        free(binary_data);
        isUploadingImage = false;
        capturingForFeeding = false;  // Reset feeding flag
        return;
    }

    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-Feeding-Action", "true");  // Tell server this is a feeding action

    int httpCode = http.sendRequest("POST", binary_data, data_length);

    if (httpCode == 200) {
        Serial.println("✅ Image uploaded successfully");
        
        // Reset feeding flag
        capturingForFeeding = false;
        
        // Reset hunger indicators locally (server will sync)
        showFoodIcon = false;
        petIsHungry = false;
        justFedPet = true;  // Ignore server updates for 10 seconds
        lastFeedTime = millis();
        Serial.println("🍽️ Feeding complete - hunger reset locally (ignoring server for 10s)");
        
        // Trigger eating-finished state to show GOOD! text
        // Image upload = feeding complete (the frame IS the food)
        if (currentScreenType == "FOOD_MENU") {
            justFinishedEating = true;
            eatingFinishTime = millis();
            Serial.println("🎉 Feeding complete! Showing GOOD! text...");
        }
    } else {
        Serial.printf("❌ Upload failed: %d (%s)\n",
                      httpCode,
                      http.errorToString(httpCode).c_str());
        
        // Reset feeding flag even on failure
        capturingForFeeding = false;
    }

    http.end();
    free(binary_data);

    isUploadingImage = false;
}

void sendAudioData(String audioBase64) {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (audioBase64.length() == 0) {
        Serial.println("⚠️  Audio data empty, skipping");
        return;
    }
    
    Serial.printf("🎵 Sending audio: %d bytes base64\n", audioBase64.length());
    
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(15000);  // Shorter timeout for smaller audio
    
    if (!http.begin("https://kakuproject-90943350924.asia-south1.run.app/upload-audio")) {
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
    
    if (httpCode == 200) {
        Serial.println("✅ Audio data sent!");
    } else {
        Serial.printf("❌ Audio send failed: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
    }
    
    http.end();
    vTaskDelay(pdMS_TO_TICKS(200));  // Server breathing room after audio upload (non-blocking)
}

void sendAllDataToServer(SensorData data) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("❌ WiFi not connected (status=%d), skipping data send\n", WiFi.status());
        return;
    }
    
    Serial.printf("🌐 Connecting to server: %s\n", serverUrl);
    
    HTTPClient http;
    http.setConnectTimeout(10000);  // 10 second connection timeout
    http.setTimeout(15000);          // 15 second read timeout (for large payloads)
    
    if (!http.begin(serverUrl)) {
        Serial.println("❌ Failed to begin HTTP connection");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Create comprehensive JSON payload
    StaticJsonDocument<8192> jsonDoc;  // Increased size for image/audio data
    
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
    
    String payload;
    serializeJson(jsonDoc, payload);
    
    Serial.printf("\n📊 Sending data: %d bytes\n", payload.length());
    Serial.printf("    Sensors: ✅ | Image: %s | Audio: %s\n",
                  data.has_new_image ? "✅" : "⬜",
                  data.has_new_audio ? "✅" : "⬜");
    Serial.println("⏳ Waiting for server response...");
    
    int httpCode = http.POST(payload);
    
    if (httpCode > 0) {
        Serial.printf("📤 POST Response: %d\n", httpCode);
        if (httpCode == 200) {
            Serial.println("✅ All data sent successfully!");
            Serial.printf("    Accel: X=%.2f, Y=%.2f, Z=%.2f m/s²\n", 
                         data.accel_x, data.accel_y, data.accel_z);
            Serial.printf("    Gyro:  X=%.2f, Y=%.2f, Z=%.2f °/s\n", 
                         data.gyro_x, data.gyro_y, data.gyro_z);
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
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate) {
    uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
    uint32_t byte_rate = sample_rate * SAMPLE_BITS / 8;

    const uint8_t header[] = {
        'R','I','F','F',
        file_size, file_size >> 8, file_size >> 16, file_size >> 24,
        'W','A','V','E','f','m','t',' ',
        0x10,0x00,0x00,0x00,
        0x01,0x00,
        0x01,0x00,
        sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24,
        byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24,
        0x02,0x00,
        0x10,0x00,
        'd','a','t','a',
        wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24,
    };

    memcpy(wav_header, header, sizeof(header));
}

// ================= EVENT POLLING FUNCTIONS =================
void pollForEvents() {
    HTTPClient http;
    http.setReuse(true);  // Reuse TCP/TLS connection
    
    Serial.println("🔍 Polling for important events...");
    
    if (!http.begin(eventsUrl)) {
        Serial.println("❌ Failed to initialize HTTP client for events");
        return;
    }
    
    // Set timeout
    http.setTimeout(5000);
    
    // Add headers
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "ESP32-Dashboard/2.0");
    
    // Make GET request
    int httpCode = http.GET();
    
    if (httpCode > 0) {
        Serial.printf("📡 Server response: %d\n", httpCode);
        
        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            
            if (response.length() > 0) {
                Serial.println("📋 Server Response:");
                Serial.println("-------------------");
                
                // Parse JSON response
                DynamicJsonDocument doc(2048);
                DeserializationError error = deserializeJson(doc, response);
                
                if (!error) {
                    if (doc.containsKey("events")) {
                        JsonArray events = doc["events"];
                        
                        if (events.size() > 0) {
                            Serial.printf("🚨 FOUND %d IMPORTANT EVENT(S):\n", events.size());
                            Serial.println("========================================");
                            
                            for (int i = 0; i < events.size(); i++) {
                                JsonObject event = events[i];
                                
                                int event_id = event["id"].as<int>();
                                const char* event_type = event["event_type"];
                                const char* message = event["message"];
                                const char* created_at = event["created_at"];
                                
                                Serial.printf("   🚨 EVENT #%d:\n", i + 1);
                                Serial.printf("     ID: %d\n", event_id);
                                Serial.printf("     Type: %s\n", event_type);
                                Serial.printf("     Message: %s\n", message);
                                Serial.printf("     Time: %s\n", created_at);
                                Serial.println();
                                
                                // Process the event
                                processEvent(event_type, message);
                                
                                // Acknowledge event received
                                acknowledgeEvent(event_id);
                                
                                vTaskDelay(pdMS_TO_TICKS(100));  // Small delay between events
                            }
                        } else {
                            Serial.println("✅ No new important events (all quiet)");
                        }
                    }
                    
                    if (doc.containsKey("message")) {
                        Serial.printf("💬 Status: %s\n", doc["message"].as<const char*>());
                    }
                } else {
                    Serial.println("❌ JSON parsing error");
                    Serial.println(response);
                }
                
                Serial.println("-------------------");
            } else {
                Serial.println("✅ Empty response (no events)");
            }
        } else {
            Serial.printf("⚠️ Unexpected response code: %d\n", httpCode);
        }
    } else {
        Serial.printf("❌ HTTP request failed: %s\n", http.errorToString(httpCode).c_str());
        Serial.println("   Server might be down or unreachable");
    }
    
    http.end();
    Serial.println("");
}

// Send cleaning request to server (remove poop)
void sendCleanRequest() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi not connected, cannot send cleaning request");
        return;
    }
    
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(5000);
    
    String cleanUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/pet/clean";
    Serial.println("🧹 Sending cleaning request to server...");
    
    if (!http.begin(cleanUrl)) {
        Serial.println("❌ Failed to initialize HTTP client for cleaning");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Empty POST body
    int httpCode = http.POST("{}");
    
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
        Serial.printf("❌ Cleaning request failed: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
}

// ================= SEND INJECTION REQUEST =================
// Called after injection animation completes — clears sick_pending on server, resumes hunger
// ================= SEND COVER HAPPY REQUEST =================
// Called each time right-tilt gesture cycles the menu — happiness +5 on server
void sendCoverHappyRequest() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(5000);
    
    String url = "https://kakuproject-90943350924.asia-south1.run.app/api/pet/cover-happy";
    if (!http.begin(url)) return;
    
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST("{}");
    
    if (httpCode == HTTP_CODE_OK) {
        Serial.println("\U0001f600 Cover detected → happiness +5 sent to server");
    } else {
        Serial.printf("\u26a0\ufe0f cover-happy response: %d\n", httpCode);
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
    
    String injectUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/pet/inject";
    Serial.println("💉 Sending injection request to server...");
    
    if (!http.begin(injectUrl)) {
        Serial.println("❌ Failed to initialize HTTP client for injection");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST("{}");
    
    if (httpCode > 0) {
        Serial.printf("📡 Injection server response: %d\n", httpCode);
        if (httpCode == HTTP_CODE_OK) {
            Serial.println("✅ Server injection acknowledged — sick_pending cleared, hunger resumes");
        } else {
            Serial.printf("⚠️ Injection unexpected response: %d\n", httpCode);
        }
    } else {
        Serial.printf("❌ Injection request failed: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
}

void processEvent(const char* event_type, const char* message) {
    Serial.println("🔧 Processing event...");
    
    // Simple event processing - you can extend this based on your needs
    if (strcmp(event_type, "high_sound") == 0) {
        Serial.println("🔊 High sound detected - might want to take action!");
        digitalWrite(LED_PIN, HIGH);  // Turn on LED for high sound
        vTaskDelay(pdMS_TO_TICKS(200));
        digitalWrite(LED_PIN, LOW);   // Blink LED
    }
    else if (strcmp(event_type, "sudden_motion") == 0) {
        Serial.println("🏃 Sudden motion detected - something's happening!");
        // Blink LED multiple times for motion
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(100));
            digitalWrite(LED_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    else if (strcmp(event_type, "alert") == 0) {
        Serial.println("⚠️ Generic alert received!");
        digitalWrite(LED_PIN, HIGH);  // Solid LED for alert
        vTaskDelay(pdMS_TO_TICKS(500));
        digitalWrite(LED_PIN, LOW);
    }
    else {
        Serial.printf("❓ Unknown event type: %s\n", event_type);
    }
    
    Serial.printf("   📝 Event message: %s\n", message);
}

void acknowledgeEvent(int event_id) {
    HTTPClient http;
    http.setReuse(true);  // Reuse TCP/TLS connection
    
    Serial.printf("📤 Acknowledging event #%d...\n", event_id);
    
    if (!http.begin(eventReceivedUrl)) {
        Serial.println("❌ Failed to initialize HTTP client for acknowledgment");
        return;
    }
    
    // Set timeout and headers
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "ESP32-Dashboard/2.0");
    
    // Create JSON payload
    DynamicJsonDocument doc(256);
    doc["device_id"] = "ESP32_001";
    doc["event_id"] = event_id;
    doc["status"] = "received";
    doc["timestamp"] = millis();  // Simple timestamp
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Send POST request
    int httpCode = http.POST(jsonString);
    
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK || httpCode == 200) {
            String response = http.getString();
            Serial.printf("✅ Event acknowledged successfully: %s\n", response.c_str());
        } else {
            Serial.printf("⚠️ Acknowledgment response: %d\n", httpCode);
        }
    } else {
        Serial.printf("❌ Acknowledgment failed: %s\n", http.errorToString(httpCode).c_str());
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
