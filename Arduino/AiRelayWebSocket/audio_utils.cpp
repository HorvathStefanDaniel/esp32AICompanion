#include "audio_utils.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <driver/i2s.h>
#include "DAZI-AI-main/src/mp3_decoder/mp3_decoder.h"

void updateSpeakerFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
  if (bitsPerSample != 16) {
    Serial.println("Unsupported bits per sample for speaker");
    return;
  }
  i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
              (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
}

void applyVolumeToPcm16(uint8_t* buffer, size_t bytes) {
  if (outputVolumePercent == 100 || bytes < 2) return;
  int samples = bytes / 2;
  int16_t* samplesPtr = (int16_t*)buffer;
  for (int i = 0; i < samples; i++) {
    int32_t scaled = (samplesPtr[i] * outputVolumePercent) / 100;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    samplesPtr[i] = (int16_t)scaled;
  }
}

void stopSpeakerNoise() {
  static uint8_t silence[1024] = {0};
  size_t bytes_written = 0;
  for (int i = 0; i < 8; i++) {
    i2s_write(I2S_NUM_1, silence, sizeof(silence), &bytes_written, 50);
  }
  vTaskDelay(pdMS_TO_TICKS(30));
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_stop(I2S_NUM_1);
  vTaskDelay(pdMS_TO_TICKS(10));
  i2s_start(I2S_NUM_1);
}

void createWavHeader(uint8_t* header, int waveDataSize) {
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  unsigned int fileSize = waveDataSize + headerSize - 8;
  header[4] = (uint8_t)(fileSize & 0xFF);
  header[5] = (uint8_t)((fileSize >> 8) & 0xFF);
  header[6] = (uint8_t)((fileSize >> 16) & 0xFF);
  header[7] = (uint8_t)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 0x10; header[17] = 0x00; header[18] = 0x00; header[19] = 0x00;
  header[20] = 0x01; header[21] = 0x00;
  header[22] = 0x01; header[23] = 0x00;
  header[24] = 0x80; header[25] = 0x3E; header[26] = 0x00; header[27] = 0x00;
  header[28] = 0x00; header[29] = 0x7D; header[30] = 0x00; header[31] = 0x00;
  header[32] = 0x02; header[33] = 0x00;
  header[34] = 0x10; header[35] = 0x00;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (uint8_t)(waveDataSize & 0xFF);
  header[41] = (uint8_t)((waveDataSize >> 8) & 0xFF);
  header[42] = (uint8_t)((waveDataSize >> 16) & 0xFF);
  header[43] = (uint8_t)((waveDataSize >> 24) & 0xFF);
}

void playWavFile(const char* filename) {
  File file = SPIFFS.open(filename, FILE_READ);
  if (!file) {
    Serial.println("Failed to open WAV file");
    return;
  }
  uint8_t header[44];
  if (file.read(header, 44) < 44) {
    Serial.println("WAV file too short");
    file.close();
    return;
  }
  uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
  uint16_t channels = header[22] | (header[23] << 8);
  if (sampleRate == 0 || sampleRate > 48000) {
    Serial.println("Invalid WAV sample rate, skipping playback");
    file.close();
    return;
  }
  if (channels == 0 || channels > 2) {
    Serial.println("Invalid WAV channels, skipping playback");
    file.close();
    return;
  }
  Serial.printf("Playing: %d Hz, %s\n", sampleRate, (channels == 2) ? "Stereo" : "Mono");
  i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
  uint8_t buffer[1024];
  size_t bytes_read = 0;
  size_t bytes_written = 0;
  while (file.available()) {
    bytes_read = file.read(buffer, sizeof(buffer));
    if (bytes_read > 0) {
      applyVolumeToPcm16(buffer, bytes_read);
      i2s_write(I2S_NUM_1, buffer, bytes_read, &bytes_written, portMAX_DELAY);
    }
  }
  uint8_t silence[512] = {0};
  i2s_write(I2S_NUM_1, silence, 512, &bytes_written, 100);
  file.close();
  Serial.println("Playback done");
}

void playMp3File(const char* filename) {
  File file = SPIFFS.open(filename, FILE_READ);
  if (!file) {
    Serial.println("Failed to open MP3 file");
    return;
  }
  size_t fileSize = file.size();
  Serial.printf("Playing MP3: %u bytes\n", (unsigned)fileSize);
  if (!MP3Decoder_AllocateBuffers()) {
    Serial.println("Failed to allocate MP3 decoder");
    file.close();
    return;
  }
  uint8_t* mp3Data = (uint8_t*)malloc(fileSize);
  if (!mp3Data) {
    Serial.println("Failed to allocate MP3 buffer");
    MP3Decoder_FreeBuffers();
    file.close();
    return;
  }
  size_t totalRead = 0;
  while (totalRead < fileSize && file.available()) {
    size_t bytesRead = file.read(mp3Data + totalRead, fileSize - totalRead);
    if (bytesRead == 0) break;
    totalRead += bytesRead;
  }
  file.close();
  if (totalRead != fileSize) {
    Serial.printf("MP3 read error: got %u of %u bytes\n", (unsigned)totalRead, (unsigned)fileSize);
    free(mp3Data);
    MP3Decoder_FreeBuffers();
    return;
  }
  uint8_t* readPtr = mp3Data;
  int32_t bytesLeft = fileSize;
  int16_t outBuffer[2304];
  size_t bytes_written = 0;
  bool firstFrame = true;
  int framesDecoded = 0;
  size_t totalSamples = 0;
  i2s_zero_dma_buffer(I2S_NUM_1);
  int errorCount = 0;
  while (bytesLeft > 0) {
    int32_t offset = MP3FindSyncWord(readPtr, bytesLeft);
    if (offset < 0) break;
    readPtr += offset;
    bytesLeft -= offset;
    if (bytesLeft < 4) break;
    uint8_t* savedPtr = readPtr;
    int32_t savedBytesLeft = bytesLeft;
    int32_t result = MP3Decode(readPtr, &bytesLeft, outBuffer, 0);
    int32_t bytesConsumed = savedBytesLeft - bytesLeft;
    readPtr = savedPtr + bytesConsumed;
    if (result == ERR_MP3_NONE) {
      framesDecoded++;
      if (firstFrame) {
        MP3GetLastFrameInfo();
        int sampleRate = MP3GetSampRate();
        int channels = MP3GetChannels();
        Serial.printf("MP3: %d Hz, %d ch\n", sampleRate, channels);
        i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                    channels == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
        firstFrame = false;
      }
      int outputSamps = MP3GetOutputSamps();
      totalSamples += outputSamps;
      for (int i = 0; i < outputSamps; i++) {
        outBuffer[i] = (int16_t)((int32_t)outBuffer[i] * outputVolumePercent / 100);
      }
      i2s_write(I2S_NUM_1, outBuffer, outputSamps * 2, &bytes_written, portMAX_DELAY);
      errorCount = 0;
    } else if (result == ERR_MP3_INDATA_UNDERFLOW) {
      if (bytesLeft < 1024) break;
      errorCount++;
      if (errorCount > 10) break;
      readPtr++;
      bytesLeft--;
    } else {
      errorCount++;
      if (errorCount > 50) break;
      if (bytesConsumed == 0) {
        readPtr++;
        bytesLeft--;
      }
    }
  }
  Serial.printf("MP3: %d frames, %u samples, %d bytes remaining\n", framesDecoded, (unsigned)totalSamples, bytesLeft);
  free(mp3Data);
  MP3Decoder_FreeBuffers();
  uint8_t silence[512] = {0};
  i2s_write(I2S_NUM_1, silence, 512, &bytes_written, 100);
  Serial.println("MP3 playback done");
}
