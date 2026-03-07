# KAKU ESP32 Tamagotchi — Complete Manual Testing Checklist

**Device:** XIAO ESP32-S3 Sense  
**Firmware:** v1.0.5  
**OLED:** 64×32 SSD1306 (I2C, 0x3C)  
**Date:** ___________  
**Tester:** ___________

---

## 1. PROJECT FEATURE MAP

| # | Feature | Source |
|---|---------|--------|
| 1 | Egg hatching animation (first boot only) | `setup()` / NVS `hasHatched` |
| 2 | Pet lifecycle: INFANT → CHILD → ADULT → OLD | `PetAge` enum, age days 0-5/6-10/11-17/18+ |
| 3 | Pet physiology engine (hunger, health, energy, happiness, discipline, XP) | `handlePhysiology()` every 360s |
| 4 | Sickness engine (random chance when health ≤25 or poop present) | `handlePhysiology()` |
| 5 | Poop generation (5% chance per tick when hunger < 50) | `handlePhysiology()` |
| 6 | Emotion-based animations: IDLE, HAPPY, SAD, CRY, SURPRISE, POOP, HUNGER, SICK | `displayPetAnimation()` |
| 7 | Menu system via right-tilt 2s hold: MAIN → FOOD → TOILET → PLAY → HEALTH → STATUS → STATS → MAIN | `cycleMenu()` |
| 8 | Food menu: crying anim when hungry, feeding gesture (left tilt 3s), camera capture, eating animation | `displayFoodMenu()` |
| 9 | Toilet menu: poop indicator, cleaning gesture (left tilt 3s), slide animation | `displayToiletMenu()` |
| 10 | Play menu: 2 mini-games (Catch Food / Dodge Obstacle), tilt-controlled, KakuCoin reward | `displayPlayMenu()` |
| 11 | Health menu: sick indicator, medicine gesture (left tilt 3s), injection animation (34 frames × 3 loops) | `displayHealthMenu()` |
| 12 | Status info menu: smiley, age, flash icon, calories | `displayStatusInfoMenu()` |
| 13 | Stats menu: happiness + discipline progress bars | `displayStatsMenu()` |
| 14 | Walking animation: hardware step counter (stoss/barrier), 6-frame walk anim for 3s after step | `detectHardwareStep()` |
| 15 | Sleeping animation: 2-frame Zzz anim on MAIN when `isDeviceSleeping` | `displaySleepingAnimation()` |
| 16 | Deep sleep: 2 min inactivity → hardware deep sleep, wake on MPU6050 motion (ext1 GPIO2) | `enterDeepSleep()` |
| 17 | WiFi provisioning: QR code AP mode (KAKU_SETUP), HTML config page, NVS storage (up to 5 networks) | `startWiFiProvisioningAP()` |
| 18 | WiFi auto-reconnect: tries NVS credentials → hardcoded fallback → AP mode after 10 fails | `loop()` WiFi section |
| 19 | Network scheduler: sensor data every 10s, OLED+events every 2s (staggered, no overlap) | `loop()` staggered scheduler |
| 20 | Camera: on-demand only (init when hungry, deinit when fed), 160×120 JPEG | `cameraMonitorTask()` |
| 21 | PDM microphone: VAD (Voice Activity Detection), speech recording, STT streaming mode | `audioMonitorTask()` |
| 22 | STT mode: left tilt 10s on MAIN → stream WAV audio to server every 3s, 120s timeout | `checkSTTTiltGesture()` |
| 23 | OTA firmware update: server-pushed, 3 retries, progress bar on OLED, dual-partition A/B swap | `checkAndPerformOTA()` |
| 24 | Age transition animation: AGE UP → star burst → confetti → HAPPY BDAY → LEVEL UP → XP counter | `playAgeTransitionAnimation()` |
| 25 | CPU frequency scaling: 80 MHz idle, 160 MHz network, 240 MHz camera/OTA | `safeCpuFreq()` |
| 26 | Pet state persistence in NVS (survives deep sleep + reboots) | `savePetState()` / `loadPetState()` |
| 27 | Server sync: OLED display state, events, emotion overrides, food/poop/sick icons | `getOLEDDisplayFromServer()` |
| 28 | LED feedback: blink on sensor send success, events (high sound, motion, alert) | `processEvent()` |
| 29 | Daily maintenance reboot (24h uptime guard) | `loop()` |
| 30 | Heap safety: auto-reboot below 20 KB free | `loop()` |
| 31 | Home icon: always visible on MAIN, hidden on sub-menus | icon priority logic |
| 32 | Status icon priority: SICK > POOP > HUNGER > PLAY (mutual exclusion) | `displayPetAnimation()` |
| 33 | Play icon: appears 15 min after poop cleared, hidden when sick | `syncLocalStateToUI()` |
| 34 | SSL error recovery: auto-reset after 3 consecutive HTTP failures | `trackHttpResult()` |

---

## 2. HARDWARE TEST CHECKLIST

### 2.1 OLED Display (SSD1306 64×32, I2C 0x3C)

| # | Test | Action | Expected Result | Pass/Fail | Notes |
|---|------|--------|-----------------|-----------|-------|
| 1 | Display init | Power ON, watch serial for "OLED initialized" | "✅ OLED initialized" in serial, screen shows "KAKU / Connecting.." | ☐ | |
| 2 | Brightness & contrast | View display in dark room and under direct light | All pixels crisp, no dead rows/columns, readable in both conditions | ☐ | |
| 3 | Pixel clear | After egg animation or menu switch, check for ghost pixels | Screen fully clears between states — no artifact remnants | ☐ | |
| 4 | Text rendering | Navigate to STATUS and STATS menus | Numbers/letters readable, no garbled characters | ☐ | |
| 5 | Bitmap animations | On MAIN screen, observe pet idle animation | Pet sprite renders correctly centered, smooth frame changes | ☐ | |
| 6 | Icon rendering | Wait for hunger → food icon bottom-right; wait for poop → poop icon | Icons render at correct position without overlapping pet sprite | ☐ | |
| 7 | Display during deep sleep | Let device go to sleep (2 min idle) | Shows "Zzz... / Shake 2 wake" then blanks OLED before sleep | ☐ | |

### 2.2 MPU6050 (I2C 0x68, INT on GPIO2)

| # | Test | Action | Expected Result | Pass/Fail | Notes |
|---|------|--------|-----------------|-----------|-------|
| 1 | I2C detection | Power ON, watch serial for I2C scan | "✅ I2C device found at address 0x68 (MPU6050)" | ☐ | |
| 2 | Init success | Watch serial during boot | "✅ MPU6050 initialized successfully!" | ☐ | |
| 3 | Gravity warmup | Watch serial after MPU init | "✅ Step gravity filter warmed up" (40 samples over ~1s) | ☐ | |
| 4 | Right tilt detection | Tilt device right (~45°), hold 2s | Serial: "📱 Menu tilt right started..." then "🔄 Right tilt 2s held → Cycling menu..." | ☐ | |
| 5 | Left tilt detection | On FOOD_MENU when hungry, tilt left ~45°, hold 3s | Serial: "🍽️ Feeding gesture started..." then "📸 Triggering food image capture!" | ☐ | |
| 6 | Step detection | Walk with device in hand (natural motion) | Serial: "👣 HW Step #N stoss=X.XXXX", walking animation plays on MAIN screen | ☐ | |
| 7 | Motion interrupt | Leave device still, then shake suddenly | Serial ISR count increments, `lastMotionTime` resets (no deep sleep) | ☐ | |
| 8 | False step rejection | Tap device gently on desk / tilt slowly | No step registered (STEP_BARRIER_G2=0.25, STEP_MIN_MS=600) | ☐ | |
| 9 | Dead zone | Set device flat on table | No tilt gestures trigger, no false walking animation | ☐ | |

### 2.3 Camera (OV2640, XIAO ESP32-S3 Sense)

| # | Test | Action | Expected Result | Pass/Fail | Notes |
|---|------|--------|-----------------|-----------|-------|
| 1 | Starts OFF | Check serial at boot | "📷 Camera OFF at boot (will init when pet is hungry)" | ☐ | |
| 2 | Init on hunger | Wait until pet becomes hungry (hunger > 70) | Serial: "📷 Hunger started → camera init queued" then "Camera initialized successfully" | ☐ | |
| 3 | Capture on feed gesture | On FOOD_MENU, left tilt 3s | Serial: "📸 Core 0: Feeding triggered - capturing fresh image...", then "✅ Core 0: Image captured: N bytes" | ☐ | |
| 4 | Image upload | After capture | Serial: "📊 Sending image data: N bytes", eating animation plays on OLED | ☐ | |
| 5 | Deinit on satisfied | Feed pet successfully (hunger < 70) | Serial: "📷 Hunger satisfied → camera deinit queued" then "📷 Camera deinitialized and powered down" | ☐ | |
| 6 | No heat at idle | Touch camera module after 5 min of non-hungry state | Camera module stays cool (pins in INPUT low-power mode) | ☐ | |
| 7 | Camera + I2C conflict | Capture image then immediately read MPU | Both succeed — no I2C bus lockup (camera uses separate SCCB bus) | ☐ | |

### 2.4 PDM Microphone (I2S, CLK=GPIO42, DIN=GPIO41)

| # | Test | Action | Expected Result | Pass/Fail | Notes |
|---|------|--------|-----------------|-----------|-------|
| 1 | Init success | Watch serial at boot | "✅ PDM Microphone initialized successfully" | ☐ | |
| 2 | Ambient noise level | In quiet room, check serial (10s interval) | "🎤 Audio Energy: ~0-500" (low baseline) | ☐ | |
| 3 | Voice detection (VAD) | Speak near device for >500ms | Serial: "🎤 Core 0: Speech detected! Starting recording..." | ☐ | |
| 4 | Recording completion | Speak then stop (2s silence) | Serial: "🎤 Core 0: Recording complete! N bytes" | ☐ | |
| 5 | Mic sleep mode | Stay silent for >2 seconds | Audio task enters 500ms slow-poll (reduced CPU/heat) | ☐ | |
| 6 | STT activation | On MAIN, tilt left and hold 10 seconds | Serial: "🎤✅ STT MODE ACTIVATED!", OLED shows blinking "Talk" label | ☐ | |
| 7 | STT streaming | While STT active, speak | Serial: "🎤 STT: Queued N bytes WAV | voice=YES" every 3s | ☐ | |
| 8 | STT timeout | Activate STT, don't interact for 120s | Serial: "🎤 STT timeout (120s no interaction) — flag cleared" | ☐ | |

### 2.5 WiFi (XIAO ESP32-S3)

| # | Test | Action | Expected Result | Pass/Fail | Notes |
|---|------|--------|-----------------|-----------|-------|
| 1 | Connect to stored WiFi | Power ON with known WiFi in range | Serial: "✅ Connected to 'SSID' — IP: x.x.x.x" | ☐ | |
| 2 | Show IP on OLED | After WiFi connect (non-deep-sleep boot) | OLED briefly shows "WiFi OK / SSID / IP" | ☐ | |
| 3 | AP provisioning mode | Power ON with no known WiFi available (or 10 reconnect failures) | Serial: "📡 Starting WiFi provisioning AP mode...", OLED shows QR code | ☐ | |
| 4 | QR code scan | Scan OLED QR code with phone | Phone connects to KAKU_SETUP AP | ☐ | |
| 5 | Config page | Open http://192.168.4.1 on phone | Beautiful config page with available networks, SSID/password form | ☐ | |
| 6 | Save credentials | Select network, enter password, submit | "Saved! KAKU is restarting..." → device reboots and connects | ☐ | |
| 7 | Multiple stored WiFi | Save 2+ different networks, remove first | Device auto-connects to remaining network on next boot | ☐ | |
| 8 | Reconnect on drop | Briefly disable router, re-enable | Serial: "❌ WiFi disconnected, attempting reconnect..." then "✅ WiFi Reconnected" | ☐ | |
| 9 | Light-sleep enabled | Check serial after WiFi connect | "📶 WiFi: light-sleep ON, auto-reconnect ON, TX 11dBm" | ☐ | |
| 10 | HTTP sensor upload | Observe serial every ~10s | "📤 Queue: NET_SENSOR" followed by "📊 Sending sensor data" | ☐ | |

### 2.6 LED (GPIO21, Active LOW)

| # | Test | Action | Expected Result | Pass/Fail | Notes |
|---|------|--------|-----------------|-----------|-------|
| 1 | Off at boot | Power ON | LED stays OFF (GPIO21 HIGH) | ☐ | |
| 2 | Blink on sensor send | Wait for sensor data send success | Quick LED blink (50ms) | ☐ | |
| 3 | Event feedback | Trigger a server event (high_sound / sudden_motion) | LED blinks as per event type | ☐ | |
| 4 | Off before deep sleep | Let device enter deep sleep | LED is forced OFF (HIGH) before sleep | ☐ | |

---

## 3. INTERACTION TESTING (Real User Simulation)

### 3.1 Power ON — First Boot (Egg Hatch)

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Plug in USB-C or power supply | LED off, serial output starts | ☐ | |
| 2 | Wait for OLED | "KAKU / Connecting.." on OLED | ☐ | |
| 3 | WiFi connects | "WiFi OK / SSID / IP" on OLED (2s) | ☐ | |
| 4 | Egg animation | 4-frame egg cracking plays (2s per frame = ~8s total) | ☐ | |
| 5 | Egg slides left | Egg image slides out to the left | ☐ | |
| 6 | Infant slides in | Baby pet slowly enters from left (~3s) | ☐ | |
| 7 | Main screen | INFANT idle face + home icon at top-left | ☐ | |
| 8 | NVS flag set | Reboot device → egg animation does NOT replay | ☐ | |
| 9 | `FORCE_EGG_HATCH=true` | If set, egg replays even after NVS flag | ☐ | |

### 3.2 Power ON — Subsequent Boot

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Power ON | "📂 Pet already hatched — loading saved state from NVS..." | ☐ | |
| 2 | Pet state restored | Age, XP, level, hunger, health match previous session | ☐ | |
| 3 | No egg animation | Goes straight to main screen with appropriate age pet | ☐ | |

### 3.3 Power ON — After Deep Sleep Wake

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Shake device to wake | Device reboots | ☐ | |
| 2 | Wake reason | Serial: "Wake-up: EXT1 (MPU6050 motion)" and "RTC wake count: N" | ☐ | |
| 3 | OLED shows | "Waking up... / Please wait" briefly | ☐ | |
| 4 | State preserved | Pet stats loaded from NVS — same as before sleep | ☐ | |

### 3.4 Idle State Observation

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Leave on desk, MAIN screen | Pet shows IDLE face (static, no animation) | ☐ | |
| 2 | Home icon visible | Home icon at top-left corner | ☐ | |
| 3 | No status icons initially | No food/poop/sick/play icons (if fresh state) | ☐ | |
| 4 | Serial heartbeat | Every 3s: "📺 Current Screen: MAIN | Age: X | Emotion: IDLE" | ☐ | |
| 5 | Heap report | Every 10s: WiFi status + heap info printed | ☐ | |

### 3.5 Menu Navigation (Right Tilt Cycle)

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Tilt right, hold 2s | MAIN → FOOD_MENU. Serial: "📡 Menu cycle: MAIN → FOOD_MENU" | ☐ | |
| 2 | Wait 3s cooldown, tilt right 2s again | FOOD_MENU → TOILET_MENU | ☐ | |
| 3 | Repeat | TOILET_MENU → PLAY_MENU | ☐ | |
| 4 | Repeat | PLAY_MENU → HEALTH_MENU | ☐ | |
| 5 | Repeat | HEALTH_MENU → STATUS_INFO_MENU | ☐ | |
| 6 | Repeat | STATUS_INFO_MENU → STATS_MENU | ☐ | |
| 7 | Repeat | STATS_MENU → MAIN (full cycle) | ☐ | |
| 8 | Home icon | Home icon visible ONLY on MAIN, hidden on all others | ☐ | |
| 9 | Quick release | Tilt right for <2s then release | No menu change (hold time not met) | ☐ | |
| 10 | Cooldown respect | Tilt right 2s, immediately tilt right 2s again | Second cycle ignored (3s cooldown) | ☐ | |

### 3.6 Feeding Flow (FOOD_MENU)

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Navigate to FOOD_MENU | If not hungry: happy face + food icon at top-left | ☐ | |
| 2 | Wait for hunger (>70) | Crying animation plays; food icon appears bottom-right on MAIN | ☐ | |
| 3 | Camera inits | Serial: "📷 Hunger started → camera init queued" | ☐ | |
| 4 | On FOOD_MENU, tilt left 3s | Serial: "🍽️ Feeding gesture started..." then "📸 Triggering food image capture!" | ☐ | |
| 5 | Eating animation | Pacman eating animation plays on OLED (looping while uploading) | ☐ | |
| 6 | Image uploaded | Serial shows image upload. Animation stops. | ☐ | |
| 7 | "GOOD!" text | "GOOD!" text displays for 3 seconds | ☐ | |
| 8 | After GOOD! | Returns to happy face on FOOD_MENU | ☐ | |
| 9 | Hunger reduced | Serial shows hunger decreased by 40, XP +20 | ☐ | |
| 10 | Camera deinits | When hunger drops below 70: "📷 Hunger satisfied → camera deinit queued" | ☐ | |

### 3.7 Cleaning Flow (TOILET_MENU)

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Navigate to TOILET_MENU | No poop: shows "Cleared" + toilet icon | ☐ | |
| 2 | Wait for poop (5% chance per physio tick when hunger < 50) | Poop icon on MAIN, angry pet face | ☐ | |
| 3 | On TOILET_MENU with poop | Shows "Clean me" text | ☐ | |
| 4 | Tilt left 3s | Serial: "🚽 Cleaning gesture started..." then "🧹 Starting cleaning animation!" | ☐ | |
| 5 | Slide animation | Sprite walks from right to left across screen | ☐ | |
| 6 | Cleared | "Cleared!" text shows after sprite exits | ☐ | |
| 7 | Poop cleared | Poop icon gone, happiness +20, XP +10 | ☐ | |
| 8 | Server notified | Serial: NET_CLEAN queued, server receives cleaning request | ☐ | |

### 3.8 Medicine Flow (HEALTH_MENU)

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Navigate to HEALTH_MENU | Not sick: "All Good" + heart icon at top-left | ☐ | |
| 2 | Wait for pet to get sick | Blinking heart icon on MAIN, sad/sick animation | ☐ | |
| 3 | On HEALTH_MENU when sick | "Give Med" text. Heart icon. | ☐ | |
| 4 | Tilt left → "HOLDING..." | Shows "HOLDING..." while gesture in progress | ☐ | |
| 5 | Hold 3s | Serial: "💉 Starting medicine injection animation!" | ☐ | |
| 6 | Injection animation | 34-frame syringe animation plays 3 times | ☐ | |
| 7 | Cured | "All Good" (2s), pet health +30, isSick=false | ☐ | |
| 8 | Server notified | NET_INJECT queued, server clears sick_pending | ☐ | |

### 3.9 Play Menu (Mini-Games)

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Navigate to PLAY_MENU | "PLAY" text + play icon, static idle screen | ☐ | |
| 2 | Tilt left, hold 3s | "GET READY!" + game type (Catch Food or Dodge!) for 2s | ☐ | |
| 3 | Game starts | Score/miss counters appear, food/obstacle falls | ☐ | |
| 4 | Tilt control | Tilt left/right → paddle/character moves smoothly | ☐ | |
| 5 | Catch Food: catch item | Score increments (+1, or +5 after 5 combo) | ☐ | |
| 6 | Catch Food: miss 3 | GAME OVER screen with falling coins, score + KC displayed | ☐ | |
| 7 | Dodge: obstacle missed | Score increments, speed increases every 5 dodges | ☐ | |
| 8 | Dodge: hit obstacle | Explosion rings → scatter pixels → "CRASH!" shake → score screen | ☐ | |
| 9 | KC reward sent | Serial: "🎮 Sending game reward: Score=X, KC=Y.Y" | ☐ | |
| 10 | Return to idle | After game over animation → back to PLAY_MENU idle screen | ☐ | |

### 3.10 Deep Sleep & Wake

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Leave device completely still on desk | After 2 min: serial "😴 No motion for 2 min → entering DEEP SLEEP" | ☐ | |
| 2 | Pre-sleep OLED | "Zzz... / Shake 2 wake" shown briefly, then OLED blanks | ☐ | |
| 3 | Pet state saved | Serial: "💾 Pet state saved to NVS" before sleep | ☐ | |
| 4 | Camera off | Serial: "📷 Camera deinitialized and powered down" (if it was on) | ☐ | |
| 5 | LED off | LED forced off before sleep | ☐ | |
| 6 | Device asleep | No serial output, no OLED, very low current draw | ☐ | |
| 7 | Shake to wake | Pick up and shake device | Device reboots, serial shows "Wake-up: EXT1 (MPU6050 motion)" | ☐ | |
| 8 | State restored | Pet stats unchanged from before sleep | ☐ | |

### 3.11 OTA Update Flow

| Step | Action | Expected | Pass/Fail | Notes |
|------|--------|----------|-----------|-------|
| 1 | Enable OTA from dashboard | Server sends `ota_update: true` in OLED response | ☐ | |
| 2 | OLED: Checking | "OTA UPDATE / Checking..." on screen | ☐ | |
| 3 | Version compare | Serial: "🆕 OTA: New firmware vX.X.X available (N bytes)" | ☐ | |
| 4 | Tasks suspended | "⏸️ All tasks suspended for OTA" | ☐ | |
| 5 | Download progress | OLED shows "FLASHING" + progress bar updating every 5% | ☐ | |
| 6 | Flash complete | "OTA DONE! / vX.X.X / Rebooting.." on OLED, then reboot | ☐ | |
| 7 | After reboot | New firmware version in serial, device operates normally | ☐ | |
| 8 | No update available | If firmware is current: "Up to date!" on OLED for 2s | ☐ | |
| 9 | OTA retry | If download fails mid-way, retries up to 3 times with 30s gap | ☐ | |

---

## 4. STRESS TESTING

| # | Test | Action | Duration | Watch For | Pass/Fail | Notes |
|---|------|--------|----------|-----------|-----------|-------|
| 1 | Rapid motion | Shake device vigorously | 60 seconds | No crash, step counter doesn't overflow, no I2C lockup | ☐ | |
| 2 | Rapid tilt gestures | Alternate right/left tilt quickly | 30 seconds | Menu cooldown respected, no double-trigger, no garbled OLED | ☐ | |
| 3 | Long runtime | Leave device powered on, interacting occasionally | 4+ hours | No memory leak (heap stable), no crash, physiology engine ticking, no excessive heat | ☐ | |
| 4 | 24h uptime guard | Leave device on for 24h | 24 hours | Auto-reboot fires: "🔄 Daily maintenance reboot (24h uptime)" | ☐ | |
| 5 | Repeated sleep/wake | Trigger deep sleep + immediately shake to wake, repeat 10× | 10 cycles | NVS saves/loads correctly each time, RTC wake count increments | ☐ | |
| 6 | Repeated camera on/off | Alternately feed pet then wait for hunger, repeat 5× | 5 cycles | Camera init/deinit succeeds each time, no resource leak, no stuck pins | ☐ | |
| 7 | Game marathon | Play catch food / dodge repeatedly without exiting | 15 min | No watchdog reset (TG1WDT_SYS_RST), scores calculate correctly, reward sends | ☐ | |
| 8 | Walk + menu | While actively walking (steps detected), try tilt-right to cycle menu | — | Menu tilt blocked during walking (`petIsWalking` guard) or still works cleanly | ☐ | |
| 9 | Concurrent listeners | On FOOD_MENU when hungry, tilt left to feed, but also have STT active | — | Only one action fires — feeding takes priority or STT only on MAIN | ☐ | |
| 10 | WiFi stress | Repeatedly toggle router on/off every 30s | 5 min | Reconnection logic handles gracefully, no crash, provisioning mode if threshold hit | ☐ | |

---

## 5. CONCURRENCY / TASK STABILITY TESTING

### 5.1 FreeRTOS Task Layout

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| OLED | 0 | 1 | 8192 | Animation rendering @ ~8 FPS |
| AudioMonitor | 0 | 2 | 12288 | VAD + STT mic streaming |
| CameraMonitor | 0 | 1 | 8192 | On-demand image capture |
| Network | 1 | 1 | 16384 | Queue-driven HTTP calls |
| Arduino loop() | 1 | 1 | — | Sensors, physiology, scheduling |

### 5.2 Mutex & Shared Resource Tests

| # | Test | Action | Expected | Pass/Fail | Notes |
|---|------|--------|----------|-----------|-------|
| 1 | `petStateMutex` | Feed pet while physiology tick runs | Both complete without deadlock. State consistent. | ☐ | |
| 2 | `i2cMutex` | Walk (step detection on loop Core 1) while OLED task reads MPU (checkMenuTiltGesture on Core 0) | Both get I2C access with 200ms timeout, no bus lockup | ☐ | |
| 3 | `cameraMutex` | Capture image while networkTask reads captured buffer | Image transfer atomic, no partial data sent | ☐ | |
| 4 | `cpuFreqMutex` | Network request (160 MHz) overlaps with camera capture (240 MHz) | No frequency toggling race, one wins the mutex | ☐ | |
| 5 | `uiStringsMutex` | Cycle menu while OLED task reads `currentScreenType` | No garbled screen type, 20ms timeout on write is graceful | ☐ | |
| 6 | `sttDataMutex` | STT audio task fills buffer while network task sends previous chunk | WAV data handed off cleanly, no double-free | ☐ | |
| 7 | `networkQueue` overflow | Trigger multiple network requests at once (sensor + OLED + image) | Queue depth 8 handles burst; excess dropped with no crash | ☐ | |
| 8 | I2C bus contention | Camera SCCB + MPU6050 I2C on same Wire? | Camera uses SIOD/SIOC pins (separate from SDA=5/SCL=6), no conflict | ☐ | |
| 9 | OTA task suspend | Start OTA → all 3 Core 0 tasks suspended | Audio/Camera/OLED stop cleanly, resume on failure | ☐ | |

---

## 6. POWER STATE TESTING

| # | State | Trigger | Expected CPU/Behavior | How to Verify | Pass/Fail | Notes |
|---|-------|---------|----------------------|---------------|-----------|-------|
| 1 | Normal idle | Device on desk, MAIN screen | 80 MHz, WiFi light-sleep ON, mic slow-poll if silent | Serial: "⚡ CPU idle at 80MHz" | ☐ | |
| 2 | Network active | Sensor data sending | 160 MHz boost, returns to 80 after send | Serial: safeCpuFreq calls | ☐ | |
| 3 | Camera capture | Feeding gesture triggers | 240 MHz boost, returns to 80 after capture | Serial: "⚡ CPU: 240MHz (capturing)" then "80MHz" | ☐ | |
| 4 | OTA download | OTA update in progress | 240 MHz sustained during entire download | Serial: setCpuFrequencyMhz(240) at OTA start | ☐ | |
| 5 | Deep sleep entry | 2 min no motion | OLED blank, camera off, LED off, MPU in low-power cycle mode (gyro off, accel 5 Hz) | Serial confirms, measure current if possible: tens of µA | ☐ | |
| 6 | Deep sleep wake | Shake device | ext1 wakeup on GPIO2 LOW, full reboot, 240 MHz setup then 80 MHz idle | Serial: "Wake-up: EXT1" | ☐ | |
| 7 | Camera pins low-power | No hunger = camera off | All 14 camera GPIOs set to INPUT | Serial: "📷 Camera pins → INPUT (low-power)" | ☐ | |
| 8 | WiFi TX power | After WiFi connect | TX power reduced to 11 dBm (not max 19.5 dBm) | Serial: "TX 11dBm" | ☐ | |

---

## 7. EDGE CASE TESTING

| # | Scenario | How to Trigger | Expected Behavior | Symptom if Broken | Pass/Fail | Notes |
|---|----------|----------------|-------------------|-------------------|-----------|-------|
| 1 | No motion for 2 min | Set device on vibration-free surface | Deep sleep enters correctly | Device stays awake, battery drains | ☐ | |
| 2 | Sudden strong motion | Jerk device hard after idle | ISR fires, lastMotionTime resets, no sleep | Sleep enters despite motion | ☐ | |
| 3 | WiFi unavailable at boot | Power ON with no WiFi | After 3 retries per network → AP provisioning mode with QR code | Stuck on "Connecting" forever, no AP mode | ☐ | |
| 4 | WiFi drops mid-session | Disable router during operation | Reconnection attempts; after 10 fails → AP provisioning | Crash, infinite reconnect loop, or no AP fallback | ☐ | |
| 5 | Server down | Block server URL at router | HTTP errors increment, SSL resets after 3 failures, device continues locally | Crash, freeze, or loop restarts | ☐ | |
| 6 | MPU6050 not connected | Disconnect I2C wires | "❌ MPU6050 initialization failed after 5 seconds", device continues with dummy data | Hard crash or I2C bus lockup | ☐ | |
| 7 | OLED not connected | Disconnect OLED | "❌ OLED initialization failed!", displayReady=false, all display calls skipped | Crash trying to write to missing display | ☐ | |
| 8 | Camera capture fails | Cover lens + trigger feed | "❌ Core 0: Camera capture failed", feeding continues but no image sent | OLED stuck on eating animation forever | ☐ | |
| 9 | Feeding timeout | Trigger feed but camera buffer never fills (30s) | "⚠️ Feeding gesture timeout - resetting flags" | Stuck in feeding state, can't navigate menus | ☐ | |
| 10 | Heap critically low | Force many allocations (unlikely in normal use) | Auto-reboot when heap < 20 KB: "🚨 CRITICAL: Heap below 20KB" | Device crashes or becomes unresponsive | ☐ | |
| 11 | NVS full | Store maximum WiFi credentials (5) then add more | Oldest credential shifted out: "overwriting oldest" | NVS write fails, can't store credentials | ☐ | |
| 12 | Left tilt on wrong menu | Tilt left 3s on MAIN screen (not food/toilet/health) | No action — gesture guards check `screenTypeIs()` | Unintended action on wrong menu | ☐ | |
| 13 | STT on non-MAIN menu | Navigate to food menu, try STT tilt | STT gesture blocked: `if (!screenTypeIs("MAIN")) return` | STT activates on food menu, conflicts with feed gesture | ☐ | |
| 14 | INT pin stuck LOW | MPU6050 interrupt latched before sleep | Device attempts 20 retries to clear, prints "WARNING: INT pin stuck LOW" | Instant wake from deep sleep (never actually sleeps) | ☐ | |
| 15 | Gesture during walk | Walk with device, try right-tilt menu | Menu gesture blocked during walking (`petIsWalking` guard) | Menu changes randomly while walking | ☐ | |
| 16 | Gesture during sleep | Device in sleep animation on OLED | Feed/clean/medicine gestures blocked (`isDeviceSleeping` guard) | Actions triggered while "sleeping" | ☐ | |
| 17 | Double feed | On FOOD_MENU, complete feed, immediately tilt again | `imageAlreadySentThisSession` blocks second capture until leaving FOOD_MENU | Two images sent, double hunger reduction | ☐ | |
| 18 | OTA download stall | Simulate slow network during OTA | 120s watchdog timeout aborts download; 30s between retries (3 max) | Hang forever, watchdog reboot | ☐ | |
| 19 | Pet hunger overflow | Don't feed for many physio ticks | Hunger capped at 100 (`min(100, ...)`) | Hunger exceeds 100 or wraps negative | ☐ | |
| 20 | Age transition not on MAIN | Age-up triggers while on FOOD_MENU | `pendingAgeTransition` stays true until user returns to MAIN | Animation plays on wrong screen or never plays | ☐ | |

---

## 8. FAILURE DETECTION NOTES

| Symptom | Likely Cause | What to Check |
|---------|-------------|---------------|
| **Device reboots unexpectedly** | Watchdog timer (TG1WDT_SYS_RST) — blocking call on Core 0 | Look for `delay()` instead of `vTaskDelay()` in OLED/audio/camera tasks |
| **OLED shows garbled pixels** | Animation buffer overflow or wrong bitmap dimensions | Check frame dimensions match `#define` widths/heights in all_pets.h |
| **Pet stuck in one emotion** | `isServerEmotionOverride` stuck true, or server returns same emotion | Check if server returns "LOCAL" emotion to release override |
| **Camera captures but image not sent** | `cameraMutex` deadlock or `cameraImageReady` never set | Check serial for "📦 Core 0: Image buffered" vs "⚠️ No image data" |
| **Menu won't cycle** | MPU6050 not reading properly, or tilt threshold not reached | Check serial for "📱 Menu tilt right started..." — if missing, MPU may be stuck |
| **Deep sleep never triggers** | Motion interrupt firing constantly (vibration, electrical noise) | Check serial ISR count — if constantly incrementing, MPU sensitivity too high |
| **Deep sleep wakes immediately** | INT pin stuck LOW from previous ISR | Serial: "WARNING: INT pin stuck LOW — wake may be instant" |
| **WiFi reconnect infinite loop** | Wrong credentials, out-of-range, or DNS failure | Check serial for which SSID it's trying; `wifiReconnectFails` counter progress |
| **HTTP -1 errors** | SSL session stale, server cold start (Cloud Run) | Serial: "⚠️ HTTP error -1" → after 3, auto SSL reset should fix |
| **Eating animation stuck** | `isUploadingImage` never cleared (image upload failed or timed out) | Check for "⚠️ Feeding gesture timeout" after 30s |
| **Steps counted while stationary** | Step detection threshold too low | If `stoss` values printed are small but triggering, increase `STEP_BARRIER_G2` |
| **No steps while walking** | Step detection threshold too high | If no "👣 HW Step" in serial while walking, decrease `STEP_BARRIER_G2` |
| **Music / loud sounds trigger speech** | VAD threshold too low | If "🎤 Speech detected" fires from ambient noise, increase `VAD_THRESHOLD` |
| **High chip temperature** | Camera running continuously, CPU at 240 MHz too long, or mic not entering sleep | Check `chip_temperature` in serial; camera should deinit when not hungry |
| **"Guru Meditation Error"** | Stack overflow or null pointer | Check task stack sizes; look for which task crashed in the backtrace |
| **Heap keeps decreasing** | Memory leak — likely from DynamicJsonDocument or un-freed buffers | Monitor "🧠 Heap: N free / N min" over time — should stabilize |

---

## 9. DEBUG OBSERVATIONS SECTION

### Test Session Log

**Date:** ___________  
**Firmware Version:** ___________  
**WiFi Network:** ___________  
**Serial Monitor Baud:** 115200

---

#### Boot Sequence

| Checkpoint | Result | Notes |
|------------|--------|-------|
| Serial output starts | ☐ Pass ☐ Fail | |
| I2C scan finds devices | ☐ Pass ☐ Fail | |
| OLED initialized | ☐ Pass ☐ Fail | |
| MPU6050 initialized | ☐ Pass ☐ Fail | |
| WiFi connected | ☐ Pass ☐ Fail | |
| Mic initialized | ☐ Pass ☐ Fail | |
| All tasks started | ☐ Pass ☐ Fail | |
| Startup animation played | ☐ Pass ☐ Fail | |
| CPU dropped to 80 MHz | ☐ Pass ☐ Fail | |

---

#### Gesture Testing

| Gesture | Menu | Result | Notes |
|---------|------|--------|-------|
| Right tilt 2s | MAIN → | ☐ Pass ☐ Fail | |
| Right tilt 2s | FOOD → | ☐ Pass ☐ Fail | |
| Right tilt 2s | TOILET → | ☐ Pass ☐ Fail | |
| Right tilt 2s | PLAY → | ☐ Pass ☐ Fail | |
| Right tilt 2s | HEALTH → | ☐ Pass ☐ Fail | |
| Right tilt 2s | STATUS → | ☐ Pass ☐ Fail | |
| Right tilt 2s | STATS → | ☐ Pass ☐ Fail | |
| Left tilt 3s | FOOD_MENU (hungry) | ☐ Pass ☐ Fail | |
| Left tilt 3s | TOILET_MENU (poop) | ☐ Pass ☐ Fail | |
| Left tilt 3s | HEALTH_MENU (sick) | ☐ Pass ☐ Fail | |
| Left tilt 3s | PLAY_MENU (start game) | ☐ Pass ☐ Fail | |
| Left tilt 10s | MAIN (STT mode) | ☐ Pass ☐ Fail | |

---

#### Animation Verification

| Animation | Triggered By | Frames | Result | Notes |
|-----------|-------------|--------|--------|-------|
| Egg cracking | First boot | 4 | ☐ Pass ☐ Fail | |
| Infant idle | petAge=INFANT, IDLE | 2 | ☐ Pass ☐ Fail | |
| Child idle | petAge=CHILD, IDLE | 3 | ☐ Pass ☐ Fail | |
| Adult idle | petAge=ADULT, IDLE | 2 | ☐ Pass ☐ Fail | |
| Infant crying | Hungry INFANT | varies | ☐ Pass ☐ Fail | |
| Child crying | Hungry CHILD | varies | ☐ Pass ☐ Fail | |
| Adult crying | Hungry ADULT | varies | ☐ Pass ☐ Fail | |
| Old crying | Hungry OLD | varies | ☐ Pass ☐ Fail | |
| Happy (all ages) | Happiness > 80 | varies | ☐ Pass ☐ Fail | |
| Sad (all ages) | Happiness < 40 | varies | ☐ Pass ☐ Fail | |
| Angry (all ages) | Poop present | varies | ☐ Pass ☐ Fail | |
| Surprise (all ages) | Server override | varies | ☐ Pass ☐ Fail | |
| Eating | Feed gesture | 5 | ☐ Pass ☐ Fail | |
| Injection | Medicine gesture | 34 × 3 | ☐ Pass ☐ Fail | |
| Sleeping | Deep sleep pending | 2 | ☐ Pass ☐ Fail | |
| Walking | Steps detected | 6 (skip 2) | ☐ Pass ☐ Fail | |
| Age transition | Birthday | multi-phase | ☐ Pass ☐ Fail | |
| Clean slide | Cleaning | 2 alternating | ☐ Pass ☐ Fail | |

---

#### Network Health

| Check | Value | Status |
|-------|-------|--------|
| WiFi RSSI | _____ dBm | ☐ Good ☐ Weak |
| Sensor send success rate | _____/10 | |
| OLED poll success rate | _____/10 | |
| HTTP consecutive errors | _____ | |
| SSL resets triggered | _____ | |
| Free heap after 1 hour | _____ bytes | |
| Min heap after 1 hour | _____ bytes | |

---

#### Physiology Engine

| Stat | After Boot | After 1 hr | After 2 hr | Notes |
|------|-----------|------------|------------|-------|
| Hunger | | | | Should increase over time |
| Health | | | | Decreases with neglect |
| Energy | | | | Slowly decreases |
| Happiness | | | | Changes with interactions |
| Discipline | | | | Changes with neglect/care |
| XP | | | | Always increases |
| Level | | | | Increments on birthday |
| Age (days) | | | | Increments every 86400s uptime |
| isSick | | | | Random chance when health ≤25 |
| hasPoop | | | | 5% per tick when h < 50 |

---

#### Thermal Monitoring

| Component | Cold Start | After 30 min | After 2 hr | Notes |
|-----------|-----------|-------------|------------|-------|
| ESP32 chip temp | ___°C | ___°C | ___°C | Normal: 40-65°C |
| Camera module | Cool / Warm / Hot | | | Should be cool when not hungry |
| OLED | Cool / Warm / Hot | | | |
| Overall enclosure | Cool / Warm / Hot | | | |

---

#### Free-Form Notes

```
Write any additional observations here:




```

---

## QUICK REFERENCE: KEY SERIAL MESSAGES

| Message | Meaning |
|---------|---------|
| `✅ OLED initialized` | Display working |
| `✅ MPU6050 initialized successfully!` | Accelerometer working |
| `📷 Camera OFF at boot` | Camera idle (correct) |
| `✅ PDM Microphone initialized` | Mic working |
| `📺 Current Screen: MAIN` | Normal operation heartbeat |
| `🧠 Heap: N free / N min` | Memory health (watch min) |
| `👣 HW Step #N` | Walking detected |
| `😴 No motion for 2 min` | Entering deep sleep |
| `Wake-up: EXT1` | Woke from deep sleep via motion |
| `💓 Physiology Tick` | Pet stats engine running (every 360s) |
| `🔄 Resetting SSL connection` | Auto-recovery from HTTP errors |
| `🚨 CRITICAL: Heap below 20KB` | Emergency reboot imminent |

---

*End of Testing Checklist — KAKU Tamagotchi v1.0.5*
