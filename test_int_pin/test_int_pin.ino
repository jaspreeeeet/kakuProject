/*
  MPU6050 Motion Interrupt Test — Adafruit Library
  =================================================
  Uses Adafruit_MPU6050 for init + motion interrupt configuration.
  INT pin fires on RISING edge (active HIGH) when motion is detected.

  Wiring: MPU6050 INT → GPIO2 (D1)
          SDA → GPIO5,  SCL → GPIO6

  Required libraries: Adafruit MPU6050, Adafruit Unified Sensor
*/

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

#define I2C_SDA   5
#define I2C_SCL   6
#define INT_PIN   2

Adafruit_MPU6050 mpu;
sensors_event_t a, g, temp;

// ISR flag
volatile bool motionDetected = false;
volatile unsigned long isrTime  = 0;
volatile unsigned long isrCount = 0;

// -------------------------------------------------------
// ISR — fires on RISING edge when MPU INT goes HIGH
// -------------------------------------------------------
void IRAM_ATTR onMotionInterrupt() {
  motionDetected = true;
  isrTime = millis();
  isrCount++;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== MPU6050 Motion Interrupt Test (Adafruit) ===");

  Wire.begin(I2C_SDA, I2C_SCL);

  if (!mpu.begin(0x68, &Wire)) {
    Serial.println("ERROR: MPU6050 not found! Check wiring.");
    while (true) delay(10);
  }
  Serial.println("MPU6050 found OK");

  // --- Motion interrupt configuration ---
  // High-pass filter removes DC (gravity) so only AC (movement) triggers
  mpu.setHighPassFilter(MPU6050_HIGHPASS_5_HZ);

  // Threshold: 1 LSB = ~4mg @ ±2g range → 10 = ~40mg, needs deliberate shake
  const uint8_t motionThreshold = 10;
  mpu.setMotionDetectionThreshold(motionThreshold);

  // Duration: motion must persist this many ODR samples to count
  const uint8_t motionDuration = 20;
  mpu.setMotionDetectionDuration(motionDuration);

  // INT pin: true = latch HIGH until INT_STATUS (reg 0x3A) is read
  mpu.setInterruptPinLatch(true);

  // Polarity: false = active HIGH (pin goes HIGH on interrupt)
  mpu.setInterruptPinPolarity(false);

  // Enable motion interrupt
  mpu.setMotionInterrupt(true);

  Serial.println("Motion interrupt configured");
  Serial.printf("  Threshold : %d LSBs\n", motionThreshold);
  Serial.printf("  Duration  : %d samples\n", motionDuration);
  Serial.printf("  INT pin   : %s\n", digitalRead(INT_PIN) ? "HIGH (active!)" : "LOW (idle, ok)");
  Serial.println();

  // INT pin: active HIGH, no pull needed (MPU drives it)
  pinMode(INT_PIN, INPUT);

  // Clear any stale interrupt before attaching
  mpu.getMotionInterruptStatus();

  // Attach hardware interrupt — RISING: LOW → HIGH = motion fired
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onMotionInterrupt, RISING);

  Serial.println("Watching for motion on INT pin (GPIO2)...");
  Serial.println("Keep still — only deliberate shakes should trigger.");
  Serial.println("[ms]  PIN   Motion?  ax      ay      az\n");
}

void loop() {
  // --- Handle interrupt event ---
  if (motionDetected) {
    motionDetected = false;
    int pinBeforeClear = digitalRead(INT_PIN);       // read BEFORE clearing latch
    bool motionStatus = mpu.getMotionInterruptStatus(); // clears the latch
    mpu.getEvent(&a, &g, &temp);

    Serial.printf("\n>>> INTERRUPT #%lu  at %lums  motionStatus=%d  PIN=%s\n",
                  isrCount, isrTime, motionStatus,
                  pinBeforeClear ? "HIGH" : "LOW");
    Serial.printf("    ax=%.2f  ay=%.2f  az=%.2f m/s²\n\n",
                  a.acceleration.x, a.acceleration.y, a.acceleration.z);
  }

  // --- Baseline poll every 500ms ---
  mpu.getEvent(&a, &g, &temp);
  int pin = digitalRead(INT_PIN);

  Serial.printf("[%7lums]  PIN=%-4s  motion=%-3s  ax=%6.2f  ay=%6.2f  az=%6.2f\n",
                millis(),
                pin ? "HIGH" : "LOW",
                mpu.getMotionInterruptStatus() ? "YES" : "no",
                a.acceleration.x, a.acceleration.y, a.acceleration.z);

  delay(500);
}
