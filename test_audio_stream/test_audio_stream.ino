/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  AUDIO STREAMING TEST — Standalone mic → server → STT      ║
  ║  Records continuously, sends WAV every 5 seconds            ║
  ║  Server forwards to ElevenLabs STT, returns transcription   ║
  ╚══════════════════════════════════════════════════════════════╝

  Hardware: XIAO ESP32 S3 Sense (onboard PDM mic)
  Pins: CLK=GPIO42, DATA=GPIO41
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "driver/i2s_pdm.h"

// ================= WIFI =================
#define WIFI_SSID     "Airtel_BumbleBee-777"
#define WIFI_PASSWORD "kya karoge ."

// ================= SERVER =================
const char* AUDIO_UPLOAD_URL = "https://kakuproject-90943350924.asia-south1.run.app/api/audio-stt";

// ================= AUDIO CONFIG =================
#define SAMPLE_RATE     16000
#define BUFFER_SIZE     512        // Samples per I2S read
#define MAX_AUDIO_SIZE  (SAMPLE_RATE * 2 * 6)  // ~6 seconds max buffer (192KB)
#define SEND_INTERVAL   5000       // Send every 5 seconds
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN     2          // Bit-shift amplification

// ================= VAD CONFIG =================
#define VAD_THRESHOLD   1200       // Energy threshold for voice detection

// ================= I2S PDM MIC PINS =================
#define PDM_CLK_GPIO    42
#define PDM_DIN_GPIO    41
#define I2S_NUM         I2S_NUM_0

// ================= GLOBALS =================
i2s_chan_handle_t rx_handle = NULL;
WiFiClientSecure secureClient;      // Persistent HTTPS client

uint8_t  *audio_buffer    = NULL;   // PSRAM recording buffer (raw PCM)
size_t    audio_size       = 0;      // Current bytes written
bool      has_voice_data   = false;  // At least one VAD-positive chunk in this window
unsigned long last_send_time = 0;
unsigned long chunk_count    = 0;    // Total chunks sent

// ================= WAV HEADER =================
void generate_wav_header(uint8_t* header, uint32_t data_size, uint32_t sample_rate)
{
  uint32_t file_size = data_size + 36;
  uint32_t byte_rate = sample_rate * 2;   // 16-bit mono

  const uint8_t wav[44] = {
    'R','I','F','F',
    (uint8_t)(file_size),      (uint8_t)(file_size>>8),
    (uint8_t)(file_size>>16),  (uint8_t)(file_size>>24),
    'W','A','V','E',
    'f','m','t',' ',
    16,0,0,0,               // SubChunk1Size (PCM)
    1,0,                     // AudioFormat (PCM)
    1,0,                     // NumChannels (Mono)
    (uint8_t)(sample_rate),      (uint8_t)(sample_rate>>8),
    (uint8_t)(sample_rate>>16),  (uint8_t)(sample_rate>>24),
    (uint8_t)(byte_rate),        (uint8_t)(byte_rate>>8),
    (uint8_t)(byte_rate>>16),    (uint8_t)(byte_rate>>24),
    2,0,                     // BlockAlign (2 bytes per sample)
    16,0,                    // BitsPerSample
    'd','a','t','a',
    (uint8_t)(data_size),      (uint8_t)(data_size>>8),
    (uint8_t)(data_size>>16),  (uint8_t)(data_size>>24)
  };

  memcpy(header, wav, 44);
}

// ================= VAD (from reference) =================
bool detect_voice(int16_t *samples, int count)
{
  long sum = 0;
  for (int i = 0; i < count; i++)
    sum += abs(samples[i]);

  int level = sum / count;
  return (level > VAD_THRESHOLD);
}

// ================= I2S INIT =================
bool init_i2s()
{
  i2s_chan_config_t chan_cfg =
    I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;

  if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK)
    return false;

  i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
                  I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = (gpio_num_t)PDM_CLK_GPIO,
      .din = (gpio_num_t)PDM_DIN_GPIO,
      .invert_flags = { .clk_inv = false },
    },
  };

  if (i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_cfg) != ESP_OK)
    return false;

  if (i2s_channel_enable(rx_handle) != ESP_OK)
    return false;

  return true;
}

// ================= WIFI CONNECT =================
void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED — will retry in loop");
  }
}

// ================= SEND AUDIO TO SERVER =================
// Sends raw WAV binary via multipart/form-data (server handles STT)
bool send_audio_to_server()
{
  if (audio_size < 1024) {
    Serial.println("⚠️ Audio too short, skipping");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected");
    return false;
  }

  // Build WAV in PSRAM
  size_t wav_size = audio_size + WAV_HEADER_SIZE;
  uint8_t *wav_data = (uint8_t*)ps_malloc(wav_size);
  if (!wav_data) {
    Serial.println("❌ Failed to allocate WAV buffer");
    return false;
  }

  generate_wav_header(wav_data, audio_size, SAMPLE_RATE);
  memcpy(wav_data + WAV_HEADER_SIZE, audio_buffer, audio_size);

  // Build multipart body
  String boundary = "----ESP32Audio";
  String body_start =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  String body_end = "\r\n--" + boundary + "--\r\n";

  size_t total_size = body_start.length() + wav_size + body_end.length();
  uint8_t *post_body = (uint8_t*)ps_malloc(total_size);

  if (!post_body) {
    Serial.println("❌ Failed to allocate POST buffer");
    free(wav_data);
    return false;
  }

  memcpy(post_body, body_start.c_str(), body_start.length());
  memcpy(post_body + body_start.length(), wav_data, wav_size);
  memcpy(post_body + body_start.length() + wav_size, body_end.c_str(), body_end.length());

  free(wav_data); // WAV data copied into post body

  // Send HTTP POST (HTTPS with no cert verification)
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  http.begin(secureClient, AUDIO_UPLOAD_URL);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  chunk_count++;
  Serial.printf("📤 Sending chunk #%lu | %d bytes PCM | %.1fs audio | voice=%s\n",
    chunk_count, audio_size, (float)audio_size / (SAMPLE_RATE * 2),
    has_voice_data ? "YES" : "no");

  int code = http.POST(post_body, total_size);
  String response = http.getString();

  free(post_body);
  http.end();

  if (code == 200) {
    // Parse server response for transcription
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err && doc.containsKey("text")) {
      const char* text = doc["text"];
      if (strlen(text) > 0) {
        Serial.printf("🗣️ STT: \"%s\"\n", text);
      } else {
        Serial.println("🔇 (silence — no speech detected)");
      }
    } else {
      Serial.printf("✅ Sent OK | Server: %s\n", response.c_str());
    }
    return true;
  } else {
    Serial.printf("❌ HTTP %d: %s\n", code, response.c_str());
    return false;
  }
}

// ================= SETUP =================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║  AUDIO STREAMING TEST v1.0       ║");
  Serial.println("║  5s chunks → Server → STT        ║");
  Serial.println("╚══════════════════════════════════╝\n");

  // Check PSRAM
  if (!psramFound()) {
    Serial.println("❌ PSRAM not found! Cannot run.");
    while (1) delay(1000);
  }
  Serial.printf("✅ PSRAM: %d KB free\n", ESP.getFreePsram() / 1024);

  // Allocate audio buffer in PSRAM
  audio_buffer = (uint8_t*)ps_malloc(MAX_AUDIO_SIZE);
  if (!audio_buffer) {
    Serial.println("❌ PSRAM allocation failed!");
    while (1) delay(1000);
  }

  // Connect WiFi
  connectWiFi();

  // Setup HTTPS (skip certificate verification for Cloud Run)
  secureClient.setInsecure();

  // Init I2S PDM mic
  if (!init_i2s()) {
    Serial.println("❌ I2S init failed!");
    while (1) delay(1000);
  }

  Serial.println("✅ PDM Microphone ready");
  Serial.printf("🎤 Config: %d Hz, 16-bit mono, VAD threshold=%d\n",
    SAMPLE_RATE, VAD_THRESHOLD);
  Serial.printf("📡 Server: %s\n", AUDIO_UPLOAD_URL);
  Serial.printf("⏱️ Send interval: %d ms\n", SEND_INTERVAL);
  Serial.println("════════════════════════════════════\n");

  last_send_time = millis();
}

// ================= LOOP =================
void loop()
{
  // --- WiFi reconnect ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi lost, reconnecting...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      delay(2000);
      return;
    }
  }

  // --- Read mic samples ---
  uint8_t buffer[BUFFER_SIZE * 2]; // BUFFER_SIZE samples × 2 bytes each
  size_t bytes_read = 0;

  esp_err_t err = i2s_channel_read(rx_handle, buffer, sizeof(buffer),
                                   &bytes_read, pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytes_read == 0)
    return;

  int16_t *samples = (int16_t*)buffer;
  int sample_count = bytes_read / 2;

  // --- VAD check ---
  bool voice = detect_voice(samples, sample_count);
  if (voice) has_voice_data = true;

  // --- Apply volume gain and append to buffer ---
  if (audio_size + bytes_read < MAX_AUDIO_SIZE) {
    for (int i = 0; i < sample_count; i++) {
      int32_t amp = (int32_t)samples[i] << VOLUME_GAIN;
      if (amp > 32767)  amp = 32767;
      if (amp < -32768) amp = -32768;
      int16_t out = (int16_t)amp;
      memcpy(audio_buffer + audio_size, &out, 2);
      audio_size += 2;
    }
  }

  // --- Every 5 seconds: send & reset ---
  if (millis() - last_send_time >= SEND_INTERVAL) {
    if (audio_size > 0) {
      Serial.printf("\n⏰ 5s window complete | %d bytes | voice_detected=%s\n",
        audio_size, has_voice_data ? "YES" : "NO");

      // Send to server (server decides whether to STT or skip)
      send_audio_to_server();

      // Clear buffer for next window
      audio_size = 0;
      has_voice_data = false;
      Serial.println("🗑️ PSRAM buffer cleared\n");
    } else {
      Serial.println("⏰ 5s window — no audio data");
    }
    last_send_time = millis();
  }
}
