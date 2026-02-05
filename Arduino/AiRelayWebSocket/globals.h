#ifndef AI_RELAY_WEBSOCKET_GLOBALS_H
#define AI_RELAY_WEBSOCKET_GLOBALS_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include "config.h"

// ======================= Configuration (defined in .ino) =======================
extern const char* ssid;
extern const char* password;
extern const char* groq_api_key;
extern const char* tts_model;
extern const char* tts_voice;
extern const char* stt_model;
extern const char* llm_model;
extern const char* wake_word;
extern const char* end_word;
extern bool requireWakeEndWords;
extern bool wakeActive;
extern String commandBuffer;
extern const char* assemblyai_api_key;
extern const char* stt_ws_host;
extern const uint16_t stt_ws_port;
extern String stt_ws_path;
extern int outputVolumePercent;
extern int silenceThreshold;
extern int micTestVolumeShift;
extern TtsProvider ttsProvider;
extern const char* google_tts_api_key;
extern const char* google_tts_voice;
extern const char* google_tts_language;

// ======================= Clients & state (defined in .ino) =======================
extern WiFiClientSecure client;
extern WebSocketsClient ws;
extern const int headerSize;
extern const int waveDataSize;
extern const int bufferSize;
extern uint8_t* recording_buffer;
extern volatile bool ledRecording;
extern volatile bool ledWaiting;
extern bool wsConnected;
extern bool isProcessing;
extern String lastFinalTranscript;
extern unsigned long lastFinalMs;
extern unsigned long lastWsActivityMs;
extern unsigned long lastMicSendMs;
extern bool micTestMode;
extern bool listeningEnabled;
extern bool lastButtonState;
extern bool buttonPressed;
extern unsigned long buttonPressMs;
extern bool ttsPlaying;
extern unsigned long ttsCooldownUntilMs;
extern int lastTurnOrderHandled;

// ======================= Chat history (defined in .ino) =======================
struct ChatMessage {
  const char* role;
  String content;
};
extern ChatMessage chatHistory[HISTORY_MAX];
extern uint8_t historyCount;

// ======================= Streaming audio buffers (defined in .ino) =======================
extern int32_t mic_buffer[FRAME_SAMPLES];
extern int16_t pcm_frame[FRAME_SAMPLES];

#endif
