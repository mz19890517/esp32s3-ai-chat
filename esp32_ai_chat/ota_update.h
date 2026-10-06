#pragma once
#include <Arduino.h>

#define FW_VERSION "1.2.0"

enum OtaState : uint8_t { OTA_IDLE, OTA_RUNNING, OTA_DONE, OTA_FAIL };

void otaBegin();
void otaPoll();
OtaState otaState();
int otaPercent();
String otaMessage();
String otaVersion();
String otaSlot();
String otaFreeSpace();
bool otaArmed();
void otaSetArmed(bool a);
bool otaWebStart(const String &url);
bool otaBusy();
bool otaRestartPending();