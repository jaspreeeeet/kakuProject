// =====================================================================
//  test.ino  —  ROTATION DETECTION TEST (3 full flips → DETECTED!)
//  Hardware: XIAO ESP32S3 Sense + MPU6050 (I2C: SDA=5, SCL=6)
//  Baud: 115200
//
//  PURPOSE:
//    Detects 3 FULL rotations (flips) of the device on ONE axis.
//    Uses accelerometer angle (not gyro!) so small tilts are ignored.
//    Device must physically pass through all 4 orientations:
//      Forward:  Upright → TiltFwd → Inverted → TiltBack → Upright
//      Backward: Upright → TiltBack → Inverted → TiltFwd → Upright
//
//  HOW IT WORKS:
//    1. Computes pitch angle from accelerometer: atan2(ax, az)
//    2. Divides 360° into 4 quadrants:
//       Q0 = Upright (−45° to 45°)
//       Q1 = Tilted forward (45° to 135°)
//       Q2 = Inverted (135° to −135°, i.e. upside down)
//       Q3 = Tilted backward (−135° to −45°)
//    3. Must visit Q0→Q1→Q2→Q3→Q0 (forward flip)
//       OR Q0→Q3→Q2→Q1→Q0 (backward flip) for 1 rotation
//    4. Small tilts stay in Q0 — never counted!
//    5. After 3 rotations → 🎉 DETECTED!
// =====================================================================

#include <Wire.h>
#include "MPU6050.h"
#include <math.h>

// ================= I2C PINS (XIAO ESP32 S3 Sense) =================
#define SDA_PIN  5
#define SCL_PIN  6

// ================= ROTATION DETECTION CONFIG =================
#define READ_INTERVAL        20      // ms — read accel every 20ms (50Hz)
#define ROTATIONS_NEEDED     3       // how many full flips to detect
#define ROTATION_TIMEOUT     5000    // ms — must complete one rotation within this time
#define COOLDOWN_AFTER_3     3000    // ms — pause after 3 rotations detected

// ================= GLOBALS =================
MPU6050 mpu;
bool mpuAvailable = false;

// Quadrant tracking state machine
// Quadrants (based on pitch angle from accelerometer):
//   Q0: Upright       (-45° to  45°)
//   Q1: Tilted fwd    ( 45° to 135°)  
//   Q2: Inverted      (135° to 180° or -180° to -135°)
//   Q3: Tilted back   (-135° to -45°)
int current_quadrant     = 0;    // Which quadrant device is in now
int quadrants_visited    = 0;    // Bitmask of visited quadrants (bits 0-3)
int last_quadrant        = -1;   // Previous quadrant (to detect transitions)
int rotation_direction   = 0;    // +1 = forward, -1 = backward, 0 = unknown
int expected_next        = -1;   // Next quadrant expected for current direction
int rotation_count       = 0;    // Full rotations completed
unsigned long rotation_start_time = 0; // When current rotation attempt started
unsigned long last_read_time = 0;

// Low-pass filter for accelerometer noise
float filtered_ax = 0, filtered_az = 0;
const float LP_ALPHA = 0.3;     // Low-pass filter coefficient (0.0-1.0, lower = smoother)

// ── Helper: get quadrant from angle ──────────────────────────
int getQuadrant(float angle_deg) {
    // angle_deg is -180 to +180
    if (angle_deg >= -45 && angle_deg < 45)   return 0;  // Upright
    if (angle_deg >= 45 && angle_deg < 135)   return 1;  // Tilted forward
    if (angle_deg >= -135 && angle_deg < -45)  return 3;  // Tilted backward
    return 2; // Inverted (135 to 180 or -180 to -135)
}

const char* quadrantName(int q) {
    switch(q) {
        case 0: return "UPRIGHT";
        case 1: return "TILT-FWD";
        case 2: return "INVERTED";
        case 3: return "TILT-BACK";
        default: return "???";
    }
}

// ── Reset rotation tracking state ────────────────────────────
void resetRotationState() {
    quadrants_visited = 0;
    rotation_direction = 0;
    expected_next = -1;
    rotation_start_time = 0;
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║  ROTATION (FLIP) DETECTION TEST      ║");
    Serial.println("║  Detect 3 full flips → SUCCESS       ║");
    Serial.println("║  Must physically flip the device!     ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.println();

    // Init I2C
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);
    delay(200);

    // Init MPU6050
    Serial.println("Initializing MPU6050...");
    mpu.initialize();
    delay(100);

    if (mpu.testConnection()) {
        Serial.println("✅ MPU6050 connected!");
        mpuAvailable = true;
    } else {
        Serial.println("❌ MPU6050 NOT found! Check wiring (SDA=5, SCL=6)");
        while (1) delay(1000);
    }

    // Warm up low-pass filter
    for (int i = 0; i < 20; i++) {
        int16_t ax, ay, az;
        mpu.getAcceleration(&ax, &ay, &az);
        filtered_ax = ax / 16384.0;
        filtered_az = az / 16384.0;
        delay(20);
    }

    float init_angle = atan2(filtered_ax, filtered_az) * 180.0 / M_PI;
    current_quadrant = getQuadrant(init_angle);
    last_quadrant = current_quadrant;

    Serial.printf("✅ Starting angle: %.1f° (Quadrant: %s)\n", init_angle, quadrantName(current_quadrant));
    Serial.println();
    Serial.println("🔄 Flip the device! Full rotation required:");
    Serial.println("   Forward:  Upright → Tilt fwd → Inverted → Tilt back → Upright");
    Serial.println("   Backward: Upright → Tilt back → Inverted → Tilt fwd → Upright");
    Serial.println("   Small tilts WON'T count — device must go fully inverted!");
    Serial.println("════════════════════════════════════════\n");

    last_read_time = millis();
}

// ─────────────────────────────────────────────────────────────
void loop() {
    if (!mpuAvailable) return;

    unsigned long now = millis();
    if (now - last_read_time < READ_INTERVAL) return;
    last_read_time = now;

    // Read accelerometer
    int16_t ax_raw, ay_raw, az_raw;
    mpu.getAcceleration(&ax_raw, &ay_raw, &az_raw);

    // Low-pass filter to remove vibration noise
    float ax = ax_raw / 16384.0;
    float az = az_raw / 16384.0;
    filtered_ax = LP_ALPHA * ax + (1.0 - LP_ALPHA) * filtered_ax;
    filtered_az = LP_ALPHA * az + (1.0 - LP_ALPHA) * filtered_az;

    // Compute pitch angle: -180° to +180°
    float angle = atan2(filtered_ax, filtered_az) * 180.0 / M_PI;

    // Determine current quadrant
    int q = getQuadrant(angle);

    // Only process on quadrant change
    if (q != last_quadrant) {
        Serial.printf("   → Entered %s (angle: %.1f°)\n", quadrantName(q), angle);

        // Check timeout — if rotation takes too long, reset
        if (rotation_start_time > 0 && (now - rotation_start_time > ROTATION_TIMEOUT)) {
            Serial.println("   ⏸️ Rotation timeout — too slow, resetting");
            resetRotationState();
        }

        // State machine: detect sequential quadrant traversal
        if (quadrants_visited == 0) {
            // Starting fresh — must start from Q0 (upright)
            if (q == 0) {
                // Already at upright, wait for first move
            } else if (last_quadrant == 0) {
                // Just left Q0 — start tracking
                quadrants_visited = (1 << 0); // Mark Q0 as visited
                rotation_start_time = now;

                if (q == 1) {
                    rotation_direction = +1; // Forward rotation
                    expected_next = 1;
                    Serial.println("   📐 Forward rotation started (Q0→Q1)");
                } else if (q == 3) {
                    rotation_direction = -1; // Backward rotation
                    expected_next = 3;
                    Serial.println("   📐 Backward rotation started (Q0→Q3)");
                } else {
                    // Jumped to Q2 directly — unusual, reset
                    resetRotationState();
                }

                if (expected_next >= 0) {
                    quadrants_visited |= (1 << q);
                }
            }
        } else {
            // Already tracking a rotation — check if this is the expected next quadrant
            bool valid = false;

            if (rotation_direction == +1) {
                // Forward: Q0 → Q1 → Q2 → Q3 → Q0
                int forward_seq[] = {0, 1, 2, 3, 0};
                for (int i = 0; i < 4; i++) {
                    if (last_quadrant == forward_seq[i] && q == forward_seq[i+1]) {
                        valid = true;
                        break;
                    }
                }
            } else if (rotation_direction == -1) {
                // Backward: Q0 → Q3 → Q2 → Q1 → Q0
                int backward_seq[] = {0, 3, 2, 1, 0};
                for (int i = 0; i < 4; i++) {
                    if (last_quadrant == backward_seq[i] && q == backward_seq[i+1]) {
                        valid = true;
                        break;
                    }
                }
            }

            if (valid) {
                quadrants_visited |= (1 << q);

                // Check if all 4 quadrants visited AND returned to Q0
                if (q == 0 && quadrants_visited == 0x0F) {
                    // FULL ROTATION COMPLETED!
                    rotation_count++;
                    const char* dir = (rotation_direction == +1) ? "FORWARD" : "BACKWARD";
                    Serial.printf("\n🔄 Rotation #%d detected! (%s flip)\n\n",
                        rotation_count, dir);

                    resetRotationState();

                    // Check if 3 rotations reached
                    if (rotation_count >= ROTATIONS_NEEDED) {
                        Serial.println("🎉🎉🎉 3 FULL ROTATIONS DETECTED! 🎉🎉🎉");
                        Serial.println();
                        Serial.println("════════════════════════════════════════");
                        Serial.printf("   Total rotations: %d\n", rotation_count);
                        Serial.println("   Resetting counter in 3 seconds...");
                        Serial.println("════════════════════════════════════════\n");

                        delay(COOLDOWN_AFTER_3);

                        rotation_count = 0;
                        resetRotationState();
                        Serial.println("🔄 Counter reset! Flip again for another round.\n");
                    }
                }
            } else {
                // Wrong quadrant order — rotation broken, reset
                if (q != last_quadrant) {
                    Serial.printf("   ❌ Sequence broken (expected sequential, got %s→%s) — reset\n",
                        quadrantName(last_quadrant), quadrantName(q));
                    resetRotationState();
                }
            }
        }

        last_quadrant = q;
    }

    // Periodic angle display (every 3 seconds)
    static unsigned long last_status = 0;
    if (now - last_status > 3000) {
        last_status = now;
        Serial.printf("   📊 Angle: %.1f° | Quadrant: %s | Visited: %s%s%s%s | Rotations: %d/%d\n",
            angle, quadrantName(q),
            (quadrants_visited & 1) ? "Q0 " : "",
            (quadrants_visited & 2) ? "Q1 " : "",
            (quadrants_visited & 4) ? "Q2 " : "",
            (quadrants_visited & 8) ? "Q3 " : "",
            rotation_count, ROTATIONS_NEEDED);
    }
}