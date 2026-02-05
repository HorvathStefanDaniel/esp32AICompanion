#ifndef AI_RELAY_WEBSOCKET_CONFIG_H
#define AI_RELAY_WEBSOCKET_CONFIG_H

// ======================= PINS =======================
// Speaker (MAX98357A)
#define I2S_SPK_BCLK  5
#define I2S_SPK_LRC   4
#define I2S_SPK_DIN   6

// Mic (INMP441)
#define I2S_MIC_SCK   12
#define I2S_MIC_WS    13
#define I2S_MIC_SD    14

#define BOOT_BUTTON   0
#define USE_BOOT_BUTTON 0
#define USE_EXT_BUTTON 1
#define BUTTON_IN    16
#define BUTTON_GND   18
#define PIN_RED       1
#define PIN_GREEN     2

// ======================= AUDIO =======================
#define SAMPLE_RATE 16000
#define RECORD_TIME_SECONDS 3
#define STT_RESPONSE_TIMEOUT_MS 20000
#define FRAME_MS 200
#define FRAME_SAMPLES (SAMPLE_RATE * FRAME_MS / 1000)
#define FRAME_BYTES (FRAME_SAMPLES * 2)

// ======================= TTS PROVIDER =======================
enum TtsProvider {
  TTS_GROQ = 0,
  TTS_GOOGLE = 1
};

// ======================= TIMING =======================
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_LONG_MS 1200
#define WS_KEEPALIVE_MS 30000
#define WS_RECONNECT_MS 60000

// ======================= CHAT HISTORY =======================
#define HISTORY_MAX 8

// Set to 1 to print raw Google TTS HTTP response (first 512 bytes). Uses more RAM when on.
#define DEBUG_GOOGLE_TTS_RESPONSE 1

#endif
