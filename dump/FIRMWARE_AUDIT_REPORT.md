# KAKU ESP32 Firmware — Comprehensive Verification & Reliability Audit

**File:** `esp32_sketch/esp32_sketch.ino` (6152 lines)  
**Hardware:** XIAO ESP32-S3 Sense | OV2640 | SSD1306 64×32 | MPU6050 | PDM Mic  
**Date:** 2026-03-03

---

## Executive Summary

The firmware is **functionally sound** for normal operation — gesture triggers, STT integration, animation rendering, and network communication all work correctly. However, the audit identified **4 critical issues**, **10 high/medium bugs**, and **~25 lower-severity concerns** that affect long-term stability, thread safety, and resilience. The two most impactful findings are **unprotected cross-core shared state** (`g_petState`, `currentEmotion`, `currentScreenType`) and **I2C bus contention** (MPU6050 read from both cores without mutex). The daily reboot and heap-threshold reboot provide good safety nets that mask many of these issues in practice.

---

## Findings by Severity

### 🔴 CRITICAL (4)

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| C1 | **`g_petState` struct has ZERO synchronization** | L398 | Written by `handlePhysiology()` on Core 1 and by feeding/cleaning/medicine gestures on Core 0 (via oledTask). Concurrent writes can produce torn struct values — e.g., hunger counter wrapping to 255 or health set to garbage. |
| C2 | **`currentEmotion` (Arduino String) shared cross-core unprotected** | L411 | Written by `getOLEDDisplayFromServer()` / `syncLocalStateToUI()` (Core 1), read by `displayPetAnimation()` (Core 0). String assignment involves heap alloc/dealloc — concurrent access can corrupt heap metadata, causing a crash. |
| C3 | **`currentScreenType` (Arduino String) shared cross-core unprotected** | L409 | Written by `cycleMenu()` (Core 0), read by `getOLEDDisplayFromServer()` (Core 1). Same heap corruption risk as C2. |
| C4 | **I2C bus contention — `mpu.getAcceleration()` called from both cores without mutex** | 14+ sites | Core 0 (oledTask — gestures, step detection, game). Core 1 (loop — sensor batch, sleep detection). ESP32 Wire library MAY have an internal mutex (ESP-IDF v4+), but this is undocumented reliance. Corrupt reads → false gestures or I2C lockup. |

**Recommended fixes:**
- **C1:** Add a `SemaphoreHandle_t petStateMutex` and wrap all `g_petState` read/write with take/give.
- **C2/C3:** Replace `String currentEmotion` / `String currentScreenType` with fixed-size `char[]` arrays (e.g., `char currentEmotion[24]`) and protect with a mutex, OR use `networkDataMutex` which already exists.
- **C4:** Add a `SemaphoreHandle_t i2cMutex` and wrap all `mpu.getAcceleration()` / `mpu.getRotation()` calls.

---

### 🟠 HIGH (6)

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| H1 | **No runtime MPU6050 failure detection** | All `mpu.getAcceleration()` sites | If MPU6050 disconnects after init, reads return garbage/zeros silently. Gestures stop working. Sensor data sent to server is corrupted. No re-init attempt. No plausibility check on accel values. |
| H2 | **No I2C error detection for OLED at runtime** | All `display.*()` calls | Adafruit SSD1306 methods are void — if OLED disconnects, draws become no-ops with no error indication. oledTask wastes CPU on invisible renders. |
| H3 | **No error checking on NVS writes** | `savePetState()` L351-366 | `petPrefs.putInt()` returns bytes written (0 on failure) — never checked. `petPrefs.begin()` returns bool — never checked. If NVS is full or flash worn, pet state is silently lost. |
| H4 | **`sendAudioData()` uses `http.begin(url)` without WiFiClientSecure** | L5425 | All other HTTPS functions pass `sslNet` to `http.begin()`. This one passes URL only → likely fails silently for HTTPS or creates an unsafe connection. (Possibly dead code — verify if ever called.) |
| H5 | **base64::encode creates ~213KB String for audio data** | L4432 | `base64::encode(recording_buffer, recorded_bytes)` creates an Arduino String that can be 213KB. Combined with `sendAudioData()` String concatenation, two copies (~426KB) exist simultaneously — likely exceeds internal heap. (Mitigated if this VAD audio path is dead code.) |
| H6 | **`binary_data` in `sendImageData()` uses `malloc()` instead of `ps_malloc()`** | L5322 | Variable-size camera JPEG (10-60KB) allocated from limited internal heap (~390KB) instead of PSRAM. Fragments internal RAM. |

---

### 🟡 MEDIUM (10)

| # | Issue | Location | Impact |
|---|-------|----------|--------|
| M1 | **~13 cross-core booleans/ints lack `volatile`** | L396-639 | `isUploadingImage`, `startupComplete`, `displayReady`, `isDeviceSleeping`, `showFoodIcon`, `showPoopIcon`, `showSickIcon`, `showHomeIcon`, `showPlayIcon`, `petIsHungry`, `petIsSick`, `capturingForFeeding`, `justFinishedEating` — compiler may cache stale values in registers. |
| M2 | **Network queue fire-and-forget — silent drops** | 9+ `xQueueSend` sites | Queue depth 8 with 0-tick timeout. During STT mode (3s sends) + OLED poll (2s) + sensor (10s), queue fills. User actions (feed/clean) silently dropped. No return value checks. |
| M3 | **Sleep mode doesn't check `sttModeActive`** | L3982-3988 | If user activates STT then flips device face-down for 30s, sleep activates. STT mic streaming continues silently for up to 120s timeout — wasted power/resources. |
| M4 | **STT `stt_audio_buffer` race-condition leak (128KB PSRAM)** | L4314 cleanup block | If `sttModeActive` is set false by Core 1 at exactly the moment audioTask wraps its loop, cleanup block is skipped. Buffer leaks until next STT activation or reboot. |
| M5 | **WiFi reconnect only tries most recent stored credential** | L3856-3894 | `connectWithStoredCredentials()` (startup only) tries all saved networks. Loop reconnect only tries the newest one + hardcoded fallback. Older saved networks are ignored. |
| M6 | **No WiFi reconnect backoff** | L3856-3894 | If WiFi persistently disconnected, reconnect attempt runs every loop iteration (50ms cadence, 10s timeout). CPU/radio drain with no cooldown. |
| M7 | **No server-unreachable watchdog** | networkTask | If WiFi connected but server down, device hammers server at full polling rate. `isServerAlive()` exists but is NEVER called (dead code). `trackHttpResult()` resets SSL after 3 errors but doesn't back off. |
| M8 | **`sendSTTAudioChunk()` doesn't check `http.begin()` return** | L5944 | All other HTTP functions check `!http.begin()` and bail. This one proceeds blindly — `http.POST()` will fail unpredictably. |
| M9 | **OTA retry loop defeated — dead code** | L3685-3703 + L4808 | `checkAndPerformOTA()` clears `otaUpdateRequested` on most failure paths. Caller's `for (attempt <= 3 && otaUpdateRequested)` exits after 1st failure. 3-retry mechanism never activates. |
| M10 | **NVS flash wear: ~3,120 key writes/day** | `savePetState()` (12 keys × 260 calls/day) | Borderline for ESP32 NVS flash wear over years. Physiology tick (every 6 min) is the primary contributor. |

---

### 🔵 LOW (25)

<details>
<summary>Click to expand all LOW findings</summary>

| # | Issue | Location |
|---|-------|----------|
| L1 | `vad_buffer` declared without NULL init (`int16_t *vad_buffer;`) | L758 |
| L2 | `audio_buffer` declared but never allocated or used (dead variable) | L755 |
| L3 | `loadPetState()` called twice in setup() (redundant) | L1490, L1583 |
| L4 | `setCpuFrequencyMhz(80)` called before `cpuFreqMutex` created | L1536 |
| L5 | No PSRAM free-space monitoring (`ESP.getFreePsram()` never called) | — |
| L6 | No heap fragmentation monitoring (`heap_caps_get_largest_free_block()`) | — |
| L7 | Daily reboot uses `millis()` which overflows at 49.7 days — reboot stops | L3921 |
| L8 | `notifyServerStartupComplete()` holds HTTP connection open 3s unnecessarily | L5164 delay before L5172 http.end() |
| L9 | `checkCleaningGesture()` has no internal screen type check — relies on caller | L2811 |
| L10 | `initCamera()` return value not checked by setup() caller | L1525 |
| L11 | `initAudio()` return value not checked by setup() caller | L1529 |
| L12 | `xQueueCreate` return not null-checked — if NULL, all queue ops crash | L1564 |
| L13 | `networkTask` handle not saved (NULL) — cannot be suspended externally | L1597 |
| L14 | `sslNet` has no explicit TLS handshake timeout (uses platform default) | — |
| L15 | `setInsecure()` disables TLS certificate verification on all connections | L528 |
| L16 | No explicit OTA rollback API call (`esp_ota_mark_app_valid`) | — |
| L17 | `sendCleanRequest` / `sendCoverHappyRequest` / `sendInjectRequest` missing `setConnectTimeout()` | L5632/5680/5709 |
| L18 | `acknowledgeEvent()` has no WiFi status check | L5770 |
| L19 | I2S channel not freed on partial `initAudio()` failure | L4109-4136 |
| L20 | `i2s_channel_read()` failures silently ignored — infinite retry at 1ms | L4340 |
| L21 | Camera powered continuously via XCLK even when idle | Camera init |
| L22 | Repeated MPU6050 I2C reads in same 100ms frame (3× when on MAIN screen) | gesture checks |
| L23 | `isDeviceInverted()` reads gyro data unnecessarily (only accel used) | L3033 via `readAllSensors()` |
| L24 | `oledTask` stack 4096 is tight for deep call chains + Adafruit SSD1306 | Task creation |
| L25 | `capturedImageBuffer` variable-size PSRAM alloc/free causes fragmentation over days | L4184 |

</details>

---

### ✅ Confirmed Working / Good Practices

| Area | Status |
|------|--------|
| **STT tilt gesture (10s left hold)** | ✅ Properly gated to MAIN screen only. No conflict with feeding (FOOD_MENU) or cleaning (TOILET_MENU). Flag resets correct in all paths. |
| **Feeding gesture (3s left tilt)** | ✅ Properly gated to FOOD_MENU. Double-capture prevented by `cameraImageReady` flag. |
| **Cleaning gesture (3s left tilt)** | ✅ Works — relies on caller for screen guard (fragile but functional). |
| **Menu cycling (2s right tilt)** | ✅ Opposite direction from all left-tilt gestures — physically cannot conflict. |
| **STT timeout (120s)** | ✅ Correctly flag-only — buffer freed by audioMonitorTask, not loop(). No cross-core race on buffer. |
| **Camera fb_get/fb_return pairing** | ✅ All active sites properly pair get/return in all code paths. |
| **All ps_malloc/malloc have NULL checks** | ✅ Every allocation site checks for failure and handles gracefully. |
| **All freed pointers set to NULL** | ✅ Or immediately overwritten. No dangling pointer risks. |
| **sslNet serialized on Core 1** | ✅ All HTTP callers go through networkQueue → networkTask (Core 1). No cross-core sslNet access. |
| **OTA update safety** | ✅ Best HTTP code in the codebase — stall detection, read error tolerance, task suspension, dual-partition A/B, progress reporting. |
| **Heap monitoring + auto-reboot** | ✅ 20KB threshold triggers save + reboot. Daily 24h reboot during sleep. |
| **WiFi power optimization** | ✅ `WiFi.setSleep(true)` + TX power reduced to 11 dBm. |
| **CPU frequency management** | ✅ 80MHz idle, 160/240 boost for work, mutex-protected via `safeCpuFreq()`. |
| **Global StaticJsonDocuments** | ✅ `g_oledDoc` and `g_sensorDoc` avoid repeated heap allocation. |
| **PROGMEM for animation data** | ✅ All frame arrays use PROGMEM. Rendered via `pgm_read_byte()`. |
| **JSON response parsing** | ✅ `containsKey()` used for every optional field. Malformed JSON caught by DeserializationError. |
| **No busy-wait loops** | ✅ All while-loops have yield (`delay()`, `vTaskDelay()`). |
| **Task priorities** | ✅ Audio (pri 2) > camera/OLED (pri 1). No priority inversion risk. |

---

## Dead Code Inventory (~340 lines, ~5.5% of file)

| Function/Variable | Lines | Notes |
|---|---|---|
| `notifyServerStartupComplete()` | ~90 lines | Comment says "REMOVED: Blocking server notification - too slow" |
| `sendAllDataToServer()` | ~70 lines | Replaced by `sendSensorDataOnly()` |
| `sendAudioData()` | ~40 lines | Replaced by STT raw WAV upload |
| `acknowledgeEvent()` | ~35 lines | Events processed but never individually ack'd |
| `isServerAlive()` | ~15 lines | Never called |
| `captureImageBase64()` | ~15 lines | Replaced by binary buffer capture |
| `recordAudioBase64()` | ~5 lines | Stub returning "" |
| `audio_buffer` variable | L755 | Declared, never allocated |
| `currentMode` variable | L420 | Set to "HARDWARE", never read |
| `lastEventPoll`, `dynamicEventPollInterval`, `EVENT_POLL_INTERVAL` | L733-735 | Never used |
| `DISPLAY_CHECK_INTERVAL`, `lastDisplayCheckTime` | L393-395 | Never used |
| `SensorData.camera_image_b64`, `audio_data_b64`, `has_new_image`, `has_new_audio` | L767-770 | Always empty/false |
| Commented-out `checkCameraCover()` | ~30 lines | Dead commented block |

---

## Performance Observations

| Metric | Value | Assessment |
|--------|-------|------------|
| CPU idle frequency | 80 MHz | ✅ Good |
| CPU at 240 MHz | <1% duty cycle (camera capture only) | ✅ |
| CPU at 160 MHz | ~5% normal, ~20% during STT | ✅ Acceptable |
| OLED refresh rate | 10 FPS (100ms vTaskDelay) | ✅ Appropriate for 64×32 |
| Serial baud rate | 115200 | ✅ Standard |
| Serial in hot paths | displayFoodMenu/HealthMenu prints every 100ms frame | ⚠️ Wasteful (~5-10ms/frame) |
| WiFi power | Light-sleep + 11 dBm TX | ✅ Good |
| Thermal hottest moment | Camera capture + WiFi upload (240→160 MHz, ~10s) | ⚠️ Moderate |
| STT thermal | 120s sustained dual-core + WiFi | ⚠️ Moderate |

---

## Top 10 Recommended Fixes (Priority Order)

1. **Add mutex for `g_petState`** — Prevents torn struct reads/writes between cores. Use `xSemaphoreTake`/`Give` around all access.

2. **Replace `String currentEmotion`/`currentScreenType` with `char[]` + mutex** — Eliminates heap corruption risk from cross-core String operations. E.g., `char currentEmotion[24]; char currentScreenType[24];` protected by `networkDataMutex`.

3. **Add I2C mutex for MPU6050 access** — Create `i2cMutex`, wrap all `mpu.getAcceleration()`/`getRotation()` calls. Prevents bus contention between Core 0 (gestures) and Core 1 (sensor batch).

4. **Add `volatile` to ~13 cross-core flags** — Simple one-word fix per variable. Prevents compiler from caching stale values.

5. **Add `&& !sttModeActive` to sleep entry condition** (L3982) — Prevents entering sleep while mic is streaming.

6. **Check `http.begin()` return in `sendSTTAudioChunk()`** (L5944) — Add `if (!http.begin(sslNet, url)) { free(wav_data); return; }`.

7. **Fix OTA retry: don't clear `otaUpdateRequested` on download failure** — Let the caller's retry loop work as designed.

8. **Change `malloc()` to `ps_malloc()` in `sendImageData()`** (L5322) — Moves variable-size image copy to PSRAM, preserving internal heap.

9. **Add NVS write error checking in `savePetState()`** — Check `petPrefs.begin()` return and at least one `putInt()` return.

10. **Throttle Serial prints in `displayFoodMenu()`/`displayHealthMenu()`** — Gate with a 3-second timer like `displayPetAnimation()` already does.

---

## Architecture Diagram

```
Core 0                              Core 1
┌──────────────────────┐     ┌──────────────────────┐
│ audioMonitorTask     │     │ loop()               │
│  (pri 2, 12K stack)  │     │  WiFi reconnect      │
│  PDM mic → VAD/STT   │     │  Sensor scheduling   │
│                      │     │  Sleep detection      │
│ cameraMonitorTask    │     │  STT/feeding timeout  │
│  (pri 1, 8K stack)   │     │  Heap monitoring      │
│  On-demand capture   │     │                      │
│                      │     │ networkTask           │
│ oledTask             │     │  (pri 1, 16K stack)  │
│  (pri 1, 4K stack)   │     │  Queue-driven HTTP   │
│  10 FPS render       │     │  sslNet (shared)     │
│  Gesture detection   │     │  OTA updates         │
└──────────────────────┘     └──────────────────────┘
         │                            │
         └────── networkQueue ────────┘
                 (depth 8, 1B)

Shared State (UNPROTECTED):
  g_petState ←→ Core 0 + Core 1
  currentEmotion ←→ Core 0 + Core 1
  currentScreenType ←→ Core 0 + Core 1
  ~13 bool/int flags ←→ Core 0 + Core 1

Protected State:
  stt_wav_to_send ← sttDataMutex
  capturedImageBuffer ← cameraMutex
  detectedAudioData ← audioMutex
  CPU frequency ← cpuFreqMutex
  networkData ← networkDataMutex
```

---

## Long-Term Stability Assessment

| Factor | Rating | Notes |
|--------|--------|-------|
| Memory leaks | ⬛⬛⬛⬜⬜ | STT buffer race leak (128KB). Mitigated by daily reboot. |
| Heap fragmentation | ⬛⬛⬛⬜⬜ | Variable-size PSRAM allocs + internal heap image copy. Daily reboot mitigates. |
| Thread safety | ⬛⬛⬜⬜⬜ | Critical: g_petState, Strings, I2C all unprotected cross-core. |
| Network resilience | ⬛⬛⬛⬜⬜ | Good self-healing SSL, but no backoff, no server-down detection. |
| Error recovery | ⬛⬛⬛⬛⬜ | 20KB heap reboot + daily reboot + per-allocation failure handling. |
| Hardware resilience | ⬛⬛⬜⬜⬜ | No runtime detection of MPU/OLED/camera/mic failure after init. |
| Flash wear | ⬛⬛⬛⬜⬜ | ~3K NVS writes/day. Multi-year concern. |
| Power efficiency | ⬛⬛⬛⬛⬜ | Good CPU/WiFi management. Camera stays powered unnecessarily. |
| Code maintainability | ⬛⬛⬛⬜⬜ | ~340 lines dead code. Single 6K-line file. |

**Overall: The firmware will run reliably for days-to-weeks as-is, with the daily reboot providing an effective reset. The thread-safety issues (C1-C4) are the only findings that could cause a runtime crash in normal operation — the probability increases with heavy concurrent usage (e.g., feeding while server poll returns, or STT while sensor batch reads MPU).** 

---

*Audit temp files can be removed: `audit_part3_memory.txt`, `audit_part4_5_network_errors.txt`, `audit_part6_7_8_perf_power_oled.txt`*
