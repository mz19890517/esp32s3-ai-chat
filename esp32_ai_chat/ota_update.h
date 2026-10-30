#pragma once
#include <Arduino.h>

#define FW_VERSION "1.3.0"

enum OtaState : uint8_t
{
  OST_IDLE,
  OST_RUNNING,
  OST_DONE,
  OST_FAIL
};

// 升级通道，仅用于界面显示
enum OtaChannel : uint8_t
{
  OCH_NONE,
  OCH_NET,
  OCH_URL,
  OCH_SD,
  OCH_BLE
};

void otaBegin();
void otaPoll();

OtaState otaState();
int otaPercent();
String otaMessage();
String otaVersion();
String otaSlot();
String otaFreeSpace();
size_t otaTargetSize();       // 目标（备用）分区容量
bool otaArmed();
void otaSetArmed(bool a);
bool otaWebStart(const String &url);
bool otaBusy();
bool otaRestartPending();

// 供 TF / BLE 通道复用
void otaSetChannel(OtaChannel c);
OtaChannel otaChannel();
const char *otaChannelName();
void otaReport(OtaState s, const String &msg, int pct);
void otaRequestRestart(uint32_t delayMs);
