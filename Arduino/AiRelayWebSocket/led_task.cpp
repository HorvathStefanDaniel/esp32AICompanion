#include "led_task.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>

void ledTask(void*) {
  for (;;) {
    if (!listeningEnabled) {
      // Mic muted by switch: both LEDs off
      digitalWrite(PIN_RED, HIGH);
      digitalWrite(PIN_GREEN, HIGH);
    } else if (ledRecording) {
      digitalWrite(PIN_RED, LOW);
      digitalWrite(PIN_GREEN, HIGH);
    } else if (ledWaiting) {
      digitalWrite(PIN_RED, HIGH);
      digitalWrite(PIN_GREEN, LOW);
    } else {
      digitalWrite(PIN_RED, HIGH);
      digitalWrite(PIN_GREEN, LOW);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
