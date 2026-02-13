// Customize assistant prompts here
#ifndef PROMPTS_H
#define PROMPTS_H

#include <Arduino.h>

// Array of available prompts
extern const char* PROMPTS[];

// Number of prompts available
#define PROMPT_COUNT 5

// Current prompt index (default to 0)
extern uint8_t currentPromptIndex;

// Get the current system prompt
const char* getCurrentPrompt();

// Get the first line of the current prompt
String getCurrentPromptFirstLine();

// Get the Google TTS voice name for the current prompt
const char* getCurrentPromptVoice();

// Get SSML speaking rate for the current prompt (0.25 to 4.0, 1.0 = normal)
float getCurrentPromptSpeakingRate();

// Get SSML pitch for the current prompt (-20.0 to 20.0 semitones, 0.0 = normal)
float getCurrentPromptPitch();

#endif
