#ifndef AI_RELAY_WEBSOCKET_TTS_H
#define AI_RELAY_WEBSOCKET_TTS_H

#include <Arduino.h>

void speakGroqTTS(String text);
void speakGoogleTTS(const String& text);
void streamDecodeAndPlay(const char* b64Str);

#endif
