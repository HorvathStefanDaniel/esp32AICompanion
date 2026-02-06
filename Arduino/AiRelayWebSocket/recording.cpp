#include "recording.h"
#include "audio_utils.h"
#include "chat_utils.h"
#include "stt.h"
#include "tts.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>
#include <driver/i2s.h>

void runMicTest() {
  int32_t buffer[512];
  size_t bytes_read = 0;
  size_t bytes_written = 0;
  i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
  if (bytes_read > 0) {
    int samples = bytes_read / 4;
    int32_t signal_energy = 0;
    int16_t outBuffer[512];
    for (int i = 0; i < samples; i++) {
      int32_t sample = buffer[i] >> (14 + micTestVolumeShift);
      sample = (sample * outputVolumePercent) / 100;
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      outBuffer[i] = (int16_t)sample;
      signal_energy += abs(buffer[i] >> 14);
    }
    i2s_write(I2S_NUM_1, outBuffer, samples * 2, &bytes_written, portMAX_DELAY);
    if ((signal_energy / samples) > silenceThreshold) {
      digitalWrite(PIN_RED, LOW);
    } else {
      digitalWrite(PIN_RED, HIGH);
    }
  }
}

void processAudio(int dataSize) {
  ledWaiting = true;
  String text = transcribeAudio(dataSize);
  Serial.println("You said: " + text);
  if (text.length() > 0) {
    String reply = getChatResponse(text);
    Serial.println("AI says: " + reply);
    speakGroqTTS(reply);
  }
  ledWaiting = false;
}

void RecordAudio(bool holdToRecord) {
  Serial.println("Recording...");
  ledRecording = true;
  size_t bytes_read = 0;
  if (!recording_buffer) return;
  memset(recording_buffer, 0, bufferSize);
  int32_t buffer[256];
  for (int i = 0; i < 2; i++) {
    i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, 100);
  }
  int flash_wr_size = 0;
  uint64_t sum_abs = 0;
  uint32_t samples_total = 0;
  while (flash_wr_size < waveDataSize) {
    if (holdToRecord) {
#if USE_EXT_BUTTON
      if (digitalRead(BUTTON_IN) == HIGH) break;
#elif USE_BOOT_BUTTON
      if (digitalRead(BOOT_BUTTON) == HIGH) break;
#else
      break;
#endif
    }
    i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
    if (bytes_read > 0) {
      int samples_read = bytes_read / 4;
      if (flash_wr_size + (samples_read * 2) > waveDataSize) break;
      int16_t* wav_buffer_ptr = (int16_t*)(recording_buffer + headerSize + flash_wr_size);
      for (int i = 0; i < samples_read; i++) {
        int16_t sample = (int16_t)(buffer[i] >> 14);
        wav_buffer_ptr[i] = sample;
        sum_abs += abs(sample);
        samples_total++;
      }
      flash_wr_size += (samples_read * 2);
    }
  }
  uint32_t avg_abs = samples_total ? (uint32_t)(sum_abs / samples_total) : 0;
  createWavHeader(recording_buffer, flash_wr_size);
  ledRecording = false;
  Serial.printf("Avg Level: %lu\n", avg_abs);
  if (flash_wr_size <= 1000) {
    Serial.println("Too short");
  } else if (avg_abs < silenceThreshold) {
    Serial.println("Too quiet");
  } else {
    processAudio(flash_wr_size + headerSize);
  }
}
