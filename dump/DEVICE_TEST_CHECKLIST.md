# 🎮 KAKU Device — Full Test Checklist (Infant → Old)

## ⚡ SPEED-UP VARIABLES (modify in `esp32_sketch.ino` before upload)

### 🔧 Master Test Controls (top of file)
| Variable | Line | Default | Test Value | What it does |
|---|---|---|---|---|
| `FORCE_EGG_HATCH` | 5 | `false` | `true` | Replays egg hatch animation on every boot |
| `PHYSIO_TICK_MS` | 349 | `360000` (6 min) | `30000` (30s) | How often hunger/health/aging ticks run |
| `totalUptimeSecs += 120` | 3538 | `+= 120` | `+= 7200` | Each physio tick adds this many "seconds" to age (7200 = 2 hours per tick) |
| `86400` (age divisor) | 3539 | `86400` (24h) | `86400` | Days = totalUptimeSecs / this. Keep at 86400, just speed up uptime increment |

### 🧬 Age Thresholds (line ~471-477)
| Age Stage | Day Range | Animation Set |
|---|---|---|
| INFANT | 0–5 days | `infant_idle`, `infant_happy`, `infant_sad`, `infant_angry`, `infant_surprise`, `infant_cry` |
| CHILD | 6–10 days | `child_idle`, `child_happy`, `child_sad`, `child_angry`, `child_surprise` |
| ADULT | 11–17 days | `adult_idle`, `adult_happy`, `adult_sad`, `adult_angry`, `adult_surprise` |
| OLD | 18+ days | `old_idle`, `old_happy`, `old_sad`, `old_angry`, `old_surprise` |

### ⏱️ Quick-Age Recipe
To go from Infant → Old in ~10 minutes:
```cpp
// Line 349: Speed up tick rate
#define PHYSIO_TICK_MS 10000    // Every 10 seconds instead of 6 minutes

// Line 3538: Each tick = 1 day of aging
g_petState.totalUptimeSecs += 86400;  // +1 day per tick instead of +120s
```
This gives you: **New age stage every ~50-60 seconds** (5 days = 5 ticks = 50s for INFANT→CHILD, etc.)

### 🔄 Force Specific Age (for targeted testing)
After `loadPetState()` in `setup()` (around line 1559), add:
```cpp
g_petState.ageInt = 18;          // Force OLD stage
g_petState.totalUptimeSecs = 18 * 86400;  // Match uptime
```

---

## 📋 TEST PHASES

---

### PHASE 0: Boot & Egg Animation
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 0.1 | Set `FORCE_EGG_HATCH true` | Upload & boot | Egg crack → hatching → infant appears | ☐ |
| 0.2 | Set `FORCE_EGG_HATCH false`, reboot | Power cycle | Skips egg, goes straight to MAIN | ☐ |
| 0.3 | WiFi connects | Auto on boot | Serial shows "✅ WiFi Connected" + IP | ☐ |
| 0.4 | Server startup notification | Auto after egg | Serial shows "✅ Server acknowledged startup" | ☐ |

---

### PHASE 1: INFANT Stage (Days 0–5)
#### 1A. Main Screen
| # | Test | Expected | ✅ |
|---|---|---|---|
| 1.1 | OLED shows infant idle animation | Infant sprite animating on 64×32 | ☐ |
| 1.2 | Home icon visible (top-left) | Small house icon | ☐ |
| 1.3 | No status icons initially | No food/poop/sick/play icons | ☐ |

#### 1B. Menu Navigation (Right tilt + hold 2s)
| # | Test | Gesture | Expected Screen | ✅ |
|---|---|---|---|---|
| 1.4 | MAIN → FOOD_MENU | Tilt RIGHT, hold 2s | Food icon + "FEED" text | ☐ |
| 1.5 | FOOD → TOILET | Tilt RIGHT, hold 2s | Toilet icon + poop status | ☐ |
| 1.6 | TOILET → PLAY | Tilt RIGHT, hold 2s | Play icon + "PLAY" text | ☐ |
| 1.7 | PLAY → HEALTH | Tilt RIGHT, hold 2s | Heart icon + "HEALTH" / "All Good" | ☐ |
| 1.8 | HEALTH → STATUS | Tilt RIGHT, hold 2s | Status info screen | ☐ |
| 1.9 | STATUS → STATS | Tilt RIGHT, hold 2s | Stats screen (age, level, etc.) | ☐ |
| 1.10 | STATS → MAIN | Tilt RIGHT, hold 2s | Back to main animation | ☐ |

#### 1C. Feeding (FOOD_MENU)
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 1.11 | Wait for hunger icon | Wait for physio tick (hunger > 70) | Food icon appears on MAIN | ☐ |
| 1.12 | Navigate to FOOD_MENU | Right tilt to FOOD | Shows "FEED" or "Not Hungry" | ☐ |
| 1.13 | Trigger feeding | Tilt LEFT, hold 3s | Camera captures → eating animation → "GOOD!" | ☐ |
| 1.14 | Hunger resets | After feeding | Food icon disappears, Serial: "🍽️ Feeding complete" | ☐ |
| 1.15 | Infant cry when hungry | Emotion = CRY for infants | Cry animation on MAIN screen | ☐ |

#### 1D. Poop & Cleaning (TOILET_MENU)
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 1.16 | Wait for poop | Physio tick (5% chance when hunger < 50) | 💩 icon appears on MAIN | ☐ |
| 1.17 | POOP shows ANGRY animation | On MAIN screen with poop icon | Angry infant animation | ☐ |
| 1.18 | Navigate to TOILET | Right tilt to TOILET | Shows poop present status | ☐ |
| 1.19 | Clean poop | Tilt LEFT, hold 3s | Cleaning slide animation → poop cleared | ☐ |
| 1.20 | Poop icon gone | After cleaning | No poop icon, Serial: "cleaning request sent" | ☐ |

#### 1E. Sickness & Medicine (HEALTH_MENU)
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 1.21 | Wait for sickness | Neglect pet (low health/poop) | Heart icon appears, SICK animation (SAD) | ☐ |
| 1.22 | Navigate to HEALTH | Right tilt to HEALTH | Shows "Give Med" text | ☐ |
| 1.23 | Give medicine | Tilt LEFT, hold 3s | Injection animation → sick cured | ☐ |
| 1.24 | Healthy state | After medicine | Shows "All Good" on HEALTH_MENU | ☐ |
| 1.25 | Force sickness for testing | In code: `g_petState.isSick = true;` after `loadPetState()` | | ☐ |

#### 1F. Play Menu (PLAY_MENU)
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 1.26 | Play screen | Navigate to PLAY | Shows play icon + "PLAY" | ☐ |
| 1.27 | Start game | Tilt LEFT, hold 3s | "HOLDING..." → "GET READY!" (2s) → game starts | ☐ |
| 1.28 | Catch Food game | Forward/back tilt | Player moves, food falls, catch for points | ☐ |
| 1.29 | Dodge game | Forward/back tilt | Player dodges obstacles, score increases | ☐ |
| 1.30 | Game over (catch) | Miss 3 foods | Coin animation → score → KC reward → back to PLAY | ☐ |
| 1.31 | Game over (dodge) | Hit obstacle | Explosion → "CRASH!" → score → back to PLAY | ☐ |
| 1.32 | **No restart on game over** | After game ends | Device stays on PLAY menu, does NOT reboot | ☐ |

#### 1G. Sleep Mode
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 1.33 | Flip device face-down | Hold 30 seconds | "😴 SLEEP MODE" in Serial, sleeping animation | ☐ |
| 1.34 | Wake up | Flip right-side-up | "⏰ Woke up" in Serial, back to normal | ☐ |
| 1.35 | Network paused during sleep | Check Serial | No HTTP calls while sleeping | ☐ |

#### 1H. Walking Detection
| # | Test | Gesture | Expected | ✅ |
|---|---|---|---|---|
| 1.36 | Walk with device | Walk normally | Walking animation plays, icons hidden | ☐ |
| 1.37 | Stop walking | Stand still 3s | Back to normal animation + icons | ☐ |

---

### PHASE 2: CHILD Stage (Days 6–10)
*Speed up: change `g_petState.ageInt = 6` after `loadPetState()`*

| # | Test | Expected | ✅ |
|---|---|---|---|
| 2.1 | Animation switches to CHILD | Child sprites (idle, happy, sad, angry, surprise) | ☐ |
| 2.2 | All menu gestures work | Same as Phase 1 but with CHILD animations | ☐ |
| 2.3 | Hunger shows SAD (not CRY) | Only infants cry; child shows SAD when hungry | ☐ |
| 2.4 | Feeding works | Same gesture → camera → eating → "GOOD!" | ☐ |
| 2.5 | Poop / clean works | Same gesture → cleaning animation | ☐ |
| 2.6 | Sickness / medicine works | Same gesture → injection animation | ☐ |
| 2.7 | Play games work | Both catch food and dodge work | ☐ |

---

### PHASE 3: ADULT Stage (Days 11–17)
*Speed up: change `g_petState.ageInt = 11` after `loadPetState()`*

| # | Test | Expected | ✅ |
|---|---|---|---|
| 3.1 | Animation switches to ADULT | Adult sprites | ☐ |
| 3.2 | All interactions work | Feed, clean, medicine, play | ☐ |
| 3.3 | Hunger decay slower (8/tick) | Adult default hungerDecay = 8 | ☐ |
| 3.4 | Stats show correct age | STATUS/STATS menu shows day 11+ | ☐ |

---

### PHASE 4: OLD Stage (Days 18+)
*Speed up: change `g_petState.ageInt = 18` after `loadPetState()`*

| # | Test | Expected | ✅ |
|---|---|---|---|
| 4.1 | Animation switches to OLD | Old sprites | ☐ |
| 4.2 | All interactions work | Feed, clean, medicine, play | ☐ |
| 4.3 | Higher hunger decay (12/tick) | Old pets get hungrier faster | ☐ |
| 4.4 | Spontaneous sickness | 5% chance per tick (old age) | ☐ |
| 4.5 | Play icon still works | Navigate to PLAY, start game | ☐ |

---

### PHASE 5: Network & Reliability
| # | Test | How to trigger | Expected | ✅ |
|---|---|---|---|---|
| 5.1 | WiFi disconnect recovery | Turn off router, wait 10s, turn on | Auto-reconnects, Serial shows "✅ WiFi Reconnected" | ☐ |
| 5.2 | Server unreachable | Stop server | HTTP errors logged, SSL resets after 3 failures | ☐ |
| 5.3 | OTA update | Push firmware via dashboard | Downloads → flashes → reboots → new version | ☐ |
| 5.4 | OTA retry on failure | Kill connection mid-download | Retries 3× with 30s delay | ☐ |
| 5.5 | Heap monitoring | Serial log every 10s | "🧠 Heap: XXXXX free / XXXXX min" | ☐ |
| 5.6 | Daily reboot | Run >24h in sleep | Auto-reboots, state preserved | ☐ |
| 5.7 | Low-heap reboot | Forced memory leak test | Reboots below 20KB, state preserved | ☐ |

---

### PHASE 6: Dashboard Sync
| # | Test | Expected | ✅ |
|---|---|---|---|
| 6.1 | Sensor data appears | Dashboard shows accel, gyro, temp, mic | ☐ |
| 6.2 | Pet state syncs | Dashboard shows hunger, health, age, level | ☐ |
| 6.3 | Image upload shows | Feed pet → image appears on dashboard | ☐ |
| 6.4 | Game reward logged | Play game → KC balance updates on dashboard | ☐ |
| 6.5 | Emotion correct | Dashboard emotion matches OLED animation | ☐ |

---

## 🔧 FORCE-STATE CHEAT SHEET

Add these lines **after `loadPetState()`** (around line 1559) to force any state:

```cpp
// ===== TESTING OVERRIDES (remove before production!) =====

// Force specific age stage
g_petState.ageInt = 0;    // 0-5=INFANT, 6-10=CHILD, 11-17=ADULT, 18+=OLD
g_petState.totalUptimeSecs = g_petState.ageInt * 86400;

// Force sickness
g_petState.isSick = true;

// Force poop
g_petState.hasPoop = true;

// Force hunger (0=full, 100=starving)
g_petState.hunger = 80;  // Will trigger food icon

// Force low health
g_petState.health = 30;  // Will trigger sickness chance

// Force full stats (healthy happy pet)
g_petState.hunger = 0;
g_petState.health = 100;
g_petState.happiness = 100;
g_petState.energy = 100;
g_petState.isSick = false;
g_petState.hasPoop = false;
```

## ⏩ SPEED-UP RECIPE (copy-paste to test all ages in ~10 min)

```cpp
// Line 349 — change physio tick to every 10 seconds
#define PHYSIO_TICK_MS 10000

// Line 3538 — each tick = 1 full day
g_petState.totalUptimeSecs += 86400;  // was += 120
```

**Timeline with these settings:**
- 0:00 → Boot, egg hatches (if FORCE_EGG_HATCH true)
- 0:50 → INFANT → CHILD (day 6)
- 1:40 → CHILD → ADULT (day 11)  
- 2:50 → ADULT → OLD (day 18)
- Total: **~3 minutes** to cycle through all stages

---

## 🎯 EMOTION → ANIMATION MAPPING REFERENCE

| Emotion | Animation Used | Icon | Trigger |
|---|---|---|---|
| IDLE | `{age}_idle` | Home | Default happy state |
| HAPPY | `{age}_happy` | Home | Happiness > 80 |
| SAD | `{age}_sad` | Home | Happiness < 40 |
| CRY | `infant_cry` | Food | Infant + hungry |
| HUNGER | `{age}_sad` | Food | Non-infant + hungry |
| SICK | `{age}_sad` + ❤️ icon | Heart | `isSick = true` |
| POOP | `{age}_angry` + 💩 icon | Poop | `hasPoop = true` |
| SURPRISE | `{age}_surprise` | — | Server sensory event |

## 📌 ICON PRIORITY (only one status icon at a time)
**SICK > POOP > HUNGER > PLAY** (left to right = highest to lowest priority)
