#include "hal.h"
#include <Wire.h>

BoardType detectedBoard = BOARD_M5STICKS3;
LGFX_Freenove_S3 freenoveDisplay;

bool hasTouchscreen = false;
bool hasImu = false;
bool hasBatteryPmic = false;

bool triggerNext = false;
bool triggerSelect = false;
bool triggerPrevious = false;
bool triggerExit = false;
bool triggerShowIp = false;

static bool touchActive = false;
static int touchStartX = 0;
static int touchStartY = 0;
static int touchCurrentX = 0;
static int touchCurrentY = 0;
static unsigned long touchStartTime = 0;
static bool longPressTriggered = false;

lgfx::LGFX_Device& getHalDisplay() {
  if (detectedBoard == BOARD_FREENOVE_S3) {
    return freenoveDisplay;
  }
  return M5.Display;
}

void halInit() {
  Serial.println("[HAL] halInit() starting...");

  // 1. Initialize M5Unified (handles all M5Stack hardware automatically)
  auto configuration = M5.config();
  M5.begin(configuration);

  if (M5.getBoard() != m5::board_t::board_unknown) {
    detectedBoard = BOARD_M5STICKS3;
    hasTouchscreen = false;
    hasImu = true;
    hasBatteryPmic = true;

    M5.Display.setBrightness(255);
    M5.Display.setRotation(1);
    M5.Display.setTextFont(2);
    Serial.printf("[HAL] M5 Board Detected: type=%d (Display %dx%d)\n",
                  (int)M5.getBoard(), M5.Display.width(), M5.Display.height());
  } else {
    // 2. Non-M5 hardware (Freenove ESP32-S3 Display / Wokwi Simulator)
    detectedBoard = BOARD_FREENOVE_S3;
    hasTouchscreen = true;
    hasImu = false;
    hasBatteryPmic = false;

    Serial.println("[HAL] Non-M5 Board detected. Initializing Freenove / LGFX Display...");
    freenoveDisplay.init();
    freenoveDisplay.setBrightness(255);
    freenoveDisplay.setRotation(1);
    freenoveDisplay.setTextFont(2);
    Serial.printf("[HAL] Freenove Display Initialized: (%dx%d)\n",
                  freenoveDisplay.width(), freenoveDisplay.height());
  }
}

void halUpdate() {
  if (detectedBoard == BOARD_M5STICKS3) {
    M5.update();
    return;
  }

  // Poll pushbuttons on GPIO 0 (BtnA) and GPIO 35 (BtnB) for Freenove / Wokwi DevKit
  static unsigned long btnAPressTime = 0;
  static bool btnAPressed = false;
  static bool btnALongTriggered = false;

  bool rawBtnA = (digitalRead(0) == LOW);
  if (rawBtnA && !btnAPressed) {
    btnAPressed = true;
    btnAPressTime = millis();
    btnALongTriggered = false;
  } else if (rawBtnA && btnAPressed) {
    if (!btnALongTriggered && (millis() - btnAPressTime > 600)) {
      btnALongTriggered = true;
      triggerShowIp = true;
    }
  } else if (!rawBtnA && btnAPressed) {
    btnAPressed = false;
    if (!btnALongTriggered && (millis() - btnAPressTime < 500)) {
      triggerSelect = true;
    }
  }

  static unsigned long btnBPressTime = 0;
  static bool btnBPressed = false;
  static bool btnBLongTriggered = false;

  bool rawBtnB = (digitalRead(35) == LOW);
  if (rawBtnB && !btnBPressed) {
    btnBPressed = true;
    btnBPressTime = millis();
    btnBLongTriggered = false;
  } else if (rawBtnB && btnBPressed) {
    if (!btnBLongTriggered && (millis() - btnBPressTime > 600)) {
      btnBLongTriggered = true;
      triggerExit = true;
    }
  } else if (!rawBtnB && btnBPressed) {
    btnBPressed = false;
    if (!btnBLongTriggered && (millis() - btnBPressTime < 500)) {
      triggerNext = true;
    }
  }

  if (hasTouchscreen) {
    int x = 0, y = 0;
    bool isTouched = freenoveDisplay.getTouch(&x, &y);

    if (isTouched) {
      if (!touchActive) {
        touchActive = true;
        touchStartX = x;
        touchStartY = y;
        touchCurrentX = x;
        touchCurrentY = y;
        touchStartTime = millis();
        longPressTriggered = false;
      } else {
        touchCurrentX = x;
        touchCurrentY = y;
        if (!longPressTriggered && (millis() - touchStartTime > 600)) {
          int dx = touchCurrentX - touchStartX;
          int dy = touchCurrentY - touchStartY;
          if (abs(dx) < 20 && abs(dy) < 20) {
            longPressTriggered = true;
            if (touchStartY < 40) {
              triggerShowIp = true;
            } else {
              triggerExit = true;
            }
          }
        }
      }
    } else {
      if (touchActive) {
        touchActive = false;
        if (!longPressTriggered) {
          int dx = touchCurrentX - touchStartX;
          int dy = touchCurrentY - touchStartY;
          unsigned long duration = millis() - touchStartTime;

          if (duration < 500 && (abs(dx) > 30 || abs(dy) > 30)) {
            // Swipe Gestures
            if (abs(dx) > abs(dy)) {
              if (dx < -30) triggerNext = true;         // Swipe Left -> Next plane
              else if (dx > 30) triggerPrevious = true; // Swipe Right -> Prev plane
            } else {
              if (dy < -30) triggerNext = true;         // Swipe Up -> Next
              else if (dy > 30) triggerPrevious = true; // Swipe Down -> Prev
            }
          } else if (duration < 500 && abs(dx) <= 30 && abs(dy) <= 30) {
            // Tap Gestures
            int width = freenoveDisplay.width();
            int height = freenoveDisplay.height();

            if (touchStartY < 30) {
              // Tap Header Bar -> Show IP overlay
              triggerShowIp = true;
            } else if (touchStartY > height - 35) {
              // Tap Bottom Action Bar
              if (touchStartX < width / 3) {
                triggerPrevious = true;
              } else if (touchStartX > (width * 2) / 3) {
                triggerNext = true;
              } else {
                triggerSelect = true;
              }
            } else {
              // Tap Main Content Area
              if (touchStartX < width / 3) {
                triggerPrevious = true;
              } else if (touchStartX > (width * 2) / 3) {
                triggerNext = true;
              } else {
                triggerSelect = true;
              }
            }
          }
        }
      }
    }
  } else {
    M5.update();
  }
}

int halGetBatteryLevel() {
  if (detectedBoard == BOARD_M5STICKS3) {
    return constrain((int)M5.Power.getBatteryLevel(), 0, 100);
  }
  return 100;
}

bool halIsCharging() {
  if (detectedBoard == BOARD_M5STICKS3) {
    return M5.Power.isCharging() == m5::Power_Class::is_charging;
  }
  return true;
}

bool halReadImu(float *ax, float *ay, float *az, float *gx, float *gy, float *gz) {
  if (detectedBoard == BOARD_M5STICKS3) {
    if (!M5.Imu.getAccelData(ax, ay, az) || !M5.Imu.getGyroData(gx, gy, gz)) {
      return false;
    }
    return true;
  }
  return false;
}

bool controlNext() {
  if (detectedBoard == BOARD_M5STICKS3 && M5.BtnB.wasSingleClicked()) return true;
  if (triggerNext) { triggerNext = false; return true; }
  return false;
}

bool controlSelect() {
  if (detectedBoard == BOARD_M5STICKS3 && M5.BtnA.wasSingleClicked()) return true;
  if (triggerSelect) { triggerSelect = false; return true; }
  return false;
}

bool controlPrevious() {
  if (detectedBoard == BOARD_M5STICKS3 && M5.BtnB.wasDoubleClicked()) return true;
  if (triggerPrevious) { triggerPrevious = false; return true; }
  return false;
}

bool controlExit() {
  if (detectedBoard == BOARD_M5STICKS3 && M5.BtnB.wasHold()) return true;
  if (triggerExit) { triggerExit = false; return true; }
  return false;
}

bool controlShowIp() {
  if (detectedBoard == BOARD_M5STICKS3 && M5.BtnA.wasHold()) return true;
  if (triggerShowIp) { triggerShowIp = false; return true; }
  return false;
}
