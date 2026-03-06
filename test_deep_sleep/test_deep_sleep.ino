/*
  ESP32-S3 Deep Sleep + MPU6050 Motion Wake-Up (ISR-based)
  =========================================================
  Uses Adafruit_MPU6050 — same approach as test_int_pin.

  While AWAKE : hardware ISR fires on RISING edge (active HIGH, latched)
  Deep sleep  : MPU reconfigured to active LOW → ext1 wakeup on LOW

  Hardware: XIAO ESP32 S3 Sense + MPU6050
  I2C:      SDA = GPIO5, SCL = GPIO6
  MPU INT:  GPIO2 / D1

  Behavior:
  - Boot → init MPU → attach RISING ISR → loop every 500ms.
  - ISR fires instantly on motion → resets 10s inactivity timer.
  - No motion for 10s → reconfigure MPU active LOW → deep sleep.
  - Shake → MPU INT goes LOW → ESP32 wakes via ext1 → restart.

  Required libraries: Adafruit MPU6050, Adafruit Unified Sensor
*/

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include "driver/rtc_io.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ====== OLED CONFIG ======
#define SCREEN_WIDTH   64
#define SCREEN_HEIGHT  32
#define OLED_ADDR      0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ====== PIN CONFIG ======
#define I2C_SDA          5
#define I2C_SCL          6
#define MPU_INT_PIN      2   // GPIO2 (D1) connected to MPU6050 INT pin

// ====== BUILT-IN LED (turn off to save power) ======
#define LED_BUILTIN_PIN  21  // XIAO ESP32 S3 built-in orange LED
// NOTE: GPIO2 is now MPU6050 INT — no longer used as LED

// ====== TIMING ======
#define INACTIVITY_TIMEOUT_MS  10000  // 10 seconds of no motion → deep sleep
#define MOTION_THRESHOLD       10     // LSBs, ~40mg @ ±2g (same as test_int_pin)
#define MOTION_DURATION        20     // samples motion must persist

// ====== GLOBALS ======
Adafruit_MPU6050 mpu;
sensors_event_t a, g, temp;

volatile bool          motionDetected = false;
volatile unsigned long isrTime        = 0;
volatile unsigned long isrCount       = 0;

unsigned long lastMotionTime = 0;

// -------------------------------------------------------
// ISR — fires on RISING edge when MPU INT goes HIGH
// -------------------------------------------------------
void IRAM_ATTR onMotionInterrupt() {
  motionDetected = true;
  isrTime        = millis();
  isrCount++;
}

// -------------------------------------------------------
// Show a message on the OLED (centered, with optional second line)
// -------------------------------------------------------
void oledShow(const char* line1, const char* line2 = nullptr) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Center line 1
  int16_t x1, y1;
  uint16_t w1, h1;
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);
  int yPos = line2 ? 4 : 12;
  display.setCursor((SCREEN_WIDTH - w1) / 2, yPos);
  display.print(line1);

  if (line2) {
    uint16_t w2, h2;
    display.getTextBounds(line2, 0, 0, &x1, &y1, &w2, &h2);
    display.setCursor((SCREEN_WIDTH - w2) / 2, 20);
    display.print(line2);
  }
  display.display();
}

// -------------------------------------------------------
// Print the reason the ESP32 woke up
// -------------------------------------------------------
void printWakeUpReason() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wake-up: EXT0 (GPIO interrupt)");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Wake-up: EXT1 (GPIO interrupt)");
      break;
    case ESP_SLEEP_WAKEUP_GPIO:
      Serial.println("Wake-up: GPIO deep-sleep wakeup");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wake-up: Timer");
      break;
    default:
      Serial.printf("Wake-up: Power-on / Reset (cause=%d)\n", cause);
      break;
  }
}

// -------------------------------------------------------
// Configure MPU6050 for awake operation:
// Active HIGH, latched — ISR fires on RISING edge
// -------------------------------------------------------
void configureMPU6050Awake() {
  mpu.setHighPassFilter(MPU6050_HIGHPASS_5_HZ);

  const uint8_t motionThreshold = MOTION_THRESHOLD;
  mpu.setMotionDetectionThreshold(motionThreshold);

  const uint8_t motionDuration = MOTION_DURATION;
  mpu.setMotionDetectionDuration(motionDuration);

  mpu.setInterruptPinLatch(true);       // stay HIGH until INT_STATUS is read
  mpu.setInterruptPinPolarity(false);   // false = active HIGH
  mpu.setMotionInterrupt(true);

  Serial.printf("MPU awake config: threshold=%d LSBs, duration=%d samples, active HIGH\n",
                MOTION_THRESHOLD, MOTION_DURATION);
}

// -------------------------------------------------------
// Reconfigure MPU6050 for deep-sleep wakeup:
// Active LOW, pulse — ext1 wakes ESP32 when INT goes LOW
// -------------------------------------------------------
void configureMPU6050ForSleep() {
  mpu.setHighPassFilter(MPU6050_HIGHPASS_5_HZ);
  mpu.setMotionDetectionThreshold(MOTION_THRESHOLD);
  mpu.setMotionDetectionDuration(MOTION_DURATION);
  mpu.setInterruptPinLatch(false);      // pulse — avoids INT stuck LOW during sleep
  mpu.setInterruptPinPolarity(true);    // true = active LOW
  mpu.setMotionInterrupt(true);

  // Clear any pending interrupt
  mpu.getMotionInterruptStatus();

  Serial.println("MPU sleep config: active LOW, pulse mode");
}

// -------------------------------------------------------
// Enter deep sleep, waking on MPU6050 INT pin going LOW
// -------------------------------------------------------
void enterDeepSleep() {
  Serial.println("========================================");
  Serial.println("  No motion for 10s — entering DEEP SLEEP");
  Serial.println("  Shake/move device to wake up!");
  Serial.println("========================================");
  Serial.flush();

  // Show sleep message on OLED
  oledShow("Zzz...", "Shake to wake");
  delay(500);
  display.clearDisplay();
  display.display();  // Blank OLED before sleep

  // Detach awake ISR before reconfiguring MPU
  detachInterrupt(digitalPinToInterrupt(MPU_INT_PIN));

  // Reconfigure MPU for active-LOW wakeup
  configureMPU6050ForSleep();
  delay(50);

  Serial.printf("[DEBUG] INT pin after sleep config: %s\n",
                digitalRead(MPU_INT_PIN) ? "HIGH (ok)" : "LOW (active!)");

  // Wait for INT pin to go HIGH (idle) before sleeping
  // If stuck LOW, ESP32 would wake immediately
  pinMode(MPU_INT_PIN, INPUT_PULLUP);
  int retries = 20;
  while (digitalRead(MPU_INT_PIN) == LOW && retries > 0) {
    Serial.printf("[DEBUG] INT pin still LOW (retries left: %d), clearing...\n", retries);
    mpu.getMotionInterruptStatus();
    delay(100);
    retries--;
  }

  if (digitalRead(MPU_INT_PIN) == LOW) {
    Serial.println("WARNING: INT pin stuck LOW! Wake may be instant.");
  } else {
    Serial.println("INT pin is HIGH — safe to sleep.");
  }

  // --- Force LED off before sleeping ---
  digitalWrite(LED_BUILTIN_PIN, HIGH);  // Active LOW — HIGH = OFF

  // --- Configure deep-sleep wake-up source ---
  esp_sleep_enable_ext1_wakeup(1ULL << MPU_INT_PIN, ESP_EXT1_WAKEUP_ALL_LOW);
  Serial.println("Wakeup source: ext1 (LOW on GPIO2/D1)");

  Serial.flush();
  delay(100);

  esp_deep_sleep_start();
  // Device is now in deep sleep — execution stops here
}

// -------------------------------------------------------
// Direction detection — exact copy from esp32_sketch_test.ino
// Uses m/s² values from Adafruit getEvent()
// -------------------------------------------------------
const char* detectDirection(float ax, float ay, float az) {
  float abs_ax = abs(ax);
  float abs_ay = abs(ay);
  float abs_az = abs(az);

  if (abs_az > abs_ax && abs_az > abs_ay) {
    if (az < -7.0f)      return "INVERTED";
    if (az > 7.0f)       return "NEUTRAL";
  }
  if (abs_ax > abs_ay && abs_ax > abs_az) {
    if (ax > 5.0f)       return "RIGHT";
    if (ax < -5.0f)      return "LEFT";
  }
  if (abs_ay > abs_ax && abs_ay > abs_az) {
    if (ay > 5.0f)       return "BACK";
    if (ay < -5.0f)      return "FORWARD";
  }
  return "NEUTRAL";
}

// -------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);  // Let serial settle after deep-sleep wake

  Serial.println("\n====================================");
  Serial.println("  Deep Sleep + MPU6050 ISR Wake-Up");
  Serial.println("====================================");

  printWakeUpReason();

  // --- Turn off built-in LED ---
  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, HIGH);  // HIGH = OFF on XIAO ESP32 S3 (active LOW)
  Serial.println("Built-in LED turned OFF");

  Wire.begin(I2C_SDA, I2C_SCL);

  // Init OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed!");
  } else {
    Serial.println("OLED OK");
    oledShow("WAKING UP", "...");
  }

  // Init MPU6050 (Adafruit)
  if (!mpu.begin(0x68, &Wire)) {
    Serial.println("ERROR: MPU6050 not found! Check wiring.");
    Serial.println("  SDA -> GPIO5");
    Serial.println("  SCL -> GPIO6");
    Serial.println("  INT -> GPIO2 (D1)");
    while (true) delay(10);
  }
  Serial.println("MPU6050 OK");

  // Configure motion interrupt for awake mode (active HIGH, latched, RISING ISR)
  configureMPU6050Awake();

  // Clear any stale interrupt before attaching ISR
  Serial.printf("[DEBUG] INT pin before clear: %s\n",
                digitalRead(MPU_INT_PIN) ? "HIGH" : "LOW");
  mpu.getMotionInterruptStatus();  // clears any latched interrupt
  Serial.printf("[DEBUG] INT pin after clear : %s\n",
                digitalRead(MPU_INT_PIN) ? "HIGH (ok)" : "LOW (active!)");

  // Attach hardware interrupt — RISING: LOW → HIGH = motion fired
  pinMode(MPU_INT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(MPU_INT_PIN), onMotionInterrupt, RISING);

  lastMotionTime = millis();

  Serial.printf("Inactivity timeout: %d seconds\n", INACTIVITY_TIMEOUT_MS / 1000);
  Serial.println("ISR fires on motion — shake to reset timer!\n");

  oledShow("AWAKE", "Move me!");
}

// -------------------------------------------------------
void loop() {
  // --- Handle motion interrupt (ISR fired) ---
  if (motionDetected) {
    motionDetected = false;
    int pinBeforeClear = digitalRead(MPU_INT_PIN);    // read BEFORE clearing latch
    mpu.getMotionInterruptStatus();                   // clears the latch
    mpu.getEvent(&a, &g, &temp);

    lastMotionTime = millis();

    const char* dir = detectDirection(a.acceleration.x, a.acceleration.y, a.acceleration.z);

    Serial.printf("\n>>> INTERRUPT #%lu  at %lums  PIN=%s  dir=%s\n",
                  isrCount, isrTime,
                  pinBeforeClear ? "HIGH" : "LOW", dir);
    Serial.printf("    ax=%.2f  ay=%.2f  az=%.2f m/s²\n\n",
                  a.acceleration.x, a.acceleration.y, a.acceleration.z);

    oledShow("MOTION!", dir);
  } else {
    // --- Baseline poll every 500ms ---
    mpu.getEvent(&a, &g, &temp);
    int pin = digitalRead(MPU_INT_PIN);

    const char* dir = detectDirection(a.acceleration.x, a.acceleration.y, a.acceleration.z);

    unsigned long idle = millis() - lastMotionTime;
    unsigned long remaining = (idle < INACTIVITY_TIMEOUT_MS)
                              ? (INACTIVITY_TIMEOUT_MS - idle) / 1000
                              : 0;

    Serial.printf("[%7lums]  PIN=%-4s  dir=%-8s  idle=%lus  sleep=%lus  ax=%5.2f  ay=%5.2f  az=%5.2f\n",
                  millis(),
                  pin ? "HIGH" : "LOW",
                  dir,
                  idle / 1000,
                  remaining,
                  a.acceleration.x, a.acceleration.y, a.acceleration.z);

    char line1[16], line2[16];
    snprintf(line1, sizeof(line1), "%s", dir);
    snprintf(line2, sizeof(line2), "Sleep: %lus", remaining);
    oledShow(line1, line2);
  }

  // Check inactivity timeout
  if (millis() - lastMotionTime >= INACTIVITY_TIMEOUT_MS) {
    enterDeepSleep();
    // Never reaches here
  }

  delay(500);
}
