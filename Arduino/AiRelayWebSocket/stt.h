#ifndef AI_RELAY_WEBSOCKET_STT_H
#define AI_RELAY_WEBSOCKET_STT_H

#include <Arduino.h>
#include <WebSocketsClient.h>

void handleWsTextMessage(const uint8_t* payload, size_t length);
void wsEvent(WStype_t type, uint8_t* payload, size_t length);
void streamMicFrame();
String transcribeAudio(int dataLength);

#endif
