#include "tts.h"
#include "audio_utils.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Groq TTS: WiFi not connected!");
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    return;
  }
  
  HTTPClient http;
  http.setTimeout(20000);
  http.setReuse(false); // Don't reuse connections
  
  Serial.println("Groq TTS: Connecting...");
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
  
  Serial.println("Groq TTS: Sending POST...");
  int httpCode = http.POST(payload);
  if (httpCode == 200) {
    Serial.println("Groq TTS: Success!");
    File outfile = SPIFFS.open("/tts.wav", FILE_WRITE);
    if (outfile) {
      http.writeToStream(&outfile);
      outfile.close();
      playWavFile("/tts.wav");
    }
  } else {
    Serial.printf("TTS Error: %d\n", httpCode);
    if (httpCode < 0) {
      Serial.printf("Connection failed. WiFi status: %d\n", WiFi.status());
    }
  }
  http.end();
  digitalWrite(PIN_RED, HIGH);
  ttsPlaying = false;
  ttsCooldownUntilMs = millis() + 800;
  stopSpeakerNoise();
}

// Read chunked HTTP body in small chunks; find "audioContent" base64; decode and play PCM as we go.
static inline bool isBase64Char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

bool streamGoogleTTSChunked(Stream* stream, HTTPClient& http) {
  const char* needle = "\"audioContent\":";
  const size_t needleLen = 15;
  size_t needleIdx = 0;
  int state = 0;  // 0=match needle, 1=skip to opening quote, 2=in base64, 3=done
  bool afterBackslash = false;

  const size_t B64_BLOCK = 4096;   // decode in blocks of 4096 chars -> 3072 bytes
  const size_t READ_BUF = 2048;
  const size_t HEADER_BUF = 256;

  char* readBuf = (char*)malloc(READ_BUF);
  char* b64Buf = (char*)malloc(B64_BLOCK + 4);
  uint8_t* pcmBuf = (uint8_t*)malloc(3072);
  uint8_t* headerBuf = (uint8_t*)malloc(HEADER_BUF);
  if (!readBuf || !b64Buf || !pcmBuf || !headerBuf) {
    Serial.println("Stream: alloc failed");
    if (readBuf) free(readBuf);
    if (b64Buf) free(b64Buf);
    if (pcmBuf) free(pcmBuf);
    if (headerBuf) free(headerBuf);
    return false;
  }
  size_t b64Len = 0;
  size_t headerLen = 0;
  bool wavParsed = false;
  uint32_t sampleRate = 24000;
  uint16_t channels = 1;
  size_t dataOffset = 0;
  size_t dataSize = 0;
  size_t totalPlayed = 0;

  auto flushDecode = [&](size_t decodeChars, size_t decodedBytes) {
    if (decodedBytes == 0) return;
    if (!wavParsed) {
      size_t toCopy = (HEADER_BUF - headerLen) < decodedBytes ? (HEADER_BUF - headerLen) : decodedBytes;
      memcpy(headerBuf + headerLen, pcmBuf, toCopy);
      headerLen += toCopy;
      if (headerLen >= HEADER_BUF) {
        if (headerBuf[0] != 'R' || headerBuf[1] != 'I' || headerBuf[2] != 'F' || headerBuf[3] != 'F') {
          Serial.println("Stream: invalid WAV");
          return;
        }
        size_t pos = 12;
        while (pos + 8 <= HEADER_BUF && pos < 200) {
          uint32_t chunkLen = headerBuf[pos+4] | (headerBuf[pos+5]<<8) | (headerBuf[pos+6]<<16) | (headerBuf[pos+7]<<24);
          if (memcmp(headerBuf + pos, "fmt ", 4) == 0 && pos + 16 <= HEADER_BUF) {
            channels = headerBuf[pos+10] | (headerBuf[pos+11]<<8);
            sampleRate = headerBuf[pos+12] | (headerBuf[pos+13]<<8) | (headerBuf[pos+14]<<16) | (headerBuf[pos+15]<<24);
          }
          if (memcmp(headerBuf + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize = chunkLen;
            break;
          }
          pos += 8 + chunkLen;
          if (chunkLen & 1) pos++;
        }
        if (dataOffset == 0 || dataSize == 0) {
          Serial.println("Stream: no data chunk");
          return;
        }
        i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
        i2s_zero_dma_buffer(I2S_NUM_1);
        wavParsed = true;
        size_t headerPcm = HEADER_BUF - dataOffset;
        applyVolumeToPcm16(headerBuf + dataOffset, headerPcm);
        size_t written = 0;
        i2s_write(I2S_NUM_1, headerBuf + dataOffset, headerPcm, &written, portMAX_DELAY);
        totalPlayed += written;
        size_t fromFirst = decodedBytes - toCopy;
        if (fromFirst > 0) {
          applyVolumeToPcm16(pcmBuf + toCopy, fromFirst);
          i2s_write(I2S_NUM_1, pcmBuf + toCopy, fromFirst, &written, portMAX_DELAY);
          totalPlayed += written;
        }
      }
    } else {
      applyVolumeToPcm16(pcmBuf, decodedBytes);
      size_t written = 0;
      i2s_write(I2S_NUM_1, pcmBuf, decodedBytes, &written, portMAX_DELAY);
      totalPlayed += written;
    }
  };

  unsigned long startMs = millis();
  size_t totalBytesRead = 0;

  while (http.connected() && state != 3 && (millis() - startMs < 60000)) {
    String chunkSizeLine;
    while (stream->available() && chunkSizeLine.length() < 16) {
      char c = stream->read();
      if (c == '\n') break;
      if (c != '\r') chunkSizeLine += c;
    }
    chunkSizeLine.trim();
    int sc = chunkSizeLine.indexOf(';');
    if (sc >= 0) chunkSizeLine = chunkSizeLine.substring(0, sc);
    chunkSizeLine.trim();
    if (chunkSizeLine.length() == 0) {
      delay(5);
      continue;
    }
    size_t chunkSize = 0;
    for (size_t i = 0; i < chunkSizeLine.length(); i++) {
      char c = chunkSizeLine.charAt(i);
      uint8_t n = 0;
      if (c >= '0' && c <= '9') n = c - '0';
      else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
      else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
      else break;
      chunkSize = (chunkSize << 4) + n;
    }
    if (chunkSize == 0) break;

    while (chunkSize > 0 && (millis() - startMs < 60000)) {
      size_t toRead = (chunkSize < READ_BUF) ? chunkSize : READ_BUF;
      size_t n = stream->readBytes((uint8_t*)readBuf, toRead);
      if (n == 0) { delay(1); continue; }
      chunkSize -= n;
      totalBytesRead += n;

      for (size_t i = 0; i < n && state != 3; i++) {
        char c = readBuf[i];
        if (state == 0) {
          if (c == needle[needleIdx]) {
            needleIdx++;
            if (needleIdx == needleLen) { state = 1; needleIdx = 0; }
          } else needleIdx = 0;
          continue;
        }
        if (state == 1) {
          if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
          if (c == '"') { state = 2; continue; }
          state = 0;
          continue;
        }
        if (state == 2) {
          if (afterBackslash) {
            afterBackslash = false;
            if (c == '"' || c == '\\') { if (b64Len < B64_BLOCK + 4) b64Buf[b64Len++] = c; }
            continue;
          }
          if (c == '\\') { afterBackslash = true; continue; }
          if (c == '"') { state = 3; break; }
          if (isBase64Char(c)) {
            if (b64Len < B64_BLOCK + 4) b64Buf[b64Len++] = c;
            if (b64Len >= B64_BLOCK) {
              size_t toDecode = B64_BLOCK;
              size_t outLen = 0;
              int ret = mbedtls_base64_decode(pcmBuf, 3072, &outLen, (const uint8_t*)b64Buf, toDecode);
              if (ret == 0 && outLen > 0) flushDecode(toDecode, outLen);
              memmove(b64Buf, b64Buf + toDecode, b64Len - toDecode);
              b64Len -= toDecode;
            }
          }
        }
      }
    }
    for (int i = 0; i < 2 && stream->available(); i++) stream->read();
  }

  if (b64Len > 0 && state == 3) {
    size_t padLen = b64Len;
    while (padLen % 4 != 0) padLen++;
    if (padLen > b64Len) {
      for (size_t i = b64Len; i < padLen; i++) b64Buf[i] = '=';
      b64Buf[padLen] = '\0';
    }
    size_t maxOut = (padLen * 3) / 4 + 4;
    if (maxOut > 3072) maxOut = 3072;
    size_t outLen = 0;
    int ret = mbedtls_base64_decode(pcmBuf, maxOut, &outLen, (const uint8_t*)b64Buf, padLen);
    if (ret == 0 && outLen > 0) flushDecode(padLen, outLen);
  }

  free(readBuf);
  free(b64Buf);
  free(pcmBuf);
  free(headerBuf);

  if (!wavParsed) {
    Serial.println("Stream: no WAV header received");
    return false;
  }
  uint8_t silence[512] = {0};
  size_t written = 0;
  i2s_write(I2S_NUM_1, silence, 512, &written, 100);
  return true;
}

void speakGoogleTTS(const String& text) {
  if (text.length() == 0) return;
  Serial.println("Requesting Google TTS...");
  digitalWrite(PIN_RED, LOW);
  ttsPlaying = true;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Google TTS: WiFi not connected!");
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }

  HTTPClient http;
  http.setTimeout(60000);
  http.setReuse(false); // Don't reuse connections
  String url = String("https://texttospeech.googleapis.com/v1/text:synthesize?key=") + google_tts_api_key;
  
  Serial.println("Google TTS: Connecting...");
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

  Serial.println("Streaming download and play...");
  bool streamOk = streamGoogleTTSChunked(stream, http);
  http.end();

  if (!streamOk) {
    Serial.println("Streaming failed, falling back to Groq");
    digitalWrite(PIN_RED, HIGH);
    ttsPlaying = false;
    speakGroqTTS(text);
    return;
  }

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

  // mbedtls requires base64 length to be a multiple of 4; pad with '=' if needed
  size_t padLen = b64Len;
  while (padLen % 4 != 0) padLen++;
  const uint8_t* decodeSrc = (const uint8_t*)b64Str;
  size_t decodeLen = b64Len;
  char* paddedB64 = nullptr;
  if (padLen != b64Len) {
    paddedB64 = (char*)malloc(padLen + 1);
    if (!paddedB64) {
      Serial.println("Failed to allocate padding buffer");
      return;
    }
    memcpy(paddedB64, b64Str, b64Len);
    for (size_t i = b64Len; i < padLen; i++) paddedB64[i] = '=';
    paddedB64[padLen] = '\0';
    decodeSrc = (const uint8_t*)paddedB64;
    decodeLen = padLen;
    Serial.printf("Padded base64 to %u chars (was %u)\n", (unsigned)padLen, (unsigned)b64Len);
  }

  Serial.printf("Decoding %u b64 chars...\n", (unsigned)decodeLen);

  // Calculate decoded size (base64: 4 chars = 3 bytes)
  size_t maxDecodedSize = (decodeLen * 3) / 4 + 100;

  uint8_t* wavBuffer = nullptr;
  bool wavInPsram = false;
#if defined(ESP32)
  if (psramFound()) {
    wavBuffer = (uint8_t*)heap_caps_malloc(maxDecodedSize, MALLOC_CAP_SPIRAM);
    if (wavBuffer) wavInPsram = true;
  }
#endif
  if (!wavBuffer) {
    wavBuffer = (uint8_t*)malloc(maxDecodedSize);
  }
  if (!wavBuffer) {
    Serial.println("Failed to allocate WAV buffer");
    if (paddedB64) free(paddedB64);
    return;
  }

  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(wavBuffer, maxDecodedSize, &decodedLen,
                                   decodeSrc, decodeLen);
  if (paddedB64) free(paddedB64);

  if (ret != 0) {
    Serial.printf("Base64 decode error: %d\n", ret);
    if (wavInPsram) heap_caps_free(wavBuffer); else free(wavBuffer);
    return;
  }

  Serial.printf("Decoded %u bytes\n", (unsigned)decodedLen);
  // Debug: WAV header (RIFF size, format)
  if (decodedLen >= 8) {
    uint32_t riffSize = (uint32_t)wavBuffer[4] | ((uint32_t)wavBuffer[5] << 8) |
                        ((uint32_t)wavBuffer[6] << 16) | ((uint32_t)wavBuffer[7] << 24);
    Serial.printf("WAV RIFF size: %u, file size: %u\n", (unsigned)riffSize, (unsigned)decodedLen);
  }

  // Validate WAV header
  if (decodedLen < 44 || wavBuffer[0] != 'R' || wavBuffer[1] != 'I' ||
      wavBuffer[2] != 'F' || wavBuffer[3] != 'F') {
    Serial.println("Invalid WAV file");
    if (wavInPsram) heap_caps_free(wavBuffer); else free(wavBuffer);
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
    if (wavInPsram) heap_caps_free(wavBuffer); else free(wavBuffer);
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

  // Play in 32KB chunks for faster playback (less loop overhead)
  const size_t PLAY_CHUNK = 32768;
  size_t totalWritten = 0;

  for (size_t offset = 0; offset < dataSize; offset += PLAY_CHUNK) {
    size_t toWrite = (dataSize - offset) < PLAY_CHUNK ? (dataSize - offset) : PLAY_CHUNK;
    size_t written = 0;
    i2s_write(I2S_NUM_1, wavBuffer + dataOffset + offset, toWrite, &written, portMAX_DELAY);
    totalWritten += written;
    if ((offset / PLAY_CHUNK) % 3 == 0) Serial.print(".");
  }

  Serial.printf("\nPlayed %u bytes\n", (unsigned)totalWritten);

  uint8_t silence[512] = {0};
  size_t written = 0;
  i2s_write(I2S_NUM_1, silence, 512, &written, 100);

  if (wavInPsram) heap_caps_free(wavBuffer); else free(wavBuffer);
}
