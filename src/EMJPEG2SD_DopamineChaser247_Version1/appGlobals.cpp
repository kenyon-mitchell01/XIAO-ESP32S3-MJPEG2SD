#include "appGlobals.h"

// Define global variables
float motionVal = 0.0;      // Motion sensitivity (%)
uint8_t colorDepth = 0;     // Camera color depth
uint8_t nightSwitch = 0;    // Night/day switch threshold
uint8_t lightLevel = 0;     // Light level
bool motionTriggeredAudio = false; // Audio trigger flag
bool useMotion = true;      // Motion detection enabled by default
uint8_t minSeconds = 30;         // Default minimum recording duration (seconds)
bool stopPlayback = false;  // Playback stop flag
int ampVol = 0;             // Default amplifier volume
int micGain = 0;            // Default microphone gain
bool nightTime = false;     // Default night mode status