This needs:
- ArduinoJson by Benoit
- Websockets by Marcus 

As for the settings, for an ESP32s3 n16r8 board, settings on Arduino IDE should be:
ESP32S3 Dev Module selected
USB CDC On Boot: Disabled
CPU Freq: 240MHZ(wifi)
Flash Mode: QIO 80MHz
Flash size: 16MB(128Mb)
Partition scheme: "8M with spiffs (3MB APP/1.5MB SPIFFS)"
PSRAM: OPI PSRAM

and the serial monitor should be in 115200 baud