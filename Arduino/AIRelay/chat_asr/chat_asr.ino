#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <SPIFFS.h>
#include "Audio.h"      
// INCLUDE THE NEW I2S DRIVER
#include "driver/i2s_std.h"


//import secrets
#include "secrets.h"

// ======================= CONFIGURATION =======================

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* groq_api_key = GROQ_API_KEY;

const char* tts_model = "canopylabs/orpheus-v1-english"; 
const char* tts_voice = "autumn"; 


// ======================= PINS =======================

// Speaker (MAX98357A)
#define I2S_DOUT      6
#define I2S_BCLK      5
#define I2S_LRC       4

// Mic (INMP441)
#define I2S_MIC_SCK   12
#define I2S_MIC_WS    13
#define I2S_MIC_SD    14

#define BOOT_BUTTON   0
#define PIN_RED       1   
#define PIN_GREEN     2   

// ======================= GLOBALS =======================

Audio audio;
WiFiClientSecure client;
i2s_chan_handle_t rx_handle = NULL; // New I2S Handle for Mic

// Audio Recording Settings
#define SAMPLE_RATE 16000
#define RECORD_TIME_SECONDS 6 
const int headerSize = 44;
const int waveDataSize = RECORD_TIME_SECONDS * 32000; 
const int bufferSize = headerSize + waveDataSize;
uint8_t *recording_buffer; 

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
    if (!client.connect("api.groq.com", 443)) return "";
    
    String boundary = "------------------------ESP32Bound";
    String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    String modelParam = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + "distil-whisper-large-v3-en" + "\r\n";

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
        audio.loop(); 
    }
    client.print(tail);

    String response = "";
    bool headerEnd = false;
    while(client.connected()) {
        String line = client.readStringUntil('\n');
        if(line == "\r") headerEnd = true;
        if(headerEnd) response += line;
    }
    client.stop();
    
    int jsonStart = response.indexOf("{");
    if(jsonStart == -1) return "";
    String jsonStr = response.substring(jsonStart);
    JsonDocument doc;
    deserializeJson(doc, jsonStr);
    return doc["text"].as<String>();
}

String getChatResponse(String input) {
    Serial.println("Sending to Groq (LLM)...");
    HTTPClient http;
    http.begin("https://api.groq.com/openai/v1/chat/completions");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(groq_api_key));
    
    JsonDocument doc;
    doc["model"] = "llama3-8b-8192";
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
    } 
    http.end();
    return result;
}

void speakGroqTTS(String text) {
    Serial.println("Requesting Groq TTS...");
    digitalWrite(PIN_RED, LOW); 

    HTTPClient http;
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
        File file = SPIFFS.open("/tts.wav", FILE_WRITE);
        if (file) {
            http.writeToStream(&file);
            file.close();
            
            Serial.println("Audio saved. Playing...");
            audio.connecttoFS(SPIFFS, "/tts.wav");

            unsigned long start = millis();
            while (audio.isRunning() || millis() - start < 1000) {
                audio.loop();
                if (!audio.isRunning() && millis() - start > 500) break; 
            }
        }
    } else {
        Serial.printf("TTS Error: %d\n", httpCode);
    }
    http.end();
    digitalWrite(PIN_RED, HIGH); 
}

void processAudio(int dataSize) {
    digitalWrite(PIN_RED, LOW); 
    String text = transcribeAudio(dataSize);
    Serial.println("You said: " + text);
    
    if (text.length() > 0) {
        String reply = getChatResponse(text);
        Serial.println("AI says: " + reply);
        speakGroqTTS(reply); 
    }
    digitalWrite(PIN_RED, HIGH); 
}

void RecordAudio() {
  Serial.println("Recording...");
  digitalWrite(PIN_GREEN, LOW); 
  
  memset(recording_buffer, 0, bufferSize);
  
  size_t bytes_read = 0;
  int flash_wr_size = 0;
  
  // Enable the RX channel
  i2s_channel_enable(rx_handle);

  // Clear buffer (pop noise)
  i2s_channel_read(rx_handle, (char*)recording_buffer, 1024, &bytes_read, 100);

  while (digitalRead(BOOT_BUTTON) == LOW && flash_wr_size < waveDataSize) {
    // Read using new API
    i2s_channel_read(rx_handle, (char*)(recording_buffer + headerSize + flash_wr_size), 1024, &bytes_read, 100);
    flash_wr_size += bytes_read;
    audio.loop(); 
  }
  
  // Disable RX channel
  i2s_channel_disable(rx_handle);

  createWavHeader(recording_buffer, flash_wr_size);
  digitalWrite(PIN_GREEN, HIGH); 
  
  if (flash_wr_size > 4000) {
      processAudio(flash_wr_size + headerSize);
  }
}

// ======================= SETUP =======================

void setup() {
  delay(1000);
  Serial.begin(115200);
  
  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  digitalWrite(PIN_RED, HIGH); 
  digitalWrite(PIN_GREEN, HIGH);

  if(psramInit()){
      Serial.println("\nPSRAM Initialized");
      recording_buffer = (uint8_t*)ps_malloc(bufferSize);
  } else {
      Serial.println("\nPSRAM FAILED! Enable OPI PSRAM in Tools menu.");
      while(1);
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  
  // 1. SPEAKER SETUP (Using Audio Library)
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(14); 

  // 2. MICROPHONE SETUP (Using New ESP32 v3.0 API)
  i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle);

  i2s_std_config_t rx_std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)I2S_MIC_SCK,
          .ws = (gpio_num_t)I2S_MIC_WS,
          .dout = I2S_GPIO_UNUSED,
          .din = (gpio_num_t)I2S_MIC_SD,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  
  // Initialize the channel
  i2s_channel_init_std_mode(rx_handle, &rx_std_cfg);

  client.setInsecure(); 
  Serial.println("System Ready. Hold BOOT to speak.");
}

void loop() {
  audio.loop();
  
  if (digitalRead(BOOT_BUTTON) == LOW) {
      RecordAudio();
  }
}