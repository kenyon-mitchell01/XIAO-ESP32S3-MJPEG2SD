#include "arduino_secrets.h"

/*
* Capture ESP32 Cam JPEG images into a AVI file and store on SD
* AVI files stored on the SD card can also be selected and streamed to a browser as MJPEG.
*
* s60sc 2020 - 2024
*/

#include "appGlobals.h"
#include <esp_task_wdt.h>

unsigned long lastUpload = 0;
const unsigned long uploadInterval = 60 * 1000; // 1 minute for testing

void setup() {
  Serial.begin(115200);
  delay(2000); // Wait 2 seconds to ensure Serial Monitor can connect
  
  Serial.println("Starting setup...");
  Serial.println("Using SD card in 1-bit SPI mode with pins:");
  Serial.printf("CLK: %d (GPIO7), CMD: %d (GPIO9), D0: %d (GPIO8)\n", 
               SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  Serial.println("Note: GPIO21 used for CS (not directly configured in SD_MMC)");
  
  // rest of your setup code
  logSetup();
  LOG_INF("Selected board %s", CAM_BOARD);
  // prep storage
  if (startStorage()) {
    // Load saved user configuration
    if (loadConfig()) {
#ifndef AUXILIARY
      // initialise camera
      if (psramFound()) {
        if (ESP.getPsramSize() > 1 * ONEMEG) prepCam();
        else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Insufficient PSRAM for app: %s", fmtSize(ESP.getPsramSize()));
      } else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Need PSRAM to be enabled");
#else
      LOG_INF("AUXILIARY mode without camera");
#endif
    }
  }
  
#ifdef DEV_ONLY
  devSetup();
#endif

  // connect wifi or start config AP if router details not available
  startWifi();

  startWebServer();
  if (strlen(startupFailure)) LOG_WRN("%s", startupFailure);
  else {
    // start rest of services
#ifndef AUXILIARY
    startSustainTasks(); 
#endif
#if INCLUDE_SMTP
    prepSMTP(); 
#endif
#if INCLUDE_FTP_HFS
    prepUpload();
#endif
#if INCLUDE_UART
    prepUart();
#endif
#if INCLUDE_PERIPH
    prepPeripherals();
  #if INCLUDE_MCPWM 
    prepMotors();
  #endif
#endif
#if INCLUDE_AUDIO
    prepAudio(); 
#endif
#if INCLUDE_TGRAM
    prepTelegram();
#endif
#if INCLUDE_I2C
  prepI2C();
  #if INCLUDE_TELEM
    prepTelemetry();
  #endif
#endif
#if INCLUDE_PERIPH
    startHeartbeat();
#endif
#ifndef AUXILIARY
    prepRecording(); 
 #if INCLUDE_RTSP
    prepRTSP();
 #endif
#endif
    checkMemory();

  }  
  esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 30000, // 30 seconds
        .idle_core_mask = 0, // No specific core restrictions
        .trigger_panic = false // Disable panic on timeout
  };
  //* Disable Watchdog Temporarily (Debugging)
  esp_task_wdt_init(&wdt_config); // Set watchdog timeout to 30s, disable panic 
   // Add this line before vTaskDelete(NULL)
  optimizeTasksAndMemory();
    
    // End with this
  vTaskDelete(NULL);
}

void loop() {
  // confirm not blocked in setup
  LOG_INF("=============== Total tasks: %u ===============\n", uxTaskGetNumberOfTasks() - 1);
  unsigned long now = millis();
  if (now - lastUpload > uploadInterval) {
    uploadRecordings();
    lastUpload = now;
  }
  delay(1000);

  vTaskDelete(NULL);
}

// Add this to EMJPEG2SD_DopamineChaser247_Version1.ino
void optimizeTasksAndMemory() {
    // Reduce number of tasks by disabling unnecessary features
    Serial.println("Optimizing tasks and memory usage...");
    
    // Configure watchdog with longer timeout
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 30000,          // 30 second timeout
        .idle_core_mask = 0,          // No specific core restrictions
        .trigger_panic = true         // Enable panic on timeout for debugging
    };
    esp_task_wdt_init(&wdt_config);
    
    // Print current task info
    uint32_t totalTasks = uxTaskGetNumberOfTasks();
    Serial.printf("Initial number of tasks: %u\n", totalTasks);
    
    // Disable unnecessary features to reduce task count
#if INCLUDE_TGRAM
    // Disable Telegram if not needed
    tgramUse = false;
#endif

#if INCLUDE_SMTP
    // Disable email if not needed
    smtpUse = false;
#endif

#if INCLUDE_MQTT
    // Disable MQTT if not needed
    mqtt_active = false;
#endif

#if INCLUDE_PERIPH
    // Disable unnecessary peripherals
    if (!pirUse) {
        // If PIR not used, disable related tasks
        SVactive = false;
    }
    
    // Disable audio if not needed (can be resource intensive)
#if INCLUDE_AUDIO
    if (!motionTriggeredAudio) {
        AudActive = false;
        micGain = 0;  // Turn off microphone
    }
#endif
#endif

    // Reduce video streaming tasks if not needed
    streamVid = false;  // Disable additional video stream
    streamAud = false;  // Disable audio streaming
    streamSrt = false;  // Disable subtitle streaming
    numStreams = 1;     // Only use primary stream
    
    // Set lower frame sizes and rates for better performance
    fsizePtr = FRAMESIZE_VGA;  // Lower resolution
    FPS = 10;                  // Lower frame rate
    
    // Apply settings
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, (framesize_t)fsizePtr);
        setFPS(FPS);
        
        // Lower quality for better performance
        s->set_quality(s, 12);
        
        // Lower saturation and contrast slightly for better compression
        s->set_saturation(s, 0);
        s->set_contrast(s, 0);
    }
    
    // Print optimized task count
    totalTasks = uxTaskGetNumberOfTasks();
    Serial.printf("Optimized number of tasks: %u\n", totalTasks);
    
    // Print memory stats
    Serial.printf("Free heap: %u, largest block: %u\n", 
                 ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    Serial.printf("Free PSRAM: %u\n", ESP.getFreePsram());
    
    Serial.println("Task and memory optimization complete");
}
