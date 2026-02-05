#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <SPIFFS.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>  // Using Legacy Driver for everything (Stable)
#include <mbedtls/base64.h>  // For Google TTS base64 decoding
#include "secrets.h" 
#include "prompts.h"
// MP3 decoder from DAZI-AI library
#include "DAZI-AI-main/src/mp3_decoder/mp3_decoder.h"
#include "DAZI-AI-main/src/mp3_decoder/mp3_decoder.cpp"

// --- BUG FIX 1: Removed the Serial definition block that broke USB ---

// ======================= CONFIGURATION =======================

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* groq_api_key = GROQ_API_KEY;

const char* tts_model = "canopylabs/orpheus-v1-english"; 
const char* tts_voice = "autumn"; 
const char* stt_model = "whisper-large-v3-turbo"; 
const char* llm_model = "llama-3.1-8b-instant"; 

// ======================= WAKE/END WORD CONFIG =======================
// Set requireWakeEndWords = false to send every utterance to LLM directly
const char* wake_word = "instructor";
const char* end_word = "please";
bool requireWakeEndWords = true;

// Internal state (don't change)
bool wakeActive = false;
String commandBuffer = "";

// AssemblyAI Streaming STT (free tier available)
const char* assemblyai_api_key = ASSEMBLYAI_API_KEY;
const char* stt_ws_host = "streaming.assemblyai.com";
const uint16_t stt_ws_port = 443;
String stt_ws_path = "/v3/ws?sample_rate=16000&encoding=pcm_s16le&format_turns=true"
                     "&speech_model=universal-streaming-multilingual&language_detection=true"
                     "&end_of_turn_confidence_threshold=0.6"
                     "&min_end_of_turn_silence_when_confident=800"
                     "&max_turn_silence=1500";

// ======================= AUDIO CALIBRATION =======================
// Adjust these values after testing with mic test mode (X command)

// Speaker output volume (0-100%)
int outputVolumePercent = 90;

// Mic sensitivity threshold - lower = more sensitive
// Use M command to test, typical range 100-500
int silenceThreshold = 100;

// Mic input gain shift for test mode - lower = louder
// 0 = max (may clip), 2 = high, 4 = medium, 6 = quiet
// Use I command to test
int micTestVolumeShift = 2;

// ======================= TTS PROVIDER =======================
enum TtsProvider {
  TTS_GROQ = 0,
  TTS_GOOGLE = 1
};

// Change this to switch TTS providers
// TTS_GROQ = free unlimited, TTS_GOOGLE = 4M chars/month free (better quality)
TtsProvider ttsProvider = TTS_GOOGLE;

// Google Cloud TTS settings
const char* google_tts_api_key = GOOGLE_TTS_API_KEY;
const char* google_tts_voice = "en-US-Wavenet-D";  // Male voice, try en-US-Wavenet-F for female
const char* google_tts_language = "en-US";

// Set to 1 to print raw HTTP response (first 512 bytes hex+ASCII + total length). Uses more RAM when on.
#define DEBUG_GOOGLE_TTS_RESPONSE 1


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

// ======================= GLOBALS =======================

WiFiClientSecure client;
WebSocketsClient ws;

// Audio Recording Settings
#define SAMPLE_RATE 16000
#define RECORD_TIME_SECONDS 3 // Keep small for internal RAM safety
// (silenceThreshold is defined in AUDIO CALIBRATION section above)
#define STT_RESPONSE_TIMEOUT_MS 20000

// Streaming audio parameters
// AssemblyAI requires 100-2000ms of audio per message
#define FRAME_MS 200
#define FRAME_SAMPLES (SAMPLE_RATE * FRAME_MS / 1000)
#define FRAME_BYTES (FRAME_SAMPLES * 2)

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
const unsigned long WS_KEEPALIVE_MS = 30000;  // Send keepalive every 30s
const unsigned long WS_RECONNECT_MS = 60000;  // Force reconnect after 60s of no response

// Mic test mode state (micTestVolumeShift is in AUDIO CALIBRATION section)
bool micTestMode = false;
bool listeningEnabled = true;
bool lastButtonState = HIGH;
bool buttonPressed = false;
unsigned long buttonPressMs = 0;
bool ttsPlaying = false;
unsigned long ttsCooldownUntilMs = 0;
int lastTurnOrderHandled = -1;

const unsigned long BUTTON_DEBOUNCE_MS = 50;
const unsigned long BUTTON_LONG_MS = 1200;

struct ChatMessage {
  const char* role;
  String content;
};

const uint8_t HISTORY_MAX = 8;
ChatMessage chatHistory[HISTORY_MAX];
uint8_t historyCount = 0;

int32_t mic_buffer[FRAME_SAMPLES];
int16_t pcm_frame[FRAME_SAMPLES];

void ledTask(void*){
  for(;;){
    if (ledRecording) {
      digitalWrite(PIN_RED, LOW);    // red ON
      digitalWrite(PIN_GREEN, HIGH); // green OFF
    } else if (ledWaiting) {
      digitalWrite(PIN_RED, HIGH); // red OFF
      digitalWrite(PIN_GREEN, LOW); // green ON (processing)
    } else {
      digitalWrite(PIN_RED, HIGH);   // red OFF
      digitalWrite(PIN_GREEN, LOW);  // green ON (ready)
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ======================= HELPER FUNCTIONS =======================

void updateSpeakerFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
  if (bitsPerSample != 16) {
    Serial.println("Unsupported bits per sample for speaker");
    return;
  }
  i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
              (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
}

void applyVolumeToPcm16(uint8_t* buffer, size_t bytes) {
  if (outputVolumePercent == 100 || bytes < 2) return;
  int samples = bytes / 2;
  int16_t* samplesPtr = (int16_t*)buffer;
  for (int i = 0; i < samples; i++) {
    int32_t scaled = (samplesPtr[i] * outputVolumePercent) / 100;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    samplesPtr[i] = (int16_t)scaled;
  }
}

String toLowerCopy(const String& input) {
  String out = input;
  out.toLowerCase();
  return out;
}

String trimCopy(const String& input) {
  String out = input;
  out.trim();
  return out;
}

String extractBetween(const String& text, int startIdx, int endIdx) {
  if (startIdx < 0) startIdx = 0;
  if (endIdx < 0 || endIdx > (int)text.length()) endIdx = text.length();
  if (endIdx <= startIdx) return "";
  return text.substring(startIdx, endIdx);
}

String processWakeAndEndWords(const String& finalTranscript) {
  String lower = toLowerCopy(finalTranscript);
  String wake = String(wake_word);
  String endw = String(end_word);
  wake.toLowerCase();
  endw.toLowerCase();

  int wakeIdx = lower.indexOf(wake);
  int endIdx = lower.indexOf(endw);

  if (wakeIdx >= 0) {
    wakeActive = true;
    commandBuffer = "";
    int afterWake = wakeIdx + wake.length();
    String remainder = extractBetween(finalTranscript, afterWake, finalTranscript.length());
    remainder = trimCopy(remainder);
    if (remainder.length() > 0) {
      commandBuffer += remainder;
    }
  } else if (wakeActive) {
    if (commandBuffer.length() > 0) commandBuffer += " ";
    commandBuffer += trimCopy(finalTranscript);
  }

  if (wakeActive && endIdx >= 0) {
    // Remove end word from buffered command
    String bufferLower = toLowerCopy(commandBuffer);
    int endInBuffer = bufferLower.indexOf(endw);
    if (endInBuffer >= 0) {
      commandBuffer = extractBetween(commandBuffer, 0, endInBuffer);
    }
    String result = trimCopy(commandBuffer);
    wakeActive = false;
    commandBuffer = "";
    return result;
  }

  return "";
}

void stopSpeakerNoise() {
  // Flush a short silence buffer and clear DMA to prevent hiss/white noise
  static uint8_t silence[1024] = {0};
  size_t bytes_written = 0;
  for (int i = 0; i < 8; i++) {
    i2s_write(I2S_NUM_1, silence, sizeof(silence), &bytes_written, 50);
  }
  // Give the I2S DMA time to drain before reset
  vTaskDelay(pdMS_TO_TICKS(30));
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_stop(I2S_NUM_1);
  vTaskDelay(pdMS_TO_TICKS(10));
  i2s_start(I2S_NUM_1);
}

void clearChatHistory() {
  historyCount = 0;
}

void addHistory(const char* role, const String& content) {
  if (content.length() == 0) return;
  if (historyCount >= HISTORY_MAX) {
    for (uint8_t i = 1; i < HISTORY_MAX; i++) {
      chatHistory[i - 1] = chatHistory[i];
    }
    historyCount = HISTORY_MAX - 1;
  }
  chatHistory[historyCount].role = role;
  chatHistory[historyCount].content = content;
  historyCount++;
}

void handleWsTextMessage(const uint8_t* payload, size_t length) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("WS JSON parse error");
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "Begin") == 0) {
    Serial.print("STT session started: ");
    Serial.println(doc["id"].as<String>());
  } else if (strcmp(type, "Turn") == 0) {
    bool endOfTurn = doc["end_of_turn"] | false;
    String transcript = doc["transcript"].as<String>();
    int turnOrder = doc["turn_order"] | -1;
    if (transcript.length() > 0) {
      if (endOfTurn) {
        Serial.print("Final: ");
        Serial.println(transcript);
        
        String command = "";
        bool shouldSend = false;
        
        if (requireWakeEndWords) {
          // Mode: Require wake word + end word
          command = processWakeAndEndWords(transcript);
          if (wakeActive && commandBuffer.length() > 0) {
            shouldSend = true;
            command = commandBuffer;
            wakeActive = false;
            commandBuffer = "";
          }
          if (command.length() > 0) {
            shouldSend = true;
          }
        } else {
          // Mode: Send every final transcript directly
          command = transcript;
          shouldSend = true;
        }

        if (shouldSend &&
            !isProcessing &&
            turnOrder != lastTurnOrderHandled &&
            (transcript != lastFinalTranscript || (millis() - lastFinalMs) > 10000)) {
          isProcessing = true;
          lastFinalTranscript = transcript;
          lastFinalMs = millis();
          lastTurnOrderHandled = turnOrder;
          ledRecording = false;
          ledWaiting = true;

          String reply = getChatResponse(command);
          if (reply.length() > 0) {
            Serial.print("AI says: ");
            Serial.println(reply);
            if (ttsProvider == TTS_GOOGLE) {
              speakGoogleTTS(reply);
            } else {
              speakGroqTTS(reply);
            }
          }
          addHistory("user", command);
          addHistory("assistant", reply);

          isProcessing = false;
          ledWaiting = false;
        }
      } else {
        Serial.print("\rPartial: ");
        Serial.print(transcript);
      }
    }
  } else if (strcmp(type, "Termination") == 0) {
    Serial.println("\nSTT session terminated by server");
  }
}

void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      wsConnected = false;
      ledRecording = false;
      ledWaiting = false;
      Serial.println("WS disconnected");
      break;
    case WStype_CONNECTED:
      wsConnected = true;
      ledWaiting = true;
      lastWsActivityMs = millis();
      Serial.println("WS connected");
      break;
    case WStype_TEXT:
      lastWsActivityMs = millis();
      handleWsTextMessage(payload, length);
      break;
    case WStype_BIN:
      // Binary data from server (not expected for STT)
      Serial.printf("WS BIN received: %d bytes\n", length);
      break;
    case WStype_ERROR:
      Serial.printf("WS ERROR: %s\n", payload ? (char*)payload : "unknown");
      break;
    case WStype_PING:
      Serial.println("WS PING");
      break;
    case WStype_PONG:
      lastWsActivityMs = millis(); // Keep connection alive on pong
      break;
    default:
      Serial.printf("WS event: %d\n", type);
      break;
  }
}

// Mic-to-speaker passthrough test mode
void runMicTest() {
  int32_t buffer[512];
  size_t bytes_read = 0;
  size_t bytes_written = 0;
  
  // Read from mic (32-bit)
  i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
  
  if (bytes_read > 0) {
    int samples = bytes_read / 4;
    int32_t signal_energy = 0;
    
    // Convert 32-bit mic to 16-bit speaker with volume control
    int16_t outBuffer[512];
    for (int i = 0; i < samples; i++) {
      // Shift for volume and convert to 16-bit
      int32_t sample = buffer[i] >> (14 + micTestVolumeShift);
      // Apply output volume
      sample = (sample * outputVolumePercent) / 100;
      // Clamp
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      outBuffer[i] = (int16_t)sample;
      signal_energy += abs(buffer[i] >> 14);
    }
    
    // Write to speaker (16-bit)
    i2s_write(I2S_NUM_1, outBuffer, samples * 2, &bytes_written, portMAX_DELAY);
    
    // LED indicator
    if ((signal_energy / samples) > silenceThreshold) {
      digitalWrite(PIN_RED, LOW);  // Red ON when sound detected
    } else {
      digitalWrite(PIN_RED, HIGH); // Red OFF
    }
  }
}

void streamMicFrame() {
  if (!wsConnected) return;
  if (!listeningEnabled) {
    ledRecording = false;
    ledWaiting = false;
    return;
  }
  if (isProcessing || ttsPlaying || millis() < ttsCooldownUntilMs) {
    ledRecording = false;
    ledWaiting = true;
    return;
  }

  size_t bytes_read = 0;
  i2s_read(I2S_NUM_0, mic_buffer, sizeof(mic_buffer), &bytes_read, pdMS_TO_TICKS(250));
  if (bytes_read == 0) return;

  int samples_read = bytes_read / 4;
  if (samples_read <= 0) return;

  uint64_t sum_abs = 0;
  for (int i = 0; i < samples_read; i++) {
    int16_t sample = (int16_t)(mic_buffer[i] >> 14);
    pcm_frame[i] = sample;
    sum_abs += abs(sample);
  }

  uint32_t avg_abs = (samples_read > 0) ? (uint32_t)(sum_abs / samples_read) : 0;
  bool isVoice = avg_abs >= silenceThreshold;
  ledRecording = isVoice;
  if (isVoice) {
    ledWaiting = false;
  }

  bool sent = ws.sendBIN((uint8_t*)pcm_frame, samples_read * 2);
  lastMicSendMs = millis();
  
  // Debug: print voice detection and send status periodically
  static unsigned long lastVoiceDebugMs = 0;
  static int sendFailCount = 0;
  if (!sent) sendFailCount++;
  
  if (isVoice && millis() - lastVoiceDebugMs > 1000) {
    Serial.printf("Voice detected (level: %u) - WS send: %s (fails: %d)\n", 
                  avg_abs, sent ? "OK" : "FAIL", sendFailCount);
    lastVoiceDebugMs = millis();
    sendFailCount = 0;
  }
}

void createWavHeader(uint8_t *header, int waveDataSize){
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  unsigned int fileSize = waveDataSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 0x10; header[17] = 0x00; header[18] = 0x00; header[19] = 0x00;
  header[20] = 0x01; header[21] = 0x00;
  header[22] = 0x01; header[23] = 0x00; // Mono
  header[24] = 0x80; header[25] = 0x3E; header[26] = 0x00; header[27] = 0x00; // 16000 Hz
  header[28] = 0x00; header[29] = 0x7D; header[30] = 0x00; header[31] = 0x00; // Byte Rate
  header[32] = 0x02; header[33] = 0x00;
  header[34] = 0x10; header[35] = 0x00; // 16-bit
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (byte)(waveDataSize & 0xFF);
  header[41] = (byte)((waveDataSize >> 8) & 0xFF);
  header[42] = (byte)((waveDataSize >> 16) & 0xFF);
  header[43] = (byte)((waveDataSize >> 24) & 0xFF);
}

String transcribeAudio(int dataLength) {
    Serial.println("Sending to Groq (STT)...");
    
    client.setTimeout(15000);
    // Use setInsecure again just to be safe
    client.setInsecure();
    
    if (!client.connect("api.groq.com", 443)) {
        Serial.println("Connection failed");
        return "";
    }
    
    String boundary = "------------------------ESP32Bound";
    String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    String modelParam = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + stt_model + "\r\n";

    int contentLength = head.length() + dataLength + tail.length() + modelParam.length();

    client.println("POST /openai/v1/audio/transcriptions HTTP/1.1");
    client.println("Host: api.groq.com");
    client.println("Authorization: Bearer " + String(groq_api_key));
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.print("Content-Length: "); client.println(contentLength);
    client.println();
    
    client.print(modelParam);
    client.print(head);
    
    uint8_t *fb = recording_buffer;
    int chunk = 1024;
    for(int i=0; i<dataLength; i+=chunk){
        if(i+chunk > dataLength) chunk = dataLength - i;
        client.write(fb+i, chunk);
    }
    client.print(tail);

    // Simple response reader
    String response = "";
    bool headerEnd = false;
    unsigned long startRead = millis();
    while (client.connected() && millis() - startRead < 10000) {
        while (client.available()) {
            char c = client.read();
            if (!headerEnd) {
                if (c == '{') { // Quick hack to find JSON start
                     headerEnd = true;
                     response += c;
                }
            } else {
                response += c;
            }
            startRead = millis(); // Reset timeout on data
        }
        delay(10);
    }
    client.stop();
    
    // Parse
    int jsonStart = response.indexOf("{");
    if(jsonStart == -1 && response.length() > 0 && response[0] == '{') jsonStart = 0;
    
    if(jsonStart == -1) {
        Serial.println("Failed to find JSON in response");
        return "";
    }
    
    String jsonStr = response.substring(jsonStart);
    JsonDocument doc;
    deserializeJson(doc, jsonStr);
    return doc["text"].as<String>();
}

String getChatResponse(String input) {
    Serial.println("Sending to Groq (LLM)...");
    HTTPClient http;
    http.setTimeout(15000);
    http.begin("https://api.groq.com/openai/v1/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(groq_api_key));
    
    JsonDocument doc;
    doc["model"] = llm_model; 
    JsonArray messages = doc["messages"].to<JsonArray>();
    
    JsonObject sysMsg = messages.add<JsonObject>();
    sysMsg["role"] = "system";
    sysMsg["content"] = SYSTEM_PROMPT;
    
    for (uint8_t i = 0; i < historyCount; i++) {
        JsonObject histMsg = messages.add<JsonObject>();
        histMsg["role"] = chatHistory[i].role;
        histMsg["content"] = chatHistory[i].content;
    }

    JsonObject userMsg = messages.add<JsonObject>();
    userMsg["role"] = "user";
    userMsg["content"] = input;
    
    String payload;
    serializeJson(doc, payload);
    
    int httpCode = http.POST(payload);
    String result = "";
    if(httpCode == 200) {
        String response = http.getString();
        JsonDocument resDoc;
        deserializeJson(resDoc, response);
        result = resDoc["choices"][0]["message"]["content"].as<String>();
    } else {
        Serial.printf("LLM Error: %d\n", httpCode);
        Serial.println(http.getString());
    }
    http.end();
    return result;
}

void playWavFile(const char* filename) {
    File file = SPIFFS.open(filename, FILE_READ);
    if (!file) {
        Serial.println("Failed to open WAV file");
        return;
    }

    // --- PARSE WAV HEADER ---
    uint8_t header[44];
    if (file.read(header, 44) < 44) {
        Serial.println("WAV file too short");
        file.close();
        return;
    }

    // Extract Sample Rate (Bytes 24-27, Little Endian)
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    
    // Extract Channels (Bytes 22-23) - 1 = Mono, 2 = Stereo
    uint16_t channels = header[22] | (header[23] << 8);

    // Guard: avoid IntegerDivideByZero in I2S driver if file is corrupt/incomplete
    if (sampleRate == 0 || sampleRate > 48000) {
        Serial.println("Invalid WAV sample rate, skipping playback");
        file.close();
        return;
    }
    if (channels == 0 || channels > 2) {
        Serial.println("Invalid WAV channels, skipping playback");
        file.close();
        return;
    }

    Serial.printf("Playing: %d Hz, %s\n", sampleRate, (channels == 2) ? "Stereo" : "Mono");

    // --- RECONFIGURE SPEAKER ---
    i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);

    // --- PLAYBACK ---
    uint8_t buffer[1024]; 
    size_t bytes_read = 0;
    size_t bytes_written = 0;
    
    while(file.available()) {
        bytes_read = file.read(buffer, sizeof(buffer));
        if(bytes_read > 0) {
            applyVolumeToPcm16(buffer, bytes_read);
            i2s_write(I2S_NUM_1, buffer, bytes_read, &bytes_written, portMAX_DELAY);
        }
    }
    
    // Silence at end to prevent pop
    uint8_t silence[512] = {0};
    i2s_write(I2S_NUM_1, silence, 512, &bytes_written, 100);
    
    file.close();
    
    // Optional: Reset back to standard 16k if needed (though next file will auto-fix itself)
    // i2s_set_clk(I2S_NUM_1, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    
    Serial.println("Playback done");
}

void speakGroqTTS(String text) {
    Serial.println("Requesting Groq TTS...");
    digitalWrite(PIN_RED, LOW); // Busy (Red ON)
    ttsPlaying = true;

    HTTPClient http;
    http.setTimeout(15000);
    http.begin("https://api.groq.com/openai/v1/audio/speech");
    http.addHeader("Authorization", "Bearer " + String(groq_api_key));
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["model"] = tts_model;
    doc["voice"] = tts_voice;
    doc["input"] = text;
    doc["response_format"] = "wav";

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);

    if (httpCode == 200) {
        File outfile = SPIFFS.open("/tts.wav", FILE_WRITE);
        if (outfile) {
            http.writeToStream(&outfile);
            outfile.close();
            // Play the WAV file
            playWavFile("/tts.wav");
        }
    } else {
        Serial.printf("TTS Error: %d\n", httpCode);
    }
    http.end();
    
    digitalWrite(PIN_RED, HIGH); // Busy (Red OFF)
    ttsPlaying = false;
    ttsCooldownUntilMs = millis() + 800;
    stopSpeakerNoise();
}

// Play MP3 file using DAZI-AI MP3 decoder
void playMp3File(const char* filename) {
    File file = SPIFFS.open(filename, FILE_READ);
    if (!file) {
        Serial.println("Failed to open MP3 file");
        return;
    }
    
    size_t fileSize = file.size();
    Serial.printf("Playing MP3: %u bytes\n", (unsigned)fileSize);
    
    // Initialize MP3 decoder
    if (!MP3Decoder_AllocateBuffers()) {
        Serial.println("Failed to allocate MP3 decoder");
        file.close();
        return;
    }
    
    // Read entire file into RAM (MP3 is small enough)
    uint8_t* mp3Data = (uint8_t*)malloc(fileSize);
    if (!mp3Data) {
        Serial.println("Failed to allocate MP3 buffer");
        MP3Decoder_FreeBuffers();
        file.close();
        return;
    }
    
    // Read file in chunks to ensure we get all bytes
    size_t totalRead = 0;
    while (totalRead < fileSize && file.available()) {
        size_t bytesRead = file.read(mp3Data + totalRead, fileSize - totalRead);
        if (bytesRead == 0) break;
        totalRead += bytesRead;
    }
    file.close();
    
    if (totalRead != fileSize) {
        Serial.printf("MP3 read error: got %u of %u bytes\n", (unsigned)totalRead, (unsigned)fileSize);
        free(mp3Data);
        MP3Decoder_FreeBuffers();
        return;
    }
    
    // Decode and play
    uint8_t* readPtr = mp3Data;
    int32_t bytesLeft = fileSize;
    int16_t outBuffer[2304];  // Max samples per frame (1152 * 2 channels)
    size_t bytes_written = 0;
    bool firstFrame = true;
    
    int framesDecoded = 0;
    size_t totalSamples = 0;
    
    // Clear I2S buffer before playback to prevent pop
    i2s_zero_dma_buffer(I2S_NUM_1);
    
    int errorCount = 0;
    
    while (bytesLeft > 0) {
        // Find sync word
        int32_t offset = MP3FindSyncWord(readPtr, bytesLeft);
        if (offset < 0) {
            // No more sync words found - this is normal at end of file
            break;
        }
        
        readPtr += offset;
        bytesLeft -= offset;
        
        if (bytesLeft < 4) break;  // Not enough data for a frame header
        
        // Remember position before decode
        uint8_t* savedPtr = readPtr;
        int32_t savedBytesLeft = bytesLeft;
        
        // Decode frame - MP3Decode updates readPtr and bytesLeft
        int32_t result = MP3Decode(readPtr, &bytesLeft, outBuffer, 0);
        
        // Calculate how many bytes were consumed
        int32_t bytesConsumed = savedBytesLeft - bytesLeft;
        readPtr = savedPtr + bytesConsumed;
        
        if (result == ERR_MP3_NONE) {
            framesDecoded++;
            
            // Get frame info on first successful decode
            if (firstFrame) {
                MP3GetLastFrameInfo();
                int sampleRate = MP3GetSampRate();
                int channels = MP3GetChannels();
                Serial.printf("MP3: %d Hz, %d ch\n", sampleRate, channels);
                i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, 
                           channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
                firstFrame = false;
            }
            
            int outputSamps = MP3GetOutputSamps();
            totalSamples += outputSamps;
            
            // Apply volume
            for (int i = 0; i < outputSamps; i++) {
                outBuffer[i] = (int16_t)((int32_t)outBuffer[i] * outputVolumePercent / 100);
            }
            
            // Write to I2S
            i2s_write(I2S_NUM_1, outBuffer, outputSamps * 2, &bytes_written, portMAX_DELAY);
            errorCount = 0;  // Reset error count on success
        } else if (result == ERR_MP3_INDATA_UNDERFLOW) {
            // Not enough data in buffer - try to continue
            if (bytesLeft < 1024) {
                Serial.printf("MP3: underflow at %d bytes left\n", bytesLeft);
                break;
            }
            errorCount++;
            if (errorCount > 10) {
                Serial.printf("MP3: too many underflows at %d bytes left\n", bytesLeft);
                break;
            }
            // Skip past problematic area
            readPtr++;
            bytesLeft--;
        } else {
            // Other error - skip one byte and try again
            errorCount++;
            if (errorCount > 50) {
                Serial.printf("MP3: error %d, stopping at %d bytes left\n", result, bytesLeft);
                break;
            }
            if (bytesConsumed == 0) {
                readPtr++;
                bytesLeft--;
            }
        }
    }
    
    Serial.printf("MP3: %d frames, %u samples, %d bytes remaining\n", framesDecoded, (unsigned)totalSamples, bytesLeft);
    
    // Cleanup
    free(mp3Data);
    MP3Decoder_FreeBuffers();
    
    // Silence at end
    uint8_t silence[512] = {0};
    i2s_write(I2S_NUM_1, silence, 512, &bytes_written, 100);
    
    Serial.println("MP3 playback done");
}

// Google Cloud TTS - Parse from HTTP stream with filter (no full response in RAM; avoids NoMemory).
void speakGoogleTTS(const String& text) {
    if (text.length() == 0) return;
    Serial.println("Requesting Google TTS...");
    digitalWrite(PIN_RED, LOW);
    ttsPlaying = true;

    HTTPClient http;
    http.setTimeout(60000);  // 60s for large response
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    String url = String("https://texttospeech.googleapis.com/v1/text:synthesize?key=") + google_tts_api_key;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["input"]["text"] = text;
    doc["voice"]["languageCode"] = google_tts_language;
    doc["voice"]["name"] = google_tts_voice;
    doc["audioConfig"]["audioEncoding"] = "LINEAR16";
    doc["audioConfig"]["sampleRateHertz"] = 24000;

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);

    if (httpCode != 200) {
        Serial.printf("Google TTS HTTP error: %d\n", httpCode);
        http.end();
        speakGroqTTS(text);
        return;
    }

    JsonDocument jsonDoc;
    DeserializationError err;

#if DEBUG_GOOGLE_TTS_RESPONSE
    // Debug path: get full response, print first 512 bytes (hex + ASCII), then parse
    String response = http.getString();
    http.end();
    size_t totalLen = response.length();
    Serial.println("=== Google TTS raw response (first 512 bytes) ===");
    const size_t debugLen = (totalLen < 512) ? totalLen : 512;
    for (size_t i = 0; i < debugLen; i += 16) {
        Serial.printf("%04X: ", (unsigned)i);
        for (size_t j = 0; j < 16 && (i + j) < debugLen; j++) {
            Serial.printf("%02X ", (uint8_t)response[i + j]);
        }
        Serial.println();
    }
    Serial.println("=== ASCII (first 512, . = non-printable) ===");
    for (size_t i = 0; i < debugLen; i++) {
        char c = response[i];
        Serial.print((c >= 32 && c < 127) ? c : '.');
    }
    Serial.println();
    Serial.printf("=== Total received: %u bytes ===\n", (unsigned)totalLen);

    if (totalLen < 100) {
        Serial.println("Response too short");
        speakGroqTTS(text);
        return;
    }
    err = deserializeJson(jsonDoc, response);
    response = "";
#else
    // Normal path: parse from HTTP stream (no full response in RAM)
    Stream* stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("No response stream");
        http.end();
        speakGroqTTS(text);
        return;
    }
    // Skip HTTP response headers so ArduinoJson sees only the JSON body
    int headerState = 0;
    while (stream->available() && headerState != 4) {
        char c = stream->read();
        if (headerState == 0 && c == '\r') headerState = 1;
        else if (headerState == 1 && c == '\n') headerState = 2;
        else if (headerState == 2 && c == '\r') headerState = 3;
        else if (headerState == 3 && c == '\n') headerState = 4;
        else headerState = (c == '\r') ? 1 : 0;
    }
    err = deserializeJson(jsonDoc, *stream);
    http.end();
#endif

    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        speakGroqTTS(text);
        return;
    }

    const char* b64Str = jsonDoc["audioContent"];
    if (!b64Str) {
        Serial.println("No audioContent in JSON");
        speakGroqTTS(text);
        return;
    }

    size_t b64Len = strlen(b64Str);
    if (b64Len < 100) {
        Serial.println("audioContent too short");
        speakGroqTTS(text);
        return;
    }

    // Pad to multiple of 4 for mbedtls
    size_t padLen = b64Len;
    while (padLen % 4 != 0) padLen++;

    Serial.printf("Downloaded %u b64 chars\n", (unsigned)padLen);

    size_t maxDecodedSize = (padLen * 3) / 4 + 4;
    uint8_t* audioData = (uint8_t*)malloc(maxDecodedSize);
    if (!audioData) {
        Serial.println("Audio buffer alloc failed");
        speakGroqTTS(text);
        return;
    }

    // Copy base64 to buffer with padding (mbedtls needs contiguous buffer)
    uint8_t* b64Buf = (uint8_t*)malloc(padLen + 1);
    if (!b64Buf) {
        free(audioData);
        Serial.println("Base64 buffer alloc failed");
        speakGroqTTS(text);
        return;
    }
    memcpy(b64Buf, b64Str, b64Len);
    while (b64Len < padLen) b64Buf[b64Len++] = '=';

    size_t decodedLen = 0;
    int ret = mbedtls_base64_decode(audioData, maxDecodedSize, &decodedLen, b64Buf, padLen);
    free(b64Buf);

    if (ret != 0) {
        Serial.printf("Base64 decode error: %d\n", ret);
        free(audioData);
        speakGroqTTS(text);
        return;
    }
    
    Serial.printf("Decoded %u bytes\n", (unsigned)decodedLen);
    
    // Verify WAV header
    if (decodedLen < 44 || audioData[0] != 'R' || audioData[1] != 'I' || 
        audioData[2] != 'F' || audioData[3] != 'F') {
        Serial.println("Invalid WAV");
        free(audioData);
        speakGroqTTS(text);
        return;
    }
    
    // Parse WAV - find fmt and data chunks
    size_t pos = 12;
    uint32_t sampleRate = 24000;
    uint16_t channels = 1;
    size_t dataOffset = 0;
    size_t dataSize = 0;
    
    while (pos + 8 <= decodedLen && pos < 200) {
        uint32_t chunkSize = audioData[pos+4] | (audioData[pos+5] << 8) | 
                            (audioData[pos+6] << 16) | (audioData[pos+7] << 24);
        
        if (memcmp(audioData + pos, "fmt ", 4) == 0) {
            channels = audioData[pos+10] | (audioData[pos+11] << 8);
            sampleRate = audioData[pos+12] | (audioData[pos+13] << 8) | 
                        (audioData[pos+14] << 16) | (audioData[pos+15] << 24);
        }
        
        if (memcmp(audioData + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize = chunkSize;
            break;
        }
        
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++;
    }
    
    if (dataOffset == 0 || dataSize == 0) {
        Serial.println("No data chunk");
        free(audioData);
        speakGroqTTS(text);
        return;
    }
    
    // Limit to actual available data
    if (dataOffset + dataSize > decodedLen) {
        dataSize = decodedLen - dataOffset;
    }
    
    Serial.printf("Playing: %u Hz %s, %u bytes\n", sampleRate, 
                 channels == 1 ? "Mono" : "Stereo", (unsigned)dataSize);
    
    // Configure I2S
    i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
               channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
    i2s_zero_dma_buffer(I2S_NUM_1);
    
    // Apply volume
    applyVolumeToPcm16(audioData + dataOffset, dataSize);
    
    // Play in chunks
    size_t bytesWritten = 0;
    size_t totalWritten = 0;
    const size_t CHUNK = 1024;
    
    for (size_t off = 0; off < dataSize; off += CHUNK) {
        size_t toWrite = (off + CHUNK <= dataSize) ? CHUNK : (dataSize - off);
        i2s_write(I2S_NUM_1, audioData + dataOffset + off, toWrite, &bytesWritten, portMAX_DELAY);
        totalWritten += bytesWritten;
    }
    
    Serial.printf("Played %u bytes\n", (unsigned)totalWritten);
    
    // Silence at end
    uint8_t silence[512] = {0};
    i2s_write(I2S_NUM_1, silence, 512, &bytesWritten, 100);
    
    free(audioData);
    
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    ttsCooldownUntilMs = millis() + 500;
    stopSpeakerNoise();
}

void processAudio(int dataSize) {
    ledWaiting = true;
    String text = transcribeAudio(dataSize);
    Serial.println("You said: " + text);
    
    if (text.length() > 0) {
        String reply = getChatResponse(text);
        Serial.println("AI says: " + reply);
        speakGroqTTS(reply); 
    }
    ledWaiting = false;
}

void RecordAudio(bool holdToRecord) {
  Serial.println("Recording...");
  ledRecording = true;
  
  size_t bytes_read = 0;
  // Safety check
  if(!recording_buffer) return;
  memset(recording_buffer, 0, bufferSize);
  
  int32_t buffer[256]; // Smaller buffer to fit stack
  
  // Flush buffer
  for(int i=0; i<2; i++) {
     i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, 100);
  }

  int flash_wr_size = 0;
  uint64_t sum_abs = 0;
  uint32_t samples_total = 0;
  
  while (flash_wr_size < waveDataSize) {
      if (holdToRecord) {
#if USE_EXT_BUTTON
        if (digitalRead(BUTTON_IN) == HIGH) break;
#elif USE_BOOT_BUTTON
        if (digitalRead(BOOT_BUTTON) == HIGH) break;
#else
        break;
#endif
      }
      // Read 32-bit audio from mic
      i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
      
      if(bytes_read > 0) {
          int samples_read = bytes_read / 4;
          
          if (flash_wr_size + (samples_read * 2) > waveDataSize) break;
          
          int16_t* wav_buffer_ptr = (int16_t*)(recording_buffer + headerSize + flash_wr_size);
          
          for(int i=0; i<samples_read; i++){
              // Shift 32-bit to 16-bit. 
              // INMP441 is 24-bit data left-aligned in 32-bit slot.
              // We need to shift right. try >> 14 for good volume.
              int16_t sample = (int16_t)(buffer[i] >> 14);
              wav_buffer_ptr[i] = sample;
              sum_abs += abs(sample);
              samples_total++;
          }
          flash_wr_size += (samples_read * 2);
      }
  }
  
  uint32_t avg_abs = samples_total ? (uint32_t)(sum_abs / samples_total) : 0;
  createWavHeader(recording_buffer, flash_wr_size);
  
  ledRecording = false;
  
  Serial.printf("Avg Level: %lu\n", avg_abs);
  
  if (flash_wr_size <= 1000) {
      Serial.println("Too short");
  } else if (avg_abs < silenceThreshold) {
      Serial.println("Too quiet");
  } else {
      processAudio(flash_wr_size + headerSize);
  }
}

// ======================= SETUP =======================

void setup() {
  delay(3000); // Wait for USB

  Serial.begin(115200);
  Serial.println("\n\nStarting Voice Assistant...");

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
  int wifiAttempts = 0;
  const int maxWifiAttempts = 5;
  
  while (wifiAttempts < maxWifiAttempts) {
    wifiAttempts++;
    Serial.printf("WiFi attempt %d/%d...\n", wifiAttempts, maxWifiAttempts);
    
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid, password);
    
    unsigned long wifiStartMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - wifiStartMs > 5000) {
        Serial.println(" timeout!");
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
      Serial.println("Retrying...");
      delay(1000);
    }
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed! Restarting...");
    delay(1000);
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
      Serial.println("O      - Test Google TTS (better quality)");
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