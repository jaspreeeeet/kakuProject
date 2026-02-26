// =====================================================================
//  test_walking.ino  —  CALIBRATION + DETECTION TEST
//  Hardware: XIAO ESP32S3 + MPU6050 (SDA=5, SCL=6)
//  Baud: 115200
//
//  HOW TO USE:
//    1. Flash to device, open Serial Monitor
//    2. Type a letter + Enter to label each scenario:
//         A  →  Still flat (on table)
//         B  →  Still tilted (hold at angle, don't move)
//         C  →  Walking (hold in hand)
//         D  →  Pocket walking
//    3. Copy ALL Serial output, share with Copilot for tuning
//
//  OUTPUT FORMAT:
//    SCENARIO,<label>              ← when you change scenario
//    RAW,scenario,gx,gy,gz,stoss   ← every 50ms reading
//    STEP,#N,stoss,interval_ms     ← when step detected
//    STATS,scenario,steps_5s,total ← every 5s summary
// =====================================================================

#include <Wire.h>
#include <MPU6050.h>

// ── PIN CONFIG ────────────────────────────────────────────────
#define SDA_PIN 5
#define SCL_PIN 6

// ── TUNABLE PARAMETERS ────────────────────────────────────────
// Calibrated from real sensor data:
//   Rest noise max  : 0.00007 g²
//   Weakest step    : 0.29633 g²
//   Typical steps   : 0.50–0.90 g²
//   Step interval   : ~1000ms
const float        STEP_BARRIER_G2    = 0.10f;  // 1400x above noise, 3x below weakest step
const unsigned long STEP_MIN_MS       = 400;    // steps ~1000ms apart, 400ms debounce is safe
const unsigned long WALKING_WINDOW_MS = 3000;   // flag walking for 3s after last step
const float        LP_ALPHA           = 0.85f;  // low-pass gravity filter weight

// ── HARDWARE ──────────────────────────────────────────────────
MPU6050 mpu;
bool mpuAvailable = false;

// ── GRAVITY FILTER STATE ──────────────────────────────────────
float gravX = 0.0f, gravY = 0.0f, gravZ = 1.0f;

// ── STEP COUNTER ──────────────────────────────────────────────
uint32_t      hwStepCount        = 0;
uint32_t      intervalStepCount  = 0;
unsigned long lastHwStepTime     = 0;
unsigned long lastWalkingStepTime = 0;

// ── WALKING FLAG ──────────────────────────────────────────────
bool petIsWalking     = false;
bool lastWalkingState = false;

// ── SCENARIO LABEL (set via Serial: A/B/C/D) ─────────────────
char currentScenario = '?';

// ── TIMERS ────────────────────────────────────────────────────
unsigned long lastStatsPrint = 0;
unsigned long lastReadTime   = 0;

// ─────────────────────────────────────────────────────────────
void detectHardwareStep();

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(600);

    Serial.println("\n====================================");
    Serial.println("  test_walking.ino  CALIBRATION MODE");
    Serial.println("====================================");
    Serial.printf("  STEP_BARRIER_G2   : %.4f g2\n", STEP_BARRIER_G2);
    Serial.printf("  STEP_MIN_MS       : %lu ms\n",   STEP_MIN_MS);
    Serial.printf("  LP_ALPHA          : %.2f\n",     LP_ALPHA);
    Serial.println();
    Serial.println("Send a letter + Enter to label scenario:");
    Serial.println("  A = Still flat   B = Still tilted");
    Serial.println("  C = Walking hand D = Pocket walk");
    Serial.println("------------------------------------\n");

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);

    mpu.initialize();
    if (mpu.testConnection()) {
        mpuAvailable = true;
        mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2); // +-2g, 16384 LSB/g
        Serial.println("OK,MPU6050 connected");
    } else {
        Serial.println("ERR,MPU6050 not found,check SDA=5 SCL=6");
    }

    lastStatsPrint = millis();
    lastReadTime   = millis();

    // Warm up the LP gravity filter for 1 second before enabling step detection
    // Prevents cold-start false positives during initial filter settling
    if (mpuAvailable) {
        Serial.println("Warming up gravity filter...");
        for (int i = 0; i < 40; i++) {   // 40 × 25ms = 1 second
            int16_t ax, ay, az;
            mpu.getAcceleration(&ax, &ay, &az);
            float gx = ax / 16384.0f;
            float gy = ay / 16384.0f;
            float gz = az / 16384.0f;
            gravX = LP_ALPHA * gravX + (1.0f - LP_ALPHA) * gx;
            gravY = LP_ALPHA * gravY + (1.0f - LP_ALPHA) * gy;
            gravZ = LP_ALPHA * gravZ + (1.0f - LP_ALPHA) * gz;
            delay(25);
        }
        Serial.println("OK,gravity filter ready");
    }
}

// ─────────────────────────────────────────────────────────────
void loop() {
    // Handle scenario label input from Serial
    if (Serial.available()) {
        char c = toupper((char)Serial.read());
        if (c == 'A' || c == 'B' || c == 'C' || c == 'D') {
            currentScenario   = c;
            intervalStepCount = 0;
            Serial.printf("\nSCENARIO,%c\n", currentScenario);
        }
    }

    // Read + detect at 20Hz (every 50ms)
    if (millis() - lastReadTime >= 50) {
        lastReadTime = millis();
        detectHardwareStep();
    }

    // Update walking flag
    petIsWalking = (millis() - lastWalkingStepTime) < WALKING_WINDOW_MS;

    // Log state changes
    if (petIsWalking != lastWalkingState) {
        lastWalkingState = petIsWalking;
        Serial.printf("STATE,%s\n", petIsWalking ? "WALKING" : "IDLE");
    }

    // Every 5s: print summary
    if (millis() - lastStatsPrint >= 5000) {
        lastStatsPrint = millis();
        Serial.printf("STATS,scenario=%c,steps_5s=%u,total=%u,walking=%s\n",
                      currentScenario, intervalStepCount, hwStepCount,
                      petIsWalking ? "YES" : "NO");
        intervalStepCount = 0;
    }
}

// ─────────────────────────────────────────────────────────────
void detectHardwareStep() {
    if (!mpuAvailable) return;

    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);

    // Convert to g-units (+-2g range, 16384 LSB/g)
    float gx = ax / 16384.0f;
    float gy = ay / 16384.0f;
    float gz = az / 16384.0f;

    // Low-pass filter tracks gravity at ANY tilt orientation
    gravX = LP_ALPHA * gravX + (1.0f - LP_ALPHA) * gx;
    gravY = LP_ALPHA * gravY + (1.0f - LP_ALPHA) * gy;
    gravZ = LP_ALPHA * gravZ + (1.0f - LP_ALPHA) * gz;

    // Dynamic component only (gravity subtracted)
    float dx = gx - gravX;
    float dy = gy - gravY;
    float dz = gz - gravZ;
    float stoss = dx*dx + dy*dy + dz*dz;

    // Log every reading for calibration
    Serial.printf("RAW,%c,%.4f,%.4f,%.4f,%.5f\n",
                  currentScenario, gx, gy, gz, stoss);

    // Step detection with debounce
    unsigned long now      = millis();
    unsigned long interval = now - lastHwStepTime;
    if (stoss > STEP_BARRIER_G2 && interval > STEP_MIN_MS) {
        hwStepCount++;
        intervalStepCount++;
        lastHwStepTime       = now;
        lastWalkingStepTime  = now;
        Serial.printf("STEP,%u,%.5f,%lu\n", hwStepCount, stoss, interval);
    }
}
