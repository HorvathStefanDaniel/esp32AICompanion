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

  // Find audioContent field
  const char* needle = "\"audioContent\":";
  char* audioContentPos = strstr(responseBuffer, needle);
  
  if (!audioContentPos) {
    Serial.println("audioContent not found");
    heap_caps_free(responseBuffer);
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }
  
  // Skip to opening quote
  char* start = strchr(audioContentPos + strlen(needle), '"');
  if (!start) {
    Serial.println("Opening quote not found");
    heap_caps_free(responseBuffer);
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }
  start++; // Skip "
  
  // Allocate clean buffer for unescaped base64 (in PSRAM)
  char* cleanB64 = nullptr;
#if defined(ESP32)
  if (psramFound()) {
    cleanB64 = (char*)heap_caps_malloc(220000, MALLOC_CAP_SPIRAM);
  }
#endif
  if (!cleanB64) {
    Serial.println("Failed to allocate clean buffer");
    heap_caps_free(responseBuffer);
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }
  
  // Copy and unescape: remove \n and other JSON escapes
  char* dst = cleanB64;
  char* src = start;
  size_t cleanLen = 0;
  
  while (*src && *src != '"' && cleanLen < 219999) {
    if (*src == '\\') {
      src++; // Skip backslash
      if (*src == 'n' || *src == 'r' || *src == 't') {
        // Skip escaped whitespace chars - they shouldn't be in base64
        src++;
        continue;
      } else if (*src == '"' || *src == '\\') {
        // Keep escaped quote or backslash (though unlikely in base64)
        *dst++ = *src++;
        cleanLen++;
      } else {
        // Unknown escape, skip it
        src++;
      }
    } else {
      *dst++ = *src++;
      cleanLen++;
    }
  }
  *dst = '\0';
  
  Serial.printf("Cleaned base64: %u chars (removed %u chars)\n", 
                (unsigned)cleanLen, (unsigned)(dst - cleanB64 - cleanLen));
  
  streamDecodeAndPlay(cleanB64);
  
  heap_caps_free(cleanB64);
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

  Serial.printf("Decoding %u b64 chars...\n", (unsigned)b64Len);

  // Calculate decoded size (base64: 4 chars = 3 bytes)
  size_t maxDecodedSize = (b64Len * 3) / 4 + 100;
  
  // Allocate buffer for entire decoded WAV (in PSRAM)
  uint8_t* wavBuffer = nullptr;
#if defined(ESP32)
  if (psramFound()) {
    wavBuffer = (uint8_t*)heap_caps_malloc(maxDecodedSize, MALLOC_CAP_SPIRAM);
  }
#endif
  if (!wavBuffer) {
    Serial.println("Failed to allocate WAV buffer");
    return;
  }

  // Decode entire base64 at once
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(wavBuffer, maxDecodedSize, &decodedLen,
                                   (const uint8_t*)b64Str, b64Len);

  if (ret != 0) {
    Serial.printf("Base64 decode error: %d\n", ret);
    heap_caps_free(wavBuffer);
    return;
  }

  Serial.printf("Decoded %u bytes\n", (unsigned)decodedLen);

  // Validate WAV header
  if (decodedLen < 44 || wavBuffer[0] != 'R' || wavBuffer[1] != 'I' ||
      wavBuffer[2] != 'F' || wavBuffer[3] != 'F') {
    Serial.println("Invalid WAV file");
    heap_caps_free(wavBuffer);
    return;
  }

  // Parse WAV header
  uint32_t sampleRate = 24000;
  uint16_t channels = 1;
  size_t dataOffset = 0;
  size_t dataSize = 0;

  size_t pos = 12;
  while (pos + 8 <= decodedLen && pos < 200) {
    uint32_t chunkLen = wavBuffer[pos+4] | (wavBuffer[pos+5] << 8) |
                        (wavBuffer[pos+6] << 16) | (wavBuffer[pos+7] << 24);

    if (memcmp(wavBuffer + pos, "fmt ", 4) == 0) {
      if (pos + 16 <= decodedLen) {
        channels = wavBuffer[pos+10] | (wavBuffer[pos+11] << 8);
        sampleRate = wavBuffer[pos+12] | (wavBuffer[pos+13] << 8) |
                    (wavBuffer[pos+14] << 16) | (wavBuffer[pos+15] << 24);
      }
    }

    if (memcmp(wavBuffer + pos, "data", 4) == 0) {
      dataOffset = pos + 8;
      dataSize = chunkLen;
      break;
    }

    pos += 8 + chunkLen;
    if (chunkLen & 1) pos++;
  }

  if (dataOffset == 0 || dataSize == 0) {
    Serial.println("No data chunk found");
    heap_caps_free(wavBuffer);
    return;
  }

  if (dataOffset + dataSize > decodedLen) {
    dataSize = decodedLen - dataOffset;
  }

  Serial.printf("WAV: %u Hz, %s, %u bytes PCM\n",
                (unsigned)sampleRate,
                channels == 1 ? "Mono" : "Stereo",
                (unsigned)dataSize);

  // Configure I2S
  i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
             channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_NUM_1);

  // Apply volume to entire buffer at once
  applyVolumeToPcm16(wavBuffer + dataOffset, dataSize);

  // Play in larger chunks (8KB at a time for smooth playback)
  const size_t PLAY_CHUNK = 8192;
  size_t totalWritten = 0;

  for (size_t offset = 0; offset < dataSize; offset += PLAY_CHUNK) {
    size_t toWrite = min((size_t)PLAY_CHUNK, dataSize - offset);
    size_t written = 0;
    
    i2s_write(I2S_NUM_1, wavBuffer + dataOffset + offset, toWrite, &written, portMAX_DELAY);
    totalWritten += written;
    
    // Progress indicator
    if ((offset / PLAY_CHUNK) % 5 == 0) {
      Serial.print(".");
    }
  }

  Serial.printf("\nPlayed %u bytes\n", (unsigned)totalWritten);

  // Flush with silence
  uint8_t silence[512] = {0};
  size_t written = 0;
  i2s_write(I2S_NUM_1, silence, 512, &written, 100);

  heap_caps_free(wavBuffer);
}
