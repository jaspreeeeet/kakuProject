/*
 * test_all_animations.ino
 * 
 * Displays every animation defined in all_pets.h one by one on the SSD1306 64x32 OLED.
 * Each animation plays for ~3 seconds, then the next one starts.
 * 
 * Hardware: XIAO ESP32 S3 Sense
 *   SDA → GPIO 5
 *   SCL → GPIO 6
 *   OLED: SSD1306, 64x32, I2C address 0x3C
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "all_pets.h"

// ── OLED setup ──────────────────────────────────────────────────
#define SCREEN_WIDTH  64
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── How long to show each animation (milliseconds) ──────────────
#define ANIM_SHOW_DURATION_MS 3000   // 3 s per animation
#define LABEL_PAUSE_MS        800    // 0.8 s to show name label

// ── Animation descriptor ─────────────────────────────────────────
struct Anim {
    const char*     name;
    const uint8_t*  frames;    // pointer to PROGMEM frame array
    uint8_t         frameCount;
    uint16_t        frameBytes; // bytes per frame (= W*H/8)
    const uint16_t* delays;    // per-frame delay array (ms)
    uint8_t         w;
    uint8_t         h;
    int8_t          x;         // draw position on 64x32 screen
    int8_t          y;
};

// ── Build the animation list ──────────────────────────────────────
//  Full-screen 64x32 → x=0, y=0, frameBytes=256
//  Small icon  24x12 → centred: x=(64-24)/2=20, y=(32-12)/2=10, frameBytes=48 (NOT 36!)*
//    *Note: home_icon and food_icon have 48 bytes; poop/play/heart/aid have 36 bytes (24x12/8×rows)
//     Actually 24x12 / 8 = 36 bytes — the header says 48 rows because data includes padding.
//     We store the real byte count in frameBytes and let drawBitmap use w,h.
//  clean_slide 50x25 → x=(64-50)/2=7, y=(32-25)/2=3, frameBytes=175

const Anim animations[] = {
    // ── INFANT (64x32) ────────────────────────────────────────────
    { "Infant Idle",     (const uint8_t*)infant_frames,          INFANT_FRAME_COUNT,          256, infant_delays,          64, 32,  0,  0 },
    { "Infant Happy",    (const uint8_t*)infant_happy_frames,    INFANT_HAPPY_FRAME_COUNT,    256, infant_happy_delays,    64, 32,  0,  0 },
    { "Infant Sad",      (const uint8_t*)infant_sad_frames,      INFANT_SAD_FRAME_COUNT,      256, infant_sad_delays,      64, 32,  0,  0 },
    { "Infant Angry",    (const uint8_t*)infant_angry_frames,    INFANT_ANGRY_FRAME_COUNT,    256, infant_angry_delays,    64, 32,  0,  0 },
    { "Infant Cry",      (const uint8_t*)infant_cry_frames,      INFANT_CRY_FRAME_COUNT,      256, infant_cry_delays,      64, 32,  0,  0 },
    { "Infant Surprise", (const uint8_t*)infant_surprise_frames, INFANT_SURPRISE_FRAME_COUNT, 256, infant_surprise_delays, 64, 32,  0,  0 },
    // ── CHILD (64x32) ─────────────────────────────────────────────
    { "Child Idle",      (const uint8_t*)child_frames,           CHILD_FRAME_COUNT,           256, child_delays,           64, 32,  0,  0 },
    { "Child Blink",     (const uint8_t*)child_blinking_idle,    CHILD_BLINKING_IDLE_FRAME_COUNT, 256, child_blinking_idle_delays, 64, 32, 0, 0 },
    { "Child Happy",     (const uint8_t*)happy_child,            HAPPY_CHILD_FRAME_COUNT,     256, happy_child_delays,     64, 32,  0,  0 },
    { "Child Sad",       (const uint8_t*)child_sad_frames,       CHILD_SAD_FRAME_COUNT,       256, child_sad_delays,       64, 32,  0,  0 },
    { "Child Angry",     (const uint8_t*)child_angry_frames,     CHILD_ANGRY_FRAME_COUNT,     256, child_angry_delays,     64, 32,  0,  0 },
    { "Child Cry",       (const uint8_t*)cry_child,              CRY_CHILD_FRAME_COUNT,       256, cry_child_delays,       64, 32,  0,  0 },
    { "Child Surprise",  (const uint8_t*)child_surprise_frames,  CHILD_SURPRISE_FRAME_COUNT,  256, child_surprise_delays,  64, 32,  0,  0 },
    // ── ADULT (64x32) ─────────────────────────────────────────────
    { "Adult Idle",      (const uint8_t*)adult_frames,           ADULT_FRAME_COUNT,           256, adult_delays,           64, 32,  0,  0 },
    { "Adult Happy",     (const uint8_t*)happy_adult,            HAPPY_ADULT_FRAME_COUNT,     256, happy_adult_delays,     64, 32,  0,  0 },
    { "Adult Sad",       (const uint8_t*)adult_sad_frames,       ADULT_SAD_FRAME_COUNT,       256, adult_sad_delays,       64, 32,  0,  0 },
    { "Adult Angry",     (const uint8_t*)angry_adult,            ANGRY_ADULT_FRAME_COUNT,     256, angry_adult_delays,     64, 32,  0,  0 },
    { "Adult Cry",       (const uint8_t*)cry_adult,              CRY_ADULT_FRAME_COUNT,       256, cry_adult_delays,       64, 32,  0,  0 },
    { "Adult Surprise",  (const uint8_t*)surprise_adult,         SURPRISE_ADULT_FRAME_COUNT,  256, surprise_adult_delays,  64, 32,  0,  0 },
    // ── OLD (64x32) ───────────────────────────────────────────────
    { "Old Idle",        (const uint8_t*)old_frames,             OLD_FRAME_COUNT,             256, old_delays,             64, 32,  0,  0 },
    { "Old Happy",       (const uint8_t*)old_happy,              OLD_HAPPY_FRAME_COUNT,       256, old_happy_delays,       64, 32,  0,  0 },
    { "Old Sad",         (const uint8_t*)old_sad,                OLD_SAD_FRAME_COUNT,         256, old_sad_delays,         64, 32,  0,  0 },
    { "Old Angry",       (const uint8_t*)old_angry,              OLD_ANGRY_FRAME_COUNT,       256, old_angry_delays,       64, 32,  0,  0 },
    { "Old Cry",         (const uint8_t*)old_cry,                OLD_CRY_FRAME_COUNT,         256, old_cry_delays,         64, 32,  0,  0 },
    { "Old Surprise",    (const uint8_t*)old_surprise,           OLD_SURPRISE_FRAME_COUNT,    256, old_surprise_delays,    64, 32,  0,  0 },
    // ── Egg & Actions (64x32) ─────────────────────────────────────
    { "Egg Crack",       (const uint8_t*)egg_crack_frames,       EGG_CRACK_FRAME_COUNT,       256, egg_crack_delays,       64, 32,  0,  0 },
    { "Eating",          (const uint8_t*)eating_frames,          EATING_FRAME_COUNT,          256, eating_delays,          64, 32,  0,  0 },
    { "Injection",       (const uint8_t*)injection_frames,       INJECTION_FRAME_COUNT,       256, injection_delays,       64, 32,  0,  0 },
    // ── Clean slide (50x25) ───────────────────────────────────────
    { "Clean Slide",     (const uint8_t*)clean_slide_frames,     CLEAN_SLIDE_FRAME_COUNT,     175, NULL,                   50, 25,  7,  3 },
    // ── Small icons (24x12) centred on 64x32 ─────────────────────
    { "Home Icon",       (const uint8_t*)home_icon_frames,       HOME_ICON_FRAME_COUNT,        48, home_icon_delays,       24, 12, 20, 10 },
    { "Food Icon",       (const uint8_t*)food_icon_frames,       FOOD_ICON_FRAME_COUNT,        48, food_icon_delays,       24, 12, 20, 10 },
    { "Poop Icon",       (const uint8_t*)poop_frames,            POOP_FRAME_COUNT,             36, poop_delays,            24, 12, 20, 10 },
    { "Play Icon",       (const uint8_t*)play_icon_frames,       PLAY_ICON_FRAME_COUNT,        36, play_icon_delays,       24, 12, 20, 10 },
    { "Heart Icon",      (const uint8_t*)heart_icon_frames,      HEART_ICON_FRAME_COUNT,       36, heart_icon_delays,      24, 12, 20, 10 },
    { "Aid Icon",        (const uint8_t*)aid_icon_frames,        AID_ICON_FRAME_COUNT,         36, aid_icon_delays,        24, 12, 20, 10 },
    // ── Toilet icon (static 24x12 — shown as 1 frame) ────────────
    { "Toilet Icon",     (const uint8_t*)toilet_icon,            1,                            48, NULL,                   24, 12, 20, 10 },
};

const uint8_t ANIM_COUNT = sizeof(animations) / sizeof(animations[0]);

// ── State ─────────────────────────────────────────────────────────
uint8_t  currentAnim  = 0;
uint8_t  currentFrame = 0;
uint32_t lastFrameMs  = 0;
uint32_t animStartMs  = 0;
bool     showingLabel = true;
uint32_t labelStartMs = 0;

// ── Helpers ───────────────────────────────────────────────────────
uint16_t frameDelay(const Anim& a, uint8_t frame) {
    if (a.delays) return a.delays[frame];
    return 150; // default 150 ms when no delay array provided
}

void showLabel(const char* name) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    // Centre the text horizontally (each char ~6px wide)
    int len = strlen(name);
    int x = max(0, (SCREEN_WIDTH - len * 6) / 2);
    display.setCursor(x, 12);
    display.print(name);
    display.display();
}

void drawFrame(const Anim& a, uint8_t frame) {
    // Each frame is stored row-by-row in PROGMEM; Adafruit drawBitmap reads PROGMEM directly
    const uint8_t* framePtr = a.frames + (uint32_t)frame * a.frameBytes;
    display.clearDisplay();
    display.drawBitmap(a.x, a.y, framePtr, a.w, a.h, SSD1306_WHITE);
    display.display();
}

// ── Arduino setup ─────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Wire.begin(5, 6); // SDA=5, SCL=6 for XIAO ESP32 S3

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("SSD1306 init failed!");
        while (true) delay(1000);
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.display();
    delay(500);

    Serial.printf("Total animations: %d\n", ANIM_COUNT);

    // Show label for first animation
    showLabel(animations[0].name);
    labelStartMs = millis();
    showingLabel = true;
}

// ── Arduino loop ──────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    const Anim& anim = animations[currentAnim];

    // ── Phase 1: show name label ──────────────────────────────────
    if (showingLabel) {
        if (now - labelStartMs >= LABEL_PAUSE_MS) {
            showingLabel = false;
            currentFrame = 0;
            lastFrameMs  = now;
            animStartMs  = now;
            drawFrame(anim, currentFrame);
        }
        return;
    }

    // ── Phase 2: play frames ──────────────────────────────────────
    uint16_t delay_ms = frameDelay(anim, currentFrame);

    if (now - lastFrameMs >= delay_ms) {
        lastFrameMs = now;
        currentFrame = (currentFrame + 1) % anim.frameCount;
        drawFrame(anim, currentFrame);
    }

    // ── Phase 3: after ANIM_SHOW_DURATION_MS, go to next anim ────
    if (now - animStartMs >= ANIM_SHOW_DURATION_MS) {
        currentAnim = (currentAnim + 1) % ANIM_COUNT;
        Serial.printf("[%d/%d] %s\n", currentAnim + 1, ANIM_COUNT, animations[currentAnim].name);
        showLabel(animations[currentAnim].name);
        labelStartMs = now;
        showingLabel = true;
    }
}
