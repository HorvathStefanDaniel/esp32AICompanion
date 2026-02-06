#ifndef AI_RELAY_WEBSOCKET_TTS_H
#define AI_RELAY_WEBSOCKET_TTS_H

#include <Arduino.h>

class HTTPClient;  // forward declaration (full header only in tts.cpp)

void speakGroqTTS(String text);
void speakGoogleTTS(const String& text);
void streamDecodeAndPlay(const char* b64Str);
// Streaming: read chunked HTTP, extract base64, decode and play in chunks (no full-body buffer).
bool streamGoogleTTSChunked(Stream* stream, HTTPClient& http);

#endif
