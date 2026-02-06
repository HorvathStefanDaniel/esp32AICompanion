#include "chat_utils.h"
#include "config.h"
#include "globals.h"
#include "prompts.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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
  if (httpCode == 200) {
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
