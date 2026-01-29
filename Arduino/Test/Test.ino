#include <driver/i2s.h>
#include <WiFi.h>

// --- PIN DEFINITIONS ---
#define I2S_MIC_SCK 12
#define I2S_MIC_WS  13
#define I2S_MIC_SD  14

#define I2S_SPK_LRC 4
#define I2S_SPK_BCLK 5
#define I2S_SPK_DIN 6

#define PIN_RED     1
#define PIN_GREEN   2

// VOLUME CONTROL (32-BIT MODE)
// 0 = Max Volume (Might crash 3.3V rail!)
// 2 = High (Standard)
// 4 = Medium
// 6 = Low/Safe
const int volumeShift = 0; // Start with 4 to be safe for your power rail

void setup() {
  // 1. Kill WiFi to save power
  WiFi.mode(WIFI_OFF);
  
  Serial.begin(115200);

  // 2. Setup LEDs
  pinMode(PIN_RED, OUTPUT);   digitalWrite(PIN_RED, HIGH); // OFF
  pinMode(PIN_GREEN, OUTPUT); digitalWrite(PIN_GREEN, LOW); // ON (System Ready)

  // 3. Speaker Setup (Output)
  i2s_config_t spk_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // <--- FIXED: MONO (Prevents Chipmunk pitch)
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_pin_config_t spk_pins = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_1, &spk_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &spk_pins);

  // 4. Microphone Setup (Input)
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_pin_config_t mic_pins = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_0, &mic_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);
  
  Serial.println("System Ready. Blow into mic.");
}

void loop() {
  size_t bytes_read = 0;
  size_t bytes_written = 0;
  
  // Buffers
  int32_t buffer[512]; 

  // Read Audio
  i2s_read(I2S_NUM_0, &buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
  
  if (bytes_read > 0) {
    int samples = bytes_read / 4; 
    long signal_energy = 0;
    
    // Process Audio
    for (int i = 0; i < samples; i++) {
      // Volume Control
      buffer[i] = buffer[i] >> volumeShift;
      signal_energy += abs(buffer[i]);
    }
    
    // Write Audio
    i2s_write(I2S_NUM_1, &buffer, bytes_read, &bytes_written, portMAX_DELAY);

    // Visualizer (Red LED)
    // Threshold adjusted for 32-bit values
    if ((signal_energy / samples) > 1000000) { 
      digitalWrite(PIN_RED, LOW); // LED ON
    } else {
      digitalWrite(PIN_RED, HIGH);
    }
  }
}