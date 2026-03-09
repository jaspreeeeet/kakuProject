# Kaku Pet System Architecture & Technical Documentation

This document explains the Kaku Pet project in simple terms so any developer can easily understand how the hardware, backend server, and frontend dashboard interact.

## 1. System Overview

Kaku is a “Tamagotchi-style” digital pet built on an **ESP32 microcontroller** with a **Python Flask backend** and a **Web Dashboard**.

The system has three main parts:
1. **ESP32 Firmware:** The brain of the physical device. It reads movement (MPU6050), takes pictures (OV2640), listens to audio (PDM Mic), and displays the pet animation on a small screen (OLED).
2. **Backend Server (Flask + SQLite):** The central hub. It receives all the data from the ESP32, stores it in a SQLite database, forwards it to the frontend via WebSockets, and handles AI services (like Image Captioning and Speech-to-Text).
3. **Frontend Dashboard (HTML/JS + Vercel):** The user interface. It lets you monitor the pet's stats (health, hunger), see live sensor metrics, and manually trigger firmware (OTA) updates.

---

## 2. ESP32 Firmware Structure (`esp32_sketch.ino`)

The firmware runs on an ESP32 (specifically the XIAO ESP32S3 Sense). Since this chip has two processor cores, the firmware is split to maximize performance:

### Dual-Core Architecture (FreeRTOS Tasks)
- **Core 0 (Audio/Camera Core):**
  - **`audioMonitorTask`:** Continuously listens to the microphone. It uses VAD (Voice Activity Detection) to wait for a loud sound. When you speak, it records a few seconds of audio and sends it to the server.
  - **`cameraMonitorTask`:** Occasionally takes a picture and places it in a buffer, or sends it straight to the server to feed the pet.
- **Core 1 (Main/Network Core):**
  - **`loop()` task:** Reads the MPU6050 accelerometer to detect gestures (like tilting left or right to switch menus, or shaking to "take a step"). Updates the pet's life stats, and draws the correct animation on the OLED.
  - **`networkTask`:** A background worker. Wait for other tasks to put items into a `networkQueue` (like "Send Sensor Data" or "Upload Image"). It pops these items off the queue and makes the HTTP requests to the server without freezing the animations.

### Physiology Engine & Pet State
The ESP32 is the **absolute authority** on the pet's life. 
- Variables like `hunger`, `health`, `happiness`, `energy`, and `age` live in the ESP32 memory. 
- **Time-based decay:** Every loop, it checks if enough time has passed to make the pet hungry or age it.
- **Sickness & Poop:** If hunger/health get too bad, or if "poop" is left uncleaned, the pet becomes SICK. The local firmware forces a SICK animation until the user heals it.

### Gesture Navigation & Inputs
Instead of buttons, the user tilts the device:
- **Tilt Forward/Back/Left/Right:** The MPU6050 reads gravity. Tilting right moves to the "Food" menu. Tilting right again triggers "Feed".
- **Shake (Step detection):** Walking with the device triggers the internal step counter.

---

## 3. Server Architecture (`app.py`)

The backend is built in **Python using Flask & Flask-SocketIO**, and uses a local **SQLite database**.

### Key Responsibilities:
1. **Data Ingestion:** Receives POST requests from the ESP32 (e.g., `/api/sensor-data`).
2. **Real-time Broadcasting:** Every time data hits the server, it emits a `socketio` event so any open browsers instantly see the new data without having to refresh.
3. **Database (`pet_state.db` / `sensor_data.db`):** 
   - Uses `get_db_connection()` wrapped in a Python `threading.Lock` to prevent database corruption.
   - Stores every sensor reading, tracks the daily steps, logs AI STT (Speech-to-Text), and keeps a backup mirror of the pet state.
4. **AI Processing:**
   - **Image Captioning:** When the ESP32 uploads a JPG (`/upload`), the server spins up a background task to send the image to a HuggingFace BLIP Model to generate a caption (e.g., "A picture of a dog face").
   - **Speech-to-Text:** When the ESP32 sends audio (`/api/audio-stt`), the server forwards it to the ElevenLabs API, transcribes it, and sends the text to the dashboard.

### Device State Syncing
- **The Hardware wins:** Historically, the server controlled the pet. But to save battery, the logic was moved to the ESP32. 
- **Mirroring:** The ESP32 sends its current `hunger`, `health`, etc., in the `/api/sensor-data` payload. The server takes this data and updates its database via `update_pet_state_atomic()`. The dashboard simply reflects what the server stored.

---

## 4. Frontend Logic (`index.html`)

The dashboard is a single-page HTML application designed to be lightweight.

### How it works:
- **WebSocket connection:** On load, JavaScript connects to `ws://[server-ip]:5000/socket.io/`.
- **Event Listeners:** It listens for events like `sensor_update`, `oled_display_changed`, `stt_transcription`, and `ota_progress`. When an event fires, it updates the graphs and text on screen instantly.
- **HTTP Polling Fallback:** If WebSockets fail to connect, it has a backup mechanism that occasionally fetches `/api/latest` to keep the data relatively fresh.
- **Data Table:** There's a historical data table showing the last 50-1000 sensor readings with built-in Javascript sorting and filtering.

---

## 5. Over-the-Air (OTA) Update Flow

One of the most complex features is how firmware updates are pushed wirelessly to the ESP32.

1. **Upload firmware to server:** Through the dashboard UI, you upload a `.bin` file containing new C++ code (compiled via Arduino IDE). The server saves it and records the version in the DB.
2. **Enable OTA:** You click "Enable OTA" on the dashboard. This turns a flag on the server to `True`.
3. **Device Polls Server:** Every few seconds, the ESP32 pings `/api/oled-display/get`. It sees the `ota_update: true` flag in the response.
4. **Device Locks Up:** The ESP32 suspends the camera, stops the audio stream, and freezes the screen to free up memory.
5. **Download & Flash:** The ESP32 calls `/api/firmware/latest`, gets the download URL for the `.bin`, and streams the file chunk-by-chunk directly into its flash memory.
6. **Progress Reporting:** As it flashes, the ESP32 continuously POSTs its progress (%) to `/api/ota/progress`. The Server updates its state, and broadcasts this to the frontend.
7. **Reboot:** Upon reaching 100%, the ESP32 reboots and starts running the new code. The dashboard sees the success and hides the loader.

---

## 6. Authentication & Security

Because this is primarily an IoT local-network project:
- **General APIs:** Endpoints like `/api/sensor-data` or `/upload` do **not** require any authentication.
- **OTA Security:** Pushing raw firmware is dangerous. To prevent bad actors from flashing the device, the firmware endpoints (`/api/firmware/latest` and `/api/firmware/download`) require a hardcoded header: 
  `X-OTA-Token: KAKU_SECURE_FIRMWARE_UPDATE_2026`
- **Rate limiting/CORS:** CORS is fully open (allows any origin), and there is no strict rate limiting, making it simple to test on local networks but vulnerable on the public internet.

---

## Summary of API Communication

Everything revolves around HTTP and WebSockets. The ESP32 acts as an HTTP Client, and the Flask app acts as an HTTP Server + WebSocket Server.

- **ESP32 → Server (Push):** Posts sensor data every 1s-4s. Posts camera images when feeding. Posts audio when voice is detected.
- **ESP32 ← Server (Poll):** Asks the server "what should I draw on the screen?" or "is there a new update?".
- **Server → Dashboard (Push):** Any time the server receives something interesting from the ESP32, it emits a WebSocket broadcast immediately updating the frontend.

*End of Documentation.*
