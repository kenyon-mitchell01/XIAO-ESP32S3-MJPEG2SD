/*
* Capture ESP32 Cam JPEG images into a AVI file and store on SD
* matches file writes to the SD card sector size.
* AVI files stored on the SD card can also be selected and streamed to a browser.
*
* s60sc 2020, 2022, 2024
*/

#include "appGlobals.h"
#include "motionDetect.h"
#include "esp_camera.h" // For camera_fb_t

// Define states
#define STATE_IDLE 0
#define STATE_RECORDING 1
#define STATE_SAVING 2

#define MIN_RECORDING_TIME_MS (minSeconds * 1000) // Convert to milliseconds
#define MAX_RECORDING_TIME_MS (5 * 60 * 1000)     // 5 minutes maximum
#define COOLDOWN_TIME_MS (5 * 1000)               // 5 seconds cooldown

static uint32_t recordingStartTime = 0; // Tracks when recording started (in milliseconds)
const uint32_t MIN_RECORDING_TIME = 30 * 1000; // 30 seconds minimum recording time
const uint32_t MAX_RECORDING_TIME = 5 * 60 * 1000; // 5 minutes maximum recording time
const uint32_t MOTION_CHECK_INTERVAL = 5 * 1000; // Check motion every 5 seconds
static uint32_t lastMotionCheckTime = 0; // Tracks the last time motion was checked

#define FB_CNT 4 // number of frame buffers

// user parameters set from web
//*bool useMotion  = true; // whether to use camera for motion detection (with motionDetect.cpp)
bool dbgMotion  = false;
bool forceRecord = false; // Recording enabled by rec button

bool captureMotion = false; // Ensure global scope, Added by Grok's suggestion 04/15/2025
extern bool motionTriggeredAudio; // Declaration, not a definition


// motion detection parameters
int moveStartChecks = 5; // checks per second for start motion 
int moveStopSecs = 5; // Check motion every 5 seconds during recording
int maxFrames = 20000; // maximum number of frames in video before auto close 



// record timelapse avi independently of motion capture, file name has same format as avi except ends with T
int tlSecsBetweenFrames; // too short interval will interfere with other activities
int tlDurationMins; // a new file starts when previous ends
int tlPlaybackFPS;  // rate to playback the timelapse, min 1 


// status & control fields
uint8_t FPS = 0;


//*bool nightTime = false;
uint8_t fsizePtr; // index to frameData[]
//uint8_t minSeconds = 30; // Match MIN_RECORDING_TIME
bool doRecording = true; // whether to capture to SD or not 
uint8_t xclkMhz = 20; // camera clock rate MHz
bool doKeepFrame = false;
static bool haveSrt = false;
char camModel[10];
static int siodGpio = SIOD_GPIO_NUM;
static int siocGpio = SIOC_GPIO_NUM;
size_t maxFrameBuffSize;
framesize_t maxFS = FRAMESIZE_SVGA; // default




// header and reporting info
static uint32_t vidSize; // total video size
static uint16_t frameCnt;
static uint32_t startTime; // total overall time
static uint32_t dTimeTot; // total frame decode/monitor time
static uint32_t fTimeTot; // total frame buffering time
static uint32_t wTimeTot; // total SD write time
static uint32_t oTime; // file opening time
static uint32_t cTime; // file closing time
static uint32_t sTime; // file streaming time
static uint32_t frameInterval; // units of us between frames

// SD card storage
uint8_t iSDbuffer[(RAMSIZE + CHUNK_HDR) * 2];
static size_t highPoint;
static File aviFile;
static char aviFileName[FILE_NAME_LEN];

// SD playback
static File playbackFile;
static char partName[FILE_NAME_LEN];
static size_t readLen;
static uint8_t recFPS;
static uint32_t recDuration;
uint8_t saveFPS = 0; // used to reset FPS after playback
//static uint8_t saveFPS = 99; Claude suggested I use the above SaveFPS definition
bool doPlayback = false; // controls playback

// task control
TaskHandle_t captureHandle = NULL;
TaskHandle_t playbackHandle = NULL;
static SemaphoreHandle_t readSemaphore;
static SemaphoreHandle_t playbackSemaphore;
SemaphoreHandle_t frameSemaphore[MAX_STREAMS] = {NULL};
SemaphoreHandle_t motionSemaphore = NULL;
SemaphoreHandle_t aviMutex = NULL;
static volatile bool isPlaying = false; // controls playback on app
bool isCapturing = false;
//*bool stopPlayback = false; // controls if playback allowed
bool timeLapseOn = false;
static bool pirVal = false;

/**************** timers & ISRs ************************/

static void IRAM_ATTR frameISR() {
  // interrupt at current frame rate
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (isPlaying) xSemaphoreGiveFromISR (playbackSemaphore, &xHigherPriorityTaskWoken ); // notify playback to send frame
  vTaskNotifyGiveFromISR(captureHandle, &xHigherPriorityTaskWoken); // wake capture task to process frame
  if (xHigherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

void controlFrameTimer(bool restartTimer) {
  // frame timer control
  static hw_timer_t* frameTimer = NULL;
  // stop current timer
  if (frameTimer) {
    timerDetachInterrupt(frameTimer); 
    timerEnd(frameTimer);
    frameTimer = NULL;
  }
  if (restartTimer) {
    // (re)start timer interrupt for required framerate
    frameTimer = timerBegin(OneMHz); 
    if (frameTimer) {
      uint32_t frameInterval = OneMHz / FPS; // in units of us 
      LOG_VRB("Frame timer interval %ums for FPS %u", frameInterval/1000, FPS); 
      timerAttachInterrupt(frameTimer, &frameISR);
      timerAlarm(frameTimer, frameInterval, true, 0); // micro seconds
    } else LOG_ERR("Failed to setup frameTimer");
  }
}

uint8_t setFPS(uint8_t val) {
  // change or retrieve FPS value
  if (val) {
    FPS = val;
    // change frame timer which drives the task
    controlFrameTimer(true);
    saveFPS = FPS; // used to reset FPS after playback
  }
  return FPS;
}

uint8_t setFPSlookup(uint8_t val) {
  // set FPS from framesize lookup
  fsizePtr = val;
  return setFPS(frameData[fsizePtr].defaultFPS);
}

/**************** capture AVI  ************************/

static void openAvi() {
  // derive filename from date & time, store in date folder
  // time to open a new file on SD increases with the number of files already present
  oTime = millis();
  dateFormat(partName, sizeof(partName), true);
  STORAGE.mkdir(partName); // make date folder if not present
  dateFormat(partName, sizeof(partName), false);
  // open avi file with temporary name 
  aviFile = STORAGE.open(AVITEMP, FILE_WRITE);
  oTime = millis() - oTime;
  LOG_VRB("File opening time: %ums", oTime);
#if INCLUDE_AUDIO
  startAudioRecord();
#endif
#if INCLUDE_TELEM
  haveSrt = startTelemetry();
#endif
  // initialisation of counters
  startTime = millis();
  frameCnt = fTimeTot = wTimeTot = dTimeTot = vidSize = 0;
  highPoint = AVI_HEADER_LEN; // allot space for AVI header
  prepAviIndex();
}

static inline bool doMonitor(bool capturing) {
  // monitor incoming frames for motion 
  static uint8_t motionCnt = 0;
  // ratio for monitoring stop during capture / movement prior to capture
  uint8_t checkRate = (capturing) ? FPS*moveStopSecs : FPS/moveStartChecks;
  if (!checkRate) checkRate = 1;
  if (++motionCnt/checkRate) motionCnt = 0; // time to check for motion
  return !(bool)motionCnt;
}  

static void timeLapse(camera_fb_t* fb, bool tlStop = false) {
  // record a time lapse avi
  // Note that if FPS changed during time lapse recording, 
  //  the time lapse counters wont be modified
  static int frameCntTL, requiredFrames, intervalCnt = 0;
  static int intervalMark = tlSecsBetweenFrames * saveFPS;
  static File tlFile;
  static char TLname[FILE_NAME_LEN];
  if (tlStop) {
    // force save of file on controlled shutdown
    intervalCnt = 0;
    requiredFrames = frameCntTL - 1;
  }
  if (timeLapseOn) {
    if (timeSynchronized) {
      if (!frameCntTL) {
        // initialise time lapse avi
        requiredFrames = tlDurationMins * 60 / tlSecsBetweenFrames;
        dateFormat(partName, sizeof(partName), true);
        STORAGE.mkdir(partName); // make date folder if not present
        dateFormat(partName, sizeof(partName), false);
        int tlen = snprintf(TLname, FILE_NAME_LEN - 1, "%s_%s_%u_%u_T.%s", 
          partName, frameData[fsizePtr].frameSizeStr, tlPlaybackFPS, tlDurationMins, AVI_EXT);
        if (tlen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
        if (STORAGE.exists(TLTEMP)) STORAGE.remove(TLTEMP);
        tlFile = STORAGE.open(TLTEMP, FILE_WRITE);
        tlFile.write(aviHeader, AVI_HEADER_LEN); // space for header
        prepAviIndex(true);
        LOG_INF("Started time lapse file %s, duration %u mins, for %u frames",  TLname, tlDurationMins, requiredFrames);
        frameCntTL++; // to stop re-entering
      }
      // switch on light before capture frame if nightTime
#if INCLUDE_PERIPH
      if (nightTime && intervalCnt == intervalMark - (saveFPS / 2)) setLamp(lampLevel);
#endif
      if (intervalCnt > intervalMark) {
        // save this frame to time lapse avi
#if INCLUDE_PERIPH
        if (!lampNight) setLamp(0);
#endif
        uint8_t hdrBuff[CHUNK_HDR];
        memcpy(hdrBuff, dcBuf, 4); 
        // align end of jpeg on 4 byte boundary for AVI
        uint16_t filler = (4 - (fb->len & 0x00000003)) & 0x00000003; 
        uint32_t jpegSize = fb->len + filler;
        memcpy(hdrBuff+4, &jpegSize, 4);
        tlFile.write(hdrBuff, CHUNK_HDR); // jpeg frame details
        tlFile.write(fb->buf, jpegSize);
        buildAviIdx(jpegSize, true, true); // save avi index for frame
        frameCntTL++;
        intervalCnt = 0;   
        intervalMark = tlSecsBetweenFrames * saveFPS;  // recalc in case FPS changed 
      }
      intervalCnt++;
      if (frameCntTL > requiredFrames) {
        // finish timelapse recording
        xSemaphoreTake(aviMutex, portMAX_DELAY);
        buildAviHdr(tlPlaybackFPS, fsizePtr, --frameCntTL, true);
        xSemaphoreGive(aviMutex);
        // add index
        finalizeAviIndex(frameCntTL, true);
        size_t idxLen = 0;
        do {
          idxLen = writeAviIndex(iSDbuffer, RAMSIZE, true);
          tlFile.write(iSDbuffer, idxLen);
        } while (idxLen > 0);
        // add header
        tlFile.seek(0, SeekSet); // start of file
        tlFile.write(aviHeader, AVI_HEADER_LEN);
        tlFile.close(); 
        STORAGE.rename(TLTEMP, TLname);
        frameCntTL = intervalCnt = 0;
        LOG_INF("Finished time lapse: %s", TLname);
#if INCLUDE_FTP_HFS
        if (autoUpload) fsStartTransfer(TLname); // Transfer it to remote ftp server if requested
#endif
      }
    }
  } else frameCntTL = intervalCnt = 0;
}

void keepFrame(camera_fb_t* fb) {
  // keep required frame for external server alert
  if (fb->len < maxFrameBuffSize && alertBuffer != NULL) {
    memcpy(alertBuffer, fb->buf, fb->len);
    alertBufferSize = fb->len;
  }
}

static void saveFrame(camera_fb_t* fb) {
  // save frame on SD card
  uint32_t fTime = millis();
  // align end of jpeg on 4 byte boundary for AVI
  uint16_t filler = (4 - (fb->len & 0x00000003)) & 0x00000003; 
  size_t jpegSize = fb->len + filler;
  // add avi frame header
  memcpy(iSDbuffer+highPoint, dcBuf, 4); 
  memcpy(iSDbuffer+highPoint+4, &jpegSize, 4);
  highPoint += CHUNK_HDR;
  if (highPoint >= RAMSIZE) {
    // marker overflows buffer
    highPoint -= RAMSIZE;
    aviFile.write(iSDbuffer, RAMSIZE);
    // push overflow to buffer start
    memcpy(iSDbuffer, iSDbuffer+RAMSIZE, highPoint);
  }
  // add frame content
  size_t jpegRemain = jpegSize;
  uint32_t wTime = millis();
  while (jpegRemain >= RAMSIZE - highPoint) {
    // write to SD when RAMSIZE is filled in buffer
    memcpy(iSDbuffer+highPoint, fb->buf + jpegSize - jpegRemain, RAMSIZE - highPoint);
    aviFile.write(iSDbuffer, RAMSIZE);
    jpegRemain -= RAMSIZE - highPoint;
    highPoint = 0;
  } 
  wTime = millis() - wTime;
  wTimeTot += wTime;
  LOG_VRB("SD storage time %u ms", wTime); 
  // whats left or small frame
  memcpy(iSDbuffer+highPoint, fb->buf + jpegSize - jpegRemain, jpegRemain);
  highPoint += jpegRemain;
  
  buildAviIdx(jpegSize); // save avi index for frame
  vidSize += jpegSize + CHUNK_HDR;
  frameCnt++; 
  fTime = millis() - fTime - wTime;
  fTimeTot += fTime;
  LOG_VRB("Frame processing time %u ms", fTime);
  LOG_VRB("============================");
}

static bool closeAvi() {
  // closes the recorded file
  uint32_t vidDuration = millis() - startTime;
  uint32_t vidDurationSecs = lround(vidDuration/1000.0);
  logLine();
  LOG_VRB("Capture time %u, min seconds: %u ", vidDurationSecs, minSeconds);

  cTime = millis();
  // write remaining frame content to SD
  aviFile.write(iSDbuffer, highPoint); 
  size_t readLen = 0;
  bool haveWav = false;
#if INCLUDE_AUDIO
  // add wav file if exists
  finishAudioRecord(true);
  haveWav = haveWavFile();
  if (haveWav) {
    do {
      readLen = writeWavFile(iSDbuffer, RAMSIZE);
      aviFile.write(iSDbuffer, readLen);
    } while (readLen > 0);
  }
#endif
  // save avi index
  finalizeAviIndex(frameCnt);
  do {
    readLen = writeAviIndex(iSDbuffer, RAMSIZE);
    if (readLen) aviFile.write(iSDbuffer, readLen);
  } while (readLen > 0);
  // save avi header at start of file
  float actualFPS = (1000.0f * (float)frameCnt) / ((float)vidDuration);
  uint8_t actualFPSint = (uint8_t)(lround(actualFPS));  
  xSemaphoreTake(aviMutex, portMAX_DELAY);
  buildAviHdr(actualFPSint, fsizePtr, frameCnt);
  xSemaphoreGive(aviMutex); 
  aviFile.seek(0, SeekSet); // start of file
  aviFile.write(aviHeader, AVI_HEADER_LEN); 
  aviFile.close();
  LOG_VRB("Final SD storage time %lu ms", millis() - cTime);
  uint32_t hTime = millis();
#if INCLUDE_MQTT
  if (mqtt_active) {
    sprintf(jsonBuff, "{\"RECORD\":\"OFF\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
    mqttPublish(jsonBuff);
    mqttPublishPath("record", "off");
  }
#endif
  if (vidDurationSecs >= minSeconds) {
    // name file to include actual dateTime, FPS, duration, and frame count
    int alen = snprintf(aviFileName, FILE_NAME_LEN - 1, "%s_%s_%u_%lu%s%s.%s", 
      partName, frameData[fsizePtr].frameSizeStr, actualFPSint, vidDurationSecs, 
      haveWav ? "_S" : "", haveSrt ? "_M" : "", AVI_EXT); 
    if (alen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
    STORAGE.rename(AVITEMP, aviFileName);
    LOG_VRB("AVI close time %lu ms", millis() - hTime); 
    cTime = millis() - cTime;
#if INCLUDE_TELEM
    stopTelemetry(aviFileName);
#endif
    // AVI stats
    LOG_INF("******** AVI recording stats ********");
    LOG_ALT("Recorded %s", aviFileName);
    LOG_INF("AVI duration: %u secs", vidDurationSecs);
    LOG_INF("Number of frames: %u", frameCnt);
    LOG_INF("Required FPS: %u", FPS);
    LOG_INF("Actual FPS: %0.1f", actualFPS);
    LOG_INF("File size: %s", fmtSize(vidSize));
    if (frameCnt) {
      LOG_INF("Average frame length: %u bytes", vidSize / frameCnt);
      LOG_INF("Average frame monitoring time: %u ms", dTimeTot / frameCnt);
      LOG_INF("Average frame buffering time: %u ms", fTimeTot / frameCnt);
      LOG_INF("Average frame storage time: %u ms", wTimeTot / frameCnt);
    }
    LOG_INF("Average SD write speed: %u kB/s", ((vidSize / wTimeTot) * 1000) / 1024);
    LOG_INF("File open / completion times: %u ms / %u ms", oTime, cTime);
    LOG_INF("Busy: %u%%", std::min(100 * (wTimeTot + fTimeTot + dTimeTot + oTime + cTime) / vidDuration, (uint32_t)100));
    checkMemory();
    LOG_INF("*************************************");
#if INCLUDE_FTP_HFS
    if (autoUpload) {
      if (deleteAfter) {
        // issue #380 - in case other files failed to transfer, do whole parent folder
        dateFormat(partName, sizeof(partName), true);
        fsStartTransfer(partName); 
      } else fsStartTransfer(aviFileName); // transfer this file to remote ftp server 
    }
#endif
#if INCLUDE_TGRAM
    if (tgramUse) tgramAlert(aviFileName, "");
#endif
    if (!checkFreeStorage()) doRecording = false; 
    return true; 
  } else {
    // delete too small files if exist
    STORAGE.remove(AVITEMP);
    LOG_INF("Insufficient capture duration: %u secs", vidDurationSecs); 
    return false;
  }
}

//Debug for processFrame() to be shown in serial monitor 04-16-2025
//*enum RecordState { IDLE, RECORDING, COOLDOWN };
//*RecordState recordState = IDLE;
unsigned long recordStartTime = 0;
const unsigned long RECORD_DURATION = 60000; // 60 seconds
const unsigned long COOLDOWN_DURATION = 5000; // 5 seconds

void startRecording() {
    Serial.println("Starting recording...");
    // Add actual recording start logic here (e.g., open a file on SD card)
}

void stopRecording() {
    Serial.println("Stopping recording...");
    uint32_t startTime = millis();
    // SD write logic, e.g., file.close();
    Serial.printf("File save took %lu ms\n", millis() - startTime);
    vTaskDelay(10 / portTICK_PERIOD_MS);
}

static void processFrame() {
    // Get camera frame
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == NULL || !fb->len || fb->len > maxFrameBuffSize) {
        Serial.println("Camera capture failed or invalid frame");
        if (fb) esp_camera_fb_return(fb);
        return;
    }
    
    // Process the frame for time lapse if enabled
    timeLapse(fb);
    
    // Make the frame available for streaming if needed
    for (int i = 0; i < vidStreams; i++) {
        if (!streamBufferSize[i] && streamBuffer[i] != NULL) {
            memcpy(streamBuffer[i], fb->buf, fb->len);
            streamBufferSize[i] = fb->len;   
            xSemaphoreGive(frameSemaphore[i]); // signal frame ready for stream
        }
    }
    
    // Handle still image capture if requested
    if (doKeepFrame) {
        keepFrame(fb);
        doKeepFrame = false;
    }

    // Current time for state transitions
    uint32_t currentTime = millis();
    bool motionDetected = false;
    bool checkForMotion = false;
    
    // Determine when to check for motion
    switch (recordState) {
        case IDLE:
            // In IDLE state, always check motion
            checkForMotion = true;
            break;
            
        case RECORDING:
            // During recording, only check motion periodically
            if (currentTime - lastMotionCheckTime >= (moveStopSecs * 1000)) {
                checkForMotion = true;
                lastMotionCheckTime = currentTime;
            }
            
            // Check if we've exceeded maximum recording time
            if (currentTime - recordingStartTime >= (minSeconds * 1000 * 10)) { // 10x min time as max
                Serial.println("Max recording time reached, closing file");
                closeAvi();
                recordState = COOLDOWN;
                recordingStartTime = currentTime; // Reuse as cooldown start time
            }
            break;
            
        case COOLDOWN:
            // In COOLDOWN state, wait for cooldown period (5 seconds)
            if (currentTime - recordingStartTime >= 5000) { // 5 second cooldown
                Serial.println("Cooldown complete, returning to IDLE");
                recordState = IDLE;
            }
            break;
    }
    
    // Check for motion if needed
    if (checkForMotion && useMotion) {
        motionDetected = checkMotion(fb, recordState == RECORDING);
        motionTriggeredAudio = motionDetected; // For audio recording
        lastMotionCheckTime = currentTime;
    } else if (!useMotion && checkForMotion) {
        // Just calculate light level
        checkMotion(fb, false, true);
    }
    
#if INCLUDE_PERIPH  
    bool pirDetected = false;
    if (pirUse) {
        pirDetected = getPIRval();
        if (pirDetected && recordState == IDLE) {
            // PIR detected motion
            if (lampAuto && nightTime) setLamp(lampLevel);
            notifyMotion(fb);
        }
    }
#endif

    // Determine if we should be recording
    bool shouldRecord = motionDetected || forceRecord;
#if INCLUDE_PERIPH
    shouldRecord = shouldRecord || pirDetected;
#endif
    
    // State machine transitions
    switch (recordState) {
        case IDLE:
            if (shouldRecord) {
                // Start recording
                stopPlaying(); // terminate any playback
                stopPlayback = true; // stop any subsequent playback
                
                LOG_ALT("Capture started by %s%s%s", 
                       motionDetected ? "Motion " : "", 
                       pirVal ? "PIR " : "",
                       forceRecord ? "Button" : "");
                       
#if INCLUDE_MQTT
                if (mqtt_active) {
                    sprintf(jsonBuff, "{\"RECORD\":\"ON\", \"TIME\":\"%s\"}", 
                           esp_log_system_timestamp());
                    mqttPublish(jsonBuff);
                    mqttPublishPath("record", "on");
                }
#endif

#if INCLUDE_PERIPH
                buzzerAlert(true); // sound buzzer if enabled
#endif
                openAvi();
                recordState = RECORDING;
                recordingStartTime = currentTime;
                lastMotionCheckTime = currentTime;
                Serial.println("Started recording due to motion/manual trigger");
            }
            break;
            
        case RECORDING:
            // Always save frames while in RECORDING state
            saveFrame(fb);
            
            // Check if we should stop recording (no motion and min time reached)
            if (!shouldRecord && (currentTime - recordingStartTime >= (minSeconds * 1000))) {
                Serial.println("Motion stopped, finishing recording");
                closeAvi();
                recordState = COOLDOWN;
                recordingStartTime = currentTime; // Reuse as cooldown start time
                
#if INCLUDE_PERIPH
                if (lampAuto) setLamp(0); // switch off lamp
                buzzerAlert(false); // switch off buzzer
#endif
            }
            
#if INCLUDE_PERIPH
            // Turn off buzzer after the specified duration
            if (buzzerUse && buzzerDuration > 0 && 
                (currentTime - recordingStartTime) / 1000 >= buzzerDuration) {
                buzzerAlert(false);
            }
#endif

            // Check if we've hit the frame limit
            if (frameCnt >= maxFrames) {
                logLine();
                LOG_INF("Auto closed recording after %u frames", maxFrames);
                closeAvi();
                recordState = COOLDOWN;
                recordingStartTime = currentTime;
                forceRecord = false; // Clear the force record flag
            }
            break;
            
        // COOLDOWN state handled above
    }
    
    // Return the frame buffer to the camera
    esp_camera_fb_return(fb);
}


/********************** plackback AVI as MJPEG ***********************/

static fnameStruct extractMeta(const char* fname) {
  // extract FPS, duration, and frame count from avi filename
  fnameStruct fnameMeta;
  char fnameStr[FILE_NAME_LEN];
  strcpy(fnameStr, fname);
  // replace all '_' with space for sscanf
  replaceChar(fnameStr, '_', ' ');
  int items = sscanf(fnameStr, "%*s %*s %*s %hhu %lu", &fnameMeta.recFPS, &fnameMeta.recDuration);
  if (items != 2) LOG_ERR("failed to parse %s, items %u", fname, items);
  return fnameMeta; 
}

static void playbackFPS(const char* fname) {
  // extract meta data from filename to commence playback
  fnameStruct fnameMeta = extractMeta(fname);
  recFPS = fnameMeta.recFPS;
  if (recFPS < 1) recFPS = 1;
  recDuration = fnameMeta.recDuration;
  // temp change framerate to recorded framerate
  FPS = recFPS;
  controlFrameTimer(true); // set frametimer
}

static void readSD() {
  // read next cluster from SD for playback
  uint32_t rTime = millis();
  // read to interim dram before copying to psram
  readLen = 0;
  if (!stopPlayback) {
    readLen = playbackFile.read(iSDbuffer+RAMSIZE+CHUNK_HDR, RAMSIZE);
    LOG_VRB("SD read time %lu ms", millis() - rTime);
  }
  wTimeTot += millis() - rTime;
  xSemaphoreGive(readSemaphore); // signal that ready     
  delay(10);
}


void openSDfile(const char* streamFile) {
  // open selected file on SD for streaming
  if (stopPlayback) LOG_WRN("Playback refused - capture in progress");
  else {
    stopPlaying(); // in case already running
    strcpy(aviFileName, streamFile);
    LOG_INF("Playing %s", aviFileName);
    playbackFile = STORAGE.open(aviFileName, FILE_READ);
    playbackFile.seek(AVI_HEADER_LEN, SeekSet); // skip over header
    playbackFPS(aviFileName);
    isPlaying = true; //playback status
    doPlayback = true; // control playback
    readSD(); // prime playback task
  }
}

mjpegStruct getNextFrame(bool firstCall) {
  // get next cluster on demand when ready for opened avi
  mjpegStruct mjpegData;
  static bool remainingBuff;
  static bool completedPlayback;
  static size_t buffOffset;
  static uint32_t hTimeTot;
  static uint32_t tTimeTot;
  static uint32_t hTime;
  static size_t remainingFrame;
  static size_t buffLen;
  const uint32_t dcVal = 0x63643030; // value of 00dc marker
  if (firstCall) {
    sTime = millis();
    hTime = millis();  
    remainingBuff = completedPlayback = false;
    frameCnt = remainingFrame = vidSize = buffOffset = 0;
    wTimeTot = fTimeTot = hTimeTot = tTimeTot = 1; // avoid divide by 0
  }  
  LOG_VRB("http send time %lu ms", millis() - hTime);
  hTimeTot += millis() - hTime;
  uint32_t mTime = millis();
  if (!stopPlayback) {
    // continue sending out frames
    if (!remainingBuff) {
      // load more data from SD
      mTime = millis();
      // move final bytes to buffer start in case jpeg marker at end of buffer
      memcpy(iSDbuffer, iSDbuffer+RAMSIZE, CHUNK_HDR);
      xSemaphoreTake(readSemaphore, portMAX_DELAY); // wait for read from SD card completed
      buffLen = readLen;
      LOG_VRB("SD wait time %lu ms", millis()-mTime);
      wTimeTot += millis()-mTime;
      mTime = millis();  
      // overlap buffer by CHUNK_HDR to prevent jpeg marker being split between buffers
      memcpy(iSDbuffer+CHUNK_HDR, iSDbuffer+RAMSIZE+CHUNK_HDR, buffLen); // load new cluster from double buffer
      LOG_VRB("memcpy took %lu ms for %u bytes", millis()-mTime, buffLen);
      fTimeTot += millis() - mTime;
      remainingBuff = true;
      if (buffOffset > RAMSIZE) buffOffset = 4; // special case, marker overlaps end of buffer 
      else buffOffset = frameCnt ? 0 : CHUNK_HDR; // only before 1st frame
      xTaskNotifyGive(playbackHandle); // wake up task to get next cluster - sets readLen
    }
    mTime = millis();
    if (!remainingFrame) {
      // at start of jpeg frame marker
      uint32_t inVal;
      memcpy(&inVal, iSDbuffer + buffOffset, 4);
      if (inVal != dcVal) {
        // reached end of frames to stream
        mjpegData.buffLen = buffOffset; // remainder of final jpeg
        mjpegData.buffOffset = 0; // from start of buff
        mjpegData.jpegSize = 0; 
        stopPlayback = completedPlayback = true;
        return mjpegData;
      } else {
        // get jpeg frame size
        uint32_t jpegSize;
        memcpy(&jpegSize, iSDbuffer + buffOffset + 4, 4);
        remainingFrame = jpegSize;
        vidSize += jpegSize;
        buffOffset += CHUNK_HDR; // skip over marker 
        mjpegData.jpegSize = jpegSize; // signal start of jpeg to webServer
        mTime = millis();
        // wait on playbackSemaphore for rate control
        xSemaphoreTake(playbackSemaphore, portMAX_DELAY);
        LOG_VRB("frame timer wait %lu ms", millis()-mTime);
        tTimeTot += millis()-mTime;
        frameCnt++;
        showProgress();
      }
    } else mjpegData.jpegSize = 0; // within frame,    
    // determine amount of data to send to webServer
    if (buffOffset > RAMSIZE) mjpegData.buffLen = 0; // special case 
    else mjpegData.buffLen = (remainingFrame > buffLen - buffOffset) ? buffLen - buffOffset : remainingFrame;
    mjpegData.buffOffset = buffOffset; // from here    
    remainingFrame -= mjpegData.buffLen;
    buffOffset += mjpegData.buffLen;
    if (buffOffset >= buffLen) remainingBuff = false;
  } else {
    // finished, close SD file used for streaming
    playbackFile.close();
    logLine();
    if (!completedPlayback) LOG_INF("Force close playback");
    uint32_t playDuration = (millis() - sTime) / 1000;
    uint32_t totBusy = wTimeTot + fTimeTot + hTimeTot;
    LOG_INF("******** AVI playback stats ********");
    LOG_INF("Playback %s", aviFileName);
    LOG_INF("Recorded FPS %u, duration %u secs", recFPS, recDuration);
    LOG_INF("Playback FPS %0.1f, duration %u secs", (float)frameCnt / playDuration, playDuration);
    LOG_INF("Number of frames: %u", frameCnt);
    if (frameCnt) {
      LOG_INF("Average SD read speed: %u kB/s", ((vidSize / wTimeTot) * 1000) / 1024);
      LOG_INF("Average frame SD read time: %u ms", wTimeTot / frameCnt);
      LOG_INF("Average frame processing time: %u ms", fTimeTot / frameCnt);
      LOG_INF("Average frame delay time: %u ms", tTimeTot / frameCnt);
      LOG_INF("Average http send time: %u ms", hTimeTot / frameCnt);
      LOG_INF("Busy: %u%%", min(100 * totBusy / (totBusy + tTimeTot), (uint32_t)100));
    }
    checkMemory();
    LOG_INF("*************************************\n");
    setFPS(saveFPS); // realign with browser
    stopPlayback = isPlaying = false;
    mjpegData.buffLen = mjpegData.buffOffset = 0; // signal end of jpeg
  }
  hTime = millis();
  delay(1);
  return mjpegData;
}

void stopPlaying() {
  if (isPlaying) {
    // force stop any currently running playback
    stopPlayback = true;
    // wait till stopped cleanly, but prevent infinite loop
    uint32_t timeOut = millis();
    while (doPlayback && millis() - timeOut < MAX_FRAME_WAIT) delay(10);
    if (doPlayback) {
      // not yet closed, so force close
      logLine();
      LOG_WRN("Force closed playback");
      doPlayback = false; // stop webserver playback
      setFPS(saveFPS);
      xSemaphoreGive(playbackSemaphore);
      xSemaphoreGive(readSemaphore);
      delay(200);
    } 
    stopPlayback = false;
    isPlaying = false;
  }
}

static void playbackTask(void* parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    readSD();
  }
  vTaskDelete(NULL);
}

/******************* Startup ********************/

static void captureTask(void* parameter) {
  // woken by frame timer when time to capture frame
  uint32_t ulNotifiedValue;
  while (true) {
    ulNotifiedValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (ulNotifiedValue > FB_CNT) ulNotifiedValue = FB_CNT; // prevent too big queue if FPS excessive
    // may be more than one isr outstanding if the task delayed by SD write or jpeg decode
    while (ulNotifiedValue-- > 0) processFrame();
  }
  vTaskDelete(NULL);
}

static void startSDtasks() {
  // tasks to manage SD card operation
  xTaskCreate(&captureTask, "captureTask", CAPTURE_STACK_SIZE, NULL, CAPTURE_PRI, &captureHandle);
  xTaskCreate(&playbackTask, "playbackTask", PLAYBACK_STACK_SIZE, NULL, PLAY_PRI, &playbackHandle);
  // set initial camera framesize and FPS from configs
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, (framesize_t)fsizePtr);
  setFPS(FPS); 
  debugMemory("startSDtasks");
}

bool prepRecording() {
  // initialisation & prep for AVI capture
  readSemaphore = xSemaphoreCreateBinary();
  playbackSemaphore = xSemaphoreCreateBinary();
  aviMutex = xSemaphoreCreateMutex();
  motionSemaphore = xSemaphoreCreateBinary();
  for (int i = 0; i < vidStreams; i++) frameSemaphore[i] = xSemaphoreCreateBinary();
  reloadConfigs(); // apply camera config
  startSDtasks();
#if INCLUDE_TINYML
  LOG_INF("%sUsing TinyML", mlUse ? "" : "Not ");
#endif

  if ((fs::LittleFSFS*)&STORAGE == &LittleFS) {
    // prevent recording
    sdFreeSpaceMode = 0;
    sdMinCardFreeSpace = 0;
    doRecording = false;
    sdLog = false;
    useMotion = false;
    doRecording = false; 
    LOG_WRN("Recording disabled as no SD card");
  } else {
    LOG_INF("To record new AVI, do one of:");
    LOG_INF("- press Start Recording on web page");
#if INCLUDE_PERIPH
    if (pirUse) {
      LOG_INF("- attach PIR to pin %u", pirPin);
      LOG_INF("- raise pin %u to 3.3V", pirPin);
    }
#endif
    if (useMotion) LOG_INF("- move in front of camera");
  }
  logLine();
  debugMemory("prepRecording");
  return true;
}

void appShutdown() {
  timeLapse(NULL, true);
}

static void deleteTask(TaskHandle_t thisTaskHandle) {
  // hangs if try deleting null thisTaskHandle
  if (thisTaskHandle != NULL) vTaskDelete(thisTaskHandle);
  thisTaskHandle = NULL;
}

void endTasks() {
  for (int i = 0; i < numStreams; i++) deleteTask(sustainHandle[i]);
  deleteTask(captureHandle);
  deleteTask(playbackHandle);
#if INCLUDE_TELEM
  deleteTask(telemetryHandle);
#endif
#if INCLUDE_PERIPH
  deleteTask(DS18B20handle);
  deleteTask(servoHandle);
  deleteTask(stickHandle);
#endif
#if INCLUDE_SMTP
  deleteTask(emailHandle);
#endif
#if INCLUDE_FTP_HFS
  deleteTask(fsHandle);
#endif
#if INCLUDE_TGRAM
  deleteTask(telegramHandle);
#endif
#if INCLUDE_AUDIO
  deleteTask(audioHandle);
#endif
}

void OTAprereq() {
  // stop timer isrs, and free up heap space, or crashes esp32
  doPlayback = forceRecord = false;
  controlFrameTimer(false);
#if INCLUDE_PERIPH
  setStickTimer(false);
#endif
  stopPing();
  endTasks();
  esp_camera_deinit();
  delay(100);
}

#ifdef CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3
// This board has a configurable power supply
// Need to instal following library
#include "DFRobot_AXP313A.h" // https://github.com/cdjq/DFRobot_AXP313A
DFRobot_AXP313A axp;

static bool camPower() {
  int pwrRetry = 5;
  while (pwrRetry) { 
    if (axp.begin() == 0) {
      axp.enableCameraPower(axp.eOV2640);
      return true;
    } else {
      delay(1000); 
      pwrRetry--;
    }
  } 
  LOG_ERR("Failed to power up camera");
  return false;
}

#else

static bool camPower() {
  // dummy
  return true;
}
#endif

static esp_err_t changeXCLK(camera_config_t config) {
  //since the original setup doesnt create over 20MHz clock, we do it forcefully
  if (config.xclk_freq_hz <= 20 * OneMHz) return ESP_OK;
  esp_err_t res = ESP_OK;
  // Deinitialize the existing LEDC configuration
  ledc_stop(LEDC_LOW_SPEED_MODE, config.ledc_channel, 0);
  delay(5);
  // Configure the LEDC timer
  ledc_timer_config_t ledc_timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_1_BIT,
    .timer_num = config.ledc_timer,
    .freq_hz = (uint32_t)config.xclk_freq_hz,
    .clk_cfg = LEDC_AUTO_CLK
  };
  res = ledc_timer_config(&ledc_timer);
  if (res != ESP_OK) {
    LOG_ERR("Failed to configure timer %s", espErrMsg(res));
    return res;
  }
  // Configure the LEDC channel
  ledc_channel_config_t ledc_channel = {
    .gpio_num = XCLK_GPIO_NUM,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = config.ledc_channel,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = config.ledc_timer,
    .duty = 1,  // 50% duty cycle for 1-bit resolution
    .hpoint = 0
  };
  res = ledc_channel_config(&ledc_channel);
  if (res != ESP_OK) {
    LOG_ERR("Failed to configure channel %s", espErrMsg(res));
    return res;
  }
  delay(200); // base on datasheet, it needs < 300 ms for configuration to settle in. we just put 200ms. it doesnt hurt.
  return res;
}

bool prepCam() {
    // Initialize camera depending on model and board
    if (FRAMESIZE_INVALID != sizeof(frameData) / sizeof(frameData[0])) 
        LOG_ERR("framesize_t entries %d != frameData entries %d", FRAMESIZE_INVALID, sizeof(frameData) / sizeof(frameData[0]));
        
    if (!camPower()) {
        LOG_ERR("Failed to power camera");
        Serial.println("Failed to power camera module");
        return false;
    }
    
#if INCLUDE_I2C
    if (shareI2C(SIOD_GPIO_NUM, SIOC_GPIO_NUM)) { 
        // If shared, set camera to use shared
        siodGpio = -1;
        siocGpio = -1;
        Serial.println("Using shared I2C pins for camera");
    }
#endif

    bool res = false;
    // Buffer sizing depends on PSRAM size
    size_t psramSize = ESP.getPsramSize();
    Serial.printf("Available PSRAM: %s\n", fmtSize(psramSize));
    
    // Set maximum frame size based on available PSRAM
    maxFS = FRAMESIZE_SVGA; // Default for 2MB PSRAM
    if (psramSize > 5 * ONEMEG) {
        maxFS = FRAMESIZE_QSXGA; // 8MB PSRAM
        Serial.println("8MB PSRAM detected, using QSXGA max frame size");
    } else if (psramSize > 3 * ONEMEG) {
        maxFS = FRAMESIZE_UXGA; // 4MB PSRAM
        Serial.println("4MB PSRAM detected, using UXGA max frame size");
    } else {
        Serial.println("2MB PSRAM detected, using SVGA max frame size");
    }
    
    // Define buffer size based on maximum frame size
    maxFrameBuffSize = maxAlertBuffSize = frameData[maxFS].frameWidth * frameData[maxFS].frameHeight / 5; 
    LOG_INF("Max frame size for %s PSRAM is %s", fmtSize(psramSize), frameData[maxFS].frameSizeStr);

    // Configure camera
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_1;
    config.ledc_timer = LEDC_TIMER_1;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = siodGpio;
    config.pin_sccb_scl = siocGpio;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = xclkMhz * OneMHz;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.frame_size = maxFS;
    config.jpeg_quality = 10; // High quality for initialization
    config.fb_count = FB_CNT;
    config.sccb_i2c_port = 0;
    
    // Initialize GPIO pins that might be used for camera
#if defined(CAMERA_MODEL_ESP_EYE)
    pinMode(13, INPUT_PULLUP);
    pinMode(14, INPUT_PULLUP);
#endif

    Serial.println("Attempting to initialize OV2640 camera...");
    
    // Initialize camera with multiple retry attempts
    esp_err_t err = ESP_FAIL;
    uint8_t retries = 3; // Increased from 2 to 3
    
    while (retries && err != ESP_OK) {
        // Power cycle the camera before each retry (if applicable)
        if (retries < 3) {
            Serial.printf("Camera init retry %d...\n", 4 - retries);
#if (defined(PWDN_GPIO_NUM)) && (PWDN_GPIO_NUM > -1)
            digitalWrite(PWDN_GPIO_NUM, 1);
            delay(100);
            digitalWrite(PWDN_GPIO_NUM, 0); 
            delay(100);
#else
            delay(300); // Longer delay between retries
#endif
        }
        
        err = esp_camera_init(&config);
        if (err == ESP_OK) {
            Serial.println("Camera initialized successfully, adjusting clock...");
            err = changeXCLK(config);
            if (err != ESP_OK) {
                Serial.printf("Failed to adjust camera clock: %s\n", espErrMsg(err));
            } else {
                Serial.println("Camera clock adjusted successfully");
            }
        } else {
            Serial.printf("Camera init failed with error 0x%x (%s) on attempt %d\n", 
                         err, espErrMsg(err), 4 - retries);
        }
        
        retries--;
    }

    if (err != ESP_OK) {
        snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Camera init error 0x%x:%s on %s", 
                err, espErrMsg(err), CAM_BOARD);
        Serial.println(startupFailure);
        return false;
    }
    
    // Get camera sensor and check status
    sensor_t* s = esp_camera_sensor_get();
    if (s == NULL) {
        snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to access camera sensor on %s", CAM_BOARD);
        Serial.println(startupFailure);
        return false;
    }
    
    // Identify camera model
    switch (s->id.PID) {
        case (OV2640_PID):
            strcpy(camModel, "OV2640");
            break;
        case (OV3660_PID):
            strcpy(camModel, "OV3660");
            break;
        case (OV5640_PID):
            strcpy(camModel, "OV5640");
            break;
        default:
            strcpy(camModel, "Other");
            break;
    }
    
    Serial.printf("Camera identified as %s\n", camModel);
    
    // Set frame size from config or default
    char fsizeStr[4];
    framesize_t initialFrameSize = FRAMESIZE_SVGA;
    
    if (retrieveConfigVal("framesize", fsizeStr)) {
        initialFrameSize = (framesize_t)(atoi(fsizeStr));
        Serial.printf("Setting framesize from config: %s\n", frameData[initialFrameSize].frameSizeStr);
    } else {
        Serial.printf("Using default framesize: %s\n", frameData[initialFrameSize].frameSizeStr);
    }
    
    s->set_framesize(s, initialFrameSize);
    
    // Apply model-specific corrections
    if (s->id.PID == OV3660_PID) {
        // Initial sensors are flipped vertically and colors are a bit saturated
        s->set_vflip(s, 1); // Flip it back
        s->set_brightness(s, 1); // Up brightness slightly
        s->set_saturation(s, -2); // Lower saturation
        Serial.println("Applied OV3660-specific corrections");
    }
    
    // Apply board-specific settings
#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    Serial.println("Applied M5STACK-specific mirror/flip settings");
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
    s->set_vflip(s, 1);
    Serial.println("Applied ESP32S3_EYE-specific flip settings");
#endif

    // Verify camera functionality by capturing a test frame
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == NULL) {
        // Usually indicates a hardware issue
        snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to get camera frame - check camera hardware"); 
        Serial.println(startupFailure);
        return false;
    } else {
        Serial.printf("Test frame captured successfully: %ux%u, %u bytes\n", 
                     fb->width, fb->height, fb->len);
        esp_camera_fb_return(fb);
        fb = NULL;
        res = true;
        LOG_INF("Camera model %s ready @ %uMHz", camModel, xclkMhz); 
    }
    
    debugMemory("prepCam");
    return res;
}
