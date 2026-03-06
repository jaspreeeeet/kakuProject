/*
 * ═══════════════════════════════════════════════════════════════════
 *  TEST: Camera Power-Down / On-Demand Capture (XIAO ESP32 S3 Sense)
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Purpose:
 *    Verify that the OV2640 can be kept fully OFF at boot and only
 *    initialized when a "capture" command arrives over Serial.
 *    After capture the camera is deinited and all pins returned to
 *    low-power state.
 *
 *  Expected serial output:
 *    Booting...
 *    Camera kept in power-down mode
 *    Waiting for serial command...
 *
 *    > capture
 *    Initializing camera...
 *    Capturing image...
 *    Image captured. Size: XXXX bytes
 *    Deinitializing camera...
 *    Camera returned to low-power mode
 *    Waiting for serial command...
 *
 *  Board: XIAO ESP32 S3 Sense
 *  Library: esp_camera (bundled with ESP32-S3 Arduino core)
 */

#include "esp_camera.h"

// ═══════════════════════════════════════════════════════════════════
// Camera pins — same as main firmware (XIAO ESP32 S3 Sense)
// ═══════════════════════════════════════════════════════════════════
#define PWDN_GPIO_NUM -1  // No hardware power-down pin on this board
#define RESET_GPIO_NUM -1 // No hardware reset pin
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

// All data + control pins in one array for easy bulk low-power reset
static const int cameraPins[] = {
    XCLK_GPIO_NUM, SIOD_GPIO_NUM,  SIOC_GPIO_NUM, Y9_GPIO_NUM,  Y8_GPIO_NUM,
    Y7_GPIO_NUM,   Y6_GPIO_NUM,    Y5_GPIO_NUM,   Y4_GPIO_NUM,  Y3_GPIO_NUM,
    Y2_GPIO_NUM,   VSYNC_GPIO_NUM, HREF_GPIO_NUM, PCLK_GPIO_NUM};
static const int cameraPinCount = sizeof(cameraPins) / sizeof(cameraPins[0]);

// ═══════════════════════════════════════════════════════════════════
// Force all camera-related GPIOs into INPUT (high-impedance).
// This prevents any GPIO from sourcing/sinking current while the
// camera driver is not running.
//
// XCLK is specifically called out because the LEDC peripheral may
// leave the pin driving a clock signal even after esp_camera_deinit().
// ═══════════════════════════════════════════════════════════════════
void setCameraPinsLowPower() {
  for (int i = 0; i < cameraPinCount; i++) {
    gpio_reset_pin((gpio_num_t)cameraPins[i]); // detach from any peripheral
    pinMode(cameraPins[i], INPUT);             // high-impedance → ~0 µA
  }
  Serial.println("  Pins → INPUT (high-impedance)");
}

// ═══════════════════════════════════════════════════════════════════
// Initialize → capture one frame → deinit → return to low-power
// ═══════════════════════════════════════════════════════════════════
void captureOnce() {

  // ── 1. INIT ──────────────────────────────────────────────────────
  Serial.println("Initializing camera...");

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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  // Same parameters as main firmware
  config.xclk_freq_hz = 10000000; // 10 MHz XCLK
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QQVGA; // 160×120
  config.jpeg_quality = 20;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED (0x%x)\n", err);
    // Still clean up pins even on failure
    setCameraPinsLowPower();
    return;
  }
  Serial.println("Camera initialized OK");

  // ── 2. CAPTURE ───────────────────────────────────────────────────
  Serial.println("Capturing image...");

  // Discard first frame (often garbled / auto-exposure settling)
  camera_fb_t *discard = esp_camera_fb_get();
  if (discard)
    esp_camera_fb_return(discard);

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    Serial.printf("Image captured. Size: %u bytes (%ux%u)\n", fb->len,
                  fb->width, fb->height);
    esp_camera_fb_return(fb);
  } else {
    Serial.println("Image capture FAILED (fb == NULL)");
  }

  // ── 3. DEINIT ────────────────────────────────────────────────────
  //  esp_camera_deinit() stops the I2S/DMA engine and frees the
  //  frame buffers, but does NOT stop the LEDC clock on XCLK.
  Serial.println("Deinitializing camera...");
  esp_camera_deinit();

  // ── 4. LOW-POWER RESET ───────────────────────────────────────────
  //  gpio_reset_pin() detaches the LEDC peripheral from XCLK,
  //  stopping the 10 MHz clock that would otherwise keep drawing
  //  current through the OV2640.
  //  All data/sync pins are also set to INPUT so nothing is driven.
  setCameraPinsLowPower();
  Serial.println("Camera returned to low-power mode");
}

// ═══════════════════════════════════════════════════════════════════
// SETUP — boot into lowest-power camera state
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1500); // let USB-CDC enumerate

  Serial.println("\n========================================");
  Serial.println("  Camera Power-Down Test");
  Serial.println("  Board: XIAO ESP32 S3 Sense");
  Serial.println("========================================\n");

  Serial.println("Booting...");

  // Camera is NOT initialized here.
  // Force all camera GPIOs to INPUT so they draw no current.
  setCameraPinsLowPower();

  Serial.println("Camera kept in power-down mode");
  Serial.printf("Free heap: %u bytes | PSRAM: %u bytes\n", ESP.getFreeHeap(),
                ESP.getFreePsram());
  Serial.println("\nWaiting for serial command...");
  Serial.println("  Type 'capture' + Enter to take a photo\n");
}

// ═══════════════════════════════════════════════════════════════════
// LOOP — minimal serial command parser
// ═══════════════════════════════════════════════════════════════════
void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.equalsIgnoreCase("capture")) {
      unsigned long t0 = millis();
      captureOnce();
      Serial.printf("Total capture cycle: %lu ms\n", millis() - t0);
      Serial.printf("Free heap: %u bytes | PSRAM: %u bytes\n",
                    ESP.getFreeHeap(), ESP.getFreePsram());
      Serial.println("\nWaiting for serial command...\n");
    } else if (cmd.length() > 0) {
      Serial.printf("Unknown command: '%s'\n", cmd.c_str());
      Serial.println("Available commands: capture");
    }
  }
  delay(50); // keep loop() idle — low CPU usage
}
