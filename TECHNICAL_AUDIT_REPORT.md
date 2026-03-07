# KAKU ESP32 Tamagotchi — Full Technical Audit Report
**Date:** 2026-03-XX  
**Firmware:** v1.0.5  
**File:** `esp32_sketch/esp32_sketch.ino` (~6540 lines)  
**Platform:** XIAO ESP32-S3 Sense (dual-core, PSRAM, OV2640, PDM mic)

---

## Executive Summary

Five observed symptoms were reported:
1. Display lag / freeze
2. Slow deep sleep recovery
3. Animation corruption (garbled pixels)
4. Screen stuck on one frame
5. Wake-up slow or blocked

After a line-by-line audit of the entire firmware, **12 bugs/risks** were identified. The root causes cluster into three categories:

| Category | Bugs | Symptom Impact |
|----------|------|----------------|
| **Unprotected `display` object** | #1, #2, #3 | Corruption, freeze, garbled pixels |
| **I2C bus over-contention** | #4, #5, #6 | Lag, dropped frames, slow wake |
| **Mutex / blocking design** | #7, #8, #9, #10, #11, #12 | Hangs, stalls, race conditions |

---

## BUG #1 — CRITICAL: `display` Object Has No Mutex (Cross-Core Corruption)

**Location:** Entire codebase — `display` (Adafruit_SSD1306) is a shared resource  
**Severity:** CRITICAL — **Most likely root cause of corruption + freeze**

### The Problem

The `display` object is written to from **both cores simultaneously** with no protection:

| Writer | Core | Function | When |
|--------|------|----------|------|
| `oledTask` | Core 0 | `displayPetAnimation()` | Every 125ms continuously |
| `loop()` | Core 1 | WiFi reconnect block | On WiFi disconnect |
| `enterDeepSleep()` | Core 1 | "Zzz..." message | On inactivity timeout |
| `checkAndPerformOTA()` | Core 1 | OTA progress | During OTA (tasks suspended — OK) |
| `setup()` | Core 1 | "Connecting..." / "Waking up..." | Before tasks start — OK |
| `getOLEDDisplayFromServer()` | Core 1 (networkTask) | `playEatingAnimation()` called inline | On server emotion EATING |

`Adafruit_SSD1306` maintains an internal 256-byte framebuffer. When Core 0 is mid-`display.display()` (I2C transfer of the buffer) and Core 1 calls `display.clearDisplay()` + `display.print()`, the buffer is torn mid-transfer. The OLED receives half of frame A and half of frame B → **garbled pixels**.

### Why This Causes Your Symptoms
- **Animation corruption:** Buffer torn between two partial writes
- **Screen stuck:** Core 1 writes "WiFi Lost" over the animation buffer; Core 0 never clears it because it reads stale data
- **Display freeze:** If both cores call `display.display()` (which does `Wire.beginTransmission` + bulk write), the I2C transaction can collide, causing the SSD1306 to NAK or hang the bus

### Recommended Fix (Minimal)

Create a `displayMutex` and wrap **every** `display.*` call sequence:

```cpp
// Add to globals (near other mutex declarations)
SemaphoreHandle_t displayMutex = NULL;

// Create in setup() alongside other mutexes
displayMutex = xSemaphoreCreateMutex();
```

**In `oledTask` (Core 0)** — already the primary owner, wrap the render:
```cpp
void oledTask(void *parameter) {
  while (true) {
    if (displayReady && startupComplete) {
      if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        displayPetAnimation();
        xSemaphoreGive(displayMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(125));
  }
}
```

**In `loop()` WiFi reconnect (Core 1)** — wrap the "WiFi Lost" write:
```cpp
if (displayReady && displayMutex && 
    xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 4);
  display.println("WiFi Lost");
  display.setCursor(2, 16);
  display.printf("Retry %d/%d", wifiReconnectFails, WIFI_RECONNECT_MAX_FAILS);
  display.display();
  xSemaphoreGive(displayMutex);
}
```

**In `enterDeepSleep()` (Core 1)** — wrap the "Zzz..." write:
```cpp
if (displayMutex && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
  display.clearDisplay();
  // ... draw Zzz message ...
  display.display();
  xSemaphoreGive(displayMutex);
}
```

---

## BUG #2 — HIGH: `enterDeepSleep()` Writes Display While OLED Task Still Runs

**Location:** `enterDeepSleep()` (~line 1535-1600)  
**Severity:** HIGH

### The Problem

When `enterDeepSleep()` is called from `loop()` on Core 1, it:
1. Calls `savePetState()` (acquires `petStateMutex` with `portMAX_DELAY`)
2. Calls `deinitCamera()`
3. **Writes to `display`** — "Zzz... Shake 2 wake"
4. Calls `delay(500)` — **blocking!**
5. Detaches ISR
6. Configures MPU for sleep
7. Enters deep sleep

During steps 3-6, `oledTask` on Core 0 is **still running** and calling `displayPetAnimation()` every 125ms, which also writes to `display` AND reads I2C (MPU6050).

### Why This Causes Your Symptoms
- **Animation corruption** right before sleep: the "Zzz..." message gets overwritten by a partial animation frame
- **Slow deep sleep entry:** `delay(500)` blocks Core 1 for half a second while the OLED task fights for display access

### Recommended Fix

Suspend `oledTask` before touching the display in `enterDeepSleep()`:

```cpp
void enterDeepSleep() {
  Serial.println("Entering DEEP SLEEP — shake to wake!");

  // STOP OLED TASK FIRST — prevents display/I2C races during shutdown
  if (oledTaskHandle) vTaskSuspend(oledTaskHandle);

  savePetState();
  deinitCamera();

  // Now safe to write display from Core 1
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(8, 4);
  display.print("Zzz...");
  display.setCursor(2, 20);
  display.print("Shake 2 wake");
  display.display();
  delay(500);
  display.clearDisplay();
  display.display();
  // ... rest of deep sleep sequence ...
}
```

---

## BUG #3 — HIGH: `enterDeepSleep()` Does Raw I2C Without `i2cMutex`

**Location:** `configureMPU6050ForSleep()` (~line 1483-1510)  
**Severity:** HIGH

### The Problem

`configureMPU6050ForSleep()` writes directly to `Wire` (I2C address 0x68) using `Wire.beginTransmission()` / `Wire.write()` / `Wire.endTransmission()` — **without acquiring `i2cMutex`**.

Meanwhile, `oledTask` on Core 0 calls `checkMenuTiltGesture()` / `detectHardwareStep()` which acquire `i2cMutex` and call `mpu.getEvent()` (also I2C to 0x68) AND `display.display()` (I2C to 0x3C).

Two I2C masters on the same bus simultaneously = **bus collision**, NAK errors, or hung bus.

### Why This Causes Your Symptoms
- **Slow wake-up:** If the I2C bus hangs during sleep entry, the MPU may not be configured correctly, causing the ext1 wakeup to fail or trigger instantly (the "INT pin stuck LOW" fallback path)
- **Display freeze before sleep:** Bus collision corrupts the OLED's last frame

### Recommended Fix

If you adopt the fix from Bug #2 (suspend `oledTask` before sleep sequence), this race is eliminated because Core 0 won't be doing I2C. If you don't suspend `oledTask`, wrap the raw Wire writes:

```cpp
void configureMPU6050ForSleep() {
  // ... Adafruit mpu.set*() calls are fine (they use Wire internally) ...

  if (i2cMutex) xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500));
  Wire.beginTransmission(0x68);
  Wire.write(0x6C);
  Wire.write(0x47);
  Wire.endTransmission();

  Wire.beginTransmission(0x68);
  Wire.write(0x6B);
  Wire.write(0x28);
  Wire.endTransmission();
  if (i2cMutex) xSemaphoreGive(i2cMutex);
}
```

**Best approach: Adopt Bug #2 fix (suspend oledTask), which eliminates both Bug #2 and Bug #3.**

---

## BUG #4 — HIGH: Too Many I2C Reads Per OLED Frame (Frame Budget Blown)

**Location:** `displayPetAnimation()` (~line 3600-4100)  
**Severity:** HIGH — **Primary cause of display lag**

### The Problem

`displayPetAnimation()` is called by `oledTask` every 125ms (~8 FPS target). Inside each call, it executes:

| Call | I2C Reads | Mutex Wait (max) |
|------|-----------|-------------------|
| `checkMenuTiltGesture()` | 1× `mpu.getEvent()` | 200ms |
| `checkSTTTiltGesture()` | 1× `mpu.getEvent()` | 200ms |
| `detectHardwareStep()` | 1× `mpu.getEvent()` | 200ms |
| Menu-specific gesture check (e.g. `checkFeedingGesture()`) | 1× `mpu.getEvent()` | 200ms |

That's **4 separate mutex acquisitions + 4 I2C transactions per frame**. At 400kHz I2C, each `mpu.getEvent()` takes ~1-2ms for the actual transfer, but the **200ms mutex timeout** is the real problem. If `readAllSensors()` (loop, Core 1) or the sensor batch collector holds `i2cMutex`, each of these calls waits up to 200ms.

**Worst case:** 4 × 200ms = 800ms per frame (frame budget is 125ms).  
**Typical case:** Even 1 contention = 200ms > 125ms → frame dropped, animation stutters.

### Why This Causes Your Symptoms
- **Display lag:** Frame rendering takes longer than 125ms, causing visible stutter
- **Screen stuck:** If multiple mutex timeouts cascade, the frame may not update for 400-800ms

### Recommended Fix

**Read MPU6050 ONCE per frame** and reuse the result:

```cpp
void displayPetAnimation() {
  display.clearDisplay();

  // ── Single I2C read for all gesture checks this frame ──
  float frameAccelX = 0, frameAccelY = 0, frameAccelZ = 0;
  float frameGyroX = 0, frameGyroY = 0, frameGyroZ = 0;
  bool mpuReadOK = false;
  
  if (mpuAvailable && i2cMutex && 
      xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    mpu.getEvent(&accelEvent, &gyroEvent, &tempEvent);
    xSemaphoreGive(i2cMutex);
    frameAccelX = accelEvent.acceleration.x;
    frameAccelY = accelEvent.acceleration.y;
    frameAccelZ = accelEvent.acceleration.z;
    frameGyroX = gyroEvent.gyro.x;
    frameGyroY = gyroEvent.gyro.y;
    frameGyroZ = gyroEvent.gyro.z;
    mpuReadOK = true;
  }

  // Now pass frame data to gesture checks (no I2C inside them)
  if (mpuReadOK) {
    checkMenuTiltGestureFromCache(frameAccelX);
    checkSTTTiltGestureFromCache(frameAccelX);
    detectHardwareStepFromCache(frameAccelX, frameAccelY, frameAccelZ);
    // ... menu-specific gesture check ...
  }
  
  // ... rest of animation rendering (unchanged) ...
  display.display();
}
```

This reduces 4 I2C transactions to 1, and the total mutex hold per frame from 4×200ms to 1×50ms. **This is the single highest-impact performance fix.**

---

## BUG #5 — MEDIUM: `motionDetected` ISR Flag Read Without Clearing I2C Latch Safely

**Location:** `loop()` (~line 4330) and `onMotionInterrupt()` ISR (~line 1445)  
**Severity:** MEDIUM

### The Problem

```cpp
// In loop():
if (motionDetected) {
    motionDetected = false;
    mpu.getMotionInterruptStatus();  // I2C read to clear latched interrupt
    lastMotionTime = millis();
}
```

This `mpu.getMotionInterruptStatus()` call in `loop()` (Core 1) does I2C **without acquiring `i2cMutex`**. Meanwhile, `oledTask` (Core 0) may be reading MPU via the mutex.

### Recommended Fix

```cpp
if (motionDetected) {
    motionDetected = false;
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      mpu.getMotionInterruptStatus();
      xSemaphoreGive(i2cMutex);
    }
    lastMotionTime = millis();
}
```

---

## BUG #6 — MEDIUM: Sensor Batch Collection Competes With OLED Task for I2C

**Location:** `loop()` sensor batch (~line 4240-4280)  
**Severity:** MEDIUM

### The Problem

Every 100ms (`INTERNAL_READ_INTERVAL`), `loop()` on Core 1 acquires `i2cMutex` with 200ms timeout to read MPU6050. This runs at the **same cadence** as the OLED task (125ms), creating predictable contention.

When both fire within the same 100ms window:
- `loop()` holds `i2cMutex` for ~2ms (I2C read)
- `oledTask` tries to acquire `i2cMutex` × 4 times (Bug #4)
- One of them loses → 200ms wait or silent failure

### Recommended Fix

If Bug #4 is fixed (single MPU read per OLED frame), contention drops from 4 conflicts/frame to 1. Additionally, reduce the timeout on the batch read since it's non-critical:

```cpp
// In loop() sensor batch section:
if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
  // Skipped — not critical, catch next batch
  reading.accel_x = reading.accel_y = reading.accel_z = 0.0;
  // ...
}
```

---

## BUG #7 — MEDIUM: `petStateMutex` Uses `portMAX_DELAY` in Multiple Hot Paths

**Location:** `savePetState()`, `loadPetState()`, `syncLocalStateToUI()`, `handlePhysiology()`, `sendSensorDataOnly()`, `sendAllDataToServer()`, `getOLEDDisplayFromServer()`, all gesture handlers  
**Severity:** MEDIUM — potential deadlock / indefinite hang

### The Problem

`portMAX_DELAY` means "wait forever." If any code path holds `petStateMutex` and another calls a function that also needs it with `portMAX_DELAY`, the system hangs until the holder releases.

Critical chain:
1. `handlePhysiology()` (Core 1, loop) acquires `petStateMutex` with `portMAX_DELAY`
2. Inside that lock, it calls `syncLocalStateToUI()` — which also tries to acquire `petStateMutex` with `portMAX_DELAY`

Wait — `syncLocalStateToUI()` and `handlePhysiology()` both acquire the same mutex. Let's check:

```cpp
void handlePhysiology() {
  if (petStateMutex) xSemaphoreTake(petStateMutex, portMAX_DELAY);
  // ... modify g_petState ...
  if (petStateMutex) xSemaphoreGive(petStateMutex);
  syncLocalStateToUI();  // <-- This acquires petStateMutex again
  savePetState();         // <-- This also acquires petStateMutex
}
```

This is OK because the mutex is released before `syncLocalStateToUI()` is called. **No actual recursive deadlock here.** However, the real risk is:

- `sendSensorDataOnly()` (networkTask, Core 1) acquires `petStateMutex` with `portMAX_DELAY` while building JSON
- Simultaneously, `handlePhysiology()` (loop, Core 1) acquires `petStateMutex` with `portMAX_DELAY`
- Both are on Core 1 but in **different tasks** — the FreeRTOS scheduler can preempt one while the mutex is held
- The waiting task blocks **forever** until the holder runs and releases

### Why This Causes Your Symptoms
- If `sendSensorDataOnly()` takes a long HTTP round-trip (5-10s) while holding the mutex, `handlePhysiology()` and `syncLocalStateToUI()` stall for the duration → UI state freezes

### Recommended Fix

Replace `portMAX_DELAY` with a bounded timeout in **all** `petStateMutex` callers:

```cpp
// In savePetState(), loadPetState(), syncLocalStateToUI(), sendSensorDataOnly(), etc.:
if (petStateMutex && xSemaphoreTake(petStateMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
  Serial.println("[petState] Mutex timeout — skipping");
  return; // or continue with stale data
}
```

Check `sendSensorDataOnly()` — it holds the mutex only during JSON build, NOT during the HTTP call. This is already correct. But `savePetState()` does NVS writes under the lock which can be slow (~10-50ms). Still bounded, so the real risk is low. The fix is a safety net.

---

## BUG #8 — MEDIUM: Mutexes Created AFTER Functions That Use Them

**Location:** `setup()` (~line 1800-1850)  
**Severity:** MEDIUM

### The Problem

Timeline in `setup()`:
1. **Line ~1680:** `display.begin()` — OLED init (no mutex needed yet)
2. **Line ~1700:** Egg animation calls `savePetState()` at end → uses `petStateMutex` (still NULL)
3. **Line ~1750:** MPU warmup loop uses `i2cMutex` (still NULL)
4. **Line ~1810:** `petStateMutex = xSemaphoreCreateMutex()` — **created here**
5. **Line ~1812:** `i2cMutex = xSemaphoreCreateMutex()` — **created here**
6. **Line ~1830:** Tasks are created (oledTask, audioTask, cameraTask)
7. **Line ~1860:** `loadPetState()` + `syncLocalStateToUI()` — uses mutexes (now exist)
8. **Line ~1870:** networkTask created

The code has `if (petStateMutex)` guards everywhere, so when the mutex is NULL, the guard skips the `xSemaphoreTake()`. This means:
- Egg animation's `savePetState()` writes NVS **without mutex protection**
- MPU warmup reads I2C **without mutex protection**

Since no tasks are created yet at that point, this is **safe in practice** (only one core/thread executing). But it's a landmine for future refactoring.

### Recommended Fix

Move mutex creation to **before** any function that uses them:

```cpp
// In setup(), right after Serial.begin() and LED init:
audioMutex = xSemaphoreCreateMutex();
cameraMutex = xSemaphoreCreateMutex();
cpuFreqMutex = xSemaphoreCreateMutex();
petStateMutex = xSemaphoreCreateMutex();
i2cMutex = xSemaphoreCreateMutex();
uiStringsMutex = xSemaphoreCreateMutex();
networkDataMutex = xSemaphoreCreateMutex();
sttDataMutex = xSemaphoreCreateMutex();
networkQueue = xQueueCreate(8, sizeof(uint8_t));

// ... rest of setup (I2C, OLED, WiFi, egg animation, MPU, etc.) ...
// ... task creation last ...
```

---

## BUG #9 — MEDIUM: `loadPetState()` Called Twice in `setup()`

**Location:** `setup()` paths  
**Severity:** LOW-MEDIUM

### The Problem

On a normal (non-egg) boot:
1. First `loadPetState()` is called in the egg/boot decision block (~line 1700)
2. Second `loadPetState()` is called after task creation (~line 1860)

The second call re-reads NVS and overwrites any state changes that happened between the two calls (e.g., if the egg animation just set initial state and called `savePetState()`).

On a deep sleep wake:
- The egg animation is skipped, `loadPetState()` runs once at ~line 1700
- Then runs again at ~line 1860 — redundant but harmless (same data)

### Recommended Fix

Remove the second `loadPetState()` call (line ~1860). Keep only the one inside the boot decision block. Or restructure so that `loadPetState()` is only called once at a clear point before tasks start.

---

## BUG #10 — MEDIUM: `sslNet.stop()` Called Before Image Upload Creates Stale TLS Session

**Location:** `sendImageData()` (~line 6260) and `sendSTTAudioChunk()` (~line 6480)  
**Severity:** MEDIUM

### The Problem

The code already has a comment explaining this (a known fix was applied):
```cpp
// FIX: Reset sslNet before reuse — 97KB POST can timeout/break the TLS session.
sslNet.stop();
```

However, `sslNet` is **shared** between `sendSensorDataOnly()`, `getOLEDDisplayFromServer()`, `sendImageData()`, `sendCleanRequest()`, `sendInjectRequest()`, `sendCoverHappyRequest()`, `sendSTTAudioChunk()`, and `sendAudioData()`. All of these run on the networkTask (Core 1 queue-driven), so they're sequential — no concurrency issue.

But `sslNet.stop()` in `sendImageData()` kills the keep-alive connection that `sendSensorDataOnly()` and `getOLEDDisplayFromServer()` rely on (`http.setReuse(true)`). The next sensor/OLED call after an image upload must do a full TLS handshake (~2-3 seconds on ESP32), causing a visible lag spike in the next poll cycle.

### Why This Causes Your Symptoms
- After feeding (image upload), the next OLED poll takes ~3s instead of ~0.5s → server emotion/state update is delayed

### Recommended Fix

This is a necessary evil — the alternative (stale TLS) is worse. No change needed. Document the expected 3s delay after image upload.

---

## BUG #11 — LOW: `screenTypeIs()` / `emotionIs()` Read Without Mutex

**Location:** Global helpers (~line 480)  
**Severity:** LOW

### The Problem

```cpp
inline bool screenTypeIs(const char *s) { return strcmp(currentScreenType, s) == 0; }
inline bool emotionIs(const char *s) { return strcmp(currentEmotion, s) == 0; }
```

These do `strcmp()` without mutex. `currentScreenType` and `currentEmotion` are `char[24]` arrays modified by `setScreenType()`/`setEmotion()` which DO use `uiStringsMutex`.

On ESP32 (Xtensa), `strcmp()` on a short char array is **not atomic** — if Core 1 writes "FOOD_MENU" while Core 0 reads mid-compare, `strcmp()` could see a partially-written string. However:
- The arrays are 24 bytes (3 words on 32-bit)
- `strncpy()` writes left-to-right
- Practical corruption requires a context switch at exactly the right byte during a 3-4 cycle operation

**Risk is very low** but not zero. The existing comment acknowledges this.

### Recommended Fix (Optional)

For belt-and-suspenders safety, make the reads also use the mutex with a short timeout:

```cpp
inline bool screenTypeIs(const char *s) {
  if (uiStringsMutex && xSemaphoreTake(uiStringsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    bool result = strcmp(currentScreenType, s) == 0;
    xSemaphoreGive(uiStringsMutex);
    return result;
  }
  return strcmp(currentScreenType, s) == 0; // Fallback: best-effort
}
```

---

## BUG #12 — LOW: `delay()` Calls in Critical Paths

**Location:** Multiple locations  
**Severity:** LOW (mostly cosmetic)

### Instances

| Location | Call | Duration | Impact |
|----------|------|----------|--------|
| `enterDeepSleep()` | `delay(500)` | 500ms | Blocks Core 1, OLED task still fighting |
| `enterDeepSleep()` | `delay(50)` | 50ms | After MPU config — acceptable |
| `enterDeepSleep()` | `delay(100)` per retry | up to 2s | Waiting for INT pin HIGH |
| `deinitCamera()` | `delay(100)` | 100ms | Hardware settle — acceptable |
| Low heap reboot | `delay(500)` | 500ms | Before `ESP.restart()` — acceptable |
| Daily reboot | `delay(500)` | 500ms | Before `ESP.restart()` — acceptable |
| OTA success display | `delay(2000)` | 2s | Before reboot — acceptable |

The `delay(500)` in `enterDeepSleep()` is the most impactful — it holds Core 1 for 500ms while no useful work happens (OLED message was already sent). 

### Recommended Fix

Replace with `vTaskDelay()` and reduce where possible:

```cpp
// In enterDeepSleep(), after display.display():
vTaskDelay(pdMS_TO_TICKS(300)); // Reduced: 300ms is enough for OLED render
```

---

## Summary Table: Bug → Symptom Mapping

| Bug | Display Lag | Slow Sleep Recovery | Animation Corruption | Screen Stuck | Slow Wake |
|-----|:-----------:|:-------------------:|:-------------------:|:------------:|:---------:|
| #1 Display no mutex | | | ✅ PRIMARY | ✅ | |
| #2 Sleep writes display | | | ✅ | | |
| #3 Sleep I2C no mutex | | ✅ | | | ✅ |
| #4 4× I2C per frame | ✅ PRIMARY | | | ✅ | |
| #5 Motion clear no mutex | | | | | ⚠️ |
| #6 Sensor batch contention | ✅ | | | | |
| #7 petStateMutex portMAX_DELAY | ✅ | | | ✅ | |
| #8 Late mutex creation | | | | | |
| #9 loadPetState twice | | | | | |
| #10 sslNet.stop kills keep-alive | ⚠️ | | | | |
| #11 strcmp no mutex | | | ⚠️ | | |
| #12 delay() in critical paths | | ✅ | | | ⚠️ |

---

## Priority Fix Order (Minimal Changes, Maximum Impact)

### Phase 1 — Fix corruption + lag (3 changes)

1. **Bug #4 — Single MPU read per frame:** Refactor `displayPetAnimation()` to read MPU6050 once and pass cached values to gesture checks. **Biggest FPS improvement.**

2. **Bug #1 + #2 + #3 — Suspend `oledTask` in `enterDeepSleep()`:** Add `vTaskSuspend(oledTaskHandle)` as the first line of `enterDeepSleep()`. This eliminates the display mutex issue for sleep AND the I2C race for sleep AND the buffer corruption. For the WiFi reconnect case, either add a `displayMutex` or (simpler) set a `volatile bool displayOverride` flag that `oledTask` checks before rendering.

### Phase 2 — Harden stability (3 changes)

3. **Bug #7 — Bounded mutex timeouts:** Replace all `portMAX_DELAY` on `petStateMutex` with `pdMS_TO_TICKS(500)`.

4. **Bug #8 — Early mutex creation:** Move all `xSemaphoreCreateMutex()` calls to the top of `setup()`.

5. **Bug #5 — Add i2cMutex to motion ISR clear:** Wrap `mpu.getMotionInterruptStatus()` in `loop()` with `i2cMutex`.

### Phase 3 — Polish (optional)

6. **Bug #9 — Remove duplicate `loadPetState()`.**
7. **Bug #12 — Replace `delay()` with `vTaskDelay()` in `enterDeepSleep()`.**
8. **Bug #11 — Add mutex to `screenTypeIs()` / `emotionIs()`.**

---

## Architecture Notes (Not Bugs, Just Observations)

### Task Priority Layout
| Task | Core | Priority | Stack |
|------|------|----------|-------|
| `audioMonitorTask` | 0 | 2 (HIGH) | 12KB |
| `oledTask` | 0 | 1 | 8KB |
| `cameraMonitorTask` | 0 | 1 | 8KB |
| `networkTask` | 1 | 1 | 16KB |
| `loop()` (Arduino) | 1 | 1 | 8KB (default) |

- Core 0 has 3 tasks; audio has highest priority (correct for real-time VAD)
- Core 1 has 2 tasks at same priority; round-robin scheduling
- `oledTask` and `cameraMonitorTask` share priority on Core 0 — camera capture can preempt OLED for up to 50ms (acceptable)

### Memory Usage Assessment
- `WiFiClientSecure` (sslNet + sslOTA): ~20KB DRAM + ~45KB PSRAM per connection
- `StaticJsonDocument<2048>` (g_sensorDoc, g_oledDoc): 4KB DRAM total — good
- `otaBuf[4096]`: 4KB global — good (avoids stack allocation)
- `SensorDataBatch`: 100 × 56 bytes = 5.6KB — significant but in DRAM (not PSRAM)
- OLED framebuffer: 256 bytes (64×32 / 8) — trivial
- STT PSRAM buffer: up to 160KB — properly in PSRAM
- No DynamicJsonDocument usage found — good (prevents heap fragmentation)

### Network Schedule (No Conflicts Found)
- Sensor: every 10s (tick slot 0,20,40...)
- OLED poll: every 2s (tick slot 4,8,12...)
- Image: on-demand (async via queue)
- STT: on-demand (3s chunks via queue)
- All go through `networkQueue` depth 8 → serialized execution in `networkTask` → no HTTP overlap ✅

### Watchdog Safety
- `oledTask` uses `vTaskDelay(125ms)` — feeds WDT ✅
- `audioMonitorTask` uses `vTaskDelay(5-500ms)` — feeds WDT ✅
- `cameraMonitorTask` uses `vTaskDelay(50ms)` — feeds WDT ✅
- `loop()` uses `vTaskDelay(50ms)` — feeds WDT ✅
- `networkTask` uses `xQueueReceive()` with portMAX_DELAY — FreeRTOS handles WDT while waiting ✅
- OTA loop uses `vTaskDelay()` in retry path — feeds WDT ✅
- **No infinite busy-wait loops found** ✅

---

## Conclusion

The three highest-impact issues are:
1. **No display mutex** → corruption
2. **4× I2C reads per OLED frame** → lag
3. **`enterDeepSleep()` races with oledTask** → corruption + slow wake

Fixing these three (Phase 1 above) should resolve all five observed symptoms. The remaining bugs are stability hardening and are lower risk.
