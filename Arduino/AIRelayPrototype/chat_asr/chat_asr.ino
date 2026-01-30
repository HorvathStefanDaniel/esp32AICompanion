#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <SPIFFS.h>
#include <driver/i2s.h>  // Using Legacy Driver for everything (Stable)
#include "secrets.h" 

// --- BUG FIX 1: Removed the Serial definition block that broke USB ---

// ======================= CONFIGURATION =======================

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* groq_api_key = GROQ_API_KEY;

const char* tts_model = "canopylabs/orpheus-v1-english"; 
const char* tts_voice = "autumn"; 
const char* stt_model = "whisper-large-v3-turbo"; 
const char* llm_model = "llama-3.3-70b-versatile"; 

// ======================= PINS =======================

// Speaker (MAX98357A)
#define I2S_SPK_BCLK  5
#define I2S_SPK_LRC   4
#define I2S_SPK_DIN   6

// Mic (INMP441)
#define I2S_MIC_SCK   12
#define I2S_MIC_WS    13
#define I2S_MIC_SD    14

#define BOOT_BUTTON   0
#define USE_BOOT_BUTTON 0
#define USE_EXT_BUTTON 1
#define BUTTON_IN    16
#define BUTTON_GND   18
#define PIN_RED       1   
#define PIN_GREEN     2   

// ======================= GLOBALS =======================

WiFiClientSecure client;

// Audio Recording Settings
#define SAMPLE_RATE 16000
#define RECORD_TIME_SECONDS 3 // Keep small for internal RAM safety
#define SILENCE_THRESHOLD 500 // Increased threshold slightly
#define STT_RESPONSE_TIMEOUT_MS 20000

const int headerSize = 44;
const int waveDataSize = RECORD_TIME_SECONDS * SAMPLE_RATE * 2; 
const int bufferSize = headerSize + waveDataSize;

uint8_t *recording_buffer; 

volatile bool ledRecording = false;
volatile bool ledWaiting = false;

void ledTask(void*){
  bool greenOn = false;
  for(;;){
    if (ledRecording) {
      digitalWrite(PIN_RED, LOW);    // red ON
      digitalWrite(PIN_GREEN, HIGH); // green OFF
    } else if (ledWaiting) {
      digitalWrite(PIN_RED, HIGH); // red OFF
      greenOn = !greenOn;
      digitalWrite(PIN_GREEN, greenOn ? LOW : HIGH); // flash green
    } else {
      digitalWrite(PIN_RED, HIGH);   // red OFF
      digitalWrite(PIN_GREEN, LOW);  // green ON (ready)
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ======================= HELPER FUNCTIONS =======================

void createWavHeader(uint8_t *header, int waveDataSize){
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  unsigned int fileSize = waveDataSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 0x10; header[17] = 0x00; header[18] = 0x00; header[19] = 0x00;
  header[20] = 0x01; header[21] = 0x00;
  header[22] = 0x01; header[23] = 0x00; // Mono
  header[24] = 0x80; header[25] = 0x3E; header[26] = 0x00; header[27] = 0x00; // 16000 Hz
  header[28] = 0x00; header[29] = 0x7D; header[30] = 0x00; header[31] = 0x00; // Byte Rate
  header[32] = 0x02; header[33] = 0x00;
  header[34] = 0x10; header[35] = 0x00; // 16-bit
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (byte)(waveDataSize & 0xFF);
  header[41] = (byte)((waveDataSize >> 8) & 0xFF);
  header[42] = (byte)((waveDataSize >> 16) & 0xFF);
  header[43] = (byte)((waveDataSize >> 24) & 0xFF);
}

String transcribeAudio(int dataLength) {
    Serial.println("Sending to Groq (STT)...");
    
    client.setTimeout(15000);
    // Use setInsecure again just to be safe
    client.setInsecure();
    
    if (!client.connect("api.groq.com", 443)) {
        Serial.println("Connection failed");
        return "";
    }
    
    String boundary = "------------------------ESP32Bound";
    String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    String modelParam = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + stt_model + "\r\n";

    int contentLength = head.length() + dataLength + tail.length() + modelParam.length();

    client.println("POST /openai/v1/audio/transcriptions HTTP/1.1");
    client.println("Host: api.groq.com");
    client.println("Authorization: Bearer " + String(groq_api_key));
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.print("Content-Length: "); client.println(contentLength);
    client.println();
    
    client.print(modelParam);
    client.print(head);
    
    uint8_t *fb = recording_buffer;
    int chunk = 1024;
    for(int i=0; i<dataLength; i+=chunk){
        if(i+chunk > dataLength) chunk = dataLength - i;
        client.write(fb+i, chunk);
    }
    client.print(tail);

    // Simple response reader
    String response = "";
    bool headerEnd = false;
    unsigned long startRead = millis();
    while (client.connected() && millis() - startRead < 10000) {
        while (client.available()) {
            char c = client.read();
            if (!headerEnd) {
                if (c == '{') { // Quick hack to find JSON start
                     headerEnd = true;
                     response += c;
                }
            } else {
                response += c;
            }
            startRead = millis(); // Reset timeout on data
        }
        delay(10);
    }
    client.stop();
    
    // Parse
    int jsonStart = response.indexOf("{");
    if(jsonStart == -1 && response.length() > 0 && response[0] == '{') jsonStart = 0;
    
    if(jsonStart == -1) {
        Serial.println("Failed to find JSON in response");
        return "";
    }
    
    String jsonStr = response.substring(jsonStart);
    JsonDocument doc;
    deserializeJson(doc, jsonStr);
    return doc["text"].as<String>();
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
    sysMsg["content"] = "You are a helpful friend. Keep answers short (max 30 words).";
    
    JsonObject userMsg = messages.add<JsonObject>();
    userMsg["role"] = "user";
    userMsg["content"] = input;
    
    String payload;
    serializeJson(doc, payload);
    
    int httpCode = http.POST(payload);
    String result = "";
    if(httpCode == 200) {
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

void playWavFile(const char* filename) {
    File file = SPIFFS.open(filename, FILE_READ);
    if (!file) {
        Serial.println("Failed to open WAV file");
        return;
    }

    // --- PARSE WAV HEADER ---
    uint8_t header[44];
    if (file.read(header, 44) < 44) {
        Serial.println("WAV file too short");
        file.close();
        return;
    }

    // Extract Sample Rate (Bytes 24-27, Little Endian)
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    
    // Extract Channels (Bytes 22-23) - 1 = Mono, 2 = Stereo
    uint16_t channels = header[22] | (header[23] << 8);

    Serial.printf("Playing: %d Hz, %s\n", sampleRate, (channels == 2) ? "Stereo" : "Mono");

    // --- RECONFIGURE SPEAKER ---
    // This helper updates the clock without reinstalling the driver
    // Note: I2S_CHANNEL_MONO usually maps to ONLY_LEFT in legacy driver logic for clock setting
    i2s_set_clk(I2S_NUM_1, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);

    // --- PLAYBACK ---
    uint8_t buffer[1024]; 
    size_t bytes_read = 0;
    size_t bytes_written = 0;
    
    while(file.available()) {
        bytes_read = file.read(buffer, sizeof(buffer));
        if(bytes_read > 0) {
            i2s_write(I2S_NUM_1, buffer, bytes_read, &bytes_written, portMAX_DELAY);
        }
    }
    
    // Silence at end to prevent pop
    uint8_t silence[512] = {0};
    i2s_write(I2S_NUM_1, silence, 512, &bytes_written, 100);
    
    file.close();
    
    // Optional: Reset back to standard 16k if needed (though next file will auto-fix itself)
    // i2s_set_clk(I2S_NUM_1, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    
    Serial.println("Playback done");
}

void speakGroqTTS(String text) {
    Serial.println("Requesting Groq TTS...");
    digitalWrite(PIN_RED, LOW); // Busy (Red ON)

    HTTPClient http;
    http.setTimeout(15000);
    http.begin("https://api.groq.com/openai/v1/audio/speech");
    http.addHeader("Authorization", "Bearer " + String(groq_api_key));
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["model"] = tts_model;
    doc["voice"] = tts_voice;
    doc["input"] = text;
    doc["response_format"] = "wav";

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);

    if (httpCode == 200) {
        File outfile = SPIFFS.open("/tts.wav", FILE_WRITE);
        if (outfile) {
            http.writeToStream(&outfile);
            outfile.close();
            // Play the WAV file
            playWavFile("/tts.wav");
        }
    } else {
        Serial.printf("TTS Error: %d\n", httpCode);
    }
    http.end();
    
    digitalWrite(PIN_RED, HIGH); // Busy (Red OFF)
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
  // Safety check
  if(!recording_buffer) return;
  memset(recording_buffer, 0, bufferSize);
  
  int32_t buffer[256]; // Smaller buffer to fit stack
  
  // Flush buffer
  for(int i=0; i<2; i++) {
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
      // Read 32-bit audio from mic
      i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
      
      if(bytes_read > 0) {
          int samples_read = bytes_read / 4;
          
          if (flash_wr_size + (samples_read * 2) > waveDataSize) break;
          
          int16_t* wav_buffer_ptr = (int16_t*)(recording_buffer + headerSize + flash_wr_size);
          
          for(int i=0; i<samples_read; i++){
              // Shift 32-bit to 16-bit. 
              // INMP441 is 24-bit data left-aligned in 32-bit slot.
              // We need to shift right. try >> 14 for good volume.
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
  } else if (avg_abs < SILENCE_THRESHOLD) {
      Serial.println("Too quiet");
  } else {
      processAudio(flash_wr_size + headerSize);
  }
}

// ======================= SETUP =======================

void setup() {
  delay(3000); // Wait for USB

  Serial.begin(115200);
  Serial.println("\n\nStarting Voice Assistant...");

  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }

#if USE_BOOT_BUTTON
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
#endif
#if USE_EXT_BUTTON
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_IN, INPUT_PULLUP);
#endif
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  digitalWrite(PIN_RED, HIGH); 
  digitalWrite(PIN_GREEN, HIGH); // START OFF

  // Allocate memory (Safe malloc)
  recording_buffer = (uint8_t*)malloc(bufferSize);
  if(recording_buffer == NULL) {
      Serial.println("RAM Allocation Failed");
      while(1);
  }
  Serial.println("RAM Allocated");

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
  
  // --- SPEAKER SETUP (16-BIT MODE) ---
  // BUG FIX 3: Speaker must be 16-bit to match WAV files
  i2s_config_t spk_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // <--- CHANGED TO 16-BIT
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
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

  // --- MIC SETUP (32-BIT MODE) ---
  // INMP441 requires 32-bit clocks
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // <--- KEPT AT 32-BIT
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
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

  client.setInsecure(); 
  
  Serial.println("\n🎤 READY!");
  Serial.println("Type 'r' in Serial Monitor to record.");
#if USE_BOOT_BUTTON
  Serial.println("Hold BOOT button to speak.");
#endif
#if USE_EXT_BUTTON
  Serial.println("Hold external button (GPIO16 -> GPIO18) to speak.");
#endif

  ledRecording = false;
  ledWaiting = false;
  xTaskCreate(ledTask, "ledTask", 2048, NULL, 1, NULL);
}

void loop() {
#if USE_EXT_BUTTON
  if (digitalRead(BUTTON_IN) == LOW) {
    delay(30); // Debounce
    if(digitalRead(BUTTON_IN) == LOW) {
      Serial.println("External button trigger");
      RecordAudio(true);
      while(digitalRead(BUTTON_IN) == LOW) { delay(10); }
    }
  }
#elif USE_BOOT_BUTTON
  if (digitalRead(BOOT_BUTTON) == LOW) {
    delay(30); // Debounce
    if(digitalRead(BOOT_BUTTON) == LOW) {
      Serial.println("BOOT button trigger");
      RecordAudio(true);
      while(digitalRead(BOOT_BUTTON) == LOW) { delay(10); }
    }
  }
#endif

  while (Serial.available() > 0) {
    char c = Serial.read();
    Serial.printf("Serial RX: %c\n", c);
    if (c == 'r' || c == 'R' || c == '\n' || c == '\r') {
      Serial.println("Serial trigger -> start recording");
      RecordAudio(false);
    }
  }

  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 2000) {
    Serial.println("Loop alive");
    lastHeartbeat = millis();
  }
}