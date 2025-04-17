#ifndef MOTIONDETECT_H
#define MOTIONDETECT_H

#include "esp_camera.h" // For camera_fb_t

// Declare functions
bool isNight(uint8_t nightPix);
//*bool checkMotion(camera_fb_t* fb, bool motionStatus, bool capturing);
bool checkMotion(camera_fb_t* fb, bool motionStatus, bool lightLevelOnly); // Remove = false
void notifyMotion(camera_fb_t* fb);

#endif // MOTIONDETECT_H