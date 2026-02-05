#include "tts.h"
#include "audio_utils.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <driver/i2s.h>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

void speakGroqTTS(String text) {
  Serial.println("Requesting Groq TTS...");
  digitalWrite(PIN_RED, LOW);
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
      playWavFile("/tts.wav");
    }
  } else {
    Serial.printf("TTS Error: %d\n", httpCode);
  }
  http.end();
  digitalWrite(PIN_RED, HIGH);
  ttsPlaying = false;
  ttsCooldownUntilMs = millis() + 800;
  stopSpeakerNoise();
}

void speakGoogleTTS(const String& text) {
  if (text.length() == 0) return;
  Serial.println("Requesting Google TTS...");
  digitalWrite(PIN_RED, LOW);
  ttsPlaying = true;

  HTTPClient http;
  http.setTimeout(60000);
  String url = String("https://texttospeech.googleapis.com/v1/text:synthesize?key=") + google_tts_api_key;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument requestDoc;
  requestDoc["input"]["text"] = text;
  requestDoc["voice"]["languageCode"] = google_tts_language;
  requestDoc["voice"]["name"] = google_tts_voice;
  requestDoc["audioConfig"]["audioEncoding"] = "LINEAR16";
  requestDoc["audioConfig"]["sampleRateHertz"] = 24000;

  String payload;
  serializeJson(requestDoc, payload);

  int httpCode = http.POST(payload);
  if (httpCode != 200) {
    Serial.printf("Google TTS HTTP error: %d\n", httpCode);
    http.end();
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }

#if defined(ESP32)
  Serial.printf("Free heap: %u, PSRAM: %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
#endif

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.println("No response stream");
    http.end();
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }

  const size_t maxBodySize = 512 * 1024;
  char* responseBuffer = nullptr;
#if defined(ESP32)
  if (psramFound()) {
    responseBuffer = (char*)heap_caps_malloc(maxBodySize + 1, MALLOC_CAP_SPIRAM);
  }
#endif
  if (!responseBuffer) {
    Serial.println("Failed to allocate response buffer");
    http.end();
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }

  // Read chunked response
  size_t bytesRead = 0;
  unsigned long startMs = millis();
  Serial.println("Reading response...");

  while (http.connected() && bytesRead < maxBodySize) {
    String chunkSizeLine;
    while (stream->available() && chunkSizeLine.length() < 16) {
      char c = stream->read();
      if (c == '\n') break;
      if (c != '\r') chunkSizeLine += c;
    }
    chunkSizeLine.trim();
    int semicolon = chunkSizeLine.indexOf(';');
    if (semicolon >= 0) chunkSizeLine = chunkSizeLine.substring(0, semicolon);
    chunkSizeLine.trim();
    
    if (chunkSizeLine.length() == 0) {
      delay(10);
      if (millis() - startMs > 30000) break;
      continue;
    }
    
    size_t chunkSize = 0;
    for (size_t i = 0; i < chunkSizeLine.length(); i++) {
      char c = chunkSizeLine.charAt(i);
      uint8_t nibble = 0;
      if (c >= '0' && c <= '9') nibble = c - '0';
      else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
      else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
      else break;
      chunkSize = (chunkSize << 4) + nibble;
    }
    
    if (chunkSize == 0) break;
    if (bytesRead + chunkSize > maxBodySize) break;
    
    size_t got = 0;
    while (got < chunkSize && (millis() - startMs < 30000)) {
      if (stream->available()) {
        size_t toRead = min(chunkSize - got, (size_t)stream->available());
        size_t n = stream->readBytes(responseBuffer + bytesRead + got, toRead);
        got += n;
      } else {
        delay(1);
      }
    }
    bytesRead += got;
    if (got < chunkSize) break;
    
    for (int i = 0; i < 2 && stream->available(); i++) stream->read();
    if (bytesRead % 50000 < 32000) Serial.print(".");
  }

  responseBuffer[bytesRead] = '\0';
  http.end();

  Serial.printf("\nDownloaded %u bytes\n", (unsigned)bytesRead);

  // Manual JSON parsing - find "audioContent":"..."
  // Format: {"audioContent": "base64data..."}
  const char* needle = "\"audioContent\":";
  char* audioContentPos = strstr(responseBuffer, needle);
  
  if (!audioContentPos) {
    Serial.println("audioContent not found in JSON");
    heap_caps_free(responseBuffer);
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }
  
  // Skip to opening quote after the colon
  char* start = strchr(audioContentPos + strlen(needle), '"');
  if (!start) {
    Serial.println("audioContent value quote not found");
    heap_caps_free(responseBuffer);
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }
  start++; // Skip the opening "
  
  // Find closing quote
  char* end = start;
  while (*end && *end != '"') {
    if (*end == '\\') end++; // Skip escaped chars
    end++;
  }
  
  if (*end != '"') {
    Serial.println("audioContent closing quote not found");
    heap_caps_free(responseBuffer);
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }
  
  size_t b64Len = end - start;
  Serial.printf("Extracted %u b64 chars, starting playback...\n", (unsigned)b64Len);
  
  // Temporarily null-terminate for decoding
  char savedChar = *end;
  *end = '\0';
  
  streamDecodeAndPlay(start);
  
  *end = savedChar;
  heap_caps_free(responseBuffer);

  digitalWrite(PIN_RED, HIGH);
  ttsPlaying = false;
  ttsCooldownUntilMs = millis() + 500;
  stopSpeakerNoise();
}

void streamDecodeAndPlay(const char* b64Str) {
  size_t b64Len = strlen(b64Str);
  if (b64Len < 100) {
    Serial.println("audioContent too short");
    return;
  }

  Serial.printf("Starting decode & play (b64 len: %u)\n", (unsigned)b64Len);

  // Chunk size: decodes to ~4KB, multiple of 4 for base64
  const size_t B64_CHUNK = 5332;
  const size_t AUDIO_CHUNK = (B64_CHUNK * 3) / 4 + 100;

  uint8_t* audioBuffer = nullptr;
#if defined(ESP32)
  if (psramFound()) {
    audioBuffer = (uint8_t*)heap_caps_malloc(AUDIO_CHUNK, MALLOC_CAP_SPIRAM);
  }
  if (!audioBuffer) {
    Serial.println("PSRAM alloc failed or not available, trying heap...");
    audioBuffer = (uint8_t*)malloc(AUDIO_CHUNK);
  }
#else
  audioBuffer = (uint8_t*)malloc(AUDIO_CHUNK);
#endif
  if (!audioBuffer) {
    Serial.println("Audio buffer alloc failed completely");
    return;
  }

  bool headerParsed = false;
  uint32_t sampleRate = 24000;
  uint16_t channels = 1;
  size_t totalAudioBytes = 0;
  size_t dataStartOffset = 0;

  size_t totalChunks = (b64Len + B64_CHUNK - 1) / B64_CHUNK;
  Serial.printf("Processing %u chunks...\n", (unsigned)totalChunks);

  for (size_t offset = 0; offset < b64Len; offset += B64_CHUNK) {
    size_t remaining = b64Len - offset;
    size_t chunkSize = (remaining < B64_CHUNK) ? remaining : B64_CHUNK;

    chunkSize = (chunkSize / 4) * 4;
    if (chunkSize == 0) break;

    size_t decodedLen = 0;
    int ret = mbedtls_base64_decode(audioBuffer, AUDIO_CHUNK, &decodedLen,
                                    (const uint8_t*)(b64Str + offset), chunkSize);

    if (ret != 0 && ret != MBEDTLS_ERR_BASE64_INVALID_CHARACTER) {
      Serial.printf("Decode error at offset %u: %d\n", (unsigned)offset, ret);
      continue;
    }

    if (decodedLen == 0) continue;

    if (!headerParsed && offset == 0) {
      if (decodedLen < 44) {
        Serial.println("First chunk too small for WAV header");
        break;
      }

      if (audioBuffer[0] != 'R' || audioBuffer[1] != 'I' ||
          audioBuffer[2] != 'F' || audioBuffer[3] != 'F') {
        Serial.println("Invalid WAV signature");
        break;
      }

      size_t pos = 12;
      while (pos + 8 <= decodedLen && pos < 200) {
        uint32_t chunkLen = audioBuffer[pos+4] | (audioBuffer[pos+5] << 8) |
                            (audioBuffer[pos+6] << 16) | (audioBuffer[pos+7] << 24);

        if (memcmp(audioBuffer + pos, "fmt ", 4) == 0) {
          if (pos + 16 <= decodedLen) {
            channels = audioBuffer[pos+10] | (audioBuffer[pos+11] << 8);
            sampleRate = audioBuffer[pos+12] | (audioBuffer[pos+13] << 8) |
                        (audioBuffer[pos+14] << 16) | (audioBuffer[pos+15] << 24);
          }
        }

        if (memcmp(audioBuffer + pos, "data", 4) == 0) {
          dataStartOffset = pos + 8;
          Serial.printf("WAV header: %u Hz, %s, data at offset %u\n",
                       (unsigned)sampleRate,
                       channels == 1 ? "Mono" : "Stereo",
                       (unsigned)dataStartOffset);

          i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                     channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
          i2s_zero_dma_buffer(I2S_NUM_1);

          headerParsed = true;
          break;
        }

        pos += 8 + chunkLen;
        if (chunkLen & 1) pos++;
      }

      if (!headerParsed) {
        Serial.println("Failed to parse WAV header");
        break;
      }

      if (decodedLen > dataStartOffset) {
        size_t audioLen = decodedLen - dataStartOffset;
        applyVolumeToPcm16(audioBuffer + dataStartOffset, audioLen);

        size_t written = 0;
        i2s_write(I2S_NUM_1, audioBuffer + dataStartOffset, audioLen, &written, portMAX_DELAY);
        totalAudioBytes += written;
      }
    } else if (headerParsed) {
      applyVolumeToPcm16(audioBuffer, decodedLen);

      size_t written = 0;
      i2s_write(I2S_NUM_1, audioBuffer, decodedLen, &written, portMAX_DELAY);
      totalAudioBytes += written;
    }

    if (totalChunks > 10 && (offset / B64_CHUNK) % 10 == 0) {
      Serial.print(".");
    }
  }

  Serial.printf("\nPlayed %u audio bytes total\n", (unsigned)totalAudioBytes);

  uint8_t silence[512] = {0};
  size_t written = 0;
  i2s_write(I2S_NUM_1, silence, 512, &written, 100);

  free(audioBuffer);
}
