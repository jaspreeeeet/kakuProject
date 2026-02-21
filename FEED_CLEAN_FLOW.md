# 🍕 Feed & Clean Pet Flow Documentation

## ❌ Current Error: "Failed to feed pet"

### Root Cause Analysis:
The error occurs because:
1. **Backend requires pet to exist in database** (`get_pet_state()` returns `None` → 404 error)
2. **Pet state might not be initialized** in the SQLite `pet_state` table
3. **Device ID mismatch** between frontend and database

---

## 🔄 Complete Flow Explanation

### 1️⃣ **FRONTEND (index.html)** - User Click

#### Feed Pet Button (Line 424-425)
```html
<button onclick="feedPet()">🍕 Feed Pet</button>
```

#### Feed Function (Line 710-732)
```javascript
function feedPet() {
    fetch(API_BASE + '/api/pet/feed', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device_id: 'ESP32_001' })
    })
    .then(response => response.json())
    .then(data => {
        if (data.status === 'success') {
            alert('🍕 Pet is eating! Hunger: ' + data.hunger + '%');
        } else {
            alert('❌ Error: ' + data.message);  // <-- Shows "Pet not found"
        }
    });
}
```

#### Clean Function (Line 734-756)
```javascript
function cleanPet() {
    fetch(API_BASE + '/api/pet/clean', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device_id: 'ESP32_001' })
    })
    .then(response => response.json())
    .then(data => {
        if (data.status === 'success') {
            alert('🧹 Pet area cleaned! Cleanliness: ' + data.cleanliness + '%');
        } else {
            alert('❌ Error: ' + data.message);  // <-- Error appears here
        }
    });
}
```

**What Frontend Does:**
- Sends POST request to backend with `device_id: 'ESP32_001'`
- Waits for JSON response
- Shows alert with result or error message

---

### 2️⃣ **BACKEND (app.py)** - API Processing

#### Feed Endpoint (Line 1836-1879)
```python
@app.route('/api/pet/feed', methods=['POST'])
def pet_feed():
    device_id = data.get('device_id', 'ESP32_001')
    
    # ❌ PROBLEM: This returns None if pet doesn't exist
    state = get_pet_state(device_id)
    if not state:
        return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
    
    # Set action lock
    updates = {'action_lock': 1}
    update_pet_state_atomic(device_id, updates)
    
    # Feed logic
    updates = {
        'hunger': max(0, state['hunger'] - 40),  # Reduce hunger by 40%
        'last_feed_time': datetime.now().isoformat(),
        'digestion_due_time': (datetime.now() + timedelta(minutes=30)).isoformat(),
        'current_emotion': 'EATING',  # Change emotion to EATING
        'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
        'action_lock': 0
    }
    
    result = update_pet_state_atomic(device_id, updates)
    
    return jsonify({
        'status': 'success',
        'hunger': result['hunger'],
        'emotion': result['current_emotion'],
        'digestion_due': result['digestion_due_time']
    })
```

**What Backend Does:**
1. Gets `device_id` from request (ESP32_001)
2. **Queries database** for pet state (`SELECT * FROM pet_state WHERE device_id = 'ESP32_001'`)
3. If **pet doesn't exist** → Returns 404 error ❌
4. If pet exists:
   - Sets `action_lock = 1` (prevents simultaneous actions)
   - Reduces hunger by 40%
   - Sets emotion to `EATING` for 3 seconds
   - Schedules digestion (poop) after 30 minutes
   - Releases action lock
5. Returns success with new stats

#### Clean Endpoint (Line 1883-1930)
```python
@app.route('/api/pet/clean', methods=['POST'])
def pet_clean():
    device_id = data.get('device_id', 'ESP32_001')
    
    state = get_pet_state(device_id)
    if not state:
        return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
    
    if not state['poop_present']:
        return jsonify({'status': 'success', 'message': 'Already clean'})
    
    updates = {
        'poop_present': 0,  # Remove poop
        'poop_timestamp': None,
        'cleanliness': 100,  # Restore to 100%
        'health': min(100, state['health'] + 5),  # Boost health +5%
        'last_clean_time': datetime.now().isoformat(),
        'current_emotion': 'CLEAN_SUCCESS',
        'emotion_expire_at': (datetime.now() + timedelta(seconds=3)).isoformat(),
        'action_lock': 0
    }
    
    result = update_pet_state_atomic(device_id, updates)
    
    return jsonify({
        'status': 'success',
        'cleanliness': result['cleanliness'],
        'health': result['health']
    })
```

**What Clean Does:**
1. Checks if pet exists ❌
2. Checks if poop is present
3. If yes:
   - Removes poop (`poop_present = 0`)
   - Restores cleanliness to 100%
   - Boosts health by 5%
   - Sets emotion to `CLEAN_SUCCESS` for 3 seconds
4. Returns success

---

### 3️⃣ **ESP32 HARDWARE (esp32_sketch.ino)** - Display Update

#### Polling Server (Every 5 seconds)
```cpp
void getOLEDDisplayFromServer() {
    HTTPClient http;
    http.begin(oledDisplayUrl);  // GET /api/oled-display/get
    
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        String response = http.getString();
        deserializeJson(g_oledDoc, response);
        
        // Update emotion
        if (g_oledDoc.containsKey("current_emotion")) {
            String emotion = g_oledDoc["current_emotion"].as<String>();
            currentEmotion = emotion;  // Store: EATING, CLEAN_SUCCESS, etc.
            
            // Trigger eating animation
            if (emotion == "EATING" && currentScreenType == "FOOD_MENU") {
                playEatingAnimation();  // Show full-screen eating animation
                justFinishedEating = true;
                eatingFinishTime = millis();
            }
        }
        
        // Update pet stage
        if (g_oledDoc.containsKey("animation_id")) {
            int newAnimationId = g_oledDoc["animation_id"];
            petAge = (PetAge)newAnimationId;  // INFANT/CHILD/ADULT/OLD
        }
        
        // Update icons
        showFoodIcon = g_oledDoc["show_food_icon"];
        showPoopIcon = g_oledDoc["show_poop_icon"];
        petIsHungry = g_oledDoc["is_hungry"];
    }
}
```

#### Display Animation (30 FPS on Core 0)
```cpp
void displayPetAnimation() {
    // Called every 33ms by oledTask()
    
    if (currentEmotion == "EATING") {
        // Show eating animation (if on FOOD_MENU screen)
        playEatingAnimation();
    }
    else if (currentEmotion == "CLEAN_SUCCESS") {
        // Show happy/clean animation
        // (Currently shows regular pet with updated stats)
    }
    else {
        // Show normal pet animation based on petAge (INFANT/CHILD/ADULT/OLD)
        display.drawBitmap(0, 0, infant_frames[currentFrame], 64, 64, SSD1306_WHITE);
    }
    
    // Draw icons
    if (showFoodIcon) drawFoodIcon();
    if (showPoopIcon) drawPoopIcon();
}
```

#### Eating Animation Playback (Line 680-695)
```cpp
void playEatingAnimation() {
    Serial.println("😋 Playing eating animation...");
    
    for (uint8_t frame = 0; frame < EATING_FRAME_COUNT; frame++) {
        display.clearDisplay();
        display.drawBitmap(0, 0, eating_frames[frame], EATING_WIDTH, EATING_HEIGHT, SSD1306_WHITE);
        display.display();
        vTaskDelay(pdMS_TO_TICKS(eating_delays[frame]));  // 100ms per frame
    }
    
    Serial.println("✅ Eating animation complete!");
}
```

**What ESP32 Does:**
1. **Polls backend** every 5 seconds: `GET /api/oled-display/get`
2. Receives pet state including:
   - `current_emotion`: "EATING", "CLEAN_SUCCESS", "IDLE", "CRY", etc.
   - `animation_id`: 0=INFANT, 1=CHILD, 2=ADULT, 3=OLD
   - `show_food_icon`: true/false
   - `show_poop_icon`: true/false
   - `hunger`, `health`, `cleanliness`, `happiness`, `energy`
3. **Updates OLED display** (30 FPS):
   - If emotion = "EATING" → plays eating animation (5 frames, 100ms each)
   - If emotion = "CLEAN_SUCCESS" → shows happy pet
   - Otherwise → shows normal pet animation based on stage
4. **Shows status icons**:
   - Food icon (🍽️) if hungry
   - Poop icon (💩) if poop present
   - Home icon (🏠) if on main screen

---

## 🔧 **FIX THE ERROR**

### Problem: Pet Not Found in Database

#### Solution 1: Check if Pet Exists
Run this in PowerShell:
```powershell
python debug_db.py
```

Look for output:
```
Pet State for ESP32_001:
- Stage: INFANT
- Health: 100%
- Hunger: 50%
```

If you see **"No pet found"**, the pet doesn't exist!

#### Solution 2: Initialize Pet State
Add this fix to backend:

**File:** `app.py` (Line 1836)

**OLD CODE:**
```python
@app.route('/api/pet/feed', methods=['POST'])
def pet_feed():
    state = get_pet_state(device_id)
    if not state:
        return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
```

**NEW CODE:**
```python
@app.route('/api/pet/feed', methods=['POST'])
def pet_feed():
    state = get_pet_state(device_id)
    if not state:
        # Auto-create pet if doesn't exist
        initialize_default_pet(device_id)
        state = get_pet_state(device_id)
        
        if not state:
            return jsonify({'status': 'error', 'message': 'Pet not found'}), 404
```

Add helper function:
```python
def initialize_default_pet(device_id='ESP32_001'):
    """Create default pet state if not exists"""
    with db_lock:
        conn = get_db_connection()
        cursor = conn.cursor()
        cursor.execute('''
            INSERT OR IGNORE INTO pet_state (device_id, stage, health, hunger, cleanliness, happiness, energy)
            VALUES (?, 'INFANT', 100, 50, 100, 100, 100)
        ''', (device_id,))
        conn.commit()
        conn.close()
        print(f"✅ Initialized pet for {device_id}")
```

---

## 📊 **Timeline After Feed Button Click**

| Time | Component | Action |
|------|-----------|--------|
| 0ms | **Frontend** | User clicks "Feed Pet" → POST /api/pet/feed |
| 50ms | **Backend** | Receives request, queries database |
| 55ms | **Backend** | Sets `action_lock=1`, updates `current_emotion='EATING'` |
| 60ms | **Backend** | Reduces hunger by 40% (100% → 60%) |
| 65ms | **Backend** | Schedules digestion for 30 minutes later |
| 70ms | **Backend** | Returns success JSON to frontend |
| 75ms | **Frontend** | Shows alert: "🍕 Pet is eating! Hunger: 60%" |
| **5000ms** | **ESP32** | Next poll: GET /api/oled-display/get |
| 5050ms | **ESP32** | Receives `current_emotion: "EATING"` |
| 5055ms | **ESP32** | Triggers `playEatingAnimation()` |
| 5055-5555ms | **OLED** | Shows eating animation (5 frames × 100ms) |
| **8000ms** | **Backend** | Emotion expires (3s timeout), resets to "IDLE" |
| **10000ms** | **ESP32** | Next poll: receives `current_emotion: "IDLE"` |
| 10010ms | **OLED** | Returns to normal pet animation |
| **30min** | **Backend** | Digestion timer fires → creates poop |
| **30min+5s** | **ESP32** | Polls, receives `poop_present: true` |
| **30min+5s** | **OLED** | Shows poop icon (💩) next to pet |

---

## 🐛 **Why Error Happens**

1. **Database empty** - Pet state row doesn't exist for `ESP32_001`
2. **Device ID mismatch** - Frontend sends wrong device_id
3. **Database locked** - SQLite busy/corrupted
4. **Backend not deployed** - Old version without pet_state table

---

## ✅ **Verify Fix Works**

### Test Feed:
1. Open frontend: https://kakuproject.vercel.app
2. Click "🍕 Feed Pet"
3. **Expected:** Alert shows "Pet is eating! Hunger: X%"
4. Wait 5 seconds
5. **Expected:** ESP32 OLED shows eating animation

### Test Clean:
1. Wait for pet to poop (30 min after feeding) OR manually set in database
2. Click "🧹 Clean Pet"
3. **Expected:** Alert shows "Pet area cleaned! Cleanliness: 100%"
4. **Expected:** Poop icon disappears from OLED

---

## 🎯 **Summary**

**Feed Pet:**
- Frontend → Backend: Reduce hunger, set emotion EATING
- Backend → Database: Update pet_state table
- ESP32 polls → Gets EATING emotion
- OLED displays → Eating animation (5 frames)
- After 3s → Returns to normal
- After 30min → Creates poop

**Clean Pet:**
- Frontend → Backend: Remove poop, restore cleanliness
- Backend → Database: Set poop_present=0, cleanliness=100
- ESP32 polls → Gets poop removed
- OLED displays → Poop icon disappears

**Hardware Actions:**
- ESP32 doesn't detect button clicks
- ESP32 only **displays** what backend tells it
- All logic happens in **backend database**
- ESP32 polls every 5 seconds for updates
