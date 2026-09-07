#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include "freenove_lgfx.h"

enum BoardType {
  BOARD_M5STICKS3,
  BOARD_FREENOVE_S3
};

extern BoardType detectedBoard;
extern LGFX_Freenove_S3 freenoveDisplay;

lgfx::LGFX_Device& getHalDisplay();
#define halDisplay getHalDisplay()

typedef lgfx::LGFX_Sprite halCanvas;

extern bool hasTouchscreen;
extern bool hasImu;
extern bool hasBatteryPmic;

extern bool triggerNext;
extern bool triggerSelect;
extern bool triggerPrevious;
extern bool triggerExit;
extern bool triggerShowIp;

void halInit();
void halUpdate();

int halGetBatteryLevel();
bool halIsCharging();
bool halReadImu(float *ax, float *ay, float *az, float *gx, float *gy, float *gz);

bool controlNext();
bool controlSelect();
bool controlPrevious();
bool controlExit();
bool controlShowIp();
