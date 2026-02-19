/*
ESP32 Dashboard Client - Arduino C++ Version with Camera & Microphone
This sketch reads sensors, captures images, records audio, and sends data to dashboard

⚠️  CURRENT MODE: SENSOR DATA ONLY ⚠️
📡 ACTIVE: MPU6050 (accelerometer/gyro) + Microphone Level
❌ DISABLED: Camera Image Capture & Audio Recording/Sending
   (Camera & Audio code commented out for testing - search for "DISABLED" to re-enable)

Required Libraries:
- ArduinoJson
- WiFi
- HTTPClient
- I2Cdev
- MPU6050
- esp_camera (ESP32 Camera)

Install via Arduino IDE: Sketch > Include Library > Manage Libraries
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "MPU6050.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "driver/i2s_pdm.h"
#include "base64.h"

// ================= OLED & PET ANIMATIONS =================
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "all_pets.h"

// ================= WIFI =================
#define WIFI_SSID     "123"
#define WIFI_PASSWORD "KUNAL 26"

// ================= API =================
const char* serverUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/sensor-data";  // Google Cloud Run Production
const char* eventsUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/events?device_id=ESP32_001";  // Events endpoint
const char* eventReceivedUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/device/event/received";  // Event acknowledgment
const char* oledDisplayUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/oled-display/get";  // OLED display animation endpoint
// NOTE: Orientation endpoint removed - server computes direction from sensor data

// ================= CAMERA PINS (XIAO ESP32 S3 Sense) =================
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 10
#define SIOD_GPIO_NUM 40
#define SIOC_GPIO_NUM 39
#define Y9_GPIO_NUM 48
#define Y8_GPIO_NUM 11
#define Y7_GPIO_NUM 12
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 16
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 17
#define Y2_GPIO_NUM 15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM 47
#define PCLK_GPIO_NUM 13

// ================= AUDIO SETTINGS =================
#define SAMPLE_RATE     16000
#define SAMPLE_BITS     16
#define RECORD_SECONDS  1     // 1 second of audio (4x smaller payload)
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN     2
#define AUDIO_DATA_SIZE (SAMPLE_RATE * 2 * RECORD_SECONDS)

// ================= I2S PDM MIC PINS (XIAO ESP32 S3 Sense) =================
#define PDM_CLK_GPIO (gpio_num_t)42  // PDM CLK
#define PDM_DIN_GPIO (gpio_num_t)41  // PDM DATA
#define I2S_NUM I2S_NUM_0

// ================= OLED DISPLAY SETUP =================
#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 32
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= OLED ANIMATION DISPLAY =================
enum PetAge { INFANT = 0, CHILD = 1, ADULT = 2, OLD = 3 };
PetAge petAge = CHILD;              // Default to child
unsigned long lastAnimationTime = 0;
unsigned long lastDisplayCheckTime = 0;  // Track when we last checked server for OLED display state
const unsigned long DISPLAY_CHECK_INTERVAL = 2000;  // Poll server for OLED display state every 2 seconds
const unsigned long ANIMATION_DISPLAY_INTERVAL = 100;  // Display animation every 100ms (~10 FPS smooth)
uint8_t currentFrame = 0;
bool displayReady = false;
bool startupComplete = false;  // Track if startup egg animation is done
bool showHomeIcon = false;  // NEW: Only show home icon when server says so
bool showFoodIcon = false;  // NEW: Show food icon when pet is hungry
bool showPoopIcon = false;  // NEW: Show poop icon when poop present
String currentScreenType = "MAIN";  // NEW: Track current screen state from server
bool justFinishedEating = false;  // NEW: Track if just finished eating to show GOOD text
unsigned long eatingFinishTime = 0;  // NEW: Track when eating finished

// NEW: Mode and hunger tracking for conditional camera send
String currentMode = "AUTOMATIC";  // Mode from server: AUTOMATIC or MANUAL
bool petIsHungry = false;          // Hunger status from server (hunger > 70)

// Camera cover detection for menu switching
#define BLACK_BRIGHTNESS_TH 25     // Brightness threshold for black detection
#define BLACK_CONTRAST_TH   25     // Contrast threshold for black detection

// NEW: Menu cycling via consecutive black frame detection (uses 5-sec captures only)
int consecutiveBlackFrames = 0;        // Count consecutive black frames
unsigned long lastMenuCycleTime = 0;   // Cooldown between menu cycles (prevent spam)
const unsigned long MENU_CYCLE_COOLDOWN = 3000;  // 3 seconds cooldown

volatile bool cameraCapturing = false;  // Flag to prevent camera access conflicts
bool imageAlreadySentThisSession = false;  // Track if image sent for current food session

// MPU6050 sensor
MPU6050 mpu;
bool mpuAvailable = false;  // Track if MPU6050 is working

// Pins
const int LED_PIN = 2;         // LED indicator

// Voice Activity Detection
volatile bool speechDetected = false;
volatile bool audioReady = false;
volatile int audioEnergyLevel = 0;
String detectedAudioData = "";

// Dual-core synchronization
SemaphoreHandle_t audioMutex;
SemaphoreHandle_t cameraMutex;
TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t cameraTaskHandle = NULL;

// Camera data ready flag
volatile bool cameraImageReady = false;
uint8_t* capturedImageBuffer = NULL;
size_t capturedImageLength = 0;

// VAD Settings
#define VAD_THRESHOLD 1000        // Energy threshold for speech detection
#define VAD_MIN_DURATION 500      // Minimum 500ms of speech to trigger
#define SILENCE_TIMEOUT 2000      // 2s of silence before stopping recording

// Structure for single sensor reading with timestamp
struct SingleReading {
    unsigned long timestamp_ms;  // Millisecond timestamp
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
};

// Structure for buffered sensor data (batch of readings)
struct SensorDataBatch {
    static const int MAX_READINGS = 20;  // Store up to 20 readings
    int reading_count;
    SingleReading readings[MAX_READINGS];
    float avg_mic_level;
    int sound_data;
};

// Timing
unsigned long lastSendTime = 0;
unsigned long lastImageCapture = 0;
unsigned long lastEventPoll = 0;               // Event polling timing
unsigned long lastInternalReadTime = 0;        // Fast internal sensor reading timing
const unsigned long SEND_INTERVAL = 2000;      // Send sensor data batch every 2 seconds
const unsigned long INTERNAL_READ_INTERVAL = 100;  // NEW: Read sensor every 100ms internally
const unsigned long IMAGE_INTERVAL = 5000;     // Capture image every 5 seconds (continuous background)
const unsigned long EVENT_POLL_INTERVAL = 5000; // Poll for events every 5 seconds
unsigned long dynamicEventPollInterval = 5000;  // Dynamic backoff for event polling
// Audio now triggered by speech detection, not timer

// NEW: Sensor reading buffer
SensorDataBatch sensorBatch = {};
float totalMicLevel = 0.0;
int micReadingCount = 0;

// ⏸️ PAUSE CONTROL FOR UPLOADS
bool isUploadingImage = false;                  // Flag to pause sensor data during image upload

// Camera and audio status
bool cameraReady = false;
bool micReady = false;
uint8_t* audio_buffer = NULL;

// Audio processing buffers for VAD
int16_t* vad_buffer;
const int VAD_BUFFER_SIZE = 512;

// PDM microphone handle
i2s_chan_handle_t rx_handle = NULL;

// Structure for sensor data
struct SensorData {
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    float mic_level;
    int sound_data;
    String camera_image_b64;  // Base64 encoded image
    String audio_data_b64;    // Base64 encoded audio
    bool has_new_image;
    bool has_new_audio;
    
    // Orientation tracking fields
    String device_orientation;
    float orientation_confidence;
    float calibrated_ax, calibrated_ay, calibrated_az;
    
    // NEW: Batch of sensor readings for better step detection
    SensorDataBatch sensor_batch;
};

// ================= ORIENTATION DETECTION =================
// NOTE: Direction detection moved to Flask server
// ESP32 now only sends raw MPU6050 data

// NOTE: Orientation computation moved to Flask server (see app.py)
// Server will compute direction from raw accel data

// ================= FORWARD DECLARATIONS =================
void displayPetAnimation();
SensorData readAllSensors();
String captureImageBase64();
bool sendSensorDataOnly(SensorData data);
void sendImageData(String imageBase64);
void sendAudioData(String audioBase64);
void pollForEvents();
bool isServerAlive();
void scanI2CDevices();
bool initCamera();
bool initAudio();
void audioMonitorTask(void *parameter);
void cameraMonitorTask(void *parameter);
void processEvent(const char* event_type, const char* message);
void acknowledgeEvent(int event_id);
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate);
void sendAllDataToServer(SensorData data);
String recordAudioBase64();
void notifyServerStartupComplete();  // NEW: Notify server startup is complete
void getOLEDDisplayFromServer();  // NEW: Forward declaration
void drawHomeIcon();  // NEW: Draw home icon pixel-by-pixel
void drawFoodIcon();  // NEW: Draw food icon pixel-by-pixel (bottom-right)
void drawPoopIcon();  // NEW: Draw poop icon pixel-by-pixel (bottom-right)
void playEatingAnimation();  // NEW: Play eating animation
void drawStaticFoodIcon();  // NEW: Draw static food icon at top-left (food menu)
void drawStaticToiletIcon();  // NEW: Draw static toilet icon at top-left (toilet menu)
void displayFoodMenu();  // NEW: Display food menu screen
void displayToiletMenu();  // NEW: Display toilet menu screen
bool isFrameMostlyBlack(camera_fb_t * fb);  // NEW: Check if camera is covered
void cycleMenu();  // NEW: Cycle through menus (MAIN → FOOD → TOILET)
// void checkCameraCover();  // DISABLED: Check camera cover for menu switching (now uses 5-sec intervals)
void oledTask(void *parameter);  // NEW: OLED animation task on Core 0

// ================= OLED ANIMATION TASK (Core 0) =================
// Independent FreeRTOS task runs OLED animation on Core 0
// Decoupled from WiFi/HTTP calls on Core 1 for smooth 60 FPS display
void oledTask(void *parameter) {
    Serial.println("🎬 OLED Task started on Core 0");
    
    while (true) {
        if (displayReady && startupComplete) {
            displayPetAnimation();  // Draw animation (non-blocking)
        }
        vTaskDelay(pdMS_TO_TICKS(16));  // ~60 FPS refresh rate (every 16ms)
    }
}

// ================= SETUP FUNCTION =================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\nESP32 Dashboard Client Starting...");
    
    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // Connect to WiFi with timeout protection
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting WiFi");

    // FIX: Add 30-second timeout to prevent infinite loop
    unsigned long wifiStartTime = millis();
    const unsigned long WIFI_TIMEOUT = 30000;  // 30 seconds
    
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartTime < WIFI_TIMEOUT)) {
        Serial.print(".");
        delay(300);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n⚠️  WiFi connection timeout! Continuing without WiFi...");
        Serial.println("⚠️  Server communication disabled - running in offline mode");
    }
    
    // FIX 6: Enable WiFi modem sleep for power savings
    WiFi.setSleep(true);
    Serial.println("💤 WiFi sleep mode enabled");
    
    // FIX 4: Lower CPU frequency from 240MHz to 160MHz
    setCpuFrequencyMhz(160);
    Serial.println("⚡ CPU frequency reduced to 160MHz");
    
    // Initialize I2C and MPU6050 with timeout protection
    Serial.println("Initializing I2C...");
    Wire.begin(5, 6);  // XIAO ESP32 S3: SDA=5, SCL=6
    Wire.setClock(400000);  // Set I2C speed to 400kHz
    delay(1000);
    
    // Initialize OLED Display
    Serial.println("Initializing OLED Display...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("❌ OLED initialization failed!");
        displayReady = false;
    } else {
        Serial.println("✅ OLED initialized successfully!");
        displayReady = true;
        
        // ============ STARTUP SEQUENCE ============
        // Display startup text
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 8);
        display.println("KAKU");
        display.setCursor(0, 18);
        display.println("Starting...");
        display.display();
        delay(1000);
        
        // Play egg cracking animation from all_pets.h
        Serial.println("🥚 Playing egg cracking animation...");
        playEggCrackingAnimation();
        
        // Egg screen slides to the left and exits
        Serial.println("🥚 Sliding egg screen out...");
        slideEggScreenOut();
        
        // Infant appears gradually from left side
        Serial.println("👶 Sliding infant in from left...");
        slideInfantSlowlyFromLeft();
        
        // Mark startup as complete
        startupComplete = true;
        Serial.println("✅ Startup complete! Main screen ready.");
    }
    
    Serial.println("Scanning I2C devices...");
    scanI2CDevices();
    
    Serial.println("Initializing MPU6050...");
    bool mpuSuccess = false;
    
    // Try MPU6050 initialization with timeout
    unsigned long startTime = millis();
    while (!mpuSuccess && (millis() - startTime < 5000)) {  // 5 second timeout
        mpu.initialize();
        delay(100);
        
        if (mpu.testConnection()) {
            Serial.println("✅ MPU6050 initialized successfully!");
            mpuSuccess = true;
            mpuAvailable = true;  // Set flag for sensor readings
        } else {
            Serial.print(".");
            delay(500);
        }
    }
    
    if (!mpuSuccess) {
        Serial.println("\n❌ MPU6050 initialization failed after 5 seconds");
        Serial.println("⚠️  Continuing without MPU6050 (will send dummy sensor data)");
    }
    
    // Initialize Camera
    initCamera();
    
    // Initialize I2S Microphone
    initAudio();
    
    // Create mutexes for synchronization
    audioMutex = xSemaphoreCreateMutex();
    cameraMutex = xSemaphoreCreateMutex();
    
    // Start audio monitoring task on Core 0 (dedicated to audio/VAD)
    xTaskCreatePinnedToCore(
        audioMonitorTask,    // Task function
        "AudioMonitor",      // Task name
        8192,               // Stack size
        NULL,               // Parameters
        2,                  // Priority (high for real-time audio)
        &audioTaskHandle,   // Task handle
        0                   // Core 0 (dedicated to audio)
    );
    
    // Start camera monitoring task on Core 0 (shares core with audio)
    xTaskCreatePinnedToCore(
        cameraMonitorTask,   // Task function
        "CameraMonitor",     // Task name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority (lower than audio)
        &cameraTaskHandle,  // Task handle
        0                   // Core 0 (shared with audio)
    );
    
    // Start OLED animation task on Core 0 (independent of WiFi on Core 1)
    xTaskCreatePinnedToCore(
        oledTask,           // Task function
        "OLED",            // Task name
        4096,              // Stack size
        NULL,              // Parameters
        1,                 // Priority (lower than audio)
        NULL,              // Task handle
        0                  // Core 0 (opposite of WiFi heavy Core 1)
    );
    
    Serial.println("System Ready!");
    Serial.println("🎤 Core 0: Audio + Camera monitoring");
    Serial.println("🌐 Core 1: Sensors, WiFi transmission, OLED");
}

void scanI2CDevices() {
    Serial.println("🔍 Scanning I2C devices...");
    byte error, address;
    int deviceCount = 0;

    for (address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();

        if (error == 0) {
            Serial.printf("✅ I2C device found at address 0x%02X", address);
            if (address == 0x68 || address == 0x69) {
                Serial.print(" (MPU6050)");
            }
            Serial.println();
            deviceCount++;
        }
        else if (error == 4) {
            Serial.printf("❌ Unknown error at address 0x%02X\n", address);
        }
    }

    if (deviceCount == 0) {
        Serial.println("⚠️  No I2C devices found");
        Serial.println("   Check wiring: SDA->GPIO6, SCL->GPIO7");
    } else {
        Serial.printf("🎯 Found %d I2C device(s)\n", deviceCount);
    }
    Serial.println();
}

// ================= EGG CRACKING ANIMATION =================
void playEggCrackingAnimation() {
    // Display egg cracking animation from all_pets.h
    Serial.println("🥚 Egg animation starting...");
    
    if (displayReady) {
        // Display egg cracking sequence with all frames
        for (int i = 0; i < EGG_CRACK_FRAME_COUNT; i++) {
            display.clearDisplay();
            // Draw the actual egg crack frame from all_pets.h
            display.drawBitmap(0, 0, egg_crack_frames[i], EGG_CRACK_WIDTH, EGG_CRACK_HEIGHT, SSD1306_WHITE);
            display.display();
            Serial.printf("🥚 Egg frame %d/%d\n", i + 1, EGG_CRACK_FRAME_COUNT);
            delay(egg_crack_delays[i]);  // Use delay from all_pets.h (2 seconds each)
        }
        Serial.println("🐣 Egg hatching complete!");
    }
}

// ================= SLIDE TRANSITION ANIMATION =================
void slideEggScreenOut() {
    // Egg screen slides to the left and exits
    if (!displayReady) return;
    
    Serial.println("🥚 Egg screen sliding left...");
    const int slideSteps = 8;
    
    for (int step = 0; step <= slideSteps; step++) {
        display.clearDisplay();
        
        // Calculate x position: starts at 0, ends at -64 (completely off left side)
        int xPos = -(step * SCREEN_WIDTH) / slideSteps;
        
        // Draw the final egg frame sliding left
        display.drawBitmap(xPos, 0, egg_crack_frames[EGG_CRACK_FRAME_COUNT - 1], EGG_CRACK_WIDTH, EGG_CRACK_HEIGHT, SSD1306_WHITE);
        
        display.display();
        delay(80);
    }
    Serial.println("✅ Egg screen exit complete!");
}

void slideInfantSlowlyFromLeft() {
    // Infant appears gradually from left side (width gradually increasing)
    if (!displayReady) return;
    
    Serial.println("👶 Infant appearing slowly from left...");
    const int slideSteps = 15;  // 15 steps @ 200ms = 3 seconds
    
    for (int step = 0; step <= slideSteps; step++) {
        display.clearDisplay();
        
        // Calculate x position: starts at -width (fully left of screen, invisible)
        // ends at 0 (fully visible on screen)
        int xPos = -INFANT_WIDTH + (step * INFANT_WIDTH) / slideSteps;
        
        // Draw infant gradually appearing from left
        display.drawBitmap(xPos, 0, infant_frames[0], INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
        
        // Animation only - no text!
        display.display();
        delay(200);  // 15 steps × 200ms = 3 seconds total
    }
    
    // Final position - infant fully visible and centered
    display.clearDisplay();
    display.drawBitmap(0, 0, infant_frames[0], INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
    display.display();
    
    petAge = INFANT;
    Serial.println("✅ Infant fully visible!");
    
    // NEW: Notify server that startup is complete
    notifyServerStartupComplete();
}

// ================= HOME ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws home icon using pixel-by-pixel approach to prevent corruption with other animations
void drawHomeIcon() {
    int xOffset = 0;  // Top-left corner
    int yOffset = 0;
    
    for (uint16_t y = 0; y < HOME_ICON_HEIGHT; y++) {
        for (uint16_t x = 0; x < HOME_ICON_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * HOME_ICON_WIDTH + x;
            uint8_t bitIndex = y % 8;
            
            if (pgm_read_byte(&home_icon_frames[0][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// ================= FOOD ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws food icon at bottom-right corner using pixel-by-pixel approach
void drawFoodIcon() {
    int xOffset = SCREEN_WIDTH - FOOD_ICON_WIDTH;   // Bottom-right: 64-24 = 40
    int yOffset = SCREEN_HEIGHT - FOOD_ICON_HEIGHT;  // Bottom-right: 32-12 = 20
    
    // Draw current food icon frame (animate between 2 frames)
    uint8_t foodFrame = (millis() / food_icon_delays[0]) % FOOD_ICON_FRAME_COUNT;
    
    for (uint16_t y = 0; y < FOOD_ICON_HEIGHT; y++) {
        for (uint16_t x = 0; x < FOOD_ICON_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * FOOD_ICON_WIDTH + x;
            uint8_t bitIndex = y % 8;
            
            if (pgm_read_byte(&food_icon_frames[foodFrame][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// ================= POOP ICON DRAWING (PIXEL-BY-PIXEL) =================
// Draws poop icon at bottom-right corner using pixel-by-pixel approach
void drawPoopIcon() {
    int xOffset = SCREEN_WIDTH - POOP_WIDTH;   // Bottom-right: 64-24 = 40
    int yOffset = SCREEN_HEIGHT - POOP_HEIGHT;  // Bottom-right: 32-12 = 20
    
    // Draw current poop icon frame (animate between 2 frames)
    uint8_t poopFrame = (millis() / poop_delays[0]) % POOP_FRAME_COUNT;
    
    for (uint16_t y = 0; y < POOP_HEIGHT; y++) {
        for (uint16_t x = 0; x < POOP_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * POOP_WIDTH + x;
            uint8_t bitIndex = 7 - (y % 8);  // Different bit indexing for poop
            
            if (pgm_read_byte(&poop_frames[poopFrame][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// ================= EATING ANIMATION (Full Screen) =================
// Plays full-screen eating animation (5 frames)
void playEatingAnimation() {
    if (!displayReady) return;
    
    Serial.println("😋 Playing eating animation...");
    
    for (uint8_t frame = 0; frame < EATING_FRAME_COUNT; frame++) {
        display.clearDisplay();
        
        // Draw full-screen eating animation frame
        display.drawBitmap(0, 0, eating_frames[frame], EATING_WIDTH, EATING_HEIGHT, SSD1306_WHITE);
        
        display.display();
        vTaskDelay(pdMS_TO_TICKS(eating_delays[frame]));  // 100ms per frame
    }
    
    Serial.println("✅ Eating animation complete!");
}

// Draw static food icon at top-left (for food menu)
void drawStaticFoodIcon() {
    int xOffset = 0;  // Top-left corner
    int yOffset = 0;
    
    // Use first frame of food icon animation (no animation in menu)
    for (uint16_t y = 0; y < FOOD_ICON_HEIGHT; y++) {
        for (uint16_t x = 0; x < FOOD_ICON_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * FOOD_ICON_WIDTH + x;
            uint8_t bitIndex = y % 8;
            
            if (pgm_read_byte(&food_icon_frames[0][byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// Draw static toilet icon at top-left (for toilet menu)
void drawStaticToiletIcon() {
    int xOffset = 0;  // Top-left corner
    int yOffset = 0;
    
    for (uint16_t y = 0; y < TOILET_HEIGHT; y++) {
        for (uint16_t x = 0; x < TOILET_WIDTH; x++) {
            uint16_t byteIndex = (y / 8) * TOILET_WIDTH + x;
            uint8_t bitIndex = y % 8;
            
            if (pgm_read_byte(&toilet_icon[byteIndex]) & (1 << bitIndex)) {
                display.drawPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
}

// Display food menu screen
void displayFoodMenu() {
    display.clearDisplay();
    
    // NEW: Show EATING ANIMATION while image is uploading
    // The captured frame IS the food - no AI detection needed
    if (isUploadingImage) {
        // Show looping eating animation (Pacman) - full screen, no food icon
        uint8_t eatingFrame = (millis() / 100) % EATING_FRAME_COUNT;  // 100ms per frame
        display.drawBitmap(0, 0, eating_frames[eatingFrame], EATING_WIDTH, EATING_HEIGHT, SSD1306_WHITE);
        // Removed Serial.println to reduce clutter during animation loop
    }
    // Check if just finished eating (show GOOD for 3 seconds)
    else if (justFinishedEating && (millis() - eatingFinishTime < 3000)) {
        // Draw food icon at top-left
        drawStaticFoodIcon();
        
        // Show "GOOD" text after eating (NO newline to prevent cursor artifacts)
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(18, 12);
        display.print("GOOD!");  // Changed from println to print - prevents unwanted cursor movement
    } else if (justFinishedEating) {
        // Draw food icon at top-left
        drawStaticFoodIcon();
        
        // After GOOD text expires, show HAPPY face
        justFinishedEating = false;  // Reset flag
        const uint8_t* frameData = child_frames[0];  // HAPPY face
        display.drawBitmap(20, 8, frameData, CHILD_WIDTH, CHILD_HEIGHT, SSD1306_WHITE);
    } else {
        // Draw food icon at top-left
        drawStaticFoodIcon();
        
        // Before eating: Show SAD pet face (hungry, waiting for food)
        uint8_t sadFrame = (millis() / sad_delays[0]) % SAD_FRAME_COUNT;
        const uint8_t* frameData = sad_frames[sadFrame];  // SAD animation
        display.drawBitmap(20, 8, frameData, SAD_WIDTH, SAD_HEIGHT, SSD1306_WHITE);
    }
    
    display.display();
}

// Display toilet menu screen
void displayToiletMenu() {
    display.clearDisplay();
    
    // Draw static toilet icon at top-left (no blinking)
    drawStaticToiletIcon();
    
    display.display();
}

// ================= CAMERA COVER DETECTION FOR MENU SWITCHING =================
// Check if camera frame is mostly black (covered)
bool isFrameMostlyBlack(camera_fb_t * fb) {
    if (!fb || fb->len == 0) return false;
    
    long total = 0;
    int maxB = 0;
    int minB = 255;
    int count = 0;
    
    // Sample pixels (every 8th pixel for speed)
    for (int i = 0; i < fb->len && i < 12000; i += 8) {
        uint8_t p = fb->buf[i];
        total += p;
        count++;
        if (p > maxB) maxB = p;
        if (p < minB) minB = p;
    }
    
    if (count == 0) return false;
    
    int avg = total / count;
    int contrast = maxB - minB;
    
    return (avg <= BLACK_BRIGHTNESS_TH && contrast <= BLACK_CONTRAST_TH);
}

// Cycle through menus: MAIN → FOOD_MENU → TOILET_MENU → MAIN
void cycleMenu() {
    String newMenu;
    
    if (currentScreenType == "MAIN") {
        newMenu = "FOOD_MENU";
    } else if (currentScreenType == "FOOD_MENU") {
        newMenu = "TOILET_MENU";
    } else {
        newMenu = "MAIN";
    }
    
    // Update server with new menu selection
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    
    String url = "https://kakuproject-90943350924.asia-south1.run.app/api/oled-display/menu-switch";
    
    if (http.begin(url)) {
        http.addHeader("Content-Type", "application/json");
        
        String payload = "{\"device_id\":\"ESP32_001\",\"menu\":\"" + newMenu + "\"}";
        int httpCode = http.POST(payload);
        
        if (httpCode == 200) {
            currentScreenType = newMenu;
            
            // Reset image send flag when leaving FOOD_MENU
            if (newMenu != "FOOD_MENU") {
                imageAlreadySentThisSession = false;
                Serial.println("🔄 Reset image send flag (left FOOD_MENU)");
            }
            
            Serial.printf("📱 Menu cycled to: %s\n", newMenu.c_str());
        } else {
            Serial.printf("❌ Menu switch failed: %d\n", httpCode);
        }
        
        http.end();
    }
}

// ================= CAMERA COVER DETECTION (DISABLED - Moved to 5-sec capture) =================
// OLD: Continuous frame checking every loop (causes hardware heating)
// NEW: Black frame detection integrated into cameraMonitorTask() 5-second captures
/*
void checkCameraCover() {
    // Skip cover detection if camera is currently capturing (prevent conflict)
    if (cameraCapturing) return;
    
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) return;
    
    bool isBlack = isFrameMostlyBlack(fb);
    unsigned long now = millis();
    
    if (isBlack) {
        if (!blackActive) {
            blackActive = true;
            blackStartTime = now;
        }
    } else {
        if (blackActive) {
            unsigned long holdTime = now - blackStartTime;
            
            // Check cooldown to prevent rapid switching
            if ((now - lastCoverActionTime) > COOLDOWN_MS) {
                if (holdTime >= HOLD_TIME_MS) {
                    // 3+ second hold → Cycle menu
                    cycleMenu();
                    lastCoverActionTime = now;
                }
            }
            
            blackActive = false;
        }
    }
    
    esp_camera_fb_return(fb);
}
*/

// ================= PET ANIMATION FUNCTION =================
void displayPetAnimation() {
    if (!displayReady) return;
    
    // Display animation every 150ms
    if (millis() - lastAnimationTime >= ANIMATION_DISPLAY_INTERVAL) {
        lastAnimationTime = millis();
        
        // Check which screen to display
        if (currentScreenType == "FOOD_MENU") {
            displayFoodMenu();
            return;  // Exit early
        } else if (currentScreenType == "TOILET_MENU") {
            displayToiletMenu();
            return;  // Exit early
        }
        
        // Default: MAIN screen with pet animation
        display.clearDisplay();
        
        // Select frame based on pet age
        const uint8_t* frameData = nullptr;
        uint8_t frameCount = 0;
        
        switch (petAge) {
            case INFANT:
                frameData = infant_frames[currentFrame % INFANT_FRAME_COUNT];
                frameCount = INFANT_FRAME_COUNT;
                display.drawBitmap(0, 0, frameData, INFANT_WIDTH, INFANT_HEIGHT, SSD1306_WHITE);
                break;
            case CHILD:
                frameData = child_frames[currentFrame % CHILD_FRAME_COUNT];
                frameCount = CHILD_FRAME_COUNT;
                display.drawBitmap(0, 0, frameData, CHILD_WIDTH, CHILD_HEIGHT, SSD1306_WHITE);
                break;
            case ADULT:
                frameData = adult_frames[currentFrame % ADULT_FRAME_COUNT];
                frameCount = ADULT_FRAME_COUNT;
                display.drawBitmap(0, 0, frameData, ADULT_WIDTH, ADULT_HEIGHT, SSD1306_WHITE);
                break;
            case OLD:
                frameData = old_frames[currentFrame % OLD_FRAME_COUNT];
                frameCount = OLD_FRAME_COUNT;
                display.drawBitmap(0, 0, frameData, OLD_WIDTH, OLD_HEIGHT, SSD1306_WHITE);
                break;
        }
        
        // Draw home icon at top-left corner using pixel-by-pixel approach (no corruption)
        if (showHomeIcon && currentScreenType == "MAIN") {
            drawHomeIcon();
        }
        
        // Draw food icon at bottom-right corner (shows when pet is hungry)
        if (showFoodIcon && currentScreenType == "MAIN") {
            drawFoodIcon();
        }
        
        // Draw poop icon at bottom-right corner (shows when poop present)
        if (showPoopIcon && currentScreenType == "MAIN") {
            drawPoopIcon();
        }
        
        // Only animation - no text
        display.display();
        
        // Increment frame
        currentFrame++;
    }
}

void loop() {
    // This loop runs on Core 1 - handles sensors, camera, WiFi, HTTP I/O
    // OLED animation runs independently on Core 0 task
    
    // Only poll server after startup is complete
    if (startupComplete) {
        // Poll server for OLED display state every 2 seconds (non-blocking)
        if (millis() - lastDisplayCheckTime >= DISPLAY_CHECK_INTERVAL) {
            lastDisplayCheckTime = millis();
            getOLEDDisplayFromServer();  // Let it fail silently if server down
        }
        
        // NOTE: Camera cover detection moved to 5-second capture interval
        // No continuous checking - reduces hardware heating
    }
    
    // Check WiFi connection with timeout protection
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi disconnected, attempting reconnect...");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        // FIX: Add 20-second timeout to prevent infinite loop
        unsigned long reconnectStart = millis();
        const unsigned long RECONNECT_TIMEOUT = 20000;  // 20 seconds
        
        while (WiFi.status() != WL_CONNECTED && (millis() - reconnectStart < RECONNECT_TIMEOUT)) {
            Serial.print(".");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n✅ WiFi Reconnected");
            Serial.println(WiFi.localIP());
        } else {
            Serial.println("\n⚠️  WiFi reconnection failed! Operating in offline mode...");
            // Continue running - OLED and local sensors still work
        }
    }
    
    // Debug: Print WiFi status
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 10000) {  // Every 10 seconds
        lastWiFiCheck = millis();
        Serial.printf("🔗 Core 1 Status | WiFi: %s | IP: %s | Signal: %d dBm\n",
                      WiFi.status() == WL_CONNECTED ? "✅ Connected" : "❌ Disconnected",
                      WiFi.localIP().toString().c_str(),
                      WiFi.RSSI());
        Serial.printf("🎤 Audio Energy: %d | Speech: %s\n", 
                      audioEnergyLevel,
                      speechDetected ? "🗣️  DETECTED" : "🔇 Silent");
    }
    
    // NEW: Collect sensor readings every 100ms internally
    if (millis() - lastInternalReadTime >= INTERNAL_READ_INTERVAL) {
        lastInternalReadTime = millis();
        
        // Only buffer if we have space
        if (sensorBatch.reading_count < SensorDataBatch::MAX_READINGS) {
            SingleReading reading;
            reading.timestamp_ms = millis();
            
            if (mpuAvailable) {
                int16_t ax, ay, az;
                mpu.getAcceleration(&ax, &ay, &az);
                reading.accel_x = ax / 16384.0 * 9.81;
                reading.accel_y = ay / 16384.0 * 9.81;
                reading.accel_z = az / 16384.0 * 9.81;
                
                int16_t gx, gy, gz;
                mpu.getRotation(&gx, &gy, &gz);
                reading.gyro_x = gx / 131.0;
                reading.gyro_y = gy / 131.0;
                reading.gyro_z = gz / 131.0;
            } else {
                reading.accel_x = reading.accel_y = reading.accel_z = 0.0;
                reading.gyro_x = reading.gyro_y = reading.gyro_z = 0.0;
            }
            
            sensorBatch.readings[sensorBatch.reading_count++] = reading;
            
            // Accumulate microphone level
            if (micReady && audioEnergyLevel > 0) {
                totalMicLevel += (20.0 + (audioEnergyLevel / 100.0));
            }
            micReadingCount++;
            
            if (sensorBatch.reading_count % 5 == 0) {
                Serial.printf("📊 Buffered %d readings (ax=%.2f ay=%.2f az=%.2f)\\n", 
                             sensorBatch.reading_count, reading.accel_x, reading.accel_y, reading.accel_z);
            }
        }
    }
    
    // Read sensor data
    SensorData data = readAllSensors();
    
    // ========== CAMERA ON CORE 0 ==========
    // Check if camera image ready from Core 0
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (cameraImageReady && capturedImageBuffer != NULL) {
            // NEW: CONDITIONAL SEND - Only send image if ALL conditions met:
            // 1. Mode is AUTOMATIC (AI pet mode)
            // 2. Currently on FOOD_MENU (user wants to feed)
            // 3. Pet is hungry (hunger > 70)
            // 4. Haven't sent image this session yet
            if (currentMode == "AUTOMATIC" && 
                currentScreenType == "FOOD_MENU" && 
                petIsHungry && 
                !imageAlreadySentThisSession) {
                
                // Mark that we have a new image (Core 1 will send binary buffer directly)
                data.has_new_image = true;
                imageAlreadySentThisSession = true;  // Mark as sent (prevents resend until flag reset)
                Serial.println("📸 AUTO MODE: Sending image for food detection");
            } else {
                // Don't send - conditions not met
                if (currentMode != "AUTOMATIC") {
                    Serial.println("⏭️  Skipping image send - MANUAL mode");
                } else if (currentScreenType != "FOOD_MENU") {
                    Serial.println("⏭️  Skipping image send - not on FOOD_MENU");
                } else if (!petIsHungry) {
                    Serial.println("⏭️  Skipping image send - pet not hungry");
                } else if (imageAlreadySentThisSession) {
                    Serial.println("⏭️  Skipping image send - already sent this session");
                }
            }
        }
        xSemaphoreGive(cameraMutex);
    }
    
    // Check for speech-triggered audio (from Core 0)
    // if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    //     if (speechDetected && !detectedAudioData.isEmpty()) {
    //         data.audio_data_b64 = detectedAudioData;
    //         data.has_new_audio = true;
    //         
    //         Serial.println("🗣️  Speech detected! Preparing to send audio...");
    //         
    //         // Clear the detected audio after copying
    //         detectedAudioData = "";
    //         speechDetected = false;
    //     }
    //     xSemaphoreGive(audioMutex);
    // }
    // ========== AUDIO STILL DISABLED ==========
    
    // Send all data every 2 seconds ONLY if server is alive AND not uploading image
    if (millis() - lastSendTime >= SEND_INTERVAL) {
        // NEW: PAUSE SENSOR DATA while image is uploading (to avoid interference)
        if (isUploadingImage) {
            Serial.println("⏸️  Image uploading... PAUSING sensor data transmission");
        } else {
            lastSendTime = millis();

            if (!isServerAlive()) {
                Serial.println("🛑 Server offline. Skipping data send cycle.");
            } else {
                // NEW: Prepare batch with collected readings
                data.sensor_batch = sensorBatch;
                
                // Calculate average microphone level
                if (micReadingCount > 0) {
                    data.sensor_batch.avg_mic_level = totalMicLevel / micReadingCount;
                    data.sensor_batch.sound_data = audioEnergyLevel;
                } else {
                    data.sensor_batch.avg_mic_level = 0.0;
                    data.sensor_batch.sound_data = 0;
                }
                
                // Print batch summary before sending
                Serial.printf("📤 Sending BATCH: %d readings | Avg mic: %.1f dB | Time span: %ld ms\\n",
                             sensorBatch.reading_count,
                             data.sensor_batch.avg_mic_level,
                             sensorBatch.reading_count > 1 ? 
                             (sensorBatch.readings[sensorBatch.reading_count-1].timestamp_ms - 
                              sensorBatch.readings[0].timestamp_ms) : 0);
                
                // Send sensor data (includes batch of readings)
                bool serverAccepted = sendSensorDataOnly(data);
                
                // RESET batch after sending
                sensorBatch.reading_count = 0;
                totalMicLevel = 0.0;
                micReadingCount = 0;
                
                // ========== CONDITIONAL IMAGE SENDING ==========
                // Send image ONLY if:
                // 1. On FOOD_MENU screen
                // 2. In AUTOMATIC mode (not manual override)
                // 3. Image not already sent for this food session
                if (serverAccepted && data.has_new_image && 
                    currentScreenType == "FOOD_MENU" && 
                    !imageAlreadySentThisSession) {
                    
                    Serial.println("🍽️  Conditions met: Sending image for food detection");
                    sendImageData("");  // Binary data passed via shared buffer
                    imageAlreadySentThisSession = true;  // Mark as sent
                    Serial.println("✅ Image sent flag set - won't send again until menu changes");
                }
                
                // Send audio only if sensor data was accepted and speech detected
                // if (serverAccepted && data.has_new_audio && !data.audio_data_b64.isEmpty()) {
                //     sendAudioData(data.audio_data_b64);
                //     Serial.println("🎵 Speech audio sent to server!");
                // }
                // ========== AUDIO STILL DISABLED ==========
            }
        }
    }
    
    // FIX 5: Poll for events with optimized intervals (PAUSE during image upload)
    if (!isUploadingImage && millis() - lastEventPoll >= dynamicEventPollInterval) {
        lastEventPoll = millis();
        pollForEvents();
        dynamicEventPollInterval = 5000;  // Standard interval
    } else if (isUploadingImage && millis() - lastEventPoll >= dynamicEventPollInterval) {
        // Skip event polling during upload
        lastEventPoll = millis();  // Update time to prevent immediate poll after upload
        Serial.println("⏸️  Image uploading... PAUSING event polling");
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));  // Small delay for Core 1 (non-blocking)
}

// ================= CAMERA INITIALIZATION =================
bool initCamera() {
    Serial.println("Initializing Camera...");
    
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;  // Fixed: was pin_sccb_sda
    config.pin_sscb_scl = SIOC_GPIO_NUM;  // Fixed: was pin_sccb_scl
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QQVGA; // 160x120 - reduced for power
    config.jpeg_quality = 20;             // Lower quality = less heat
    config.fb_count = 1;                  // Single buffer = less memory
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return false;
    }
    
    cameraReady = true;
    Serial.println("Camera initialized successfully");
    return true;
}

// ================= AUDIO INITIALIZATION =================
bool initAudio() {
    Serial.println("Initializing PDM Microphone...");
    
    // Create I2S channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    // Create new I2S channel
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        Serial.printf("I2S new channel failed: %d\n", err);
        return false;
    }
    
    // Configure PDM RX mode
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_GPIO,
            .din = PDM_DIN_GPIO,
            .invert_flags = { .clk_inv = false },
        },
    };
    
    err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
    if (err != ESP_OK) {
        Serial.printf("PDM RX mode init failed: %d\n", err);
        return false;
    }
    
    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        Serial.printf("I2S channel enable failed: %d\n", err);
        return false;
    }
    
    // Allocate VAD buffer
    vad_buffer = (int16_t*)malloc(VAD_BUFFER_SIZE * sizeof(int16_t));
    if (!vad_buffer) {
        Serial.println("❌ Failed to allocate VAD buffer");
        return false;
    }
    
    micReady = true;
    Serial.println("✅ PDM Microphone initialized successfully");
    return true;
}

// ================= DUAL-CORE CAMERA MONITORING TASK =================
void cameraMonitorTask(void *parameter) {
    Serial.println("📸 Core 0: Camera monitoring task started");
    
    unsigned long lastCapture = 0;
    
    while (true) {
        if (!cameraReady) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Capture image every 5 seconds (continuous background)
        if (millis() - lastCapture >= IMAGE_INTERVAL) {
            lastCapture = millis();
            
            // Skip capture if currently uploading (prevent overlap)
            if (isUploadingImage) {
                Serial.println("⏭️  Skipping capture - upload in progress");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            cameraCapturing = true;  // Set flag to prevent conflicts
            Serial.println("📸 Core 0: Capturing image (5-sec interval)...");
            camera_fb_t *fb = esp_camera_fb_get();
            
            if (fb) {
                Serial.printf("✅ Core 0: Image captured: %d bytes (raw JPEG)\n", fb->len);
                
                // NEW: Check if frame is black (camera covered) for menu cycling
                bool isBlack = isFrameMostlyBlack(fb);
                unsigned long now = millis();
                
                if (isBlack) {
                    consecutiveBlackFrames++;
                    Serial.printf("🖤 Black frame detected (%d/2)\n", consecutiveBlackFrames);
                    
                    // If 2 consecutive black frames (10 seconds total) → Cycle menu
                    if (consecutiveBlackFrames >= 2 && (now - lastMenuCycleTime) > MENU_CYCLE_COOLDOWN) {
                        Serial.println("🔄 Camera covered for 10 seconds → Cycling menu...");
                        cycleMenu();  // MAIN → FOOD_MENU → TOILET_MENU → MAIN
                        lastMenuCycleTime = now;
                        consecutiveBlackFrames = 0;  // Reset counter
                    }
                } else {
                    // Frame not black - reset counter
                    if (consecutiveBlackFrames > 0) {
                        Serial.println("✅ Camera uncovered - reset black frame counter");
                    }
                    consecutiveBlackFrames = 0;
                }
                
                // Store raw binary in PSRAM with mutex protection
                if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    // Free previous buffer if exists
                    if (capturedImageBuffer != NULL) {
                        free(capturedImageBuffer);
                    }
                    
                    // Allocate new buffer and copy data
                    capturedImageBuffer = (uint8_t*)ps_malloc(fb->len);
                    if (capturedImageBuffer) {
                        memcpy(capturedImageBuffer, fb->buf, fb->len);
                        capturedImageLength = fb->len;
                        cameraImageReady = true;
                        Serial.println("📦 Core 0: Image buffered for Core 1");
                    } else {
                        Serial.println("❌ Core 0: Failed to allocate image buffer");
                    }
                    
                    xSemaphoreGive(cameraMutex);
                }
                
                esp_camera_fb_return(fb);
            } else {
                Serial.println("❌ Core 0: Camera capture failed");
            }
        }
        
        // Check every second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ================= DUAL-CORE AUDIO MONITORING TASK =================
void audioMonitorTask(void *parameter) {
    Serial.println("🎤 Core 0: Audio monitoring task started");
    
    size_t bytes_read;
    unsigned long speechStartTime = 0;
    unsigned long lastSoundTime = 0;
    bool currentlyRecording = false;
    
    // Audio recording buffer for when speech is detected
    uint8_t* recording_buffer = NULL;
    size_t recorded_bytes = 0;
    const size_t MAX_RECORDING_SIZE = SAMPLE_RATE * 2 * 5; // Max 5 seconds
    
    while (true) {
        if (!micReady) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Continuously read audio for VAD analysis
        esp_err_t err = i2s_channel_read(rx_handle, vad_buffer, VAD_BUFFER_SIZE * sizeof(int16_t), 
                                        &bytes_read, pdMS_TO_TICKS(10));
        
        if (err != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        
        // Calculate audio energy for Voice Activity Detection
        int32_t energy = 0;
        int samples = bytes_read / sizeof(int16_t);
        
        for (int i = 0; i < samples; i++) {
            int16_t sample = vad_buffer[i];
            energy += abs(sample);
        }
        
        energy = energy / samples; // Average energy
        audioEnergyLevel = energy; // Update global energy level
        
        unsigned long currentTime = millis();
        
        // Voice Activity Detection
        if (energy > VAD_THRESHOLD) {
            lastSoundTime = currentTime;
            
            if (!currentlyRecording) {
                // Start recording when speech detected
                if (speechStartTime == 0) {
                    speechStartTime = currentTime;
                }
                
                // Check if we've had continuous speech for minimum duration
                if (currentTime - speechStartTime >= VAD_MIN_DURATION) {
                    Serial.println("🎤 Core 0: Speech detected! Starting recording...");
                    currentlyRecording = true;
                    
                    // Allocate recording buffer
                    recording_buffer = (uint8_t*)ps_malloc(MAX_RECORDING_SIZE + WAV_HEADER_SIZE);
                    if (recording_buffer) {
                        generate_wav_header(recording_buffer, MAX_RECORDING_SIZE, SAMPLE_RATE);
                        recorded_bytes = WAV_HEADER_SIZE;
                    }
                }
            }
        } else {
            // Reset speech start if energy drops below threshold
            if (currentTime - lastSoundTime > 200) { // 200ms of silence resets start
                speechStartTime = 0;
            }
        }
        
        // If currently recording, add audio data to buffer
        if (currentlyRecording && recording_buffer && 
            (recorded_bytes + bytes_read) < (MAX_RECORDING_SIZE + WAV_HEADER_SIZE)) {
            
            // Apply volume gain and copy to recording buffer
            for (int i = 0; i < samples; i++) {
                int16_t sample = vad_buffer[i];
                int32_t amp = sample << VOLUME_GAIN;
                if (amp > 32767) amp = 32767;
                if (amp < -32768) amp = -32768;
                
                *((int16_t*)(recording_buffer + recorded_bytes)) = amp;
                recorded_bytes += sizeof(int16_t);
            }
        }
        
        // Stop recording after silence timeout or buffer full
        if (currentlyRecording && 
            ((currentTime - lastSoundTime > SILENCE_TIMEOUT) || 
             (recorded_bytes >= (MAX_RECORDING_SIZE + WAV_HEADER_SIZE - 1024)))) {
            
            Serial.printf("🎤 Core 0: Recording complete! %d bytes\n", recorded_bytes);
            
            if (recording_buffer && recorded_bytes > WAV_HEADER_SIZE) {
                // Convert to base64 and store for Core 1
                String audioB64 = base64::encode(recording_buffer, recorded_bytes);
                
                if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    detectedAudioData = audioB64;
                    speechDetected = true;
                    Serial.printf("🎤 Core 0: Audio ready for transmission (%d chars base64)\n", audioB64.length());
                    xSemaphoreGive(audioMutex);
                }
            }
            
            // Clean up
            if (recording_buffer) {
                free(recording_buffer);
                recording_buffer = NULL;
            }
            recorded_bytes = 0;
            currentlyRecording = false;
            speechStartTime = 0;
        }
        
        // FIX 3: Increase VAD delay from 1ms to 10ms (no quality loss, big power savings)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

SensorData readAllSensors() {
    SensorData data = {0};
    
    if (mpuAvailable) {
        // Read actual sensor data from MPU6050
        int16_t ax, ay, az;
        mpu.getAcceleration(&ax, &ay, &az);
        data.accel_x = ax / 16384.0 * 9.81;
        data.accel_y = ay / 16384.0 * 9.81;
        data.accel_z = az / 16384.0 * 9.81;
        
        int16_t gx, gy, gz;
        mpu.getRotation(&gx, &gy, &gz);
        data.gyro_x = gx / 131.0;
        data.gyro_y = gy / 131.0;
        data.gyro_z = gz / 131.0;
    } else {
        // MPU6050 not available - send zero values with error indication
        data.accel_x = 0.0;
        data.accel_y = 0.0;
        data.accel_z = 0.0;
        data.gyro_x = 0.0;
        data.gyro_y = 0.0;
        data.gyro_z = 0.0;
        Serial.println("⚠️  MPU6050 not available - sending zero values");
    }
    
    // Store calibrated accelerometer values (server will compute direction)
    data.calibrated_ax = data.accel_x;
    data.calibrated_ay = data.accel_y; 
    data.calibrated_az = data.accel_z;
    data.device_orientation = "COMPUTING";  // Placeholder - server will compute
    data.orientation_confidence = 0.0;       // Placeholder - server will compute
    
    // Use real microphone energy level from audio monitoring
    if (micReady && audioEnergyLevel > 0) {
        // Convert audio energy to approximate dB level
        data.mic_level = 20.0 + (audioEnergyLevel / 100.0); // Scale energy to dB range
        data.sound_data = audioEnergyLevel;
    } else {
        // Microphone not ready or no audio data
        data.mic_level = 0.0;
        data.sound_data = 0;
    }
    
    // Initialize image/audio fields
    data.camera_image_b64 = "";
    data.audio_data_b64 = "";
    data.has_new_image = false;
    data.has_new_audio = false;
    
    return data;
}

// ================= SERVER HEALTH CHECK =================
bool isServerAlive() {
    HTTPClient http;
    http.setConnectTimeout(5000);  // Increased from 2000 to 5000ms
    http.setTimeout(8000);         // Increased from 2000 to 8000ms

    if (!http.begin("https://kakuproject-90943350924.asia-south1.run.app/api/health")) {
        return false;
    }

    int code = http.GET();
    http.end();

    return (code == 200);
}

// ================= OLED DISPLAY ANIMATION POLLING =================
void getOLEDDisplayFromServer() {
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);

    if (!http.begin(oledDisplayUrl)) {
        return;  // Silently fail, keep showing current animation
    }

    int httpCode = http.GET();
    
    if (httpCode == 200) {
        String response = http.getString();
        
        // Parse JSON response with multiple fields from server
        DynamicJsonDocument doc(768);
        DeserializationError error = deserializeJson(doc, response);
        
        if (!error) {
            // Handle animation_id
            if (doc.containsKey("animation_id")) {
                int newAnimationId = doc["animation_id"].as<int>();
                String animationName = doc["animation_name"] | "UNKNOWN";
                
                if (newAnimationId >= 0 && newAnimationId <= 3) {
                    if ((int)petAge != newAnimationId) {
                        petAge = (PetAge)newAnimationId;
                        Serial.printf("🎬 Animation: %s (id: %d)\n", animationName.c_str(), newAnimationId);
                    }
                }
            }
            
            // NEW: Handle screen_type / screen_state
            if (doc.containsKey("screen_type")) {
                String newScreenType = doc["screen_type"].as<String>();
                if (currentScreenType == "FOOD_MENU" && newScreenType != "FOOD_MENU") {
                    imageAlreadySentThisSession = false;  // Reset flag when leaving FOOD_MENU
                }
                currentScreenType = newScreenType;
                Serial.printf("📺 Screen Type: %s\n", currentScreenType.c_str());
            } else if (doc.containsKey("screen_state")) {
                String newScreenState = doc["screen_state"].as<String>();
                if (currentScreenType == "FOOD_MENU" && newScreenState != "FOOD_MENU") {
                    imageAlreadySentThisSession = false;  // Reset flag when leaving FOOD_MENU
                }
                currentScreenType = newScreenState;
                Serial.printf("📺 Screen State: %s\n", currentScreenType.c_str());
            }
            
            // NEW: Handle show_home_icon flag
            if (doc.containsKey("show_home_icon")) {
                bool newShowIcon = doc["show_home_icon"].as<bool>();
                if (showHomeIcon != newShowIcon) {
                    showHomeIcon = newShowIcon;
                    Serial.printf("🏠 Home Icon: %s\n", showHomeIcon ? "SHOW" : "HIDE");
                }
            }
            
            // NEW: Handle show_food_icon flag
            if (doc.containsKey("show_food_icon")) {
                bool newShowFood = doc["show_food_icon"].as<bool>();
                if (showFoodIcon != newShowFood) {
                    showFoodIcon = newShowFood;
                    Serial.printf("🍽️  Food Icon: %s\n", showFoodIcon ? "SHOW" : "HIDE");
                }
            }
            
            // NEW: Handle show_poop_icon flag
            if (doc.containsKey("show_poop_icon")) {
                showPoopIcon = doc["show_poop_icon"].as<bool>();
                Serial.printf("💩 Poop Icon: %s\n", showPoopIcon ? "SHOW" : "HIDE");
            }
            
            // NEW: Handle mode (AUTOMATIC/MANUAL)
            if (doc.containsKey("mode")) {
                currentMode = doc["mode"].as<String>();
                Serial.printf("🤖 Mode: %s\n", currentMode.c_str());
            }
            
            // NEW: Handle hunger status for conditional camera send
            if (doc.containsKey("is_hungry")) {
                petIsHungry = doc["is_hungry"].as<bool>();
                Serial.printf("🍽️  Hungry: %s\n", petIsHungry ? "YES" : "NO");
            }
            
            // NEW: Handle current_emotion to trigger eating animation on FOOD_MENU
            if (doc.containsKey("current_emotion")) {
                String emotion = doc["current_emotion"].as<String>();
                if (emotion == "EATING" && currentScreenType == "FOOD_MENU") {
                    Serial.println("😋 Emotion: EATING - triggering animation on FOOD MENU!");
                    playEatingAnimation();  // Play 5-frame Pacman eating
                    justFinishedEating = true;  // Set flag to show GOOD text
                    eatingFinishTime = millis();  // Record finish time
                }
            }
            
            // NEW: Handle current_menu (maps to currentScreenType)
            if (doc.containsKey("current_menu")) {
                String menu = doc["current_menu"].as<String>();
                if (currentScreenType != menu) {
                    currentScreenType = menu;
                    Serial.printf("📱 Menu Changed: %s\n", currentScreenType.c_str());
                }
            }
        }
    }
    
    http.end();
}

// ================= STARTUP COMPLETE NOTIFICATION =================
void notifyServerStartupComplete() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️  WiFi not connected, cannot notify server");
        return;
    }
    
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    
    const char* startupUrl = "https://kakuproject-90943350924.asia-south1.run.app/api/device/startup-complete";
    
    if (!http.begin(startupUrl)) {
        Serial.println("❌ Failed to connect for startup notification");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Create startup notification payload
    DynamicJsonDocument doc(256);
    doc["device_id"] = "ESP32_001";
    doc["status"] = "startup_complete";
    doc["timestamp"] = millis();
    doc["pet_stage"] = petAge;  // Send current pet stage
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("📤 Notifying server: Startup complete (payload: %d bytes)\n", payload.length());
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
        String response = http.getString();
        Serial.println("✅ Server acknowledged startup!");
        
        // Parse response - server might send initial state
        DynamicJsonDocument responseDoc(768);
        DeserializationError error = deserializeJson(responseDoc, response);
        
        if (!error) {
            if (responseDoc.containsKey("animation_id")) {
                int animId = responseDoc["animation_id"].as<int>();
                petAge = (PetAge)animId;
                Serial.printf("   Initial animation set to: %d\n", animId);
            }
            if (responseDoc.containsKey("show_home_icon")) {
                showHomeIcon = responseDoc["show_home_icon"].as<bool>();
                Serial.printf("   Home icon: %s\n", showHomeIcon ? "ENABLED" : "DISABLED");
            }
            if (responseDoc.containsKey("show_food_icon")) {
                showFoodIcon = responseDoc["show_food_icon"].as<bool>();
                Serial.printf("   Food icon: %s\n", showFoodIcon ? "ENABLED" : "DISABLED");
            }
        }
    } else {
        Serial.printf("⚠️  Server response: %d\n", httpCode);
    }
    
    http.end();
}

// ================= CAMERA FUNCTIONS =================
String captureImageBase64() {
    Serial.println("📸 Capturing image...");
    
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("❌ Camera capture failed");
        return "";
    }
    
    // Convert to base64
    String imageB64 = base64::encode(fb->buf, fb->len);
    
    Serial.printf("✅ Image captured: %d bytes → %d chars base64\n", fb->len, imageB64.length());
    
    esp_camera_fb_return(fb);
    return imageB64;
}

// ================= AUDIO FUNCTIONS =================
// Note: Audio recording is now handled by audioMonitorTask on Core 0
// This function is kept for compatibility but not used in dual-core mode

String recordAudioBase64() {
    Serial.println("⚠️  recordAudioBase64() called, but using VAD on Core 0 instead");
    return "";  // Return empty - audio handled by voice detection
}

// ================= UNIFIED DATA TRANSMISSION =================
bool sendSensorDataOnly(SensorData data) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("❌ WiFi not connected, skipping send\n");
        return false;
    }
    
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(10000);
    
    if (!http.begin(serverUrl)) {
        Serial.println("❌ Failed to begin HTTP connection");
        return false;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Larger JSON payload - sensor data + batch of readings
    StaticJsonDocument<4096> jsonDoc;
    
    jsonDoc["accel_x"] = data.accel_x;
    jsonDoc["accel_y"] = data.accel_y;
    jsonDoc["accel_z"] = data.accel_z;
    jsonDoc["gyro_x"] = data.gyro_x;
    jsonDoc["gyro_y"] = data.gyro_y;
    jsonDoc["gyro_z"] = data.gyro_z;
    jsonDoc["mic_level"] = data.mic_level;
    jsonDoc["sound_data"] = data.sound_data;
    
    // Add sensor batch with all buffered readings
    JsonObject batchObj = jsonDoc.createNestedObject("sensor_batch");
    batchObj["reading_count"] = data.sensor_batch.reading_count;
    batchObj["avg_mic_level"] = data.sensor_batch.avg_mic_level;
    batchObj["sound_data"] = data.sensor_batch.sound_data;
    
    // Serialize all readings in the batch
    JsonArray readingsArray = batchObj.createNestedArray("readings");
    for (int i = 0; i < data.sensor_batch.reading_count; i++) {
        JsonObject readingObj = readingsArray.createNestedObject();
        readingObj["timestamp_ms"] = data.sensor_batch.readings[i].timestamp_ms;
        readingObj["accel_x"] = data.sensor_batch.readings[i].accel_x;
        readingObj["accel_y"] = data.sensor_batch.readings[i].accel_y;
        readingObj["accel_z"] = data.sensor_batch.readings[i].accel_z;
        readingObj["gyro_x"] = data.sensor_batch.readings[i].gyro_x;
        readingObj["gyro_y"] = data.sensor_batch.readings[i].gyro_y;
        readingObj["gyro_z"] = data.sensor_batch.readings[i].gyro_z;
    }
    
    String payload;
    serializeJson(jsonDoc, payload);
    
    Serial.printf("📊 Sending sensor data: %d bytes\n", payload.length());
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
        Serial.println("✅ Sensor data sent!");
        Serial.printf("    Accel: X=%.2f, Y=%.2f, Z=%.2f m/s²\n", 
                     data.accel_x, data.accel_y, data.accel_z);
        Serial.printf("    Gyro:  X=%.2f, Y=%.2f, Z=%.2f °/s\n", 
                     data.gyro_x, data.gyro_y, data.gyro_z);
        Serial.printf("    Orient: %s (%.1f%% confidence)\n", 
                     data.device_orientation.c_str(), data.orientation_confidence);
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite(LED_PIN, LOW);
    } else {
        Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
    return (httpCode == 200);
}

void sendImageData(String imageBase64) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️ WiFi not connected");
        return;
    }

    isUploadingImage = true;
    
    // Get binary data from Core 0 with mutex protection
    uint8_t* binary_data = NULL;
    size_t data_length = 0;
    
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (capturedImageBuffer != NULL && capturedImageLength > 0) {
            // Copy data for sending
            binary_data = (uint8_t*)malloc(capturedImageLength);
            if (binary_data) {
                memcpy(binary_data, capturedImageBuffer, capturedImageLength);
                data_length = capturedImageLength;
            }
            
            // Free Core 0 buffer
            free(capturedImageBuffer);
            capturedImageBuffer = NULL;
            capturedImageLength = 0;
            cameraImageReady = false;
        }
        xSemaphoreGive(cameraMutex);
    }
    
    if (!binary_data || data_length == 0) {
        Serial.println("⚠️ No image data to send");
        isUploadingImage = false;
        return;
    }

    Serial.printf("🖼️ Sending image: %d bytes (raw binary from Core 0)\n", data_length);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(30000);
    http.setConnectTimeout(10000);

    if (!http.begin(client, "https://kakuproject-90943350924.asia-south1.run.app/upload")) {
        Serial.println("❌ HTTP begin failed");
        free(binary_data);
        isUploadingImage = false;
        return;
    }

    http.addHeader("Content-Type", "application/octet-stream");

    int httpCode = http.sendRequest("POST", binary_data, data_length);

    if (httpCode == 200) {
        Serial.println("✅ Image uploaded successfully");
        
        // NEW: Trigger "eating finished" state to show GOOD! text
        // Image upload = feeding complete (the frame IS the food)
        if (currentScreenType == "FOOD_MENU") {
            justFinishedEating = true;
            eatingFinishTime = millis();
            Serial.println("🎉 Feeding complete! Showing GOOD! text...");
        }
    } else {
        Serial.printf("❌ Upload failed: %d (%s)\n",
                      httpCode,
                      http.errorToString(httpCode).c_str());
    }

    http.end();
    free(binary_data);

    isUploadingImage = false;
}

void sendAudioData(String audioBase64) {
    if (WiFi.status() != WL_CONNECTED) return;
    
    if (audioBase64.length() == 0) {
        Serial.println("⚠️  Audio data empty, skipping");
        return;
    }
    
    Serial.printf("🎵 Sending audio: %d bytes base64\n", audioBase64.length());
    
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(15000);  // Shorter timeout for smaller audio
    
    if (!http.begin("https://kakuproject-90943350924.asia-south1.run.app/upload-audio")) {
        Serial.println("❌ Failed to connect to audio server");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Build JSON manually to avoid buffer overflow
    String payload = "{\"audio_data\":\"";
    payload += audioBase64;
    payload += "\"}";
    
    Serial.printf("📨 Payload size: %d bytes\n", payload.length());
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
        Serial.println("✅ Audio data sent!");
    } else {
        Serial.printf("❌ Audio send failed: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
    }
    
    http.end();
    vTaskDelay(pdMS_TO_TICKS(200));  // Server breathing room after audio upload (non-blocking)
}

void sendAllDataToServer(SensorData data) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("❌ WiFi not connected (status=%d), skipping data send\n", WiFi.status());
        return;
    }
    
    Serial.printf("🌐 Connecting to server: %s\n", serverUrl);
    
    HTTPClient http;
    http.setConnectTimeout(10000);  // 10 second connection timeout
    http.setTimeout(15000);          // 15 second read timeout (for large payloads)
    
    if (!http.begin(serverUrl)) {
        Serial.println("❌ Failed to begin HTTP connection");
        return;
    }
    
    http.addHeader("Content-Type", "application/json");
    
    // Create comprehensive JSON payload
    StaticJsonDocument<8192> jsonDoc;  // Increased size for image/audio data
    
    // Basic sensor data
    jsonDoc["accel_x"] = data.accel_x;
    jsonDoc["accel_y"] = data.accel_y;
    jsonDoc["accel_z"] = data.accel_z;
    jsonDoc["gyro_x"] = data.gyro_x;
    jsonDoc["gyro_y"] = data.gyro_y;
    jsonDoc["gyro_z"] = data.gyro_z;
    jsonDoc["mic_level"] = data.mic_level;
    jsonDoc["sound_data"] = data.sound_data;
    
    // Metadata
    jsonDoc["timestamp"] = millis();
    jsonDoc["device_id"] = "esp32_xiao_s3";
    
    // Add image data if available
    if (data.has_new_image && !data.camera_image_b64.isEmpty()) {
        jsonDoc["camera_image"] = data.camera_image_b64;
        Serial.println("📸 Including image data in payload");
    }
    
    // Add audio data if available  
    if (data.has_new_audio && !data.audio_data_b64.isEmpty()) {
        jsonDoc["audio_data"] = data.audio_data_b64;
        Serial.println("🎵 Including audio data in payload");
    }
    
    String payload;
    serializeJson(jsonDoc, payload);
    
    Serial.printf("\n📊 Sending data: %d bytes\n", payload.length());
    Serial.printf("    Sensors: ✅ | Image: %s | Audio: %s\n",
                  data.has_new_image ? "✅" : "⬜",
                  data.has_new_audio ? "✅" : "⬜");
    Serial.println("⏳ Waiting for server response...");
    
    int httpCode = http.POST(payload);
    
    if (httpCode > 0) {
        Serial.printf("📤 POST Response: %d\n", httpCode);
        if (httpCode == 200) {
            Serial.println("✅ All data sent successfully!");
            Serial.printf("    Accel: X=%.2f, Y=%.2f, Z=%.2f m/s²\n", 
                         data.accel_x, data.accel_y, data.accel_z);
            Serial.printf("    Gyro:  X=%.2f, Y=%.2f, Z=%.2f °/s\n", 
                         data.gyro_x, data.gyro_y, data.gyro_z);
            Serial.printf("    Mic:   %.1f dB\n", data.mic_level);
            
            // Success LED blink
            digitalWrite(LED_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(100));
            digitalWrite(LED_PIN, LOW);
        } else {
            Serial.printf("❌ Server error: %s\n", http.getString().c_str());
        }
    } else {
        Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
}

// WAV Header generation function
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate) {
    uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
    uint32_t byte_rate = sample_rate * SAMPLE_BITS / 8;

    const uint8_t header[] = {
        'R','I','F','F',
        file_size, file_size >> 8, file_size >> 16, file_size >> 24,
        'W','A','V','E','f','m','t',' ',
        0x10,0x00,0x00,0x00,
        0x01,0x00,
        0x01,0x00,
        sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24,
        byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24,
        0x02,0x00,
        0x10,0x00,
        'd','a','t','a',
        wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24,
    };

    memcpy(wav_header, header, sizeof(header));
}

// ================= EVENT POLLING FUNCTIONS =================
void pollForEvents() {
    HTTPClient http;
    
    Serial.println("🔍 Polling for important events...");
    
    if (!http.begin(eventsUrl)) {
        Serial.println("❌ Failed to initialize HTTP client for events");
        return;
    }
    
    // Set timeout
    http.setTimeout(5000);
    
    // Add headers
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "ESP32-Dashboard/2.0");
    
    // Make GET request
    int httpCode = http.GET();
    
    if (httpCode > 0) {
        Serial.printf("📡 Server response: %d\n", httpCode);
        
        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            
            if (response.length() > 0) {
                Serial.println("📋 Server Response:");
                Serial.println("-------------------");
                
                // Parse JSON response
                DynamicJsonDocument doc(2048);
                DeserializationError error = deserializeJson(doc, response);
                
                if (!error) {
                    if (doc.containsKey("events")) {
                        JsonArray events = doc["events"];
                        
                        if (events.size() > 0) {
                            Serial.printf("🚨 FOUND %d IMPORTANT EVENT(S):\n", events.size());
                            Serial.println("========================================");
                            
                            for (int i = 0; i < events.size(); i++) {
                                JsonObject event = events[i];
                                
                                int event_id = event["id"].as<int>();
                                const char* event_type = event["event_type"];
                                const char* message = event["message"];
                                const char* created_at = event["created_at"];
                                
                                Serial.printf("   🚨 EVENT #%d:\n", i + 1);
                                Serial.printf("     ID: %d\n", event_id);
                                Serial.printf("     Type: %s\n", event_type);
                                Serial.printf("     Message: %s\n", message);
                                Serial.printf("     Time: %s\n", created_at);
                                Serial.println();
                                
                                // Process the event
                                processEvent(event_type, message);
                                
                                // Acknowledge event received
                                acknowledgeEvent(event_id);
                                
                                vTaskDelay(pdMS_TO_TICKS(100));  // Small delay between events
                            }
                        } else {
                            Serial.println("✅ No new important events (all quiet)");
                        }
                    }
                    
                    if (doc.containsKey("message")) {
                        Serial.printf("💬 Status: %s\n", doc["message"].as<const char*>());
                    }
                } else {
                    Serial.println("❌ JSON parsing error");
                    Serial.println(response);
                }
                
                Serial.println("-------------------");
            } else {
                Serial.println("✅ Empty response (no events)");
            }
        } else {
            Serial.printf("⚠️ Unexpected response code: %d\n", httpCode);
        }
    } else {
        Serial.printf("❌ HTTP request failed: %s\n", http.errorToString(httpCode).c_str());
        Serial.println("   Server might be down or unreachable");
    }
    
    http.end();
    Serial.println("");
}

void processEvent(const char* event_type, const char* message) {
    Serial.println("🔧 Processing event...");
    
    // Simple event processing - you can extend this based on your needs
    if (strcmp(event_type, "high_sound") == 0) {
        Serial.println("🔊 High sound detected - might want to take action!");
        digitalWrite(LED_PIN, HIGH);  // Turn on LED for high sound
        vTaskDelay(pdMS_TO_TICKS(200));
        digitalWrite(LED_PIN, LOW);   // Blink LED
    }
    else if (strcmp(event_type, "sudden_motion") == 0) {
        Serial.println("🏃 Sudden motion detected - something's happening!");
        // Blink LED multiple times for motion
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(100));
            digitalWrite(LED_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    else if (strcmp(event_type, "alert") == 0) {
        Serial.println("⚠️ Generic alert received!");
        digitalWrite(LED_PIN, HIGH);  // Solid LED for alert
        vTaskDelay(pdMS_TO_TICKS(500));
        digitalWrite(LED_PIN, LOW);
    }
    else {
        Serial.printf("❓ Unknown event type: %s\n", event_type);
    }
    
    Serial.printf("   📝 Event message: %s\n", message);
}

void acknowledgeEvent(int event_id) {
    HTTPClient http;
    
    Serial.printf("📤 Acknowledging event #%d...\n", event_id);
    
    if (!http.begin(eventReceivedUrl)) {
        Serial.println("❌ Failed to initialize HTTP client for acknowledgment");
        return;
    }
    
    // Set timeout and headers
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "ESP32-Dashboard/2.0");
    
    // Create JSON payload
    DynamicJsonDocument doc(256);
    doc["device_id"] = "ESP32_001";
    doc["event_id"] = event_id;
    doc["status"] = "received";
    doc["timestamp"] = millis();  // Simple timestamp
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Send POST request
    int httpCode = http.POST(jsonString);
    
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK || httpCode == 200) {
            String response = http.getString();
            Serial.printf("✅ Event acknowledged successfully: %s\n", response.c_str());
        } else {
            Serial.printf("⚠️ Acknowledgment response: %d\n", httpCode);
        }
    } else {
        Serial.printf("❌ Acknowledgment failed: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
}

/*
WIRING DIAGRAM for XIAO ESP32 S3 Sense: 
===========================================

🔌 Built-in Components (No Wiring Needed):
- Camera Module (OV2640) - Built into XIAO ESP32 S3 Sense
- PDM Microphone - Built into XIAO ESP32 S3 Sense  
- PSRAM - Built into XIAO ESP32 S3 Sense

🔧 External Components to Connect:

MPU6050 (Accelerometer/Gyroscope):
- VCC -> 3.3V
- GND -> GND  
- SDA -> GPIO 21 (I2C Data)
- SCL -> GPIO 22 (I2C Clock)
- INT -> Not connected (optional)

LED Indicator:
- + -> GPIO 2 (with 220Ω resistor)
- - -> GND

📋 PIN ASSIGNMENTS (XIAO ESP32 S3 Sense):
==========================================

Camera Pins (Built-in OV2640):
- XCLK  -> GPIO 10
- SIOD  -> GPIO 40 (I2C SDA)
- SIOC  -> GPIO 39 (I2C SCL) 
- Y9    -> GPIO 48
- Y8    -> GPIO 11
- Y7    -> GPIO 12
- Y6    -> GPIO 14
- Y5    -> GPIO 16
- Y4    -> GPIO 18
- Y3    -> GPIO 17
- Y2    -> GPIO 15
- VSYNC -> GPIO 38
- HREF  -> GPIO 47
- PCLK  -> GPIO 13

I2S PDM Microphone (Built-in):
- CLK -> GPIO 42 (I2S WS)
- DATA-> GPIO 41 (I2S SD)

MPU6050 (External):
- SDA -> GPIO 5  (XIAO ESP32 S3)
- SCL -> GPIO 6  (XIAO ESP32 S3)
  
Alternative I2C pins if GPIO 6/7 don't work:
- SDA -> GPIO 21, SCL -> GPIO 22 (Generic ESP32)
- Check your specific board's pinout diagram

Other:
- LED -> GPIO 2

⚠️  IMPORTANT NOTES:
===================
1. XIAO ESP32 S3 Sense has BUILT-IN camera and microphone
2. No external camera/mic wiring needed
3. Only connect MPU6050 externally via I2C
4. Camera uses JPEG compression for efficient transmission
5. 🎤 SMART AUDIO: Voice Activity Detection with dual-core processing
6. All data sent to dashboard via WiFi

🚀 FEATURES:
============
- ✅ MPU6050 sensor data (every 1 second) - Core 1
- ✅ Camera images (every 7 seconds) - Core 1
- ✅ 🎤 SMART AUDIO: Voice Activity Detection - Core 0
- ✅ Dual-core processing for optimal performance
- ✅ Real-time dashboard updates
- ✅ WiFi connectivity with auto-reconnect
- ✅ LED status indicators
- ✅ PSRAM for audio/image buffering

🧠 DUAL-CORE ARCHITECTURE:
==========================
**Core 0 (Audio Core):**
- Continuous microphone monitoring
- Voice Activity Detection (VAD)
- Energy-based speech detection
- Automatic audio recording when speech detected
- Real-time audio processing (high priority)

**Core 1 (Main Core):**
- WiFi management and HTTP transmission
- Sensor data collection (MPU6050)
- Camera image capture
- Dashboard communications
- LED status indicators

🎤 INTELLIGENT AUDIO SYSTEM:
============================
**Voice Activity Detection:**
- Continuously monitors audio energy levels
- Detects speech when energy > 1000 threshold
- Requires minimum 500ms of continuous speech
- Records up to 5 seconds of audio
- Stops recording after 2 seconds of silence
- Only transmits audio when speech is detected

**Energy-Based Detection:**
```
Audio Energy > 1000    → Speech Detected 🗣️
Audio Energy < 1000    → Silent 🔇
Continuous Speech 500ms+ → Start Recording 🎙️
Silence 2000ms+ → Stop Recording & Send 📤
```

**Benefits:**
- ⚡ No bandwidth wasted on silent audio
- 🔋 Power efficient - only records when needed
- 📡 Real-time speech detection and transmission
- 🎯 High accuracy voice activity detection
- 🚀 Multi-core performance optimization

📊 DATA TRANSMISSION SCHEDULE:
==============================
**Real-time (Core 1):**
- Sensor readings: Every 1 second (always)
- Camera images: Every 7 seconds

**Event-driven (Core 0 → Core 1):**
- Audio: Only when speech detected
- Voice detection: Continuous monitoring
- Inter-core communication via mutex/semaphores

🔧 SMART THRESHOLDS:
===================
- VAD_THRESHOLD: 1000 (adjust based on environment)
- VAD_MIN_DURATION: 500ms (minimum speech length)
- SILENCE_TIMEOUT: 2000ms (stop recording delay)
- MAX_RECORDING: 5 seconds (prevent buffer overflow)

🎛️ TUNING VOICE DETECTION:
===========================
**Quiet Environment:** Lower VAD_THRESHOLD to 500-800
**Noisy Environment:** Raise VAD_THRESHOLD to 1500-2000
**Sensitive Detection:** Decrease VAD_MIN_DURATION to 300ms
**Less False Triggers:** Increase VAD_MIN_DURATION to 800ms

💾 MEMORY USAGE:
===============
- Images: ~1-3KB (QQVGA 160x120, quality 20)
- Audio: ~32KB per 1-second recording (only when speech)
- VAD Buffer: 1KB for real-time energy analysis
- PSRAM: Dynamic allocation for recordings
- Automatic cleanup after transmission

🌐 NETWORK ENDPOINTS:
====================
- Sensors: POST /api/sensor-data (small, frequent)
- Images: POST /upload (binary, every 7s)
- Audio: POST /upload-audio (JSON, speech-triggered)

⚡ PERFORMANCE BENEFITS:
========================
1. **Dual-Core**: Audio processing doesn't block main operations
2. **Event-Driven**: Audio only sent when speech detected
3. **Energy-Efficient**: No continuous audio transmission
4. **Real-Time**: Voice detection with minimal latency
5. **Intelligent**: Automatic silence detection and recording stop

🔧 CONFIGURATION:
=================
Update these before uploading:
1. WIFI_SSID and WIFI_PASSWORD
2. Server URL: "https://kakuproject-90943350924.asia-south1.run.app"
3. Adjust VAD_THRESHOLD for your environment
4. Three separate optimized endpoints for different data types

**Data Sending Examples:**
- Sensor readings: Every 1 second (always) - 146 bytes JSON
- Camera images: Every 7 seconds - 1-3KB binary
- Audio recordings: Only when you speak - 32KB+ base64
- Silence periods: 0 bytes audio transmission ✨
*/
