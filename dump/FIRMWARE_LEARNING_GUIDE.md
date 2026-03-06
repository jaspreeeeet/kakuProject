# KAKU Firmware Learning Guide
## A Complete Embedded Systems Education Through Your Own Project

**Target Audience:** Beginner to intermediate embedded systems learners  
**Firmware File:** `esp32_sketch/esp32_sketch.ino` (~6,300 lines)  
**Hardware:** XIAO ESP32-S3 Sense + MPU6050 + SSD1306 OLED 64×32  
**Date:** 2026-03-03  

---

> **How to read this document:**  
> Every major concept is explained in **plain English first**, then **technically**. Code snippets reference actual lines in your firmware. Diagrams use ASCII art. Each section explains *why* a technique was chosen, *what breaks if done wrong*, and *what alternatives exist*.

---

# Table of Contents

1. [Project Overview](#chapter-1-project-overview)
2. [FreeRTOS & Multi-Core Architecture](#chapter-2-freertos--multi-core-architecture)
3. [Memory Management](#chapter-3-memory-management)
4. [Concurrency & Synchronization](#chapter-4-concurrency--synchronization)
5. [Networking & HTTPS](#chapter-5-networking--https)
6. [I2C & Sensor Handling](#chapter-6-i2c--sensor-handling)
7. [Camera Pipeline](#chapter-7-camera-pipeline)
8. [Audio & STT Pipeline](#chapter-8-audio--stt-pipeline)
9. [OLED Rendering](#chapter-9-oled-rendering)
10. [OTA Update System](#chapter-10-ota-update-system)
11. [Reliability & Stability Concepts](#chapter-11-reliability--stability-concepts)
12. [Power & Thermal Optimization](#chapter-12-power--thermal-optimization)
13. [Common Embedded Mistakes (Using Your Code As Example)](#chapter-13-common-embedded-mistakes-using-your-code-as-example)
14. [How To Improve Further](#chapter-14-how-to-improve-further)
15. [Glossary](#chapter-15-glossary)

---

# Chapter 1: Project Overview

## What Is This Project?

Imagine a Tamagotchi — that little digital pet from the 1990s that lived on a keychain. You fed it, played with it, cleaned up after it, and if you neglected it, it got sick.

Your KAKU project is a **modern Tamagotchi** built on professional-grade hardware. Instead of a tiny plastic toy with a single-core 4-bit CPU, you have:

- A **dual-core 240MHz processor** with 8MB of extra RAM
- A **camera** that can see what you're feeding it
- A **microphone** that listens for your voice
- An **accelerometer/gyroscope** that detects tilt gestures
- A **tiny OLED screen** that shows pet animations
- **WiFi** that connects to a cloud server for AI processing
- **Over-the-air updates** so you can upgrade the firmware without opening the case

This isn't a toy project — it uses the same techniques found in commercial IoT products, medical devices, and industrial controllers. Every chapter in this guide teaches real-world embedded systems concepts through code you wrote yourself.

## The Hardware

### XIAO ESP32-S3 Sense

```
┌─────────────────────────────────────────────┐
│              XIAO ESP32-S3 Sense            │
│                                             │
│  ┌─────────┐  ┌──────────┐  ┌───────────┐  │
│  │ Xtensa  │  │ 8MB      │  │ WiFi      │  │
│  │ LX7     │  │ PSRAM    │  │ 802.11    │  │
│  │ Dual    │  │ (extra   │  │ b/g/n     │  │
│  │ Core    │  │  RAM)    │  │           │  │
│  │ 240MHz  │  │          │  │ BLE 5.0   │  │
│  └─────────┘  └──────────┘  └───────────┘  │
│                                             │
│  ┌─────────┐  ┌──────────┐  ┌───────────┐  │
│  │ OV2640  │  │ PDM Mic  │  │ 512KB     │  │
│  │ Camera  │  │ (built   │  │ SRAM      │  │
│  │ (built  │  │  in)     │  │ (main     │  │
│  │  in)    │  │          │  │  RAM)     │  │
│  └─────────┘  └──────────┘  └───────────┘  │
│                                             │
│  GPIO Pins: 5(SDA) 6(SCL) 42(MicCLK)       │
│             41(MicDIN) 10(CamXCLK) ...      │
└─────────────────────────────────────────────┘
```

**Why this chip?**  
The ESP32-S3 is one of the most capable microcontrollers available for under $15. It has:
- **Two CPU cores** — one can handle audio while the other handles WiFi. On a single-core chip, recording audio would block network requests.
- **8MB PSRAM** — camera frames and audio buffers need lots of memory. The chip's internal 512KB SRAM isn't enough. PSRAM provides extra space at the cost of slightly slower access.
- **Built-in camera and microphone** — the "Sense" variant has these on-board. No wiring needed.
- **Hardware TLS/SSL** — HTTPS encryption is accelerated in hardware. Without this, a single HTTPS request would take 5+ seconds on a microcontroller.

**What if you used a cheaper chip?**  
An ESP32 (original, not S3) has no PSRAM by default and a weaker CPU. Camera + audio + WiFi simultaneously would be impossible. An Arduino Uno has 2KB of RAM and no WiFi — it couldn't even store a single camera frame.

### External Components

| Component | Connection | What It Does |
|-----------|-----------|--------------|
| **MPU6050** | I2C: GPIO 5 (SDA), GPIO 6 (SCL) | Measures tilt, rotation, steps |
| **SSD1306 OLED** | I2C: Same bus as MPU6050 | 64×32 pixel display for pet animations |
| **OV2640 Camera** | Built-in (13 GPIO pins) | 160×120 JPEG images for food recognition |
| **PDM Microphone** | Built-in: GPIO 42 (CLK), GPIO 41 (DIN) | Voice detection & speech-to-text |

## System Architecture — The Big Picture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         ESP32-S3 Dual Core                         │
│                                                                     │
│  ┌─────────────────────────┐   ┌─────────────────────────────────┐ │
│  │     CORE 0              │   │          CORE 1                 │ │
│  │  "Sensor Core"          │   │     "Network Core"              │ │
│  │                         │   │                                 │ │
│  │  audioMonitorTask       │   │  loop()                         │ │
│  │   - PDM mic reading     │   │   - WiFi reconnect              │ │
│  │   - Voice detection     │   │   - Sensor batch (I2C)          │ │
│  │   - STT streaming       │   │   - Gesture detection           │ │
│  │   Priority: 2 (HIGH)    │   │   - Sleep/walk detection        │ │
│  │   Stack: 12KB           │   │   - Physiology engine           │ │
│  │                         │   │   - Queue network requests      │ │
│  │  cameraMonitorTask      │   │                                 │ │
│  │   - On-demand capture   │   │  networkTask                    │ │
│  │   - PSRAM buffering     │   │   - Queue-driven HTTP           │ │
│  │   Priority: 1           │   │   - Sensor upload (2s)          │ │
│  │   Stack: 8KB            │   │   - OLED poll (5s)              │ │
│  │                         │   │   - Image/audio upload          │ │
│  │  oledTask               │   │   - OTA firmware update         │ │
│  │   - Pet animation       │   │   Priority: 1                   │ │
│  │   - Menu rendering      │   │   Stack: 16KB                   │ │
│  │   - Icon display        │   │                                 │ │
│  │   Priority: 1           │   │                                 │ │
│  │   Stack: 8KB            │   │                                 │ │
│  └────────────┬────────────┘   └──────────────┬──────────────────┘ │
│               │                                │                    │
│               └────────── SHARED ──────────────┘                    │
│                    8 Mutexes + 1 Queue                              │
│                    Global SSL clients                               │
│                    Shared state structs                              │
└─────────────────────────────────────────────────────────────────────┘
                              │
                              │ HTTPS (TLS 1.2)
                              ▼
                    ┌──────────────────┐
                    │  Google Cloud    │
                    │  Run Server      │
                    │  (Flask/Python)  │
                    │                  │
                    │  - AI processing │
                    │  - Pet state DB  │
                    │  - Dashboard     │
                    │  - OTA hosting   │
                    └──────────────────┘
```

### Why Two Cores?

**Simple analogy:** Imagine you're cooking dinner while talking on the phone. You can stir the pot (Core 0: sensors) while having a conversation (Core 1: network). If you had only one hand (single core), you'd have to put the phone down every time you stirred, missing parts of the conversation.

**Technical explanation:** The ESP32-S3 has two Xtensa LX7 cores. FreeRTOS (the operating system) lets you "pin" tasks to specific cores:
- **Core 0** handles time-sensitive local work: reading the microphone continuously (16,000 samples/second), capturing camera frames, and drawing to the OLED.
- **Core 1** handles network-bound work: WiFi, HTTPS requests (which can take 500ms–3s each), and the main `loop()` which collects sensor data and detects gestures.

**What if both tasks ran on one core?** If audio monitoring and WiFi shared a core, every HTTP request (which blocks for 500ms+) would create a 500ms gap in audio recording. Speech detection would miss words. Camera captures during a network request would stall.

### Data Flow Overview

```
User tilts device left (feeding gesture)
         │
         ▼
loop() on Core 1 detects tilt via MPU6050
         │
         ▼
Sets capturingForFeeding = true
         │
         ▼
cameraMonitorTask on Core 0 sees the flag
         │
         ▼
Captures JPEG, stores in PSRAM under cameraMutex
         │
         ▼
Sets cameraImageReady = true
         │
         ▼
loop() on Core 1 sees cameraImageReady
         │
         ▼
Sends NET_IMAGE to networkQueue
         │
         ▼
networkTask on Core 1 dequeues NET_IMAGE
         │
         ▼
POSTs image to server via HTTPS (sslNet)
         │
         ▼
Server runs food recognition AI
         │
         ▼
Next OLED poll: server returns updated pet state
         │
         ▼
OLED shows eating animation
```

This pipeline crosses two cores, uses mutexes, a queue, PSRAM, and HTTPS — all concepts covered in later chapters.

---

# Chapter 2: FreeRTOS & Multi-Core Architecture

## What Is FreeRTOS?

**Simple explanation:** Your ESP32 has two brains (CPU cores), and it needs to run five different jobs at the same time: listen to the microphone, watch through the camera, animate the screen, handle WiFi, and manage the main game logic. FreeRTOS is the **traffic controller** that decides which job runs when, for how long, and on which brain.

**Technical explanation:** FreeRTOS (Free Real-Time Operating System) is a preemptive multitasking kernel. It provides:
- **Tasks** — independent threads of execution, each with its own stack
- **Scheduling** — a priority-based round-robin scheduler that decides which task runs
- **Synchronization primitives** — mutexes, semaphores, and queues for safe inter-task communication
- **Timing** — `vTaskDelay()` for sleeping without busy-waiting

On Arduino, you normally have just `setup()` and `loop()`. That's one task. FreeRTOS lets you create as many tasks as you want, each running "simultaneously" (the scheduler rapidly switches between them, giving the illusion of parallelism — and on two cores, some actually run in parallel).

## Tasks In Your Firmware

Your firmware creates **5 tasks** (4 explicitly + the Arduino `loop()`):

```cpp
// From setup() — actual code in your firmware:

// Task 1: Audio monitoring (Core 0, Priority 2, 12KB stack)
xTaskCreatePinnedToCore(audioMonitorTask, "AudioMonitor", 12288,
                        NULL, 2, &audioTaskHandle, 0);

// Task 2: Camera monitoring (Core 0, Priority 1, 8KB stack)
xTaskCreatePinnedToCore(cameraMonitorTask, "CameraMonitor", 8192,
                        NULL, 1, &cameraTaskHandle, 0);

// Task 3: OLED animation (Core 0, Priority 1, 8KB stack)
xTaskCreatePinnedToCore(oledTask, "OLED", 8192,
                        NULL, 1, &oledTaskHandle, 0);

// Task 4: Network handler (Core 1, Priority 1, 16KB stack)
xTaskCreatePinnedToCore(networkTask, "Network", 16384,
                        NULL, 1, NULL, 1);

// Task 5: loop() — runs on Core 1, Priority 1 (Arduino default)
```

### Understanding `xTaskCreatePinnedToCore()`

Let's break down this call:

```cpp
xTaskCreatePinnedToCore(
    audioMonitorTask,  // Function to run (like main() for this task)
    "AudioMonitor",    // Human-readable name (for debugging)
    12288,             // Stack size in bytes (12KB)
    NULL,              // Parameter to pass to the function
    2,                 // Priority (higher number = higher priority)
    &audioTaskHandle,  // Pointer to store the task handle
    0                  // Core to pin to (0 or 1)
);
```

**Stack size** is how much memory each task gets for its local variables, function call chain, and temporary data. Think of it as the task's personal workspace. If a task needs more than its stack allows, it crashes with a "stack overflow." (Chapter 3 covers this in depth.)

**Priority** determines who runs first when multiple tasks are ready. In your firmware:
- **Priority 2:** `audioMonitorTask` — highest, because missing audio samples creates gaps in recordings
- **Priority 1:** Everything else — camera, OLED, network, loop

**Core pinning** locks a task to a specific core. Without pinning, FreeRTOS can move tasks between cores, which complicates cache behavior and I2C bus sharing.

### Why These Specific Core Assignments?

```
CORE 0 (Sensor Core)                 CORE 1 (Network Core)
┌──────────────────────┐             ┌──────────────────────┐
│ audioMonitorTask (P2)│             │ loop()          (P1) │
│  - Reads mic 16kHz   │             │  - Gesture detect    │
│  - Must not miss     │             │  - Physiology tick   │
│    samples           │             │  - WiFi reconnect    │
│                      │             │  - Sensor batch I2C  │
│ cameraMonitorTask(P1)│             │                      │
│  - Occasional, bursty│             │ networkTask     (P1) │
│  - Uses PSRAM (slow) │             │  - HTTP requests     │
│                      │             │  - Each takes 500ms+ │
│ oledTask        (P1) │             │  - Uses WiFi radio   │
│  - 50ms frame rate   │             │  - OTA updates       │
│  - I2C writes (fast) │             │                      │
└──────────────────────┘             └──────────────────────┘
```

**Why is audio on Core 0?** Audio recording at 16kHz means a new sample every 62.5 microseconds. If the WiFi stack (which lives on Core 1 by default) interrupted audio recording, you'd get clicks and gaps. Pinning audio to Core 0 isolates it from WiFi interrupts.

**Why is networking on Core 1?** The ESP32's WiFi/BLE radio driver runs on Core 0 at a low level, but the Arduino WiFi library expects to be called from Core 1. More importantly, HTTP requests block for hundreds of milliseconds — putting them on Core 0 would freeze the OLED and audio.

**What if you put everything on one core?** The scheduler would time-slice: run audio for 1ms, switch to camera for 1ms, switch to OLED for 1ms, etc. But context switching has overhead (~5μs each), and HTTP blocking would starve all other tasks. Audio quality would degrade noticeably.

### Task Lifecycle

Every FreeRTOS task is an infinite loop:

```cpp
void audioMonitorTask(void *parameter) {
    // One-time setup
    Serial.println("Audio task started");
    
    while (true) {        // ← Infinite loop — task NEVER returns
        // Do work
        readMicrophone();
        
        // Yield CPU to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));  // Sleep 10ms
    }
    // Code here is UNREACHABLE — if you accidentally 
    // break out, FreeRTOS crashes
}
```

**Critical rule:** A FreeRTOS task function must NEVER return. If it does, the behavior is undefined (usually a crash). That's why every task in your firmware has `while (true)` at its core.

### `vTaskDelay()` vs `delay()`

```cpp
// BAD — Arduino delay: busy-waits, wastes CPU, blocks other tasks on same core
delay(100);

// GOOD — FreeRTOS delay: yields CPU, other tasks can run during the wait
vTaskDelay(pdMS_TO_TICKS(100));
```

**Simple analogy:** `delay(100)` is like standing in line at the store doing nothing for 100ms. `vTaskDelay(100)` is like saying "wake me up in 100ms" and going to sleep — the store serves other customers while you wait.

`pdMS_TO_TICKS()` converts milliseconds to FreeRTOS "ticks" (the scheduler's internal time unit). On ESP32, 1 tick = 1ms by default, so they're the same — but using the macro is correct practice because tick rate can be configured differently on other platforms.

### Task Suspension and Resumption

During OTA (over-the-air firmware update), your firmware suspends all non-essential tasks:

```cpp
// From checkAndPerformOTA() — suspending tasks to free resources:
otaInProgress = true;
if (audioTaskHandle)  vTaskSuspend(audioTaskHandle);
if (cameraTaskHandle) vTaskSuspend(cameraTaskHandle);
if (oledTaskHandle)   vTaskSuspend(oledTaskHandle);

// ... OTA download and flash ...

// Resume on failure:
if (oledTaskHandle)   vTaskResume(oledTaskHandle);
if (cameraTaskHandle) vTaskResume(cameraTaskHandle);
if (audioTaskHandle)  vTaskResume(audioTaskHandle);
```

**Why suspend?** OTA downloads a firmware binary over HTTPS and writes it to flash. This needs maximum CPU, memory, and WiFi bandwidth. Suspending audio/camera/OLED frees ~30KB of stack space and eliminates CPU contention.

**What if you didn't suspend?** The audio task might allocate a recording buffer mid-OTA, fragmenting PSRAM. The camera might trigger a capture, competing for the SSL connection. The OLED task might try to draw while the OTA code is writing to the screen.

**Risk of suspension:** If OTA fails and you forget to resume a task, that feature is permanently dead until reboot. Your code handles this with `vTaskResume()` in every failure path — but it's easy to miss one.

### The Network Queue Pattern

Instead of having `loop()` call HTTP functions directly, your firmware uses a **producer-consumer queue**:

```cpp
// Created in setup():
networkQueue = xQueueCreate(8, sizeof(uint8_t));
//                          ↑         ↑
//                    8 slots    Each slot is 1 byte (the request type)

// Producer (loop() on Core 1):
uint8_t req = NET_SENSOR;
xQueueSend(networkQueue, &req, 0);  // 0 = don't wait if queue full

// Consumer (networkTask on Core 1):
uint8_t reqType;
if (xQueueReceive(networkQueue, &reqType, pdMS_TO_TICKS(100)) == pdTRUE) {
    switch (reqType) {
        case NET_SENSOR: sendSensorDataOnly(dataCopy); break;
        case NET_OLED:   getOLEDDisplayFromServer();   break;
        case NET_IMAGE:  sendImageData("");             break;
        case NET_STT:    sendSTTAudioChunk();           break;
        // ...
    }
}
```

**Why a queue instead of direct calls?**

1. **Serialization:** HTTP requests must happen one at a time (single SSL connection). Without a queue, two requests could collide if `loop()` triggered a sensor send while a camera upload was in progress.

2. **Decoupling:** `loop()` doesn't need to know how long an HTTP request takes. It just drops a message in the queue and moves on. `networkTask` processes them in order.

3. **Backpressure:** If the network is slow, the queue fills up (8 slots). New requests with timeout=0 are silently dropped rather than blocking `loop()`.

**What if you used direct function calls instead?**
```cpp
// BAD — calling HTTP directly from loop():
void loop() {
    sendSensorData();     // Blocks 500ms
    getOLEDDisplay();     // Blocks 500ms
    // Total: 1000ms — gesture detection is frozen for 1 second
}
```
With a queue:
```cpp
// GOOD — queue-based:
void loop() {
    xQueueSend(networkQueue, &sensorReq, 0);  // Returns in ~1μs
    detectGestures();  // Runs immediately, no blocking
}
```

### The Request Type Enum

```cpp
enum NetReqType : uint8_t {
    NET_SENSOR     = 0,  // Sensor batch → every 2s
    NET_OLED       = 1,  // Poll OLED state + events → every 5s
    NET_EVENTS     = 2,  // UNUSED (events now bundled with OLED)
    NET_IMAGE      = 3,  // Camera image → on feeding gesture
    NET_CLEAN      = 4,  // Cleaning request → left tilt on TOILET_MENU
    NET_INJECT     = 5,  // Medicine request → left tilt on HEALTH_MENU
    NET_HAPPY      = 6,  // Happiness boost → right tilt menu cycle
    NET_GAME_REWARD = 7, // Game score + KakuCoin → after game over
    NET_STT        = 8   // WAV audio chunk → during STT mode
};
```

Each type is just a number (1 byte). The queue passes this byte from producer to consumer. The `switch` statement in `networkTask` dispatches to the correct handler.

**Design lesson:** `NET_EVENTS` (ID 2) is defined but unused. Events were originally a separate HTTP call but were later bundled into the `NET_OLED` response to save an SSL handshake. The enum value was kept for backward compatibility, not deleted — a common real-world practice.

---

# Chapter 3: Memory Management

## The Memory Landscape

**Simple explanation:** Your ESP32 has two kinds of memory: fast (SRAM) and slow-but-plentiful (PSRAM). Think of SRAM as your desk (small, within arm's reach) and PSRAM as a filing cabinet across the room (bigger, but takes longer to grab things from).

**Technical breakdown:**

```
┌─────────────────────────────────────────────┐
│            ESP32-S3 Memory Map              │
│                                             │
│  ┌──────────────────────┐                   │
│  │    512KB SRAM         │ ← Fast (1 cycle) │
│  │  ┌────────────────┐  │                   │
│  │  │ FreeRTOS heap  │  │ ~390KB usable     │
│  │  │ Task stacks    │  │                   │
│  │  │ Global vars    │  │                   │
│  │  │ Static buffers │  │                   │
│  │  └────────────────┘  │                   │
│  └──────────────────────┘                   │
│                                             │
│  ┌──────────────────────┐                   │
│  │    8MB PSRAM          │ ← Slower (SPI)   │
│  │  ┌────────────────┐  │                   │
│  │  │ Camera frames  │  │ Via ps_malloc()   │
│  │  │ Audio buffers  │  │                   │
│  │  │ Image copies   │  │                   │
│  │  │ STT WAV chunks │  │                   │
│  │  └────────────────┘  │                   │
│  └──────────────────────┘                   │
│                                             │
│  ┌──────────────────────┐                   │
│  │   NVS (Flash)         │ ← Non-volatile   │
│  │  ┌────────────────┐  │   Survives reboot │
│  │  │ WiFi creds     │  │   Slow writes     │
│  │  │ Pet state      │  │   ~3000 bytes/sec │
│  │  └────────────────┘  │                   │
│  └──────────────────────┘                   │
└─────────────────────────────────────────────┘
```

### SRAM — The Precious Resource

You have ~390KB of usable SRAM. That sounds like a lot, but look at what consumes it:

| Consumer | Size | Notes |
|----------|------|-------|
| FreeRTOS kernel | ~20KB | Scheduler, idle tasks |
| WiFi stack | ~40KB | Always allocated when WiFi is active |
| TLS/SSL buffers | ~32KB per connection | You have 2 (`sslOTA` + `sslNet`) = ~64KB |
| audioMonitorTask stack | 12KB | Large because of STT WAV building |
| cameraMonitorTask stack | 8KB | PSRAM heap operations |
| oledTask stack | 8KB | Deep animation call chains |
| networkTask stack | 16KB | HTTP + JSON + OTA all on this stack |
| Arduino loop() stack | 8KB | Default Arduino task |
| Global variables | ~5KB | State structs, counters, flags |
| **Total** | **~181KB** | Leaving ~209KB free for heap |

**What is "heap free"?** It's the SRAM that hasn't been allocated yet. Your firmware monitors this:

```cpp
// From loop() — the heap guard:
uint32_t freeHeap = ESP.getFreeHeap();
if (freeHeap < 20000) {  // Below 20KB = danger zone
    Serial.println("🚨 CRITICAL: Heap below 20KB — rebooting to recover!");
    savePetState();
    ESP.restart();
}
```

**Why 20KB threshold?** When free heap drops below ~20KB, the next `malloc()` (memory allocation) might fail. WiFi internally allocates buffers during each HTTP request. If allocation fails mid-request, the WiFi stack enters an inconsistent state and crashes. Rebooting is safer than continuing with critically low memory.

### Stack vs Heap

```
STACK (each task has its own)          HEAP (shared by all tasks)
┌──────────────────────┐               ┌──────────────────────┐
│ Local variables      │               │ Dynamically allocated│
│ Function arguments   │               │ malloc() / new       │
│ Return addresses     │               │ Can be any size      │
│                      │               │ Must be freed        │
│ Fixed size (set at   │               │ Shared between all   │
│ task creation)       │               │ tasks                │
│                      │               │                      │
│ Grows DOWN ↓         │               │ Can fragment over    │
│ Overflow = crash     │               │ time                 │
└──────────────────────┘               └──────────────────────┘
```

**Stack overflow example:** The OLED task originally had a 4096-byte stack. When deep animation functions called other functions that called other functions (like `displayPetAnimation()` → `drawIcon()` → `display.drawBitmap()`), the stack grew past 4096 bytes and corrupted adjacent memory. The fix was increasing it to 8192.

```cpp
// BEFORE (too small):
xTaskCreatePinnedToCore(oledTask, "OLED", 4096, ...);

// AFTER (safe):
xTaskCreatePinnedToCore(oledTask, "OLED", 8192, ...);
```

**How do you know the right stack size?** FreeRTOS has `uxTaskGetStackHighWaterMark()` which returns how much stack space was NEVER used (the high water mark). If it returns 200, you only had 200 bytes to spare — dangerously close to overflow. A good rule of thumb: set the stack to 2× the actual usage, measured during the worst case (deepest function call chain).

### PSRAM — The Workhorse

PSRAM (Pseudo-Static RAM) is 8MB of extra memory connected via SPI bus. It's slower than SRAM (~4× latency) but vastly larger.

**When to use PSRAM:**
- Large buffers that don't need to be accessed at microsecond speed
- Camera frames (tens of KB each)
- Audio recording buffers
- Image copies for cross-core transfer

**How to allocate PSRAM:**

```cpp
// Regular malloc — uses SRAM (fast, limited):
uint8_t *buffer = (uint8_t *)malloc(4096);

// PSRAM malloc — uses PSRAM (slower, 8MB available):
uint8_t *buffer = (uint8_t *)ps_malloc(4096);
```

**Your firmware's PSRAM usage:**

```cpp
// Camera image buffering (cameraMonitorTask):
capturedImageBuffer = (uint8_t *)ps_malloc(fb->len);
if (capturedImageBuffer) {
    memcpy(capturedImageBuffer, fb->buf, fb->len);
}

// STT audio buffer:
stt_audio_buffer = (uint8_t *)ps_malloc(STT_MAX_AUDIO_SIZE);
// STT_MAX_AUDIO_SIZE = 16000 * 2 * 4 = 128KB

// STT WAV chunk for network send:
uint8_t *wav_buf = (uint8_t *)ps_malloc(wav_total);
```

**What if PSRAM allocation fails?** Always check the return value:

```cpp
capturedImageBuffer = (uint8_t *)ps_malloc(fb->len);
if (capturedImageBuffer) {
    // Success — use the buffer
    memcpy(capturedImageBuffer, fb->buf, fb->len);
} else {
    // PSRAM exhausted — handle gracefully
    Serial.println("❌ Failed to allocate image buffer");
}
```

If you don't check and write to a NULL pointer, the ESP32 reboots with a "LoadProhibited" exception. You'll see this in the serial monitor as a cryptic crash dump.

### Static vs Dynamic Allocation

**Dynamic allocation** (`malloc`/`free`, `new`/`delete`) requests memory at runtime. It's flexible but dangerous on embedded systems:
- **Fragmentation:** After many allocate/free cycles, free memory becomes scattered into small unusable chunks
- **Determinism:** `malloc` can take unpredictable amounts of time
- **Leaks:** Forgetting `free()` slowly consumes all memory

**Static allocation** reserves memory at compile time. It's predictable and safe:

```cpp
// DYNAMIC (heap, risky on embedded):
DynamicJsonDocument doc(4096);  // Allocates 4KB from heap each call

// STATIC (stack or global, safe):
StaticJsonDocument<4096> g_sensorDoc;  // Fixed 4KB, allocated once
```

Your firmware uses **global StaticJsonDocuments** to avoid heap fragmentation:

```cpp
// Allocated once at startup, reused forever:
StaticJsonDocument<768> g_oledDoc;     // For OLED/events polling
StaticJsonDocument<4096> g_sensorDoc;  // For sensor data sending
```

**Why this matters:** If every sensor send created a `DynamicJsonDocument<4096>`, that's a 4KB allocation + deallocation every 2 seconds. After days of operation, the heap becomes fragmented — plenty of total free space, but no single block large enough for 4KB. The next allocation fails, and you crash.

### NVS (Non-Volatile Storage)

NVS is flash-based storage that survives reboots. Your firmware uses it to persist:

```cpp
// Saving pet state to NVS:
void savePetState() {
    if (petStateMutex) xSemaphoreTake(petStateMutex, portMAX_DELAY);
    petPrefs.begin("pet", false);  // Open namespace "pet", read-write
    petPrefs.putInt("hunger", g_petState.hunger);
    petPrefs.putInt("health", g_petState.health);
    petPrefs.putInt("happiness", g_petState.happiness);
    // ... more fields ...
    petPrefs.end();
    if (petStateMutex) xSemaphoreGive(petStateMutex);
}
```

**Flash wear:** Flash memory has limited write cycles (~100,000 per sector). NVS uses wear-leveling internally, but writing every 2 minutes is borderline. Your physiology engine ticks every 120 seconds and saves state — that's 720 writes/day, which would last ~138 days per sector. NVS distributes writes across multiple sectors, extending lifetime significantly, but it's still worth being aware of.

**Alternative:** Store state in battery-backed SRAM (not available on ESP32-S3) or reduce save frequency to once every 10 minutes.

---

# Chapter 4: Concurrency & Synchronization

## The Fundamental Problem

**Simple explanation:** Imagine two people trying to write on the same whiteboard at the same time. Person A is writing "Temperature: 25°C" while Person B erases it and writes "Temperature: 30°C". The result might be "Temperature: 30°C" or "Temperatu30°C" — corrupted data. This is a **race condition**.

**Technical explanation:** When two tasks share data without synchronization, one task might read data while the other is halfway through modifying it. The result is unpredictable — sometimes correct, sometimes corrupted. Worse, race conditions are intermittent: they might happen once per hour or once per week, making them nearly impossible to reproduce.

## Mutexes — The Bathroom Lock

**Simple explanation:** A mutex is like a bathroom lock. Only one person can use the bathroom at a time. If someone else wants to use it, they wait outside until the first person unlocks the door.

**Technical explanation:** A mutex (Mutual Exclusion) is a binary lock. A task "takes" the mutex before accessing shared data and "gives" it back afterward. Any other task trying to take the same mutex will block until it's released.

### Your Firmware's 8 Mutexes

```
Mutex              Timeout    What It Protects              Typical Contention
─────────────────────────────────────────────────────────────────────────────
petStateMutex      FOREVER    g_petState struct             Core 1 read/write
i2cMutex           200ms      MPU6050 I2C bus               Core 0 & 1 both access
uiStringsMutex     20ms       currentScreenType/Emotion     Multiple writers
cpuFreqMutex       10ms       setCpuFrequencyMhz() calls    Both cores
audioMutex         100ms      detectedAudioData buffer      Core 0 write, Core 1 read
cameraMutex        100ms      capturedImageBuffer           Core 0 write, Core 1 read
networkDataMutex   50ms       g_pendingSensor struct         Core 1 writers
sttDataMutex       50-100ms   stt_wav_to_send buffer        Core 0 write, Core 1 read
```

### Mutex Usage Pattern

```cpp
// PATTERN: Take → Use → Give
if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    // ✅ We have exclusive access to I2C bus
    mpu.getAcceleration(&ax, &ay, &az);
    mpu.getRotation(&gx, &gy, &gz);
    xSemaphoreGive(i2cMutex);  // Release so others can use I2C
} else {
    // ⚠️ Couldn't get mutex within 200ms — someone else is holding it
    Serial.println("[I2C] Mutex timeout");
    // Fill with safe defaults instead of reading stale hardware
    reading.accel_x = 0.0;
    reading.accel_y = 0.0;
    reading.accel_z = 0.0;
}
```

### Why Different Timeouts?

**`portMAX_DELAY` (wait forever):** Used for `petStateMutex` because the pet state is critical — every access must succeed. If it took 5 seconds, you'd rather wait than proceed with wrong data.

```cpp
if (petStateMutex) xSemaphoreTake(petStateMutex, portMAX_DELAY);
g_petState.hunger = min(100, g_petState.hunger + hungerDecay);
if (petStateMutex) xSemaphoreGive(petStateMutex);
```

**200ms (`i2cMutex`):** I2C reads should take <5ms. If the mutex is held for 200ms, something is wrong (the other task crashed while holding it, causing a "deadlock"). The 200ms timeout allows the waiting task to recover instead of hanging forever.

**20ms (`uiStringsMutex`):** String updates to the screen type/emotion should be near-instant. If the mutex isn't available within 20ms, the update is simply skipped — the screen will catch up on the next frame.

```cpp
// From setScreenType() — skip if mutex busy:
void setScreenType(const char *newType) {
    if (uiStringsMutex &&
        xSemaphoreTake(uiStringsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        strncpy(currentScreenType, newType, sizeof(currentScreenType) - 1);
        xSemaphoreGive(uiStringsMutex);
    }
    // If mutex was busy, we just don't update — no harm done
}
```

**10ms (`cpuFreqMutex`):** CPU frequency changes are a single register write. If two cores try simultaneously, the hardware behavior is undefined. The 10ms mutex just serializes the writes — if it times out, the frequency change is skipped, which is merely a performance optimization miss, not a crash.

### What Happens Without Mutexes?

**Example: I2C bus corruption without `i2cMutex`**

```
Core 0: oledTask calls display.display()
  → Sends I2C START condition to OLED (address 0x3C)
  → Starts writing display buffer bytes...

          Core 1: loop() calls mpu.getAcceleration()
            → Sends I2C START condition to MPU6050 (address 0x68)
            → I2C bus now has TWO masters talking simultaneously!
            → SDA/SCL lines are corrupted (electrical conflict)
            → Both devices receive garbage data
            → MPU6050 might lock up (SDA held low)
            → OLED shows corrupted pixels
            → I2C bus is now STUCK until power cycle
```

**With `i2cMutex`:** Core 1 waits until Core 0 finishes its OLED write, then safely reads the MPU6050. No bus collision.

### Deadlocks — The Deadly Embrace

**Simple explanation:** Imagine two people at a narrow doorway. Person A says "you go first" and Person B also says "you go first." Neither moves. They're stuck forever.

**Technical explanation:** A deadlock occurs when Task A holds Mutex X and waits for Mutex Y, while Task B holds Mutex Y and waits for Mutex X. Neither can proceed.

```
DEADLOCK SCENARIO (this does NOT happen in your code, but illustrates the concept):

Task A:                          Task B:
  take(mutexX) ✅                  take(mutexY) ✅
  take(mutexY) ⏳ WAITING          take(mutexX) ⏳ WAITING
  // Needs Y, held by B            // Needs X, held by A
  // DEADLOCK — both wait forever
```

**How your firmware avoids deadlocks:**
1. **Single-mutex operations:** Most code paths take only ONE mutex at a time
2. **Timeouts:** `i2cMutex` uses 200ms timeout, not `portMAX_DELAY`, so even if a deadlock occurred, one side would time out and release
3. **Consistent ordering:** Where multiple mutexes are needed, they're always taken in the same order

**One remaining risk:** `savePetState()` takes `petStateMutex` (with portMAX_DELAY) and then calls NVS functions which internally may acquire a flash mutex. If any other code path took the flash mutex first and then tried to take `petStateMutex`, you'd have a deadlock. This is noted in the reliability audit as a remaining HIGH risk.

### Volatile Variables

**Simple explanation:** When a variable is `volatile`, you're telling the compiler: "Don't be clever with this variable. Always read it from memory, not from a register cache."

**Technical explanation:** Compilers optimize code by caching frequently-read variables in CPU registers. If two cores share a variable, Core 0 might cache it in a register and never see Core 1's update. `volatile` prevents this optimization.

```cpp
// Variables shared between cores — MUST be volatile:
volatile bool cameraCapturing = false;
volatile bool sttModeActive = false;
volatile int pendingRewardScore = 0;
volatile float pendingRewardKC = 0;
```

**When to use `volatile`:**
- Boolean flags set on one core, read on another
- Interrupt-modified variables
- Hardware-mapped registers

**When `volatile` is NOT enough:**
- Structs with multiple fields (one core might read field A before field B is updated)
- 64-bit values on a 32-bit CPU (read/write isn't atomic)
- Any non-trivial data → use a mutex instead

**Example of where `volatile` alone fails:**

```cpp
// BAD — volatile alone, not atomic for struct:
volatile struct { int a; int b; } shared;
// Core 0 writes: shared.a = 10; shared.b = 20;
// Core 1 reads between those two writes:
//   shared.a = 10, shared.b = OLD_VALUE  ← inconsistent!

// GOOD — mutex protects the entire struct:
if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    local_copy = shared;  // Atomic copy of entire struct
    xSemaphoreGive(mutex);
}
```

### Cross-Core Communication Patterns in Your Firmware

**Pattern 1: Flag + Data + Mutex (Camera Pipeline)**

```
Core 0 (cameraMonitorTask)         Core 1 (loop → networkTask)
─────────────────────────          ─────────────────────────
1. Wait for capturingForFeeding    
   (volatile bool, set by Core 1)  
                                   3. loop() sets 
2. Capture image from camera          capturingForFeeding = true
   
4. Take cameraMutex                
5. Copy image to PSRAM buffer      
6. Set cameraImageReady = true     
7. Give cameraMutex                
                                   8. loop() sees cameraImageReady
                                   9. Queue NET_IMAGE to networkQueue
                                   10. networkTask dequeues
                                   11. Take cameraMutex
                                   12. Read image buffer
                                   13. POST to server via HTTPS
                                   14. Give cameraMutex
                                   15. Free image buffer
```

**Pattern 2: Queue (Network Requests)**

```
Core 1 loop()                      Core 1 networkTask
─────────────                      ──────────────────
xQueueSend(NET_SENSOR, 0)  ───►   xQueueReceive() → sendSensorData()
xQueueSend(NET_OLED, 0)    ───►   xQueueReceive() → getOLEDDisplay()
```

**Pattern 3: Double-Buffer Handoff (STT Audio)**

```
Core 0 (audioMonitorTask)          Core 1 (networkTask)
─────────────────────────          ─────────────────────
1. Fill stt_audio_buffer           
   from microphone                 
2. Build WAV header in             
   new ps_malloc'd buffer          
3. Take sttDataMutex              
4. Swap: stt_wav_to_send =        
   new buffer                      
5. Give sttDataMutex              
6. Queue NET_STT                   
                                   7. Take sttDataMutex
                                   8. Grab stt_wav_to_send pointer
                                   9. Set stt_wav_to_send = NULL
                                   10. Give sttDataMutex
                                   11. POST WAV data to server
                                   12. free(wav_data)
```

This "swap and take ownership" pattern means only one core owns the buffer at a time. The producer allocates, the consumer frees. No data races.

---

# Chapter 5: Networking & HTTPS

## Why HTTPS on a Microcontroller?

**Simple explanation:** HTTP is like sending a postcard — anyone who handles it can read the message. HTTPS is like sending a sealed letter — only the intended recipient can open it. Your pet's sensor data, camera images, and voice recordings travel over WiFi and the internet. Without HTTPS, anyone on the same WiFi network could capture and view everything.

**Technical explanation:** HTTPS = HTTP + TLS (Transport Layer Security). TLS provides:
1. **Encryption:** Data is encrypted with AES-128/256, unreadable without the key
2. **Authentication:** The server proves its identity with a certificate
3. **Integrity:** Any tampering is detected via HMAC

On an ESP32-S3, TLS uses the mbedTLS library with hardware-accelerated AES. Despite this, a TLS handshake still takes ~800ms and uses ~32KB of RAM per connection.

## SSL Client Architecture

```cpp
// Two global SSL clients — created once, reused forever:
WiFiClientSecure sslOTA;   // Dedicated to OTA firmware downloads
WiFiClientSecure sslNet;   // Shared for ALL other network calls

// Initialized in setup():
sslOTA.setInsecure();      // Skip certificate validation (see note below)
sslOTA.setTimeout(120);    // 120 seconds (OTA downloads are slow)

sslNet.setInsecure();      // Skip certificate validation
sslNet.setTimeout(10);     // 10 seconds for normal requests
```

### Why `setInsecure()`?

This disables server certificate verification. **In production, this is a security risk** — it means your ESP32 can't verify it's talking to the real server (a man-in-the-middle could intercept traffic).

**Why it's used here:** Certificate verification requires storing the server's root CA certificate (or the entire CA bundle — ~150KB), which consumes precious flash and RAM. For a personal project on your own WiFi, the risk is acceptable.

**Production alternative:** Use `sslNet.setCACert(root_ca)` with the server's certificate. Google Cloud Run uses Google Trust Services certificates — you'd store that specific root CA.

### Why Two SSL Clients?

```
sslOTA: Dedicated to firmware downloads
  - 120 second timeout (firmware files are large)
  - Used ONCE during OTA, then idle
  - Cannot share with sslNet because OTA streams data 
    continuously while flashing

sslNet: Shared for everything else
  - 10 second timeout (normal API calls)
  - Used every 2-5 seconds for sensor/OLED/events
  - Reused via http.setReuse(true) (keep-alive)
```

**What if you used only one?** During OTA, the firmware downloads a large binary through `sslOTA` while simultaneously wanting to report OTA progress via `sslNet`. With one client, the progress report would interrupt the download stream, corrupting the firmware binary. With two clients, both can be active simultaneously (on the same core, just interleaved).

### The HTTP Request Lifecycle

```cpp
bool sendSensorDataOnly(SensorData data) {
    // 1. Check WiFi connectivity
    if (WiFi.status() != WL_CONNECTED) return false;

    // 2. Create HTTP client (lightweight wrapper, not a new connection)
    HTTPClient http;
    http.setReuse(true);            // Keep TCP connection alive
    http.setConnectTimeout(2000);   // 2s to establish connection
    http.setTimeout(5000);          // 5s for response

    // 3. Begin request (associates with SSL client + URL)
    if (!http.begin(sslNet, String(serverUrl))) return false;

    // 4. Set headers
    http.addHeader("Content-Type", "application/json");

    // 5. Build JSON payload using global StaticJsonDocument
    g_sensorDoc.clear();
    g_sensorDoc["accel_x"] = data.accel_x;
    // ... more fields ...

    String payload;
    serializeJson(g_sensorDoc, payload);

    // 6. Send POST request (this blocks for 500ms–3s)
    int httpCode = http.POST(payload);

    // 7. Track result for SSL auto-reset
    trackHttpResult(httpCode);

    // 8. Handle response
    if (httpCode == 200) {
        // Success
    }

    // 9. Clean up
    http.end();
    return httpCode == 200;
}
```

### Connection Reuse (`setReuse(true)`)

**Without reuse:** Every HTTP request requires:
1. TCP handshake (SYN → SYN-ACK → ACK): ~100ms
2. TLS handshake (key exchange, certificate): ~800ms
3. HTTP request/response: ~200ms
4. TCP teardown: ~50ms
**Total: ~1,150ms per request**

**With reuse:** After the first request, the TCP+TLS connection stays open:
1. HTTP request/response: ~200ms
**Total: ~200ms per request (5.7× faster)**

```cpp
http.setReuse(true);  // Tells the server: "Don't close the connection"
```

Your firmware sends sensor data every 2s and OLED polls every 5s. Without reuse, that's constant TLS handshakes.

### SSL Error Recovery

Connections go stale after WiFi drops, server restarts, or TLS timeouts. Your firmware tracks consecutive errors and auto-resets:

```cpp
int httpConsecutiveErrors = 0;
const int HTTP_ERROR_RESET_THRESHOLD = 3;

void trackHttpResult(int httpCode) {
    if (httpCode > 0) {
        httpConsecutiveErrors = 0;  // Success resets counter
    } else {
        httpConsecutiveErrors++;
        if (httpConsecutiveErrors >= HTTP_ERROR_RESET_THRESHOLD) {
            resetSSLConnection();  // Tear down and rebuild SSL
        }
    }
}

void resetSSLConnection() {
    sslNet.stop();        // Close TCP socket
    sslNet.setInsecure(); // Re-apply SSL config (stop() clears it!)
    sslNet.setTimeout(10);
    httpConsecutiveErrors = 0;
}
```

**Why `stop()` clears the config:** `WiFiClientSecure::stop()` resets all internal state, including the `setInsecure()` flag. If you don't re-apply it, the next connection attempt will try to verify the server certificate (which you haven't configured), and it will fail with a TLS error. This is a common ESP32 gotcha.

### CPU Frequency Scaling for Network

HTTPS encryption is CPU-intensive. Your firmware boosts the clock before network calls:

```cpp
// In networkTask switch cases:
case NET_SENSOR:
    safeCpuFreq(160);          // 160MHz for HTTP
    sendSensorDataOnly(data);
    safeCpuFreq(80);           // Back to idle
    break;

case NET_IMAGE:
    safeCpuFreq(160);          // 160MHz for image upload
    sendImageData("");
    safeCpuFreq(80);
    break;
```

At 80MHz, an AES encryption round takes ~2× longer than at 160MHz. Since TLS does many AES rounds per packet, the total request time is noticeably slower. But running at 160MHz constantly wastes power and generates heat — so boosting only during network calls is the right tradeoff.

### Response Size Guards

Every `http.getString()` call in your firmware is protected by a size check:

```cpp
int responseSize = http.getSize();
if (responseSize > 4096) {
    Serial.printf("[SAFETY] Response too large: %d bytes, skipping\n", responseSize);
    http.end();
    return;
}
String response = http.getString();
```

**Why?** `http.getString()` allocates a `String` on the heap equal to the response size. If the server returns an unexpected 1MB response (bug, attack, or corrupted data), the ESP32 would try to allocate 1MB of SRAM (which it doesn't have), potentially crashing. The size guard prevents this.

**Note:** `http.getSize()` returns -1 when the server doesn't send a `Content-Length` header (chunked transfer encoding). Your guards handle this with `>= 0` checks where needed.

### WiFi Reconnection Strategy

```
WiFi drops
    │
    ▼
Try stored credentials (most recent first)
    │
    ├── Connected? ────► Reset fail counter, continue
    │
    ▼
Try hardcoded WIFI_SSID/WIFI_PASSWORD
    │
    ├── Connected? ────► Reset fail counter, continue
    │
    ▼
Increment wifiReconnectFails
    │
    ├── < 10 failures? ─► Try again next loop() iteration
    │
    ▼
≥ 10 consecutive failures
    │
    ▼
Enter Wi-Fi AP Provisioning Mode
  - Creates "KAKU_SETUP" access point
  - Serves config web page
  - User connects phone and enters new WiFi credentials
  - Stores new credentials in NVS
  - Reboots to connect with new credentials
```

This is a robust fallback: if you move to a new WiFi network, after 10 failed reconnect attempts (~100 seconds), the device becomes a WiFi hotspot where you can configure the new network from your phone.

---

# Chapter 6: I2C & Sensor Handling

## What Is I2C?

**Simple explanation:** I2C (pronounced "I-squared-C") is a two-wire communication bus. Think of it as a shared phone line where multiple devices can talk, but they take turns. One device is the "master" (the ESP32) and the others are "slaves" (MPU6050 at address 0x68, OLED at address 0x3C). The master initiates every conversation.

**Technical explanation:** I2C uses two signals:
- **SDA** (Serial Data) — GPIO 5: bidirectional data line
- **SCL** (Serial Clock) — GPIO 6: clock generated by the master

```
ESP32-S3                MPU6050           SSD1306 OLED
  │                      │                   │
  ├──── SDA (GPIO 5) ────┼───────────────────┤
  │                      │                   │
  ├──── SCL (GPIO 6) ────┼───────────────────┤
  │                      │                   │
  ├──── 3.3V ────────────┼───────────────────┤
  │                      │                   │
  └──── GND ─────────────┴───────────────────┘
```

Both devices share the same two wires. They're distinguished by their 7-bit addresses:
- MPU6050: `0x68` (or `0x69` if AD0 pin is high)
- SSD1306: `0x3C` (or `0x3D` depending on configuration)

### How an I2C Transaction Works

```
┌─────────┬────────────┬─────┬──────────┬──────────┬─────┐
│  START  │  Address   │ R/W │   Data   │   Data   │ STOP│
│ (SDA↓)  │  1101000   │  0  │ 00111011 │ 11010010 │     │
│         │  (0x68)    │ (W) │ (reg 3B) │ (value)  │     │
└─────────┴────────────┴─────┴──────────┴──────────┴─────┘
```

1. **START:** Master pulls SDA low while SCL is high (a special signal meaning "I'm starting a transaction")
2. **Address + R/W:** 7-bit slave address + 1 bit indicating read or write
3. **Data bytes:** Register address, then data to read/write
4. **STOP:** Master releases SDA while SCL is high (transaction complete)

### Why I2C Needs a Mutex

Your firmware has TWO cores accessing I2C:
- **Core 0:** OLED task writes display buffers via I2C
- **Core 1:** Loop reads MPU6050 accelerometer/gyro via I2C

```
WITHOUT MUTEX (dangerous):
──────────────────────────
Core 0: START → 0x3C(W) → display_data[0] → display_data[1] →
Core 1:                                   START → 0x68(W) → reg_addr
        ↑ BUS COLLISION: Two masters driving SDA/SCL simultaneously
        Result: Corrupted data, I2C bus may lock up
```

```
WITH MUTEX (safe):
──────────────────
Core 0: [take i2cMutex] → START → 0x3C → data... → STOP → [give i2cMutex]
Core 1:                    [waiting for mutex...]
Core 1: [take i2cMutex] → START → 0x68 → data... → STOP → [give i2cMutex]
```

### MPU6050 Sensor Reading

The MPU6050 is a 6-axis Inertial Measurement Unit (IMU):
- **Accelerometer:** Measures acceleration in X, Y, Z axes (gravity + motion)
- **Gyroscope:** Measures rotation speed in X, Y, Z axes

```cpp
// From readAllSensors() — reading with I2C mutex protection:
if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    Serial.println("[I2C] Mutex timeout in readAllSensors");
    return data;  // Return zeroed struct — safe default
}
mpu.getAcceleration(&ax, &ay, &az);  // 6 bytes read via I2C
mpu.getRotation(&gx, &gy, &gz);      // 6 bytes read via I2C
if (i2cMutex) xSemaphoreGive(i2cMutex);

// Convert raw 16-bit values to physical units:
data.accel_x = ax / 16384.0 * 9.81;  // LSB → g → m/s²
//              ↑       ↑       ↑
//       raw value  sensitivity  gravity constant
//                  (±2g range)
data.gyro_x = gx / 131.0;            // LSB → degrees/second
//                  ↑
//            sensitivity (±250°/s range)
```

**Why 16384?** The MPU6050 returns 16-bit signed integers. At ±2g sensitivity, the full range (-32768 to +32767) maps to -2g to +2g. So 1g = 16384 LSB.

**Why 131?** At ±250°/s sensitivity, the full range maps to -250 to +250 degrees/second. So 1°/s ≈ 131 LSB.

### Sensor Batch Collection

Your firmware collects sensor readings every 100ms in `loop()`:

```cpp
// From loop() — high-frequency sensor collection:
if (millis() - lastInternalReadTime >= INTERNAL_READ_INTERVAL) { // 100ms
    lastInternalReadTime = millis();
    
    if (sensorBatch.reading_count < SensorDataBatch::MAX_READINGS) {
        SingleReading reading;
        reading.timestamp_ms = millis();
        
        if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
            // Mutex timeout — fill with safe zeroes
            reading.accel_x = reading.accel_y = reading.accel_z = 0.0;
            reading.gyro_x = reading.gyro_y = reading.gyro_z = 0.0;
        } else {
            mpu.getAcceleration(&ax, &ay, &az);
            mpu.getRotation(&gx, &gy, &gz);
            if (i2cMutex) xSemaphoreGive(i2cMutex);
            
            reading.accel_x = ax / 16384.0 * 9.81;
            // ... convert all values ...
        }
        
        sensorBatch.readings[sensorBatch.reading_count++] = reading;
    }
}
```

**Why batch?** Sending individual readings to the server every 100ms would require 10 HTTP requests per second — impossible given each takes 200ms+. Instead, readings are batched locally and sent as an array every 2 seconds. The server receives ~20 readings per batch and can compute step detection, orientation, and motion patterns more accurately than from a single snapshot.

### Step Detection Algorithm

```cpp
// Hardware step counter — runs in loop() every 100ms:
float ax_f = reading.accel_x;
float ay_f = reading.accel_y;
float az_f = reading.accel_z;

// Low-pass filter to estimate gravity direction:
grav_x = LP_ALPHA_STEP * grav_x + (1 - LP_ALPHA_STEP) * ax_f;
grav_y = LP_ALPHA_STEP * grav_y + (1 - LP_ALPHA_STEP) * ay_f;
grav_z = LP_ALPHA_STEP * grav_z + (1 - LP_ALPHA_STEP) * az_f;

// Dynamic acceleration (total - gravity) = user motion:
float dyn_x = ax_f - grav_x;
float dyn_y = ay_f - grav_y;
float dyn_z = az_f - grav_z;

// Magnitude squared of dynamic component:
float mag2 = dyn_x*dyn_x + dyn_y*dyn_y + dyn_z*dyn_z;

// Step detection: magnitude exceeds threshold + debounce timer:
if (mag2 > STEP_BARRIER_G2 && (millis() - lastHwStepTime > STEP_MIN_MS)) {
    hwStepCount++;
    lastHwStepTime = millis();
    petIsWalking = true;
    lastWalkingStepTime = millis();
}
```

**How it works:**
1. **Low-pass filter** separates gravity (constant ~9.81 m/s²) from motion (changing rapidly). `LP_ALPHA_STEP = 0.85` means 85% of the previous estimate + 15% of the new reading.
2. **Dynamic acceleration** is what's left after subtracting gravity — this is pure user motion.
3. **Magnitude threshold** (`STEP_BARRIER_G2 = 0.25`) detects when motion energy exceeds a step's impact.
4. **Debounce** (`STEP_MIN_MS = 600ms`) prevents double-counting a single footstep.

**What if the threshold is too low?** Touching or slightly moving the device registers as a step. The threshold of 0.25g² was increased specifically to filter out finger taps.

**What if the threshold is too high?** Light footsteps while the device is in a pocket might not register. It's always a tradeoff between sensitivity and false positives.

### Tilt Gesture Detection

Your firmware uses accelerometer data to detect device orientation:

```
NORMAL HOLD       LEFT TILT           RIGHT TILT
(Y dominant)      (X negative)        (X positive)
     ___                ___                ___
    |   |            __|   |          |   |__
    | Y↑|           |  X←  |          |  X→  |
    |   |            ‾‾|   |          |   |‾‾
    |___|               ‾‾‾                ‾‾‾
  
  accel_y ≈ 9.81   accel_x < -5.0    accel_x > 5.0
```

```cpp
// Right tilt detection (menu cycling):
if (accel_x > 5.0 && !holdingRightForMenu) {
    holdingRightForMenu = true;
    menuTiltHoldStartTime = millis();
}
if (holdingRightForMenu && (millis() - menuTiltHoldStartTime >= 2000)) {
    // Held right for 2 seconds → cycle menu
    cycleMenu();
    holdingRightForMenu = false;
}
```

The 2-second hold prevents accidental menu changes from brief tilts.

---

# Chapter 7: Camera Pipeline

## Architecture Overview

The camera pipeline spans two cores and involves PSRAM buffering, mutex protection, and asynchronous HTTPS upload:

```
USER GESTURE          CORE 0                    CORE 1
────────────          ──────                    ──────
Left tilt 3s ──────► capturingForFeeding=true
on FOOD_MENU          │
                      ▼
              cameraMonitorTask
              sees flag, boosts to 240MHz
                      │
                      ▼
              esp_camera_fb_get()
              (hardware JPEG encode)
                      │
                      ▼
              Take cameraMutex
              ps_malloc(fb->len)
              memcpy to PSRAM buffer
              cameraImageReady=true
              Give cameraMutex
              Drop to 80MHz
                                              loop() sees cameraImageReady
                                              xQueueSend(NET_IMAGE)
                                                      │
                                                      ▼
                                              networkTask dequeues
                                              safeCpuFreq(160)
                                              sendImageData()
                                              POST image to server
                                              safeCpuFreq(80)
                                                      │
                                                      ▼
                                              Server: AI food recognition
                                                      │
                                                      ▼
                                              Next OLED poll:
                                              Server sends eating emotion
                                              Pet plays eating animation
```

## Camera Configuration

```cpp
config.xclk_freq_hz = 10000000;        // 10 MHz clock (reduced from 20MHz)
config.pixel_format = PIXFORMAT_JPEG;   // Hardware JPEG compression
config.frame_size = FRAMESIZE_QQVGA;    // 160×120 pixels
config.jpeg_quality = 20;               // Quality 1-63 (lower = better quality)
config.fb_count = 1;                    // Single frame buffer
config.fb_location = CAMERA_FB_IN_PSRAM; // Store frame in PSRAM
config.grab_mode = CAMERA_GRAB_LATEST;   // Get most recent frame
```

**Why 160×120?** This is the smallest standard resolution. A 160×120 JPEG at quality 20 is typically 1-3KB — small enough to upload quickly over HTTP. Higher resolutions would use more PSRAM and take longer to transmit. Since the server's AI model only needs to identify food types (not read fine text), 160×120 is sufficient.

**Why 10MHz clock?** The camera sensor runs off an external clock provided by the ESP32. At 20MHz, the camera and ESP32 generate more heat. At 10MHz, frame capture is slower (~100ms vs ~50ms) but thermal output is halved. For a sealed device running 24/7, thermal management matters more than frame rate.

**Why single frame buffer?** With `fb_count = 2`, the camera uses double-buffering (one frame being captured while you process the previous one). But with on-demand capture (not continuous), double-buffering wastes 3-6KB of PSRAM for no benefit.

## The Double-Buffer Handoff

```cpp
// Core 0: cameraMonitorTask — producer
void cameraMonitorTask(void *parameter) {
    while (true) {
        if (capturingForFeeding && !cameraImageReady) {
            safeCpuFreq(240);  // Max CPU for JPEG encoding
            
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                // Copy frame to our own PSRAM buffer under mutex
                if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (capturedImageBuffer != NULL)
                        free(capturedImageBuffer);  // Free previous frame
                    
                    capturedImageBuffer = (uint8_t *)ps_malloc(fb->len);
                    if (capturedImageBuffer) {
                        memcpy(capturedImageBuffer, fb->buf, fb->len);
                        capturedImageLength = fb->len;
                        cameraImageReady = true;
                    }
                    xSemaphoreGive(cameraMutex);
                }
                esp_camera_fb_return(fb);  // Return frame buffer to driver
            }
            safeCpuFreq(80);  // Back to idle
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms polling interval
    }
}
```

**Why copy to a separate buffer?** The camera driver owns `fb->buf`. Calling `esp_camera_fb_return(fb)` makes that memory available for the next capture. If `networkTask` was still uploading the old frame when a new capture happened, the data would be overwritten mid-upload. The `memcpy` to `capturedImageBuffer` creates an independent copy that the network task can consume at its own pace.

**Why `ps_malloc` instead of `malloc`?** A 3KB JPEG frame is best stored in PSRAM. Using SRAM `malloc` for this would consume precious internal RAM that's needed for WiFi buffers and task stacks.

---

# Chapter 8: Audio & STT Pipeline

## Two Modes of Operation

The audio system has distinct operating modes:

```
MODE 1: Voice Activity Detection (VAD)
─────────────────────────────────────
Default mode. Continuously monitors mic energy.
When speech detected (energy > 1000 for 500ms+):
  → Records up to 5 seconds of audio
  → Base64 encodes the WAV
  → Stores in detectedAudioData under audioMutex
  → VAD-detected audio is currently unused (dead path)

MODE 2: Speech-to-Text (STT) Streaming
─────────────────────────────────────
Activated by 10-second left tilt hold.
Continuously records 3-second WAV chunks:
  → Builds WAV buffer in PSRAM
  → Hands off buffer under sttDataMutex
  → Queues NET_STT to networkQueue
  → networkTask POSTs raw WAV to /api/audio-stt
  → Server returns transcribed text
  → Times out after 120s of inactivity
```

## PDM Microphone

**Simple explanation:** PDM (Pulse Density Modulation) is a digital microphone format. Instead of sending an analog voltage proportional to sound pressure, it sends a rapid stream of 1s and 0s. More 1s = louder positive pressure, more 0s = quieter or negative pressure. The ESP32's I2S hardware converts this bitstream into 16-bit audio samples.

```cpp
// PDM microphone configuration:
i2s_pdm_rx_config_t pdm_rx_cfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),  // 16kHz
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,  // 16-bit samples
        I2S_SLOT_MODE_MONO),       // Single channel
    .gpio_cfg = {
        .clk = PDM_CLK_GPIO,  // GPIO 42 (clock output to mic)
        .din = PDM_DIN_GPIO,  // GPIO 41 (data input from mic)
    },
};
```

**16kHz sample rate:** Telephone-quality audio. Speech frequencies range from 300Hz to 3,400Hz. By Nyquist's theorem, you need at least 2× the highest frequency → 6,800Hz minimum. 16kHz provides headroom and is the standard for speech recognition APIs.

## Voice Activity Detection (VAD)

```cpp
// Energy-based VAD — simple but effective:
int32_t energy = 0;
int samples = bytes_read / sizeof(int16_t);
for (int i = 0; i < samples; i++) {
    energy += abs(vad_buffer[i]);  // Sum absolute values
}
energy = energy / samples;  // Average energy per sample

if (energy > VAD_THRESHOLD) {  // VAD_THRESHOLD = 1000
    // Sound detected
    lastSoundTime = currentTime;
    if (speechStartTime == 0) speechStartTime = currentTime;
    
    // Only start recording after 500ms of continuous speech:
    if (currentTime - speechStartTime >= VAD_MIN_DURATION) {
        currentlyRecording = true;
        recording_buffer = (uint8_t *)ps_malloc(MAX_RECORDING_SIZE + WAV_HEADER_SIZE);
        // ... begin capturing audio ...
    }
}
```

**How energy-based VAD works:**
1. Read a chunk of audio samples (512 samples = 32ms of audio at 16kHz)
2. Sum the absolute values of all samples
3. Divide by sample count to get average energy
4. If energy exceeds threshold → speech is happening
5. Require 500ms of continuous speech before starting to record (filters out door slams, coughs)

```
Audio Energy Signal:

Energy
  ↑
4000│         ╭──╮    ╭────────╮
    │         │  │    │ Human  │    ╭─╮
2000│    ╭╮   │  │    │ Speech │    │ │ Cough
    │    ││   │  │    │        │    │ │
1000│----++---+--+----+--------+----+-+-------  ← VAD_THRESHOLD
    │  ╱╲ ╱╲                                      
  0 │╱    ╲   ╲   Background Noise
    └─────────────────────────────────────────► Time
          ↑                ↑
      Door slam         Recording starts
     (too brief,        (>500ms speech)
      <500ms)
```

### Mic Sleep — Power Optimization

```cpp
// Silent for 2+ seconds and not recording → slow-poll to save power:
if (!currentlyRecording && (currentTime - lastSoundTime) > 2000) {
    audioEnergyLevel = 0;
    vTaskDelay(pdMS_TO_TICKS(500));  // Sleep 500ms between reads
    continue;
}
```

**Why?** I2S PDM reading is continuous by default — the ESP32 reads samples as fast as possible. During silence, these reads provide no useful data but keep the I2S peripheral (and DMA) active, consuming power and generating heat. Sleeping 500ms between reads during silence dramatically reduces audio task CPU usage while still detecting the onset of speech within 500ms.

## STT Streaming Pipeline

The STT pipeline is more complex, involving a double-buffer handoff across cores:

```
audioMonitorTask (Core 0)                    networkTask (Core 1)
┌─────────────────────────┐                  ┌────────────────────────┐
│ 1. Read 10ms of audio   │                  │                        │
│                         │                  │                        │
│ 2. Append to            │                  │                        │
│    stt_audio_buffer     │                  │                        │
│    (128KB PSRAM)        │                  │                        │
│                         │                  │                        │
│ 3. Every 3 seconds:     │                  │                        │
│    ┌─────────────────┐  │                  │                        │
│    │ ps_malloc WAV    │  │                  │                        │
│    │ (header + audio) │  │                  │                        │
│    └────────┬────────┘  │                  │                        │
│             ▼            │                  │                        │
│    Take sttDataMutex     │                  │                        │
│    stt_wav_to_send = buf │──── swap ──────►│ Take sttDataMutex     │
│    xQueueSend(NET_STT)   │                  │ Grab stt_wav_to_send  │
│    Give sttDataMutex     │                  │ Set ptr to NULL       │
│             │            │                  │ Give sttDataMutex     │
│    Reset audio counter   │                  │         │              │
│                         │                  │         ▼              │
│ 4. Continue recording   │                  │ sslNet.stop()         │
│    next 3s chunk        │                  │ POST raw WAV to server│
│                         │                  │ free(wav_data)         │
│                         │                  │ Parse JSON response    │
│                         │                  │ Print transcribed text │
└─────────────────────────┘                  └────────────────────────┘
```

**Why `sslNet.stop()` before STT send?** STT audio chunks are ~96KB (3 seconds × 16,000 samples × 2 bytes + 44 byte header). After sending such a large POST, the TLS session state can become corrupted — mbedTLS may have freed internal PSRAM buffers, and reusing the session would write to freed memory. Stopping and rebuilding the SSL connection ensures a clean state.

**Why raw WAV instead of multipart?** The original implementation wrapped the WAV data in a multipart form body, which required a second buffer (~160KB) for the multipart wrapper. This doubled peak memory usage to ~500KB. Sending raw WAV bytes with `Content-Type: audio/wav` eliminates the copy entirely.

---

# Chapter 9: OLED Rendering

## Display Specifications

```
┌──────────────────────────────────┐
│      SSD1306 OLED 64×32         │
│                                  │
│  Resolution: 64 × 32 pixels     │
│  Colors: Monochrome (white/off)  │
│  Interface: I2C (address 0x3C)   │
│  Refresh: ~50ms/frame            │
│  Memory: 256 bytes framebuffer   │
│  Power: ~5mA typical             │
└──────────────────────────────────┘
```

**64×32 is tiny.** A single character in the smallest font is 6×8 pixels, giving you ~10 characters per line and 4 lines max. Every pixel matters. Your animations are carefully crafted bitmaps stored in `all_pets.h`.

## The OLED Task

The OLED runs as its own FreeRTOS task on Core 0:

```cpp
void oledTask(void *parameter) {
    while (true) {
        displayPetAnimation();      // Main rendering function
        vTaskDelay(pdMS_TO_TICKS(50));  // ~20 FPS
    }
}
```

**Why its own task?** If OLED rendering ran inside `loop()`, every network stall would freeze the animation. The pet would appear unresponsive, even though the device is just waiting for an HTTP response. With its own task on Core 0, animations run smoothly regardless of Core 1's network activity.

### Animations — Bitmap Arrays

Pet animations are stored as arrays of bitmaps in `all_pets.h`:

```cpp
// Example: A walking animation frame (stored as PROGMEM bitmap)
const unsigned char infantWalk1 [] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x1e, 0x00,
    // ... each byte represents 8 horizontal pixels ...
};
```

Each animation has multiple frames. The OLED task cycles through them:

```
Frame 1          Frame 2          Frame 3
┌────────┐      ┌────────┐      ┌────────┐
│  ●_●   │      │  ●_●   │      │  ●_●   │
│  ╭─╮   │      │  ╭─╮   │      │  ╭─╮   │
│  │ │   │  →   │  ╱ │   │  →   │  │ ╲   │
│  ╱ ╲   │      │ ╱  ╲   │      │  ╱  ╲  │
└────────┘      └────────┘      └────────┘
  Standing        Walking L       Walking R
```

### Emotion-Based Animation Selection

```cpp
void displayPetAnimation() {
    // Select animation based on pet age and current emotion
    if (emotionIs("SLEEPING")) {
        // Show sleeping animation (Z's floating up)
    } else if (emotionIs("EATING")) {
        // Show eating animation (mouth moving)
    } else if (emotionIs("HAPPY")) {
        // Show happy animation (bouncing)
    } else if (petIsWalking) {
        // Show walking animation (legs moving)
    } else {
        // Show idle animation (breathing/blinking)
    }
    
    // Overlay icons on top of animation:
    if (showFoodIcon) drawFoodIcon();     // Bottom-right
    if (showPoopIcon) drawPoopIcon();     // Bottom-right
    if (showSickIcon) drawSickIcon();     // Bottom-right (blinking)
    if (showHomeIcon) drawHomeIcon();     // Top-left
}
```

### Screen Type / Menu System

```
MAIN ──►  FOOD_MENU ──► TOILET_MENU ──► PLAY_MENU ──► HEALTH_MENU ──► STATUS ──► STATS ──► MAIN
                                                                                              ↑
Left tilt 3s on each menu:                                                                    │
  FOOD_MENU    → Capture food image                                                           │
  TOILET_MENU  → Send clean request                                              Right tilt ──┘
  HEALTH_MENU  → Send medicine                                                   cycles menu
  PLAY_MENU    → Start minigame
```

Menu state is stored in `currentScreenType` (char array), protected by `uiStringsMutex`.

### I2C Sharing with MPU6050

Both OLED and MPU6050 are on the same I2C bus. The OLED task writes display data via I2C (up to 256 bytes per frame), and `loop()` reads sensor data via I2C (12 bytes per read). The `i2cMutex` ensures they don't collide:

```
OLED frame write:  ~256 bytes at 400kHz ≈ 5ms
MPU6050 read:      ~12 bytes at 400kHz ≈ 0.3ms
```

Since OLED writes are 16× longer than MPU6050 reads, the sensor rarely has to wait long for the mutex. But if the OLED calls deep rendering chains that hold the mutex for extended periods, sensors might time out — that's why the 200ms timeout was chosen (much larger than the worst-case OLED write).

---

# Chapter 10: OTA Update System

## What Is OTA?

**Simple explanation:** OTA (Over-The-Air) means updating the device's software wirelessly, without opening the case or connecting a USB cable. You upload a new firmware version to your cloud server, press a button on the dashboard, and the device downloads and installs it automatically.

**Technical explanation:** The ESP32 has dual flash partitions (A and B). The current firmware runs from partition A. An OTA update downloads new firmware to partition B, then reboots into partition B. If the new firmware fails to start, the bootloader automatically rolls back to partition A. This is called **A/B partitioning**.

```
FLASH MEMORY LAYOUT:
┌──────────────────────┐  0x000000
│ Bootloader           │  16KB
├──────────────────────┤  0x008000
│ Partition Table      │  4KB
├──────────────────────┤  0x009000
│ NVS (Non-Volatile)   │  20KB
├──────────────────────┤  0x010000
│ OTA Partition A      │  ~1.5MB  ← Running firmware
│ (app0)               │
├──────────────────────┤  0x190000
│ OTA Partition B      │  ~1.5MB  ← OTA writes here
│ (app1)               │
├──────────────────────┤  0x310000
│ SPIFFS / Data        │  remaining
└──────────────────────┘
```

## OTA Flow

```
Dashboard "Enable OTA" button pressed
         │
         ▼
Server sets ota_update=true in OLED response
         │
         ▼
getOLEDDisplayFromServer() on ESP32 sees flag
otaUpdateRequested = true
         │
         ▼
networkTask dequeues NET_OLED, enters OTA block
         │
         ▼
┌────────────────────────────────────────────────────┐
│ ATTEMPT LOOP (3 tries, 30s between retries)        │
│                                                    │
│  1. safeCpuFreq(240) — max CPU for download        │
│                                                    │
│  2. GET /api/firmware/latest                        │
│     Response: { version, download_url, file_size }  │
│                                                    │
│  3. Verify newer version available                  │
│                                                    │
│  4. postOTAProgress("downloading", 0, version)      │
│     (Reports to dashboard via sslNet, BEFORE         │ 
│      sslOTA connects — can't hold 2 SSL at once)    │
│                                                    │
│  5. SUSPEND audio/camera/OLED tasks                 │
│     - Frees ~30KB stack memory                      │
│     - Eliminates CPU contention                     │
│     - Stops I2C / PSRAM interference                │
│                                                    │
│  6. Update.begin(contentLength)                     │
│     - Prepares partition B for writing              │
│                                                    │
│  7. Download loop:                                  │
│     ┌──────────────────────────────────────┐        │
│     │ Read 4KB into otaBuf[4096]           │        │
│     │ Update.write(otaBuf, bytesRead)      │        │
│     │ Update OLED progress bar every 5%    │        │
│     │ Exponential backoff on read errors   │        │
│     │ Abort after 120s of no data          │        │
│     │ Abort after 150 consecutive errors   │        │
│     └──────────────────────────────────────┘        │
│                                                    │
│  8. Update.end(true) — finalize flash              │
│                                                    │
│  9. postOTAProgress("rebooting", 100, version)     │
│                                                    │
│  10. ESP.restart() — boot into new firmware         │
│                                                    │
│  ON FAILURE:                                       │
│  - Resume suspended tasks                           │
│  - Clear otaUpdateRequested                         │
│  - safeCpuFreq(80)                                 │
│  - If attempts remaining: wait 30s, retry          │
│  - If all 3 failed: give up, try on next poll      │
└────────────────────────────────────────────────────┘
```

## Key Design Decisions

### Global OTA Buffer

```cpp
uint8_t otaBuf[4096];  // Global, not on stack
```

**Why global?** `networkTask` has a 16KB stack. If `otaBuf` were a local variable inside `checkAndPerformOTA()`, that's 4KB consumed from the 16KB stack — leaving only 12KB for all other local variables, HTTP client state, and function call chains during OTA. Making it global costs 4KB of SRAM permanently but prevents stack overflow during the most memory-intensive operation.

### Task Suspension During OTA

```cpp
otaInProgress = true;
if (audioTaskHandle)  vTaskSuspend(audioTaskHandle);
if (cameraTaskHandle) vTaskSuspend(cameraTaskHandle);
if (oledTaskHandle)   vTaskSuspend(oledTaskHandle);
```

**Why suspend?**  
- **Audio task:** Might allocate a recording buffer (128KB PSRAM) mid-download
- **Camera task:** Might trigger esp_camera_fb_get() which uses DMA and I2C
- **OLED task:** Shares I2C bus; writes would interfere with download speed

**Risk:** If the code takes an early return (error path) without resuming tasks, those features are permanently dead until reboot. Your code resumes tasks in EVERY failure path, but this pattern is fragile.

### Exponential Backoff on Read Errors

```cpp
// Backoff: 100ms → 200ms → 400ms
int backoff = min(400, 100 * (1 + readErrors / 25));
vTaskDelay(pdMS_TO_TICKS(backoff));
```

**Why?** Google Cloud Run can "cold start" mid-stream — the server process pauses while a new instance spins up. This can cause 5-15 seconds of no data. Fixed-interval retries might give up too quickly. Exponential backoff starts gentle (100ms) and increases gradually, tolerating up to 30 seconds of pauses (150 retries × ~200ms average).

### Progress Reporting to Dashboard

```cpp
void postOTAProgress(const char *otaStatus, int progress,
                     const String &targetVersion, const char *message) {
    // Uses sslNet (NOT sslOTA — which is busy downloading)
    HTTPClient http;
    http.begin(sslNet, ".../api/ota/progress");
    // POST { device_id, ota_status, progress, target_version, message }
}
```

This function reports OTA progress to the cloud dashboard. It uses `sslNet` (the general-purpose SSL client) because `sslOTA` is busy streaming the firmware binary. Note: during the actual download loop, progress is NOT reported to the server (it would interrupt the download stream) — only OLED progress bar updates are performed.

---

# Chapter 11: Reliability & Stability Concepts

## Why Reliability Matters on Embedded Systems

**Simple explanation:** Your KAKU device is meant to run 24/7 without anyone pressing a reset button. If it crashes at 3 AM, no one reboots it. If memory slowly leaks, it crashes after 3 days. It must be self-healing.

**Technical explanation:** Unlike a web server where you can SSH in and restart a process, an embedded device in a sealed enclosure has exactly one recovery mechanism: software watchdog + automatic reboot. Every design decision must account for:
- **Memory exhaustion** — heap fragmentation after days of operation
- **Communication failures** — WiFi drops, server timeouts, DNS failures
- **Hardware hangs** — I2C bus lockup, camera sensor freeze
- **State corruption** — race conditions, partial updates

## Defense-in-Depth: Your Firmware's Safety Layers

```
Layer 1: PREVENTION
├── Mutexes prevent race conditions (8 mutexes)
├── Static allocation prevents fragmentation (StaticJsonDocument)
├── Timeouts prevent infinite waits (200ms I2C, 10ms CPU freq)
├── Size guards prevent OOM from large responses
└── CPU freq mutex prevents register corruption

Layer 2: DETECTION
├── Heap monitoring every 10 seconds
├── HTTP error counter (3 consecutive → SSL reset)
├── WiFi reconnect counter (10 fails → AP provisioning)
├── Serial logging for debugging
└── OTA progress reporting to dashboard

Layer 3: RECOVERY
├── SSL auto-reset after consecutive errors
├── WiFi auto-reconnect with fallback to AP mode
├── Daily scheduled reboot (24h uptime, during sleep)
├── Emergency reboot at <20KB free heap
├── OTA retry (3 attempts with 30s backoff)
└── NVS state persistence (survives reboots)
```

## Watchdog Concept

A watchdog timer is a hardware countdown. If the software doesn't "feed" (reset) the watchdog before it reaches zero, the hardware forces a reboot. This catches infinite loops and deadlocks.

The ESP32 has a built-in Task Watchdog Timer (TWDT). FreeRTOS's idle task feeds it automatically. If any task starves the idle task (by running for too long without a `vTaskDelay()`), the watchdog triggers.

Your firmware doesn't configure custom watchdogs, but the default TWDT is always active. The `vTaskDelay()` calls in every task loop ensure the watchdog is fed.

## State Persistence Across Reboots

```cpp
// Before any reboot:
savePetState();   // Write all pet stats to NVS flash
delay(500);       // Allow NVS write to complete
ESP.restart();    // Reboot

// On startup:
loadPetState();   // Read pet stats from NVS
syncLocalStateToUI();  // Push loaded state to OLED variables
```

**Why `delay(500)` before `ESP.restart()`?** NVS writes to flash are buffered. Without a brief delay, the reboot might happen before the write is flushed, losing the last state update. 500ms is conservative — NVS typically flushes in ~50ms.

## The Daily Reboot Pattern

```cpp
static const unsigned long DAILY_REBOOT_MS = 86400000UL;  // 24 hours

if (millis() > DAILY_REBOOT_MS && isDeviceSleeping) {
    Serial.println("Daily maintenance reboot");
    savePetState();
    delay(500);
    ESP.restart();
}
```

**Why reboot daily?**
1. **Memory fragmentation:** Even with careful allocation, tiny gaps accumulate in the heap. After 24 hours, the heap might have 100KB free but no single block larger than 4KB.
2. **Memory leaks:** Any undiscovered leak (even 1 byte/minute) adds up to 1.4KB/day. Not fatal, but compounds.
3. **Driver state:** WiFi and I2C drivers can accumulate error state that's hard to clean up programmatically.

**Why only during sleep?** Rebooting while the user is interacting with the pet would be jarring. `isDeviceSleeping` is true when the device has been stationary for 30+ seconds — likely sitting on a shelf, not being played with.

---

# Chapter 12: Power & Thermal Optimization

## Why Power Matters

**Simple explanation:** The ESP32 in a sealed plastic case generates heat. More heat means higher internal temperature, which means more power consumption (a positive feedback loop), which can lead to WiFi instability, sensor errors, and shortened component lifespan.

**Technical explanation:** The ESP32-S3 consumes:
- **240MHz active:** ~100mW (CPU only)
- **160MHz active:** ~65mW
- **80MHz active:** ~35mW
- **Light sleep:** ~1mW
- **WiFi TX (11dBm):** ~140mW during transmission

In a sealed enclosure with no ventilation, ambient temperature can rise 10-20°C above room temperature. At 240MHz with WiFi active, the chip junction temperature can exceed 60°C.

## CPU Frequency Scaling

Your firmware dynamically adjusts CPU speed:

```cpp
// Idle: 80 MHz (most of the time)
safeCpuFreq(80);

// HTTP requests: 160 MHz (TLS encryption needs speed)
safeCpuFreq(160);
sendSensorDataOnly(data);
safeCpuFreq(80);

// Camera capture: 240 MHz (JPEG encoding is CPU-intensive)
safeCpuFreq(240);
camera_fb_t *fb = esp_camera_fb_get();
safeCpuFreq(80);

// OTA download: 240 MHz (maximum throughput needed)
setCpuFrequencyMhz(240);
// ... download entire firmware ...
```

**Savings calculation:**
- Time at 80MHz: ~80% (idle, waiting, sensor batch)
- Time at 160MHz: ~15% (HTTP requests every 2-5 seconds)
- Time at 240MHz: ~5% (camera captures, rare OTA)
- Average power: 0.80 × 35 + 0.15 × 65 + 0.05 × 100 = **42.75mW** vs 100mW constant at 240MHz
- **57% power reduction** vs running at max speed always

### `safeCpuFreq()` — The Mutex Wrapper

```cpp
void safeCpuFreq(int mhz) {
    if (cpuFreqMutex &&
        xSemaphoreTake(cpuFreqMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        setCpuFrequencyMhz(mhz);
        xSemaphoreGive(cpuFreqMutex);
    }
}
```

**Why a mutex?** `setCpuFrequencyMhz()` modifies a hardware register shared by both cores. If Core 0 tries to set 80MHz while Core 1 simultaneously sets 160MHz, the register might end up in an undefined state. The mutex serializes the calls.

**Why 10ms timeout?** Setting CPU frequency is a near-instant register write. If the mutex is held for >10ms, something is severely wrong. Skipping the frequency change is harmless — it just means running at the current speed a bit longer.

## WiFi Power Optimization

```cpp
// During WiFi initialization:
WiFi.setSleep(true);       // Enable light-sleep between packets
WiFi.setTxPower(WIFI_POWER_11dBm);  // Reduce from default 19.5dBm
```

**`WiFi.setSleep(true)` (Light Sleep):** Between WiFi packets, the radio enters a low-power state. The ESP32 wakes up periodically (DTIM interval, usually 100ms) to check for incoming data. This reduces WiFi power from ~140mW continuous to ~20mW average, at the cost of ~100ms latency on incoming data.

**`WiFi.setTxPower(11dBm)`:** Default TX power is 19.5dBm (~89mW). Reducing to 11dBm (~12.5mW) cuts radio power by 86%. Range is reduced, but if the device is on the same desk as the router, maximum range isn't needed. This is the single biggest heat reduction.

**Tradeoff:** Lower TX power = shorter WiFi range. If the device is far from the router, increase to 15dBm or 17dBm.

## Audio Power Optimization

```cpp
// In audioMonitorTask — mic sleep during silence:
if (!currentlyRecording && (currentTime - lastSoundTime) > 2000) {
    audioEnergyLevel = 0;
    vTaskDelay(pdMS_TO_TICKS(500));  // Sleep 500ms between reads
    continue;
}
```

During silence (which is most of the time), the audio task reads the mic once every 500ms instead of continuously. The I2S peripheral and DMA channel idle between reads. This reduces audio task CPU usage from ~15% to <1% during quiet periods.

## Camera Power Optimization

```cpp
// Camera only runs on gesture trigger (not continuously):
config.xclk_freq_hz = 10000000;     // 10 MHz (half of default 20 MHz)
config.frame_size = FRAMESIZE_QQVGA; // 160×120 (smallest useful size)
config.jpeg_quality = 20;            // Lower quality = less data to process
config.fb_count = 1;                 // Single buffer (no wasteful double-buffering)
```

The camera is **off by default**. It only activates when the user triggers a feeding gesture (left tilt on FOOD_MENU). A single capture at 160×120 takes ~100ms at 240MHz, after which the CPU drops back to 80MHz. The camera sensor remains powered but inactive between captures.

**What if the camera ran continuously?** At 20MHz XCLK with VGA resolution, the camera alone would consume ~200mW and push the chip temperature to 70°C+ in a sealed enclosure.

---

# Chapter 13: Common Embedded Mistakes (Using Your Code As Example)

This chapter documents real bugs found in the KAKU firmware and explains why they happened, how they were fixed, and how to prevent similar issues.

## Mistake 1: Stack Overflow from Deep Call Chains

**The bug:** The OLED task had a 4096-byte stack. Deep animation call chains (`displayPetAnimation()` → multiple icon draws → `display.drawBitmap()` → Adafruit GFX internal calls) consumed more than 4096 bytes, corrupting adjacent memory and causing random crashes.

**The symptom:** The device would crash randomly during certain animations — not every time, but often enough to notice. Serial monitor showed "Stack canary watchpoint" or garbage characters.

**Why it happened:** In desktop programming, the stack is virtually unlimited (usually 1-8MB). In FreeRTOS, each task has a fixed stack set at creation time. If the call chain goes deeper than expected, the stack grows past its boundary ("overflow") and writes into other memory — possibly another task's stack, a mutex structure, or the heap metadata.

**The fix:**
```cpp
// BEFORE: Too small
xTaskCreatePinnedToCore(oledTask, "OLED", 4096, ...);

// AFTER: 2× headroom
xTaskCreatePinnedToCore(oledTask, "OLED", 8192, ...);
```

**Prevention:** Use `uxTaskGetStackHighWaterMark(&taskHandle)` in development to measure actual stack usage, then set the stack size to 2× the measured high water mark.

## Mistake 2: `continue` Inside a Function (Not a Loop)

**The bug:** During a code patch, the `continue` keyword was accidentally used inside `loop()` —  which is a function, not a syntactic loop construct. In C/C++, `continue` only works inside `for`, `while`, or `do-while` loops.

**The code:**
```cpp
void loop() {
    // ...
    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.println("[I2C] Mutex timeout");
        reading.accel_x = 0.0;
        continue;  // ← COMPILE ERROR: 'continue' not within a loop
    }
    // ...
}
```

Even though `loop()` is called repeatedly by the Arduino framework (it runs in a `while(true)` internally), the `continue` keyword sees `loop()` as a plain function — there's no `while` or `for` in the source code of `loop()` itself.

**The fix:** Restructure with `if/else`:
```cpp
if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    reading.accel_x = reading.accel_y = reading.accel_z = 0.0;
    reading.gyro_x = reading.gyro_y = reading.gyro_z = 0.0;
} else {
    mpu.getAcceleration(&ax, &ay, &az);
    mpu.getRotation(&gx, &gy, &gz);
    if (i2cMutex) xSemaphoreGive(i2cMutex);
    reading.accel_x = ax / 16384.0 * 9.81;
    // ... convert values ...
}
```

**Lesson:** Always test-compile after manual patches. Static analysis tools would have caught this instantly.

## Mistake 3: Creating WiFiClientSecure on the Stack

**The problem (before fix):** `sendAudioData()` created a local `WiFiClientSecure`:

```cpp
void sendAudioData(String audioBase64) {
    WiFiClientSecure localSSL;  // ← 16KB allocated on networkTask's stack
    localSSL.setInsecure();
    // ... HTTP request using localSSL ...
}  // localSSL destroyed here — 16KB freed from stack
```

`WiFiClientSecure` allocates ~16KB for TLS buffers. `networkTask` has a 16KB stack. That leaves 0 bytes for everything else — instant stack overflow.

**The fix:** Use the global `sslNet` client shared across all non-OTA requests:

```cpp
void sendAudioData(String audioBase64) {
    HTTPClient http;
    http.begin(sslNet, url);  // Uses global SSL client — 0 bytes on stack
    // ...
}
```

**Lesson:** On microcontrollers, always be aware of the stack cost of local variables. Objects that internally use `malloc` (like `WiFiClientSecure`) may allocate far more memory than their `sizeof()` suggests.

## Mistake 4: I2C Mutex with `portMAX_DELAY` (Deadlock Risk)

**The original code:**
```cpp
if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
    mpu.getAcceleration(&ax, &ay, &az);
    xSemaphoreGive(i2cMutex);
}
```

**The problem:** If the task holding the I2C mutex crashes or enters an infinite loop (say, the I2C bus locks up and `mpu.getAcceleration()` never returns), every other task waiting for `i2cMutex` with `portMAX_DELAY` waits **forever**. The entire system freezes.

**The fix:** Use 200ms timeout with fallback:
```cpp
if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    Serial.println("[I2C] Mutex timeout");
    // Use safe defaults instead of crashing
    return data;
}
```

**Lesson:** Use `portMAX_DELAY` only for truly critical, always-available resources. For hardware access (which can physically lock up), always use a finite timeout.

## Mistake 5: Using `DynamicJsonDocument` in Hot Paths

**Problem:** `DynamicJsonDocument` calls `malloc()` internally. In functions called every 2 seconds (like `sendSensorDataOnly()`), this creates heap fragmentation over time.

**Example:**
```cpp
// BAD — allocates and frees 4096 bytes every 2 seconds:
void sendSensorDataOnly(SensorData data) {
    DynamicJsonDocument doc(4096);  // malloc(4096)
    doc["accel_x"] = data.accel_x;
    // ...
}  // Destructor calls free(4096)
```

After 24 hours: 43,200 malloc/free cycles. The heap becomes Swiss cheese — plenty of total free space, but fragmented into tiny unusable blocks.

**Fix:** Global StaticJsonDocument reused forever:
```cpp
StaticJsonDocument<4096> g_sensorDoc;  // Allocated once in BSS segment

void sendSensorDataOnly(SensorData data) {
    g_sensorDoc.clear();  // Reset contents, keeps memory
    g_sensorDoc["accel_x"] = data.accel_x;
    // ...
}
```

**Lesson:** On embedded systems, prefer static allocation for any repeatedly-used buffer. Reserve dynamic allocation for genuinely variable-size, infrequent operations.

## Mistake 6: Not Checking `http.getSize()` Before `http.getString()`

**Problem:** `http.getString()` allocates a `String` of size `http.getSize()`. If the server sends an unexpectedly large response (attack, bug, corrupted data), this could allocate hundreds of KB, exhausting the heap.

**Before fix:**
```cpp
String response = http.getString();  // Could be ANY size
```

**After fix:**
```cpp
int size = http.getSize();
if (size > 4096) {
    Serial.printf("[SAFETY] Response too large: %d bytes\n", size);
    http.end();
    return;
}
String response = http.getString();
```

**Lesson:** Never trust external data. Always validate sizes before allocating memory based on them.

## Mistake 7: `sslNet.stop()` Clears Configuration

```cpp
void resetSSLConnection() {
    sslNet.stop();
    // ← If you stop here, setInsecure() is lost!
    // The next http.begin(sslNet, ...) will try certificate validation
    // and fail because no CA cert is loaded.
    
    sslNet.setInsecure();  // Must re-apply after stop()
    sslNet.setTimeout(10); // Must re-apply after stop()
}
```

**Why?** `WiFiClientSecure::stop()` is a "hard reset" that clears all internal state, including configuration. This is documented in the ESP32 Arduino source but easy to miss. The symptom would be that after an SSL reset, the next HTTP request fails with "SSL handshake failed" — and the auto-reset triggers again, creating an infinite loop of failures.

---

# Chapter 14: How To Improve Further

## Priority 1: Critical Improvements

### 1.1 — Make `savePetState()` Non-Blocking

**Current risk:** `savePetState()` holds `petStateMutex` with `portMAX_DELAY` while writing to NVS flash. Flash writes can take 10-50ms. During this time, any other task trying to read `g_petState` is blocked.

**Improvement:** Copy state under mutex, then write to NVS without the mutex:

```cpp
void savePetState() {
    PetLocalState copy;
    if (petStateMutex) {
        xSemaphoreTake(petStateMutex, portMAX_DELAY);
        copy = g_petState;  // Struct copy (fast, <1μs)
        xSemaphoreGive(petStateMutex);
    }
    // Now write to NVS without holding the mutex:
    petPrefs.begin("pet", false);
    petPrefs.putInt("hunger", copy.hunger);
    // ... write all fields from copy ...
    petPrefs.end();
}
```

### 1.2 — Remove Dead VAD Audio Path

The VAD mode records audio into a `detectedAudioData` String buffer (~213KB when base64-encoded). This data is never sent — the STT path replaced it. The VAD recording code should be removed to save:
- ~213KB peak PSRAM during recording
- CPU time for base64 encoding
- Code complexity

### 1.3 — Add `WiFi.disconnect()` Before `WiFi.begin()` in Reconnection

```cpp
// Current code (missing disconnect):
WiFi.begin(ssid.c_str(), pass.c_str());

// Improved code:
WiFi.disconnect(true);  // true = also erase stored AP
delay(100);
WiFi.begin(ssid.c_str(), pass.c_str());
```

Without `disconnect()`, the WiFi stack might try to reconnect to the old AP while simultaneously connecting to the new one, causing driver state confusion.

## Priority 2: Quality Improvements

### 2.1 — Add Hardware Watchdog

```cpp
// In loop():
esp_task_wdt_init(30, true);  // 30 second timeout, panic on timeout
esp_task_wdt_add(NULL);       // Add current task to watchdog

// In every iteration of loop():
esp_task_wdt_reset();  // Feed the watchdog
```

If `loop()` hangs for 30 seconds (deadlock, infinite loop, I2C bus lockup), the watchdog forces a reboot.

### 2.2 — Add Stack High Water Mark Monitoring

```cpp
// Add to the 10-second debug output:
UBaseType_t audioHWM = uxTaskGetStackHighWaterMark(audioTaskHandle);
UBaseType_t cameraHWM = uxTaskGetStackHighWaterMark(cameraTaskHandle);
UBaseType_t oledHWM = uxTaskGetStackHighWaterMark(oledTaskHandle);

Serial.printf("Stack HWM: Audio=%u Camera=%u OLED=%u\n",
              audioHWM, cameraHWM, oledHWM);
```

This tells you exactly how close each task is to stack overflow. If any value drops below 500 bytes, increase that task's stack size immediately.

### 2.3 — Replace Strings with Fixed-Size Char Arrays

`String` (Arduino's dynamic string class) uses `malloc` internally. Every concatenation may trigger a `realloc()`. In the WiFi config web page handler, there are 60+ `String` concatenations — each potentially fragmenting the heap.

**Alternative:** Use a pre-allocated char buffer with `snprintf()`:

```cpp
// Instead of:
String html = "<html><body>";
html += "<h1>KAKU Setup</h1>";
html += "<p>Enter WiFi credentials:</p>";
// ... 60 more concatenations ...

// Use:
char html[4096];
int pos = 0;
pos += snprintf(html + pos, sizeof(html) - pos, "<html><body>");
pos += snprintf(html + pos, sizeof(html) - pos, "<h1>KAKU Setup</h1>");
// ...
```

### 2.4 — Replace `portMAX_DELAY` on `petStateMutex`

Remaining sites using `portMAX_DELAY` are all on `petStateMutex`. Change to 500ms timeout:

```cpp
// Instead of:
if (petStateMutex) xSemaphoreTake(petStateMutex, portMAX_DELAY);

// Use:
if (petStateMutex &&
    xSemaphoreTake(petStateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    // ... access g_petState ...
    xSemaphoreGive(petStateMutex);
} else {
    Serial.println("[PET] Mutex timeout — skipping");
}
```

This eliminates the last potential infinite-wait deadlock in the codebase.

## Priority 3: Future Features

### 3.1 — Add Bluetooth Low Energy (BLE) for Proximity Detection

The ESP32-S3 has BLE 5.0. You could use iBeacon/Eddystone scanning to detect when the owner's phone is nearby, enabling proximity-based features (pet gets happier when you're close).

### 3.2 — Add Deep Sleep Mode

When the device detects extended inactivity (30+ minutes), enter ESP32 deep sleep mode (~10μA). Wake on MPU6050 motion interrupt. This would extend battery life from hours to days for a battery-powered version.

### 3.3 — Add Over-the-Air Configuration

Instead of hardcoding API URLs and thresholds, fetch a configuration JSON from the server on startup. This allows changing behavior without a firmware update.

---

# Chapter 15: Glossary

| Term | Simple Definition | Technical Definition |
|------|-------------------|---------------------|
| **AES** | A way to scramble data so only the intended recipient can read it | Advanced Encryption Standard — symmetric-key block cipher used in TLS |
| **AP Mode** | The ESP32 becomes a WiFi hotspot | Access Point mode — the device creates its own wireless network |
| **Backoff** | Waiting longer between retries after failures | Exponential increase in retry delay: 100ms → 200ms → 400ms... |
| **Bitmap** | A picture stored as a grid of on/off pixels | Array of bytes where each bit represents one pixel (monochrome) |
| **BSS** | The place in memory where global variables live before initialization | Block Started by Symbol — zero-initialized global/static data segment |
| **Core** | One of two "brains" in the CPU | An independent execution unit in a multi-core processor |
| **DMA** | Hardware that moves data without bothering the CPU | Direct Memory Access — peripheral-to-memory transfers without CPU intervention |
| **Deadlock** | Two tasks waiting for each other forever | Circular mutex dependency where no task can proceed |
| **Flash** | Storage that keeps data when power is off (like a USB drive) | Non-volatile NAND flash memory, 100K write cycles per sector |
| **FreeRTOS** | The traffic controller that runs multiple tasks "simultaneously" | Preemptive real-time operating system kernel with priority-based scheduling |
| **GPIO** | A pin on the chip you can control with software | General-Purpose Input/Output — configurable digital pin |
| **Heap** | A pool of shared memory any task can borrow from | Dynamic memory region managed by malloc/free allocator |
| **HTTPS** | Secure web communication (the lock icon in your browser) | HTTP over TLS — encrypted, authenticated, integrity-checked transport |
| **I2C** | A two-wire bus for talking to sensors and displays | Inter-Integrated Circuit — half-duplex serial bus with master/slave addressing |
| **I2S** | A digital audio interface | Inter-IC Sound — serial bus for PCM/PDM audio data |
| **IMU** | A chip that measures motion and rotation | Inertial Measurement Unit — accelerometer + gyroscope |
| **ISR** | A function that runs immediately when a hardware event occurs | Interrupt Service Routine — preempts normal execution for time-critical events |
| **JSON** | A text format for structured data (like a digital form) | JavaScript Object Notation — human-readable key-value data interchange format |
| **Mutex** | A lock that only one task can hold at a time | Mutual Exclusion semaphore — binary synchronization primitive with ownership |
| **NVS** | A mini-database on the flash chip that survives reboots | Non-Volatile Storage — key-value store on flash with wear leveling |
| **OTA** | Updating software wirelessly | Over-The-Air — firmware update via network download, no physical connection |
| **PDM** | A digital microphone format | Pulse Density Modulation — 1-bit high-rate digital representation of analog audio |
| **PSRAM** | Extra RAM connected via SPI (slower but much larger) | Pseudo-Static RAM — SPI-attached DRAM with auto-refresh, appears as static RAM |
| **Priority** | Which task gets to run first when multiple are ready | Numeric task priority: higher number = scheduler prefers this task |
| **Queue** | A pipe between tasks: one puts messages in, another takes them out | Thread-safe FIFO buffer with blocking send/receive operations |
| **Race Condition** | Two tasks modifying shared data simultaneously, causing corruption | Undefined behavior from unsynchronized concurrent access to shared state |
| **RTOS** | An operating system designed for real-time embedded devices | Real-Time Operating System — deterministic scheduling with bounded latency |
| **SSL/TLS** | The encryption layer that makes HTTPS secure | Secure Sockets Layer / Transport Layer Security — cryptographic protocol |
| **Semaphore** | A signaling mechanism between tasks (like a traffic light) | Counting or binary synchronization primitive without ownership tracking |
| **SPI** | A four-wire fast communication bus | Serial Peripheral Interface — full-duplex synchronous serial bus |
| **SRAM** | The fast, limited main memory inside the chip | Static Random-Access Memory — flip-flop based, no refresh needed |
| **Stack** | Each task's private workspace for local variables | LIFO memory structure for function frames, local variables, return addresses |
| **Stack Overflow** | A task's workspace grows beyond its boundary, corrupting other memory | Writing past the allocated stack boundary, corrupting adjacent memory regions |
| **Task** | An independent job running on the processor | FreeRTOS thread with own stack, priority, state, and optional core affinity |
| **Tick** | The scheduler's heartbeat (1ms on ESP32 by default) | FreeRTOS scheduler temporal quantum — configurable via configTICK_RATE_HZ |
| **VAD** | Detecting when someone is talking (vs silence) | Voice Activity Detection — classifying audio frames as speech or non-speech |
| **Volatile** | Telling the compiler "always read this from memory, don't cache it" | C/C++ qualifier preventing compiler optimization of memory access |
| **WAV** | A standard audio file format with a 44-byte header | Waveform Audio File Format — uncompressed PCM audio with RIFF container |
| **Watchdog** | A safety timer that reboots the device if software hangs | Hardware countdown timer that triggers reset if not periodically serviced |
| **Wear Leveling** | Spreading flash writes evenly to prevent wearing out one spot | Algorithm distributing erase/write cycles across flash sectors to extend lifetime |

---

## Appendix: Quick Reference — Your Firmware's Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `FIRMWARE_VERSION` | `"1.0.5"` | Current firmware version string |
| `PHYSIO_TICK_MS` | 360,000ms (6min) | Physiology update interval |
| `INTERNAL_READ_INTERVAL` | 100ms | Sensor batch collection interval |
| `SAMPLE_RATE` | 16,000 Hz | Audio sample rate |
| `VAD_THRESHOLD` | 1,000 | Energy threshold for voice detection |
| `VAD_MIN_DURATION` | 500ms | Minimum speech duration to trigger recording |
| `SILENCE_TIMEOUT` | 2,000ms | Silence duration to stop recording |
| `STT_SEND_INTERVAL` | 3,000ms | STT WAV chunk interval |
| `STT_TIMEOUT_MS` | 120,000ms | STT inactivity timeout |
| `STT_TILT_HOLD_TIME` | 10,000ms | Hold duration to activate STT |
| `STEP_BARRIER_G2` | 0.25 g² | Step detection acceleration threshold |
| `STEP_MIN_MS` | 600ms | Minimum time between detected steps |
| `LP_ALPHA_STEP` | 0.85 | Low-pass filter weight for gravity estimate |
| `MENU_TILT_HOLD_TIME` | 2,000ms | Right-tilt hold to cycle menu |
| `MENU_CYCLE_COOLDOWN` | 3,000ms | Cooldown between menu cycles |
| `NEUTRAL_SLEEP_TIMEOUT` | 30,000ms | Neutral position duration → sleep mode |
| `WALKING_WINDOW_MS` | 3,000ms | Walking animation duration after last step |
| `HTTP_ERROR_RESET_THRESHOLD` | 3 | Consecutive HTTP errors before SSL reset |
| `WIFI_RECONNECT_MAX_FAILS` | 10 | Consecutive WiFi failures → AP provisioning |
| `DAILY_REBOOT_MS` | 86,400,000ms | Automatic reboot after 24h uptime |
| Heap critical threshold | 20,000 bytes | Emergency reboot below this free heap |

---

*End of KAKU Firmware Learning Guide*  
*Generated from production firmware analysis — esp32_sketch.ino v1.0.5*
