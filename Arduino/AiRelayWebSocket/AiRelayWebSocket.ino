#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>
#include "secrets.h"
#include "prompts.h"
#include "config.h"
#include "globals.h"
#include "audio_utils.h"
#include "chat_utils.h"
#include "tts.h"
#include "stt.h"
#include "recording.h"
#include "led_task.h"
#include "DAZI-AI-main/src/mp3_decoder/mp3_decoder.h"
#include "DAZI-AI-main/src/mp3_decoder/mp3_decoder.cpp"

#if defined(ESP32)
extern "C" {
  #include "esp_psram.h"
}
#endif

// ======================= CONFIGURATION =======================
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* groq_api_key = GROQ_API_KEY;

const char* tts_model = "canopylabs/orpheus-v1-english";
const char* tts_voice = "autumn";
const char* stt_model = "whisper-large-v3-turbo";
const char* llm_model = "llama-3.1-8b-instant";

// Wake/end word: set requireWakeEndWords = false to send every utterance to LLM
const char* wake_word = "instructor";
const char* end_word = "please";
bool requireWakeEndWords = true;
bool wakeActive = false;
String commandBuffer = "";

// AssemblyAI Streaming STT
const char* assemblyai_api_key = ASSEMBLYAI_API_KEY;
const char* stt_ws_host = "streaming.assemblyai.com";
const uint16_t stt_ws_port = 443;
String stt_ws_path = "/v3/ws?sample_rate=16000&encoding=pcm_s16le&format_turns=true"
                     "&speech_model=universal-streaming-multilingual&language_detection=true"
                     "&end_of_turn_confidence_threshold=0.6"
                     "&min_end_of_turn_silence_when_confident=800"
                     "&max_turn_silence=1500";

// Audio calibration (use M/V/I serial commands to tune)
int outputVolumePercent = 10;
int silenceThreshold = 100;
int micTestVolumeShift = 2;

TtsProvider ttsProvider = TTS_GOOGLE;

const char* google_tts_api_key = GOOGLE_TTS_API_KEY;
const char* google_tts_voice = "en-US-Wavenet-D";
const char* google_tts_language = "en-US";

// ======================= GLOBALS =======================

WiFiClientSecure client;
WebSocketsClient ws;

const int headerSize = 44;
const int waveDataSize = RECORD_TIME_SECONDS * SAMPLE_RATE * 2; 
const int bufferSize = headerSize + waveDataSize;

uint8_t *recording_buffer; 

volatile bool ledRecording = false;
volatile bool ledWaiting = false;

bool wsConnected = false;
bool isProcessing = false;
String lastFinalTranscript = "";
unsigned long lastFinalMs = 0;
unsigned long lastWsActivityMs = 0;  // Track last WS activity for keep-alive
unsigned long lastMicSendMs = 0;     // Track when we last sent audio
// WS_KEEPALIVE_MS, WS_RECONNECT_MS from config.h

// Mic test mode state (micTestVolumeShift is in AUDIO CALIBRATION section)
bool micTestMode = false;
bool listeningEnabled = true;
bool lastButtonState = HIGH;
bool buttonPressed = false;
unsigned long buttonPressMs = 0;
bool ttsPlaying = false;
unsigned long ttsCooldownUntilMs = 0;
int lastTurnOrderHandled = -1;

// BUTTON_DEBOUNCE_MS, BUTTON_LONG_MS from config.h
// ChatMessage struct and extern declarations in globals.h

ChatMessage chatHistory[HISTORY_MAX];
uint8_t historyCount = 0;

int32_t mic_buffer[FRAME_SAMPLES];
int16_t pcm_frame[FRAME_SAMPLES];

// ======================= SETUP =======================

void setup() {
  delay(3000); // Wait for USB

  Serial.begin(115200);
  Serial.println("\n\nStarting Voice Assistant...");
#if defined(ESP32)
  Serial.printf("PSRAM: %s, Free: %u bytes\n",
                psramFound() ? "Found" : "Not found",
                (unsigned)ESP.getFreePsram());
#endif

  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }

#if USE_BOOT_BUTTON
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
#endif
#if USE_EXT_BUTTON
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_IN, INPUT_PULLUP);
#endif
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  digitalWrite(PIN_RED, HIGH); 
  digitalWrite(PIN_GREEN, HIGH); // START OFF

  // Allocate memory (Safe malloc)
  recording_buffer = (uint8_t*)malloc(bufferSize);
  if(recording_buffer == NULL) {
      Serial.println("RAM Allocation Failed");
      while(1);
  }
  Serial.println("RAM Allocated");

  // WiFi connection with timeout and retry
  WiFi.mode(WIFI_STA);           // Explicit STA mode (required on some ESP32-S3)
  WiFi.persistent(false);        // Don't use stored credentials
#if defined(ESP32)
  WiFi.setSleep(false);          // Disable power save for more reliable connect
#endif
  int wifiAttempts = 0;
  const int maxWifiAttempts = 5;
  const unsigned long wifiTimeoutMs = 15000;  // 15s (some routers/DHCP are slow)

  while (wifiAttempts < maxWifiAttempts) {
    wifiAttempts++;
    Serial.printf("WiFi attempt %d/%d (SSID: %s)...\n", wifiAttempts, maxWifiAttempts, ssid);

    WiFi.disconnect(true);
    delay(200);
    WiFi.begin(ssid, password);

    unsigned long wifiStartMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - wifiStartMs > wifiTimeoutMs) {
        int st = WiFi.status();
        Serial.printf("\nTimeout! status=%d ", st);
        if (st == 1) Serial.println("(NO_SSID_AVAIL - wrong name or 5GHz?)");
        else if (st == 4) Serial.println("(CONNECT_FAILED - wrong password?)");
        else Serial.println("(see WL_* in WiFi library)");
        break;
      }
      delay(250);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi Connected!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      break;
    }

    if (wifiAttempts < maxWifiAttempts) {
      Serial.println("Retrying in 2s...");
      delay(2000);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed! Check: SSID/password in secrets.h, use 2.4GHz band.");
    Serial.println("Restarting in 5s...");
    delay(5000);
    ESP.restart();
  }
  
  // --- SPEAKER SETUP (16-BIT MODE) ---
  // BUG FIX 3: Speaker must be 16-bit to match WAV files
  i2s_config_t spk_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // <--- CHANGED TO 16-BIT
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false
  };
  
  i2s_pin_config_t spk_pins = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  i2s_driver_install(I2S_NUM_1, &spk_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &spk_pins);

  // --- MIC SETUP (32-BIT MODE) ---
  // INMP441 requires 32-bit clocks
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // <--- KEPT AT 32-BIT
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false
  };
  
  i2s_pin_config_t mic_pins = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  
  i2s_driver_install(I2S_NUM_0, &mic_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);

  client.setInsecure();
  
  // Set headers and event handler BEFORE connecting
  ws.onEvent(wsEvent);
  String authHeader = String("Authorization: ") + assemblyai_api_key;
  ws.setExtraHeaders(authHeader.c_str());
  ws.setReconnectInterval(5000);
  
  // Now connect
  ws.beginSSL(stt_ws_host, stt_ws_port, stt_ws_path.c_str());
  
  Serial.println("\n🎤 READY!");
  Serial.println("Always-on mode: AssemblyAI Streaming STT.");
  Serial.println("Short press: toggle listening. Hold: reset chat history.");
  Serial.println("Type H for help with serial commands.");
  Serial.print("TTS provider: ");
  Serial.println(ttsProvider == TTS_GOOGLE ? "Google" : "Groq");
  if (requireWakeEndWords) {
    Serial.printf("Wake/end words: '%s'...'%s'\n", wake_word, end_word);
  } else {
    Serial.println("Wake/end words: Disabled (direct mode)");
  }

  ledRecording = false;
  ledWaiting = false;
  xTaskCreate(ledTask, "ledTask", 2048, NULL, 1, NULL);
}

void loop() {
  static unsigned long lastButtonChangeMs = 0;
  bool buttonState = digitalRead(BUTTON_IN);
  if (buttonState != lastButtonState) {
    lastButtonChangeMs = millis();
  }
  if ((millis() - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS) {
    if (!buttonPressed && lastButtonState == HIGH && buttonState == LOW) {
      buttonPressed = true;
      buttonPressMs = millis();
    }
    if (buttonPressed && lastButtonState == LOW && buttonState == HIGH) {
      unsigned long pressDuration = millis() - buttonPressMs;
      buttonPressed = false;
      if (pressDuration >= BUTTON_LONG_MS) {
        clearChatHistory();
        listeningEnabled = true;
        Serial.println("Chat history cleared");
      } else {
        listeningEnabled = !listeningEnabled;
        Serial.print("Listening: ");
        Serial.println(listeningEnabled ? "ON" : "OFF");
      }
      ledRecording = false;
      ledWaiting = false;
    }
  }
  lastButtonState = buttonState;

  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'T' || c == 't') {
      // Test TTS with fixed phrase
      String testPhrase = "Hello, this is a test of the text to speech system.";
      Serial.println("Testing TTS: " + testPhrase);
      if (ttsProvider == TTS_GOOGLE) {
        speakGoogleTTS(testPhrase);
      } else {
        speakGroqTTS(testPhrase);
      }
    } else if (c == 'G' || c == 'g') {
      // Force test Groq TTS
      Serial.println("Testing Groq TTS...");
      speakGroqTTS("Hello, this is a Groq test.");
    } else if (c == 'O' || c == 'o') {
      // Test Google TTS
      Serial.println("Testing Google TTS...");
      speakGoogleTTS("Hello, this is a Google Cloud text to speech test.");
    } else if (c == 'P' || c == 'p') {
      // Toggle TTS provider
      if (ttsProvider == TTS_GROQ) {
        ttsProvider = TTS_GOOGLE;
        Serial.println("TTS provider: Google");
      } else {
        ttsProvider = TTS_GROQ;
        Serial.println("TTS provider: Groq");
      }
    } else if (c == 'M' || c == 'm') {
      // Adjust mic sensitivity
      String digits = "";
      unsigned long start = millis();
      while (millis() - start < 300) {
        while (Serial.available() > 0) {
          char d = Serial.read();
          if (d >= '0' && d <= '9') digits += d;
        }
      }
      if (digits.length() > 0) {
        silenceThreshold = digits.toInt();
        Serial.printf("Mic threshold set to %d\n", silenceThreshold);
      } else {
        Serial.printf("Current mic threshold: %d (lower=more sensitive)\n", silenceThreshold);
      }
    } else if (c == 'V' || c == 'v') {
      // Adjust volume
      String digits = "";
      unsigned long start = millis();
      while (millis() - start < 300) {
        while (Serial.available() > 0) {
          char d = Serial.read();
          if (d >= '0' && d <= '9') digits += d;
        }
      }
      if (digits.length() > 0) {
        outputVolumePercent = digits.toInt();
        if (outputVolumePercent > 100) outputVolumePercent = 100;
        Serial.printf("Volume set to %d%%\n", outputVolumePercent);
      } else {
        Serial.printf("Current volume: %d%%\n", outputVolumePercent);
      }
    } else if (c == 'W' || c == 'w') {
      // Toggle wake/end word requirement
      requireWakeEndWords = !requireWakeEndWords;
      if (requireWakeEndWords) {
        Serial.printf("Wake/end word mode: ON (say '%s'...command...'%s')\n", wake_word, end_word);
      } else {
        Serial.println("Wake/end word mode: OFF (every final transcript sent to LLM)");
      }
    } else if (c == 'X' || c == 'x') {
      // Toggle mic test mode
      micTestMode = !micTestMode;
      if (micTestMode) {
        Serial.println("MIC TEST MODE: ON - speak into mic to hear on speaker");
        Serial.printf("  Mic gain shift: %d (lower=louder, 0=max)\n", micTestVolumeShift);
        Serial.printf("  Output volume: %d%%\n", outputVolumePercent);
        digitalWrite(PIN_GREEN, HIGH); // Turn off green
        // Set speaker to 16kHz mono
        i2s_set_clk(I2S_NUM_1, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
      } else {
        Serial.println("MIC TEST MODE: OFF - returning to normal operation");
        digitalWrite(PIN_GREEN, LOW);
        digitalWrite(PIN_RED, HIGH);
      }
    } else if (c == 'I' || c == 'i') {
      // Adjust mic test input gain
      String digits = "";
      unsigned long start = millis();
      while (millis() - start < 300) {
        while (Serial.available() > 0) {
          char d = Serial.read();
          if (d >= '0' && d <= '9') digits += d;
        }
      }
      if (digits.length() > 0) {
        micTestVolumeShift = digits.toInt();
        if (micTestVolumeShift > 10) micTestVolumeShift = 10;
        Serial.printf("Mic input gain shift: %d (lower=louder)\n", micTestVolumeShift);
      } else {
        Serial.printf("Current mic gain shift: %d (lower=louder, try I0-I6)\n", micTestVolumeShift);
      }
    } else if (c == 'H' || c == 'h' || c == '?') {
      // Help - list all commands
      Serial.println("\n=== Serial Commands ===");
      Serial.println("T      - Test current TTS provider");
      Serial.println("P      - Toggle TTS provider (Groq <-> Google)");
      Serial.println("G      - Test Groq TTS (free, unlimited)");
      Serial.println("O      - Test Google TTS: \"Hello, this is a Google Cloud text to speech test.\"");
      Serial.println("M      - Show mic sensitivity threshold");
      Serial.println("M###   - Set mic threshold (lower=more sensitive, e.g. M200)");
      Serial.println("V      - Show volume");
      Serial.println("V###   - Set volume 0-100 (e.g. V50)");
      Serial.println("W      - Toggle wake/end word mode");
      Serial.println("X      - Toggle mic test mode (hear mic on speaker)");
      Serial.println("I      - Show mic input gain (test mode)");
      Serial.println("I#     - Set mic input gain shift (0=loud, 4=medium, 6=quiet)");
      Serial.println("H/?    - Show this help");
      Serial.println("");
      Serial.printf("Current settings:\n");
      const char* providerName = (ttsProvider == TTS_GOOGLE) ? "Google" : "Groq";
      Serial.printf("  TTS Provider: %s\n", providerName);
      Serial.printf("  Mic threshold: %d\n", silenceThreshold);
      Serial.printf("  Mic test gain: %d\n", micTestVolumeShift);
      Serial.printf("  Volume: %d%%\n", outputVolumePercent);
      Serial.printf("  Wake/end words: %s\n", requireWakeEndWords ? "Required" : "Disabled");
      Serial.printf("  Mic test mode: %s\n", micTestMode ? "ON" : "OFF");
      Serial.println("=======================\n");
    }
  }

  // Handle mic test mode or normal operation
  if (micTestMode) {
    runMicTest();
    // Skip WS operations in test mode but allow serial commands above
  } else {
    ws.loop();
    streamMicFrame();
  }
  
  // Check for stale WebSocket connection (skip in test mode)
  if (!micTestMode && wsConnected && !isProcessing && !ttsPlaying && lastWsActivityMs > 0) {
    unsigned long now = millis();
    unsigned long silentTime = now - lastWsActivityMs;
    
    // Proactive reconnect: If idle too long, reconnect in background
    // This ensures connection is fresh when user starts speaking
    if (silentTime > WS_RECONNECT_MS) {
      Serial.println("WS session idle - proactive reconnect...");
      ws.disconnect();
      wsConnected = false;
      lastWsActivityMs = 0;
      delay(500);
      // Re-set auth header before reconnecting
      String authHeader = String("Authorization: ") + assemblyai_api_key;
      ws.setExtraHeaders(authHeader.c_str());
      ws.beginSSL(stt_ws_host, stt_ws_port, stt_ws_path.c_str());
    }
    // Quick reconnect: If we're sending audio but getting no response
    else if (lastMicSendMs > lastWsActivityMs && 
             now - lastMicSendMs > 5000 && 
             now - lastWsActivityMs > 10000) {
      Serial.println("WS not responding to audio - quick reconnect...");
      ws.disconnect();
      wsConnected = false;
      lastWsActivityMs = 0;
      delay(500);
      // Re-set auth header before reconnecting
      String authHeader = String("Authorization: ") + assemblyai_api_key;
      ws.setExtraHeaders(authHeader.c_str());
      ws.beginSSL(stt_ws_host, stt_ws_port, stt_ws_path.c_str());
    }
  }
}