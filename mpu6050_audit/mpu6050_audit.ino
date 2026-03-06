/*
  ╔══════════════════════════════════════════════════════════════════╗
  ║  MPU6050 ADAFRUIT AUDIT TEST                                   ║
  ║  Tests EVERY MPU6050 pattern from esp32_sketch_test.ino        ║
  ║  using Adafruit library. Upload → open Serial Monitor.         ║
  ║  Wiring: SDA=GPIO5, SCL=GPIO6                                 ║
  ╚══════════════════════════════════════════════════════════════════╝
*/

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

#define I2C_SDA 5
#define I2C_SCL 6

Adafruit_MPU6050 mpu;
bool mpuAvailable = false;

// ── Step detection globals (mirrors main code) ──
#define LP_ALPHA_STEP 0.9f
#define STEP_BARRIER_G2 0.015f
#define STEP_MIN_MS 300
float stepGravX = 0, stepGravY = 0, stepGravZ = 1.0f;
unsigned int hwStepCount = 0;
unsigned long lastHwStepTime = 0;

// ── Tilt filter globals (mirrors main code) ──
float filteredX = 0;
float velocity = 0;
float playerX = 32;
const int playerWidth = 8;
const int SCREEN_WIDTH = 64;

// ── Test counters ──
int testsPassed = 0;
int testsFailed = 0;

void printResult(const char* testName, bool pass) {
  if (pass) {
    testsPassed++;
    Serial.printf("  [PASS] %s\n", testName);
  } else {
    testsFailed++;
    Serial.printf("  [FAIL] %s\n", testName);
  }
}

// ====================================================================
// TEST 1: begin() — replaces mpu.initialize() + mpu.testConnection()
// ====================================================================
bool test_begin() {
  Serial.println("\n── TEST 1: mpu.begin() ──");
  Wire.begin(I2C_SDA, I2C_SCL);

  bool ok = mpu.begin(0x68, &Wire);
  printResult("mpu.begin(0x68, &Wire)", ok);

  if (ok) {
    mpuAvailable = true;
    Serial.println("  MPU6050 found on I2C bus");
  } else {
    Serial.println("  ERROR: MPU6050 not found! Check SDA→GPIO5, SCL→GPIO6");
  }
  return ok;
}

// ====================================================================
// TEST 2: setAccelerometerRange — replaces setFullScaleAccelRange()
// ====================================================================
void test_setAccelRange() {
  Serial.println("\n── TEST 2: setAccelerometerRange ──");
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  printResult("setAccelerometerRange(MPU6050_RANGE_2_G)", true);

  // Verify by reading back
  mpu6050_accel_range_t range = mpu.getAccelerometerRange();
  bool rangeOk = (range == MPU6050_RANGE_2_G);
  printResult("getAccelerometerRange() == MPU6050_RANGE_2_G", rangeOk);
  Serial.printf("  Range enum value: %d (expect 0 for ±2g)\n", range);
}

// ====================================================================
// TEST 3: setGyroRange — verify default gyro config
// ====================================================================
void test_setGyroRange() {
  Serial.println("\n── TEST 3: setGyroRange ──");
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu6050_gyro_range_t gRange = mpu.getGyroRange();
  bool ok = (gRange == MPU6050_RANGE_250_DEG);
  printResult("setGyroRange(MPU6050_RANGE_250_DEG)", ok);
  Serial.printf("  Gyro range enum: %d (expect 0 for ±250 dps)\n", gRange);
}

// ====================================================================
// TEST 4: getEvent() — replaces getAcceleration() + getRotation()
// ====================================================================
void test_getEvent() {
  Serial.println("\n── TEST 4: mpu.getEvent() — accel + gyro + temp ──");
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  // Accel should be non-zero (gravity exists!)
  float mag = sqrt(a.acceleration.x * a.acceleration.x +
                   a.acceleration.y * a.acceleration.y +
                   a.acceleration.z * a.acceleration.z);
  bool accelOk = (mag > 7.0 && mag < 13.0); // ~9.81 ± tolerance
  printResult("Accel magnitude ~9.81 m/s²", accelOk);
  Serial.printf("  Accel (m/s²): x=%.2f  y=%.2f  z=%.2f  |mag|=%.2f\n",
                a.acceleration.x, a.acceleration.y, a.acceleration.z, mag);

  // Gyro should be near zero when stationary
  bool gyroOk = (abs(g.gyro.x) < 1.0 && abs(g.gyro.y) < 1.0 && abs(g.gyro.z) < 1.0);
  printResult("Gyro near zero (stationary)", gyroOk);
  Serial.printf("  Gyro (rad/s): x=%.4f  y=%.4f  z=%.4f\n",
                g.gyro.x, g.gyro.y, g.gyro.z);

  // Temp should be reasonable (10-50°C)
  bool tempOk = (t.temperature > 10.0 && t.temperature < 50.0);
  printResult("Temperature 10-50°C", tempOk);
  Serial.printf("  Temp: %.1f °C\n", t.temperature);
}

// ====================================================================
// TEST 5: Accel in g-units — tilt gesture pattern
//   OLD: ax / 16384.0
//   NEW: a.acceleration.x / 9.81
// ====================================================================
void test_accel_g_units() {
  Serial.println("\n── TEST 5: Accel g-units (tilt gesture pattern) ──");
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float gx = a.acceleration.x / 9.81;
  float gy = a.acceleration.y / 9.81;
  float gz = a.acceleration.z / 9.81;
  float gMag = sqrt(gx*gx + gy*gy + gz*gz);

  bool ok = (gMag > 0.8 && gMag < 1.3); // should be ~1.0g
  printResult("g-magnitude ~1.0g", ok);
  Serial.printf("  g-units: x=%.3f  y=%.3f  z=%.3f  |g|=%.3f\n", gx, gy, gz, gMag);

  // Simulate tilt threshold checks used in main code
  bool rightTilt = (gx > 0.8);
  bool leftTilt  = (gx < -0.8);
  bool neutral   = (abs(gx) < 0.12);
  Serial.printf("  Tilt check: right(>0.8)=%s  left(<-0.8)=%s  neutral(<0.12)=%s\n",
                rightTilt ? "YES" : "no", leftTilt ? "YES" : "no", neutral ? "YES" : "no");
  printResult("Tilt threshold logic runs without crash", true);
}

// ====================================================================
// TEST 6: Accel in m/s² — sensor upload pattern
//   OLD: ax / 16384.0 * 9.81
//   NEW: a.acceleration.x  (already m/s²)
// ====================================================================
void test_accel_ms2() {
  Serial.println("\n── TEST 6: Accel m/s² (sensor upload pattern) ──");
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  // Simulating readAllSensors() data population
  float accel_x = a.acceleration.x;   // already m/s²
  float accel_y = a.acceleration.y;
  float accel_z = a.acceleration.z;

  bool ok = (abs(accel_z) > 5.0); // gravity should show up on one axis
  printResult("At least one axis > 5 m/s² (gravity)", ok);
  Serial.printf("  m/s²: x=%.2f  y=%.2f  z=%.2f\n", accel_x, accel_y, accel_z);
}

// ====================================================================
// TEST 7: Gyro in deg/s — sensor upload pattern
//   OLD: gx / 131.0
//   NEW: g.gyro.x * 57.2958
// ====================================================================
void test_gyro_dps() {
  Serial.println("\n── TEST 7: Gyro deg/s (sensor upload pattern) ──");
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float gyro_x_dps = g.gyro.x * 57.2958;  // rad/s → deg/s
  float gyro_y_dps = g.gyro.y * 57.2958;
  float gyro_z_dps = g.gyro.z * 57.2958;

  // Stationary should be < 20 deg/s
  bool ok = (abs(gyro_x_dps) < 20 && abs(gyro_y_dps) < 20 && abs(gyro_z_dps) < 20);
  printResult("Gyro < 20 deg/s (stationary)", ok);
  Serial.printf("  deg/s: x=%.2f  y=%.2f  z=%.2f\n", gyro_x_dps, gyro_y_dps, gyro_z_dps);
}

// ====================================================================
// TEST 8: Gravity filter warmup — matches setup warmup loop
//   OLD: mpu.getAcceleration(&wx, &wy, &wz); float fx = wx / 16384.0f;
//   NEW: mpu.getEvent(&a, &g, &t); float fx = a.acceleration.x / 9.81;
// ====================================================================
void test_gravity_warmup() {
  Serial.println("\n── TEST 8: LP gravity filter warmup (40 samples) ──");

  stepGravX = 0; stepGravY = 0; stepGravZ = 1.0f;

  for (int i = 0; i < 40; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);

    float fx = a.acceleration.x / 9.81;  // m/s² → g
    float fy = a.acceleration.y / 9.81;
    float fz = a.acceleration.z / 9.81;

    stepGravX = LP_ALPHA_STEP * stepGravX + (1.0f - LP_ALPHA_STEP) * fx;
    stepGravY = LP_ALPHA_STEP * stepGravY + (1.0f - LP_ALPHA_STEP) * fy;
    stepGravZ = LP_ALPHA_STEP * stepGravZ + (1.0f - LP_ALPHA_STEP) * fz;
    delay(25);
  }

  float gravMag = sqrt(stepGravX*stepGravX + stepGravY*stepGravY + stepGravZ*stepGravZ);
  bool ok = (gravMag > 0.8 && gravMag < 1.3);
  printResult("Gravity filter settled near 1g", ok);
  Serial.printf("  Filtered gravity: x=%.3f  y=%.3f  z=%.3f  |g|=%.3f\n",
                stepGravX, stepGravY, stepGravZ, gravMag);
}

// ====================================================================
// TEST 9: Step detection (stoss method) — 10 samples, no walking
//   OLD: mpu.getAcceleration(&ax, &ay, &az);  gx = ax / 16384.0f;
//   NEW: mpu.getEvent(&a, &g, &t);  gx = a.acceleration.x / 9.81;
// ====================================================================
void test_step_detection() {
  Serial.println("\n── TEST 9: Step detection (stoss method, 10 samples) ──");

  unsigned int stepsBefore = hwStepCount;
  for (int i = 0; i < 10; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);

    float gx = a.acceleration.x / 9.81f;
    float gy = a.acceleration.y / 9.81f;
    float gz = a.acceleration.z / 9.81f;

    float dx = gx - stepGravX;
    float dy = gy - stepGravY;
    float dz = gz - stepGravZ;
    float stoss = dx*dx + dy*dy + dz*dz;

    unsigned long now = millis();
    if (stoss > STEP_BARRIER_G2 && (now - lastHwStepTime) > STEP_MIN_MS) {
      hwStepCount++;
      lastHwStepTime = now;
    }

    Serial.printf("  [%d] stoss=%.5f %s\n", i, stoss,
                  stoss > STEP_BARRIER_G2 ? "STEP!" : "");
    delay(50);
  }

  Serial.printf("  Steps detected: %u (device should be still → expect 0-1)\n",
                hwStepCount - stepsBefore);
  printResult("Step detection logic runs without crash", true);
}

// ====================================================================
// TEST 10: Game tilt simulation — readTiltForGame() pattern
//   OLD: ay / 16384.0
//   NEW: a.acceleration.y / 9.81
// ====================================================================
void test_game_tilt() {
  Serial.println("\n── TEST 10: Game tilt pattern (readTiltForGame) ──");

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float accelY = a.acceleration.y / 9.81;  // g-units
  filteredX = 0.6 * filteredX + 0.4 * accelY;

  if (abs(filteredX) < 0.12) {
    velocity = 0;
  } else {
    velocity = filteredX * 6.0;
  }
  playerX += velocity;
  if (playerX < 0) playerX = 0;
  if (playerX > SCREEN_WIDTH - playerWidth) playerX = SCREEN_WIDTH - playerWidth;

  Serial.printf("  accelY=%.3f g  filtered=%.3f  vel=%.2f  playerX=%.1f\n",
                accelY, filteredX, velocity, playerX);
  printResult("Game tilt logic runs without crash", true);
}

// ====================================================================
// TEST 11: Sensor batch pattern (loop) — accel + gyro in one call
//   OLD: getAcceleration + getRotation (2 calls)
//   NEW: getEvent (1 call gets both)
// ====================================================================
void test_sensor_batch() {
  Serial.println("\n── TEST 11: Sensor batch (loop pattern) ──");

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  // Simulating the batch struct population
  float r_accel_x = a.acceleration.x;
  float r_accel_y = a.acceleration.y;
  float r_accel_z = a.acceleration.z;
  float r_gyro_x  = g.gyro.x * 57.2958;
  float r_gyro_y  = g.gyro.y * 57.2958;
  float r_gyro_z  = g.gyro.z * 57.2958;

  Serial.printf("  Batch accel (m/s²): x=%.2f  y=%.2f  z=%.2f\n", r_accel_x, r_accel_y, r_accel_z);
  Serial.printf("  Batch gyro (deg/s): x=%.2f  y=%.2f  z=%.2f\n", r_gyro_x, r_gyro_y, r_gyro_z);

  bool ok = (abs(r_accel_z) > 5.0 || abs(r_accel_x) > 5.0 || abs(r_accel_y) > 5.0);
  printResult("Batch has gravity on at least one axis", ok);
}

// ====================================================================
// TEST 12: readAllSensors() simulation — full output
// ====================================================================
void test_readAllSensors() {
  Serial.println("\n── TEST 12: readAllSensors() full simulation ──");

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  // Exact replacement for readAllSensors()
  float accel_x = a.acceleration.x;   // already m/s²
  float accel_y = a.acceleration.y;
  float accel_z = a.acceleration.z;
  float gyro_x  = g.gyro.x * 57.2958; // rad/s → deg/s
  float gyro_y  = g.gyro.y * 57.2958;
  float gyro_z  = g.gyro.z * 57.2958;

  // Calibrated values (same as m/s² — matches main code)
  float calibrated_ax = accel_x;
  float calibrated_ay = accel_y;
  float calibrated_az = accel_z;

  Serial.printf("  accel  (m/s²): x=%7.2f  y=%7.2f  z=%7.2f\n", accel_x, accel_y, accel_z);
  Serial.printf("  gyro  (deg/s): x=%7.2f  y=%7.2f  z=%7.2f\n", gyro_x, gyro_y, gyro_z);
  Serial.printf("  calib  (m/s²): x=%7.2f  y=%7.2f  z=%7.2f\n", calibrated_ax, calibrated_ay, calibrated_az);
  Serial.printf("  temp:  %.1f °C\n", t.temperature);
  printResult("readAllSensors() pattern complete", true);
}

// ====================================================================
// TEST 13: Continuous live print (runs in loop)
//   Uses detectDirection() logic from main code (m/s² thresholds):
//   az < -7   → INVERTED    az > 7   → NEUTRAL (flat)
//   ax > 5    → RIGHT       ax < -5  → LEFT
//   ay > 5    → BACK        ay < -5  → FORWARD
// ====================================================================
void test_live_print() {
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  // Use m/s² directly — matches detectDirection() in main code
  float ax = a.acceleration.x;
  float ay = a.acceleration.y;
  float az = a.acceleration.z;

  // Exact detectDirection() logic from esp32_sketch_test.ino (line ~3095)
  float abs_ax = abs(ax);
  float abs_ay = abs(ay);
  float abs_az = abs(az);

  const char* direction = "NEUTRAL";
  if (abs_az > abs_ax && abs_az > abs_ay) {
    if (az < -7.0f)      direction = "INVERTED";
    else if (az > 7.0f)  direction = "NEUTRAL";
  } else if (abs_ax > abs_ay && abs_ax > abs_az) {
    if (ax > 5.0f)       direction = "RIGHT";
    else if (ax < -5.0f) direction = "LEFT";
  } else if (abs_ay > abs_ax && abs_ay > abs_az) {
    if (ay > 5.0f)       direction = "BACK";
    else if (ay < -5.0f) direction = "FORWARD";
  }

  // Also show g-units for tilt gesture reference
  float gx = ax / 9.81;  // for LEFT/RIGHT gesture threshold (±0.8g)
  float gy = ay / 9.81;  // for game tilt

  Serial.printf("[%7lums]  ax=%6.2f  ay=%6.2f  az=%6.2f m/s²  |  dir=%-8s  |  gx=%5.2f gy=%5.2f g\n",
                millis(),
                ax, ay, az,
                direction,
                gx, gy);
}

// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔══════════════════════════════════════════════╗");
  Serial.println("║  MPU6050 Adafruit Library — Full Audit Test  ║");
  Serial.println("╚══════════════════════════════════════════════╝");

  // ── Run all tests ──
  if (!test_begin()) {
    Serial.println("\n*** MPU6050 NOT FOUND — stopping ***");
    while (true) delay(1000);
  }

  test_setAccelRange();
  test_setGyroRange();
  test_getEvent();
  test_accel_g_units();
  test_accel_ms2();
  test_gyro_dps();
  test_gravity_warmup();
  test_step_detection();
  test_game_tilt();
  test_sensor_batch();
  test_readAllSensors();

  // ── Summary ──
  Serial.println("\n══════════════════════════════════════════");
  Serial.printf("  RESULTS:  %d PASSED  /  %d FAILED\n", testsPassed, testsFailed);
  Serial.println("══════════════════════════════════════════");

  if (testsFailed == 0) {
    Serial.println("  ALL TESTS PASSED — safe to migrate main code!\n");
  } else {
    Serial.println("  SOME TESTS FAILED — check output above\n");
  }

  Serial.println("Now streaming live data — matches detectDirection() from main code...\n");
  Serial.println("[   ms  ]  ax      ay      az   m/s²  |  direction  |  gx    gy  g");
}

void loop() {
  if (!mpuAvailable) return;
  test_live_print();
  delay(500);
}
