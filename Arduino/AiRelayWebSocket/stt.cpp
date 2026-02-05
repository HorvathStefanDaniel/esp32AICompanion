#include "stt.h"
#include "chat_utils.h"
#include "tts.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

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

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
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
      Serial.printf("WS BIN received: %d bytes\n", length);
      break;
    case WStype_ERROR:
      Serial.printf("WS ERROR: %s\n", payload ? (char*)payload : "unknown");
      break;
    case WStype_PING:
      Serial.println("WS PING");
      break;
    case WStype_PONG:
      lastWsActivityMs = millis();
      break;
    default:
      Serial.printf("WS event: %d\n", type);
      break;
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

String transcribeAudio(int dataLength) {
  Serial.println("Sending to Groq (STT)...");
  client.setTimeout(15000);
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
  client.print("Content-Length: ");
  client.println(contentLength);
  client.println();
  client.print(modelParam);
  client.print(head);
  uint8_t* fb = recording_buffer;
  int chunk = 1024;
  for (int i = 0; i < dataLength; i += chunk) {
    if (i + chunk > dataLength) chunk = dataLength - i;
    client.write(fb + i, chunk);
  }
  client.print(tail);
  String response = "";
  bool headerEnd = false;
  unsigned long startRead = millis();
  while (client.connected() && millis() - startRead < 10000) {
    while (client.available()) {
      char c = client.read();
      if (!headerEnd) {
        if (c == '{') {
          headerEnd = true;
          response += c;
        }
      } else {
        response += c;
      }
      startRead = millis();
    }
    delay(10);
  }
  client.stop();
  int jsonStart = response.indexOf("{");
  if (jsonStart == -1 && response.length() > 0 && response[0] == '{') jsonStart = 0;
  if (jsonStart == -1) {
    Serial.println("Failed to find JSON in response");
    return "";
  }
  String jsonStr = response.substring(jsonStart);
  JsonDocument doc;
  deserializeJson(doc, jsonStr);
  return doc["text"].as<String>();
}
