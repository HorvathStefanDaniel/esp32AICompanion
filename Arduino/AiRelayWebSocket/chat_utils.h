#ifndef AI_RELAY_WEBSOCKET_CHAT_UTILS_H
#define AI_RELAY_WEBSOCKET_CHAT_UTILS_H

#include <Arduino.h>

// String helpers
String toLowerCopy(const String& input);
String trimCopy(const String& input);
String extractBetween(const String& text, int startIdx, int endIdx);

// Wake/end word processing
String processWakeAndEndWords(const String& finalTranscript);

// Chat history
void clearChatHistory();
void addHistory(const char* role, const String& content);

// LLM
String getChatResponse(String input);

#endif
