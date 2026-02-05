#ifndef AI_RELAY_WEBSOCKET_RECORDING_H
#define AI_RELAY_WEBSOCKET_RECORDING_H

#include <Arduino.h>

void processAudio(int dataSize);
void RecordAudio(bool holdToRecord);
void runMicTest();

#endif
