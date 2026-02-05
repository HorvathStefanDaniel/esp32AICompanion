#ifndef AI_RELAY_WEBSOCKET_AUDIO_UTILS_H
#define AI_RELAY_WEBSOCKET_AUDIO_UTILS_H

#include <Arduino.h>

// Speaker format and volume
void updateSpeakerFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample);
void applyVolumeToPcm16(uint8_t* buffer, size_t bytes);
void stopSpeakerNoise();

// WAV header and playback
void createWavHeader(uint8_t* header, int waveDataSize);
void playWavFile(const char* filename);
void playMp3File(const char* filename);

#endif
